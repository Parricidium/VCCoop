// Decor renverse partage : poteau, lampadaire, panneau, borne, poubelle... renverse par la voiture d'un joueur.
// Seule la machine ou la voiture est vraiment physique le renverse (chez les autres c'est une copie gelee, qui ne
// touche rien) : ils entendaient le choc sans voir le poteau tomber. Chaque machine repere ses objets du decor qui
// passent de "statique" a "mobile" (CEntity::bIsStatic, +0x51 bit 0x04, retire par la collision du jeu, reVC
// CPhysical::ApplyCollision / 1.0 FUN_004B6600) et envoie le modele, la position d'origine et les vitesses ; les
// autres renversent le meme objet de la meme facon (statique retire, vitesses, CPhysical::AddToMovingList 0x4BAE90
// si l'objet n'est pas encore dans la liste des objets en mouvement, +0xE0). L'hote fait suivre aux autres invites.
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "vehicles.h"
#include <math.h>
#include <string.h>

using namespace game;

enum { RL_WORLD_OBJECT = 19, OBJECT_POOL_ENTRY = 0x1A0 };
enum { OBJ_TOPPLE = 0, OBJ_DAMAGE = 1, OBJ_GLASS = 2, OBJ_CRASH = 3 };
#pragma pack(push, 1)
struct RlObject { uint8_t type, from, kind; int16_t model; float pos[3], vel[3], turn[3]; };
// Casse : l'effet du jeu rejoue chez les autres avec les memes arguments (voir plus bas).
struct RlCrash { uint8_t type, from, kind, s1, s2, pad[3]; uint32_t veh; float power, speed; };
struct RlSmash { uint8_t type, from, kind, flag; int16_t model; float pos[3], amount, speed[3], point[3]; };
#pragma pack(pop)

static Pool *ObjectPool() { return *(Pool **)0x94DBE0; }   // CPools::ms_pObjectPool (460 objets)

struct ObjTrack { uint32_t handle; int16_t model; Vec3 pos; bool wasStatic, done; };
static ObjTrack g_track[512];

static bool IsStatic(void *o) { return (Field<uint8_t>(o, 0x51) & 0x04) != 0; }

// Renverse chez nous l'objet recu (le plus proche du meme modele, encore debout, a moins de 1 m de sa position).
static bool Topple(const RlObject &m)
{
    Pool *p = ObjectPool();
    if (!p) return false;
    int best = -1;
    float bestD = 1.0f;
    for (int i = 0; i < p->size && i < 512; i++) {
        if (p->flags[i] & 0x80) continue;
        void *o = p->objects + i * OBJECT_POOL_ENTRY;
        if (ModelIndex(o) != m.model || !IsStatic(o)) continue;
        float dx = Pos(o).x - m.pos[0], dy = Pos(o).y - m.pos[1], dz = Pos(o).z - m.pos[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return false;
    void *o = p->objects + best * OBJECT_POOL_ENTRY;
    g_track[best].done = true;   // pas renvoye aux autres
    Field<uint8_t>(o, 0x51) &= ~0x04;
    MoveSpeed(o) = { m.vel[0], m.vel[1], m.vel[2] };
    TurnSpeed(o) = { m.turn[0], m.turn[1], m.turn[2] };
    if (!Field<void *>(o, 0xE0)) ((void(__fastcall *)(void *))0x4BAE90)(o);
    return true;
}

static void OnSmash(int from, const uint8_t *data, int len);

void ObjSyncOnReliable(int from, const uint8_t *data, int len)
{
    if (len >= 3 && data[2] != OBJ_TOPPLE) { OnSmash(from, data, len); return; }
    if (len < (int)sizeof(RlObject)) return;
    RlObject m = *(const RlObject *)data;
    if (g_cfg.host) {   // un invite a renverse un objet : aux autres invites aussi
        for (int i = 1; i < MAX_PLAYERS; i++)
            if (i != from && i != m.from && g_players[i].connected) NetSendReliableTo(i, &m, sizeof(m));
    }
    if (GameState() != GS_PLAYING) return;
    bool ok = Topple(m);
    if (g_cfg.logScripts) Log("decor : objet %d renverse par le joueur %d %s", m.model, m.from, ok ? "renverse ici aussi" : "introuvable ici");
}

void ObjSyncFrame(bool inGame)
{
    Pool *p = ObjectPool();
    if (!inGame || !p) { memset(g_track, 0, sizeof(g_track)); return; }
    for (int i = 0; i < p->size && i < 512; i++) {
        ObjTrack &t = g_track[i];
        if (p->flags[i] & 0x80) { t.handle = 0; continue; }
        void *o = p->objects + i * OBJECT_POOL_ENTRY;
        uint32_t h = (uint32_t)(i << 8) | p->flags[i];
        if (t.handle != h || t.model != ModelIndex(o)) {   // nouvel objet dans cette case
            t = { h, ModelIndex(o), Pos(o), IsStatic(o), false };
            continue;
        }
        bool st = IsStatic(o);
        if (t.wasStatic && !st && !t.done) {
            // Renverse ici, et il bouge vraiment (pas un objet que le jeu libere sans le deplacer).
            Vec3 v = MoveSpeed(o);
            float dx = Pos(o).x - t.pos.x, dy = Pos(o).y - t.pos.y, dz = Pos(o).z - t.pos.z;
            if (v.x * v.x + v.y * v.y + v.z * v.z > 0.0001f || dx * dx + dy * dy + dz * dz > 0.01f) {
                t.done = true;
                Vec3 w = TurnSpeed(o);
                RlObject m = { RL_WORLD_OBJECT, (uint8_t)g_localId, OBJ_TOPPLE, t.model, { t.pos.x, t.pos.y, t.pos.z }, { v.x, v.y, v.z }, { w.x, w.y, w.z } };
                NetSendReliable(&m, sizeof(m));
                static uint32_t lastLog;
                if (GetTickCount() - lastLog > 2000) { lastLog = GetTickCount(); Log("decor : objet %d renverse ici, envoye aux autres", t.model); }
            }
        }
        t.wasStatic = st;
    }
}

// --- Decor qui vole en eclats ---
// Cartons, poubelles, feux tricolores, barrieres... : CObject::ObjectDamage (0x4E0990, thiscall, montant du choc) joue
// l'effet choisi par le modele (cache + debris, change de modele, etc., reVC Object.cpp). Vitres : CGlass::
// WindowRespondsToCollision (0x553C10 : objet, montant, vitesse, point d'impact, explosion) les brise en eclats.
// Seule la machine ou le choc est physique le voyait (la voiture d'un autre joueur est une copie gelee chez nous, ses
// balles aussi). Crochet sur les deux : si l'etat de l'objet a change pendant l'appel (drapeaux de CEntity +0x50..0x57,
// de CObject +0x16C..0x16F), on envoie modele, position et arguments ; les autres rejouent le meme appel sur leur objet.
static bool g_remoteSmash;   // en train de rejouer une casse recue (pas renvoyee)

struct ObjFlags { uint8_t e[8], o[4]; };
static void TakeFlags(void *o, ObjFlags &f) { memcpy(f.e, (uint8_t *)o + 0x50, 8); memcpy(f.o, (uint8_t *)o + 0x16C, 4); }
static bool InObjectPool(void *o)
{
    Pool *p = ObjectPool();
    if (!p || (uint8_t *)o < p->objects) return false;
    int i = (int)((uint8_t *)o - p->objects) / OBJECT_POOL_ENTRY;
    return i < p->size && !(p->flags[i] & 0x80) && p->objects + i * OBJECT_POOL_ENTRY == (uint8_t *)o;
}

static void SendSmash(void *o, int kind, float amount, const float *speed, const float *point, uint8_t flag)
{
    if (g_remoteSmash || GameState() != GS_PLAYING || g_localId < 0 || !InObjectPool(o)) return;
    RlSmash m = { RL_WORLD_OBJECT, (uint8_t)g_localId, (uint8_t)kind, flag, ModelIndex(o), { Pos(o).x, Pos(o).y, Pos(o).z }, amount,
                  { speed ? speed[0] : 0, speed ? speed[1] : 0, speed ? speed[2] : 0 }, { point ? point[0] : 0, point ? point[1] : 0, point ? point[2] : 0 } };
    NetSendReliable(&m, sizeof(m));
    static uint32_t lastLog;
    if (GetTickCount() - lastLog > 2000) { lastLog = GetTickCount(); Log("decor : objet %d %s ici, envoye aux autres", m.model, kind == OBJ_GLASS ? "vitre brisee" : "casse"); }
}

typedef void(__fastcall *ObjectDamage_t)(void *obj, void *edx, float amount);
static ObjectDamage_t o_ObjectDamage;
static void __fastcall h_ObjectDamage(void *obj, void *edx, float amount)
{
    ObjFlags a, b;
    TakeFlags(obj, a);
    o_ObjectDamage(obj, edx, amount);
    TakeFlags(obj, b);
    if (memcmp(&a, &b, sizeof(a))) SendSmash(obj, OBJ_DAMAGE, amount, NULL, NULL, 0);
}

typedef void(__cdecl *WindowHit_t)(void *obj, float amount, float sx, float sy, float sz, float px, float py, float pz, uint8_t explosion);
static WindowHit_t o_WindowHit;
static void __cdecl h_WindowHit(void *obj, float amount, float sx, float sy, float sz, float px, float py, float pz, uint8_t explosion)
{
    ObjFlags a, b;
    TakeFlags(obj, a);
    o_WindowHit(obj, amount, sx, sy, sz, px, py, pz, explosion);
    TakeFlags(obj, b);
    float sp[3] = { sx, sy, sz }, pt[3] = { px, py, pz };
    if (memcmp(&a, &b, sizeof(a))) SendSmash(obj, OBJ_GLASS, amount, sp, pt, explosion);
}

static void OnCrash(int from, const uint8_t *data, int len);
static void OnSmash(int from, const uint8_t *data, int len)
{
    if (len >= 3 && data[2] == OBJ_CRASH) { OnCrash(from, data, len); return; }
    if (len < (int)sizeof(RlSmash)) return;
    RlSmash m = *(const RlSmash *)data;
    if (g_cfg.host)   // un invite a casse un objet : aux autres invites aussi
        for (int i = 1; i < MAX_PLAYERS; i++)
            if (i != from && i != m.from && g_players[i].connected) NetSendReliableTo(i, &m, sizeof(m));
    if (GameState() != GS_PLAYING || m.from == g_localId) return;
    // Notre objet : meme modele, le plus proche a moins de 2 m (un objet deja bouscule a pu glisser un peu).
    Pool *p = ObjectPool();
    if (!p) return;
    void *best = NULL;
    float bestD = 2.0f * 2.0f;
    for (int i = 0; i < p->size; i++) {
        if (p->flags[i] & 0x80) continue;
        void *o = p->objects + i * OBJECT_POOL_ENTRY;
        if (ModelIndex(o) != m.model) continue;
        float dx = Pos(o).x - m.pos[0], dy = Pos(o).y - m.pos[1], dz = Pos(o).z - m.pos[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) { bestD = d; best = o; }
    }
    if (!best || !o_WindowHit || !o_ObjectDamage) { if (g_cfg.logScripts) Log("decor : objet %d casse par le joueur %d introuvable ici", m.model, m.from); return; }
    g_remoteSmash = true;
    if (m.kind == OBJ_GLASS) o_WindowHit(best, m.amount, m.speed[0], m.speed[1], m.speed[2], m.point[0], m.point[1], m.point[2], m.flag);
    else o_ObjectDamage(best, NULL, m.amount);
    g_remoteSmash = false;
    if (g_cfg.logScripts) Log("decor : objet %d casse par le joueur %d, casse ici aussi", m.model, m.from);
}

// --- Bruits de choc des vehicules ---
// Un choc (contre un mur, un poteau, une voiture) n'est calcule que chez le proprietaire du vehicule : les autres
// n'entendaient rien. cDMAudio::ReportCollision (0x5F99A0 : entite 1, entite 2, surfaces, force, vitesse) : pour un
// vehicule a nous en reseau, on envoie surfaces et force (fort seulement, 4 par seconde au plus) ; les autres
// rejouent le choc sur leur copie (entite 1 = entite 2 = la copie : le son part de sa position).
typedef void(__fastcall *ReportCollision_t)(void *dm, void *edx, void *e1, void *e2, uint8_t s1, uint8_t s2, float power, float speed);
static ReportCollision_t o_ReportCollision;
static bool g_remoteCrash;

static void __fastcall h_ReportCollision(void *dm, void *edx, void *e1, void *e2, uint8_t s1, uint8_t s2, float power, float speed)
{
    o_ReportCollision(dm, edx, e1, e2, s1, s2, power, speed);
    if (g_remoteCrash || g_localId < 0 || power < 50.0f) return;
    void *v = NULL;
    uint8_t sa = s1, sb = s2;
    if (e1 && (Field<uint8_t>(e1, 0x50) & 7) == 2) v = e1;                          // 2 : vehicule
    else if (e2 && (Field<uint8_t>(e2, 0x50) & 7) == 2) { v = e2; sa = s2; sb = s1; }
    uint32_t id = v ? NetVehicleId(v) : 0;
    if (!id || NetVehicleIsCopy(v)) return;
    static uint32_t last;
    uint32_t now = GetTickCount();
    if (now - last < 250) return;
    last = now;
    RlCrash m = { RL_WORLD_OBJECT, (uint8_t)g_localId, OBJ_CRASH, sa, sb, { 0, 0, 0 }, id, power, speed };
    NetSendReliable(&m, sizeof(m));
}

static void OnCrash(int from, const uint8_t *data, int len)
{
    if (len < (int)sizeof(RlCrash)) return;
    RlCrash m = *(const RlCrash *)data;
    if (g_cfg.host)
        for (int i = 1; i < MAX_PLAYERS; i++)
            if (i != from && i != m.from && g_players[i].connected) NetSendReliableTo(i, &m, sizeof(m));
    if (GameState() != GS_PLAYING || m.from == g_localId || !o_ReportCollision) return;
    void *v = NetVehicleById(m.veh);
    if (!v) return;
    g_remoteCrash = true;
    o_ReportCollision((void *)0xA10B8A, NULL, v, v, m.s1, m.s2, m.power, m.speed);
    g_remoteCrash = false;
    static uint32_t lastLog;
    if (g_cfg.logScripts && GetTickCount() - lastLog > 2000) { lastLog = GetTickCount(); Log("decor : choc du vehicule %08X du joueur %d (force %.0f)", m.veh, m.from, m.power); }
}

void InstallObjSync()
{
    static const uint8_t colPro[] = { 0x83, 0xEC, 0x08, 0x89, 0x4C, 0x24, 0x04, 0x8B, 0x44, 0x24, 0x14 };
    o_ReportCollision = (ReportCollision_t)MakeDetour(0x5F99A0, colPro, sizeof(colPro), (void *)h_ReportCollision);
    static const uint8_t dmgPro[] = { 0x89, 0xC8, 0x53, 0x56, 0x57, 0x55, 0x81, 0xEC, 0x28, 0x02, 0x00, 0x00 };
    o_ObjectDamage = (ObjectDamage_t)MakeDetour(0x4E0990, dmgPro, sizeof(dmgPro), (void *)h_ObjectDamage);
    static const uint8_t glassPro[] = { 0x53, 0x56, 0x57, 0x55, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00 };
    o_WindowHit = (WindowHit_t)MakeDetour(0x553C10, glassPro, sizeof(glassPro), (void *)h_WindowHit);
}
