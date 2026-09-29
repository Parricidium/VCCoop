// Places dans les vehicules (seats.cpp).
#pragma once
#include "game.h"

bool WarpIntoSeat(void *ped, void *veh, int seat);          // seat : 0 conducteur, 1..8 passager
void WarpOutOfVehicle(void *ped, const game::Vec3 *at);    // at NULL : a cote du vehicule
// Montee avec l'animation du jeu (marche, portiere, assise), sans l'IA des scripts : seat 0 volant, sinon passager
// (premiere place libre). walk : s'il est loin de la portiere, il y marche d'abord. Vrai si la scene a demarre.
bool StartEnterAnimated(void *ped, void *veh, int seat, bool walk);
// Descente avec l'animation du jeu (CPed::SetExitCar 0x516C60, porte choisie par le jeu), sans l'objectif LEAVE_CAR
// (l'IA du double d'un joueur ne le traitait pas : il restait assis). Vrai si la scene a demarre.
bool StartExitAnimated(void *ped, void *veh);
bool EnterInProgress(void *ped);   // en train de marcher vers la portiere / de monter
float DoorDistance(void *ped, void *veh, int seat);   // distance a la portiere qu'il prendrait (1e9 si pas de place)
void AbortEnter(void *ped);        // interrompt proprement (vehicule et personnage nettoyes)
void LogEnterProgress(void *ped, const char *who);   // diagnostic
