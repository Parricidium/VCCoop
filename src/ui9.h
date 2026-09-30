// Interface moderne du menu en jeu (F10) : dessinee par VCCoop sur le peripherique Direct3D 9 du pont (Rendu=9 ou 12) :
// verre depoli (image du jeu floutee derriere le panneau), formes arrondies lissees, ombres douces, police Segoe UI.
// Sans pont (Rendu=8) : UiReady() est faux, le menu garde les rectangles et la police du jeu.
// Couleurs : 0xRRGGBBAA. Coordonnees : pixels de l'ecran.
#pragma once
#include <windows.h>
#include <stdint.h>

enum { UI_LEFT, UI_CENTER, UI_RIGHT };

bool UiReady();
void UiBegin();
// Rectangle arrondi : degrade vertical (haut -> bas), liseré (epaisseur bw, 0 : aucun).
void UiRect(float x0, float y0, float x1, float y1, float r, uint32_t top, uint32_t bottom, uint32_t border = 0, float bw = 0);
// Degrade horizontal (gauche -> droite) : element actif, rose -> orange comme le lanceur.
void UiRectH(float x0, float y0, float x1, float y1, float r, uint32_t left, uint32_t right, uint32_t border = 0, float bw = 0);
// Panneau de verre : l'image derriere, floutee, teintee par tint (alpha = force de la teinte).
void UiGlass(float x0, float y0, float x1, float y1, float r, uint32_t tint, uint32_t border, float bw);
// Ombre douce autour d'un rectangle arrondi.
void UiShadow(float x0, float y0, float x1, float y1, float r, float spread, uint32_t color);
void UiTri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color);
// Texte (Windows-1252) ; y : haut de la ligne ; px : hauteur de la police. Rend la largeur.
float UiText(float x, float y, float px, uint32_t color, int align, const char *s, bool bold = false);
float UiTextWidth(const char *s, float px, bool bold = false);
void UiEnd();       // dessine la liste (dans la scene du jeu, apres son interface)
void UiRelease();   // avant Reset du peripherique
