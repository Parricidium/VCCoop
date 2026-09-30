// Vignettes 3D (voir thumbs.h). Un fil de travail (priorite basse) lit le modele et ses textures (jeu, VCCoop\mods,
// packs vccpkN.img) et le rend en logiciel ; le fil principal copie les images finies dans une texture de 2048 x 2048
// (100 cases, les moins recemment vues cedent leur place).
#include "util.h"
#include "vccoop.h"
#include "bridge.h"
#include "thumbs.h"
#include "../launcher/model3d.h"
#include <d3d9.h>
#include <deque>
#include <map>
#include <string>
#include <vector>

enum { ATLAS = 2048, CELL = 204, COLS = 10, SLOTS = 100, UPLOADS_PER_FRAME = 4 };

struct Req { std::string key, model; int kind; };
struct Done { std::string key; int w, h; std::vector<uint32_t> px; };
struct Slot { std::string key; DWORD used; int w, h; };

static CRITICAL_SECTION g_cs;
static HANDLE g_thread, g_wake;
static std::deque<Req> g_queue;
static std::vector<Done> g_done;
static std::map<std::string, bool> g_asked;   // demandee (en file ou en cours)
static std::map<std::string, int> g_slotOf;   // prete : case
static Slot g_slots[SLOTS];
static IDirect3DTexture9 *g_atlas;
static IDirect3DSurface9 *g_atlasSurf, *g_tmp;
static volatile bool g_failed;
struct Lock { Lock() { EnterCriticalSection(&g_cs); } ~Lock() { LeaveCriticalSection(&g_cs); } };

static DWORD WINAPI Worker(void *)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    wchar_t dir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, GameDir(), -1, dir, MAX_PATH);
    if (!ImgOpen(dir)) { Log("vignettes : models\\gta3.dir illisible"); g_failed = true; return 0; }
    Log("vignettes : catalogue du jeu ouvert");
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        for (;;) {
            Req r;
            { Lock l; if (g_queue.empty()) break; r = g_queue.front(); g_queue.pop_front(); }
            Model3D *m = ModelLoad(r.model);
            Done d;
            d.key = r.key;
            d.w = r.kind == THUMB_VEHICLE ? CELL : CELL * 2 / 3;
            d.h = r.kind == THUMB_VEHICLE ? CELL * 5 / 8 : CELL;
            d.px.resize((size_t)d.w * d.h);
            ModelRender(m, d.px.data(), d.w, d.h, r.kind == THUMB_VEHICLE ? 0.6f : 0.35f, r.kind == THUMB_VEHICLE ? 2 : 0);
            if (!m) { static int n; if (n++ < 10) Log("vignettes : modele %s introuvable", r.model.c_str()); }
            ModelFree(m);
            Lock l;
            g_done.push_back(std::move(d));
        }
    }
}

static bool Start()
{
    static bool started;
    if (started) return !g_failed;
    started = true;
    InitializeCriticalSection(&g_cs);
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    return g_thread != NULL;
}

bool ThumbGet(int kind, const char *model, float uv[4], float *aspect)
{
    if (!ModernRenderer() || !Start() || g_failed) return false;
    std::string key = (kind == THUMB_VEHICLE ? "v:" : "p:") + std::string(model);
    Lock l;
    auto it = g_slotOf.find(key);
    if (it != g_slotOf.end() && g_atlas) {
        Slot &s = g_slots[it->second];
        s.used = GetTickCount();
        float x = (float)(it->second % COLS) * CELL, y = (float)(it->second / COLS) * CELL;
        uv[0] = x / ATLAS; uv[1] = y / ATLAS; uv[2] = (x + s.w) / ATLAS; uv[3] = (y + s.h) / ATLAS;
        if (aspect) *aspect = (float)s.w / s.h;
        return true;
    }
    if (!g_asked.count(key)) {
        g_asked[key] = true;
        std::string lower = model;
        for (auto &c : lower) c = (char)tolower((unsigned char)c);
        g_queue.push_back({ key, lower, kind });
        SetEvent(g_wake);
    }
    return false;
}

IDirect3DTexture9 *ThumbAtlas() { return g_atlas; }

void ThumbFrame()
{
    if (!g_thread) return;
    IDirect3DDevice9 *dev = BridgeDevice9();
    if (!dev) return;
    std::vector<Done> ready;
    {
        Lock l;
        for (int i = 0; i < UPLOADS_PER_FRAME && !g_done.empty(); i++) { ready.push_back(std::move(g_done.back())); g_done.pop_back(); }
    }
    if (ready.empty()) return;
    if (!g_atlas) {
        if (FAILED(dev->CreateTexture(ATLAS, ATLAS, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_atlas, NULL)) ||
            FAILED(dev->CreateOffscreenPlainSurface(CELL, CELL, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_tmp, NULL))) {
            Log("vignettes : texture impossible");
            if (g_atlas) { g_atlas->Release(); g_atlas = NULL; }
            g_failed = true;
            return;
        }
        g_atlas->GetSurfaceLevel(0, &g_atlasSurf);
    }
    for (Done &d : ready) {
        // case libre, sinon la moins recemment vue
        int slot = 0;
        for (int i = 0; i < SLOTS; i++) {
            if (g_slots[i].key.empty()) { slot = i; break; }
            if (g_slots[i].used < g_slots[slot].used) slot = i;
        }
        D3DLOCKED_RECT lr;
        if (FAILED(g_tmp->LockRect(&lr, NULL, 0))) continue;
        for (int y = 0; y < d.h; y++) memcpy((BYTE *)lr.pBits + y * lr.Pitch, &d.px[(size_t)y * d.w], d.w * 4);
        g_tmp->UnlockRect();
        RECT src = { 0, 0, d.w, d.h };
        POINT dst = { (slot % COLS) * CELL, (slot / COLS) * CELL };
        if (FAILED(dev->UpdateSurface(g_tmp, &src, g_atlasSurf, &dst))) continue;
        Lock l;
        if (!g_slots[slot].key.empty()) { g_slotOf.erase(g_slots[slot].key); g_asked.erase(g_slots[slot].key); }
        g_slots[slot] = { d.key, GetTickCount(), d.w, d.h };
        g_slotOf[d.key] = slot;
    }
}

void ThumbRelease()
{
    if (!g_thread) return;
    Lock l;
    if (g_atlasSurf) { g_atlasSurf->Release(); g_atlasSurf = NULL; }
    if (g_atlas) { g_atlas->Release(); g_atlas = NULL; }
    if (g_tmp) { g_tmp->Release(); g_tmp = NULL; }
    for (auto &s : g_slots) s = Slot();
    g_slotOf.clear();
    g_asked.clear();   // (redemandees : rendues a nouveau)
}
