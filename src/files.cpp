// Dossier des reglages et sauvegardes : le jeu lit "Personal" (Mes documents) dans le registre et y ajoute
// "\GTA Vice City User Files" (0x6019E0 au demarrage, 0x602240 ensuite). On lui donne le dossier du jeu :
// chaque instance a ses propres reglages et ses sauvegardes coop restent a part de celles du solo.
#include "util.h"
#include "vccoop.h"
#include <string.h>

static LSTATUS(WINAPI *o_RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);

static LSTATUS WINAPI h_RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size)
{
    if (g_cfg.localUserFiles && name && _stricmp(name, "Personal") == 0 && data && size) {
        char dir[MAX_PATH];
        lstrcpynA(dir, GameDir(), MAX_PATH);
        size_t n = strlen(dir);
        if (n && dir[n - 1] == '\\') dir[--n] = 0;
        if (n + 1 <= *size) {
            memcpy(data, dir, n + 1);
            *size = (DWORD)(n + 1);
            if (type) *type = REG_SZ;
            static bool logged;
            if (!logged) { Log("dossier des sauvegardes : %s\\GTA Vice City User Files", dir); logged = true; }
            return ERROR_SUCCESS;
        }
    }
    return o_RegQueryValueExA(key, name, reserved, type, data, size);
}

// CdStreamInit (0x4088E0) cree un semaphore NOMME "CdStream" : deux instances du jeu partagent alors le
// meme objet systeme et se volent leurs signaux, puis se figent ensemble dans CdStreamSync. Anonyme, il reste
// propre a chaque processus.
static HANDLE(WINAPI *o_CreateSemaphoreA)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCSTR);
static HANDLE WINAPI h_CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name)
{
    if (name && strcmp(name, "CdStream") == 0) name = NULL;
    return o_CreateSemaphoreA(sa, initial, maximum, name);
}

void InstallFileHooks()
{
    o_CreateSemaphoreA = (decltype(o_CreateSemaphoreA))HookImport("kernel32.dll", "CreateSemaphoreA", (void *)h_CreateSemaphoreA);
    o_RegQueryValueExA = (decltype(o_RegQueryValueExA))HookImport("advapi32.dll", "RegQueryValueExA", (void *)h_RegQueryValueExA);
}
