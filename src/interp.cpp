// Interpolation des entites distantes : horloges, pistes d'etats, crochet apres la physique du jeu.
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include "interp.h"
#include <math.h>
#include <string.h>

using namespace game;

// --- Horloges ---
// Ecart (notre heure - heure de l'expediteur) le plus faible observe : c'est celui des paquets arrives le plus vite.
// Il remonte de 2 ms par seconde pour suivre la derive des horloges, et repart de zero s'il saute (autre machine).
static uint32_t g_offset[MAX_PLAYERS];
static bool g_haveOffset[MAX_PLAYERS];
static uint32_t g_relaxAt[MAX_PLAYERS];

void ClockSample(int src, uint32_t senderTime)
{
    if (src < 0 || src >= MAX_PLAYERS) return;
    uint32_t now = GetTickCount(), off = now - senderTime;
    if (!g_haveOffset[src] || (int32_t)(off - g_offset[src]) < 0 || (int32_t)(off - g_offset[src]) > 2000) {
        g_offset[src] = off;
        g_haveOffset[src] = true;
        g_relaxAt[src] = now;
    } else if (now - g_relaxAt[src] >= 1000) {
        g_offset[src] += 2;
        g_relaxAt[src] = now;
    }
}

// --- Pistes ---
void Track::Push(const Snap &n)
{
    if (count && (int32_t)(n.t - s[count - 1].t) <= 0) return;   // en retard ou en double (UDP)
    // Long silence (vehicule a l'arret, envoye 1 fois par seconde) : l'entite n'a pas bouge entre les deux ;
    // on repete l'ancien etat juste avant le nouveau, sinon on la verrait glisser lentement pendant tout l'ecart.
    if (count && n.t - s[count - 1].t > 300) {
        Snap hold = s[count - 1];
        hold.t = n.t - 60;
        hold.vel[0] = hold.vel[1] = hold.vel[2] = 0;
        if (count == TRACK_SNAPS) { for (int i = 1; i < count; i++) s[i - 1] = s[i]; count--; }
        s[count++] = hold;
    }
    if (count == TRACK_SNAPS) { for (int i = 1; i < count; i++) s[i - 1] = s[i]; count--; }
    s[count++] = n;
}

static void Normalize(float *v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static float LerpAngle(float a, float b, float u)
{
    float d = b - a;
    while (d > 3.14159265f) d -= 6.28318531f;
    while (d < -3.14159265f) d += 6.28318531f;
    return a + d * u;
}

bool TrackSample(const Track &tr, int src, Snap &o, bool linear)
{
    if (!tr.count) return false;
    uint32_t rt = GetTickCount() - (src >= 0 && src < MAX_PLAYERS ? g_offset[src] : 0) - INTERP_DELAY_MS;
    const Snap &newest = tr.s[tr.count - 1];
    if ((int32_t)(rt - newest.t) >= 0) {
        // Plus rien de recent : on prolonge avec la vitesse.
        float dt = (float)(rt - newest.t);
        if (dt > 250.0f) dt = 250.0f;
        o = newest;
        if (linear) {
            if (tr.count >= 2 && newest.t - tr.s[tr.count - 2].t > 0) {
                const Snap &p = tr.s[tr.count - 2];
                float f = 20.0f / (float)(newest.t - p.t);
                for (int k = 0; k < 3; k++) o.vel[k] = (newest.pos[k] - p.pos[k]) * f;
            } else o.vel[0] = o.vel[1] = o.vel[2] = 0;
        }
        for (int k = 0; k < 3; k++) o.pos[k] += o.vel[k] * dt / 20.0f;
        return true;
    }
    if ((int32_t)(rt - tr.s[0].t) <= 0) { o = tr.s[0]; return true; }
    int i = tr.count - 1;
    while (i > 1 && (int32_t)(rt - tr.s[i - 1].t) < 0) i--;
    const Snap &a = tr.s[i - 1], &b = tr.s[i];
    float span = (float)(b.t - a.t), u = (float)(rt - a.t) / span;
    o = b;
    o.t = rt;
    // Hermite (positions et vitesses des deux etats) : la trajectoire suit les virages au lieu de les couper.
    float D = span / 20.0f, u2 = u * u, u3 = u2 * u;
    float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    for (int k = 0; k < 3; k++) {
        if (linear) {
            o.pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * u;
            o.vel[k] = (b.pos[k] - a.pos[k]) / D;
        } else {
            o.pos[k] = h00 * a.pos[k] + h10 * D * a.vel[k] + h01 * b.pos[k] + h11 * D * b.vel[k];
            o.vel[k] = a.vel[k] + (b.vel[k] - a.vel[k]) * u;
        }
        o.right[k] = a.right[k] + (b.right[k] - a.right[k]) * u;
        o.fwd[k] = a.fwd[k] + (b.fwd[k] - a.fwd[k]) * u;
    }
    Normalize(o.right);
    Normalize(o.fwd);
    o.heading = LerpAngle(a.heading, b.heading, u);
    return true;
}

// --- Crochet apres CGame::Process (appel dans Idle, 0x4A5DA0) ---
// Distance d'affichage (DistanceAffichage, en %) : CCamera::Process (dans CGame::Process) vient de calculer
// m_fLODDistMultiplier (0x7E4778 : jusqu'ou les modeles detailles, passants et vehicules restent visibles) et
// m_fGenerationDistMultiplier (0x7E477C : jusqu'ou la circulation et les passants apparaissent / sont gardes).
// On les augmente (la generation moitie moins, pour rester dans les limites du jeu : ~110 vehicules, ~140
// personnages) ; la memoire de chargement (CStreaming::ms_memoryAvailable, 0x94DD54, 45 Mo) suit.
static void ApplyDrawDistance()
{
    float f = g_cfg.drawDistance / 100.0f;
    if (f <= 1.0f) return;
    *(float *)0x7E4778 *= f;
    float gen = 1.0f + (f - 1.0f) * 0.5f;
    if (gen > 1.6f) gen = 1.6f;
    *(float *)0x7E477C *= gen;
    int mem = 45 * 1024 * 1024 + (int)((f - 1.0f) * 80 * 1024 * 1024);
    if (mem > 256 * 1024 * 1024) mem = 256 * 1024 * 1024;
    if (*(int *)0x94DD54 < mem) *(int *)0x94DD54 = mem;
}

static void __cdecl h_GameProcess()
{
    ((void(__cdecl *)())0x4A4410)();
    ApplyDrawDistance();
    if (GameState() != GS_PLAYING || !FindPlayerPed()) return;
    PuppetsAfterProcess();
    VehiclesAfterProcess();
    GhostsAfterProcess();
}

void InstallInterp()
{
    static const uint8_t call[] = { 0xE8, 0x6B, 0xE6, 0xFF, 0xFF };   // call 0x4A4410
    if (memcmp((void *)0x4A5DA0, call, sizeof(call)) != 0) { Log("interpolation : appel de CGame::Process introuvable"); return; }
    PatchCall(0x4A5DA0, (void *)h_GameProcess);
}
