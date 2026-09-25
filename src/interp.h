// Interpolation des entites distantes (interp.cpp).
// Chaque etat recu (joueur, vehicule, personnage) porte l'heure de son envoi. On affiche les autres un peu dans
// le passe (INTERP_DELAY_MS) en interpolant entre deux etats recus : le mouvement reste fluide malgre la gigue du
// reseau. Sans etat assez recent, on extrapole avec la vitesse (250 ms au plus).
#pragma once
#include <stdint.h>

enum { INTERP_DELAY_MS = 110, TRACK_SNAPS = 10 };

struct Snap {
    uint32_t t;             // heure d'envoi (GetTickCount de l'expediteur)
    float pos[3], vel[3];   // vitesse : unites du jeu par 1/50 s
    float right[3], fwd[3]; // orientation (vehicules)
    float heading;          // cap (personnages)
};

struct Track {
    Snap s[TRACK_SNAPS];    // du plus ancien au plus recent
    int count;
    void Clear() { count = 0; }
    void Push(const Snap &n);
};

// Horloge de chaque joueur (0..3) : ecart entre son GetTickCount et le notre, a appeler a chaque reception.
void ClockSample(int src, uint32_t senderTime);
// Etat a afficher maintenant pour cette piste (dont la source est le joueur src). Faux si elle est vide.
// linear : pour les personnages, dont la vitesse du jeu ne suit pas le mouvement (il vient des animations) ; la
// vitesse est alors deduite des positions recues.
bool TrackSample(const Track &tr, int src, Snap &out, bool linear = false);

// Crochet juste apres CGame::Process (physique faite, rendu pas encore commence) : c'est la qu'on place les
// entites distantes, sinon la physique de l'image suivante les deplace avant qu'on les voie.
void InstallInterp();
void PuppetsAfterProcess();    // coop.cpp
void VehiclesAfterProcess();   // vehicles.cpp
void GhostsAfterProcess();     // entities.cpp
