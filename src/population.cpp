// Population partagee (hybride selon la distance a l'hote).
//  - Invite a moins de SHARE_ENTER_M de l'hote : "mode partage". Sa propre population s'arrete (densite des pietons
//    a 0, generateurs de circulation et de voitures garees sautes) et ses passants / voitures locaux sont retires,
//    meme a l'ecran (deux mondes superposes se rentraient dedans) ; il voit a la place les copies de ceux de l'hote
//    (entities.cpp, vehicles.cpp), que l'hote ne lui envoie que dans ce mode.
//  - Au-dela de SHARE_LEAVE_M : il retrouve sa propre population (la marge evite les bascules en boucle).
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
// Recherche par la police : la police de l'hote ne poursuit que lui (son joueur) ; l'invite garde donc la
// generation de vehicules de son jeu, qui fait venir SA police. Le reste de ce qu'elle cree (circulation) est
// retire aussitot par CleanLocalPopulation, les forces de l'ordre sont gardees.
static bool Wanted() { void *me = FindPlayerPed(); void *w = me ? Field<void *>(me, 0x5F4) : NULL; return w && Field<int>(w, 0x20) > 0; }
static void __cdecl h_GenerateRandomCars() { if (!g_shared || Wanted()) o_GenerateRandomCars(); }

// Forces de l'ordre (policiers, SWAT, FBI, armee : type de personnage 6) et leurs vehicules.
static bool LawPed(void *ped) { return ped && PedType(ped) == 6; }
bool IsLawPed(void *ped) { return LawPed(ped); }
bool LocalWanted() { return Wanted(); }
static bool LawVehicle(void *v);
bool IsLawVehicle(void *v) { return LawVehicle(v); }
static bool LawVehicle(void *v)
{
    if (LawPed(VehDriver(v))) return true;
    for (int i = 0; i < 8; i++) if (LawPed(VehPassenger(v, i))) return true;
    static const char *const names[] = { "police", "enforcer", "fbiranch", "vicechee", "predator", "hunter", "rhino", "barracks", "polmav", "fbicar" };
    const char *m = ModelName(ModelIndex(v));
    for (const char *n : names) if (_stricmp(m, n) == 0) return true;
    return false;
}

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
    return ped != me && CharCreatedBy(ped) == 1 && !IsGhostPed(ped) && !IsPuppet(ped) && !InVehicle(ped) && !(LawPed(ped) && Wanted());
}

static bool LocalAmbientVehicle(void *v, void *me)
{
    uint8_t by = Field<uint8_t>(v, 0x1F8);
    if ((by != VEH_RANDOM && by != VEH_PARKED) || NetVehicleId(v) || PedVehicle(me) == v || (LawVehicle(v) && Wanted())) return false;
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
    if (now - last < 250) return;
    last = now;
    void *me = FindPlayerPed();
    int peds = 0, cars = 0;
    Pool *pp = PedPool();
    for (int i = 0; i < pp->size; i++) {
        if (pp->flags[i] & 0x80) continue;
        void *ped = pp->objects + i * PED_POOL_ENTRY;
        if (LocalAmbientPed(ped, me)) { RemovePed(ped); peds++; }
    }
    Pool *vp = VehiclePool();
    for (int i = 0; i < vp->size; i++) {
        if (vp->flags[i] & 0x80) continue;
        void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
        // Sauf celle ou il est peut-etre en train de monter.
        float dx = Pos(v).x - Pos(me).x, dy = Pos(v).y - Pos(me).y;
        if (LocalAmbientVehicle(v, me) && (dx * dx + dy * dy > 36.0f || !OnScreen(v))) { DeleteVehicleWithOccupants(v); cars++; }
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
    // Meme zone affichee que l'hote (CGame::currArea des deux cotes : l'hote envoie la sienne). Le code de zone du
    // personnage (m_nAreaCode) n'est pas tenu a jour par le jeu apres un chargement ou une porte : compare a la zone
    // de l'hote, il coupait la population partagee a 2 m de lui (plus de police ni de circulation de l'hote).
    (void)me;
    // Zone differente (l'hote passe une porte, cinematique d'interieur) : on ne bascule qu'apres 3 s de desaccord,
    // sinon la population etait detruite et recreee a chaque porte.
    static uint32_t areaMismatchSince;
    bool sameArea = h.state.area == (uint8_t)*(int *)0x978810;
    if (sameArea) areaMismatchSince = 0; else if (!areaMismatchSince) areaMismatchSince = GetTickCount();
    bool hostHere = h.connected && h.state.inGame && (sameArea || (g_shared && GetTickCount() - areaMismatchSince < 3000));
    float dx = h.state.pos[0] - Pos(me).x, dy = h.state.pos[1] - Pos(me).y;
    float d2 = dx * dx + dy * dy;
    // Mission secondaire en cours (taxi...) : il lui faut ses propres passants (clients, cibles) ; population locale.
    bool want = hostHere && !GuestSideMission() && d2 < (g_shared ? SHARE_LEAVE_M * SHARE_LEAVE_M : SHARE_ENTER_M * SHARE_ENTER_M);
    if (want != g_shared) {
        g_shared = want;
        if (want) { g_savedPedDensity = PedDensity(); Log("population : partagee avec l'hote"); }
        else { PedDensity() = g_savedPedDensity; Log("population : locale (loin de l'hote)"); }
    }
    static uint32_t lastStat;
    if (g_cfg.logScripts && GetTickCount() - lastStat > 10000) {
        lastStat = GetTickCount();
        int localPeds = 0, ghosts = 0, localCars = 0, copies = 0, law = 0;
        Pool *pp = PedPool();
        for (int i = 0; i < pp->size; i++) {
            if (pp->flags[i] & 0x80) continue;
            void *ped = pp->objects + i * PED_POOL_ENTRY;
            if (IsGhostPed(ped)) ghosts++;
            else if (CharCreatedBy(ped) == 1) { localPeds++; if (LawPed(ped)) law++; }
        }
        Pool *vp = VehiclePool();
        for (int i = 0; i < vp->size; i++) {
            if (vp->flags[i] & 0x80) continue;
            void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
            if (NetVehicleId(v)) copies++;
            else if (Field<uint8_t>(v, 0x1F8) == 1 || Field<uint8_t>(v, 0x1F8) == 3) localCars++;
        }
        Log("population (%s) : passants locaux %d (dont police %d), copies de l'hote %d ; voitures locales %d, reseau %d",
            g_shared ? "partagee" : "locale", localPeds, law, ghosts, localCars, copies);
    }
    if (!g_shared) return;
    if (PedDensity() != 0.0f) { g_savedPedDensity = PedDensity(); PedDensity() = 0.0f; }   // une mission a pu la changer
    CleanLocalPopulation();
}
