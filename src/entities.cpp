// Personnages de mission : l'hote fait foi. Il envoie l'etat de chaque personnage cree par un script
// (CharCreatedBy == mission) ; chaque invite en garde une copie qui suit cet etat.
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "vehicles.h"
#include "entities.h"
#include "mirror.h"
#include "seats.h"
#include "combat.h"
#include <string.h>

using namespace game;

enum { MAX_GHOSTS = 96, MAX_SENT = 96 };

// --- Hote : personnages de mission deja annonces (pour signaler leur disparition) ---
static uint32_t g_sent[MAX_SENT];
static int g_sentCount;

static void HostScan()
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
        if (ped == player || IsPuppet(ped)) continue;
        // Personnages de mission partout ; passants seulement pres d'un invite (population partagee).
        if (CharCreatedBy(ped) != PED_CHAR_MISSION &&
            !(CharCreatedBy(ped) == 1 && NearAnyGuest(&Pos(ped).x, AreaCode(ped), 100.0f))) continue;
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
        m.area = AreaCode(ped);
        m.weapon = WeaponTypeInSlot(ped, CurrentWeaponSlot(ped));
        if (InVehicle(ped) && PedVehicle(ped)) {
            m.vehicleId = NetVehicleId(PedVehicle(ped));
            int seat = SeatOf(PedVehicle(ped), ped);
            m.seat = (uint8_t)(seat < 0 ? 0 : seat);
        }
        NetSendToGuests(&m, sizeof(m));
        if (seenCount < MAX_SENT) seen[seenCount++] = m.handle;
    }
    // Ceux qui ont disparu depuis le dernier passage.
    for (int i = 0; i < g_sentCount; i++) {
        bool still = false;
        for (int j = 0; j < seenCount && !still; j++) still = seen[j] == g_sent[i];
        if (!still) {
            MsgPedRemove r = { MSG_PED_REMOVE, g_sent[i] };
            NetSendToGuests(&r, sizeof(r));
        }
    }
    memcpy(g_sent, seen, seenCount * sizeof(uint32_t));
    g_sentCount = seenCount;
}

// --- Invite : copies ---
struct Ghost {
    bool used;
    uint32_t handle;
    void *ped;
    MsgPed state;
    uint32_t lastRecv;
    int lastMoveState;
    bool dead;
};
static Ghost g_ghosts[MAX_GHOSTS];

static Ghost *FindGhost(uint32_t handle)
{
    for (auto &g : g_ghosts) if (g.used && g.handle == handle) return &g;
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
    Ghost *g = FindGhost(host);
    if (!g || !g->ped) return false;
    guest = PedHandle(g->ped);
    return true;
}

bool GhostHostHandle(void *ped, uint32_t &host)
{
    for (auto &g : g_ghosts) if (g.used && g.ped == ped) { host = g.handle; return true; }
    return false;
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
    CivilianPedCtor(ped, m.pedType, m.model);
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
    HoldWeapon(ped, m.weapon);

    void *want = m.vehicleId ? NetVehicleById(m.vehicleId) : NULL;
    void *cur = InVehicle(ped) ? PedVehicle(ped) : NULL;
    if (cur && (cur != want || SeatOf(cur, ped) != m.seat)) {
        Vec3 at = { m.pos[0], m.pos[1], m.pos[2] };
        WarpOutOfVehicle(ped, &at);
        cur = NULL;
    }
    if (want && !cur && WarpIntoSeat(ped, want, m.seat)) cur = want;
    uint8_t &flags = Field<uint8_t>(ped, 0x52);
    if (cur) { flags |= 0x04; return; }
    if (m.vehicleId) flags &= ~0x04;   // vehicule pas encore copie : cache
    else flags |= 0x04;

    float age = (GetTickCount() - g.lastRecv) / 1000.0f;
    if (age > 0.25f) age = 0.25f;
    Vec3 target = { m.pos[0] + m.speed[0] * age * 50.0f, m.pos[1] + m.speed[1] * age * 50.0f, m.pos[2] + m.speed[2] * age * 50.0f };
    Vec3 &p = Pos(ped);
    float dx = target.x - p.x, dy = target.y - p.y, dz = target.z - p.z;
    if (dx * dx + dy * dy + dz * dz > 25.0f) p = target;
    else { p.x += dx * 0.5f; p.y += dy * 0.5f; p.z += dz * 0.5f; }
    MoveSpeed(ped) = { m.speed[0], m.speed[1], m.speed[2] };
    SetHeadingMatrix(ped, m.heading);
    Heading(ped) = HeadingGoal(ped) = m.heading;
    SetMoveStateFn(ped, m.moveState);
    SetMoveAnim(ped);
}

static void OnPed(const MsgPed &m)
{
    if (GameState() != GS_PLAYING) return;
    Ghost *g = FindGhost(m.handle);
    if (!g) {
        for (auto &x : g_ghosts) if (!x.used) { memset(&x, 0, sizeof(x)); x.used = true; x.handle = m.handle; g = &x; break; }
        if (!g) return;
    }
    g->state = m;
    g->lastRecv = GetTickCount();
}

static void OnPedRemove(uint32_t handle)
{
    Ghost *g = FindGhost(handle);
    if (g) {
        Log("entites : personnage %08X disparu chez l'hote", handle);
        DestroyGhost(*g);
    }
}

void EntitiesInit()
{
    g_onPed = OnPed;
    g_onPedRemove = OnPedRemove;
}

void EntitiesFrame(bool inGame)
{
    if (!inGame) {
        for (auto &g : g_ghosts) if (g.used) { g.ped = NULL; g.used = false; }
        g_sentCount = 0;
        return;
    }
    if (g_cfg.host) { HostScan(); return; }
    uint32_t now = GetTickCount();
    for (auto &g : g_ghosts) {
        if (!g.used) continue;
        if (now - g.lastRecv > 5000) { DestroyGhost(g); continue; }   // l'hote ne l'envoie plus
        if (!g.ped) CreateGhost(g);
        if (g.ped) UpdateGhost(g);
    }
}
