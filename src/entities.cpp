// Personnages partages. L'hote envoie ses personnages de mission (partout) et ses passants (pres des invites) ; un
// invite recherche envoie sa propre police (elle ne poursuit que lui, cf. population.cpp). Chaque autre joueur en
// garde une copie ("ghost") qui suit l'etat recu ; une copie est reperee par (proprietaire, reference de pool).
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "vehicles.h"
#include "entities.h"
#include "mirror.h"
#include "seats.h"
#include "combat.h"
#include "interp.h"
#include "anims.h"
#include "population.h"
#include <string.h>

using namespace game;

enum { MAX_GHOSTS = 160, MAX_SENT = 160 };

// --- Nos personnages deja annonces (pour signaler leur disparition) ---
static uint32_t g_sent[MAX_SENT];
static int g_sentCount;

bool IsGhostPed(void *ped);

static void ScanOwnPeds()
{
    static uint32_t last;
    uint32_t now = GetTickCount();
    if (now - last < 66) return;   // 15 fois par seconde
    last = now;

    uint32_t seen[MAX_SENT];
    int seenCount = 0;
    Pool *pool = PedPool();
    void *player = FindPlayerPed();
    for (int i = 0; i < pool->size; i++) {
        if (pool->flags[i] & 0x80) continue;
        void *ped = pool->objects + i * PED_POOL_ENTRY;
        if (ped == player || IsPuppet(ped) || IsGhostPed(ped)) continue;
        if (g_cfg.host) {
            // Personnages de mission partout ; passants seulement pres d'un invite (population partagee).
            if (CharCreatedBy(ped) != PED_CHAR_MISSION &&
                !(CharCreatedBy(ped) == 1 && NearAnyGuest(&Pos(ped).x, AreaCode(ped), (float)AMBIENT_SHARE_M))) continue;
        } else {
            // Invite : seulement sa police, tant qu'il est recherche.
            if (CharCreatedBy(ped) != 1 || !IsLawPed(ped) || !LocalWanted()) continue;
        }
        MsgPed m = {};
        m.type = MSG_PED;
        m.handle = PedHandle(ped);
        m.model = (uint16_t)ModelIndex(ped);
        lstrcpynA(m.modelName, ModelName(m.model), sizeof(m.modelName));
        Vec3 p = Pos(ped), v = MoveSpeed(ped);
        memcpy(m.pos, &p, 12);
        memcpy(m.speed, &v, 12);
        m.heading = Heading(ped);
        m.health = Health(ped);
        m.moveState = (uint8_t)MoveState(ped);
        m.pedState = (uint8_t)PedState(ped);
        m.pedType = (uint8_t)PedType(ped);
        m.time = now;
        m.owner = (uint8_t)g_localId;
        m.ambient = CharCreatedBy(ped) == 1;
        m.shots = PedShotCount(ped);
        if (InVehicle(ped)) m.anims[0].id = m.anims[1].id = -1;
        else CollectAnimSlots(ped, m.anims, 2);
        m.area = AreaCode(ped);
        m.weapon = WeaponTypeInSlot(ped, CurrentWeaponSlot(ped));
        if (InVehicle(ped) && PedVehicle(ped)) {
            m.vehicleId = NetVehicleId(PedVehicle(ped));
            int seat = SeatOf(PedVehicle(ped), ped);
            m.seat = (uint8_t)(seat < 0 ? 0 : seat);
        }
        NetSendToAll(&m, sizeof(m));   // hote : a tous les invites ; invite : a l'hote, qui relaie
        if (seenCount < MAX_SENT) seen[seenCount++] = m.handle;
    }
    // Ceux qui ont disparu depuis le dernier passage.
    for (int i = 0; i < g_sentCount; i++) {
        bool still = false;
        for (int j = 0; j < seenCount && !still; j++) still = seen[j] == g_sent[i];
        if (!still) {
            MsgPedRemove r = { MSG_PED_REMOVE, g_sent[i], (uint8_t)g_localId };
            NetSendToAll(&r, sizeof(r));
        }
    }
    memcpy(g_sent, seen, seenCount * sizeof(uint32_t));
    g_sentCount = seenCount;
}

// --- Copies des personnages des autres ---
struct Ghost {
    bool used;
    uint8_t owner;
    uint32_t handle;
    void *ped;
    MsgPed state;
    uint32_t lastRecv;
    int lastMoveState;
    bool dead;
    uint8_t lastShots;
    Track track;
    AnimMirror anims;
};
static Ghost g_ghosts[MAX_GHOSTS];

static Ghost *FindGhost(uint8_t owner, uint32_t handle)
{
    for (auto &g : g_ghosts) if (g.used && g.owner == owner && g.handle == handle) return &g;
    return NULL;
}

uint32_t g_hostPlayerHandle = 0xFFFFFFFF;

bool GuestPedForHost(uint32_t host, uint32_t &guest)
{
    if (host == g_hostPlayerHandle) {   // le Tommy de l'hote : chez nous, c'est son pantin
        void *p = PuppetPed(0);
        if (!p) return false;
        guest = PedHandle(p);
        return true;
    }
    Ghost *g = FindGhost(0, host);
    if (!g || !g->ped) return false;
    guest = PedHandle(g->ped);
    return true;
}

bool GhostOwner(void *ped, uint8_t &owner, uint32_t &handle)
{
    for (auto &g : g_ghosts) if (g.used && g.ped == ped) { owner = g.owner; handle = g.handle; return true; }
    return false;
}

bool GhostHostHandle(void *ped, uint32_t &host)
{
    uint8_t owner;
    return GhostOwner(ped, owner, host) && owner == 0;
}

bool IsGhostPed(void *ped)
{
    for (auto &g : g_ghosts) if (g.used && g.ped == ped) return true;
    return false;
}

static void DestroyGhost(Ghost &g)
{
    if (g.ped) {
        void *ped = g.ped;
        CleanUpOldReference(ped, &g.ped);
        if (InVehicle(ped)) WarpOutOfVehicle(ped, NULL);
        WorldRemove(ped);
        RemoveReferencesToDeletedObject(ped);
        DeleteEntity(ped);
        g.ped = NULL;
    }
    g.used = false;
}

// Charge le modele du personnage ; vrai quand il est pret. Les personnages speciaux (Lance...) occupent
// un emplacement 109+ dont le contenu depend du script : on y charge le meme nom que chez l'hote.
static bool EnsureModel(const MsgPed &m)
{
    bool special = m.model >= MI_SPECIAL01 && _stricmp(ModelName(m.model), m.modelName) != 0;
    if (special) {
        RequestSpecialModel(m.model, m.modelName, 1 | 8);
        return false;
    }
    if (!HasModelLoaded(m.model)) { RequestModel(m.model, 1 | 8); return false; }
    return true;
}

static void CreateGhost(Ghost &g)
{
    const MsgPed &m = g.state;
    if (!EnsureModel(m)) return;
    void *ped = PedAlloc();
    if (!ped) return;
    // Une copie de policier reste un simple passant (type civil) habille en policier : de type "police", le jeu la
    // prendrait pour un de ses vrais policiers (poursuites, compte des agents...).
    CivilianPedCtor(ped, m.pedType == 6 ? PEDTYPE_CIVMALE : m.pedType, m.model);
    CharCreatedBy(ped) = PED_CHAR_MISSION;
    Field<uint8_t>(ped, 0x14E) &= ~0x02;   // ne reagit pas aux menaces
    Field<uint8_t>(ped, 0x53) |= 0x1E;     // invulnerable (les degats passeront par l'hote)
    Pos(ped) = { m.pos[0], m.pos[1], m.pos[2] };
    SetHeadingMatrix(ped, m.heading);
    Heading(ped) = HeadingGoal(ped) = m.heading;
    AreaCode(ped) = m.area;
    WorldAdd(ped);
    g.ped = ped;
    g.lastMoveState = -1;
    g.anims = {};
    RegisterReference(ped, &g.ped);
    Log("entites : copie du personnage %08X (%s) creee", m.handle, m.modelName);
}

static void UpdateGhost(Ghost &g)
{
    const MsgPed &m = g.state;
    void *ped = g.ped;
    AreaCode(ped) = m.area;
    // Mort chez l'hote : la copie meurt ici aussi (avec son animation), puis on la laisse tranquille.
    if (m.health <= 0.0f) {
        if (!g.dead) {
            g.dead = true;
            if (InVehicle(ped)) WarpOutOfVehicle(ped, NULL);
            Field<uint8_t>(ped, 0x53) &= ~0x1E;
            ApplyDamage(ped, NULL, m.weapon > 0 ? m.weapon : 0, 1000.0f, 0, 0);
            Log("entites : %08X meurt (comme chez l'hote)", g.handle);
        }
        return;
    }
    Health(ped) = m.health;
    bool armed = HoldWeapon(ped, m.weapon);
    // Ses tirs (police d'un invite recherche, personnages de mission) : rejoues ici, avec de vraies balles.
    if (armed && !InVehicle(ped)) for (int n = 0; g.lastShots != m.shots && n < 3; n++) { g.lastShots++; PuppetShoot(ped, m.weapon); }
    g.lastShots = m.shots;

    void *want = m.vehicleId ? NetVehicleById(m.vehicleId) : NULL;
    void *cur = InVehicle(ped) ? PedVehicle(ped) : NULL;
    if (cur && (cur != want || (SeatOf(cur, ped) == 0) != (m.seat == 0))) {
        Vec3 at = { m.pos[0], m.pos[1], m.pos[2] };
        WarpOutOfVehicle(ped, &at);
        cur = NULL;
    }
    if (want && !cur && WarpIntoSeat(ped, want, m.seat)) cur = want;
    uint8_t &flags = Field<uint8_t>(ped, 0x52);
    if (cur) { flags |= 0x04; return; }
    if (m.vehicleId) flags &= ~0x04;   // vehicule pas encore copie : cache
    else flags |= 0x04;

    // Position et cap : apres la physique, par interpolation (GhostsAfterProcess).
    PedState(ped) = 0;   // etat "aucun" : l'IA ne remet pas le deplacement a "immobile" (cf. coop.cpp)
    // Se battre, tomber, se relever... comme chez l'hote ; nos propres coups n'y font rien (c'est l'hote qui decide).
    ClearLocalReactions(ped, g.anims);
    if (ApplyActionAnims(ped, m.anims, 2, g.anims)) return;
    SetMoveStateFn(ped, m.moveState);
    SetMoveAnim(ped);
}

static void OnPed(const MsgPed &m)
{
    if (GameState() != GS_PLAYING || m.owner == g_localId || m.owner >= MAX_PLAYERS) return;
    Ghost *g = FindGhost(m.owner, m.handle);
    // Invite avec son propre monde (loin de l'hote) : les passants partages envoyes pour un autre invite ne le
    // concernent pas (ils se superposaient aux siens).
    if (m.ambient && !g_cfg.host && !PopulationShared()) {
        if (g) DestroyGhost(*g);
        return;
    }
    if (!g) {
        for (auto &x : g_ghosts)
            if (!x.used) { memset(&x, 0, sizeof(x)); x.used = true; x.owner = m.owner; x.handle = m.handle; g = &x; break; }
        if (!g) return;
    }
    g->state = m;
    g->lastRecv = GetTickCount();
    ClockSample(m.owner, m.time);
    Snap n = {};
    n.t = m.time;
    for (int k = 0; k < 3; k++) { n.pos[k] = m.pos[k]; n.vel[k] = m.speed[k]; }
    n.heading = m.heading;
    g->track.Push(n);
}

// Apres la physique : les copies a pied (et vivantes) sont placees a leur position interpolee.
void GhostsAfterProcess()
{
    for (auto &g : g_ghosts) {
        if (!g.used || !g.ped || g.dead || InVehicle(g.ped) || g.state.vehicleId) continue;
        Snap n;
        if (!TrackSample(g.track, g.owner, n, true)) continue;
        float jx = n.pos[0] - Pos(g.ped).x, jy = n.pos[1] - Pos(g.ped).y, jz = n.pos[2] - Pos(g.ped).z;
        if (jx * jx + jy * jy + jz * jz > 400.0f) Teleport(g.ped, { n.pos[0], n.pos[1], n.pos[2] });
        Pos(g.ped) = { n.pos[0], n.pos[1], n.pos[2] };
        MoveSpeed(g.ped) = { n.vel[0], n.vel[1], n.vel[2] };
        SetHeadingMatrix(g.ped, n.heading);
        Heading(g.ped) = HeadingGoal(g.ped) = n.heading;
    }
}

static void OnPedRemove(uint8_t owner, uint32_t handle)
{
    Ghost *g = FindGhost(owner, handle);
    if (g) {
        Log("entites : personnage %08X disparu chez le joueur %d", handle, owner);
        DestroyGhost(*g);
    }
}

// --- Invite : doublons des scripts du jeu ---
// Les fils du script principal (bus, vendeurs, Ammu-Nation, club...) tournent aussi chez l'invite et y creent leurs
// propres personnages et vehicules, en plus des copies de ceux de l'hote : tout apparaissait en double. Les siens
// sont rendus invisibles et intangibles (pas supprimes : son script s'en sert encore) tant qu'une copie de l'hote
// les remplace : pour un personnage, une copie du meme modele a moins de 4 m ; pour un vehicule, en population
// partagee (il voit alors ceux de l'hote). Sans copie a cote (l'hote est loin), ils restent visibles.
enum { MAX_HIDDEN = 96 };
static void *g_hidden[MAX_HIDDEN];

static void SetHidden(void *e, bool hide)
{
    uint8_t &vis = Field<uint8_t>(e, 0x52), &col = Field<uint8_t>(e, 0x51);
    if (hide) { vis &= ~0x04; col &= ~0x01; } else { vis |= 0x04; col |= 0x01; }
}

static bool HideSlot(void *e, bool hide)
{
    for (auto &h : g_hidden)
        if (h == e) {
            if (!hide) { SetHidden(e, false); CleanUpOldReference(e, &h); h = NULL; }
            return true;
        }
    if (!hide) return false;
    for (auto &h : g_hidden)
        if (!h) { h = e; RegisterReference(e, &h); SetHidden(e, true); return true; }
    return false;
}

static bool GhostOfModelNear(int model, const Vec3 &p, float r)
{
    for (auto &g : g_ghosts) {
        if (!g.used || !g.ped || ModelIndex(g.ped) != model) continue;
        float dx = Pos(g.ped).x - p.x, dy = Pos(g.ped).y - p.y, dz = Pos(g.ped).z - p.z;
        if (dx * dx + dy * dy + dz * dz < r * r) return true;
    }
    return false;
}

static void DedupeScriptEntities()
{
    static uint32_t last;
    if (GetTickCount() - last < 500) return;
    last = GetTickCount();
    void *me = FindPlayerPed();
    void *myVeh = InVehicle(me) ? PedVehicle(me) : NULL;
    bool shared = PopulationShared();
    int hiddenPeds = 0, hiddenCars = 0;
    Pool *vp = VehiclePool();
    for (int i = 0; i < vp->size; i++) {
        if (vp->flags[i] & 0x80) continue;
        void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
        if (Field<uint8_t>(v, 0x1F8) != VEHICLE_MISSION || NetVehicleId(v) || v == myVeh) continue;
        bool hide = shared;
        bool wasHidden = HideSlot(v, hide);
        if (!hide && wasHidden) {
            if (VehDriver(v)) SetHidden(VehDriver(v), false);
            for (int k = 0; k < 8; k++) if (VehPassenger(v, k)) SetHidden(VehPassenger(v, k), false);
        }
        if (hide) {
            hiddenCars++;
            if (VehDriver(v)) SetHidden(VehDriver(v), true);
            for (int k = 0; k < 8; k++) if (VehPassenger(v, k)) SetHidden(VehPassenger(v, k), true);
        }
    }
    Pool *pp = PedPool();
    for (int i = 0; i < pp->size; i++) {
        if (pp->flags[i] & 0x80) continue;
        void *ped = pp->objects + i * PED_POOL_ENTRY;
        if (ped == me || CharCreatedBy(ped) != PED_CHAR_MISSION || IsGhostPed(ped) || IsPuppet(ped)) continue;
        if (InVehicle(ped)) continue;   // traite avec son vehicule
        bool hide = GhostOfModelNear(ModelIndex(ped), Pos(ped), 4.0f);
        HideSlot(ped, hide);
        if (hide) hiddenPeds++;
    }
    static int lastPeds = -1, lastCars = -1;
    if ((hiddenPeds != lastPeds || hiddenCars != lastCars) && g_cfg.logScripts)
        Log("entites : doublons de nos scripts masques : %d personnages, %d vehicules", hiddenPeds, hiddenCars);
    lastPeds = hiddenPeds; lastCars = hiddenCars;
}

void EntitiesInit()
{
    g_onPed = OnPed;
    g_onPedRemove = OnPedRemove;
}

void EntitiesFrame(bool inGame)
{
    if (!inGame) {
        for (auto &h : g_hidden) h = NULL;
        for (auto &g : g_ghosts) if (g.used) { g.ped = NULL; g.used = false; }
        g_sentCount = 0;
        return;
    }
    ScanOwnPeds();   // hote : ses personnages ; invite : sa police s'il est recherche
    if (!g_cfg.host && !GuestSideMission()) DedupeScriptEntities();   // sa mission cree ses propres personnages et vehicules
    uint32_t now = GetTickCount();
    for (auto &g : g_ghosts) {
        if (!g.used) continue;
        if (now - g.lastRecv > 5000 || !g_players[g.owner].connected) { DestroyGhost(g); continue; }   // plus envoye
        if (!g.ped) CreateGhost(g);
        if (g.ped) UpdateGhost(g);
    }
}
