// Combat (combat.cpp).
#pragma once
#include <stdint.h>

void InstallCombatHooks();
void CombatOnReliable(int from, const uint8_t *data, int len);
bool HoldWeapon(void *ped, int weapon);   // arme en main (visuel), faux tant que le modele charge
uint8_t LocalShotCount();
void TestMeleeHit(void *victim);
void PassengerShooting();   // chaque image : tir sur le cote pour le joueur passager
extern bool g_testPassengerFire;   // autotest : un coup de poing du joueur local sur victim
void PuppetShoot(void *ped, int weapon);   // tir visuel du Tommy d'un autre joueur (ou d'une copie de personnage)
uint8_t PedShotCount(void *ped);           // tirs reels de ce personnage chez nous (pour les rejouer sur sa copie)
bool ApplyDamage(void *ped, void *damager, int weapon, float damage, int piece, uint8_t dir);   // sans renvoi reseau
void SendVehicleDamage(uint8_t owner, uint32_t id, float damage);   // vehicles.cpp : la copie a perdu de la sante ici
bool LocalBulletRecently();
void ForgetProjectileSource(void *ped);   // avant de detruire un pantin / une copie   // le joueur local a tire a balles depuis moins de 1,5 s
