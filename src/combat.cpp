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

enum { RL_DAMAGE_PED = 10, RL_DAMAGE_PLAYER = 11 };

#pragma pack(push, 1)
struct RlDamagePed { uint8_t type, attacker, dir; uint32_t hostHandle; int32_t weapon, piece; float damage; };
struct RlDamagePlayer { uint8_t type, dir; uint32_t npcHandle; int32_t weapon, piece; float damage; };
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

void CombatOnReliable(int from, const uint8_t *data, int len)
{
    if (data[0] >= 20) { SaveShareOnReliable(from, data, len); return; }   // saveshare.cpp
    if (data[0] == RL_DAMAGE_PED && g_cfg.host && len >= (int)sizeof(RlDamagePed)) {
        const RlDamagePed &d = *(const RlDamagePed *)data;
        void *ped = PedFromHandle(d.hostHandle);
        if (!ped) return;
        void *attacker = PuppetPed(from);
        ApplyDamage(ped, attacker, d.weapon, d.damage, d.piece, d.dir);
        if (g_cfg.logScripts) Log("combat : joueur %d touche %08X (%.0f, arme %d) -> sante %.0f", from, d.hostHandle, d.damage, d.weapon, Health(ped));
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
        GiveWeapon(ped, weapon, 9999);
        SetCurrentWeapon(ped, weapon);
    }
    return true;
}

void InstallCombatHooks()
{
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
