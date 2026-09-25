// Vehicules reseau. Tout vehicule conduit par un joueur recoit un identifiant ; son proprietaire (le dernier
// joueur au volant) envoie son etat, les autres instances en ont une copie qui suit cet etat. Si un autre
// joueur prend le volant d'une copie, il en devient proprietaire et c'est l'ancien qui suit.
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "vehicles.h"
#include "mirror.h"
#include <math.h>
#include <string.h>

using namespace game;

enum { MAX_NET_VEHICLES = 64 };

struct NetVehicle {
    bool used;
    uint32_t id;
    uint8_t owner;
    void *veh;          // vehicule local (remis a NULL par le jeu s'il le detruit)
    bool ours;          // cree par nous (copie d'un vehicule distant)
    uint32_t lastRecv, lastSend;
    bool haveState;
    MsgVehicle state;   // dernier etat recu (vehicules distants)
};
static NetVehicle g_vehs[MAX_NET_VEHICLES];
static uint32_t g_vehCounter;

static NetVehicle *FindById(uint32_t id)
{
    for (auto &e : g_vehs) if (e.used && e.id == id) return &e;
    return NULL;
}

static NetVehicle *FindByPtr(void *veh)
{
    for (auto &e : g_vehs) if (e.used && e.veh == veh) return &e;
    return NULL;
}

static NetVehicle *Alloc(uint32_t id)
{
    for (auto &e : g_vehs)
        if (!e.used) { memset(&e, 0, sizeof(e)); e.used = true; e.id = id; return &e; }
    return NULL;
}

static void Bind(NetVehicle &e, void *veh)
{
    e.veh = veh;
    if (veh) RegisterReference(veh, &e.veh);
}

static void Unbind(NetVehicle &e)
{
    if (e.veh) CleanUpOldReference(e.veh, &e.veh);
    e.veh = NULL;
}

bool GuestVehicleForHost(uint32_t host, uint32_t &guest)
{
    for (auto &e : g_vehs)
        if (e.used && e.owner == 0 && e.haveState && e.state.poolHandle == host && e.veh) {
            guest = VehicleHandle(e.veh);
            return true;
        }
    return false;
}

void *NetVehicleById(uint32_t id)
{
    NetVehicle *e = FindById(id);
    return e ? e->veh : NULL;
}

uint32_t NetVehicleId(void *veh)
{
    NetVehicle *e = FindByPtr(veh);
    return e ? e->id : 0;
}

// --- Envoi (vehicules dont on est proprietaire) ---
static void SendVehicle(NetVehicle &e)
{
    void *v = e.veh;
    MsgVehicle m = {};
    m.type = MSG_VEHICLE;
    m.owner = e.owner;
    m.id = e.id;
    m.model = (uint16_t)ModelIndex(v);
    m.vclass = (uint8_t)VehClass(v);
    m.color1 = Field<uint8_t>(v, 0x1A0);
    m.color2 = Field<uint8_t>(v, 0x1A1);
    m.driver = 0xFF;
    void *drv = VehDriver(v);
    if (drv && drv == FindPlayerPed()) m.driver = (uint8_t)g_localId;
    Vec3 p = Pos(v), r = Field<Vec3>(v, 0x04), f = Field<Vec3>(v, 0x14), s = MoveSpeed(v), t = TurnSpeed(v);
    memcpy(m.pos, &p, 12); memcpy(m.right, &r, 12); memcpy(m.fwd, &f, 12);
    memcpy(m.speed, &s, 12); memcpy(m.turn, &t, 12);
    m.health = VehHealth(v);
    m.steer = Field<float>(v, 0x1EC);
    m.gas = Field<float>(v, 0x1F0);
    m.brake = Field<float>(v, 0x1F4);
    m.poolHandle = VehicleHandle(v);
    NetSendToAll(&m, sizeof(m));
    e.lastSend = GetTickCount();
}

// --- Copies des vehicules distants ---
static void *CreateCopy(const MsgVehicle &m)
{
    if (!HasModelLoaded(m.model)) {
        RequestModel(m.model, 1 | 8);   // garde en memoire, prioritaire ; on reessaiera a l'image suivante
        return NULL;
    }
    void *v = VehicleAlloc();
    if (!v) { Log("vehicules : plus de place pour une copie"); return NULL; }
    switch (m.vclass) {
    case VCLASS_BOAT: BoatCtor(v, m.model, VEHICLE_MISSION); break;
    case VCLASS_BIKE: BikeCtor(v, m.model, VEHICLE_MISSION); break;
    default: AutomobileCtor(v, m.model, VEHICLE_MISSION); break;
    }
    Field<uint8_t>(v, 0x1A0) = m.color1;
    Field<uint8_t>(v, 0x1A1) = m.color2;
    Field<Vec3>(v, 0x04) = { m.right[0], m.right[1], m.right[2] };
    Field<Vec3>(v, 0x14) = { m.fwd[0], m.fwd[1], m.fwd[2] };
    Field<Vec3>(v, 0x24) = { m.right[1] * m.fwd[2] - m.right[2] * m.fwd[1], m.right[2] * m.fwd[0] - m.right[0] * m.fwd[2],
                             m.right[0] * m.fwd[1] - m.right[1] * m.fwd[0] };
    Pos(v) = { m.pos[0], m.pos[1], m.pos[2] };
    SetEntityStatus(v, STATUS_ABANDONED);
    WorldAdd(v);
    Log("vehicules : copie de %08X (modele %d, classe %d, couleurs %d/%d -> %d/%d) creee", m.id, m.model, m.vclass,
        m.color1, m.color2, Field<uint8_t>(v, 0x1A0), Field<uint8_t>(v, 0x1A1));
    return v;
}

static void ApplyState(NetVehicle &e)
{
    const MsgVehicle &m = e.state;
    void *v = e.veh;
    // Position : on rattrape l'etat recu extrapole avec la vitesse (unites du jeu : distance par 1/50 s).
    float age = (GetTickCount() - e.lastRecv) / 1000.0f;
    if (age > 0.3f) age = 0.3f;
    Vec3 target = { m.pos[0] + m.speed[0] * age * 50.0f, m.pos[1] + m.speed[1] * age * 50.0f, m.pos[2] + m.speed[2] * age * 50.0f };
    Vec3 &p = Pos(v);
    float dx = target.x - p.x, dy = target.y - p.y, dz = target.z - p.z;
    if (dx * dx + dy * dy + dz * dz > 100.0f) p = target;
    else { p.x += dx * 0.3f; p.y += dy * 0.3f; p.z += dz * 0.3f; }
    Field<Vec3>(v, 0x04) = { m.right[0], m.right[1], m.right[2] };
    Field<Vec3>(v, 0x14) = { m.fwd[0], m.fwd[1], m.fwd[2] };
    Field<Vec3>(v, 0x24) = { m.right[1] * m.fwd[2] - m.right[2] * m.fwd[1], m.right[2] * m.fwd[0] - m.right[0] * m.fwd[2],
                             m.right[0] * m.fwd[1] - m.right[1] * m.fwd[0] };
    MoveSpeed(v) = { m.speed[0], m.speed[1], m.speed[2] };
    TurnSpeed(v) = { m.turn[0], m.turn[1], m.turn[2] };
    Field<uint8_t>(v, 0x1A0) = m.color1;   // SET_CAR_COLOUR peut les changer en cours de route
    Field<uint8_t>(v, 0x1A1) = m.color2;
    Field<float>(v, 0x1EC) = m.steer;
    Field<float>(v, 0x1F0) = m.gas;
    Field<float>(v, 0x1F4) = m.brake;
    VehHealth(v) = m.health;
    // Sans IA : un vehicule "abandonne" garde sa physique mais personne ne le conduit.
    if (EntityStatus(v) != STATUS_WRECKED) SetEntityStatus(v, STATUS_ABANDONED);
}

static void OnVehicle(const MsgVehicle &m)
{
    if (m.owner == g_localId) return;
    NetVehicle *e = FindById(m.id);
    if (!e) { e = Alloc(m.id); if (!e) return; }
    if (e->owner == g_localId && e->veh && e->owner != m.owner)
        Log("vehicules : %08X repris par le joueur %d", m.id, m.owner);
    e->owner = m.owner;
    e->state = m;
    e->haveState = true;
    e->lastRecv = GetTickCount();
}

static void OnVehRemove(uint32_t id)
{
    NetVehicle *e = FindById(id);
    if (!e || e->owner == g_localId) return;
    if (e->veh && e->ours && !VehDriver(e->veh)) {
        void *v = e->veh;
        Unbind(*e);
        WorldRemove(v);
        RemoveReferencesToDeletedObject(v);
        DeleteEntity(v);
    } else {
        Unbind(*e);
    }
    e->used = false;
}

void VehiclesInit()
{
    g_onVehicle = OnVehicle;
    g_onVehRemove = OnVehRemove;
}

void VehiclesFrame(bool inGame)
{
    if (!inGame) {
        // Hors partie (chargement, menu) : le monde est detruit par le jeu, on oublie tout.
        for (auto &e : g_vehs) if (e.used) { e.veh = NULL; e.used = false; }
        return;
    }
    uint32_t now = GetTickCount();
    void *ped = FindPlayerPed();
    void *myVeh = InVehicle(ped) ? PedVehicle(ped) : NULL;
    if (myVeh && VehDriver(myVeh) == ped && VehClass(myVeh) != VCLASS_TRAIN) {
        NetVehicle *e = FindByPtr(myVeh);
        if (!e) {
            e = Alloc(((uint32_t)g_localId << 24) | (++g_vehCounter & 0xFFFFFF));
            if (e) {
                e->owner = (uint8_t)g_localId;
                Bind(*e, myVeh);
                Log("vehicules : je prends %08X (modele %d, couleurs %d/%d)", e->id, ModelIndex(myVeh),
                    Field<uint8_t>(myVeh, 0x1A0), Field<uint8_t>(myVeh, 0x1A1));
            }
        } else if (e->owner != g_localId) {
            e->owner = (uint8_t)g_localId;
            Log("vehicules : je reprends %08X", e->id);
        }
        if (e && now - e->lastSend >= 33) SendVehicle(*e);
    }

    // Hote : les vehicules crees par les scripts de mission sont a lui et partent chez les invites.
    if (g_cfg.host) {
        static uint32_t lastScan;
        if (now - lastScan > 250) {
            lastScan = now;
            Pool *pool = VehiclePool();
            for (int i = 0; i < pool->size; i++) {
                if (pool->flags[i] & 0x80) continue;
                void *v = pool->objects + i * VEHICLE_POOL_ENTRY;
                if (Field<uint8_t>(v, 0x1F8) != VEHICLE_MISSION || VehClass(v) == VCLASS_TRAIN || FindByPtr(v)) continue;
                NetVehicle *e = Alloc(((uint32_t)g_localId << 24) | (++g_vehCounter & 0xFFFFFF));
                if (!e) break;
                e->owner = (uint8_t)g_localId;
                Bind(*e, v);
                Log("vehicules : vehicule de mission %08X (modele %d)", e->id, ModelIndex(v));
            }
        }
    }

    for (auto &e : g_vehs) {
        if (!e.used) continue;
        if (e.owner == g_localId) {
            if (!e.veh) {   // detruit chez nous : on previent les autres
                MsgVehRemove r = { MSG_VEH_REMOVE, e.id };
                NetSendToAll(&r, sizeof(r));
                e.used = false;
                continue;
            }
            if (e.veh != myVeh) {
                // Sans nous au volant : 10 fois par seconde s'il bouge, 1 fois sinon.
                Vec3 v = MoveSpeed(e.veh);
                bool moving = v.x * v.x + v.y * v.y + v.z * v.z > 0.0001f || VehDriver(e.veh);
                if (now - e.lastSend >= (moving ? 100u : 1000u)) SendVehicle(e);
            }
            continue;
        }
        if (!e.haveState) continue;
        if (!e.veh) {
            void *v = CreateCopy(e.state);
            if (v) { Bind(e, v); e.ours = true; }
        }
        if (e.veh) ApplyState(e);
    }
}
