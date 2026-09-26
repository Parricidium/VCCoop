// Conditions de mission elargies aux invites (hote). Chaque condition de script passe son resultat a
// CRunningScript::UpdateCompareFlag (0x463F00), qui applique ensuite la negation et les enchainements ET / OU.
// Quand une condition "joueur" (se trouver quelque part, etre dans telle voiture...) est fausse pour l'hote,
// on la teste aussi pour chaque invite, avec les parametres que le jeu vient de lire (ScriptParams, 0x7D7438) ;
// si l'un d'eux la remplit, elle devient vraie. Ainsi n'importe quel joueur peut atteindre un objectif.
// Les marqueurs (cylindres) dessines par ces conditions sont aussi envoyes aux invites.
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "vehicles.h"
#include "conditions.h"
#include <math.h>
#include <string.h>

using namespace game;

static int32_t P(int i) { return ((int32_t *)0x7D7438)[i]; }
static float F(int i) { return ((float *)0x7D7438)[i]; }

// Entite (personnage ou vehicule) a laquelle se rapporte une reference de pool de l'hote.
static bool EntityPos(uint32_t h, Vec3 &out)
{
    if (void *p = PedFromHandle(h)) { out = Pos(p); return true; }
    Pool *vp = VehiclePool();
    int i = (int)(h >> 8);
    if (i >= 0 && i < vp->size && vp->flags[i] == (uint8_t)(h & 0xFF)) { out = Pos(vp->objects + i * VEHICLE_POOL_ENTRY); return true; }
    return false;
}

enum Mode { ANY, ON_FOOT, IN_CAR };

static bool InBox(const MsgState &s, float x, float y, float z, float rx, float ry, float rz, bool use3d)
{
    return fabsf(s.pos[0] - x) < rx && fabsf(s.pos[1] - y) < ry && (!use3d || fabsf(s.pos[2] - z) < rz);
}

static bool ModeOk(const MsgState &s, Mode m, bool stopped)
{
    if (m == ON_FOOT && s.inVehicle) return false;
    if (m == IN_CAR && !s.inVehicle) return false;
    if (stopped && s.speed[0] * s.speed[0] + s.speed[1] * s.speed[1] + s.speed[2] * s.speed[2] > 0.0001f) return false;
    return true;
}

// Numerotation (aiguillage des opcodes 200-299, 0x444BE0) :
//   00EC-00F1 joueur, position 2D : joueur x y rx ry sphere      (+0 / +1 / +2 = tous moyens / a pied / en voiture,
//   00F2-00F4 joueur pres d'un perso 2D : joueur perso rx ry sphere          +3.. = "arrete")
//   00F5-00FA joueur, position 3D : joueur x y z rx ry rz sphere
//   00FB-00FD joueur pres d'un perso 3D : joueur perso rx ry rz sphere
//   00FE-0103 / 0104-0106 : les memes en 2D pour un PERSONNAGE (les missions y mettent souvent $PLAYER_ACTOR).
static bool IsPlayerLocate(uint16_t op) { return op >= 0x00EC && op <= 0x00FD; }
static bool IsCharLocate(uint16_t op) { return op >= 0x00FE && op <= 0x0106; }

// Vrai si un invite (au moins) remplit la condition op, parametres dans ScriptParams.
static bool AnyGuestSatisfies(uint16_t op)
{
    // Un LOCATE de personnage ne concerne les joueurs que s'il vise le Tommy de l'hote : on le lit comme
    // le LOCATE "joueur" correspondant (meme disposition des parametres apres le premier).
    if (IsCharLocate(op)) op = (uint16_t)(op - 0x00FE + 0x00EC);
    // "Joueur pres d'un personnage" (00F2-00F4, 00FB-00FD) : c'est presque toujours la logique d'un accompagnateur
    // (Ken, Lance... le suivre, l'attendre). Rempli par un invite reste a cote de lui, le script de l'accompagnateur
    // croyait l'hote la et la mission de l'hote attendait sans fin (The Party). Reserve a l'hote.
    if ((op >= 0x00F2 && op <= 0x00F4) || (op >= 0x00FB && op <= 0x00FD)) return false;
    for (int i = 1; i < MAX_PLAYERS; i++) {
        const NetPlayer &g = g_players[i];
        if (!g.connected || !g.state.inGame) continue;
        const MsgState &s = g.state;
        if (op >= 0x00EC && op <= 0x00F1) {
            if (ModeOk(s, (Mode)((op - 0x00EC) % 3), op >= 0x00EF) && InBox(s, F(1), F(2), 0, F(3), F(4), 0, false)) return true;
        } else if (op >= 0x00F2 && op <= 0x00F4) {
            Vec3 c;
            if (EntityPos((uint32_t)P(1), c) && ModeOk(s, (Mode)(op - 0x00F2), false) && InBox(s, c.x, c.y, 0, F(2), F(3), 0, false)) return true;
        } else if (op >= 0x00F5 && op <= 0x00FA) {
            if (ModeOk(s, (Mode)((op - 0x00F5) % 3), op >= 0x00F8) && InBox(s, F(1), F(2), F(3), F(4), F(5), F(6), true)) return true;
        } else if (op >= 0x00FB && op <= 0x00FD) {
            Vec3 c;
            if (EntityPos((uint32_t)P(1), c) && ModeOk(s, (Mode)(op - 0x00FB), false) && InBox(s, c.x, c.y, c.z, F(2), F(3), F(4), true)) return true;
        } else if (op == 0x00DC) {   // IS_PLAYER_IN_CAR joueur voiture
            void *v = s.inVehicle ? NetVehicleById(s.vehicleId) : NULL;
            if (v && VehicleHandle(v) == (uint32_t)P(1)) return true;
        } else if (op == 0x00DE) {   // IS_PLAYER_IN_MODEL joueur modele
            void *v = s.inVehicle ? NetVehicleById(s.vehicleId) : NULL;
            if (v && ModelIndex(v) == P(1)) return true;
        } else if (op == 0x00E0) {   // IS_PLAYER_IN_ANY_CAR joueur
            if (s.inVehicle) return true;
        }
    }
    return false;
}

static bool IsLocate(uint16_t op) { return IsPlayerLocate(op) || IsCharLocate(op); }

static uint16_t g_curOp;
static void *g_curScript;

void ConditionsBeginCommand(void *script, uint16_t op) { g_curScript = script; g_curOp = op; }

typedef void(__fastcall *UpdateCompareFlag_t)(void *script, void *edx, uint8_t flag);
static UpdateCompareFlag_t o_UpdateCompareFlag;

static void __fastcall h_UpdateCompareFlag(void *script, void *edx, uint8_t flag)
{
    // Invite : les scripts hors mission ne voient aucun pickup ramasse (HAS_PICKUP_BEEN_COLLECTED 0214). Le moteur
    // donne toujours armes / sante / gilet, mais les reactions du script principal, qui appartiennent a l'histoire
    // de l'hote, ne se declenchent pas chez lui : vetements (le thread "pickups" coupait le controle, rhabillait et
    // affichait "Vetements changes !" en boucle : l'invite apparait sur ce pickup), paquets caches, saccages.
    if (!g_cfg.host && flag && g_curOp == 0x0214 && script == g_curScript && !Field<bool>(script, 0x85)) flag = 0;
    if (g_cfg.host && !flag && script == g_curScript && Field<bool>(script, 0x85)) {
        uint16_t op = g_curOp;
        // Le sujet doit etre l'hote : joueur 0 ($PLAYER_CHAR) ou, pour un LOCATE de personnage, son Tommy.
        bool aboutHost = IsCharLocate(op) ? (uint32_t)P(0) == PedHandle(FindPlayerPed()) : P(0) == 0;
        if ((IsLocate(op) || op == 0x00DC || op == 0x00DE || op == 0x00E0) && aboutHost && AnyGuestSatisfies(op)) {
            flag = 1;
            static uint16_t lastOp;
            static uint32_t lastLog;
            if (op != lastOp || GetTickCount() - lastLog > 5000) {
                Log("conditions : %04X remplie par un invite (%.8s)", op, (char *)script + 8);
                lastOp = op;
                lastLog = GetTickCount();
            }
        }
    }
    o_UpdateCompareFlag(script, edx, flag);
}

// --- Marqueurs de destination : un LOCATE avec "sphere" dessine un cylindre chaque image chez l'hote ---
// L'hote envoie les coordonnees (a 5 Hz par marqueur) ; l'invite dessine le meme cylindre tant qu'il en recoit.
#pragma pack(push, 1)
struct MsgMarker { uint8_t type; uint8_t use3d; uint16_t pad; uint32_t id; float x, y, z, rx, ry, rz; };
#pragma pack(pop)

AutotestMarker g_mainMarker, g_missionMarker;

void ConditionsAfterCommand(void *script, uint16_t op, int ip)
{
    if (!g_cfg.host) return;
    bool is2d = (op >= 0x00EC && op <= 0x00F1) || (op >= 0x00FE && op <= 0x0103);
    bool is3d = op >= 0x00F5 && op <= 0x00FA;
    if (!is2d && !is3d) return;
    int sphere = P(is2d ? 5 : 7);
    if (g_cfg.logScripts) {
        static int seen[64], n;
        bool known = false;
        for (int i = 0; i < n; i++) known |= seen[i] == ip;
        if (!known && n < 64) {
            seen[n++] = ip;
            Log("conditions : LOCATE %04X @%X (%.8s) %.1f %.1f r %.1f %.1f sphere %d", op, ip, (char *)script + 8,
                F(1), F(2), F(is2d ? 3 : 4), F(is2d ? 4 : 5), sphere);
        }
    }
    // Autotest : les debuts de mission du script principal sont des LOCATE "a pied" de petit rayon, sans sphere
    // (le marqueur rose est dessine a part) ; ceux des missions ont la sphere.
    bool mission = Field<bool>(script, 0x85);
    bool onFootSmall = (op == 0x00ED || op == 0x00F6) && F(is2d ? 3 : 4) < 3.0f;
    if (mission ? sphere != 0 : onFootSmall) {
        AutotestMarker &am = mission ? g_missionMarker : g_mainMarker;
        am.x = F(1); am.y = F(2); am.z = is3d ? F(3) : 0.0f; am.at = GetTickCount(); am.ip = ip;
    }
    if (!sphere || !mission) return;
    static struct { uint32_t id, last; } seen[16];
    uint32_t now = GetTickCount(), id = (uint32_t)ip;
    int slot = -1;
    for (int i = 0; i < 16; i++) if (seen[i].id == id) slot = i;
    if (slot < 0) { slot = 0; for (int i = 1; i < 16; i++) if (seen[i].last < seen[slot].last) slot = i; seen[slot].id = id; seen[slot].last = 0; }
    if (now - seen[slot].last < 200) return;
    seen[slot].last = now;
    MsgMarker m = { MSG_MARKER, (uint8_t)is3d, 0, id, F(1), F(2), is3d ? F(3) : 0.0f,
                    is3d ? F(4) : F(3), is3d ? F(5) : F(4), is3d ? F(6) : 0.0f };
    NetSendToGuests(&m, sizeof(m));
}

// Invite : cylindres recus, redessines chaque image pendant 0,5 s (CTheScripts::HighlightImportantArea, 0x45F080,
// appele par les LOCATE eux-memes, cf. 0x463090).
static struct { uint32_t id, until; MsgMarker m; } g_markers[16];

void OnMarker(const uint8_t *data, int len)
{
    if (len < (int)sizeof(MsgMarker)) return;
    const MsgMarker &m = *(const MsgMarker *)data;
    int slot = -1;
    for (int i = 0; i < 16; i++) if (g_markers[i].id == m.id) slot = i;
    if (slot < 0) for (int i = 0; i < 16; i++) if (g_markers[i].until < GetTickCount()) { slot = i; break; }
    if (slot < 0) return;
    g_markers[slot].id = m.id;
    g_markers[slot].until = GetTickCount() + 500;
    g_markers[slot].m = m;
    g_missionMarker.x = m.x; g_missionMarker.y = m.y; g_missionMarker.z = m.z; g_missionMarker.at = GetTickCount(); g_missionMarker.ip = (int)m.id;
}

void ConditionsFrame(bool inGame)
{
    if (g_cfg.host || !inGame) return;
    uint32_t now = GetTickCount();
    for (auto &k : g_markers) {
        if (k.until < now) continue;
        const MsgMarker &m = k.m;
        // CTheScripts::HighlightImportantArea(id, x1, y1, x2, y2, z)
        ((void(__cdecl *)(uint32_t, float, float, float, float, float))0x45F080)(
            k.id, m.x - m.rx, m.y - m.ry, m.x + m.rx, m.y + m.ry, m.use3d ? m.z : *(float *)0x68A2D4);   // hauteur "2D" du jeu
    }
}

void InstallConditions()
{
    static const uint8_t pro[] = { 0x80, 0xB9, 0x82, 0x00, 0x00, 0x00, 0x00 };
    o_UpdateCompareFlag = (UpdateCompareFlag_t)MakeDetour(0x463F00, pro, sizeof(pro), (void *)h_UpdateCompareFlag);
}
