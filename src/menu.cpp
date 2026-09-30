// Menu COOP dans le vrai menu du jeu : une entree "COOP" dans le menu principal (et la pause) ouvre l'ecran 33
// (inutilise par le jeu), dont le contenu est refait selon la situation :
//  - accueil : Creer une partie / Rejoindre / Adresse / Pseudo / reglages / Retour ;
//  - salon (reseau demarre, au menu) : joueurs connectes, puis Nouvelle partie / Charger une partie (hote) ou
//    "en attente de l'hote" (invite), reglages ; l'invite suit l'hote tout seul quand celui-ci entre en jeu ;
//  - en partie : joueurs, reglages.
//  - Les textes passent par un crochet de CText::Get (0x584F30) : cles "VCC_..." (FR ou EN selon la langue
//    du jeu), certaines dynamiques (adresse, pseudo, etat de la connexion).
//  - Les actions 60+ (inconnues du jeu) sont traitees dans un crochet de CMenuManager::ProcessButtonPresses
//    (0x4990DD) ; la saisie de l'adresse et du pseudo passe par WM_CHAR (sous-classement de la fenetre).
#include "util.h"
#include "vccoop.h"
#include "bridge.h"
#include "net.h"
#include "game.h"
#include "saveshare.h"
#include "panel.h"
#include <string.h>
#include <stdio.h>

using namespace game;

// --- Table des ecrans du menu (aScreens, 0x6D8B70) ---
#pragma pack(push, 1)
struct MenuEntry { uint16_t action; char label[8]; uint8_t saveSlot; int8_t target; uint16_t x, y, align; };
struct MenuScreen { char name[8]; int8_t prevPage, parentEntry; MenuEntry entries[12]; };
#pragma pack(pop)
static_assert(sizeof(MenuEntry) == 0x12 && sizeof(MenuScreen) == 0xE2, "table des menus");
static MenuScreen *Screens() { return (MenuScreen *)0x6D8B70; }

enum { PAGE_MAIN = 29, PAGE_NEW_GAME = 7, PAGE_COOP = 33 };
enum { ACT_CHANGEMENU = 4, ACT_GOBACK = 34, ACT_CREATE = 60, ACT_JOIN, ACT_ADDRESS, ACT_NICK,
       ACT_FRIENDLY, ACT_MONEY, ACT_NAMES, ACT_WEAPONS, ACT_INFO, ACT_NEWGAME, ACT_LOADGAME, ACT_DRAWDIST,
       ACT_OPTIONS, ACT_OPTCOOP, ACT_OPTVIDEO, ACT_BACKSUB, ACT_MSAA, ACT_ANISO, ACT_JOINPAGE, ACT_SHADOWS,
       ACT_RENDERER, ACT_SHADOWQ, ACT_WATER, ACT_LIGHTS, ACT_LIGHTSHADOWS, ACT_MOON, ACT_CLOSELOBBY, ACT_DISCONNECT, ACT_REFLECT, ACT_AO, ACT_FPS, ACT_POPZONE, ACT_POPDENS, ACT_RAGDOLL,
       ACT_POSTPAGE, ACT_AMBPAGE, ACT_SMAA, ACT_BLOOM, ACT_GRADE, ACT_SHARPEN, ACT_SOFTPART, ACT_LAMPS,
       ACT_WET, ACT_RAYS, ACT_WIND, ACT_BEAMS, ACT_HAZE, ACT_CARREFL, ACT_GI };
// Sous-pages de l'ecran COOP (meme ecran 33, contenu refait) : accueil / salon / en partie, puis Options,
// Options coop, Options video. Echap (ou Retour) remonte d'un cran.
enum { SUB_MAIN, SUB_OPTIONS, SUB_COOP, SUB_VIDEO, SUB_JOIN, SUB_POST, SUB_AMB };   // Effets d'image, Ambiance : sous-pages de Options video
static int g_sub, g_subParent;
enum { PAGE_LOAD_GAME = 8 };

static uint8_t *Menu() { return (uint8_t *)0x869630; }   // FrontEndMenuManager
static int CurrentPage() { return *(int *)(Menu() + 0xF8); }
static int CurrentEntry() { return *(int *)(Menu() + 0x30); }
static bool French() { return *(int *)(Menu() + 0x50) == 1; }
static void SwitchToNewScreen(int page) { ((void(__thiscall *)(void *, int))0x4983EF)(Menu(), page); }

// --- Etat ---
enum EditField { EDIT_NONE, EDIT_ADDRESS, EDIT_NICK };
static EditField g_edit;
static char g_editBuf[64];
static bool g_netStarted;       // le reseau a demarre (depuis le menu, la ligne de commande ou Reseau=1)
static bool g_joining;          // invite : on attend la reponse de l'hote avant la nouvelle partie
// Actions decidees hors du traitement du menu (connexion reussie, tests) : executees au prochain passage du jeu
// dans ProcessButtonPresses, seul endroit ou SwitchToNewScreen (qui redessine) est a sa place.
static int g_pendingPage = -1;      // ecran a ouvrir
static int g_pendingSelect = -1;    // entree a valider sur l'ecran courant
static uint32_t g_joinSince;

bool CoopNetworkStarted() { return g_netStarted; }
void MenuRequestPage(int page) { g_pendingPage = page; }
void MenuRequestSelect(int entry) { g_pendingSelect = entry; }
static bool g_pendingBack;
void MenuRequestBack() { g_pendingBack = true; }   // autotest : comme Echap
int MenuCurrentPage() { return CurrentPage(); }

void CoopStartNetwork()
{
    if (g_netStarted) return;
    g_netStarted = true;
    NetStart();
}

static void SaveIni()
{
    char ini[MAX_PATH];
    lstrcpynA(ini, IniPath(), MAX_PATH);
    WritePrivateProfileStringA("VCCoop", "Adresse", g_cfg.address, PlayerIniPath());   // a part : gardes aux mises a jour
    WritePrivateProfileStringA("VCCoop", "Pseudo", g_cfg.playerName, PlayerIniPath());
    WritePrivateProfileStringA("VCCoop", "TirAmi", g_cfg.friendlyFire ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "ArgentPartage", g_cfg.shareMoney ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "AfficherPseudos", g_cfg.showNames ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "GarderArmes", g_cfg.keepWeapons ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "CorpsMous", g_cfg.ragdoll ? "1" : "0", ini);
    char dd[16];
    wsprintfA(dd, "%d", g_cfg.drawDistance);
    WritePrivateProfileStringA("VCCoop", "DistanceAffichage", dd, ini);
    wsprintfA(dd, "%d", g_cfg.msaa);
    WritePrivateProfileStringA("VCCoop", "Anticrenelage", dd, ini);
    WritePrivateProfileStringA("VCCoop", "OmbresSoleil", g_cfg.sunShadows ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "Rendu", g_cfg.renderer == 12 ? "12" : g_cfg.renderer == 9 ? "9" : "8", ini);
    WritePrivateProfileStringA("VCCoop", "EauModerne", g_cfg.modernWater ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "RefletsEau", g_cfg.waterReflections ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "LumieresDynamiques", g_cfg.dynLights ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "OmbresLumieres", g_cfg.lightShadows ? "4" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "OmbresLune", g_cfg.moonShadows ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "OcclusionAmbiante", g_cfg.ambientOcclusion ? "1" : "0", ini);
    {   // ray tracing (onglet RAY TRACING du lanceur et du panneau F10)
        struct { const char *key; int v; } rt[] = {
            { "RTOmbres", g_cfg.rtShadows }, { "RTRayons", g_cfg.rtRays }, { "RTResolution", g_cfg.rtScale },
            { "RTReflets", g_cfg.rtRefl }, { "RTLumiere", g_cfg.rtGI }, { "RTOcclusion", g_cfg.rtAO },
            { "RTDouceur", g_cfg.rtSoft }, { "RTDistance", g_cfg.rtDist }, { "RTRefletsForce", g_cfg.rtReflK },
            { "RTRefletsSol", g_cfg.rtGloss }, { "RTLumiereForce", g_cfg.rtGIK }, { "RTOcclusionForce", g_cfg.rtAOK },
            { "RTLissage", g_cfg.rtSmooth } };
        char v[16];
        for (auto &r : rt) { wsprintfA(v, "%d", r.v); WritePrivateProfileStringA("VCCoop", r.key, v, ini); }
    }
    WritePrivateProfileStringA("VCCoop", "SMAA", g_cfg.smaa ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "Eclat", g_cfg.bloom ? "1" : "0", ini);
    wsprintfA(dd, "%d", g_cfg.grade);
    WritePrivateProfileStringA("VCCoop", "Etalonnage", dd, ini);
    wsprintfA(dd, "%d", g_cfg.sharpen);
    WritePrivateProfileStringA("VCCoop", "Nettete", dd, ini);
    WritePrivateProfileStringA("VCCoop", "ParticulesDouces", g_cfg.softParticles ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "LampadairesEclairent", g_cfg.lampLights ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "RoutesMouillees", g_cfg.wetRoads ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "RayonsSoleil", g_cfg.sunRays ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "VegetationVent", g_cfg.windPlants ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "FaisceauxPhares", g_cfg.beams ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "Brume", g_cfg.haze ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "RefletsVoitures", g_cfg.carReflections ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "LumiereIndirecte", g_cfg.indirectLight ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "VuePremierePersonne", g_cfg.fpsView ? "1" : "0", ini);
    wsprintfA(dd, "%d", g_cfg.zonePop);
    WritePrivateProfileStringA("VCCoop", "ZonePopulation", dd, ini);
    wsprintfA(dd, "%d", g_cfg.popDensity);
    WritePrivateProfileStringA("VCCoop", "DensitePopulation", dd, ini);
    wsprintfA(dd, "%d", g_cfg.shadowRes);
    WritePrivateProfileStringA("VCCoop", "OmbresResolution", dd, ini);
    if (!WritePrivateProfileStringA("VCCoop", "FiltrageAnisotrope", g_cfg.aniso ? "1" : "0", ini))
        Log("reglages : ecriture impossible dans %s (erreur %lu)", ini, GetLastError());
}

void MenuSaveIni() { SaveIni(); }   // panel.cpp : onglet Hote du menu en jeu

// --- Textes ---
static wchar_t g_text[56][80];

static const wchar_t *Put(int slot, const char *s)
{
    // La police du jeu : majuscules sans accents.
    int i = 0;
    for (; s[i] && i < 79; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        g_text[slot][i] = (wchar_t)(unsigned char)c;
    }
    g_text[slot][i] = 0;
    return g_text[slot];
}

static const wchar_t *CoopText(const char *key)
{
    bool fr = French();
    char buf[96];
    if (!strcmp(key, "VCC_MM")) return Put(0, "COOP");
    if (!strcmp(key, "VCC_TIT")) return Put(1, g_sub == SUB_OPTIONS ? "Options" : g_sub == SUB_COOP ? (fr ? "Options coop" : "Coop options")
                                              : g_sub == SUB_VIDEO ? (fr ? "Options video" : "Video options") : g_sub == SUB_JOIN ? (fr ? "Rejoindre" : "Join")
                                              : g_sub == SUB_POST ? (fr ? "Effets d'image" : "Image effects") : g_sub == SUB_AMB ? (fr ? "Ambiance" : "Atmosphere") : "COOP");
    if (!strcmp(key, "VCC_JP")) return Put(23, fr ? "Rejoindre" : "Join");
    if (!strcmp(key, "VCC_OPT")) return Put(18, "Options");
    if (!strcmp(key, "VCC_OC")) return Put(19, fr ? "Options coop" : "Coop options");
    if (!strcmp(key, "VCC_OV")) return Put(20, fr ? "Options video" : "Video options");
    if (!strcmp(key, "VCC_CRE")) return Put(2, fr ? "Creer une partie" : "Host a game");
    if (!strcmp(key, "VCC_JOI")) {
        if (g_joining) wsprintfA(buf, fr ? "Se connecter : connexion..." : "Connect: connecting...");
        else if (g_netStarted && !g_cfg.host && g_localId > 0) wsprintfA(buf, fr ? "Se connecter : connecte" : "Connect: connected");
        else wsprintfA(buf, fr ? "Se connecter" : "Connect");
        return Put(3, buf);
    }
    if (!strcmp(key, "VCC_IP")) {
        wsprintfA(buf, "%s : %s%s", fr ? "Adresse" : "Address", g_edit == EDIT_ADDRESS ? g_editBuf : g_cfg.address,
                  g_edit == EDIT_ADDRESS && (GetTickCount() / 400) % 2 ? "-" : "");
        return Put(4, buf);
    }
    // Reglages. Tir ami et argent partage : c'est l'hote qui decide (chez un invite connecte, on montre les siens).
    const char *yes = fr ? "Oui" : "On", *no = fr ? "Non" : "Off";
    bool guestOnline = g_netStarted && !g_cfg.host && g_localId > 0;
    if (!strcmp(key, "VCC_TA")) {
        wsprintfA(buf, "%s : %s%s", fr ? "Tir ami" : "Friendly fire", g_cfg.friendlyFire ? yes : no, guestOnline ? (fr ? " (hote)" : " (host)") : "");
        return Put(6, buf);
    }
    if (!strcmp(key, "VCC_AP")) {
        wsprintfA(buf, "%s : %s", fr ? "Argent partage" : "Shared money", g_cfg.shareMoney ? yes : no);
        return Put(7, buf);
    }
    if (!strcmp(key, "VCC_PS2")) {
        wsprintfA(buf, "%s : %s", fr ? "Pseudos" : "Names", g_cfg.showNames ? yes : no);
        return Put(8, buf);
    }
    if (!strcmp(key, "VCC_PZ")) {
        wsprintfA(buf, "%s : %d%%", fr ? "Zone de population" : "Population area", g_cfg.zonePop);
        return Put(33, buf);
    }
    if (!strcmp(key, "VCC_PD")) {
        wsprintfA(buf, "%s : %d%%", fr ? "Densite de population" : "Population density", g_cfg.popDensity);
        return Put(34, buf);
    }
    if (!strcmp(key, "VCC_PP")) return Put(36, fr ? "Effets d'image..." : "Image effects...");
    if (!strcmp(key, "VCC_AM")) return Put(37, fr ? "Ambiance..." : "Atmosphere...");
    if (!strcmp(key, "VCC_SM")) { wsprintfA(buf, "SMAA : %s", g_cfg.smaa ? yes : no); return Put(38, buf); }
    if (!strcmp(key, "VCC_BL")) { wsprintfA(buf, "%s : %s", fr ? "Eclat (lumieres qui debordent)" : "Bloom", g_cfg.bloom ? yes : no); return Put(39, buf); }
    if (!strcmp(key, "VCC_GR")) {
        static const char *fN[3] = { "Original", "Vice", "Film" };
        wsprintfA(buf, "%s : %s", fr ? "Etalonnage" : "Color grading", fN[g_cfg.grade < 0 || g_cfg.grade > 2 ? 0 : g_cfg.grade]);
        return Put(40, buf);
    }
    if (!strcmp(key, "VCC_NT")) {
        if (g_cfg.sharpen > 0) wsprintfA(buf, "%s : %d%%", fr ? "Nettete" : "Sharpening", g_cfg.sharpen);
        else wsprintfA(buf, "%s : %s", fr ? "Nettete" : "Sharpening", no);
        return Put(41, buf);
    }
    if (!strcmp(key, "VCC_SP")) { wsprintfA(buf, "%s : %s", fr ? "Particules douces" : "Soft particles", g_cfg.softParticles ? yes : no); return Put(42, buf); }
    if (!strcmp(key, "VCC_LP")) { wsprintfA(buf, "%s : %s", fr ? "Lampadaires et neons eclairent" : "Street lamps and neons cast light", g_cfg.lampLights ? yes : no); return Put(43, buf); }
    if (!strcmp(key, "VCC_WE")) { wsprintfA(buf, "%s : %s", fr ? "Routes mouillees, sols brillants" : "Wet roads, shiny floors", g_cfg.wetRoads ? yes : no); return Put(44, buf); }
    if (!strcmp(key, "VCC_RY")) { wsprintfA(buf, "%s : %s", fr ? "Rayons de soleil" : "Sun rays", g_cfg.sunRays ? yes : no); return Put(45, buf); }
    if (!strcmp(key, "VCC_WI")) { wsprintfA(buf, "%s : %s", fr ? "Vegetation au vent" : "Plants in the wind", g_cfg.windPlants ? yes : no); return Put(46, buf); }
    if (!strcmp(key, "VCC_BE")) { wsprintfA(buf, "%s : %s", fr ? "Faisceaux des phares" : "Headlight beams", g_cfg.beams ? yes : no); return Put(47, buf); }
    if (!strcmp(key, "VCC_HZ")) { wsprintfA(buf, "%s : %s", fr ? "Brume au loin" : "Distance haze", g_cfg.haze ? yes : no); return Put(48, buf); }
    if (!strcmp(key, "VCC_CR")) { wsprintfA(buf, "%s : %s", fr ? "Reflets des voitures" : "Car reflections", g_cfg.carReflections ? yes : no); return Put(49, buf); }
    if (!strcmp(key, "VCC_GI")) { wsprintfA(buf, "%s : %s", fr ? "Lumiere indirecte" : "Indirect light", g_cfg.indirectLight ? yes : no); return Put(50, buf); }
    if (!strcmp(key, "VCC_RG")) {
        wsprintfA(buf, "%s : %s", fr ? "Corps mous" : "Ragdolls", g_cfg.ragdoll ? yes : no);
        return Put(35, buf);
    }
    if (!strcmp(key, "VCC_GA")) {
        wsprintfA(buf, "%s : %s", fr ? "Garder ses armes" : "Keep weapons", g_cfg.keepWeapons ? yes : no);
        return Put(9, buf);
    }
    if (!strcmp(key, "VCC_DD")) {
        wsprintfA(buf, "%s : %d%%", fr ? "Distance d'affichage" : "Draw distance", g_cfg.drawDistance);
        return Put(10, buf);
    }
    if (!strcmp(key, "VCC_AA")) {
        if (g_cfg.msaa >= 2) wsprintfA(buf, "%s : %dx%s", fr ? "Anticrenelage" : "Anti-aliasing", g_cfg.msaa, fr ? " (apres relance)" : " (after restart)");
        else wsprintfA(buf, "%s : %s%s", fr ? "Anticrenelage" : "Anti-aliasing", no, fr ? " (apres relance)" : " (after restart)");
        return Put(21, buf);
    }
    if (!strcmp(key, "VCC_SH")) {
        wsprintfA(buf, "%s : %s", fr ? "Ombres du soleil" : "Sun shadows", g_cfg.sunShadows ? yes : no);
        return Put(24, buf);
    }
    if (!strcmp(key, "VCC_RD")) {
        wsprintfA(buf, "%s : %s%s", fr ? "Rendu" : "Renderer", g_cfg.renderer == 12 ? "Ray tracing" : g_cfg.renderer == 9 ? "Direct3D 9" : "Direct3D 8",
                  fr ? " (apres relance)" : " (after restart)");
        return Put(25, buf);
    }
    if (!strcmp(key, "VCC_WA")) {
        // 3 reglages sur une ligne (la page du jeu n'a que 12 lignes) : non / oui / oui + reflets.
        wsprintfA(buf, "%s : %s", fr ? "Eau moderne" : "Modern water",
                  !g_cfg.modernWater ? no : g_cfg.waterReflections ? (fr ? "avec reflets" : "with reflections") : yes);
        return Put(12, buf);
    }
    if (!strcmp(key, "VCC_RF")) {
        wsprintfA(buf, "%s : %s", fr ? "Reflets sur l'eau" : "Water reflections", g_cfg.waterReflections ? yes : no);
        return Put(30, buf);
    }
    if (!strcmp(key, "VCC_LI")) {
        // non / oui / avec ombres
        wsprintfA(buf, "%s : %s", fr ? "Lumieres dynamiques" : "Dynamic lights",
                  !g_cfg.dynLights ? no : g_cfg.lightShadows ? (fr ? "avec ombres" : "with shadows") : yes);
        return Put(13, buf);
    }
    if (!strcmp(key, "VCC_LS")) {
        wsprintfA(buf, "%s : %s", fr ? "Ombres des lumieres" : "Light shadows", g_cfg.lightShadows ? yes : no);
        return Put(14, buf);
    }
    if (!strcmp(key, "VCC_FP")) {
        char k[8];
        if (g_cfg.fpsKey >= VK_F1 && g_cfg.fpsKey <= VK_F12) wsprintfA(k, "F%d", g_cfg.fpsKey - VK_F1 + 1); else wsprintfA(k, "%c", g_cfg.fpsKey);
        wsprintfA(buf, "%s (%s) : %s", fr ? "Vue premiere personne" : "First person view", k, g_cfg.fpsView ? yes : no);
        return Put(32, buf);
    }
    if (!strcmp(key, "VCC_AO")) {
        wsprintfA(buf, "%s : %s", fr ? "Occlusion ambiante" : "Ambient occlusion", g_cfg.ambientOcclusion ? yes : no);
        return Put(31, buf);
    }
    if (!strcmp(key, "VCC_MO")) {
        wsprintfA(buf, "%s : %s", fr ? "Ombres de la lune" : "Moon shadows", g_cfg.moonShadows ? yes : no);
        return Put(27, buf);
    }
    if (!strcmp(key, "VCC_SQ")) {
        const char *q = g_cfg.shadowRes >= 8192 ? (fr ? "ultra" : "ultra") : g_cfg.shadowRes >= 4096 ? (fr ? "haute" : "high") : (fr ? "moyenne" : "medium");
        wsprintfA(buf, "%s : %s", fr ? "Qualite des ombres" : "Shadow quality", q);
        return Put(26, buf);
    }
    if (!strcmp(key, "VCC_AF")) {
        wsprintfA(buf, "%s : %s", fr ? "Filtrage anisotrope" : "Anisotropic filtering", g_cfg.aniso ? yes : no);
        return Put(22, buf);
    }
    // Salon : joueurs (VCC_P0..3 = les connectes, dans l'ordre), attente de l'invite, lancer la partie.
    if (!strncmp(key, "VCC_P", 5) && key[5] >= '0' && key[5] <= '3' && !key[6]) {
        int want = key[5] - '0', k = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            bool self = i == g_localId || (g_localId < 0 && i == 0 && g_cfg.host);
            if (!self && !g_players[i].connected) continue;
            if (k++ != want) continue;
            const char *name = self ? g_cfg.playerName : g_players[i].state.name;
            bool inGame = self ? GameState() == GS_PLAYING : g_players[i].state.inGame != 0;
            wsprintfA(buf, "%s%s - %s", name, i == 0 ? (fr ? " (hote)" : " (host)") : "", inGame ? (fr ? "en jeu" : "in game") : (fr ? "au menu" : "in menu"));
            int pct = self ? ModsPercent() : g_players[i].state.modsPct;
            if (i != 0 && pct < 100) wsprintfA(buf + strlen(buf), fr ? " (mods %d%%)" : " (mods %d%%)", pct);
            return Put(11 + want, buf);
        }
        return Put(11 + want, "-");
    }
    if (!strcmp(key, "VCC_CL")) return Put(28, g_cfg.host ? (fr ? "Fermer le salon" : "Close the lobby") : (fr ? "Quitter le salon" : "Leave the lobby"));
    if (!strcmp(key, "VCC_DC")) return Put(29, fr ? "Se deconnecter" : "Disconnect");
    if (!strcmp(key, "VCC_NW")) return Put(15, fr ? "Nouvelle partie" : "New game");
    if (!strcmp(key, "VCC_LD")) return Put(16, fr ? "Charger une partie" : "Load a game");
    if (!strcmp(key, "VCC_WT")) {
        if (g_localId <= 0 && g_joining) wsprintfA(buf, fr ? "Connexion a %s..." : "Connecting to %s...", g_cfg.address);
        else if (g_localId <= 0) wsprintfA(buf, fr ? "Pas de reponse de %s" : "No answer from %s", g_cfg.address);
        else if (GuestWaitingForSave()) wsprintfA(buf, fr ? "Chargement de la partie de l'hote..." : "Loading the host's game...");
        else if (!ModsReady()) wsprintfA(buf, fr ? "Mods de l'hote : %d%%" : "Host's mods: %d%%", ModsPercent());
        else if (g_players[0].state.inGame) wsprintfA(buf, fr ? "L'hote est en jeu : on y va" : "The host is in game: joining");
        else wsprintfA(buf, fr ? "En attente de l'hote..." : "Waiting for the host...");
        return Put(17, buf);
    }
    if (!strcmp(key, "VCC_PSE")) {
        wsprintfA(buf, "%s : %s%s", fr ? "Pseudo" : "Nickname", g_edit == EDIT_NICK ? g_editBuf : g_cfg.playerName,
                  g_edit == EDIT_NICK && (GetTickCount() / 400) % 2 ? "-" : "");
        return Put(5, buf);
    }
    return NULL;
}

typedef const wchar_t *(__fastcall *TextGet_t)(void *text, void *edx, const char *key);
static TextGet_t o_TextGet;
static const wchar_t *__fastcall h_TextGet(void *text, void *edx, const char *key)
{
    if (key && key[0] == 'V' && key[1] == 'C' && key[2] == 'C' && key[3] == '_') {
        const wchar_t *t = CoopText(key);
        if (t) return t;
    }
    return o_TextGet(text, edx, key);
}

// --- Actions ---
static void BeginEdit(EditField f)
{
    g_edit = f;
    lstrcpynA(g_editBuf, f == EDIT_ADDRESS ? g_cfg.address : g_cfg.playerName, sizeof(g_editBuf));
}

static void EndEdit(bool keep)
{
    if (keep && g_editBuf[0]) {
        if (g_edit == EDIT_ADDRESS) lstrcpynA(g_cfg.address, g_editBuf, sizeof(g_cfg.address));
        else lstrcpynA(g_cfg.playerName, g_editBuf, sizeof(g_cfg.playerName));
        SaveIni();
    }
    g_edit = EDIT_NONE;
}

static void SubBack()
{
    if (g_sub == SUB_POST || g_sub == SUB_AMB) { g_sub = SUB_VIDEO; *(int *)(Menu() + 0x30) = 0; return; }   // (parent de Options video garde)
    g_sub = g_subParent;
    g_subParent = SUB_MAIN;
    *(int *)(Menu() + 0x30) = 0;
}

static void OnCoopAction(int action)
{
    switch (action) {
    case ACT_CREATE:   // l'hote ouvre son salon (le contenu de l'ecran change au prochain passage)
        if (!g_netStarted) { g_cfg.host = true; CoopStartNetwork(); Log("menu : partie coop creee (salon)"); }
        break;
    case ACT_NEWGAME: case ACT_LOADGAME: {
        // Un invite telecharge encore les mods : on attend (sinon il jouerait avec d'autres modeles que nous).
        bool waiting = false;
        for (int i = 1; i < MAX_PLAYERS; i++) if (g_players[i].connected && g_players[i].state.modsPct < 100) waiting = true;
        if (waiting) { if (g_onNotice) g_onNotice("un invite telecharge encore les mods", "a guest is still downloading the mods", 0); break; }
        SwitchToNewScreen(action == ACT_NEWGAME ? PAGE_NEW_GAME : PAGE_LOAD_GAME);   // "Commencer une nouvelle partie ?" / liste des sauvegardes
        break;
    }
    case ACT_DRAWDIST: {
        static const int steps[] = { 100, 150, 200, 300, 400 };
        int i = 0;
        while (i < 5 && steps[i] <= g_cfg.drawDistance) i++;
        g_cfg.drawDistance = steps[i % 5];
        SaveIni();
        break;
    }
    case ACT_INFO: break;
    case ACT_JOIN:
        if (!g_netStarted) {
            g_cfg.host = false;
            CoopStartNetwork();
            Log("menu : connexion a %s", g_cfg.address);
        }
        if (!g_cfg.host) { g_joining = true; g_joinSince = GetTickCount(); }
        break;
    case ACT_FRIENDLY:
        if (g_netStarted && !g_cfg.host && g_localId > 0) break;   // l'hote decide
        g_cfg.friendlyFire = !g_cfg.friendlyFire; SaveIni(); break;
    case ACT_MONEY: g_cfg.shareMoney = !g_cfg.shareMoney; SaveIni(); break;
    case ACT_NAMES: g_cfg.showNames = !g_cfg.showNames; SaveIni(); break;
    case ACT_WEAPONS: g_cfg.keepWeapons = !g_cfg.keepWeapons; SaveIni(); break;
    case ACT_RAGDOLL: g_cfg.ragdoll = !g_cfg.ragdoll; SaveIni(); break;
    case ACT_POPZONE: g_cfg.zonePop = g_cfg.zonePop >= 200 ? 100 : g_cfg.zonePop + 25; SaveIni(); break;   // 100..200 par 25
    case ACT_POPDENS: {
        static const int steps[] = { 50, 100, 150, 200, 250, 300 };
        int i = 0;
        while (i < 6 && steps[i] <= g_cfg.popDensity) i++;
        g_cfg.popDensity = steps[i % 6];
        SaveIni();
        break;
    }
    case ACT_ADDRESS: BeginEdit(EDIT_ADDRESS); break;
    case ACT_NICK: BeginEdit(EDIT_NICK); break;
    case ACT_MSAA: g_cfg.msaa = g_cfg.msaa >= 8 ? 0 : g_cfg.msaa < 2 ? 2 : g_cfg.msaa * 2; SaveIni(); break;
    case ACT_ANISO: g_cfg.aniso = !g_cfg.aniso; SaveIni(); break;
    case ACT_SHADOWS: g_cfg.sunShadows = !g_cfg.sunShadows; SaveIni(); break;
    case ACT_RENDERER:   // ray tracing -> Direct3D 9 -> Direct3D 8 -> ray tracing (si vcrt64.exe est la)
        g_cfg.renderer = g_cfg.renderer == 12 ? 9 : g_cfg.renderer == 9 ? 8 : RtHelperPresent() ? 12 : 9;
        SaveIni();
        break;
    case ACT_WATER:   // non -> oui -> oui + reflets -> non
        if (!g_cfg.modernWater) { g_cfg.modernWater = true; g_cfg.waterReflections = false; }
        else if (!g_cfg.waterReflections) g_cfg.waterReflections = true;
        else g_cfg.modernWater = false;
        SaveIni(); break;
    case ACT_REFLECT: g_cfg.waterReflections = !g_cfg.waterReflections; SaveIni(); break;
    case ACT_LIGHTS:  // non -> oui -> avec ombres -> non
        if (!g_cfg.dynLights) { g_cfg.dynLights = true; g_cfg.lightShadows = 0; }
        else if (!g_cfg.lightShadows) g_cfg.lightShadows = 4;
        else g_cfg.dynLights = false;
        SaveIni(); break;
    case ACT_LIGHTSHADOWS: g_cfg.lightShadows = g_cfg.lightShadows ? 0 : 4; SaveIni(); break;
    case ACT_MOON: g_cfg.moonShadows = !g_cfg.moonShadows; SaveIni(); break;
    case ACT_AO: g_cfg.ambientOcclusion = !g_cfg.ambientOcclusion; SaveIni(); break;
    case ACT_FPS: g_cfg.fpsView = !g_cfg.fpsView; SaveIni(); break;
    case ACT_SHADOWQ:
        g_cfg.shadowRes = g_cfg.shadowRes >= 8192 ? 2048 : g_cfg.shadowRes >= 4096 ? 8192 : 4096;
        SaveIni();
        Gfx9SettingsChanged();
        break;
    case ACT_CLOSELOBBY: case ACT_DISCONNECT:
        // Salon ferme (hote) ou quitte (invite), ou invite qui se deconnecte en jeu (il continue seul) : le reseau
        // s'arrete, les autres sont prevenus, l'accueil COOP (Creer / Rejoindre) revient.
        NetStop();
        g_netStarted = false;
        g_joining = false;
        g_sub = SUB_MAIN; g_subParent = SUB_MAIN;
        *(int *)(Menu() + 0x30) = 0;
        Log("menu : %s", action == ACT_DISCONNECT ? "deconnexion" : g_cfg.host ? "salon ferme" : "salon quitte");
        break;
    case ACT_OPTIONS: g_subParent = SUB_MAIN; g_sub = SUB_OPTIONS; *(int *)(Menu() + 0x30) = 0; break;
    case ACT_JOINPAGE: g_subParent = SUB_MAIN; g_sub = SUB_JOIN; *(int *)(Menu() + 0x30) = 0; break;
    case ACT_OPTCOOP: g_subParent = g_sub; g_sub = SUB_COOP; *(int *)(Menu() + 0x30) = 0; break;
    case ACT_OPTVIDEO: g_subParent = g_sub; g_sub = SUB_VIDEO; *(int *)(Menu() + 0x30) = 0; break;
    case ACT_POSTPAGE: g_sub = SUB_POST; *(int *)(Menu() + 0x30) = 0; break;
    case ACT_AMBPAGE: g_sub = SUB_AMB; *(int *)(Menu() + 0x30) = 0; break;
    case ACT_SMAA: g_cfg.smaa = !g_cfg.smaa; SaveIni(); break;
    case ACT_BLOOM: g_cfg.bloom = !g_cfg.bloom; SaveIni(); break;
    case ACT_GRADE: g_cfg.grade = (g_cfg.grade + 1) % 3; SaveIni(); break;
    case ACT_SHARPEN: g_cfg.sharpen = g_cfg.sharpen >= 100 ? 0 : g_cfg.sharpen + 20; SaveIni(); break;
    case ACT_SOFTPART: g_cfg.softParticles = !g_cfg.softParticles; SaveIni(); break;
    case ACT_LAMPS: g_cfg.lampLights = !g_cfg.lampLights; SaveIni(); break;
    case ACT_WET: g_cfg.wetRoads = !g_cfg.wetRoads; SaveIni(); break;
    case ACT_RAYS: g_cfg.sunRays = !g_cfg.sunRays; SaveIni(); break;
    case ACT_WIND: g_cfg.windPlants = !g_cfg.windPlants; SaveIni(); break;
    case ACT_BEAMS: g_cfg.beams = !g_cfg.beams; SaveIni(); break;
    case ACT_HAZE: g_cfg.haze = !g_cfg.haze; SaveIni(); break;
    case ACT_CARREFL: g_cfg.carReflections = !g_cfg.carReflections; SaveIni(); break;
    case ACT_GI: g_cfg.indirectLight = !g_cfg.indirectLight; SaveIni(); break;
    case ACT_BACKSUB: SubBack(); break;
    }
}

typedef void(__fastcall *Buttons_t)(void *menu, void *edx, int down, int up, int select, int back, int wheel);
static Buttons_t o_Buttons;
static void __fastcall h_Buttons(void *menu, void *edx, int down, int up, int select, int back, int wheel)
{
    if (g_edit != EDIT_NONE) return;   // saisie en cours : le menu ne bouge pas
    // Menus resserres en grand ecran (display.cpp) : la souris recoit la transformation inverse, pour que les
    // clics et le curseur (dessine resserre) tombent juste. m_nMouseTempPosX +0x64 (brut), m_nMousePosX +0x12C.
    if (MenuSqueezeActive()) {
        float cx = *(int *)0x9B48DC * 0.5f;
        int raw = *(int *)(Menu() + 0x64);
        *(int *)(Menu() + 0x12C) = (int)(cx + (raw - cx) / MenuSqueezeFactor());
    }
    if (g_pendingPage >= 0) { int p = g_pendingPage; g_pendingPage = -1; SwitchToNewScreen(p); return; }
    if (g_pendingSelect >= 0) { *(int *)(Menu() + 0x30) = g_pendingSelect; g_pendingSelect = -1; select = 1; }
    if (g_pendingBack) { g_pendingBack = false; back = 1; }
    if ((char)back && CurrentPage() == PAGE_COOP && g_sub != SUB_MAIN) { SubBack(); return; }   // Echap : un cran plus haut
    if ((char)select && CurrentPage() == PAGE_COOP) {
        int action = Screens()[PAGE_COOP].entries[CurrentEntry()].action;
        if (action >= ACT_CREATE) { OnCoopAction(action); return; }
    }
    o_Buttons(menu, edx, down, up, select, back, wheel);
}

static LRESULT CALLBACK h_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// --- Contenu de l'ecran COOP, refait quand la situation change ---
static uint32_t g_layoutKey = 0xFFFFFFFF;

enum { COOP_LINE = 22 };
static int g_coopItems = 1;
float CoopMenuTextScale() { return CurrentPage() == PAGE_COOP ? 0.75f : 1.0f; }

static void BuildCoopPage()
{
    bool inGame = GameState() == GS_PLAYING;
    int players = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) players += (i == g_localId || g_players[i].connected || (g_localId < 0 && i == 0 && g_cfg.host && g_netStarted)) ? 1 : 0;
    bool lobby = g_netStarted && !inGame;
    // Hors de l'ecran COOP : on repart de l'accueil la prochaine fois.
    static int lastPage = -1;
    if (CurrentPage() != PAGE_COOP && lastPage == PAGE_COOP) { g_sub = SUB_MAIN; g_subParent = SUB_MAIN; }
    if (CurrentPage() == PAGE_COOP && lastPage != PAGE_COOP) *(int *)(Menu() + 0x30) = 0;   // en entrant : la 1re ligne
    lastPage = CurrentPage();
    // Jamais au-dela de la derniere ligne (la souris ou la page precedente laissaient la barre sur une ligne vide).
    if (CurrentPage() == PAGE_COOP && CurrentEntry() >= g_coopItems) *(int *)(Menu() + 0x30) = g_coopItems - 1;
    if (g_sub == SUB_JOIN && g_netStarted && !g_cfg.host && g_localId > 0) { g_sub = SUB_MAIN; g_subParent = SUB_MAIN; }   // connecte : le salon
    uint32_t key = (inGame ? 1 : 0) | (lobby ? 2 : 0) | (g_cfg.host ? 4 : 0) | (g_netStarted ? 8 : 0) | (players << 4) | (g_sub << 8);
    if (key == g_layoutKey) return;
    g_layoutKey = key;
    struct Item { uint16_t act; const char *label; } items[16];
    int n = 0;
    static const char *const pl[] = { "VCC_P0", "VCC_P1", "VCC_P2", "VCC_P3" };
    if (g_sub == SUB_OPTIONS) {
        items[n++] = { ACT_OPTCOOP, "VCC_OC" }; items[n++] = { ACT_OPTVIDEO, "VCC_OV" };
    } else if (g_sub == SUB_JOIN) {
        items[n++] = { ACT_ADDRESS, "VCC_IP" }; items[n++] = { ACT_JOIN, "VCC_JOI" };
    } else if (g_sub == SUB_COOP) {
        items[n++] = { ACT_FRIENDLY, "VCC_TA" }; items[n++] = { ACT_MONEY, "VCC_AP" };
        items[n++] = { ACT_NAMES, "VCC_PS2" }; items[n++] = { ACT_WEAPONS, "VCC_GA" };
        items[n++] = { ACT_POPZONE, "VCC_PZ" }; items[n++] = { ACT_POPDENS, "VCC_PD" };   // (reglages de l'hote, envoyes)
        items[n++] = { ACT_RAGDOLL, "VCC_RG" };
    } else if (g_sub == SUB_VIDEO) {
        items[n++] = { ACT_DRAWDIST, "VCC_DD" }; items[n++] = { ACT_MSAA, "VCC_AA" }; items[n++] = { ACT_ANISO, "VCC_AF" };
        items[n++] = { ACT_SHADOWS, "VCC_SH" };
        if (ModernRenderer()) {
            if (g_cfg.renderer != 12) items[n++] = { ACT_SHADOWQ, "VCC_SQ" };   // (ray tracing : pas de cascades)
            items[n++] = { ACT_WATER, "VCC_WA" };    // reflets compris
            items[n++] = { ACT_LIGHTS, "VCC_LI" };   // ombres des lumieres comprises
        }
        items[n++] = { ACT_FPS, "VCC_FP" };
        items[n++] = { ACT_RENDERER, "VCC_RD" };
        if (ModernRenderer()) { items[n++] = { ACT_POSTPAGE, "VCC_PP" }; items[n++] = { ACT_AMBPAGE, "VCC_AM" }; }
    } else if (g_sub == SUB_POST) {
        items[n++] = { ACT_SMAA, "VCC_SM" }; items[n++] = { ACT_BLOOM, "VCC_BL" };
        items[n++] = { ACT_GRADE, "VCC_GR" }; items[n++] = { ACT_SHARPEN, "VCC_NT" };
    } else if (g_sub == SUB_AMB) {
        items[n++] = { ACT_AO, "VCC_AO" }; items[n++] = { ACT_MOON, "VCC_MO" };
        items[n++] = { ACT_LAMPS, "VCC_LP" }; items[n++] = { ACT_SOFTPART, "VCC_SP" };
        items[n++] = { ACT_WET, "VCC_WE" }; items[n++] = { ACT_RAYS, "VCC_RY" }; items[n++] = { ACT_WIND, "VCC_WI" };
        items[n++] = { ACT_BEAMS, "VCC_BE" }; items[n++] = { ACT_HAZE, "VCC_HZ" };
        items[n++] = { ACT_CARREFL, "VCC_CR" }; items[n++] = { ACT_GI, "VCC_GI" };
    } else if (!g_netStarted && !inGame) {
        // Accueil : Creer / Rejoindre / Pseudo / Options (coop + video : a regler avant de creer ou rejoindre).
        items[n++] = { ACT_CREATE, "VCC_CRE" }; items[n++] = { ACT_JOINPAGE, "VCC_JP" };
        items[n++] = { ACT_NICK, "VCC_PSE" }; items[n++] = { ACT_OPTIONS, "VCC_OPT" };
    } else {
        for (int i = 0; i < players && i < 4; i++) items[n++] = { ACT_INFO, pl[i] };
        if (lobby && g_cfg.host) { items[n++] = { ACT_NEWGAME, "VCC_NW" }; items[n++] = { ACT_LOADGAME, "VCC_LD" }; }
        else if (lobby) items[n++] = { ACT_INFO, "VCC_WT" };
        if (g_cfg.host) items[n++] = { ACT_OPTCOOP, "VCC_OC" };   // salon et en partie : l'hote regle la coop
        if (inGame || lobby) items[n++] = { ACT_OPTVIDEO, "VCC_OV" };   // salon et en partie (Echap > COOP) : options video
        if (lobby) items[n++] = { ACT_CLOSELOBBY, "VCC_CL" };                       // fermer / quitter le salon
        else if (inGame && !g_cfg.host && g_netStarted) items[n++] = { ACT_DISCONNECT, "VCC_DC" };   // invite : se deconnecter
    }
    // La page du jeu a 12 lignes en tout (MenuScreen::entries) : au-dela, on ecrasait la page suivante et le jeu
    // plantait en ouvrant Options video (13 lignes en 28y). "Retour" garde toujours sa place.
    if (n > 11) { Log("menu : %d lignes, coupees a 11 (+ Retour)", n); n = 11; }
    items[n++] = { (uint16_t)(g_sub == SUB_MAIN ? ACT_GOBACK : ACT_BACKSUB), "FEDS_TB" };
    MenuScreen &c = Screens()[PAGE_COOP];
    memset(c.entries, 0, sizeof(c.entries));
    for (int i = 0; i < n; i++) {
        MenuEntry &e = c.entries[i];
        e.action = items[i].act;
        lstrcpynA(e.label, items[i].label, 8);
        e.target = 0x7F;   // "Retour" : page d'ouverture (principal ou pause)
        e.align = 3;       // centre
    }
    // Lignes placees par nous, plus serrees que celles du jeu (29 unites sur 448) : jusqu'a 12 lignes tiennent sans
    // descendre au pied de l'ecran (le texte de cette page est aussi plus petit, CoopMenuTextScale).
    for (int i = 0; i < n; i++) { c.entries[i].x = 320; c.entries[i].y = (uint16_t)(110 + i * COOP_LINE); }
    g_coopItems = n;
    if (CurrentPage() == PAGE_COOP && CurrentEntry() >= n) *(int *)(Menu() + 0x30) = n - 1;
}

void MenuFrame()
{
    BuildCoopPage();
    // Invite dans le salon : des que l'hote est en jeu, on le suit. S'il a charge une sauvegarde, elle arrive
    // (saveshare.cpp) et se charge seule ; sinon, nouvelle partie ("Oui" valide automatiquement).
    static bool autoYes;
    if (!g_cfg.host && g_netStarted && GameState() == GS_FRONTEND && g_localId > 0 && !g_cfg.autoStart) {
        static uint32_t hostInGameSince;
        g_joining = false;
        if (!g_players[0].state.inGame || GuestWaitingForSave() || !ModsReady()) hostInGameSince = 0;
        else if (!hostInGameSince) hostInGameSince = GetTickCount();
        else if (GetTickCount() - hostInGameSince > 2500 && CurrentPage() != PAGE_NEW_GAME && !autoYes) {
            autoYes = true;
            Log("menu : l'hote est en jeu, nouvelle partie");
            g_pendingPage = PAGE_NEW_GAME;
        }
    }
    // "Oui" une seconde apres l'ouverture de l'ecran (le menu ignore une validation faite des son ouverture).
    static uint32_t onNewGameSince;
    if (!autoYes || CurrentPage() != PAGE_NEW_GAME) onNewGameSince = 0;
    else if (!onNewGameSince) onNewGameSince = GetTickCount();
    else if (GetTickCount() - onNewGameSince > 1000 && g_pendingSelect < 0 && g_pendingPage < 0) { g_pendingSelect = 2; autoYes = false; }   // FEM_YES
    // Test (TestMenu=N) : ouvre l'ecran N du menu 3 s apres l'arrivee au menu principal.
    static uint32_t atMenu;
    static bool tested;
    if (g_cfg.testMenu && !tested && GameState() == GS_FRONTEND) {
        if (!atMenu) atMenu = GetTickCount();
        else if (GetTickCount() - atMenu > 3000) {
            tested = true;
            Log("menu : test, ecran %d (page avant %d)", g_cfg.testMenu, CurrentPage());
            g_pendingPage = g_cfg.testMenu;
        }
    }
    if (tested && g_cfg.testMenu) { static int n; if (n++ % 60 == 0 && n < 400) Log("menu : page %d entree %d", CurrentPage(), CurrentEntry()); }
    // Test (TestMenuPlan=creer|rejoindre) : valide l'entree voulue de l'ecran COOP, puis "Oui" a la nouvelle partie,
    // en passant par le vrai traitement des boutons du menu.
    if (tested && g_cfg.testMenuPlan[0] && GameState() == GS_FRONTEND) {
        static uint32_t since, step;
        uint32_t now = GetTickCount();
        if (!since) since = now;
        if (now - since < 1500) return;
        if (_stricmp(g_cfg.testMenuPlan, "saisie") == 0) {
            // Ouvre le champ Adresse, tape "10.0.0.5" (en laissant voir la saisie 3 s), puis Entree.
            static const char *typed = "10.0.0.5";
            static int pos;
            if (step == 0 && CurrentPage() == PAGE_COOP) { g_pendingSelect = 1; step = 10; since = now; }   // Rejoindre
            else if (step == 10 && now - since > 1500) { g_pendingSelect = 0; step = 1; since = now; }      // Adresse
            else if (step == 1 && g_edit == EDIT_ADDRESS && typed[pos]) h_WndProc(GameWindow(), WM_CHAR, (unsigned char)typed[pos++], 0);
            else if (step == 1 && !typed[pos] && now - since > 4500) { h_WndProc(GameWindow(), WM_CHAR, 13, 0); step = 2; Log("menu : test, adresse saisie : %s", g_cfg.address); }
            return;
        }
        if (_stricmp(g_cfg.testMenuPlan, "options") == 0) {   // Options > Options video, puis Echap x2 (retour accueil)
            static bool shown, pagesDone;
            // Lignes cherchees par action (leur place change selon l'etat du menu).
            auto find = [](int act) { for (int i = 0; i < 12; i++) if (Screens()[PAGE_COOP].entries[i].action == act) return i; return -1; };
            if (step == 0 && CurrentPage() == PAGE_COOP && find(ACT_OPTIONS) >= 0) { g_pendingSelect = find(ACT_OPTIONS); step = 1; since = now; }
            else if (step == 0 && CurrentPage() == PAGE_COOP && find(ACT_OPTVIDEO) >= 0) { g_pendingSelect = find(ACT_OPTVIDEO); step = 2; since = now; }   // salon
            else if (step == 1 && now - since > 1500 && find(ACT_OPTVIDEO) >= 0) {
                g_pendingSelect = find(ACT_OPTVIDEO); step = 2; since = now; Log("menu : test, sous-page %d", g_sub);
            } else if (step == 2 && now - since > 1500 && g_sub == SUB_VIDEO && !shown) {
                shown = true;
                int n = 0; for (int i = 0; i < 12; i++) n += Screens()[PAGE_COOP].entries[i].action != 0;
                Log("menu : test, Options video ouvertes : %d lignes, page suivante intacte : %.8s", n, Screens()[PAGE_COOP + 1].name);
            }
            else if (step == 2 && now - since > 3000 && !pagesDone && find(ACT_POSTPAGE) >= 0) { g_pendingSelect = find(ACT_POSTPAGE); step = 20; since = now; }
            else if ((step == 20 || step == 22) && now - since > 1500) {
                int n = 0; for (int i = 0; i < 12; i++) n += Screens()[PAGE_COOP].entries[i].action != 0;
                Log("menu : test, sous-page %d : %d lignes", g_sub, n);
                g_pendingBack = true; step++; since = now;
            }
            else if (step == 21 && now - since > 1500 && find(ACT_AMBPAGE) >= 0) { g_pendingSelect = find(ACT_AMBPAGE); step = 22; since = now; }
            else if (step == 23 && now - since > 1500) { pagesDone = true; step = 2; since = now - 3000; Log("menu : test, retour sous-page %d", g_sub); }
            else if (step == 2 && now - since > 4000 && (pagesDone || find(ACT_POSTPAGE) < 0)) { g_pendingBack = true; step = 3; since = now; Log("menu : test, sous-page %d", g_sub); }
            else if (step == 3 && now - since > 1500) { g_pendingBack = true; step = 4; since = now; Log("menu : test, sous-page %d", g_sub); }
            else if (step == 4 && now - since > 1500) { step = 5; Log("menu : test, sous-page %d, page %d", g_sub, CurrentPage()); }
            return;
        }
        if (_stricmp(g_cfg.testMenuPlan, "fermer") == 0) {   // Creer une partie, puis Fermer le salon
            if (step == 0 && CurrentPage() == PAGE_COOP) { g_pendingSelect = 0; step = 1; since = now; Log("menu : test, Creer"); }
            else if (step == 1 && now - since > 3000) {
                for (int i = 0; i < 12; i++) if (Screens()[PAGE_COOP].entries[i].action == ACT_CLOSELOBBY) g_pendingSelect = i;
                step = 2; since = now; Log("menu : test, Fermer le salon (reseau %d)", g_netStarted);
            } else if (step == 2 && now - since > 2000) { step = 3; Log("menu : test, apres fermeture : reseau %d, 1re entree %d", g_netStarted, Screens()[PAGE_COOP].entries[0].action); }
            return;
        }
        bool create = _stricmp(g_cfg.testMenuPlan, "creer") == 0;
        if (step == 0 && CurrentPage() == PAGE_COOP) {
            g_pendingSelect = create ? 0 : 1;
            Log("menu : test, %s valide", create ? "Creer" : "Rejoindre");
            step = 1; since = now;
        } else if (step == 1 && create && CurrentPage() == PAGE_COOP && now - since > 8000) {
            for (int i = 0; i < 12; i++) if (Screens()[PAGE_COOP].entries[i].action == ACT_NEWGAME) g_pendingSelect = i;
            Log("menu : test, salon : Nouvelle partie");
            since = now;
        } else if (step == 1 && create && CurrentPage() == PAGE_NEW_GAME) {
            g_pendingSelect = 2;   // FEM_YES
            Log("menu : test, Oui a la nouvelle partie");
            step = 2;
        }
    }
    if (!g_joining) return;
    if (g_localId > 0) {
        g_joining = false;   // la suite (suivre l'hote) est plus haut
        Log("menu : connecte, dans le salon");
    } else if (GetTickCount() - g_joinSince > 10000) {
        g_joining = false;
        Log("menu : pas de reponse de %s", g_cfg.address);
    }
}

// --- Saisie de texte : la fenetre du jeu est sous-classee pendant qu'on edite ---
static WNDPROC o_WndProc;
static LRESULT CALLBACK h_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_edit == EDIT_NONE && PanelWndProc(msg, wp, lp)) return 0;   // menu en jeu, tchat (panel.cpp)
    if (g_edit != EDIT_NONE) {
        if (msg == WM_CHAR) {
            size_t n = strlen(g_editBuf);
            if (wp == 8) { if (n) g_editBuf[n - 1] = 0; }
            else if (wp == 13) EndEdit(true);
            else if (wp == 27) EndEdit(false);
            else if (wp == 22) {   // Ctrl+V : colle le texte du presse-papiers (adresse copiee depuis Discord, Radmin...)
                if (OpenClipboard(hwnd)) {
                    if (HANDLE h = GetClipboardData(CF_TEXT)) {
                        if (const char *t = (const char *)GlobalLock(h)) {
                            size_t cap = g_edit == EDIT_NICK ? 16u : 60u;
                            for (; *t && n + 1 < cap; t++) {
                                unsigned char c = (unsigned char)*t;
                                if (c < 32 || c >= 127 || (g_edit == EDIT_ADDRESS && c == ' ')) continue;   // retours a la ligne, espaces
                                g_editBuf[n++] = (char)c;
                            }
                            g_editBuf[n] = 0;
                            GlobalUnlock(h);
                        }
                    }
                    CloseClipboard();
                }
            }
            else if (wp >= 32 && wp < 127 && n + 1 < (g_edit == EDIT_NICK ? 16u : 60u)) { g_editBuf[n] = (char)wp; g_editBuf[n + 1] = 0; }
            return 0;
        }
        // Le jeu lit le clavier par WM_KEYDOWN/UP : on les avale pendant la saisie. WM_CHAR arrive quand meme
        // (TranslateMessage est appele par la boucle de messages avant la distribution).
        if (msg == WM_KEYDOWN || msg == WM_KEYUP) return 0;
    }
    return CallWindowProcA(o_WndProc, hwnd, msg, wp, lp);
}

void MenuWindowCreated(HWND hwnd)
{
    if (!o_WndProc) o_WndProc = (WNDPROC)SetWindowLongA(hwnd, GWL_WNDPROC, (LONG)h_WndProc);
}

// L'ecran 33 sert au jeu de marqueur "pas encore d'ecran" : a l'ouverture du menu (0x4A3BCD) la page vaut 33,
// puis a chaque image (0x4A37A4) 33 devient le menu principal (29) ou la pause (32). On choisit directement la
// bonne page a l'ouverture et on neutralise la conversion : l'ecran 33 devient le notre.
static void __cdecl PickOpeningPage()
{
    *(int *)(Menu() + 0xF8) = *(char *)(Menu() + 0x6C) ? PAGE_MAIN : 32;   // m_bGameNotLoaded ? principal : pause
}

static __declspec(naked) void PickOpeningPageStub()
{
    __asm {
        pushad
        call PickOpeningPage
        popad
        ret
    }
}

static bool FreePage33()
{
    static const uint8_t cmp33[] = { 0x83, 0xBB, 0xF8, 0x00, 0x00, 0x00, 0x21 };
    static const uint8_t mov33[] = { 0xC7, 0x83, 0xF8, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00 };
    if (memcmp((void *)0x4A380F, cmp33, sizeof(cmp33)) != 0 || memcmp((void *)0x4A3D09, mov33, sizeof(mov33)) != 0) {
        Log("menu : code du marqueur de l'ecran 33 inattendu");
        return false;
    }
    uint8_t never = 0x7F;
    Patch(0x4A3815, &never, 1);
    PatchCall(0x4A3D09, (void *)PickOpeningPageStub, sizeof(mov33));
    // Les ecrans du jeu qui "reviennent" a 33 (stats, briefing, carte, options, audio, affichage, langue, quitter...)
    // voulaient dire "retour a la page d'ouverture" : ils visent maintenant 127, que la conversion (0x4A3815, ci-dessus)
    // change en menu principal (29) ou en pause (32). Sinon Echap depuis la pause ramenait au menu principal du jeu
    // (JOUER / COOP / OPTIONS...) sans jamais revenir a la partie.
    int fixed = 0;
    for (int p = 0; p < PAGE_COOP; p++) {
        MenuScreen &s = Screens()[p];
        if (s.prevPage == PAGE_COOP) { s.prevPage = 0x7F; fixed++; }
        for (MenuEntry &e : s.entries) if (e.label[0] && e.target == PAGE_COOP) { e.target = 0x7F; fixed++; }
    }
    Log("menu : %d retours vers la page d'ouverture corriges", fixed);
    return true;
}

void InstallMenu()
{
    if (!FreePage33()) return;
    // Menu principal : Commencer partie / COOP / Options / Quitter.
    MenuScreen &mm = Screens()[PAGE_MAIN];
    if (mm.entries[0].action != ACT_CHANGEMENU || strcmp(mm.entries[1].label, "FEP_OPT") != 0 || mm.entries[3].action != 0) {
        Log("menu : menu principal inattendu, pas d'entree COOP");
        return;
    }
    memmove(&mm.entries[2], &mm.entries[1], 2 * sizeof(MenuEntry));
    MenuEntry coop = {};
    coop.action = ACT_CHANGEMENU;
    memcpy(coop.label, "VCC_MM", 7);
    coop.target = PAGE_COOP;
    coop.align = mm.entries[2].align;
    mm.entries[1] = coop;
    // Menu pause (32) : Reprendre / ... / Briefing / COOP / Options / Quitter (reglages accessibles en partie).
    MenuScreen &pause = Screens()[32];
    if (!strcmp(pause.entries[5].label, "FEP_OPT") && !pause.entries[7].label[0]) {
        memmove(&pause.entries[6], &pause.entries[5], 2 * sizeof(MenuEntry));
        pause.entries[5] = coop;
        pause.entries[5].align = pause.entries[6].align;
    }

    // Ecran COOP (33, vide dans le jeu) : son contenu est fait par BuildCoopPage.
    MenuScreen &c = Screens()[PAGE_COOP];
    memset(&c, 0, sizeof(c));
    memcpy(c.name, "VCC_TIT", 8);
    c.prevPage = 0x7F;   // retour : menu principal ou pause, selon qu'on est en partie
    c.parentEntry = 1;
    BuildCoopPage();

    static const uint8_t textPro[] = { 0x53, 0x55, 0x89, 0xCB, 0x83, 0xEC, 0x40 };
    o_TextGet = (TextGet_t)MakeDetour(0x584F30, textPro, sizeof(textPro), (void *)h_TextGet);
    static const uint8_t btnPro[] = { 0x53, 0x56, 0x57, 0x55, 0x89, 0xCD };
    o_Buttons = (Buttons_t)MakeDetour(0x4990DD, btnPro, sizeof(btnPro), (void *)h_Buttons);
    Log("menu : entree COOP ajoutee");
}
