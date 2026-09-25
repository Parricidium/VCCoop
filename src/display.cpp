// Affichage moderne : resolution native.
// psSelectDevice (0x600DA0) choisit au premier lancement le premier 640x480 de la liste (puis le mode
// enregistre). En plein ecran fenetre (Fenetre=2) on prend a chaque lancement la resolution du bureau ;
// en fenetre simple (Fenetre=1) 1280x720 si ce mode existe. Choisi apres coup (le moteur est pret a ce
// moment-la) : si le mode n'existe pas, le choix du jeu est garde.
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include <math.h>
#include <string.h>

struct VideoMode { int width, height, depth, flags; };
static int NumVideoModes() { return ((int(__cdecl *)())0x642B40)(); }
static void VideoModeInfo(VideoMode *m, int idx) { ((void(__cdecl *)(VideoMode *, int))0x642B70)(m, idx); }
static int SetVideoMode(int idx) { return ((int(__cdecl *)(int))0x642BD0)(idx); }

// RsGlobal : largeur / hauteur (0x9B48DC / 0x9B48E0) et maxima (0x9B48E4 / 0x9B48E8) ; mode courant 0x783EEC ;
// mode enregistre dans les reglages du menu (FrontEndMenuManager +0x60).
static int &CurMode() { return *(int *)0x783EEC; }

typedef int(__cdecl *SelectDevice_t)();
static SelectDevice_t o_SelectDevice;

static int __cdecl h_SelectDevice()
{
    int r = o_SelectDevice();
    if (!r || !g_cfg.windowed) return r;
    int w = g_cfg.borderless ? GetSystemMetrics(SM_CXSCREEN) : 1280;
    int h = g_cfg.borderless ? GetSystemMetrics(SM_CYSCREEN) : 720;
    int best = -1, bestDepth = 0;
    for (int i = 0, n = NumVideoModes(); i < n; i++) {
        VideoMode m;
        VideoModeInfo(&m, i);
        if (m.width == w && m.height == h && (m.flags & 1) && m.depth > bestDepth) { best = i; bestDepth = m.depth; }
    }
    VideoMode cur;
    VideoModeInfo(&cur, CurMode());
    if (best < 0) { Log("affichage : pas de mode %dx%d, on garde %dx%d", w, h, cur.width, cur.height); return r; }
    if (best != CurMode() && SetVideoMode(best)) {
        CurMode() = best;
        *(int *)0x9B48DC = w; *(int *)0x9B48E4 = w;
        *(int *)0x9B48E0 = h; *(int *)0x9B48E8 = h;
        *(int *)0x869690 = best;   // pour que le menu Affichage l'indique
    }
    Log("affichage : %dx%d (%d bits), mode %d", w, h, bestDepth, CurMode());
    return r;
}

// --- Grand ecran (16:9, 21:9, 32:9) : image au vrai format, champ de vision elargi ("Hor+") ---
// Le jeu ne connait que 4:3 et 16:9 (option du menu) et etire l'image a la fenetre. CDraw::CalculateAspectRatio
// (0x54A270) donne maintenant le vrai rapport largeur / hauteur, et CDraw::SetFOV (0x54A2E0) elargit le champ
// horizontal pour garder le champ vertical du 4:3 : on voit plus sur les cotes, sans deformation.
static float ScreenAspect()
{
    int w = *(int *)0x9B48DC, h = *(int *)0x9B48E0;
    return h > 0 ? (float)w / h : 4.0f / 3.0f;
}

static bool Widescreen() { return g_cfg.widescreen && ScreenAspect() > 4.0f / 3.0f + 0.01f; }

static float __cdecl CalcAspectRatio()
{
    bool menuWide = *(bool *)0x869652;     // option "grand ecran" du menu
    bool bars = *(bool *)0x7E46F5;         // bandes de cinematique (TheCamera.m_WideScreenOn)
    float r;
    if (Widescreen()) r = ScreenAspect() * (bars ? 0.9375f : 1.0f);   // meme reduction que le jeu avec les bandes
    else if (menuWide) r = bars ? 1.6666666f : 1.7777778f;
    else r = bars ? 1.25f : 1.3333334f;
    *(float *)0x94DD38 = r;
    return r;
}

static void __cdecl SetFOV(float fov)
{
    if (Widescreen()) {
        const float d2r = 3.14159265f / 180.0f;
        fov = 2.0f * atanf(tanf(fov * 0.5f * d2r) * ScreenAspect() / (4.0f / 3.0f)) / d2r;
    }
    *(float *)0x696658 = fov;
}

// --- Interface en grand ecran ---
// Le HUD et le radar placent tout en "x * largeur / 640" : chaque module du jeu a sa propre constante 1/640.
// En la remplacant par 1/640 * (4/3) / format, les elements gardent leurs proportions (radar rond, chiffres
// normaux) ; ceux accroches a droite restent a droite (ils sont calcules depuis le bord).
static const uintptr_t kHudScaleX[] = {
    0x697A70,   // CHud
    0x68FD14,   // CRadar
};

void UpdateHudScale()
{
    static float applied = -1.0f;
    float v = 1.0f / 640.0f;
    if (Widescreen()) v *= (4.0f / 3.0f) / ScreenAspect();
    if (v == applied) return;
    for (uintptr_t a : kHudScaleX) {
        if (*(float *)a != applied && applied >= 0.0f) continue;   // pas a nous : on n'y touche pas
        Patch(a, &v, sizeof(v));
    }
    applied = v;
    Log("affichage : echelle horizontale de l'interface %.6f (format %.3f)", v, ScreenAspect());
}

void InstallDisplay()
{
    for (uintptr_t a : kHudScaleX)
        if (*(float *)a != 1.0f / 640.0f) { Log("affichage : constante d'interface inattendue en %06X", (unsigned)a); return; }
    static const uint8_t aspectPro[] = { 0x80, 0x3D, 0x52, 0x96, 0x86, 0x00, 0x00 };
    static const uint8_t fovPro[] = { 0xD9, 0x44, 0x24, 0x04, 0xD9, 0x1D, 0x58, 0x66, 0x69, 0x00, 0xC3 };
    if (!memcmp((void *)0x54A270, aspectPro, sizeof(aspectPro)) && !memcmp((void *)0x54A2E0, fovPro, sizeof(fovPro))) {
        PatchJump(0x54A270, (void *)CalcAspectRatio, 5);
        PatchJump(0x54A2E0, (void *)SetFOV, 5);
    } else {
        Log("affichage : CDraw inattendu, pas de grand ecran");
    }
    static const uint8_t pro[] = { 0x53, 0x56, 0x55, 0x31, 0xDB, 0x83, 0xEC, 0x10 };
    o_SelectDevice = (SelectDevice_t)MakeDetour(0x600DA0, pro, sizeof(pro), (void *)h_SelectDevice);
}
