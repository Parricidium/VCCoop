// Ray tracing materiel (Rendu=12) : cote jeu.
//
// Le pilote refuse DXR aux processus 32 bits : vcrt64.exe (VCCoop\, 64 bits, Direct3D 12) trace les rayons. On lui
// envoie par memoire partagee (rt\rtshared.h) les maillages et les textures des dessins notes par gfx9.cpp (une fois,
// tant que le jeu garde le tampon), puis a chaque image la liste des dessins (maillage + matrice) et la camera ; il
// renvoie la visibilite du soleil par pixel, que gfx9.cpp pose a la place des cascades. Le pont (bridge.cpp) nous
// previent quand un tampon ou une texture est libere ou reecrit (RtForgetResource).
#include "util.h"
#include "vccoop.h"
#include "bridge.h"
#include "../rt/rtshared.h"
#include <d3d9.h>
#include <string.h>
#include <unordered_map>
#include <vector>

static HANDLE g_map, g_go, g_done;
static BYTE *g_base;
static RtHeader *g_hdr;
static HANDLE g_helper;
static bool g_started, g_dead;
static bool g_pending;
static uint32_t g_seq;
static IDirect3DTexture9 *g_result[3];
static UINT g_resW, g_resH;
static bool g_haveResult;
static CRITICAL_SECTION g_lock;
struct RtLock { RtLock() { EnterCriticalSection(&g_lock); } ~RtLock() { LeaveCriticalSection(&g_lock); } };

static const char *HelperPath()
{
    static char p[MAX_PATH];
    if (!p[0]) wsprintfA(p, "%sVCCoop\\vcrt64.exe", GameDir());
    return p;
}
bool RtHelperPresent() { return GetFileAttributesA(HelperPath()) != INVALID_FILE_ATTRIBUTES; }

void RtStart()
{
    if (g_started) return;
    g_started = true;
    InitializeCriticalSection(&g_lock);
    DWORD pid = GetCurrentProcessId();
    char name[64];
    wsprintfA(name, "Local\\VCCoopRT_%lu", pid);
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, RT_MAP_SIZE, name);
    g_base = g_map ? (BYTE *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, RT_MAP_SIZE) : NULL;
    if (!g_base) { Log("ray tracing : memoire partagee impossible (%lu), rendu Direct3D 9", GetLastError()); g_dead = true; return; }
    g_hdr = (RtHeader *)g_base;
    memset(g_hdr, 0, sizeof *g_hdr);
    g_hdr->magic = RT_MAGIC; g_hdr->version = RT_PROTOCOL;
    wsprintfA(name, "Local\\VCCoopRT_go_%lu", pid);
    g_go = CreateEventA(NULL, FALSE, FALSE, name);
    wsprintfA(name, "Local\\VCCoopRT_done_%lu", pid);
    g_done = CreateEventA(NULL, FALSE, FALSE, name);
    char cmd[MAX_PATH + 32];
    wsprintfA(cmd, "\"%s\" %lu", HelperPath(), pid);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        Log("ray tracing : %s introuvable ou refuse (%lu), rendu Direct3D 9", HelperPath(), GetLastError());
        g_dead = true;
        return;
    }
    CloseHandle(pi.hThread);
    g_helper = pi.hProcess;
    Log("ray tracing : vcrt64.exe lance (processus %lu)", pi.dwProcessId);
}

// Pret : le programme a ouvert la carte (DXR). En echec : message une fois, puis rendu Direct3D 9 (cascades).
bool RtReady()
{
    if (!g_started || g_dead || !g_hdr) return false;
    if (g_hdr->helperState == 1) {
        if (WaitForSingleObject(g_helper, 0) == WAIT_OBJECT_0) { Log("ray tracing : vcrt64.exe s'est arrete, rendu Direct3D 9"); g_dead = true; return false; }
        static bool logged;
        if (!logged) { logged = true; Log("ray tracing : pret sur %s", g_hdr->adapter); }
        return true;
    }
    if (g_hdr->helperState == 2 || WaitForSingleObject(g_helper, 0) == WAIT_OBJECT_0) {
        Log("ray tracing : indisponible (%s), rendu Direct3D 9", g_hdr->error[0] ? g_hdr->error : "vcrt64.exe arrete");
        g_dead = true;
    }
    return false;
}

void RtBeforeReset()
{
    for (auto &t : g_result) if (t) { t->Release(); t = NULL; }
    g_haveResult = false;
    g_resW = g_resH = 0;
}
IDirect3DTexture9 *RtResult(int plane) { return g_haveResult && plane >= 0 && plane < 3 ? g_result[plane] : NULL; }
void RtResultSize(float *w, float *h) { *w = (float)g_resW; *h = (float)g_resH; }

// ---------------------------------------------------------------- caches
struct MeshKey {
    void *vb, *ib;
    UINT stride, fvf, type, base, minIdx, numVerts, start, count;
    bool operator==(const MeshKey &o) const { return !memcmp(this, &o, sizeof *this); }
};
struct MeshKeyHash {
    size_t operator()(const MeshKey &k) const
    {
        const BYTE *p = (const BYTE *)&k; size_t h = 2166136261u;
        for (size_t i = 0; i < sizeof k; i++) h = (h ^ p[i]) * 16777619u;
        return h;
    }
};
enum { ST_NEW, ST_SENT, ST_BAD };
struct MeshVal { uint32_t id; uint8_t state; };
static std::unordered_map<MeshKey, MeshVal, MeshKeyHash> g_meshCache;
static std::unordered_map<uint32_t, MeshKey> g_meshKeys;
static std::unordered_map<void *, std::vector<uint32_t>> g_bufMeshes;
struct TexVal { uint32_t id; uint8_t state; };
static std::unordered_map<void *, TexVal> g_texCache;
static std::vector<uint32_t> g_meshDel, g_texDel;
static uint32_t g_nextMesh = 1, g_nextTex = 1;

// Tampon ou texture libere, ou reecrit par le jeu : ses maillages et sa texture sont oublies (et effaces chez vcrt64).
void RtForgetResource(void *real)
{
    if (!g_started || g_dead) return;
    RtLock lock;
    auto b = g_bufMeshes.find(real);
    if (b != g_bufMeshes.end()) {
        for (uint32_t id : b->second) {
            auto k = g_meshKeys.find(id);
            if (k == g_meshKeys.end()) continue;
            auto m = g_meshCache.find(k->second);
            if (m != g_meshCache.end()) { if (m->second.state == ST_SENT) g_meshDel.push_back(id); g_meshCache.erase(m); }
            g_meshKeys.erase(k);
        }
        g_bufMeshes.erase(b);
    }
    auto t = g_texCache.find(real);
    if (t != g_texCache.end()) { if (t->second.state == ST_SENT) g_texDel.push_back(t->second.id); g_texCache.erase(t); }
}

// ---------------------------------------------------------------- commandes
static uint32_t g_off, g_limit;
static void *Cmd(uint32_t type, uint32_t bytes)
{
    uint32_t need = sizeof(RtCmd) + ((bytes + 15) & ~15u);
    if (g_off + need > g_limit) return NULL;
    RtCmd *c = (RtCmd *)(g_base + RT_CMD_OFFSET + g_off);
    c->type = type; c->bytes = bytes; c->pad[0] = c->pad[1] = 0;
    g_off += need;
    return c + 1;
}

static UINT VertexCount(UINT type, UINT count)
{
    switch (type) { case D3DPT_TRIANGLELIST: return count * 3; case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return count + 2; }
    return 0;
}
static int UvOffset(DWORD fvf)
{
    if (!(fvf & D3DFVF_TEXCOUNT_MASK)) return -1;
    int o = 12;
    if (fvf & D3DFVF_NORMAL) o += 12;
    if (fvf & D3DFVF_PSIZE) o += 4;
    if (fvf & D3DFVF_DIFFUSE) o += 4;
    if (fvf & D3DFVF_SPECULAR) o += 4;
    return o;
}

static std::vector<float> g_pos, g_uv, g_nrm;
static std::vector<DWORD> g_col;
static std::vector<uint32_t> g_tris;

// Sommets [first, first + n) et primitives du dessin -> positions, uv, triangles (indices locaux).
static bool Extract(const RtDraw &d)
{
    const GfxDraw &g = d.d;
    UINT nIdx = VertexCount(g.type, g.count);
    if (!nIdx || g.count > 200000) return false;
    UINT first = g.indexed ? g.baseVertex + g.minIndex : g.start;
    UINT n = g.indexed ? g.numVerts : nIdx;
    if (!n || n > 65536 || !d.stride || d.stride > 256) return false;
    const BYTE *verts = NULL;
    bool vbLocked = false, ibLocked = false;
    if (d.vbData) verts = d.vbData + (size_t)first * d.stride;
    else {
        void *p = NULL;
        if (FAILED(d.vb->Lock(first * d.stride, n * d.stride, &p, D3DLOCK_READONLY)) || !p) return false;
        verts = (const BYTE *)p; vbLocked = true;
    }
    const WORD *idx = NULL;
    if (g.indexed) {
        if (d.ibData) idx = d.ibData + g.start;
        else {
            D3DINDEXBUFFER_DESC id;
            void *p = NULL;
            if (FAILED(d.ib->GetDesc(&id)) || id.Format != D3DFMT_INDEX16 || (g.start + nIdx) * 2 > id.Size ||
                FAILED(d.ib->Lock(g.start * 2, nIdx * 2, &p, D3DLOCK_READONLY)) || !p) {
                if (vbLocked) d.vb->Unlock();
                return false;
            }
            idx = (const WORD *)p; ibLocked = true;
        }
    }
    int uvo = UvOffset(d.fvf);
    int co = (d.fvf & D3DFVF_DIFFUSE) ? 12 + ((d.fvf & D3DFVF_NORMAL) ? 12 : 0) + ((d.fvf & D3DFVF_PSIZE) ? 4 : 0) : -1;   // couleur "cuite"
    bool hasN = (d.fvf & D3DFVF_NORMAL) && d.stride >= 24;   // normale juste apres la position
    g_pos.resize(n * 3); g_nrm.resize(n * 3); g_uv.resize(n * 2); g_col.resize(n);
    for (UINT i = 0; i < n; i++) {
        const BYTE *v = verts + (size_t)i * d.stride;
        memcpy(&g_pos[i * 3], v, 12);
        if (hasN) memcpy(&g_nrm[i * 3], v + 12, 12);
        else g_nrm[i * 3] = g_nrm[i * 3 + 1] = g_nrm[i * 3 + 2] = 0;
        if (uvo >= 0 && uvo + 8 <= (int)d.stride) memcpy(&g_uv[i * 2], v + uvo, 8);
        else g_uv[i * 2] = g_uv[i * 2 + 1] = 0;
        g_col[i] = co >= 0 && co + 4 <= (int)d.stride ? *(const DWORD *)(v + co) : 0xFFFFFFFF;
    }
    g_tris.clear();
    bool bad = false;
    auto at = [&](UINT j) -> UINT { UINT v = idx ? (UINT)idx[j] - g.minIndex : j; if (v >= n) bad = true; return v < n ? v : 0; };
    for (UINT k = 0; k < g.count && !bad; k++) {
        UINT a, b, c;
        if (g.type == D3DPT_TRIANGLELIST) { a = at(k * 3); b = at(k * 3 + 1); c = at(k * 3 + 2); }
        else if (g.type == D3DPT_TRIANGLESTRIP) { a = at(k); b = at(k + 1); c = at(k + 2); }
        else { a = at(0); b = at(k + 1); c = at(k + 2); }
        if (a == b || b == c || a == c) continue;
        g_tris.push_back(a); g_tris.push_back(b); g_tris.push_back(c);
    }
    if (ibLocked) d.ib->Unlock();
    if (vbLocked) d.vb->Unlock();
    return !bad && !g_tris.empty();
}

static bool WriteMesh(uint32_t id, bool transient)
{
    uint32_t nv = (uint32_t)g_pos.size() / 3, nt = (uint32_t)g_tris.size() / 3;
    BYTE *p = (BYTE *)Cmd(RT_CMD_MESH, sizeof(RtMesh) + nv * RT_VERTEX_BYTES + nt * 12);
    if (!p) return false;
    RtMesh *m = (RtMesh *)p;
    m->id = id; m->vertices = nv; m->triangles = nt; m->flags = transient ? RT_MESH_TRANSIENT : 0;
    BYTE *q = p + sizeof(RtMesh);
    memcpy(q, g_pos.data(), nv * 12); q += nv * 12;
    memcpy(q, g_nrm.data(), nv * 12); q += nv * 12;
    memcpy(q, g_uv.data(), nv * 8); q += nv * 8;
    memcpy(q, g_col.data(), nv * 4); q += nv * 4;
    memcpy(q, g_tris.data(), nt * 12);
    return true;
}

// Texture : niveau d'au plus 256 texels de cote ; DXT tel quel, le reste converti en BGRA 8 bits.
static int WriteTexture(uint32_t id, IDirect3DBaseTexture9 *bt)
{
    if (bt->GetType() != D3DRTYPE_TEXTURE) return ST_BAD;
    IDirect3DTexture9 *t = (IDirect3DTexture9 *)bt;
    DWORD levels = t->GetLevelCount();
    D3DSURFACE_DESC sd;
    if (FAILED(t->GetLevelDesc(0, &sd)) || sd.Pool == D3DPOOL_DEFAULT) return ST_BAD;
    bool bc = sd.Format == D3DFMT_DXT1 || sd.Format == D3DFMT_DXT2 || sd.Format == D3DFMT_DXT3 || sd.Format == D3DFMT_DXT4 || sd.Format == D3DFMT_DXT5;
    UINT level = 0;
    while (level + 1 < levels) {
        D3DSURFACE_DESC nd;
        if (FAILED(t->GetLevelDesc(level, &nd))) return ST_BAD;
        if (nd.Width <= 256 && nd.Height <= 256) break;
        D3DSURFACE_DESC nx;
        if (FAILED(t->GetLevelDesc(level + 1, &nx)) || (bc && (nx.Width < 4 || nx.Height < 4))) break;
        level++;
    }
    if (FAILED(t->GetLevelDesc(level, &sd))) return ST_BAD;
    UINT w = sd.Width, h = sd.Height;
    if (bc && ((w & 3) || (h & 3))) return ST_BAD;
    uint32_t fmt = RT_TEX_BGRA8;
    UINT rows = h, rowBytes = w * 4;
    switch ((DWORD)sd.Format) {
    case D3DFMT_DXT1: fmt = RT_TEX_BC1; rows = h / 4; rowBytes = w / 4 * 8; break;
    case D3DFMT_DXT2: case D3DFMT_DXT3: fmt = RT_TEX_BC2; rows = h / 4; rowBytes = w / 4 * 16; break;
    case D3DFMT_DXT4: case D3DFMT_DXT5: fmt = RT_TEX_BC3; rows = h / 4; rowBytes = w / 4 * 16; break;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_L8: case D3DFMT_A8L8: break;
    default: {   // (palettes et formats rares : sans texture, donc sans test alpha)
        static int n;
        if (n++ < 20) Log("ray tracing : texture au format %lu (%ux%u) non envoyee", (DWORD)sd.Format, w, h);
        return ST_BAD;
    }
    }
    BYTE *p = (BYTE *)Cmd(RT_CMD_TEX, sizeof(RtTex) + rows * rowBytes);
    if (!p) return ST_NEW;   // plus de place dans cette image : a la suivante
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(level, &lr, NULL, D3DLOCK_READONLY))) { g_off -= sizeof(RtCmd) + ((sizeof(RtTex) + rows * rowBytes + 15) & ~15u); return ST_BAD; }
    RtTex *th = (RtTex *)p;
    th->id = id; th->format = fmt; th->width = w; th->height = h;
    BYTE *dst = p + sizeof(RtTex);
    for (UINT y = 0; y < rows; y++) {
        const BYTE *s = (const BYTE *)lr.pBits + (size_t)y * lr.Pitch;
        BYTE *o = dst + (size_t)y * rowBytes;
        switch ((DWORD)sd.Format) {
        case D3DFMT_X8R8G8B8: for (UINT x = 0; x < w; x++) { memcpy(o + x * 4, s + x * 4, 3); o[x * 4 + 3] = 255; } break;
        case D3DFMT_R5G6B5: for (UINT x = 0; x < w; x++) { WORD c = ((const WORD *)s)[x]; o[x * 4] = (BYTE)((c & 31) * 255 / 31); o[x * 4 + 1] = (BYTE)(((c >> 5) & 63) * 255 / 63); o[x * 4 + 2] = (BYTE)((c >> 11) * 255 / 31); o[x * 4 + 3] = 255; } break;
        case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: for (UINT x = 0; x < w; x++) { WORD c = ((const WORD *)s)[x]; o[x * 4] = (BYTE)((c & 31) * 255 / 31); o[x * 4 + 1] = (BYTE)(((c >> 5) & 31) * 255 / 31); o[x * 4 + 2] = (BYTE)(((c >> 10) & 31) * 255 / 31); o[x * 4 + 3] = (sd.Format == D3DFMT_X1R5G5B5 || (c & 0x8000)) ? 255 : 0; } break;
        case D3DFMT_A4R4G4B4: for (UINT x = 0; x < w; x++) { WORD c = ((const WORD *)s)[x]; o[x * 4] = (BYTE)((c & 15) * 17); o[x * 4 + 1] = (BYTE)(((c >> 4) & 15) * 17); o[x * 4 + 2] = (BYTE)(((c >> 8) & 15) * 17); o[x * 4 + 3] = (BYTE)((c >> 12) * 17); } break;
        case D3DFMT_L8: for (UINT x = 0; x < w; x++) { o[x * 4] = o[x * 4 + 1] = o[x * 4 + 2] = s[x]; o[x * 4 + 3] = 255; } break;
        case D3DFMT_A8L8: for (UINT x = 0; x < w; x++) { o[x * 4] = o[x * 4 + 1] = o[x * 4 + 2] = s[x * 2]; o[x * 4 + 3] = s[x * 2 + 1]; } break;
        default: memcpy(o, s, rowBytes); break;   // A8R8G8B8 et blocs DXT : meme disposition
        }
    }
    t->UnlockRect(level);
    return ST_SENT;
}

// ---------------------------------------------------------------- image
static std::vector<RtInstance> g_inst;
static uint32_t g_frameIndex;
static int g_statSent, g_statTexSent, g_statDrawn, g_statSkipped, g_statTimeouts, g_statAlpha, g_statAlphaTex;

static void Collect()
{
    g_pending = false;
    UINT w = g_hdr->outW, h = g_hdr->outH;
    if (!w || !h || w > RT_MAX_OUT_W || h > RT_MAX_OUT_H) return;
    IDirect3DDevice9 *dev = BridgeDevice9();
    if (!dev) return;
    if (!g_result[0] || g_resW != w || g_resH != h) {
        for (auto &t : g_result) if (t) { t->Release(); t = NULL; }
        static const D3DFORMAT fmt[3] = { D3DFMT_A16B16G16R16F, D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8 };
        for (int k = 0; k < 3; k++)
            if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, fmt[k], D3DPOOL_DEFAULT, &g_result[k], NULL))) {
                Log("ray tracing : textures du resultat impossibles (%ux%u)", w, h);
                for (auto &t : g_result) if (t) { t->Release(); t = NULL; }
                g_haveResult = false; return;
            }
        g_resW = w; g_resH = h;
    }
    const BYTE *src = g_base + RT_OUT_OFFSET;
    static const UINT bpp[3] = { 8, 4, 4 };
    for (int k = 0; k < 3; k++) {
        D3DLOCKED_RECT lr;
        if (FAILED(g_result[k]->LockRect(0, &lr, NULL, D3DLOCK_DISCARD))) return;
        for (UINT y = 0; y < h; y++) memcpy((BYTE *)lr.pBits + (size_t)y * lr.Pitch, src + (size_t)y * w * bpp[k], w * bpp[k]);
        g_result[k]->UnlockRect(0);
        src += (size_t)w * h * bpp[k];
    }
    g_haveResult = true;
}

bool RtTrace(const RtDraw *draws, int count, const RtParams &prm)
{
    UINT outW = prm.outW, outH = prm.outH;
    if (!RtReady()) return false;
    RtLock lock;
    if (g_pending) {
        // Image precedente pas encore rendue (au lancement : premiers envois tres gros) : on garde l'ancienne.
        if (g_hdr->doneSeq == g_seq) Collect();
        else if (WaitForSingleObject(g_done, 0) == WAIT_OBJECT_0 || g_hdr->doneSeq == g_seq) Collect();
        else return g_haveResult;
    }
    if (outW > RT_MAX_OUT_W || outH > RT_MAX_OUT_H) {   // (pleine resolution d'un grand ecran : ramenee a 1920x1080)
        float k = min((float)RT_MAX_OUT_W / outW, (float)RT_MAX_OUT_H / outH);
        outW = (UINT)(outW * k); outH = (UINT)(outH * k);
    }
    g_off = 0;
    // Place gardee pour la camera et les dessins ; les envois prennent le reste (au plus 32 Mo par image).
    uint32_t tail = (uint32_t)(sizeof(RtCmd) + sizeof(RtFrame) + (count + 1) * sizeof(RtInstance) + 64);
    g_limit = RT_CMD_SIZE - tail;
    if (g_limit > (32u << 20)) g_limit = 32u << 20;

    for (uint32_t id : g_meshDel) { uint32_t *p = (uint32_t *)Cmd(RT_CMD_MESH_DEL, 4); if (!p) break; *p = id; }
    g_meshDel.clear();
    for (uint32_t id : g_texDel) { uint32_t *p = (uint32_t *)Cmd(RT_CMD_TEX_DEL, 4); if (!p) break; *p = id; }
    g_texDel.clear();

    g_inst.clear();
    int newMeshes = 0;
    uint32_t transientId = 0x80000000u;
    for (int i = 0; i < count; i++) {
        const RtDraw &d = draws[i];
        uint32_t mesh = 0;
        if (d.vbData || d.ibData) {
            // Sommets reecrits par le jeu a chaque image (personnages, eau) : maillage de cette image seulement.
            if (Extract(d) && WriteMesh(transientId, true)) mesh = transientId++;
        } else {
            MeshKey k = { d.vb, d.ib, d.stride, d.fvf, d.d.type, d.d.baseVertex, d.d.minIndex, d.d.numVerts, d.d.start, d.d.count };
            auto it = g_meshCache.find(k);
            if (it == g_meshCache.end()) {
                MeshVal v = { g_nextMesh++, ST_NEW };
                if (g_nextMesh >= 0x7FFFFFF0u) g_nextMesh = 1;
                it = g_meshCache.emplace(k, v).first;
                g_meshKeys[v.id] = k;
                g_bufMeshes[d.vb].push_back(v.id);
                if (d.ib) g_bufMeshes[d.ib].push_back(v.id);
            }
            MeshVal &v = it->second;
            if (v.state == ST_NEW && newMeshes < 3000) {
                if (!Extract(d)) v.state = ST_BAD;
                else if (WriteMesh(v.id, false)) { v.state = ST_SENT; newMeshes++; g_statSent++; }
            }
            if (v.state == ST_SENT) mesh = v.id;
        }
        if (!mesh) { g_statSkipped++; continue; }
        uint32_t tex = 0;
        if (d.tex) {
            auto t = g_texCache.find(d.tex);
            if (t == g_texCache.end()) t = g_texCache.emplace(d.tex, TexVal{ g_nextTex++, ST_NEW }).first;
            if (t->second.state == ST_NEW) { t->second.state = (uint8_t)WriteTexture(t->second.id, d.tex); if (t->second.state == ST_SENT) g_statTexSent++; }
            if (t->second.state == ST_SENT) tex = t->second.id;
        }
        RtInstance in;
        const float *w = d.world;   // Direct3D : v' = v * W (lignes) -> 3x4 en colonnes
        for (int r = 0; r < 3; r++) { in.transform[r * 4 + 0] = w[0 * 4 + r]; in.transform[r * 4 + 1] = w[1 * 4 + r]; in.transform[r * 4 + 2] = w[2 * 4 + r]; in.transform[r * 4 + 3] = w[3 * 4 + r]; }
        in.mesh = mesh; in.tex = tex;
        in.alphaRef = d.alphaRef;
        in.tint = d.tint; in.pad[0] = in.pad[1] = in.pad[2] = 0;
        static int dbg = GetPrivateProfileIntA("VCCoop", "RTDebug", 0, IniPath());   // 1 : test alpha coupe (diagnostic)
        if (dbg == 1) tex = 0;
        in.flags = (d.alphaTest && tex ? RT_INST_ALPHA : 0) | (d.vehicle ? RT_INST_VEHICLE : 0) | (d.dynamic ? RT_INST_DYNAMIC : 0);
        if (d.alphaTest) { g_statAlpha++; if (tex) g_statAlphaTex++; }
        g_inst.push_back(in);
    }

    g_limit = RT_CMD_SIZE;
    RtFrame *f = (RtFrame *)Cmd(RT_CMD_FRAME, (uint32_t)(sizeof(RtFrame) + g_inst.size() * sizeof(RtInstance)));
    if (!f) return g_haveResult;
    memset(f, 0, sizeof *f);
    memcpy(f->view, prm.view, 64); memcpy(f->proj, prm.proj, 64);
    memcpy(f->sun, prm.sun, 16); memcpy(f->sunColor, prm.sunColor, 16); memcpy(f->ambient, prm.ambient, 16);
    memcpy(f->skyTop, prm.skyTop, 16); memcpy(f->skyBottom, prm.skyBottom, 16);
    f->sunAngle = prm.sunAngle;
    f->outW = outW; f->outH = outH;
    f->features = prm.features;
    f->raysPerPixel = g_cfg.rtRays;
    f->frameIndex = g_frameIndex++;
    f->instanceCount = (uint32_t)g_inst.size();
    f->maxDistance = prm.maxDist;
    f->wetness = prm.wetness;
    f->aoRadius = prm.aoRadius;
    f->reset = prm.reset ? 1 : 0;
    if (!g_inst.empty()) memcpy(f + 1, g_inst.data(), g_inst.size() * sizeof(RtInstance));
    g_statDrawn += (int)g_inst.size();

    g_hdr->cmdBytes = g_off;
    MemoryBarrier();
    ResetEvent(g_done);   // (signal reste d'une image rendue en retard : l'attente ne doit pas repartir tout de suite)
    g_hdr->frameSeq = ++g_seq;
    g_pending = true;
    SetEvent(g_go);
    // Attente de l'image (quelques ms) ; trop long (gros envois) : l'image suivante prend la precedente.
    DWORD until = GetTickCount() + (g_haveResult ? 40 : 250);
    while (g_hdr->doneSeq != g_seq) {
        int left = (int)(until - GetTickCount());
        if (left <= 0 || WaitForSingleObject(g_done, left) != WAIT_OBJECT_0) break;
    }
    if (g_hdr->doneSeq == g_seq) Collect();
    else g_statTimeouts++;

    static DWORD lastLog;
    if (GetTickCount() - lastLog > 10000) {
        lastLog = GetTickCount();
        Log("ray tracing : %d dessins envoyes (%d sans maillage, %d avec test alpha dont %d avec texture), %d maillages et %d textures nouveaux, %d en retard ; vcrt64 : %u maillages, %u textures, %.2f ms, image %ux%u",
            g_statDrawn, g_statSkipped, g_statAlpha, g_statAlphaTex, g_statSent, g_statTexSent, g_statTimeouts, g_hdr->meshCount, g_hdr->texCount, g_hdr->gpuMs, g_hdr->outW, g_hdr->outH);
        g_statDrawn = g_statSkipped = g_statSent = g_statTexSent = g_statTimeouts = g_statAlpha = g_statAlphaTex = 0;
    }
    return g_haveResult;
}
