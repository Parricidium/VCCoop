// Mode fenetre force et comportement en arriere-plan : deux instances doivent pouvoir tourner cote a cote
// sans voler la souris ni le premier plan.
#include "util.h"
#include "vccoop.h"
#include <mmsystem.h>
#include "bridge.h"

// --- Declarations minimales de Direct3D 8 (le SDK Windows ne fournit plus d3d8.h) ---
struct D3DPRESENT_PARAMETERS8 {
    UINT BackBufferWidth, BackBufferHeight;
    UINT BackBufferFormat;
    UINT BackBufferCount;
    UINT MultiSampleType;
    UINT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    UINT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
};
enum { D3DFMT_X8R8G8B8 = 22, D3DFMT_A8R8G8B8 = 21 };
enum { VT_D3D_CREATEDEVICE = 15, VT_D3D_CHECKMSAA = 11, VT_DEV_RESET = 14, VT_DEV_PRESENT = 15, VT_DEV_SETTSS = 63 };

typedef void *(WINAPI *Direct3DCreate8_t)(UINT);
typedef HRESULT(WINAPI *CreateDevice_t)(void *, UINT, UINT, HWND, DWORD, D3DPRESENT_PARAMETERS8 *, void **);
typedef HRESULT(WINAPI *Reset_t)(void *, D3DPRESENT_PARAMETERS8 *);
typedef HRESULT(WINAPI *Present_t)(void *, const RECT *, const RECT *, HWND, const void *);

static Direct3DCreate8_t o_Direct3DCreate8;
// Original de CreateDevice pour chaque vtable IDirect3D8 accrochee.
static struct { void **vt; CreateDevice_t orig; } g_d3dVt[4];
static int g_d3dVtCount;
static Reset_t o_Reset;
static Present_t o_Present;

// --- Rendu : anticrenelage (multisampling du tampon d'image) et filtrage anisotrope ---
// Le jeu n'en propose aucun. Le multisampling se decide a la creation du peripherique (au lancement) ; le
// filtrage se force a chaque reglage de texture (SetTextureStageState : MINFILTER 6, MAGFILTER 5, MIPFILTER 7,
// MAXANISOTROPY 21 ; LINEAR 2, ANISOTROPIC 3).
static UINT g_msaaChosen;
typedef HRESULT(WINAPI *CheckMsaa_t)(void *, UINT, UINT, UINT, BOOL, UINT);
typedef HRESULT(WINAPI *SetTss_t)(void *, DWORD, DWORD, DWORD);
static SetTss_t o_SetTss;

static void ChooseMsaa(void *d3d, UINT adapter, UINT type, D3DPRESENT_PARAMETERS8 *pp)
{
    g_msaaChosen = 0;
    if (g_cfg.msaa < 2) return;
    CheckMsaa_t check = (CheckMsaa_t)(*(void ***)d3d)[VT_D3D_CHECKMSAA];
    for (UINT s = (UINT)(g_cfg.msaa > 8 ? 8 : g_cfg.msaa); s >= 2; s /= 2) {
        if (SUCCEEDED(check(d3d, adapter, type, pp->BackBufferFormat, pp->Windowed, s)) &&
            (!pp->EnableAutoDepthStencil || SUCCEEDED(check(d3d, adapter, type, pp->AutoDepthStencilFormat, pp->Windowed, s)))) {
            g_msaaChosen = s;
            break;
        }
    }
    Log("rendu : anticrenelage demande %dx, obtenu %ux", g_cfg.msaa, g_msaaChosen);
}

static void ApplyMsaa(D3DPRESENT_PARAMETERS8 *pp)
{
    if (!g_msaaChosen) return;
    pp->MultiSampleType = g_msaaChosen;
    pp->SwapEffect = 1;   // D3DSWAPEFFECT_DISCARD, obligatoire avec le multisampling
}

static HRESULT WINAPI h_SetTss(void *dev, DWORD stage, DWORD state, DWORD value)
{
    // Numeros Direct3D 8 : MAGFILTER 16, MINFILTER 17, MIPFILTER 18, MAXANISOTROPY 21 (avant 28r on testait 6 et 7,
    // ALPHAARG2 et BUMPENVMAT00 : le filtrage anisotrope ne s'appliquait pas).
    if (g_cfg.aniso) {
        if (state == 17 && value == 2) { o_SetTss(dev, stage, 21, 16); value = 3; }   // MINFILTER lineaire -> anisotrope 16x
        else if (state == 18 && value == 0) value = 2;                                   // MIPFILTER aucun -> lineaire (trilineaire)
    }
    return o_SetTss(dev, stage, state, value);
}
static HWND g_hwnd;
static HWND g_prevForeground;   // fenetre qui avait le premier plan au lancement (instances de test)

HWND GameWindow() { return g_hwnd; }

bool GameHasFocus()
{
    return g_hwnd && GetForegroundWindow() == g_hwnd;
}

static void MakeWindowed(D3DPRESENT_PARAMETERS8 *pp)
{
    if (!g_cfg.windowed) return;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->FullScreen_PresentationInterval = 0;
    if (pp->BackBufferFormat != D3DFMT_X8R8G8B8 && pp->BackBufferFormat != D3DFMT_A8R8G8B8)
        pp->BackBufferFormat = D3DFMT_X8R8G8B8;
}

static BOOL(WINAPI *o_SetWindowPos)(HWND, HWND, int, int, int, int, UINT) = SetWindowPos;

static void FitWindow(HWND hwnd, int w, int h)
{
    if (!g_cfg.windowed || !hwnd) return;
    if (g_cfg.borderless) {
        // Sans bordure : la fenetre couvre l'ecran ; le jeu rend a sa resolution, etiree a l'ecran par Windows.
        SetWindowLongA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongA(hwnd, GWL_EXSTYLE, 0);
        o_SetWindowPos(hwnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED);
        return;
    }
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    SetWindowLongA(hwnd, GWL_STYLE, style);
    SetWindowLongA(hwnd, GWL_EXSTYLE, 0);
    RECT r = { 0, 0, w, h };
    AdjustWindowRect(&r, style, FALSE);
    o_SetWindowPos(hwnd, HWND_NOTOPMOST, g_cfg.winX, g_cfg.winY, r.right - r.left, r.bottom - r.top,
                   SWP_FRAMECHANGED | SWP_NOACTIVATE);
    char title[128];
    wsprintfA(title, "GTA Vice City - VCCoop (%s)", g_cfg.playerName);
    SetWindowTextA(hwnd, title);
}

// Limiteur d'images : sans lui le jeu tourne a plusieurs milliers d'images/s en fenetre. Chaque image part a
// intervalle regulier : minuterie Windows a 1 ms, Sleep tant qu'il reste plus de 3 ms, puis attente active.
// Le limiteur du jeu (option du menu, plafond RsGlobal.maxFPS 0x9B48EC = 30) est aligne sur le notre.
static void LimitFrameRate()
{
    if (g_cfg.maxFps <= 0) return;
    static LARGE_INTEGER freq, next;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        timeBeginPeriod(1);
        *(int *)0x9B48EC = g_cfg.maxFps;
    }
    LONGLONG step = freq.QuadPart / g_cfg.maxFps;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (next.QuadPart == 0 || now.QuadPart - next.QuadPart > step * 4) next = now;   // gros retard : on repart
    for (;;) {
        LONGLONG left = next.QuadPart - now.QuadPart;
        if (left <= 0) break;
        if (left * 1000 > freq.QuadPart * 3) Sleep(1);   // Sleep(1) peut deborder de ~2 ms
        else YieldProcessor();
        QueryPerformanceCounter(&now);
    }
    next.QuadPart += step;

    // Statistique de regularite (journal toutes les 10 s) : ecart min / max entre deux images.
    static LARGE_INTEGER last, since;
    static double minMs = 1e9, maxMs = 0;
    static int count;
    if (last.QuadPart) {
        double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
        if (ms < minMs) minMs = ms;
        if (ms > maxMs) maxMs = ms;
        count++;
    } else since = now;
    last = now;
    if ((now.QuadPart - since.QuadPart) > freq.QuadPart * 10) {
        Log("images : %.1f/s, ecart %.1f a %.1f ms", count * (double)freq.QuadPart / (now.QuadPart - since.QuadPart), minMs, maxMs);
        since = now; count = 0; minMs = 1e9; maxMs = 0;
    }
}

// Windows donne le premier plan a la fenetre d'un processus qu'on vient de lancer, malgre SW_SHOWNOACTIVATE.
// Une instance d'arriere-plan le rend aussitot a la fenetre qui l'avait (pendant ses premieres secondes).
static void GiveBackForeground()
{
    static DWORD start;
    if (!g_cfg.background || !g_prevForeground) return;
    if (!start) start = GetTickCount();
    if (GetTickCount() - start > 15000) { g_prevForeground = NULL; return; }
    if (GetForegroundWindow() == g_hwnd && IsWindow(g_prevForeground)) {
        SetForegroundWindow(g_prevForeground);
        Log("premier plan rendu a la fenetre precedente");
    }
}

static HRESULT WINAPI h_Present(void *dev, const RECT *src, const RECT *dst, HWND wnd, const void *dirty)
{
    WatchdogFrame();
    GiveBackForeground();
    UpdateHudScale();
    // Le menu peut presenter une image depuis l'interieur de notre propre boucle (SwitchToNewScreen dessine) :
    // pas de boucle coop imbriquee.
    static bool inFrame;
    if (!inFrame) { inFrame = true; CoopFrame(); inFrame = false; }
    LimitFrameRate();
    return o_Present(dev, src, dst, wnd, dirty);
}

static HRESULT WINAPI h_Reset(void *dev, D3DPRESENT_PARAMETERS8 *pp)
{
    MakeWindowed(pp);
    ApplyMsaa(pp);
    GfxBeforeReset();
    HRESULT hr = o_Reset(dev, pp);
    Log("Reset %ux%u fenetre=%d -> 0x%08lX", pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed, hr);
    FitWindow(g_hwnd, pp->BackBufferWidth, pp->BackBufferHeight);
    return hr;
}

static HRESULT WINAPI h_CreateDevice(void *d3d, UINT adapter, UINT type, HWND focus, DWORD flags,
                                     D3DPRESENT_PARAMETERS8 *pp, void **out)
{
    MakeWindowed(pp);
    g_hwnd = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    CreateDevice_t orig = NULL;
    for (int i = 0; i < g_d3dVtCount; i++)
        if (g_d3dVt[i].vt == *(void ***)d3d) orig = g_d3dVt[i].orig;
    if (!orig) return E_FAIL;
    ChooseMsaa(d3d, adapter, type, pp);
    ApplyMsaa(pp);
    HRESULT hr = orig(d3d, adapter, type, focus, flags, pp, out);
    if (FAILED(hr) && g_msaaChosen) {   // refus : sans anticrenelage
        Log("rendu : peripherique refuse avec anticrenelage (0x%08lX), sans", hr);
        g_msaaChosen = 0;
        pp->MultiSampleType = 0;
        hr = orig(d3d, adapter, type, focus, flags, pp, out);
    }
    Log("CreateDevice %ux%u fmt=%u fenetre=%d -> 0x%08lX", pp->BackBufferWidth, pp->BackBufferHeight,
        pp->BackBufferFormat, pp->Windowed, hr);
    if (SUCCEEDED(hr) && *out) {
        void **vt = *(void ***)*out;
        if (!o_Reset) {
            o_Reset = (Reset_t)PatchPointer(&vt[VT_DEV_RESET], (void *)h_Reset);
            o_Present = (Present_t)PatchPointer(&vt[VT_DEV_PRESENT], (void *)h_Present);
            o_SetTss = (SetTss_t)PatchPointer(&vt[VT_DEV_SETTSS], (void *)h_SetTss);
            if (!IsBridgeDevice(*out)) GfxHookDevice(*out);   // Direct3D 8 d'origine : ancien rendu des ombres
        }
        FitWindow(g_hwnd, pp->BackBufferWidth, pp->BackBufferHeight);
    }
    return hr;
}

static void *WINAPI h_Direct3DCreate8(UINT sdk)
{
    void *d3d = g_cfg.renderer == 9 ? BridgeCreate8(sdk) : NULL;   // Rendu=9 : notre pont vers Direct3D 9
    if (!d3d) d3d = o_Direct3DCreate8(sdk);
    // Chaque objet peut avoir sa propre vtable (le jeu en cree deux) : on les accroche toutes.
    if (d3d) {
        void **vt = *(void ***)d3d;
        if (vt[VT_D3D_CREATEDEVICE] != (void *)h_CreateDevice && g_d3dVtCount < 4) {
            g_d3dVt[g_d3dVtCount].vt = vt;
            g_d3dVt[g_d3dVtCount++].orig = (CreateDevice_t)PatchPointer(&vt[VT_D3D_CREATEDEVICE], (void *)h_CreateDevice);
        }
    }
    Log("Direct3DCreate8(%u) -> %p", sdk, d3d);
    return d3d;
}

// Le jeu recentre le curseur en permanence : on ne le laisse faire que s'il a le premier plan.
static BOOL(WINAPI *o_SetCursorPos)(int, int);
static BOOL WINAPI h_SetCursorPos(int x, int y)
{
    if (!GameHasFocus()) return TRUE;
    return o_SetCursorPos(x, y);
}

// --- Fenetre en arriere-plan (instances de test) : ouverte a la position voulue, jamais activee ---
static HWND(WINAPI *o_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static HWND WINAPI h_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                                     HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    if (g_cfg.windowed) { x = g_cfg.winX; y = g_cfg.winY; }
    HWND hwnd = o_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (!parent && !g_hwnd) { g_hwnd = hwnd; MenuWindowCreated(hwnd); }
    return hwnd;
}

static BOOL WINAPI h_SetWindowPos(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags)
{
    if (g_cfg.windowed && hwnd == g_hwnd) {
        if (g_cfg.borderless) { x = 0; y = 0; w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN); flags &= ~(SWP_NOMOVE | SWP_NOSIZE); }
        else { x = g_cfg.winX; y = g_cfg.winY; flags &= ~SWP_NOMOVE; }
    }
    if (g_cfg.background) flags |= SWP_NOACTIVATE;
    return o_SetWindowPos(hwnd, after, x, y, w, h, flags);
}

static BOOL(WINAPI *o_ShowWindow)(HWND, int);
static BOOL WINAPI h_ShowWindow(HWND hwnd, int cmd)
{
    if (g_cfg.background && (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_SHOWDEFAULT)) cmd = SW_SHOWNOACTIVATE;
    return o_ShowWindow(hwnd, cmd);
}

static HWND(WINAPI *o_SetFocus)(HWND);
static HWND WINAPI h_SetFocus(HWND hwnd)
{
    if (g_cfg.background && !GameHasFocus()) return NULL;
    return o_SetFocus(hwnd);
}

// Sans le premier plan, le jeu ne doit ni capturer ni enfermer la souris.
static BOOL(WINAPI *o_ClipCursor)(const RECT *);
static BOOL WINAPI h_ClipCursor(const RECT *r)
{
    if (r && !GameHasFocus()) return TRUE;
    return o_ClipCursor(r);
}

static HWND(WINAPI *o_SetCapture)(HWND);
static HWND WINAPI h_SetCapture(HWND hwnd)
{
    if (!GameHasFocus()) return NULL;
    return o_SetCapture(hwnd);
}

void InstallWindowHooks()
{
    SetProcessDPIAware();
    g_prevForeground = GetForegroundWindow();
    o_CreateWindowExA = (decltype(o_CreateWindowExA))HookImport("user32.dll", "CreateWindowExA", (void *)h_CreateWindowExA);
    o_SetWindowPos = (decltype(o_SetWindowPos))HookImport("user32.dll", "SetWindowPos", (void *)h_SetWindowPos);
    o_ShowWindow = (decltype(o_ShowWindow))HookImport("user32.dll", "ShowWindow", (void *)h_ShowWindow);
    o_SetFocus = (decltype(o_SetFocus))HookImport("user32.dll", "SetFocus", (void *)h_SetFocus);
    o_ClipCursor = (decltype(o_ClipCursor))HookImport("user32.dll", "ClipCursor", (void *)h_ClipCursor);
    o_SetCapture = (decltype(o_SetCapture))HookImport("user32.dll", "SetCapture", (void *)h_SetCapture);
    o_Direct3DCreate8 = (Direct3DCreate8_t)HookImport("d3d8.dll", "Direct3DCreate8", (void *)h_Direct3DCreate8);
    o_SetCursorPos = (BOOL(WINAPI *)(int, int))HookImport("user32.dll", "SetCursorPos", (void *)h_SetCursorPos);
}
