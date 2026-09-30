// Protocole entre le jeu (dinput8.dll, 32 bits) et vcrt64.exe (64 bits, Direct3D 12 + DXR).
//
// Le pilote refuse le ray tracing materiel (DXR) a un processus 32 bits (niveau 0 sur une RTX 4090, 30/09) : les
// rayons sont traces par un programme 64 bits a cote du jeu. Le jeu lui envoie par memoire partagee ce qu'il dessine
// (maillages une fois, textures une fois, puis a chaque image : les dessins avec leur matrice et la camera) ; il
// renvoie trois images (demi-resolution par defaut). Aucun pointeur, types a taille fixe : les deux cotes n'ont pas
// la meme taille de pointeur.
#pragma once
#include <stdint.h>

#define RT_MAGIC 0x31545256u   // "VRT1"
#define RT_PROTOCOL 3

enum {
    RT_CMD_OFFSET = 4096,            // commandes d'une image (envois + dessins + camera)
    RT_CMD_SIZE = 40 << 20,
    RT_OUT_OFFSET = RT_CMD_OFFSET + RT_CMD_SIZE,   // images rendues
    RT_OUT_SIZE = 34 << 20,
    RT_MAP_SIZE = RT_OUT_OFFSET + RT_OUT_SIZE,     // taille de la memoire partagee
    RT_MAX_OUT_W = 1920, RT_MAX_OUT_H = 1080,      // 16 octets par pixel : 33 Mo au plus
    RT_OUT_BPP = 16,
};

// En-tete au debut de la memoire partagee.
struct RtHeader {
    uint32_t magic, version;
    volatile uint32_t helperState;   // 0 : demarre, 1 : pret (DXR), 2 : en echec (message dans error)
    volatile uint32_t frameSeq;      // jeu -> programme : image demandee
    volatile uint32_t doneSeq;       // programme -> jeu : image rendue
    uint32_t cmdBytes;               // taille des commandes de l'image
    uint32_t outW, outH;             // taille des images rendues
    float gpuMs;                     // temps de la derniere image (diagnostic)
    uint32_t meshCount, texCount;    // objets en memoire chez le programme (diagnostic)
    char error[256];
    char adapter[128];
};

// Commandes : [RtCmd][charge utile], alignees sur 16 octets.
enum {
    RT_CMD_MESH = 1,      // RtMesh + float pos[3*n] + float normale[3*n] (0 : aucune) + float uv[2*n] + uint32 couleur[n] (D3DCOLOR) + uint32 idx[3*tris]
    RT_CMD_MESH_DEL = 2,  // uint32 id
    RT_CMD_TEX = 3,       // RtTex + donnees (BGRA8, ou blocs BC1/BC2/BC3)
    RT_CMD_TEX_DEL = 4,   // uint32 id
    RT_CMD_FRAME = 5,     // RtFrame + RtInstance[count] (toujours la derniere commande)
};
struct RtCmd { uint32_t type, bytes; uint32_t pad[2]; };   // bytes : charge utile (sans l'en-tete)

enum { RT_MESH_TRANSIENT = 1 };   // maillage de cette image seulement (sommets reecrits par le jeu)
struct RtMesh { uint32_t id, vertices, triangles, flags; };
enum { RT_VERTEX_BYTES = 12 + 12 + 8 + 4 };

enum { RT_TEX_BGRA8 = 0, RT_TEX_BC1 = 1, RT_TEX_BC2 = 2, RT_TEX_BC3 = 3 };
struct RtTex { uint32_t id, format, width, height; };

enum { RT_INST_ALPHA = 1, RT_INST_VEHICLE = 2, RT_INST_DYNAMIC = 4 };
struct RtInstance {
    float transform[12];   // 3x4, lignes (x' = ligne 0 . (x, y, z, 1))
    uint32_t mesh, tex;    // tex 0 : aucune
    float alphaRef;
    uint32_t flags;
    uint32_t tint;         // couleur de la matiere (D3DCOLOR) : peinture des voitures
    uint32_t pad[3];
};

enum { RT_FEAT_SUN = 1, RT_FEAT_AO = 2, RT_FEAT_REFL = 4, RT_FEAT_GI = 8 };
struct RtFrame {
    float view[16], proj[16];     // camera principale du jeu (conventions Direct3D : vecteurs lignes)
    float sun[4];                 // vers le soleil (ou la lune), w = force
    float sunColor[4];            // couleur de la lumiere directe
    float ambient[4];             // lumiere ambiante (ciel)
    float skyTop[4], skyBottom[4];
    float sunAngle;               // demi-angle du disque (radians) : ombres douces
    uint32_t outW, outH;          // taille des images demandees
    uint32_t features;            // RT_FEAT_*
    uint32_t raysPerPixel;
    uint32_t frameIndex;          // bruit different a chaque image
    uint32_t instanceCount;
    float maxDistance;            // au-dela : pas de rayon (ciel, brouillard)
    float wetness;                // sol mouille (pluie) : reflets du sol
    float aoRadius;               // portee de l'occlusion (m)
    uint32_t reset;               // 1 : pas d'historique (changement de camera brutal, cinematique)
    uint32_t pad;
};

// Images rendues, a la suite (outW x outH chacune) :
//   0 : 4 demi-flottants : x = visibilite du soleil, y = profondeur le long de l'axe de vue (m, 0 : ciel),
//       z = occlusion ambiante (1 : degage), w = reserve ;
//   1 : RGBA 8 bits : reflet (rgb) et son poids (a) ;
//   2 : RGBA 8 bits : lumiere indirecte recue (rgb, x 4 : 255 = 4.0), a = reserve.
