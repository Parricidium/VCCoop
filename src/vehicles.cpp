// Vehicules reseau. Tout vehicule conduit par un joueur recoit un identifiant ; son proprietaire (le dernier
// joueur au volant) envoie son etat, les autres instances en ont une copie qui suit cet etat. Si un autre
// joueur prend le volant d'une copie, il en devient proprietaire et c'est l'ancien qui suit.
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "vehicles.h"
#include "mirror.h"
#include "seats.h"
#include "interp.h"
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
    bool ambient;       // hote : voiture de la circulation partagee (retiree quand plus aucun invite n'est pres)
    MsgVehicle state;   // dernier etat recu (vehicules distants)
    Track track;        // etats recus, pour l'interpolation
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
    m.time = GetTickCount();
    // Station : celle qu'ecoute le conducteur (cMusicManager 0x980038, station en cours +0x3984), sinon celle du vehicule.
    m.radio = drv && drv == FindPlayerPed() ? (uint8_t)*(int *)(0x980038 + 0x3984) : Field<uint8_t>(v, 0x23C);
    // Rotation des roues (par 1/50 s) : calculee par le jeu au rendu (CAutomobile / CBike::PreRender).
    float step = TimeStep() > 0.01f ? TimeStep() : 1.0f;
    if (m.vclass == VCLASS_BIKE) { m.wheelSpin[0] = Field<float>(v, 0x418) / step; m.wheelSpin[1] = Field<float>(v, 0x41C) / step; }
    else if (m.vclass != VCLASS_BOAT) for (int i = 0; i < 4; i++) m.wheelSpin[i] = Field<float>(v, 0x4F0 + i * 4) / step;
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
    // Position, orientation, volant et roues : apres la physique (VehiclesAfterProcess).
    Field<uint8_t>(v, 0x1A0) = m.color1;   // SET_CAR_COLOUR peut les changer en cours de route
    Field<uint8_t>(v, 0x1A1) = m.color2;
    VehHealth(v) = m.health;
    // Radio : celle que le conducteur a choisie (ou eteinte). Passager : on l'impose a notre musique, comme
    // SET_RADIO_CHANNEL (041E) des missions.
    Field<uint8_t>(v, 0x23C) = m.radio;
    void *me = FindPlayerPed();
    if (me && InVehicle(me) && PedVehicle(me) == v && SeatOf(v, me) > 0 && *(int *)(0x980038 + 0x3984) != m.radio) {
        static uint32_t last;
        if (GetTickCount() - last > 1000) {
            last = GetTickCount();
            int32_t args[2] = { m.radio, -1 };
            MirrorLocal(0x041E, 2, args);
            Log("vehicules : radio %d (celle du conducteur)", m.radio);
        }
    }
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
    ClockSample(m.owner, m.time);
    Snap n = {};
    n.t = m.time;
    for (int k = 0; k < 3; k++) { n.pos[k] = m.pos[k]; n.vel[k] = m.speed[k]; n.right[k] = m.right[k]; n.fwd[k] = m.fwd[k]; }
    e->track.Push(n);
}

// Apres la physique : chaque copie est placee a son etat interpole, avec le volant et la rotation des roues du
// proprietaire (un vehicule "abandonne" a le volant droit et freine, ses roues ne tournaient pas).
void VehiclesAfterProcess()
{
    for (auto &e : g_vehs) {
        if (!e.used || e.owner == g_localId || !e.veh || !e.haveState) continue;
        void *v = e.veh;
        const MsgVehicle &m = e.state;
        Snap n;
        if (!TrackSample(e.track, m.owner, n)) continue;
        Pos(v) = { n.pos[0], n.pos[1], n.pos[2] };
        Field<Vec3>(v, 0x04) = { n.right[0], n.right[1], n.right[2] };
        Field<Vec3>(v, 0x14) = { n.fwd[0], n.fwd[1], n.fwd[2] };
        Vec3 up = { n.right[1] * n.fwd[2] - n.right[2] * n.fwd[1], n.right[2] * n.fwd[0] - n.right[0] * n.fwd[2],
                    n.right[0] * n.fwd[1] - n.right[1] * n.fwd[0] };
        Field<Vec3>(v, 0x24) = up;
        // "right" recalcule pour une matrice bien orthogonale apres le melange des deux etats.
        Field<Vec3>(v, 0x04) = { n.fwd[1] * up.z - n.fwd[2] * up.y, n.fwd[2] * up.x - n.fwd[0] * up.z, n.fwd[0] * up.y - n.fwd[1] * up.x };
        MoveSpeed(v) = { n.vel[0], n.vel[1], n.vel[2] };
        TurnSpeed(v) = { m.turn[0], m.turn[1], m.turn[2] };
        Field<float>(v, 0x1EC) = m.steer;
        Field<float>(v, 0x1F0) = m.gas;
        Field<float>(v, 0x1F4) = m.brake;
        // Roues : le rendu ne les fait tourner que si elles touchent le sol dans notre physique ; sinon on le fait.
        float step = TimeStep();
        if (m.vclass == VCLASS_BIKE) {
            if (Field<float>(v, 0x3F0) <= 0.0f && Field<float>(v, 0x3F4) <= 0.0f) Field<float>(v, 0x410) += m.wheelSpin[0] * step;
            if (Field<float>(v, 0x3F8) <= 0.0f && Field<float>(v, 0x3FC) <= 0.0f) Field<float>(v, 0x414) += m.wheelSpin[1] * step;
        } else if (m.vclass != VCLASS_BOAT) {
            for (int i = 0; i < 4; i++)
                if (Field<float>(v, 0x4A4 + i * 4) <= 0.0f) Field<float>(v, 0x4D0 + i * 4) += m.wheelSpin[i] * step;
        }
        if (g_cfg.logScripts && (m.speed[0] * m.speed[0] + m.speed[1] * m.speed[1]) > 0.01f) {
            static uint32_t lastLog;
            if (GetTickCount() - lastLog > 1000) {
                lastLog = GetTickCount();
                bool bike = m.vclass == VCLASS_BIKE;
                Log("vehicules : copie %08X roule, volant %.2f, roues %.2f/%.2f (recu %.2f/%.2f), contact %.2f/%.2f", m.id, m.steer,
                    Field<float>(v, bike ? 0x410 : 0x4D0), Field<float>(v, bike ? 0x414 : 0x4D8), m.wheelSpin[0], m.wheelSpin[bike ? 1 : 2],
                    Field<float>(v, bike ? 0x3F0 : 0x4A4), Field<float>(v, bike ? 0x3F8 : 0x4AC));
            }
        }
    }
}

static void OnVehRemove(uint32_t id)
{
    NetVehicle *e = FindById(id);
    if (!e || e->owner == g_localId) return;
    void *me = FindPlayerPed();
    if (e->veh && e->ours && !(me && InVehicle(me) && PedVehicle(me) == e->veh)) {
        // Copie : on la supprime, apres avoir fait descendre ses occupants (copies ou Tommy distants).
        void *v = e->veh;
        Unbind(*e);
        void *occ[9] = { VehDriver(v) };
        for (int i = 0; i < 8; i++) occ[i + 1] = VehPassenger(v, i);
        for (void *p : occ) if (p) WarpOutOfVehicle(p, NULL);
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
                uint8_t by = Field<uint8_t>(v, 0x1F8);
                if (VehClass(v) == VCLASS_TRAIN || FindByPtr(v)) continue;
                // Vehicules de mission partout ; circulation et voitures garees seulement pres d'un invite.
                bool ambient = (by == 1 || by == 3) && NearAnyGuest(&Pos(v).x, AreaCode(v), (float)AMBIENT_SHARE_M);
                if (by != VEHICLE_MISSION && !ambient) continue;
                NetVehicle *e = Alloc(((uint32_t)g_localId << 24) | (++g_vehCounter & 0xFFFFFF));
                if (!e) break;
                e->owner = (uint8_t)g_localId;
                e->ambient = ambient;
                Bind(*e, v);
                if (!ambient) Log("vehicules : vehicule de mission %08X (modele %d)", e->id, ModelIndex(v));
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
            // Circulation partagee : plus aucun invite assez pres -> on la retire chez eux (on la garde ici).
            if (e.ambient && e.veh != myVeh && !NearAnyGuest(&Pos(e.veh).x, AreaCode(e.veh), AMBIENT_SHARE_M + 40.0f)) {
                MsgVehRemove r = { MSG_VEH_REMOVE, e.id };
                NetSendToAll(&r, sizeof(r));
                Unbind(e);
                e.used = false;
                continue;
            }
            if (e.veh != myVeh) {
                // Sans nous au volant : 20 fois par seconde s'il bouge, 1 fois sinon.
                Vec3 v = MoveSpeed(e.veh);
                bool moving = v.x * v.x + v.y * v.y + v.z * v.z > 0.0001f || VehDriver(e.veh);
                if (now - e.lastSend >= (moving ? 50u : 1000u)) SendVehicle(e);
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
