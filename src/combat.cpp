// Combat : armes en main des Tommy distants et des copies, et degats qui traversent le reseau.
//  - Invite : les degats qu'il inflige a la copie d'un personnage de mission partent chez l'hote (rien en local).
//  - Hote : les degats que recoit le Tommy d'un invite (IA, explosions...) partent chez cet invite, qui les
//    subit sur son vrai Tommy. Pas de tir ami entre joueurs pour l'instant.
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "entities.h"
#include "combat.h"
#include "mirror.h"
#include "saveshare.h"
#include "camera.h"
#include <string.h>

using namespace game;

enum { RL_DAMAGE_PED = 10, RL_DAMAGE_PLAYER = 11, RL_DAMAGE_PVP = 12, RL_FIGHT_REACT = 13, RL_FIGHT_REACT_PED = 14,
       RL_PROJECTILE = 15 };

#pragma pack(push, 1)
struct RlDamagePed { uint8_t type, attacker, dir; uint32_t hostHandle; int32_t weapon, piece; float damage; uint8_t owner; };
struct RlDamagePlayer { uint8_t type, dir; uint32_t npcHandle; int32_t weapon, piece; float damage; };
// Un joueur en touche un autre (tir ami) : part chez la victime (par l'hote si besoin), qui subit le coup chez elle.
struct RlDamagePvp { uint8_t type, attacker, victim, dir; int32_t weapon, piece; float damage; };
// Reaction a un coup de poing / pied d'un joueur : parade (CPed::StartFightDefend) ou chute (CPed::SetFall), rejouee
// chez la victime. kind 0 : defend(a, b, c) ; kind 1 : fall(p0, p1, a).
struct RlFightReact { uint8_t type, attacker, victim, kind, a, b, c; int32_t p0, p1; };
// Meme chose quand un invite frappe la copie d'un personnage de l'hote : la reaction est jouee chez l'hote.
struct RlFightReactPed { uint8_t type, attacker, kind, a, b, c; uint32_t hostHandle; int32_t p0, p1; uint8_t owner; };
// Grenade, cocktail Molotov, roquette... lances par un joueur : rejoues depuis son Tommy chez les autres, avec la
// position et la vitesse exactes du projectile chez lui.
struct RlProjectile { uint8_t type, player; int16_t weapon; float power, pos[3], speed[3]; };
#pragma pack(pop)

static void SendToOwner(uint8_t owner, const void *d, int len);   // plus bas
int WantedLevel(void *ped);   // coop.cpp

typedef bool(__fastcall *InflictDamage_t)(void *ped, void *edx, void *damager, int weapon, float damage, int piece, uint8_t dir);
static InflictDamage_t o_InflictDamage;   // trampoline : FLD d'origine (6 octets) puis jmp 0x525B26
static bool g_bypass;

// --- Tirs : on compte ceux du joueur local ; ceux des autres sont rejoues en visuel sur leur Tommy ---
typedef bool(__fastcall *Fire_t)(void *weapon, void *edx, void *shooter, void *source);
static Fire_t o_Fire;
static uint8_t g_localShots;
static bool g_cosmetic;

// Tirs des personnages (policiers d'un invite recherche, personnages de mission) : comptes par reference de pool,
// pour que leurs copies chez les autres tirent aussi (et que leurs balles y fassent mal).
static struct { uint32_t handle; uint8_t count; } g_pedShots[64];
static int g_pedShotsNext;

uint8_t PedShotCount(void *ped)
{
    uint32_t h = PedHandle(ped);
    for (auto &e : g_pedShots) if (e.handle == h) return e.count;
    return 0;
}

static void NoteShot(void *ped)
{
    uint32_t h = PedHandle(ped);
    for (auto &e : g_pedShots) if (e.handle == h) { e.count++; return; }
    g_pedShots[g_pedShotsNext] = { h, 1 };
    g_pedShotsNext = (g_pedShotsNext + 1) % 64;
}

static bool __fastcall h_Fire(void *weapon, void *edx, void *shooter, void *source)
{
    bool r = o_Fire(weapon, edx, shooter, source);
    if (r && !g_cosmetic && shooter) {
        if (shooter == FindPlayerPed()) g_localShots++;
        else if (IsPedEntity(shooter) && !IsPuppet(shooter) && !IsGhostPed(shooter)) NoteShot(shooter);
    }
    return r;
}

uint8_t LocalShotCount() { return g_localShots; }

// Armes a balles seulement (colt 17 .. fusil laser 29, M60 32, minigun 33) : grenades, roquettes, lance-flammes
// exploseraient localement et divergeraient d'une machine a l'autre.
static bool CosmeticWeapon(int w) { return (w >= 17 && w <= 29) || w == 31 || w == 32 || w == 33; }   // 31 : lance-flammes

void PuppetShoot(void *ped, int weapon)
{
    if (!CosmeticWeapon(weapon)) return;
    uint8_t *wpn = (uint8_t *)ped + 0x408 + CurrentWeaponSlot(ped) * 0x18;
    if (*(int *)wpn != weapon) return;
    *(int *)(wpn + 4) = 0;        // pret a tirer
    *(int *)(wpn + 8) = 50;       // balles dans le chargeur
    *(int *)(wpn + 0xC) = 9999;   // reserve
    g_cosmetic = true;
    bool ok = o_Fire(wpn, NULL, ped, NULL);
    g_cosmetic = false;
    if (g_cfg.logScripts) { static int n; if (n++ < 20) Log("combat : tir visuel du Tommy distant (arme %d) -> %d", weapon, ok); }
}

bool ApplyDamage(void *ped, void *damager, int weapon, float damage, int piece, uint8_t dir)
{
    g_bypass = true;
    bool r = o_InflictDamage(ped, NULL, damager, weapon, damage, piece, dir);
    g_bypass = false;
    return r;
}

static bool __fastcall h_InflictDamage(void *ped, void *edx, void *damager, int weapon, float damage, int piece, uint8_t dir)
{
    if (g_bypass || GameState() != GS_PLAYING) return o_InflictDamage(ped, edx, damager, weapon, damage, piece, dir);
    void *me = FindPlayerPed();
    // Explosions (arme 41) : le jeu donne comme auteur le lanceur du projectile. Les projectiles des joueurs sont
    // rejoues sur chaque machine (RL_PROJECTILE), chacune subit donc l'explosion chez elle : celle d'un autre joueur
    // nous blesse ici si le tir ami est permis, blesse nos personnages, et rien ne part par le reseau (sinon double).
    bool explosion = weapon == 41;
    bool byPuppet = damager && (IsPuppet(damager) || (!IsPedEntity(damager) && IsPuppetVehicle(damager)));
    if (byPuppet && explosion) {
        if (ped == me) return g_cfg.friendlyFire ? o_InflictDamage(ped, edx, damager, weapon, damage, piece, dir) : false;
        if (PuppetPlayer(ped) < 0 && !IsGhostPed(ped)) return o_InflictDamage(ped, edx, damager, weapon, damage, piece, dir);
        return false;
    }
    // Les coups portes par le Tommy d'un autre joueur sont decides sur sa machine et arrivent par le reseau :
    // ici ils ne font rien (evite les doubles degats et le tir ami involontaire).
    if (byPuppet) return false;
    // Balles d'une copie de personnage (ses tirs sont rejoues ici) : celles des personnages de l'hote nous arrivent
    // deja par le reseau (RL_DAMAGE_PLAYER), on ne les compte pas deux fois ; celles de la police d'un autre invite
    // ne viennent que d'ici, elles comptent.
    if (ped == me && damager && IsPedEntity(damager) && IsGhostPed(damager)) {
        uint8_t owner;
        uint32_t handle;
        if (GhostOwner(damager, owner, handle) && owner == 0 && !g_cfg.host) return false;
    }
    // Nous touchons le Tommy d'un autre joueur (coup, balle, voiture) : rien ici, le coup part chez lui.
    int victim = PuppetPlayer(ped);
    if (victim >= 0 && damager && (damager == me || damager == PedVehicle(me))) {
        if (g_cfg.friendlyFire && !explosion) {
            RlDamagePvp d = { RL_DAMAGE_PVP, (uint8_t)g_localId, (uint8_t)victim, dir, weapon, piece, damage };
            if (g_cfg.host) NetSendReliableTo(victim, &d, sizeof(d));
            else NetSendReliable(&d, sizeof(d));
            if (g_cfg.logScripts) Log("combat : je touche le joueur %d (%.0f, arme %d)", victim, damage, weapon);
        }
        return false;
    }
    // Copie du personnage d'un autre joueur (passant de l'hote, policier d'un invite) : le coup compte chez lui.
    {
        uint8_t owner;
        uint32_t handle;
        if (GhostOwner(ped, owner, handle)) {
            if (!explosion && (damager == me || (damager && damager == PedVehicle(me)))) {
                RlDamagePed d = { RL_DAMAGE_PED, (uint8_t)g_localId, dir, handle, weapon, piece, damage, owner };
                SendToOwner(owner, &d, sizeof(d));
            }
            return false;
        }
    }
    if (!g_cfg.host) {
        if (IsPuppet(ped)) return false;
    } else {
        int pid = PuppetPlayer(ped);
        if (pid > 0) {
            bool friendly = damager && (damager == me || IsPuppet(damager) || damager == PedVehicle(me));
            // Seuls les coups d'un personnage (balles, poings, explosions de l'IA) partent chez lui. Chutes, noyade,
            // chocs de vehicules : son propre jeu les calcule ; renvoyer ceux de son Tommy chez nous le tuait a son
            // arrivee (chute de 565 points en le deplacant pres de l'hote).
            if (!damager || !IsPedEntity(damager)) return false;
            if (IsGhostPed(damager)) return false;   // police d'un invite (copie) : ses vrais policiers le touchent deja chez lui
            if (!friendly) {
                uint32_t npc = damager && IsPedEntity(damager) ? PedHandle(damager) : 0xFFFFFFFF;
                RlDamagePlayer d = { RL_DAMAGE_PLAYER, dir, npc, weapon, piece, damage };
                NetSendReliableTo(pid, &d, sizeof(d));
            }
            return false;
        }
    }
    return o_InflictDamage(ped, edx, damager, weapon, damage, piece, dir);
}

// --- Corps a corps entre joueurs ---
// CPed::FightHitPed (attaquant, victime...) appelle, sur la victime, StartFightDefend (se proteger, encaisser) puis
// InflictDamage, et parfois SetFall (a terre). Sur le Tommy d'un autre joueur, rien de cela ne doit se passer chez
// nous : c'est envoye chez lui, ou il le vit vraiment (et nous le renvoie par ses animations).
typedef void(__fastcall *FightHitPed_t)(void *ped, void *edx, void *victim, void *a, void *b, int piece);
typedef void(__fastcall *FightDefend_t)(void *ped, void *edx, int dir, int level, int unk);
typedef void(__fastcall *SetFall_t)(void *ped, void *edx, int timeout, int anim, int unk);
static FightHitPed_t o_FightHitPed;
static FightDefend_t o_FightDefend;
static SetFall_t o_SetFall;
static void *g_pvpVictim;   // Tommy distant en train d'etre frappe (pendant FightHitPed) par nous ou, chez l'hote, par l'IA
static bool g_pvpByPlayer;
static void *g_ghostVictim;   // invite : copie d'un personnage de l'hote que nous frappons

// Vers le proprietaire d'une copie : l'hote l'envoie directement, un invite passe par l'hote (qui fait suivre).
static void SendToOwner(uint8_t owner, const void *d, int len)
{
    if (g_cfg.host) NetSendReliableTo(owner, d, len); else NetSendReliable(d, len);
}

static void SendGhostReact(void *ghost, uint8_t kind, int a, int b, int c, int p0, int p1)
{
    uint8_t owner;
    uint32_t handle;
    if (!GhostOwner(ghost, owner, handle)) return;
    RlFightReactPed r = { RL_FIGHT_REACT_PED, (uint8_t)g_localId, kind, (uint8_t)a, (uint8_t)b, (uint8_t)c, handle, p0, p1, owner };
    SendToOwner(owner, &r, sizeof(r));
}

static void SendFightReact(void *puppet, uint8_t kind, int a, int b, int c, int p0, int p1)
{
    int victim = PuppetPlayer(puppet);
    if (victim < 0 || (g_pvpByPlayer && !g_cfg.friendlyFire)) return;
    RlFightReact r = { RL_FIGHT_REACT, (uint8_t)(g_pvpByPlayer ? g_localId : 0xFF), (uint8_t)victim, kind, (uint8_t)a, (uint8_t)b, (uint8_t)c, p0, p1 };
    if (g_cfg.host) NetSendReliableTo(victim, &r, sizeof(r)); else NetSendReliable(&r, sizeof(r));
}

static void __fastcall h_FightHitPed(void *ped, void *edx, void *victim, void *a, void *b, int piece)
{
    // Nous, ou chez l'hote un personnage de l'IA (chez les invites, les copies ne se battent pas pour de vrai).
    void *me = FindPlayerPed();
    bool pvp = GameState() == GS_PLAYING && victim && IsPuppet(victim) && (ped == me || (g_cfg.host && !IsPuppet(ped)));
    if (pvp) { g_pvpVictim = victim; g_pvpByPlayer = ped == me; }
    bool ghost = GameState() == GS_PLAYING && ped == me && victim && IsGhostPed(victim);
    if (ghost) g_ghostVictim = victim;
    o_FightHitPed(ped, edx, victim, a, b, piece);
    if (pvp) g_pvpVictim = NULL;
    if (ghost) g_ghostVictim = NULL;
}

static void __fastcall h_FightDefend(void *ped, void *edx, int dir, int level, int unk)
{
    if (GameState() == GS_PLAYING && IsPuppet(ped)) {
        if (ped == g_pvpVictim) SendFightReact(ped, 0, dir, level, unk, 0, 0);
        return;
    }
    if (GameState() == GS_PLAYING && IsGhostPed(ped)) {
        if (ped == g_ghostVictim) SendGhostReact(ped, 0, dir, level, unk, 0, 0);
        return;
    }
    o_FightDefend(ped, edx, dir, level, unk);
}

static void __fastcall h_SetFall(void *ped, void *edx, int timeout, int anim, int unk)
{
    if (GameState() == GS_PLAYING && IsPuppet(ped)) {
        if (ped == g_pvpVictim) SendFightReact(ped, 1, unk, 0, 0, timeout, anim);
        return;
    }
    if (GameState() == GS_PLAYING && IsGhostPed(ped)) {
        if (ped == g_ghostVictim) SendGhostReact(ped, 1, unk, 0, 0, timeout, anim);
        return;
    }
    o_SetFall(ped, edx, timeout, anim, unk);
}

// --- Projectiles (CProjectileInfo::AddProjectile 0x5C7250) : objets en vol ms_apProjectile[32] en 0x94B708 ;
// en 0x7DB888, gaProjectileInfo[32] (0x1C octets : arme, lanceur, minuterie, en service, derniere position +0x10).
typedef bool(__cdecl *AddProjectile_t)(void *source, int weapon, float x, float y, float z, float power);
static AddProjectile_t o_AddProjectile;
static bool g_replayingProjectile;
bool g_testDropProjectile;
static void **Projectiles() { return (void **)0x94B708; }
static float *ProjectileLastPos(int i) { return (float *)(0x7DB888 + i * 0x1C + 0x10); }

static bool __cdecl h_AddProjectile(void *source, int weapon, float x, float y, float z, float power)
{
    void *before[32];
    memcpy(before, Projectiles(), sizeof(before));
    bool ok = o_AddProjectile(source, weapon, x, y, z, power);
    // Autotest "grenade" : la grenade tombe a nos pieds (pour voir ce que le jeu passe comme auteur de l'explosion).
    if (ok && g_testDropProjectile) for (int i = 0; i < 32; i++) if (Projectiles()[i] && Projectiles()[i] != before[i]) MoveSpeed(Projectiles()[i]) = { 0, 0, 0.05f };
    if (!ok || g_replayingProjectile || GameState() != GS_PLAYING || g_localId < 0) return ok;
    void *me = FindPlayerPed();
    if (!source || (source != me && !(me && InVehicle(me) && source == PedVehicle(me)))) return ok;
    for (int i = 0; i < 32; i++) {
        void *p = Projectiles()[i];
        if (!p || p == before[i]) continue;
        RlProjectile r = { RL_PROJECTILE, (uint8_t)g_localId, (int16_t)weapon, power,
                           { Pos(p).x, Pos(p).y, Pos(p).z }, { MoveSpeed(p).x, MoveSpeed(p).y, MoveSpeed(p).z } };
        NetSendReliable(&r, sizeof(r));
        if (g_cfg.logScripts) Log("combat : projectile %d lance", weapon);
        break;
    }
    return ok;
}

static void ReplayProjectile(const RlProjectile &r)
{
    void *thrower = PuppetPed(r.player);
    if (!thrower || !o_AddProjectile) return;
    void *before[32];
    memcpy(before, Projectiles(), sizeof(before));
    g_replayingProjectile = true;
    bool ok = o_AddProjectile(thrower, r.weapon, r.pos[0], r.pos[1], r.pos[2], r.power);
    g_replayingProjectile = false;
    if (!ok) return;
    for (int i = 0; i < 32; i++) {
        void *p = Projectiles()[i];
        if (!p || p == before[i]) continue;
        Pos(p) = { r.pos[0], r.pos[1], r.pos[2] };
        MoveSpeed(p) = { r.speed[0], r.speed[1], r.speed[2] };
        memcpy(ProjectileLastPos(i), r.pos, 12);   // sinon le test de collision part de l'ancienne position
        break;
    }
    if (g_cfg.logScripts) Log("combat : projectile %d du joueur %d rejoue", r.weapon, r.player);
}

// --- Tir en passager ---
// Le jeu ne fait tirer que le conducteur (CVehicle::DoDriveByShootings, 0x5C97B0 : regarder a gauche / a droite +
// tirer, armes de poing et mitraillettes). Passager, on fait la meme chose pour lui : animation de tir sur le cote,
// CWeapon::FireFromCar. Le tir est attribue au conducteur du vehicule : il devient nous le temps du tir, sinon
// les degats seraient ceux du Tommy distant (ignores) et les munitions les siennes.
static void *GetPad0() { return ((void *(__cdecl *)(int))0x4AB060)(0); }

bool g_testPassengerFire;   // autotest : tire a gauche en continu

void PassengerShooting()
{
    void *me = FindPlayerPed();
    if (!me || !InVehicle(me) || !PedVehicle(me)) return;
    void *veh = PedVehicle(me);
    // Visee libre (clic droit, camera.cpp) : conducteur aussi, dans la direction de la camera. Sinon passager seul,
    // en regardant a gauche / a droite comme le conducteur du jeu.
    bool freeAim = FreeAimActive();
    if ((!freeAim && SeatOf(veh, me) <= 0) || VehClass(veh) == VCLASS_BIKE) return;
    uint8_t *slot = (uint8_t *)me + 0x408 + CurrentWeaponSlot(me) * 0x18;
    int type = *(int *)slot;
    uint8_t *info = ((uint8_t *(__cdecl *)(int))0x5D5710)(type);
    if (!info || *(int *)(info + 0x60) != 5) return;   // pas une arme de tir en voiture
    void *pad = GetPad0();
    bool left = g_testPassengerFire || ((bool(__thiscall *)(void *))0x4AAC90)(pad);
    bool right = !left && ((bool(__thiscall *)(void *))0x4AAC60)(pad);
    if (freeAim) {   // le cote ou l'on vise (camera active : direction en +0x188 + n * 0x1CC + 0x168)
        uint8_t *cam = (uint8_t *)0x7E4688 + 0x188 + ((uint8_t *)0x7E4688)[0x76] * 0x1CC;
        Vec3 f = Field<Vec3>(cam, 0x168), r = Field<Vec3>(veh, 0x04);
        left = f.x * r.x + f.y * r.y < 0.0f;
        right = !left;
    }
    void *clump = Field<void *>(me, 0x4C);
    bool low = (Field<uint8_t>(veh, 0x1FA) >> 3 & 1) != 0;
    int animL = low ? 0x70 : 0x6E, animR = low ? 0x71 : 0x6F;
    auto stop = [&](int id) { for (void *a = FirstAssoc(clump); a; a = NextAssoc(a)) if (Field<int16_t>(a, 0x2C) == id) Field<float>(a, 0x1C) = -1000.0f; };
    if (!left && !right) { stop(animL); stop(animR); return; }
    int want = left ? animL : animR;
    stop(left ? animR : animL);
    bool playing = false;
    for (void *a = FirstAssoc(clump); a; a = NextAssoc(a)) playing |= Field<int16_t>(a, 0x2C) == want && Field<float>(a, 0x1C) >= 0.0f;
    if (!playing) BlendAnimation(clump, 0, want, 8.0f);
    bool fire = g_testPassengerFire || (freeAim ? *(bool *)0x94D788 != 0 : ((bool(__thiscall *)(void *))0x4AAA60)(pad));   // clic gauche / CPad::GetCarGunFired
    uint32_t now = TimeInMs();
    if (!fire || *(uint32_t *)(slot + 0x10) >= now || *(int *)(slot + 0xC) <= 0) return;
    void *driver = VehDriver(veh);
    VehDriver(veh) = me;
    ((bool(__thiscall *)(void *, void *, bool, bool))0x5D44E0)(slot, veh, left, right);
    VehDriver(veh) = driver;
    *(uint32_t *)(slot + 0x10) = now + 70;
    g_localShots++;
}

// Autotest : ce que fait CPed::FightHitPed quand le joueur local frappe victim (parade puis degats).
void TestMeleeHit(void *victim)
{
    void *me = FindPlayerPed();
    bool ghost = IsGhostPed(victim), pvp = IsPuppet(victim);
    if (ghost) g_ghostVictim = victim;
    if (pvp) { g_pvpVictim = victim; g_pvpByPlayer = true; }
    ((void(__thiscall *)(void *, int, int, int))0x52A340)(victim, 0, 2, 0);   // StartFightDefend (par notre crochet)
    ((bool(__thiscall *)(void *, void *, int, float, int, uint8_t))0x525B20)(victim, me, 0, 15.0f, 0, 0);
    g_ghostVictim = g_pvpVictim = NULL;
}

void CombatOnReliable(int from, const uint8_t *data, int len)
{
    if (data[0] == RL_PROJECTILE && len >= (int)sizeof(RlProjectile)) {
        const RlProjectile &r = *(const RlProjectile *)data;
        if (g_cfg.host)   // on fait suivre aux autres invites
            for (int i = 1; i < MAX_PLAYERS; i++) if (i != r.player && g_players[i].connected) NetSendReliableTo(i, &r, sizeof(r));
        if (r.player != g_localId) ReplayProjectile(r);
        return;
    }
    if (data[0] == RL_FIGHT_REACT_PED && len >= (int)sizeof(RlFightReactPed)) {
        const RlFightReactPed &r = *(const RlFightReactPed *)data;
        if (r.owner != g_localId) {   // hote : pour un invite, on fait suivre
            if (g_cfg.host && r.owner < MAX_PLAYERS) NetSendReliableTo(r.owner, &r, sizeof(r));
            return;
        }
        void *ped = PedFromHandle(r.hostHandle);
        if (!ped || InVehicle(ped) || !o_FightDefend || !o_SetFall) return;
        if (r.kind == 0) o_FightDefend(ped, NULL, r.a, r.b, r.c);
        else o_SetFall(ped, NULL, r.p0, r.p1, r.a);
        if (g_cfg.logScripts) Log("combat : %08X %s (coup du joueur %d)", r.hostHandle, r.kind ? "a terre" : "encaisse", r.attacker);
        return;
    }
    if (data[0] == RL_FIGHT_REACT && len >= (int)sizeof(RlFightReact)) {
        const RlFightReact &r = *(const RlFightReact *)data;
        if (r.victim != g_localId) {
            if (g_cfg.host && r.victim < MAX_PLAYERS) NetSendReliableTo(r.victim, &r, sizeof(r));
            return;
        }
        void *me = FindPlayerPed();
        if (!me || (r.attacker != 0xFF && !g_cfg.friendlyFire) || InVehicle(me) || !o_FightDefend || !o_SetFall) return;
        if (r.kind == 0) o_FightDefend(me, NULL, r.a, r.b, r.c);
        else o_SetFall(me, NULL, r.p0, r.p1, r.a);
        if (g_cfg.logScripts) Log("combat : coup du joueur %d, %s", r.attacker, r.kind ? "a terre" : "encaisse");
        return;
    }
    if (data[0] >= 20) { SaveShareOnReliable(from, data, len); return; }   // saveshare.cpp
    if (data[0] == RL_DAMAGE_PED && len >= (int)sizeof(RlDamagePed)) {
        const RlDamagePed &d = *(const RlDamagePed *)data;
        if (d.owner != g_localId) {   // hote : un invite frappe la police d'un autre invite, on fait suivre
            if (g_cfg.host && d.owner < MAX_PLAYERS) NetSendReliableTo(d.owner, &d, sizeof(d));
            return;
        }
        void *ped = PedFromHandle(d.hostHandle);
        if (!ped) return;
        (void)from;
        void *attacker = PuppetPed(d.attacker);
        bool wasAlive = Health(ped) > 0.0f, cop = PedType(ped) == 6;
        ApplyDamage(ped, attacker, d.weapon, d.damage, d.piece, d.dir);
        if (g_cfg.logScripts) Log("combat : joueur %d touche %08X (%.0f, arme %d) -> sante %.0f", from, d.hostHandle, d.damage, d.weapon, Health(ped));
        // Le jeu n'enregistre pas de crime pour un coup porte par un pantin : on donne les etoiles nous-memes (elles
        // sont partagees ensuite). Passant tue : 1 etoile, 3 en deux minutes : 2 ; policier touche : 2, tue : 3.
        if (g_cfg.host) {
            static uint32_t kills[8];
            static int killAt;
            uint32_t now = GetTickCount();
            bool killed = wasAlive && Health(ped) <= 0.0f;
            int want = 0;
            if (cop) want = killed ? 3 : 2;
            else if (killed) {
                kills[killAt++ % 8] = now;
                int recent = 0;
                for (uint32_t t : kills) if (t && now - t < 120000) recent++;
                want = recent >= 3 ? 2 : 1;
            }
            void *me = FindPlayerPed();
            if (want && me && WantedLevel(me) < want) {
                int32_t a[2] = { 0, want };
                MirrorLocal(0x010E, 2, a);   // ALTER_WANTED_LEVEL_NO_DROP
                Log("police : recherche %d (crime du joueur %d sur un personnage de l'hote)", want, from);
            }
        }
    } else if (data[0] == RL_DAMAGE_PVP && len >= (int)sizeof(RlDamagePvp)) {
        const RlDamagePvp &d = *(const RlDamagePvp *)data;
        if (d.victim != g_localId) {   // hote : un invite en touche un autre, on fait suivre
            if (g_cfg.host && d.victim < MAX_PLAYERS) NetSendReliableTo(d.victim, &d, sizeof(d));
            return;
        }
        void *me = FindPlayerPed();
        if (!me || !g_cfg.friendlyFire) return;
        ApplyDamage(me, PuppetPed(d.attacker), d.weapon, d.damage, d.piece, d.dir);
        if (g_cfg.logScripts) Log("combat : touche par le joueur %d (%.0f, arme %d) -> sante %.0f", d.attacker, d.damage, d.weapon, Health(me));
    } else if (data[0] == RL_DAMAGE_PLAYER && !g_cfg.host && len >= (int)sizeof(RlDamagePlayer)) {
        const RlDamagePlayer &d = *(const RlDamagePlayer *)data;
        void *me = FindPlayerPed();
        if (!me) return;
        uint32_t ghost;
        void *attacker = d.npcHandle != 0xFFFFFFFF && GuestPedForHost(d.npcHandle, ghost) ? PedFromHandle(ghost) : NULL;
        ApplyDamage(me, attacker, d.weapon, d.damage, d.piece, d.dir);
        if (g_cfg.logScripts) Log("combat : touche par l'IA de l'hote (%.0f, arme %d) -> sante %.0f", d.damage, d.weapon, Health(me));
    }
}

// Met une arme en main (visuel) ; faux tant que son modele n'est pas charge.
bool HoldWeapon(void *ped, int weapon)
{
    if (weapon <= 0) {
        if (WeaponTypeInSlot(ped, CurrentWeaponSlot(ped)) != 0) SetCurrentWeapon(ped, 0);
        return true;
    }
    int model = *(int *)(0x782A14 + weapon * 0x64 + 0x54);
    if (model > 0 && !HasModelLoaded(model)) { RequestModel(model, 1); return false; }
    if (WeaponTypeInSlot(ped, CurrentWeaponSlot(ped)) != weapon) {
        // Peu de munitions : une copie tuee chez l'invite lache son arme comme chez l'hote (pas 9999 balles). Les tirs
        // visuels des Tommy distants remplissent le chargeur eux-memes (PuppetShoot).
        GiveWeapon(ped, weapon, 30);
        SetCurrentWeapon(ped, weapon);
    }
    return true;
}

void InstallCombatHooks()
{
    static const uint8_t hitPro[] = { 0x53, 0x56, 0x57, 0x55, 0x89, 0xCD, 0x83, 0xEC, 0x60 };
    static const uint8_t defendPro[] = { 0x53, 0x56, 0x55, 0x89, 0xCD, 0x83, 0xEC, 0x50 };
    static const uint8_t fallPro[] = { 0x53, 0x56, 0x57, 0x55, 0x89, 0xCD, 0x83, 0xEC, 0x10 };
    o_FightHitPed = (FightHitPed_t)MakeDetour(0x527800, hitPro, sizeof(hitPro), (void *)h_FightHitPed);
    o_FightDefend = (FightDefend_t)MakeDetour(0x52A340, defendPro, sizeof(defendPro), (void *)h_FightDefend);
    o_SetFall = (SetFall_t)MakeDetour(0x4FD9F0, fallPro, sizeof(fallPro), (void *)h_SetFall);
    static const uint8_t projPro[] = { 0x53, 0x56, 0x57, 0x55, 0xD9, 0x05, 0xCC, 0xD1, 0x69, 0x00 };
    o_AddProjectile = (AddProjectile_t)MakeDetour(0x5C7250, projPro, sizeof(projPro), (void *)h_AddProjectile);

    static const uint8_t firePro[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x28 };
    if (memcmp((void *)0x5D45E0, firePro, sizeof(firePro)) == 0) {
        uint8_t *t = (uint8_t *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(t, firePro, sizeof(firePro));
        t[7] = 0xE9;
        *(int32_t *)(t + 8) = (int32_t)(0x5D45E7 - ((uintptr_t)t + 12));
        o_Fire = (Fire_t)t;
        PatchJump(0x5D45E0, (void *)h_Fire, 7);
    } else {
        Log("combat : prologue de CWeapon::Fire inattendu, tirs non reproduits");
    }
    static const uint8_t prologue[] = { 0xD9, 0x05, 0x70, 0x41, 0x69, 0x00 };   // fld dword [0x694170]
    if (memcmp((void *)0x525B20, prologue, sizeof(prologue)) != 0) {
        Log("combat : prologue d'InflictDamage inattendu, crochet non pose");
        return;
    }
    uint8_t *tramp = (uint8_t *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tramp, prologue, sizeof(prologue));
    tramp[6] = 0xE9;
    *(int32_t *)(tramp + 7) = (int32_t)(0x525B26 - ((uintptr_t)tramp + 11));
    o_InflictDamage = (InflictDamage_t)tramp;
    PatchJump(0x525B20, (void *)h_InflictDamage, 6);
}
