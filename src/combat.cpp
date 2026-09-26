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
#include <string.h>

using namespace game;

enum { RL_DAMAGE_PED = 10, RL_DAMAGE_PLAYER = 11, RL_DAMAGE_PVP = 12, RL_FIGHT_REACT = 13, RL_FIGHT_REACT_PED = 14 };

#pragma pack(push, 1)
struct RlDamagePed { uint8_t type, attacker, dir; uint32_t hostHandle; int32_t weapon, piece; float damage; };
struct RlDamagePlayer { uint8_t type, dir; uint32_t npcHandle; int32_t weapon, piece; float damage; };
// Un joueur en touche un autre (tir ami) : part chez la victime (par l'hote si besoin), qui subit le coup chez elle.
struct RlDamagePvp { uint8_t type, attacker, victim, dir; int32_t weapon, piece; float damage; };
// Reaction a un coup de poing / pied d'un joueur : parade (CPed::StartFightDefend) ou chute (CPed::SetFall), rejouee
// chez la victime. kind 0 : defend(a, b, c) ; kind 1 : fall(p0, p1, a).
struct RlFightReact { uint8_t type, attacker, victim, kind, a, b, c; int32_t p0, p1; };
// Meme chose quand un invite frappe la copie d'un personnage de l'hote : la reaction est jouee chez l'hote.
struct RlFightReactPed { uint8_t type, attacker, kind, a, b, c; uint32_t hostHandle; int32_t p0, p1; };
#pragma pack(pop)

typedef bool(__fastcall *InflictDamage_t)(void *ped, void *edx, void *damager, int weapon, float damage, int piece, uint8_t dir);
static InflictDamage_t o_InflictDamage;   // trampoline : FLD d'origine (6 octets) puis jmp 0x525B26
static bool g_bypass;

// --- Tirs : on compte ceux du joueur local ; ceux des autres sont rejoues en visuel sur leur Tommy ---
typedef bool(__fastcall *Fire_t)(void *weapon, void *edx, void *shooter, void *source);
static Fire_t o_Fire;
static uint8_t g_localShots;
static bool g_cosmetic;

static bool __fastcall h_Fire(void *weapon, void *edx, void *shooter, void *source)
{
    bool r = o_Fire(weapon, edx, shooter, source);
    if (r && !g_cosmetic && shooter && shooter == FindPlayerPed()) g_localShots++;
    return r;
}

uint8_t LocalShotCount() { return g_localShots; }

// Armes a balles seulement (colt 17 .. fusil laser 29, M60 32, minigun 33) : grenades, roquettes, lance-flammes
// exploseraient localement et divergeraient d'une machine a l'autre.
static bool CosmeticWeapon(int w) { return (w >= 17 && w <= 29) || w == 32 || w == 33; }

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
    // Les coups portes par le Tommy d'un autre joueur sont decides sur sa machine et arrivent par le reseau :
    // ici ils ne font rien (evite les doubles degats et le tir ami involontaire).
    if (damager && (IsPuppet(damager) || (!IsPedEntity(damager) && IsPuppetVehicle(damager)))) return false;
    // Nous touchons le Tommy d'un autre joueur (coup, balle, voiture) : rien ici, le coup part chez lui.
    int victim = PuppetPlayer(ped);
    if (victim >= 0 && damager && (damager == me || damager == PedVehicle(me))) {
        if (g_cfg.friendlyFire) {
            RlDamagePvp d = { RL_DAMAGE_PVP, (uint8_t)g_localId, (uint8_t)victim, dir, weapon, piece, damage };
            if (g_cfg.host) NetSendReliableTo(victim, &d, sizeof(d));
            else NetSendReliable(&d, sizeof(d));
            if (g_cfg.logScripts) Log("combat : je touche le joueur %d (%.0f, arme %d)", victim, damage, weapon);
        }
        return false;
    }
    if (!g_cfg.host) {
        uint32_t host;
        if (GhostHostHandle(ped, host)) {
            // Seuls nos propres coups comptent (le reste est deja simule chez l'hote).
            if (damager == me || (damager && damager == PedVehicle(me))) {
                RlDamagePed d = { RL_DAMAGE_PED, (uint8_t)g_localId, dir, host, weapon, piece, damage };
                NetSendReliable(&d, sizeof(d));
            }
            return false;
        }
        if (IsPuppet(ped)) return false;
    } else {
        int pid = PuppetPlayer(ped);
        if (pid > 0) {
            bool friendly = damager && (damager == me || IsPuppet(damager) || damager == PedVehicle(me));
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

static void SendGhostReact(void *ghost, uint8_t kind, int a, int b, int c, int p0, int p1)
{
    uint32_t host;
    if (!GhostHostHandle(ghost, host)) return;
    RlFightReactPed r = { RL_FIGHT_REACT_PED, (uint8_t)g_localId, kind, (uint8_t)a, (uint8_t)b, (uint8_t)c, host, p0, p1 };
    NetSendReliable(&r, sizeof(r));
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
    bool ghost = GameState() == GS_PLAYING && !g_cfg.host && ped == me && victim && IsGhostPed(victim);
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
    if (GameState() == GS_PLAYING && !g_cfg.host && IsGhostPed(ped)) {
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
    if (GameState() == GS_PLAYING && !g_cfg.host && IsGhostPed(ped)) {
        if (ped == g_ghostVictim) SendGhostReact(ped, 1, unk, 0, 0, timeout, anim);
        return;
    }
    o_SetFall(ped, edx, timeout, anim, unk);
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
    if (data[0] == RL_FIGHT_REACT_PED && g_cfg.host && len >= (int)sizeof(RlFightReactPed)) {
        const RlFightReactPed &r = *(const RlFightReactPed *)data;
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
    if (data[0] == RL_DAMAGE_PED && g_cfg.host && len >= (int)sizeof(RlDamagePed)) {
        const RlDamagePed &d = *(const RlDamagePed *)data;
        void *ped = PedFromHandle(d.hostHandle);
        if (!ped) return;
        void *attacker = PuppetPed(from);
        ApplyDamage(ped, attacker, d.weapon, d.damage, d.piece, d.dir);
        if (g_cfg.logScripts) Log("combat : joueur %d touche %08X (%.0f, arme %d) -> sante %.0f", from, d.hostHandle, d.damage, d.weapon, Health(ped));
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
