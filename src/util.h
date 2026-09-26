// Outils communs : journal, ecriture en memoire du jeu, crochets d'IAT.
#pragma once
#include <windows.h>
#include <stdint.h>

void LogInit(const char *path);
void Log(const char *fmt, ...);

// Ecrit n octets a l'adresse donnee (retire la protection le temps de l'ecriture).
void Patch(uintptr_t addr, const void *bytes, size_t n);
void PatchNop(uintptr_t addr, size_t n);
// Remplace l'instruction a addr par un CALL/JMP rel32 vers dst ; les octets restants jusqu'a n sont des NOP.
void PatchCall(uintptr_t addr, void *dst, size_t n = 5);
void PatchJump(uintptr_t addr, void *dst, size_t n = 5);
// Remplace un pointeur (entree de vtable, IAT...) et renvoie l'ancien.
void *PatchPointer(void **slot, void *value);
// Crochet d'une importation de gta-vc.exe ; renvoie la fonction d'origine (NULL si absente).
void *HookImport(const char *dll, const char *func, void *hook);

// Detour : copie les n premiers octets de addr (instructions entieres et deplacables) dans un trampoline suivi
// d'un saut vers addr+n, puis remplace le debut de addr par un saut vers hook. Renvoie le trampoline (a appeler
// pour executer la fonction d'origine), ou NULL si les octets attendus ne sont pas la.
void *MakeDetour(uintptr_t addr, const void *expected, size_t n, void *hook);

// Chemin du dossier du jeu (avec antislash final).
const char *GameDir();
const char *IniPath();   // vccoop.ini a lire et ecrire (dossier du jeu, ou copie modifiable)
