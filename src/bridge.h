// Pont Direct3D 8 -> Direct3D 9 (bridge.cpp) et rendu moderne qui s'appuie dessus (gfx9.cpp).
#pragma once
#include <windows.h>

// Rendu=9 : Direct3DCreate8 du jeu renvoie notre IDirect3D8, qui traduit tout vers Direct3D 9. NULL si d3d9.dll
// est absent ou refuse (le jeu garde alors son Direct3D 8 d'origine et l'ancien rendu gfx.cpp).
void *BridgeCreate8(UINT sdk);
bool IsBridgeDevice(void *dev8);
struct IDirect3DDevice9 *BridgeDevice9();

// --- Appels du pont vers le rendu moderne (gfx9.cpp) ---
struct GfxDraw {
    bool indexed;
    UINT type, baseVertex, minIndex, numVerts, start, count;
};
void Gfx9DeviceCreated(IDirect3DDevice9 *dev, UINT width, UINT height, bool msaa);
void Gfx9BeforeReset();
void Gfx9AfterReset(UINT width, UINT height, bool msaa);
void Gfx9BeginScene();
void Gfx9EndScene();
bool Gfx9Intercept(DWORD fvf, const GfxDraw &d, bool up);   // vrai : dessin garde par le rendu moderne, pas affiche
void Gfx9BeforeDraw(DWORD fvf, bool up);           // avant chaque dessin du jeu (peut poser le masque d'ombre)
void Gfx9AfterDraw(DWORD fvf, const GfxDraw &d);    // apres chaque dessin du jeu (tampons de sommets / d'indices)
void Gfx9BeforePresent();
void Gfx9DrawDone();          // apres chaque dessin du jeu (retire nos shaders poses pour ce dessin)
void Gfx9BeforeHud();         // players.cpp, avant Render2dStuff : post-traitement (SMAA, eclat, etalonnage, nettete)
void InstallGfx9Hooks();
void Gfx9SettingsChanged();   // qualite des ombres changee au menu : ressources refaites a la prochaine image
// Projeteurs d'ombre seulement : les dessins du jeu sont notes par le rendu moderne mais pas affiches (objets hors
// du champ de la camera dont l'ombre peut y tomber).
extern bool g_bridgeCasterOnly;
// Tampon de sommets dynamique : copie en memoire du contenu ecrit par le jeu (les dessins sont rejoues plus tard
// dans l'image, apres que le jeu a pu reecrire le tampon).
const BYTE *BridgeVertexMirror(struct IDirect3DVertexBuffer9 *vb);
const BYTE *BridgeIndexMirror(struct IDirect3DIndexBuffer9 *ib);

// --- Ray tracing materiel (Rendu=12, rt.cpp) : vcrt64.exe (64 bits, Direct3D 12 + DXR) trace les rayons ---
// Un dessin note par gfx9.cpp. vbData / ibData : sommets / indices en memoire (tampons reecrits a chaque image ;
// sinon lus dans le tampon du jeu une fois, gardes chez vcrt64 tant que le tampon vit).
struct RtDraw {
    struct IDirect3DVertexBuffer9 *vb;
    struct IDirect3DIndexBuffer9 *ib;
    struct IDirect3DBaseTexture9 *tex;
    const BYTE *vbData;
    const WORD *ibData;
    UINT stride;
    DWORD fvf;
    GfxDraw d;
    const float *world;
    float alphaRef;
    DWORD tint;          // couleur de la matiere (peinture des voitures)
    bool alphaTest, vehicle, dynamic;
};
// Une image : camera, lumieres, fonctions (1 soleil, 2 occlusion, 4 reflets, 8 lumiere indirecte).
struct RtParams {
    const float *view, *proj;
    float sun[4], sunColor[4], ambient[4], skyTop[4], skyBottom[4];
    float sunAngle, maxDist, wetness, aoRadius;
    unsigned features;
    bool reset;
    UINT outW, outH;
};
bool RtHelperPresent();       // VCCoop\vcrt64.exe present
void RtStart();               // Rendu=12 : lance vcrt64.exe (a la creation du peripherique)
bool RtReady();               // vcrt64.exe pret (carte DXR)
void RtBeforeReset();
void RtForgetResource(void *real);   // tampon ou texture Direct3D 9 libere ou reecrit (bridge.cpp)
// Envoie l'image ; vrai si une image tracee est disponible (celle-ci, ou la precedente si vcrt64 est en retard).
bool RtTrace(const RtDraw *draws, int count, const RtParams &p);
// 0 : x = visibilite du soleil, y = profondeur (m), z = occlusion (demi-flottants) ; 1 : reflet (rgb) et son poids (a) ;
// 2 : lumiere indirecte (rgb x 4).
struct IDirect3DTexture9 *RtResult(int plane = 0);
void RtResultSize(float *w, float *h);
