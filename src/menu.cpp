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
#include "net.h"
#include "game.h"
#include "saveshare.h"
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
       ACT_FRIENDLY, ACT_MONEY, ACT_NAMES, ACT_WEAPONS, ACT_INFO, ACT_NEWGAME, ACT_LOADGAME, ACT_DRAWDIST };
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
    wsprintfA(ini, "%svccoop.ini", GameDir());
    WritePrivateProfileStringA("VCCoop", "Adresse", g_cfg.address, ini);
    WritePrivateProfileStringA("VCCoop", "Pseudo", g_cfg.playerName, ini);
    WritePrivateProfileStringA("VCCoop", "TirAmi", g_cfg.friendlyFire ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "ArgentPartage", g_cfg.shareMoney ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "AfficherPseudos", g_cfg.showNames ? "1" : "0", ini);
    WritePrivateProfileStringA("VCCoop", "GarderArmes", g_cfg.keepWeapons ? "1" : "0", ini);
    char dd[16];
    wsprintfA(dd, "%d", g_cfg.drawDistance);
    WritePrivateProfileStringA("VCCoop", "DistanceAffichage", dd, ini);
}

// --- Textes ---
static wchar_t g_text[20][80];

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
    if (!strcmp(key, "VCC_TIT")) return Put(1, "COOP");
    if (!strcmp(key, "VCC_CRE")) return Put(2, fr ? "Creer une partie" : "Host a game");
    if (!strcmp(key, "VCC_JOI")) {
        if (g_joining) wsprintfA(buf, fr ? "Rejoindre : connexion..." : "Join: connecting...");
        else if (g_netStarted && !g_cfg.host && g_localId > 0) wsprintfA(buf, fr ? "Rejoindre : connecte" : "Join: connected");
        else wsprintfA(buf, fr ? "Rejoindre" : "Join");
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
    if (!strcmp(key, "VCC_GA")) {
        wsprintfA(buf, "%s : %s", fr ? "Garder ses armes" : "Keep weapons", g_cfg.keepWeapons ? yes : no);
        return Put(9, buf);
    }
    if (!strcmp(key, "VCC_DD")) {
        wsprintfA(buf, "%s : %d%%", fr ? "Distance d'affichage" : "Draw distance", g_cfg.drawDistance);
        return Put(10, buf);
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
            return Put(11 + want, buf);
        }
        return Put(11 + want, "-");
    }
    if (!strcmp(key, "VCC_NW")) return Put(15, fr ? "Nouvelle partie" : "New game");
    if (!strcmp(key, "VCC_LD")) return Put(16, fr ? "Charger une partie" : "Load a game");
    if (!strcmp(key, "VCC_WT")) {
        if (g_localId <= 0 && g_joining) wsprintfA(buf, fr ? "Connexion a %s..." : "Connecting to %s...", g_cfg.address);
        else if (g_localId <= 0) wsprintfA(buf, fr ? "Pas de reponse de %s" : "No answer from %s", g_cfg.address);
        else if (GuestWaitingForSave()) wsprintfA(buf, fr ? "Chargement de la partie de l'hote..." : "Loading the host's game...");
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

static void OnCoopAction(int action)
{
    switch (action) {
    case ACT_CREATE:   // l'hote ouvre son salon (le contenu de l'ecran change au prochain passage)
        if (!g_netStarted) { g_cfg.host = true; CoopStartNetwork(); Log("menu : partie coop creee (salon)"); }
        break;
    case ACT_NEWGAME: SwitchToNewScreen(PAGE_NEW_GAME); break;    // "Commencer une nouvelle partie ?" du jeu
    case ACT_LOADGAME: SwitchToNewScreen(PAGE_LOAD_GAME); break;  // liste des sauvegardes : elle partira aux invites
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
    case ACT_ADDRESS: BeginEdit(EDIT_ADDRESS); break;
    case ACT_NICK: BeginEdit(EDIT_NICK); break;
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
    if ((char)select && CurrentPage() == PAGE_COOP) {
        int action = Screens()[PAGE_COOP].entries[CurrentEntry()].action;
        if (action >= ACT_CREATE) { OnCoopAction(action); return; }
    }
    o_Buttons(menu, edx, down, up, select, back, wheel);
}

static LRESULT CALLBACK h_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// --- Contenu de l'ecran COOP, refait quand la situation change ---
static uint32_t g_layoutKey = 0xFFFFFFFF;

static void BuildCoopPage()
{
    bool inGame = GameState() == GS_PLAYING;
    int players = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) players += (i == g_localId || g_players[i].connected || (g_localId < 0 && i == 0 && g_cfg.host && g_netStarted)) ? 1 : 0;
    bool lobby = g_netStarted && !inGame;
    uint32_t key = (inGame ? 1 : 0) | (lobby ? 2 : 0) | (g_cfg.host ? 4 : 0) | (g_netStarted ? 8 : 0) | (players << 4);
    if (key == g_layoutKey) return;
    g_layoutKey = key;
    struct Item { uint16_t act; const char *label; } items[12];
    int n = 0;
    static const char *const pl[] = { "VCC_P0", "VCC_P1", "VCC_P2", "VCC_P3" };
    if (!g_netStarted && !inGame) {
        items[n++] = { ACT_CREATE, "VCC_CRE" }; items[n++] = { ACT_JOIN, "VCC_JOI" };
        items[n++] = { ACT_ADDRESS, "VCC_IP" }; items[n++] = { ACT_NICK, "VCC_PSE" };
    } else {
        for (int i = 0; i < players && i < 4; i++) items[n++] = { ACT_INFO, pl[i] };
        if (lobby && g_cfg.host) { items[n++] = { ACT_NEWGAME, "VCC_NW" }; items[n++] = { ACT_LOADGAME, "VCC_LD" }; }
        else if (lobby) items[n++] = { ACT_INFO, "VCC_WT" };
    }
    const Item settings[] = { { ACT_FRIENDLY, "VCC_TA" }, { ACT_MONEY, "VCC_AP" }, { ACT_NAMES, "VCC_PS2" },
                              { ACT_WEAPONS, "VCC_GA" }, { ACT_DRAWDIST, "VCC_DD" } };
    for (const Item &s : settings) if (n < 11) items[n++] = s;
    items[n++] = { ACT_GOBACK, "FEDS_TB" };
    MenuScreen &c = Screens()[PAGE_COOP];
    memset(c.entries, 0, sizeof(c.entries));
    for (int i = 0; i < n; i++) {
        MenuEntry &e = c.entries[i];
        e.action = items[i].act;
        lstrcpynA(e.label, items[i].label, 8);
        e.target = 0x7F;   // "Retour" : page d'ouverture (principal ou pause)
        e.align = 3;       // centre
    }
    c.entries[0].x = 320;
    c.entries[0].y = 110;   // meme depart pour tous les contenus (la barre de selection suit ce depart)
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
        if (!g_players[0].state.inGame || GuestWaitingForSave()) hostInGameSince = 0;
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
            if (step == 0 && CurrentPage() == PAGE_COOP) { g_pendingSelect = 2; step = 1; since = now; }
            else if (step == 1 && g_edit == EDIT_ADDRESS && typed[pos]) h_WndProc(GameWindow(), WM_CHAR, (unsigned char)typed[pos++], 0);
            else if (step == 1 && !typed[pos] && now - since > 4500) { h_WndProc(GameWindow(), WM_CHAR, 13, 0); step = 2; Log("menu : test, adresse saisie : %s", g_cfg.address); }
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
    if (g_edit != EDIT_NONE) {
        if (msg == WM_CHAR) {
            size_t n = strlen(g_editBuf);
            if (wp == 8) { if (n) g_editBuf[n - 1] = 0; }
            else if (wp == 13) EndEdit(true);
            else if (wp == 27) EndEdit(false);
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
