// Reproduction des commandes de presentation des missions (mirror.cpp).
#pragma once
#include <stdint.h>

void MirrorInit();
void MirrorFrame(bool inGame);
// Hote, appeles par le crochet de script autour de l'execution d'une commande.
bool MirrorBefore(void *script, int ip, uint16_t op);
void MirrorAfter(void *script);
void MirrorMissionEnd();
void MirrorHostNewGame();   // hote : nouvelle partie lancee depuis le menu pause
void MirrorMissionStart(int mission);   // numero de la mission (0 = INITIAL : ses objets et marqueurs sont permanents)
void MirrorOnTimers(const uint8_t *buf, int len);   // invite : valeurs des minuteurs de mission de l'hote
void MirrorPlayerJoined(int peer);
void MirrorGuestsGetHostSave();      // hote : tous les invites vont charger sa sauvegarde   // hote : envoie l'etat de l'histoire a un nouvel invite
void RequestGather();
void MirrorFollowHostArea(int area);
void CoopWatchWalls();   // coop.cpp : quelques secondes de surveillance "pas dans un mur" apres une teleportation   // invite : prend la zone visible de l'hote (il est pose a cote de lui)
void MirrorLocal(uint16_t op, int n, const int32_t *vals);   // invite : execute une commande de script chez soi
struct MirrorPoint { float x, y, z; int serial; };
extern MirrorPoint g_lastObjective, g_lastContact;   // hote : derniers marqueurs poses par les missions   // coop.cpp : l'invite se replacera a cote de l'hote

// script.cpp : execute une commande sans passer par notre crochet.
char CallOriginalProcessOneCommand(void *script);
// Traductions de references hote -> invite (entities.cpp, vehicles.cpp).
bool GuestPedForHost(uint32_t host, uint32_t &guest);
bool GuestVehicleForHost(uint32_t host, uint32_t &guest);
