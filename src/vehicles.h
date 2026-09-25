// Vehicules reseau (vehicles.cpp).
#pragma once
#include <stdint.h>

void VehiclesInit();
void VehiclesFrame(bool inGame);
void *NetVehicleById(uint32_t id);   // vehicule local correspondant (NULL si pas encore cree)
uint32_t NetVehicleId(void *veh);    // 0 si le vehicule n'est pas en reseau
