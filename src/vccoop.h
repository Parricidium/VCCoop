// VCCoop : coop des missions de GTA Vice City (gta-vc.exe 1.0 uniquement).
#pragma once
#include <windows.h>

struct Config {
    bool windowed;
    bool borderless;
    bool widescreen;    // GrandEcran=1 : image au vrai format de l'ecran (16:9, 21:9...)    // Fenetre=2 : fenetre sans bordure qui couvre l'ecran
    int winX, winY;
    bool background;    // instance de test : fenetre jamais activee (a placer hors ecran avec FenetreX/Y)
    bool skipIntro;     // demarre sans les videos Rockstar/intro
    bool localUserFiles; // reglages/sauvegardes dans le dossier du jeu plutot que Mes documents
    int maxFps;         // 0 = pas de limite
    char playerName[32];
    bool host;          // Role=hote / invite
    char address[64];   // invite : adresse de l'hote
    int port;
    bool autoStart;
    bool netAuto;
    int testMenu;
    char testMenuPlan[16];   // test : "creer" / "rejoindre" depuis l'ecran COOP       // test : ecran de menu a ouvrir au demarrage       // reseau demarre d'office (ligne de commande -vccoop, ou Reseau=1) ; sinon par le menu COOP
    bool logScripts;
    int logOpcodes;
    char traceScript[9];
    int watchPuppetField;   // diagnostic : surveille ce champ du Tommy distant (SurveilleTommy=0x24C)   // diagnostic : trace d'un script nomme    // diagnostic : chaque opcode des scripts de mission
    char autotest[16];  // instances de test : "passer" / "marche" (pilote la manette 0)    // diagnostic : scripts actifs dans le journal     // instances de test : nouvelle partie directement
};
extern Config g_cfg;

// patches.cpp
void InstallGamePatches();

// files.cpp
void InstallFileHooks();

// window.cpp
void InstallWindowHooks();
HWND GameWindow();
bool GameHasFocus();

// crash.cpp
void InstallCrashLog();

// watchdog.cpp
void StartWatchdog();
void WatchAddress(uintptr_t addr);   // diagnostic : point d'arret materiel en ecriture
void WatchdogFrame();

// script.cpp
void InstallScriptHooks();
// combat.cpp
void InstallCombatHooks();

// display.cpp
void InstallDisplay();
void UpdateHudScale();
bool MenuSqueezeActive();
float MenuSqueezeFactor();

// population.cpp
void InstallPopulation();

// menu.cpp
void InstallMenu();
void MenuWindowCreated(HWND hwnd);
void MenuFrame();
void CoopStartNetwork();
bool CoopNetworkStarted();

// autotest.cpp
void AutotestFrame();

void TogglePassenger();   // coop.cpp : touche G

// coop.cpp : appele une fois par image, sur le fil du jeu, juste avant l'affichage.
void CoopFrame();
