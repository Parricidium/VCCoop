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
#include <math.h>
#include <string.h>

using namespace game;

enum { RL_WORLD_OBJECT = 19, OBJECT_POOL_ENTRY = 0x1A0 };
#pragma pack(push, 1)
struct RlObject { uint8_t type, from; int16_t model; float pos[3], vel[3], turn[3]; };
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

void ObjSyncOnReliable(int from, const uint8_t *data, int len)
{
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
                RlObject m = { RL_WORLD_OBJECT, (uint8_t)g_localId, t.model, { t.pos.x, t.pos.y, t.pos.z }, { v.x, v.y, v.z }, { w.x, w.y, w.z } };
                NetSendReliable(&m, sizeof(m));
                static uint32_t lastLog;
                if (GetTickCount() - lastLog > 2000) { lastLog = GetTickCount(); Log("decor : objet %d renverse ici, envoye aux autres", t.model); }
            }
        }
        t.wasStatic = st;
    }
}
