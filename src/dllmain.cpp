// Point d'entree : VCCoop se fait passer pour dinput8.dll (le jeu l'importe statiquement)
// et renvoie DirectInput8Create vers la vraie DLL du systeme.
#include "util.h"
#include "vccoop.h"
#include "conditions.h"
#include "interp.h"
#include "camera.h"
#include "net.h"
#include "anims.h"
#include "vehicles.h"
#include <stdio.h>
#include <stdlib.h>

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
    HRESULT hr = real ? real(inst, ver, riid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) HookDirectInput(*out);   // camera.cpp : clic droit de la souris
    return hr;
}

static void LoadConfig()
{
    char ini[MAX_PATH];
    lstrcpynA(ini, IniPath(), MAX_PATH);
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
    g_cfg.friendlyFire = GetPrivateProfileIntA("VCCoop", "TirAmi", 1, ini) != 0;
    g_cfg.shareWanted = GetPrivateProfileIntA("VCCoop", "RecherchePartagee", 0, ini) != 0;
    g_cfg.hostPolice = GetPrivateProfileIntA("VCCoop", "PoliceHote", 1, ini) != 0;
    g_cfg.shareMoney = GetPrivateProfileIntA("VCCoop", "ArgentPartage", 1, ini) != 0;
    g_cfg.keepWeapons = GetPrivateProfileIntA("VCCoop", "GarderArmes", 1, ini) != 0;
    g_cfg.respawnAtHost = GetPrivateProfileIntA("VCCoop", "ReapparitionHote", 0, ini) != 0;
    g_cfg.drawDistance = GetPrivateProfileIntA("VCCoop", "DistanceAffichage", 200, ini);
    g_cfg.zonePop = GetPrivateProfileIntA("VCCoop", "ZonePopulation", 150, ini);
    g_cfg.popDensity = GetPrivateProfileIntA("VCCoop", "DensitePopulation", 150, ini);
    g_cfg.ragdoll = GetPrivateProfileIntA("VCCoop", "CorpsMous", 1, ini) != 0;
    if (g_cfg.popDensity < 50) g_cfg.popDensity = 50;
    if (g_cfg.popDensity > 300) g_cfg.popDensity = 300;
    if (g_cfg.zonePop < 100) g_cfg.zonePop = 100;
    if (g_cfg.zonePop > 200) g_cfg.zonePop = 200;
    g_cfg.msaa = GetPrivateProfileIntA("VCCoop", "Anticrenelage", 4, ini);
    g_cfg.aniso = GetPrivateProfileIntA("VCCoop", "FiltrageAnisotrope", 1, ini) != 0;
    g_cfg.sunShadows = GetPrivateProfileIntA("VCCoop", "OmbresSoleil", 1, ini) != 0;
    g_cfg.shadowRes = GetPrivateProfileIntA("VCCoop", "OmbresResolution", 4096, ini);
    g_cfg.renderer = GetPrivateProfileIntA("VCCoop", "Rendu", 9, ini);
    g_cfg.modernWater = GetPrivateProfileIntA("VCCoop", "EauModerne", 1, ini) != 0;
    g_cfg.waterReflections = GetPrivateProfileIntA("VCCoop", "RefletsEau", 1, ini) != 0;
    g_cfg.dynLights = GetPrivateProfileIntA("VCCoop", "LumieresDynamiques", 1, ini) != 0;
    g_cfg.lightShadows = GetPrivateProfileIntA("VCCoop", "OmbresLumieres", 4, ini);
    if (g_cfg.lightShadows < 0) g_cfg.lightShadows = 0;
    if (g_cfg.lightShadows > 4) g_cfg.lightShadows = 4;
    g_cfg.moonShadows = GetPrivateProfileIntA("VCCoop", "OmbresLune", 1, ini) != 0;
    g_cfg.ambientOcclusion = GetPrivateProfileIntA("VCCoop", "OcclusionAmbiante", 1, ini) != 0;
    g_cfg.smaa = GetPrivateProfileIntA("VCCoop", "SMAA", 1, ini) != 0;
    g_cfg.bloom = GetPrivateProfileIntA("VCCoop", "Eclat", 1, ini) != 0;
    g_cfg.grade = GetPrivateProfileIntA("VCCoop", "Etalonnage", 1, ini);
    if (g_cfg.grade < 0 || g_cfg.grade > 2) g_cfg.grade = 1;
    g_cfg.sharpen = GetPrivateProfileIntA("VCCoop", "Nettete", 40, ini);
    if (g_cfg.sharpen < 0) g_cfg.sharpen = 0;
    if (g_cfg.sharpen > 100) g_cfg.sharpen = 100;
    g_cfg.softParticles = GetPrivateProfileIntA("VCCoop", "ParticulesDouces", 1, ini) != 0;
    g_cfg.lampLights = GetPrivateProfileIntA("VCCoop", "LampadairesEclairent", 1, ini) != 0;
    g_cfg.wetRoads = GetPrivateProfileIntA("VCCoop", "RoutesMouillees", 1, ini) != 0;
    g_cfg.sunRays = GetPrivateProfileIntA("VCCoop", "RayonsSoleil", 1, ini) != 0;
    g_cfg.windPlants = GetPrivateProfileIntA("VCCoop", "VegetationVent", 1, ini) != 0;
    g_cfg.beams = GetPrivateProfileIntA("VCCoop", "FaisceauxPhares", 1, ini) != 0;
    g_cfg.haze = GetPrivateProfileIntA("VCCoop", "Brume", 1, ini) != 0;
    g_cfg.carReflections = GetPrivateProfileIntA("VCCoop", "RefletsVoitures", 1, ini) != 0;
    g_cfg.indirectLight = GetPrivateProfileIntA("VCCoop", "LumiereIndirecte", 1, ini) != 0;
    g_cfg.captureSecs = GetPrivateProfileIntA("VCCoop", "CaptureRendu", 0, ini);
    g_cfg.freeCam = GetPrivateProfileIntA("VCCoop", "CameraLibre", 1, ini) != 0;
    g_cfg.fpsView = GetPrivateProfileIntA("VCCoop", "VuePremierePersonne", 1, ini) != 0;
    {
        char k[16];
        GetPrivateProfileStringA("VCCoop", "ToucheVue", "F6", k, sizeof(k), ini);
        CharUpperA(k);
        if (k[0] == 'F' && k[1] >= '1' && k[1] <= '9') g_cfg.fpsKey = VK_F1 + atoi(k + 1) - 1;
        else if (k[0] && !k[1]) g_cfg.fpsKey = (unsigned char)k[0];
        else g_cfg.fpsKey = VK_F6;
    }
    g_cfg.camSensitivity = GetPrivateProfileIntA("VCCoop", "SensibiliteCamera", 100, ini) / 100.0f;
    g_cfg.showNames = GetPrivateProfileIntA("VCCoop", "AfficherPseudos", 1, ini) != 0;
    g_cfg.sharedMods = GetPrivateProfileIntA("VCCoop", "ModsPartages", 1, ini) != 0;
    GetPrivateProfileStringA("VCCoop", "Tenue", "", g_cfg.skin, sizeof(g_cfg.skin), ini);
    GetPrivateProfileStringA("VCCoop", "Autotest", "", g_cfg.autotest, sizeof(g_cfg.autotest), ini);
    g_cfg.logOpcodes = GetPrivateProfileIntA("VCCoop", "JournalOpcodes", 0, ini);
    g_cfg.testModel = GetPrivateProfileIntA("VCCoop", "TestModele", 0, ini);
    GetPrivateProfileStringA("VCCoop", "TraceScript", "", g_cfg.traceScript, sizeof(g_cfg.traceScript), ini);
    g_cfg.watchPuppetField = GetPrivateProfileIntA("VCCoop", "SurveilleTommy", 0, ini);
    g_cfg.autoStart = GetPrivateProfileIntA("VCCoop", "AutoDemarrer", 0, ini) != 0;
    g_cfg.borderless = GetPrivateProfileIntA("VCCoop", "Fenetre", 1, ini) == 2;
    g_cfg.widescreen = GetPrivateProfileIntA("VCCoop", "GrandEcran", 1, ini) != 0;

    // Pseudo, adresse, port et tenue : dans vccoop-joueur.ini (absent du paquet, une mise a jour ne les efface pas).
    // Premier lancement : repris de vccoop.ini.
    const char *pj = PlayerIniPath();
    if (GetFileAttributesA(pj) == INVALID_FILE_ATTRIBUTES) {
        char port[16];
        wsprintfA(port, "%d", g_cfg.port);
        WritePrivateProfileStringA("VCCoop", "Pseudo", g_cfg.playerName, pj);
        WritePrivateProfileStringA("VCCoop", "Adresse", g_cfg.address, pj);
        WritePrivateProfileStringA("VCCoop", "Port", port, pj);
        WritePrivateProfileStringA("VCCoop", "Tenue", g_cfg.skin, pj);
        Log("reglages : %s cree (pseudo, adresse, port et tenue repris de vccoop.ini)", pj);
    } else {
        GetPrivateProfileStringA("VCCoop", "Pseudo", g_cfg.playerName, g_cfg.playerName, sizeof(g_cfg.playerName), pj);
        GetPrivateProfileStringA("VCCoop", "Adresse", g_cfg.address, g_cfg.address, sizeof(g_cfg.address), pj);
        g_cfg.port = GetPrivateProfileIntA("VCCoop", "Port", g_cfg.port, pj);
        GetPrivateProfileStringA("VCCoop", "Tenue", g_cfg.skin, g_cfg.skin, sizeof(g_cfg.skin), pj);
    }

    // Ligne de commande (raccourcis Heberger / Rejoindre) : -vccoop hote | -vccoop invite <adresse>
    const char *cmd = GetCommandLineA();
    g_cfg.testMenu = GetPrivateProfileIntA("VCCoop", "TestMenu", 0, ini);
    GetPrivateProfileStringA("VCCoop", "TestMenuPlan", "", g_cfg.testMenuPlan, sizeof(g_cfg.testMenuPlan), ini);
    g_cfg.netAuto = GetPrivateProfileIntA("VCCoop", "Reseau", 0, ini) != 0;
    // Salon du lanceur : l'hote entre directement en partie (-vccoop-partie nouvelle | <emplacement 1..8>).
    g_cfg.startSlot = -1;
    if (const char *part = strstr(cmd, "-vccoop-partie ")) {
        char what[16] = "";
        sscanf(part + 15, "%15s", what);
        int slot = atoi(what);
        if (_stricmp(what, "nouvelle") == 0) { g_cfg.startSlot = 0; g_cfg.autoStart = true; }
        else if (slot >= 1 && slot <= 8) { g_cfg.startSlot = slot; g_cfg.autoStart = false; }   // (AutoDemarrer des instances de test ignore)
    }
    const char *opt = strstr(cmd, "-vccoop ");
    if (opt) {
        g_cfg.netAuto = true;
        char role[16] = "", addr[64] = "";
        sscanf(opt + 8, "%15s %63s", role, addr);
        if (_stricmp(role, "hote") == 0) g_cfg.host = true;
        else if (_stricmp(role, "invite") == 0) {
            g_cfg.host = false;
            if (addr[0] && addr[0] != '-') {
                lstrcpynA(g_cfg.address, addr, sizeof(g_cfg.address));
                WritePrivateProfileStringA("VCCoop", "Adresse", g_cfg.address, pj);   // retrouvee dans le menu la prochaine fois
            }
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
    if (reason == DLL_PROCESS_DETACH) { NetSendBye(); return TRUE; }   // fermeture du jeu : les autres le savent tout de suite
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    char logPath[MAX_PATH];
    wsprintfA(logPath, "%svccoop.log", GameDir());
    LogInit(logPath);
    LoadConfig();
    Log("VCCoop %s charge, dossier %s", VCCOOP_VERSION, GameDir());
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
    InstallMods();
    InstallScriptHooks();
    InstallCombatHooks();
    InstallMenu();
    InstallDisplay();
    InstallInterface();
    InstallPopulation();
    InstallObjSync();
    InstallVehicleAudio();
    InstallConditions();
    InstallInterp();
    InstallEnterHooks();
    InstallAnimGuards();
    InstallAsiLoader();
    InstallPlayers();
    InstallCamera();
    return TRUE;
}
