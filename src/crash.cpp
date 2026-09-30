// Journal de plantage : adresse fautive, registres et adresses de retour du jeu (et du mod) trouvees sur la pile.
#include "util.h"
#include "vccoop.h"

static HMODULE g_self;   // dinput8.dll (le mod)

static void OnCrash(EXCEPTION_POINTERS *ep)
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
    // Adresses de retour : jeu (0x401000-0x67E000) et mod (m+decalage, voir build\dinput8.map). Une recursion sans fin
    // (debordement de pile) s'y voit comme une adresse qui se repete.
    uintptr_t selfLo = (uintptr_t)g_self, selfHi = selfLo;
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_self;
        if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE) selfHi = selfLo + ((IMAGE_NT_HEADERS *)((BYTE *)dos + dos->e_lfanew))->OptionalHeader.SizeOfImage;
    }
    char buf[1024];
    int n = wsprintfA(buf, "  pile :");
    DWORD *sp = (DWORD *)c->Esp;
    for (int i = 0, found = 0; i < 8192 && found < 32; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        DWORD v = sp[i];
        if (v >= 0x401000 && v < 0x67E000) { n += wsprintfA(buf + n, " %06lX", v); found++; }
        else if (v >= selfLo + 0x1000 && v < selfHi) { n += wsprintfA(buf + n, " m+%lX", (DWORD)(v - selfLo)); found++; }
    }
    Log("%s", buf);
}

// Le jeu remplace le filtre d'exceptions de haut niveau : on passe par un gestionnaire vectoriel, limite
// au fil du jeu et hors du code ajoute par le downgrader (0xA11000-0xA20000), qui provoque volontairement
// des fautes au demarrage et les rattrape.
// Le rapport est ecrit par un fil a part (sa propre pile) : sur un debordement de pile, l'ecrire depuis le fil fautif
// faisait replanter le gestionnaire dans Log avant la moindre ligne (JD, 30/09 : jeu ferme sans rien au journal,
// Windows : faute dans Log). Le gestionnaire lui-meme n'utilise presque pas de pile.
static DWORD g_gameThread;
static DWORD WINAPI CrashThread(void *p) { OnCrash((EXCEPTION_POINTERS *)p); return 0; }

static LONG WINAPI OnVectored(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    if (GetCurrentThreadId() != g_gameThread) return EXCEPTION_CONTINUE_SEARCH;
    if (at >= 0xA11000 && at < 0xA20000) return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW) {
        static int count;
        if (count++ < 3) {
            HANDLE t = CreateThread(NULL, 256 * 1024, CrashThread, ep, 0, NULL);
            if (t) { WaitForSingleObject(t, 5000); CloseHandle(t); }
        }
        // Instance de test hors ecran : pas de boite "Unhandled Exception" sur l'ecran de JD, on s'arrete la.
        if (g_cfg.background) TerminateProcess(GetCurrentProcess(), 3);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashLog()
{
    g_gameThread = GetCurrentThreadId();
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&InstallCrashLog, &g_self);
    AddVectoredExceptionHandler(1, OnVectored);
}
