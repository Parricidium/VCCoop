// Reproduction de la "presentation" des missions : l'hote capture certaines commandes de ses scripts de
// mission (fondus, textes, camera, marqueurs radar, cinematiques, dialogues...) avec leurs parametres
// evalues, et les envoie sur le flux fiable. Chaque invite les rejoue dans un script prive, en traduisant
// les references d'entites (personnages et vehicules copies, marqueurs et objets crees par ces commandes).
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "vehicles.h"
#include "entities.h"
#include "mirror.h"
#include "combat.h"
#include "saveshare.h"
#include <string.h>

using namespace game;

// Signature des parametres :
//   v valeur   l etiquette de texte (8 octets)
//   P personnage   C vehicule   O objet   B marqueur   (references en entree, a traduire)
//   M personnage, mais le Tommy de l'hote devient le notre (tenue changee par la mission : chacun s'habille)
//   b marqueur cree   o objet cree   k pickup cree   (sorties : on retient la correspondance hote -> invite)
//   K pickup (reference en entree)
//   * signature libre : tous les parametres sont relus apres execution (commandes sans reference d'entite)
struct OpSig { uint16_t op; const char *sig; const char *name; };
static const OpSig g_ops[] = {
    { 0x016A, "vv", "DO_FADE" },
    { 0x0169, "vvv", "SET_FADING_COLOUR" },
    { 0x02A3, "v", "SWITCH_WIDESCREEN" },
    { 0x01B4, "vv", "SET_PLAYER_CONTROL" },
    { 0x00BA, "lvv", "PRINT_BIG" },
    { 0x00BB, "lvv", "PRINT" },
    { 0x00BC, "lvv", "PRINT_NOW" },
    { 0x00BE, "", "CLEAR_PRINTS" },
    { 0x03E5, "l", "PRINT_HELP" },
    { 0x03E6, "", "CLEAR_HELP" },
    { 0x01E3, "lvvv", "PRINT_WITH_NUMBER_BIG" },
    { 0x01E4, "lvvv", "PRINT_WITH_NUMBER" },
    { 0x01E5, "lvvv", "PRINT_WITH_NUMBER_NOW" },
    { 0x015F, "vvvvvv", "SET_FIXED_CAMERA_POSITION" },
    { 0x0160, "vvvv", "POINT_CAMERA_AT_POINT" },
    { 0x0158, "Cvv", "POINT_CAMERA_AT_CAR" },
    { 0x0159, "Pvv", "POINT_CAMERA_AT_CHAR" },
    { 0x015A, "", "RESTORE_CAMERA" },
    { 0x02EB, "", "RESTORE_CAMERA_JUMPCUT" },
    { 0x0373, "", "SET_CAMERA_BEHIND_PLAYER" },
    { 0x0186, "Cb", "ADD_BLIP_FOR_CAR" },
    { 0x0187, "Pb", "ADD_BLIP_FOR_CHAR" },
    { 0x018A, "vvvb", "ADD_BLIP_FOR_COORD" },
    { 0x02A7, "vvvvb", "ADD_SPRITE_BLIP_FOR_CONTACT_POINT" },
    { 0x02A8, "vvvvb", "ADD_SPRITE_BLIP_FOR_COORD" },
    { 0x0164, "B", "REMOVE_BLIP" },
    { 0x0165, "Bv", "CHANGE_BLIP_COLOUR" },
    { 0x018B, "Bv", "CHANGE_BLIP_DISPLAY" },
    { 0x0168, "Bv", "CHANGE_BLIP_SCALE" },
    { 0x02E4, "l", "LOAD_CUTSCENE" },
    { 0x0244, "vvv", "SET_CUTSCENE_OFFSET" },
    { 0x02E5, "vo", "CREATE_CUTSCENE_OBJECT" },
    { 0x02E6, "Ol", "SET_CUTSCENE_ANIM" },
    { 0x02F4, "Ovo", "CREATE_CUTSCENE_HEAD" },
    { 0x02F5, "Ol", "SET_CUTSCENE_HEAD_ANIM" },
    { 0x02E7, "", "START_CUTSCENE" },
    { 0x02EA, "", "CLEAR_CUTSCENE" },
    { 0x023C, "vl", "LOAD_SPECIAL_CHARACTER" },
    { 0x0296, "v", "UNLOAD_SPECIAL_CHARACTER" },
    { 0x0247, "v", "REQUEST_MODEL" },
    { 0x0249, "v", "MARK_MODEL_AS_NO_LONGER_NEEDED" },
    { 0x038B, "", "LOAD_ALL_MODELS_NOW" },
    { 0x03CF, "vl", "LOAD_MISSION_AUDIO" },
    { 0x03D1, "v", "PLAY_MISSION_AUDIO" },
    { 0x040D, "v", "CLEAR_MISSION_AUDIO" },
    { 0x04CE, "vvvvb", "ADD_SHORT_RANGE_SPRITE_BLIP_FOR_COORD" },
    { 0x02F3, "*", "LOAD_SPECIAL_MODEL" },
    { 0x03CB, "*", "LOAD_SCENE" },
    { 0x041D, "*", "SET_NEAR_CLIP" },
    { 0x0363, "*", "SET_VISIBILITY_OF_CLOSEST_OBJECT_OF_TYPE" },
    { 0x04BB, "*", "SET_AREA_VISIBLE" },
    { 0x0395, "*", "CLEAR_AREA" },
    { 0x00C0, "*", "SET_TIME_OF_DAY" },
    { 0x04E4, "*", "REQUEST_COLLISION" },
    { 0x054C, "*", "LOAD_MISSION_TEXT" },
    { 0x03EF, "*", "MAKE_PLAYER_SAFE_FOR_CUTSCENE" },
    { 0x03BF, "*", "SET_EVERYONE_IGNORE_PLAYER" },
    { 0x0055, "vvvv", "SET_PLAYER_COORDINATES" },
    { 0x0213, "vvvvvk", "CREATE_PICKUP" },
    { 0x032B, "vvvvvvk", "CREATE_PICKUP_WITH_AMMO" },
    { 0x02E1, "vvvvk", "CREATE_MONEY_PICKUP" },
    { 0x0215, "K", "REMOVE_PICKUP" },
    { 0x0352, "Ml", "UNDRESS_CHAR" },
    { 0x0353, "M", "DRESS_CHAR" },
    // Configuration du monde faite par l'intro (population des zones, densites) : l'invite ne la joue pas.
    { 0x0152, "*", "SET_ZONE_CAR_INFO" },
    { 0x0324, "*", "SET_ZONE_PED_GROUP_INFO" },
    { 0x015C, "*", "SET_ZONE_GANG_INFO" },
    { 0x01EB, "*", "SET_CAR_DENSITY_MULTIPLIER" },
    { 0x03DE, "*", "SET_PED_DENSITY_MULTIPLIER" },
};

MirrorPoint g_lastObjective, g_lastContact;

enum { RL_SCRIPT_CMD = 1, RL_MISSION_END = 2, RL_MISSION_START = 3, RL_GLOBALS = 4 };

// Variables globales du script principal : ScriptSpace [8, 0x8620) (le GOTO du debut de main.scm les enjambe).
// Les missions y posent leurs drapeaux (mission reussie, compteurs, deblocages) que le script principal lit pour
// avancer l'histoire. L'hote photographie cette zone au debut de chaque mission et envoie ce qui a change a la fin :
// le script principal de chaque invite avance alors comme le sien (points de contact, objectifs...).
enum { GLOBALS_BEGIN = 8, GLOBALS_END = 0x8620 };
static uint8_t g_globSnap[GLOBALS_END];
static bool g_globSnapValid;

const OpSig *FindOp(uint16_t op)
{
    for (auto &o : g_ops) if (o.op == op) return &o;
    return NULL;
}

// ======================================================================= Hote : marqueurs actifs
// Les marqueurs radar poses par les missions et pas encore retires sont gardes (la commande telle qu'envoyee),
// pour les rejouer chez un invite qui arrive en cours de mission.
struct ActiveBlip { uint32_t hostBlip; int len; uint8_t cmd[64]; };
static ActiveBlip g_activeBlips[64];
static int g_activeBlipCount;

static bool CreatesBlip(uint16_t op) { return op == 0x0186 || op == 0x0187 || op == 0x018A || op == 0x02A7 || op == 0x02A8 || op == 0x04CE; }

static void RememberActiveBlip(uint16_t op, const uint8_t *cmd, int len)
{
    if (op == 0x0164) {   // REMOVE_BLIP : parametre 'B' (1 octet de genre + 4 de valeur) apres l'entete de 4 octets
        uint32_t h;
        memcpy(&h, cmd + 5, 4);
        for (int i = 0; i < g_activeBlipCount; i++)
            if (g_activeBlips[i].hostBlip == h) { g_activeBlips[i] = g_activeBlips[--g_activeBlipCount]; break; }
        return;
    }
    if (!CreatesBlip(op) || len > 64 || g_activeBlipCount >= 64) return;
    // La sortie 'b' est le dernier parametre : ses 4 derniers octets sont la reference du marqueur chez l'hote.
    ActiveBlip &b = g_activeBlips[g_activeBlipCount++];
    memcpy(&b.hostBlip, cmd + len - 4, 4);
    b.len = len;
    memcpy(b.cmd, cmd, len);
}

static void SendActiveBlips(int peer)
{
    for (int i = 0; i < g_activeBlipCount; i++) NetSendReliableTo(peer, g_activeBlips[i].cmd, g_activeBlips[i].len);
    if (g_activeBlipCount) Log("miroir : %d marqueurs en cours envoyes a un nouvel invite", g_activeBlipCount);
}

// ======================================================================= Hote : capture
struct ParamRef {
    char kind;
    uint8_t type;       // type SCM d'origine (1..6), 0 pour une etiquette
    int where;          // position de l'etiquette, ou decalage de variable globale, ou indice local
    int32_t literal;
};
static ParamRef g_params[24];
static int g_paramCount;
static const OpSig *g_pending;
static bool g_captureOnly;   // commande du script principal : on relit ses coordonnees sans l'envoyer
static int g_pendingIp;

// Lit la liste des parametres de la commande a ip (avant son execution). Faux si elle ne colle pas a la signature.
bool MirrorBefore(void *script, int ip, uint16_t op)
{
    g_pending = NULL;
    g_captureOnly = false;
    if (!Field<bool>(script, 0x85)) {
        // Script principal : pas reproduit, mais ses marqueurs d'objectif / de contact interessent l'autotest.
        if (op != 0x018A && op != 0x02A7) return false;
        g_captureOnly = true;
    }
    const OpSig *sig = FindOp(op);
    if (!sig) return false;
    if (sig->sig[0] == '*') { g_pending = sig; g_pendingIp = ip; return true; }
    uint8_t *ss = ScriptSpace();
    int at = ip + 2, n = 0;
    for (const char *k = sig->sig; *k; k++, n++) {
        ParamRef &p = g_params[n];
        p.kind = *k;
        uint8_t t = ss[at];
        if (*k == 'l') {
            if (t < 0x20) goto mismatch;
            p.type = 0; p.where = at; at += 8;
            continue;
        }
        p.type = t;
        switch (t) {
        case 1: case 6: memcpy(&p.literal, ss + at + 1, 4); at += 5; break;
        case 2: p.where = *(uint16_t *)(ss + at + 1); at += 3; break;
        case 3: p.where = *(uint16_t *)(ss + at + 1); at += 3; break;
        case 4: p.literal = (int8_t)ss[at + 1]; at += 2; break;
        case 5: p.literal = *(int16_t *)(ss + at + 1); at += 3; break;
        default: goto mismatch;
        }
        if ((*k == 'b' || *k == 'o' || *k == 'k') && t != 2 && t != 3) goto mismatch;   // une sortie est forcement une variable
    }
    g_paramCount = n;
    g_pending = sig;
    return true;
mismatch:
    static uint16_t warned[64];
    static int warnedCount;
    for (int i = 0; i < warnedCount; i++) if (warned[i] == op) return false;
    if (warnedCount < 64) warned[warnedCount++] = op;
    Log("miroir : %04X (%s) ne correspond pas a sa signature, non reproduit", op, sig->name);
    return false;
}

// Signature libre : relit tous les parametres entre la commande et la suivante (la commande ne saute pas).
static bool ParseFree(void *script, int ip)
{
    uint8_t *ss = ScriptSpace();
    int at = ip + 2, end = Field<int>(script, 0x10), n = 0;
    if (end <= at || end - at > 200) return at == end ? (g_paramCount = 0, true) : false;
    while (at < end && n < 24) {
        ParamRef &p = g_params[n++];
        uint8_t t = ss[at];
        p.kind = 'v';
        p.type = t;
        switch (t) {
        case 0: p.literal = 0; p.type = 0xFF; at += 1; break;   // fin de liste d'arguments
        case 1: case 6: memcpy(&p.literal, ss + at + 1, 4); at += 5; break;
        case 2: case 3: p.where = *(uint16_t *)(ss + at + 1); at += 3; break;
        case 4: p.literal = (int8_t)ss[at + 1]; at += 2; break;
        case 5: p.literal = *(int16_t *)(ss + at + 1); at += 3; break;
        default: p.kind = 'l'; p.type = 0; p.where = at; at += 8; break;
        }
    }
    g_paramCount = n;
    return at == end;
}

// Apres execution : lit les valeurs (les sorties sont maintenant remplies) et envoie la commande.
void MirrorAfter(void *script)
{
    if (!g_pending) return;
    const OpSig *sig = g_pending;
    g_pending = NULL;
    if (sig->sig[0] == '*' && !ParseFree(script, g_pendingIp)) {
        Log("miroir : parametres de %s illisibles, non reproduit", sig->name);
        return;
    }
    uint8_t buf[MAX_RELIABLE_PAYLOAD];
    int len = 0;
    buf[len++] = RL_SCRIPT_CMD;
    memcpy(buf + len, &sig->op, 2); len += 2;
    buf[len++] = (uint8_t)g_paramCount;
    for (int i = 0; i < g_paramCount; i++) {
        ParamRef &p = g_params[i];
        if (p.type == 0xFF) { buf[len++] = 'e'; continue; }   // fin de liste
        buf[len++] = (uint8_t)p.kind;
        if (p.kind == 'l') { memcpy(buf + len, ScriptSpace() + p.where, 8); len += 8; continue; }
        int32_t v = p.literal;
        if (p.type == 2) v = *(int32_t *)(ScriptSpace() + p.where);
        else if (p.type == 3) v = Field<int32_t>(script, 0x30 + p.where * 4);
        memcpy(buf + len, &v, 4); len += 4;
    }
    if (!g_captureOnly) {
        NetSendReliable(buf, len);
        RememberActiveBlip(sig->op, buf, len);
    }
    // Autotest : dernier objectif / point de contact poses par les missions (coordonnees x, y, z en tete).
    if (sig->op == 0x018A || sig->op == 0x02A7) {
        float c[3];
        for (int i = 0; i < 3; i++) {
            ParamRef &p = g_params[i];
            int32_t v = p.literal;
            if (p.type == 2) v = *(int32_t *)(ScriptSpace() + p.where);
            else if (p.type == 3) v = Field<int32_t>(script, 0x30 + p.where * 4);
            memcpy(&c[i], &v, 4);
        }
        MirrorPoint &mp = sig->op == 0x018A ? g_lastObjective : g_lastContact;
        mp.x = c[0]; mp.y = c[1]; mp.z = c[2]; mp.serial++;
        Log("miroir : %s en %.1f %.1f %.1f", sig->op == 0x018A ? "objectif" : "contact", c[0], c[1], c[2]);
    }
    if (g_cfg.logScripts && !g_captureOnly) Log("miroir : envoi %s", sig->name);
}

void MirrorMissionStart()
{
    memcpy(g_globSnap, ScriptSpace(), GLOBALS_END);
    g_globSnapValid = true;
    uint8_t b = RL_MISSION_START;
    NetSendReliable(&b, 1);
    Log("miroir : debut de mission envoye");
}

// Seules les valeurs "drapeau" (petits entiers) circulent : elles portent l'avancement de l'histoire. Les autres
// globales contiennent aussi des references d'entites (marqueurs, objets, pickups) propres a chaque machine, qu'il
// ne faut surtout pas ecraser, et des coordonnees que le script de l'invite calcule lui-meme.
// -1..255 : au-dela, une valeur peut etre une reference de pool (case << 8 | compteur, 256 des la case 1).
static bool IsFlagValue(uint32_t v) { return (int32_t)v >= -1 && (int32_t)v <= 255; }

// peer < 0 : a tous ; changedOnly : seulement ce qui differe de la photo (sinon tout ce qui est non nul).
static void SendGlobals(int peer, bool changedOnly)
{
    uint8_t buf[MAX_RELIABLE_PAYLOAD];
    int len = 1, total = 0;
    buf[0] = RL_GLOBALS;
    for (int off = GLOBALS_BEGIN; off + 4 <= GLOBALS_END; off += 4) {
        uint32_t now = *(uint32_t *)(ScriptSpace() + off);
        uint32_t before = *(uint32_t *)(g_globSnap + off);
        if (changedOnly ? (now == before || !IsFlagValue(before)) : now == 0) continue;
        if (!IsFlagValue(now)) continue;
        uint16_t o = (uint16_t)off;
        memcpy(buf + len, &o, 2); memcpy(buf + len + 2, &now, 4);
        len += 6; total++;
        if (len + 6 > MAX_RELIABLE_PAYLOAD) {
            if (peer < 0) NetSendReliable(buf, len); else NetSendReliableTo(peer, buf, len);
            len = 1;
        }
    }
    if (len > 1) { if (peer < 0) NetSendReliable(buf, len); else NetSendReliableTo(peer, buf, len); }
    Log("miroir : %d variables globales envoyees (%s)", total, changedOnly ? "changees par la mission" : "etat complet");
}

static void SendGlobalChanges()
{
    if (!g_globSnapValid) return;
    SendGlobals(-1, true);
    memcpy(g_globSnap, ScriptSpace(), GLOBALS_END);
}

// Hote : chaque invite recoit l'etat complet de l'histoire des que l'hote est en partie (a son arrivee, ou plus
// tard si l'hote etait encore au menu).
static bool g_synced[MAX_PLAYERS];
static bool g_sameSave[MAX_PLAYERS];   // l'invite va charger la sauvegarde que l'hote vient de charger
void MirrorGuestsGetHostSave() { for (int i = 1; i < MAX_PLAYERS; i++) if (g_players[i].connected) g_sameSave[i] = true; }
void MirrorPlayerJoined(int peer) { if (peer > 0 && peer < MAX_PLAYERS) g_synced[peer] = false; }

static void HostSyncNewcomers(bool inGame)
{
    // Nouvel invite : d'abord la sauvegarde de l'hote s'il en a charge une, puis (une fois l'invite en partie)
    // l'etat complet de l'histoire. Chaque retour en partie d'un invite (apres un chargement) le renvoie aussi.
    static bool saveSent[MAX_PLAYERS], wasInGame[MAX_PLAYERS];
    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (!g_players[i].connected) { saveSent[i] = wasInGame[i] = false; continue; }
        if (!saveSent[i] && !g_synced[i]) { saveSent[i] = true; if (HostHasSave()) { SendHostSave(i); g_sameSave[i] = true; } }
        bool in = g_players[i].state.inGame != 0;
        // Retour en partie apres avoir charge la sauvegarde de l'hote : memes variables que lui, rien a envoyer.
        if (in && !wasInGame[i]) { if (g_sameSave[i]) { g_sameSave[i] = false; g_synced[i] = true; } else g_synced[i] = false; }
        wasInGame[i] = in;
        if (inGame && in && !g_synced[i]) { g_synced[i] = true; SendGlobals(i, false); SendActiveBlips(i); }
    }
}

void MirrorMissionEnd()
{
    SendGlobalChanges();
    uint8_t b = RL_MISSION_END;
    NetSendReliable(&b, 1);
    Log("miroir : fin de mission envoyee");
}

// ======================================================================= Invite : rejeu
// Correspondances hote -> invite pour les marqueurs et objets crees par les commandes rejouees.
struct HandlePair { uint32_t host, guest; };
static HandlePair g_blips[128], g_objs[128], g_pickups[128];
static int g_blipCount, g_objCount, g_pickupCount;

static bool MapGet(HandlePair *m, int n, uint32_t host, uint32_t &guest)
{
    for (int i = 0; i < n; i++) if (m[i].host == host) { guest = m[i].guest; return true; }
    return false;
}

static void MapSet(HandlePair *m, int &n, int cap, uint32_t host, uint32_t guest)
{
    for (int i = 0; i < n; i++) if (m[i].host == host) { m[i].guest = guest; return; }
    if (n < cap) m[n++] = { host, guest };
}

static void MapDel(HandlePair *m, int &n, uint32_t host)
{
    for (int i = 0; i < n; i++) if (m[i].host == host) { m[i] = m[--n]; return; }
}

struct Pending { uint8_t data[MAX_RELIABLE_PAYLOAD]; int len; uint32_t since; };
enum { QUEUE_SIZE = 4096 };   // l'invite peut recevoir la mission 0 (~400 commandes) avant d'etre en partie
static Pending g_queue[QUEUE_SIZE];
static int g_qHead, g_qTail;   // anneau
static uint8_t g_script[0x100];  // notre CRunningScript prive
static bool g_scriptReady;
enum { SCRATCH = 0x370E8 + 0x40 };   // zone des missions de ScriptSpace : jamais utilisee chez l'invite

static bool Translate(char kind, uint32_t host, uint32_t &guest)
{
    switch (kind) {
    case 'M':
        if (host == g_hostPlayerHandle && FindPlayerPed()) { guest = PedHandle(FindPlayerPed()); return true; }
        return GuestPedForHost(host, guest);
    case 'P': return GuestPedForHost(host, guest);
    case 'C': return GuestVehicleForHost(host, guest);
    case 'O': return MapGet(g_objs, g_objCount, host, guest);
    case 'B': return MapGet(g_blips, g_blipCount, host, guest);
    case 'K': return MapGet(g_pickups, g_pickupCount, host, guest);
    }
    guest = host;
    return true;
}

// Rejoue une commande ; faux si une reference n'a pas encore d'equivalent local (on reessaiera).
static bool Execute(const uint8_t *d, int len, bool force)
{
    uint16_t op;
    memcpy(&op, d + 1, 2);
    int n = d[3], at = 4;
    uint8_t *ss = ScriptSpace();
    int w = SCRATCH;
    memcpy(ss + w, &op, 2); w += 2;
    struct Out { char kind; uint32_t host; int local; } outs[4];
    int outCount = 0;
    for (int i = 0; i < n; i++) {
        char kind = (char)d[at++];
        if (kind == 'e') { ss[w++] = 0; continue; }
        if (kind == 'l') { memcpy(ss + w, d + at, 8); w += 8; at += 8; continue; }
        uint32_t v;
        memcpy(&v, d + at, 4); at += 4;
        if (kind == 'b' || kind == 'o' || kind == 'k') {
            ss[w] = 3;                                   // variable locale de notre script
            *(uint16_t *)(ss + w + 1) = (uint16_t)outCount;
            w += 3;
            int idx = outCount++;
            outs[idx] = { kind, v, idx };
            continue;
        }
        // Teleportation du joueur par la mission : chaque invite est pose un peu a cote (pas sur l'hote).
        if (op == 0x0055 && i == 1) { float x; memcpy(&x, &v, 4); x += 1.5f * g_localId; memcpy(&v, &x, 4); }
        uint32_t g;
        if (!Translate(kind, v, g)) {
            if (!force) return false;
            g = (uint32_t)-1;
        }
        ss[w] = 1;
        memcpy(ss + w + 1, &g, 4);
        w += 5;
    }
    (void)len;

    if (!g_scriptReady) {
        ((void(__fastcall *)(void *))0x450CF0)(g_script);   // CRunningScript::Init
        memcpy(g_script + 8, "vccoop\0", 8);
        g_scriptReady = true;
    }
    memset(g_script + 0x30, 0, 16 * 4);
    Field<int>(g_script, 0x10) = SCRATCH;
    CallOriginalProcessOneCommand(g_script);

    for (int i = 0; i < outCount; i++) {
        uint32_t g = Field<uint32_t>(g_script, 0x30 + i * 4);
        if (outs[i].kind == 'b') MapSet(g_blips, g_blipCount, 128, outs[i].host, g);
        else if (outs[i].kind == 'k') MapSet(g_pickups, g_pickupCount, 128, outs[i].host, g);
        else MapSet(g_objs, g_objCount, 128, outs[i].host, g);
    }
    if (op == 0x0164) {   // REMOVE_BLIP : on oublie la correspondance
        uint32_t host;
        memcpy(&host, d + 5, 4);
        MapDel(g_blips, g_blipCount, host);
    }
    if (op == 0x0215) {   // REMOVE_PICKUP
        uint32_t host;
        memcpy(&host, d + 5, 4);
        MapDel(g_pickups, g_pickupCount, host);
    }
    if (op == 0x02EA) g_objCount = 0;   // CLEAR_CUTSCENE detruit les objets de la cinematique
    if (g_cfg.logScripts) { const OpSig *s = FindOp(op); Log("miroir : rejoue %s", s ? s->name : "?"); }
    return true;
}

// Fin de mission chez l'hote : ses scripts retirent eux-memes leurs marqueurs (commandes reproduites) et les
// marqueurs attaches aux entites disparaissent avec les copies. On oublie seulement les objets de cinematique.
// Construit et rejoue une commande locale a parametres entiers.
static void Local(uint16_t op, int n, const int32_t *vals)
{
    uint8_t cmd[64];
    int len = 0;
    cmd[len++] = RL_SCRIPT_CMD;
    memcpy(cmd + len, &op, 2); len += 2;
    cmd[len++] = (uint8_t)n;
    for (int i = 0; i < n; i++) { cmd[len++] = 'v'; memcpy(cmd + len, &vals[i], 4); len += 4; }
    Execute(cmd, len, true);
}

void MirrorLocal(uint16_t op, int n, const int32_t *vals) { Local(op, n, vals); }

static void MissionEnd()
{
    // Remet l'ecran comme le laisse la fin d'une mission (le dernier fondu est souvent fait par le script
    // principal de l'hote, qui n'est pas reproduit) : fondu d'entree, plus de bandes, controles, camera.
    int32_t fade[2] = { 500, 1 }, off[1] = { 0 }, control[2] = { 0, 1 }, slot1[1] = { 1 }, slot2[1] = { 2 };
    Local(0x016A, 2, fade);
    Local(0x00BE, 0, NULL);        // CLEAR_PRINTS : sous-titres restes a l'ecran
    Local(0x03E6, 0, NULL);        // CLEAR_HELP
    Local(0x040D, 1, slot1);       // CLEAR_MISSION_AUDIO 1 et 2
    Local(0x040D, 1, slot2);
    Local(0x02A3, 1, off);
    Local(0x01B4, 2, control);
    Local(0x02EB, 0, NULL);
    // Pickups poses par la mission : chez l'hote, le nettoyage de fin de mission les retire ; les notres ont ete crees
    // par notre script prive, on les retire nous-memes.
    for (int i = 0; i < g_pickupCount; i++) { int32_t h[1] = { (int32_t)g_pickups[i].guest }; Local(0x0215, 1, h); }
    g_pickupCount = 0;
    g_objCount = 0;
    RequestGather();
    Log("miroir : fin de mission chez l'hote");
}

static void OnReliable(int from, const uint8_t *data, int len)
{
    if (len < 1) return;
    if (data[0] >= 10) { CombatOnReliable(from, data, len); return; }   // combat.cpp
    if (g_cfg.host) return;
    int next = (g_qTail + 1) % QUEUE_SIZE;
    if (next == g_qHead) { Log("miroir : file pleine"); return; }
    Pending &p = g_queue[g_qTail];
    memcpy(p.data, data, len);
    p.len = len;
    p.since = 0;
    g_qTail = next;
}

void MirrorInit()
{
    g_onReliable = OnReliable;
    g_onJoin = MirrorPlayerJoined;
}

void MirrorFrame(bool inGame)
{
    if (g_cfg.host) { if (!inGame) g_activeBlipCount = 0; HostSyncNewcomers(inGame); return; }
    if (!inGame) { g_blipCount = g_objCount = g_pickupCount = 0; return; }
    uint32_t now = GetTickCount();
    while (g_qHead != g_qTail) {
        Pending &p = g_queue[g_qHead];
        if (!p.since) p.since = now;
        bool done;
        if (p.data[0] == RL_MISSION_END) { MissionEnd(); done = true; }
        else if (p.data[0] == RL_GLOBALS) {
            for (int at = 1; at + 6 <= p.len; at += 6) {
                uint16_t off; uint32_t v;
                memcpy(&off, p.data + at, 2); memcpy(&v, p.data + at + 2, 4);
                if (off >= GLOBALS_BEGIN && off + 4 <= GLOBALS_END) *(uint32_t *)(ScriptSpace() + off) = v;
            }
            Log("miroir : %d variables globales recues de l'hote", (p.len - 1) / 6);
            done = true;
        }
        else if (p.data[0] == RL_MISSION_START) { RequestGather(); Log("miroir : debut de mission chez l'hote"); done = true; }
        else if (p.data[0] == RL_SCRIPT_CMD) done = Execute(p.data, p.len, now - p.since > 3000);
        else done = true;
        if (!done) break;   // on garde l'ordre : la suite attend
        g_qHead = (g_qHead + 1) % QUEUE_SIZE;
    }
}
