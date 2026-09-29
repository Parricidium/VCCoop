// Population partagee (hybride selon la distance a l'hote).
//  - Invite a moins de SHARE_ENTER_M de l'hote : "mode partage". Sa propre population s'arrete (densite des pietons
//    a 0, generateurs de circulation et de voitures garees sautes) et ses passants / voitures locaux sont retires,
//    meme a l'ecran (deux mondes superposes se rentraient dedans) ; il voit a la place les copies de ceux de l'hote
//    (entities.cpp, vehicles.cpp), que l'hote ne lui envoie que dans ce mode.
//  - Au-dela de SHARE_LEAVE_M : il retrouve sa propre population (la marge evite les bascules en boucle).
#include "util.h"
#include <math.h>
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
// Hote : un invite recherche dans sa zone (2 etoiles et plus) n'avait que les policiers deja la, car le jeu ne fait
// naitre des voitures de police que d'apres les etoiles du joueur local (CCarCtrl::GenerateOneRandomCar : niveau > 1,
// NumLawEnforcerCars < m_MaximumLawEnforcerVehicles, m_CurrentCops < m_MaxCops). Le temps de la generation, l'hote
// "prend" le niveau de l'invite (CWanted : +0x19 m_MaxCops, +0x1A vehicules, +0x1C barrages, +0x20 niveau ; valeurs de
// CWanted::UpdateWantedLevel, sans barrages) ; ses 0x28 premiers octets sont remis juste apres. Les voitures de police
// ainsi nees sont envoyees sur l'invite (coop.cpp, HostPoliceChasesGuests).
static float Dist2(const Vec3 &a, float x, float y);
static int GuestWantedNearHost()
{
    void *me = FindPlayerPed();
    if (!me) return 0;
    const float hz = HOST_ZONE_M * g_cfg.zonePop / 100.0f;
    int best = 0;
    for (int i = 1; i < MAX_PLAYERS; i++) {
        const NetPlayer &g = g_players[i];
        if (!g.connected || !g.state.inGame || g.state.down || g.state.health <= 0.0f || g.state.area != (uint8_t)*(int *)0x978810) continue;
        if (Dist2(Pos(me), g.state.pos[0], g.state.pos[1]) < hz * hz && g.state.wanted > best) best = g.state.wanted;
    }
    return best;
}
static void __cdecl h_GenerateRandomCars()
{
    if (g_hostClose && !OwnPolice()) return;
    void *me = FindPlayerPed();
    void *w = me ? Field<void *>(me, 0x5F4) : NULL;
    int lvl = g_cfg.host && g_cfg.hostPolice && w ? GuestWantedNearHost() : 0;
    if (lvl < 2 || lvl <= Field<int>(w, 0x20)) { o_GenerateRandomCars(); return; }
    if (lvl > 6) lvl = 6;
    static const uint8_t maxVeh[7] = { 0, 1, 2, 2, 2, 3, 3 }, maxCops[7] = { 0, 1, 3, 4, 6, 8, 10 };
    uint8_t save[0x28];
    memcpy(save, w, sizeof(save));
    Field<int>(w, 0x20) = lvl;
    Field<uint8_t>(w, 0x19) = maxCops[lvl];
    Field<uint8_t>(w, 0x1A) = maxVeh[lvl];
    Field<int16_t>(w, 0x1C) = 0;
    o_GenerateRandomCars();
    memcpy(w, save, sizeof(save));
}

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
    const float hz = HOST_ZONE_M * g_cfg.zonePop / 100.0f;
    const float z2 = hz * hz;
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

// Portee des voitures garees : CCarGenerator::CheckIfWithinRangeOfAnyPlayers (0x5A6D00) ne fait naitre la voiture
// qu'entre 90 et 110 m du joueur (x CCamera::m_fGenerationDistMultiplier, 0x7E477C). Ce multiplicateur, on l'agrandit
// (ZonePopulation, DistanceAffichage : x1,83 par defaut) : la bande passait a 181-201 m, ou le sol du spot n'est pas
// encore charge (CWorld::FindGroundZFor3DCoord echoue, la voiture n'est pas creee), et plus pres il est trop tard.
// Aucune voiture garee n'apparaissait : PCJ-600 de PCJ Playground, motocross, etc. Le test de portee se fait donc
// avec le multiplicateur d'origine du jeu.
extern float g_genBoost;   // interp.cpp
typedef bool(__fastcall *CarGenRange_t)(void *gen, void *edx);
static CarGenRange_t o_CarGenRange;
static bool __fastcall h_CarGenRange(void *gen, void *edx)
{
    float &mult = *(float *)0x7E477C;
    float saved = mult;
    if (g_genBoost > 1.0f) mult = saved / g_genBoost;
    bool r = o_CarGenRange(gen, edx);
    mult = saved;
    return r;
}

// --- Reserves du jeu agrandies ---
// CPools::Initialise (0x4C02xx) cree la reserve des personnages (140) et des vehicules (110) : joueurs, copies,
// passants et voitures s'y partagent la place. Doublees (comme le font les "limit adjusters" en .asi), avant que le
// jeu ne les cree (notre dll est chargee avant WinMain). Les tableaux du mod indexes par case vont jusqu'a 512 / 256.
enum { PED_POOL = 280, VEH_POOL = 220 };
static void *g_poolCtor;
static __declspec(naked) void VehiclePoolSize()
{
    __asm {
        pop eax             // retour
        push VEH_POOL       // taille (au lieu de 110)
        push eax
        jmp g_poolCtor      // CPool::CPool(taille, nom), thiscall : ecx = la reserve
    }
}
static void EnlargePools()
{
    const uint8_t *p = (const uint8_t *)0x4C02C7;   // push 140 ; call CPool<CPed>::CPool
    const uint8_t *v = (const uint8_t *)0x4C02E9;   // push 110 ; call CPool<CVehicle>::CPool
    if (p[0] != 0x68 || *(const uint32_t *)(p + 1) != 140 || v[0] != 0x6A || v[1] != 110 || v[2] != 0xE8) {
        Log("population : reserves du jeu inattendues, taille d'origine gardee");
        return;
    }
    uint32_t peds = PED_POOL;
    Patch(0x4C02C8, &peds, 4);
    // Noeuds qui rangent chaque entite dans la grille du monde (EntryInfoNode, 3200) : doubles aussi, sinon ils
    // pourraient manquer avec deux fois plus de personnages et de vehicules.
    if (*(const uint8_t *)0x4C02A5 == 0x68 && *(const uint32_t *)0x4C02A6 == 0xC80) { uint32_t nodes = 0x1900; Patch(0x4C02A6, &nodes, 4); }
    g_poolCtor = (void *)(0x4C02EB + 5 + *(const int32_t *)(v + 3));
    PatchCall(0x4C02E9, (void *)VehiclePoolSize, 7);
    Log("population : reserves agrandies (%d personnages, %d vehicules)", PED_POOL, VEH_POOL);
}

void InstallPopulation()
{
    EnlargePools();
    static const uint8_t genPro[] = { 0x80, 0x3D, 0xB2, 0x0A, 0xA1, 0x00, 0x00 };
    o_GenerateRandomCars = (GenCars_t)MakeDetour(0x4292A0, genPro, sizeof(genPro), (void *)h_GenerateRandomCars);
    static const uint8_t carGenPro[] = { 0x53, 0x56, 0x57, 0x55, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00 };
    o_CarGenProcess = (CarGen_t)MakeDetour(0x5A71C0, carGenPro, sizeof(carGenPro), (void *)h_CarGenProcess);
    static const uint8_t rangePro[] = { 0x0F, 0xB6, 0x05, 0xFB, 0x0A, 0xA1, 0x00, 0x53, 0x56 };   // movzx eax,[PlayerInFocus]
    o_CarGenRange = (CarGenRange_t)MakeDetour(0x5A6D00, rangePro, sizeof(rangePro), (void *)h_CarGenRange);
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
    // Invite : copies des vehicules de l'hote, pour reperer les voitures garees nees deux fois (chacun son
    // generateur : la voiture devant l'hotel apparaissait deux fois, l'une au-dessus de l'autre).
    static Vec3 copies[128];
    int ncopies = 0;
    if (!g_cfg.host)
        for (int i = 0; i < vp->size && ncopies < 128; i++) {
            if (vp->flags[i] & 0x80) continue;
            void *w = vp->objects + i * VEHICLE_POOL_ENTRY;
            if (NetVehicleId(w)) copies[ncopies++] = Pos(w);
        }
    for (int i = 0; i < vp->size && i < 256; i++) {
        if (vp->flags[i] & 0x80) { g_vehSeen[i] = 0; continue; }
        void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
        bool born = Newborn(g_vehSeen, g_vehBirth, i, VehicleHandle(v), now);
        if (!LocalAmbientVehicle(v, me) || (g_cfg.host && LawVehicle(v))) continue;
        // Posee dans la copie d'une voiture de l'hote : c'est la meme, la notre part (meme a l'ecran : deux voitures
        // empilees se voient plus qu'une qui disparait). L'hote garde toujours la sienne.
        if (!VehDriver(v)) {
            bool stacked = false;
            for (int k = 0; k < ncopies && !stacked; k++) {
                float ddx = copies[k].x - Pos(v).x, ddy = copies[k].y - Pos(v).y, ddz = copies[k].z - Pos(v).z;
                stacked = ddx * ddx + ddy * ddy < 3.5f * 3.5f && fabsf(ddz) < 3.0f;
            }
            if (stacked) {
                if (g_cfg.logScripts) Log("population : vehicule garde en double avec une copie de l'hote (modele %d), retire", ModelIndex(v));
                DeleteVehicleWithOccupants(v);
                cars++;
                continue;
            }
        }
        if (!OtherPopulates(Pos(v).x, Pos(v).y)) continue;
        float dx = Pos(v).x - Pos(me).x, dy = Pos(v).y - Pos(me).y;
        if (born || (!OnScreen(v) && dx * dx + dy * dy > 40.0f * 40.0f)) { DeleteVehicleWithOccupants(v); cars++; }
    }
    if ((peds || cars) && g_cfg.logScripts) Log("population : %d passants et %d vehicules en double retires", peds, cars);
}

// --- Population gardee tant qu'on est dans la zone, zone agrandie ---
// Le jeu retire un passant hors de l'ecran des 25 m (a l'ecran : ~51 m) et une voiture hors de l'ecran des 40 m
// (a l'ecran : 120 m) : en tournant la camera, la rue se vidait derriere soi. Sauf pendant ses minuteries de
// "portee etendue" (CPed +0x35C, lue par CPopulation::ManagePopulation 0x53D690 ; CVehicle +0x214,
// m_nSetPieceExtendedRangeTime, lue par CCarCtrl::PossiblyRemoveVehicle 0x426030), qu'on prolonge : ils ne partent
// plus qu'a la distance normale, qu'on les regarde ou non. Nombre maximal (MaxNumberOfPedsInUse 0x694DC8 = 25,
// MaxNumberOfCarsInUse 0x686FCC = 12) a l'echelle de ZonePopulation.
static void KeepPopulation()
{
    static uint32_t last;
    uint32_t now = GetTickCount();
    if (now - last < 250) return;
    last = now;
    *(int *)0x694DC8 = 25 * g_cfg.zonePop / 100 * g_cfg.popDensity / 100;
    *(int *)0x686FCC = 12 * g_cfg.zonePop / 100 * g_cfg.popDensity / 100;
    // DensitePopulation : multiplie la densite voulue par le jeu (et par les missions, SET_PED/CAR_DENSITY_MULTIPLIER :
    // une valeur changee par quelqu'un d'autre que nous devient la nouvelle base, rues videes comprises).
    static float lastPed = -1.0f, basePed = 1.0f, lastCar = -1.0f, baseCar = 1.0f;
    float k = g_cfg.popDensity / 100.0f;
    if (!g_hostClose) {   // invite colle a l'hote : densite a 0 (c'est l'hote qui peuple), on n'y touche pas
        if (PedDensity() != lastPed) basePed = PedDensity();
        lastPed = PedDensity() = basePed * k;
    }
    float &carDensity = *(float *)0x686FC8;   // CCarCtrl::CarDensityMultiplier
    if (carDensity != lastCar) baseCar = carDensity;
    lastCar = carDensity = baseCar * k;
    uint32_t until = *(uint32_t *)0x974B2C + 3000;   // horloge du jeu (CTimer::m_snTimeInMilliseconds)
    void *me = FindPlayerPed();
    int nPeds = 0, nCars = 0;
    Pool *pp = PedPool();
    for (int i = 0; i < pp->size; i++) {
        if (pp->flags[i] & 0x80) continue;
        void *ped = pp->objects + i * PED_POOL_ENTRY;
        if (ped == me || CharCreatedBy(ped) != 1 || IsPuppet(ped) || IsGhostPed(ped)) continue;
        Field<uint32_t>(ped, 0x35C) = until;
        nPeds++;
    }
    Pool *vp = VehiclePool();
    for (int i = 0; i < vp->size; i++) {
        if (vp->flags[i] & 0x80) continue;
        void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
        if (Field<uint8_t>(v, 0x1F8) != 1 || NetVehicleIsCopy(v)) continue;   // circulation (RANDOM_VEHICLE) d'ici
        Field<uint32_t>(v, 0x214) = until;
        nCars++;
    }
    static uint32_t lastLog;
    if (now - lastLog > 10000) {
        lastLog = now;
        Log("population : %d passants et %d voitures d'ici (plafonds %d / %d, reserves %d / %d, zone %d%%, densite %d%%)",
            nPeds, nCars, *(int *)0x694DC8, *(int *)0x686FCC, pp->size, vp->size, g_cfg.zonePop, g_cfg.popDensity);
    }
}

void PopulationFrame(bool inGame)
{
    if (inGame) KeepPopulation();
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
