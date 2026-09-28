// Ce qui est a chaque invite et que la sauvegarde partagee de l'hote ecraserait : son argent, ses armes et les
// immeubles qu'il a achetes (drapeaux du script principal ecrits par SES missions d'achat). Garde dans un fichier a
// son pseudo a cote de vccoop.ini, remis apres chaque chargement de la sauvegarde de l'hote.
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "overlay.h"
#include "mirror.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

using namespace game;

enum { GLOBALS_BEGIN = 8, GLOBALS_END = 0x8620, MAX_FLAGS = 64 };
static struct { uint16_t off; int32_t val; } g_flags[MAX_FLAGS];
static int g_flagCount;
static uint8_t g_buySnap[GLOBALS_END];
static bool g_buyRunning, g_loaded, g_restorePending;
static uint32_t g_lastSave;

static const char *OverlayPath()
{
    static char path[MAX_PATH];
    if (!path[0]) {
        lstrcpynA(path, IniPath(), MAX_PATH);
        char *slash = strrchr(path, '\\');
        if (slash) slash[1] = 0;
        char name[40];
        int n = 0;
        for (const char *c = g_cfg.playerName; *c && n < 30; c++) name[n++] = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ? *c : '_';
        name[n] = 0;
        wsprintfA(path + lstrlenA(path), "joueur-%s.ini", name[0] ? name : "invite");
    }
    return path;
}

static int &Money() { return *(int *)(0x94AD28 + 0xA0); }   // CWorld::Players[0].m_nMoney

static void SetFlag(uint16_t off, int32_t val)
{
    for (int i = 0; i < g_flagCount; i++) if (g_flags[i].off == off) { g_flags[i].val = val; return; }
    if (g_flagCount < MAX_FLAGS) g_flags[g_flagCount++] = { off, val };
}

static void Save()
{
    void *me = FindPlayerPed();
    if (!me) return;
    char buf[2048];
    wsprintfA(buf, "%d", Money());
    WritePrivateProfileStringA("Joueur", "Argent", buf, OverlayPath());
    int n = 0;
    for (int s = 0; s < 10 && n < 1900; s++) {
        int w = WeaponTypeInSlot(me, s), ammo = Field<int>(me, 0x408 + s * 0x18 + 0xC);
        if (w > 0) n += wsprintfA(buf + n, "%d:%d,", w, ammo);
    }
    buf[n] = 0;
    WritePrivateProfileStringA("Joueur", "Armes", buf, OverlayPath());
    n = 0;
    for (int i = 0; i < g_flagCount && n < 1900; i++) n += wsprintfA(buf + n, "%d:%d,", g_flags[i].off, g_flags[i].val);
    buf[n] = 0;
    WritePrivateProfileStringA("Joueur", "Immeubles", buf, OverlayPath());
    g_lastSave = GetTickCount();
}

static void Load()
{
    g_loaded = true;
    char buf[2048];
    GetPrivateProfileStringA("Joueur", "Immeubles", "", buf, sizeof(buf), OverlayPath());
    g_flagCount = 0;
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        int off = 0, val = 0;
        if (sscanf(t, "%d:%d", &off, &val) == 2 && off >= GLOBALS_BEGIN && off + 4 <= GLOBALS_END) SetFlag((uint16_t)off, val);
    }
}

// Apres le chargement de la sauvegarde de l'hote : argent, armes et immeubles de l'invite remis.
static void Restore()
{
    void *me = FindPlayerPed();
    if (!me || !g_loaded) return;
    char buf[2048];
    int money = GetPrivateProfileIntA("Joueur", "Argent", -1, OverlayPath());
    if (money >= 0) Money() = money;
    GetPrivateProfileStringA("Joueur", "Armes", "", buf, sizeof(buf), OverlayPath());
    int given = 0;
    // Les armes de la sauvegarde de l'hote d'abord retirees (GiveWeapon ajoutait les munitions a chaque chargement).
    if (buf[0]) { int32_t z[1] = { 0 }; MirrorLocal(0x03B8, 1, z); }   // REMOVE_ALL_PLAYER_WEAPONS
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        int w = 0, ammo = 0;
        if (sscanf(t, "%d:%d", &w, &ammo) != 2 || w <= 0 || w >= 40) continue;
        int model = *(int *)(0x782A14 + w * 0x64 + 0x54);   // CWeaponInfo : modele
        if (model > 0 && !HasModelLoaded(model)) { RequestModel(model, 1); ((void(__cdecl *)(bool))0x40B5F0)(false); }
        if (model > 0 && !HasModelLoaded(model)) continue;
        GiveWeapon(me, w, ammo > 0 ? ammo : 1);
        given++;
    }
    for (int i = 0; i < g_flagCount; i++) *(int32_t *)(ScriptSpace() + g_flags[i].off) = g_flags[i].val;
    Log("joueur : argent %d, %d armes et %d drapeaux d'immeubles remis apres le chargement", money, given, g_flagCount);
}

void OverlayBuyStart()
{
    if (g_cfg.host) return;
    memcpy(g_buySnap, ScriptSpace(), GLOBALS_END);
    g_buyRunning = true;
}

void OverlayBuyEnd()
{
    if (g_cfg.host || !g_buyRunning) return;
    g_buyRunning = false;
    int n = 0;
    for (int off = GLOBALS_BEGIN; off + 4 <= GLOBALS_END; off += 4) {
        int32_t now = *(int32_t *)(ScriptSpace() + off), before = *(int32_t *)(g_buySnap + off);
        if (now == before || off == 313 * 4 || now < -1 || now > 255) continue;   // $313 = ONMISSION
        SetFlag((uint16_t)off, now);
        n++;
    }
    Save();
    Log("joueur : immeuble achete, %d drapeaux gardes (%d en tout)", n, g_flagCount);
}

void OverlayLoadedHostSave() { g_restorePending = true; }

void OverlayFrame(bool inGame)
{
    if (g_cfg.host || g_localId <= 0) return;
    if (!g_loaded) Load();
    static bool wasInGame;
    if (inGame && !wasInGame && g_restorePending) { g_restorePending = false; Restore(); }
    if (!inGame && wasInGame) Save();
    if (inGame && GetTickCount() - g_lastSave > 30000) Save();
    wasInGame = inGame;
}
