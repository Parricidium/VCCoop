#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

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

// vccoop.ini : celui du dossier du jeu. Si ce dossier n'est pas modifiable (Program Files sans droits
// d'administrateur : les reglages changes dans le menu ne se gardaient pas), une copie dans
// %LOCALAPPDATA%\VCCoop\<dossier du jeu>ccoop.ini sert a la place (creee a partir de celui du jeu).
static char g_iniPath[MAX_PATH];

const char *IniPath()
{
    if (g_iniPath[0]) return g_iniPath;
    wsprintfA(g_iniPath, "%svccoop.ini", GameDir());
    HANDLE h = CreateFileA(g_iniPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return g_iniPath; }
    DWORD err = GetLastError();
    char base[MAX_PATH], alt[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH)) return g_iniPath;
    // Nom du sous-dossier : le chemin du jeu sans caracteres genants.
    char tag[MAX_PATH];
    int n = 0;
    for (const char *c = GameDir(); *c && n < 100; c++) tag[n++] = (*c == '\\' || *c == ':' || *c == ' ') ? '_' : *c;
    tag[n] = 0;
    wsprintfA(alt, "%s\\VCCoop", base);
    CreateDirectoryA(alt, NULL);
    wsprintfA(alt, "%s\\VCCoop\\%s", base, tag);
    CreateDirectoryA(alt, NULL);
    lstrcatA(alt, "\\vccoop.ini");
    if (GetFileAttributesA(alt) == INVALID_FILE_ATTRIBUTES) CopyFileA(g_iniPath, alt, TRUE);
    Log("reglages : %s non modifiable (erreur %lu), copie utilisee : %s", g_iniPath, err, alt);
    lstrcpynA(g_iniPath, alt, MAX_PATH);
    return g_iniPath;
}

// vccoop-joueur.ini : pseudo, adresse de l'hote, port et tenue, a cote de vccoop.ini. Il n'est pas dans le paquet :
// installer une nouvelle version (qui remplace vccoop.ini) ne les efface plus.
const char *PlayerIniPath()
{
    static char path[MAX_PATH];
    if (path[0]) return path;
    lstrcpynA(path, IniPath(), MAX_PATH);
    char *slash = strrchr(path, '\\');
    lstrcpyA(slash ? slash + 1 : path, "vccoop-joueur.ini");
    return path;
}

// Chaque partie a aussi son journal dans le dossier logs (vccoop-AAAA-MM-JJ_HH-MM-SS.log), garde : apres un
// plantage, on relance souvent le jeu avant de penser a envoyer le journal. Les 50 plus recents sont conserves.
static FILE *g_logKept;

static int CompareNames(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void OpenKeptLog()
{
    char dir[MAX_PATH], path[MAX_PATH], pattern[MAX_PATH];
    wsprintfA(dir, "%slogs", GameDir());
    CreateDirectoryA(dir, NULL);
    // Menage : les noms horodates se trient dans l'ordre chronologique ; on ne garde que les 49 derniers (+ celui-ci).
    wsprintfA(pattern, "%s\\vccoop-*.log", dir);
    static char names[512][64];
    int n = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (n < 512) lstrcpynA(names[n++], fd.cFileName, 64); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    qsort(names, n, sizeof(names[0]), CompareNames);
    for (int i = 0; i + 49 < n; i++) { wsprintfA(path, "%s\\%s", dir, names[i]); DeleteFileA(path); }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wsprintfA(path, "%s\\vccoop-%04d-%02d-%02d_%02d-%02d-%02d.log", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    g_logKept = fopen(path, "w");
}

void LogInit(const char *path)
{
    InitializeCriticalSection(&g_logLock);
    g_log = fopen(path, "w");   // vccoop.log, a cote du jeu : la partie en cours
    OpenKeptLog();
}

void Log(const char *fmt, ...)
{
    if (!g_log && !g_logKept) return;
    EnterCriticalSection(&g_logLock);
    char line[2048];
    int n = wsprintfA(line, "[%8lu] ", GetTickCount());
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = 0;
    FILE *files[2] = { g_log, g_logKept };
    for (FILE *f : files) if (f) { fputs(line, f); fputc('\n', f); fflush(f); }
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

void *MakeDetour(uintptr_t addr, const void *expected, size_t n, void *hook)
{
    if (memcmp((void *)addr, expected, n) != 0) {
        Log("detour : octets inattendus en 0x%06X, crochet non pose", (unsigned)addr);
        return NULL;
    }
    uint8_t *t = (uint8_t *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(t, expected, n);
    t[n] = 0xE9;
    *(int32_t *)(t + n + 1) = (int32_t)((addr + n) - ((uintptr_t)t + n + 5));
    PatchJump(addr, hook, n);
    return t;
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
