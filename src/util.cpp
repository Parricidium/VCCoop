#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE *g_log;
static CRITICAL_SECTION g_logLock;
static char g_gameDir[MAX_PATH];

const char *GameDir()
{
    if (!g_gameDir[0]) {
        GetModuleFileNameA(NULL, g_gameDir, MAX_PATH);
        char *slash = strrchr(g_gameDir, '\\');
        if (slash) slash[1] = 0;
    }
    return g_gameDir;
}

void LogInit(const char *path)
{
    InitializeCriticalSection(&g_logLock);
    g_log = fopen(path, "w");
}

void Log(const char *fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    fprintf(g_log, "[%8lu] ", GetTickCount());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

void Patch(uintptr_t addr, const void *bytes, size_t n)
{
    DWORD old;
    VirtualProtect((void *)addr, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)addr, bytes, n);
    VirtualProtect((void *)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, n);
}

void PatchNop(uintptr_t addr, size_t n)
{
    uint8_t buf[64];
    memset(buf, 0x90, n);
    Patch(addr, buf, n);
}

static void PatchRel(uintptr_t addr, void *dst, size_t n, uint8_t op)
{
    uint8_t buf[64];
    memset(buf, 0x90, n);
    buf[0] = op;
    int32_t rel = (int32_t)((uintptr_t)dst - (addr + 5));
    memcpy(buf + 1, &rel, 4);
    Patch(addr, buf, n);
}

void PatchCall(uintptr_t addr, void *dst, size_t n) { PatchRel(addr, dst, n, 0xE8); }
void PatchJump(uintptr_t addr, void *dst, size_t n) { PatchRel(addr, dst, n, 0xE9); }

void *PatchPointer(void **slot, void *value)
{
    void *old = *slot;
    Patch((uintptr_t)slot, &value, sizeof(value));
    return old;
}

void *HookImport(const char *dll, const char *func, void *hook)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char *)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, func) == 0)
                return PatchPointer((void **)&iat->u1.Function, hook);
        }
    }
    Log("HookImport : %s!%s introuvable", dll, func);
    return NULL;
}
