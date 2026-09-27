// Animations "d'action" (coups, sauts, chutes, portieres...) recopiees d'une machine a l'autre (anims.cpp).
#pragma once
#include "net.h"

// Ce qu'on a mis sur un personnage distant (pour le retirer quand l'original ne l'a plus).
struct AnimMirror {
    int16_t ids[6];
    int count;
    bool inAction;   // une animation recue occupe tout le corps (la marche est suspendue)
};

// Les n animations d'action les plus visibles d'un personnage (id -1 : case vide).
void CollectAnimSlots(void *ped, AnimSlot *out, int n);
// Applique les animations recues ; vrai tant que l'une d'elles remplace la marche (ne pas appeler SetMoveAnim).
// A la fin d'une action, oblige SetMoveAnim a remettre la marche ou l'attente.
bool ApplyActionAnims(void *ped, const AnimSlot *slots, int n, AnimMirror &m);
// Efface les reactions (touche, a terre, se relever) que notre jeu met de lui-meme sur un personnage distant :
// chez lui, il ne les vit pas forcement ; on ne montre que celles recues.
void ClearLocalReactions(void *ped, AnimMirror &m);
void EnsureLiveAnim(void *ped);   // plus aucune animation vivante : remet l'attente (sinon plantage 0x403ED2)
void InstallAnimGuards();         // detours des callbacks d'image du moteur (liste vide)
