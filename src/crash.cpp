// Journal de plantage : adresse fautive, registres et adresses de retour du jeu trouvees sur la pile.
#include "util.h"
#include "vccoop.h"

static LONG WINAPI OnCrash(EXCEPTION_POINTERS *ep)
{
    const CONTEXT *c = ep->ContextRecord;
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    HMODULE mod = NULL;
    char modName[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)r->ExceptionAddress, &mod))
        GetModuleFileNameA(mod, modName, MAX_PATH);
    Log("PLANTAGE code %08lX en %p (%s, +%lX) acces %p", r->ExceptionCode, r->ExceptionAddress, modName,
        (DWORD)((uintptr_t)r->ExceptionAddress - (uintptr_t)mod),
        r->NumberParameters > 1 ? (void *)r->ExceptionInformation[1] : NULL);
    Log("  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX",
        c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    char buf[512];
    int n = wsprintfA(buf, "  pile :");
    DWORD *sp = (DWORD *)c->Esp;
    for (int i = 0, found = 0; i < 4096 && found < 16; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        DWORD v = sp[i];
        if (v >= 0x401000 && v < 0x67E000) { n += wsprintfA(buf + n, " %06lX", v); found++; }
    }
    Log("%s", buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Le jeu remplace le filtre d'exceptions de haut niveau : on passe par un gestionnaire vectoriel, limite
// au fil du jeu et hors du code ajoute par le downgrader (0xA11000-0xA20000), qui provoque volontairement
// des fautes au demarrage et les rattrape.
static DWORD g_gameThread;

static LONG WINAPI OnVectored(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    if (GetCurrentThreadId() != g_gameThread) return EXCEPTION_CONTINUE_SEARCH;
    if (at >= 0xA11000 && at < 0xA20000) return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW) {
        static int count;
        if (count++ < 3) OnCrash(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashLog()
{
    g_gameThread = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, OnVectored);
}
