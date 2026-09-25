// Reproduction des commandes de presentation des missions (mirror.cpp).
#pragma once
#include <stdint.h>

void MirrorInit();
void MirrorFrame(bool inGame);
// Hote, appeles par le crochet de script autour de l'execution d'une commande.
bool MirrorBefore(void *script, int ip, uint16_t op);
void MirrorAfter(void *script);
void MirrorMissionEnd();
void MirrorMissionStart();
void MirrorPlayerJoined(int peer);
void MirrorGuestsGetHostSave();      // hote : tous les invites vont charger sa sauvegarde   // hote : envoie l'etat de l'histoire a un nouvel invite
void RequestGather();
struct MirrorPoint { float x, y, z; int serial; };
extern MirrorPoint g_lastObjective, g_lastContact;   // hote : derniers marqueurs poses par les missions   // coop.cpp : l'invite se replacera a cote de l'hote

// script.cpp : execute une commande sans passer par notre crochet.
char CallOriginalProcessOneCommand(void *script);
// Traductions de references hote -> invite (entities.cpp, vehicles.cpp).
bool GuestPedForHost(uint32_t host, uint32_t &guest);
bool GuestVehicleForHost(uint32_t host, uint32_t &guest);
