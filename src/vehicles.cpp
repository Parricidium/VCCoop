// Vehicules reseau. Tout vehicule conduit par un joueur recoit un identifiant ; son proprietaire (le dernier
// joueur au volant) envoie son etat, les autres instances en ont une copie qui suit cet etat. Si un autre
// joueur prend le volant d'une copie, il en devient proprietaire et c'est l'ancien qui suit.
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "vehicles.h"
#include "entities.h"
#include "mirror.h"
#include "seats.h"
#include "interp.h"
#include "population.h"
#include "combat.h"
#include <math.h>
#include <string.h>

using namespace game;

enum { MAX_NET_VEHICLES = 128 };   // le pool du jeu en compte 110 ; a 64, la circulation partagee remplissait tout

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
    uint32_t lastDamageSync;
    bool blown;         // copie deja explosee (EXPLODE_CAR une seule fois)
    float peak[3];      // vitesse recue la plus forte des 400 dernieres ms (celle d'avant un choc, CopyCollisions)
    uint32_t peakAt;
    uint32_t hostHandle;   // reference de pool chez l'hote (traduction des commandes de mission), meme si un invite l'a reprise
    int model;             // modele au moment de l'association : s'il change, la case du pool a ete reutilisee (entree perimee)
    uint32_t idleSince;    // proprietaire d'une copie : depuis quand elle est vide et immobile
    float appliedHealth;   // copie : derniere sante du proprietaire posee (une baisse locale = quelqu'un l'abime ici)
};
static NetVehicle g_vehs[MAX_NET_VEHICLES];
static uint32_t g_vehCounter;

static NetVehicle *FindById(uint32_t id)
{
    for (auto &e : g_vehs) if (e.used && e.id == id) return &e;
    return NULL;
}

static bool Stale(const NetVehicle &e);
static NetVehicle *FindByPtr(void *veh)
{
    for (auto &e : g_vehs) if (e.used && e.veh == veh && !Stale(e)) return &e;
    return NULL;
}

static NetVehicle *Alloc(uint32_t id)
{
    for (auto &e : g_vehs)
        if (!e.used) { memset(&e, 0, sizeof(e)); e.used = true; e.id = id; return &e; }
    return NULL;
}

static void Unbind(NetVehicle &e);
// Pour le vehicule du joueur local : table pleine -> on evince une voiture de circulation partagee a nous (les
// autres apprennent son retrait), sinon personne ne le voyait rouler (pas d'identifiant, double cache).
static NetVehicle *AllocPriority(uint32_t id, void *myVeh)
{
    if (NetVehicle *e = Alloc(id)) return e;
    for (auto &e : g_vehs) {
        if (!e.used || e.owner != g_localId || !e.ambient || e.veh == myVeh || VehDriver(e.veh)) continue;
        MsgVehRemove r = { MSG_VEH_REMOVE, e.id };
        NetSendToAll(&r, sizeof(r));
        Unbind(e);
        memset(&e, 0, sizeof(e));
        e.used = true;
        e.id = id;
        Log("vehicules : table pleine, une voiture de circulation cede sa place");
        return &e;
    }
    Log("vehicules : table pleine, vehicule du joueur non annonce");
    return NULL;
}

static void Bind(NetVehicle &e, void *veh)
{
    e.veh = veh;
    e.model = veh ? ModelIndex(veh) : 0;
    if (veh) RegisterReference(veh, &e.veh);
}

// Vehicule supprime par le jeu sans que notre reference ait ete effacee, et sa case reprise par un autre : l'entree
// ne vaut plus rien (le double d'un joueur allait monter dans une voiture de l'intro disparue).
static bool Stale(const NetVehicle &e) { return e.veh && e.model && ModelIndex(e.veh) != e.model; }

static void Unbind(NetVehicle &e)
{
    if (e.veh) CleanUpOldReference(e.veh, &e.veh);
    e.veh = NULL;
}

bool GuestVehicleForHost(uint32_t host, uint32_t &guest)
{
    for (auto &e : g_vehs)
        if (e.used && e.hostHandle == host && e.veh) {
            guest = VehicleHandle(e.veh);
            return true;
        }
    return false;
}

static void SendVehicle(NetVehicle &e);
// Vehicule cree par le menu en jeu (panel.cpp) : en reseau tout de suite, a nous, les autres le voient arriver.
void NetVehicleShare(void *veh)
{
    if (!veh || g_localId < 0 || FindByPtr(veh)) return;
    NetVehicle *e = Alloc(((uint32_t)g_localId << 24) | (++g_vehCounter & 0xFFFFFF));
    if (!e) return;
    e->owner = (uint8_t)g_localId;
    Bind(*e, veh);
    SendVehicle(*e);
    Log("vehicules : %08X cree par le menu (modele %d)", e->id, ModelIndex(veh));
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

// Copie : roule-t-elle chez son proprietaire ? (sa vitesse physique est tenue a zero ici)
bool NetVehicleIsCopy(void *veh)
{
    NetVehicle *e = FindByPtr(veh);
    return e && e->owner != g_localId;
}

bool NetVehicleMoving(void *veh)
{
    NetVehicle *e = FindByPtr(veh);
    if (!e) { Vec3 s = MoveSpeed(veh); return s.x * s.x + s.y * s.y > 0.01f; }
    if (e->owner == g_localId || !e->haveState) { Vec3 s = MoveSpeed(veh); return s.x * s.x + s.y * s.y > 0.01f; }
    return e->state.speed[0] * e->state.speed[0] + e->state.speed[1] * e->state.speed[1] > 0.01f;
}

// Un autre joueur a abime notre vehicule chez lui (balles, batte, feu sur sa copie) : meme perte de sante ici ;
// en dessous de 250 le jeu allume lui-meme le moteur, puis l'epave part chez tout le monde par MsgVehicle.
void ApplyVehicleDamage(uint32_t id, float damage)
{
    NetVehicle *e = FindById(id);
    if (!e || e->owner != g_localId || !e->veh || damage <= 0.0f || EntityStatus(e->veh) == STATUS_WRECKED) return;
    if (Field<uint8_t>(e->veh, 0x53) & 0x02) return;   // pare-balles (SET_CAR_PROOFS des missions)
    float &h = VehHealth(e->veh);
    h -= damage;
    if (h < 0.0f) h = 0.0f;
    if (g_cfg.logScripts) Log("vehicules : %08X abime par un autre joueur (-%.0f) -> sante %.0f", id, damage, h);
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
    m.ambient = e.ambient && m.driver == 0xFF;   // une voiture conduite par un joueur n'est jamais "de la circulation"
    m.wrecked = EntityStatus(v) == STATUS_WRECKED;
    uint8_t f9 = Field<uint8_t>(v, 0x1F9);
    m.vflags = ((f9 & 0x40) ? 1 : 0) | ((f9 & 0x10) ? 2 : 0) | (Field<uint8_t>(v, 0x245) ? 4 : 0) | (Field<int16_t>(v, 0x240) ? 8 : 0) |
               (VehClass(v) == VCLASS_CAR && (Field<uint8_t>(v, 0x501) & 1) ? 16 : 0);
    m.doorLock = (int8_t)Field<int>(v, 0x230);
    if (VehClass(v) == VCLASS_BIKE) { m.lean = Field<float>(v, 0x46C); m.pedLean = Field<float>(v, 0x478); }
    // Station : celle qu'ecoute le conducteur (cMusicManager 0x980038, station en cours +0x3984), sinon celle du vehicule.
    if (m.vclass == VCLASS_CAR) memcpy(m.damage, (uint8_t *)v + 0x2A0, sizeof(m.damage));
    m.radio = drv && drv == FindPlayerPed() ? (uint8_t)*(int *)(0x980038 + 0x3984) : Field<uint8_t>(v, 0x23C);
    // Rotation des roues (par 1/50 s) : calculee par le jeu au rendu (CAutomobile / CBike::PreRender).
    float step = TimeStep() > 0.01f ? TimeStep() : 1.0f;
    if (m.vclass == VCLASS_BIKE) { m.wheelSpin[0] = Field<float>(v, 0x418) / step; m.wheelSpin[1] = Field<float>(v, 0x41C) / step; }
    else if (m.vclass != VCLASS_BOAT) for (int i = 0; i < 4; i++) m.wheelSpin[i] = Field<float>(v, 0x4F0 + i * 4) / step;
    NetSendToAll(&m, sizeof(m));
    e.lastSend = GetTickCount();
}

// Copie : gelee pour la physique du jeu (CPhysical bIsFrozen, 0x11B bit 0 : ApplyMoveSpeed/ApplyTurnSpeed ne la
// deplacent plus, verifie dans l'exe 0x4B877D...) et a l'epreuve des explosions locales (0x52 bit 0x02 ; elle
// explose quand son proprietaire l'annonce, EXPLODE_CAR). Sa vitesse reste celle recue : roues, son du moteur et
// pose du motard (pied a terre sous 0,02) en dependent.
static void SetCopyFlags(void *v, bool copy)
{
    if (copy) { Field<uint8_t>(v, 0x11B) |= 0x01; Field<uint8_t>(v, 0x52) |= 0x02; }
    else { Field<uint8_t>(v, 0x11B) &= ~0x01; Field<uint8_t>(v, 0x52) &= ~0x02; }
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
    Field<uint8_t>(v, 0x53) |= 0x08;   // bCollisionProof : ses degats sont ceux du proprietaire (SyncDamage)
    SetCopyFlags(v, true);
    WorldAdd(v);
    Log("vehicules : copie de %08X (modele %d, classe %d, couleurs %d/%d -> %d/%d) creee", m.id, m.model, m.vclass,
        m.color1, m.color2, Field<uint8_t>(v, 0x1A0), Field<uint8_t>(v, 0x1A1));
    return v;
}

// --- Degats visibles (voitures) : la copie montre ceux du proprietaire ---
// CDamageManager (+0x2A0) : portes (6), ailes / pare-chocs / pare-brise (7 panneaux, 4 bits chacun en +0x14),
// phares (+0x10), pneus, moteur. Chaque piece est redessinee par CAutomobile::SetDoorDamage / SetPanelDamage /
// SetBumperDamage (noeuds du modele, comme dans CAutomobile::VehicleDamage). Si la copie a des degats que le
// proprietaire n'a pas (reparee au Pay'n'Spray, ou abimee ici), on la repare (CAutomobile::Fix) et on reapplique.
static int DoorStatus(const uint8_t *dm, int d) { return ((int(__thiscall *)(const void *, int))0x5A9810)(dm, d); }
static int PanelStatus(const uint8_t *dm, int p) { return (*(const uint32_t *)(dm + 0x14) >> (p * 4)) & 0xF; }
static int WheelStatus(const uint8_t *dm, int w) { return ((int(__thiscall *)(const void *, int))0x5A9830)(dm, w); }
static int EngineStatus(const uint8_t *dm) { return ((int(__thiscall *)(const void *))0x5A97E0)(dm); }

// Portes : 0 intacte, 1 abimee, 2 battante, 3 arrachee. Entre 1 et 2 le jeu passe tout seul selon le mouvement
// (porte qui bat, qui se referme) : on ne compare que la gravite, sinon la porte s'ouvrait et se refermait en
// boucle chez les autres (reparation + reapplication a chaque synchro).
static int DoorSeverity(int s) { return s == 0 ? 0 : s == 3 ? 2 : 1; }

static void SyncDamage(void *v, const uint8_t *od)
{
    uint8_t *dm = (uint8_t *)v + 0x2A0;
    if (!memcmp(dm + 0x10, od + 0x10, 8) && !memcmp(dm, od, 9)) {   // panneaux, phares, moteur, pneus egaux
        bool same = true;
        for (int d = 0; d < 6; d++) same &= DoorSeverity(DoorStatus(dm, d)) == DoorSeverity(DoorStatus(od, d));
        if (same) return;
    }
    // Pas pendant que le joueur local monte ou descend (etats 0x37..0x3F) : la portiere est a lui a ce moment-la.
    void *me = FindPlayerPed();
    if (me && PedVehicle(me) == v && (EnteringState(PedState(me)) || ExitingState(PedState(me)))) return;
    static const int doorNode[6] = { 0x11, 0x12, 0x0F, 0x0B, 0x10, 0x0C };
    static const int panelNode[5] = { 0x0D, 0x09, 0x0E, 0x0A, 0x13 };
    bool fix = false;
    for (int d = 0; d < 6; d++) fix |= DoorSeverity(DoorStatus(dm, d)) > DoorSeverity(DoorStatus(od, d));
    for (int p = 0; p < 7; p++) fix |= PanelStatus(dm, p) != 0 && PanelStatus(od, p) == 0;
    if (fix) ((void(__thiscall *)(void *))0x588530)(v);   // CAutomobile::Fix
    for (int d = 0; d < 6; d++) {
        int s = DoorStatus(od, d);
        if (DoorSeverity(s) <= DoorSeverity(DoorStatus(dm, d))) continue;   // seulement plus abimee
        ((void(__thiscall *)(void *, int, int))0x5A9820)(dm, d, s);                              // SetDoorStatus
        ((void(__thiscall *)(void *, int, int, bool))0x59B150)(v, doorNode[d], d, true);         // sans piece volante
    }
    for (int p = 0; p < 7; p++) {
        int s = PanelStatus(od, p);
        if (s == PanelStatus(dm, p)) continue;
        uint32_t &bits = *(uint32_t *)(dm + 0x14);
        bits = (bits & ~(0xFu << (p * 4))) | ((uint32_t)s << (p * 4));
        if (p < 5) ((void(__thiscall *)(void *, int, int, bool))0x59B2A0)(v, panelNode[p], p, false);
        else ((void(__thiscall *)(void *, int, int, bool))0x59B370)(v, p == 5 ? 7 : 8, p, true);   // pare-chocs
    }
    *(uint32_t *)(dm + 0x10) = *(const uint32_t *)(od + 0x10);   // phares
    for (int w = 0; w < 4; w++) ((void(__thiscall *)(void *, int, int))0x5A9840)(dm, w, WheelStatus(od, w));
    ((void(__thiscall *)(void *, int))0x5A97F0)(dm, EngineStatus(od));
    if (g_cfg.logScripts)
        Log("vehicules : degats copies (reparee %d) : capot %d, coffre %d, panneaux %08X -> %08X, identique %d", fix,
            DoorStatus(dm, 0), DoorStatus(dm, 1), *(const uint32_t *)(od + 0x14), *(uint32_t *)(dm + 0x14), !memcmp(dm, od, 24));
}

static void ApplyState(NetVehicle &e)
{
    const MsgVehicle &m = e.state;
    void *v = e.veh;
    // Position, orientation, volant et roues : apres la physique (VehiclesAfterProcess).
    Field<uint8_t>(v, 0x1A0) = m.color1;   // SET_CAR_COLOUR peut les changer en cours de route
    Field<uint8_t>(v, 0x1A1) = m.color2;
    // Sante tombee chez nous depuis la derniere fois (balles, batte, flammes du joueur local sur la copie : le jeu
    // baisse +0x204 sans passer par les chocs) : l'ecart part chez le proprietaire, qui l'applique a la vraie
    // voiture ; la copie reprend la sante du proprietaire. Seulement si notre Tommy est a portee (nos propres
    // personnages tirent aussi sur les copies, mais eux sont copies chez le proprietaire et y tirent deja).
    float local = VehHealth(v);
    void *me0 = FindPlayerPed();
    // (Seulement juste apres un tir a balles du joueur local : un feu local sur la copie, une explosion rejouee la
    // font aussi baisser, et ces degats-la existent deja chez le proprietaire ; sans ce filtre sa voiture se vidait.)
    if (me0 && e.appliedHealth > 0.0f && local < e.appliedHealth - 0.5f && EntityStatus(v) != STATUS_WRECKED && LocalBulletRecently()) {
        float dx = Pos(v).x - Pos(me0).x, dy = Pos(v).y - Pos(me0).y;
        if (dx * dx + dy * dy < 60.0f * 60.0f) SendVehicleDamage(m.owner, m.id, e.appliedHealth - local);
    }
    VehHealth(v) = m.health;
    e.appliedHealth = m.health;
    // Degats : ceux du proprietaire (2 fois par seconde au plus, une reparation + reapplication coute un peu).
    Field<uint8_t>(v, 0x53) |= 0x08;   // bCollisionProof : pas de degats de choc chez nous pour une copie
    SetCopyFlags(v, true);
    if (m.vclass == VCLASS_CAR && VehClass(v) == VCLASS_CAR && GetTickCount() - e.lastDamageSync > 500) {
        e.lastDamageSync = GetTickCount();
        SyncDamage(v, m.damage);
    }
    Field<int>(v, 0x230) = m.doorLock;   // verrou : on ne monte pas dans une copie que l'original refuse
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
    // Epave chez le proprietaire : la copie explose aussi (EXPLODE_CAR 020B), une fois.
    if (m.wrecked && !e.blown && EntityStatus(v) != STATUS_WRECKED) {
        e.blown = true;
        int32_t h[1] = { (int32_t)VehicleHandle(v) };
        MirrorLocal(0x020B, 1, h);
        Log("vehicules : copie %08X detruite (epave chez son proprietaire)", m.id);
    }
    // Sans IA : un vehicule "abandonne" garde sa physique mais personne ne le conduit.
    if (EntityStatus(v) != STATUS_WRECKED) SetEntityStatus(v, STATUS_ABANDONED);
}

static void DeleteCopy(NetVehicle &e);

static bool AnyPlayerNear(const Vec3 &p, float r)
{
    void *me = FindPlayerPed();
    if (me) { float dx = Pos(me).x - p.x, dy = Pos(me).y - p.y; if (dx * dx + dy * dy < r * r) return true; }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const NetPlayer &n = g_players[i];
        if (i == g_localId || !n.connected || !n.state.inGame) continue;
        float dx = n.state.pos[0] - p.x, dy = n.state.pos[1] - p.y;
        if (dx * dx + dy * dy < r * r) return true;
    }
    return false;
}

static void OnVehicle(const MsgVehicle &m)
{
    if (m.owner == g_localId) return;
    NetVehicle *e = FindById(m.id);
    // Invite avec son propre monde (loin de l'hote) : la circulation partagee envoyee pour un autre invite ne le
    // concerne pas, sinon elle se superposait a la sienne.
    // Voiture de la circulation d'un autre joueur trop loin de nous : pas de copie.
    if (m.ambient) {
        void *me = FindPlayerPed();
        float dx = me ? m.pos[0] - Pos(me).x : 0, dy = me ? m.pos[1] - Pos(me).y : 0;
        if (!me || dx * dx + dy * dy > (float)AMBIENT_DROP_M * AMBIENT_DROP_M) { if (e) { DeleteCopy(*e); e->used = false; } return; }
    }
    if (!e) { e = Alloc(m.id); if (!e) return; }
    if (e->owner == g_localId && e->veh && e->owner != m.owner)
        Log("vehicules : %08X repris par le joueur %d", m.id, m.owner);
    e->owner = m.owner;
    if (m.owner == 0) e->hostHandle = m.poolHandle;
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
    void *meP = FindPlayerPed();
    for (auto &e : g_vehs) {
        if (!e.used || e.owner == g_localId || !e.veh || !e.haveState) continue;
        void *v = e.veh;
        const MsgVehicle &m = e.state;
        Snap n;
        if (!TrackSample(e.track, m.owner, n)) continue;
        // Diagnostic : passager d'une copie (copie qui s'enfonce dans la route, etincelles chez JD) : toutes les 3 s,
        // ou l'on est, ou le proprietaire la met, ou la physique l'avait mise, contacts des roues, pneus, sante.
        if (meP && InVehicle(meP) && PedVehicle(meP) == v) {
            static uint32_t lastDiag;
            if (GetTickCount() - lastDiag > 3000) {
                lastDiag = GetTickCount();
                const uint8_t *dm = (const uint8_t *)v + 0x2A0;
                Log("vehicules : passager de %08X : proprietaire z %.2f, physique z %.2f, recu z %.2f, contact %.2f %.2f %.2f %.2f, pneus %d%d%d%d, sante %.0f/%.0f, statut %d, vitesse %.2f",
                    m.id, m.pos[2], Pos(v).z, n.pos[2], Field<float>(v, 0x4A4), Field<float>(v, 0x4A8), Field<float>(v, 0x4AC), Field<float>(v, 0x4B0),
                    WheelStatus(dm, 0), WheelStatus(dm, 1), WheelStatus(dm, 2), WheelStatus(dm, 3), VehHealth(v), m.health, EntityStatus(v),
                    sqrtf(m.speed[0] * m.speed[0] + m.speed[1] * m.speed[1]));
            }
        }
        float jx = n.pos[0] - Pos(v).x, jy = n.pos[1] - Pos(v).y, jz = n.pos[2] - Pos(v).z;
        if (jx * jx + jy * jy + jz * jz > 400.0f) Teleport(v, { n.pos[0], n.pos[1], n.pos[2] });   // grand ecart : secteurs a jour
        Pos(v) = { n.pos[0], n.pos[1], n.pos[2] };
        Field<Vec3>(v, 0x04) = { n.right[0], n.right[1], n.right[2] };
        Field<Vec3>(v, 0x14) = { n.fwd[0], n.fwd[1], n.fwd[2] };
        Vec3 up = { n.right[1] * n.fwd[2] - n.right[2] * n.fwd[1], n.right[2] * n.fwd[0] - n.right[0] * n.fwd[2],
                    n.right[0] * n.fwd[1] - n.right[1] * n.fwd[0] };
        Field<Vec3>(v, 0x24) = up;
        // "right" recalcule pour une matrice bien orthogonale apres le melange des deux etats.
        Field<Vec3>(v, 0x04) = { n.fwd[1] * up.z - n.fwd[2] * up.y, n.fwd[2] * up.x - n.fwd[0] * up.z, n.fwd[0] * up.y - n.fwd[1] * up.x };
        // Copie gelee (SetCopyFlags) : la physique du jeu ne la deplace plus (avant 28k elle integrait la vitesse
        // recue : 10-16 cm de haut en bas par image, tremblements et etincelles pour le passager), mais la vitesse
        // recue reste posee : roues au sol qui tournent, son du moteur, pose du motard, CanPedExitCar.
        SetCopyFlags(v, true);
        {   // vitesse bornee, jamais NaN (saut de position = vitesse enorme : hors du monde, plantage 0x4B0347)
            Vec3 vel = { n.vel[0], n.vel[1], n.vel[2] };
            float vl = sqrtf(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
            if (!(vl <= 3.0f)) { float k = vl > 0 && vl < 1e30f ? 3.0f / vl : 0.0f; vel = { vel.x * k, vel.y * k, vel.z * k }; }
            MoveSpeed(v) = vel;
        }
        TurnSpeed(v) = { m.turn[0], m.turn[1], m.turn[2] };
        // Phares, moteur, sirene, klaxon, taxi : l'etat "abandonne" les eteint chez nous a chaque image.
        uint8_t &f9 = Field<uint8_t>(v, 0x1F9);
        f9 = (f9 & ~0x50) | ((m.vflags & 1) ? 0x40 : 0) | ((m.vflags & 2) ? 0x10 : 0);
        Field<uint8_t>(v, 0x245) = (m.vflags & 4) ? 1 : 0;
        if (m.vflags & 8) Field<int16_t>(v, 0x240) = 2;
        if (m.vclass == VCLASS_CAR && VehClass(v) == VCLASS_CAR) Field<uint8_t>(v, 0x501) = (Field<uint8_t>(v, 0x501) & ~1) | ((m.vflags & 16) ? 1 : 0);
        // Moto : inclinaison et penchement du pilote (sinon elle prenait les virages droite comme un i).
        if (m.vclass == VCLASS_BIKE && VehClass(v) == VCLASS_BIKE) {
            Field<float>(v, 0x46C) = Field<float>(v, 0x470) = m.lean;
            Field<float>(v, 0x478) = m.pedLean;
            Field<uint8_t>(v, 0x2C0) = 0;   // bLeanMatrixClean : matrice d'inclinaison a refaire au rendu
        }
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

// Copie : on la supprime, apres avoir fait descendre ses occupants (copies ou Tommy distants). Si c'est notre
// vehicule a nous (ou que nous sommes dedans), on le garde et on l'oublie seulement.
static void DeleteCopy(NetVehicle &e)
{
    void *me = FindPlayerPed();
    if (e.veh && e.ours && !(me && InVehicle(me) && PedVehicle(me) == e.veh)) {
        void *v = e.veh;
        Unbind(e);
        void *occ[9] = { VehDriver(v) };
        for (int i = 0; i < 8; i++) occ[i + 1] = VehPassenger(v, i);
        for (void *p : occ) if (p) WarpOutOfVehicle(p, NULL);
        WorldRemove(v);
        RemoveReferencesToDeletedObject(v);
        DeleteEntity(v);
    } else {
        Unbind(e);
    }
}

static void OnVehRemove(uint32_t id)
{
    NetVehicle *e = FindById(id);
    if (!e || e->owner == g_localId) return;
    DeleteCopy(*e);
    e->used = false;
}

// Un joueur est parti : ses copies disparaissent ; un vehicule a nous qu'il conduisait (ou dans lequel on est)
// redevient a nous. Sans cela ses voitures restaient figees partout, et un jeu relance (meme numero de joueur,
// compteur repris a zero) retombait sur ces vieux identifiants : on voyait l'ancienne voiture a la place de la nouvelle.
static void PlayerLeft(int who)
{
    void *me = FindPlayerPed();
    int dropped = 0, adopted = 0;
    for (auto &e : g_vehs) {
        if (!e.used || e.owner != who) continue;
        bool inside = me && InVehicle(me) && PedVehicle(me) == e.veh && VehDriver(e.veh) == me;   // passager : on ne la reprend pas
        if (e.veh && (!e.ours || inside)) {
            e.owner = (uint8_t)g_localId;
            e.ambient = false;
            Field<uint8_t>(e.veh, 0x53) &= ~0x08;
            SetCopyFlags(e.veh, false);
            adopted++;
        } else {
            DeleteCopy(e);
            e.used = false;
            dropped++;
        }
    }
    if (dropped || adopted) Log("vehicules : joueur %d parti, %d copies retirees, %d vehicules repris", who, dropped, adopted);
}

void VehiclesInit()
{
    g_onVehicle = OnVehicle;
    g_onVehRemove = OnVehRemove;
    g_vehCounter = (GetTickCount() * 2654435761u) & 0xFFFFFF;   // pas les memes identifiants d'un lancement a l'autre
}

// --- Chocs contre la copie du vehicule d'un autre joueur ---
// Une copie est gelee (sa position vient du reseau) : pour notre physique c'est un mur de masse infinie. Percute par
// un autre joueur, notre vehicule ne bougeait pas ; celui qui percutait s'arretait net. Chaque machine corrige les
// vehicules dont elle fait la physique (le sien, sa circulation) : dans l'axe du choc, la vitesse devient celle
// d'un vrai choc entre deux masses (vitesse d'avant le choc, vitesse recue de la copie, CPhysical::m_fMass +0xB8,
// enregistrements de collision +0xE6 / +0xE8, CVehicle cf. reVC Physical.h). Symetrique : l'autre fait de meme.
static Vec3 g_prevSpeed[256];

// Boite de collision du vehicule dans le plan (CColModel : boite min +0x10, max +0x1C ; modele +0x1C de sa fiche).
struct Box2 { float cx, cy, ax, ay, bx, by, hx, hy; };
static bool BoxOf(void *e, Box2 &b)
{
    void *mi = ModelInfo(ModelIndex(e));
    uint8_t *col = mi ? *(uint8_t **)((uint8_t *)mi + 0x1C) : NULL;
    if (!col) return false;
    Vec3 mn = *(Vec3 *)(col + 0x10), mx = *(Vec3 *)(col + 0x1C);
    Vec3 r = Field<Vec3>(e, 0x4), f = Field<Vec3>(e, 0x14), p = Pos(e);
    float ox = (mn.x + mx.x) * 0.5f, oy = (mn.y + mx.y) * 0.5f;
    b.hx = (mx.x - mn.x) * 0.5f; b.hy = (mx.y - mn.y) * 0.5f;
    float lr = sqrtf(r.x * r.x + r.y * r.y), lf = sqrtf(f.x * f.x + f.y * f.y);
    if (lr < 0.1f || lf < 0.1f || b.hx < 0.2f || b.hy < 0.2f || b.hx > 10.0f || b.hy > 20.0f) return false;
    b.ax = r.x / lr; b.ay = r.y / lr; b.bx = f.x / lf; b.by = f.y / lf;
    b.cx = p.x + r.x * ox + f.x * oy; b.cy = p.y + r.y * ox + f.y * oy;
    return true;
}
// Les deux boites se chevauchent-elles (axes separateurs) ?
static bool Overlap(const Box2 &a, const Box2 &b)
{
    const float axes[4][2] = { { a.ax, a.ay }, { a.bx, a.by }, { b.ax, b.ay }, { b.bx, b.by } };
    float dx = b.cx - a.cx, dy = b.cy - a.cy;
    for (auto &L : axes) {
        float pa = a.hx * fabsf(a.ax * L[0] + a.ay * L[1]) + a.hy * fabsf(a.bx * L[0] + a.by * L[1]);
        float pb = b.hx * fabsf(b.ax * L[0] + b.ay * L[1]) + b.hy * fabsf(b.bx * L[0] + b.by * L[1]);
        if (fabsf(dx * L[0] + dy * L[1]) > pa + pb + 0.05f) return false;
    }
    return true;
}

static void CopyCollisions()
{
    // Chez nous, l'autre s'est deja arrete contre notre copie quand la sienne arrive sur nous (retard du reseau) :
    // on garde la vitesse d'avant le choc.
    uint32_t now = GetTickCount();
    for (auto &e : g_vehs) {
        if (!e.used || !e.haveState) continue;
        float c2 = e.state.speed[0] * e.state.speed[0] + e.state.speed[1] * e.state.speed[1];
        float p2 = e.peak[0] * e.peak[0] + e.peak[1] * e.peak[1];
        if (c2 >= p2 || now - e.peakAt > 400) { memcpy(e.peak, e.state.speed, 12); e.peakAt = now; }
    }
    Pool *vp = VehiclePool();
    int n = vp->size < 256 ? vp->size : 256;
    for (int i = 0; i < n; i++) {
        if (vp->flags[i] & 0x80) continue;
        uint8_t *v = vp->objects + i * VEHICLE_POOL_ENTRY;
        NetVehicle *mine = FindByPtr(v);
        if (mine && mine->owner != g_localId) continue;   // copie : sa physique est chez son proprietaire
        Vec3 prev = g_prevSpeed[i];
        g_prevSpeed[i] = MoveSpeed(v);
        // Le vehicule touche : m_pDamageEntity (+0x108, pose a chaque choc de vehicule) ; les enregistrements de
        // collision (+0xE6/+0xE8) seulement si le jeu les tient pour ce vehicule (bUseCollisionRecords).
        uint8_t *hits[7];
        int nh = 0;
        if (Field<uint8_t *>(v, 0x108)) hits[nh++] = Field<uint8_t *>(v, 0x108);
        int records = Field<uint8_t>(v, 0xE6);
        for (int r = 0; r < records && r < 6; r++) if (Field<uint8_t *>(v, 0xE8 + r * 4) != hits[0] || !nh) hits[nh++] = Field<uint8_t *>(v, 0xE8 + r * 4);
        // Un vehicule a l'arret (statique) ne calcule pas ses collisions et la copie, gelee, non plus : ils se
        // traversaient sans reaction. On cherche aussi les copies dont la boite chevauche la sienne.
        Box2 mb;
        bool haveBox = false;
        for (auto &e : g_vehs) {
            if (nh >= 7) break;
            if (!e.used || !e.veh || e.owner == g_localId || !e.haveState || Stale(e)) continue;
            float dx = Pos(e.veh).x - Pos(v).x, dy = Pos(e.veh).y - Pos(v).y;
            if (dx * dx + dy * dy > 10.0f * 10.0f) continue;
            if (!haveBox) { if (!BoxOf(v, mb)) break; haveBox = true; }
            Box2 ob;
            if (!BoxOf(e.veh, ob) || !Overlap(mb, ob)) continue;
            bool dup = false;
            for (int k = 0; k < nh; k++) dup |= hits[k] == (uint8_t *)e.veh;
            if (!dup) hits[nh++] = (uint8_t *)e.veh;
        }
        for (int r = 0; r < nh; r++) {
            uint8_t *o = hits[r];
            if (!o || (Field<uint8_t>(o, 0x50) & 7) != 2) continue;   // un vehicule
            NetVehicle *c = FindByPtr(o);
            if (!c || c->owner == g_localId || !c->haveState) continue;
            Vec3 a = Pos(v), b = Pos(o);
            float nx = a.x - b.x, ny = a.y - b.y, len = sqrtf(nx * nx + ny * ny);
            if (len < 0.01f) continue;
            nx /= len; ny /= len;
            float m1 = Field<float>(v, 0xB8), m2 = Field<float>(o, 0xB8);
            if (m1 <= 0.0f || m2 <= 0.0f) continue;
            float u1 = prev.x * nx + prev.y * ny;                                   // nous, avant le choc
            float u2 = c->peak[0] * nx + c->peak[1] * ny;                           // l'autre (vitesse recue d'avant le choc)
            float approach = u2 - u1;
            if (approach <= 0.002f) continue;                                       // on s'eloigne deja
            // Une fois par choc (les boites restent en contact plusieurs images).
            static struct { void *a, *b; uint32_t t; } recent[16];
            static int recentAt;
            bool again = false;
            for (auto &rc : recent) again |= rc.a == (void *)v && rc.b == (void *)o && GetTickCount() - rc.t < 250;
            if (again) continue;
            recent[recentAt++ % 16] = { v, o, GetTickCount() };
            float want = u1 + 1.2f * m2 / (m1 + m2) * approach;                     // choc peu elastique (e = 0,2)
            Vec3 &cur = MoveSpeed(v);
            float delta = want - (cur.x * nx + cur.y * ny);
            if (delta > 1.0f) delta = 1.0f;
            if (delta < -1.0f) delta = -1.0f;
            cur.x += nx * delta;
            cur.y += ny * delta;
            Field<uint8_t>(v, 0x51) &= ~0x04;   // bIsStatic : une voiture garee se reveille
            static uint32_t lastLog;
            if (GetTickCount() - lastLog > 2000) {
                lastLog = GetTickCount();
                Log("vehicules : choc avec la copie %08X du joueur %d (a %.2f), notre vitesse %.2f -> %.2f dans l'axe", c->id, c->owner, u2, u1, want);
            }
        }
    }
}

// --- Renverse par la voiture d'un autre joueur ---
// Pour notre Tommy a pied, le jeu decide la chute d'apres la vitesse de la voiture qui le touche (CPed::ProcessControl,
// reVC Ped.cpp) : la copie de la voiture d'un joueur a une vitesse nulle (sa position vient du reseau), on restait
// debout "comme un poteau". Au contact de sa boite, avec la vitesse recue d'avant le choc, on fait ce que fait le jeu :
// chute "gros impact" dans le bon sens (ANIM_STD_HIGHIMPACT_FRONT 25 + direction, CPed::SetFall 0x4FD9F0), degats
// (CPed::InflictDamage 0x525B20, arme 39 = percute par un vehicule, comptes si le tir ami est permis), projete.
static void RunOverByPlayers()
{
    void *me = FindPlayerPed();
    if (!me || InVehicle(me) || Health(me) <= 0.0f) return;
    static uint32_t lastHit;
    uint32_t now = GetTickCount();
    if (now - lastHit < 1500) return;
    Vec3 mp = Pos(me);
    for (auto &e : g_vehs) {
        if (!e.used || !e.veh || e.owner == g_localId || !e.haveState || Stale(e)) continue;
        float vx = e.peak[0], vy = e.peak[1], sp2 = vx * vx + vy * vy;
        if (sp2 < 0.1f * 0.1f) continue;   // moins de ~18 km/h : elle pousse seulement
        Box2 b;
        if (!BoxOf(e.veh, b) || fabsf(mp.z - Pos(e.veh).z) > 2.0f) continue;
        float dx = mp.x - b.cx, dy = mp.y - b.cy;
        float lx = dx * b.ax + dy * b.ay, ly = dx * b.bx + dy * b.by;
        if (fabsf(lx) > b.hx + 0.35f || fabsf(ly) > b.hy + 0.35f) continue;
        if (vx * dx + vy * dy <= 0.0f) continue;   // elle s'eloigne
        float kmh = sqrtf(sp2) * 50.0f * 3.6f;
        // Direction du coup par rapport a nous (CPed::GetLocalDirection du vecteur oppose a sa vitesse) : 0 avant,
        // 1 gauche, 2 arriere, 3 droite.
        float ang = atan2f(vx, -vy) - Heading(me) + 0.785398f;
        while (ang < 0.0f) ang += 6.283185f;
        int dir = (int)(ang / 1.570796f) & 3;
        ((void(__thiscall *)(void *, int, int, int))0x4FD9F0)(me, 1000, 25 + dir, 0);
        float dmg = kmh < 40.0f ? 10.0f : kmh < 80.0f ? 25.0f : 50.0f;
        ((bool(__thiscall *)(void *, void *, int, float, int, uint8_t))0x525B20)(me, e.veh, 39, dmg, 3, (uint8_t)dir);
        // Projete selon la vitesse : lent, bouscule ; vite, projete loin ; tres vite, par-dessus le capot (plus haut,
        // moins loin : la voiture passe dessous).
        if (kmh < 40.0f) MoveSpeed(me) = { vx * 0.6f, vy * 0.6f, 0.06f };
        else if (kmh < 70.0f) MoveSpeed(me) = { vx * 0.8f, vy * 0.8f, 0.12f };
        else MoveSpeed(me) = { vx * 0.45f, vy * 0.45f, 0.22f + (kmh - 70.0f) * 0.002f };
        lastHit = now;
        Log("vehicules : renverse par la voiture %08X du joueur %d a %.0f km/h (direction %d, sante %.0f)", e.id, e.owner, kmh, dir, Health(me));
        return;
    }
}

// --- Sons du vehicule pour son passager ---
// Pour le jeu, le vehicule ou l'on est assis est "le vehicule du joueur" : son moteur est joue d'apres NOTRE manette
// (Pads[0].GetAccelerate / GetBrake, reVC ProcessPlayersVehicleEngine), et le passager n'accelere pas : moteur au
// ralenti pendant que l'autre joueur roule a fond. Dans le code audio seulement (appels 0x5EE3A5..0x5F3E72 ; ceux
// de 0x60A7xx pilotent la voiture et ne sont pas touches), en passager on rend les pedales du conducteur, que la
// copie recoit (+0x1F0 accelerateur, +0x1F4 frein), comme le fait le jeu pendant un replay.
static void *PassengerVehicle()
{
    void *me = FindPlayerPed();
    if (!me || !InVehicle(me)) return NULL;
    void *v = PedVehicle(me);
    return v && VehDriver(v) != me ? v : NULL;
}
static int16_t Pedal(void *v, int off) { float g = Field<float>(v, off); return (int16_t)((g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g) * 255.0f); }
static int16_t __fastcall h_AudioAccelerate(void *pad, void *)
{
    void *v = PassengerVehicle();
    if (v && pad == (void *)0x7DBCB0) {
        int16_t r = Pedal(v, 0x1F0);
        static bool logged;
        if (!logged && r > 0) { logged = true; Log("vehicules : passager, son du moteur a l'accelerateur du conducteur (%d/255)", r); }
        return r;
    }
    return ((int16_t(__thiscall *)(void *))0x4AA760)(pad);
}
static int16_t __fastcall h_AudioBrake(void *pad, void *)
{
    void *v = PassengerVehicle();
    if (v && pad == (void *)0x7DBCB0) return Pedal(v, 0x1F4);
    return ((int16_t(__thiscall *)(void *))0x4AA960)(pad);
}
void InstallVehicleAudio()
{
    static const uintptr_t accel[] = { 0x5EE3B1, 0x5EE3C0, 0x5F1107, 0x5F33D6, 0x5F37E3, 0x5F3E64 };
    static const uintptr_t brake[] = { 0x5EE3A5, 0x5EE3CC, 0x5F1115, 0x5F33E2, 0x5F37F0, 0x5F3E72 };
    int n = 0;
    auto hook = [&](uintptr_t at, uintptr_t fn, void *to) {
        if (*(uint8_t *)at != 0xE8 || (uintptr_t)(at + 5 + *(int32_t *)(at + 1)) != fn) return;
        PatchCall(at, to);
        n++;
    };
    for (uintptr_t a : accel) hook(a, 0x4AA760, (void *)h_AudioAccelerate);
    for (uintptr_t a : brake) hook(a, 0x4AA960, (void *)h_AudioBrake);
    Log("vehicules : sons du passager d'apres les pedales du conducteur (%d appels sur 12)", n);
}

// Notre voiture percute le Tommy d'un autre joueur (a pied) : il tombe chez lui ; ici, son double devient traversable
// un moment (comme un passant couche), et la voiture continue sa route au lieu de buter dessus. Recherche un peu en
// avant de la voiture (la collision du jeu arrive avant nous dans l'image).
static void MyCarHitsPlayers()
{
    void *me = FindPlayerPed();
    if (!me || !InVehicle(me)) return;
    void *v = PedVehicle(me);
    if (!v || VehDriver(v) != me) return;
    Vec3 mv = MoveSpeed(v);
    float sp = sqrtf(mv.x * mv.x + mv.y * mv.y);
    if (sp < 0.1f) return;   // moins de ~18 km/h : on pousse seulement
    Box2 b;
    if (!BoxOf(v, b)) return;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        void *p = PuppetPed(i);
        if (!p || InVehicle(p)) continue;
        Vec3 pp = Pos(p);
        if (fabsf(pp.z - Pos(v).z) > 2.0f) continue;
        float dx = pp.x - b.cx - mv.x * 3.0f, dy = pp.y - b.cy - mv.y * 3.0f;   // 3 images en avance
        float lx = dx * b.ax + dy * b.ay, ly = dx * b.bx + dy * b.by;
        if (fabsf(lx) > b.hx + 0.6f || fabsf(ly) > b.hy + 0.6f + sp * 3.0f) continue;
        static uint32_t last[MAX_PLAYERS];
        if (GetTickCount() - last[i] > 1500) {
            last[i] = GetTickCount();
            Log("vehicules : ma voiture percute le joueur %d a %.0f km/h, il devient traversable", i, sp * 180.0f);
        }
        PuppetPassThrough(i, 1500);
    }
}

// --- Sons des copies ---
// Le moteur audio (reVC AudioLogic.cpp, ProcessVehicleEngine / Skidding / SirenOrAlarm) lit sur chaque vehicule : roues
// au sol (CAutomobile +0x5C4..0x5C6, CBike +0x4DC..0x4DE), rapport engage (CVehicle +0x208), etat des pneus (patinent,
// glissent, bloques : CAutomobile +0x5CC, CBike +0x4E4) et, pour la sirene, un statut autre que "abandonne". Une copie
// est figee et "abandonnee" (sa physique est chez son proprietaire) : moteur hurlant roues en l'air au premier rapport,
// jamais de crissement, sirene muette. Juste avant le traitement audio, on les deduit de la vitesse et des pedales
// recues ; le statut est remis juste apres.
static void *g_audioStatus[64];
static int g_audioStatusN;

void VehiclesBeforeAudio()
{
    g_audioStatusN = 0;
    for (auto &e : g_vehs) {
        if (!e.used || !e.veh || e.owner == g_localId || !e.haveState) continue;
        void *v = e.veh;
        int cls = VehClass(v);
        if ((cls != VCLASS_CAR && cls != VCLASS_BIKE) || EntityStatus(v) == STATUS_WRECKED) continue;
        Vec3 mv = MoveSpeed(v), f = Field<Vec3>(v, 0x14), r = Field<Vec3>(v, 0x04);
        float fwd = mv.x * f.x + mv.y * f.y + mv.z * f.z, side = fabsf(mv.x * r.x + mv.y * r.y + mv.z * r.z);
        float sp = sqrtf(mv.x * mv.x + mv.y * mv.y);
        bool ground = fabsf(mv.z) < 0.08f;
        int gear = fwd < -0.01f ? 0 : 1 + (int)(sp * 180.0f / 40.0f);   // ~40 km/h par rapport
        Field<uint8_t>(v, 0x208) = (uint8_t)(gear > 5 ? 5 : gear);
        float gas = Field<float>(v, 0x1F0), brake = Field<float>(v, 0x1F4);
        int ws = (side > 0.08f && sp > 0.1f) ? 2                        // glisse (derapage)
               : (brake > 0.8f && sp > 0.15f) ? 3                       // roues bloquees (freinage)
               : (gas > 0.9f && sp < 0.05f && ground) ? 1 : 0;          // patinent (demarrage)
        if (!ground) ws = 0;
        if (cls == VCLASS_CAR) {
            uint8_t n = ground ? 4 : 0;
            Field<uint8_t>(v, 0x5C4) = n; Field<uint8_t>(v, 0x5C5) = n; Field<uint8_t>(v, 0x5C6) = n;
            for (int i = 0; i < 4; i++) Field<int>(v, 0x5CC + i * 4) = ws;
        } else {
            uint8_t n = ground ? 2 : 0;
            Field<uint8_t>(v, 0x4DC) = n; Field<uint8_t>(v, 0x4DD) = n; Field<uint8_t>(v, 0x4DE) = n;
            for (int i = 0; i < 2; i++) Field<int>(v, 0x4E4 + i * 4) = ws;
        }
        if (EntityStatus(v) == STATUS_ABANDONED && VehDriver(v) && g_audioStatusN < 64) {
            SetEntityStatus(v, STATUS_PHYSICS);
            g_audioStatus[g_audioStatusN++] = v;
        }
    }
}

void VehiclesAfterAudio()
{
    for (int i = 0; i < g_audioStatusN; i++)
        if (EntityStatus(g_audioStatus[i]) == STATUS_PHYSICS) SetEntityStatus(g_audioStatus[i], STATUS_ABANDONED);
    g_audioStatusN = 0;
}

void VehiclesFrame(bool inGame)
{
    if (inGame) { CopyCollisions(); RunOverByPlayers(); MyCarHitsPlayers(); }
    static bool wasConnected[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        bool c = i != g_localId && g_players[i].connected;
        if (wasConnected[i] && !c && inGame) PlayerLeft(i);
        wasConnected[i] = c;
    }
    if (!inGame) {
        // Hors partie (chargement, menu) : le monde est detruit par le jeu, on oublie tout ; les autres apprennent
        // tout de suite que nos vehicules n'existent plus (sinon leurs copies restaient figees).
        for (auto &e : g_vehs) {
            if (!e.used) continue;
            if (e.owner == g_localId && g_localId >= 0) { MsgVehRemove r = { MSG_VEH_REMOVE, e.id }; NetSendToAll(&r, sizeof(r)); }
            e.veh = NULL; e.used = false;
        }
        return;
    }
    uint32_t now = GetTickCount();
    void *ped = FindPlayerPed();
    void *myVeh = InVehicle(ped) ? PedVehicle(ped) : NULL;
    if (myVeh && VehDriver(myVeh) == ped && VehClass(myVeh) != VCLASS_TRAIN) {
        NetVehicle *e = FindByPtr(myVeh);
        // Deux joueurs au volant de la meme voiture en meme temps (chacun est monte dans sa copie avant de voir
        // l'autre) : sans regle, chacun reprenait la voiture 30 fois par seconde et elle tremblait. Le plus petit
        // numero de joueur la garde, l'autre descend.
        if (e && e->owner != g_localId && e->haveState && e->state.driver == e->owner && e->owner < g_localId && now - e->lastRecv < 1000) {
            Log("vehicules : %08X, le joueur %d etait deja au volant : je descends", e->id, e->owner);
            WarpOutOfVehicle(ped, NULL);
            e = NULL;
            myVeh = NULL;
        } else if (!e) {
            e = AllocPriority(((uint32_t)g_localId << 24) | (++g_vehCounter & 0xFFFFFF), myVeh);
            if (e) {
                e->owner = (uint8_t)g_localId;
                Bind(*e, myVeh);
                Field<uint8_t>(myVeh, 0x53) &= ~0x08;   // ancienne copie gardee (proprietaire parti au menu) : s'abime a nouveau
                SetCopyFlags(myVeh, false);
                Log("vehicules : je prends %08X (modele %d, couleurs %d/%d)", e->id, ModelIndex(myVeh),
                    Field<uint8_t>(myVeh, 0x1A0), Field<uint8_t>(myVeh, 0x1A1));
            }
        } else if (e->owner != g_localId) {
            e->owner = (uint8_t)g_localId;
            Field<uint8_t>(myVeh, 0x53) &= ~0x08;   // c'etait une copie : elle peut de nouveau s'abimer
            SetCopyFlags(myVeh, false);
            // Verrou recopie du proprietaire (voiture volee a la circulation, verrouillee chez lui) : chez nous on ne
            // pouvait plus y monter au volant, ni en descendre. Deverrouillee, sauf vehicule de mission (cree par script).
            int &lock = Field<int>(myVeh, 0x230);
            if (Field<uint8_t>(myVeh, 0x1F8) != 2 && lock != 0 && lock != 1) { Log("vehicules : %08X deverrouillee (verrou %d recu)", e->id, lock); lock = 1; }
            Log("vehicules : je reprends %08X", e->id);
        }
        // Voiture de la circulation partagee prise par l'hote : elle n'est plus "ambiante" (un invite a plus de
        // 210 m la supprimait, et l'hote disparaissait avec).
        if (e && e->ambient) { e->ambient = false; Log("vehicules : %08X n'est plus une voiture de circulation (joueur au volant)", e->id); }
        if (e && now - e->lastSend >= 33) SendVehicle(*e);
    }

    // En train de monter (animation en cours) : le vehicule recoit tout de suite son identifiant, pour que les autres
    // voient la montee sur notre double.
    if (g_cfg.logScripts && !myVeh && PedVehicle(ped) && !InVehicle(ped) && EnteringState(PedState(ped))) {
        static uint32_t lastDbg;
        if (now - lastDbg > 500) {
            lastDbg = now;
            NetVehicle *f = FindByPtr(PedVehicle(ped));
            Log("vehicules : montee dans %p (modele %d) -> entree %08X (modele %d, veh %p, perime %d)", PedVehicle(ped), ModelIndex(PedVehicle(ped)),
                f ? f->id : 0, f ? f->model : 0, f ? f->veh : NULL, f ? Stale(*f) : 0);
        }
    }
    if (!myVeh && PedVehicle(ped) && !InVehicle(ped) && EnteringState(PedState(ped)) && VehClass(PedVehicle(ped)) != VCLASS_TRAIN && !FindByPtr(PedVehicle(ped))) {
        NetVehicle *e = AllocPriority(((uint32_t)g_localId << 24) | (++g_vehCounter & 0xFFFFFF), PedVehicle(ped));
        if (e) {
            e->owner = (uint8_t)g_localId;
            Bind(*e, PedVehicle(ped));
            SendVehicle(*e);
            Log("vehicules : je monte dans %08X (modele %d)", e->id, ModelIndex(PedVehicle(ped)));
        }
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

    // Invite : sa circulation et ses voitures garees pres d'un autre joueur partent chez les autres (il peuple son coin
    // du monde) ; ses vehicules de police seulement s'il a une police a lui.
    if (!g_cfg.host && g_localId > 0) {
        static uint32_t lastLawScan;
        if (now - lastLawScan > 250) {
            lastLawScan = now;
            Pool *pool = VehiclePool();
            for (int i = 0; i < pool->size; i++) {
                if (pool->flags[i] & 0x80) continue;
                void *v = pool->objects + i * VEHICLE_POOL_ENTRY;
                uint8_t by = Field<uint8_t>(v, 0x1F8);
                if ((by != 1 && by != 3) || FindByPtr(v) || (IsLawVehicle(v) && !LocalWanted())) continue;
                if (VehClass(v) == VCLASS_TRAIN || !NearOtherPlayer(&Pos(v).x, AreaCode(v), (float)AMBIENT_SHARE_M)) continue;
                NetVehicle *e = Alloc(((uint32_t)g_localId << 24) | (++g_vehCounter & 0xFFFFFF));
                if (!e) break;
                e->owner = (uint8_t)g_localId;
                e->ambient = true;
                Bind(*e, v);
            }
        }
    }

    for (auto &e : g_vehs) {
        if (!e.used) continue;
        // Plus d'etat depuis 10 s (le proprietaire l'a oublie, ou son message de retrait s'est perdu) : on la retire.
        if (e.owner != g_localId && e.haveState && now - e.lastRecv > 10000) { DeleteCopy(e); e.used = false; continue; }
        if (e.owner == g_localId) {
            if (Stale(e)) { Log("vehicules : %08X perime (case reutilisee), retire", e.id); e.veh = NULL; }
            if (!e.veh) {   // detruit chez nous : on previent les autres
                MsgVehRemove r = { MSG_VEH_REMOVE, e.id };
                NetSendToAll(&r, sizeof(r));
                e.used = false;
                continue;
            }
            // Circulation partagee : plus aucun invite assez pres -> on la retire chez eux (on la garde ici).
            bool keepShared = NearOtherPlayer(&Pos(e.veh).x, AreaCode(e.veh), AMBIENT_SHARE_M + 40.0f);
            if (e.ambient && e.veh != myVeh && !keepShared) {
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
                // Copie reprise puis abandonnee : vide, immobile depuis une minute et loin de tout le monde, on la
                // rend (sinon chaque voiture empruntee restait pour toujours et la table se remplissait).
                if (moving || !e.ours) e.idleSince = 0;
                else if (!e.idleSince) e.idleSince = now;
                else if (now - e.idleSince > 60000 && !AnyPlayerNear(Pos(e.veh), 150.0f)) {
                    MsgVehRemove r = { MSG_VEH_REMOVE, e.id };
                    NetSendToAll(&r, sizeof(r));
                    DeleteCopy(e);
                    e.used = false;
                    Log("vehicules : copie %08X abandonnee, rendue", e.id);
                    continue;
                }
                if (now - e.lastSend >= (moving ? 50u : 1000u)) SendVehicle(e);
            }
            continue;
        }
        if (!e.haveState) continue;
        if (!e.veh) {
            void *v = CreateCopy(e.state);
            if (v) {
                Bind(e, v);
                e.ours = true;
                // Deja une epave chez son proprietaire : une carcasse a froid (statut epave, sante 0), sans la
                // boule de feu de EXPLODE_CAR (elle re-explosait a chaque retour dans la zone partagee).
                if (e.state.wrecked) { e.blown = true; VehHealth(v) = 0.0f; SetEntityStatus(v, STATUS_WRECKED); }
            }
        }
        if (e.veh) ApplyState(e);
    }
}
