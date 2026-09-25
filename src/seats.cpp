// Monter / descendre instantanement d'un vehicule, a n'importe quelle place (conducteur ou passager).
// Pour les Tommy distants, les copies de personnages de mission et le joueur local en passager.
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include "seats.h"

using namespace game;

bool WarpIntoSeat(void *ped, void *veh, int seat)
{
    if (seat == 0) {
        if (VehDriver(veh) && VehDriver(veh) != ped) return false;
        WarpPedIntoCar(ped, veh);
        if (VehDriver(veh) != ped) SetDriver(veh, ped);   // WarpPedIntoCar ne l'inscrit pas toujours
    } else {
        // Comme CREATE_CHAR_AS_PASSENGER : etat "conduite", vehicule reference, place de passager, animations.
        PedState(ped) = PED_DRIVING;
        PedVehicle(ped) = veh;
        RegisterReference(veh, &PedVehicle(ped));
        InVehicle(ped) = true;
        if (!AddPassenger(veh, ped)) {
            InVehicle(ped) = false;
            CleanUpOldReference(veh, &PedVehicle(ped));
            PedVehicle(ped) = NULL;
            return false;
        }
        Field<uint8_t>(ped, 0x51) &= ~0x01;   // bUsesCollision
        AddInCarAnims(ped, veh, false);
    }
    Field<uint8_t>(ped, 0x52) |= 0x04;   // visible
    return true;
}

void WarpOutOfVehicle(void *ped, const Vec3 *at)
{
    void *veh = InVehicle(ped) ? PedVehicle(ped) : NULL;
    if (veh) {
        if (VehDriver(veh) == ped) RemoveDriver(veh);
        else if (SeatOf(veh, ped) > 0) RemovePassenger(veh, ped);
        CleanUpOldReference(veh, &PedVehicle(ped));
    }
    InVehicle(ped) = false;
    PedVehicle(ped) = NULL;
    RemoveInCarAnims(ped, false);
    SetIdle(ped);
    Field<uint8_t>(ped, 0x51) |= 0x01;
    if (at) Pos(ped) = *at;
    else if (veh) { Vec3 p = Pos(veh); Pos(ped) = { p.x + Field<Vec3>(veh, 0x04).x * 2.0f, p.y + Field<Vec3>(veh, 0x04).y * 2.0f, p.z + 0.5f }; }
}
