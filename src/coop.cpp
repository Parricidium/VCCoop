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
#include "population.h"
#include "conditions.h"
#include "interp.h"
#include "anims.h"
#include "players.h"
#include "camera.h"
#include "overlay.h"

int WantedLevel(void *ped);   // plus bas : etoiles de recherche
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
    Track track;        // etats recus, pour l'interpolation (interp.cpp)
    AnimMirror anims;   // animations d'action recues, posees sur lui
    int blip;           // son point de couleur sur le radar (-1 : aucun)
    bool entering, exiting;   // montee / descente animee en cours (objectif donne au personnage, le jeu joue la scene)
    uint32_t busySince;
    uint32_t seatedAt;        // derniere installation dans un vehicule (montee finie ou pose)
    uint32_t exitedAt;        // derniere descente animee finie...
    uint32_t exitedFrom;      // ...et de quel vehicule reseau (pas reassis dedans tant que son etat dit encore "a bord")
};

// La tenue de Tommy est le modele 0, propre a chaque instance : le Tommy d'un autre joueur utilise un
// emplacement de personnage special reserve (special18..21 = modeles 126..129 pour les joueurs 0..3),
// charge avec la tenue de ce joueur.
// Une tenue de passant (choisie avec F7) est un modele normal : on l'utilise tel quel (le charger dans un emplacement
// special plante, cf. players.cpp). Renvoie le modele a utiliser, -1 tant qu'il charge.
enum { MI_PUPPET_BASE = 126 };
static int EnsurePuppetModel(int player, const char *outfit)
{
    int regular = RegularPedModel(outfit);
    if (regular > 0) {
        if (!HasModelLoaded(regular)) { RequestModel(regular, 1 | 8); return -1; }
        return regular;
    }
    int model = MI_PUPPET_BASE + player;
    const char *want = outfit[0] ? outfit : "player";
    if (_stricmp(ModelName(model), want) != 0) { RequestSpecialModel(model, want, 1 | 8); return -1; }
    return HasModelLoaded(model) ? model : -1;
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
    RemovePlayerBlip(pp.blip);
    CleanUpOldReference(ped, &pp.ped);
    if (InVehicle(ped)) WarpOutOfVehicle(ped, NULL);
    WorldRemove(ped);
    RemoveReferencesToDeletedObject(ped);
    DeleteEntity(ped);
    pp.ped = NULL;
}

static void CreatePuppet(Puppet &pp, const MsgState &s)
{
    int model = EnsurePuppetModel(s.id, s.outfit);
    if (model < 0) return;   // on reessaiera a l'image suivante
    void *ped = PedAlloc();
    if (!ped) { Log("coop : plus de place pour un personnage"); return; }
    CivilianPedCtor(ped, PEDTYPE_CIVMALE, model);
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
    pp.anims = {};
    pp.blip = AddPlayerBlip(ped, s.id);
    RegisterReference(ped, &pp.ped);
    if (g_cfg.watchPuppetField) WatchAddress((uintptr_t)ped + g_cfg.watchPuppetField);
    Log("coop : Tommy de %s cree (%p, tenue %s) en %.1f %.1f %.1f", s.name, ped, s.outfit, s.pos[0], s.pos[1], s.pos[2]);
}

// Le Tommy distant monte dans la copie locale de son vehicule (a sa place), ou en descend.
// Renvoie vrai s'il est dans un vehicule (sa position est alors celle du vehicule).
static void EvictNpcDriver(void *veh, const MsgState &s)
{
    // Hote : un personnage de l'IA a pris le volant de la voiture que ce joueur conduit (le proprietaire revenu
    // chercher sa voiture...) : on le fait descendre, c'est le joueur qui conduit.
    void *npc = VehDriver(veh);
    if (!npc || IsPuppet(npc) || npc == FindPlayerPed()) return;
    WarpOutOfVehicle(npc, NULL);
    ((void(__thiscall *)(void *))0x521720)(npc);   // CPed::ClearObjective
    Log("coop : un personnage conduisait la voiture de %s, il descend", s.name);
}

// Montee et descente animees : quand il monte chez lui (animation en cours), son double recoit l'objectif "monter
// dans cette voiture" des scripts (SET_CHAR_OBJ_ENTER_CAR_AS_DRIVER 01D5) et le jeu joue portiere et animation ;
// pareil pour descendre (SET_CHAR_OBJ_LEAVE_CAR 01D3). Pendant ce temps on ne touche a rien. Si ca traine (porte
// bloquee, voiture partie), on le pose directement comme avant.
// Vehicule conduit ici par le joueur local ou par un autre pantin : sa physique est a lui, on n'y touche pas
// (immobiliser la voiture de l'hote le temps qu'un pantin en descende la rendait inconduisible).
static bool LocallyDriven(void *veh, void *self)
{
    void *drv = VehDriver(veh);
    return drv && drv != self && (drv == FindPlayerPed() || IsPuppet(drv));
}

static bool UpdatePuppetVehicle(Puppet &pp, const MsgState &s)
{
    void *ped = pp.ped;
    void *want = s.inVehicle ? NetVehicleById(s.vehicleId) : NULL;
    void *cur = InVehicle(ped) ? PedVehicle(ped) : NULL;
    uint32_t now = GetTickCount();
    // Son vehicule est une epave chez nous (il y est mort, ou elle a explose) : on ne l'y installe pas (le jeu
    // detruisait aussitot le pantin, qui etait recree a chaque image) ; il reste cache le temps qu'il en sorte.
    if (want && EntityStatus(want) == STATUS_WRECKED && !cur) return false;
    if (pp.entering) {
        // La voiture demarre chez lui avant que son double soit assis : on le pose tout de suite (sinon il courait derriere).
        Vec3 ws = want ? MoveSpeed(want) : Vec3{ 0, 0, 0 };
        bool driving = want && (ws.x * ws.x + ws.y * ws.y > 0.01f);
        if (cur) { pp.entering = false; pp.seatedAt = now; Field<int>(ped, 0x164) = 0; Log("coop : Tommy %d est monte (animation)", s.id); }
        // Chez lui c'est fini depuis 2 s et notre double n'y est toujours pas : on le pose.
        else if (!(EnteringState(PedState(ped)) && now - pp.busySince < 12000) &&
                 (now - pp.busySince > 6000 || (!PedVehicle(ped) && now - pp.busySince > 2500) || driving || (s.inVehicle && now - pp.busySince > 3500))) {
            pp.entering = false;
            ((void(__thiscall *)(void *))0x521720)(ped);
            Vec3 at = Pos(ped);
            WarpOutOfVehicle(ped, &at);   // remet l'etat et les animations d'aplomb avant de le poser dans la voiture
            Log("coop : Tommy %d : montee animee abandonnee (%s)", s.id, driving ? "la voiture roule deja" : "trop long");
        }
        else return true;
    }
    if (pp.exiting) {
        if (!cur) { pp.exiting = false; pp.exitedAt = now; Field<int>(ped, 0x164) = 0; pp.lastMoveState = -1; Log("coop : Tommy %d est descendu (animation)", s.id); }
        // Jamais pendant que l'animation de sortie joue (etat 60 : porte, ou rampe hors d'une voiture retournee,
        // plusieurs secondes) : la couper laissait le personnage sans animation (plantage 0x403ED2).
        else if ((PedState(ped) != 60 && now - pp.busySince > 4000) || now - pp.busySince > 12000) {
            pp.exiting = false;
            ((void(__thiscall *)(void *))0x521720)(ped);
            Vec3 at = { s.pos[0], s.pos[1], s.pos[2] };
            WarpOutOfVehicle(ped, &at);
            cur = NULL;
            Log("coop : Tommy %d : descente animee abandonnee (etat %d)", s.id, PedState(ped));
        } else {
            // Le jeu refuse de sortir tant que le vehicule bouge (CanPedExitCar) : une copie pilotee par le reseau
            // est tenue immobile le temps qu'il accepte (jamais un vehicule que quelqu'un conduit ici).
            if (!LocallyDriven(cur, ped)) { MoveSpeed(cur) = { 0, 0, 0 }; TurnSpeed(cur) = { 0, 0, 0 }; }
            return true;
        }
    }
    // Notre animation de montee est finie avant la sienne (son etat dit encore "en train de monter") : il reste
    // assis (avant : pose dehors puis reassis quand son etat passait a "a bord" : la voiture partait vide).
    if (cur && !s.inVehicle && s.enterId && NetVehicleById(s.enterId) == cur) return true;
    if (!cur && !s.inVehicle && s.enterId) {
        void *veh = NetVehicleById(s.enterId);
        // Passager : l'IA du jeu refuse de faire monter un personnage en passager d'une voiture conduite par un
        // joueur (essaye dans les deux sens) : il sera pose a sa place quand il sera assis chez lui.
        bool passenger = s.enterSeat != 0;
        // Moto : l'objectif "monter au volant" n'aboutit pas (le double etait pose puis repris a chaque image) : pose direct.
        if (veh && !passenger && VehClass(veh) != VCLASS_BIKE) {
            EvictNpcDriver(veh, s);
            bool ok = !VehDriver(veh);
            if (ok) {
                int32_t a[2] = { (int32_t)PedHandle(ped), (int32_t)VehicleHandle(veh) };
                MirrorLocal(passenger ? 0x01D4 : 0x01D5, 2, a);
                pp.entering = true;
                pp.busySince = now;
                Log("coop : Tommy %d monte dans %08X (animation, %s)", s.id, s.enterId, passenger ? "passager" : "au volant");
                return true;
            }
        }
    }
    if (cur && s.exiting && (s.inVehicle ? cur == want : true) && now - pp.seatedAt > 1000) {
        int32_t a[2] = { (int32_t)PedHandle(ped), (int32_t)VehicleHandle(cur) };
        if (!LocallyDriven(cur, ped)) { MoveSpeed(cur) = { 0, 0, 0 }; TurnSpeed(cur) = { 0, 0, 0 }; }
        MirrorLocal(0x01D3, 2, a);
        pp.exitedFrom = NetVehicleId(cur);
        pp.exiting = true;
        pp.busySince = now;
        pp.anims.count = 0;
        Log("coop : Tommy %d descend (animation)", s.id);
        return true;
    }
    // Deja dehors chez lui (sortie posee directement, ou animation deja finie) et notre double encore a bord : il
    // descend quand meme avec l'animation du jeu, sauf si la voiture roule (il en a saute).
    if (cur && !s.inVehicle && !s.enterId && !LocallyDriven(cur, ped)) {
        Vec3 cs = MoveSpeed(cur);
        if (cs.x * cs.x + cs.y * cs.y < 0.01f) {
            int32_t a[2] = { (int32_t)PedHandle(ped), (int32_t)VehicleHandle(cur) };
            MoveSpeed(cur) = { 0, 0, 0 };
            TurnSpeed(cur) = { 0, 0, 0 };
            MirrorLocal(0x01D3, 2, a);
            pp.exitedFrom = NetVehicleId(cur);
            pp.exiting = true;
            pp.busySince = now;
            pp.anims.count = 0;
            Log("coop : Tommy %d descend (animation, deja dehors chez lui)", s.id);
            return true;
        }
    }
    // Passager : n'importe quelle place de passager convient (chaque machine range ses passagers a sa facon).
    if (cur && (cur != want || (SeatOf(cur, ped) == 0) != (s.seat == 0))) {
        Vec3 at = { s.pos[0], s.pos[1], s.pos[2] };
        WarpOutOfVehicle(ped, &at);
        pp.lastMoveState = -1;
        pp.anims.count = 0;
        Log("coop : Tommy %d descend du vehicule (pose)", s.id);
        cur = NULL;
    }
    // Notre animation de descente est finie avant la sienne (son etat dit encore "a bord") : on ne le reassoit pas
    // (avant : reassis, puis redescendu une seconde fois quand son etat passait a "dehors").
    if (want && !cur && s.vehicleId == pp.exitedFrom && now - pp.exitedAt < 3000 && !s.enterId) return false;
    if (want && !cur && s.seat == 0) EvictNpcDriver(want, s);
    if (want && !cur && WarpIntoSeat(ped, want, s.seat)) {
        pp.seatedAt = now;
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
    (void)np;
    // Sa demarche : le groupe d'animation de son Tommy (player, player2armed, playercsaw...), pas celui du passant
    // dont le pantin porte le modele.
    if (s.animGroup && Field<int>(ped, 0x1F4) != s.animGroup && AnimAvailable(s.animGroup, 0)) {
        Field<int>(ped, 0x1F4) = s.animGroup;
        Field<int>(ped, 0x250) = -1;   // SetMoveAnim refond la marche avec le nouveau groupe
    }
    // Mort / arrete : il reste couche (animation recue) et ne bloque plus le passage.
    bool down = s.down || s.health <= 0.0f;
    // (Seulement a pied : assis dans un vehicule, le jeu coupe lui-meme la collision du personnage ; la remettre a
    // chaque image faisait un corps solide DANS la voiture de l'hote, qui devenait inconduisible et delogeait le pantin.)
    if (!InVehicle(ped)) {
        uint8_t &col = Field<uint8_t>(ped, 0x51);
        if (down) col &= ~0x01; else col |= 0x01;
    }
    if (UpdatePuppetVehicle(pp, s)) {
        pp.anims.count = 0;
        if (pp.entering || pp.exiting) return;   // le jeu joue la scene : on ne touche a rien
        // Chez lui il est assis : ce que notre jeu lui fait subir (ejecte d'un coup de coude par le motard qui reprend
        // sa moto, tire dehors...) ne compte pas. Si on le retrouve hors de l'etat "conduite" ou sans son animation
        // assise, on le reinstalle a sa place (sinon il etait traine sous la moto, couche).
        void *veh = PedVehicle(ped);
        if (!veh) return;
        // Ses tirs en drive-by (le jeu tire depuis le vehicule quand le tireur est a bord).
        for (int n = 0; pp.lastShots != s.shots && n < 3; n++) { pp.lastShots++; PuppetShoot(ped, s.weapon); }
        pp.lastShots = s.shots;
        if (PedState(ped) != PED_DRIVING || !Field<void *>(ped, 0x1F8)) {
            static uint32_t lastFix;
            if (GetTickCount() - lastFix > 300) {
                lastFix = GetTickCount();
                Vec3 at = Pos(veh);
                WarpOutOfVehicle(ped, &at);
                if (WarpIntoSeat(ped, veh, s.seat)) Log("coop : Tommy %d remis a sa place (le jeu l'en avait deloge)", s.id);
            }
        }
        return;
    }
    // Position et cap : places apres la physique, par interpolation (PuppetsAfterProcess).
    // En vehicule mais sans copie locale (pas encore creee, ou passager) : cache en attendant.
    uint8_t &flags = Field<uint8_t>(ped, 0x52);
    // Cache pendant une cinematique (la sienne ou la notre) : les pantins etaient plantes debout dans la scene.
    bool cutscene = s.cutscene || *(bool *)0xA10AB2;
    if (s.inVehicle || cutscene) flags &= ~0x04; else flags |= 0x04;
    // Visee et tirs : bras leve tant qu'il vise, chaque nouveau tir est rejoue (3 au plus par image).
    if (s.aiming) SetAimFlag(ped, s.heading);
    else if (IsAimingGun(ped)) ClearAimFlag(ped);
    for (int n = 0; pp.lastShots != s.shots && n < 3; n++) { pp.lastShots++; PuppetShoot(ped, s.weapon); }
    pp.lastShots = s.shots;

    // L'IA "au repos" (etat 1, FUN_004FDEB0) remettait le deplacement a "immobile" a chaque image : l'animation de
    // marche redemarrait sans cesse (on voyait l'autre joueur glisser dans une pose figee). Dans l'etat 0 ("aucun"),
    // le jeu ne fait rien : c'est l'etat recu du reseau qui pilote seul l'animation.
    PedState(ped) = 0;
    // Coup de poing, saut, chute... : tant qu'une telle animation est en cours chez lui, on ne relance pas celle de
    // marche (elle la ferait disparaitre). Les reactions que notre jeu lui donne (nos coups) sont effacees : on voit
    // celles qu'il vit chez lui.
    ClearLocalReactions(ped, pp.anims);
    if (ApplyActionAnims(ped, s.anims, 3, pp.anims) || down) { EnsureLiveAnim(ped); return; }
    int before = MoveState(ped);
    SetMoveStateFn(ped, s.moveState ? s.moveState : 1);   // 0 ("aucun") : SetMoveAnim ne poserait rien
    SetMoveAnim(ped);
    EnsureLiveAnim(ped);
    if (s.moveState != pp.lastMoveState) {
        if (g_cfg.logScripts) Log("coop : Tommy %d deplacement %d -> %d (il etait a %d)", s.id, pp.lastMoveState, s.moveState, before);
        pp.lastMoveState = s.moveState;
    }
}

// Chaque etat recu entre dans la piste de son Tommy.
static void OnState(const MsgState &s)
{
    if (s.id >= MAX_PLAYERS || s.id == g_localId) return;
    ClockSample(s.id, s.time);
    // Au menu / en chargement, sa position est (0, 0, 0) : on l'oublie, sinon son Tommy traversait la carte vers
    // ce point en arrivant (cf. plantage chez JD a l'arrivee d'un invite).
    if (!s.inGame) { g_puppets[s.id].track.Clear(); return; }
    Snap n = {};
    n.t = s.time;
    for (int k = 0; k < 3; k++) { n.pos[k] = s.pos[k]; n.vel[k] = s.speed[k]; }
    n.heading = s.heading;
    g_puppets[s.id].track.Push(n);
}

// Apres la physique : les Tommy a pied sont places a leur position interpolee.
void PuppetsAfterProcess()
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Puppet &pp = g_puppets[i];
        if (!pp.ped || InVehicle(pp.ped) || pp.entering || pp.exiting) continue;
        Snap n;
        if (!TrackSample(pp.track, i, n, true)) continue;
        // Grand ecart (arrivee, teleportation) : par CEntity::Teleport, qui remet le personnage dans les bons secteurs.
        float jx = n.pos[0] - Pos(pp.ped).x, jy = n.pos[1] - Pos(pp.ped).y, jz = n.pos[2] - Pos(pp.ped).z;
        if (jx * jx + jy * jy + jz * jz > 400.0f) Teleport(pp.ped, { n.pos[0], n.pos[1], n.pos[2] });
        if (g_cfg.logScripts) {
            // Regularite : pas d'une image a l'autre pendant qu'il se deplace (min / moyenne / max sur 2 s).
            static float lastX, lastY, mn = 1e9f, mx, sum;
            static int cnt;
            static uint32_t since;
            float d = sqrtf((n.pos[0] - lastX) * (n.pos[0] - lastX) + (n.pos[1] - lastY) * (n.pos[1] - lastY));
            lastX = n.pos[0]; lastY = n.pos[1];
            if (d > 0.01f && d < 5.0f) { if (d < mn) mn = d; if (d > mx) mx = d; sum += d; cnt++; }
            if (GetTickCount() - since > 2000) {
                if (cnt > 10) Log("interpolation : Tommy %d, pas par image %.3f / %.3f / %.3f m (%d images)", i, mn, sum / cnt, mx, cnt);
                since = GetTickCount(); mn = 1e9f; mx = sum = 0; cnt = 0;
            }
        }
        Pos(pp.ped) = { n.pos[0], n.pos[1], n.pos[2] };
        MoveSpeed(pp.ped) = { n.vel[0], n.vel[1], n.vel[2] };
        SetHeadingMatrix(pp.ped, n.heading);
        Heading(pp.ped) = HeadingGoal(pp.ped) = n.heading;
    }
}

static void UpdatePuppets(bool inGame)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Puppet &pp = g_puppets[i];
        const NetPlayer &np = g_players[i];
        bool want = inGame && i != g_localId && np.connected && np.state.inGame;
        if (!want) {
            if (pp.ped && inGame) DestroyPuppet(pp);
            if (!inGame) { pp.ped = NULL; pp.blip = -1; }   // le monde (et les marqueurs) ont ete detruits
            if (!np.connected) pp.track.Clear();
            continue;
        }
        // Changement de tenue : on le recree avec la nouvelle.
        if (pp.ped && _stricmp(pp.outfit, np.state.outfit) != 0 && !InVehicle(pp.ped)) {
            Log("coop : %s change de tenue (%s -> %s)", np.state.name, pp.outfit, np.state.outfit);
            DestroyPuppet(pp);
        }
        if (!pp.ped) {   // detruit par le jeu : son point aussi ; pas plus d'une creation par seconde s'il disparait aussitot
            static uint32_t createdAt[MAX_PLAYERS];
            uint32_t now = GetTickCount();
            if (now - createdAt[i] < 1000) continue;
            createdAt[i] = now;
            RemovePlayerBlip(pp.blip);
            CreatePuppet(pp, np.state);
        }
        if (pp.ped) UpdatePuppet(pp, np);
    }
}

// Invite : le fondu de l'ecran suit celui de l'hote (les fondus de mission, et ceux que son script principal fait
// hors mission). Notre propre traitement du fondu est coupe pour ne pas lutter.
static uint32_t g_localDownUntil;   // invite : mort / arrete (le fondu de l'hote n'est pas impose pendant ce temps)

static void FollowHostFade(bool inGame)
{
    if (g_cfg.host || !inGame || g_localId <= 0) return;
    const NetPlayer &h = g_players[0];
    if (!h.connected || !h.state.inGame || GetTickCount() < g_localDownUntil) return;
    // Hote mort ou arrete : son ecran noir n'est pas le notre.
    static uint32_t hostDownUntil;
    if (h.state.down) hostDownUntil = GetTickCount() + 1500;
    if (GetTickCount() < hostDownUntil) return;
    float f = h.state.fade;
    if (f < 0.0f) f = 0.0f;
    if (f > 255.0f) f = 255.0f;
    CamFade() = f;
    CamFading() = false;
    DrawFadeValue() = (uint8_t)(f + 0.5f);
}

static void *g_boarding, *g_leaving;
static uint32_t g_boardingSince, g_leavingSince;
static int g_boardingSeat;

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
    s.time = now;
    s.modsPct = (uint8_t)ModsPercent();
    for (AnimSlot &a : s.anims) a.id = -1;
    lstrcpynA(s.name, g_cfg.playerName, sizeof(s.name));
    void *ped = inGame ? FindPlayerPed() : NULL;
    lstrcpynA(s.outfit, ped ? PedOutfit(ped) : ModelName(MI_PLAYER), sizeof(s.outfit));
    if (ped) {
        s.inGame = 1;
        Vec3 p = Pos(ped), v = MoveSpeed(ped);
        s.pos[0] = p.x; s.pos[1] = p.y; s.pos[2] = p.z;
        s.speed[0] = v.x; s.speed[1] = v.y; s.speed[2] = v.z;
        s.heading = Heading(ped);
        s.health = Health(ped);
        s.armour = Armour(ped);
        // La zone ou l'on est, c'est CGame::currArea (celle dont le jeu affiche les batiments), pas le code du
        // Tommy : apres une cinematique, le script remet dehors avec SET_AREA_VISIBLE 0 sans toucher au personnage,
        // qui gardait "hotel" ; un invite pose a cote de l'hote au retour prenait cette zone et perdait la ville.
        s.area = (uint8_t)*(int *)0x978810;
        s.moveState = (uint8_t)MoveState(ped);
        s.pedState = (uint8_t)PedState(ped);
        s.fade = CamFade();
        s.animGroup = (uint8_t)Field<int>(ped, 0x1F4);
        s.cutscene = *(bool *)0xA10AB2 ? 1 : 0;
        {
            int wb = *(int *)(0x94AD28 + 0xCC);   // CWorld::Players[0].m_nPlayerState : 1 mort, 2 arrete
            s.down = (Health(ped) <= 0.0f || PedState(ped) == 54 || PedState(ped) == 55 || wb == 1 || wb == 2) ? 1 : 0;
        }
        s.weapon = (uint8_t)WeaponTypeInSlot(ped, CurrentWeaponSlot(ped));
        s.shots = LocalShotCount();
        // Montee / descente en cours (animation du jeu) : les autres la jouent sur notre double.
        int st = PedState(ped);
        if (!InVehicle(ped) && PedVehicle(ped) && EnteringState(st)) {
            s.enterId = NetVehicleId(PedVehicle(ped));
            s.enterSeat = (g_boarding == PedVehicle(ped) && g_boardingSeat) || (VehDriver(PedVehicle(ped)) && VehDriver(PedVehicle(ped)) != ped) ? 1 : 0;
        }
        s.exiting = InVehicle(ped) && ExitingState(st);
        s.aiming = IsAimingGun(ped) ? 1 : 0;
        s.inVehicle = InVehicle(ped) ? 1 : 0;
        s.shared = PopulationShared() ? 1 : 0;
        s.ping = g_cfg.host ? 0 : g_myPing;
        s.wanted = (uint8_t)WantedLevel(ped);
        if (!s.inVehicle) CollectAnimSlots(ped, s.anims, 3);   // aussi en visee : rechargement, accroupi, coup recu
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
                   CamFade(), (uint8_t)CamFading(), (uint8_t)CamWidescreen(), (uint8_t)g_cfg.friendlyFire };
    NetSendToGuests(&w, sizeof(w));
}

static void OnWorld(const MsgWorld &w)
{
    if (GameState() != GS_PLAYING) return;
    g_hostPlayerHandle = w.playerHandle;
    g_cfg.friendlyFire = w.friendlyFire != 0;   // c'est le reglage de l'hote qui compte
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
    // Mort : il reapparait a l'hopital le plus proche, comme en solo (ReapparitionHote=1 : pres de l'hote). (PED_DEAD = 55, verifie dans
    // CPed::SetDead ; 54 = PED_DIE. Surtout pas 56 et suivants : etats de vol de voiture, l'invite etait teleporte
    // en pleine action.)
    static bool wasDown;
    static int savedWeapons[10], savedAmmo[10];
    if (inGame && !g_cfg.host) {
        void *me = FindPlayerPed();
        uint8_t *info = (uint8_t *)0x94AD28;   // CWorld::Players[0] ; m_nPlayerState +0xCC (1 mort, 2 arrete)
        int wb = *(int *)(info + 0xCC);
        bool down = me && (Health(me) <= 0.0f || PedState(me) == 54 || PedState(me) == 55 || wb == 1 || wb == 2);
        if (down) g_localDownUntil = GetTickCount() + 4000;
        if (down && !wasDown) {
            Log("coop : %s", wb == 2 ? "arrete" : "mort");
            // Garder armes et argent : les "sortie gratuite" de l'hopital et de la prison (m_bGetOutOfJailFree +0x145,
            // m_bGetOutOfHospitalFree +0x146), comme le fait le jeu apres certaines missions.
            if (g_cfg.keepWeapons) {
                info[0x145] = 1; info[0x146] = 1;
                for (int s = 0; s < 10; s++) {   // et au cas ou : ses armes, rendues a la reapparition
                    savedWeapons[s] = WeaponTypeInSlot(me, s);
                    savedAmmo[s] = Field<int>(me, 0x408 + s * 0x18 + 0xC);
                }
            }
            // Reapparaitre pres de l'hote plutot qu'a l'hopital / au commissariat (OVERRIDE_NEXT_RESTART). Desactive par
            // defaut (choix de JD : trop facile en mission ; il revient par ses propres moyens).
            const NetPlayer &h = g_players[0];
            if (g_cfg.respawnAtHost && h.connected && h.state.inGame && !h.state.inVehicle) {
                int32_t r[4];
                float v[4] = { h.state.pos[0] + 2.0f, h.state.pos[1], h.state.pos[2], h.state.heading * 57.2958f };
                memcpy(r, v, sizeof(r));
                MirrorLocal(0x016E, 4, r);
            }
        }
        if (wasDown && !down) {
            if (g_cfg.respawnAtHost) gathered = false;   // sinon il reste a l'hopital / au commissariat
            Log("coop : de retour apres la mort / l'arrestation");
            if (g_cfg.keepWeapons) {
                bool missing = false;
                for (int s = 0; s < 10; s++)
                    if (savedWeapons[s] > 0 && WeaponTypeInSlot(me, s) != savedWeapons[s] && savedAmmo[s] > 0) {
                        int model = *(int *)(0x782A14 + savedWeapons[s] * 0x64 + 0x54);   // CWeaponInfo : modele
                        if (model > 0 && !HasModelLoaded(model)) RequestModel(model, 1);
                        missing = true;
                    }
                if (missing) ((void(__cdecl *)(bool))0x40B5F0)(false);   // CStreaming::LoadAllRequestedModels
                for (int s = 0; s < 10; s++)
                    if (missing && savedWeapons[s] > 0 && WeaponTypeInSlot(me, s) != savedWeapons[s] && savedAmmo[s] > 0) {
                        GiveWeapon(me, savedWeapons[s], savedAmmo[s]);
                        Log("coop : arme %d rendue (%d balles)", savedWeapons[s], savedAmmo[s]);
                    }
            }
            memset(savedWeapons, 0, sizeof(savedWeapons));
        }
        wasDown = down;
    }
    if (!inGame) { gathered = false; return; }
    if (g_cfg.host || gathered || g_localId <= 0) return;
    if (GuestSideMission()) { gathered = true; return; }   // en pleine course de taxi : on ne le deplace pas
    const NetPlayer &h = g_players[0];
    if (!h.connected || !h.state.inGame) return;
    void *ped = FindPlayerPed();
    float hh = h.state.heading;
    if (InVehicle(ped)) {
        // En vehicule : la demande restait en attente sans limite et le teleportait des qu'il descendait, des minutes
        // plus tard, n'importe ou. Au volant et pas trop loin : la voiture vient avec lui, 8 m derriere l'hote ;
        // sinon on renonce.
        void *veh = PedVehicle(ped);
        float dx = h.state.pos[0] - Pos(ped).x, dy = h.state.pos[1] - Pos(ped).y;
        gathered = true;
        if (veh && VehDriver(veh) == ped && dx * dx + dy * dy < 150.0f * 150.0f && !h.state.inVehicle) {
            Pos(veh) = { h.state.pos[0] + sinf(hh) * 8.0f, h.state.pos[1] - cosf(hh) * 8.0f, h.state.pos[2] + 0.5f };
            MoveSpeed(veh) = { 0, 0, 0 };
            TurnSpeed(veh) = { 0, 0, 0 };
            SetHeadingMatrix(veh, hh);
            AreaCode(ped) = h.state.area;
            MirrorFollowHostArea(h.state.area);
            Log("coop : pose en voiture derriere l'hote");
        } else Log("coop : regroupement abandonne (en vehicule)");
        return;
    }
    // Derriere l'hote (d'ou il vient, donc un endroit libre), decale d'un pas par joueur.
    float back = 1.5f + 0.8f * (g_localId - 1), side = (g_localId % 2 ? 0.6f : -0.6f);
    Pos(ped) = { h.state.pos[0] + sinf(hh) * back + cosf(hh) * side, h.state.pos[1] - cosf(hh) * back + sinf(hh) * side,
                 h.state.pos[2] + 0.3f };
    MoveSpeed(ped) = { 0, 0, 0 };
    SetHeadingMatrix(ped, hh);   // regarde dans la meme direction que l'hote
    Heading(ped) = HeadingGoal(ped) = hh;
    AreaCode(ped) = h.state.area;
    MirrorFollowHostArea(h.state.area);   // pose dans l'interieur ou il est (ou dehors)
    MirrorLocal(0x0373, 0, NULL);   // SET_CAMERA_BEHIND_PLAYER
    gathered = true;
    Log("coop : pose a cote de l'hote (%.1f %.1f %.1f)", Pos(ped).x, Pos(ped).y, Pos(ped).z);
}

// --- Recherche de la police partagee (RecherchePartagee=1) ---
// L'hote prend le plus haut niveau des joueurs (un crime d'un invite attire aussi sa police) ; les invites prennent
// celui de l'hote a chaque fois qu'il change (hausse, ou police semee : tout le monde retombe a zero).
int WantedLevel(void *ped) { void *w = Field<void *>(ped, 0x5F4); return w ? Field<int>(w, 0x20) : 0; }
static void SetWantedLevel(void *ped, int level)
{
    void *w = Field<void *>(ped, 0x5F4);
    if (w) ((void(__thiscall *)(void *, int))0x4D1FA0)(w, level);
}

static void ShareWanted(bool inGame)
{
    static int lastHost = -1;
    if (!inGame || !g_cfg.shareWanted) { lastHost = -1; return; }
    void *me = FindPlayerPed();
    if (g_cfg.host) {
        // Quand la recherche de l'hote baisse (police semee), les invites l'apprennent un peu apres : pendant 2 s on
        // ignore leurs anciens niveaux, sinon on remontait aussitot.
        static int lastMine;
        static uint32_t quietUntil;
        int mine = WantedLevel(me), best = mine;
        if (mine < lastMine) quietUntil = GetTickCount() + 2000;
        lastMine = mine;
        if (GetTickCount() < quietUntil) return;
        for (int i = 1; i < MAX_PLAYERS; i++)
            if (g_players[i].connected && g_players[i].state.inGame && g_players[i].state.wanted > best) best = g_players[i].state.wanted;
        if (best > mine) { SetWantedLevel(me, best); Log("police : recherche %d (crime d'un invite)", best); }
        return;
    }
    const NetPlayer &h = g_players[0];
    if (g_localId <= 0 || !h.connected || !h.state.inGame) return;
    if (h.state.wanted != lastHost) {
        lastHost = h.state.wanted;
        if (WantedLevel(me) != lastHost) { SetWantedLevel(me, lastHost); Log("police : recherche %d (comme l'hote)", lastHost); }
    }
}

// Hote : un personnage de l'IA qui veut prendre le volant d'une voiture conduite par un autre joueur (son Tommy
// chez nous) y renonce ; sinon il s'asseyait "par-dessus" lui. (Objectif +0x164 : 0x12 = monter au volant ;
// vehicule vise +0x170.) Pour les places passager, le jeu le fait deja attendre devant la portiere si c'est plein.
static void KeepAIOffPlayerCars()
{
    static uint32_t last;
    if (!g_cfg.host || GetTickCount() - last < 250) return;
    last = GetTickCount();
    Pool *pp = PedPool();
    void *me = FindPlayerPed();
    for (int i = 0; i < pp->size; i++) {
        if (pp->flags[i] & 0x80) continue;
        void *ped = pp->objects + i * PED_POOL_ENTRY;
        if (ped == me || IsPuppet(ped) || Field<int>(ped, 0x164) != 0x12) continue;
        void *car = Field<void *>(ped, 0x170);
        if (!car || !VehDriver(car) || !IsPuppet(VehDriver(car))) continue;
        ((void(__thiscall *)(void *))0x521720)(ped);   // CPed::ClearObjective
        Log("coop : un personnage renonce a la voiture conduite par un joueur");
    }
}

static bool OtherPlayersConnected()
{
    for (int i = 0; i < MAX_PLAYERS; i++) if (i != g_localId && g_players[i].connected) return true;
    return false;
}

// Un autre joueur est-il a bord (au volant ou passager) ?
static bool PlayerAboard(void *v)
{
    if (VehDriver(v) && IsPuppet(VehDriver(v))) return true;
    for (int i = 0; i < 8; i++) if (VehPassenger(v, i) && IsPuppet(VehPassenger(v, i))) return true;
    return false;
}

// Touche F (ou G) : pres d'un vehicule ou se trouve un autre joueur (a moins de 6 m), on s'installe a la premiere
// place libre (le volant s'il est libre, sinon passager) au lieu de le lui voler ; passager, on en descend.
// VC ne sait pas faire monter le joueur en passager : on l'installe directement. Vrai si on a fait quelque chose.
// Montee / descente en passager avec l'animation du jeu : on donne a notre propre Tommy l'objectif des scripts
// (SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER 01D4 / LEAVE_CAR 01D3). S'il ne bouge pas (le joueur ne suit pas toujours les
// objectifs) ou que ca traine, on le pose directement comme avant.

static void BoardingFrame()
{
    void *me = FindPlayerPed();
    uint32_t now = GetTickCount();
    if (g_boarding && me) {
        bool in = InVehicle(me) && PedVehicle(me) == g_boarding;
        bool moving = EnteringState(PedState(me));
        if (in) { Log("coop : a bord (animation, %u ms)", now - g_boardingSince); g_boarding = NULL; }
        else if (now - g_boardingSince > 4000 || (now - g_boardingSince > 700 && !moving)) {
            ((void(__thiscall *)(void *))0x521720)(me);   // CPed::ClearObjective
            if (WarpIntoSeat(me, g_boarding, g_boardingSeat)) Log("coop : je monte a bord (pose directement, place %d)", SeatOf(g_boarding, me));
            else Log("coop : plus de place dans ce vehicule");
            g_boarding = NULL;
        }
    }
    if (g_leaving && me) {
        static uint32_t lastAsk;
        if (!InVehicle(me)) { Log("coop : descendu (animation, %u ms)", now - g_leavingSince); g_leaving = NULL; }
        else if (ExitingState(PedState(me))) { if (now - g_leavingSince > 15000) { g_leaving = NULL; Log("coop : descente trop longue, on laisse faire"); } }
        else {
            // CPed::SetExitCar refuse tant que la voiture bouge (CanPedExitCar 0x5B8180 : retournee et qui tangue, sur
            // le flanc...) : on attend qu'elle se pose en redonnant l'objectif, au lieu de poser le joueur dehors
            // sans animation (ce qui laissait son double sans animation chez les autres : plantage 0x403ED2).
            bool calm = ((bool(__thiscall *)(void *, char))0x5B8180)(g_leaving, 0);
            if ((calm && now - g_leavingSince > 1500) || now - g_leavingSince > 10000) {
                ((void(__thiscall *)(void *))0x521720)(me);
                WarpOutOfVehicle(me, NULL);
                Log("coop : je descends (pose directement, vehicule %s)", calm ? "au repos" : "encore en mouvement");
                g_leaving = NULL;
            } else if (now - lastAsk > 1000) {
                lastAsk = now;
                int32_t a[2] = { (int32_t)PedHandle(me), (int32_t)VehicleHandle(g_leaving) };
                MirrorLocal(0x01D3, 2, a);
                if (!calm) Log("coop : le vehicule bouge encore, descente en attente");
            }
        }
    }
}

bool TogglePassenger()
{
    void *me = FindPlayerPed();
    if (!me) return false;
    if (g_boarding || g_leaving) return true;   // deja en cours
    if (InVehicle(me)) {
        void *veh = PedVehicle(me);
        if (veh && SeatOf(veh, me) > 0) {
            int32_t a[2] = { (int32_t)PedHandle(me), (int32_t)VehicleHandle(veh) };
            MirrorLocal(0x01D3, 2, a);
            g_leaving = veh;
            g_leavingSince = GetTickCount();
            Log("coop : je descends (passager, animation)");
            return true;
        }
        return false;
    }
    Pool *pool = VehiclePool();
    void *best = NULL;
    float bestD = 36.0f;
    for (int i = 0; i < pool->size; i++) {
        if (pool->flags[i] & 0x80) continue;
        void *v = pool->objects + i * VEHICLE_POOL_ENTRY;
        if (!PlayerAboard(v) || EntityStatus(v) == STATUS_WRECKED) continue;
        float dx = Pos(v).x - Pos(me).x, dy = Pos(v).y - Pos(me).y, dz = Pos(v).z - Pos(me).z;
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) { bestD = d; best = v; }
    }
    if (!best) return false;
    int seat = VehDriver(best) ? 1 : 0;
    if (seat && Field<uint8_t>(best, 0x1CC) >= Field<uint8_t>(best, 0x1D0)) { Log("coop : plus de place dans ce vehicule"); return true; }
    int32_t a[2] = { (int32_t)PedHandle(me), (int32_t)VehicleHandle(best) };
    MirrorLocal(seat ? 0x01D4 : 0x01D5, 2, a);
    g_boarding = best;
    RegisterReference(best, &g_boarding);
    g_boardingSince = GetTickCount();
    g_boardingSeat = seat;
    Log("coop : je monte a bord (animation, place %d)", seat);
    return true;   // meme plein : on ne le vole pas a l'autre joueur
}

// CPad::ExitVehicleJustDown (0x4AA870) et GetExitVehicle (0x4AA8F0), appeles par CPlayerInfo::Process pour monter
// ou descendre : si la touche sert a monter a bord du vehicule d'un autre joueur, le jeu ne la voit pas (sinon il
// tirait le conducteur dehors ou nous asseyait dans la copie, et les deux parties se desynchronisaient).
typedef bool(__fastcall *PadBool_t)(void *pad, void *edx);
static PadBool_t o_ExitJustDown, o_GetExit;
static uint32_t g_enterHandledFrame = 0xFFFFFFFF;

static bool EnterExitHandled(void *pad, bool pressed, bool justDown)
{
    if (!pressed || pad != (void *)0x7DBCB0 || GameState() != GS_PLAYING || !FindPlayerPed() || !OtherPlayersConnected()) return false;
    uint32_t f = FrameCounter();
    if (f - g_enterHandledFrame < 15) return true;   // la meme pression, encore vue les images suivantes
    if (!justDown) {
        // Touche maintenue. Passager : le jeu n'a rien prevu (il n'appellera pas ExitVehicleJustDown), on descend
        // nous-memes a l'appui. A pied : on laisse le jeu demander ExitVehicleJustDown.
        static uint32_t lastHeld = 0xFFFFFFF0;
        bool rising = f != lastHeld && f - lastHeld > 1;
        if (f != lastHeld) lastHeld = f;
        void *me = FindPlayerPed();
        if (!InVehicle(me) || !PedVehicle(me) || SeatOf(PedVehicle(me), me) <= 0) return false;
        if (rising && TogglePassenger()) g_enterHandledFrame = f;
        return true;
    }
    if (!TogglePassenger()) return false;
    g_enterHandledFrame = f;
    return true;
}

static bool __fastcall h_ExitJustDown(void *pad, void *edx)
{
    bool r = o_ExitJustDown(pad, edx);
    return EnterExitHandled(pad, r, true) ? false : r;
}

static bool __fastcall h_GetExit(void *pad, void *edx)
{
    bool r = o_GetExit(pad, edx);
    return EnterExitHandled(pad, r, false) ? false : r;
}

void InstallEnterHooks()
{
    static const uint8_t justDown[] = { 0x53, 0x89, 0xCB, 0x66, 0x83, 0xBB, 0xF0, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t held[] = { 0x66, 0x83, 0xB9, 0xF0, 0x00, 0x00, 0x00, 0x00 };
    o_ExitJustDown = (PadBool_t)MakeDetour(0x4AA870, justDown, sizeof(justDown), (void *)h_ExitJustDown);
    o_GetExit = (PadBool_t)MakeDetour(0x4AA8F0, held, sizeof(held), (void *)h_GetExit);
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
        netStarted = true; g_onWorld = OnWorld; g_onState = OnState; VehiclesInit(); EntitiesInit(); MirrorInit();
        if (g_cfg.netAuto) CoopStartNetwork();
    }
    InterfaceFrame();

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
    ModsFrame();
    SaveShareFrame();
    AutotestFrame();
    NetPoll();
    // En coop, le menu Pause n'arrete pas le monde (sinon toute la partie des invites se fige derriere le menu
    // de l'hote) : on leve la pause joueur tant qu'un autre joueur est connecte.
    if (GameState() == GS_PLAYING && MenuActive() && UserPause() && OtherPlayersConnected()) UserPause() = false;
    bool inGame = GameState() == GS_PLAYING && FindPlayerPed() != NULL;
    GatherToHost(inGame);
    PopulationFrame(inGame);
    ConditionsFrame(inGame);
    if (inGame) { PassengerKey(); BoardingFrame(); }
    MouseFocusFrame();
    PlayersFrame(inGame);
    ShareWanted(inGame);
    if (inGame) KeepAIOffPlayerCars();
    if (inGame) { CameraFrame(); PassengerShooting(); }
    VehiclesFrame(inGame);
    EntitiesFrame(inGame);
    MirrorFrame(inGame);
    OverlayFrame(inGame);
    SendLocalState(inGame);
    FollowHostFade(inGame);
    if (g_cfg.host && inGame) SendWorld();
    UpdatePuppets(inGame);

    if (inGame && g_cfg.logScripts) {
        // Meme mesure que l'interpolation, sur notre propre Tommy (pour comparer avec ce que voient les autres).
        static float lastX, lastY, mn = 1e9f, mx, sum;
        static int cnt;
        static uint32_t since;
        void *me = FindPlayerPed();
        float d = sqrtf((Pos(me).x - lastX) * (Pos(me).x - lastX) + (Pos(me).y - lastY) * (Pos(me).y - lastY));
        lastX = Pos(me).x; lastY = Pos(me).y;
        if (d > 0.01f && d < 5.0f) { if (d < mn) mn = d; if (d > mx) mx = d; sum += d; cnt++; }
        if (GetTickCount() - since > 2000) {
            if (cnt > 10) Log("deplacement local : pas par image %.3f / %.3f / %.3f m (%d images)", mn, sum / cnt, mx, cnt);
            since = GetTickCount(); mn = 1e9f; mx = sum = 0; cnt = 0;
        }
    }
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
        if (ped && g_cfg.logScripts) {
            char line[512];
            int n = wsprintfA(line, "  animations :");
            for (void *as = FirstAssoc(Field<void *>(ped, 0x4C)); as && n < 440; as = NextAssoc(as))
                n += wsprintfA(line + n, " %d/%d/%d(%d%%)", Field<int16_t>(as, 0x2C), Field<int16_t>(as, 0xC),
                               Field<int16_t>(as, 0xE), (int)(Field<float>(as, 0x18) * 100));
            Log("%s", line);
        }
        for (int i = 0; i < MAX_PLAYERS; i++)
            if (void *pp = g_puppets[i].ped) {
                char anims[256];
                int n = 0;
                anims[0] = 0;
                for (void *as = FirstAssoc(Field<void *>(pp, 0x4C)); as && n < 200; as = NextAssoc(as))
                    n += wsprintfA(anims + n, " %d(%d%%)", Field<int16_t>(as, 0x2C), (int)(Field<float>(as, 0x18) * 100));
                Log("  Tommy %d : %.1f %.1f %.1f, etat %d, vehicule %d, animations%s", i, Pos(pp).x, Pos(pp).y, Pos(pp).z,
                    PedState(pp), InVehicle(pp), anims);
            }
    }
}
