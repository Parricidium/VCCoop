// Chien de garde de diagnostic : si le fil du jeu ne produit plus d'image, on journalise ou il est bloque
// (EIP et chaine EBP) pour retrouver la fonction dans Ghidra.
#include "util.h"
#include "vccoop.h"
#include <stdlib.h>

static DWORD g_mainThreadId;
static volatile LONG g_frameCount;

void WatchdogFrame() { InterlockedIncrement(&g_frameCount); }

// Diagnostic (SurveilleEcriture=0xADRESSE) : point d'arret materiel en ecriture sur 4 octets ; chaque
// ecriture par le fil du jeu est journalisee avec l'instruction responsable et l'adresse de retour.
static uintptr_t g_watchAddr;
static LONG WINAPI OnSingleStep(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    static uintptr_t seen[200][2];
    static int seenCount;
    uintptr_t eip = ep->ContextRecord->Eip, val = *(DWORD *)g_watchAddr;
    bool known = false;
    for (int i = 0; i < seenCount; i++) known |= seen[i][0] == eip && seen[i][1] == val;
    if (!known && seenCount < 200) {
        seen[seenCount][0] = eip;
        seen[seenCount++][1] = val;
        DWORD *sp = (DWORD *)ep->ContextRecord->Esp;
        char buf[256];
        int n = wsprintfA(buf, "ecriture en %08lX = %08lX par eip %08lX, pile :", (DWORD)g_watchAddr,
                          *(DWORD *)g_watchAddr, (DWORD)eip);
        for (int i = 0, f = 0; i < 256 && f < 6; i++) {
            if (IsBadReadPtr(sp + i, 4)) break;
            if (sp[i] >= 0x401000 && sp[i] < 0x67E000) { n += wsprintfA(buf + n, " %06lX", sp[i]); f++; }
        }
        Log("%s", buf);
    }
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void ArmWatch(HANDLE th)
{
    if (!g_watchAddr) return;
    SuspendThread(th);
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(th, &ctx);
    ctx.Dr0 = g_watchAddr;
    ctx.Dr7 = 1 | (1 << 16) | (3 << 18);   // L0, ecriture, 4 octets
    SetThreadContext(th, &ctx);
    ResumeThread(th);
    Log("surveillance des ecritures en %08lX", (DWORD)g_watchAddr);
}

static volatile uintptr_t g_watchRequest;   // adresse a surveiller demandee en cours de partie
void WatchAddress(uintptr_t addr) { g_watchRequest = addr; }

static DWORD WINAPI WatchdogThread(LPVOID)
{
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, g_mainThreadId);
    if (!th) return 0;
    if (g_watchAddr) { AddVectoredExceptionHandler(1, OnSingleStep); Sleep(5000); ArmWatch(th); }
    LONG last = -1;
    int stalls = 0, ticks = 0;
    for (;;) {
        Sleep(3000);
        if (g_watchRequest) {
            static bool vehAdded;
            if (!vehAdded) { AddVectoredExceptionHandler(1, OnSingleStep); vehAdded = true; }
            g_watchAddr = g_watchRequest;
            g_watchRequest = 0;
            ArmWatch(th);
        }
        LONG now = g_frameCount;
        // Mode diagnostic (JournalScripts=1) : un releve toutes les ~9 s meme si des images arrivent.
        bool sample = g_cfg.logScripts && (++ticks % 3) == 0;
        if (now != last && !sample) { last = now; stalls = 0; continue; }
        if (now != last) { last = now; stalls = 0; }
        else if (++stalls > 5) continue;   // quelques relevés suffisent
        SuspendThread(th);
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        GetThreadContext(th, &ctx);
        char buf[512];
        int n = wsprintfA(buf, "chien de garde : images %ld eip=%08lX pile:", now, ctx.Eip);
        // Adresses de retour trouvees sur la pile qui tombent dans le code du jeu (plus fiable qu'EBP
        // car le jeu est compile sans pointeur de cadre).
        DWORD *sp = (DWORD *)ctx.Esp;
        int found = 0;
        for (int i = 0; i < 2048 && found < 12; i++) {
            DWORD v;
            if (IsBadReadPtr(sp + i, 4)) break;
            v = sp[i];
            if (v >= 0x401000 && v < 0x67E000) { n += wsprintfA(buf + n, " %06lX", v); found++; }
        }
        ResumeThread(th);
        Log("%s", buf);
    }
}

void StartWatchdog()
{
    char ini[MAX_PATH], v[32];
    lstrcpynA(ini, IniPath(), MAX_PATH);
    GetPrivateProfileStringA("VCCoop", "SurveilleEcriture", "", v, sizeof(v), ini);
    if (v[0]) g_watchAddr = strtoul(v, NULL, 0);
    g_mainThreadId = GetCurrentThreadId();
    CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
}
