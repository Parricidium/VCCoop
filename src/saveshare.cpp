// Partage de la sauvegarde de l'hote.
//  - Hote : quand il charge une sauvegarde (m_bWantToLoad passe a 1), le fichier de l'emplacement choisi part chez
//    les invites (flux fiable, par morceaux) ; il le garde pour ceux qui arrivent ensuite.
//  - Invite : il l'ecrit dans son emplacement 8 (reserve a la coop) et le charge, par l'ecran de chargement du jeu
//    s'il est au menu, par les memes drapeaux que le jeu s'il est deja en partie.
// Sauvegardes : <dossier>\GTAVCsf<emplacement+1>.b (base "...\GTAVCsf" en 0x97509C, format "%s%i.b").
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "saveshare.h"
#include "mirror.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace game;

enum { RL_SAVE_BEGIN = 20, RL_SAVE_DATA = 21, RL_SAVE_END = 22 };
enum { COOP_SLOT = 7, CHUNK = MAX_RELIABLE_PAYLOAD - 8 };

static uint8_t *Menu() { return (uint8_t *)0x869630; }
static int &CurrSaveSlot() { return *(int *)(Menu() + 0x100); }

static void SlotPath(int slot, char *out, int n) { _snprintf(out, n, "%s%i.b", (const char *)0x97509C, slot + 1); }
static void PopulateSlotInfo() { ((void(__cdecl *)())0x61D4A0)(); }

static uint32_t Checksum(const uint8_t *d, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ d[i]) * 16777619u;
    return h;
}

// --- Hote ---
static uint8_t *g_hostSave;
static int g_hostSaveLen;

bool HostHasSave() { return g_hostSave != NULL; }

void SendHostSave(int peer)
{
    if (!g_hostSave) return;
    uint8_t buf[MAX_RELIABLE_PAYLOAD];
    buf[0] = RL_SAVE_BEGIN;
    memcpy(buf + 1, &g_hostSaveLen, 4);
    if (peer < 0) NetSendReliable(buf, 5); else NetSendReliableTo(peer, buf, 5);
    for (int off = 0; off < g_hostSaveLen; off += CHUNK) {
        int n = g_hostSaveLen - off < CHUNK ? g_hostSaveLen - off : CHUNK;
        buf[0] = RL_SAVE_DATA;
        memcpy(buf + 1, &off, 4);
        memcpy(buf + 5, g_hostSave + off, n);
        if (peer < 0) NetSendReliable(buf, 5 + n); else NetSendReliableTo(peer, buf, 5 + n);
    }
    uint32_t sum = Checksum(g_hostSave, g_hostSaveLen);
    buf[0] = RL_SAVE_END;
    memcpy(buf + 1, &sum, 4);
    if (peer < 0) NetSendReliable(buf, 5); else NetSendReliableTo(peer, buf, 5);
    Log("sauvegarde : %d octets envoyes (%s)", g_hostSaveLen, peer < 0 ? "a tous" : "a un nouvel invite");
}

static void HostWatchLoad()
{
    static bool wasLoading, wasRestarting;
    bool loading = MenuWantToLoad() != 0, restarting = MenuWantToRestart() != 0;
    if (loading && !wasLoading) {
        char path[MAX_PATH];
        SlotPath(CurrSaveSlot(), path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *d = n > 0 ? (uint8_t *)malloc(n) : NULL;
            if (d && fread(d, 1, n, f) == (size_t)n) {
                free(g_hostSave);
                g_hostSave = d;
                g_hostSaveLen = (int)n;
                Log("sauvegarde : l'hote charge l'emplacement %d (%ld octets)", CurrSaveSlot() + 1, n);
                SendHostSave(-1);
                MirrorGuestsGetHostSave();
            } else free(d);
            fclose(f);
        } else Log("sauvegarde : impossible de lire %s", path);
    } else if (restarting && !wasRestarting && !loading) {
        // Nouvelle partie : plus de sauvegarde a partager.
        free(g_hostSave);
        g_hostSave = NULL;
        g_hostSaveLen = 0;
    }
    wasLoading = loading;
    wasRestarting = restarting;
}

// --- Invite ---
static uint8_t *g_rx;
static int g_rxLen, g_rxGot;
static bool g_loadPending;
bool g_saveIncoming;

bool GuestWaitingForSave() { return g_saveIncoming || g_loadPending; }

void SaveShareOnReliable(int from, const uint8_t *d, int len)
{
    (void)from;
    if (g_cfg.host) return;
    if (d[0] == RL_SAVE_BEGIN && len >= 5) {
        free(g_rx);
        memcpy(&g_rxLen, d + 1, 4);
        g_rx = g_rxLen > 0 && g_rxLen < 8 * 1024 * 1024 ? (uint8_t *)malloc(g_rxLen) : NULL;
        g_rxGot = 0;
        g_saveIncoming = g_rx != NULL;
        Log("sauvegarde : reception de %d octets", g_rxLen);
    } else if (d[0] == RL_SAVE_DATA && len > 5 && g_rx) {
        int off;
        memcpy(&off, d + 1, 4);
        int n = len - 5;
        if (off >= 0 && off + n <= g_rxLen) { memcpy(g_rx + off, d + 5, n); g_rxGot += n; }
    } else if (d[0] == RL_SAVE_END && len >= 5 && g_rx) {
        uint32_t sum;
        memcpy(&sum, d + 1, 4);
        g_saveIncoming = false;
        if (g_rxGot != g_rxLen || Checksum(g_rx, g_rxLen) != sum) { Log("sauvegarde : recue incomplete ou abimee"); return; }
        char path[MAX_PATH];
        SlotPath(COOP_SLOT, path, sizeof(path));
        FILE *f = fopen(path, "wb");
        if (!f) { Log("sauvegarde : impossible d'ecrire %s", path); return; }
        fwrite(g_rx, 1, g_rxLen, f);
        fclose(f);
        PopulateSlotInfo();
        g_loadPending = true;
        Log("sauvegarde : ecrite dans l'emplacement %d, chargement", COOP_SLOT + 1);
    }
}

static void GuestLoad()
{
    if (!g_loadPending) return;
    int state = GameState();
    if ((state == GS_FRONTEND || state == GS_PLAYING) && MenuActive()) {
        // Au menu : l'ecran "chargement en cours" (12) du jeu verifie l'emplacement et lance le chargement.
        CurrSaveSlot() = COOP_SLOT;
        MenuRequestPage(12);
        g_loadPending = false;
    } else if (state == GS_PLAYING && !MenuActive()) {
        // En partie : comme un joueur (Pause puis Charger) : on ouvre le menu (m_bStartUpFrontEndRequested,
        // menu +0x12) ; au prochain passage, la branche "menu actif" ci-dessus passe par l'ecran 12.
        // (Lever directement les drapeaux redemarrer + charger saute l'arret de l'audio et la fermeture du
        // menu : la camera de l'invite plantait ensuite dans Process_FlyBy.)
        *(bool *)(0x869630 + 0x12) = true;
    }
}

void SaveShareFrame()
{
    if (g_cfg.host) HostWatchLoad();
    else GuestLoad();
}
