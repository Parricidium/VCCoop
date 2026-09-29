// Camera libre en vehicule (comme GTA V), CameraLibre=1 :
//  - la souris fait tourner la camera autour du vehicule ; sans mouvement pendant 2,5 s, elle revient derriere ;
//  - clic droit (arme de tir en voiture : pistolets, mitraillettes) : on vise au centre de l'ecran, clic gauche
//    tire la ou on vise (conducteur ou passager) ;
//  - la direction a la souris du jeu est coupee (CVehicle::m_bDisableMouseSteering) : la souris ne sert qu'a la camera.
// La camera du jeu est calculee par CCam::Process (0x48351A) ; juste apres, CCamera::Process construit la vue a
// partir de sa position (m_vecSource +0x174), sa direction (m_vecFront +0x168) et son "haut" (m_vecUp +0x18C) :
// c'est la qu'on les remplace. Les tirs passent par CWeapon::FireFromCar, dont le point vise est calcule par
// CWeapon::DoDriveByAutoAiming (0x5CA400) : on y met le point vise par la camera.
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include "camera.h"
#include "panel.h"
#include <math.h>
#include <string.h>

using namespace game;

static uint8_t *TheCam() { return (uint8_t *)0x7E4688; }             // (adresse ou commence la matrice)
static uint8_t *ActiveCam() { return TheCam() + 0x188 + TheCam()[0x76] * 0x1CC; }
static uint8_t *Mouse() { return (uint8_t *)0x94D788; }              // CPad::NewMouseControllerState

static bool g_orbit, g_aim;
static float g_yaw, g_pitch = 0.22f;
static uint32_t g_lastMove;
float g_testMouseX;   // autotest : mouvement de souris simule
bool g_testAim;

bool FreeAimActive() { return g_aim; }

// --- Souris a la source (DirectInput) ---
// Le clic droit sert au jeu a freiner en voiture. En vehicule avec la camera libre, il sert a viser : on garde son
// etat pour nous (g_realRmb) et on le cache au jeu. IDirectInput8A::CreateDevice (vtable 3) -> pour la souris,
// IDirectInputDevice8A::GetDeviceState (vtable 9) ; DIMOUSESTATE(2) : boutons en +12.
static bool g_realRmb;
typedef HRESULT(__stdcall *GetState_t)(void *dev, DWORD size, void *data);
typedef HRESULT(__stdcall *CreateDevice_t)(void *di, const GUID &guid, void **dev, void *outer);
static GetState_t o_GetState;
static CreateDevice_t o_CreateDevice;

// Apres un Alt+Tab (ou une perte du premier plan), DirectInput rend DIERR_INPUTLOST / DIERR_NOTACQUIRED et la souris
// restait morte : on reprend le peripherique (Acquire, vtable 7) et on relit.
static uint32_t g_mouseReads;   // lectures de la souris par le jeu (diagnostic)

// Retour au premier plan (Alt+Tab) : le peripherique souris est relache puis repris explicitement, et on verifie que
// le jeu la lit encore ; sinon on le dit dans le journal (drapeau "au premier plan" du jeu 0x6D59FC, peripherique
// 0x813D3C).
void MouseFocusFrame()
{
    static bool wasFocus;
    static uint32_t lastCheck, readsAtCheck;
    bool focus = GameHasFocus();
    void *dev = *(void **)0x813D3C;
    uint32_t now = GetTickCount();
    if (focus && !wasFocus && dev) {
        void **vt = *(void ***)dev;
        HRESULT hu = ((HRESULT(__stdcall *)(void *))vt[8])(dev);   // Unacquire
        HRESULT ha = ((HRESULT(__stdcall *)(void *))vt[7])(dev);   // Acquire
        Log("souris : premier plan retrouve, relachee %08X, reprise %08X, jeu actif %d", (unsigned)hu, (unsigned)ha, *(int *)0x6D59FC);
        if (!*(int *)0x6D59FC) { *(int *)0x6D59FC = 1; Log("souris : le jeu se croyait en arriere-plan, corrige"); }
        lastCheck = now;
        readsAtCheck = g_mouseReads;
    }
    if (focus && GameState() == GS_PLAYING && now - lastCheck > 5000) {
        if (g_mouseReads == readsAtCheck)
            Log("souris : pas lue par le jeu depuis 5 s (peripherique %p, jeu actif %d, menu %d)", dev, *(int *)0x6D59FC, (int)*(char *)0x869668);
        lastCheck = now;
        readsAtCheck = g_mouseReads;
    }
    wasFocus = focus;
}

static HRESULT __stdcall h_GetState(void *dev, DWORD size, void *data)
{
    g_mouseReads++;
    HRESULT hr = o_GetState(dev, size, data);
    if (hr == (HRESULT)0x8007001E || hr == (HRESULT)0x8007000C || hr == (HRESULT)0x80070005) {
        typedef HRESULT(__stdcall *Acquire_t)(void *);
        HRESULT ha = ((Acquire_t)(*(void ***)dev)[7])(dev);
        if (SUCCEEDED(ha)) hr = o_GetState(dev, size, data);
        static uint32_t lastLog;
        if (GetTickCount() - lastLog > 2000) { lastLog = GetTickCount(); Log("souris : perdue (%08X), reprise -> %08X / %08X", (unsigned)hr, (unsigned)ha, (unsigned)hr); }
    }
    if (SUCCEEDED(hr) && data && (size == 16 || size == 20)) {
        uint8_t &rmb = ((uint8_t *)data)[13];
        // Menu en jeu ouvert : la souris pilote son curseur, le jeu ne recoit rien (ni camera, ni tir).
        if (PanelWantsMouse() && GameState() == GS_PLAYING) {
            long *axes = (long *)data;
            PanelMouse(axes[0], axes[1], axes[2], (((uint8_t *)data)[12] & 0x80) != 0);
            memset(data, 0, size);
            return hr;
        }
        g_realRmb = (rmb & 0x80) != 0;
        void *me = GameState() == GS_PLAYING ? FindPlayerPed() : NULL;
        if (g_cfg.freeCam && me && InVehicle(me)) rmb = 0;
    }
    return hr;
}

static HRESULT __stdcall h_CreateDevice(void *di, const GUID &guid, void **dev, void *outer)
{
    HRESULT hr = o_CreateDevice(di, guid, dev, outer);
    static const GUID sysMouse = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    if (SUCCEEDED(hr) && dev && *dev && !memcmp(&guid, &sysMouse, sizeof(GUID)) && !o_GetState) {
        void **vt = *(void ***)*dev;
        o_GetState = (GetState_t)PatchPointer(&vt[9], (void *)h_GetState);
        Log("souris : lue a la source (DirectInput)");
    }
    return hr;
}

void HookDirectInput(void *di)
{
    if (o_CreateDevice || !di) return;
    void **vt = *(void ***)di;
    o_CreateDevice = (CreateDevice_t)PatchPointer(&vt[3], (void *)h_CreateDevice);
}

static bool DriveByWeapon(void *ped)
{
    int type = WeaponTypeInSlot(ped, CurrentWeaponSlot(ped));
    uint8_t *info = ((uint8_t *(__cdecl *)(int))0x5D5710)(type);
    return info && *(int *)(info + 0x60) == 5;
}

static void Normalize(Vec3 &v)
{
    float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l > 1e-5f) { v.x /= l; v.y /= l; v.z /= l; }
}

// --- Vue a la premiere personne (touche ToucheVue, F6 par defaut ; VuePremierePersonne=1) ---
// A pied et en vehicule, la camera est posee dans la tete de Tommy, comme la vue de visee du M4 du jeu
// (CCam::Process_1rstPersonPedOnPC 0x481AB3) : os "head" du squelette (GetAnimHierarchyFromSkinClump 0x57F250,
// ConvertPedNode2BoneTag 0x405DE0, RpHAnimIDGetIndex 0x646390, RpHAnimHierarchyGetMatrixArray 0x646370), point
// (0.06, 0.05, 0) de l'os (les yeux) transforme par RwV3dTransformPoints 0x647160, puis l'os mis a l'echelle 0
// (RwMatrixScale 0x644190) : la tete n'est pas dessinee. Plan proche a 0,1 m (RwCameraSetNearClipPlane 0x64A860 sur
// Scene.camera 0x8100BC ; 0,9 m sinon : le volant, les bras disparaissaient). La marche suit la vue : le jeu tire
// TheCamera.Orientation de la vue finale.
static bool g_fps;
bool FirstPersonActive() { return g_fps; }

static bool HeadEyes(void *ped, Vec3 &eyes)
{
    void *clump = Field<void *>(ped, 0x4C);
    if (!clump) return false;
    void *hier = ((void *(__cdecl *)(void *))0x57F250)(clump);
    if (!hier) return false;
    int tag = ((int(__cdecl *)(int))0x405DE0)(2);   // PED_HEAD
    int idx = ((int(__cdecl *)(void *, int))0x646390)(hier, tag);
    uint8_t *mats = ((uint8_t *(__cdecl *)(void *))0x646370)(hier);
    if (idx < 0 || !mats) return false;
    uint8_t *m = mats + idx * 0x40;
    float p[3] = { 0.06f, 0.05f, 0.0f };
    ((void *(__cdecl *)(float *, const float *, int, void *))0x647160)(p, p, 1, m);
    float zero[3] = { 0, 0, 0 };
    ((void *(__cdecl *)(void *, const float *, int))0x644190)(m, zero, 1);   // tete cachee (rwCOMBINEPRECONCAT)
    eyes = { p[0], p[1], p[2] };
    return true;
}

// Vitres du vehicule du joueur : vues de l'interieur, teintees et avec leur reflet d'environnement, elles
// couvraient tout l'ecran de noir. Pendant le dessin de ce vehicule (CEntity::Render, vtable[13], detournee dans la
// vtable de sa classe), le pont Direct3D 9 saute les dessins transparents (gfx9.cpp, Gfx9Intercept).
bool g_hideOwnGlass;
static bool g_fpsShown;   // la vue a la premiere personne est affichee cette image
typedef void(__fastcall *EntRender_t)(void *e, void *edx);
static struct { void **vt; EntRender_t orig; } g_vehRender[8];
static int g_vehRenderCount;
static void __fastcall h_VehRender(void *e, void *edx)
{
    void **vt = *(void ***)e;
    EntRender_t orig = NULL;
    for (int i = 0; i < g_vehRenderCount; i++) if (g_vehRender[i].vt == vt) orig = g_vehRender[i].orig;
    if (!orig) return;
    void *me = FindPlayerPed();
    bool own = g_fpsShown && me && InVehicle(me) && PedVehicle(me) == e;
    g_hideOwnGlass = own;
    orig(e, edx);
    g_hideOwnGlass = false;
}
static void HookVehicleRender(void *veh)
{
    void **vt = *(void ***)veh;
    for (int i = 0; i < g_vehRenderCount; i++) if (g_vehRender[i].vt == vt) return;
    if (g_vehRenderCount >= 8 || vt[13] == (void *)h_VehRender) return;
    g_vehRender[g_vehRenderCount].vt = vt;
    g_vehRender[g_vehRenderCount].orig = (EntRender_t)PatchPointer(&vt[13], (void *)h_VehRender);
    g_vehRenderCount++;
}

static void MouseOrbit(void *me, uint32_t now, uint32_t recenterMs = 2500, float pitchMin = -0.25f);   // plus bas : la souris tourne la vue (camera libre et premiere personne)

// Vrai si la vue a ete remplacee.
static bool FirstPersonView(void *cam, short mode)
{
    void *me = FindPlayerPed();
    if (!me || *(bool *)0xA10AB2) return false;   // cinematique : la camera du jeu
    void *veh = InVehicle(me) ? PedVehicle(me) : NULL;
    // A pied : la camera de suivi (MODE_FOLLOWPED 4) seulement (visee, bagarre, arrestation... : celle du jeu).
    // En vehicule : la camera de poursuite (18), derriere la voiture (3) ou le bateau (22).
    if (veh ? (mode != 18 && mode != 3 && mode != 22) : mode != 4) return false;
    Vec3 eyes;
    if (!HeadEyes(me, eyes)) return false;
    Vec3 &src = Field<Vec3>(cam, 0x174), &front = Field<Vec3>(cam, 0x168), &up = Field<Vec3>(cam, 0x18C);
    Vec3 f;
    if (veh) {
        HookVehicleRender(veh);
        // Droit devant le vehicule (avec son tangage et son roulis) ; la souris tourne la tete librement, qui ne
        // revient droit devant qu'au bout de 5 s sans souris.
        MouseOrbit(me, GetTickCount(), 5000, -0.9f);   // tete : on peut aussi lever les yeux
        Vec3 vr = Field<Vec3>(veh, 0x4), vf = Field<Vec3>(veh, 0x14), vu = Field<Vec3>(veh, 0x24), vp = Pos(veh);
        // Yeux fixes dans le repere du vehicule : les os de la tete datent de l'image precedente (la voiture a avance
        // depuis) et la vue reculait d'autant plus qu'on allait vite. Position mesuree vehicule arrete ; en roulant,
        // la premiere mesure est corrigee de la vitesse (une image).
        static void *s_veh;
        static Vec3 s_local;
        Vec3 mv = MoveSpeed(veh);
        bool calm = mv.x * mv.x + mv.y * mv.y + mv.z * mv.z < 0.01f * 0.01f;
        if (veh != s_veh || calm) {
            Vec3 e = eyes;
            if (!calm) { float ts = TimeStep(); e = { e.x + mv.x * ts, e.y + mv.y * ts, e.z + mv.z * ts }; }
            Vec3 d = { e.x - vp.x, e.y - vp.y, e.z - vp.z };
            s_local = { d.x * vr.x + d.y * vr.y + d.z * vr.z, d.x * vf.x + d.y * vf.y + d.z * vf.z, d.x * vu.x + d.y * vu.y + d.z * vu.z };
            s_veh = veh;
        }
        eyes = { vp.x + vr.x * s_local.x + vf.x * s_local.y + vu.x * s_local.z,
                 vp.y + vr.y * s_local.x + vf.y * s_local.y + vu.y * s_local.z,
                 vp.z + vr.z * s_local.x + vf.z * s_local.y + vu.z * s_local.z };
        float yaw = g_yaw, pitch = g_pitch - 0.22f;
        float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
        f = { (vf.x * cy - vr.x * sy) * cp - vu.x * sp, (vf.y * cy - vr.y * sy) * cp - vu.y * sp, (vf.z * cy - vr.z * sy) * cp - vu.z * sp };
        Normalize(f);
        Vec3 right = { f.y * vu.z - f.z * vu.y, f.z * vu.x - f.x * vu.z, f.x * vu.y - f.y * vu.x };   // f x haut du vehicule
        Normalize(right);
        up = { right.y * f.z - right.z * f.y, right.z * f.x - right.x * f.z, right.x * f.y - right.y * f.x };
    } else {
        // A pied : la direction de la camera du jeu (sa souris, sa sensibilite, son inversion), depuis les yeux. Les os
        // datent de l'image precedente : en courant, la camera restait derriere la tete (on la voyait, coupee par le
        // plan proche). Avancee d'une image de deplacement, plus 8 cm vers l'avant.
        f = front;
        Vec3 mv = MoveSpeed(me);
        float ts = TimeStep();
        Vec3 pf = Field<Vec3>(me, 0x14);
        eyes = { eyes.x + mv.x * ts + pf.x * 0.08f, eyes.y + mv.y * ts + pf.y * 0.08f, eyes.z + mv.z * ts };
        Normalize(f);
        Vec3 right = { f.y, -f.x, 0 };
        Normalize(right);
        up = { right.y * f.z - right.z * f.y, right.z * f.x - right.x * f.z, right.x * f.y - right.y * f.x };
        g_aim = false;
    }
    src = eyes;
    front = f;
    ((void(__cdecl *)(void *, float))0x64A860)(*(void **)0x8100BC, 0.1f);   // plan proche
    // CCamera::m_bMoveCamToAvoidGeom (+0x5D) : pose par le jeu pres d'un obstacle (en vehicule, presque toujours avec
    // la camera dans l'habitacle) ; CCamera::Process decalait alors la position et visait son propre point : la vue
    // partait vers le ciel, de travers.
    TheCam()[0x5D] = 0;
    if (g_cfg.logScripts) {
        static uint32_t last;
        if (GetTickCount() - last > 1500) {
            last = GetTickCount();
            Vec3 pp = Pos(me), vp = veh ? Pos(veh) : pp;
            Log("camera : premiere personne, yeux %.2f %.2f %.2f, perso %.2f %.2f %.2f, vehicule %.2f %.2f %.2f, regard %.2f %.2f %.2f, mode %d",
                eyes.x, eyes.y, eyes.z, pp.x, pp.y, pp.z, vp.x, vp.y, vp.z, f.x, f.y, f.z, mode);
        }
    }
    return true;
}

typedef void(__fastcall *CamProcess_t)(void *cam, void *edx);
static CamProcess_t o_CamProcess;

static void __fastcall h_CamProcess(void *cam, void *edx)
{
    // Camera de cinematique (MODE_FLYBY 17) sans son trace (CCamera::m_arrPathArray, 0x7E4E98..0x7E4EA4) : le jeu
    // plante (0x47E4C9) ; ca arrive chez l'invite quand les commandes camera de fin de mission de l'hote arrivent
    // toutes dans la meme image. On fait comme si la cinematique etait finie (m_bcutsceneFinished, 0x7E46D5), le
    // test que la fonction fait elle-meme en entrant.
    if (*(short *)((uint8_t *)cam + 0xC) == 17) {
        void **path = (void **)0x7E4E98;
        if (!path[0] || !path[1] || !path[2] || !path[3]) {
            if (!TheCam()[0x4D]) Log("camera : cinematique sans trace, arretee (evite le plantage 0x47E4C9)");
            TheCam()[0x4D] = 1;
        }
    }
    o_CamProcess(cam, edx);
    if (cam != ActiveCam()) return;
    short mode = *(short *)((uint8_t *)cam + 0xC);
    g_fpsShown = g_fps && FirstPersonView(cam, mode);
    if (g_fpsShown) return;
    if (!g_cfg.freeCam) return;
    void *me = FindPlayerPed();
    void *veh = me && InVehicle(me) ? PedVehicle(me) : NULL;
    // Camera de poursuite du vehicule seulement (pas les cameras de mission, cinematiques, vue interieure...).
    static short loggedMode = -1;
    if (veh && mode != loggedMode && g_cfg.logScripts) { loggedMode = mode; Log("camera : mode %d en vehicule", mode); }
    if (!veh || (mode != 18 && mode != 3) || *(bool *)0xA10AB2) { g_orbit = g_aim = false; g_yaw = 0; return; }

    uint32_t now = GetTickCount();
    MouseOrbit(me, now);
    if (!g_orbit) return;

    Vec3 &src = Field<Vec3>(cam, 0x174), &front = Field<Vec3>(cam, 0x168), &up = Field<Vec3>(cam, 0x18C);
    Vec3 target = Pos(veh);
    target.z += 0.9f;
    float dx = src.x - target.x, dy = src.y - target.y, dz = src.z - target.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist < 3.0f || dist > 30.0f) dist = 7.0f;
    Vec3 vf = Field<Vec3>(veh, 0x14);
    float a = atan2f(-vf.x, vf.y) + g_yaw;
    Vec3 f = { -sinf(a) * cosf(g_pitch), cosf(a) * cosf(g_pitch), -sinf(g_pitch) };
    Vec3 p = { target.x - f.x * dist, target.y - f.y * dist, target.z - f.z * dist };
    // Pas a travers les murs : si un batiment coupe la vue, on rapproche la camera.
    uint8_t col[64] = {};
    void *hit = NULL;
    float t0[3] = { target.x, target.y, target.z }, t1[3] = { p.x, p.y, p.z };
    if (((bool(__cdecl *)(const float *, const float *, void *, void **, bool, bool, bool, bool, bool, bool, bool, bool))0x4D92D0)(
            t0, t1, col, &hit, true, false, false, true, false, false, true, false)) {
        Vec3 h = *(Vec3 *)col;
        p = { h.x + f.x * 0.3f, h.y + f.y * 0.3f, h.z + f.z * 0.3f };
    }
    Vec3 right = { f.y, -f.x, 0 };   // f x (0, 0, 1)
    Normalize(right);
    Vec3 u = { right.y * f.z - right.z * f.y, right.z * f.x - right.x * f.z, right.x * f.y - right.y * f.x };
    src = p;
    front = f;
    up = u;
    if (g_cfg.logScripts) {
        static uint32_t lastLog;
        if (now - lastLog > 700) {
            lastLog = now;
            Log("camera : orbite %.2f rad (tangage %.2f), visee %d, camera en %.1f %.1f %.1f vers %.2f %.2f %.2f", g_yaw, g_pitch, g_aim,
                p.x, p.y, p.z, f.x, f.y, f.z);
        }
    }
}

// Souris en vehicule : tourne la camera (g_yaw, g_pitch) ; sans mouvement pendant 2,5 s, retour en douceur derriere
// le vehicule (vue libre) ou droit devant (premiere personne). Clic droit avec une arme de tir : visee.
static void MouseOrbit(void *me, uint32_t now, uint32_t recenterMs, float pitchMin)
{
    float mx = *(float *)(Mouse() + 8) + g_testMouseX, my = *(float *)(Mouse() + 0xC);
    if (fabsf(mx) + fabsf(my) > 0.3f) {
        g_orbit = true;
        g_lastMove = now;
        g_yaw -= mx * 0.005f * g_cfg.camSensitivity;
        g_pitch -= my * 0.004f * g_cfg.camSensitivity;   // souris vers le haut : la camera regarde plus haut (retour de JD)
        if (g_pitch < pitchMin) g_pitch = pitchMin;
        if (g_pitch > 1.2f) g_pitch = 1.2f;
    }
    g_aim = (g_realRmb || Mouse()[1] || g_testAim) && DriveByWeapon(me);
    if (g_aim) { g_orbit = true; g_lastMove = now; }
    if (g_orbit && !g_aim && now - g_lastMove > recenterMs) {
        while (g_yaw > 3.14159f) g_yaw -= 6.28318f;
        while (g_yaw < -3.14159f) g_yaw += 6.28318f;
        g_yaw *= 0.9f;
        g_pitch += (0.22f - g_pitch) * 0.1f;
        if (fabsf(g_yaw) < 0.02f) { g_orbit = false; g_yaw = 0; g_pitch = 0.22f; }
    }
}

// Point vise : rayon depuis la camera (celle qu'on vient de calculer) ; le vehicule du joueur est ignore en partant
// un peu devant lui.
static void AimPoint(float *out)
{
    uint8_t *c = ActiveCam();
    Vec3 s = Field<Vec3>(c, 0x174), f = Field<Vec3>(c, 0x168);
    float start[3] = { s.x + f.x * 4.0f, s.y + f.y * 4.0f, s.z + f.z * 4.0f };
    float end[3] = { s.x + f.x * 120.0f, s.y + f.y * 120.0f, s.z + f.z * 120.0f };
    uint8_t col[64] = {};
    void *hit = NULL;
    if (((bool(__cdecl *)(const float *, const float *, void *, void **, bool, bool, bool, bool, bool, bool, bool, bool))0x4D92D0)(
            start, end, col, &hit, true, true, true, true, false, false, false, true)) {
        memcpy(out, col, 12);
        return;
    }
    memcpy(out, end, 12);
}

typedef void(__cdecl *AutoAim_t)(void *shooter, void *veh, float *start, float *end);
static AutoAim_t o_AutoAim;

static void __cdecl h_AutoAim(void *shooter, void *veh, float *start, float *end)
{
    void *me = FindPlayerPed();
    if (g_aim && me && (shooter == me || (InVehicle(me) && veh == PedVehicle(me)))) { AimPoint(end); return; }
    o_AutoAim(shooter, veh, start, end);
}

// Chaque image : coupe la direction a la souris des voitures tant que la camera libre est active. Le jeu la fait si
// CCamera::m_bUseMouse3rdPerson (0xA10B4C, qui sert aussi a la camera a pied : surtout pas a zero, la souris ne
// marchait plus a pied) et pas CVehicle::m_bDisableMouseSteering (0x69C610) : c'est ce dernier qu'on pose.
void CameraFrame()
{
    // Touche de la premiere personne (en partie, jeu au premier plan, hors menus).
    static bool was;
    bool down = g_cfg.fpsView && g_cfg.fpsKey && GameHasFocus() && !PanelCapturesKeys() && GameState() == GS_PLAYING && !*(char *)0x869668 &&
                (GetAsyncKeyState(g_cfg.fpsKey) & 0x8000);
    static int test = -1;   // TestPremierePersonne=1 : active des l'arrivee (captures)
    if (test < 0) test = GetPrivateProfileIntA("VCCoop", "TestPremierePersonne", 0, IniPath());
    if (test > 0 && GameState() == GS_PLAYING) { test = 0; g_fps = true; Log("camera : premiere personne (test)"); }
    if (down && !was) { g_fps = !g_fps; g_yaw = 0; g_pitch = 0.22f; g_orbit = false; Log("camera : premiere personne %s", g_fps ? "oui" : "non"); }
    was = down;
    if (!g_cfg.fpsView) g_fps = false;
    if (!g_cfg.freeCam) return;
    *(bool *)0x69C610 = true;
    // La 2026.09.27c remettait m_bUseMouse3rdPerson a zero (et le jeu a pu l'enregistrer dans gta_vc.set en
    // passant par les options) : on la remet une fois, sinon la souris resterait sans effet a pied.
    static bool restored;
    if (!restored) { restored = true; *(bool *)0xA10B4C = true; }
}

void InstallCamera()
{
    static const uint8_t camPro[] = { 0xD9, 0x05, 0xD8, 0xAD, 0x68, 0x00, 0x53, 0x56, 0x57, 0x55 };
    static const uint8_t aimPro[] = { 0x53, 0x56, 0x57, 0x55, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00 };
    o_CamProcess = (CamProcess_t)MakeDetour(0x48351A, camPro, sizeof(camPro), (void *)h_CamProcess);
    o_AutoAim = (AutoAim_t)MakeDetour(0x5CA400, aimPro, sizeof(aimPro), (void *)h_AutoAim);
}
