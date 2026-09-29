// Corps mous (ragdoll) : un personnage qui meurt, ou qu'un vehicule percute, s'effondre et roule comme un corps
// mou au lieu de jouer son animation de chute.
//
// Squelette : la hierarchie d'os du personnage (RpHAnimHierarchy, GetAnimHierarchyFromSkinClump 0x57F250) garde une
// matrice MONDE par os, recalculee par CPed::PreRender (CEntity::UpdateRpHAnim) puis lue par le rendu. Apres
// PreRender (crochet sur l'entree 12 de la table virtuelle de la classe du personnage), on y ecrit nos matrices.
//
// Simulation (Verlet, 2 sous-pas) : 17 points (bassin, torse, cou, tete + sommet du crane, epaules, coudes,
// mains, hanches, genoux, pieds) a l'origine des os de depart, relies par des longueurs fixes (os, ceinture
// scapulaire et bassin rigides) et des distances minimales (coudes, genoux, tete) ; gravite ; collisions contre
// batiments et objets (CWorld::ProcessLineOfSight 0x4D92D0 du point precedent au nouveau : point et normale
// d'impact) ; les vehicules repoussent les points qui entrent dans leur boite et leur donnent leur vitesse.
// Os pilotes : repere (direction de l'os vers l'enfant, reference laterale) de depart et courant, rotation = courant
// x depart^T appliquee a la matrice de depart ; les autres os suivent leur parent rigidement.
//
// Coop : le proprietaire du personnage (l'hote pour ses passants et personnages de mission, un invite pour ceux
// de son coin du monde) simule et envoie la pose (MSG_RAGDOLL, 15 fois par seconde, puis la pose finale) ; chez
// les autres, la copie (entities.cpp) affiche cette pose, sans simuler.
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "entities.h"
#include "ragdoll.h"
#include <math.h>
#include <string.h>

using namespace game;

namespace {

enum { NP = 17, MAX_RAG = 24, MAX_ACTIVE = 8, MAX_NODES = 64, ND = 12 };
enum { P_PELVIS, P_CHEST, P_NECK, P_HEAD, P_HEADTOP, P_RUA, P_RFA, P_RH, P_LUA, P_LFA, P_LH, P_RTH, P_RCA, P_RFO, P_LTH, P_LCA, P_LFO };
const int kTag[NP] = { 1, 3, 4, 5, -1, 22, 23, 24, 32, 33, 34, 51, 52, 53, 41, 42, 43 };   // BoneTag (reVC Bones.h)

struct V3 { float x, y, z; };
inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(V3 a, float k) { return { a.x * k, a.y * k, a.z * k }; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Len(V3 a) { return sqrtf(Dot(a, a)); }
inline V3 Norm(V3 a) { float l = Len(a); return l > 1e-6f ? a * (1.0f / l) : V3{ 0, 0, 1 }; }
inline bool Finite(V3 a) { return a.x == a.x && a.y == a.y && a.z == a.z && fabsf(a.x) < 1e6f && fabsf(a.y) < 1e6f && fabsf(a.z) < 1e6f; }

// Liens rigides (os et entretoises du tronc) et distances minimales (articulations qui ne se replient pas a fond).
const uint8_t kLinks[][2] = {
    { P_PELVIS, P_CHEST }, { P_CHEST, P_NECK }, { P_NECK, P_HEAD }, { P_HEAD, P_HEADTOP }, { P_NECK, P_HEADTOP },
    { P_CHEST, P_RUA }, { P_CHEST, P_LUA }, { P_NECK, P_RUA }, { P_NECK, P_LUA }, { P_RUA, P_LUA },
    { P_PELVIS, P_RUA }, { P_PELVIS, P_LUA }, { P_PELVIS, P_NECK },
    { P_RUA, P_RFA }, { P_RFA, P_RH }, { P_LUA, P_LFA }, { P_LFA, P_LH },
    { P_PELVIS, P_RTH }, { P_PELVIS, P_LTH }, { P_RTH, P_LTH }, { P_CHEST, P_RTH }, { P_CHEST, P_LTH },
    { P_RTH, P_RCA }, { P_RCA, P_RFO }, { P_LTH, P_LCA }, { P_LCA, P_LFO },
};
enum { NL = sizeof(kLinks) / sizeof(kLinks[0]) };
struct MinLink { uint8_t a, mid, b; };
const MinLink kMins[] = {
    { P_RUA, P_RFA, P_RH }, { P_LUA, P_LFA, P_LH }, { P_RTH, P_RCA, P_RFO }, { P_LTH, P_LCA, P_LFO },
};
enum { NM = sizeof(kMins) / sizeof(kMins[0]) };
// Tonus : ressorts doux vers les distances de depart entre articulations non voisines (le corps garde une forme au
// lieu de se replier comme un chiffon) ; raideur par iteration.
const uint8_t kSoft[][2] = {
    { P_RUA, P_RH }, { P_LUA, P_LH }, { P_RTH, P_RFO }, { P_LTH, P_LFO }, { P_PELVIS, P_RFO }, { P_PELVIS, P_LFO },
    { P_PELVIS, P_HEAD }, { P_CHEST, P_HEADTOP }, { P_PELVIS, P_HEADTOP }, { P_CHEST, P_RFA }, { P_CHEST, P_LFA },
};
enum { NS = sizeof(kSoft) / sizeof(kSoft[0]) };
const float kSoftK = 0.07f;
// Poids : gravite un peu plus forte que la vraie (le corps "tombe lourd", pas en papier), air freinant, elan des
// voitures en partie seulement transmis.
const float kGravity = 13.0f, kAirKeep = 0.99f, kCarTransfer = 0.8f;

// Os pilotes : de la particule "from" vers "to" ; reference laterale : entre deux particules (ref >= 0) ou le repere
// courant du parent pilote (parent >= 0).
struct Drive { int tag, from, to, refA, refB, parent; };
const Drive kDrives[ND] = {
    { 1, P_PELVIS, P_CHEST, P_RTH, P_LTH, -1 },
    { 3, P_CHEST, P_NECK, P_RUA, P_LUA, -1 },
    { 4, P_NECK, P_HEAD, -1, -1, 1 },
    { 5, P_HEAD, P_HEADTOP, -1, -1, 2 },
    { 22, P_RUA, P_RFA, -1, -1, 1 },
    { 23, P_RFA, P_RH, -1, -1, 4 },
    { 32, P_LUA, P_LFA, -1, -1, 1 },
    { 33, P_LFA, P_LH, -1, -1, 6 },
    { 51, P_RTH, P_RCA, -1, -1, 0 },
    { 52, P_RCA, P_RFO, -1, -1, 8 },
    { 41, P_LTH, P_LCA, -1, -1, 0 },
    { 42, P_LCA, P_LFO, -1, -1, 10 },
};

struct Frame { V3 x, y, z; };
Frame MakeFrame(V3 d, V3 r)
{
    Frame f;
    f.x = Norm(d);
    V3 z = Cross(f.x, r);
    if (Len(z) < 1e-4f) z = Cross(f.x, fabsf(f.x.z) < 0.9f ? V3{ 0, 0, 1 } : V3{ 1, 0, 0 });
    f.z = Norm(z);
    f.y = Cross(f.z, f.x);
    return f;
}
// Rotation "depart -> courant" appliquee a un vecteur : R v = Fc (F0^T v).
inline V3 Rot(const Frame &f0, const Frame &fc, V3 v) { return fc.x * Dot(f0.x, v) + fc.y * Dot(f0.y, v) + fc.z * Dot(f0.z, v); }

struct Rag {
    bool used, pending, ready, remote, alive, sleeping, ending, broken;
    void *ped;
    uint32_t handle;          // reference de pool locale
    uint8_t owner;            // copie : son proprietaire
    uint32_t ownerHandle;     // copie : sa reference chez le proprietaire
    int nodes;
    int parent[MAX_NODES], driveOf[MAX_NODES];
    float b0[MAX_NODES][16];  // matrices de depart (RwMatrix : right, up, at, pos)
    int pnode[NP];
    V3 p[NP], q[NP], target[NP];
    bool haveTarget;
    float rest[NL], minDist[NM], soft[NS];
    Frame f0[ND];
    V3 startVel, topple;
    float startZ;
    uint32_t startAt, lastSend, lastRecv, sleepAt, endAt, still, lastWake;
    V3 wakePos;
    bool noSupportWake;   // reveille "sans support" et reste sur place : support invisible au test, on n'y regarde plus
    int endSends;
};
Rag g_rag[MAX_RAG];

// --- Matrices d'os ---
void *Hier(void *ped)
{
    void *clump = Field<void *>(ped, 0x4C);
    return clump ? ((void *(__cdecl *)(void *))0x57F250)(clump) : NULL;   // GetAnimHierarchyFromSkinClump
}
float *Mats(void *hier) { return ((float *(__cdecl *)(void *))0x646370)(hier); }   // RpHAnimHierarchyGetMatrixArray
int IndexOf(void *hier, int tag) { return ((int(__cdecl *)(void *, int))0x646390)(hier, tag); }   // RpHAnimIDGetIndex
inline V3 MPos(const float *m) { return { m[12], m[13], m[14] }; }

bool Capture(Rag &r)
{
    void *hier = Hier(r.ped);
    float *mats = hier ? Mats(hier) : NULL;
    if (!mats) return false;
    int n = Field<int>(hier, 0x04);   // numNodes
    if (n <= 0 || n > MAX_NODES) return false;
    r.nodes = n;
    memcpy(r.b0, mats, n * 64);
    // Parents d'apres la pile de la hierarchie (drapeaux PUSH 2 / POP 1 de RpHAnimNodeInfo, +0x10, 16 octets).
    const uint8_t *info = Field<const uint8_t *>(hier, 0x10);
    int stack[MAX_NODES], sp = 0, cur = -1;
    bool infoOk = info != NULL;
    for (int i = 0; i < n; i++) {
        r.driveOf[i] = -1;
        if (!infoOk) { r.parent[i] = -1; continue; }
        int flags = *(const int *)(info + i * 16 + 8);
        if (flags & 2) { if (sp < MAX_NODES) stack[sp++] = cur; }
        r.parent[i] = cur;
        if (flags & 1) cur = sp > 0 ? stack[--sp] : -1;
        else cur = i;
    }
    for (int i = 0; i < NP; i++) {
        if (kTag[i] < 0) continue;
        int idx = IndexOf(hier, kTag[i]);
        if (idx < 0 || idx >= n) return false;
        r.pnode[i] = idx;
        r.p[i] = MPos(r.b0[idx]);
    }
    for (int d = 0; d < ND; d++) r.driveOf[r.pnode[kDrives[d].from]] = d;
    // Sommet du crane : dans le prolongement du cou.
    r.p[P_HEADTOP] = r.p[P_HEAD] + Norm(r.p[P_HEAD] - r.p[P_NECK]) * 0.16f;
    for (int i = 0; i < NP; i++) if (!Finite(r.p[i])) return false;
    for (int l = 0; l < NL; l++) r.rest[l] = Len(r.p[kLinks[l][0]] - r.p[kLinks[l][1]]);
    for (int m = 0; m < NM; m++)
        r.minDist[m] = 0.62f * (Len(r.p[kMins[m].a] - r.p[kMins[m].mid]) + Len(r.p[kMins[m].mid] - r.p[kMins[m].b]));
    for (int k = 0; k < NS; k++) r.soft[k] = Len(r.p[kSoft[k][0]] - r.p[kSoft[k][1]]);
    for (int d = 0; d < ND; d++) {
        const Drive &k = kDrives[d];
        V3 ref = k.parent < 0 ? r.p[k.refA] - r.p[k.refB] : r.f0[k.parent].z;
        r.f0[d] = MakeFrame(r.p[k.to] - r.p[k.from], ref);
    }
    return true;
}

void CurrentFrames(const Rag &r, Frame *fc)
{
    for (int d = 0; d < ND; d++) {
        const Drive &k = kDrives[d];
        V3 ref = k.parent < 0 ? r.p[k.refA] - r.p[k.refB] : fc[k.parent].z;
        fc[d] = MakeFrame(r.p[k.to] - r.p[k.from], ref);
    }
}

// Nos matrices dans la hierarchie ; w < 1 : melange avec la pose de l'animation (fin d'une chute, il se releve).
void WritePose(Rag &r, float w)
{
    void *hier = Hier(r.ped);
    float *mats = hier ? Mats(hier) : NULL;
    if (!mats || Field<int>(hier, 0x04) != r.nodes) return;
    Frame fc[ND];
    CurrentFrames(r, fc);
    static Frame df0[MAX_NODES], dfc[MAX_NODES];   // rotation de chaque os (repere de depart -> courant)
    static V3 pos[MAX_NODES];
    const int pelvis = r.pnode[P_PELVIS];
    for (int i = 0; i < r.nodes; i++) {
        const float *b = r.b0[i];
        V3 bp = MPos(b);
        int d = r.driveOf[i];
        int par = r.parent[i];
        if (d >= 0) {
            df0[i] = r.f0[d]; dfc[i] = fc[d];
            pos[i] = r.p[kDrives[d].from];
        } else if (par >= 0 && par < i) {
            df0[i] = df0[par]; dfc[i] = dfc[par];
            pos[i] = pos[par] + Rot(df0[par], dfc[par], bp - MPos(r.b0[par]));
        } else {   // racine (ou hierarchie illisible) : suit le bassin
            df0[i] = r.f0[0]; dfc[i] = fc[0];
            pos[i] = r.p[P_PELVIS] + Rot(r.f0[0], fc[0], bp - MPos(r.b0[pelvis]));
        }
        V3 ax[3];
        for (int a = 0; a < 3; a++) ax[a] = Rot(df0[i], dfc[i], V3{ b[a * 4], b[a * 4 + 1], b[a * 4 + 2] });
        float *m = mats + i * 16;
        if (w >= 1.0f) {
            for (int a = 0; a < 3; a++) { m[a * 4] = ax[a].x; m[a * 4 + 1] = ax[a].y; m[a * 4 + 2] = ax[a].z; }
            m[12] = pos[i].x; m[13] = pos[i].y; m[14] = pos[i].z;
        } else {
            for (int a = 0; a < 3; a++) {
                V3 an = { m[a * 4], m[a * 4 + 1], m[a * 4 + 2] };
                V3 mix = an * (1.0f - w) + ax[a] * w;
                m[a * 4] = mix.x; m[a * 4 + 1] = mix.y; m[a * 4 + 2] = mix.z;
            }
            m[12] = m[12] * (1.0f - w) + pos[i].x * w;
            m[13] = m[13] * (1.0f - w) + pos[i].y * w;
            m[14] = m[14] * (1.0f - w) + pos[i].z * w;
        }
    }
}

// --- Crochet CPed::PreRender (entree 12 de la table virtuelle) ---
typedef void(__fastcall *PreRender_t)(void *ped, void *edx);
struct { void **vt; PreRender_t orig; } g_hooks[8];
int g_hookCount;

Rag *FindRag(void *ped) { for (auto &r : g_rag) if (r.used && r.ped == ped) return &r; return NULL; }

void __fastcall h_PreRender(void *ped, void *edx)
{
    void **vt = *(void ***)ped;
    PreRender_t orig = NULL;
    for (int i = 0; i < g_hookCount; i++) if (g_hooks[i].vt == vt) orig = g_hooks[i].orig;
    if (orig) orig(ped, edx);
    Rag *r = FindRag(ped);
    if (!r || r->broken) return;
    if (r->pending) {
        r->pending = false;
        if (!Capture(*r)) { r->broken = true; Log("ragdoll : squelette illisible (modele %d), animation du jeu gardee", ModelIndex(ped)); return; }
        float dt = TimeStep() / 50.0f;
        if (dt < 0.005f || dt > 0.05f) dt = 1.0f / 30.0f;
        for (int i = 0; i < NP; i++) {
            // Bascule : vitesse proportionnelle a la hauteur au-dessus du bassin (tete et torse partent, pieds restent).
            float hgt = r->p[i].z - r->p[P_PELVIS].z + 0.9f;
            if (hgt < 0.0f) hgt = 0.0f;
            r->q[i] = r->p[i] - (r->startVel + r->topple * (hgt / 1.7f)) * dt;
        }
        if (r->remote && r->haveTarget) memcpy(r->p, r->target, sizeof(r->p));   // pose deja recue
        r->ready = true;
        r->startAt = GetTickCount();
        r->startZ = r->p[P_PELVIS].z;
    }
    if (!r->ready) return;
    float w = 1.0f;
    if (r->ending) {
        w = 1.0f - (GetTickCount() - r->endAt) / 300.0f;
        if (w <= 0.0f) { r->used = false; return; }
    }
    WritePose(*r, w);
}

void HookPreRender(void *ped)
{
    void **vt = *(void ***)ped;
    if (vt[12] == (void *)h_PreRender) return;
    for (int i = 0; i < g_hookCount; i++) if (g_hooks[i].vt == vt) return;
    if (g_hookCount >= 8) return;
    g_hooks[g_hookCount].vt = vt;
    g_hooks[g_hookCount].orig = (PreRender_t)PatchPointer(&vt[12], (void *)h_PreRender);
    Log("ragdoll : crochet PreRender pose (table %p, fonction %p)", vt, g_hooks[g_hookCount].orig);
    g_hookCount++;
}

Rag *NewRag(void *ped)
{
    Rag *slot = NULL;
    for (auto &r : g_rag) if (!r.used) { slot = &r; break; }
    if (!slot) {   // plein : on libere le plus ancien corps immobile
        for (auto &r : g_rag) if (r.sleeping && (!slot || r.sleepAt < slot->sleepAt)) slot = &r;
        if (!slot) return NULL;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = slot->pending = true;
    slot->ped = ped;
    slot->handle = PedHandle(ped);
    HookPreRender(ped);
    return slot;
}

bool StillValid(const Rag &r)
{
    Pool *p = PedPool();
    int i = (int)((uint8_t *)r.ped - p->objects) / PED_POOL_ENTRY;
    return i >= 0 && i < p->size && !(p->flags[i] & 0x80) && (uint32_t)((i << 8) | p->flags[i]) == r.handle;
}

// --- Monde : batiments et objets, vehicules ---
typedef bool(__cdecl *LineOfSight_t)(const float *, const float *, void *, void **, bool, bool, bool, bool, bool, bool, bool, bool);
bool Hit(V3 a, V3 b, V3 &point, V3 &normal)
{
    uint8_t col[64] = {};
    void *ent = NULL;
    float s[3] = { a.x, a.y, a.z }, e[3] = { b.x, b.y, b.z };
    if (!((LineOfSight_t)0x4D92D0)(s, e, col, &ent, true, false, false, true, false, false, false, false)) return false;
    point = { ((float *)col)[0], ((float *)col)[1], ((float *)col)[2] };
    normal = { ((float *)col)[4], ((float *)col)[5], ((float *)col)[6] };
    return Finite(point) && Finite(normal);
}

void PushByVehicles(Rag &r, float stepFrac)
{
    Pool *vp = VehiclePool();
    for (int i = 0; i < vp->size; i++) {
        if (vp->flags[i] & 0x80) continue;
        void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
        V3 c = { Pos(v).x, Pos(v).y, Pos(v).z };
        if (Dot(c - r.p[P_PELVIS], c - r.p[P_PELVIS]) > 8.0f * 8.0f) continue;
        void *mi = *(void **)(0x92D4C8 + ModelIndex(v) * 4);
        void *cm = mi ? Field<void *>(mi, 0x1C) : NULL;
        if (!cm) continue;
        V3 mn = Field<V3>(cm, 0x10), mx = Field<V3>(cm, 0x1C);
        V3 ax[3] = { Field<V3>(v, 0x04), Field<V3>(v, 0x14), Field<V3>(v, 0x24) };
        Vec3 ms = MoveSpeed(v);
        V3 carStep = V3{ ms.x, ms.y, ms.z } * (TimeStep() * stepFrac);
        for (int k = 0; k < NP; k++) {
            V3 d = r.p[k] - c;
            float l[3] = { Dot(d, ax[0]), Dot(d, ax[1]), Dot(d, ax[2]) };
            float lo[3] = { mn.x - 0.06f, mn.y - 0.06f, mn.z - 0.06f }, hi[3] = { mx.x + 0.06f, mx.y + 0.06f, mx.z + 0.06f };
            bool in = true;
            for (int a = 0; a < 3 && in; a++) in = l[a] > lo[a] && l[a] < hi[a];
            if (!in) continue;
            int best = 0; float pen = 1e9f, dir = 1.0f;
            for (int a = 0; a < 3; a++) {
                float up = hi[a] - l[a], dn = l[a] - lo[a];
                if (up < pen) { pen = up; best = a; dir = 1.0f; }
                if (dn < pen) { pen = dn; best = a; dir = -1.0f; }
            }
            r.p[k] = r.p[k] + ax[best] * (pen * dir);
            // Il prend la vitesse de la voiture (et un peu de hauteur quand elle le heurte de face).
            float sp = Len(carStep);
            // Pose sur le toit ou le capot : il suit la voiture (frottement) ; heurte de cote : une partie de l'elan.
            r.q[k] = r.p[k] - carStep * (best == 2 ? 1.0f : kCarTransfer) - V3{ 0, 0, best == 2 ? 0.0f : sp * 0.15f };
        }
    }
}

void Simulate(Rag &r)
{
    float dt = TimeStep() / 50.0f;
    if (dt < 0.005f) dt = 0.005f;
    if (dt > 0.05f) dt = 0.05f;
    const int sub = 2;
    float h = dt / sub;
    V3 prevFrame[NP];
    memcpy(prevFrame, r.p, sizeof(prevFrame));
    for (int s = 0; s < sub; s++) {
        for (int i = 0; i < NP; i++) {
            V3 v = (r.p[i] - r.q[i]) * kAirKeep;
            r.q[i] = r.p[i];
            r.p[i] = r.p[i] + v + V3{ 0, 0, -kGravity * h * h };
        }
        PushByVehicles(r, 1.0f / sub);
        for (int it = 0; it < 10; it++) {
            for (int l = 0; l < NL; l++) {
                V3 &a = r.p[kLinks[l][0]], &b = r.p[kLinks[l][1]];
                V3 d = b - a;
                float len = Len(d);
                if (len < 1e-5f) continue;
                V3 corr = d * (0.5f * (len - r.rest[l]) / len);
                a = a + corr; b = b - corr;
            }
            for (int k = 0; k < NS; k++) {
                V3 &a = r.p[kSoft[k][0]], &b = r.p[kSoft[k][1]];
                V3 d = b - a;
                float len = Len(d);
                if (len < 1e-5f) continue;
                V3 corr = d * (0.5f * kSoftK * (len - r.soft[k]) / len);
                a = a + corr; b = b - corr;
            }
            for (int m = 0; m < NM; m++) {
                V3 &a = r.p[kMins[m].a], &b = r.p[kMins[m].b];
                V3 d = b - a;
                float len = Len(d);
                if (len >= r.minDist[m] || len < 1e-5f) continue;
                V3 corr = d * (0.5f * (len - r.minDist[m]) / len);
                a = a + corr; b = b - corr;
            }
        }
    }
    // Collisions : du point de l'image precedente au nouveau (un peu en arriere, pour ne pas partir de la surface).
    for (int i = 0; i < NP; i++) {
        V3 move = r.p[i] - prevFrame[i];
        float ml = Len(move);
        if (ml < 1e-5f) continue;
        V3 start = prevFrame[i] - move * (0.05f / ml);
        V3 pt, n;
        if (!Hit(start, r.p[i] + move * (0.04f / ml), pt, n)) continue;
        n = Norm(n);
        r.p[i] = pt + n * 0.06f;
        V3 v = r.p[i] - r.q[i];
        float vn = Dot(v, n);
        V3 vt = v - n * vn;
        r.q[i] = r.p[i] - (vt * 0.35f - n * (vn < 0 ? 0.0f : vn));   // lourd : frottement fort, pas de rebond
    }
    for (int i = 0; i < NP; i++) if (!Finite(r.p[i])) { r.broken = true; Log("ragdoll : simulation invalide, abandon"); return; }
    float maxMove = 0.0f;
    for (int i = 0; i < NP; i++) { float d = Len(r.p[i] - prevFrame[i]); if (d > maxMove) maxMove = d; }
    r.still = maxMove < 0.004f ? r.still + 1 : 0;
}

// --- Reseau ---
#pragma pack(push, 1)
struct MsgRagdoll { uint8_t type, owner, flags, count; uint32_t handle; float base[3]; int16_t p[NP][3]; };
#pragma pack(pop)
enum { RF_ACTIVE = 1, RF_SLEEP = 2, RF_END = 4 };

bool OthersConnected()
{
    for (int i = 0; i < MAX_PLAYERS; i++) if (i != g_localId && g_players[i].connected) return true;
    return false;
}

void Send(Rag &r, uint8_t flags)
{
    if (g_localId < 0 || !OthersConnected()) return;
    MsgRagdoll m = {};
    m.type = MSG_RAGDOLL;
    m.owner = (uint8_t)g_localId;
    m.flags = flags;
    m.count = NP;
    m.handle = r.handle;
    V3 b = r.p[P_PELVIS];
    m.base[0] = b.x; m.base[1] = b.y; m.base[2] = b.z;
    for (int i = 0; i < NP; i++) {
        V3 d = (r.p[i] - b) * 1000.0f;   // en millimetres
        float c[3] = { d.x, d.y, d.z };
        for (int a = 0; a < 3; a++) m.p[i][a] = (int16_t)(c[a] > 32000.0f ? 32000 : c[a] < -32000.0f ? -32000 : (int)c[a]);
    }
    NetSendToAll(&m, sizeof(m));
}

// Corps immobile (endormi) : plus rien sous lui (la voiture sur laquelle il etait tombe est partie), ou une voiture
// qui roule le touche : il se remet a bouger (avant : il restait fige en l'air).
bool NeedsWake(const Rag &r)
{
    V3 pv = r.p[P_PELVIS], ch = r.p[P_CHEST];
    if (!r.noSupportWake) {
        uint8_t col[64];
        void *ent = NULL;
        bool supported = false;
        const V3 pts[2] = { pv, ch };
        for (V3 c : pts) {
            float s[3] = { c.x, c.y, c.z + 0.15f }, e[3] = { c.x, c.y, c.z - 0.45f };
            memset(col, 0, sizeof(col));
            if (((LineOfSight_t)0x4D92D0)(s, e, col, &ent, true, true, false, true, false, false, false, false)) supported = true;
        }
        if (!supported) return true;
    }
    Pool *vp = VehiclePool();
    for (int i = 0; i < vp->size; i++) {
        if (vp->flags[i] & 0x80) continue;
        void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
        Vec3 ms = MoveSpeed(v);
        if (ms.x * ms.x + ms.y * ms.y + ms.z * ms.z < 0.05f * 0.05f) continue;   // roule vraiment (pas une epave qui tangue)
        float dx = Pos(v).x - pv.x, dy = Pos(v).y - pv.y, dz = Pos(v).z - pv.z;
        if (dx * dx + dy * dy + dz * dz < 2.5f * 2.5f) return true;
    }
    return false;
}

void KeepPedOnBody(Rag &r)
{
    // Le personnage (bouding sphere, argent ou arme laches, tests du jeu) suit le bassin ; plus de vitesse a lui.
    Vec3 &pos = Pos(r.ped);
    pos.x = r.p[P_PELVIS].x;
    pos.y = r.p[P_PELVIS].y;
    if (!r.alive) pos.z = r.p[P_PELVIS].z;
    MoveSpeed(r.ped) = { 0, 0, 0 };
}

}   // namespace

// Personnages locaux : debut (mort, ou chute violente = percute), simulation, envoi ; copies : pose recue.
void RagdollAfterProcess()
{
    if (!g_cfg.ragdoll) { for (auto &r : g_rag) r.used = false; return; }
    uint32_t now = GetTickCount();
    void *me = FindPlayerPed();
    // Nouveaux : personnages a nous (pas les joueurs, pas les copies) qui meurent ou tombent vite.
    Pool *pool = PedPool();
    for (int i = 0; i < pool->size; i++) {
        if (pool->flags[i] & 0x80) continue;
        void *ped = pool->objects + i * PED_POOL_ENTRY;
        if (ped == me || IsPuppet(ped) || IsGhostPed(ped) || InVehicle(ped)) continue;
        int st = PedState(ped);
        Vec3 ms = MoveSpeed(ped);
        float sp = sqrtf(ms.x * ms.x + ms.y * ms.y + ms.z * ms.z);
        bool dying = st == 54;                  // PED_DIE (l'animation de mort commence)
        bool knocked = st == 42 && sp > 0.08f;  // PED_FALL projete (percute par un vehicule, explosion : > 4 m/s)
        Rag *r = FindRag(ped);
        if (r && r->used) {
            if (dying && r->alive) r->alive = false;   // percute puis mort : il reste mou
            continue;
        }
        if (!dying && !knocked) continue;
        int active = 0;
        for (auto &x : g_rag) if (x.used && !x.sleeping && !x.remote) active++;
        if (active >= MAX_ACTIVE) continue;
        r = NewRag(ped);
        if (!r) continue;
        r->alive = !dying;
        r->startVel = V3{ ms.x, ms.y, ms.z } * (50.0f * kCarTransfer);   // m/s (vitesse du jeu : par 1/50 s)
        // Tue par une balle ou un coup : le jeu a remis sa vitesse a zero ; il bascule a l'oppose du joueur le plus
        // proche (probablement le tireur), le haut du corps plus que les jambes (Capture).
        if (sp < 0.02f) {
            void *best = NULL;
            float bd = 30.0f * 30.0f;
            for (int k = -1; k < MAX_PLAYERS; k++) {
                void *pl = k < 0 ? me : PuppetPed(k);
                if (!pl) continue;
                float dx = Pos(ped).x - Pos(pl).x, dy = Pos(ped).y - Pos(pl).y, d = dx * dx + dy * dy;
                if (d < bd && d > 0.01f) { bd = d; best = pl; }
            }
            V3 away = best ? Norm(V3{ Pos(ped).x - Pos(best).x, Pos(ped).y - Pos(best).y, 0 }) : V3{ -sinf(Heading(ped)), cosf(Heading(ped)), 0 } * -1.0f;
            r->topple = away * 2.2f;
        }
        if (g_cfg.logScripts) Log("ragdoll : %08X %s (%.1f m/s)", r->handle, dying ? "meurt" : "percute", sp * 50.0f);
    }
    for (auto &r : g_rag) {
        if (!r.used) continue;
        if (!StillValid(r)) { r.used = false; continue; }
        if (r.ending && now - r.endAt > 400) { r.used = false; continue; }   // (pas dessine : PreRender ne l'a pas libere)
        if (r.broken || !r.ready) continue;
        if (r.remote) {
            if (r.haveTarget && !r.ending) {
                float k = TimeStep() * 0.35f;
                if (k > 1.0f) k = 1.0f;
                for (int i = 0; i < NP; i++) r.p[i] = r.p[i] + (r.target[i] - r.p[i]) * k;
                KeepPedOnBody(r);
            }
            if (!r.sleeping && !r.ending && now - r.lastRecv > 4000) { r.ending = true; r.endAt = now; }
            continue;
        }
        if (r.ending) continue;
        if (!r.sleeping) {
            Simulate(r);
            if (r.broken) continue;
            // Passe a travers le sol (collision manquee : sol pas encore charge...) : remonte et fige.
            if (r.p[P_PELVIS].z < r.startZ - 6.0f) {
                float up = r.startZ - 0.8f - r.p[P_PELVIS].z;
                for (int i = 0; i < NP; i++) { r.p[i].z += up; r.q[i] = r.p[i]; }
                r.still = 1000;
                Log("ragdoll : %08X passait sous le sol, remonte", r.handle);
            }
            KeepPedOnBody(r);
            if (!r.alive && (r.still > 45 || now - r.startAt > 12000)) {
                r.sleeping = true;
                r.sleepAt = now;
                if (r.lastWake && Len(r.p[P_PELVIS] - r.wakePos) < 0.1f) r.noSupportWake = true;
                Send(r, RF_SLEEP);
                r.lastSend = now;
            } else if (now - r.lastSend >= 66) {
                Send(r, RF_ACTIVE);
                r.lastSend = now;
            }
        } else {
            if (now - r.lastWake >= 250) {
                r.lastWake = now;
                if (NeedsWake(r)) {
                    r.sleeping = false;
                    r.still = 0;
                    r.startAt = now;
                    r.startZ = r.p[P_PELVIS].z;
                    r.wakePos = r.p[P_PELVIS];
                    for (int i = 0; i < NP; i++) r.q[i] = r.p[i];
                    if (g_cfg.logScripts) Log("ragdoll : %08X se remet a bouger (plus de support, ou voiture)", r.handle);
                    continue;
                }
            }
            if (now - r.sleepAt < 6000 && now - r.lastSend >= 1000) {   // pose finale, redite (pertes, arrivees)
                Send(r, RF_SLEEP);
                r.lastSend = now;
            }
        }
        // Percute mais vivant : quand le jeu le fait se relever, on rend la main a l'animation (fondu de 0,3 s),
        // debout la ou son corps est tombe.
        if (r.alive && PedState(r.ped) != 42 && now - r.startAt > 400) {
            Vec3 &pos = Pos(r.ped);
            pos.x = r.p[P_PELVIS].x;
            pos.y = r.p[P_PELVIS].y;
            pos.z = r.p[P_PELVIS].z + 0.9f;
            r.ending = true;
            r.endAt = now;
            for (int k = 0; k < 3; k++) Send(r, RF_END);
            if (g_cfg.logScripts) Log("ragdoll : %08X se releve (etat %d, %u ms)", r.handle, PedState(r.ped), now - r.startAt);
        }
    }
}

void RagdollOnMsg(const uint8_t *buf, int len)
{
    if (len < (int)sizeof(MsgRagdoll) || !g_cfg.ragdoll || GameState() != GS_PLAYING) return;
    const MsgRagdoll &m = *(const MsgRagdoll *)buf;
    if (m.owner == g_localId || m.count != NP) return;
    void *ped = GhostPedOf(m.owner, m.handle);
    if (!ped) return;
    Rag *r = FindRag(ped);
    if (m.flags & RF_END) {
        if (r && r->ready && !r->ending) { r->ending = true; r->endAt = GetTickCount(); }
        return;
    }
    if (!r) {
        r = NewRag(ped);
        if (!r) return;
        r->remote = true;
        r->owner = m.owner;
        r->ownerHandle = m.handle;
        if (g_cfg.logScripts) Log("ragdoll : copie %08X du joueur %d, pose recue", m.handle, m.owner);
    }
    V3 b = { m.base[0], m.base[1], m.base[2] };
    for (int i = 0; i < NP; i++) r->target[i] = b + V3{ m.p[i][0] * 0.001f, m.p[i][1] * 0.001f, m.p[i][2] * 0.001f };
    if (!r->haveTarget && r->ready) memcpy(r->p, r->target, sizeof(r->p));
    r->haveTarget = true;
    r->lastRecv = GetTickCount();
    if (m.flags & RF_SLEEP) { r->sleeping = true; r->sleepAt = GetTickCount(); }
    else r->sleeping = false;   // reveille chez le proprietaire
}

void RagdollReset()
{
    for (auto &r : g_rag) r.used = false;
}
