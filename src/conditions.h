// Conditions de mission elargies aux invites, marqueurs de destination (conditions.cpp).
#pragma once
#include <stdint.h>

void InstallConditions();
void ConditionsBeginCommand(void *script, uint16_t op);          // script.cpp, avant chaque commande
void ConditionsAfterCommand(void *script, uint16_t op, int ip);  // script.cpp, apres (hote)
void ConditionsFrame(bool inGame);
void OnMarker(const uint8_t *data, int len);
void OnCorona(const uint8_t *data, int len);
// Autotest : derniers cylindres vus (hote : les siens ; invite : ceux recus de l'hote pendant les missions).
struct AutotestMarker { float x, y, z; uint32_t at; int ip; };
extern AutotestMarker g_mainMarker, g_missionMarker;
