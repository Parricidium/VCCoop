// Boucle coop, une fois par image sur le fil du jeu : reseau, envoi de notre etat, Tommy distants.
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "vehicles.h"
#include "entities.h"
#include "mirror.h"
#include "seats.h"
#include "combat.h"
#include "saveshare.h"
#include <math.h>
#include <string.h>

using namespace game;

void game::SetHeadingMatrix(void *e, float h)
{
    float c = cosf(h), s = sinf(h);
    Field<Vec3>(e, 0x04) = { c, s, 0 };
    Field<Vec3>(e, 0x14) = { -s, c, 0 };
    Field<Vec3>(e, 0x24) = { 0, 0, 1 };
}

// --- Tommy distants : un personnage de mission par joueur, deplace selon son etat reseau ---
struct Puppet {
    void *ped;          // remis a NULL par le jeu s'il detruit le personnage (RegisterReference)
    int lastMoveState;
    uint8_t lastShots;  // dernier compteur de tirs rejoue
    char outfit[21];    // tenue avec laquelle il a ete cree
};

// La tenue de Tommy est le modele 0, propre a chaque instance : le Tommy d'un autre joueur utilise un
// emplacement de personnage special reserve (special18..21 = modeles 126..129 pour les joueurs 0..3),
// charge avec la tenue de ce joueur.
enum { MI_PUPPET_BASE = 126 };
static bool EnsurePuppetModel(int player, const char *outfit)
{
    int model = MI_PUPPET_BASE + player;
    const char *want = outfit[0] ? outfit : "player";
    if (_stricmp(ModelName(model), want) != 0) { RequestSpecialModel(model, want, 1 | 8); return false; }
    return HasModelLoaded(model);
}
static Puppet g_puppets[MAX_PLAYERS];

void *PuppetPed(int player) { return player >= 0 && player < MAX_PLAYERS ? g_puppets[player].ped : NULL; }

bool IsPuppetVehicle(void *veh)
{
    for (auto &pp : g_puppets) if (pp.ped && InVehicle(pp.ped) && PedVehicle(pp.ped) == veh && VehDriver(veh) == pp.ped) return true;
    return false;
}

int PuppetPlayer(void *ped)
{
    for (int i = 0; i < MAX_PLAYERS; i++) if (g_puppets[i].ped && g_puppets[i].ped == ped) return i;
    return -1;
}

bool IsPuppet(void *ped)
{
    for (auto &pp : g_puppets) if (pp.ped && pp.ped == ped) return true;
    return false;
}

static void DestroyPuppet(Puppet &pp)
{
    if (!pp.ped) return;
    void *ped = pp.ped;
    CleanUpOldReference(ped, &pp.ped);
    if (InVehicle(ped)) WarpOutOfVehicle(ped, NULL);
    WorldRemove(ped);
    RemoveReferencesToDeletedObject(ped);
    DeleteEntity(ped);
    pp.ped = NULL;
}

static void CreatePuppet(Puppet &pp, const MsgState &s)
{
    if (!EnsurePuppetModel(s.id, s.outfit)) return;   // on reessaiera a l'image suivante
    void *ped = PedAlloc();
    if (!ped) { Log("coop : plus de place pour un personnage"); return; }
    CivilianPedCtor(ped, PEDTYPE_CIVMALE, MI_PUPPET_BASE + s.id);
    lstrcpynA(pp.outfit, s.outfit, sizeof(pp.outfit));
    pp.lastShots = s.shots;
    CharCreatedBy(ped) = PED_CHAR_MISSION;         // jamais retire par la population
    Field<uint8_t>(ped, 0x14E) &= ~0x02;           // bRespondsToThreats = 0 : ne fuit pas, ne riposte pas
    Field<uint8_t>(ped, 0x53) |= 0x1E;             // invulnerable pour l'instant (balles, feu, chocs, melee)
    Pos(ped) = { s.pos[0], s.pos[1], s.pos[2] };
    SetHeadingMatrix(ped, s.heading);
    Heading(ped) = HeadingGoal(ped) = s.heading;
    AreaCode(ped) = s.area;
    WorldAdd(ped);
    pp.ped = ped;
    pp.lastMoveState = -1;
    RegisterReference(ped, &pp.ped);
    Log("coop : Tommy de %s cree (%p, tenue %s) en %.1f %.1f %.1f", s.name, ped, s.outfit, s.pos[0], s.pos[1], s.pos[2]);
}

// Le Tommy distant monte dans la copie locale de son vehicule (a sa place), ou en descend.
// Renvoie vrai s'il est dans un vehicule (sa position est alors celle du vehicule).
static bool UpdatePuppetVehicle(Puppet &pp, const MsgState &s)
{
    void *ped = pp.ped;
    void *want = s.inVehicle ? NetVehicleById(s.vehicleId) : NULL;
    void *cur = InVehicle(ped) ? PedVehicle(ped) : NULL;
    if (cur && (cur != want || SeatOf(cur, ped) != s.seat)) {
        Vec3 at = { s.pos[0], s.pos[1], s.pos[2] };
        WarpOutOfVehicle(ped, &at);
        pp.lastMoveState = -1;
        Log("coop : Tommy %d descend du vehicule", s.id);
        cur = NULL;
    }
    if (want && !cur && WarpIntoSeat(ped, want, s.seat)) {
        Log("coop : Tommy %d monte dans %08X (place %d)", s.id, s.vehicleId, s.seat);
        cur = want;
    }
    return cur != NULL;
}

static void UpdatePuppet(Puppet &pp, const NetPlayer &np)
{
    const MsgState &s = np.state;
    void *ped = pp.ped;
    Health(ped) = 100.0f;
    AreaCode(ped) = s.area;
    HoldWeapon(ped, s.weapon);
    if (UpdatePuppetVehicle(pp, s)) return;
    // Position : on rattrape la position recue (extrapolee avec sa vitesse) en douceur ; saut si trop loin.
    float age = (GetTickCount() - np.lastSeen) / 1000.0f;
    if (age > 0.25f) age = 0.25f;
    Vec3 target = { s.pos[0] + s.speed[0] * age * 50.0f, s.pos[1] + s.speed[1] * age * 50.0f,
                    s.pos[2] + s.speed[2] * age * 50.0f };
    Vec3 &p = Pos(ped);
    float dx = target.x - p.x, dy = target.y - p.y, dz = target.z - p.z;
    if (dx * dx + dy * dy + dz * dz > 25.0f) p = target;
    else { p.x += dx * 0.5f; p.y += dy * 0.5f; p.z += dz * 0.5f; }
    MoveSpeed(ped) = { s.speed[0], s.speed[1], s.speed[2] };
    SetHeadingMatrix(ped, s.heading);
    Heading(ped) = HeadingGoal(ped) = s.heading;
    AreaCode(ped) = s.area;
    Health(ped) = 100.0f;
    // En vehicule mais sans copie locale (pas encore creee, ou passager) : cache en attendant.
    uint8_t &flags = Field<uint8_t>(ped, 0x52);
    if (s.inVehicle) flags &= ~0x04; else flags |= 0x04;
    // Visee et tirs : bras leve tant qu'il vise, chaque nouveau tir est rejoue (3 au plus par image).
    if (s.aiming) SetAimFlag(ped, s.heading);
    else if (IsAimingGun(ped)) ClearAimFlag(ped);
    for (int n = 0; pp.lastShots != s.shots && n < 3; n++) { pp.lastShots++; PuppetShoot(ped, s.weapon); }
    pp.lastShots = s.shots;

    // L'IA du personnage remet son etat de deplacement a chaque image : on impose le notre a chaque fois
    // (SetMoveAnim ne relance l'animation que si elle change).
    int before = MoveState(ped);
    SetMoveStateFn(ped, s.moveState);
    SetMoveAnim(ped);
    if (s.moveState != pp.lastMoveState) {
        if (g_cfg.logScripts) Log("coop : Tommy %d deplacement %d -> %d (il etait a %d)", s.id, pp.lastMoveState, s.moveState, before);
        pp.lastMoveState = s.moveState;
    }
}

static void UpdatePuppets(bool inGame)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Puppet &pp = g_puppets[i];
        const NetPlayer &np = g_players[i];
        bool want = inGame && i != g_localId && np.connected && np.state.inGame;
        if (!want) { if (pp.ped && inGame) DestroyPuppet(pp); if (!inGame) pp.ped = NULL; continue; }
        // Changement de tenue : on le recree avec la nouvelle.
        if (pp.ped && _stricmp(pp.outfit, np.state.outfit) != 0 && !InVehicle(pp.ped)) {
            Log("coop : %s change de tenue (%s -> %s)", np.state.name, pp.outfit, np.state.outfit);
            DestroyPuppet(pp);
        }
        if (!pp.ped) CreatePuppet(pp, np.state);
        if (pp.ped) UpdatePuppet(pp, np);
    }
}

// Invite : le fondu de l'ecran suit celui de l'hote (les fondus de mission, et ceux que son script principal fait
// hors mission). Notre propre traitement du fondu est coupe pour ne pas lutter.
static void FollowHostFade(bool inGame)
{
    if (g_cfg.host || !inGame || g_localId <= 0) return;
    const NetPlayer &h = g_players[0];
    if (!h.connected || !h.state.inGame) return;
    float f = h.state.fade;
    if (f < 0.0f) f = 0.0f;
    if (f > 255.0f) f = 255.0f;
    CamFade() = f;
    CamFading() = false;
    DrawFadeValue() = (uint8_t)(f + 0.5f);
}

static void SendLocalState(bool inGame)
{
    static uint32_t seq, lastSend;
    uint32_t now = GetTickCount();
    if (now - lastSend < 33) return;
    lastSend = now;

    MsgState s = {};
    s.type = MSG_STATE;
    s.id = (uint8_t)g_localId;
    s.seq = ++seq;
    lstrcpynA(s.name, g_cfg.playerName, sizeof(s.name));
    lstrcpynA(s.outfit, ModelName(MI_PLAYER), sizeof(s.outfit));
    void *ped = inGame ? FindPlayerPed() : NULL;
    if (ped) {
        s.inGame = 1;
        Vec3 p = Pos(ped), v = MoveSpeed(ped);
        s.pos[0] = p.x; s.pos[1] = p.y; s.pos[2] = p.z;
        s.speed[0] = v.x; s.speed[1] = v.y; s.speed[2] = v.z;
        s.heading = Heading(ped);
        s.health = Health(ped);
        s.armour = Armour(ped);
        s.area = AreaCode(ped);
        s.moveState = (uint8_t)MoveState(ped);
        s.pedState = (uint8_t)PedState(ped);
        s.fade = CamFade();
        s.weapon = (uint8_t)WeaponTypeInSlot(ped, CurrentWeaponSlot(ped));
        s.shots = LocalShotCount();
        s.aiming = IsAimingGun(ped) ? 1 : 0;
        s.inVehicle = InVehicle(ped) ? 1 : 0;
        if (s.inVehicle && PedVehicle(ped)) {
            s.vehicleId = NetVehicleId(PedVehicle(ped));
            int seat = SeatOf(PedVehicle(ped), ped);
            s.seat = (uint8_t)(seat < 0 ? 0 : seat);
        }
    }
    NetSendState(s);
    if (g_localId >= 0) g_players[g_localId].state = s;
}

// --- Heure et meteo : l'hote fait foi ---
static void SendWorld()
{
    static uint32_t last;
    uint32_t now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    MsgWorld w = { MSG_WORLD, ClockHours(), ClockMinutes(), (uint8_t)(ClockSeconds() & 0xFF),
                   OldWeather(), NewWeather(), ForcedWeather(), PedHandle(FindPlayerPed()),
                   CamFade(), (uint8_t)CamFading(), (uint8_t)CamWidescreen() };
    NetSendToGuests(&w, sizeof(w));
}

static void OnWorld(const MsgWorld &w)
{
    if (GameState() != GS_PLAYING) return;
    g_hostPlayerHandle = w.playerHandle;
    // L'heure n'est recalee que si elle derive de plus d'une minute (sinon les deux horloges avancent seules).
    int local = ClockHours() * 60 + ClockMinutes(), remote = w.hours * 60 + w.minutes;
    int diff = remote - local;
    if (diff < -720) diff += 1440;
    if (diff > 720) diff -= 1440;
    if (diff < -1 || diff > 1) {
        ClockHours() = w.hours;
        ClockMinutes() = w.minutes;
        ClockSeconds() = w.seconds;
    }
    // Meteo : on impose le temps de l'hote (celui vers lequel il va, ou celui qu'un script a force).
    short want = w.forcedWeather >= 0 ? w.forcedWeather : w.newWeather;
    if (ForcedWeather() != want) {
        ForcedWeather() = want;
        OldWeather() = w.oldWeather;
        NewWeather() = w.newWeather;
    }
}

// Invite : a son arrivee en partie, apres un chargement, et au debut / a la fin de chaque mission de l'hote,
// il est pose a cote de l'hote.
static bool g_gathered;
void RequestGather() { g_gathered = false; }

static void GatherToHost(bool inGame)
{
    bool &gathered = g_gathered;
    // Mort ou arrete : il reapparait a l'hopital / au commissariat ; on le ramene ensuite pres de l'hote.
    static bool wasDown;
    if (inGame && !g_cfg.host) {
        void *me = FindPlayerPed();
        bool down = me && (Health(me) <= 0.0f || PedState(me) == 54 || PedState(me) == 55 || PedState(me) == 56);
        if (wasDown && !down) { gathered = false; Log("coop : de retour apres la mort / l'arrestation"); }
        wasDown = down;
    }
    if (!inGame) { gathered = false; return; }
    if (g_cfg.host || gathered || g_localId <= 0) return;
    const NetPlayer &h = g_players[0];
    if (!h.connected || !h.state.inGame) return;
    void *ped = FindPlayerPed();
    if (InVehicle(ped)) return;
    float hh = h.state.heading;
    // Derriere l'hote (d'ou il vient, donc un endroit libre), decale d'un pas par joueur.
    float back = 1.5f + 0.8f * (g_localId - 1), side = (g_localId % 2 ? 0.6f : -0.6f);
    Pos(ped) = { h.state.pos[0] + sinf(hh) * back + cosf(hh) * side, h.state.pos[1] - cosf(hh) * back + sinf(hh) * side,
                 h.state.pos[2] + 0.3f };
    MoveSpeed(ped) = { 0, 0, 0 };
    AreaCode(ped) = h.state.area;
    gathered = true;
    Log("coop : pose a cote de l'hote (%.1f %.1f %.1f)", Pos(ped).x, Pos(ped).y, Pos(ped).z);
}

static bool OtherPlayersConnected()
{
    for (int i = 0; i < MAX_PLAYERS; i++) if (i != g_localId && g_players[i].connected) return true;
    return false;
}

// Touche G : monter comme passager dans le vehicule d'un autre joueur (a moins de 6 m), ou en descendre.
// VC ne permet pas de monter en passager ; on installe directement le joueur a une place libre.
void TogglePassenger()
{
    void *me = FindPlayerPed();
    if (!me) return;
    if (InVehicle(me)) {
        void *veh = PedVehicle(me);
        if (veh && SeatOf(veh, me) > 0) { WarpOutOfVehicle(me, NULL); Log("coop : je descends (passager)"); }
        return;
    }
    Pool *pool = VehiclePool();
    void *best = NULL;
    float bestD = 36.0f;
    for (int i = 0; i < pool->size; i++) {
        if (pool->flags[i] & 0x80) continue;
        void *v = pool->objects + i * VEHICLE_POOL_ENTRY;
        if (!VehDriver(v) || !IsPuppet(VehDriver(v))) continue;
        float dx = Pos(v).x - Pos(me).x, dy = Pos(v).y - Pos(me).y, dz = Pos(v).z - Pos(me).z;
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) { bestD = d; best = v; }
    }
    if (best && WarpIntoSeat(me, best, 1)) Log("coop : je monte en passager (place %d)", SeatOf(best, me));
}

static void PassengerKey()
{
    static bool wasDown;
    bool down = GameHasFocus() && (GetAsyncKeyState('G') & 0x8000);
    if (down && !wasDown) TogglePassenger();
    wasDown = down;
}

void CoopFrame()
{
    static bool netStarted, autoStarted;
    static DWORD frames, lastLog;
    if (!netStarted) {
        netStarted = true; g_onWorld = OnWorld; VehiclesInit(); EntitiesInit(); MirrorInit();
        if (g_cfg.netAuto) CoopStartNetwork();
    }

    // Instances de test : lance directement une nouvelle partie depuis le menu.
    if (g_cfg.autoStart && !autoStarted && GameState() == GS_FRONTEND) {
        autoStarted = true;
        MenuWantToLoad() = 0;
        MenuFirstTime() = 0;
        MenuWantToRestart() = 1;
        Log("demarrage automatique d'une nouvelle partie");
    }

    // Le vrai "Commencer partie" ferme aussi le menu et leve la pause qu'il avait posee (CTimer::m_UserPause) ;
    // sans ca l'horloge reste a 0 et le jeu attend indefiniment derriere l'ecran de chargement.
    static bool autoUnpaused;
    if (autoStarted && !autoUnpaused && GameState() == GS_PLAYING) {
        autoUnpaused = true;
        MenuActive() = 0;
        UserPause() = false;
        Log("pause du menu levee apres le demarrage automatique");
    }

    MenuFrame();
    SaveShareFrame();
    AutotestFrame();
    NetPoll();
    // En coop, le menu Pause n'arrete pas le monde (sinon toute la partie des invites se fige derriere le menu
    // de l'hote) : on leve la pause joueur tant qu'un autre joueur est connecte.
    if (GameState() == GS_PLAYING && MenuActive() && UserPause() && OtherPlayersConnected()) UserPause() = false;
    bool inGame = GameState() == GS_PLAYING && FindPlayerPed() != NULL;
    GatherToHost(inGame);
    if (inGame) PassengerKey();
    VehiclesFrame(inGame);
    EntitiesFrame(inGame);
    MirrorFrame(inGame);
    SendLocalState(inGame);
    FollowHostFade(inGame);
    if (g_cfg.host && inGame) SendWorld();
    UpdatePuppets(inGame);

    frames++;
    DWORD now = GetTickCount();
    if (now - lastLog >= 10000) {
        lastLog = now;
        void *ped = inGame ? FindPlayerPed() : NULL;
        Log("image %lu, etat %d, joueur %d, premier plan=%d%s", frames, GameState(), g_localId, GameHasFocus(),
            ped ? "" : " (pas en partie)");
        if (ped) Log("  moi : %.1f %.1f %.1f cap %.2f marche %d", Pos(ped).x, Pos(ped).y, Pos(ped).z, Heading(ped), MoveState(ped));
        if (g_cfg.logScripts && GameState() == GS_PLAYING) {
            char line[1024];
            int n = wsprintfA(line, "  scripts :");
            for (void *sc = ActiveScripts(); sc && n < 900; sc = Field<void *>(sc, 0)) {
                int ip = Field<int>(sc, 0x10);
                n += wsprintfA(line + n, " %.8s@%X(op %04X,reveil %d)", (char *)sc + 8, ip,
                               *(uint16_t *)(ScriptSpace() + ip), Field<int>(sc, 0x7C));
            }
            Log("%s", line);
            Log("  horloge %u ms, pas %.3f, image %u, pause joueur %d, pause code %d", TimeInMs(), TimeStep(),
                FrameCounter(), UserPause(), CodePause());
            Log("  camera : fondu %.1f (en cours %d, sens %d), bandes %d", CamFade(), CamFading(), CamFadeDir(), CamWidescreen());
        }
        for (int i = 0; i < MAX_PLAYERS; i++)
            if (g_puppets[i].ped)
                Log("  Tommy %d : %.1f %.1f %.1f", i, Pos(g_puppets[i].ped).x, Pos(g_puppets[i].ped).y, Pos(g_puppets[i].ped).z);
    }
}
