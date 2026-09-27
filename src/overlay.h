// Etat propre a chaque invite (argent, armes, immeubles achetes), garde d'un chargement a l'autre de la
// sauvegarde de l'hote.
#pragma once

void OverlayFrame(bool inGame);
void OverlayBuyStart();          // invite : sa mission d'achat d'immeuble commence (photo des variables)
void OverlayBuyEnd();            // invite : elle finit (ce qui a change lui appartient)
void OverlayLoadedHostSave();    // invite : la sauvegarde de l'hote vient d'etre chargee (remise au prochain passage en jeu)
