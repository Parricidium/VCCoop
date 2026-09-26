// Partage de la sauvegarde de l'hote (saveshare.cpp).
#pragma once
#include <stdint.h>

void SaveShareFrame();
void SaveShareOnReliable(int from, const uint8_t *data, int len);
bool HostHasSave();
void SendHostSave(int peer);        // hote : peer < 0 = a tous
bool GuestWaitingForSave();         // invite : une sauvegarde arrive ou va etre chargee
void MenuRequestPage(int page);     // menu.cpp : ouvre un ecran au prochain passage du menu
void MenuRequestSelect(int entry);  // menu.cpp : valide une entree de l'ecran courant au prochain passage
int MenuCurrentPage();
void MenuRequestBack();             // menu.cpp : autotest, comme la touche Echap
