// Population partagee (hybride selon la distance a l'hote).
//  - Invite a moins de 60 m de l'hote : "mode partage". Sa propre population s'arrete (densite des pietons a 0,
//    generateurs de circulation et de voitures garees sautes) et ses passants / voitures locaux sont retires des
//    qu'ils sont hors ecran ; il voit a la place les copies de ceux de l'hote (entities.cpp, vehicles.cpp).
//  - Au-dela de 100 m : il retrouve sa propre population (la marge evite les bascules en boucle).
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "entities.h"
#include "vehicles.h"
#include "seats.h"
#include "population.h"
#include <string.h>

using namespace game;

enum { SHARE_ENTER_M = 60, SHARE_LEAVE_M = 100 };
enum { VEH_RANDOM = 1, VEH_PARKED = 3 };

static bool g_shared;
static float g_savedPedDensity = 1.0f;

bool PopulationShared() { return g_shared; }

static float &PedDensity() { return *(float *)0x694DC0; }   // CPopulation::PedDensityMultiplier
static bool OnScreen(void *e) { return ((bool(__thiscall *)(void *))0x4885D0)(e); }
static void RemovePed(void *ped) { ((void(__cdecl *)(void *))0x53B160)(ped); }   // CPopulation::RemovePed

// --- Generateurs saute en mode partage ---
typedef void(__cdecl *GenCars_t)();
static GenCars_t o_GenerateRandomCars;
static void __cdecl h_GenerateRandomCars() { if (!g_shared) o_GenerateRandomCars(); }

typedef void(__fastcall *CarGen_t)(void *gen, void *edx);
static CarGen_t o_CarGenProcess;
static void __fastcall h_CarGenProcess(void *gen, void *edx) { if (!g_shared) o_CarGenProcess(gen, edx); }

void InstallPopulation()
{
    static const uint8_t genPro[] = { 0x80, 0x3D, 0xB2, 0x0A, 0xA1, 0x00, 0x00 };
    o_GenerateRandomCars = (GenCars_t)MakeDetour(0x4292A0, genPro, sizeof(genPro), (void *)h_GenerateRandomCars);
    static const uint8_t carGenPro[] = { 0x53, 0x56, 0x57, 0x55, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00 };
    o_CarGenProcess = (CarGen_t)MakeDetour(0x5A71C0, carGenPro, sizeof(carGenPro), (void *)h_CarGenProcess);
}

// Une entite ambiante locale peut-elle etre retiree ? (jamais ce qu'un joueur ou le reseau utilise)
static bool LocalAmbientPed(void *ped, void *me)
{
    return ped != me && CharCreatedBy(ped) == 1 && !IsGhostPed(ped) && !IsPuppet(ped) && !InVehicle(ped);
}

static bool LocalAmbientVehicle(void *v, void *me)
{
    uint8_t by = Field<uint8_t>(v, 0x1F8);
    if ((by != VEH_RANDOM && by != VEH_PARKED) || NetVehicleId(v) || PedVehicle(me) == v) return false;
    void *drv = VehDriver(v);
    if (drv && (drv == me || IsPuppet(drv) || IsGhostPed(drv))) return false;
    for (int i = 0; i < 8; i++) {
        void *p = VehPassenger(v, i);
        if (p && (p == me || IsPuppet(p) || IsGhostPed(p))) return false;
    }
    return true;
}

static void DeleteVehicleWithOccupants(void *v)
{
    void *occ[9] = { VehDriver(v) };
    for (int i = 0; i < 8; i++) occ[i + 1] = VehPassenger(v, i);
    for (void *p : occ) {
        if (!p) continue;
        WarpOutOfVehicle(p, NULL);
        RemovePed(p);
    }
    WorldRemove(v);
    RemoveReferencesToDeletedObject(v);
    DeleteEntity(v);
}

static void CleanLocalPopulation()
{
    static uint32_t last;
    uint32_t now = GetTickCount();
    if (now - last < 500) return;
    last = now;
    void *me = FindPlayerPed();
    int peds = 0, cars = 0;
    Pool *pp = PedPool();
    for (int i = 0; i < pp->size; i++) {
        if (pp->flags[i] & 0x80) continue;
        void *ped = pp->objects + i * PED_POOL_ENTRY;
        if (LocalAmbientPed(ped, me) && !OnScreen(ped)) { RemovePed(ped); peds++; }
    }
    Pool *vp = VehiclePool();
    for (int i = 0; i < vp->size; i++) {
        if (vp->flags[i] & 0x80) continue;
        void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
        if (LocalAmbientVehicle(v, me) && !OnScreen(v)) { DeleteVehicleWithOccupants(v); cars++; }
    }
    if ((peds || cars) && g_cfg.logScripts) Log("population : %d passants et %d vehicules locaux retires", peds, cars);
}

void PopulationFrame(bool inGame)
{
    if (!inGame || g_cfg.host || g_localId <= 0) {
        if (g_shared) { g_shared = false; PedDensity() = g_savedPedDensity; }
        return;
    }
    const NetPlayer &h = g_players[0];
    void *me = FindPlayerPed();
    bool hostHere = h.connected && h.state.inGame && h.state.area == AreaCode(me);
    float dx = h.state.pos[0] - Pos(me).x, dy = h.state.pos[1] - Pos(me).y;
    float d2 = dx * dx + dy * dy;
    bool want = hostHere && d2 < (g_shared ? SHARE_LEAVE_M * SHARE_LEAVE_M : SHARE_ENTER_M * SHARE_ENTER_M);
    if (want != g_shared) {
        g_shared = want;
        if (want) { g_savedPedDensity = PedDensity(); Log("population : partagee avec l'hote"); }
        else { PedDensity() = g_savedPedDensity; Log("population : locale (loin de l'hote)"); }
    }
    static uint32_t lastStat;
    if (g_cfg.logScripts && GetTickCount() - lastStat > 10000) {
        lastStat = GetTickCount();
        int localPeds = 0, ghosts = 0, localCars = 0, copies = 0;
        Pool *pp = PedPool();
        for (int i = 0; i < pp->size; i++) {
            if (pp->flags[i] & 0x80) continue;
            void *ped = pp->objects + i * PED_POOL_ENTRY;
            if (IsGhostPed(ped)) ghosts++;
            else if (CharCreatedBy(ped) == 1) localPeds++;
        }
        Pool *vp = VehiclePool();
        for (int i = 0; i < vp->size; i++) {
            if (vp->flags[i] & 0x80) continue;
            void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
            if (NetVehicleId(v)) copies++;
            else if (Field<uint8_t>(v, 0x1F8) == 1 || Field<uint8_t>(v, 0x1F8) == 3) localCars++;
        }
        Log("population (%s) : passants locaux %d, copies de l'hote %d ; voitures locales %d, reseau %d",
            g_shared ? "partagee" : "locale", localPeds, ghosts, localCars, copies);
    }
    if (!g_shared) return;
    if (PedDensity() != 0.0f) { g_savedPedDensity = PedDensity(); PedDensity() = 0.0f; }   // une mission a pu la changer
    CleanLocalPopulation();
}
