// VCCoop - lanceur : modeles des personnages du jeu en 3D (aperçu des tenues et portraits).
// Lit les fichiers du jeu du joueur (models\gta3.img / gta3.dir, data\default.ide, et VCCoop\mods qui les
// remplacent) : aucun fichier de Rockstar n'est livre avec le mod. Rendu logiciel (pas de carte graphique a
// partager avec une fenetre transparente), dans un tampon ARGB premultiplie.
#pragma once
#include <string>
#include <vector>
#include <stdint.h>

struct Model3D;

// Ouvre le catalogue du jeu (dossier avec \ final). Faux si models\gta3.dir est introuvable.
bool ImgOpen(const std::wstring &gameDir);
// Tenues au choix, dans l'ordre du menu F7 du mod (players.cpp BuildSkinList) : player, play1..12, ig*, passants 1..108.
std::vector<std::string> SkinList();
// Modele et textures du meme nom. NULL si introuvable ou illisible.
Model3D *ModelLoad(const std::string &name);
// Modele d'un fichier .dff (mod), textures du .txd donne ou, a defaut, celles du jeu du meme nom.
Model3D *ModelLoadPath(const std::wstring &dffPath, const std::wstring &txdPath);
void ModelFree(Model3D *m);
void ModelBounds(const Model3D *m, float *lo, float *hi);
// Rendu dans out (w x h, ARGB premultiplie, fond transparent). yaw : rotation autour de la verticale (0 = de face).
// style : 0 = personnage en pied, 1 = portrait (visage), 2 = objet ou vehicule (toute la boite, vue du dessus).
void ModelRender(const Model3D *m, uint32_t *out, int w, int h, float yaw, int style);
