// Autotest (instances de test uniquement) : pilote la manette 0 en ecrivant l'etat "manette PC temporaire"
// que CPad::Update fusionne dans NewState au debut de l'image suivante. Aucune touche n'est injectee dans
// Windows : rien ne peut partir dans une autre fenetre.
//   Autotest=passer : tape la croix pendant les cinematiques pour les passer
//   Autotest=marche : idem, puis marche en rond une fois qu'on a la main
//   Autotest=voiture : comme marche, mais monte dans le vehicule le plus proche, roule 4 s puis descend
//   Autotest=taxi : monte dans le vehicule le plus proche, attend un passager (autre joueur), roule 4 s
//   Autotest=passager : des qu'un autre joueur est au volant pres de nous, monte a cote de lui (touche G), et
//                       redescend (G) 6 s plus tard
//   Autotest=bagarre : toutes les 2 s, alternativement un coup de poing (rond) et un saut (carre)
//   Autotest=cogneur : toutes les 2 s, le joueur local "frappe" (10 points, a mains nues) le Tommy du joueur voisin
//   Autotest=boxeur : se place a 1 m du Tommy du joueur voisin, face a lui, et lui donne un coup de poing toutes les 2 s
//   Autotest=mort : (invite) prend un pistolet, meurt, et dit ou il reapparait et s'il a garde son arme
//   Autotest=coupure : (invite) coupe le reseau 12 s au bout de 10 s de jeu (doit revenir sans recharger)
//   Autotest=moto : (hote) fait apparaitre un Faggio a cote de lui, s'assoit dessus, roule doucement par moments
//   Autotest=cible : (hote) cree un personnage de mission a cote de lui ; toutes les 3 s il "blesse" le Tommy de l'invite
//   Autotest=frappe : (invite) toutes les 2 s, inflige 25 points a la copie du personnage de mission le plus proche
//   Autotest=histoire : (hote) passe les cinematiques et se teleporte sur le dernier objectif / point de contact
//   Autotest=tireur : (hote) prend un pistolet et tire une balle par seconde droit devant
//   Autotest=sauvecharge : (hote) sauvegarde dans l'emplacement 1 puis recharge cette sauvegarde (une fois)
//   Autotest=loin : (invite) passe les cinematiques, puis au bout de 30 s se teleporte a 300 m (population locale)
//   Autotest=objectif : se teleporte sur le dernier cylindre de mission actif (invite : ceux de l'hote)
//   Autotest=principal : (hote) se teleporte sur les cylindres du script principal (lance les missions)
//   Autotest=cours : cycles de 3 s : marche, course, sprint, arret (tourne un peu pour rester dans la zone)
//   Autotest=rejoindre : idem, puis se teleporte devant le joueur 0, un peu de cote (une fois)
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include "net.h"
#include "vehicles.h"
#include "entities.h"
#include "mirror.h"
#include "combat.h"
#include "saveshare.h"
#include "conditions.h"
#include "seats.h"
#include <math.h>
#include <string.h>

using namespace game;

// CControllerState (0x2A octets, des short) : LeftStickX +0, LeftStickY +2, ..., Cross +0x20
enum { PAD_LSTICK_X = 0x00, PAD_LSTICK_Y = 0x02, PAD_SQUARE = 0x1C, PAD_TRIANGLE = 0x1E, PAD_CROSS = 0x20, PAD_CIRCLE = 0x22 };
static uint8_t *PadJoyState() { return (uint8_t *)0x7DBCB0 + 0x96; }   // Pads[0].PCTempJoyState
static short &PadDisableControls() { return *(short *)(0x7DBCB0 + 0xF0); }
static bool CutsceneRunning() { return *(bool *)0xA10AB2; }

static void Press(int field, short value) { *(short *)(PadJoyState() + field) = value; }

void AutotestFrame()
{
    if (!g_cfg.autotest[0] || GameState() != GS_PLAYING || !FindPlayerPed()) return;
    static uint32_t frame, controlSince;
    frame++;

    if (CutsceneRunning() || PadDisableControls()) {
        controlSince = 0;
        if (frame % 45 == 0) Press(PAD_CROSS, 255);   // une pression toutes les 1,5 s
        return;
    }
    if (!controlSince) {
        controlSince = frame;
        Log("autotest : le joueur a la main (image %u)", frame);
    }
    if (_stricmp(g_cfg.autotest, "rejoindre") == 0) {
        static bool done;
        const NetPlayer &host = g_players[g_localId == 0 ? 1 : 0];   // l'hote va aupres du joueur 1
        if (!done && frame - controlSince > 60 && g_localId >= 0 && host.connected && host.state.inGame) {
            done = true;
            void *ped = FindPlayerPed();
            Vec3 &p = Pos(ped);
            // 8 m devant l'hote et 2,5 m sur sa droite (avant = (-sin h, cos h), droite = (cos h, sin h)) :
            // dans le champ de sa camera sans etre cache par son corps.
            float hh = host.state.heading, fx = -sinf(hh), fy = cosf(hh), rx = cosf(hh), ry = sinf(hh);
            p = { host.state.pos[0] + fx * 8.0f + rx * 2.5f, host.state.pos[1] + fy * 8.0f + ry * 2.5f, host.state.pos[2] + 0.5f };
            MoveSpeed(ped) = { 0, 0, 0 };
            float h = hh + 3.14159f;
            SetHeadingMatrix(ped, h);
            Heading(ped) = HeadingGoal(ped) = h;
            Log("autotest : teleporte pres de l'hote (%.1f %.1f %.1f)", p.x, p.y, p.z);
        }
    }
    if (_stricmp(g_cfg.autotest, "taxi") == 0) {
        static uint32_t entered, drive;
        uint32_t t = frame - controlSince;
        void *ped = FindPlayerPed();
        if (!entered && t > 150) { entered = frame; Press(PAD_TRIANGLE, 255); Log("autotest : triangle (taxi)"); }
        if (entered && !drive && InVehicle(ped) && PedVehicle(ped)) {
            uint32_t myId = NetVehicleId(PedVehicle(ped));
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (i != g_localId && g_players[i].connected && g_players[i].state.inVehicle &&
                    g_players[i].state.vehicleId == myId && g_players[i].state.seat > 0) {
                    drive = frame;
                    Log("autotest : passager a bord (joueur %d), je roule ; radio %d -> 3", i, *(int *)(0x980038 + 0x3984));
                    int32_t radio[2] = { 3, -1 };
                    MirrorLocal(0x041E, 2, radio);   // change de station (le passager doit suivre)
                }
        }
        if (drive && frame - drive > 60 && frame - drive < 180) Press(PAD_CROSS, 255);
        return;
    }
    if (_stricmp(g_cfg.autotest, "bagarre") == 0) {
        uint32_t t = frame - controlSince;
        if (t < 150) return;
        uint32_t k = (t - 150) % 120;
        if (k < 4) Press(((t - 150) / 120) % 2 ? PAD_SQUARE : PAD_CIRCLE, 255);
        if (k == 0) Log("autotest : %s", ((t - 150) / 120) % 2 ? "saut" : "coup de poing");
        return;
    }
    bool onNpc = _stricmp(g_cfg.autotest, "boxeurpnj") == 0;   // (invite) la copie d'un personnage de mission
    if (onNpc || _stricmp(g_cfg.autotest, "boxeur") == 0) {
        uint32_t t = frame - controlSince;
        void *victim = onNpc ? NULL : PuppetPed(g_localId == 0 ? 1 : 0);
        if (onNpc && PuppetPed(0)) {   // la copie la plus proche de l'hote (la cible qu'il a creee devant lui)
            Pool *pool = PedPool();
            Vec3 hp = Pos(PuppetPed(0));
            float best = 100.0f;
            for (int i = 0; i < pool->size; i++) {
                if (pool->flags[i] & 0x80) continue;
                void *p = pool->objects + i * PED_POOL_ENTRY;
                if (!IsGhostPed(p) || Health(p) <= 0 || InVehicle(p)) continue;
                float dx = Pos(p).x - hp.x, dy = Pos(p).y - hp.y, d = dx * dx + dy * dy;
                if (d < best) { best = d; victim = p; }
            }
        }
        void *me = FindPlayerPed();
        if (!victim || t < 150) return;
        uint32_t k = (t - 150) % 60;
        if (k == 0) {
            float h = Heading(victim);
            Vec3 v = Pos(victim);
            Pos(me) = { v.x + sinf(h) * 1.0f, v.y - cosf(h) * 1.0f, v.z };   // derriere lui (la ou il y a de la place)
            MoveSpeed(me) = { 0, 0, 0 };
            float face = atan2f(-(v.x - Pos(me).x), v.y - Pos(me).y);   // tourne vers lui (avant = (-sin, cos))
            SetHeadingMatrix(me, face);
            Heading(me) = HeadingGoal(me) = face;
            Log("autotest : coup de poing sur l'autre joueur");
        }
        if (onNpc) { if (k == 3) { TestMeleeHit(victim); Log("autotest : coup simule sur la copie"); } }
        else if (k >= 2 && k < 5) Press(PAD_CIRCLE, 255);
        return;
    }
    if (_stricmp(g_cfg.autotest, "cogneur") == 0) {
        uint32_t t = frame - controlSince;
        void *victim = PuppetPed(g_localId == 0 ? 1 : 0);
        if (victim && t > 150 && t % 60 == 0) {
            ((bool(__thiscall *)(void *, void *, int, float, int, uint8_t))0x525B20)(victim, FindPlayerPed(), 0, 10.0f, 0, 0);
            Log("autotest : je frappe l'autre joueur");
        }
        return;
    }
    if (_stricmp(g_cfg.autotest, "grenade") == 0) {   // grenade toutes les 4 s, puis lance-flammes
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        static int armed;
        int weapon = t < 600 ? 12 : 31;
        if (armed != weapon && t > 100) {
            int model = *(int *)(0x782A14 + weapon * 0x64 + 0x54);
            if (model > 0 && !HasModelLoaded(model)) { RequestModel(model, 1); return; }
            GiveWeapon(me, weapon, 50); SetCurrentWeapon(me, weapon);
            armed = weapon;
            Log("autotest : arme %d en main", weapon);
        }
        if (armed == 12 && t > 150 && t % 120 < 20) Press(PAD_CIRCLE, 255);
        if (armed == 31 && t % 120 < 40) Press(PAD_CIRCLE, 255);
        return;
    }
    if (_stricmp(g_cfg.autotest, "recherche") == 0) {   // (invite) 2 etoiles au bout de 5 s de jeu
        static bool done;
        void *w = Field<void *>(FindPlayerPed(), 0x5F4);
        if (!done && w && frame - controlSince > 150) {
            done = true;
            ((void(__thiscall *)(void *, int))0x4D1FA0)(w, 2);
            Log("autotest : je me fais rechercher (2 etoiles)");
        }
        return;
    }
    if (_stricmp(g_cfg.autotest, "coupure") == 0) {
        extern uint32_t g_netMuteUntil;
        static bool done;
        if (!done && frame - controlSince > 300) {
            done = true;
            g_netMuteUntil = GetTickCount() + 12000;
            Log("autotest : coupure reseau de 12 s");
        }
        return;
    }
    if (_stricmp(g_cfg.autotest, "mort") == 0) {
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        static int step;
        if (step == 0 && t > 100) {
            int model = *(int *)(0x782A14 + 17 * 0x64 + 0x54);
            if (!HasModelLoaded(model)) { RequestModel(model, 1); return; }
            GiveWeapon(me, 17, 60); SetCurrentWeapon(me, 17);
            step = 1;
            Log("autotest : pistolet en main (creneau %d), argent %d", CurrentWeaponSlot(me), *(int *)(0x94AD28 + 0xA0));
        } else if (step == 1 && t > 200) {
            Health(me) = 0.0f;
            step = 2;
            Log("autotest : je meurs en %.1f %.1f %.1f", Pos(me).x, Pos(me).y, Pos(me).z);
        } else if (step == 2 && *(int *)(0x94AD28 + 0xCC) == 0 && Health(me) > 0 && t > 400) {
            step = 3;
            char w[128]; int n = 0;
            for (int s = 0; s < 10; s++) n += wsprintfA(w + n, " %d", WeaponTypeInSlot(me, s));
            Log("autotest : de retour en %.1f %.1f %.1f, armes%s, argent %d", Pos(me).x, Pos(me).y, Pos(me).z, w, *(int *)(0x94AD28 + 0xA0));
        }
        return;
    }
    if (_stricmp(g_cfg.autotest, "moto") == 0) {
        static void *bike;
        static uint32_t seated;
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        enum { MI_FAGGIO = 192 };
        if (!bike && !seated && t > 60) {
            if (!HasModelLoaded(MI_FAGGIO)) { RequestModel(MI_FAGGIO, 1); return; }
            void *v = VehicleAlloc();
            BikeCtor(v, MI_FAGGIO, VEHICLE_MISSION);
            float h = Heading(me);
            Pos(v) = { Pos(me).x - sinf(h) * 3.0f, Pos(me).y + cosf(h) * 3.0f, Pos(me).z };
            SetHeadingMatrix(v, h);
            SetEntityStatus(v, STATUS_ABANDONED);
            WorldAdd(v);
            bike = v;
            RegisterReference(v, &bike);
            Log("autotest : Faggio cree");
        }
        if (bike && !seated && t > 120) {
            seated = frame;
            WarpIntoSeat(me, bike, 0);
            Log("autotest : sur le Faggio (conducteur %d)", VehDriver(bike) == me);
        }
        // Avance 2 s toutes les 8 s.
        if (seated && InVehicle(me) && (frame - seated) % 240 > 180) Press(PAD_CROSS, 160);
        return;
    }
    bool withF = _stricmp(g_cfg.autotest, "passagerf") == 0;   // meme chose avec la touche F (manette : triangle)
    if (withF || _stricmp(g_cfg.autotest, "passager") == 0) {
        static bool boarded, left;
        static uint32_t boardedAt;
        if (boarded && withF && frame - boardedAt == 30) Log("autotest : a bord=%d (place %d)", InVehicle(FindPlayerPed()),
            InVehicle(FindPlayerPed()) && PedVehicle(FindPlayerPed()) ? SeatOf(PedVehicle(FindPlayerPed()), FindPlayerPed()) : -1);
        if (boarded && !left && frame - boardedAt > 180) {
            left = true;
            Log("autotest : touche %s (descendre)", withF ? "F" : "G");
            if (withF) Press(PAD_TRIANGLE, 255); else TogglePassenger();
        }
        if (left && frame - boardedAt == 200) Log("autotest : descendu, a pied=%d", !InVehicle(FindPlayerPed()));
        const NetPlayer &h = g_players[0];
        if (!boarded && g_localId > 0 && h.connected && h.state.inVehicle && h.state.seat == 0 && frame - controlSince > 30) {
            void *ped = FindPlayerPed();
            float dx = h.state.pos[0] - Pos(ped).x, dy = h.state.pos[1] - Pos(ped).y;
            if (dx * dx + dy * dy < 64.0f) {
                boarded = true;
                boardedAt = frame;
                Log("autotest : l'hote est au volant, touche %s", withF ? "F" : "G");
                if (withF) Press(PAD_TRIANGLE, 255); else TogglePassenger();
            }
        }
        return;
    }
    bool mainOnly = _stricmp(g_cfg.autotest, "principal") == 0, both = _stricmp(g_cfg.autotest, "mission") == 0;
    if (mainOnly || both || _stricmp(g_cfg.autotest, "objectif") == 0) {
        static int doneIp = -1;
        static uint32_t lastMove;
        uint32_t now = GetTickCount();
        bool missionActive = g_missionMarker.at && now - g_missionMarker.at < 1000;
        const AutotestMarker &mk = mainOnly ? g_mainMarker : both ? (missionActive ? g_missionMarker : g_mainMarker) : g_missionMarker;
        void *me = FindPlayerPed();
        void *veh = InVehicle(me) ? PedVehicle(me) : NULL;
        if (!mk.at || now - mk.at > 1000 || now - lastMove < 8000) return;   // marqueur actif seulement
        if (veh && !both) return;
        float dx = mk.x - Pos(me).x, dy = mk.y - Pos(me).y;
        if (mk.ip == doneIp && dx * dx + dy * dy < 4.0f) return;
        doneIp = mk.ip;
        lastMove = now;
        float z = mk.z != 0.0f ? mk.z : Pos(me).z;
        void *mover = veh ? veh : me;   // au volant : la voiture (et ses passagers, Lance...) vient avec
        Pos(mover) = { mk.x, mk.y, z + 1.0f };
        MoveSpeed(mover) = { 0, 0, 0 };
        Log("autotest : %s sur le cylindre %s (%.1f %.1f %.1f)", veh ? "voiture" : "a pied", &mk == &g_mainMarker ? "du script principal" : "de mission", mk.x, mk.y, z);
        return;
    }
    if (_stricmp(g_cfg.autotest, "cours") == 0) {
        uint32_t t = frame - controlSince;
        if (t < 90) return;
        int phase = (int)((t / 90) % 4);   // 3 s par phase a 30 images/s
        if (phase == 0) { Press(PAD_LSTICK_Y, -60); Press(PAD_LSTICK_X, 40); }            // marche
        else if (phase == 1) { Press(PAD_LSTICK_Y, -128); Press(PAD_LSTICK_X, 40); }      // course
        else if (phase == 2) { Press(PAD_LSTICK_Y, -128); Press(PAD_CROSS, 255); }         // sprint
        static int lastPhase = -1;
        if (phase != lastPhase) { lastPhase = phase; Log("autotest : phase %d, deplacement %d", phase, MoveState(FindPlayerPed())); }
        return;
    }
    if (_stricmp(g_cfg.autotest, "loin") == 0) {
        static bool gone;
        if (!gone && frame - controlSince > 900) {
            gone = true;
            void *me = FindPlayerPed();
            Pos(me) = { Pos(me).x + 300.0f, Pos(me).y, Pos(me).z + 20.0f };
            Log("autotest : je pars a 300 m");
        }
        return;
    }
    if (_stricmp(g_cfg.autotest, "sauvecharge") == 0) {
        // Comme a une planque : ACTIVATE_SAVE_MENU (m_bSaveMenuActive, menu +0x3B) ouvre le menu de sauvegarde
        // (ecran 15), emplacement 1, "Oui" (16), le jeu sauvegarde (17), "OK" (18) ; puis on recharge.
        static int step;
        static uint32_t at;
        uint32_t t = frame - controlSince;
        if (step == 0 && t > 300) { *(bool *)(0x869630 + 0x3B) = true; step = 1; at = frame; Log("autotest : menu de sauvegarde"); }
        else if (step == 1 && MenuCurrentPage() == 15 && frame - at > 60) { MenuRequestSelect(0); step = 2; at = frame; }
        else if (step == 2 && MenuCurrentPage() == 16 && frame - at > 30) { MenuRequestSelect(2); step = 3; at = frame; }
        else if (step == 3 && MenuCurrentPage() == 18 && frame - at > 30) { Log("autotest : sauvegarde faite"); MenuRequestSelect(1); step = 4; at = frame; }
        else if (step == 4 && MenuActive() && MenuCurrentPage() == 18 && frame - at > 60) { MenuRequestSelect(1); at = frame; }   // "OK"
        else if (step == 4 && MenuActive() && MenuCurrentPage() == 15 && frame - at > 60) { MenuRequestSelect(8); at = frame; }   // "Annuler"
        else if (step == 4 && MenuActive() && frame - at > 300) { Log("autotest : menu toujours ouvert (ecran %d)", MenuCurrentPage()); at = frame; }
        else if (step == 4 && !MenuActive() && frame - at > 90) {
            step = 5;
            *(int *)(0x869630 + 0x100) = 0;
            MenuWantToRestart() = 1;
            MenuWantToLoad() = 1;
            Log("autotest : chargement de l'emplacement 1");
        }
        return;
    }
    if (_stricmp(g_cfg.autotest, "tireur") == 0) {
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        static bool armed;
        if (!armed && t > 100) {
            int model = *(int *)(0x782A14 + 17 * 0x64 + 0x54);
            if (!HasModelLoaded(model)) { RequestModel(model, 1); return; }   // comme le ferait un script
            GiveWeapon(me, 17, 500); SetCurrentWeapon(me, 17);
            armed = true;
            Log("autotest : pistolet en main (modele %d)", model);
        }
        if (armed && t > 200 && t % 30 < 3) Press(PAD_CIRCLE, 255);
        if (t > 200 && t % 300 == 0) Log("autotest : %u tirs", (unsigned)LocalShotCount());
        return;
    }
    if (_stricmp(g_cfg.autotest, "histoire") == 0) {
        static int doneObjective = 0, doneContact = 0;
        static uint32_t lastMove;
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        if (t < 90 || frame - lastMove < 150 || InVehicle(me)) return;
        const MirrorPoint *mp = NULL;
        if (g_lastObjective.serial != doneObjective) mp = &g_lastObjective;
        else if (g_lastContact.serial != doneContact) mp = &g_lastContact;
        if (!mp) return;
        if (mp == &g_lastObjective) doneObjective = mp->serial; else doneContact = mp->serial;
        Pos(me) = { mp->x, mp->y, mp->z + 1.0f };
        MoveSpeed(me) = { 0, 0, 0 };
        lastMove = frame;
        Log("autotest : teleporte sur %s (%.1f %.1f %.1f)", mp == &g_lastObjective ? "l'objectif" : "le contact", mp->x, mp->y, mp->z);
        return;
    }
    if (_stricmp(g_cfg.autotest, "cible") == 0) {
        static void *target;
        static uint32_t spawned;
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        if (!spawned && t > 150) {
            spawned = frame;
            int model = -1;
            Pool *pool = PedPool();
            for (int i = 0; i < pool->size && model < 0; i++) {   // un modele deja charge : celui d'un passant
                if (pool->flags[i] & 0x80) continue;
                void *p = pool->objects + i * PED_POOL_ENTRY;
                if (p != me && CharCreatedBy(p) == 1) model = ModelIndex(p);
            }
            if (model < 0) { Log("autotest : pas de passant pour copier un modele"); return; }
            void *ped = PedAlloc();
            CivilianPedCtor(ped, PEDTYPE_CIVMALE, model);
            CharCreatedBy(ped) = PED_CHAR_MISSION;
            float h = Heading(me);
            Pos(ped) = { Pos(me).x - sinf(h) * 4.0f, Pos(me).y + cosf(h) * 4.0f, Pos(me).z };
            WorldAdd(ped);
            target = ped;
            RegisterReference(ped, &target);
            Log("autotest : cible creee (%08X, modele %d)", PedHandle(ped), model);
        }
        void *victim = PuppetPed(1);
        if (target && victim && t % 90 == 0 && Health(target) > 0) {
            ((bool(__thiscall *)(void *, void *, int, float, int, uint8_t))0x525B20)(victim, target, 17, 10.0f, 0, 0);
            Log("autotest : la cible blesse le Tommy de l'invite");
        }
        if (target && t % 150 == 0) Log("autotest : sante de la cible %.0f", Health(target));
        return;
    }
    if (_stricmp(g_cfg.autotest, "frappe") == 0) {
        uint32_t t = frame - controlSince;
        if (t < 60 || t % 60 != 0) return;
        void *me = FindPlayerPed();
        Pool *pool = PedPool();
        for (int i = 0; i < pool->size; i++) {
            if (pool->flags[i] & 0x80) continue;
            void *p = pool->objects + i * PED_POOL_ENTRY;
            if (!IsGhostPed(p) || Health(p) <= 0) continue;
            ((bool(__thiscall *)(void *, void *, int, float, int, uint8_t))0x525B20)(p, me, 17, 25.0f, 0, 0);
            Log("autotest : je frappe la copie %08X (ma sante %.0f)", PedHandle(p), Health(me));
            break;
        }
        return;
    }
    bool car = _stricmp(g_cfg.autotest, "voiture") == 0;
    if (_stricmp(g_cfg.autotest, "marche") == 0 || car) {
        // Attend qu'un autre joueur soit a 6-12 m depuis 3 s (place par "rejoindre"), puis court tout droit 3 s et s'arrete.
        static uint32_t nearSince, runStart;
        void *ped = FindPlayerPed();
        bool isNear = false;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i == g_localId || !g_players[i].connected || !g_players[i].state.inGame) continue;
            float dx = g_players[i].state.pos[0] - Pos(ped).x, dy = g_players[i].state.pos[1] - Pos(ped).y;
            if (dx * dx + dy * dy > 36.0f && dx * dx + dy * dy < 144.0f) isNear = true;   // entre 6 et 12 m : il s'est teleporte
        }
        if (!runStart) {
            if (!isNear) nearSince = 0;
            else if (!nearSince) nearSince = frame;
            else if (frame - nearSince > 90) { runStart = frame; Log("autotest : l'autre joueur est la, je %s", car ? "prends la voiture" : "cours"); }
        } else if (!car) {
            if (frame - runStart < 90) Press(PAD_LSTICK_Y, -128);
        } else {
            // Triangle (monter) a t=0 ; accelere (croix) de 3 s a 7 s ; Triangle (descendre) a 10 s.
            uint32_t t = frame - runStart;
            if (t == 1 || t == 300) { Press(PAD_TRIANGLE, 255); Log("autotest : triangle (%s)", t == 1 ? "monter" : "descendre"); }
            if (t >= 90 && t < 210) Press(PAD_CROSS, 255);
        }
    }
}
