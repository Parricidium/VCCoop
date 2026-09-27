// Personnages de mission de l'hote reproduits chez les invites (entities.cpp).
#pragma once
#include <stdint.h>

void EntitiesInit();
void EntitiesFrame(bool inGame);
bool IsGhostPed(void *ped);     // copie d'un personnage de l'hote (invite)
bool GhostHostHandle(void *ped, uint32_t &host);   // reference du personnage chez l'hote (copie d'un perso de l'hote)
bool GhostOwner(void *ped, uint8_t &owner, uint32_t &handle);   // copie : son proprietaire et sa reference chez lui
void *GhostPedOf(uint8_t owner, uint32_t handle);   // copie chez nous du personnage (proprietaire, reference), NULL sinon
int PuppetPlayer(void *ped);    // numero du joueur dont c'est le Tommy, -1 sinon (coop.cpp)
bool IsPuppetVehicle(void *veh); // vehicule conduit par le Tommy d'un autre joueur (coop.cpp)
bool IsPuppet(void *ped);       // Tommy d'un autre joueur (coop.cpp)
void *PuppetPed(int player);    // son personnage chez nous (coop.cpp), NULL s'il n'existe pas
extern uint32_t g_hostPlayerHandle;   // reference de pool du Tommy de l'hote chez l'hote
