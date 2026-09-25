// Monter / descendre instantanement d'un vehicule, a n'importe quelle place (conducteur ou passager).
// Pour les Tommy distants, les copies de personnages de mission et le joueur local en passager.
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include "seats.h"

using namespace game;

enum { OBJECTIVE_NONE = 0, OBJECTIVE_ENTER_CAR_AS_DRIVER = 0x12 };
static int &Objective(void *ped) { return Field<int>(ped, 0x164); }

bool WarpIntoSeat(void *ped, void *veh, int seat)
{
    if (seat == 0) {
        if (VehDriver(veh) && VehDriver(veh) != ped) return false;
        // CPed::WarpPedIntoCar ne fait presque rien si le personnage n'a pas l'objectif "monter au volant" : ni
        // conducteur inscrit, ni animation assise (le Tommy distant restait debout dans la voiture, en marchant).
        int objective = Objective(ped);
        Objective(ped) = OBJECTIVE_ENTER_CAR_AS_DRIVER;
        WarpPedIntoCar(ped, veh);
        Objective(ped) = objective == OBJECTIVE_ENTER_CAR_AS_DRIVER ? OBJECTIVE_NONE : objective;
        if (VehDriver(veh) != ped) SetDriver(veh, ped);
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

// Comme WARP_CHAR_FROM_CAR_TO_COORD (0362) : l'animation assise (m_pVehicleAnim, +0x1F8) doit etre retiree. Les
// animations de moto ne sont pas des animations "en voiture" (drapeau 0x2000) : RemoveInCarAnims les laissait, le
// personnage gardait une animation dont le bloc pouvait etre decharge ensuite (plantage en 0x403ED2 quand un
// passager descendait d'une moto avec G).
void WarpOutOfVehicle(void *ped, const Vec3 *at)
{
    void *veh = InVehicle(ped) ? PedVehicle(ped) : NULL;
    if (veh) {
        if (VehDriver(veh) == ped) {
            RemoveDriver(veh);
            if (EntityStatus(veh) != STATUS_WRECKED) SetEntityStatus(veh, STATUS_ABANDONED);
        } else if (SeatOf(veh, ped) > 0) RemovePassenger(veh, ped);
        CleanUpOldReference(veh, &PedVehicle(ped));
    }
    RemoveInCarAnims(ped, true);
    InVehicle(ped) = false;
    PedVehicle(ped) = NULL;
    if (PedState(ped) == 11) ((void(__thiscall *)(void *))0x4F7920)(ped);   // suivait un chemin : on l'oublie
    PedState(ped) = 1;                       // PED_IDLE
    Field<int>(ped, 0x248) = 0;              // m_eLastPedState
    Field<uint8_t>(ped, 0x51) |= 0x01;       // bUsesCollision
    MoveSpeed(ped) = { 0, 0, 0 };
    ((void(__thiscall *)(void *))0x4FF5A0)(ped);   // arme en main a nouveau visible
    void *&vehAnim = Field<void *>(ped, 0x1F8);
    if (vehAnim) Field<float>(vehAnim, 0x1C) = -1000.0f;   // blendDelta : disparait tout de suite
    vehAnim = NULL;
    ((void(__thiscall *)(void *))0x50CCF0)(ped);   // les animations restantes se terminent normalement
    SetMoveStateFn(ped, 0);
    BlendAnimation(Field<void *>(ped, 0x4C), Field<int>(ped, 0x1F4), 3, 1000.0f);   // debout, au repos
    Vec3 p;
    if (at) p = *at;
    else if (veh) { Vec3 v = Pos(veh); p = { v.x + Field<Vec3>(veh, 0x04).x * 2.0f, v.y + Field<Vec3>(veh, 0x04).y * 2.0f, v.z + 0.5f }; }
    else p = Pos(ped);
    Teleport(ped, p);
}
