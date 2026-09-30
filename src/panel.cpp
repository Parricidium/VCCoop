// Menu en jeu (ToucheMenuJeu, F10) : onglets Joueurs (tout le monde : aller vers un joueur), Vehicules, Outils,
// Monde (admins) et Hote (l'hote). L'hote est admin d'office et en nomme d'autres (par pseudo, AdminsJeu= dans son
// vccoop.ini). Tchat (ToucheTchat, T) : pendant la saisie, Tommy sort son telephone (CPed::SetAnswerMobile), et les
// autres le voient (animations recopiees, telephone pose sur le double par coop.cpp PuppetPhone).
// Dessin par les fonctions 2D du jeu (CSprite2d::Draw2DPolygon, CFont) : marche avec les deux rendus (D3D8 et D3D9).
// La souris est lue a la source (DirectInput, camera.cpp) et cachee au jeu tant que le menu est ouvert ; le clavier
// passe par la fenetre (menu.cpp) : les appuis sont avales, les relachements passent (pas de touche collee).
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "panel.h"
#include "ui9.h"
#include "players.h"
#include "mirror.h"
#include "vehicles.h"
#include "seats.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

using namespace game;

enum { RL_CHAT = 30, RL_ADMINS = 31, RL_ADMIN_REQ = 32, RL_BRING = 33 };
#pragma pack(push, 1)
struct RlChat { uint8_t type, player; char text[100]; };
struct RlAdmins { uint8_t type, mask; };
struct RlAdminReq { uint8_t type, cmd, target; int32_t a; };
struct RlBring { uint8_t type, target, area; float pos[3]; };
#pragma pack(pop)
enum { ADM_TIME = 1, ADM_WEATHER = 2, ADM_KICK = 3 };

void NetKick(int id);   // net.cpp
void NetVehicleShare(void *veh);   // vehicles.cpp
void MenuSaveIni();     // menu.cpp

static int ScreenW() { return *(int *)0x9B48DC; }
static int ScreenH() { return *(int *)0x9B48E0; }
static bool French() { return *(int *)(0x869630 + 0x50) == 1; }
static int &Money() { return *(int *)(0x94AD28 + 0xA0); }   // CWorld::Players[0].m_nMoney
static bool PlayerFree() { return !*(bool *)0xA10AB2 && *(short *)(0x7DBCB0 + 0xF0) == 0; }   // pas de cinematique, controles rendus

// ======================================================================= Etat
static bool g_open, g_typing, g_skipChar, g_phoneOn;
static int g_phoneState;
static int g_tab;
static int g_menuKey = VK_F10, g_chatKey = 'T';
static float g_mx = -1, g_my;
static long g_accX, g_accY, g_accWheel;
static bool g_left, g_prevLeft, g_click;
static int g_scroll, g_vehCat;
static uint8_t g_adminMask;   // bit i : le joueur i est admin (l'hote, bit 0, l'est toujours)
static bool g_noPolice;
static char g_input[100];
static int g_inputLen;

static bool IsAdmin(int id) { return id == 0 || (g_adminMask & (1 << id)) != 0; }
static bool LocalAdmin() { return g_cfg.host || g_localId < 0 || IsAdmin(g_localId); }

// Actions demandees par un clic (au dessin) et faites dans la logique du jeu (PanelFrame) : on ne cree pas de vehicule
// en plein rendu.
enum { A_NONE, A_GOTO, A_BRING, A_KICK, A_TOGGLE_ADMIN, A_SPAWN, A_HEAL, A_WEAPONS, A_MONEY, A_REPAIR, A_FLIP, A_WANTED,
       A_NOPOLICE, A_TIME, A_WEATHER, A_OPTION };
struct Action { int kind, a; };
static Action g_actions[8];
static int g_actionCount;
static void Queue(int kind, int a = 0) { if (g_actionCount < 8) g_actions[g_actionCount++] = { kind, a }; }

// ======================================================================= Vehicules (data\default.ide)
enum { CAT_CARS, CAT_BIKES, CAT_BOATS, CAT_AIR, CAT_COUNT };
struct VehEntry { int id; char name[24]; uint8_t cat; };
static VehEntry g_vehList[160];
static int g_vehCount;

static void LoadVehicleList()
{
    static bool done;
    if (done) return;
    done = true;
    char path[MAX_PATH];
    wsprintfA(path, "%sdata\\default.ide", GameDir());
    FILE *f = fopen(path, "r");
    if (!f) { Log("menu jeu : %s introuvable, pas de liste de vehicules", path); return; }
    char line[512];
    bool cars = false;
    while (fgets(line, sizeof(line), f) && g_vehCount < 160) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!_strnicmp(p, "cars", 4)) { cars = true; continue; }
        if (!_strnicmp(p, "end", 3)) { cars = false; continue; }
        if (!cars || *p == '#' || *p < '0' || *p > '9') continue;
        char *tok[6] = {};
        int n = 0;
        for (char *t = strtok(p, ","); t && n < 6; t = strtok(NULL, ",")) { while (*t == ' ' || *t == '\t') t++; tok[n++] = t; }
        if (n < 4) continue;
        int id = atoi(tok[0]);
        char name[24], type[16];
        if (sscanf(tok[1], "%23s", name) != 1 || sscanf(tok[3], "%15s", type) != 1) continue;
        // Ni trains, ni voitures telecommandees, ni les avions du decor (ils ne se pilotent pas).
        if (!_stricmp(type, "train") || !_strnicmp(name, "rc", 2) || !_stricmp(name, "airtrain") || !_stricmp(name, "deaddodo")) continue;
        VehEntry &e = g_vehList[g_vehCount++];
        e.id = id;
        lstrcpynA(e.name, name, sizeof(e.name));
        if (e.name[0] >= 'a' && e.name[0] <= 'z') e.name[0] -= 32;
        e.cat = !_stricmp(type, "bike") ? CAT_BIKES : !_stricmp(type, "boat") ? CAT_BOATS :
                (!_stricmp(type, "heli") || !_stricmp(type, "plane")) ? CAT_AIR : CAT_CARS;
    }
    fclose(f);
    Log("menu jeu : %d vehicules dans default.ide", g_vehCount);
}

static void LoadModelNow(int model)
{
    if (HasModelLoaded(model)) return;
    RequestModel(model, 1);
    ((void(__cdecl *)(bool))0x40B5F0)(false);   // CStreaming::LoadAllRequestedModels
}

static void SpawnVehicle(int index)
{
    if (index < 0 || index >= g_vehCount) return;
    const VehEntry &e = g_vehList[index];
    void *me = FindPlayerPed();
    if (!me) return;
    LoadModelNow(e.id);
    if (!HasModelLoaded(e.id)) { Log("menu jeu : modele %d (%s) pas charge, vehicule non cree", e.id, e.name); return; }
    void *v = VehicleAlloc();
    if (!v) { Log("menu jeu : plus de place pour un vehicule"); return; }
    switch (e.cat) {
    case CAT_BOATS: BoatCtor(v, e.id, 1); break;
    case CAT_BIKES: BikeCtor(v, e.id, 1); break;
    default: AutomobileCtor(v, e.id, 1); break;   // helicopteres et avions aussi (comme CREATE_CAR)
    }
    // Devant le joueur (ou devant son vehicule), dans le meme sens.
    Vec3 at = Pos(me), fwd;
    if (InVehicle(me) && PedVehicle(me)) {
        void *cur = PedVehicle(me);
        at = Pos(cur);
        fwd = Field<Vec3>(cur, 0x14);
    } else {
        float h = Heading(me);
        fwd = { -sinf(h), cosf(h), 0.0f };
    }
    float l = sqrtf(fwd.x * fwd.x + fwd.y * fwd.y);
    if (l < 0.01f) { fwd = { 0, 1, 0 }; l = 1; }
    fwd.x /= l; fwd.y /= l; fwd.z = 0;
    float dist = e.cat == CAT_AIR ? 12.0f : e.cat == CAT_BOATS ? 9.0f : InVehicle(me) ? 9.0f : 6.0f;
    Field<Vec3>(v, 0x04) = { fwd.y, -fwd.x, 0.0f };   // droite
    Field<Vec3>(v, 0x14) = fwd;                       // avant
    Field<Vec3>(v, 0x24) = { 0.0f, 0.0f, 1.0f };      // haut
    Pos(v) = { at.x + fwd.x * dist, at.y + fwd.y * dist, at.z + 0.6f };
    AreaCode(v) = AreaCode(me);
    SetEntityStatus(v, STATUS_ABANDONED);
    WorldAdd(v);
    NetVehicleShare(v);   // les autres le voient tout de suite
    Log("menu jeu : %s (modele %d) cree en %.1f %.1f %.1f", e.name, e.id, Pos(v).x, Pos(v).y, Pos(v).z);
}

// ======================================================================= Actions
static void TeleportMe(float x, float y, float z, int area)
{
    void *me = FindPlayerPed();
    if (!me || !PlayerFree()) return;
    if (InVehicle(me) && PedVehicle(me) && VehDriver(PedVehicle(me)) != me) WarpOutOfVehicle(me, NULL);   // passager : on descend
    if (area != AreaCode(me)) MirrorFollowHostArea(area);
    int32_t p[4] = { 0 };
    float xyz[3] = { x, y, z };
    memcpy(p + 1, xyz, 12);
    MirrorLocal(0x0055, 4, p);   // SET_PLAYER_COORDINATES (avec son vehicule s'il conduit)
    CoopWatchWalls();
    Log("menu jeu : teleporte en %.1f %.1f %.1f (zone %d)", x, y, z, area);
}

static void GotoPlayer(int id)
{
    if (id < 0 || id >= MAX_PLAYERS || id == g_localId || !g_players[id].connected || !g_players[id].state.inGame) return;
    const MsgState &s = g_players[id].state;
    float side = s.inVehicle ? 4.0f : 1.5f;
    TeleportMe(s.pos[0] + side * cosf(s.heading), s.pos[1] + side * sinf(s.heading), s.pos[2] + 0.3f, s.area);
    Log("menu jeu : vers %s", s.name);
}

static void Bring(int id)
{
    void *me = FindPlayerPed();
    if (!me || id == g_localId || id < 0 || id >= MAX_PLAYERS) return;
    float h = Heading(me);
    RlBring b = { RL_BRING, (uint8_t)id, AreaCode(me), { Pos(me).x + 1.5f * cosf(h), Pos(me).y + 1.5f * sinf(h), Pos(me).z + 0.3f } };
    if (g_cfg.host) NetSendReliableTo(id, &b, sizeof(b));
    else NetSendReliable(&b, sizeof(b));   // l'hote verifie et fait suivre
    Log("menu jeu : %s amene ici", g_players[id].state.name);
}

static void HostWorld(int cmd, int a)
{
    if (cmd == ADM_TIME) {
        int32_t t[2] = { a % 24, 0 };
        MirrorLocal(0x00C0, 2, t);   // SET_TIME_OF_DAY
        Log("menu jeu : heure %d h", t[0]);
    } else if (cmd == ADM_WEATHER) {
        if (a < 0) MirrorLocal(0x01BD, 0, NULL);   // RELEASE_WEATHER
        else { int32_t w[1] = { a }; MirrorLocal(0x01B6, 1, w); }   // FORCE_WEATHER_NOW
        Log("menu jeu : meteo %d", a);
    }
}

static void AdminRequest(int cmd, int target, int a)
{
    if (g_cfg.host) {
        if (cmd == ADM_KICK) NetKick(target);
        else HostWorld(cmd, a);
        return;
    }
    RlAdminReq r = { RL_ADMIN_REQ, (uint8_t)cmd, (uint8_t)target, a };
    NetSendReliable(&r, sizeof(r));
}

static const char *AdminsKey() { return "AdminsJeu"; }

static bool NameIsAdmin(const char *name)
{
    char list[512];
    GetPrivateProfileStringA("VCCoop", AdminsKey(), "", list, sizeof(list), IniPath());
    for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) if (name[0] && !_stricmp(t, name)) return true;
    return false;
}

static void ToggleAdmin(int id)
{
    if (!g_cfg.host || id <= 0 || id >= MAX_PLAYERS || !g_players[id].connected) return;
    const char *name = g_players[id].state.name;
    char list[512], out[512] = "";
    GetPrivateProfileStringA("VCCoop", AdminsKey(), "", list, sizeof(list), IniPath());
    bool was = false;
    for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) {
        if (!_stricmp(t, name)) { was = true; continue; }
        if (out[0]) lstrcatA(out, ",");
        lstrcatA(out, t);
    }
    if (!was && lstrlenA(out) + lstrlenA(name) + 2 < (int)sizeof(out)) { if (out[0]) lstrcatA(out, ","); lstrcatA(out, name); }
    WritePrivateProfileStringA("VCCoop", AdminsKey(), out, IniPath());
    Log("menu jeu : %s %s admin", name, was ? "n'est plus" : "devient");
}

static void GiveWeaponsPack(void *me)
{
    static const int pack[][2] = { { 10, 1 }, { 12, 10 }, { 18, 100 }, { 20, 80 }, { 25, 400 }, { 26, 400 }, { 28, 40 }, { 30, 10 } };
    for (auto &w : pack) {
        int model = *(int *)(0x782A14 + w[0] * 0x64 + 0x54);   // CWeaponInfo : modele
        if (model > 0) LoadModelNow(model);
        if (model > 0 && !HasModelLoaded(model)) continue;
        GiveWeapon(me, w[0], w[1]);
    }
}

static void RunAction(const Action &ac)
{
    void *me = FindPlayerPed();
    if (!me) return;
    void *veh = InVehicle(me) ? PedVehicle(me) : NULL;
    bool admin = LocalAdmin();
    switch (ac.kind) {
    case A_GOTO: GotoPlayer(ac.a); break;
    case A_BRING: if (admin) Bring(ac.a); break;
    case A_KICK: if (admin && ac.a > 0) AdminRequest(ADM_KICK, ac.a, 0); break;
    case A_TOGGLE_ADMIN: ToggleAdmin(ac.a); break;
    case A_SPAWN: if (admin) SpawnVehicle(ac.a); break;
    case A_HEAL:
        if (!admin) break;
        if (Health(me) < 100.0f) Health(me) = 100.0f;
        if (Armour(me) < 100.0f) Armour(me) = 100.0f;
        break;
    case A_WEAPONS: if (admin) GiveWeaponsPack(me); break;
    case A_MONEY: if (admin) Money() += 10000; break;
    case A_REPAIR:
        if (!admin || !veh) break;
        VehHealth(veh) = 1000.0f;
        if (VehClass(veh) == VCLASS_CAR || VehClass(veh) == VCLASS_HELI || VehClass(veh) == VCLASS_PLANE)
            ((void(__thiscall *)(void *))0x588530)(veh);   // CAutomobile::Fix
        break;
    case A_FLIP: {
        if (!admin || !veh) break;
        Vec3 f = Field<Vec3>(veh, 0x14);
        float l = sqrtf(f.x * f.x + f.y * f.y);
        if (l < 0.01f) { f = Field<Vec3>(veh, 0x24); l = sqrtf(f.x * f.x + f.y * f.y); if (l < 0.01f) { f = { 0, 1, 0 }; l = 1; } }
        f.x /= l; f.y /= l; f.z = 0;
        Field<Vec3>(veh, 0x04) = { f.y, -f.x, 0.0f };
        Field<Vec3>(veh, 0x14) = f;
        Field<Vec3>(veh, 0x24) = { 0.0f, 0.0f, 1.0f };
        Pos(veh).z += 1.0f;
        MoveSpeed(veh) = { 0, 0, 0 };
        TurnSpeed(veh) = { 0, 0, 0 };
        break;
    }
    case A_WANTED: if (admin) { int32_t p[1] = { 0 }; MirrorLocal(0x0110, 1, p); } break;   // CLEAR_WANTED_LEVEL
    case A_NOPOLICE:
        if (!admin) break;
        g_noPolice = !g_noPolice;
        { int32_t p[1] = { g_noPolice ? 0 : 6 }; MirrorLocal(0x01F0, 1, p); }   // SET_MAX_WANTED_LEVEL
        if (g_noPolice) { int32_t p[1] = { 0 }; MirrorLocal(0x0110, 1, p); }
        break;
    case A_TIME: if (admin) AdminRequest(ADM_TIME, 0, ac.a); break;
    case A_WEATHER: if (admin) AdminRequest(ADM_WEATHER, 0, ac.a); break;
    case A_OPTION:
        if (!g_cfg.host) break;
        switch (ac.a) {
        case 0: g_cfg.friendlyFire = !g_cfg.friendlyFire; break;
        case 1: g_cfg.shareMoney = !g_cfg.shareMoney; break;
        case 2: g_cfg.keepWeapons = !g_cfg.keepWeapons; break;
        case 3: g_cfg.showNames = !g_cfg.showNames; break;
        case 4: g_cfg.shareWanted = !g_cfg.shareWanted; WritePrivateProfileStringA("VCCoop", "RecherchePartagee", g_cfg.shareWanted ? "1" : "0", IniPath()); break;
        case 5: g_cfg.respawnAtHost = !g_cfg.respawnAtHost; WritePrivateProfileStringA("VCCoop", "ReapparitionHote", g_cfg.respawnAtHost ? "1" : "0", IniPath()); break;
        case 6: {
            static const int steps[] = { 50, 100, 150, 200, 250, 300 };
            int i = 0;
            while (i < 6 && steps[i] <= g_cfg.popDensity) i++;
            g_cfg.popDensity = steps[i % 6];
            break;
        }
        }
        MenuSaveIni();
        break;
    }
}

// ======================================================================= Tchat
struct ChatLine { wchar_t text[140]; uint32_t color, at; };
static ChatLine g_lines[8];
static int g_lineCount;
static uint32_t g_lastChatAt;

static void AddChatLine(int player, const char *text)
{
    if (g_lineCount == 8) { memmove(g_lines, g_lines + 1, sizeof(ChatLine) * 7); g_lineCount = 7; }
    ChatLine &l = g_lines[g_lineCount++];
    const char *name = player == g_localId ? g_cfg.playerName : (player >= 0 && player < MAX_PLAYERS ? g_players[player].state.name : "?");
    char buf[140];
    _snprintf(buf, sizeof(buf), "%s : %s", name, text);
    buf[sizeof(buf) - 1] = 0;
    MultiByteToWideChar(1252, 0, buf, -1, l.text, 140);
    l.color = player >= 0 && player < MAX_PLAYERS ? PlayerColor(player) : 0xFFFFFFFF;
    l.at = g_lastChatAt = GetTickCount();
    Log("tchat : %s", buf);
}

static void SendChat(const char *text)
{
    if (!text[0]) return;
    RlChat c = { RL_CHAT, (uint8_t)(g_localId < 0 ? 0 : g_localId) };
    lstrcpynA(c.text, text, sizeof(c.text));
    AddChatLine(g_localId < 0 ? 0 : g_localId, c.text);
    if (g_localId >= 0) NetSendReliable(&c, (int)(offsetof(RlChat, text) + strlen(c.text) + 1));
}

// La police du jeu n'a pas d'accents : lettres accentuees (page de code 1252) ramenees a leur lettre simple.
static char PlainChar(unsigned char c)
{
    if (c >= 32 && c < 127) return (char)c;
    static const char *from = "\xC0\xC1\xC2\xC3\xC4\xC7\xC8\xC9\xCA\xCB\xCC\xCD\xCE\xCF\xD2\xD3\xD4\xD5\xD6\xD9\xDA\xDB\xDC"
                              "\xE0\xE1\xE2\xE3\xE4\xE7\xE8\xE9\xEA\xEB\xEC\xED\xEE\xEF\xF2\xF3\xF4\xF5\xF6\xF9\xFA\xFB\xFC";
    static const char *to = "AAAAACEEEEIIIIOOOOOUUUUaaaaaceeeeiiiiooooouuuu";
    for (int i = 0; from[i]; i++) if ((unsigned char)from[i] == c) return to[i];
    return 0;
}

static void EndTyping(bool send)
{
    if (send) SendChat(g_input);
    g_typing = false;
    g_input[0] = 0;
    g_inputLen = 0;
}

// ======================================================================= Entrees
bool PanelWantsMouse() { return g_open; }
bool PanelCapturesKeys() { return g_open || g_typing; }

void PanelMouse(long dx, long dy, long wheel, bool left)
{
    g_accX += dx;
    g_accY += dy;
    g_accWheel += wheel;
    g_left = left;
}

static bool InGameNow() { return GameState() == GS_PLAYING && !MenuActive() && FindPlayerPed() != NULL; }

bool PanelWndProc(UINT msg, WPARAM wp, LPARAM lp)
{
    bool repeat = (lp & 0x40000000) != 0;
    bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    if (!InGameNow()) {
        if (g_typing) EndTyping(false);
        g_open = false;
        return (down || up) && wp == (WPARAM)g_menuKey && g_menuKey == VK_F10;   // F10 : jamais la barre de menus de Windows
    }
    if ((down || up) && wp == (WPARAM)g_menuKey) {
        if (down && !repeat && !g_typing) {
            g_open = !g_open;
            if (g_open && g_mx < 0) { g_mx = ScreenW() * 0.5f; g_my = ScreenH() * 0.5f; }
            Log("menu jeu : %s", g_open ? "ouvert" : "ferme");
        }
        return true;
    }
    if (g_typing) {
        if (msg == WM_CHAR) {
            if (g_skipChar) { g_skipChar = false; if (wp == (WPARAM)g_chatKey || wp == (WPARAM)(g_chatKey | 0x20)) return true; }
            if (wp == 13) EndTyping(true);
            else if (wp == 27) EndTyping(false);
            else if (wp == 8) { if (g_inputLen) g_input[--g_inputLen] = 0; }
            else if (g_inputLen + 1 < (int)sizeof(g_input)) {
                char c = PlainChar((unsigned char)wp);
                if (c) { g_input[g_inputLen++] = c; g_input[g_inputLen] = 0; }
            }
            return true;
        }
        return msg == WM_KEYDOWN;   // les relachements passent : pas de touche collee chez le jeu (Alt+Tab, Alt+F4 aussi)
    }
    if (g_open) {
        if (down && wp == VK_ESCAPE) { g_open = false; Log("menu jeu : ferme (Echap)"); }
        return msg == WM_KEYDOWN || msg == WM_CHAR;
    }
    if (msg == WM_KEYDOWN && !repeat && wp == (WPARAM)g_chatKey && GameHasFocus()) {
        g_typing = true;
        g_skipChar = true;
        g_input[0] = 0;
        g_inputLen = 0;
        return true;
    }
    return false;
}

// ======================================================================= Dessin
// Menu moderne (ui9.cpp : verre, formes arrondies, Segoe UI) quand le pont Direct3D 9 est la ; sinon rectangles et
// police du jeu.
static bool g_modern;
static float g_u;   // unite : hauteur d'ecran / 100
static const uint32_t K_PINK = 0xFF4F8BFF, K_ORANGE = 0xFF8A5BFF;

static void Poly(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t rgba)
{
    uint8_t c[4] = { (uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), (uint8_t)rgba };
    ((void(__cdecl *)(float, float, float, float, float, float, float, float, const uint8_t *))0x578520)(x0, y0, x1, y1, x2, y2, x3, y3, c);
}
// Rectangle plein : CSprite2d::Draw2DPolygon (haut gauche, haut droit, bas gauche, bas droit).
static void Rect(float x0, float y0, float x1, float y1, uint32_t rgba)
{
    if (g_modern) {
        if (rgba == 0x1A1424F0) rgba = 0xFFFFFF0C;   // (lignes : blanc a peine visible sur le verre)
        float r = (y1 - y0) < g_u * 0.6f ? 0 : g_u * 0.8f;
        UiRect(x0, y0, x1, y1, r, rgba, rgba);
        return;
    }
    Poly(x0, y0, x1, y0, x0, y1, x1, y1, rgba);
}
static void Frame(float x0, float y0, float x1, float y1, float t, uint32_t rgba)
{
    if (g_modern) return;
    Rect(x0, y0, x1, y0 + t, rgba); Rect(x0, y1 - t, x1, y1, rgba);
    Rect(x0, y0, x0 + t, y1, rgba); Rect(x1 - t, y0, x1, y1, rgba);
}

static void FontColor(uint32_t rgba, bool drop = true)
{
    uint8_t c[4] = { (uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), (uint8_t)rgba };
    ((void(__cdecl *)(const uint8_t *))0x550170)(c);
    uint8_t d[4] = { 0, 0, 0, (uint8_t)(drop ? 255 : 0) };
    ((void(__cdecl *)(const uint8_t *))0x54FF30)(d);
}

// Hauteur de ligne de la police a cette taille (pixels).
static const float kTextScale = 1.25f;   // toutes les tailles de texte du menu
static float LineH(float size) { return size * kTextScale * g_u / 4.48f * 13.5f; }

enum { AL_LEFT, AL_CENTER };
static void UiLine(float x, float y, float size, uint32_t rgba, int align, const char *s, bool bold)
{
    float px = LineH(size) * 1.12f;   // (Segoe UI plus petite que la police du jeu a hauteur egale)
    y -= LineH(size) * 0.1f;
    int a = align == AL_CENTER ? UI_CENTER : UI_LEFT;
    UiText(x + 1, y + 1.5f, px, 0x00000070, a, s, bold);   // ombre legere
    UiText(x, y, px, rgba, a, s, bold);
}
static void Text(float x, float y, float size, uint32_t rgba, int align, const wchar_t *s)
{
    if (g_modern) {
        char a[200];
        WideCharToMultiByte(1252, 0, s, -1, a, sizeof(a), NULL, NULL);
        UiLine(x, y, size, rgba, align, a, size >= 0.6f);
        return;
    }
    ((void(__cdecl *)())0x5500D0)();                   // SetBackgroundOff
    ((void(__cdecl *)())0x550080)();                   // SetBackGroundOnlyTextOff
    ((void(__cdecl *)())0x550020)();                   // SetPropOn
    ((void(__cdecl *)())0x550040)();                   // ni centre ni a droite
    if (align == AL_CENTER) ((void(__cdecl *)())0x550120)();   // SetCentreOn
    ((void(__cdecl *)(float))0x5500F0)(10000.0f);     // SetCentreSize
    ((void(__cdecl *)(float))0x550100)(10000.0f);     // SetWrapx
    ((void(__cdecl *)(short))0x54FFE0)(1);             // police de l'interface
    ((void(__cdecl *)(short))0x54FF20)(1);             // ombre
    float sy = size * kTextScale * g_u / 4.48f;        // FontSetup(players.cpp) : meme echelle pour size * kTextScale = 1
    ((void(__cdecl *)(float, float))0x550230)(sy * 0.5f, sy);
    FontColor(rgba);
    ((void(__cdecl *)(float, float, const wchar_t *))0x551040)(x, y, s);
}
static void TextA(float x, float y, float size, uint32_t rgba, int align, const char *s)
{
    if (g_modern) { UiLine(x, y, size, rgba, align, s, size >= 0.6f); return; }
    wchar_t w[160];
    MultiByteToWideChar(1252, 0, s, -1, w, 160);
    Text(x, y, size, rgba, align, w);
}

enum : uint32_t {
    C_BG = 0x0D0A14E6, C_PANEL = 0x1A1424F0, C_LINE = 0xFF4FA0FF, C_BTN = 0x2C2238FF, C_BTN_HOT = 0x4A3560FF,
    C_BTN_ON = 0xB0306EFF, C_TEXT = 0xFFFFFFFF, C_DIM = 0xB8B0C8FF, C_OFF = 0x5A5064FF
};

static bool Inside(float x0, float y0, float x1, float y1) { return g_mx >= x0 && g_mx < x1 && g_my >= y0 && g_my < y1; }

// Bouton : vrai au clic (une seule fois : le clic est consomme).
static bool Button(float x0, float y0, float x1, float y1, const char *label, bool on = false, bool enabled = true, float size = 0.55f)
{
    bool hot = enabled && Inside(x0, y0, x1, y1);
    if (g_modern) {
        float r = (y1 - y0) * 0.34f;
        if (!enabled) UiRect(x0, y0, x1, y1, r, 0xFFFFFF08, 0xFFFFFF06);
        else if (on) UiRectH(x0, y0, x1, y1, r, K_PINK, K_ORANGE);
        else UiRect(x0, y0, x1, y1, r, hot ? 0xFFFFFF30 : 0xFFFFFF18, hot ? 0xFFFFFF22 : 0xFFFFFF0E, hot ? 0xFF4F8BD0 : 0xFFFFFF22, 1.2f);
        UiLine((x0 + x1) * 0.5f, (y0 + y1) * 0.5f - LineH(size) * 0.47f, size, enabled ? 0xFFFFFFFF : 0xFFFFFF55, AL_CENTER, label, true);
        if (hot && g_click) { g_click = false; return true; }
        return false;
    }
    Rect(x0, y0, x1, y1, !enabled ? 0x221C2AFF : on ? C_BTN_ON : hot ? C_BTN_HOT : C_BTN);
    if (hot) Frame(x0, y0, x1, y1, 1.0f + g_u * 0.12f, C_LINE);
    TextA((x0 + x1) * 0.5f, (y0 + y1) * 0.5f - LineH(size) * 0.5f, size, enabled ? C_TEXT : C_OFF, AL_CENTER, label);
    if (hot && g_click) { g_click = false; return true; }
    return false;
}

static void DrawCursor()
{
    float x = g_mx, y = g_my, s = g_u * 2.2f;
    if (g_modern) {
        UiTri(x - 1.5f, y - 2.5f, x - 1.5f, y + s + 3, x + s * 0.74f + 3, y + s * 0.72f + 1.5f, 0x000000C0);
        UiTri(x, y, x, y + s, x + s * 0.7f, y + s * 0.7f, 0xFFFFFFFF);
        return;
    }
    Poly(x - 1, y - 2, x - 1, y - 2, x - 1, y + s + 2, x + s * 0.72f + 2, y + s * 0.72f + 1, 0x000000FF);
    Poly(x, y, x, y, x, y + s, x + s * 0.7f, y + s * 0.7f, 0xFFFFFFFF);
}

static const char *Tr(const char *fr, const char *en) { return French() ? fr : en; }

static void DrawPlayersTab(float x0, float y0, float x1, float y1)
{
    bool fr = French();
    float row = g_u * 5.6f, y = y0 + g_u;
    void *me = FindPlayerPed();
    bool admin = LocalAdmin();
    TextA(x0 + g_u, y, 0.5f, C_DIM, AL_LEFT, Tr("Cliquez sur ALLER pour vous teleporter pres d'un joueur.", "Click GO TO to teleport next to a player."));
    y += row * 0.8f;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        bool self = i == g_localId || (g_localId < 0 && i == 0);
        if (!self && !g_players[i].connected) continue;
        const MsgState &s = g_players[i].state;
        Rect(x0 + g_u * 0.5f, y, x1 - g_u * 0.5f, y + row - g_u * 0.6f, C_PANEL);
        Rect(x0 + g_u * 0.5f, y, x0 + g_u * 1.1f, y + row - g_u * 0.6f, PlayerColor(i));
        char line[160];
        const char *role = i == 0 ? Tr("hote", "host") : IsAdmin(i) ? "admin" : "";
        int ping = self ? 0 : (i == 0 ? g_myPing : s.ping);
        float health = self && me ? Health(me) : s.health;
        if (self) _snprintf(line, sizeof(line), "%s%s%s%s  (%s)", g_cfg.playerName, role[0] ? "  [" : "", role, role[0] ? "]" : "", Tr("vous", "you"));
        else if (!s.inGame) _snprintf(line, sizeof(line), "%s%s%s%s  %s", s.name, role[0] ? "  [" : "", role, role[0] ? "]" : "", Tr("(au menu)", "(in menu)"));
        else {
            float dist = 0;
            if (me) { float dx = s.pos[0] - Pos(me).x, dy = s.pos[1] - Pos(me).y, dz = s.pos[2] - Pos(me).z; dist = sqrtf(dx * dx + dy * dy + dz * dz); }
            _snprintf(line, sizeof(line), fr ? "%s%s%s%s   sante %d   ping %d   %d m" : "%s%s%s%s   health %d   ping %d   %d m",
                      s.name, role[0] ? "  [" : "", role, role[0] ? "]" : "", (int)health, ping, (int)dist);
        }
        line[sizeof(line) - 1] = 0;
        float ty = y + (row - g_u * 0.6f) * 0.5f - LineH(0.55f) * 0.5f;
        TextA(x0 + g_u * 2.0f, ty, 0.55f, C_TEXT, AL_LEFT, line);
        if (!self) {
            float bw = g_u * 13.0f, bh = row - g_u * 1.6f, bx = x1 - g_u * 1.2f - bw, by = y + g_u * 0.5f;
            if (g_cfg.host && i != 0) {
                if (Button(bx, by, bx + bw, by + bh, Tr("EXPULSER", "KICK"))) Queue(A_KICK, i);
                bx -= bw + g_u;
                if (Button(bx, by, bx + bw, by + bh, "ADMIN", IsAdmin(i))) Queue(A_TOGGLE_ADMIN, i);
                bx -= bw + g_u;
            } else if (admin && i != 0) {
                if (Button(bx, by, bx + bw, by + bh, Tr("EXPULSER", "KICK"))) Queue(A_KICK, i);
                bx -= bw + g_u;
            }
            if (admin) {
                if (Button(bx, by, bx + bw, by + bh, Tr("AMENER", "BRING"), false, s.inGame)) Queue(A_BRING, i);
                bx -= bw + g_u;
            }
            if (Button(bx, by, bx + bw, by + bh, Tr("ALLER", "GO TO"), false, s.inGame && PlayerFree())) { Queue(A_GOTO, i); g_open = false; }
        }
        y += row;
        if (y > y1 - row) break;
    }
}

static void DrawVehiclesTab(float x0, float y0, float x1, float y1)
{
    LoadVehicleList();
    static const char *catFr[] = { "VOITURES", "MOTOS", "BATEAUX", "AERIENS" }, *catEn[] = { "CARS", "BIKES", "BOATS", "AIRCRAFT" };
    float bw = (x1 - x0 - g_u * 5) / 4, bh = g_u * 4.2f, y = y0 + g_u;
    for (int c = 0; c < CAT_COUNT; c++) {
        float bx = x0 + g_u + c * (bw + g_u);
        if (Button(bx, y, bx + bw, y + bh, French() ? catFr[c] : catEn[c], g_vehCat == c)) { g_vehCat = c; g_scroll = 0; }
    }
    y += bh + g_u * 1.2f;
    bool blocked = MissionUnderway() || !PlayerFree();
    TextA(x0 + g_u, y, 0.5f, blocked ? 0xFF8080FF : C_DIM, AL_LEFT, blocked ? Tr("Pas pendant une mission ni une cinematique.", "Not during a mission or a cutscene.")
                                                                           : Tr("Cliquez : le vehicule apparait devant vous, tout le monde le voit. Molette : defiler.",
                                                                                "Click: the vehicle appears in front of you, everybody sees it. Wheel: scroll."));
    y += LineH(0.5f) + g_u;
    int idx[160], n = 0;
    for (int i = 0; i < g_vehCount; i++) if (g_vehList[i].cat == g_vehCat) idx[n++] = i;
    const int cols = 4;
    float cw = (x1 - x0 - g_u * (cols + 1)) / cols, ch = g_u * 4.2f;
    int rows = (int)((y1 - y - g_u) / (ch + g_u * 0.8f));
    if (rows < 1) rows = 1;
    int maxScroll = (n + cols - 1) / cols - rows;
    if (maxScroll < 0) maxScroll = 0;
    if (g_scroll > maxScroll) g_scroll = maxScroll;
    if (g_scroll < 0) g_scroll = 0;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            int k = (g_scroll + r) * cols + c;
            if (k >= n) break;
            float bx = x0 + g_u + c * (cw + g_u), by = y + r * (ch + g_u * 0.8f);
            if (Button(bx, by, bx + cw, by + ch, g_vehList[idx[k]].name, false, !blocked, 0.5f)) Queue(A_SPAWN, idx[k]);
        }
    if (maxScroll > 0) {
        char s[32];
        _snprintf(s, sizeof(s), "%d / %d", g_scroll + 1, maxScroll + 1);
        TextA(x1 - g_u * 6, y1 - LineH(0.45f) - g_u * 0.3f, 0.45f, C_DIM, AL_CENTER, s);
    }
}

static void DrawToolsTab(float x0, float y0, float x1, float y1)
{
    void *me = FindPlayerPed();
    bool inVeh = me && InVehicle(me);
    float bw = (x1 - x0 - g_u * 4) / 3, bh = g_u * 5.0f, y = y0 + g_u * 1.5f;
    struct B { const char *fr, *en; int act; bool on, enabled; } b[] = {
        { "SANTE + GILET", "HEALTH + ARMOUR", A_HEAL, false, true },
        { "ARMES", "WEAPONS", A_WEAPONS, false, true },
        { "+10 000 $", "+$10,000", A_MONEY, false, true },
        { "REPARER LE VEHICULE", "REPAIR VEHICLE", A_REPAIR, false, inVeh },
        { "REMETTRE SUR ROUES", "FLIP VEHICLE", A_FLIP, false, inVeh },
        { "ETOILES A ZERO", "CLEAR WANTED", A_WANTED, false, true },
        { "POLICE IGNOREE", "NO POLICE", A_NOPOLICE, g_noPolice, true },
    };
    for (int i = 0; i < (int)(sizeof(b) / sizeof(b[0])); i++) {
        float bx = x0 + g_u + (i % 3) * (bw + g_u), by = y + (i / 3) * (bh + g_u);
        if (Button(bx, by, bx + bw, by + bh, French() ? b[i].fr : b[i].en, b[i].on, b[i].enabled)) Queue(b[i].act);
    }
}

static void DrawWorldTab(float x0, float y0, float x1, float y1)
{
    bool fr = French();
    float y = y0 + g_u;
    char s[64];
    _snprintf(s, sizeof(s), fr ? "HEURE : %02d:%02d" : "TIME: %02d:%02d", ClockHours(), ClockMinutes());
    TextA(x0 + g_u, y, 0.6f, C_TEXT, AL_LEFT, s);
    y += LineH(0.6f) + g_u;
    float bw = (x1 - x0 - g_u * 7) / 6, bh = g_u * 4.6f;
    static const int hours[] = { 6, 9, 12, 16, 20, 23 };
    for (int i = 0; i < 6; i++) {
        float bx = x0 + g_u + i * (bw + g_u);
        _snprintf(s, sizeof(s), "%d H", hours[i]);
        if (Button(bx, y, bx + bw, y + bh, s, ClockHours() == hours[i])) Queue(A_TIME, hours[i]);
    }
    y += bh + g_u * 2.5f;
    TextA(x0 + g_u, y, 0.6f, C_TEXT, AL_LEFT, Tr("METEO", "WEATHER"));
    y += LineH(0.6f) + g_u;
    static const char *wFr[] = { "AUTO", "SOLEIL", "NUAGES", "PLUIE", "BROUILLARD", "GRAND SOLEIL", "OURAGAN" };
    static const char *wEn[] = { "AUTO", "SUNNY", "CLOUDY", "RAIN", "FOG", "EXTRA SUNNY", "HURRICANE" };
    float ww = (x1 - x0 - g_u * 5) / 4;
    for (int i = 0; i < 7; i++) {
        float bx = x0 + g_u + (i % 4) * (ww + g_u), by = y + (i / 4) * (bh + g_u);
        bool on = i == 0 ? ForcedWeather() < 0 : ForcedWeather() == i - 1;
        if (Button(bx, by, bx + ww, by + bh, fr ? wFr[i] : wEn[i], on)) Queue(A_WEATHER, i - 1);
    }
    if (!g_cfg.host) TextA(x0 + g_u, y1 - LineH(0.45f) - g_u, 0.45f, C_DIM, AL_LEFT, Tr("Heure et meteo sont celles de l'hote : la demande passe par lui.", "Time and weather are the host's: the request goes through them."));
}

static void DrawHostTab(float x0, float y0, float x1, float y1)
{
    bool fr = French();
    const char *yes = fr ? "OUI" : "YES", *no = fr ? "NON" : "NO";
    float y = y0 + g_u * 1.2f, bh = g_u * 4.4f, lw = (x1 - x0) * 0.62f;
    char dens[16];
    _snprintf(dens, sizeof(dens), "%d %%", g_cfg.popDensity);
    struct O { const char *fr, *en, *value; bool on; } o[] = {
        { "Tir ami (les joueurs se blessent entre eux)", "Friendly fire", g_cfg.friendlyFire ? yes : no, g_cfg.friendlyFire },
        { "Argent des missions partage", "Shared mission money", g_cfg.shareMoney ? yes : no, g_cfg.shareMoney },
        { "Un invite mort garde ses armes", "Guests keep weapons when they die", g_cfg.keepWeapons ? yes : no, g_cfg.keepWeapons },
        { "Pseudos au-dessus des joueurs", "Names above players", g_cfg.showNames ? yes : no, g_cfg.showNames },
        { "Etoiles de police communes", "Shared wanted level", g_cfg.shareWanted ? yes : no, g_cfg.shareWanted },
        { "Reapparition pres de l'hote", "Respawn next to the host", g_cfg.respawnAtHost ? yes : no, g_cfg.respawnAtHost },
        { "Densite de population", "Population density", dens, false },
    };
    for (int i = 0; i < 7; i++) {
        Rect(x0 + g_u * 0.5f, y, x1 - g_u * 0.5f, y + bh, C_PANEL);
        TextA(x0 + g_u * 1.5f, y + bh * 0.5f - LineH(0.55f) * 0.5f, 0.55f, C_TEXT, AL_LEFT, fr ? o[i].fr : o[i].en);
        if (Button(x0 + lw, y + g_u * 0.4f, x1 - g_u * 1.2f, y + bh - g_u * 0.4f, o[i].value, o[i].on)) Queue(A_OPTION, i);
        y += bh + g_u * 0.6f;
    }
}

// Ray tracing (Rendu=12) : reglages appliques tout de suite (vcrt64.exe les recoit a l'image suivante) et enregistres
// dans vccoop.ini (memes cles que l'onglet RAY TRACING du lanceur). Deux colonnes : libelle, [-] valeur.
static int g_testClickRow = -1;   // TestMenuJeu=4 : clic simule sur la valeur de cette ligne
static void DrawRtTab(float x0, float y0, float x1, float y1)
{
    bool fr = French();
    struct Row { const char *fr, *en; bool *b; int *v; int vals[6]; int n; const char *labFr[6], *labEn[6]; const char *suffix; };
    static const char *onOffFr[] = { "NON", "OUI" }, *onOffEn[] = { "OFF", "ON" };
    Row rows[] = {
        { "Ombres trac\xE9" "es", "Ray-traced shadows", &g_cfg.rtShadows, NULL, {}, 0, {}, {}, "" },
        { "Rayons d'ombre par pixel", "Shadow rays per pixel", NULL, &g_cfg.rtRays, { 1, 2, 4, 8, 16 }, 5, {}, {}, "" },
        { "Douceur des ombres", "Shadow softness", NULL, &g_cfg.rtSoft, { 0, 1, 2 }, 3, { "NETTES", "DOUCES", "TR\xC8S DOUCES" }, { "SHARP", "SOFT", "VERY SOFT" }, "" },
        { "Port\xE9" "e des ombres", "Shadow distance", NULL, &g_cfg.rtDist, { 200, 400, 600, 1000, 1500 }, 5, {}, {}, " m" },
        { "R\xE9solution des rayons", "Ray resolution", NULL, &g_cfg.rtScale, { 50, 100 }, 2, { "DEMIE", "PLEINE" }, { "HALF", "FULL" }, "" },
        { "Lissage du bruit", "Noise smoothing", NULL, &g_cfg.rtSmooth, { 0, 1, 2 }, 3, { "FAIBLE", "MOYEN", "FORT" }, { "LOW", "MEDIUM", "HIGH" }, "" },
        { "Occlusion trac\xE9" "e", "Ray-traced occlusion", &g_cfg.rtAO, NULL, {}, 0, {}, {}, "" },
        { "Reflets trac\xE9s", "Ray-traced reflections", &g_cfg.rtRefl, NULL, {}, 0, {}, {}, "" },
        { "Force des reflets", "Reflection strength", NULL, &g_cfg.rtReflK, { 25, 50, 75, 100, 150, 200 }, 6, {}, {}, " %" },
        { "Sols brillants", "Shiny ground", NULL, &g_cfg.rtGloss, { 0, 1, 2 }, 3, { "PLUIE", "L\xC9GERS", "MIROIRS" }, { "RAIN", "LIGHT", "MIRRORS" }, "" },
        { "Lumi\xE8re trac\xE9" "e", "Ray-traced lighting", &g_cfg.rtGI, NULL, {}, 0, {}, {}, "" },
        { "Force de la lumi\xE8re", "Lighting strength", NULL, &g_cfg.rtGIK, { 25, 50, 75, 100, 150, 200 }, 6, {}, {}, " %" },
        { "Force de l'occlusion", "Occlusion strength", NULL, &g_cfg.rtAOK, { 25, 50, 75, 100, 150 }, 5, {}, {}, " %" },
    };
    const int n = (int)(sizeof(rows) / sizeof(rows[0])), perCol = (n + 1) / 2;
    float colW = (x1 - x0 - g_u * 3) / 2, bh = g_u * 4.0f, gap = g_u * 0.5f;
    TextA(x0 + g_u, y0 + g_u * 0.4f, 0.48f, C_DIM, AL_LEFT, Tr("Change tout de suite, gard\xE9 pour les prochaines parties.", "Applies right away, kept for the next games."));
    float top = y0 + g_u * 0.8f + LineH(0.48f);
    bool changed = false;
    for (int i = 0; i < n; i++) {
        Row &r = rows[i];
        float cx0 = x0 + g_u + (i / perCol) * (colW + g_u), cx1 = cx0 + colW;
        float y = top + (i % perCol) * (bh + gap);
        Rect(cx0, y, cx1, y + bh, C_PANEL);
        TextA(cx0 + g_u, y + bh * 0.5f - LineH(0.5f) * 0.5f, 0.5f, C_TEXT, AL_LEFT, fr ? r.fr : r.en);
        float vw = colW * 0.42f, vx1 = cx1 - g_u * 0.5f, vx0 = vx1 - vw, by0 = y + g_u * 0.35f, by1 = y + bh - g_u * 0.35f;
        char val[32];
        if (g_testClickRow == i) { g_testClickRow = -1; g_mx = r.b ? vx1 - g_u : vx1 - g_u * 1.5f; g_my = (by0 + by1) * 0.5f; g_click = true; Log("test menu : clic sur %s", r.fr); }
        if (g_modern) {   // interrupteur (oui / non) ou selecteur "< valeur >" (clic : moitie gauche = precedent)
            if (r.b) {
                float th = (by1 - by0) * 0.78f, tw = th * 1.9f, tx1 = vx1 - g_u * 0.3f, tx0 = tx1 - tw, ty0 = (by0 + by1 - th) * 0.5f, ty1 = ty0 + th;
                if (*r.b) UiRectH(tx0, ty0, tx1, ty1, th * 0.5f, K_PINK, K_ORANGE);
                else UiRect(tx0, ty0, tx1, ty1, th * 0.5f, 0xFFFFFF26, 0xFFFFFF1C);
                float kr = th * 0.5f - 2.5f, kx = *r.b ? tx1 - th * 0.5f : tx0 + th * 0.5f;
                UiRect(kx - kr, ty0 + 2.5f, kx + kr, ty1 - 2.5f, kr, 0xFFFFFFFF, 0xF2EEF6FF);
                if (g_click && Inside(cx0, y, cx1, y + bh)) { g_click = false; *r.b = !*r.b; changed = true; }
                continue;
            }
            int k = 0;
            for (int j = 0; j < r.n; j++) if (r.vals[j] <= *r.v) k = j;
            if (r.labFr[0]) _snprintf(val, sizeof(val), "%s", fr ? r.labFr[k] : r.labEn[k]);
            else _snprintf(val, sizeof(val), "%d%s", r.vals[k], r.suffix);
            bool hot = Inside(vx0, by0, vx1, by1);
            UiRect(vx0, by0, vx1, by1, (by1 - by0) * 0.5f, 0xFFFFFF14, 0xFFFFFF0A, hot ? 0xFF4F8BD0 : 0xFFFFFF26, 1.2f);
            float ty = (by0 + by1) * 0.5f - LineH(0.5f) * 0.5f;
            UiLine(vx0 + g_u * 1.0f, ty - g_u * 0.55f, 0.8f, K_PINK, AL_LEFT, "\x8B", true);
            UiLine(vx1 - g_u * 2.0f, ty - g_u * 0.55f, 0.8f, K_PINK, AL_LEFT, "\x9B", true);
            UiLine((vx0 + vx1) * 0.5f, ty, 0.5f, 0xFFFFFFFF, AL_CENTER, val, true);
            if (hot && g_click) {
                g_click = false;
                k = g_mx < (vx0 + vx1) * 0.5f ? (k + r.n - 1) % r.n : (k + 1) % r.n;
                *r.v = r.vals[k];
                changed = true;
            }
            continue;
        }
        if (r.b) {
            if (Button(vx0, by0, vx1, by1, fr ? onOffFr[*r.b] : onOffEn[*r.b], *r.b)) { *r.b = !*r.b; changed = true; }
            continue;
        }
        int k = 0;
        for (int j = 0; j < r.n; j++) if (r.vals[j] <= *r.v) k = j;
        if (r.labFr[0]) _snprintf(val, sizeof(val), "%s", fr ? r.labFr[k] : r.labEn[k]);
        else _snprintf(val, sizeof(val), "%d%s", r.vals[k], r.suffix);
        float aw = bh * 0.9f;
        // (pas de "<" dans la police du jeu : "-")
        if (Button(vx0 - aw - g_u * 0.4f, by0, vx0 - g_u * 0.4f, by1, "-", false, k > 0)) { *r.v = r.vals[k - 1]; changed = true; }
        if (Button(vx0, by0, vx1, by1, val, false)) { *r.v = r.vals[(k + 1) % r.n]; changed = true; }
    }
    if (changed) {
        MenuSaveIni();
        Log("ray tracing : reglages ombres %d rayons %d douceur %d portee %d resolution %d lissage %d occlusion %d/%d reflets %d/%d sol %d lumiere %d/%d",
            g_cfg.rtShadows, g_cfg.rtRays, g_cfg.rtSoft, g_cfg.rtDist, g_cfg.rtScale, g_cfg.rtSmooth, g_cfg.rtAO, g_cfg.rtAOK,
            g_cfg.rtRefl, g_cfg.rtReflK, g_cfg.rtGloss, g_cfg.rtGI, g_cfg.rtGIK);
    }
}

static void DrawChat()
{
    uint32_t now = GetTickCount();
    bool show = g_typing || g_open || (g_lineCount && now - g_lastChatAt < 15000);
    if (!show) return;
    float x = g_u * 2.0f, lh = LineH(0.5f) * 1.05f;
    float y = ScreenH() * 0.30f;
    int first = g_lineCount > 6 ? g_lineCount - 6 : 0;
    for (int i = first; i < g_lineCount; i++) {
        uint32_t age = now - g_lines[i].at;
        if (!g_typing && !g_open && age > 15000) continue;
        Text(x, y, 0.5f, g_lines[i].color, AL_LEFT, g_lines[i].text);
        y += lh;
    }
    if (g_typing) {
        y += lh * 0.3f;
        float w = ScreenW() * 0.36f;
        Rect(x - g_u * 0.6f, y - g_u * 0.3f, x + w, y + LineH(0.5f) + g_u * 0.3f, 0x000000A0);
        char buf[140];
        _snprintf(buf, sizeof(buf), "%s : %s", Tr("Dire", "Say"), g_input);
        buf[sizeof(buf) - 1] = 0;
        TextA(x, y, 0.5f, C_TEXT, AL_LEFT, buf);
    }
}

void PanelDraw()
{
    g_u = ScreenH() / 100.0f;
    if (!InGameNow()) { g_click = false; return; }
    g_modern = UiReady();
    if (g_modern) UiBegin();
    DrawChat();
    if (g_open) {
        float W = (float)ScreenW(), H = (float)ScreenH();
        g_mx += g_accX * H / 900.0f;
        g_my += g_accY * H / 900.0f;
        g_accX = g_accY = 0;
        if (g_mx < 0) g_mx = 0; if (g_mx > W - 1) g_mx = W - 1;
        if (g_my < 0) g_my = 0; if (g_my > H - 1) g_my = H - 1;
        g_click = g_left && !g_prevLeft;
        g_prevLeft = g_left;
        if (g_accWheel) { g_scroll += g_accWheel > 0 ? -1 : 1; g_accWheel = 0; }

        float pw = W * 0.62f;
        if (pw > H * 1.25f) pw = H * 1.25f;
        float ph = H * 0.68f, x0 = (W - pw) * 0.5f, y0 = H * 0.14f, x1 = x0 + pw, y1 = y0 + ph;
        float th = g_u * 5.5f;
        if (g_modern) {   // verre depoli arrondi, ombre douce, liseré clair ; filet rose -> orange sous le titre
            UiShadow(x0, y0 + g_u * 0.6f, x1, y1 + g_u * 0.6f, g_u * 2.4f, g_u * 3.5f, 0x00000080);
            UiGlass(x0, y0, x1, y1, g_u * 2.4f, 0x160A2AC8, 0xFFFFFF38, 1.3f);
            UiRectH(x0 + g_u * 2, y0 + th - 1.0f, x1 - g_u * 2, y0 + th + 1.0f, 1.0f, K_PINK, K_ORANGE);
        } else {
            Rect(x0, y0, x1, y1, C_BG);
            Frame(x0, y0, x1, y1, g_u * 0.2f, C_LINE);
            Rect(x0, y0, x1, y0 + th, 0x2A1030FF);
        }
        TextA(x0 + g_u * 2, y0 + th * 0.5f - LineH(0.75f) * 0.5f, 0.75f, g_modern ? K_PINK : C_LINE, AL_LEFT, "VCCOOP");
        char who[64];
        _snprintf(who, sizeof(who), "%s%s", g_cfg.playerName, g_cfg.host ? Tr("  -  hote", "  -  host") : LocalAdmin() ? "  -  admin" : "");
        TextA(x0 + g_u * 20, y0 + th * 0.5f - LineH(0.5f) * 0.5f, 0.5f, C_DIM, AL_LEFT, who);
        if (g_modern ? Button(x1 - th + g_u * 0.8f, y0 + g_u * 0.8f, x1 - g_u * 0.8f, y0 + th - g_u * 0.8f, "X") : Button(x1 - th, y0, x1, y0 + th, "X")) g_open = false;

        // Onglets : selon le role.
        bool admin = LocalAdmin();
        struct T { const char *fr, *en; int id; bool show; } tabs[] = {
            { "JOUEURS", "PLAYERS", 0, true }, { "V\xC9HICULES", "VEHICLES", 1, admin }, { "OUTILS", "TOOLS", 2, admin },
            { "MONDE", "WORLD", 3, admin }, { "H\xD4TE", "HOST", 4, g_cfg.host }, { "RAY TRACING", "RAY TRACING", 5, g_cfg.renderer == 12 } };
        int shown = 0;
        for (auto &t : tabs) shown += t.show;
        bool tabOk = false;
        for (auto &t : tabs) tabOk |= t.show && t.id == g_tab;
        if (!tabOk) g_tab = 0;
        float tw = (pw - g_u * (shown + 1)) / shown, ty = y0 + th + g_u, tbh = g_u * 4.6f;
        int k = 0;
        for (auto &t : tabs) {
            if (!t.show) continue;
            float bx = x0 + g_u + k * (tw + g_u);
            if (Button(bx, ty, bx + tw, ty + tbh, French() ? t.fr : t.en, g_tab == t.id, true, shown > 5 ? 0.5f : 0.6f)) { g_tab = t.id; g_scroll = 0; }
            k++;
        }
        float cy0 = ty + tbh + g_u, cy1 = y1 - g_u * 3.6f;
        if (!g_modern) Rect(x0 + g_u * 0.5f, cy0 - g_u * 0.3f, x1 - g_u * 0.5f, cy0 - g_u * 0.15f, C_LINE);
        switch (g_tab) {
        case 0: DrawPlayersTab(x0, cy0, x1, cy1); break;
        case 1: DrawVehiclesTab(x0, cy0, x1, cy1); break;
        case 2: DrawToolsTab(x0, cy0, x1, cy1); break;
        case 3: DrawWorldTab(x0, cy0, x1, cy1); break;
        case 4: DrawHostTab(x0, cy0, x1, cy1); break;
        case 5: DrawRtTab(x0, cy0, x1, cy1); break;
        }
        char hint[128];
        _snprintf(hint, sizeof(hint), French() ? "Echap ou F10 : fermer     %c : tchat" : "Esc or F10: close     %c: chat", g_chatKey);
        TextA((x0 + x1) * 0.5f, y1 - g_u * 2.8f, 0.45f, C_DIM, AL_CENTER, hint);
        g_click = false;
    }
    ((void(__cdecl *)())0x550250)();   // CFont::DrawFonts : nos textes, par-dessus nos rectangles
    if (g_open) DrawCursor();
    if (g_modern) UiEnd();
}

// ======================================================================= Reseau
void PanelOnReliable(int from, const uint8_t *data, int len)
{
    switch (data[0]) {
    case RL_CHAT: {
        if (len < 3) return;
        RlChat c = {};
        memcpy(&c, data, len < (int)sizeof(c) ? len : (int)sizeof(c));
        c.text[sizeof(c.text) - 1] = 0;
        if (g_cfg.host) {   // on fait suivre aux autres invites
            c.player = (uint8_t)from;
            for (int i = 1; i < MAX_PLAYERS; i++) if (i != from && g_players[i].connected) NetSendReliableTo(i, &c, (int)(offsetof(RlChat, text) + strlen(c.text) + 1));
        }
        if (c.player != g_localId) AddChatLine(c.player, c.text);
        return;
    }
    case RL_ADMINS:
        if (!g_cfg.host && len >= (int)sizeof(RlAdmins)) {
            uint8_t mask = ((const RlAdmins *)data)->mask;
            if (g_localId > 0 && (mask ^ g_adminMask) & (1 << g_localId))
                Log("menu jeu : %s", mask & (1 << g_localId) ? "je deviens admin" : "je ne suis plus admin");
            g_adminMask = mask;
        }
        return;
    case RL_ADMIN_REQ: {
        if (!g_cfg.host || len < (int)sizeof(RlAdminReq)) return;
        const RlAdminReq &r = *(const RlAdminReq *)data;
        if (!IsAdmin(from)) { Log("menu jeu : demande d'admin de %d refusee (pas admin)", from); return; }
        if (r.cmd == ADM_KICK && r.target > 0) NetKick(r.target);
        else HostWorld(r.cmd, r.a);
        return;
    }
    case RL_BRING: {
        if (len < (int)sizeof(RlBring)) return;
        const RlBring &b = *(const RlBring *)data;
        if (g_cfg.host && from > 0) {
            if (!IsAdmin(from)) return;
            if (b.target != 0) { NetSendReliableTo(b.target, &b, sizeof(b)); return; }
        }
        if (b.target == g_localId || (g_cfg.host && b.target == 0)) {
            TeleportMe(b.pos[0], b.pos[1], b.pos[2], b.area);
            Log("menu jeu : amene par un admin");
        }
        return;
    }
    }
}

// Hote : qui est admin (pseudos de AdminsJeu), envoye a tous quand ca change ou quand quelqu'un arrive.
static void HostAdmins()
{
    static uint32_t last;
    static uint8_t sentMask = 0xFF, sentConnected;
    if (GetTickCount() - last < 1000) return;
    last = GetTickCount();
    uint8_t mask = 1, connected = 0;
    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (!g_players[i].connected) continue;
        connected |= 1 << i;
        if (NameIsAdmin(g_players[i].state.name)) mask |= 1 << i;
    }
    g_adminMask = mask;
    if (mask == sentMask && connected == sentConnected) return;
    sentMask = mask;
    sentConnected = connected;
    RlAdmins a = { RL_ADMINS, mask };
    NetSendReliable(&a, sizeof(a));
}

// ======================================================================= Image
static void TestPanel(bool inGame)
{
    static int mode = -1;
    if (mode < 0) mode = GetPrivateProfileIntA("VCCoop", "TestMenuJeu", 0, IniPath());
    if (mode <= 0 || !inGame) return;
    static uint32_t start, step;
    uint32_t now = GetTickCount();
    if (!PlayerFree()) { if (!step) start = 0; return; }
    if (!start) start = now;
    uint32_t t = now - start;
    if (mode == 2) {   // TestMenuJeu=2 : tchat seul, 10 s de saisie toutes les 25 s (telephone vu par les autres)
        uint32_t c = t % 25000;
        if (c > 5000 && c < 15000 && !g_typing) { g_typing = true; lstrcpyA(g_input, "J'arrive, attends-moi"); g_inputLen = lstrlenA(g_input); Log("test menu : tchat"); }
        else if (c >= 15000 && g_typing) EndTyping(true);
        return;
    }
    if (mode == 4) {   // TestMenuJeu=4 : onglet RAY TRACING, clics simules sur quelques valeurs
        if (step == 0 && t > 3000) { step = 1; g_open = true; g_tab = 5; Log("test menu : ray tracing (clics)"); }
        else if (step >= 1 && step <= 6 && t > 3000 + step * 2500) { static const int rows[6] = { 2, 2, 9, 0, 4, 11 }; g_testClickRow = rows[step - 1]; step++; }
        return;
    }
    if (mode == 3) {   // TestMenuJeu=3 : onglet RAY TRACING ouvert, sols en miroirs au bout de 10 s (reglage en direct)
        if (step == 0 && t > 3000) { step = 1; g_open = true; g_mx = ScreenW() * 0.9f; g_my = ScreenH() * 0.95f; g_tab = 5; Log("test menu : ray tracing"); }
        else if (step == 1 && t > 10000) { step = 2; g_cfg.rtGloss = 2; g_cfg.rtReflK = 150; MenuSaveIni(); Log("test menu : sols en miroirs"); }
        else if (step == 2 && t > 16000) { step = 3; g_open = false; }
        return;
    }
    // Deroulement : 6 s tchat (telephone), puis menu : onglets toutes les 4 s, un vehicule, puis ferme.
    if (step == 0 && t > 6000) { step = 1; g_typing = true; lstrcpyA(g_input, "Salut ! on se retrouve a la plage"); g_inputLen = lstrlenA(g_input); Log("test menu : tchat"); }
    else if (step == 1 && t > 12000) { step = 2; EndTyping(true); }
    else if (step == 2 && t > 14000) { step = 3; g_open = true; g_mx = ScreenW() * 0.47f; g_my = ScreenH() * 0.40f; g_tab = 0; Log("test menu : ouvert"); }
    else if (step == 3 && t > 18000) { step = 4; g_tab = LocalAdmin() ? 1 : 0; }
    else if (step == 4 && t > 20000) { step = 5; for (int i = 0; i < g_vehCount; i++) if (!_stricmp(g_vehList[i].name, "Infernus")) Queue(A_SPAWN, i); }
    else if (step == 5 && t > 22000) { step = 6; g_tab = 2; }
    else if (step == 6 && t > 26000) { step = 7; g_tab = 3; }
    else if (step == 7 && t > 30000) { step = 8; g_tab = g_cfg.host ? 4 : 0; }
    else if (step == 8 && t > 34000) { step = 9; g_open = false; Log("test menu : ferme"); }
}

void PanelFrame(bool inGame)
{
    TestPanel(inGame);
    if (g_cfg.host && g_localId >= 0) HostAdmins();
    if (!inGame) { g_open = false; if (g_typing) EndTyping(false); g_actionCount = 0; g_phoneOn = false; return; }
    for (int i = 0; i < g_actionCount; i++) RunAction(g_actions[i]);
    g_actionCount = 0;
    // Telephone pendant la saisie : a pied, au repos, main libre.
    void *me = FindPlayerPed();
    if (g_typing && !g_phoneOn && me && !InVehicle(me) && PedState(me) == 1 && PlayerFree() && Health(me) > 0.0f) {
        ((void(__thiscall *)(void *))0x4F59C0)(me);   // CPed::SetAnswerMobile
        g_phoneOn = true;
        g_phoneState = PedState(me);
        Log("tchat : telephone sorti (etat %d)", g_phoneState);
    } else if (!g_typing && g_phoneOn) {
        g_phoneOn = false;
        if (me && PedState(me) == g_phoneState) ((void(__thiscall *)(void *))0x4F58C0)(me);   // CPed::ClearAnswerMobile
    }
}

// ======================================================================= Installation
static int ParseKey(const char *k, int def)
{
    if ((k[0] == 'F' || k[0] == 'f') && k[1] >= '1' && k[1] <= '9') return VK_F1 + atoi(k + 1) - 1;
    if (k[0] && !k[1]) return (k[0] >= 'a' && k[0] <= 'z') ? k[0] - 32 : k[0];
    return def;
}

// Rejeu de la partie (F1 : revoir les dernieres secondes, F2 / F3 : l'enregistrer / le relire) : CReplay::Update
// (0x624EC0), appele par CGame::Process en 0x4A45C3. En coop le rejeu fige le monde local et casse la synchro : on
// ne l'appelle plus du tout (plus d'enregistrement non plus).
static void __cdecl NoReplay() {}

void InstallPanel()
{
    char k[16];
    GetPrivateProfileStringA("VCCoop", "ToucheMenuJeu", "F10", k, sizeof(k), IniPath());
    g_menuKey = ParseKey(k, VK_F10);
    GetPrivateProfileStringA("VCCoop", "ToucheTchat", "T", k, sizeof(k), IniPath());
    g_chatKey = ParseKey(k, 'T');
    static const uint8_t call[] = { 0xE8, 0xF8, 0x08, 0x18, 0x00 };   // call 0x624EC0
    if (memcmp((void *)0x4A45C3, call, sizeof(call)) == 0) { PatchCall(0x4A45C3, (void *)NoReplay); Log("rejeu (F1-F3) desactive"); }
    else Log("rejeu : appel introuvable, laisse");
}
