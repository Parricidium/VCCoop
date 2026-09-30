// Protocole entre le jeu (dinput8.dll, 32 bits) et vcrt64.exe (64 bits, Direct3D 12 + DXR).
//
// Le pilote refuse le ray tracing materiel (DXR) a un processus 32 bits (niveau 0 sur une RTX 4090, 30/09) : les
// rayons sont traces par un programme 64 bits a cote du jeu. Le jeu lui envoie par memoire partagee ce qu'il dessine
// (maillages une fois, textures une fois, puis a chaque image : les dessins avec leur matrice et la camera) ; il
// renvoie une image (demi-resolution par defaut) : visibilite du soleil, profondeur, etc. Aucun pointeur, types a
// taille fixe : les deux cotes n'ont pas la meme taille de pointeur.
#pragma once
#include <stdint.h>

#define RT_MAGIC 0x31545256u   // "VRT1"
#define RT_PROTOCOL 1

enum {
    RT_CMD_OFFSET = 4096,            // commandes d'une image (envois + dessins + camera)
    RT_CMD_SIZE = 40 << 20,
    RT_OUT_OFFSET = RT_CMD_OFFSET + RT_CMD_SIZE,   // image rendue
    RT_OUT_SIZE = 30 << 20,
    RT_MAP_SIZE = RT_OUT_OFFSET + RT_OUT_SIZE,     // taille de la memoire partagee
    RT_MAX_OUT_W = 2560, RT_MAX_OUT_H = 1440,      // 8 octets par pixel : 29,5 Mo au plus
};

// En-tete au debut de la memoire partagee.
struct RtHeader {
    uint32_t magic, version;
    volatile uint32_t helperState;   // 0 : demarre, 1 : pret (DXR), 2 : en echec (message dans error)
    volatile uint32_t frameSeq;      // jeu -> programme : image demandee
    volatile uint32_t doneSeq;       // programme -> jeu : image rendue
    uint32_t cmdBytes;               // taille des commandes de l'image
    uint32_t outW, outH;             // taille de l'image rendue
    float gpuMs;                     // temps GPU de la derniere image (diagnostic)
    uint32_t meshCount, texCount;    // objets en memoire chez le programme (diagnostic)
    char error[256];
    char adapter[128];
};

// Commandes : [RtCmd][charge utile], alignees sur 16 octets.
enum {
    RT_CMD_MESH = 1,      // RtMesh + float pos[3*n] + float uv[2*n] + uint32 idx[3*tris]
    RT_CMD_MESH_DEL = 2,  // uint32 id
    RT_CMD_TEX = 3,       // RtTex + donnees (BGRA8, ou blocs BC1/BC2/BC3)
    RT_CMD_TEX_DEL = 4,   // uint32 id
    RT_CMD_FRAME = 5,     // RtFrame + RtInstance[count] (toujours la derniere commande)
};
struct RtCmd { uint32_t type, bytes; uint32_t pad[2]; };   // bytes : charge utile (sans l'en-tete)

enum { RT_MESH_TRANSIENT = 1 };   // maillage de cette image seulement (sommets reecrits par le jeu)
struct RtMesh { uint32_t id, vertices, triangles, flags; };

enum { RT_TEX_BGRA8 = 0, RT_TEX_BC1 = 1, RT_TEX_BC2 = 2, RT_TEX_BC3 = 3 };
struct RtTex { uint32_t id, format, width, height; };

enum { RT_INST_ALPHA = 1, RT_INST_VEHICLE = 2, RT_INST_DYNAMIC = 4, RT_INST_CAMERA = 8 };
struct RtInstance {
    float transform[12];   // 3x4, lignes (x' = ligne 0 . (x, y, z, 1))
    uint32_t mesh, tex;    // tex 0 : aucune
    float alphaRef;
    uint32_t flags;
};

enum { RT_FEAT_SUN = 1 };
struct RtFrame {
    float view[16], proj[16];     // camera principale du jeu (conventions Direct3D : vecteurs lignes)
    float sun[4];                 // vers le soleil (ou la lune), w = force
    float sunAngle;               // demi-angle du disque (radians) : ombres douces
    uint32_t outW, outH;          // taille de l'image demandee
    uint32_t features;            // RT_FEAT_*
    uint32_t raysPerPixel;
    uint32_t frameIndex;          // bruit different a chaque image
    uint32_t instanceCount;
    float maxDistance;            // au-dela : pas de rayon (ciel, brouillard)
    uint32_t pad[3];
};

// Image rendue : outW x outH pixels de 4 demi-flottants :
//   x = visibilite du soleil (0..1), y = distance a la camera le long de l'axe de vue (m, 0 : ciel), z, w : reserves.
