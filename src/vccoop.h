// VCCoop : coop des missions de GTA Vice City (gta-vc.exe 1.0 uniquement).
#pragma once
#include <windows.h>

#define VCCOOP_VERSION "2026.09.28za"
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
    bool friendlyFire;  // TirAmi=1 (hote) : les joueurs peuvent se blesser entre eux
    bool shareWanted;   // RecherchePartagee=1 : etoiles de police communes
    bool hostPolice;    // PoliceHote=1 : pres de l'hote, seule sa police existe et elle poursuit aussi les invites
    bool shareMoney;    // ArgentPartage=1 (hote) : l'argent donne par les missions va aussi aux invites
    int drawDistance;   // DistanceAffichage=200 (en %) : detail, passants et vehicules visibles plus loin
    int msaa;           // Anticrenelage=4 : 0, 2, 4 ou 8 echantillons (au prochain lancement)
    bool aniso;         // FiltrageAnisotrope=1 : textures nettes au loin (16x, trilineaire)
    bool sunShadows;    // OmbresSoleil=1 : ombres projetees du soleil (carte d'ombre, gfx.cpp)
    int shadowRes;      // OmbresResolution=4096 (ou 2048) : taille de la carte d'ombre
    bool waterReflections;   // RefletsEau=1 : la scene se reflete dans l'eau moderne
    bool modernWater;   // EauModerne=1 (rendu moderne) : eau turquoise, reflets, refraction, ecume
    int lightShadows;   // OmbresLumieres=4 : nombre de lumieres (0 a 4) qui projettent une ombre
    bool ambientOcclusion;   // OcclusionAmbiante=1 (rendu moderne) : coins et dessous des objets assombris
    bool moonShadows;   // OmbresLune=1 : la lune projette des ombres (plus faibles que le soleil)
    bool dynLights;     // LumieresDynamiques=1 (rendu moderne) : lampadaires, phares, explosions eclairent par pixel
    int renderer;       // Rendu=9 : Direct3D 9 par notre pont (rendu moderne, gfx9.cpp) ; 8 : Direct3D 8 d'origine
    int captureSecs;    // CaptureRendu=N (test) : image du rendu enregistree toutes les N s (dossier captures du jeu, rendu-*.bmp)
    bool freeCam;       // CameraLibre=1 : camera a la souris autour du vehicule, visee au clic droit
    float camSensitivity; // SensibiliteCamera=100 (en %)
    bool showNames;
    bool sharedMods;    // ModsPartages=1 : VCCoop\mods\ charge par le jeu et distribue aux invites (mods.cpp)
    bool keepWeapons;   // GarderArmes=1 : un invite mort ou arrete garde armes et argent
    bool respawnAtHost; // ReapparitionHote=1 : il reapparait pres de l'hote (0 par defaut : a l'hopital)     // AfficherPseudos=1 : pseudo au-dessus de la tete des autres joueurs
    char skin[24];      // Tenue=... : tenue choisie avec F7 (vide : celle du jeu)
    int logOpcodes;
    char traceScript[9];
    int watchPuppetField;   // diagnostic : surveille ce champ du Tommy distant (SurveilleTommy=0x24C)   // diagnostic : trace d'un script nomme    // diagnostic : chaque opcode des scripts de mission
    char autotest[16];
    int testModel;      // test : modele de la voiture de l'autotest "porte" (TestModele)  // instances de test : "passer" / "marche" (pilote la manette 0)    // diagnostic : scripts actifs dans le journal     // instances de test : nouvelle partie directement
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
bool GuestMayStartMission();   // script.cpp : invite au volant d'un vehicule de mission secondaire, ou achat d'immeuble
bool GuestSideMission();   // invite : il joue une mission secondaire (taxi, ambulance...) chez lui
bool IsPropertyPickup(uint32_t handle);   // script.cpp : icone d'immeuble creee par le script principal
void RegisterPropertyPickup(uint32_t handle);   // mirror.cpp : icone "a vendre" recreee par une mission de l'hote
void NotePropertyCollected();             // conditions.cpp : l'invite vient d'en ramasser une (achat a suivre)
// combat.cpp
void InstallCombatHooks();

// display.cpp
void InstallDisplay();
void UpdateHudScale();
bool MenuSqueezeActive();
float CoopMenuTextScale();   // menu.cpp : texte plus petit sur l'ecran COOP (beaucoup de lignes)
float MenuSqueezeFactor();
void SuspendMenuSqueeze(bool on);

// gfx.cpp : socle du rendu moderne (interception des dessins Direct3D 8)
void GfxHookDevice(void *dev);
void GfxBeforeReset();

// mods.cpp : mods partages (modeles remplaces, conduite, couleurs) et leur distribution
void InstallMods();
void ModsFrame();
bool ModsReady();      // invite : mods de l'hote telecharges (ou pas de serveur) ; hote : toujours
int ModsPercent();

// interface.cpp : texte des menus (blanc a contour, taille) et images de VCCoop\interface
void InstallInterface();
void InterfaceFrame();   // libere l'image de chargement une fois en jeu

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

bool TogglePassenger();   // coop.cpp : touches F / G (monter a bord du vehicule d un autre joueur, en descendre)
void InstallEnterHooks(); // coop.cpp
void InstallAsiLoader();  // asi.cpp : mods .asi du dossier du jeu
void InstallPlayers();    // players.cpp : pseudos a l'ecran

// coop.cpp : appele une fois par image, sur le fil du jeu, juste avant l'affichage.
void CoopFrame();
