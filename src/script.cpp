// Crochet de la machine virtuelle des scripts : CRunningScript::ProcessOneCommand (0x44FBE0) passe par nous
// avant chaque opcode.
//  - Invite : les missions de l'histoire ne se lancent jamais chez lui (START_MISSION 0417 est consomme sans effet) ;
//    c'est l'hote qui les joue et qui en reproduit le contenu. Exception : les missions secondaires de vehicule
//    (taxi, ambulance, pompiers, vigilante, livreur de pizza), qu'il joue seul chez lui.
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include "mirror.h"
#include "conditions.h"
#include "overlay.h"
#include <string.h>

using namespace game;

typedef char(__fastcall *ProcessOneCommand_t)(void *script);
static ProcessOneCommand_t o_ProcessOneCommand;   // trampoline : 7 octets d'origine puis jmp 0x44FBE7

// CRunningScript::CollectParameters(int *ip, short count) : lit les parametres dans ScriptParams.
static void CollectParameters(void *script, int count)
{
    ((void(__thiscall *)(void *, int *, short))0x451010)(script, &Field<int>(script, 0x10), (short)count);
}

enum { OP_TERMINATE_THIS_SCRIPT = 0x004E, OP_START_MISSION = 0x0417 };

char CallOriginalProcessOneCommand(void *script) { return o_ProcessOneCommand(script); }

// Vehicules des missions secondaires : c'est en y etant (et en appuyant sur le bouton de mission) que le script
// principal lance taxi, ambulance, pompiers, vigilante, pizzas. Aucune mission de l'histoire ne demarre ainsi.
static bool InSideMissionVehicle()
{
    void *me = FindPlayerPed();
    if (!me || !InVehicle(me) || !PedVehicle(me) || VehDriver(PedVehicle(me)) != me) return false;
    // (pas "kaufman" : les missions d'histoire de Kaufman Cabs se lancent au volant d'un taxi Kaufman)
    static const char *const names[] = { "taxi", "cabbie", "zebra", "ambulan", "firetruk", "police", "enforcer",
                                         "fbiranch", "vicechee", "predator", "hunter", "rhino", "barracks", "polmav", "pizzaboy" };
    const char *m = ModelName(ModelIndex(PedVehicle(me)));
    for (const char *n : names) if (_stricmp(m, n) == 0) return true;
    return false;
}

static bool g_sideMission, g_buyMission;
bool GuestSideMission() { return g_sideMission; }

// --- Achat d'immeubles par l'invite ---
// Les icones d'immeuble (CREATE_PROTECTION_PICKUP 0517 verrouillee / 0518 a vendre) sont creees par le script
// principal et INITIAL de chaque machine ; leur reference est ecrite dans une globale. Quand l'invite en ramasse
// une, son script principal lance la mission d'achat (BUYPRO1..SKUMBUY) : elle tourne chez lui, avec son argent,
// et ecrit ses propres drapeaux de possession (gardes par overlay.cpp).
static uint32_t g_propPickups[32];
static int g_propAt;
static uint32_t g_propCollectedAt;
bool IsPropertyPickup(uint32_t handle) { for (uint32_t h : g_propPickups) if (h && h == handle) return true; return false; }
void NotePropertyCollected() { g_propCollectedAt = GetTickCount(); }
static bool PropertyBuyPending() { return g_propCollectedAt && GetTickCount() - g_propCollectedAt < 3000; }

// Position du dernier parametre (la sortie) d'une commande a n parametres, -1 si ce n'est pas une globale.
static int LastGlobalParam(int ip, int n)
{
    uint8_t *ss = ScriptSpace();
    int at = ip + 2;
    for (int i = 0; i < n; i++) {
        uint8_t t = ss[at];
        if (i == n - 1) return t == 2 ? *(uint16_t *)(ss + at + 1) : -1;
        switch (t) {
        case 1: case 6: at += 5; break;
        case 2: case 3: case 5: at += 3; break;
        case 4: at += 2; break;
        default: at += 8; break;   // etiquette
        }
    }
    return -1;
}

static char __fastcall h_ProcessOneCommand(void *script)
{
    int ip = Field<int>(script, 0x10);
    uint16_t op = *(uint16_t *)(ScriptSpace() + ip) & 0x7FFF;
    if (!g_cfg.host && op == OP_TERMINATE_THIS_SCRIPT && Field<bool>(script, 0x85) && g_sideMission) {
        g_sideMission = false;
        Log("script : fin de la mission secondaire (%.8s)", (char *)script + 8);
    }
    if (!g_cfg.host && op == OP_TERMINATE_THIS_SCRIPT && Field<bool>(script, 0x85) && g_buyMission) {
        g_buyMission = false;
        OverlayBuyEnd();
    }
    if (!g_cfg.host && op == OP_START_MISSION && !g_sideMission && (InSideMissionVehicle() || PropertyBuyPending())) {
        g_sideMission = true;
        g_buyMission = PropertyBuyPending() && !InSideMissionVehicle();
        g_propCollectedAt = 0;
        if (g_buyMission) OverlayBuyStart();
        Log("script : l'invite lance une mission %s (par %.8s)", g_buyMission ? "d'achat d'immeuble" : "secondaire", (char *)script + 8);
        return o_ProcessOneCommand(script);
    }
    if (!g_cfg.host && (op == 0x0517 || op == 0x0518)) {   // icone d'immeuble : on retient sa reference
        int gofs = LastGlobalParam(ip, op == 0x0517 ? 5 : 6);
        char r = o_ProcessOneCommand(script);
        if (gofs > 0) { g_propPickups[g_propAt++ % 32] = *(uint32_t *)(ScriptSpace() + gofs); if (g_cfg.logScripts) Log("script : icone d'immeuble %08X (globale %d)", g_propPickups[(g_propAt - 1) % 32], gofs / 4); }
        return r;
    }
    if (!g_cfg.host && op == OP_START_MISSION) {
        static uint32_t lastLog;
        Field<int>(script, 0x10) = ip + 2;
        CollectParameters(script, 1);
        // INITIAL (mission 0) : pas une mission d'histoire mais la mise en place du monde (generateurs de voitures
        // garees, pickups, marqueurs des boutiques, objets) : chaque invite la joue lui-meme.
        if (*(int *)0x7D7438 == 0) {
            Field<int>(script, 0x10) = ip;
            Log("script : l'invite joue INITIAL lui-meme");
            return o_ProcessOneCommand(script);
        }
        uint32_t now = GetTickCount();
        if (now - lastLog > 5000) {
            Log("script : mission %d non lancee chez l'invite (%.8s)", *(int *)0x7D7438, (char *)script + 8);
            lastLog = now;
        }
        return 0;
    }
    // Diagnostic JournalOpcodes=2 : compte les opcodes de tous les scripts pendant 15 s apres la 2e fin de mission.
    if (g_cfg.logOpcodes == 2) {
        static uint32_t counts[0x800], start;
        static int ends;
        static bool dumped;
        if (op == OP_TERMINATE_THIS_SCRIPT && Field<bool>(script, 0x85) && ++ends == 2) start = GetTickCount();
        if (start && !dumped) {
            if (op < 0x800) counts[op]++;
            if (GetTickCount() - start > 15000) {
                dumped = true;
                char line[2048];
                int n = wsprintfA(line, "opcodes apres l'intro :");
                for (int i = 0; i < 0x800 && n < 2000; i++) if (counts[i]) n += wsprintfA(line + n, " %04X:%u", i, counts[i]);
                Log("%s", line);
            }
        }
    }
    static int logged;                       // trace remise a zero a chaque lancement de mission
    if (op == OP_START_MISSION) logged = 0;
    if (g_cfg.logOpcodes == 1 && Field<bool>(script, 0x85)) {
        if (logged < 4000) {
            logged++;
            char hex[64];
            for (int i = 0; i < 20; i++) wsprintfA(hex + i * 3, "%02X ", ScriptSpace()[ip + 2 + i]);
            Log("op %04X @%X (%.8s) %s", op, ip, (char *)script + 8, hex);
        }
    }
    // Diagnostic : textes d'aide / messages affiches par n'importe quel script (etiquette, script, position).
    if (g_cfg.logScripts && (op == 0x03E5 || op == 0x00BC || op == 0x00BB)) {
        static char last[9];
        const char *label = (const char *)ScriptSpace() + ip + 2;
        if (memcmp(last, label, 8) != 0) {
            memcpy(last, label, 8);
            void *me = FindPlayerPed();
            Log("script : %04X '%.8s' par %.8s (mission %d) joueur en %.1f %.1f, modele 0 '%s'", op, label,
                (char *)script + 8, Field<bool>(script, 0x85), me ? Pos(me).x : 0.0f, me ? Pos(me).y : 0.0f, ModelName(0));
        }
    }
    // Diagnostic TraceScript=nom : chaque opcode d'un script nomme, avec ses octets (6000 au plus).
    if (g_cfg.traceScript[0] && !_strnicmp((char *)script + 8, g_cfg.traceScript, 8)) {
        static int traced;
        if (traced++ < 6000) {
            char hex[64];
            for (int i = 0; i < 20; i++) wsprintfA(hex + i * 3, "%02X ", ScriptSpace()[ip + 2 + i]);
            Log("trace %04X @%X %s", op, ip, hex);
        }
    }
    ConditionsBeginCommand(script, op);
    if (g_cfg.host) {
        if (op == OP_TERMINATE_THIS_SCRIPT && Field<bool>(script, 0x85)) MirrorMissionEnd();
        if (op == OP_START_MISSION) {
            int at = ip + 2, n = 0;
            uint8_t t = ScriptSpace()[at];
            if (t == 4) n = (int8_t)ScriptSpace()[at + 1];
            else if (t == 5) n = *(int16_t *)(ScriptSpace() + at + 1);
            else if (t == 1) n = *(int32_t *)(ScriptSpace() + at + 1);
            else if (t == 2) n = *(int32_t *)(ScriptSpace() + *(uint16_t *)(ScriptSpace() + at + 1));
            MirrorMissionStart(n);
            Log("script : l'hote lance la mission %d (%.8s)", n, (char *)script + 8);
        }
        if (MirrorBefore(script, ip, op)) {
            char r = o_ProcessOneCommand(script);
            MirrorAfter(script);
            return r;
        }
        char r = o_ProcessOneCommand(script);
        ConditionsAfterCommand(script, op, ip);
        return r;
    }
    return o_ProcessOneCommand(script);
}

void InstallScriptHooks()
{
    static const uint8_t prologue[] = { 0x66, 0xFF, 0x05, 0x66, 0x0A, 0xA1, 0x00 };
    if (memcmp((void *)0x44FBE0, prologue, sizeof(prologue)) != 0) {
        Log("script : prologue inattendu, crochet non pose");
        return;
    }
    uint8_t *tramp = (uint8_t *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tramp, prologue, sizeof(prologue));
    tramp[7] = 0xE9;
    *(int32_t *)(tramp + 8) = (int32_t)(0x44FBE7 - ((uintptr_t)tramp + 12));
    o_ProcessOneCommand = (ProcessOneCommand_t)tramp;
    PatchJump(0x44FBE0, (void *)h_ProcessOneCommand, 7);
}
