// Menu en jeu (touche ToucheMenuJeu, F10 par defaut) et tchat (touche ToucheTchat, T) : panel.cpp.
#pragma once
#include <windows.h>
#include <stdint.h>

void InstallPanel();
void PanelFrame(bool inGame);          // logique : actions demandees, telephone du tchat, admins (hote)
void PanelDraw();                      // dessin, apres l'interface du jeu (Render2dStuff)
bool PanelWndProc(UINT msg, WPARAM wp, LPARAM lp);   // vrai : message avale (menu ouvert, saisie du tchat)
bool PanelWantsMouse();                // menu ouvert : la souris est a nous
void PanelMouse(long dx, long dy, long wheel, bool left);   // lue a la source (camera.cpp, DirectInput)
bool PanelCapturesKeys();              // menu ouvert ou saisie : les raccourcis du mod se taisent
void PanelOnReliable(int from, const uint8_t *data, int len);
