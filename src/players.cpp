// Reperes des joueurs : un point de couleur par joueur sur le radar et la carte, le pseudo au-dessus de la tete
// (AfficherPseudos), et le choix de tenue (touche F7 : n'importe quel passant, apercu en 3D devant soi).
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "entities.h"
#include "mirror.h"
#include "players.h"
#include "camera.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace game;

// Couleur de chaque joueur (0 = hote) : RGBA, rouge dans l'octet de poids fort (format des marqueurs radar).
static const uint32_t kColors[MAX_PLAYERS] = { 0x3399FFFF, 0xFF9933FF, 0x33DD66FF, 0xB266FFFF };
uint32_t PlayerColor(int id) { return kColors[id & 3]; }

// ======================================================================= Radar
// CRadar::SetEntityBlip (type 2 = personnage, reference de pool, couleur ignoree, affichage 2 = radar seulement)
static int SetEntityBlip(int type, uint32_t handle, int display) { return ((int(__cdecl *)(int, uint32_t, uint32_t, int))0x4C3B40)(type, handle, 0, display); }
static void ChangeBlipColour(int blip, uint32_t rgba) { ((void(__cdecl *)(int, uint32_t))0x4C3930)(blip, rgba); }
static void ChangeBlipScale(int blip, int scale) { ((void(__cdecl *)(int, int))0x4C3840)(blip, scale); }
static void ClearBlip(int blip) { ((void(__cdecl *)(int))0x4C3990)(blip); }

int AddPlayerBlip(void *ped, int player)
{
    int blip = SetEntityBlip(2, PedHandle(ped), 2);
    if (blip == -1) return -1;
    ChangeBlipColour(blip, PlayerColor(player));
    ChangeBlipScale(blip, 3);
    return blip;
}

void RemovePlayerBlip(int &blip)
{
    if (blip != -1) ClearBlip(blip);
    blip = -1;
}

// ======================================================================= Texte a l'ecran
static void FontScale(float w, float h) { ((void(__cdecl *)(float, float))0x550230)(w, h); }
// SetColor / SetDropColor prennent l'ADRESSE d'un CRGBA (octets r, g, b, a) dans le 1.0.
static void FontColor(uint32_t rgba)
{
    uint8_t c[4] = { (uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), (uint8_t)rgba };
    ((void(__cdecl *)(const uint8_t *))0x550170)(c);
}
static void FontDropColor(uint32_t rgba)
{
    uint8_t c[4] = { (uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), (uint8_t)rgba };
    ((void(__cdecl *)(const uint8_t *))0x54FF30)(c);
}
static void FontPrint(float x, float y, const wchar_t *s) { ((void(__cdecl *)(float, float, const wchar_t *))0x551040)(x, y, s); }
static void FontSetup(float scale)
{
    ((void(__cdecl *)())0x5500D0)();                   // SetBackgroundOff
    ((void(__cdecl *)())0x550080)();                   // SetBackGroundOnlyTextOff
    ((void(__cdecl *)())0x550020)();                   // SetPropOn
    ((void(__cdecl *)())0x550040)();                   // SetRightJustifyOff
    ((void(__cdecl *)())0x550120)();                   // SetCentreOn
    ((void(__cdecl *)(float))0x5500F0)(10000.0f);     // SetCentreSize
    ((void(__cdecl *)(float))0x550100)(10000.0f);     // SetWrapx
    ((void(__cdecl *)(short))0x54FFE0)(1);             // SetFontStyle : police de l'interface
    ((void(__cdecl *)(short))0x54FF20)(1);             // SetDropShadowPosition
    FontDropColor(0x000000FF);
    float h = (float)*(int *)0x9B48E0;                 // hauteur de l'ecran (RsGlobal.maximumHeight)
    FontScale(scale * h / 448.0f * 0.5f, scale * h / 448.0f);
}

static int ScreenW() { return *(int *)0x9B48DC; }
static int ScreenH() { return *(int *)0x9B48E0; }

// CSprite::CalcScreenCoors : point du monde -> ecran (pixels) ; faux s'il est derriere la camera.
static bool ToScreen(const Vec3 &p, float &sx, float &sy, float &dist)
{
    float in[3] = { p.x, p.y, p.z }, out[3], w, h;
    if (!((bool(__cdecl *)(const float *, float *, float *, float *, bool))0x5778B0)(in, out, &w, &h, false)) return false;
    sx = out[0]; sy = out[1]; dist = out[2];
    return true;
}

// ======================================================================= Pseudos
static void DrawNametags()
{
    if (!g_cfg.showNames) return;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        void *ped = PuppetPed(i);
        if (!ped || !(Field<uint8_t>(ped, 0x52) & 0x04)) continue;   // absent ou cache
        const NetPlayer &np = g_players[i];
        if (!np.connected || !np.state.name[0]) continue;
        Vec3 head = InVehicle(ped) && PedVehicle(ped) ? Pos(ped) : Pos(ped);
        head.z += 1.15f;
        float sx, sy, d;
        if (!ToScreen(head, sx, sy, d) || d > 60.0f || d < 0.5f) continue;
        uint32_t c = PlayerColor(i);
        float a = d < 40.0f ? 1.0f : (60.0f - d) / 20.0f;   // s'efface au loin
        c = (c & 0xFFFFFF00) | (uint32_t)(255 * a);
        wchar_t name[32];
        MultiByteToWideChar(CP_ACP, 0, np.state.name, -1, name, 32);
        FontSetup(d < 10.0f ? 0.7f : 0.7f * 10.0f / d + 0.35f);
        FontColor(c);
        FontDropColor(0x00000000 | (uint32_t)(200 * a));
        FontPrint(sx, sy, name);
    }
}

// ======================================================================= Choix de la tenue
// Tenues possibles : celles de Tommy (player, play1..), les personnages de l'histoire (ig*) et tous les passants
// (modeles 1 a 108), a condition que leur modele existe dans models\gta3.dir (sinon le chargement planterait).
static char g_skins[160][24];
static int g_skinCount;

static void BuildSkinList()
{
    char path[MAX_PATH];
    wsprintfA(path, "%smodels\\gta3.dir", GameDir());
    FILE *f = fopen(path, "rb");
    if (!f) { Log("tenues : %s introuvable", path); return; }
    // Le modele special est charge avec la texture du meme nom (CStreaming::RequestSpecialModel) : il faut les deux.
    static char dff[4096][24], txd[4096][24];
    int n = 0, nt = 0;
    struct { uint32_t off, size; char name[24]; } e;
    while (fread(&e, sizeof(e), 1, f) == 1) {
        e.name[23] = 0;
        char *dot = strrchr(e.name, '.');
        if (!dot) continue;
        bool isDff = _stricmp(dot, ".dff") == 0, isTxd = _stricmp(dot, ".txd") == 0;
        *dot = 0;
        if (isDff && n < 4096) lstrcpynA(dff[n++], e.name, 24);
        else if (isTxd && nt < 4096) lstrcpynA(txd[nt++], e.name, 24);
    }
    fclose(f);
    auto has = [&](const char *name) {
        bool d = false, t = false;
        for (int i = 0; i < n && !d; i++) d = _stricmp(dff[i], name) == 0;
        for (int i = 0; i < nt && !t; i++) t = _stricmp(txd[i], name) == 0;
        return d && t;
    };
    char (*dir)[24] = dff;
    auto add = [&](const char *name) {
        if (g_skinCount >= 160 || !name[0] || !has(name)) return;
        for (int i = 0; i < g_skinCount; i++) if (_stricmp(g_skins[i], name) == 0) return;
        lstrcpynA(g_skins[g_skinCount], name, 24);
        CharLowerA(g_skins[g_skinCount]);
        g_skinCount++;
    };
    add("player");
    for (int i = 1; i <= 12; i++) { char s[16]; wsprintfA(s, "play%d", i); add(s); }
    for (int i = 0; i < n; i++) if (_strnicmp(dir[i], "ig", 2) == 0) add(dir[i]);
    for (int m = 1; m <= 108; m++) add(ModelName(m));
    Log("tenues : %d au choix", g_skinCount);
}

typedef void(__fastcall *Undress_t)(void *ped, void *edx, const char *name);
static Undress_t o_Undress;
static void Undress(void *ped, const char *name) { ((void(__thiscall *)(void *, const char *))0x4EF030)(ped, name); }
static void Dress(void *ped) { ((void(__thiscall *)(void *))0x4EEFD0)(ped); }
static void LoadAllRequestedModels() { ((void(__cdecl *)(bool))0x40B5F0)(false); }

// Un passant ordinaire (modeles 1 a 108) a deja son propre modele : le charger comme tenue speciale (a la place du
// modele 0, comme les vetements de Tommy) plante. On donne alors directement ce modele au personnage.
int RegularPedModel(const char *name)
{
    if (!name || !name[0]) return -1;
    for (int m = 1; m <= 108; m++) if (ModelInfo(m) && _stricmp(ModelName(m), name) == 0) return m;
    return -1;
}

// Comme Undress + Dress, mais vers un modele normal (deja charge).
void SetPedModel(void *ped, int model)
{
    ((void(__thiscall *)(void *))(*(void ***)ped)[6])(ped);   // DeleteRwObject (libere l'ancien modele)
    WorldRemove(ped);
    ModelIndex(ped) = (short)model;
    Dress(ped);                                                // SetModelIndex(model), etat remis, WorldAdd
}

// Les missions (et le magasin de vetements) habillent Tommy par Undress + Dress, qui reprend le modele en cours :
// s'il porte un modele de passant, on le remet sur le modele 0 pour que la tenue de la mission s'applique.
static void __fastcall h_Undress(void *ped, void *edx, const char *name)
{
    o_Undress(ped, edx, name);
    if (PedType(ped) == 0 && ModelIndex(ped) != MI_PLAYER) ModelIndex(ped) = MI_PLAYER;
}

// Nom de la tenue portee (celui qu'on envoie aux autres joueurs).
const char *PedOutfit(void *ped) { return ModelName(ModelIndex(ped) >= 0 ? ModelIndex(ped) : MI_PLAYER); }

static bool ApplySkin(const char *name)
{
    void *me = FindPlayerPed();
    if (!me || InVehicle(me)) return false;
    char lower[24], previous[24];
    lstrcpynA(lower, name, sizeof(lower));
    CharLowerA(lower);
    lstrcpynA(previous, PedOutfit(me), sizeof(previous));
    int regular = RegularPedModel(lower);
    if (regular > 0) {
        if (!HasModelLoaded(regular)) { RequestModel(regular, 1 | 8); LoadAllRequestedModels(); }
        if (!HasModelLoaded(regular)) { Log("tenues : %s ne se charge pas", lower); return false; }
        SetPedModel(me, regular);
        return true;
    }
    Undress(me, lower);   // (remet le modele 0 si Tommy portait un passant, cf. h_Undress)
    LoadAllRequestedModels();
    if (!HasModelLoaded(MI_PLAYER)) {
        // Modele illisible : on remet l'ancien plutot que d'habiller Tommy avec rien (plantage dans Dress).
        Log("tenues : %s ne se charge pas, retour a %s", lower, previous);
        int prevRegular = RegularPedModel(previous);
        Undress(me, prevRegular > 0 ? "player" : previous);
        LoadAllRequestedModels();
        if (!HasModelLoaded(MI_PLAYER)) { Undress(me, "player"); LoadAllRequestedModels(); }
        Dress(me);
        if (prevRegular > 0 && HasModelLoaded(prevRegular)) SetPedModel(me, prevRegular);
        return false;
    }
    Dress(me);
    return true;
}

static bool g_menuOpen;
static int g_menuIndex;
static char g_menuOriginal[24];

static int FloatBits(float f) { int i; memcpy(&i, &f, 4); return i; }

static void MenuCamera()
{
    void *me = FindPlayerPed();
    float h = Heading(me), fx = -sinf(h), fy = cosf(h);
    Vec3 p = Pos(me);
    int32_t cam[6] = { FloatBits(p.x + fx * 3.2f), FloatBits(p.y + fy * 3.2f), FloatBits(p.z + 0.6f), 0, 0, 0 };
    int32_t at[4] = { FloatBits(p.x), FloatBits(p.y), FloatBits(p.z + 0.2f), 2 };
    MirrorLocal(0x015F, 6, cam);   // SET_FIXED_CAMERA_POSITION : devant lui
    MirrorLocal(0x0160, 4, at);    // POINT_CAMERA_AT_POINT (coupe franche)
}

// Cinematique en cours, ou controles retires par le jeu (mission, fondu) : pas de choix de tenue.
static bool PlayerFree()
{
    return !*(bool *)0xA10AB2 && *(short *)(0x7DBCB0 + 0xF0) == 0;
}

static void OpenMenu()
{
    void *me = FindPlayerPed();
    if (!me || InVehicle(me) || !g_skinCount || !PlayerFree()) return;
    lstrcpynA(g_menuOriginal, PedOutfit(me), sizeof(g_menuOriginal));
    g_menuIndex = 0;
    for (int i = 0; i < g_skinCount; i++) if (_stricmp(g_skins[i], g_menuOriginal) == 0) g_menuIndex = i;
    int32_t off[2] = { 0, 0 };
    MirrorLocal(0x01B4, 2, off);   // SET_PLAYER_CONTROL joueur 0, non
    MoveSpeed(me) = { 0, 0, 0 };
    MenuCamera();
    g_menuOpen = true;
    Log("tenues : choix ouvert (tenue actuelle %s)", g_menuOriginal);
}

static void CloseMenu(bool keep)
{
    if (!keep && _stricmp(PedOutfit(FindPlayerPed()), g_menuOriginal) != 0) ApplySkin(g_menuOriginal);
    if (keep) {
        lstrcpynA(g_cfg.skin, PedOutfit(FindPlayerPed()), sizeof(g_cfg.skin));
        char ini[MAX_PATH];
        wsprintfA(ini, "%svccoop.ini", GameDir());
        WritePrivateProfileStringA("VCCoop", "Tenue", g_cfg.skin, ini);
        Log("tenues : %s choisie", g_cfg.skin);
    }
    int32_t on[2] = { 0, 1 };
    MirrorLocal(0x02EB, 0, NULL);   // RESTORE_CAMERA_JUMPCUT
    MirrorLocal(0x0373, 0, NULL);   // SET_CAMERA_BEHIND_PLAYER
    MirrorLocal(0x01B4, 2, on);
    g_menuOpen = false;
}

bool SkinMenuOpen() { return g_menuOpen; }

static bool KeyEdge(int vk, bool &was)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !was;
    was = down;
    return edge;
}

static void RdvKey(bool inGame);

void PlayersFrame(bool inGame)
{
    RdvKey(inGame);
    static bool built;
    if (!built && inGame) { built = true; BuildSkinList(); }
    // Tenue choisie les fois precedentes : remise en arrivant en partie (apres un chargement aussi).
    static bool applied;
    if (!inGame) { applied = false; if (g_menuOpen) g_menuOpen = false; return; }
    if (!applied && g_cfg.skin[0] && !InVehicle(FindPlayerPed())) {
        applied = true;
        bool known = false;
        for (int i = 0; i < g_skinCount; i++) known |= _stricmp(g_skins[i], g_cfg.skin) == 0;
        if (known && _stricmp(PedOutfit(FindPlayerPed()), g_cfg.skin) != 0) { ApplySkin(g_cfg.skin); Log("tenues : %s remise", g_cfg.skin); }
    }
    // Autotest=tenue : ouvre le choix 5 s apres l'arrivee, avance d'une tenue toutes les 2 s, garde la 4e.
    if (_stricmp(g_cfg.autotest, "tenue") == 0) {
        static uint32_t start, step;
        uint32_t now = GetTickCount();
        if (!PlayerFree() && step == 0) { start = 0; return; }   // attend d'avoir la main
        if (!start) start = now;
        if (step == 0 && now - start > 5000) { OpenMenu(); step = 1; }
        else if (g_menuOpen && step >= 1 && now - start > 5000 + step * 2000) {
            // TestTenues=a,b,c : cette suite-la ; sinon on saute de 17 en 17.
            char list[256], *tok, *ctx = NULL;
            char ini[MAX_PATH];
            wsprintfA(ini, "%svccoop.ini", GameDir());
            GetPrivateProfileStringA("VCCoop", "TestTenues", "", list, sizeof(list), ini);
            const char *name = NULL;
            int k = 1;
            for (tok = strtok_s(list, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx), k++) if (k == (int)step) name = tok;
            if (!list[0] && step <= 4) { g_menuIndex = (g_menuIndex + 17) % g_skinCount; name = g_skins[g_menuIndex]; }
            if (!name) { CloseMenu(true); step = 1000; return; }
            Log("autotest : tenue %s", name);
            bool ok = ApplySkin(name);
            Log("autotest : tenue %s %s", name, ok ? "mise" : "refusee");
            MenuCamera();
            step++;
        }
        return;
    }
    static bool wF7, wLeft, wRight, wEnter, wBack;
    bool focus = GameHasFocus();
    bool f7 = focus && KeyEdge(VK_F7, wF7);
    if (!g_menuOpen) {
        if (f7 && !MenuActive()) OpenMenu();
        return;
    }
    // Le menu est ouvert : une mission qui reprend la main, ou le joueur monte dans un vehicule, le ferme.
    if (InVehicle(FindPlayerPed())) { CloseMenu(false); return; }
    static uint32_t repeatAt;
    bool left = focus && KeyEdge(VK_LEFT, wLeft), right = focus && KeyEdge(VK_RIGHT, wRight);
    // Touche maintenue : defilement (5 par seconde apres une demi-seconde).
    bool heldL = focus && (GetAsyncKeyState(VK_LEFT) & 0x8000), heldR = focus && (GetAsyncKeyState(VK_RIGHT) & 0x8000);
    uint32_t now = GetTickCount();
    if (left || right) repeatAt = now + 500;
    else if ((heldL || heldR) && now >= repeatAt) { repeatAt = now + 200; left = heldL; right = heldR; }
    if (left || right) {
        g_menuIndex = (g_menuIndex + (right ? 1 : g_skinCount - 1)) % g_skinCount;
        ApplySkin(g_skins[g_menuIndex]);
        MenuCamera();
    }
    if (focus && KeyEdge(VK_RETURN, wEnter)) CloseMenu(true);
    else if (f7 || (focus && KeyEdge(VK_BACK, wBack))) CloseMenu(false);
}

// ======================================================================= Messages (arrivees, departs, coupures)
static wchar_t g_notice[96];
static uint32_t g_noticeUntil, g_noticeColor;

static void OnNotice(const char *fr, const char *en, int player)
{
    bool french = *(int *)(0x869630 + 0x50) == 1;
    const char *name = player >= 0 && player < MAX_PLAYERS && g_players[player].state.name[0] ? g_players[player].state.name : "";
    char text[96];
    if (!g_cfg.host && player == 0 && strstr(fr, "hote")) lstrcpynA(text, french ? fr : en, sizeof(text));   // phrase complete
    else _snprintf(text, sizeof(text), "%s %s", name, french ? fr : en);
    text[sizeof(text) - 1] = 0;
    MultiByteToWideChar(CP_ACP, 0, text, -1, g_notice, 96);
    g_noticeColor = player >= 0 ? PlayerColor(player) : 0xFFFFFFFF;
    g_noticeUntil = GetTickCount() + 5000;
}

static void DrawNotice()
{
    if (!g_noticeUntil || GetTickCount() > g_noticeUntil) return;
    FontSetup(0.7f);
    FontColor(g_noticeColor);
    FontPrint(ScreenW() * 0.5f, ScreenH() * 0.12f, g_notice);
}

// ======================================================================= Point de rendez-vous (touche B)
// Chaque joueur peut poser un repere (la ou il regarde, jusqu'a 300 m) : fleche dans le monde et point sur le radar,
// a sa couleur, chez tout le monde. B a nouveau le retire. Renvoye toutes les 3 s (arrivees en cours de route).
struct Rdv { bool active; float pos[3]; int blip; };
static Rdv g_rdv[MAX_PLAYERS];

static int SetCoordBlip(float x, float y, float z) { return ((int(__cdecl *)(int, float, float, float, uint32_t, int))0x4C3C80)(4, x, y, z, 0, 3); }

static void ApplyRdv(int player, bool active, const float *pos)
{
    if (player < 0 || player >= MAX_PLAYERS || player == g_localId && !active && !g_rdv[player].active) return;
    Rdv &r = g_rdv[player];
    bool moved = active && (!r.active || fabsf(r.pos[0] - pos[0]) + fabsf(r.pos[1] - pos[1]) + fabsf(r.pos[2] - pos[2]) > 0.5f);
    if (r.active && (!active || moved)) { RemovePlayerBlip(r.blip); r.active = false; }
    if (active && moved) {
        memcpy(r.pos, pos, sizeof(r.pos));
        r.blip = SetCoordBlip(pos[0], pos[1], pos[2]);
        if (r.blip != -1) { ChangeBlipColour(r.blip, PlayerColor(player)); ChangeBlipScale(r.blip, 3); }
        r.active = true;
        if (player != g_localId && g_onNotice) g_onNotice("a pose un point de rendez-vous", "set a meeting point", player);
    }
}

static void OnRdv(const MsgRdv &m) { if (m.player != g_localId) ApplyRdv(m.player, m.active != 0, m.pos); }

static void SendRdv()
{
    const Rdv &r = g_rdv[g_localId];
    MsgRdv m = { MSG_RDV, (uint8_t)g_localId, (uint8_t)r.active, { r.pos[0], r.pos[1], r.pos[2] } };
    NetSendToAll(&m, sizeof(m));
}

// Point vise : rayon depuis la camera (a notre adresse de TheCamera, la matrice commence en +0 : avant = "up"
// +0x10, position +0x30 ; verifie en jeu).
static bool AimPoint(float *out)
{
    uint8_t *cam = (uint8_t *)0x7E4688;
    float o[3] = { Field<float>(cam, 0x30), Field<float>(cam, 0x34), Field<float>(cam, 0x38) };
    float f[3] = { Field<float>(cam, 0x10), Field<float>(cam, 0x14), Field<float>(cam, 0x18) };
    float e[3] = { o[0] + f[0] * 300.0f, o[1] + f[1] * 300.0f, o[2] + f[2] * 300.0f };
    uint8_t colPoint[64] = {};
    void *hit = NULL;
    bool ok = ((bool(__cdecl *)(const float *, const float *, void *, void **, bool, bool, bool, bool, bool, bool, bool, bool))0x4D92D0)(
        o, e, colPoint, &hit, true, true, false, true, false, false, false, false);
    if (!ok) return false;
    memcpy(out, colPoint, 12);   // CColPoint : point d'impact en tete
    return true;
}

static void RdvKey(bool inGame)
{
    static bool wasB;
    static uint32_t lastSend;
    if (!inGame || g_localId < 0) { for (auto &r : g_rdv) { r.active = false; r.blip = -1; } return; }
    for (int i = 0; i < MAX_PLAYERS; i++)   // joueur parti : son repere aussi
        if (i != g_localId && g_rdv[i].active && !g_players[i].connected) ApplyRdv(i, false, g_rdv[i].pos);
    bool b = GameHasFocus() && !g_menuOpen && KeyEdge('B', wasB);
    static bool autoDone;   // Autotest=rdv : en pose un 20 s apres l'arrivee
    static uint32_t autoAt;
    if (_stricmp(g_cfg.autotest, "rdv") == 0 && !autoDone) {
        if (!autoAt) autoAt = GetTickCount();
        if (GetTickCount() - autoAt > 35000) { autoDone = true; b = true; }
    }
    if (b) {
        Rdv &r = g_rdv[g_localId];
        if (r.active) { ApplyRdv(g_localId, false, r.pos); Log("rendez-vous : retire"); }
        else {
            float p[3];
            void *me = FindPlayerPed();
            bool aimed = AimPoint(p);
            if (!aimed) { p[0] = Pos(me).x; p[1] = Pos(me).y; p[2] = Pos(me).z; }
            ApplyRdv(g_localId, true, p);
            Log("rendez-vous : pose en %.1f %.1f %.1f (%s, joueur en %.1f %.1f)", p[0], p[1], p[2], aimed ? "vise" : "a ses pieds",
                Pos(me).x, Pos(me).y);
        }
        SendRdv();
        lastSend = GetTickCount();
    } else if (g_rdv[g_localId].active && GetTickCount() - lastSend > 3000) {
        SendRdv();
        lastSend = GetTickCount();
    }
}

// ======================================================================= Liste des joueurs (touche Tab maintenue)
static void DrawPlayerList()
{
    bool forced = _stricmp(g_cfg.autotest, "liste") == 0;   // autotest : toujours affichee
    if (!forced && (!GameHasFocus() || !(GetAsyncKeyState(VK_TAB) & 0x8000) || g_menuOpen)) return;
    bool fr = *(int *)(0x869630 + 0x50) == 1;
    void *me = FindPlayerPed();
    float y = ScreenH() * 0.22f, step = ScreenH() * 0.045f;
    FontSetup(0.8f);
    FontColor(0xFFFFFFFF);
    FontPrint(ScreenW() * 0.5f, y, fr ? L"JOUEURS" : L"PLAYERS");
    y += step * 1.3f;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const NetPlayer &np = g_players[i];
        bool self = i == g_localId;
        if (!self && !np.connected) continue;
        const MsgState &s = np.state;
        wchar_t line[160], name[32];
        MultiByteToWideChar(CP_ACP, 0, self ? g_cfg.playerName : s.name, -1, name, 32);
        float dist = 0;
        if (!self && me && s.inGame) {
            float dx = s.pos[0] - Pos(me).x, dy = s.pos[1] - Pos(me).y, dz = s.pos[2] - Pos(me).z;
            dist = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        int ping = self ? (g_cfg.host ? 0 : g_myPing) : (i == 0 ? (g_cfg.host ? 0 : g_myPing) : s.ping);
        float health = self && me ? Health(me) : s.health, armour = self && me ? Armour(me) : s.armour;
        if (!s.inGame && !self)
            swprintf(line, 160, fr ? L"%s%s   (au menu)" : L"%s%s   (in menu)", name, i == 0 ? (fr ? L" (hote)" : L" (host)") : L"");
        else if (self)
            swprintf(line, 160, fr ? L"%s%s   sante %d   gilet %d   ping %d ms   (vous)" : L"%s%s   health %d   armour %d   ping %d ms   (you)",
                     name, i == 0 ? (fr ? L" (hote)" : L" (host)") : L"", (int)health, (int)armour, ping);
        else
            swprintf(line, 160, fr ? L"%s%s   sante %d   gilet %d   ping %d ms   %d m%s" : L"%s%s   health %d   armour %d   ping %d ms   %d m%s",
                     name, i == 0 ? (fr ? L" (hote)" : L" (host)") : L"", (int)health, (int)armour, ping, (int)dist,
                     s.inVehicle ? (fr ? L"   en vehicule" : L"   in a vehicle") : L"");
        FontSetup(0.65f);
        FontColor(PlayerColor(i));
        FontPrint(ScreenW() * 0.5f, y, line);
        y += step;
    }
}

// ======================================================================= Dessin (avant l'interface du jeu)
static void DrawSkinMenu()
{
    if (!g_menuOpen) return;
    wchar_t line[128];
    bool fr = *(int *)(0x869630 + 0x50) == 1;
    FontSetup(0.9f);
    FontColor(0xFFFFFFFF);
    swprintf(line, 128, L"%S  (%d/%d)", g_skins[g_menuIndex], g_menuIndex + 1, g_skinCount);
    FontPrint(ScreenW() * 0.5f, ScreenH() * 0.70f, line);
    FontSetup(0.6f);
    FontColor(0xDDDDDDFF);
    FontPrint(ScreenW() * 0.5f, ScreenH() * 0.76f, fr ? L"GAUCHE / DROITE : CHOISIR   ENTREE : GARDER   RETOUR : ANNULER"
                                                   : L"LEFT / RIGHT: CHOOSE   ENTER: KEEP   BACKSPACE: CANCEL");
}

static void __cdecl h_Render2dStuff()
{
    if (GameState() == GS_PLAYING && FindPlayerPed()) { DrawNametags(); DrawSkinMenu(); DrawNotice(); DrawPlayerList();
        if (FreeAimActive()) { FontSetup(1.0f); FontColor(0xFFFFFFE0); FontPrint(ScreenW() * 0.5f, ScreenH() * 0.5f - ScreenH() * 0.03f, L"+"); } }
    ((void(__cdecl *)())0x4A6190)();
}

void InstallPlayers()
{
    g_onNotice = OnNotice;
    g_onRdv = OnRdv;
    static const uint8_t undressPro[] = { 0x53, 0x89, 0xCB, 0x8B, 0x43, 0x4C, 0x56, 0x55 };
    o_Undress = (Undress_t)MakeDetour(0x4EF030, undressPro, sizeof(undressPro), (void *)h_Undress);
    static const uint8_t call[] = { 0xE8, 0xFD, 0x00, 0x00, 0x00 };   // call 0x4A6190 (depuis 0x4A608E)
    if (memcmp((void *)0x4A608E, call, sizeof(call)) != 0) { Log("reperes : appel de Render2dStuff introuvable"); return; }
    PatchCall(0x4A608E, (void *)h_Render2dStuff);
}
