// Reseau VCCoop : UDP, un hote et jusqu'a MAX_PLAYERS-1 invites. L'hote relaie l'etat de chacun a tous.
#pragma once
#include <stdint.h>

enum { MAX_PLAYERS = 4, NET_VERSION = 4 };

enum MsgType : uint8_t {
    MSG_HELLO = 1,   // invite -> hote : je veux entrer (nom)
    MSG_WELCOME,     // hote -> invite : ton numero de joueur
    MSG_FULL,        // hote -> invite : partie pleine ou version differente
    MSG_STATE,       // etat d'un joueur (invite -> hote, puis hote -> tous)
    MSG_BYE,         // depart d'un joueur
    MSG_WORLD,       // hote -> invites : heure et meteo (1 fois par seconde)
    MSG_VEHICLE,     // etat d'un vehicule reseau, envoye par son proprietaire (relaye par l'hote)
    MSG_VEH_REMOVE,  // le vehicule n'existe plus chez son proprietaire
    MSG_PED,         // hote -> invites : personnage de mission
    MSG_PED_REMOVE,  // hote -> invites : personnage de mission disparu
    MSG_RELIABLE,    // enveloppe fiable et ordonnee : seq + charge utile (voir NetSendReliable)
    MSG_ACK,         // accuse de reception cumulatif d'un flux fiable
    MSG_MARKER,      // hote -> invites : cylindre de destination d'une mission (conditions.cpp)
};

#pragma pack(push, 1)
struct MsgHello { uint8_t type, version; char name[24]; };
struct MsgWelcome { uint8_t type, id; };
struct MsgBye { uint8_t type, id; };

// Animation "d'action" (tout sauf marcher / courir / attendre), rejouee sur la copie du personnage chez les autres.
struct AnimSlot { int16_t id; uint8_t group, blend; float time; };   // id -1 : aucune

// Etat d'un joueur, envoye ~30 fois par seconde.
struct MsgState {
    uint8_t type, id;
    uint8_t inGame;     // en partie (sinon au menu / en chargement)
    uint8_t area;       // interieur (m_nAreaCode)
    uint32_t seq;
    float pos[3];
    float speed[3];
    float heading;
    float health, armour;
    uint8_t moveState, pedState, inVehicle, weapon;
    uint32_t vehicleId;  // vehicule reseau occupe (0 = aucun)
    uint8_t seat;        // 0 = conducteur
    char outfit[21];     // tenue : nom du modele 0 chez ce joueur ("player", "play4"...)
    float fade;          // niveau du fondu de sa camera (l'hote fait foi pour les invites)
    uint8_t shots;       // compteur de tirs (chaque nouveau tir est rejoue en visuel chez les autres)
    uint8_t aiming;      // vise (bras leve)
    char name[24];
    uint32_t time;       // GetTickCount de l'envoi (interpolation, interp.cpp)
    AnimSlot anims[3];   // animations en cours hors marche (coups, sauts, chutes...), les plus visibles d'abord
    uint8_t shared;      // invite : il voit les passants et la circulation de l'hote (population.cpp)
};
struct MsgWorld {
    uint8_t type;
    uint8_t hours, minutes, seconds;
    short oldWeather, newWeather, forcedWeather;
    uint32_t playerHandle;   // reference de pool du Tommy de l'hote (pour traduire les commandes qui le visent)
    float fade;              // niveau du fondu de la camera de l'hote (0 = image claire, 255 = noir)
    uint8_t fading, widescreen;
    uint8_t friendlyFire;    // les joueurs peuvent se blesser entre eux (reglage TirAmi de l'hote)
};
// Vehicule reseau : identifiant = (numero du joueur qui l'a cree << 24) | compteur.
struct MsgVehicle {
    uint8_t type, owner, driver, vclass;   // driver : numero du joueur au volant, 0xFF sinon
    uint32_t id;
    uint16_t model;
    uint8_t color1, color2;
    float pos[3], right[3], fwd[3], speed[3], turn[3];
    float health, steer, gas, brake;
    uint32_t poolHandle;     // reference de pool chez le proprietaire (traduction des commandes de l'hote)
    uint32_t time;           // GetTickCount de l'envoi
    float wheelSpin[4];      // rotation des roues par 1/50 s (moto : avant, arriere)
};
struct MsgVehRemove { uint8_t type; uint32_t id; };

// Personnage de mission de l'hote, identifie par sa reference de pool chez l'hote.
struct MsgPed {
    uint8_t type, moveState, pedState, pedType;
    uint32_t handle;
    uint16_t model;
    uint8_t area, seat;
    uint32_t vehicleId;     // vehicule reseau occupe (0 = a pied)
    float pos[3], speed[3];
    float heading, health;
    int32_t weapon;         // type d'arme en main
    char modelName[21];     // pour les personnages speciaux (Lance, Ken...)
    uint32_t time;          // GetTickCount de l'envoi
    AnimSlot anims[2];      // animations d'action (se battre, tomber, se relever...)
};
struct MsgPedRemove { uint8_t type; uint32_t handle; };
#pragma pack(pop)

struct NetPlayer {
    bool connected;
    MsgState state;     // dernier etat recu
    uint32_t lastSeen;  // GetTickCount de la derniere reception
};

extern NetPlayer g_players[MAX_PLAYERS];
extern int g_localId;   // 0 = hote ; -1 = invite pas encore accepte

bool NetStart();        // selon g_cfg (hote ou invite)
void NetPoll();         // lit tous les paquets en attente
void NetSendState(const MsgState &s);
void NetSendToGuests(const void *data, int len);   // hote seulement
extern void (*g_onWorld)(const MsgWorld &w);       // invite : appele a la reception d'un MsgWorld
extern void (*g_onState)(const MsgState &s);       // chaque etat de joueur recu (pour son interpolation)
extern void (*g_onVehicle)(const MsgVehicle &v);
extern void (*g_onVehRemove)(uint32_t id);
extern void (*g_onPed)(const MsgPed &p);
extern void (*g_onPedRemove)(uint32_t handle);
void NetSendToAll(const void *data, int len);      // hote : a tous les invites ; invite : a l'hote (qui relaie)

// Flux fiable et ordonne (renvoye jusqu'a accuse de reception). Hote : vers chaque invite ; invite : vers l'hote.
// La charge utile commence par son propre octet de type (RL_*).
enum { MAX_RELIABLE_PAYLOAD = 480 };
void NetSendReliable(const void *data, int len);
void NetSendReliableTo(int peer, const void *data, int len);   // hote : a un invite precis
extern void (*g_onReliable)(int from, const uint8_t *data, int len);
extern void (*g_onJoin)(int peer);   // hote : un invite vient d'entrer
bool NetIsHost();
// Hote : un invite en partie, en population partagee, est-il a moins de r metres de p (meme interieur) ?
bool NearAnyGuest(const float *p, uint8_t area, float r);
// Distances de la population partagee : l'invite la rejoint a SHARE_ENTER_M de l'hote, la quitte a SHARE_LEAVE_M ;
// l'hote lui envoie alors ses passants et sa circulation jusqu'a AMBIENT_SHARE_M autour de lui.
enum { SHARE_ENTER_M = 120, SHARE_LEAVE_M = 170, AMBIENT_SHARE_M = 200 };
