// Interface personnalisee des menus :
//  - texte des menus blanc a contour rose (StyleMenus, CouleurTexteMenus, CouleurContourMenus) et plus petit
//    (TailleTexteMenus, en %) : la police du jeu est une image de 32 points par lettre, agrandie a 40-50 points
//    en 1080p (floue) ; plus petite, elle reste nette ;
//  - images prises dans VCCoop\interface (png, jpg ou bmp, a n'importe quelle taille) : fond_menu (tout l'ecran,
//    sans deformation : l'image est recadree au format de l'ecran), logo (en haut a gauche, transparence du png
//    gardee), chargement* (ecrans de chargement, tires au hasard).
// Les images sont lues avec WIC (Windows), mises a une taille en puissance de deux (ce que demande la carte
// graphique en Direct3D 8) puis chargees par le lecteur de textures du jeu depuis un TXD fabrique en memoire.
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

using namespace game;

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")



// --- Reglages ---
static bool g_style = true;
static uint8_t g_fill[3] = { 255, 255, 255 }, g_edge[3] = { 255, 150, 225 };
static float g_textScale = 0.8f;

static void ParseColor(const char *s, uint8_t *out)
{
    int r, g, b;
    if (sscanf(s, "%d,%d,%d", &r, &g, &b) == 3) { out[0] = (uint8_t)r; out[1] = (uint8_t)g; out[2] = (uint8_t)b; }
}

static void LoadSettings()
{
    char ini[MAX_PATH], buf[64];
    wsprintfA(ini, "%svccoop.ini", GameDir());
    g_style = GetPrivateProfileIntA("VCCoop", "StyleMenus", 1, ini) != 0;
    GetPrivateProfileStringA("VCCoop", "CouleurTexteMenus", "255,255,255", buf, sizeof(buf), ini);
    ParseColor(buf, g_fill);
    GetPrivateProfileStringA("VCCoop", "CouleurContourMenus", "255,150,225", buf, sizeof(buf), ini);
    ParseColor(buf, g_edge);
    int size = GetPrivateProfileIntA("VCCoop", "TailleTexteMenus", 80, ini);
    if (size < 40) size = 40;
    if (size > 150) size = 150;
    g_textScale = size / 100.0f;
}

// --- Images ---
struct Image {
    void *dict;      // RwTexDictionary (le detruire detruit la texture)
    void *tex;       // RwTexture
    int w, h;        // taille d'origine du fichier (pour son format)
};

static int ScreenW() { return *(int *)0x9B48DC; }
static int ScreenH() { return *(int *)0x9B48E0; }

// Puissance de deux la plus proche (1080 -> 1024, 1920 -> 2048), 2048 au plus.
static int Pow2(int v)
{
    int p = 1;
    while (p * 2 <= v) p *= 2;
    if (v > p + p / 4) p *= 2;
    if (p > 2048) p = 2048;
    if (p < 8) p = 8;
    return p;
}

static bool ReadPixels(const wchar_t *path, std::vector<uint8_t> &px, int &srcW, int &srcH, int &tw, int &th)
{
    static bool com;
    if (!com) { com = true; CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); }
    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *sc = NULL;
    IWICFormatConverter *conv = NULL;
    bool ok = false;
    UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) goto done;
    if (FAILED(f->CreateDecoderFromFilename(path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    if (FAILED(dec->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h) goto done;
    srcW = (int)w; srcH = (int)h;
    tw = Pow2(srcW); th = Pow2(srcH);
    if (FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(frame, tw, th, WICBitmapInterpolationModeFant))) goto done;
    if (FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) goto done;
    px.resize((size_t)tw * th * 4);
    ok = SUCCEEDED(conv->CopyPixels(NULL, tw * 4, (UINT)px.size(), px.data()));
done:
    if (conv) conv->Release();
    if (sc) sc->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (f) f->Release();
    return ok;
}

// TXD en memoire : dictionnaire (0x16) { struct { 1 texture } ; texture native D3D8 (0x15) { struct { en-tete, 1
// niveau en 8888 } ; extension } ; extension }.
static void PutChunk(std::vector<uint8_t> &out, uint32_t type, const std::vector<uint8_t> &body)
{
    uint32_t hdr[3] = { type, (uint32_t)body.size(), 0x1003FFFF };
    out.insert(out.end(), (uint8_t *)hdr, (uint8_t *)hdr + 12);
    out.insert(out.end(), body.begin(), body.end());
}

static std::vector<uint8_t> BuildTxd(const std::vector<uint8_t> &px, int w, int h)
{
    std::vector<uint8_t> st(88 + 4, 0);
    uint8_t *p = st.data();
    *(uint32_t *)(p + 0) = 8;          // D3D8
    *(uint32_t *)(p + 4) = 0x3302;     // filtre lineaire, bords bloques
    strcpy((char *)p + 8, "vccimg");
    *(uint32_t *)(p + 72) = 0x0500;    // 8888
    *(uint32_t *)(p + 76) = 1;         // transparence
    *(uint16_t *)(p + 80) = (uint16_t)w;
    *(uint16_t *)(p + 82) = (uint16_t)h;
    p[84] = 32; p[85] = 1; p[86] = 4; p[87] = 0;   // 32 bits, 1 niveau, texture, pas de compression
    *(uint32_t *)(p + 88) = (uint32_t)px.size();
    st.insert(st.end(), px.begin(), px.end());
    std::vector<uint8_t> native, empty, dictStruct(4, 0), dict, out;
    PutChunk(native, 1, st);
    PutChunk(native, 3, empty);
    dictStruct[0] = 1;
    PutChunk(dict, 1, dictStruct);
    PutChunk(dict, 0x15, native);
    PutChunk(dict, 3, empty);
    PutChunk(out, 0x16, dict);
    return out;
}

struct RwMemory { uint8_t *start; uint32_t length; };
static void *StreamOpen(int type, int access, void *data) { return ((void *(__cdecl *)(int, int, void *))0x6459C0)(type, access, data); }
static int StreamFindChunk(void *s, uint32_t type) { return ((int(__cdecl *)(void *, uint32_t, uint32_t *, uint32_t *))0x64FAC0)(s, type, NULL, NULL); }
static void *TxdStreamRead(void *s) { return ((void *(__cdecl *)(void *))0x61E710)(s); }
static void StreamClose(void *s, void *data) { ((int(__cdecl *)(void *, void *))0x6458F0)(s, data); }
static void *TxdFind(void *dict, const char *name) { return ((void *(__cdecl *)(void *, const char *))0x64E060)(dict, name); }
static void TxdDestroy(void *dict) { ((int(__cdecl *)(void *))0x64DD90)(dict); }
static void RenderState(int state, int value) { ((int(__cdecl *)(int, int))0x649BA0)(state, value); }

static bool LoadImageFile(const std::wstring &path, Image &img)
{
    memset(&img, 0, sizeof(img));
    std::vector<uint8_t> px;
    int tw, th;
    if (!ReadPixels(path.c_str(), px, img.w, img.h, tw, th)) {
        Log("interface : image illisible %S", path.c_str());
        return false;
    }
    std::vector<uint8_t> txd = BuildTxd(px, tw, th);
    RwMemory mem = { txd.data(), (uint32_t)txd.size() };
    void *s = StreamOpen(3, 1, &mem);
    if (!s) return false;
    if (StreamFindChunk(s, 0x16)) img.dict = TxdStreamRead(s);
    StreamClose(s, &mem);
    if (img.dict) img.tex = TxdFind(img.dict, "vccimg");
    if (!img.tex) {
        Log("interface : texture refusee par le jeu (%S, %dx%d)", path.c_str(), tw, th);
        if (img.dict) TxdDestroy(img.dict);
        memset(&img, 0, sizeof(img));
        return false;
    }
    Log("interface : %S (%dx%d, texture %dx%d)", path.c_str(), img.w, img.h, tw, th);
    return true;
}

static void FreeImage(Image &img)
{
    if (img.dict) TxdDestroy(img.dict);
    memset(&img, 0, sizeof(img));
}

static std::wstring Folder()
{
    wchar_t dir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, GameDir(), -1, dir, MAX_PATH);
    return std::wstring(dir) + L"VCCoop\\interface\\";
}

static bool IsImageName(const wchar_t *n)
{
    const wchar_t *e = wcsrchr(n, L'.');
    return e && (!_wcsicmp(e, L".png") || !_wcsicmp(e, L".jpg") || !_wcsicmp(e, L".jpeg") || !_wcsicmp(e, L".bmp"));
}

// Fichiers dont le nom commence par prefix (fond_menu.png, chargement1.jpg...).
static std::vector<std::wstring> FindImages(const wchar_t *prefix)
{
    std::vector<std::wstring> list;
    WIN32_FIND_DATAW fd;
    std::wstring pattern = Folder() + prefix + L"*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return list;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsImageName(fd.cFileName)) list.push_back(Folder() + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return list;
}

// Dessin : CSprite2d::Draw(CRect const&, CRGBA const&) sur un CSprite2d a nous (un simple pointeur de texture).
// CRect du jeu : gauche, bas, droite, haut.
static void DrawImage(const Image &img, float x0, float y0, float x1, float y1, const uint8_t *color)
{
    void *sprite = img.tex;
    float rect[4] = { x0, y1, x1, y0 };
    RenderState(9, 2);   // filtre lineaire
    ((void(__thiscall *)(void *, const float *, const uint8_t *))0x578710)(&sprite, rect, color);
}

// Tout l'ecran, sans deformation : l'image est agrandie jusqu'a couvrir l'ecran, le surplus depasse.
static void DrawCover(const Image &img, const uint8_t *color)
{
    float W = (float)ScreenW(), H = (float)ScreenH();
    float ia = (float)img.w / img.h, sa = W / H;
    float w = W, h = H;
    if (sa > ia) h = W / ia; else w = H * ia;
    SuspendMenuSqueeze(true);
    DrawImage(img, (W - w) * 0.5f, (H - h) * 0.5f, (W + w) * 0.5f, (H + h) * 0.5f, color);
    SuspendMenuSqueeze(false);
}

// --- Menu : fond et logo ---
static Image g_bg, g_logo;
static bool g_menuLoaded;

static void LoadMenuImages()
{
    if (g_menuLoaded) return;
    g_menuLoaded = true;
    std::vector<std::wstring> bg = FindImages(L"fond_menu"), logo = FindImages(L"logo");
    if (!bg.empty()) LoadImageFile(bg[rand() % bg.size()], g_bg);
    if (!logo.empty()) LoadImageFile(logo[0], g_logo);
}

// Appels de CSprite2d::Draw pour le fond du menu (0x4A272A pendant le fondu, 0x4A2CB2 ensuite).
static void __fastcall h_DrawBackground(void *sprite, void *, const float *rect, const uint8_t *color)
{
    LoadMenuImages();
    if (g_bg.tex) { DrawCover(g_bg, color); return; }
    ((void(__thiscall *)(void *, const float *, const uint8_t *))0x578710)(sprite, rect, color);
}

// Les quatre quadrilateres noirs autour du cadre du menu : avec notre fond, on le laisse voir en entier.
static void __cdecl h_FrameQuad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, const uint8_t *color)
{
    if (g_bg.tex) return;
    ((void(__cdecl *)(float, float, float, float, float, float, float, float, const uint8_t *))0x578520)(x1, y1, x2, y2, x3, y3, x4, y4, color);
}

// Logo (0x4A3503) : dans la case du logo du jeu (en haut a gauche), a la hauteur de la case, a son format.
static void __fastcall h_DrawLogo(void *sprite, void *, const float *rect, const uint8_t *color)
{
    LoadMenuImages();
    if (!g_logo.tex) { ((void(__thiscall *)(void *, const float *, const uint8_t *))0x578710)(sprite, rect, color); return; }
    float W = (float)ScreenW(), H = (float)ScreenH();
    float h = H * 130.0f / 448.0f, w = h * g_logo.w / g_logo.h;
    if (w > 3.0f * h) { w = 3.0f * h; h = w * g_logo.h / g_logo.w; }
    float x = W * 27.0f / 640.0f, y = H * 8.0f / 448.0f;
    if (MenuSqueezeActive()) x = W * 0.5f + (x - W * 0.5f) * MenuSqueezeFactor();   // meme place que le logo d'origine
    SuspendMenuSqueeze(true);
    DrawImage(g_logo, x, y, x + w, y + h, color);
    SuspendMenuSqueeze(false);
}

// --- Ecrans de chargement ---
// LoadingScreen (0x4A69D0) dessine l'image choisie par le jeu (loadsc0..13) en 0x4A6A7E. A chaque nouvelle image
// du jeu, on prend la suivante des notres (dans un ordre tire au hasard) ; une seule est chargee a la fois.
static Image g_load;
static std::vector<std::wstring> g_loadList;
static size_t g_loadNext;
static void *g_lastGameSplash;

static void __fastcall h_DrawSplash(void *sprite, void *, const float *rect, const uint8_t *color)
{
    static bool listed;
    if (!listed) {
        listed = true;
        g_loadList = FindImages(L"chargement");
        for (size_t i = g_loadList.size(); i > 1; i--) std::swap(g_loadList[i - 1], g_loadList[rand() % i]);
    }
    void *gameTex = sprite ? *(void **)sprite : NULL;
    if (!g_loadList.empty() && (gameTex != g_lastGameSplash || !g_load.tex)) {
        g_lastGameSplash = gameTex;
        FreeImage(g_load);
        for (size_t tries = 0; tries < g_loadList.size() && !g_load.tex; tries++)
            LoadImageFile(g_loadList[g_loadNext++ % g_loadList.size()], g_load);
    }
    if (g_load.tex) { DrawCover(g_load, color); return; }
    ((void(__thiscall *)(void *, const float *, const uint8_t *))0x578710)(sprite, rect, color);
}

void InterfaceFrame()
{
    if (g_load.tex && GameState() == GS_PLAYING) FreeImage(g_load);
}

// --- Texte ---
// CFont::Details : couleur 0x97F820, echelle 0x97F824 / 0x97F828, fond 0x97F83B, ombre 0x97F860 (position),
// 0x97F862 (couleur). CFont::PrintString (0x551040) met le texte en attente ; CFont::DrawFonts (0x550250) l'affiche.
static bool g_inFrontEnd;
typedef void(__cdecl *PrintString_t)(float x, float y, const wchar_t *s);
static PrintString_t o_PrintString;
static void DrawFonts() { ((void(__cdecl *)())0x550250)(); }

static void StyledPrint(float x, float y, const wchar_t *s);
static void __cdecl h_PrintString(float x, float y, const wchar_t *s)
{
    static int depth;
    if (!g_inFrontEnd || !s || depth) { o_PrintString(x, y, s); return; }
    depth++;
    StyledPrint(x, y, s);
    depth--;
}

static void StyledPrint(float x, float y, const wchar_t *s)
{
    uint8_t *col = (uint8_t *)0x97F820;
    float &sx = *(float *)0x97F824, &sy = *(float *)0x97F828;
    int16_t &shadow = *(int16_t *)0x97F860;
    uint8_t saved[4] = { col[0], col[1], col[2], col[3] };
    float osx = sx, osy = sy;
    int16_t oshadow = shadow;
    bool pink = saved[0] == 255 && saved[1] == 150 && saved[2] == 225;
    bool dim = saved[0] == 0xC3 && saved[1] == 0x5A && saved[2] == 0xA5;
    bool shadowPass = saved[0] == 0x1E && saved[1] == 0x1E && saved[2] == 0x1E;
    if (g_style && shadowPass) return;   // ombre dessinee a la main par le menu : le contour la remplace

    // Plus petit, a la meme place : le haut descend de la moitie de la hauteur gagnee (lettres ~18 points x echelle).
    sx = osx * g_textScale;
    sy = osy * g_textScale;
    float ty = y + (1.0f - g_textScale) * osy * 9.0f;

    if (g_style && (pink || dim) && !*(bool *)0x97F83B) {
        float o = ScreenH() / 450.0f;   // 2,4 points en 1080p
        if (o < 1.0f) o = 1.0f;
        float ox = MenuSqueezeActive() ? o / MenuSqueezeFactor() : o;   // le texte est resserre ensuite
        shadow = 0;
        col[0] = g_edge[0]; col[1] = g_edge[1]; col[2] = g_edge[2];
        if (dim) { col[0] /= 2; col[1] /= 2; col[2] /= 2; }
        static const int dirs[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
        for (auto &d : dirs) { o_PrintString(x + d[0] * ox, ty + d[1] * o, s); DrawFonts(); }
        col[0] = g_fill[0]; col[1] = g_fill[1]; col[2] = g_fill[2];
        if (dim) { col[0] = (uint8_t)(col[0] * 3 / 5); col[1] = (uint8_t)(col[1] * 3 / 5); col[2] = (uint8_t)(col[2] * 3 / 5); }
        o_PrintString(x, ty, s);
        DrawFonts();
        memcpy(col, saved, 4);
        shadow = oshadow;
    } else {
        o_PrintString(x, ty, s);
    }
    sx = osx;
    sy = osy;
}

// CMenuManager::DrawFrontEnd (0x4A212D) : tout le menu (fond, page, logo, souris).
typedef void(__fastcall *DrawFrontEnd_t)(void *menu, void *edx, char arg);
static DrawFrontEnd_t o_DrawFrontEnd;
static void __fastcall h_DrawFrontEnd(void *menu, void *edx, char arg)
{
    g_inFrontEnd = true;
    o_DrawFrontEnd(menu, edx, arg);
    g_inFrontEnd = false;
}

static bool PatchDrawCall(uintptr_t at, uintptr_t expected, void *hook)
{
    if (*(uint8_t *)at != 0xE8 || at + 5 + *(int32_t *)(at + 1) != expected) {
        Log("interface : appel inattendu en %06X", (unsigned)at);
        return false;
    }
    PatchCall(at, hook);
    return true;
}

void InstallInterface()
{
    LoadSettings();
    srand(GetTickCount());
    static const uint8_t feProlog[] = { 0x53, 0x56, 0x89, 0xCE, 0x55 };
    static const uint8_t psProlog[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x38 };
    o_DrawFrontEnd = (DrawFrontEnd_t)MakeDetour(0x4A212D, feProlog, sizeof(feProlog), (void *)h_DrawFrontEnd);
    o_PrintString = (PrintString_t)MakeDetour(0x551040, psProlog, sizeof(psProlog), (void *)h_PrintString);
    PatchDrawCall(0x4A272A, 0x578710, (void *)h_DrawBackground);
    PatchDrawCall(0x4A2CB2, 0x578710, (void *)h_DrawBackground);
    PatchDrawCall(0x4A3503, 0x578710, (void *)h_DrawLogo);
    PatchDrawCall(0x4A6A7E, 0x578710, (void *)h_DrawSplash);
    static const uintptr_t quads[] = { 0x4A2831, 0x4A292B, 0x4A2A34, 0x4A2DB9, 0x4A2EB3, 0x4A2FC2, 0x4A30D1 };
    for (uintptr_t a : quads) PatchDrawCall(a, 0x578520, (void *)h_FrameQuad);
    Log("interface : texte des menus %s, taille %d%%, dossier %S", g_style ? "blanc a contour" : "d'origine",
        (int)(g_textScale * 100 + 0.5f), Folder().c_str());
}
