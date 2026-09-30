// VCCoop - lanceur : lecture des modeles des personnages (RenderWare 3.x : DFF / TXD dans gta3.img) et rendu logiciel.
// Voir model3d.h. Formats d'apres la documentation publique de RenderWare (sections Clump, FrameList, Geometry,
// Material, Texture Native) ; pose d'origine du modele (sans animation).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "model3d.h"
#include <map>
#include <algorithm>
#include <math.h>
#include <string.h>
#include <stdio.h>

using std::min;
using std::max;

namespace {

struct Entry { uint32_t off, size; };            // en secteurs de 2048 octets
std::wstring g_dir;
std::map<std::string, Entry> g_img;              // nom du fichier en minuscules -> place dans gta3.img
std::vector<std::string> g_dffOrder;             // modeles dans l'ordre de gta3.dir
std::map<std::string, std::wstring> g_mods;      // VCCoop\mods : nom en minuscules -> chemin
std::vector<std::wstring> g_packImgs;            // packs de vehicules (vccpkN.img a la racine du jeu, voir mods.cpp du mod)
std::map<std::string, std::pair<int, Entry>> g_pack;   // nom -> (archive, place) ; le dernier pack l'emporte
std::vector<std::string> g_pedNames;             // default.ide, section peds : index = numero du modele
std::map<std::string, float> g_wheelScale;       // default.ide, section cars : taille des roues de chaque vehicule

std::string Lower(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }
std::string Narrow(const std::wstring &w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

bool ReadRange(const std::wstring &path, uint64_t off, uint32_t size, std::vector<uint8_t> &out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    if (size == 0xFFFFFFFF) { LARGE_INTEGER s; GetFileSizeEx(f, &s); size = (uint32_t)min<LONGLONG>(s.QuadPart, 64 << 20); }
    LARGE_INTEGER p; p.QuadPart = (LONGLONG)off;
    out.resize(size);
    DWORD got = 0;
    bool ok = SetFilePointerEx(f, p, NULL, FILE_BEGIN) && (size == 0 || ReadFile(f, out.data(), size, &got, NULL));
    CloseHandle(f);
    out.resize(got);
    return ok;
}

void ScanMods(const std::wstring &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ScanMods(dir + n + L"\\"); continue; }
        std::string a = Lower(Narrow(n));
        if (a.size() > 4 && (a.compare(a.size() - 4, 4, ".dff") == 0 || a.compare(a.size() - 4, 4, ".txd") == 0) && !g_mods.count(a)) g_mods[a] = dir + n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool GetFile(const std::string &name, std::vector<uint8_t> &out)
{
    std::string n = Lower(name);
    auto m = g_mods.find(n);
    if (m != g_mods.end()) return ReadRange(m->second, 0, 0xFFFFFFFF, out);
    auto p = g_pack.find(n);
    if (p != g_pack.end()) return ReadRange(g_packImgs[p->second.first], (uint64_t)p->second.second.off * 2048, p->second.second.size * 2048, out);
    auto e = g_img.find(n);
    if (e == g_img.end()) return false;
    return ReadRange(g_dir + L"models\\gta3.img", (uint64_t)e->second.off * 2048, e->second.size * 2048, out);
}

// --- flux RenderWare
struct Chunk { uint32_t type, size, lib; };
uint32_t Ver(uint32_t lib) { return (lib & 0xFFFF0000) ? (((lib >> 14) & 0x3FF00) + 0x30000) | ((lib >> 16) & 0x3F) : lib << 8; }

struct Rd {
    const uint8_t *p, *e;
    bool ok = true;
    Rd(const uint8_t *a, const uint8_t *b) : p(a), e(b) {}
    bool left(size_t n) const { return ok && (size_t)(e - p) >= n; }
    bool chunk(Chunk &c) { if (!left(12)) { ok = false; return false; } memcpy(&c, p, 12); p += 12; if ((size_t)(e - p) < c.size) { ok = false; return false; } return true; }
    uint32_t u32() { if (!left(4)) { ok = false; return 0; } uint32_t v; memcpy(&v, p, 4); p += 4; return v; }
    uint16_t u16() { if (!left(2)) { ok = false; return 0; } uint16_t v; memcpy(&v, p, 2); p += 2; return v; }
    uint8_t u8() { if (!left(1)) { ok = false; return 0; } return *p++; }
    float f32() { uint32_t v = u32(); float f; memcpy(&f, &v, 4); return f; }
    void skip(size_t n) { if (!left(n)) { ok = false; p = e; } else p += n; }
};

struct Tex { std::string name; int w = 0, h = 0; std::vector<uint32_t> px; };   // ARGB (non premultiplie)
struct Mat { uint8_t r = 255, g = 255, b = 255, a = 255; std::string tex; int ti = -1; };
struct Geo {
    std::vector<float> pos, nrm, uv;
    std::vector<int> tri;                         // 3 sommets + materiau
    std::vector<Mat> mats;
    bool skinned = false;
    int numBones = 0;
    std::vector<uint8_t> bidx;                    // Skin PLG : 4 os par sommet
    std::vector<float> bw;                        // et leurs poids
    std::vector<float> inv;                       // matrice inverse de la pose d'origine de chaque os (12 floats)
};
struct Frame { float m[12]; int parent; std::string name; int hanim = -1; };   // droite, haut, avant, position

// Matrices "lignes" de RenderWare (p' = x*droite + y*haut + z*avant + position) : Mul(A, B) = A puis B.
void Mul(const float *A, const float *B, float *C)
{
    float N[12];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) N[i * 3 + j] = A[i * 3 + 0] * B[j] + A[i * 3 + 1] * B[3 + j] + A[i * 3 + 2] * B[6 + j];
    for (int j = 0; j < 3; j++) N[9 + j] = A[9] * B[j] + A[10] * B[3 + j] + A[11] * B[6 + j] + B[9 + j];
    memcpy(C, N, sizeof(N));
}
void Apply(const float *M, const float *p, float *o, bool point)
{
    for (int j = 0; j < 3; j++) o[j] = p[0] * M[j] + p[1] * M[3 + j] + p[2] * M[6 + j] + (point ? M[9 + j] : 0);
}

std::string ReadString(Rd &r)
{
    Chunk c;
    if (!r.chunk(c)) return "";
    std::string s((const char *)r.p, strnlen((const char *)r.p, c.size));
    r.skip(c.size);
    return s;
}

bool ParseMaterial(Rd &r, uint32_t ver, Mat &m)
{
    Chunk st;
    if (!r.chunk(st) || st.type != 1) return false;
    Rd s(r.p, r.p + st.size);
    s.u32();
    m.r = s.u8(); m.g = s.u8(); m.b = s.u8(); m.a = s.u8();
    s.u32();
    uint32_t textured = s.u32();
    r.skip(st.size);
    (void)ver;
    if (textured) {
        Chunk tc;
        if (!r.chunk(tc)) return false;
        Rd t(r.p, r.p + tc.size);
        Chunk ts;
        if (t.chunk(ts)) { t.skip(ts.size); m.tex = Lower(ReadString(t)); }
        r.skip(tc.size);
    }
    return r.ok;
}

bool ParseGeometry(Rd &r, uint32_t ver, Geo &g)
{
    Chunk st;
    if (!r.chunk(st) || st.type != 1) return false;
    Rd s(r.p, r.p + st.size);
    uint32_t flags = s.u32(), numTris = s.u32(), numVerts = s.u32(), numMorph = s.u32();
    if (numVerts > 200000 || numTris > 400000) return false;
    if (ver < 0x34000) s.skip(12);
    std::vector<float> uv;
    std::vector<int> tri;
    if (!(flags & 0x01000000)) {
        if (flags & 8) s.skip(numVerts * 4);
        int numUV = (flags >> 16) & 0xFF;
        if (!numUV) numUV = (flags & 0x80) ? 2 : (flags & 4) ? 1 : 0;
        for (int k = 0; k < numUV; k++) {
            if (k == 0 && s.left(numVerts * 8)) { uv.resize(numVerts * 2); memcpy(uv.data(), s.p, numVerts * 8); }
            s.skip(numVerts * 8);
        }
        if (!s.left(numTris * 8)) return false;
        tri.resize(numTris * 4);
        for (uint32_t i = 0; i < numTris; i++) {
            uint16_t a[4];
            memcpy(a, s.p + i * 8, 8);
            tri[i * 4 + 0] = a[1]; tri[i * 4 + 1] = a[0]; tri[i * 4 + 2] = a[3]; tri[i * 4 + 3] = a[2];   // (v2, v1, materiau, v3)
        }
        s.skip(numTris * 8);
    }
    for (uint32_t k = 0; k < numMorph && s.ok; k++) {
        s.skip(16);
        uint32_t hasV = s.u32(), hasN = s.u32();
        if (hasV) { if (k == 0 && s.left(numVerts * 12)) { g.pos.resize(numVerts * 3); memcpy(g.pos.data(), s.p, numVerts * 12); } s.skip(numVerts * 12); }
        if (hasN) { if (k == 0 && s.left(numVerts * 12)) { g.nrm.resize(numVerts * 3); memcpy(g.nrm.data(), s.p, numVerts * 12); } s.skip(numVerts * 12); }
    }
    r.skip(st.size);
    if (g.pos.size() != numVerts * 3) return false;
    g.uv = uv.empty() ? std::vector<float>(numVerts * 2, 0.0f) : uv;
    for (size_t i = 0; i < tri.size(); i += 4)
        if ((uint32_t)tri[i] < numVerts && (uint32_t)tri[i + 1] < numVerts && (uint32_t)tri[i + 2] < numVerts)
            g.tri.insert(g.tri.end(), tri.begin() + i, tri.begin() + i + 4);
    // liste des materiaux, puis extensions (Skin PLG 0x116 : modele a squelette, sommets deja dans la pose d'origine)
    while (r.left(12)) {
        Chunk c;
        if (!r.chunk(c)) break;
        Rd sub(r.p, r.p + c.size);
        if (c.type == 0x08) {
            Chunk ms;
            if (sub.chunk(ms)) {
                Rd m(sub.p, sub.p + ms.size);
                uint32_t n = m.u32();
                std::vector<int32_t> idx;
                for (uint32_t i = 0; i < n && m.ok && i < 4096; i++) idx.push_back((int32_t)m.u32());
                sub.skip(ms.size);
                for (uint32_t i = 0; i < idx.size(); i++) {
                    Mat mat;
                    if (idx[i] >= 0 && idx[i] < (int)g.mats.size()) mat = g.mats[idx[i]];
                    else {
                        Chunk mc;
                        if (!sub.chunk(mc)) break;
                        Rd mr(sub.p, sub.p + mc.size);
                        ParseMaterial(mr, ver, mat);
                        sub.skip(mc.size);
                    }
                    g.mats.push_back(mat);
                }
            }
        } else if (c.type == 0x03) {
            while (sub.left(12)) {
                Chunk e;
                if (!sub.chunk(e)) break;
                if (e.type == 0x50E) {
                    // Bin Mesh : les triangles groupes par materiau, tels que le jeu les dessine (listes ou bandes). Certains
                    // exports (mods convertis) ne mettent le vrai materiau qu'ici (la liste de la structure dit 0 partout).
                    Rd k(sub.p, sub.p + e.size);
                    uint32_t strip = k.u32(), meshes = k.u32();
                    k.u32();
                    size_t nv = g.pos.size() / 3;
                    std::vector<int> tris;
                    bool ok = meshes > 0 && meshes < 4096;
                    for (uint32_t mI = 0; ok && mI < meshes; mI++) {
                        uint32_t n = k.u32(), mat = k.u32();
                        if (!k.left((size_t)n * 4)) { ok = false; break; }
                        std::vector<uint32_t> idx(n);
                        memcpy(idx.data(), k.p, (size_t)n * 4);
                        k.skip((size_t)n * 4);
                        for (uint32_t v : idx) if (v >= nv) ok = false;
                        if (!ok) break;
                        if (strip & 1) {
                            for (uint32_t i = 2; i < n; i++) {
                                uint32_t a0 = idx[i - 2], a1 = idx[i - 1], a2 = idx[i];
                                if (a0 == a1 || a1 == a2 || a0 == a2) continue;   // degeneres (jonctions)
                                if (i & 1) tris.insert(tris.end(), { (int)a1, (int)a0, (int)a2, (int)mat });
                                else tris.insert(tris.end(), { (int)a0, (int)a1, (int)a2, (int)mat });
                            }
                        } else for (uint32_t i = 0; i + 2 < n; i += 3) tris.insert(tris.end(), { (int)idx[i], (int)idx[i + 1], (int)idx[i + 2], (int)mat });
                    }
                    if (ok && !tris.empty()) g.tri.swap(tris);
                }
                if (e.type == 0x116) {
                    g.skinned = true;
                    Rd k(sub.p, sub.p + e.size);
                    int nb = k.u8(), used = k.u8();
                    k.u8(); k.u8();
                    k.skip(used);
                    size_t nv = g.pos.size() / 3;
                    if (nb > 0 && nb <= 128 && k.left(nv * 20)) {
                        g.bidx.assign(k.p, k.p + nv * 4);
                        k.skip(nv * 4);
                        g.bw.resize(nv * 4);
                        memcpy(g.bw.data(), k.p, nv * 16);
                        k.skip(nv * 16);
                        size_t rest = (size_t)(k.e - k.p), per = rest >= (size_t)nb * 68 ? 68 : 64;   // GTA : 4 octets avant chaque matrice
                        for (int b = 0; b < nb && k.left(per); b++) {
                            if (per == 68) k.u32();
                            float m16[16];
                            memcpy(m16, k.p, 64);
                            k.skip(64);
                            for (int r = 0; r < 4; r++) for (int c = 0; c < 3; c++) g.inv.push_back(m16[r * 4 + c]);
                        }
                        if ((int)g.inv.size() == nb * 12) g.numBones = nb; else { g.bidx.clear(); g.bw.clear(); g.inv.clear(); }
                    }
                }
                sub.skip(e.size);
            }
        }
        r.skip(c.size);
    }
    return true;
}

// --- textures
uint32_t ARGB(int a, int r, int g, int b) { return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; }
uint32_t C565(uint16_t c) { int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31; return ARGB(255, r * 255 / 31, g * 255 / 63, b * 255 / 31); }

void DecodeDxt(const uint8_t *d, size_t len, int w, int h, int kind, std::vector<uint32_t> &px)
{
    px.assign((size_t)w * h, 0);
    int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = kind == 1 ? 8 : 16;
    if (len < (size_t)bw * bh * bs) return;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const uint8_t *b = d + (size_t)(by * bw + bx) * bs;
            const uint8_t *cb = kind == 1 ? b : b + 8;
            uint16_t c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
            uint32_t col[4] = { C565(c0), C565(c1), 0, 0 };
            int r0 = (col[0] >> 16) & 255, g0 = (col[0] >> 8) & 255, b0 = col[0] & 255;
            int r1 = (col[1] >> 16) & 255, g1 = (col[1] >> 8) & 255, b1 = col[1] & 255;
            if (c0 > c1 || kind != 1) {
                col[2] = ARGB(255, (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3);
                col[3] = ARGB(255, (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3);
            } else {
                col[2] = ARGB(255, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2);
                col[3] = 0;   // transparent
            }
            uint32_t bits = cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24;
            uint8_t alpha[16];
            if (kind == 3) for (int i = 0; i < 16; i++) { int v = (b[i / 2] >> ((i & 1) * 4)) & 15; alpha[i] = (uint8_t)(v * 17); }
            else if (kind == 5) {
                int a0 = b[0], a1 = b[1];
                uint64_t ab = 0;
                for (int i = 0; i < 6; i++) ab |= (uint64_t)b[2 + i] << (8 * i);
                for (int i = 0; i < 16; i++) {
                    int code = (int)((ab >> (3 * i)) & 7), a;
                    if (code == 0) a = a0; else if (code == 1) a = a1;
                    else if (a0 > a1) a = ((8 - code) * a0 + (code - 1) * a1) / 7;
                    else a = code == 6 ? 0 : code == 7 ? 255 : ((6 - code) * a0 + (code - 1) * a1) / 5;
                    alpha[i] = (uint8_t)a;
                }
            }
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                uint32_t c = col[(bits >> (2 * i)) & 3];
                if (kind != 1) c = (c & 0xFFFFFF) | (uint32_t)alpha[i] << 24;
                px[(size_t)y * w + x] = c;
            }
        }
}

bool ParseTexNative(Rd &r, Tex &t)
{
    Chunk st;
    if (!r.chunk(st) || st.type != 1) return false;
    Rd s(r.p, r.p + st.size);
    uint32_t platform = s.u32();
    if (platform != 8 && platform != 9) return false;
    s.u32();
    char name[33] = {};
    memcpy(name, s.p, min<size_t>(32, (size_t)(s.e - s.p)));
    s.skip(64);
    t.name = Lower(name);
    uint32_t raster = s.u32(), fmt = s.u32();
    int w = s.u16(), h = s.u16();
    int depth = s.u8();
    s.u8(); s.u8();
    int comp = s.u8();
    (void)depth;
    if (!s.ok || w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    int dxt = 0;
    if (platform == 8) dxt = comp == 1 ? 1 : comp == 3 ? 3 : comp == 5 ? 5 : 0;
    else if (comp & 8) dxt = fmt == 0x31545844 ? 1 : fmt == 0x33545844 ? 3 : fmt == 0x35545844 ? 5 : 0;   // 'DXT1' 'DXT3' 'DXT5'
    std::vector<uint32_t> pal;
    if (raster & 0x2000) { for (int i = 0; i < 256; i++) { uint8_t c[4] = { s.u8(), s.u8(), s.u8(), s.u8() }; pal.push_back(ARGB(c[3], c[0], c[1], c[2])); } }
    else if (raster & 0x4000) { for (int i = 0; i < 16; i++) { uint8_t c[4] = { s.u8(), s.u8(), s.u8(), s.u8() }; pal.push_back(ARGB(c[3], c[0], c[1], c[2])); } }
    uint32_t size = s.u32();
    if (!s.ok || !s.left(size)) return false;
    const uint8_t *d = s.p;
    t.w = w; t.h = h;
    if (dxt) DecodeDxt(d, size, w, h, dxt, t.px);
    else {
        t.px.assign((size_t)w * h, 0xFFFFFFFF);
        int kind = raster & 0xF00;
        size_t n = (size_t)w * h;
        for (size_t i = 0; i < n; i++) {
            uint32_t c = 0xFFFF00FF;
            if (!pal.empty() && (raster & 0x2000)) { if (i < size) c = pal[d[i]]; }
            else if (!pal.empty()) { if (i / 2 < size) c = pal[(d[i / 2] >> ((i & 1) * 4)) & 15]; }
            else if (kind == 0x500 || kind == 0x600) { if (i * 4 + 3 < size) c = ARGB(kind == 0x600 ? 255 : d[i * 4 + 3], d[i * 4 + 2], d[i * 4 + 1], d[i * 4]); }
            else if (i * 2 + 1 < size) {
                uint16_t v = d[i * 2] | d[i * 2 + 1] << 8;
                if (kind == 0x200) c = C565(v);
                else if (kind == 0x300) c = ARGB(((v >> 12) & 15) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17);
                else c = ARGB((kind == 0x100 && !(v & 0x8000)) ? 0 : 255, ((v >> 10) & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, (v & 31) * 255 / 31);
            }
            t.px[i] = c;
        }
    }
    return true;
}

bool ParseTxd(const std::vector<uint8_t> &data, std::vector<Tex> &out)
{
    Rd r(data.data(), data.data() + data.size());
    Chunk c;
    if (!r.chunk(c) || c.type != 0x16) return false;
    Rd b(r.p, r.p + c.size);
    Chunk st;
    if (!b.chunk(st)) return false;
    b.skip(st.size);
    while (b.left(12)) {
        Chunk tc;
        if (!b.chunk(tc)) break;
        if (tc.type == 0x15) { Rd t(b.p, b.p + tc.size); Tex tex; if (ParseTexNative(t, tex)) out.push_back(std::move(tex)); }
        b.skip(tc.size);
    }
    return !out.empty();
}

}   // namespace

struct Model3D {
    std::vector<float> pos, nrm, uv;
    std::vector<int> tri;       // 3 sommets + materiau
    std::vector<Mat> mats;
    std::vector<Tex> texs;
    float lo[3], hi[3];
};

bool ImgOpen(const std::wstring &gameDir)
{
    g_dir = gameDir;
    g_img.clear(); g_dffOrder.clear(); g_mods.clear(); g_pedNames.clear(); g_wheelScale.clear();
    std::vector<uint8_t> d;
    if (!ReadRange(gameDir + L"models\\gta3.dir", 0, 0xFFFFFFFF, d)) return false;
    for (size_t i = 0; i + 32 <= d.size(); i += 32) {
        Entry e;
        memcpy(&e.off, &d[i], 4);
        memcpy(&e.size, &d[i + 4], 4);
        char name[25] = {};
        memcpy(name, &d[i + 8], 24);
        std::string n = Lower(name);
        if (!g_img.count(n)) {
            g_img[n] = e;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".dff") == 0) g_dffOrder.push_back(n.substr(0, n.size() - 4));
        }
    }
    ScanMods(gameDir + L"VCCoop\\mods\\");
    // Packs de vehicules : archives vccpkN.img / .dir posees a la racine du jeu par le mod (en jeu seulement).
    g_packImgs.clear(); g_pack.clear();
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((gameDir + L"vccpk*.dir").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring dir = gameDir + fd.cFileName, img = dir.substr(0, dir.size() - 4) + L".img";
                std::vector<uint8_t> pd;
                if (GetFileAttributesW(img.c_str()) == INVALID_FILE_ATTRIBUTES || !ReadRange(dir, 0, 0xFFFFFFFF, pd)) continue;
                int k = (int)g_packImgs.size();
                g_packImgs.push_back(img);
                for (size_t i = 0; i + 32 <= pd.size(); i += 32) {
                    Entry e;
                    memcpy(&e.off, &pd[i], 4);
                    memcpy(&e.size, &pd[i + 4], 4);
                    char name[25] = {};
                    memcpy(name, &pd[i + 8], 24);
                    g_pack[Lower(name)] = { k, e };
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    // default.ide, section peds : "numero, nom, txd, ..."
    std::vector<uint8_t> ide;
    if (ReadRange(gameDir + L"data\\default.ide", 0, 0xFFFFFFFF, ide)) {
        std::string text(ide.begin(), ide.end());
        // cars : "... , roue, echelle des roues" (deux derniers champs)
        {
            bool inCars = false;
            size_t q = 0;
            while (q < text.size()) {
                size_t e = text.find('\n', q);
                if (e == std::string::npos) e = text.size();
                std::string line = text.substr(q, e - q);
                q = e + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
                std::string low = Lower(line);
                if (!inCars) { if (low == "cars") inCars = true; continue; }
                if (low == "end") break;
                if (line.empty() || line[0] == '#') continue;
                std::vector<std::string> f;
                size_t a = 0;
                while (a <= line.size()) { size_t c = line.find(',', a); if (c == std::string::npos) c = line.size(); std::string v = line.substr(a, c - a); v.erase(0, v.find_first_not_of(" \t")); v.erase(v.find_last_not_of(" \t") + 1); f.push_back(v); a = c + 1; }
                if (f.size() >= 13) { float sc = (float)atof(f.back().c_str()); if (sc > 0.1f && sc < 3.0f) g_wheelScale[Lower(f[1])] = sc; }
            }
        }
        bool in = false;
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(p, e - p);
            p = e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            std::string low = Lower(line);
            if (!in) { if (low == "peds") in = true; continue; }
            if (low == "end") break;
            if (line.empty() || line[0] == '#') continue;
            int id = atoi(line.c_str());
            size_t c1 = line.find(','), c2 = c1 == std::string::npos ? c1 : line.find(',', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos || id < 0 || id > 400) continue;
            std::string name = line.substr(c1 + 1, c2 - c1 - 1);
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            if ((int)g_pedNames.size() <= id) g_pedNames.resize(id + 1);
            g_pedNames[id] = Lower(name);
        }
    }
    return !g_img.empty();
}

std::vector<std::string> SkinList()
{
    std::vector<std::string> out;
    auto has = [](const std::string &n) { return g_img.count(n + ".dff") && g_img.count(n + ".txd"); };
    auto add = [&](const std::string &n) {
        if (out.size() >= 160 || n.empty() || !has(n)) return;
        for (auto &o : out) if (o == n) return;
        out.push_back(n);
    };
    add("player");
    for (int i = 1; i <= 12; i++) add("play" + std::to_string(i));
    for (auto &n : g_dffOrder) if (n.compare(0, 2, "ig") == 0) add(n);
    for (int m = 1; m <= 108 && m < (int)g_pedNames.size(); m++) add(g_pedNames[m]);
    return out;
}

static Model3D *ModelFromData(const std::vector<uint8_t> &dff, const std::vector<uint8_t> &txd, const std::string &modelName)
{
    auto ws = g_wheelScale.find(Lower(modelName));
    float wheelScale = ws != g_wheelScale.end() ? ws->second : 0.7f;
    Rd r(dff.data(), dff.data() + dff.size());
    Chunk clump;
    if (!r.chunk(clump) || clump.type != 0x10) return NULL;
    uint32_t ver = Ver(clump.lib);
    Rd c(r.p, r.p + clump.size);
    std::vector<Frame> frames;
    std::vector<Geo> geos;
    std::vector<std::pair<int, int>> atomics;   // (cadre, geometrie)
    std::vector<int> hier;                       // os n -> numero HAnim
    while (c.left(12)) {
        Chunk k;
        if (!c.chunk(k)) break;
        Rd s(c.p, c.p + k.size);
        if (k.type == 0x0E) {
            Chunk st;
            if (s.chunk(st)) {
                Rd f(s.p, s.p + st.size);
                uint32_t n = f.u32();
                for (uint32_t i = 0; i < n && f.left(56) && i < 512; i++) {
                    Frame fr;
                    for (int j = 0; j < 12; j++) fr.m[j] = f.f32();
                    fr.parent = (int)f.u32();
                    f.u32();
                    frames.push_back(fr);
                }
                s.skip(st.size);
                // une extension par cadre : nom (0x253F2FE) et HAnim (0x11E : numero de l'os ; le premier porte la liste)
                for (size_t i = 0; i < frames.size() && s.left(12); i++) {
                    Chunk ec;
                    if (!s.chunk(ec)) break;
                    Rd e(s.p, s.p + ec.size);
                    while (e.left(12)) {
                        Chunk x;
                        if (!e.chunk(x)) break;
                        if (x.type == 0x253F2FE) frames[i].name = Lower(std::string((const char *)e.p, strnlen((const char *)e.p, x.size)));
                        else if (x.type == 0x11E) {
                            Rd h(e.p, e.p + x.size);
                            h.u32();
                            frames[i].hanim = (int)h.u32();
                            uint32_t nodes = h.u32();
                            if (nodes > 0 && nodes < 256) {
                                h.u32(); h.u32();
                                hier.clear();
                                for (uint32_t j = 0; j < nodes && h.left(12); j++) { hier.push_back((int)h.u32()); h.u32(); h.u32(); }
                            }
                        }
                        e.skip(x.size);
                    }
                    s.skip(ec.size);
                }
            }
        } else if (k.type == 0x1A) {
            Chunk st;
            if (s.chunk(st)) {
                s.skip(st.size);
                while (s.left(12)) {
                    Chunk gc;
                    if (!s.chunk(gc)) break;
                    if (gc.type == 0x0F) { Rd g(s.p, s.p + gc.size); Geo geo; if (!ParseGeometry(g, ver, geo)) geo = Geo(); geos.push_back(std::move(geo)); }
                    s.skip(gc.size);
                }
            }
        } else if (k.type == 0x14) {
            Chunk st;
            if (s.chunk(st)) { Rd a(s.p, s.p + st.size); int fi = (int)a.u32(), gi = (int)a.u32(); atomics.push_back({ fi, gi }); }
        }
        c.skip(k.size);
    }
    // Matrices des cadres dans la pose d'origine, bras baisses le long du corps (rotation a l'epaule autour de l'axe
    // avant, Z dans cet espace ou la verticale est Y) : l'apercu n'est pas en croix.
    std::vector<float> ltm(frames.size() * 12);
    for (size_t i = 0; i < frames.size(); i++) {
        float *M = &ltm[i * 12];
        int par = frames[i].parent;
        if (par >= 0 && par < (int)i) Mul(frames[i].m, &ltm[par * 12], M);
        else memcpy(M, frames[i].m, 48);
        if (frames[i].name.find("upperarm") != std::string::npos) {
            float dir = M[0] >= 0 ? 1.0f : -1.0f, a = -1.2f * dir, c = cosf(a), sn = sinf(a);
            float j[3] = { M[9], M[10], M[11] };
            float T1[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, -j[0], -j[1], -j[2] };
            float R[12] = { c, sn, 0, -sn, c, 0, 0, 0, 1, 0, 0, 0 };
            float T2[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, j[0], j[1], j[2] };
            float W[12];
            Mul(T1, R, W);
            Mul(W, T2, W);
            Mul(M, W, M);
        }
    }
    auto boneFrame = [&](int b) {
        if (b < 0 || b >= (int)hier.size()) return -1;
        for (size_t i = 0; i < frames.size(); i++) if (frames[i].hanim == hier[b]) return (int)i;
        return -1;
    };

    Model3D *m = new Model3D();
    ParseTxd(txd, m->texs);
    bool haveWheel = false;
    for (auto &at : atomics) {
        if (at.second < 0 || at.second >= (int)geos.size()) continue;
        Geo &g = geos[at.second];
        if (g.pos.empty()) continue;
        if (at.first >= 0 && at.first < (int)frames.size()) {   // vehicules : pas les pieces abimees ni la version lointaine
            const std::string &fn = frames[at.first].name;
            if (fn.find("_dam") != std::string::npos || fn.find("_vlo") != std::string::npos) continue;
            if (!fn.compare(0, 5, "wheel") && fn.find("dummy") == std::string::npos) haveWheel = true;
        }
        {   // pieces aux coordonnees absurdes (restes d'un outil d'export dans certains mods) : ignorees, sinon la boite
            // englobante part a l'infini et le modele n'est plus cadre
            bool bad = false;
            for (float v : g.pos) if (!(v == v) || fabsf(v) > 500.0f) { bad = true; break; }
            if (bad) continue;
        }
        // matrice du cadre (et de ses parents), sauf pour un modele a squelette : sommets deja places
        float M[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
        // Squelette : chaque sommet suit ses os (poids x matrice inverse de la pose d'origine x matrice de l'os).
        std::vector<float> skin;   // (os -> matrice finale), vide si pas de squelette lisible
        if (g.skinned && g.numBones > 0) {
            skin.resize(g.numBones * 12);
            for (int b = 0; b < g.numBones; b++) {
                int f = boneFrame(b);
                if (f < 0) { skin.clear(); break; }
                Mul(&g.inv[b * 12], &ltm[f * 12], &skin[b * 12]);
            }
        }
        // Espace des personnages de GTA : verticale Y, visage vers -Z ; remis a la verticale Z, visage vers +Y (camera).
        static const float kUpright[12] = { -1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0 };
        if (g.skinned && skin.empty()) memcpy(M, kUpright, sizeof(M));   // secours : sans squelette lisible
        // Roues du modele (wheel_lf...) : comme le jeu, a l'echelle des roues du vehicule (default.ide) ; celles de gauche
        // tournees d'un demi-tour (le modele de roue est fait pour la droite).
        if (!g.skinned && at.first >= 0 && at.first < (int)frames.size()) {
            const std::string &fn = frames[at.first].name;
            if (!fn.compare(0, 6, "wheel_") && fn.find("dummy") == std::string::npos) {
                float k = wheelScale, flip = (fn.size() > 6 && fn[6] == 'l') ? -1.0f : 1.0f;
                float W[12] = { k * flip, 0, 0, 0, k * flip, 0, 0, 0, k, 0, 0, 0 };
                memcpy(M, W, sizeof(M));
            }
        }
        if (!g.skinned) {
            for (int f = at.first, guard = 0; f >= 0 && f < (int)frames.size() && guard < 64; f = frames[f].parent, guard++) {
                const float *L = frames[f].m;
                float N[12];
                for (int i = 0; i < 3; i++)   // N = L * M (M d'abord dans le repere du cadre, puis le cadre)
                    for (int j = 0; j < 3; j++) N[i * 3 + j] = M[i * 3 + 0] * L[0 * 3 + j] + M[i * 3 + 1] * L[1 * 3 + j] + M[i * 3 + 2] * L[2 * 3 + j];
                for (int j = 0; j < 3; j++) N[9 + j] = M[9] * L[j] + M[10] * L[3 + j] + M[11] * L[6 + j] + L[9 + j];
                memcpy(M, N, sizeof(M));
            }
        }
        int base = (int)m->pos.size() / 3, matBase = (int)m->mats.size();
        size_t nv = g.pos.size() / 3;
        bool hasN = g.nrm.size() == g.pos.size();
        std::vector<float> nrm(g.pos.size(), 0.0f);
        if (!hasN) {   // normales des faces
            for (size_t t = 0; t < g.tri.size(); t += 4) {
                const float *a = &g.pos[g.tri[t] * 3], *b = &g.pos[g.tri[t + 1] * 3], *cc = &g.pos[g.tri[t + 2] * 3];
                float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { cc[0] - a[0], cc[1] - a[1], cc[2] - a[2] };
                float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
                for (int k = 0; k < 3; k++) for (int j = 0; j < 3; j++) nrm[g.tri[t + k] * 3 + j] += n[j];
            }
        }
        for (size_t i = 0; i < nv; i++) {
            const float *p = &g.pos[i * 3], *n = hasN ? &g.nrm[i * 3] : &nrm[i * 3];
            float o[3], q[3];
            if (!skin.empty()) {
                float acc[3] = { 0, 0, 0 }, nacc[3] = { 0, 0, 0 }, wsum = 0;
                for (int k = 0; k < 4; k++) {
                    float w = g.bw[i * 4 + k];
                    int b = g.bidx[i * 4 + k];
                    if (w <= 0 || b >= g.numBones) continue;
                    float t[3], tn[3];
                    Apply(&skin[b * 12], p, t, true);
                    Apply(&skin[b * 12], n, tn, false);
                    for (int j = 0; j < 3; j++) { acc[j] += t[j] * w; nacc[j] += tn[j] * w; }
                    wsum += w;
                }
                if (wsum < 1e-4f) { acc[0] = p[0]; acc[1] = p[1]; acc[2] = p[2]; memcpy(nacc, n, 12); wsum = 1; }
                for (int j = 0; j < 3; j++) acc[j] /= wsum;
                Apply(kUpright, acc, o, true);
                Apply(kUpright, nacc, q, false);
            } else { Apply(M, p, o, true); Apply(M, n, q, false); }
            for (int j = 0; j < 3; j++) m->pos.push_back(o[j]);
            float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
            if (l < 1e-6f) l = 1;
            for (int j = 0; j < 3; j++) m->nrm.push_back(q[j] / l);
            m->uv.push_back(g.uv[i * 2]);
            m->uv.push_back(g.uv[i * 2 + 1]);
        }
        for (auto &mt : g.mats) {
            Mat x = mt;
            for (int t = 0; t < (int)m->texs.size(); t++) if (m->texs[t].name == x.tex) x.ti = t;
            // couleurs "a peindre" des vehicules (le jeu les remplace par celles de carcols.dat) : peinture neutre
            if (x.r == 60 && x.g == 255 && x.b == 0) { x.r = 205; x.g = 208; x.b = 216; }
            else if (x.r == 255 && x.g == 0 && x.b == 175) { x.r = 168; x.g = 170; x.b = 182; }
            m->mats.push_back(x);
        }
        for (size_t t = 0; t < g.tri.size(); t += 4) {
            m->tri.push_back(base + g.tri[t]); m->tri.push_back(base + g.tri[t + 1]); m->tri.push_back(base + g.tri[t + 2]);
            int mi = g.tri[t + 3];
            m->tri.push_back(mi >= 0 && mi < (int)g.mats.size() ? matBase + mi : -1);
        }
    }
    // Roues : dans le jeu elles viennent d'un modele commun a tous les vehicules ; ici des roues simples aux emplacements
    // wheel_*_dummy du modele.
    if (!haveWheel && !m->tri.empty()) {
        for (size_t f = 0; f < frames.size(); f++) {
            const std::string &fn = frames[f].name;
            if (fn.compare(0, 6, "wheel_") || fn.find("dummy") == std::string::npos) continue;
            float M[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
            for (int k = (int)f, guard = 0; k >= 0 && k < (int)frames.size() && guard < 64; k = frames[k].parent, guard++) Mul(M, frames[k].m, M);
            float cx = M[9], cy = M[10], cz = M[11];
            const float R = 0.5f * wheelScale, Wd = 0.12f;
            int matTyre = (int)m->mats.size(), matHub = matTyre + 1;
            Mat tyre; tyre.r = 34; tyre.g = 34; tyre.b = 38;
            Mat hub; hub.r = 175; hub.g = 178; hub.b = 188;
            m->mats.push_back(tyre); m->mats.push_back(hub);
            const int S = 18;
            int base = (int)m->pos.size() / 3;
            for (int k = 0; k < S; k++) {   // bande de roulement : 2 anneaux ; flancs : 2 anneaux + centres
                float a = k * 6.2831853f / S, c = cosf(a), sn = sinf(a);
                for (int side = -1; side <= 1; side += 2) {
                    m->pos.insert(m->pos.end(), { cx + side * Wd, cy + c * R, cz + sn * R });
                    m->nrm.insert(m->nrm.end(), { 0, c, sn });
                    m->uv.insert(m->uv.end(), { 0, 0 });
                }
            }
            for (int k = 0; k < S; k++) {
                int a0 = base + k * 2, a1 = base + ((k + 1) % S) * 2;
                m->tri.insert(m->tri.end(), { a0, a0 + 1, a1 + 1, matTyre, a0, a1 + 1, a1, matTyre });
            }
            for (int side = -1; side <= 1; side += 2) {
                int c0 = (int)m->pos.size() / 3;
                m->pos.insert(m->pos.end(), { cx + side * Wd, cy, cz });
                m->nrm.insert(m->nrm.end(), { (float)side, 0, 0 });
                m->uv.insert(m->uv.end(), { 0, 0 });
                int r0 = (int)m->pos.size() / 3;
                for (int k = 0; k < S; k++) {
                    float a = k * 6.2831853f / S;
                    m->pos.insert(m->pos.end(), { cx + side * Wd, cy + cosf(a) * R, cz + sinf(a) * R });
                    m->nrm.insert(m->nrm.end(), { (float)side, 0, 0 });
                    m->uv.insert(m->uv.end(), { 0, 0 });
                }
                for (int k = 0; k < S; k++) m->tri.insert(m->tri.end(), { c0, r0 + k, r0 + (k + 1) % S, k % 3 == 0 ? matTyre : matHub });
            }
        }
    }
    if (m->tri.empty()) { delete m; return NULL; }
    for (int j = 0; j < 3; j++) { m->lo[j] = 1e9f; m->hi[j] = -1e9f; }
    for (size_t i = 0; i < m->pos.size(); i += 3)
        for (int j = 0; j < 3; j++) { float v = m->pos[i + j]; if (v < m->lo[j]) m->lo[j] = v; if (v > m->hi[j]) m->hi[j] = v; }
    return m;
}

Model3D *ModelLoad(const std::string &name)
{
    std::vector<uint8_t> dff, txd;
    if (!GetFile(name + ".dff", dff) || !GetFile(name + ".txd", txd)) return NULL;
    return ModelFromData(dff, txd, name);
}

Model3D *ModelLoadPath(const std::wstring &dffPath, const std::wstring &txdPath)
{
    std::vector<uint8_t> dff, txd;
    if (!ReadRange(dffPath, 0, 0xFFFFFFFF, dff)) return NULL;
    std::string base = Lower(Narrow(dffPath));
    size_t sl = base.find_last_of("\\/"), dot = base.rfind('.');
    base = base.substr(sl == std::string::npos ? 0 : sl + 1, dot == std::string::npos ? std::string::npos : dot - (sl == std::string::npos ? 0 : sl + 1));
    if (txdPath.empty() || !ReadRange(txdPath, 0, 0xFFFFFFFF, txd)) {
        // textures du jeu du meme nom (mod qui ne remplace que le modele)
        auto e = g_img.find(base + ".txd");
        if (e != g_img.end()) ReadRange(g_dir + L"models\\gta3.img", (uint64_t)e->second.off * 2048, e->second.size * 2048, txd);
    }
    return ModelFromData(dff, txd, base);
}

void ModelFree(Model3D *m) { delete m; }
std::string ModelInfo(const Model3D *m)
{
    std::string out;
    if (!m) return out;
    char b[256];
    for (auto &t : m->texs) {
        double r = 0, g = 0, bl = 0, al = 0;
        for (uint32_t c : t.px) { r += (c >> 16) & 255; g += (c >> 8) & 255; bl += c & 255; al += c >> 24; }
        double n = t.px.empty() ? 1 : (double)t.px.size();
        sprintf_s(b, "%s %dx%d moy %.0f %.0f %.0f a%.0f\n", t.name.c_str(), t.w, t.h, r / n, g / n, bl / n, al / n);
        out += b;
    }
    int used = 0, untex = 0;
    for (size_t i = 0; i < m->tri.size(); i += 4) { int mi = m->tri[i + 3]; if (mi >= 0 && m->mats[mi].ti >= 0) used++; else untex++; }
    sprintf_s(b, "triangles textures %d, sans texture %d\n", used, untex);
    out += b;
    for (auto &mt : m->mats) { sprintf_s(b, "mat %d %d %d %d tex '%s' ti %d\n", mt.r, mt.g, mt.b, mt.a, mt.tex.c_str(), mt.ti); out += b; }
    return out;
}
void ModelBounds(const Model3D *m, float *lo, float *hi) { for (int j = 0; j < 3; j++) { lo[j] = m ? m->lo[j] : 0; hi[j] = m ? m->hi[j] : 0; } }

void ModelRender(const Model3D *m, uint32_t *out, int w, int h, float yaw, int style)
{
    bool portrait = style == 1, object = style == 2;
    memset(out, 0, (size_t)w * h * 4);
    if (!m || w <= 0 || h <= 0) return;
    const int SS = 2, W = w * SS, H = h * SS;
    std::vector<float> zb((size_t)W * H, 1e30f);
    std::vector<uint32_t> cb((size_t)W * H, 0);

    float height = max(0.1f, m->hi[2] - m->lo[2]);
    float cx = (m->lo[0] + m->hi[0]) * 0.5f, cy = (m->lo[1] + m->hi[1]) * 0.5f;
    float tz = portrait ? m->hi[2] - 0.085f * height : m->lo[2] + 0.5f * height;
    float frameH = portrait ? 0.21f * height : 1.08f * height;
    float pitch = portrait ? 0.06f : 0.10f, fov = 0.42f;
    float dist = frameH * 0.5f / tanf(fov * 0.5f);
    if (object) {   // objet, vehicule : toute la boite, vue un peu du dessus
        float dx = m->hi[0] - m->lo[0], dy = m->hi[1] - m->lo[1], radius = 0.5f * sqrtf(dx * dx + dy * dy + height * height);
        pitch = 0.38f;
        dist = radius / sinf(fov * 0.5f) * 0.86f;
        float aspect = (float)w / h;
        if (aspect < 1) dist /= aspect;
    }
    // camera devant le personnage (il regarde vers +Y), tournee de yaw autour de lui
    float toCam[3] = { sinf(yaw) * cosf(pitch), cosf(yaw) * cosf(pitch), sinf(pitch) };
    float cam[3] = { cx + toCam[0] * dist, cy + toCam[1] * dist, tz + toCam[2] * dist };
    float f[3] = { -toCam[0], -toCam[1], -toCam[2] };
    float r[3] = { f[1], -f[0], 0 };   // f x (0,0,1)
    float rl = sqrtf(r[0] * r[0] + r[1] * r[1]);
    r[0] /= rl; r[1] /= rl;
    float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    float focal = (H * 0.5f) / tanf(fov * 0.5f);
    float L[3];
    for (int j = 0; j < 3; j++) L[j] = -f[j] * 0.55f + u[j] * 0.6f - r[j] * 0.45f;
    float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (int j = 0; j < 3; j++) L[j] /= ll;

    size_t nv = m->pos.size() / 3;
    std::vector<float> sx(nv), sy(nv), sz(nv);
    for (size_t i = 0; i < nv; i++) {
        float d[3] = { m->pos[i * 3] - cam[0], m->pos[i * 3 + 1] - cam[1], m->pos[i * 3 + 2] - cam[2] };
        float xv = d[0] * r[0] + d[1] * r[1] + d[2] * r[2], yv = d[0] * u[0] + d[1] * u[1] + d[2] * u[2], zv = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
        sz[i] = zv;
        if (zv < 0.05f) zv = 0.05f;
        sx[i] = W * 0.5f + xv / zv * focal;
        sy[i] = H * 0.5f - yv / zv * focal;
    }
    for (size_t t = 0; t < m->tri.size(); t += 4) {
        int i0 = m->tri[t], i1 = m->tri[t + 1], i2 = m->tri[t + 2], mi = m->tri[t + 3];
        if (sz[i0] < 0.05f || sz[i1] < 0.05f || sz[i2] < 0.05f) continue;
        float x0 = sx[i0], y0 = sy[i0], x1 = sx[i1], y1 = sy[i1], x2 = sx[i2], y2 = sy[i2];
        float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (fabsf(area) < 1e-6f) continue;
        int minX = max(0, (int)floorf(min(x0, min(x1, x2)))), maxX = min(W - 1, (int)ceilf(max(x0, max(x1, x2))));
        int minY = max(0, (int)floorf(min(y0, min(y1, y2)))), maxY = min(H - 1, (int)ceilf(max(y0, max(y1, y2))));
        if (minX > maxX || minY > maxY) continue;
        const Mat *mat = mi >= 0 ? &m->mats[mi] : NULL;
        const Tex *tex = (mat && mat->ti >= 0) ? &m->texs[mat->ti] : NULL;
        float iz0 = 1 / sz[i0], iz1 = 1 / sz[i1], iz2 = 1 / sz[i2];
        const float *n0 = &m->nrm[i0 * 3], *n1 = &m->nrm[i1 * 3], *n2 = &m->nrm[i2 * 3];
        const float *uv0 = &m->uv[i0 * 2], *uv1 = &m->uv[i1 * 2], *uv2 = &m->uv[i2 * 2];
        float inv = 1 / area;
        for (int y = minY; y <= maxY; y++) {
            float py = y + 0.5f;
            for (int x = minX; x <= maxX; x++) {
                float px = x + 0.5f;
                float b0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv;
                float b1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv;
                float b2 = 1 - b0 - b1;
                if (b0 < 0 || b1 < 0 || b2 < 0) continue;
                float iz = b0 * iz0 + b1 * iz1 + b2 * iz2, z = 1 / iz;
                size_t o = (size_t)y * W + x;
                if (z >= zb[o]) continue;
                float cr = 1, cg = 1, cbl = 1, ca = 1;
                if (tex && tex->w > 0) {
                    float tu = (b0 * uv0[0] * iz0 + b1 * uv1[0] * iz1 + b2 * uv2[0] * iz2) * z;
                    float tv = (b0 * uv0[1] * iz0 + b1 * uv1[1] * iz1 + b2 * uv2[1] * iz2) * z;
                    // bilineaire, en repetition
                    float fx = tu * tex->w - 0.5f, fy = tv * tex->h - 0.5f;
                    int ix = (int)floorf(fx), iy = (int)floorf(fy);
                    float ax = fx - ix, ay = fy - iy;
                    float acc[4] = { 0, 0, 0, 0 };
                    for (int k = 0; k < 4; k++) {
                        int qx = ((ix + (k & 1)) % tex->w + tex->w) % tex->w, qy = ((iy + (k >> 1)) % tex->h + tex->h) % tex->h;
                        uint32_t c = tex->px[(size_t)qy * tex->w + qx];
                        float wgt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
                        acc[0] += ((c >> 16) & 255) * wgt; acc[1] += ((c >> 8) & 255) * wgt; acc[2] += (c & 255) * wgt; acc[3] += (c >> 24) * wgt;
                    }
                    cr = acc[0] / 255; cg = acc[1] / 255; cbl = acc[2] / 255; ca = acc[3] / 255;
                }
                if (mat) { cr *= mat->r / 255.0f; cg *= mat->g / 255.0f; cbl *= mat->b / 255.0f; ca *= mat->a / 255.0f; }
                if (ca < 0.5f) continue;   // cheveux, cils : detoures
                float n[3] = { b0 * n0[0] + b1 * n1[0] + b2 * n2[0], b0 * n0[1] + b1 * n1[1] + b2 * n2[1], b0 * n0[2] + b1 * n1[2] + b2 * n2[2] };
                float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (nl > 1e-6f) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
                float facing = -(n[0] * f[0] + n[1] * f[1] + n[2] * f[2]);
                if (facing < 0) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; facing = -facing; }
                float diff = max(0.0f, n[0] * L[0] + n[1] * L[1] + n[2] * L[2]);
                float rim = 1 - facing;
                rim = rim * rim * rim * 0.55f;
                float lit = 0.42f + 0.72f * diff;
                int R = (int)min(255.0f, (cr * lit + rim * 1.0f) * 255), G = (int)min(255.0f, (cg * lit + rim * 0.36f) * 255), B = (int)min(255.0f, (cbl * lit + rim * 0.58f) * 255);
                zb[o] = z;
                cb[o] = 0xFF000000u | (uint32_t)R << 16 | (uint32_t)G << 8 | (uint32_t)B;
            }
        }
    }
    // reduction 2x2 : bords lisses, fond transparent
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int n = 0, R = 0, G = 0, B = 0;
            for (int k = 0; k < SS * SS; k++) {
                uint32_t c = cb[(size_t)(y * SS + k / SS) * W + x * SS + k % SS];
                if (!c) continue;
                n++; R += (c >> 16) & 255; G += (c >> 8) & 255; B += c & 255;
            }
            if (!n) continue;
            int A = n * 255 / (SS * SS);
            R = R / n * A / 255; G = G / n * A / 255; B = B / n * A / 255;
            out[(size_t)y * w + x] = (uint32_t)A << 24 | (uint32_t)R << 16 | (uint32_t)G << 8 | (uint32_t)B;
        }
}
