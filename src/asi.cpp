// Chargement des mods .asi (comme l'"Ultimate ASI Loader", qui porte souvent le meme nom dinput8.dll et ne peut
// donc pas cohabiter avec VCCoop) : les .asi du dossier du jeu, de scripts\ et de plugins\ sont charges au demarrage
// du jeu, juste avant WinMain (premier appel du CRT a GetStartupInfoA : tous les DLL sont alors initialises).
// ChargerASI=0 dans vccoop.ini pour ne pas les charger.
#include "util.h"
#include "vccoop.h"

static int LoadFolder(const char *sub)
{
    char pattern[MAX_PATH];
    wsprintfA(pattern, "%s%s*.asi", GameDir(), sub);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[MAX_PATH];
        wsprintfA(path, "%s%s%s", GameDir(), sub, fd.cFileName);
        // Deja charge par un autre chargeur : LoadLibrary renvoie le meme module sans le reinitialiser.
        HMODULE m = LoadLibraryA(path);
        if (m) { n++; Log("asi : %s%s charge", sub, fd.cFileName); }
        else Log("asi : %s%s n'a pas pu etre charge (erreur %lu)", sub, fd.cFileName, GetLastError());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

typedef void(WINAPI *GetStartupInfoA_t)(LPSTARTUPINFOA);
static GetStartupInfoA_t o_GetStartupInfoA;

static void WINAPI h_GetStartupInfoA(LPSTARTUPINFOA si)
{
    static bool done;
    if (!done) {
        done = true;
        int n = LoadFolder("") + LoadFolder("scripts\\") + LoadFolder("plugins\\");
        if (n) Log("asi : %d mod(s) charge(s)", n);
    }
    o_GetStartupInfoA(si);
}

void InstallAsiLoader()
{
    char ini[MAX_PATH];
    wsprintfA(ini, "%svccoop.ini", GameDir());
    if (!GetPrivateProfileIntA("VCCoop", "ChargerASI", 1, ini)) return;
    o_GetStartupInfoA = (GetStartupInfoA_t)HookImport("kernel32.dll", "GetStartupInfoA", (void *)h_GetStartupInfoA);
    if (!o_GetStartupInfoA) Log("asi : GetStartupInfoA introuvable, mods .asi non charges");
}
