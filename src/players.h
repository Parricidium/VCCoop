// Reperes des joueurs : radar, pseudos, choix de tenue (players.cpp).
#pragma once
#include <stdint.h>

uint32_t PlayerColor(int id);            // couleur du joueur (RGBA, rouge en poids fort)
int AddPlayerBlip(void *ped, int player); // point de couleur sur le radar ; -1 si pas de place
void RemovePlayerBlip(int &blip);
void PlayersFrame(bool inGame);          // touche F7 et menu de tenue
bool SkinMenuOpen();
// Menu de tenue (F7), pour le panneau moderne des portraits (panel.cpp) : liste, tenue essayee, choix a la souris.
int SkinCount();
const char *SkinName(int i);
int SkinIndex();
void SkinChoose(int i);          // essayer la tenue i (comme Gauche / Droite)
void SkinMenuClose(bool keep);   // garder (Entree) ou annuler (Retour)
void SkinMenuOpenNow();          // (tests : comme F7)
int RegularPedModel(const char *name);   // modele de passant (1..108) de ce nom, -1 sinon
bool RedressPed(void *ped, int model, const char *special);   // change le modele sur place (pantin qui change de tenue)
const char *PedOutfit(void *ped);        // nom de la tenue portee (modele special ou de passant)
