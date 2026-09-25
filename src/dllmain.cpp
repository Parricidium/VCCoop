// Point d'entree : VCCoop se fait passer pour dinput8.dll (le jeu l'importe statiquement)
// et renvoie DirectInput8Create vers la vraie DLL du systeme.
#include "util.h"
#include "vccoop.h"
#include <stdio.h>

Config g_cfg;

typedef HRESULT(WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, const GUID &, LPVOID *, void *);

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, const GUID &riid, LPVOID *out, void *outer)
{
    static DirectInput8Create_t real;
    if (!real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        lstrcatA(path, "\\dinput8.dll");
        real = (DirectInput8Create_t)GetProcAddress(LoadLibraryA(path), "DirectInput8Create");
    }
    return real ? real(inst, ver, riid, out, outer) : E_FAIL;
}

static void LoadConfig()
{
    char ini[MAX_PATH];
    wsprintfA(ini, "%svccoop.ini", GameDir());
    g_cfg.windowed = GetPrivateProfileIntA("VCCoop", "Fenetre", 1, ini) != 0;
    g_cfg.winX = GetPrivateProfileIntA("VCCoop", "FenetreX", 40, ini);
    g_cfg.winY = GetPrivateProfileIntA("VCCoop", "FenetreY", 40, ini);
    g_cfg.background = GetPrivateProfileIntA("VCCoop", "ArrierePlan", 0, ini) != 0;
    g_cfg.skipIntro = GetPrivateProfileIntA("VCCoop", "SansIntro", 1, ini) != 0;
    g_cfg.localUserFiles = GetPrivateProfileIntA("VCCoop", "SauvegardesLocales", 1, ini) != 0;
    g_cfg.maxFps = GetPrivateProfileIntA("VCCoop", "ImagesParSeconde", 30, ini);
    GetPrivateProfileStringA("VCCoop", "Pseudo", "Tommy", g_cfg.playerName, sizeof(g_cfg.playerName), ini);
    char role[16];
    GetPrivateProfileStringA("VCCoop", "Role", "hote", role, sizeof(role), ini);
    g_cfg.host = _stricmp(role, "invite") != 0;
    GetPrivateProfileStringA("VCCoop", "Adresse", "127.0.0.1", g_cfg.address, sizeof(g_cfg.address), ini);
    g_cfg.port = GetPrivateProfileIntA("VCCoop", "Port", 7790, ini);
    g_cfg.logScripts = GetPrivateProfileIntA("VCCoop", "JournalScripts", 0, ini) != 0;
    GetPrivateProfileStringA("VCCoop", "Autotest", "", g_cfg.autotest, sizeof(g_cfg.autotest), ini);
    g_cfg.logOpcodes = GetPrivateProfileIntA("VCCoop", "JournalOpcodes", 0, ini) != 0;
    g_cfg.autoStart = GetPrivateProfileIntA("VCCoop", "AutoDemarrer", 0, ini) != 0;
    g_cfg.borderless = GetPrivateProfileIntA("VCCoop", "Fenetre", 1, ini) == 2;

    // Ligne de commande (raccourcis Heberger / Rejoindre) : -vccoop hote | -vccoop invite <adresse>
    const char *cmd = GetCommandLineA();
    g_cfg.testMenu = GetPrivateProfileIntA("VCCoop", "TestMenu", 0, ini);
    GetPrivateProfileStringA("VCCoop", "TestMenuPlan", "", g_cfg.testMenuPlan, sizeof(g_cfg.testMenuPlan), ini);
    g_cfg.netAuto = GetPrivateProfileIntA("VCCoop", "Reseau", 0, ini) != 0;
    const char *opt = strstr(cmd, "-vccoop ");
    if (opt) {
        g_cfg.netAuto = true;
        char role[16] = "", addr[64] = "";
        sscanf(opt + 8, "%15s %63s", role, addr);
        if (_stricmp(role, "hote") == 0) g_cfg.host = true;
        else if (_stricmp(role, "invite") == 0) {
            g_cfg.host = false;
            if (addr[0] && addr[0] != '-') lstrcpynA(g_cfg.address, addr, sizeof(g_cfg.address));
        }
    }
}

// Verifie qu'on tourne bien sur le 1.0 : l'octet de tete de CRunningScript::ProcessOneCommand
// (inc word [CTheScripts::CommandsExecuted]) n'existe qu'a cette adresse dans cette version.
static bool IsVersion10()
{
    static const unsigned char sig[] = { 0x66, 0xFF, 0x05, 0x66, 0x0A, 0xA1, 0x00 };
    return memcmp((void *)0x44FBE0, sig, sizeof(sig)) == 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    char logPath[MAX_PATH];
    wsprintfA(logPath, "%svccoop.log", GameDir());
    LogInit(logPath);
    LoadConfig();
    Log("VCCoop charge, dossier %s", GameDir());
    InstallCrashLog();

    if (!IsVersion10()) {
        Log("gta-vc.exe n'est pas la version 1.0 : VCCoop desactive");
        MessageBoxA(NULL, "VCCoop a besoin de gta-vc.exe en version 1.0.\nLe mod est desactive.", "VCCoop", MB_ICONWARNING);
        return TRUE;
    }
    StartWatchdog();
    InstallGamePatches();
    InstallWindowHooks();
    InstallFileHooks();
    InstallScriptHooks();
    InstallCombatHooks();
    InstallMenu();
    return TRUE;
}
