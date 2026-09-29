// VCCoop : coop des missions de GTA Vice City (gta-vc.exe 1.0 uniquement).
#pragma once
#include <windows.h>

#define VCCOOP_VERSION "2026.09.29r"
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
    int startSlot;          // -vccoop-partie (salon du lanceur) : 0 = nouvelle partie, 1..8 = charger cet emplacement, -1 = rien
    bool netAuto;
    int testMenu;
    char testMenuPlan[16];   // test : "creer" / "rejoindre" depuis l'ecran COOP       // test : ecran de menu a ouvrir au demarrage       // reseau demarre d'office (ligne de commande -vccoop, ou Reseau=1) ; sinon par le menu COOP
    bool logScripts;
    bool friendlyFire;  // TirAmi=1 (hote) : les joueurs peuvent se blesser entre eux
    bool shareWanted;   // RecherchePartagee=1 : etoiles de police communes
    bool hostPolice;    // PoliceHote=1 : pres de l'hote, seule sa police existe et elle poursuit aussi les invites
    bool shareMoney;    // ArgentPartage=1 (hote) : l'argent donne par les missions va aussi aux invites
    int popDensity;     // DensitePopulation=150 (en %, 50 a 300) : passants et voitures en meme temps
    bool ragdoll;       // CorpsMous=1 : corps mous des personnages morts ou percutes (ragdoll.cpp)
    int zonePop;        // ZonePopulation=150 (en %, 100 a 200) : zone ou naissent passants et voitures, et leur nombre
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
    bool smaa;          // SMAA=1 : anticrenelage d'image (bords en escalier), en plus ou a la place du MSAA
    bool bloom;         // Eclat=1 : neons, soleil, phares debordent en lumiere douce
    int grade;          // Etalonnage=1 : 0 Original, 1 Vice (couleurs "Miami 80"), 2 Film
    int sharpen;        // Nettete=40 (0 a 100) : netteté adaptative des textures
    bool softParticles; // ParticulesDouces=1 : fumee, poussiere, explosions sans coupure nette contre le decor
    bool lampLights;
    bool wetRoads;      // RoutesMouillees=1 : sol sombre et brillant sous la pluie (reflets, flaques), sols brillants des interieurs
    bool sunRays;       // RayonsSoleil=1 : faisceaux de soleil entre les immeubles et les palmiers
    bool windPlants;    // VegetationVent=1 : palmiers et arbres plient au vent
    bool beams;         // FaisceauxPhares=1 : cones des phares visibles dans la pluie et le brouillard
    bool haze;
    bool carReflections;   // RefletsVoitures=1 : la carrosserie reflete la rue, les neons et le ciel
    bool indirectLight;    // LumiereIndirecte=1 : les surfaces eclairees teintent leurs voisines (facade rose -> trottoir)          // Brume=1 : la ville se fond au loin dans une brume qui suit l'heure    // LampadairesEclairent=1 : reverberes, neons et enseignes eclairent vraiment (sans leur tache peinte)
    bool dynLights;     // LumieresDynamiques=1 (rendu moderne) : lampadaires, phares, explosions eclairent par pixel
    int renderer;       // Rendu=9 : Direct3D 9 par notre pont (rendu moderne, gfx9.cpp) ; 8 : Direct3D 8 d'origine
    int captureSecs;    // CaptureRendu=N (test) : image du rendu enregistree toutes les N s (dossier captures du jeu, rendu-*.bmp)
    bool fpsView;       // VuePremierePersonne=1 : la touche ToucheVue passe en vue depuis la tete de Tommy
    int fpsKey;         // ToucheVue=F6 : code de touche Windows (F1..F12, ou une lettre)
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
void ObjSyncFrame(bool inGame);   // objsync.cpp : decor renverse partage
void ObjSyncOnReliable(int from, const uint8_t *data, int len);
void InstallObjSync();             // objsync.cpp : decor casse partage (crochets ObjectDamage / vitres)
bool GuestMayStartMission();
void PuppetPassThrough(int player, uint32_t ms);   // coop.cpp : Tommy d'un joueur traversable un moment (percute)   // script.cpp : invite au volant d'un vehicule de mission secondaire, ou achat d'immeuble
bool MissionUnderway();    // hote : sa mission (hors INITIAL) ; invite : celle de l'hote ou sa mission secondaire
bool HostOnMission();      // invite : l'hote joue une mission (mirror.cpp) : pas de mission secondaire ici en meme temps
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
