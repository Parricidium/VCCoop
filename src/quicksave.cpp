// Sauvegarde automatique et sauvegarde rapide (hote seulement : c'est sa sauvegarde que les invites recoivent).
// - SauvegardeAuto=1 : apres une mission de l'histoire reussie (son script joue PLAY_MISSION_PASSED_TUNE), une fois
//   la mission finie et le joueur libre, le jeu enregistre dans l'emplacement SauvegardeAutoEmplacement (8 par
//   defaut) en passant par le menu de sauvegarde d'une planque, comme l'autotest "sauvecharge".
// - ToucheSauvegarde=F5 : hors mission, ouvre ce menu de sauvegarde (on choisit l'emplacement). Jamais pendant une
//   mission : le script en cours ne se sauvegarde pas, et une partie rechargee en pleine mission desynchronise.
// Rien n'est envoye sur le reseau : le protocole ne change pas.
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include "saveshare.h"
#include "panel.h"

using namespace game;

static bool g_passedTune;      // la mission en cours a joue l'air de mission reussie
static bool g_autoPending;     // sauvegarde automatique a faire des que possible
static uint32_t g_autoSince;   // fin de la mission reussie
static int g_step;             // 0 = rien ; 1..4 = sauvegarde automatique en cours dans le menu
static uint32_t g_stepAt;

static bool PlayerFree() { return !*(bool *)0xA10AB2 && *(short *)(0x7DBCB0 + 0xF0) == 0; }   // pas de cinematique, controles rendus
static void OpenSaveMenu() { *(bool *)(0x869630 + 0x3B) = true; }   // ACTIVATE_SAVE_MENU (m_bSaveMenuActive)

void QuickSaveOnCommand(void *script, uint16_t op)
{
    if (op == 0x0394 && Field<bool>(script, 0x85) && !g_passedTune) {   // PLAY_MISSION_PASSED_TUNE, script de mission
        g_passedTune = true;
        Log("sauvegarde auto : air de mission reussie (%.8s)", (char *)script + 8);
    }
}

void QuickSaveOnMissionStart() { g_passedTune = false; }

// Autotest=sauveauto (hote) : comme une mission reussie qui vient de finir
void QuickSaveTestTrigger() { g_passedTune = true; QuickSaveOnMissionEnd(true); }

void QuickSaveOnMissionEnd(bool storyMission)
{
    if (storyMission && g_passedTune && g_cfg.autoSave) { g_autoPending = true; g_autoSince = GetTickCount(); Log("sauvegarde auto : mission reussie, sauvegarde a faire"); }
    g_passedTune = false;
}

static bool CanSave()
{
    return g_cfg.host && GameState() == GS_PLAYING && FindPlayerPed() && !MenuActive() && !MissionUnderway() && PlayerFree() && !MenuWantToLoad();
}

void QuickSaveFrame()
{
    if (!g_cfg.host) return;
    uint32_t now = GetTickCount();

    // Sauvegarde rapide : front montant de la touche
    static bool wasDown;
    bool down = g_cfg.saveKey && GameHasFocus() && !PanelCapturesKeys() && (GetAsyncKeyState(g_cfg.saveKey) & 0x8000);
    if (down && !wasDown && g_step == 0) {
        if (CanSave()) { OpenSaveMenu(); Log("sauvegarde rapide : menu de sauvegarde ouvert"); }
        else Log("sauvegarde rapide : impossible maintenant (mission, cinematique ou menu)");
    }
    wasDown = down;

    // Sauvegarde automatique : 3 s apres la fin de la mission, des que le joueur est libre
    if (g_step == 0 && g_autoPending && now - g_autoSince > 3000 && CanSave()) {
        g_autoPending = false;
        OpenSaveMenu();
        g_step = 1; g_stepAt = now;
        Log("sauvegarde auto : emplacement %d", g_cfg.autoSaveSlot);
        return;
    }
    if (g_step == 0) return;
    int page = MenuCurrentPage();
    if (now - g_stepAt > 15000) { Log("sauvegarde auto : abandon (ecran %d)", page); g_step = 0; return; }
    // Ecrans du menu : 15 = choix de l'emplacement, 16 = confirmation, 17 = enregistrement, 18 = "OK". Le menu ignore
    // une entree validee pendant sa transition : on attend 1 s sur chaque ecran, et on revalide toutes les secondes.
    static uint32_t lastReq;
    bool ready = now - lastReq > 1000;
    if (g_step <= 3 && page == 15 && ready) { MenuRequestSelect(g_cfg.autoSaveSlot - 1); lastReq = now; g_step = 2; }
    else if (g_step >= 2 && g_step <= 3 && page == 16 && ready) { MenuRequestSelect(2); lastReq = now; g_step = 3; }   // "Oui"
    else if (g_step >= 2 && page == 18 && ready) {   // "OK" apres l'enregistrement
        if (g_step != 4) Log("sauvegarde auto : faite (emplacement %d)", g_cfg.autoSaveSlot);
        MenuRequestSelect(1); lastReq = now; g_step = 4;
    }
    else if (g_step == 4 && MenuActive() && page == 15 && ready) { MenuRequestSelect(8); lastReq = now; }   // "Annuler" : retour au jeu
    else if (g_step == 4 && !MenuActive()) g_step = 0;
}
