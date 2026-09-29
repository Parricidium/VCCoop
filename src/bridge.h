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
