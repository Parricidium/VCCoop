// Vehicules reseau (vehicles.cpp).
#pragma once
#include <stdint.h>

void VehiclesInit();
void VehiclesFrame(bool inGame);
void *NetVehicleById(uint32_t id);   // vehicule local correspondant (NULL si pas encore cree)
uint32_t NetVehicleId(void *veh);    // 0 si le vehicule n'est pas en reseau
bool NetVehicleMoving(void *veh);
bool NetVehicleIsCopy(void *veh);    // copie d'un vehicule d'un autre joueur (sa physique est chez lui)    // roule (vitesse recue pour une copie, physique sinon)
void ApplyVehicleDamage(uint32_t id, float damage);   // proprietaire : un autre joueur a abime ce vehicule chez lui
