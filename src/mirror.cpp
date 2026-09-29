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
//   g variable globale du script : on envoie son adresse (minuteurs / compteurs a l'ecran, lus en direct par le jeu)
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
    { 0x0109, "vv", "ADD_SCORE" },   // argent des missions (ArgentPartage) : chacun recoit la meme somme
    { 0x0394, "*", "PLAY_MISSION_PASSED_TUNE" },
    { 0x0318, "*", "SET_LATEST_MISSION_PASSED" },
    { 0x030C, "*", "PLAYER_MADE_PROGRESS" },
    { 0x0317, "*", "INCREMENT_MISSION_ATTEMPTS" },
    // Icones d'immeuble a vendre (Shakedown les recree) : sortie ecrite dans la globale de l'invite (kind 'z').
    { 0x0518, "vvvvlk", "CREATE_PROTECTION_PICKUP" },
    { 0x0213, "vvvvvk", "CREATE_PICKUP" },
    { 0x032B, "vvvvvvk", "CREATE_PICKUP_WITH_AMMO" },
    { 0x02E1, "vvvvk", "CREATE_MONEY_PICKUP" },
    { 0x0215, "K", "REMOVE_PICKUP" },
    // Objets poses par les missions (mallettes, bombes, portes...).
    { 0x0107, "vvvvo", "CREATE_OBJECT" },
    { 0x029B, "vvvvo", "CREATE_OBJECT_NO_OFFSET" },
    { 0x0108, "O", "DELETE_OBJECT" },
    { 0x0177, "Ov", "SET_OBJECT_HEADING" },
    { 0x01BC, "Ovvv", "SET_OBJECT_COORDINATES" },
    { 0x0453, "Ovvv", "SET_OBJECT_ROTATION" },
    { 0x0382, "Ov", "SET_OBJECT_COLLISION" },
    { 0x0392, "Ov", "MAKE_OBJECT_TARGETTABLE" },
    { 0x01C7, "O", "DONT_REMOVE_OBJECT" },
    // Declencheurs de mission : portes et grilles qui coulissent (objets, souvent ceux du script principal :
    // reference dans une globale -> 'g'), decor echange, garages ouverts/fermes (garage cree par le script
    // principal : reference dans une globale, la meme chez l'invite).
    { 0x034E, "Ovvvvvvv", "SLIDE_OBJECT" },
    { 0x0360, "g", "OPEN_GARAGE" },
    { 0x0361, "g", "CLOSE_GARAGE" },
    { 0x0299, "g", "ACTIVATE_GARAGE" },
    { 0x02B9, "g", "DEACTIVATE_GARAGE" },
    { 0x021B, "gC", "SET_TARGET_CAR_FOR_MISSION_GARAGE" },
    { 0x014C, "gv", "SWITCH_CAR_GENERATOR" },
    { 0x035C, "OCvvv", "PLACE_OBJECT_RELATIVE_TO_CAR" },
    { 0x0566, "Ov", "SET_OBJECT_AREA_VISIBLE" },
    { 0x01C4, "O", "MARK_OBJECT_AS_NO_LONGER_NEEDED" },
    // Textes, sons, explosions scriptees (auteur nul : chacun les subit chez lui une fois)
    { 0x03D5, "l", "CLEAR_THIS_PRINT" },
    { 0x03D6, "l", "CLEAR_THIS_BIG_PRINT" },
    { 0x03EB, "", "CLEAR_SMALL_PRINTS" },
    { 0x02FD, "lvvvv", "PRINT_WITH_2_NUMBERS_NOW" },
    { 0x018C, "vvvv", "ADD_ONE_OFF_SOUND" },
    { 0x020C, "vvvv", "ADD_EXPLOSION" },
    { 0x0565, "vvvv", "ADD_EXPLOSION_NO_SOUND" },
    { 0x04F7, "gvvl", "DISPLAY_NTH_ONSCREEN_COUNTER_WITH_STRING" },
    { 0x0396, "v", "FREEZE_ONSCREEN_TIMER" },
    // Marqueurs radar (anciennes variantes et pickups / objets)
    { 0x0167, "vvvvvb", "ADD_BLIP_FOR_COORD_OLD" },
    { 0x0161, "Cvvb", "ADD_BLIP_FOR_CAR_OLD" },
    { 0x0162, "Pvvb", "ADD_BLIP_FOR_CHAR_OLD" },
    { 0x0188, "Ob", "ADD_BLIP_FOR_OBJECT" },
    { 0x03DC, "Kb", "ADD_BLIP_FOR_PICKUP" },
    { 0x0570, "vvvvb", "ADD_SHORT_RANGE_SPRITE_BLIP_FOR_CONTACT_POINT" },
    { 0x0166, "Bv", "DIM_BLIP" },
    // Camera et cinematiques
    { 0x03C8, "", "SET_CAMERA_IN_FRONT_OF_PLAYER" },
    { 0x0003, "v", "SHAKE_CAM" },
    { 0x0460, "vv", "SET_INTERPOLATION_PARAMETERS" },
    { 0x04BC, "l", "SET_CUTSCENE_ANIM_TO_LOOP" },
    { 0x0569, "l", "LOAD_UNCOMPRESSED_ANIM" },
    { 0x03AD, "v", "SWITCH_RUBBISH" },
    { 0x04F9, "vv", "SET_EXTRA_COLOURS" },
    { 0x04FA, "v", "CLEAR_EXTRA_COLOURS" },
    // Joueur (chacun le sien : index 0 = soi) et monde
    { 0x0171, "vv", "SET_PLAYER_HEADING" },
    { 0x012A, "vvvv", "WARP_PLAYER_FROM_CAR_TO_COORD" },
    { 0x01F7, "vv", "SET_POLICE_IGNORE_PLAYER" },
    { 0x01F0, "v", "SET_MAX_WANTED_LEVEL" },
    { 0x01B1, "vvv", "GIVE_WEAPON_TO_PLAYER" },
    { 0x01B8, "vv", "SET_CURRENT_PLAYER_WEAPON" },
    { 0x016C, "vvvv", "ADD_HOSPITAL_RESTART" },
    { 0x016D, "vvvv", "ADD_POLICE_RESTART" },
    { 0x03F1, "vv", "SET_THREAT_FOR_PED_TYPE" },
    { 0x03F2, "vv", "CLEAR_THREAT_FOR_PED_TYPE" },
    { 0x01E7, "vvvvvv", "SWITCH_ROADS_ON" },
    { 0x01E8, "vvvvvv", "SWITCH_ROADS_OFF" },
    { 0x022A, "vvvvvv", "SWITCH_PED_ROADS_ON" },
    { 0x022B, "vvvvvv", "SWITCH_PED_ROADS_OFF" },
    // Vehicules et personnages de mission (copies)
    { 0x020A, "Cv", "LOCK_CAR_DOORS" },
    { 0x0568, "Pv", "SET_CHAR_NEVER_TARGETTED" },
    { 0x04F5, "Pvv", "SET_CHAR_AS_PLAYER_FRIEND" },
    // Fils du script principal lances par une mission (achats de commerces apres Shakedown...) : liste blanche.
    { 0x004F, "*", "START_NEW_SCRIPT" },
    { 0x03B6, "*", "SWAP_NEAREST_BUILDING_MODEL" },
    { 0x02FA, "gv", "CHANGE_GARAGE_TYPE" },
    // Minuteurs et compteurs de mission a l'ecran : le jeu lit la variable en direct (et decompte lui-meme le
    // minuteur) ; l'hote envoie la valeur de ces variables 3 fois par seconde (MSG_TIMERS).
    { 0x014E, "gv", "DISPLAY_ONSCREEN_TIMER" },
    { 0x014F, "g", "CLEAR_ONSCREEN_TIMER" },
    { 0x0150, "gv", "DISPLAY_ONSCREEN_COUNTER" },
    { 0x0151, "g", "CLEAR_ONSCREEN_COUNTER" },
    { 0x03C3, "gvl", "DISPLAY_ONSCREEN_TIMER_WITH_STRING" },
    { 0x03C4, "gvl", "DISPLAY_ONSCREEN_COUNTER_WITH_STRING" },
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

enum { RL_SCRIPT_CMD = 1, RL_MISSION_END = 2, RL_MISSION_START = 3, RL_GLOBALS = 4, RL_NEW_GAME = 5 };

// Variables globales du script principal : ScriptSpace [8, 0x8620) (le GOTO du debut de main.scm les enjambe).
// Les missions y posent leurs drapeaux (mission reussie, compteurs, deblocages) que le script principal lit pour
// avancer l'histoire. L'hote photographie cette zone au debut de chaque mission et envoie ce qui a change a la fin :
// le script principal de chaque invite avance alors comme le sien (points de contact, objectifs...).
enum { GLOBALS_BEGIN = 8, GLOBALS_END = 0x8620 };
static uint8_t g_globSnap[GLOBALS_END];
static bool g_globSnapValid;
static bool g_missionRunning;   // hote : une mission est en cours
static int g_hostMission;       // hote : son numero
// Missions de l'hote qui ne sont pas reproduites chez les invites : les missions secondaires de vehicule (taxi,
// ambulance, pompiers, justicier, pizza : chacun joue les siennes) et les achats d'immeubles (chacun achete les
// siens, avec son argent).
static bool IsSideMission(int m) { return m == 75 || m == 76 || m == 77 || m == 78 || m == 92 || IsChallengeMission(m); }
static bool IsBuyMission(int m) { return m >= 36 && m <= 50; }   // BUYPRO1..SKUMBUY (table de main.scm)
static bool g_hostQuiet;        // hote : la mission en cours n'est pas reproduite
bool MirrorHostQuiet() { return g_missionRunning && g_hostQuiet; }
// Variables "par joueur" : jamais envoyees aux invites. Les drapeaux de possession des immeubles (ecrits par les
// missions d'achat 39..50, releves dans main.scm, plus tout ce qu'une mission d'achat change chez l'hote).
static uint8_t g_perPlayer[0x8620 / 4 / 8 + 1];
static bool PerPlayer(int off) { return (g_perPlayer[(off / 4) >> 3] >> ((off / 4) & 7)) & 1; }
static void SetPerPlayer(int off) { g_perPlayer[(off / 4) >> 3] |= (uint8_t)(1 << ((off / 4) & 7)); }
static uint8_t g_buySnap[0x8620];
static void InitPerPlayer()
{
    static const int idx[] = { 338, 307, 1092, 999, 1271, 1303, 1798, 1304, 1799, 1300, 1795, 1305, 1800, 1301, 1796, 1302, 1797, 1299, 1794 };
    for (int i : idx) SetPerPlayer(i * 4);
}
// Sorties (marqueur, objet, pickup) des commandes reproduites : leurs references chez l'hote. Une reference d'entree
// qui n'en vient pas (entite creee par le script principal, dont l'invite a la sienne dans la meme globale) est
// envoyee comme adresse de globale ('g') : l'invite lit alors sa propre reference.
static uint32_t g_hostOuts[512];
static int g_hostOutAt;
static void RememberHostOut(uint32_t h) { g_hostOuts[g_hostOutAt++ % 512] = h; }
static bool IsHostOut(uint32_t h) { for (uint32_t x : g_hostOuts) if (x == h) return true; return false; }

static void TrackTimer(uint16_t op, uint16_t offset);
static int g_timerCount;

const OpSig *FindOp(uint16_t op)
{
    for (auto &o : g_ops) if (o.op == op) return &o;
    return NULL;
}

// ======================================================================= Hote : marqueurs actifs
// Les marqueurs radar poses par les missions et pas encore retires sont gardes (la commande telle qu'envoyee),
// pour les rejouer chez un invite qui arrive en cours de mission.
struct ActiveBlip { uint32_t hostBlip; uint16_t op; int len; uint8_t cmd[64]; };
static ActiveBlip g_activeBlips[64];
static int g_activeBlipCount;

// Icones fixes de la carte : points de contact des missions (lettre du commanditaire) et icones d'endroits. Une mission
// reussie ajoute souvent celui de la suivante dans son propre script : il doit survivre a la fin de la mission.
static bool PersistentBlip(uint16_t op) { return op == 0x02A7 || op == 0x0570 || op == 0x02A8 || op == 0x04CE; }

static bool CreatesBlip(uint16_t op)
{
    return op == 0x0186 || op == 0x0187 || op == 0x018A || op == 0x02A7 || op == 0x02A8 || op == 0x04CE ||
           op == 0x0167 || op == 0x0161 || op == 0x0162 || op == 0x0188 || op == 0x03DC || op == 0x0570;
}

static void RememberActiveBlip(uint16_t op, const uint8_t *cmd, int len, uint32_t h)
{
    if (op == 0x0164) {   // REMOVE_BLIP : h = reference du marqueur chez l'hote
        for (int i = 0; i < g_activeBlipCount; i++)
            if (g_activeBlips[i].hostBlip == h) { g_activeBlips[i] = g_activeBlips[--g_activeBlipCount]; break; }
        return;
    }
    if (!CreatesBlip(op) || len > 64 || g_activeBlipCount >= 64) return;
    ActiveBlip &b = g_activeBlips[g_activeBlipCount++];
    b.hostBlip = h;
    b.op = op;
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
    if (op == 0x0109 && !g_cfg.shareMoney) return false;
    if (g_missionRunning && (g_hostMission == 0 || g_hostQuiet) && !g_captureOnly) return false;   // INITIAL, secondaires, achats : chacun joue les siennes
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
        if (*k == 'g' && t != 2) goto mismatch;   // le jeu lui-meme suppose une globale
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
    uint32_t entity = 0;   // reference chez l'hote du marqueur cree ou retire (liste des marqueurs actifs)
    for (int i = 0; i < g_paramCount; i++) {
        ParamRef &p = g_params[i];
        if (p.type == 0xFF) { buf[len++] = 'e'; continue; }   // fin de liste
        if (p.kind == 'l') { buf[len++] = 'l'; memcpy(buf + len, ScriptSpace() + p.where, 8); len += 8; continue; }
        int32_t v = p.literal;
        char kind = p.kind;
        if (kind == 'g') v = p.where;
        else if (p.type == 2) v = *(int32_t *)(ScriptSpace() + p.where);
        else if (p.type == 3) v = Field<int32_t>(script, 0x30 + p.where * 4);
        if (kind == 'B' || kind == 'b') entity = (uint32_t)v;
        // Objet / marqueur / pickup du script principal (pas une sortie reproduite) : l'invite a le sien dans la
        // meme globale.
        if ((kind == 'O' || kind == 'B' || kind == 'K') && p.type == 2 && !IsHostOut((uint32_t)v)) { kind = 'g'; v = p.where; }
        if (kind == 'b' || kind == 'o' || kind == 'k') {
            RememberHostOut((uint32_t)v);
            if (p.type == 2) {   // sortie dans une globale : l'invite y ecrira sa propre reference ('x' 'y' 'z')
                buf[len++] = (uint8_t)(kind == 'b' ? 'x' : kind == 'o' ? 'y' : 'z');
                memcpy(buf + len, &v, 4); len += 4;
                uint16_t o = (uint16_t)p.where;
                memcpy(buf + len, &o, 2); len += 2;
                continue;
            }
        }
        buf[len++] = (uint8_t)kind;
        memcpy(buf + len, &v, 4); len += 4;
    }
    // Une mission qui retire en boucle un marqueur deja retire (law2) : on ne l'envoie qu'une fois.
    static uint32_t removed[32];
    static int removedAt;
    if (sig->op == 0x0164 && entity) {
        for (uint32_t r : removed) if (r == entity) return;
        removed[removedAt++ % 32] = entity;
    } else if (CreatesBlip(sig->op) && entity) {
        for (uint32_t &r : removed) if (r == entity) r = 0;   // reference reutilisee par un nouveau marqueur
    }
    if (!g_captureOnly) {
        NetSendReliable(buf, len);
        RememberActiveBlip(sig->op, buf, len, entity);
        uint16_t o = sig->op;
        if (g_params[0].kind == 'g' && (o == 0x014E || o == 0x014F || o == 0x0150 || o == 0x0151 || o == 0x03C3 || o == 0x03C4 || o == 0x04F7)) TrackTimer(o, (uint16_t)g_params[0].where);
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

// Photo des variables : prise a l'entree en partie, puis rafraichie a chaque envoi de changements. Elle n'est plus
// reprise au debut de chaque mission : ce que le script principal change entre deux missions (achat d'une
// propriete, appel telephonique qui debloque une mission...) etait alors oublie et n'arrivait jamais aux invites.

// Regroupement des invites pres de l'hote au debut et a la fin d'une mission : seulement si l'hote est a pied. Les
// missions secondaires (taxi, pizza, ambulance, justicier...) se lancent et se terminent en vehicule : les invites
// n'ont pas a etre teleportes pour celles-la.
static uint8_t GatherFlag() { void *me = FindPlayerPed(); return me && !InVehicle(me) ? 1 : 0; }

void MirrorMissionStart(int mission)
{
    if (!g_globSnapValid) { memcpy(g_globSnap, ScriptSpace(), GLOBALS_END); g_globSnapValid = true; }
    g_missionRunning = true;
    g_hostMission = mission;
    g_hostQuiet = IsSideMission(mission) || IsBuyMission(mission);
    if (g_hostQuiet) {
        if (IsBuyMission(mission)) memcpy(g_buySnap, ScriptSpace(), GLOBALS_END);
        Log("miroir : mission %d (%s) jouee par l'hote seul, non reproduite", mission, IsBuyMission(mission) ? "achat d'immeuble" : "secondaire");
        return;
    }
    uint8_t b[4] = { RL_MISSION_START, GatherFlag(), (uint8_t)mission, (uint8_t)(mission >> 8) };
    NetSendReliable(b, 4);
    Log("miroir : debut de mission %d envoye (regroupement %d)", mission, b[1]);
}

// Seules les valeurs "drapeau" (petits entiers) circulent : elles portent l'avancement de l'histoire. Les autres
// globales contiennent aussi des references d'entites (marqueurs, objets, pickups) propres a chaque machine, qu'il
// ne faut surtout pas ecraser, et des coordonnees que le script de l'invite calcule lui-meme.
// -1..255 : au-dela, une valeur peut etre une reference de pool (case << 8 | compteur, 256 des la case 1).
// Une reference de pool de la case 0 vaut 1..127 (identifiant sur 7 bits) : si c'est un personnage ou un vehicule
// vivant chez l'hote, ce n'est pas un drapeau (l'invite l'aurait prise pour une de ses entites : plantage).
static bool LiveSlot0Handle(uint32_t v)
{
    if (v < 1 || v > 127) return false;
    Pool *vp = VehiclePool(), *pp = PedPool();
    return (vp && vp->size > 0 && vp->flags[0] == (uint8_t)v) || (pp && pp->size > 0 && pp->flags[0] == (uint8_t)v);
}
static bool IsFlagValue(uint32_t v) { return (int32_t)v >= -1 && (int32_t)v <= 255 && !LiveSlot0Handle(v); }

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
        if (!IsFlagValue(now) || PerPlayer(off)) continue;
        uint16_t o = (uint16_t)off;
        memcpy(buf + len, &o, 2); memcpy(buf + len + 2, &now, 4);
        len += 6; total++;
        if (len + 6 > MAX_RELIABLE_PAYLOAD) {
            if (peer < 0) NetSendReliable(buf, len); else NetSendReliableTo(peer, buf, len);
            len = 1;
        }
    }
    if (len > 1) { if (peer < 0) NetSendReliable(buf, len); else NetSendReliableTo(peer, buf, len); }
    if (total || !changedOnly) Log("miroir : %d variables globales envoyees (%s)", total, changedOnly ? "changees" : "etat complet");
}

static void SendGlobalChanges()
{
    if (!g_globSnapValid) return;
    SendGlobals(-1, true);
    memcpy(g_globSnap, ScriptSpace(), GLOBALS_END);
}

// Entre deux missions : seulement les variables qui ont change ET ne bougent plus depuis le passage precedent
// (les compteurs et minuteurs du script principal changent sans arret, ils ne portent pas l'histoire).
static uint8_t g_globPrev[GLOBALS_END];
static void SendStableChanges()
{
    if (!g_globSnapValid) return;
    uint8_t buf[MAX_RELIABLE_PAYLOAD];
    int len = 1, total = 0;
    buf[0] = RL_GLOBALS;
    for (int off = GLOBALS_BEGIN; off + 4 <= GLOBALS_END; off += 4) {
        uint32_t now = *(uint32_t *)(ScriptSpace() + off), sent = *(uint32_t *)(g_globSnap + off), prev = *(uint32_t *)(g_globPrev + off);
        if (now == sent || now != prev || !IsFlagValue(now) || !IsFlagValue(sent) || PerPlayer(off)) continue;
        *(uint32_t *)(g_globSnap + off) = now;
        uint16_t o = (uint16_t)off;
        memcpy(buf + len, &o, 2); memcpy(buf + len + 2, &now, 4);
        len += 6; total++;
        if (len + 6 > MAX_RELIABLE_PAYLOAD) { NetSendReliable(buf, len); len = 1; }
    }
    if (len > 1) NetSendReliable(buf, len);
    memcpy(g_globPrev, ScriptSpace(), GLOBALS_END);
    if (total) Log("miroir : %d variables globales envoyees (changees hors mission)", total);
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
        // Retour apres une coupure : il est deja dans notre partie, on ne lui fait pas tout recharger.
        // Un invite qui arrive alors que l'hote joue deja recoit la sauvegarde telle qu'elle a ete chargee : elle a
        // pu vieillir (missions faites depuis), il aura donc aussi l'etat complet des variables une fois en partie.
        if (!saveSent[i] && !g_synced[i]) { saveSent[i] = true; if (HostHasSave() && !g_peerRejoin[i]) { SendHostSave(i); if (!inGame) g_sameSave[i] = true; } }
        if (g_peerRejoin[i] && g_players[i].state.inGame) { g_peerRejoin[i] = false; wasInGame[i] = true; g_synced[i] = false; }
        bool in = g_players[i].state.inGame != 0;
        // Retour en partie apres avoir charge la sauvegarde de l'hote : memes variables que lui, rien a envoyer.
        if (in && !wasInGame[i]) { if (g_sameSave[i]) { g_sameSave[i] = false; g_synced[i] = true; } else g_synced[i] = false; }
        wasInGame[i] = in;
        if (inGame && in && !g_synced[i]) { g_synced[i] = true; SendGlobals(i, false); SendActiveBlips(i); }
    }
}

void MirrorMissionEnd()
{
    g_missionRunning = false;
    if (g_hostQuiet) {
        g_hostQuiet = false;
        g_timerCount = 0;
        if (IsBuyMission(g_hostMission)) {
            // Ce que la mission d'achat a change (drapeaux de possession) reste a l'hote : jamais envoye.
            int n = 0;
            for (int off = GLOBALS_BEGIN; off + 4 <= GLOBALS_END; off += 4)
                if (off != 313 * 4 && *(uint32_t *)(ScriptSpace() + off) != *(uint32_t *)(g_buySnap + off) && !PerPlayer(off)) { SetPerPlayer(off); n++; }
            Log("miroir : mission d'achat %d finie, %d variables gardees par joueur", g_hostMission, n);
        }
        SendGlobalChanges();
        Log("miroir : fin de la mission %d (non reproduite)", g_hostMission);
        return;
    }
    SendGlobalChanges();
    uint8_t b[4] = { RL_MISSION_END, GatherFlag(), (uint8_t)g_hostMission, (uint8_t)(g_hostMission >> 8) };
    NetSendReliable(b, 4);
    // Le nettoyage de fin de mission du jeu retire les marqueurs et les minuteurs : un invite qui arrive apres n'a
    // rien a rejouer.
    // INITIAL : ses marqueurs restent (boutiques...) ; sinon seules les icones fixes (contacts des missions suivantes).
    if (g_hostMission != 0) {
        int kept = 0;
        for (int i = 0; i < g_activeBlipCount; i++) if (PersistentBlip(g_activeBlips[i].op)) g_activeBlips[kept++] = g_activeBlips[i];
        g_activeBlipCount = kept;
    }
    g_timerCount = 0;
    Log("miroir : fin de la mission %d envoyee (regroupement %d)", g_hostMission, b[1]);
}

// ======================================================================= Hote : minuteurs / compteurs a l'ecran
// Variables globales affichees par DISPLAY_ONSCREEN_TIMER / COUNTER : leur valeur part aux invites 3 fois par
// seconde (sans accuse : la suivante corrige), tant que l'affichage n'est pas efface.
static struct { uint16_t offset; bool clock; } g_timers[8];

static void TrackTimer(uint16_t op, uint16_t offset)
{
    bool clear = op == 0x014F || op == 0x0151;
    for (int i = 0; i < g_timerCount; i++)
        if (g_timers[i].offset == offset) { if (clear) g_timers[i] = g_timers[--g_timerCount]; return; }
    if (!clear && g_timerCount < 8) g_timers[g_timerCount++] = { offset, op == 0x014E || op == 0x03C3 };
}

static void SendTimers()
{
    static uint32_t last;
    if (!g_timerCount || GetTickCount() - last < 300) return;
    last = GetTickCount();
    uint8_t buf[2 + 8 * 6];
    buf[0] = MSG_TIMERS;
    buf[1] = (uint8_t)g_timerCount;
    for (int i = 0; i < g_timerCount; i++) {
        memcpy(buf + 2 + i * 6, &g_timers[i].offset, 2);
        memcpy(buf + 4 + i * 6, ScriptSpace() + g_timers[i].offset, 4);
    }
    NetSendToGuests(buf, 2 + g_timerCount * 6);
}

// Invite : les valeurs recues vont directement dans nos variables (le jeu les affiche et decompte les minuteurs).
void MirrorOnTimers(const uint8_t *buf, int len)
{
    if (g_cfg.host || len < 2 || GameState() != GS_PLAYING) return;
    int n = buf[1];
    if (len < 2 + n * 6) return;
    for (int i = 0; i < n; i++) {
        uint16_t off;
        memcpy(&off, buf + 2 + i * 6, 2);
        if (off >= GLOBALS_BEGIN && off + 4 <= GLOBALS_END) memcpy(ScriptSpace() + off, buf + 4 + i * 6, 4);
    }
}

// ======================================================================= Invite : rejeu
// Correspondances hote -> invite pour les marqueurs et objets crees par les commandes rejouees.
struct HandlePair { uint32_t host, guest; bool keep; };   // keep : icone fixe, gardee en fin de mission
static HandlePair g_blips[128], g_objs[128], g_pickups[128], g_props[32];
static int g_blipCount, g_objCount, g_pickupCount, g_propCount;

static bool MapGet(HandlePair *m, int n, uint32_t host, uint32_t &guest)
{
    for (int i = 0; i < n; i++) if (m[i].host == host) { guest = m[i].guest; return true; }
    return false;
}

static void MapSet(HandlePair *m, int &n, int cap, uint32_t host, uint32_t guest, bool keep = false)
{
    for (int i = 0; i < n; i++) if (m[i].host == host) { m[i].guest = guest; m[i].keep = keep; return; }
    if (n < cap) m[n++] = { host, guest, keep };
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
// Brouillon ou l'on ecrit la commande a rejouer : la FIN de la zone des missions de ScriptSpace. Le jeu charge
// chaque mission a +0x370E8 (35000 octets reserves, la plus grosse de main.scm en fait 32408) ; ecrire au debut de
// cette zone corrompait le code de la mission en cours (celle de l'hote, ou INITIAL / une mission secondaire /
// un achat d'immeuble chez l'invite).
enum { SCRATCH = 0x370E8 + 35000 - 0x100 };

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
    case 'K': return MapGet(g_pickups, g_pickupCount, host, guest) || MapGet(g_props, g_propCount, host, guest);
    }
    guest = host;
    return true;
}

// Rejoue une commande ; faux si une reference n'a pas encore d'equivalent local (on reessaiera).
// Zone visible (SET_AREA_VISIBLE 04BB : interieur ou ville ; les batiments des autres zones sont decharges) :
// chez l'invite, seulement s'il est emmene avec l'hote (teleportation de la mission juste avant ou juste apres).
// Resté dehors, il perdait sa ville quand l'hote entrait dans l'hotel, et la gardait perdue si c'etait le script
// principal de l'hote (non reproduit) qui remettait la ville.
static void Local(uint16_t op, int n, const int32_t *vals);
static int g_pendingArea = -1;
static uint32_t g_pendingAreaAt, g_lastTeleportAt;
static int g_mirrorArea;   // zone posee par nous (-1 : la zone courante vient du jeu, portes)
static void SetArea(int area)
{
    if (*(int *)0x978810 == area) return;   // CGame::currArea
    int32_t v[1] = { area };
    Local(0x04BB, 1, v);
    g_mirrorArea = area;
    if (void *me = FindPlayerPed()) AreaCode(me) = (uint8_t)area;   // notre Tommy dans la meme zone que la ville affichee
    Log("miroir : zone visible %d (avec l'hote)", area);
}
void MirrorFollowHostArea(int area) { SetArea(area); }   // coop.cpp : regroupement pres de l'hote

// Minuteurs / compteurs affiches chez nous sur ordre de l'hote : effaces a la fin de sa mission (chez lui, c'est le
// nettoyage de fin de mission du jeu qui le fait).
static struct { uint16_t offset; bool clock; } g_guestTimers[8];
static int g_guestTimerCount;
static void GuestTrackTimer(uint16_t op, uint16_t offset)
{
    bool clear = op == 0x014F || op == 0x0151;
    for (int i = 0; i < g_guestTimerCount; i++)
        if (g_guestTimers[i].offset == offset) { if (clear) g_guestTimers[i] = g_guestTimers[--g_guestTimerCount]; return; }
    if (!clear && g_guestTimerCount < 8) g_guestTimers[g_guestTimerCount++] = { offset, op == 0x014E || op == 0x03C3 };
}

// Animation de cinematique cherchable sans risque : le groupe du jeu (CCutsceneMgr::ms_cutsceneAssociations, 0x97867C)
// peut garder des entrees vides (modele introuvable au chargement) que sa recherche (0x401190) lit sans verifier.
static bool CutsceneAnimSafe(const uint8_t *label)
{
    char name[9];
    memcpy(name, label, 8); name[8] = 0;
    uint8_t *g = (uint8_t *)0x97867C;
    uint8_t *list = *(uint8_t **)(g + 4);
    int n = *(int *)(g + 8);
    if (!*(uint8_t *)0xA10B51 || !list || n <= 0 || n > 512) return false;   // CCutsceneMgr::ms_loaded
    for (int i = 0; i < n; i++) {
        const char *h = *(const char **)(list + i * 0x3C + 0x14);
        if (!h) return false;
        if (!_stricmp(h, name)) return true;
    }
    return false;
}

// Premier texte (etiquette de 8 octets) d'une commande recue.
static const uint8_t *FirstLabel(const uint8_t *d, int len)
{
    int n = d[3], at = 4;
    for (int i = 0; i < n && at < len; i++) {
        char k = (char)d[at++];
        if (k == 'l') return at + 8 <= len ? d + at : NULL;
        if (k == 'e') continue;
        at += (k == 'x' || k == 'y' || k == 'z') ? 6 : 4;
    }
    return NULL;
}

static bool Execute(const uint8_t *d, int len, bool force)
{
    uint16_t op;
    memcpy(&op, d + 1, 2);
    int n = d[3], at = 4;
    if (op == 0x04BB && d[0] == RL_SCRIPT_CMD && n >= 1 && d[4] == 'v' && !force) {
        int32_t area;
        memcpy(&area, d + 5, 4);
        // Retour a la ville chez l'hote alors que c'est nous (le miroir) qui avions mis l'invite dans un interieur :
        // on le suit toujours, sinon il restait dans une zone sans ville (cinematique en interieur, sortie par une
        // commande non reproduite).
        if (area == 0 && g_mirrorArea > 0 && *(int *)0x978810 == g_mirrorArea) SetArea(0);
        else if (GetTickCount() - g_lastTeleportAt < 2000) SetArea(area);
        else { g_pendingArea = area; g_pendingAreaAt = GetTickCount(); }
        return true;
    }
    // Objet : son modele doit etre charge (REQUEST_MODEL est rejoue avant, mais le chargement prend un moment).
    // Un numero negatif designe un objet par son nom dans la table du script : le jeu le traduit lui-meme.
    if ((op == 0x0107 || op == 0x029B) && d[0] == RL_SCRIPT_CMD && n >= 1 && d[4] == 'v') {
        int32_t model;
        memcpy(&model, d + 5, 4);
        if (model >= 0 && !HasModelLoaded(model)) {
            RequestModel(model, 1 | 8);
            if (!force) return false;
            Log("miroir : modele %d pas charge, objet non cree", model);
            return true;
        }
    }
    // En pleine course de taxi (ou autre mission secondaire jouee ici) : la mission de l'hote ne nous teleporte pas.
    if ((op == 0x0055 || op == 0x012A) && d[0] == RL_SCRIPT_CMD && GuestSideMission()) { Log("miroir : teleportation de l'hote ignoree (mission secondaire en cours)"); return true; }
    // Objet que la mission de l'hote ne gere plus : il reste chez lui (pas detruit a la fin de mission), donc chez nous aussi.
    if (op == 0x01C4 && d[0] == RL_SCRIPT_CMD && n >= 1) { uint32_t h; memcpy(&h, d + 5, 4); MapDel(g_objs, g_objCount, h); return true; }
    // Eclairage d'interieur : seulement si l'on est dans un interieur (on suit la zone de l'hote).
    if (op == 0x04F9 && d[0] == RL_SCRIPT_CMD && *(int *)0x978810 == 0) return true;
    // Arme donnee : son modele doit etre charge (sinon plantage dans les animations, 0x4056A3).
    if (op == 0x01B1 && d[0] == RL_SCRIPT_CMD && n >= 2 && d[9] == 'v') {
        int32_t w; memcpy(&w, d + 10, 4);
        if (w > 0 && w < 40) {
            int model = *(int *)(0x782A14 + w * 0x64 + 0x54);
            if (model > 0 && !HasModelLoaded(model)) { RequestModel(model, 1); ((void(__cdecl *)(bool))0x40B5F0)(false); }
            if (model > 0 && !HasModelLoaded(model)) { if (!force) return false; Log("miroir : modele d'arme %d pas charge, arme non donnee", model); return true; }
        }
    }
    // Objet de cinematique : son modele doit etre charge (sinon SetModelIndex sans modele : plantage 0x4E0450, vu a
    // l'arrivee d'un invite pendant la cinematique d'intro de l'hote). Charge ici, sinon rejoue plus tard, sinon laisse.
    if (op == 0x02E5 && d[0] == RL_SCRIPT_CMD && n >= 1 && d[4] == 'v') {
        int32_t model; memcpy(&model, d + 5, 4);
        if (model > 0 && model < 6500 && !HasModelLoaded(model)) { RequestModel(model, 1); ((void(__cdecl *)(bool))0x40B5F0)(false); }
        // Personnage special (109-129) ou objet de cinematique (295-299) : l'emplacement peut se dire charge sans modele
        // (invite arrive apres les LOAD_SPECIAL_CHARACTER de l'hote) : on verifie le modele lui-meme (m_clump, +0x28).
        bool clumpSlot = (model >= 109 && model <= 129) || (model >= 295 && model <= 299);
        void *mi = model > 0 && model < 6500 ? ModelInfo(model) : NULL;
        bool ready = mi && HasModelLoaded(model) && (!clumpSlot || *(void **)((uint8_t *)mi + 0x28));
        if (!ready) { if (!force) return false; Log("miroir : modele %d de la cinematique pas charge, objet non cree", model); return true; }
    }
    // Animation d'un objet de cinematique : seulement si le groupe la contient sans entree vide avant elle (sinon
    // plantage 0x401212 : l'objet reste simplement immobile).
    if ((op == 0x02E6 || op == 0x02F5 || op == 0x04BC) && d[0] == RL_SCRIPT_CMD) {
        const uint8_t *label = FirstLabel(d, len);
        if (!label || !CutsceneAnimSafe(label)) {
            Log("miroir : animation de cinematique %.8s introuvable (%04X, cinematique %s chargee %d), ignoree", label ? (const char *)label : "?", op, (const char *)0x7EFE08, *(uint8_t *)0xA10B51);
            return true;
        }
    }
    if (op == 0x02E4 && d[0] == RL_SCRIPT_CMD) { const uint8_t *label = FirstLabel(d, len); Log("miroir : cinematique %.8s de l'hote", label ? (const char *)label : "?"); }
    // Son de mission (dialogue, telephone) : l'hote attend HAS_MISSION_AUDIO_LOADED avant de le jouer ; chez nous le
    // LOAD rejoue juste avant n'a souvent pas fini : on attend qu'il le soit (3 s au plus), sinon le son etait perdu.
    if (op == 0x03D1 && d[0] == RL_SCRIPT_CMD && n >= 1 && d[4] == 'v' && !force) {
        int32_t slot; memcpy(&slot, d + 5, 4);
        if (slot >= 1 && slot <= 2 && ((char(__thiscall *)(void *, int))0x5F9800)((void *)0x78D718, slot - 1) != 1) return false;
    }
    // Fils du script principal : seulement ceux que rien d'autre ne lance chez l'invite, et une seule fois.
    if (op == 0x004F && d[0] == RL_SCRIPT_CMD && n >= 1 && d[4] == 'v') {
        static const struct { int32_t label; const char *name; } allowed[] = {
            { 51691, "coubuy" }, { 51785, "carbuy" }, { 51925, "pornbuy" }, { 52019, "icebuy" }, { 52167, "taxibuy" },
            { 52315, "bankbuy" }, { 52409, "boatbuy" }, { 52549, "strpbuy" }, { 67994, "psave2" }, { 57959, "shoot" },
            { 53723, "gangmem" }, { 49438, "ambbank" } };
        int32_t label; memcpy(&label, d + 5, 4);
        const char *name = NULL;
        for (auto &a : allowed) if (a.label == label) name = a.name;
        if (!name) return true;
        for (void *sc = ActiveScripts(); sc; sc = Field<void *>(sc, 0))
            if (!_strnicmp((char *)sc + 8, name, 8)) return true;   // deja la (sauvegarde chargee)
        Log("miroir : fil %s lance chez nous (comme chez l'hote)", name);
    }
    uint8_t *ss = ScriptSpace();
    int w = SCRATCH;
    memcpy(ss + w, &op, 2); w += 2;
    struct Out { char kind; uint32_t host; int local; int gofs; } outs[4];
    int outCount = 0;
    for (int i = 0; i < n; i++) {
        char kind = (char)d[at++];
        if (kind == 'e') { ss[w++] = 0; continue; }
        if (kind == 'l') { memcpy(ss + w, d + at, 8); w += 8; at += 8; continue; }
        uint32_t v;
        memcpy(&v, d + at, 4); at += 4;
        if (kind == 'g') {   // adresse d'une globale : la meme chez nous
            ss[w] = 2;
            *(uint16_t *)(ss + w + 1) = (uint16_t)v;
            w += 3;
            if (op == 0x014E || op == 0x014F || op == 0x0150 || op == 0x0151 || op == 0x03C3 || op == 0x03C4 || op == 0x04F7) GuestTrackTimer(op, (uint16_t)v);
            continue;
        }
        if (kind == 'b' || kind == 'o' || kind == 'k' || kind == 'x' || kind == 'y' || kind == 'z') {
            int gofs = -1;
            if (kind == 'x' || kind == 'y' || kind == 'z') {   // sortie a ecrire aussi dans notre globale
                uint16_t o;
                memcpy(&o, d + at, 2); at += 2;
                gofs = o;
                kind = kind == 'x' ? 'b' : kind == 'y' ? 'o' : 'k';
            }
            ss[w] = 3;                                   // variable locale de notre script
            *(uint16_t *)(ss + w + 1) = (uint16_t)outCount;
            w += 3;
            int idx = outCount++;
            outs[idx] = { kind, v, idx, gofs };
            continue;
        }
        // Teleportation du joueur par la mission : chaque invite est pose un peu a cote (pas sur l'hote).
        if ((op == 0x0055 || op == 0x012A) && i == 1) { float x; memcpy(&x, &v, 4); x += 1.5f * g_localId; memcpy(&v, &x, 4); }
        uint32_t g;
        if (!Translate(kind, v, g)) {
            // Suppression (marqueur, pickup, objet...) de quelque chose qu'on n'a pas : rien a faire, tout de suite.
            // Avant, chacune attendait 3 s ; une mission qui retire le meme marqueur en boucle (law2) remplissait la
            // file plus vite qu'elle ne se vidait, et plus rien ne passait derriere (objectifs, cinematiques, fin de
            // mission) pour le reste de la partie. File en retard (40 commandes) : on n'attend plus non plus.
            bool removal = op == 0x0164 || op == 0x0215 || op == 0x0108 || op == 0x01C4 || op == 0x009B || op == 0x00A6 || op == 0x01C2 || op == 0x01C3;
            int backlog = (g_qTail - g_qHead + QUEUE_SIZE) % QUEUE_SIZE;
            if (!force && !removal && backlog < 40) return false;
            // Pas d'equivalent chez nous : la commande est sautee. (Avant : executee avec -1, que CPool::GetAt ne
            // verifie pas : pointeur faux, plantage possible dans le gestionnaire d'opcode.)
            static uint32_t lastLog;
            if (!removal || GetTickCount() - lastLog > 10000) {
                lastLog = GetTickCount();
                const OpSig *s = FindOp(op);
                Log("miroir : %s ignoree (reference %c %08X introuvable chez nous, file %d)", s ? s->name : "?", kind, v, backlog);
            }
            return true;
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
    if (op == 0x0055 || op == 0x012A) {   // teleporte avec l'hote : sa zone visible (recue juste avant) s'applique
        g_lastTeleportAt = GetTickCount();
        CoopWatchWalls();   // decale de l'hote : peut-etre dans un mur (interieur pas encore charge)
        if (g_pendingArea >= 0 && GetTickCount() - g_pendingAreaAt < 5000) SetArea(g_pendingArea);
        g_pendingArea = -1;
    }

    for (int i = 0; i < outCount; i++) {
        uint32_t g = Field<uint32_t>(g_script, 0x30 + i * 4);
        if (outs[i].gofs >= GLOBALS_BEGIN && outs[i].gofs + 4 <= GLOBALS_END) *(uint32_t *)(ss + outs[i].gofs) = g;
        if (outs[i].kind == 'k' && op == 0x0518) {   // icone "a vendre" : permanente, achetable par nous
            MapSet(g_props, g_propCount, 32, outs[i].host, g);
            RegisterPropertyPickup(g);
            continue;
        }
        if (outs[i].kind == 'b') {
            MapSet(g_blips, g_blipCount, 128, outs[i].host, g, PersistentBlip(op));
            if (PersistentBlip(op)) Log("miroir : icone fixe %04X de l'hote (%08X), gardee apres la mission", op, g);
        }
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
    if (op == 0x02EA) {   // CLEAR_CUTSCENE detruit les objets de la cinematique
        g_objCount = 0;
        // Le LOAD_SCENE de l'hote vise l'endroit ou IL est : si on n'y est pas, le decor autour de nous pouvait rester
        // absent un moment apres la cinematique (GG, 29/09, apres LAW_2B). On charge aussi le notre.
        if (void *me = FindPlayerPed()) {
            float xyz[3] = { Pos(me).x, Pos(me).y, Pos(me).z };
            int32_t p[3];
            memcpy(p, xyz, 12);
            Local(0x03CB, 3, p);   // LOAD_SCENE
            Log("miroir : fin de cinematique, decor charge autour de nous");
        }
    }
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

// Invite : l'hote est en mission (debut recu, pas encore la fin). Pendant ce temps, pas de defi ni de mission
// secondaire lancee chez nous (script.cpp) : la camionnette Top Fun reprise par un invite lui lancait sa propre
// course RC pendant celle de l'hote, et son jeu passait en voiture telecommandee.
static bool g_hostOnMission;
bool HostOnMission() { return g_hostOnMission; }
bool MissionUnderway() { return g_cfg.host ? (g_missionRunning && g_hostMission != 0) : (g_hostOnMission || GuestSideMission()); }

static void MissionEnd(bool gather, int mission)
{
    g_hostOnMission = false;
    // Remet l'ecran comme le laisse la fin d'une mission (le dernier fondu est souvent fait par le script
    // principal de l'hote, qui n'est pas reproduit) : fondu d'entree, plus de bandes, controles, camera.
    int32_t fade[2] = { 500, 1 }, off[1] = { 0 }, control[2] = { 0, 1 }, slot1[1] = { 1 }, slot2[1] = { 2 };
    Local(0x016A, 2, fade);
    Local(0x03EB, 0, NULL);        // CLEAR_SMALL_PRINTS : sous-titres restes a l'ecran (pas les grands messages : "Mission accomplie", "Mission echouee")
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
    // Objets, marqueurs radar et minuteurs de la mission : le nettoyage de fin de mission du jeu les retire chez
    // l'hote sans passer par une commande ; ici on le fait nous-memes. Sauf pour INITIAL (mission 0) : ce qu'elle
    // pose (objets du decor, marqueurs des boutiques) est permanent.
    if (mission != 0) {
        for (int i = 0; i < g_objCount; i++) { int32_t h[1] = { (int32_t)g_objs[i].guest }; Local(0x0108, 1, h); }
        g_objCount = 0;
        // (sauf les icones fixes : points de contact des missions suivantes, ajoutes par la mission reussie)
        int kept = 0;
        for (int i = 0; i < g_blipCount; i++) {
            if (g_blips[i].keep) { g_blips[kept++] = g_blips[i]; continue; }
            int32_t h[1] = { (int32_t)g_blips[i].guest };
            Local(0x0164, 1, h);
        }
        if (kept) Log("miroir : %d icones de contact gardees apres la mission", kept);
        g_blipCount = kept;
        for (int i = 0; i < g_guestTimerCount; i++) { int32_t o[1] = { g_guestTimers[i].offset }; Local(g_guestTimers[i].clock ? 0x014F : 0x0151, 1, o); }
        g_guestTimerCount = 0;
    }
    if (gather) RequestGather();
    Log("miroir : fin de la mission %d chez l'hote (regroupement %d)", mission, gather);
}

// Hote : nouvelle partie lancee (menu pause) alors que des invites jouent : ils recommencent aussi, sinon ils
// gardaient l'ancien monde avec les variables de la nouvelle partie par-dessus.
void MirrorHostNewGame()
{
    uint8_t b = RL_NEW_GAME;
    NetSendReliable(&b, 1);
    for (int i = 1; i < MAX_PLAYERS; i++) g_synced[i] = false;
    Log("miroir : nouvelle partie de l'hote annoncee aux invites");
}

static void OnReliable(int from, const uint8_t *data, int len)
{
    if (len < 1) return;
    if (data[0] >= 10) { CombatOnReliable(from, data, len); return; }   // combat.cpp
    if (g_cfg.host) return;
    if (data[0] == RL_NEW_GAME) {
        g_qHead = g_qTail = 0;   // ce qui restait de l'ancienne partie
        g_hostOnMission = false;
        if (GameState() == GS_PLAYING) {
            MenuWantToLoad() = 0;
            MenuFirstTime() = 0;
            MenuWantToRestart() = 1;
            Log("miroir : l'hote recommence une nouvelle partie, nous aussi");
        }
        return;
    }
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
    InitPerPlayer();
}

// Argent (ADD_SCORE 0109) arrive pendant que l'invite n'est pas en partie (chargement, menu) : garde et donne a
// son retour, au lieu d'etre jete avec le reste de la file.
static int g_pendingMoney;
static void KeepPendingMoney(const Pending &p)
{
    if (p.data[0] != RL_SCRIPT_CMD || p.len < 14) return;
    uint16_t op;
    memcpy(&op, p.data + 1, 2);
    if (op != 0x0109 || p.data[4] != 'v' || p.data[9] != 'v') return;
    int32_t amount;
    memcpy(&amount, p.data + 10, 4);
    g_pendingMoney += amount;
}

void MirrorFrame(bool inGame)
{
    if (g_cfg.host) {
        if (!inGame) { g_activeBlipCount = 0; g_globSnapValid = false; g_missionRunning = false; }
        HostSyncNewcomers(inGame);
        if (inGame) SendTimers();
        // Entre deux missions, les changements du script principal partent au fil de l'eau (toutes les 2 s).
        static uint32_t lastDiff;
        if (inGame && !g_globSnapValid) { memcpy(g_globSnap, ScriptSpace(), GLOBALS_END); memcpy(g_globPrev, g_globSnap, GLOBALS_END); g_globSnapValid = true; lastDiff = GetTickCount(); }
        if (inGame && !g_missionRunning && GetTickCount() - lastDiff > 2000) { lastDiff = GetTickCount(); SendStableChanges(); }
        return;
    }
    if (*(int *)0x978810 != g_mirrorArea) g_mirrorArea = -1;   // le jeu a change de zone lui-meme (porte) : on ne suit plus
    if (!inGame) {
        g_blipCount = g_objCount = g_pickupCount = g_propCount = 0;
        // Hors partie (salon, chargement), la presentation des missions de l'hote n'a pas de sens : rejouees d'un coup
        // a l'arrivee (cameras fixes, textes, sons de l'intro...), elles faisaient planter la camera. On ne garde que
        // les variables de l'histoire ; l'hote renvoie l'etat complet (et les marqueurs) quand on arrive en partie.
        int kept = g_qHead, dropped = 0;
        for (int i = g_qHead; i != g_qTail; i = (i + 1) % QUEUE_SIZE) {
            if (g_queue[i].data[0] == RL_GLOBALS) { if (kept != i) g_queue[kept] = g_queue[i]; kept = (kept + 1) % QUEUE_SIZE; }
            else { KeepPendingMoney(g_queue[i]); dropped++; }
        }
        g_qTail = kept;
        if (dropped) Log("miroir : %d commandes ignorees hors partie (argent garde : %d)", dropped, g_pendingMoney);
        return;
    }
    uint32_t now = GetTickCount();
    if (g_pendingMoney) {
        *(int *)(0x94AD28 + 0xA0) += g_pendingMoney;   // CWorld::Players[0].m_nMoney
        Log("miroir : %d $ recus pendant le chargement, ajoutes", g_pendingMoney);
        g_pendingMoney = 0;
    }
    while (g_qHead != g_qTail) {
        Pending &p = g_queue[g_qHead];
        if (!p.since) p.since = now;
        bool done;
        if (p.data[0] == RL_MISSION_END) { MissionEnd(p.len < 2 || p.data[1], p.len >= 4 ? (int16_t)(p.data[2] | (p.data[3] << 8)) : -1); done = true; }
        else if (p.data[0] == RL_GLOBALS) {
            for (int at = 1; at + 6 <= p.len; at += 6) {
                uint16_t off; uint32_t v;
                memcpy(&off, p.data + at, 2); memcpy(&v, p.data + at + 2, 4);
                if (off >= GLOBALS_BEGIN && off + 4 <= GLOBALS_END) *(uint32_t *)(ScriptSpace() + off) = v;
            }
            Log("miroir : %d variables globales recues de l'hote", (p.len - 1) / 6);
            done = true;
        }
        else if (p.data[0] == RL_MISSION_START) {
            int mission = p.len >= 4 ? (int16_t)(p.data[2] | (p.data[3] << 8)) : -1;
            g_hostOnMission = mission != 0;
            if (p.len < 2 || p.data[1]) RequestGather();
            Log("miroir : debut de mission chez l'hote (%d)", mission);
            done = true;
        }
        else if (p.data[0] == RL_SCRIPT_CMD) done = Execute(p.data, p.len, now - p.since > 3000);
        else done = true;
        if (!done) break;   // on garde l'ordre : la suite attend
        g_qHead = (g_qHead + 1) % QUEUE_SIZE;
    }
}
