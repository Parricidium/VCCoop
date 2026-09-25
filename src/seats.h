// Places dans les vehicules (seats.cpp).
#pragma once
#include "game.h"

bool WarpIntoSeat(void *ped, void *veh, int seat);          // seat : 0 conducteur, 1..8 passager
void WarpOutOfVehicle(void *ped, const game::Vec3 *at);    // at NULL : a cote du vehicule
