// Adresses et structures de gta-vc.exe 1.0 (source : plugin-sdk de DK22Pac, verifiees dans Ghidra).
#pragma once
#include <stdint.h>

namespace game {

// --- Etat global ---
inline int &GameState() { return *(int *)0x9B5F08; }   // 7 = menu, 9 = en jeu
enum { GS_FRONTEND = 7, GS_PLAYING = 9 };
// FrontEndMenuManager (0x869630) : m_bMenuActive +0x38, m_bWantToRestart +0x39, m_bFirstTime +0x3A,
// m_bWantToLoad +0x3C. WinMain sort de sa boucle interne sur m_bWantToRestart, puis passe a l'etat 8
// (nouvelle partie) si ni m_bWantToLoad ni m_bFirstTime.
inline char &MenuActive() { return *(char *)0x869668; }
inline char &MenuWantToRestart() { return *(char *)0x869669; }
inline char &MenuFirstTime() { return *(char *)0x86966A; }
inline char &MenuWantToLoad() { return *(char *)0x86966C; }

// --- Entites : champs utilises (offsets depuis le debut de l'objet) ---
// CPlaceable : CMatrix en +0x04 (right +0x04, up/avant +0x14, at +0x24, pos +0x34)
// CEntity    : m_nModelIndex +0x5C (short), m_nAreaCode +0x5F, drapeaux +0x51..0x55
// CPhysical  : m_vecMoveSpeed +0x70
// CPed       : CharCreatedBy +0x160, m_ePedState +0x244, m_eMoveState +0x24C, m_fHealth +0x354,
//              m_fArmour +0x358, m_fHeadingCurrent +0x374, m_fHeadingGoal +0x378,
//              m_pVehicle +0x3A8, m_bInVehicle +0x3AC, m_nCurrentWeapon +0x504
struct Vec3 { float x, y, z; };

template <typename T> inline T &Field(void *obj, int off) { return *(T *)((uint8_t *)obj + off); }

inline Vec3 &Pos(void *e) { return Field<Vec3>(e, 0x34); }
inline Vec3 &MoveSpeed(void *e) { return Field<Vec3>(e, 0x70); }
inline uint8_t &AreaCode(void *e) { return Field<uint8_t>(e, 0x5F); }
inline short &ModelIndex(void *e) { return Field<short>(e, 0x5C); }

enum { PED_CHAR_MISSION = 2 };
inline uint8_t &CharCreatedBy(void *p) { return Field<uint8_t>(p, 0x160); }
inline int &PedState(void *p) { return Field<int>(p, 0x244); }
inline int &MoveState(void *p) { return Field<int>(p, 0x24C); }
inline float &Health(void *p) { return Field<float>(p, 0x354); }
inline float &Armour(void *p) { return Field<float>(p, 0x358); }
inline float &Heading(void *p) { return Field<float>(p, 0x374); }
inline float &HeadingGoal(void *p) { return Field<float>(p, 0x378); }
inline void *&PedVehicle(void *p) { return Field<void *>(p, 0x3A8); }
inline bool &InVehicle(void *p) { return Field<bool>(p, 0x3AC); }

// Orientation autour de Z : "right" = (cos, sin, 0), "up" (l'avant) = (-sin, cos, 0), "at" = (0, 0, 1).
void SetHeadingMatrix(void *e, float heading);

// --- Fonctions du jeu ---
inline void *FindPlayerPed() { return ((void *(__cdecl *)())0x4BC120)(); }
inline void *PedAlloc() { return ((void *(__cdecl *)(unsigned))0x50DA60)(0x600); }
inline void CivilianPedCtor(void *p, int pedType, unsigned model) { ((void(__thiscall *)(void *, int, unsigned))0x4EAE00)(p, pedType, model); }
inline void WorldAdd(void *e) { ((void(__cdecl *)(void *))0x4DB3F0)(e); }
inline void WorldRemove(void *e) { ((void(__cdecl *)(void *))0x4DB310)(e); }
inline void RegisterReference(void *e, void **ref) { ((void(__thiscall *)(void *, void **))0x4C6AC0)(e, ref); }
inline void CleanUpOldReference(void *e, void **ref) { ((void(__thiscall *)(void *, void **))0x4C6A80)(e, ref); }
inline void SetMoveStateFn(void *p, int state) { ((void(__thiscall *)(void *, int))0x50D110)(p, state); }
inline void SetMoveAnim(void *p) { ((void(__thiscall *)(void *))0x50CD50)(p); }
// A appeler avant de detruire une entite (comme DELETE_CHAR / DELETE_CAR) : efface les pointeurs que d'autres
// objets du jeu (camera, IA, vehicules...) gardent vers elle.
inline void RemoveReferencesToDeletedObject(void *e) { ((void(__cdecl *)(void *))0x4D5090)(e); }
// vtable des entites : 0 = Add, 1 = Remove, 2 = destructeur ("scalar deleting", libere aussi l'objet).
inline void DeleteEntity(void *e) { ((void(__thiscall *)(void *, int))(*(void ***)e)[2])(e, 1); }

enum { PEDTYPE_CIVMALE = 4 };
enum { MI_PLAYER = 0 };

// --- Scripts (machine virtuelle SCM) ---
// CRunningScript (0x88 octets) : m_pNext +0, m_szName[8] +8, m_nIp +0x10, m_nWakeTime +0x7C, m_bIsMission +0x85
inline void *ActiveScripts() { return *(void **)0x975338; }
inline uint8_t *ScriptSpace() { return (uint8_t *)0x821280; }
// --- CTimer ---
inline uint32_t &TimeInMs() { return *(uint32_t *)0x974B2C; }
inline float &TimeStep() { return *(float *)0x975424; }
inline uint32_t &FrameCounter() { return *(uint32_t *)0xA0D898; }
inline bool &UserPause() { return *(bool *)0xA10B36; }
inline bool &CodePause() { return *(bool *)0xA10B76; }

// --- Heure et meteo ---
inline uint8_t &ClockHours() { return *(uint8_t *)0xA10B6B; }
inline uint8_t &ClockMinutes() { return *(uint8_t *)0xA10B92; }
inline uint16_t &ClockSeconds() { return *(uint16_t *)0xA10A3C; }
inline short &OldWeather() { return *(short *)0xA10AAA; }
inline short &NewWeather() { return *(short *)0xA10A2E; }
inline short &ForcedWeather() { return *(short *)0xA10A42; }

// --- Vehicules ---
// CPhysical : m_vecTurnSpeed +0x7C. CEntity : m_nType (3 bits bas) / m_nState (5 bits hauts) en +0x50.
// CVehicle  : couleurs +0x1A0/+0x1A1, m_pDriver +0x1A8, m_fSteerAngle +0x1EC, m_fGasPedal +0x1F0,
//             m_fBreakPedal +0x1F4, m_nCreatedBy +0x1F8, m_fHealth +0x204, m_nVehicleClass +0x29C
enum { VCLASS_CAR = 0, VCLASS_BOAT = 1, VCLASS_TRAIN = 2, VCLASS_HELI = 3, VCLASS_PLANE = 4, VCLASS_BIKE = 5 };
enum { STATUS_PHYSICS = 3, STATUS_ABANDONED = 4, STATUS_WRECKED = 5 };
enum { VEHICLE_MISSION = 2 };
inline Vec3 &TurnSpeed(void *e) { return Field<Vec3>(e, 0x7C); }
inline int EntityStatus(void *e) { return Field<uint8_t>(e, 0x50) >> 3; }
inline void SetEntityStatus(void *e, int st) { uint8_t &b = Field<uint8_t>(e, 0x50); b = (uint8_t)((b & 7) | (st << 3)); }
inline void *&VehDriver(void *v) { return Field<void *>(v, 0x1A8); }
inline uint32_t &VehClass(void *v) { return Field<uint32_t>(v, 0x29C); }
inline float &VehHealth(void *v) { return Field<float>(v, 0x204); }

inline void *VehicleAlloc() { return ((void *(__cdecl *)(unsigned))0x5BAB20)(0x600); }
inline void AutomobileCtor(void *v, int model, uint8_t by) { ((void(__thiscall *)(void *, int, uint8_t))0x59E620)(v, model, by); }
inline void BikeCtor(void *v, int model, uint8_t by) { ((void(__thiscall *)(void *, int, uint8_t))0x615740)(v, model, by); }
inline void BoatCtor(void *v, int model, uint8_t by) { ((void(__thiscall *)(void *, int, uint8_t))0x5A6470)(v, model, by); }
inline void WarpPedIntoCar(void *ped, void *veh) { ((void(__thiscall *)(void *, void *))0x4EF8B0)(ped, veh); }
inline void SetDriver(void *veh, void *ped) { ((void(__thiscall *)(void *, void *))0x5B89F0)(veh, ped); }
inline bool AddPassenger(void *veh, void *ped) { return ((bool(__thiscall *)(void *, void *))0x5B8E60)(veh, ped); }
inline void RemovePassenger(void *veh, void *ped) { ((void(__thiscall *)(void *, void *))0x5B8CE0)(veh, ped); }
inline int AddInCarAnims(void *ped, void *veh, bool driver) { return ((int(__thiscall *)(void *, void *, bool))0x512520)(ped, veh, driver); }
inline void *&VehPassenger(void *v, int i) { return Field<void *>(v, 0x1AC + i * 4); }   // 8 places
enum { PED_DRIVING = 50 };
// Place occupee par ped : 0 = conducteur, 1..8 = passager, -1 = aucune.
inline int SeatOf(void *veh, void *ped)
{
    if (Field<void *>(veh, 0x1A8) == ped) return 0;
    for (int i = 0; i < 8; i++) if (Field<void *>(veh, 0x1AC + i * 4) == ped) return i + 1;
    return -1;
}
inline void RemoveDriver(void *veh) { ((void(__thiscall *)(void *))0x5B8920)(veh); }
inline void RemoveInCarAnims(void *ped, bool b) { ((void(__thiscall *)(void *, bool))0x512440)(ped, b); }
inline void GiveWeapon(void *ped, int type, unsigned ammo) { ((void(__thiscall *)(void *, int, unsigned, bool))0x4FFA30)(ped, type, ammo, true); }
inline void SetCurrentWeapon(void *ped, int type) { ((void(__thiscall *)(void *, int))0x4FF8E0)(ped, type); }
inline void SetAimFlag(void *ped, float heading) { ((void(__thiscall *)(void *, float))0x50B5B0)(ped, heading); }
inline void ClearAimFlag(void *ped) { ((void(__thiscall *)(void *))0x50B4A0)(ped); }
inline bool IsAimingGun(void *ped) { return (Field<uint8_t>(ped, 0x14C) & 0x80) != 0; }
inline void SetIdle(void *ped) { ((void(__thiscall *)(void *))0x4FDFD0)(ped); }

// --- Chargement des modeles ---
inline void RequestModel(int model, int flags) { ((void(__cdecl *)(int, int))0x40E310)(model, flags); }
inline bool HasModelLoaded(int model) { return *(uint8_t *)(0x94DDD8 + model * 20) == 1; }

// --- Pools : { objets, octets d'etat (bit 7 = libre), taille, premier libre } ---
struct Pool { uint8_t *objects; uint8_t *flags; int size; int firstFree; };
enum { PED_POOL_ENTRY = 0x6D8, VEHICLE_POOL_ENTRY = 0x5DC };
inline Pool *PedPool() { return *(Pool **)0x97F2AC; }
inline Pool *VehiclePool() { return *(Pool **)0xA0FDE4; }
inline uint32_t VehicleHandle(void *v) { Pool *p = VehiclePool(); int i = (int)((uint8_t *)v - p->objects) / VEHICLE_POOL_ENTRY; return (uint32_t)(i << 8) | p->flags[i]; }
inline void *PedFromHandle(uint32_t h)
{
    Pool *p = PedPool();
    int i = (int)(h >> 8);
    if (i < 0 || i >= p->size || p->flags[i] != (uint8_t)(h & 0xFF)) return NULL;
    return p->objects + i * PED_POOL_ENTRY;
}
enum { ENTITY_TYPE_VEHICLE = 2, ENTITY_TYPE_PED = 3 };
inline bool IsPedEntity(void *e) { return (Field<uint8_t>(e, 0x50) & 7) == ENTITY_TYPE_PED; }
inline uint32_t PedHandle(void *ped) { Pool *p = PedPool(); int i = (int)((uint8_t *)ped - p->objects) / PED_POOL_ENTRY; return (uint32_t)(i << 8) | p->flags[i]; }

// --- Modeles ---
inline void *ModelInfo(int model) { return ((void **)0x92D4C8)[model]; }
inline const char *ModelName(int model) { void *mi = ModelInfo(model); return mi ? (const char *)mi + 4 : ""; }
enum { MI_SPECIAL01 = 109 };
inline void RequestSpecialModel(int model, const char *name, int flags) { ((void(__cdecl *)(int, const char *, int))0x40AA60)(model, name, flags); }

// --- Personnages : champs supplementaires ---
inline int &PedType(void *p) { return Field<int>(p, 0x3D4); }
inline uint8_t &CurrentWeaponSlot(void *p) { return Field<uint8_t>(p, 0x504); }
inline int WeaponTypeInSlot(void *p, int slot) { return Field<int>(p, 0x408 + slot * 0x18); }

// --- Camera (TheCamera, a verifier) ---
inline uint8_t *Camera() { return (uint8_t *)0x7E4688; }
inline float &CamFade() { return *(float *)(Camera() + 0x910); }        // m_fFLOATingFade (0 = image claire)
inline bool &CamFading() { return *(bool *)(Camera() + 0x86B); }
inline short &CamFadeDir() { return *(short *)(Camera() + 0x940); }
inline bool &CamWidescreen() { return *(bool *)(Camera() + 0x6D); }
inline uint8_t &DrawFadeValue() { return *(uint8_t *)0xA10B16; }   // valeur reellement dessinee (ProcessFade)

} // namespace game
