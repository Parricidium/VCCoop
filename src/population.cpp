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
// PoliceHote=1 : en population partagee, l'invite n'a pas de police a lui (c'est celle de l'hote qui le poursuit,
// coop.cpp HostPoliceChasesGuests) ; ses etoiles restent affichees mais ne font rien venir.
static bool OwnPolice() { return Wanted() && !(g_cfg.hostPolice && g_shared); }
static bool g_hostClose;   // invite colle a l'hote (< 40 m) : tout ce qu'il ferait naitre serait dans la zone de l'hote
static void __cdecl h_GenerateRandomCars() { if (!g_hostClose || OwnPolice()) o_GenerateRandomCars(); }

// A qui revient de peupler ce point ? Hote : a moins de HOST_ZONE_M de lui. Invite : au-dela, s'il est le plus proche.
static bool HostPos(Vec3 &out)
{
    const NetPlayer &h = g_players[0];
    if (!h.connected || !h.state.inGame || h.state.area != (uint8_t)*(int *)0x978810 || GetTickCount() - h.lastStateAt > 3000) return false;
    out = { h.state.pos[0], h.state.pos[1], h.state.pos[2] };
    return true;
}
static float Dist2(const Vec3 &a, float x, float y) { return (a.x - x) * (a.x - x) + (a.y - y) * (a.y - y); }
// Vrai si un AUTRE joueur doit peupler ce point (nous n'y faisons rien naitre).
static bool OtherPopulates(float x, float y)
{
    const float z2 = (float)HOST_ZONE_M * HOST_ZONE_M;
    if (!g_cfg.host) {
        Vec3 h;
        return HostPos(h) && Dist2(h, x, y) < z2;
    }
    void *me = FindPlayerPed();
    if (!me) return false;
    float dh = Dist2(Pos(me), x, y);
    if (dh < z2) return false;
    for (int i = 1; i < MAX_PLAYERS; i++) {
        const NetPlayer &g = g_players[i];
        if (!g.connected || !g.state.inGame || g.state.area != (uint8_t)*(int *)0x978810 || GetTickCount() - g.lastStateAt > 3000) continue;
        float dx = g.state.pos[0] - x, dy = g.state.pos[1] - y;
        if (dx * dx + dy * dy < dh) return true;
    }
    return false;
}

// Forces de l'ordre (policiers, SWAT, FBI, armee : type de personnage 6) et leurs vehicules.
static bool LawPed(void *ped) { return ped && PedType(ped) == 6; }
bool IsLawPed(void *ped) { return LawPed(ped); }
bool LocalWanted() { return OwnPolice(); }   // a une police a lui (qu'il partage avec les autres)
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

// Voitures garees a emplacement fixe (PCJ-600 de l'hotel...) : le generateur ne fait naitre sa voiture que quand
// SON joueur passe a 90-110 m du spot (reVC CarGen.cpp). Chacun garde donc ses generateurs, sauf pour les spots que
// l'autre peuple (avant : en population partagee, ceux de l'invite etaient coupes et ceux de l'hote ne couvraient que
// l'hote : un invite a 100 m de l'hote, pres du spot, n'avait la PCJ chez personne).
typedef void(__fastcall *CarGen_t)(void *gen, void *edx);
static CarGen_t o_CarGenProcess;
static void __fastcall h_CarGenProcess(void *gen, void *edx)
{
    const float *p = (const float *)((uint8_t *)gen + 4);   // CCarGenerator::m_vecPos
    if (!OtherPopulates(p[0], p[1])) o_CarGenProcess(gen, edx);
}

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
    return ped != me && CharCreatedBy(ped) == 1 && !IsGhostPed(ped) && !IsPuppet(ped) && !InVehicle(ped) && !(LawPed(ped) && OwnPolice());
}

static bool LocalAmbientVehicle(void *v, void *me)
{
    uint8_t by = Field<uint8_t>(v, 0x1F8);
    if ((by != VEH_RANDOM && by != VEH_PARKED) || NetVehicleId(v) || PedVehicle(me) == v || (LawVehicle(v) && OwnPolice())) return false;
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

// Fusion des populations : nos passants / voitures ambiants nes dans la zone qu'un autre joueur peuple sont retires
// aussitot (ils naissent hors de l'ecran : invisible) ; ceux qui y entrent plus tard ne sont retires que hors de
// l'ecran et loin de nous. Plus rien ne disparait sous les yeux (avant : tout le local etait retire d'un coup en
// entrant en mode partage, et tout revenait d'un coup en en sortant).
static uint16_t g_pedSeen[512], g_vehSeen[256];     // reference vue dans chaque case (0 : rien)
static uint32_t g_pedBirth[512], g_vehBirth[256];   // quand elle est apparue

static bool Newborn(uint16_t *seen, uint32_t *birth, int slot, uint32_t handle, uint32_t now)
{
    uint16_t h = (uint16_t)(handle & 0xFFFF);
    if (seen[slot] != h) { seen[slot] = h; birth[slot] = now; }
    return now - birth[slot] < 1500;
}

static void MergePopulation()
{
    static uint32_t last;
    uint32_t now = GetTickCount();
    if (now - last < 250) return;
    last = now;
    void *me = FindPlayerPed();
    int peds = 0, cars = 0;
    Pool *pp = PedPool();
    for (int i = 0; i < pp->size && i < 512; i++) {
        if (pp->flags[i] & 0x80) { g_pedSeen[i] = 0; continue; }
        void *ped = pp->objects + i * PED_POOL_ENTRY;
        bool born = Newborn(g_pedSeen, g_pedBirth, i, PedHandle(ped), now);
        if (!LocalAmbientPed(ped, me) || (g_cfg.host && LawPed(ped))) continue;
        if (!OtherPopulates(Pos(ped).x, Pos(ped).y)) continue;
        float dx = Pos(ped).x - Pos(me).x, dy = Pos(ped).y - Pos(me).y;
        if (born || (!OnScreen(ped) && dx * dx + dy * dy > 40.0f * 40.0f)) { RemovePed(ped); peds++; }
    }
    Pool *vp = VehiclePool();
    for (int i = 0; i < vp->size && i < 256; i++) {
        if (vp->flags[i] & 0x80) { g_vehSeen[i] = 0; continue; }
        void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
        bool born = Newborn(g_vehSeen, g_vehBirth, i, VehicleHandle(v), now);
        if (!LocalAmbientVehicle(v, me) || (g_cfg.host && LawVehicle(v))) continue;
        if (!OtherPopulates(Pos(v).x, Pos(v).y)) continue;
        float dx = Pos(v).x - Pos(me).x, dy = Pos(v).y - Pos(me).y;
        if (born || (!OnScreen(v) && dx * dx + dy * dy > 40.0f * 40.0f)) { DeleteVehicleWithOccupants(v); cars++; }
    }
    if ((peds || cars) && g_cfg.logScripts) Log("population : %d passants et %d vehicules en double retires", peds, cars);
}

void PopulationFrame(bool inGame)
{
    if (inGame && g_cfg.host) {
        bool any = false;
        for (int i = 1; i < MAX_PLAYERS; i++) any |= g_players[i].connected;
        if (any) MergePopulation();
        return;
    }
    if (!inGame || g_cfg.host || g_localId <= 0) {
        if (g_shared) g_shared = false;
        if (g_hostClose) { g_hostClose = false; PedDensity() = g_savedPedDensity; }
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
        Log(want ? "population : pres de l'hote" : "population : loin de l'hote");
    }
    // Colle a l'hote : il ne fait plus naitre de passants (tous seraient dans la zone de l'hote, nes puis retires).
    bool close = hostHere && !GuestSideMission() && d2 < 40.0f * 40.0f;
    if (close != g_hostClose) {
        g_hostClose = close;
        if (close) g_savedPedDensity = PedDensity(); else PedDensity() = g_savedPedDensity;
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
    if (g_hostClose && PedDensity() != 0.0f) { g_savedPedDensity = PedDensity(); PedDensity() = 0.0f; }   // une mission a pu la changer
    if (!GuestSideMission()) MergePopulation();
}
