// Population partagee (population.cpp).
#pragma once

void InstallPopulation();
void PopulationFrame(bool inGame);
bool PopulationShared();
bool IsLawPed(void *ped);        // forces de l'ordre (type 6)
bool IsLawVehicle(void *v);      // vehicule de police / FBI / armee
bool LocalWanted();              // le joueur local a des etoiles   // invite : il voit les passants et voitures de l'hote
