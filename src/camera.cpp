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
static HRESULT __stdcall h_GetState(void *dev, DWORD size, void *data)
{
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

typedef void(__fastcall *CamProcess_t)(void *cam, void *edx);
static CamProcess_t o_CamProcess;

static void __fastcall h_CamProcess(void *cam, void *edx)
{
    o_CamProcess(cam, edx);
    if (!g_cfg.freeCam || cam != ActiveCam()) return;
    void *me = FindPlayerPed();
    void *veh = me && InVehicle(me) ? PedVehicle(me) : NULL;
    short mode = *(short *)((uint8_t *)cam + 0xC);
    // Camera de poursuite du vehicule seulement (pas les cameras de mission, cinematiques, vue interieure...).
    static short loggedMode = -1;
    if (veh && mode != loggedMode && g_cfg.logScripts) { loggedMode = mode; Log("camera : mode %d en vehicule", mode); }
    if (!veh || (mode != 18 && mode != 3) || *(bool *)0xA10AB2) { g_orbit = g_aim = false; g_yaw = 0; return; }

    uint32_t now = GetTickCount();
    float mx = *(float *)(Mouse() + 8) + g_testMouseX, my = *(float *)(Mouse() + 0xC);
    if (fabsf(mx) + fabsf(my) > 0.3f) {
        g_orbit = true;
        g_lastMove = now;
        g_yaw -= mx * 0.005f * g_cfg.camSensitivity;
        g_pitch -= my * 0.004f * g_cfg.camSensitivity;   // souris vers le haut : la camera regarde plus haut (retour de JD)
        if (g_pitch < -0.25f) g_pitch = -0.25f;
        if (g_pitch > 1.2f) g_pitch = 1.2f;
    }
    g_aim = (g_realRmb || Mouse()[1] || g_testAim) && DriveByWeapon(me);
    if (g_aim) { g_orbit = true; g_lastMove = now; }
    if (!g_orbit) return;
    if (now - g_lastMove > 2500) {   // plus de souris : retour en douceur derriere le vehicule
        while (g_yaw > 3.14159f) g_yaw -= 6.28318f;
        while (g_yaw < -3.14159f) g_yaw += 6.28318f;
        g_yaw *= 0.9f;
        g_pitch += (0.22f - g_pitch) * 0.1f;
        if (fabsf(g_yaw) < 0.02f) { g_orbit = false; g_yaw = 0; return; }
    }

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
