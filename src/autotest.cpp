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
//   Autotest=pcj : (hote) va chercher la PCJ-600 de PCJ Playground et monte dessus (la mission doit demarrer) ;
//                  (invite) regarde le premier checkpoint (cercles de l'hote). pcjinvite : l'invite fait le defi
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
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
#include "camera.h"
#include <math.h>
#include <string.h>

#pragma optimize("", off)
static int StackEater(int depth) { volatile char pad[4096]; pad[0] = (char)depth; return StackEater(depth + 1) + pad[0]; }   // (Autotest=debordement)
#pragma optimize("", on)

using namespace game;
int WantedLevel(void *ped);   // coop.cpp

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
    // TestMeteo=N (ini) : meteo forcee au debut (0 soleil, 1 nuages, 2 pluie, 3 brouillard) : FORCE_WEATHER_NOW.
    {
        static bool weatherSet;
        int w = GetPrivateProfileIntA("VCCoop", "TestMeteo", -1, IniPath());
        if (!weatherSet && w >= 0 && frame - controlSince > 20) { weatherSet = true; int32_t a[1] = { w }; MirrorLocal(0x01B6, 1, a); Log("autotest : meteo %d", w); }
    }
    // Prises de vue (ini) : TestSansTexte=1 efface aides et sous-titres ; TestCamera=avant,droite,haut,avant,droite,haut :
    // camera fixe placee et pointee par rapport au joueur (son repere au moment de la premiere pose).
    {
        if (frame % 4 == 0 && GetPrivateProfileIntA("VCCoop", "TestSansTexte", 0, IniPath())) { MirrorLocal(0x03E6, 0, NULL); MirrorLocal(0x00BE, 0, NULL); }
        static bool camRead, camOn;
        static float cam[6], base[4];
        if (!camRead) {
            camRead = true;
            char v[128];
            GetPrivateProfileStringA("VCCoop", "TestCamera", "", v, sizeof(v), IniPath());
            camOn = v[0] && sscanf(v, "%f,%f,%f,%f,%f,%f", &cam[0], &cam[1], &cam[2], &cam[3], &cam[4], &cam[5]) == 6;
            GetPrivateProfileStringA("VCCoop", "TestCameraAbs", "", v, sizeof(v), IniPath());   // x,y,z,cible x,y,z (monde)
            if (v[0] && sscanf(v, "%f,%f,%f,%f,%f,%f", &cam[0], &cam[1], &cam[2], &cam[3], &cam[4], &cam[5]) == 6) { camOn = true; base[3] = -1; }
        }
        uint32_t t = frame - controlSince;
        if (camOn && base[3] < 0 && t >= 150 && t % 45 == 0) {   // camera absolue
            float c[6] = { cam[0], cam[1], cam[2], 0, 0, 0 };
            MirrorLocal(0x015F, 6, (const int32_t *)c);
            int32_t pt[4]; memcpy(pt, &cam[3], 12); pt[3] = 2;
            MirrorLocal(0x0160, 4, pt);
        } else if (camOn && t >= 150 && t % 45 == 0) {
            void *me = FindPlayerPed();
            if (!base[3]) { base[0] = Pos(me).x; base[1] = Pos(me).y; base[2] = Pos(me).z; base[3] = Heading(me) + 1000.0f; }
            float h = base[3] - 1000.0f, fx = -sinf(h), fy = cosf(h), rx = cosf(h), ry = sinf(h);
            float c[6] = { base[0] + fx * cam[0] + rx * cam[1], base[1] + fy * cam[0] + ry * cam[1], base[2] + cam[2], 0, 0, 0 };
            MirrorLocal(0x015F, 6, (const int32_t *)c);
            float at[3] = { base[0] + fx * cam[3] + rx * cam[4], base[1] + fy * cam[3] + ry * cam[4], base[2] + cam[5] };
            int32_t pt[4]; memcpy(pt, at, 12); pt[3] = 2;
            MirrorLocal(0x0160, 4, pt);
        }
    }
    bool farJoin = _stricmp(g_cfg.autotest, "loin") == 0;   // comme rejoindre, mais 160 m devant l'hote (hors de sa zone)
    if (farJoin || _stricmp(g_cfg.autotest, "rejoindre") == 0) {
        static bool done;
        const NetPlayer &host = g_players[g_localId == 0 ? 1 : 0];   // l'hote va aupres du joueur 1
        // Prises de vue (TestEcart) : on se replace tant que l'hote s'eloigne (il peut etre teleporte apres nous).
        if (done && frame % 30 == 0 && host.connected && GetPrivateProfileIntA("VCCoop", "TestSuivre", 0, IniPath())) {
            float dx = Pos(FindPlayerPed()).x - host.state.pos[0], dy = Pos(FindPlayerPed()).y - host.state.pos[1];
            if (dx * dx + dy * dy > 15.0f * 15.0f) done = false;
        }
        if (!done && frame - controlSince > 60 && g_localId >= 0 && host.connected && host.state.inGame) {
            done = true;
            void *ped = FindPlayerPed();
            Vec3 &p = Pos(ped);
            // 8 m devant l'hote et 2,5 m sur sa droite (avant = (-sin h, cos h), droite = (cos h, sin h)) :
            // dans le champ de sa camera sans etre cache par son corps.
            float hh = host.state.heading, fx = -sinf(hh), fy = cosf(hh), rx = cosf(hh), ry = sinf(hh);
            float ahead = farJoin ? 160.0f : 8.0f, side = 2.5f;
            char ec[64];   // TestEcart=devant,cote (prises de vue)
            GetPrivateProfileStringA("VCCoop", "TestEcart", "", ec, sizeof(ec), IniPath());
            float turn = 180.0f;
            if (ec[0]) sscanf(ec, "%f,%f,%f", &ahead, &side, &turn);
            p = { host.state.pos[0] + fx * ahead + rx * side, host.state.pos[1] + fy * ahead + ry * side, host.state.pos[2] + (farJoin ? 3.0f : 0.5f) };
            MoveSpeed(ped) = { 0, 0, 0 };
            float h = hh + turn * 0.0174533f;
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
        // Camera libre : la souris (simulee) fait le tour, puis visee et tir a l'Uzi depuis le volant.
        if (drive) {
            uint32_t k = frame - drive;
            g_testMouseX = (k > 40 && k < 160) ? 12.0f : 0.0f;
            if (k == 170) {
                int model = *(int *)(0x782A14 + 23 * 0x64 + 0x54);
                if (!HasModelLoaded(model)) { RequestModel(model, 1); ((void(__cdecl *)(bool))0x40B5F0)(false); }
                GiveWeapon(ped, 23, 200); SetCurrentWeapon(ped, 23);
                Log("autotest : Uzi au volant, tirs %u", (unsigned)LocalShotCount());
            }
            g_testAim = k > 180 && k < 260;
            g_testPassengerFire = k > 200 && k < 260;
            if (k == 265) Log("autotest : tirs en visee libre : %u", (unsigned)LocalShotCount());
        }
        // Degats (verif. de leur synchro) : capot arrache, pare-brise et pare-chocs avant casses.
        if (drive && frame - drive == 30 && InVehicle(ped) && PedVehicle(ped)) {
            void *v = PedVehicle(ped);
            uint8_t *dm = (uint8_t *)v + 0x2A0;
            ((void(__thiscall *)(void *, int, int))0x5A9820)(dm, 0, 3);
            ((void(__thiscall *)(void *, int, int, bool))0x59B150)(v, 0x11, 0, false);
            *(uint32_t *)(dm + 0x14) |= (3u << 16) | (3u << 20);
            ((void(__thiscall *)(void *, int, int, bool))0x59B2A0)(v, 0x13, 4, true);
            ((void(__thiscall *)(void *, int, int, bool))0x59B370)(v, 7, 5, false);
            Log("autotest : ma voiture est cassee (capot, pare-brise, pare-chocs)");
        }
        return;
    }
    // Autotest=memevoiture : l'hote cree une voiture vide ; les deux joueurs se placent a 2 m d'elle et appuient sur F
    // presque en meme temps : un seul doit prendre le volant, l'autre monte en passager (coop.cpp BoardingFrame).
    if (_stricmp(g_cfg.autotest, "memevoiture") == 0) {
        static void *car;
        static uint32_t readyAt, pressed;
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        if (g_cfg.host && !car && t > 200) {
            if (!HasModelLoaded(130)) { RequestModel(130, 1); return; }
            float h = Heading(me);
            void *v = VehicleAlloc();
            AutomobileCtor(v, 130, 1);
            Pos(v) = { Pos(me).x - sinf(h) * 4.0f, Pos(me).y + cosf(h) * 4.0f, Pos(me).z + 0.3f };
            SetHeadingMatrix(v, h);
            SetEntityStatus(v, STATUS_ABANDONED);
            WorldAdd(v);
            car = v; RegisterReference(v, &car);
            Log("autotest : memevoiture, voiture vide creee");
        }
        if (!g_cfg.host && !car) {   // la copie de la voiture de l'hote la plus proche
            Pool *vp = VehiclePool();
            for (int i = 0; i < vp->size && !car; i++) {
                if (vp->flags[i] & 0x80) continue;
                void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
                if (ModelIndex(v) == 130 && NetVehicleId(v) && !VehDriver(v)) { car = v; RegisterReference(v, &car); }
            }
        }
        if (car && !readyAt) {
            Vec3 r = Field<Vec3>(car, 0x04);
            float side = g_cfg.host ? -2.2f : 2.2f;   // l'hote a gauche (portiere conducteur), l'invite a droite
            Pos(me) = { Pos(car).x + r.x * side, Pos(car).y + r.y * side, Pos(car).z + 0.4f };
            MoveSpeed(me) = { 0, 0, 0 };
            readyAt = frame;
        }
        if (readyAt && !pressed && frame - readyAt > (g_cfg.host ? 120u : 90u)) {   // l'invite (arrive apres) appuie un peu plus tot
            pressed = frame;
            Press(PAD_TRIANGLE, 255);
            Log("autotest : memevoiture, F");
        }
        if (pressed && (frame - pressed == 150 || frame - pressed == 300)) {
            void *v = InVehicle(me) ? PedVehicle(me) : NULL;
            Log("autotest : memevoiture, a bord %d, place %d", v != NULL, v ? SeatOf(v, me) : -1);
        }
        return;
    }
    // Autotest=ragdoll : (hote) tue d'un coup un passant proche (pousse en l'air), puis fonce en voiture sur un
    // autre ; corps mous chez lui et, chez l'invite (rejoindre), la meme pose recue.
    if (_stricmp(g_cfg.autotest, "ragdoll") == 0) {
        static void *car, *victim;
        static uint32_t launchAt;
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        if (!g_cfg.host) {   // invite : camera fixe derriere l'hote, dans l'axe de son regard (la meme scene)
            const MsgState &hs = g_players[0].state;
            if (t >= 250 && t % 60 == 10 && g_players[0].connected) {
                float fx = -sinf(hs.heading), fy = cosf(hs.heading);
                float cam[6] = { hs.pos[0] - fx * 3.5f, hs.pos[1] - fy * 3.5f, hs.pos[2] + 1.5f, 0, 0, 0 };
                float at[3] = { hs.pos[0] + fx * 9.0f, hs.pos[1] + fy * 9.0f, hs.pos[2] - 0.5f };
                MirrorLocal(0x015F, 6, (const int32_t *)cam);
                int32_t pt[4]; memcpy(pt, at, 12); pt[3] = 2;
                MirrorLocal(0x0160, 4, pt);
            }
            return;
        }
        auto nearestCiv = [&](void *skip) -> void * {
            Pool *pp = PedPool();
            void *best = NULL;
            float bd = 40.0f * 40.0f;
            for (int i = 0; i < pp->size; i++) {
                if (pp->flags[i] & 0x80) continue;
                void *p = pp->objects + i * PED_POOL_ENTRY;
                if (p == me || p == skip || IsPuppet(p) || IsGhostPed(p) || InVehicle(p) || Health(p) <= 0.0f || CharCreatedBy(p) != 1) continue;
                if (PedType(p) != 4 && PedType(p) != 5) continue;
                float dx = Pos(p).x - Pos(me).x, dy = Pos(p).y - Pos(me).y, d = dx * dx + dy * dy;
                if (d > 9.0f && d < bd) { bd = d; best = p; }
            }
            return best;
        };
        static void *first;
        // Devant la camera (TheCamera, matrice en 0x7E4688 : avant +0x10, position +0x30), a hauteur de l'hote.
        Vec3 cf = *(Vec3 *)(0x7E4688 + 0x10), cp = *(Vec3 *)(0x7E4688 + 0x30);
        float cl = sqrtf(cf.x * cf.x + cf.y * cf.y); if (cl < 0.1f) cl = 1.0f;
        float hx = cf.x / cl, hy = cf.y / cl, hh = atan2f(-hx, hy);
        Vec3 base = { cp.x + hx * 3.0f, cp.y + hy * 3.0f, Pos(me).z };
        if (t == 200) { int32_t hm[2] = { 12, 0 }; MirrorLocal(0x00C0, 2, hm); }
        if (t == 280) {
            first = nearestCiv(NULL);
            if (first) { Pos(first) = { base.x + hx * 9.0f - hy * 1.5f, base.y + hy * 9.0f + hx * 1.5f, base.z }; MoveSpeed(first) = { 0, 0, 0 }; }
        }
        if (t == 300) {
            if (first && Health(first) > 0.0f) {
                float dx = Pos(first).x - Pos(me).x, dy = Pos(first).y - Pos(me).y, l = sqrtf(dx * dx + dy * dy);
                ((bool(__thiscall *)(void *, void *, int, float, int, uint8_t))0x525B20)(first, me, 0, 1000.0f, 3, 0);
                Log("autotest : ragdoll, passant %08X tue a %.0f m (etat %d)", PedHandle(first), l, PedState(first));
            } else Log("autotest : ragdoll, aucun passant");
        }
        static bool launched;
        if (t >= 600 && !launched) {
            if (!HasModelLoaded(130)) { RequestModel(130, 1); return; }
            victim = nearestCiv(first);
            if (victim) {
                RegisterReference(victim, &victim);
                // Le passant a 7 m devant la camera ; la voiture arrive de sa droite (traverse l'image).
                Pos(victim) = { base.x + hx * 9.0f + hy * 0.5f, base.y + hy * 9.0f - hx * 0.5f, base.z };
                MoveSpeed(victim) = { 0, 0, 0 };
                void *v = VehicleAlloc();
                AutomobileCtor(v, 130, 1);
                Pos(v) = { Pos(victim).x + hx * 14.0f, Pos(victim).y + hy * 14.0f, Pos(victim).z + 0.3f };   // du fond, vers nous
                SetHeadingMatrix(v, hh + 3.14159f);
                SetEntityStatus(v, STATUS_ABANDONED);
                WorldAdd(v);
                car = v; RegisterReference(v, &car);
                launchAt = frame;
                launched = true;
                Log("autotest : ragdoll, voiture lancee sur %08X", PedHandle(victim));
            }
        }
        // 3e phase : un passant tue sur le toit d'une voiture arretee, puis la voiture demarre (il doit tomber).
        static void *roofCar, *roofVictim;
        static uint32_t roofAt;
        if (t >= 1000 && !roofCar) {
            roofVictim = nearestCiv(first);
            if (roofVictim && roofVictim != victim && HasModelLoaded(130)) {
                RegisterReference(roofVictim, &roofVictim);
                void *v = VehicleAlloc();
                AutomobileCtor(v, 130, 1);
                Pos(v) = { base.x + hx * 9.0f - hy * 1.5f, base.y + hy * 9.0f + hx * 1.5f, base.z + 0.3f };
                SetHeadingMatrix(v, hh + 1.5708f);
                SetEntityStatus(v, STATUS_ABANDONED);
                WorldAdd(v);
                roofCar = v; RegisterReference(v, &roofCar);
                Pos(roofVictim) = { Pos(v).x, Pos(v).y, Pos(v).z + 2.2f };
                MoveSpeed(roofVictim) = { 0, 0, 0 };
                ((bool(__thiscall *)(void *, void *, int, float, int, uint8_t))0x525B20)(roofVictim, me, 0, 1000.0f, 3, 0);
                roofAt = frame;
                Log("autotest : ragdoll, passant %08X tue sur le toit d'une voiture", PedHandle(roofVictim));
            }
        }
        if (roofCar && frame - roofAt >= 150 && frame - roofAt < 200) {   // 5 s plus tard, elle part
            Vec3 f = Field<Vec3>(roofCar, 0x14);
            MoveSpeed(roofCar) = { f.x * 0.2f, f.y * 0.2f, MoveSpeed(roofCar).z };
            if (frame - roofAt == 150) Log("autotest : ragdoll, la voiture du toit demarre");
        }
        if (roofVictim && roofAt && (frame - roofAt) % 30 == 0 && frame - roofAt <= 300)
            Log("autotest : ragdoll, corps du toit en z %.2f (voiture z %.2f)", Pos(roofVictim).z, roofCar ? Pos(roofCar).z : 0.0f);
        // 4e phase : un passant contre un mur, tue en etant pousse vers le mur ; son corps doit rester de ce cote.
        static void *wallVictim;
        static uint32_t wallAt;
        static float wall[6];   // point du mur et normale (vers le cote ou est le passant)
        if (t >= 1500 && !wallAt) {
            wallAt = frame;
            bool found = false;
            float best = 1e9f;
            for (int k = 0; k < 16; k++) {
                float a = k * 0.3927f, s[3] = { Pos(me).x, Pos(me).y, Pos(me).z + 0.3f }, e[3] = { s[0] + cosf(a) * 20.0f, s[1] + sinf(a) * 20.0f, s[2] };
                uint8_t col[64] = {};
                void *ent = NULL;
                if (!((bool(__cdecl *)(const float *, const float *, void *, void **, bool, bool, bool, bool, bool, bool, bool, bool))0x4D92D0)(s, e, col, &ent, true, false, false, true, false, false, false, false)) continue;
                float *pt = (float *)col, *n = pt + 4;
                float d = (pt[0] - s[0]) * (pt[0] - s[0]) + (pt[1] - s[1]) * (pt[1] - s[1]);
                if (fabsf(n[2]) > 0.3f || d < 4.0f || d > best) continue;
                best = d; found = true;
                float nl = sqrtf(n[0] * n[0] + n[1] * n[1]);
                wall[0] = pt[0]; wall[1] = pt[1]; wall[2] = pt[2]; wall[3] = n[0] / nl; wall[4] = n[1] / nl; wall[5] = 0;
            }
            wallVictim = found ? nearestCiv(first) : NULL;
            if (wallVictim == victim || wallVictim == roofVictim) wallVictim = NULL;
            if (wallVictim) {
                RegisterReference(wallVictim, &wallVictim);
                Pos(wallVictim) = { wall[0] + wall[3] * 0.45f, wall[1] + wall[4] * 0.45f, Pos(me).z };
                MoveSpeed(wallVictim) = { -wall[3] * 0.12f, -wall[4] * 0.12f, 0 };   // pousse vers le mur (6 m/s)
                ((bool(__thiscall *)(void *, void *, int, float, int, uint8_t))0x525B20)(wallVictim, me, 0, 1000.0f, 3, 0);
                MoveSpeed(wallVictim) = { -wall[3] * 0.12f, -wall[4] * 0.12f, 0 };
                Log("autotest : ragdoll, passant %08X tue contre un mur a %.1f m (normale %.2f %.2f)", PedHandle(wallVictim), sqrtf(best), wall[3], wall[4]);
            } else Log("autotest : ragdoll, pas de mur ou de passant pour la 4e phase");
        }
        if (wallVictim && wallAt && (frame - wallAt == 60 || frame - wallAt == 150 || frame - wallAt == 300)) {
            float side = (Pos(wallVictim).x - wall[0]) * wall[3] + (Pos(wallVictim).y - wall[1]) * wall[4];
            Log("autotest : ragdoll, corps contre le mur : %.2f m devant (%s)", side, side > 0 ? "bon cote" : "TRAVERSE");
        }
        if (car && victim && frame - launchAt >= 20 && frame - launchAt < 120) {
            float dx = Pos(victim).x - Pos(car).x, dy = Pos(victim).y - Pos(car).y, l = sqrtf(dx * dx + dy * dy);
            if (l > 0.5f && (frame - launchAt) % 10 == 0) Log("autotest : ragdoll, voiture a %.1f m de la victime (etat %d, sante %.0f)", l, PedState(victim), Health(victim));
            if (l > 1.5f && PedState(victim) != 42 && PedState(victim) < 54) MoveSpeed(car) = { dx / l * 0.35f, dy / l * 0.35f, MoveSpeed(car).z };
        }
        return;
    }
    // Autotest=casse : (hote) casse l'objet cassable (carton, poubelle...) et la vitre les plus proches, par les
    // fonctions du jeu (ObjectDamage / WindowRespondsToCollision) ; l'invite (rejoindre) doit les voir casses aussi.
    if (_stricmp(g_cfg.autotest, "casse") == 0) {
        uint32_t t = frame - controlSince;
        if (!g_cfg.host || (t != 400 && t != 700)) return;
        void *me = FindPlayerPed();
        Pool *op = *(Pool **)0x94DBE0;
        void *box = NULL, *glass = NULL;
        float bb = 80.0f * 80.0f, bg = 80.0f * 80.0f;
        for (int i = 0; i < op->size; i++) {
            if (op->flags[i] & 0x80) continue;
            void *o = op->objects + i * 0x1A0;
            if (!(Field<uint8_t>(o, 0x51) & 0x01)) continue;   // deja casse (plus de collision)
            float dx = Pos(o).x - Pos(me).x, dy = Pos(o).y - Pos(me).y, d = dx * dx + dy * dy;
            uint16_t mf = *(uint16_t *)(*(uint8_t **)(0x92D4C8 + ModelIndex(o) * 4) + 0x42);
            if (mf & 0x6000) { if (d < bg) { bg = d; glass = o; } }
            else if (Field<uint8_t>(o, 0x178) && d < bb) { bb = d; box = o; }
        }
        if (box) {
            Log("autotest : casse %s (modele %d, effet %d) a %.0f m", ModelName(ModelIndex(box)), ModelIndex(box), Field<uint8_t>(box, 0x178), sqrtf(bb));
            ((void(__thiscall *)(void *, float))0x4E0990)(box, 5000.0f);
        }
        if (glass) {
            Log("autotest : brise la vitre %s (modele %d) a %.0f m", ModelName(ModelIndex(glass)), ModelIndex(glass), sqrtf(bg));
            ((void(__cdecl *)(void *, float, float, float, float, float, float, float, uint8_t))0x553C10)(glass, 2000.0f, 0.1f, 0.0f, 0.0f,
                Pos(glass).x, Pos(glass).y, Pos(glass).z + 1.0f, 0);
        }
        if (!box && !glass) Log("autotest : casse, rien de cassable a 80 m");
        return;
    }
    // Autotest=decor : (hote) fonce en voiture sur l'objet du decor le plus proche (lampadaire, borne, panneau...) ;
    // l'invite doit le voir tomber aussi (objsync.cpp).
    if (_stricmp(g_cfg.autotest, "decor") == 0) {
        static void *car, *target;
        static uint32_t seatedAt;
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        if (!g_cfg.host) return;
        if (!HasModelLoaded(130)) { RequestModel(130, 1); return; }
        if (!target && t > 150 && t % 30 == 0) {
            Pool *op = *(Pool **)0x94DBE0;
            float best = 60.0f * 60.0f;
            for (int i = 0; i < op->size; i++) {
                if (op->flags[i] & 0x80) continue;
                void *o = op->objects + i * 0x1A0;
                if (!(Field<uint8_t>(o, 0x51) & 0x04)) continue;
                const char *n = ModelName(ModelIndex(o));
                if (!strstr(n, "lamp") && !strstr(n, "bollard") && !strstr(n, "sign") && !strstr(n, "hydrant") && !strstr(n, "bin") && !strstr(n, "post") && !strstr(n, "parkingmeter")) continue;
                float dx = Pos(o).x - Pos(me).x, dy = Pos(o).y - Pos(me).y, d = dx * dx + dy * dy;
                if (d > 25.0f && d < best) { best = d; target = o; }
            }
            if (target) {
                float dx = Pos(target).x - Pos(me).x, dy = Pos(target).y - Pos(me).y, l = sqrtf(dx * dx + dy * dy);
                float h = atan2f(-dx / l, dy / l);
                void *v = VehicleAlloc();
                AutomobileCtor(v, 130, 1);
                Pos(v) = { Pos(target).x - dx / l * 10.0f, Pos(target).y - dy / l * 10.0f, Pos(target).z + 0.5f };
                SetHeadingMatrix(v, h);
                SetEntityStatus(v, STATUS_ABANDONED);
                WorldAdd(v);
                car = v; RegisterReference(v, &car);
                WarpIntoSeat(me, v, 0);
                seatedAt = frame;
                Log("autotest : decor, cible %s (modele %d) a %.1f m", ModelName(ModelIndex(target)), ModelIndex(target), l);
            }
        }
        if (car && target && frame - seatedAt == 60) {
            float dx = Pos(target).x - Pos(car).x, dy = Pos(target).y - Pos(car).y, l = sqrtf(dx * dx + dy * dy);
            MoveSpeed(car) = { dx / l * 0.4f, dy / l * 0.4f, 0 };
            Log("autotest : decor, lance vers la cible");
        }
        return;
    }
    // Autotest=renverse : l'hote au volant sur une route degagee ; l'invite, a pied, se place 8 m devant sa voiture ;
    // l'hote le percute a ~60 km/h (vehicles.cpp RunOverByPlayers, chez l'invite).
    if (_stricmp(g_cfg.autotest, "renverse") == 0) {
        static void *car;
        static uint32_t seatedAt, placedAt;
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        if (g_cfg.host && t == 31) {
            int32_t p[4] = { 0 };
            float xyz[3] = { 250.0f, -1250.0f, 11.0f };
            memcpy(p + 1, xyz, 12);
            MirrorLocal(0x0055, 4, p);
        }
        if (!HasModelLoaded(130)) { RequestModel(130, 1); return; }
        if (g_cfg.host) {
            if (!car && t > 150) {
                float h = Heading(me);
                void *v = VehicleAlloc();
                AutomobileCtor(v, 130, 1);
                Pos(v) = { Pos(me).x - sinf(h) * 3.0f, Pos(me).y + cosf(h) * 3.0f, Pos(me).z + 0.3f };
                SetHeadingMatrix(v, h);
                SetEntityStatus(v, STATUS_ABANDONED);
                WorldAdd(v);
                car = v; RegisterReference(v, &car);
                WarpIntoSeat(me, v, 0);
                seatedAt = frame;
                Log("autotest : renverse, voiture prete");
            }
            void *victim = PuppetPed(1);
            if (car && victim && frame - seatedAt == 240) {
                float dx = Pos(victim).x - Pos(car).x, dy = Pos(victim).y - Pos(car).y, l = sqrtf(dx * dx + dy * dy);
                if (l > 0.1f) MoveSpeed(car) = { dx / l * 0.33f, dy / l * 0.33f, 0 };
                Log("autotest : renverse, lance vers l'invite a %.1f m", l);
            }
            if (car && frame - seatedAt > 240 && frame - seatedAt < 330 && (frame - seatedAt) % 6 == 0) {
                Vec3 s2 = MoveSpeed(car);
                Log("autotest : renverse, ma voiture a %.0f km/h", sqrtf(s2.x * s2.x + s2.y * s2.y) * 180.0f);
            }
        } else if (!placedAt && PuppetPed(0) && InVehicle(PuppetPed(0))) {
            void *target = PedVehicle(PuppetPed(0));
            Vec3 f = Field<Vec3>(target, 0x14);
            Pos(me) = { Pos(target).x + f.x * 8.0f, Pos(target).y + f.y * 8.0f, Pos(target).z + 0.5f };
            MoveSpeed(me) = { 0, 0, 0 };
            placedAt = frame;
            Log("autotest : renverse, a pied devant la voiture de l'hote");
        }
        if (!g_cfg.host && placedAt && (frame - placedAt) % 30 == 0 && frame - placedAt < 600)
            Log("autotest : renverse, moi en %.1f %.1f sante %.0f etat %d", Pos(me).x, Pos(me).y, Health(me), PedState(me));
        return;
    }
    // Autotest=percute : l'hote attend au volant ; l'invite arrive 14 m derriere et lui fonce dessus (chocs entre
    // vehicules de joueurs, vehicles.cpp CopyCollisions).
    if (_stricmp(g_cfg.autotest, "percute") == 0) {
        static void *car;
        static uint32_t seatedAt;
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        if (g_cfg.host && t == 31) {   // route degagee (Ocean Beach, rue droite)
            int32_t p[4] = { 0 };
            float xyz[3] = { 250.0f, -1250.0f, 11.0f };
            memcpy(p + 1, xyz, 12);
            MirrorLocal(0x0055, 4, p);
            Log("autotest : percute, hote sur la route");
        }
        if (!HasModelLoaded(130)) { RequestModel(130, 1); return; }
        void *target = NULL;
        if (!g_cfg.host && PuppetPed(0) && InVehicle(PuppetPed(0))) target = PedVehicle(PuppetPed(0));
        if (!car && t > 150 && (g_cfg.host || target)) {
            Vec3 tf = target ? Field<Vec3>(target, 0x14) : Vec3{ 0, 1, 0 };   // avant du vehicule (Heading : personnages seulement)
            float h = g_cfg.host ? Heading(me) : atan2f(-tf.x, tf.y);
            Vec3 base = g_cfg.host ? Pos(me) : Pos(target);
            float fx = -sinf(h), fy = cosf(h);
            float off = g_cfg.host ? 3.0f : -14.0f;
            if (!g_cfg.host) off = -7.0f;   // 7 m derriere la cible, dans son axe
            void *v = VehicleAlloc();
            AutomobileCtor(v, 130, 1);
            Pos(v) = { base.x + fx * off, base.y + fy * off, base.z + 0.3f };
            SetHeadingMatrix(v, h);
            SetEntityStatus(v, STATUS_ABANDONED);
            WorldAdd(v);
            car = v; RegisterReference(v, &car);
            WarpIntoSeat(me, v, 0);
            seatedAt = frame;
            Log("autotest : percute, voiture prete (%s)", g_cfg.host ? "cible" : "belier");
        }
        if (!car) return;
        // (invite) lance droit sur la cible a ~60 km/h (0,33 par 1/50 s), sans compter sur l'accelerateur
        if (!g_cfg.host && frame - seatedAt == 60 && target) {
            float dx = Pos(target).x - Pos(car).x, dy = Pos(target).y - Pos(car).y, l = sqrtf(dx * dx + dy * dy);
            if (l > 0.1f) MoveSpeed(car) = { dx / l * 0.33f, dy / l * 0.33f, 0 };
            Log("autotest : percute, lance vers la cible a %.1f m", l);
        }
        if ((frame - seatedAt) % 10 == 0 && frame - seatedAt > 60 && frame - seatedAt < 400) {
            Vec3 s = MoveSpeed(car);
            float sp = sqrtf(s.x * s.x + s.y * s.y) * 50.0f * 3.6f;
            if (sp > 0.5f || !g_cfg.host) Log("autotest : percute, ma voiture a %.0f km/h en %.1f %.1f", sp, Pos(car).x, Pos(car).y);
        }
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
            float best = 40.0f * 40.0f;   // (passant ordinaire : un personnage de mission ne reagit pas aux coups)
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
        extern bool g_testDropProjectile;
        g_testDropProjectile = true;
        if (armed == 12 && t > 150 && t % 120 < 20) Press(PAD_CIRCLE, 255);
        if (armed == 31 && t % 120 < 40) Press(PAD_CIRCLE, 255);
        return;
    }
    if (_stricmp(g_cfg.autotest, "menucarte") == 0) {   // pause -> carte, et on y reste (captures)
        static int step;
        static uint32_t at;
        uint32_t now = GetTickCount();
        if (step == 0 && frame - controlSince > 60) { *(bool *)(0x869630 + 0x12) = true; step = 1; at = now; }
        else if (step == 1 && now - at > 1500) { MenuRequestPage(6); step = 2; Log("autotest : carte"); }
        return;
    }
    if (_stricmp(g_cfg.autotest, "menuretour") == 0) {   // pause -> stats -> Echap -> Echap : de retour en jeu ?
        static int step;
        static uint32_t at;
        uint32_t now = GetTickCount();
        if (step == 0 && frame - controlSince > 60) { *(bool *)(0x869630 + 0x12) = true; step = 1; at = now; Log("autotest : menu pause"); }
        else if (step == 1 && now - at > 1500) { Log("autotest : page %d, vers les options", MenuCurrentPage()); MenuRequestPage(27); step = 2; at = now; }
        else if (step == 2 && now - at > 1500) { Log("autotest : page %d, Echap", MenuCurrentPage()); MenuRequestBack(); step = 3; at = now; }
        else if (step == 3 && now - at > 1500) { Log("autotest : page %d, Reprendre", MenuCurrentPage()); MenuRequestSelect(0); step = 4; at = now; }
        else if (step == 4 && now - at > 1500) { Log("autotest : menu actif %d, page %d", MenuActive(), MenuCurrentPage()); step = 5; }
        return;
    }
    // TestPos=x,y,z (ini) : Autotest=route (et recherche) s'y teleportent au debut (route passante : circulation, police).
    bool route = _stricmp(g_cfg.autotest, "route") == 0;
    if (route || _stricmp(g_cfg.autotest, "recherche") == 0) {
        static bool placed;
        char pos[64];
        GetPrivateProfileStringA("VCCoop", "TestPos", "", pos, sizeof(pos), IniPath());
        float xyz[3];
        if (!placed && pos[0] && frame - controlSince > (g_cfg.host ? 31u : 450u) && sscanf(pos, "%f,%f,%f", &xyz[0], &xyz[1], &xyz[2]) == 3) {
            placed = true;
            int32_t p[4] = { 0 };
            if (!g_cfg.host) xyz[0] += 3.0f;
            memcpy(p + 1, xyz, 12);
            MirrorLocal(0x0055, 4, p);
            Log("autotest : place en %.0f %.0f", xyz[0], xyz[1]);
        }
        if (route) return;
    }
    if (_stricmp(g_cfg.autotest, "recherche") == 0) {   // (invite) 2 etoiles au bout de 5 s de jeu
        static bool done;
        void *w = Field<void *>(FindPlayerPed(), 0x5F4);
        if (!done && w && frame - controlSince > 150) {
            done = true;
            int stars = g_cfg.testModel > 0 && g_cfg.testModel <= 6 ? g_cfg.testModel : 2;   // TestModele=N : N etoiles
            ((void(__thiscall *)(void *, int))0x4D1FA0)(w, stars);
            Log("autotest : je me fais rechercher (%d etoiles)", stars);
        }
        // TestFinRecherche=N (ini) : etoiles retirees N images apres (l'hote garde les voitures de police venues pour lui).
        static bool cleared;
        int endAt = GetPrivateProfileIntA("VCCoop", "TestFinRecherche", 0, IniPath());
        if (done && !cleared && endAt > 0 && frame - controlSince > 150u + (uint32_t)endAt) {
            cleared = true;
            ((void(__thiscall *)(void *, int))0x4D1FA0)(w, 0);
            Log("autotest : plus recherche");
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
    // Coupure d'un seul cote : on n'envoie plus rien pendant 12 s mais on recoit toujours (l'autre nous perd, pas
    // nous) ; c'est le cas ou le flux fiable restait bloque pour de bon.
    if (_stricmp(g_cfg.autotest, "coupuresortie") == 0) {
        extern uint32_t g_netMuteSendUntil;
        static bool done;
        if (!done && frame - controlSince > 300) {
            done = true;
            g_netMuteSendUntil = GetTickCount() + 12000;
            Log("autotest : plus d'envoi reseau pendant 12 s");
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
    // Autotest=debordement : recursion sans fin sur le fil du jeu (verifie que le journal de plantage s'ecrit quand la
    // pile est pleine : crash.cpp l'ecrit depuis un autre fil).
    if (_stricmp(g_cfg.autotest, "debordement") == 0) {
        if (frame - controlSince == 60) { Log("autotest : debordement de pile provoque"); volatile int r = StackEater(0); (void)r; }
        return;
    }
    if (_stricmp(g_cfg.autotest, "midi") == 0) {   // horloge a 12 h (ombres du soleil), puis on regarde
        static bool done;
        if (!done && frame - controlSince > 30) {
            done = true;
            // TestHeure=H, TestPos=x,y,z, TestCap=degres (vccoop.ini) : sinon 16 h 30 devant l'hotel Ocean View.
            char ini[MAX_PATH], pos[64];
            lstrcpynA(ini, IniPath(), MAX_PATH);
            int32_t t[2] = { (int32_t)GetPrivateProfileIntA("VCCoop", "TestHeure", 16, ini), 30 };
            MirrorLocal(0x00C0, 2, t);
            float xyz[3] = { 260.0f, -1290.0f, 12.0f };   // devant l'hotel Ocean View, cote plage : en plein soleil
            GetPrivateProfileStringA("VCCoop", "TestPos", "", pos, sizeof(pos), ini);
            if (pos[0]) sscanf(pos, "%f,%f,%f", &xyz[0], &xyz[1], &xyz[2]);
            int32_t p[4] = { 0 };
            memcpy(p + 1, xyz, 12);
            MirrorLocal(0x0055, 4, p);
            GetPrivateProfileStringA("VCCoop", "TestCap", "", pos, sizeof(pos), ini);
            if (pos[0]) { float h = (float)atof(pos); int32_t a[2] = { 0, 0 }; memcpy(&a[1], &h, 4); MirrorLocal(0x0171, 2, a); }
            int meteo = (int)GetPrivateProfileIntA("VCCoop", "TestMeteo", -1, ini);   // 0 soleil, 1 nuages, 2 pluie, 3 brouillard
            if (meteo >= 0) { int32_t w[1] = { meteo }; MirrorLocal(0x01B6, 1, w); }   // FORCE_WEATHER_NOW
            Log("autotest : %d h 30 en %.0f %.0f %.0f", t[0], xyz[0], xyz[1], xyz[2]);
        }
        return;
    }
    // Autotest=phares : de nuit (TestHeure, 23 h), Tommy au volant d'une voiture a l'arret, une seconde voiture
    // posee dans le faisceau (ombres des phares).
    if (_stricmp(g_cfg.autotest, "phares") == 0) {
        static void *car, *other;
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        if (t == 31) {
            char ini[MAX_PATH], pos[64];
            lstrcpynA(ini, IniPath(), MAX_PATH);
            int32_t hm[2] = { (int32_t)GetPrivateProfileIntA("VCCoop", "TestHeure", 23, ini), 30 };
            MirrorLocal(0x00C0, 2, hm);
            float xyz[3] = { 250.0f, -1250.0f, 11.0f };
            GetPrivateProfileStringA("VCCoop", "TestPos", "", pos, sizeof(pos), ini);
            if (pos[0]) sscanf(pos, "%f,%f,%f", &xyz[0], &xyz[1], &xyz[2]);
            int32_t p[4] = { 0 };
            memcpy(p + 1, xyz, 12);
            MirrorLocal(0x0055, 4, p);
            Log("autotest : phares, %d h 30", hm[0]);
        }
        // TestVise=x,y,z (ini) : camera fixe au-dessus du joueur, pointee sur ce point (captures).
        if (t >= 120 && t % 60 == 0) {
            char vis[64]; float at[3];
            GetPrivateProfileStringA("VCCoop", "TestVise", "", vis, sizeof(vis), IniPath());
            if (vis[0] && sscanf(vis, "%f,%f,%f", &at[0], &at[1], &at[2]) == 3) {
                float cam[6] = { Pos(me).x, Pos(me).y, Pos(me).z + 4.0f, 0, 0, 0 };
                MirrorLocal(0x015F, 6, (const int32_t *)cam);
                int32_t pt[4]; memcpy(pt, at, 12); pt[3] = 2;
                MirrorLocal(0x0160, 4, pt);
            }
        }
        int MI_CAR = g_cfg.testModel ? g_cfg.testModel : 130;
        if (!other && t > 90 && t < 140) {
            if (!HasModelLoaded(MI_CAR) || !HasModelLoaded(130)) { RequestModel(MI_CAR, 1); RequestModel(130, 1); return; }
            float h = Heading(me), fx = -sinf(h), fy = cosf(h);
            // Moto (Angel, Pizzaboy, PCJ, Faggio, Freeway, Sanchez) : le constructeur de CBike.
            bool bike = MI_CAR == 166 || MI_CAR == 178 || (MI_CAR >= 191 && MI_CAR <= 193) || MI_CAR == 198;
            void *v = VehicleAlloc();
            if (bike) BikeCtor(v, MI_CAR, VEHICLE_MISSION); else AutomobileCtor(v, MI_CAR, 1);
            Pos(v) = { Pos(me).x + fx * 2.5f, Pos(me).y + fy * 2.5f, Pos(me).z + 0.3f };
            SetHeadingMatrix(v, h);
            SetEntityStatus(v, STATUS_ABANDONED);
            WorldAdd(v);
            car = v; RegisterReference(v, &car);
            void *w = VehicleAlloc();
            AutomobileCtor(w, 130, 1);   // cible eclairee : une voiture
            Pos(w) = { Pos(me).x + fx * 11.0f + fy * 1.5f, Pos(me).y + fy * 11.0f - fx * 1.5f, Pos(me).z + 0.3f };
            SetHeadingMatrix(w, h + 1.5708f);
            SetEntityStatus(w, STATUS_ABANDONED);
            WorldAdd(w);
            other = w; RegisterReference(w, &other);
            Log("autotest : voitures creees");
        }
        if (car && t == 170) { WarpIntoSeat(me, car, 0); Log("autotest : au volant (conducteur %d)", VehDriver(car) == me); }
        return;
    }
    // Autotest=pcj : (hote) PCJ Playground. Le generateur de la PCJ-600 (main.scm, 507.4 -308.8) ne la fait naitre que
    // quand le joueur est a 90-110 m (x multiplicateur de distance) : on se pose a 150 m, on attend, on revient sur le
    // spot ; si elle est la, on monte dessus : le fil "O4X4_1" du script principal doit lancer la mission T4X4_1.
    // Autotest=pcjinvite : la meme chose faite par un invite (defi joue chez lui, IsChallengeMission).
    bool pcjGuest = _stricmp(g_cfg.autotest, "pcjinvite") == 0;
    if (pcjGuest || _stricmp(g_cfg.autotest, "pcj") == 0) {
        static void *pcj;
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        const float gx = 507.4f, gy = -308.8f;
        auto warp = [](float x, float y, float z) { int32_t p[4] = { 0 }; float xyz[3] = { x, y, z }; memcpy(p + 1, xyz, 12); MirrorLocal(0x0055, 4, p); };
        if (!g_cfg.host && !pcjGuest) {   // invite : pres du premier checkpoint, tourne vers lui (il doit voir le cercle de l'hote)
            if (t == 300) {
                warp(474.0f, -400.4f, -100.0f);
                float h = 1.5708f;   // vers l'ouest
                SetHeadingMatrix(me, h);
                Heading(me) = HeadingGoal(me) = h;
                Log("autotest : pcj (invite), pres du premier checkpoint");
            }
            if (t == 330) {   // camera fixe a 8 m du checkpoint, pointee dessus
                float cam[6] = { 466.0f, -394.0f, 21.0f, 0, 0, 0 }, at[3] = { 460.0f, -400.4f, 18.0f };
                MirrorLocal(0x015F, 6, (const int32_t *)cam);
                int32_t pt[4]; memcpy(pt, at, 12); pt[3] = 2;
                MirrorLocal(0x0160, 4, pt);
            }
            return;
        }
        if (t == 31) { int32_t hm[2] = { 12, 0 }; MirrorLocal(0x00C0, 2, hm); warp(gx, gy + 150.0f, -100.0f); Log("autotest : pcj, a 150 m du spot"); }
        if (t == 200) warp(gx, gy + 80.0f, -100.0f);
        if (t > 31 && t < 330 && t % 60 == 0) {
            float dx = Pos(me).x - gx, dy = Pos(me).y - gy;
            Log("autotest : pcj, a %.0f m, voitures garees %d, multiplicateur %.2f", sqrtf(dx * dx + dy * dy), *(int *)0x978D88, *(float *)0x7E477C);
        }
        if (t == 330) warp(gx - 2.0f, gy + 6.0f, 13.0f);
        if (t == 400 || t == 700) {
            Pool *vp = VehiclePool();
            int n = 0;
            for (int i = 0; i < vp->size; i++) {
                if (vp->flags[i] & 0x80) continue;
                void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
                float dx = Pos(v).x - gx, dy = Pos(v).y - gy;
                if (dx * dx + dy * dy > 15.0f * 15.0f) continue;
                n++;
                Log("autotest : pcj, vehicule %d (cree par %d) a %.1f m du spot", ModelIndex(v), Field<uint8_t>(v, 0x1F8), sqrtf(dx * dx + dy * dy));
                if (ModelIndex(v) == 191 && !pcj) { pcj = v; RegisterReference(v, &pcj); }
            }
            if (!n) Log("autotest : pcj, aucun vehicule sur le spot (voitures garees %d)", *(int *)0x978D88);
        }
        if (pcj && t == 430) { WarpIntoSeat(me, pcj, 0); Log("autotest : pcj, sur la moto (conducteur %d)", VehDriver(pcj) == me); }
        if (t > 430 && t < 1200 && t % 90 == 0)
            Log("autotest : pcj, en mission %d, drapeaux %d %d %d", *(int *)(ScriptSpace() + 1252), *(int *)(ScriptSpace() + 1368),
                *(int *)(ScriptSpace() + 1416), *(int *)(ScriptSpace() + 1356));
        return;
    }
    if (_stricmp(g_cfg.autotest, "porte") == 0) {
        static void *car;
        static int lastState = -1;
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        int MI_LANDSTAL = g_cfg.testModel ? g_cfg.testModel : 130;
        if (!car && t > 60 && t < 70) {
            if (!HasModelLoaded(MI_LANDSTAL)) { RequestModel(MI_LANDSTAL, 1); return; }
            void *v = VehicleAlloc();
            AutomobileCtor(v, MI_LANDSTAL, 1);
            float h = Heading(me);
            Pos(v) = { Pos(me).x - sinf(h) * 4.0f, Pos(me).y + cosf(h) * 4.0f, Pos(me).z + 0.3f };
            SetHeadingMatrix(v, h + 1.5708f);
            SetEntityStatus(v, STATUS_ABANDONED);
            WorldAdd(v);
            car = v;
            RegisterReference(v, &car);
            Log("autotest : voiture creee");
        }
        if (car && (t == 120 || t == 500)) { Press(PAD_TRIANGLE, 255); Log("autotest : triangle (%s)", t == 120 ? "monter" : "descendre"); }
        int st = PedState(me);
        if (st != lastState) { lastState = st; Log("autotest : etat %d, vehicule %p, a bord %d", st, PedVehicle(me), InVehicle(me)); }
        return;
    }
    // Voiture RETOURNEE (sur le toit) : l'hote s'y installe au volant ; l'invite (Autotest=passager) monte a cote
    // puis en redescend avec G : la sortie en rampant doit se jouer chez les deux (avant : pose directement chez
    // l'invite, pantin pose sans animation chez l'hote, plantage 0x403ED2 quelques images plus tard).
    // Hote recherche (3 etoiles) : sa police doit aussi poursuivre l'invite (Autotest=rejoindre) pose a cote.
    if (_stricmp(g_cfg.autotest, "police") == 0) {
        uint32_t t = frame - controlSince;
        if (t == 150) {
            int32_t ign[2] = { 0, 0 }; MirrorLocal(0x01F7, 2, ign);   // SET_POLICE_IGNORE_PLAYER 0 (le debut de partie l'active)
            int32_t a[2] = { 0, 3 }; MirrorLocal(0x010D, 2, a); Log("autotest : recherche 3 etoiles");
        }
        if (t > 150 && t % 300 == 0) {
            int cops = 0, onGuest = 0;
            Pool *pool = PedPool();
            for (int i = 0; i < pool->size; i++) {
                if (pool->flags[i] & 0x80) continue;
                void *p = pool->objects + i * PED_POOL_ENTRY;
                if (PedType(p) != 6) continue;
                cops++;
                void *tg = Field<void *>(p, 0x16C);
                if (tg && PuppetPlayer(tg) > 0) onGuest++;
            }
            Log("autotest : %d policiers, %d sur un invite, recherche %d", cops, onGuest, WantedLevel(FindPlayerPed()));
        }
        return;
    }
    bool drive = _stricmp(g_cfg.autotest, "rouler") == 0;
    if (drive || _stricmp(g_cfg.autotest, "tonneau") == 0) {
        static void *car;
        static int lastState = -1;
        uint32_t t = frame - controlSince;
        void *me = FindPlayerPed();
        int MI_LANDSTAL = g_cfg.testModel ? g_cfg.testModel : 130;
        static uint32_t boardedAt;
        if (!car && t > 60 && t < 100) {
            if (!HasModelLoaded(MI_LANDSTAL)) { RequestModel(MI_LANDSTAL, 1); return; }
            void *v = VehicleAlloc();
            AutomobileCtor(v, MI_LANDSTAL, 1);
            float h = Heading(me);
            Pos(v) = { Pos(me).x - sinf(h) * 2.5f, Pos(me).y + cosf(h) * 2.5f, Pos(me).z + 0.3f };   // a moins de 6 m de l'invite (touche G)
            SetHeadingMatrix(v, drive ? h : h + 1.5708f);   // 'rouler' : dans le sens de la marche (sinon face au mur)
            SetEntityStatus(v, STATUS_ABANDONED);
            WorldAdd(v);
            car = v;
            RegisterReference(v, &car);
            Log("autotest : voiture creee");
        }
        if (car && t == 150) { WarpIntoSeat(me, car, 0); Log("autotest : au volant (conducteur %d)", VehDriver(car) == me); }
        // Le passager (l'invite) est a bord : 40 images plus tard la voiture est retournee sur le toit, 1,2 m en
        // l'air (elle retombe et tangue encore quand l'invite veut descendre, 6 s apres avoir appuye sur G).
        if (car && !boardedAt && Field<uint8_t>(car, 0x1CC) > 0) { boardedAt = frame; Log("autotest : passager a bord"); }
        if (drive) {   // roule 8 s (a fond), freine 3 s, et ainsi de suite
            if (car && boardedAt && ((frame - boardedAt) % 330) < 240) Press(PAD_CROSS, 255);
            if (car && boardedAt && (frame - boardedAt) % 90 == 0 && InVehicle(me)) Log("autotest : je roule, z %.2f, vitesse %.2f", Pos(car).z, sqrtf(MoveSpeed(car).x * MoveSpeed(car).x + MoveSpeed(car).y * MoveSpeed(car).y));
            return;
        }
        if (car && boardedAt && frame - boardedAt == 40) {
            Vec3 r = Field<Vec3>(car, 0x04);
            Field<Vec3>(car, 0x04) = { -r.x, -r.y, 0 };
            Field<Vec3>(car, 0x24) = { 0, 0, -1 };
            Pos(car).z += 1.2f;
            MoveSpeed(car) = { 0, 0, 0 };
            TurnSpeed(car) = { 0.02f, 0, 0 };
            Log("autotest : voiture retournee sur le toit");
        }
        int st = PedState(me);
        if (st != lastState) { lastState = st; Log("autotest : etat %d, vehicule %p, a bord %d", st, PedVehicle(me), InVehicle(me)); }
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
    bool stay = _stricmp(g_cfg.autotest, "passagerreste") == 0;   // monte et reste a bord
    if (withF || stay || _stricmp(g_cfg.autotest, "passager") == 0) {
        static bool boarded, left;
        static uint32_t boardedAt;
        if (boarded && withF && frame - boardedAt == 30) Log("autotest : a bord=%d (place %d)", InVehicle(FindPlayerPed()),
            InVehicle(FindPlayerPed()) && PedVehicle(FindPlayerPed()) ? SeatOf(PedVehicle(FindPlayerPed()), FindPlayerPed()) : -1);
        // Tir en passager : Uzi en main, on tire a gauche pendant 2 s.
        if (boarded && withF && frame - boardedAt == 40) {
            void *me = FindPlayerPed();
            int model = *(int *)(0x782A14 + 23 * 0x64 + 0x54);
            if (!HasModelLoaded(model)) { RequestModel(model, 1); ((void(__cdecl *)(bool))0x40B5F0)(false); }
            GiveWeapon(me, 23, 200); SetCurrentWeapon(me, 23);
            Log("autotest : Uzi en main, tirs %u", (unsigned)LocalShotCount());
        }
        if (boarded && withF) g_testPassengerFire = frame - boardedAt > 60 && frame - boardedAt < 120;
        if (boarded && withF && frame - boardedAt == 125) Log("autotest : tirs apres 2 s : %u", (unsigned)LocalShotCount());
        if (boarded && !left && !stay && frame - boardedAt > 180) {
            left = true;
            Log("autotest : touche %s (descendre)", withF ? "F" : "G");
            if (withF) Press(PAD_TRIANGLE, 255); else TogglePassenger();
        }
        if (left && frame - boardedAt == 200) Log("autotest : descendu, a pied=%d", !InVehicle(FindPlayerPed()));
        const NetPlayer &h = g_players[g_localId == 0 ? 1 : 0];   // l'autre joueur (l'hote peut aussi etre passager)
        if (!boarded && g_localId >= 0 && h.connected && h.state.inVehicle && h.state.seat == 0 && frame - controlSince > 30) {
            void *ped = FindPlayerPed();
            float dx = h.state.pos[0] - Pos(ped).x, dy = h.state.pos[1] - Pos(ped).y;
            if (dx * dx + dy * dy < 64.0f) {
                boarded = true;
                boardedAt = frame;
                // Pose a 2 m de la portiere avant droite (la montee animee se joue pres de la porte, sinon pose directe).
                if (void *v = NetVehicleById(h.state.vehicleId)) {
                    Vec3 r = Field<Vec3>(v, 0x04);
                    Pos(ped) = { Pos(v).x + r.x * 2.2f, Pos(v).y + r.y * 2.2f, Pos(v).z + 0.4f };
                    MoveSpeed(ped) = { 0, 0, 0 };
                }
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
            // TestDistance=N (vccoop.ini) : on part a N m sur le cote du vehicule le plus proche.
            if (t == 1) {
                char ini[MAX_PATH];
                lstrcpynA(ini, IniPath(), MAX_PATH);
                int dist = GetPrivateProfileIntA("VCCoop", "TestDistance", 0, ini);
                Pool *vp = VehiclePool();
                void *best = NULL;
                float bd = 1e9f;
                for (int i = 0; i < vp->size && dist != 0; i++) {
                    if (vp->flags[i] & 0x80) continue;
                    void *v = vp->objects + i * VEHICLE_POOL_ENTRY;
                    float dx = Pos(v).x - Pos(ped).x, dy = Pos(v).y - Pos(ped).y, d = dx * dx + dy * dy;
                    if (d < bd) { bd = d; best = v; }
                }
                if (best) {
                    Vec3 r = Field<Vec3>(best, 0x04);
                    Pos(ped) = { Pos(best).x - r.x * dist, Pos(best).y - r.y * dist, Pos(best).z + 0.5f };
                    Log("autotest : place a %d m du vehicule", dist);
                }
            }
            if (t == 5 || t == 300) { Press(PAD_TRIANGLE, 255); Log("autotest : triangle (%s)", t == 5 ? "monter" : "descendre"); }
            if (t >= 90 && t < 210) Press(PAD_CROSS, 255);
            if (t % 15 == 0 && t < 300)
                Log("autotest : t=%u etat %d, deplacement %d, en vehicule %d, objectif %d, pas %.3f", t, PedState(ped), MoveState(ped),
                    InVehicle(ped), Field<int>(ped, 0x164), TimeStep());
        }
    }
}
