// Vignettes 3D des vehicules et des tenues pour le menu moderne (F10, F7) : rendues en arriere-plan par le moteur
// logiciel du lanceur (launcher\model3d.cpp) a partir des fichiers du jeu, des mods et des packs (un vehicule modde
// apparait tel qu'il est), rangees dans une texture du peripherique Direct3D 9 du pont.
#pragma once
#include <windows.h>

enum { THUMB_VEHICLE = 0, THUMB_PED = 1 };
// Vrai si la vignette est prete : uv (u0, v0, u1, v1) dans ThumbAtlas() et son format largeur / hauteur. Sinon elle
// est demandee (prete quelques images plus tard).
bool ThumbGet(int kind, const char *model, float uv[4], float *aspect);
struct IDirect3DTexture9 *ThumbAtlas();
void ThumbFrame();     // envoie a la carte les vignettes finies (fil principal, avant de dessiner l'interface)
void ThumbRelease();   // avant Reset du peripherique
