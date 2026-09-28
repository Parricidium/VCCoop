// Monter / descendre instantanement d'un vehicule, a n'importe quelle place (conducteur ou passager).
// Pour les Tommy distants, les copies de personnages de mission et le joueur local en passager.
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include "seats.h"
#include <math.h>

using namespace game;

enum { OBJECTIVE_NONE = 0, OBJECTIVE_ENTER_CAR_AS_PASSENGER = 0x11, OBJECTIVE_ENTER_CAR_AS_DRIVER = 0x12 };
static int &Objective(void *ped) { return Field<int>(ped, 0x164); }

// ======================================================================= Montee animee, sans l'IA des scripts
// (retro-ingenierie du 27/09, re\out\enter1-4.c, seekcar.c) : la sequence du jeu (marche vers la portiere, alignement,
// ouverture, assise, fermeture) se lance avec CPed::SetSeekCar (loin) ou CPed::SetEnterCar (a la portiere), a
// condition de poser nous-memes l'objectif (+0x164 : 0x12 volant / 0x11 passager, lu par les callbacks), le vehicule
// vise (+0x170) et le noeud de porte (+0x380, SetEnterCar ignore son argument). Le jeu ne refuse pas les voitures
// conduites par un joueur : nos echecs venaient d'abandons qui laissaient le vehicule "pollue" (+0x1CD, +0x1CE).
static void  SetSeekCar(void *ped, void *veh, int node)  { ((void(__thiscall *)(void *, void *, int))0x4F54D0)(ped, veh, node); }
static void  SetEnterCar(void *ped, void *veh, int node) { ((void(__thiscall *)(void *, void *, int))0x518080)(ped, veh, node); }
static void  QuitEnteringCar(void *ped)                  { ((void(__thiscall *)(void *))0x5179D0)(ped); }
static bool  IsPedInControl(void *ped)                   { return ((bool(__thiscall *)(void *))0x501950)(ped); }
static void  DoorPos(Vec3 *out, void *veh, int node)     { ((void(__cdecl *)(Vec3 *, void *, int))0x5164D0)(out, veh, node); }
static void  ClearObjectiveFn(void *ped)                 { ((void(__thiscall *)(void *))0x521720)(ped); }
static void  SetIdleFn(void *ped)                        { ((void(__thiscall *)(void *))0x4FDFD0)(ped); }
enum { PED_SEEK_CAR = 0x18, PED_CARJACK = 0x38, PED_ENTER_CAR = 0x3A };
static int FreePassengerSlot(void *veh);

bool EnterInProgress(void *ped)
{
    int st = PedState(ped);
    return !InVehicle(ped) && (st == PED_SEEK_CAR || st == PED_ENTER_CAR || st == PED_CARJACK);
}

static int DoorFlag(void *veh, int node)
{
    if (VehClass(veh) == VCLASS_BIKE) return (node == 0x0F || node == 0x0B) ? 5 : 10;
    return node == 0x0F ? 1 : node == 0x10 ? 2 : node == 0x0B ? 4 : 8;
}

// Noeud de porte pour une place : volant 0xF/0xB (le plus proche), passagers 0xB (avant droite -> place 0),
// 0x10 (arriere gauche -> 1), 0xC (arriere droite -> 2) ; moto : passager 0x10/0xC.
static float DoorDist2(void *ped, void *veh, int node)
{
    Vec3 d, p = Pos(ped);
    DoorPos(&d, veh, node);
    return (d.x - p.x) * (d.x - p.x) + (d.y - p.y) * (d.y - p.y);
}

static int DoorNodeForSeat(void *ped, void *veh, int seat)
{
    bool bike = VehClass(veh) == VCLASS_BIKE;
    int a, b;
    if (seat == 0) { a = 0x0F; b = 0x0B; }
    else if (bike) { a = 0x10; b = 0x0C; }
    else if (seat == 1) return 0x0B;
    else if (seat == 2) return 0x10;
    else if (seat == 3) return 0x0C;
    else return 0x0B;
    return DoorDist2(ped, veh, a) <= DoorDist2(ped, veh, b) ? a : b;
}

// Distance (m) du personnage a la portiere qu'il prendrait pour cette place.
float DoorDistance(void *ped, void *veh, int seat)
{
    int slot = 0;
    if (seat != 0) { slot = FreePassengerSlot(veh); if (slot < 0) return 1e9f; }
    return sqrtf(DoorDist2(ped, veh, seat == 0 ? 0 : slot + 1));
}

// Place passager libre (indice 0..2 des pPassengers, pour le noeud de porte), -1 si aucune.
static int FreePassengerSlot(void *veh)
{
    if (Field<uint8_t>(veh, 0x1CC) >= Field<uint8_t>(veh, 0x1D0)) return -1;
    if (VehClass(veh) == VCLASS_BIKE) return VehPassenger(veh, 0) ? -1 : 0;
    int max = Field<uint8_t>(veh, 0x1D0);
    for (int i = 0; i < 3 && i < max; i++) if (!VehPassenger(veh, i)) return i;
    return -1;
}

// Drapeaux de porte / compteur "en train de monter" laisses par une montee interrompue sans nettoyage : effaces si
// plus personne ne monte dans ce vehicule (sinon toute montee suivante etait refusee en silence).
static void CleanOrphanEntryFlags(void *veh)
{
    if (!Field<uint8_t>(veh, 0x1CD) && !Field<uint8_t>(veh, 0x1CE)) return;
    Pool *pool = PedPool();
    for (int i = 0; i < pool->size; i++) {
        if (pool->flags[i] & 0x80) continue;
        void *p = pool->objects + i * PED_POOL_ENTRY;
        int st = PedState(p);
        if ((st == PED_ENTER_CAR || st == PED_CARJACK || st == PED_SEEK_CAR) && PedVehicle(p) == veh) return;
    }
    Log("sieges : drapeaux de montee orphelins effaces sur %p (%d, %02X)", veh, Field<uint8_t>(veh, 0x1CD), Field<uint8_t>(veh, 0x1CE));
    Field<uint8_t>(veh, 0x1CD) = 0;
    Field<uint8_t>(veh, 0x1CE) = 0;
    Field<uint8_t>(veh, 0x1FB) &= ~0x10;
}

void AbortEnter(void *ped)
{
    if (InVehicle(ped)) return;
    int st = PedState(ped);
    void *veh = PedVehicle(ped);
    if (st == PED_ENTER_CAR || st == PED_CARJACK || (Field<void *>(ped, 0x1F8) && veh)) QuitEnteringCar(ped);
    else if (st == PED_SEEK_CAR) { ClearObjectiveFn(ped); SetIdleFn(ped); }
    Objective(ped) = 0;
    Field<int>(ped, 0x168) = 0;
    void *&target = Field<void *>(ped, 0x170);
    if (target) { CleanUpOldReference(target, &target); target = NULL; }
    if (veh) CleanOrphanEntryFlags(veh);
}

bool StartEnterAnimated(void *ped, void *veh, int seat, bool walk)
{
    if (InVehicle(ped) || !IsPedInControl(ped) || Field<void *>(ped, 0x1F8)) return false;
    if (EnterInProgress(ped)) return false;
    if (EntityStatus(veh) == STATUS_WRECKED || (Field<uint8_t>(veh, 0x1FB) & 0x10)) return false;
    if (Field<Vec3>(veh, 0x24).z < 0.3f) return false;   // sur le flanc ou le toit
    int lock = Field<int>(veh, 0x230);
    if (lock == 2 || lock == 4 || lock == 5 || lock == 7) return false;
    int slot = -1;
    if (seat == 0) { if (VehDriver(veh)) return false; }
    else { slot = FreePassengerSlot(veh); if (slot < 0) return false; }
    CleanOrphanEntryFlags(veh);
    int node = DoorNodeForSeat(ped, veh, seat == 0 ? 0 : slot + 1);
    if (Field<uint8_t>(veh, 0x1CE) & DoorFlag(veh, node)) return false;   // quelqu'un monte deja par la
    Field<int>(ped, 0x168) = 0;                                              // m_prevObjective : retombe a 0 une fois assis
    Objective(ped) = seat == 0 ? OBJECTIVE_ENTER_CAR_AS_DRIVER : OBJECTIVE_ENTER_CAR_AS_PASSENGER;
    void *&target = Field<void *>(ped, 0x170);
    if (target) CleanUpOldReference(target, &target);
    target = veh;
    RegisterReference(veh, &target);
    Field<int16_t>(ped, 0x380) = (int16_t)node;
    Vec3 door, p = Pos(ped);
    DoorPos(&door, veh, node);
    float d2 = (door.x - p.x) * (door.x - p.x) + (door.y - p.y) * (door.y - p.y);
    if (walk && d2 > 2.5f * 2.5f) SetSeekCar(ped, veh, 0);   // loin : le jeu marche a la portiere puis lance SetEnterCar
    else SetEnterCar(ped, veh, node);
    int st = PedState(ped);
    if (st != PED_ENTER_CAR && st != PED_SEEK_CAR) {   // refuse (porte pas prete, animation residuelle...)
        Objective(ped) = 0;
        CleanUpOldReference(target, &target);
        target = NULL;
        return false;
    }
    return true;
}

// Diagnostic d'une montee en cours (une ligne par seconde).
void LogEnterProgress(void *ped, const char *who)
{
    void *veh = PedVehicle(ped);
    Vec3 door = { 0, 0, 0 }, p = Pos(ped);
    int node = Field<int16_t>(ped, 0x380);
    if (veh && node) DoorPos(&door, veh, node);
    void *a = Field<void *>(ped, 0x1F8);
    Log("sieges : %s etat %d, objectif %d, porte %X, veh %p (montent %d, drapeaux %02X, statut %d), anim %p [id %d t %.2f/%.2f fondu %.2f dr %04X], dist porte %.2f, deplacement %d, controle %d, collision %d",
        who, PedState(ped), Objective(ped), node, veh, veh ? Field<uint8_t>(veh, 0x1CD) : 0, veh ? Field<uint8_t>(veh, 0x1CE) : 0,
        veh ? EntityStatus(veh) : -1, a, a ? Field<int16_t>(a, 0x2C) : -1, a ? Field<float>(a, 0x20) : 0.0f, a && Field<void *>(a, 0x14) ? Field<float>(Field<void *>(a, 0x14), 0x10) : 0.0f,
        a ? Field<float>(a, 0x18) : 0.0f, a ? Field<uint16_t>(a, 0x2E) : 0,
        veh ? sqrtf((door.x - p.x) * (door.x - p.x) + (door.y - p.y) * (door.y - p.y)) : 0.0f, MoveState(ped), IsPedInControl(ped), Field<uint8_t>(ped, 0x51) & 1);
}

bool WarpIntoSeat(void *ped, void *veh, int seat)
{
    if (EnterInProgress(ped)) AbortEnter(ped);   // montee animee en cours : nettoyee avant de poser
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
        // Plus de place (m_nNumPassengers +0x1CC, m_nMaxPassengers +0x1D0) : on ne touche a rien. (Avant, l'etat
        // "conduite" restait pose sur un personnage reste dehors : plantage chez un invite qui voulait monter dans
        // une 2 places ou etaient deja l'hote et une passagere de mission.)
        if (Field<uint8_t>(veh, 0x1CC) >= Field<uint8_t>(veh, 0x1D0)) return false;
        // Comme CREATE_CHAR_AS_PASSENGER : etat "conduite", vehicule reference, place de passager, animations.
        int oldState = PedState(ped);
        PedState(ped) = PED_DRIVING;
        PedVehicle(ped) = veh;
        RegisterReference(veh, &PedVehicle(ped));
        InVehicle(ped) = true;
        if (!AddPassenger(veh, ped)) {
            InVehicle(ped) = false;
            CleanUpOldReference(veh, &PedVehicle(ped));
            PedVehicle(ped) = NULL;
            PedState(ped) = oldState;
            return false;
        }
        Field<uint8_t>(ped, 0x51) &= ~0x01;   // bUsesCollision
        AddInCarAnims(ped, veh, false);
        // Pas ejecte quand un conducteur monte avec l'animation (PedSetInCarCB donne "descendre" aux passagers sans ce bit).
        if (ped != FindPlayerPed()) Field<uint8_t>(ped, 0x156) |= 0x80;   // bStayInCarOnJack (0x08 = otage : cris de panique)
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
    if (EnterInProgress(ped)) AbortEnter(ped);   // QuitEnteringCar : compteur et drapeau de porte du vehicule rendus
    if (!InVehicle(ped) && !PedVehicle(ped) && !Field<void *>(ped, 0x1F8)) return;   // rien a defaire
    if (ped != FindPlayerPed()) Field<uint8_t>(ped, 0x156) &= ~0x80;
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
    SetMoveStateFn(ped, 1);   // PEDMOVE_STILL (avec 0, l'etat envoye aux autres ne leur laissait poser aucune animation)
    BlendAnimation(Field<void *>(ped, 0x4C), Field<int>(ped, 0x1F4), 3, 1000.0f);   // debout, au repos
    Vec3 p;
    if (at) p = *at;
    else if (veh) {
        // A cote du vehicule, a l'horizontale : "droite" projetee au sol (sur le flanc ou sur le toit, elle pointait
        // en l'air ou sous le sol et le personnage tombait ou passait sous la route).
        Vec3 v = Pos(veh), r = Field<Vec3>(veh, 0x04);
        float l = sqrtf(r.x * r.x + r.y * r.y);
        if (l < 0.3f) { r = Field<Vec3>(veh, 0x14); l = sqrtf(r.x * r.x + r.y * r.y); }
        if (l < 0.01f) { r = { 1, 0, 0 }; l = 1.0f; }
        bool upsideDown = Field<Vec3>(veh, 0x24).z < 0.0f;
        p = { v.x + r.x / l * 2.5f, v.y + r.y / l * 2.5f, v.z + (upsideDown ? 0.3f : 0.5f) };
    }
    else p = Pos(ped);
    Teleport(ped, p);
}
