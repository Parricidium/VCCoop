// Rendu moderne (local, optionnel) sur le peripherique Direct3D 8 du jeu.
// Le jeu (RenderWare) dessine tout en fonctions fixes : ~480 dessins par image, formats de sommets 0x112
// (position, normale, UV) et 0x142 (+ couleur) pour le monde, les vehicules et les personnages, 0x144 pour le HUD.
// On intercepte chaque dessin et on le rejoue avec nos propres shaders (vs_1_1 / ps_1_4, assembles ici a la main :
// pas de D3DX).
//
// Ombres du soleil (OmbresSoleil=1) :
//  1. chaque dessin 3D de l'image est note (tampons, matrice monde, parametres) ; a la fin de l'image, tout est
//     redessine depuis le soleil dans une carte de profondeur (texture G16R16 2048x2048, projection orthogonale de
//     300 m autour de la camera) ;
//  2. juste apres chaque dessin du jeu, l'objet est redessine une seconde fois avec un shader qui compare sa
//     profondeur vue du soleil a la carte de l'image precedente, et assombrit (melange dest * src) ce qui est a
//     l'ombre. Test de profondeur "inferieur ou egal" avec un petit biais : seules les surfaces visibles sont touchees.
// Table virtuelle IDirect3DDevice8 utilisee : 6 GetDirect3D, 20 CreateTexture, 26 CreateDepthStencilSurface,
// 31 SetRenderTarget, 32 GetRenderTarget, 33 GetDepthStencilSurface, 34 BeginScene, 35 EndScene, 36 Clear,
// 37 SetTransform, 40 SetViewport, 50 SetRenderState, 51 GetRenderState, 60 GetTexture, 61 SetTexture,
// 62 GetTextureStageState, 63 SetTextureStageState, 70 DrawPrimitive, 71 DrawIndexedPrimitive, 72/73 ...UP,
// 75 CreateVertexShader, 76 SetVertexShader, 79 SetVertexShaderConstant, 83 SetStreamSource, 85 SetIndices,
// 87 CreatePixelShader, 88 SetPixelShader, 91 SetPixelShaderConstant.
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include <math.h>
#include <string.h>
#include <vector>

using namespace game;

enum { VT_GETD3D = 6, VT_CREATETEX = 20, VT_CREATEDS = 26, VT_SETRT = 31, VT_GETRT = 32, VT_GETDS = 33, VT_BEGINSCENE = 34, VT_ENDSCENE = 35,
       VT_CLEAR = 36, VT_SETTRANSFORM = 37, VT_SETVIEWPORT = 40, VT_SETRS = 50, VT_GETRS = 51, VT_GETTEXTURE = 60, VT_SETTEXTURE = 61,
       VT_GETTSS = 62, VT_SETTSS = 63, VT_DRAWPRIM = 70, VT_DRAWINDEXED = 71, VT_DRAWPRIMUP = 72, VT_DRAWINDEXEDUP = 73,
       VT_CREATEVS = 75, VT_SETVS = 76, VT_SETVSCONST = 79, VT_SETSTREAM = 83, VT_SETINDICES = 85, VT_CREATEPS = 87, VT_SETPS = 88,
       VT_SETPSCONST = 91, VT_TEX_GETSURFACE = 15, VT_D3D_CHECKFORMAT = 10 };
enum { RS_ZENABLE = 7, RS_ZWRITE = 14, RS_ALPHATEST = 15, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULL = 22, RS_ZFUNC = 23, RS_ALPHABLEND = 27,
       RS_FOG = 28, RS_ZBIAS = 47, RS_LIGHTING = 137, RS_COLORWRITE = 168 };
enum { TSS_MAG = 5, TSS_MIN = 6, TSS_MIP = 7, TSS_ADDRU = 13, TSS_ADDRV = 14, TSS_BORDER = 22 };

typedef HRESULT(WINAPI *DrawPrim_t)(void *, UINT, UINT, UINT);
typedef HRESULT(WINAPI *DrawIndexed_t)(void *, UINT, UINT, UINT, UINT, UINT);
typedef HRESULT(WINAPI *DrawPrimUP_t)(void *, UINT, UINT, const void *, UINT);
typedef HRESULT(WINAPI *DrawIndexedUP_t)(void *, UINT, UINT, UINT, UINT, const void *, UINT, const void *, UINT);
typedef HRESULT(WINAPI *SetDword_t)(void *, DWORD);
typedef HRESULT(WINAPI *SetTransform_t)(void *, DWORD, const float *);
typedef HRESULT(WINAPI *SetTexture_t)(void *, DWORD, void *);
typedef HRESULT(WINAPI *GetTexture_t)(void *, DWORD, void **);
typedef HRESULT(WINAPI *Scene_t)(void *);
typedef HRESULT(WINAPI *SetStream_t)(void *, UINT, void *, UINT);
typedef HRESULT(WINAPI *SetIndices_t)(void *, void *, UINT);
typedef HRESULT(WINAPI *SetRS_t)(void *, DWORD, DWORD);
typedef HRESULT(WINAPI *GetRS_t)(void *, DWORD, DWORD *);
typedef HRESULT(WINAPI *SetTSS_t)(void *, DWORD, DWORD, DWORD);
typedef HRESULT(WINAPI *GetTSS_t)(void *, DWORD, DWORD, DWORD *);
typedef HRESULT(WINAPI *SetVSConst_t)(void *, DWORD, const void *, DWORD);
typedef HRESULT(WINAPI *CreateVS_t)(void *, const DWORD *, const DWORD *, DWORD *, DWORD);
typedef HRESULT(WINAPI *CreatePS_t)(void *, const DWORD *, DWORD *);
typedef HRESULT(WINAPI *CreateTex_t)(void *, UINT, UINT, UINT, DWORD, UINT, UINT, void **);
typedef HRESULT(WINAPI *CreateDS_t)(void *, UINT, UINT, UINT, UINT, void **);
typedef HRESULT(WINAPI *SetRT_t)(void *, void *, void *);
typedef HRESULT(WINAPI *GetSurf_t)(void *, void **);
typedef HRESULT(WINAPI *GetSurfLevel_t)(void *, UINT, void **);
typedef HRESULT(WINAPI *Clear_t)(void *, DWORD, const void *, DWORD, DWORD, float, DWORD);
typedef HRESULT(WINAPI *SetViewport_t)(void *, const void *);
typedef HRESULT(WINAPI *CheckFormat_t)(void *, UINT, UINT, UINT, DWORD, UINT, UINT);
typedef HRESULT(WINAPI *GetD3D_t)(void *, void **);
typedef ULONG(WINAPI *Unk_t)(void *);

static DrawPrim_t o_DrawPrim;
static DrawIndexed_t o_DrawIndexed;
static DrawPrimUP_t o_DrawPrimUP;
static DrawIndexedUP_t o_DrawIndexedUP;
static SetDword_t o_SetVS, o_SetPS;
static SetTransform_t o_SetTransform;
static SetTexture_t o_SetTexture;
static Scene_t o_BeginScene, o_EndScene;
static SetStream_t o_SetStream;
static SetIndices_t o_SetIndices;
static void *g_dev;
static void **g_vt;

template <class T> static T Vt(int i) { return (T)g_vt[i]; }
static ULONG AddRef(void *o) { return ((Unk_t)(*(void ***)o)[1])(o); }
static ULONG Release(void *o) { return o ? ((Unk_t)(*(void ***)o)[2])(o) : 0; }

// --- Etat courant ---
static DWORD g_vs, g_ps;
static float g_world[16], g_view[16], g_proj[16];
static void *g_vb, *g_ib;
static UINT g_stride, g_baseIndex;
static bool g_inOurDraw;   // nos propres dessins : pas d'interception

// --- Diagnostic (JournalScripts=1, toutes les 10 s) ---
static struct { int draws, drawsVs, drawsUp, tris, textured, fvf[8]; DWORD fvfCode[8]; } g_frame;

static int PrimTris(UINT type, UINT count) { return type == 4 || type == 5 || type == 6 ? (int)count : 0; }

static void NoteDraw(UINT type, UINT count, bool up)
{
    g_frame.draws++;
    if (up) g_frame.drawsUp++;
    g_frame.tris += PrimTris(type, count);
    if (g_vs > 0xFFFF) g_frame.drawsVs++;
    else for (int i = 0; i < 8; i++) {
        if (g_frame.fvfCode[i] == g_vs && g_frame.fvf[i]) { g_frame.fvf[i]++; break; }
        if (!g_frame.fvf[i]) { g_frame.fvfCode[i] = g_vs; g_frame.fvf[i] = 1; break; }
    }
}

// ======================================================================= Matrices (lignes, vecteurs lignes)
struct M4 { float m[16]; };
static M4 Mul(const M4 &a, const M4 &b)
{
    M4 r;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0;
        for (int k = 0; k < 4; k++) s += a.m[i * 4 + k] * b.m[k * 4 + j];
        r.m[i * 4 + j] = s;
    }
    return r;
}
static M4 Transpose(const M4 &a) { M4 r; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i * 4 + j] = a.m[j * 4 + i]; return r; }
static M4 Identity() { M4 r = {}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
static M4 LookAt(Vec3 eye, Vec3 at, Vec3 up)
{
    Vec3 z = { at.x - eye.x, at.y - eye.y, at.z - eye.z };
    float l = sqrtf(z.x * z.x + z.y * z.y + z.z * z.z); z = { z.x / l, z.y / l, z.z / l };
    Vec3 x = { up.y * z.z - up.z * z.y, up.z * z.x - up.x * z.z, up.x * z.y - up.y * z.x };
    l = sqrtf(x.x * x.x + x.y * x.y + x.z * x.z); x = { x.x / l, x.y / l, x.z / l };
    Vec3 y = { z.y * x.z - z.z * x.y, z.z * x.x - z.x * x.z, z.x * x.y - z.y * x.x };
    M4 r = Identity();
    r.m[0] = x.x; r.m[4] = x.y; r.m[8] = x.z;  r.m[12] = -(x.x * eye.x + x.y * eye.y + x.z * eye.z);
    r.m[1] = y.x; r.m[5] = y.y; r.m[9] = y.z;  r.m[13] = -(y.x * eye.x + y.y * eye.y + y.z * eye.z);
    r.m[2] = z.x; r.m[6] = z.y; r.m[10] = z.z; r.m[14] = -(z.x * eye.x + z.y * eye.y + z.z * eye.z);
    return r;
}
static M4 Ortho(float w, float h, float zn, float zf)
{
    M4 r = {};
    r.m[0] = 2.0f / w; r.m[5] = 2.0f / h; r.m[10] = 1.0f / (zf - zn); r.m[14] = -zn / (zf - zn); r.m[15] = 1;
    return r;
}

// ======================================================================= Shaders assembles a la main
// Jetons Direct3D 8 : version, instructions (opcode), parametres (0x80000000 | type << 28 | ... | numero).
enum { T_TEMP = 0, T_INPUT = 1, T_CONST = 2, T_TEX = 3, T_RASTOUT = 4, T_ATTROUT = 5, T_TEXOUT = 6 };
enum { OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_DP4 = 9, OP_TEXCOORD = 64, OP_TEX = 66, OP_CMP = 88, OP_END = 0xFFFF };
struct Asm {
    std::vector<DWORD> t;
    void op(DWORD o) { t.push_back(o); }
    void dst(int type, int n, int mask = 0xF) { t.push_back(0x80000000u | (DWORD)type << 28 | (DWORD)mask << 16 | (DWORD)n); }
    void src(int type, int n, int swz = 0xE4, int mod = 0) { t.push_back(0x80000000u | (DWORD)type << 28 | (DWORD)mod << 24 | (DWORD)swz << 16 | (DWORD)n); }
};
static const int SWZ_ZZZZ = 0xAA, SWZ_XXXX = 0x00, SWZ_YYYY = 0x55, SWZ_AAAA = 0xFF;

// vs de profondeur : oPos = v0 * c0..c3 (matrice transposee), oT0 = z (projection orthogonale : z lineaire 0..1).
static std::vector<DWORD> VsDepth()
{
    Asm a; a.op(0xFFFE0101);
    for (int i = 0; i < 4; i++) { a.op(OP_DP4); a.dst(T_TEMP, 0, 1 << i); a.src(T_INPUT, 0); a.src(T_CONST, i); }
    a.op(OP_MOV); a.dst(T_RASTOUT, 0); a.src(T_TEMP, 0);
    a.op(OP_MOV); a.dst(T_TEXOUT, 0); a.src(T_TEMP, 0, SWZ_ZZZZ);
    a.op(OP_MOV); a.dst(T_TEXOUT, 1); a.src(T_INPUT, 2);   // uv de l'objet (v2 : apres position et normale ou couleur)
    a.op(OP_END);
    return a.t;
}
// ps de profondeur : couleur = t0 (la profondeur), dans le rouge (texture G16R16) ; alpha = celui de la texture de
// l'objet (etage 1), pour que le test alpha du jeu decoupe les feuillages et grillages.
static std::vector<DWORD> PsDepth()
{
    Asm a; a.op(0xFFFF0104);
    a.op(OP_TEXCOORD); a.dst(T_TEMP, 0, 0x7); a.src(T_TEX, 0);
    a.op(OP_TEX); a.dst(T_TEMP, 1); a.src(T_TEX, 1);
    a.op(OP_MOV); a.dst(T_TEMP, 0, 0x7); a.src(T_TEMP, 0, SWZ_XXXX);   // .x seul est initialise : replique
    a.op(OP_MOV); a.dst(T_TEMP, 0, 0x8); a.src(T_TEMP, 1, 0xFF);        // alpha
    a.op(OP_END);
    return a.t;
}
// vs recepteur : oPos = v0 * c0..c3 (comme le jeu) ; r1 = v0 * c4..c7 (vers la carte d'ombre, deja en UV).
// oT0 = uv, oT2/oT3/oT4 = uv decalees d'un texel (c9, c10, c11 : filtrage 2x2), oT1 = (profondeur - biais c8.x,
// profondeur - c8.y), oT5 = uv de l'objet.
static std::vector<DWORD> VsRecv()
{
    Asm a; a.op(0xFFFE0101);
    for (int i = 0; i < 4; i++) { a.op(OP_DP4); a.dst(T_TEMP, 0, 1 << i); a.src(T_INPUT, 0); a.src(T_CONST, i); }
    a.op(OP_MOV); a.dst(T_RASTOUT, 0); a.src(T_TEMP, 0);
    for (int i = 0; i < 4; i++) { a.op(OP_DP4); a.dst(T_TEMP, 1, 1 << i); a.src(T_INPUT, 0); a.src(T_CONST, 4 + i); }
    a.op(OP_MOV); a.dst(T_TEXOUT, 0); a.src(T_TEMP, 1);
    a.op(OP_MOV); a.dst(T_TEXOUT, 1); a.src(T_INPUT, 2);   // uv de l'objet (etages 0-3 seulement : au-dela, le pilote ne lit plus)
    a.op(OP_ADD); a.dst(T_TEMP, 3); a.src(T_TEMP, 1); a.src(T_CONST, 11);   // second echantillon : + (dx, dy)
    a.op(OP_MOV); a.dst(T_TEXOUT, 2); a.src(T_TEMP, 3);
    // oT3 = (z - biais, 0.999 - z) : les deux positifs (texcrd ecrete les valeurs negatives a 0), c8 = (biais, 0.999, -1, 0).
    a.op(OP_ADD); a.dst(T_TEMP, 2, 0x1); a.src(T_TEMP, 1, SWZ_ZZZZ); a.src(T_CONST, 8, SWZ_XXXX, 1);
    a.op(OP_MAD); a.dst(T_TEMP, 2, 0x2); a.src(T_TEMP, 1, SWZ_ZZZZ); a.src(T_CONST, 8, SWZ_ZZZZ); a.src(T_CONST, 8, SWZ_YYYY);
    a.op(OP_MOV); a.dst(T_TEMP, 2, 0xC); a.src(T_CONST, 8, 0xFF);
    a.op(OP_MOV); a.dst(T_TEXOUT, 3); a.src(T_TEMP, 2);
    a.op(OP_END);
    return a.t;
}
// ps recepteur : r0 = carte (t0), r1 = profondeur (t1), r3 = texture de l'objet (t2) ; r2 = carte + biais (c0) -
// profondeur ; couleur = r2 >= 0 ? c1 (eclaire, 1) : c2 (ombre) ; au-dela de la portee de la carte (profondeur >= 1,
// c3) : eclaire ; alpha = celui de la texture (test alpha du jeu).
// Phase 1 : 4 echantillons de la carte (t0, t2, t3, t4), r4 = (profondeur - biais, profondeur - 1), r5 = texture de
// l'objet ; chaque echantillon compare : carte - (profondeur - biais) >= 0 -> 1 (c1) sinon 0 (c0).
// Phase 2 : moyenne (x c2 = 0.25) -> facteur ; couleur = facteur * c3 + c4 (c4 = teinte d'ombre, c3 = 1 - c4) ;
// au-dela de la portee (profondeur - 1 >= 0) : eclaire ; alpha = celui de la texture.
// Une seule phase (le pilote refuse la seconde) : 2 echantillons en diagonale (t0, t4), 8 instructions.
// r0/r1 = carte - (profondeur - biais) >= 0 -> 1 sinon 0 ; somme ; couleur = somme * c3 (= (1 - ombre) / 2) + c4 (ombre).
// OmbresDebug (vccoop.ini) : 1 = tout a l'ombre (la passe dessine-t-elle ?), 2 = alpha force a 1, 3 = sans le test hors portee.
static int g_debug;
static std::vector<DWORD> PsRecv()
{
    Asm a; a.op(0xFFFF0104);
    a.op(OP_TEX); a.dst(T_TEMP, 0); a.src(T_TEX, 0);
    a.op(OP_TEX); a.dst(T_TEMP, 1); a.src(T_TEX, 2);
    a.op(OP_TEXCOORD); a.dst(T_TEMP, 4, 0x7); a.src(T_TEX, 3);
    a.op(OP_TEX); a.dst(T_TEMP, 5); a.src(T_TEX, 1);
    for (int i = 0; i < 2; i++) {
        a.op(OP_SUB); a.dst(T_TEMP, i); a.src(T_TEMP, i, SWZ_XXXX); a.src(T_TEMP, 4, SWZ_XXXX);
        a.op(OP_CMP); a.dst(T_TEMP, i); a.src(T_TEMP, i, SWZ_XXXX); a.src(T_CONST, 1); a.src(T_CONST, 0);
    }
    if (g_debug == 1) { a.op(OP_MOV); a.dst(T_TEMP, 0); a.src(T_CONST, 0); }
    else { a.op(OP_ADD); a.dst(T_TEMP, 0); a.src(T_TEMP, 0); a.src(T_TEMP, 1); }
    a.op(OP_MAD); a.dst(T_TEMP, 0); a.src(T_TEMP, 0); a.src(T_CONST, 3); a.src(T_CONST, 4);
    if (g_debug != 3) { a.op(OP_CMP); a.dst(T_TEMP, 0, 0x7); a.src(T_TEMP, 4, SWZ_YYYY, 1); a.src(T_CONST, 1); a.src(T_TEMP, 0); }   // -(0.999 - z) >= 0 : hors portee, eclaire
    if (g_debug == 2) { a.op(OP_MOV); a.dst(T_TEMP, 0, 0x8); a.src(T_CONST, 1, SWZ_AAAA); }
    else { a.op(OP_MOV); a.dst(T_TEMP, 0, 0x8); a.src(T_TEMP, 5, SWZ_AAAA); }
    a.op(OP_END);
    return a.t;
}
// Declaration de flux pour un format de sommets : position, normale, [couleur], [uv] (on ne lit que v0).
static std::vector<DWORD> Decl(DWORD fvf)
{
    std::vector<DWORD> d;
    d.push_back(0x20000000);   // D3DVSD_STREAM(0) : type de jeton 1 << 29
    int reg = 0;
    auto add = [&](int type) { d.push_back(0x40000000u | (DWORD)type << 16 | (DWORD)reg++); };
    add(2);                                  // XYZ : FLOAT3
    if (fvf & 0x10) add(2);                  // NORMAL
    if (fvf & 0x40) add(4);                  // DIFFUSE : D3DCOLOR
    if (fvf & 0x80) add(4);                  // SPECULAR
    int tex = (fvf >> 8) & 0xF;
    for (int i = 0; i < tex; i++) add(1);    // FLOAT2
    d.push_back(0xFFFFFFFF);
    return d;
}

// ======================================================================= Ombres du soleil
enum { MAX_RECORDS = 3000 };
static int SHADOW_SIZE = 4096;   // OmbresResolution (2048 ou 4096)
static const float SHADOW_RANGE = 250.0f;   // metres autour de la camera
static float g_sunStrength;      // 0 (soleil au ras de l'horizon) .. 1 (plein jour) : les ombres apparaissent avec l'aube
struct Record { void *vb, *ib, *tex; UINT stride, baseIndex; DWORD fvf; float world[16]; UINT type, minIdx, numVerts, start, count; bool indexed, blended; DWORD alphaTest, alphaRef, alphaFunc; };
static std::vector<Record> g_records;
static void *g_shadowTex, *g_shadowSurf, *g_shadowDs;   // carte d'ombre (image precedente), sa surface, son tampon de profondeur
static DWORD g_vsDepth[16], g_vsRecv[16], g_psDepth, g_psRecv;   // par format de sommets (index = fvf >> 4 & 0xF ...)
static bool g_shadowReady, g_shadowFailed, g_shadowValid;
static M4 g_sunViewProj;   // pour l'image courante (recepteur : carte de l'image precedente, calculee avec cette matrice-la)
static M4 g_sunViewProjPrev;
static bool g_sunUp;

static int FvfSlot(DWORD fvf) { return (int)(((fvf >> 4) & 0x7) | (((fvf >> 8) & 1) << 3)); }   // normale, couleur, speculaire, 1 uv

static bool CreateShadowResources()
{
    if (g_shadowReady || g_shadowFailed) return g_shadowReady;
    g_shadowFailed = true;
    g_debug = GetPrivateProfileIntA("VCCoop", "OmbresDebug", 0, IniPath());
    if (g_debug) Log("rendu : OmbresDebug=%d", g_debug);
    void *d3d = NULL;
    Vt<GetD3D_t>(VT_GETD3D)(g_dev, &d3d);
    UINT fmt = 34;   // D3DFMT_G16R16
    if (d3d) {
        CheckFormat_t check = (CheckFormat_t)(*(void ***)d3d)[VT_D3D_CHECKFORMAT];
        if (FAILED(check(d3d, 0, 1, 22, 1, 3, 34))) { fmt = 21; Log("rendu : G16R16 refuse comme cible, carte d'ombre 8 bits (A8R8G8B8)"); }
        Release(d3d);
    }
    SHADOW_SIZE = g_cfg.shadowRes >= 4096 ? 4096 : 2048;
    if (FAILED(Vt<CreateTex_t>(VT_CREATETEX)(g_dev, SHADOW_SIZE, SHADOW_SIZE, 1, 1, fmt, 0, &g_shadowTex)) || !g_shadowTex) {
        SHADOW_SIZE = 2048;
        if (FAILED(Vt<CreateTex_t>(VT_CREATETEX)(g_dev, SHADOW_SIZE, SHADOW_SIZE, 1, 1, fmt, 0, &g_shadowTex)) || !g_shadowTex) { Log("rendu : carte d'ombre impossible a creer"); return false; }
    }
    if (FAILED(((GetSurfLevel_t)(*(void ***)g_shadowTex)[VT_TEX_GETSURFACE])(g_shadowTex, 0, &g_shadowSurf))) return false;
    if (FAILED(Vt<CreateDS_t>(VT_CREATEDS)(g_dev, SHADOW_SIZE, SHADOW_SIZE, 80, 0, &g_shadowDs))) { Log("rendu : tampon de profondeur d'ombre impossible (D16)"); return false; }
    DWORD caps[64] = {};
    ((HRESULT(WINAPI *)(void *, void *))g_vt[7])(g_dev, caps);   // GetDeviceCaps : vs 49, ps 51
    std::vector<DWORD> pd = PsDepth(), pr = PsRecv();
    HRESULT h1 = Vt<CreatePS_t>(VT_CREATEPS)(g_dev, pd.data(), &g_psDepth), h2 = Vt<CreatePS_t>(VT_CREATEPS)(g_dev, pr.data(), &g_psRecv);
    if (FAILED(h1) || FAILED(h2)) {
        // Diagnostic : variantes du recepteur pour trouver l'instruction refusee.
        DWORD hh;
        HRESULT hv[6];
        for (int v = 0; v < 6; v++) {
            Asm a; a.op(0xFFFF0104);
            a.op(OP_TEX); a.dst(T_TEMP, 0); a.src(T_TEX, 0);
            a.op(OP_TEX); a.dst(T_TEMP, 1); a.src(T_TEX, 2);
            a.op(OP_TEXCOORD); a.dst(T_TEMP, 4, 0x7); a.src(T_TEX, 1);
            a.op(OP_TEX); a.dst(T_TEMP, 5); a.src(T_TEX, 5);
            a.op(OP_SUB); a.dst(T_TEMP, 0); a.src(T_TEMP, 0, SWZ_XXXX); a.src(T_TEMP, 4, SWZ_XXXX);
            if (v >= 1) { a.op(OP_CMP); a.dst(T_TEMP, 0); a.src(T_TEMP, 0, SWZ_XXXX); a.src(T_CONST, 1); a.src(T_CONST, 0); }
            if (v >= 2) { a.op(OP_SUB); a.dst(T_TEMP, 1); a.src(T_TEMP, 1, SWZ_XXXX); a.src(T_TEMP, 4, SWZ_XXXX); a.op(OP_CMP); a.dst(T_TEMP, 1); a.src(T_TEMP, 1, SWZ_XXXX); a.src(T_CONST, 1); a.src(T_CONST, 0); }
            if (v >= 3) { a.op(0xFFFD); a.op(OP_ADD); a.dst(T_TEMP, 0); a.src(T_TEMP, 0); a.src(T_TEMP, 1); }
            if (v >= 4) { a.op(OP_CMP); a.dst(T_TEMP, 0, 0x7); a.src(T_TEMP, 4, SWZ_YYYY); a.src(T_CONST, 1); a.src(T_TEMP, 0); }
            if (v >= 5) { a.op(OP_MOV); a.dst(T_TEMP, 0, 0x8); a.src(T_TEMP, 5, SWZ_AAAA); }
            a.op(OP_END);
            hv[v] = Vt<CreatePS_t>(VT_CREATEPS)(g_dev, a.t.data(), &hh);
        }
        Log("rendu : pixel shaders refuses : profondeur %08lX, recepteur %08lX ; variantes %08lX %08lX %08lX %08lX %08lX %08lX",
            h1, h2, hv[0], hv[1], hv[2], hv[3], hv[4], hv[5]);
        return false;
    }
    g_shadowFailed = false;
    g_shadowReady = true;
    Log("rendu : ombres du soleil pretes (carte %dx%d, format %u)", SHADOW_SIZE, SHADOW_SIZE, fmt);
    return true;
}

static bool VsFor(DWORD fvf, bool recv, DWORD &handle)
{
    int slot = FvfSlot(fvf);
    DWORD *table = recv ? g_vsRecv : g_vsDepth;
    if (table[slot] == 0xFFFFFFFF) return false;
    if (!table[slot]) {
        std::vector<DWORD> d = Decl(fvf), f = recv ? VsRecv() : VsDepth();
        if (FAILED(Vt<CreateVS_t>(VT_CREATEVS)(g_dev, d.data(), f.data(), &table[slot], 0)) || !table[slot]) {
            table[slot] = 0xFFFFFFFF;
            Log("rendu : vertex shader refuse pour le format %X", fvf);
            return false;
        }
    }
    handle = table[slot];
    return true;
}

static Vec3 SunDir()
{
    // Soleil : au ras de l'horizon a 6 h 30 (aube du jeu), culmine a 13 h, se couche vers 19 h 30 ; incline vers le sud.
    float t = ClockHours() + ClockMinutes() / 60.0f;
    float a = (t - 6.5f) / 13.0f;            // 0 lever .. 1 coucher
    float el = sinf(a * 3.14159265f) * 0.9f; // hauteur (60 degres au plus)
    float az = a * 3.14159265f;              // est -> ouest
    Vec3 d = { cosf(az) * 0.9f, -0.45f, el };
    float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    return { d.x / l, d.y / l, d.z / l };
}

// XYZ (pas XYZRHW) et une texture : monde (0x142, couleurs pre-calculees), vehicules et personnages (0x112, normales).
static bool Is3D(DWORD fvf) { return fvf <= 0xFFFF && (fvf & 0xE) == 0x2 && (fvf & 0x100); }

// Apres le dessin du jeu : redessine l'objet avec le test d'ombre (carte de l'image precedente).
static void ReceiverPass(const Record &r)
{
    DWORD vs;
    if (!VsFor(r.fvf, true, vs)) return;
    M4 world; memcpy(world.m, r.world, 64);
    M4 view; memcpy(view.m, g_view, 64);
    M4 proj; memcpy(proj.m, g_proj, 64);
    M4 wvp = Transpose(Mul(Mul(world, view), proj));
    M4 bias = Identity(); bias.m[0] = 0.5f; bias.m[5] = -0.5f; bias.m[12] = 0.5f; bias.m[13] = 0.5f;
    M4 shadow = Transpose(Mul(Mul(world, g_sunViewProjPrev), bias));
    SetVSConst_t setc = Vt<SetVSConst_t>(VT_SETVSCONST);
    SetRS_t rs = Vt<SetRS_t>(VT_SETRS);
    GetRS_t grs = Vt<GetRS_t>(VT_GETRS);
    SetTSS_t tss = Vt<SetTSS_t>(VT_SETTSS);
    GetTSS_t gtss = Vt<GetTSS_t>(VT_GETTSS);
    static const DWORD states[] = { RS_ALPHABLEND, RS_SRCBLEND, RS_DESTBLEND, RS_ZWRITE, RS_ZFUNC, RS_FOG, RS_ALPHATEST, RS_LIGHTING, RS_ZBIAS, RS_COLORWRITE };
    DWORD saved[10];
    for (int i = 0; i < 10; i++) grs(g_dev, states[i], &saved[i]);
    static const DWORD tstates[] = { TSS_MAG, TSS_MIN, TSS_MIP, TSS_ADDRU, TSS_ADDRV, TSS_BORDER };
    DWORD tsaved[6];
    for (int i = 0; i < 6; i++) gtss(g_dev, 0, tstates[i], &tsaved[i]);
    void *tex0 = NULL;
    Vt<GetTexture_t>(VT_GETTEXTURE)(g_dev, 0, &tex0);

    rs(g_dev, RS_ALPHABLEND, 1); rs(g_dev, RS_SRCBLEND, 1); rs(g_dev, RS_DESTBLEND, 3);   // ZERO, SRCCOLOR : dest * src
    rs(g_dev, RS_ZWRITE, 0); rs(g_dev, RS_ZFUNC, 4); rs(g_dev, RS_FOG, 0); rs(g_dev, RS_LIGHTING, 0); rs(g_dev, RS_ZBIAS, 1);   // test alpha : celui du jeu
    rs(g_dev, RS_COLORWRITE, 0x7);
    tss(g_dev, 0, TSS_MAG, 1); tss(g_dev, 0, TSS_MIN, 1); tss(g_dev, 0, TSS_MIP, 0);   // point
    tss(g_dev, 0, TSS_ADDRU, 5); tss(g_dev, 0, TSS_ADDRV, 5); tss(g_dev, 0, TSS_BORDER, 0xFFFFFFFF);   // hors carte : eclaire
    o_SetTexture(g_dev, 0, g_shadowTex);
    o_SetTexture(g_dev, 1, r.tex);
    o_SetTexture(g_dev, 2, g_shadowTex);
    tss(g_dev, 2, TSS_MAG, 1); tss(g_dev, 2, TSS_MIN, 1); tss(g_dev, 2, TSS_MIP, 0); tss(g_dev, 2, TSS_ADDRU, 5); tss(g_dev, 2, TSS_ADDRV, 5); tss(g_dev, 2, TSS_BORDER, 0xFFFFFFFF);
    setc(g_dev, 0, wvp.m, 4);
    setc(g_dev, 4, shadow.m, 4);
    float texel = 1.0f / SHADOW_SIZE;
    float c8[4] = { 0.0006f, 0.999f, -1, 0 }, c9[4] = { texel, 0, 0, 0 }, c10[4] = { 0, texel, 0, 0 }, c11[4] = { texel, texel, 0, 0 };   // biais ~55 cm sur 900 m
    setc(g_dev, 8, c8, 1); setc(g_dev, 9, c9, 1); setc(g_dev, 10, c10, 1); setc(g_dev, 11, c11, 1);
    float dark = 1.0f - 0.45f * g_sunStrength;
    float p0[4] = { 0, 0, 0, 0 }, p1[4] = { 1, 1, 1, 1 }, p2[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
    float db = dark * 0.92f + 0.08f;   // ombre un peu bleutee
    float p3[4] = { (1 - dark) * 0.5f, (1 - dark) * 0.5f, (1 - db) * 0.5f, 0 }, p4[4] = { dark, dark, db, 1 };
    SetVSConst_t setp = Vt<SetVSConst_t>(VT_SETPSCONST);
    setp(g_dev, 0, p0, 1); setp(g_dev, 1, p1, 1); setp(g_dev, 2, p2, 1); setp(g_dev, 3, p3, 1); setp(g_dev, 4, p4, 1);
    o_SetVS(g_dev, vs);
    o_SetPS(g_dev, g_psRecv);
    g_inOurDraw = true;
    if (r.indexed) o_DrawIndexed(g_dev, r.type, r.minIdx, r.numVerts, r.start, r.count);
    else o_DrawPrim(g_dev, r.type, r.start, r.count);
    g_inOurDraw = false;
    o_SetVS(g_dev, g_vs);
    o_SetPS(g_dev, g_ps);
    o_SetTexture(g_dev, 0, tex0);
    o_SetTexture(g_dev, 1, NULL);
    o_SetTexture(g_dev, 2, NULL);
    Release(tex0);
    for (int i = 0; i < 10; i++) rs(g_dev, states[i], saved[i]);
    for (int i = 0; i < 6; i++) tss(g_dev, 0, tstates[i], tsaved[i]);
}

// Fin d'image : la carte d'ombre de l'image, avec tout ce qui a ete dessine.
static void ShadowPass()
{
    void *rt = NULL, *ds = NULL;
    Vt<GetSurf_t>(VT_GETRT)(g_dev, &rt);
    Vt<GetSurf_t>(VT_GETDS)(g_dev, &ds);
    if (FAILED(Vt<SetRT_t>(VT_SETRT)(g_dev, g_shadowSurf, g_shadowDs))) { Release(rt); Release(ds); return; }
    struct { DWORD x, y, w, h; float zn, zf; } vp = { 0, 0, SHADOW_SIZE, SHADOW_SIZE, 0, 1 };
    Vt<SetViewport_t>(VT_SETVIEWPORT)(g_dev, &vp);
    Vt<Clear_t>(VT_CLEAR)(g_dev, 0, NULL, 3, 0xFFFFFFFF, 1.0f, 0);
    SetRS_t rs = Vt<SetRS_t>(VT_SETRS);
    GetRS_t grs = Vt<GetRS_t>(VT_GETRS);
    static const DWORD states[] = { RS_ZENABLE, RS_ZWRITE, RS_ZFUNC, RS_ALPHABLEND, RS_ALPHATEST, RS_FOG, RS_LIGHTING, RS_CULL, RS_ZBIAS, RS_COLORWRITE };
    DWORD saved[10];
    for (int i = 0; i < 10; i++) grs(g_dev, states[i], &saved[i]);
    rs(g_dev, RS_ZENABLE, 1); rs(g_dev, RS_ZWRITE, 1); rs(g_dev, RS_ZFUNC, 4); rs(g_dev, RS_ALPHABLEND, 0);
    DWORD savedRef, savedFunc;
    grs(g_dev, 24, &savedRef); grs(g_dev, 25, &savedFunc);
    rs(g_dev, RS_FOG, 0); rs(g_dev, RS_LIGHTING, 0); rs(g_dev, RS_CULL, 1); rs(g_dev, RS_ZBIAS, 0); rs(g_dev, RS_COLORWRITE, 0xF);
    o_SetPS(g_dev, g_psDepth);
    SetVSConst_t setc = Vt<SetVSConst_t>(VT_SETVSCONST);
    g_inOurDraw = true;
    int drawn = 0;
    for (const Record &r : g_records) {
        DWORD vs;
        if (!VsFor(r.fvf, false, vs)) continue;
        M4 world; memcpy(world.m, r.world, 64);
        M4 wvp = Transpose(Mul(world, g_sunViewProj));
        setc(g_dev, 0, wvp.m, 4);
        o_SetVS(g_dev, vs);
        o_SetTexture(g_dev, 1, r.tex);
        rs(g_dev, RS_ALPHATEST, r.alphaTest); rs(g_dev, 24, r.alphaRef); rs(g_dev, 25, r.alphaFunc);
        o_SetStream(g_dev, 0, r.vb, r.stride);
        if (r.indexed) { o_SetIndices(g_dev, r.ib, r.baseIndex); o_DrawIndexed(g_dev, r.type, r.minIdx, r.numVerts, r.start, r.count); }
        else o_DrawPrim(g_dev, r.type, r.start, r.count);
        drawn++;
    }
    g_inOurDraw = false;
    o_SetTexture(g_dev, 1, NULL);
    rs(g_dev, 24, savedRef); rs(g_dev, 25, savedFunc);
    o_SetPS(g_dev, g_ps);
    o_SetVS(g_dev, g_vs);
    o_SetStream(g_dev, 0, g_vb, g_stride);
    o_SetIndices(g_dev, g_ib, g_baseIndex);
    for (int i = 0; i < 10; i++) rs(g_dev, states[i], saved[i]);
    Vt<SetRT_t>(VT_SETRT)(g_dev, rt, ds);
    Release(rt); Release(ds);
    g_sunViewProjPrev = g_sunViewProj;
    g_shadowValid = drawn > 0;
    static uint32_t lastLog;
    if (g_cfg.logScripts && GetTickCount() - lastLog > 10000) { lastLog = GetTickCount(); Log("rendu : carte d'ombre : %d objets", drawn); }
}

static void ReleaseRecords()
{
    for (Record &r : g_records) { Release(r.vb); Release(r.ib); Release(r.tex); }
    g_records.clear();
}

static int g_why[6];   // diagnostic : pourquoi un dessin n'est pas note
static void MaybeRecord(UINT type, UINT minIdx, UINT numVerts, UINT start, UINT count, bool indexed)
{
    if (!g_cfg.sunShadows || !g_sunUp) { g_why[0]++; return; }
    if (g_inOurDraw) { g_why[1]++; return; }
    if (!Is3D(g_vs)) { g_why[2]++; return; }
    if (!g_vb) { g_why[3]++; return; }
    if (indexed && !g_ib) { g_why[4]++; return; }
    if (!g_shadowReady && !CreateShadowResources()) { g_why[5]++; return; }
    Record r;
    r.vb = g_vb; r.ib = indexed ? g_ib : NULL; r.stride = g_stride; r.baseIndex = g_baseIndex; r.fvf = g_vs;
    memcpy(r.world, g_world, 64);
    r.type = type; r.minIdx = minIdx; r.numVerts = numVerts; r.start = start; r.count = count; r.indexed = indexed;
    GetRS_t grs = Vt<GetRS_t>(VT_GETRS);
    // Surfaces melangees (feuillages, ombres du jeu, vitres, particules) : elles projettent une ombre (decoupee par
    // l'alpha de leur texture) mais n'en recoivent pas (leur rectangle entier serait assombri).
    DWORD blend = 0;
    grs(g_dev, RS_ALPHABLEND, &blend);
    r.blended = blend != 0;
    grs(g_dev, RS_ALPHATEST, &r.alphaTest); grs(g_dev, 24, &r.alphaRef); grs(g_dev, 25, &r.alphaFunc);
    if (r.blended) { r.alphaTest = 1; r.alphaRef = 128; r.alphaFunc = 7; }   // GREATEREQUAL
    r.tex = NULL;
    Vt<GetTexture_t>(VT_GETTEXTURE)(g_dev, 0, &r.tex);   // +1 reference
    if (g_shadowValid && !r.blended) ReceiverPass(r);
    if (g_records.size() < MAX_RECORDS) { AddRef(r.vb); if (r.ib) AddRef(r.ib); g_records.push_back(r); }
    else Release(r.tex);
}

// ======================================================================= Crochets
static HRESULT WINAPI h_DrawPrim(void *dev, UINT type, UINT start, UINT count)
{
    if (!g_inOurDraw) NoteDraw(type, count, false);
    HRESULT hr = o_DrawPrim(dev, type, start, count);
    MaybeRecord(type, 0, 0, start, count, false);
    return hr;
}
static HRESULT WINAPI h_DrawIndexed(void *dev, UINT type, UINT minIdx, UINT numVerts, UINT start, UINT count)
{
    if (!g_inOurDraw) NoteDraw(type, count, false);
    HRESULT hr = o_DrawIndexed(dev, type, minIdx, numVerts, start, count);
    MaybeRecord(type, minIdx, numVerts, start, count, true);
    return hr;
}
static HRESULT WINAPI h_DrawPrimUP(void *dev, UINT type, UINT count, const void *data, UINT stride) { NoteDraw(type, count, true); return o_DrawPrimUP(dev, type, count, data, stride); }
static HRESULT WINAPI h_DrawIndexedUP(void *dev, UINT type, UINT minIdx, UINT numVerts, UINT count, const void *idx, UINT idxFmt, const void *data, UINT stride)
{ NoteDraw(type, count, true); return o_DrawIndexedUP(dev, type, minIdx, numVerts, count, idx, idxFmt, data, stride); }
static HRESULT WINAPI h_SetVS(void *dev, DWORD handle) { if (!g_inOurDraw) g_vs = handle; return o_SetVS(dev, handle); }
static HRESULT WINAPI h_SetPS(void *dev, DWORD handle) { if (!g_inOurDraw) g_ps = handle; return o_SetPS(dev, handle); }
static HRESULT WINAPI h_SetTexture(void *dev, DWORD stage, void *tex) { if (stage == 0 && tex && !g_inOurDraw) g_frame.textured++; return o_SetTexture(dev, stage, tex); }
static HRESULT WINAPI h_SetStream(void *dev, UINT stream, void *vb, UINT stride) { if (stream == 0 && !g_inOurDraw) { g_vb = vb; g_stride = stride; } return o_SetStream(dev, stream, vb, stride); }
static HRESULT WINAPI h_SetIndices(void *dev, void *ib, UINT base) { if (!g_inOurDraw) { g_ib = ib; g_baseIndex = base; } return o_SetIndices(dev, ib, base); }
static HRESULT WINAPI h_SetTransform(void *dev, DWORD state, const float *m)
{
    if (m) { if (state == 256) memcpy(g_world, m, 64); else if (state == 2) memcpy(g_view, m, 64); else if (state == 3) memcpy(g_proj, m, 64); }
    return o_SetTransform(dev, state, m);
}

static HRESULT WINAPI h_BeginScene(void *dev)
{
    memset(&g_frame, 0, sizeof(g_frame));
    ReleaseRecords();
    // Soleil de cette image : projection orthogonale de 300 m autour de la camera, vue depuis le soleil.
    Vec3 sun = SunDir();
    g_sunStrength = (sun.z - 0.02f) / 0.22f;   // apparait avec l'aube, plein a 14 degres
    if (g_sunStrength > 1) g_sunStrength = 1;
    g_sunUp = g_cfg.sunShadows && g_sunStrength > 0.0f && GameState() == GS_PLAYING && *(int *)0x978810 == 0;
    if (g_sunUp) {
        Vec3 cam = *(Vec3 *)(Camera() + 0x30);
        Vec3 eye = { cam.x + sun.x * 400.0f, cam.y + sun.y * 400.0f, cam.z + sun.z * 400.0f };
        Vec3 up = fabsf(sun.z) > 0.95f ? Vec3{ 0, 1, 0 } : Vec3{ 0, 0, 1 };
        M4 vp = Mul(LookAt(eye, cam, up), Ortho(SHADOW_RANGE, SHADOW_RANGE, 1.0f, 900.0f));
        // Grille de la carte alignee sur ses texels : sinon les bords des ombres tremblaient a chaque mouvement de camera.
        float cx = cam.x * vp.m[0] + cam.y * vp.m[4] + cam.z * vp.m[8] + vp.m[12];
        float cy = cam.x * vp.m[1] + cam.y * vp.m[5] + cam.z * vp.m[9] + vp.m[13];
        float step = 2.0f / SHADOW_SIZE;
        M4 snap = Identity();
        snap.m[12] = -fmodf(cx, step);
        snap.m[13] = -fmodf(cy, step);
        g_sunViewProj = Mul(vp, snap);
    } else g_shadowValid = false;
    return o_BeginScene(dev);
}
static HRESULT WINAPI h_EndScene(void *dev)
{
    if (g_sunUp && g_shadowReady && !g_records.empty()) ShadowPass();
    static uint32_t lastLog;
    if (g_cfg.logScripts && GetTickCount() - lastLog > 10000) {
        lastLog = GetTickCount();
        char fv[160];
        int n = 0;
        for (int i = 0; i < 8 && g_frame.fvf[i]; i++) n += wsprintfA(fv + n, " %X:%d", g_frame.fvfCode[i], g_frame.fvf[i]);
        Log("rendu : %d dessins (%d par shader, %d UP), %d triangles, %d textures ; FVF%s ; ombres %d objets, soleil %d ; refus %d/%d/%d/%d/%d/%d",
            g_frame.draws, g_frame.drawsVs, g_frame.drawsUp, g_frame.tris, g_frame.textured, fv, (int)g_records.size(), g_sunUp,
            g_why[0], g_why[1], g_why[2], g_why[3], g_why[4], g_why[5]);
        memset(g_why, 0, sizeof(g_why));
    }
    return o_EndScene(dev);
}

// Appele une fois par la creation du peripherique (window.cpp).
void GfxHookDevice(void *dev)
{
    if (o_DrawPrim || !dev) return;
    g_dev = dev;
    g_vt = *(void ***)dev;
    void **vt = g_vt;
    o_BeginScene = (Scene_t)PatchPointer(&vt[VT_BEGINSCENE], (void *)h_BeginScene);
    o_EndScene = (Scene_t)PatchPointer(&vt[VT_ENDSCENE], (void *)h_EndScene);
    o_SetTransform = (SetTransform_t)PatchPointer(&vt[VT_SETTRANSFORM], (void *)h_SetTransform);
    o_SetTexture = (SetTexture_t)PatchPointer(&vt[VT_SETTEXTURE], (void *)h_SetTexture);
    o_DrawPrim = (DrawPrim_t)PatchPointer(&vt[VT_DRAWPRIM], (void *)h_DrawPrim);
    o_DrawIndexed = (DrawIndexed_t)PatchPointer(&vt[VT_DRAWINDEXED], (void *)h_DrawIndexed);
    o_DrawPrimUP = (DrawPrimUP_t)PatchPointer(&vt[VT_DRAWPRIMUP], (void *)h_DrawPrimUP);
    o_DrawIndexedUP = (DrawIndexedUP_t)PatchPointer(&vt[VT_DRAWINDEXEDUP], (void *)h_DrawIndexedUP);
    o_SetVS = (SetDword_t)PatchPointer(&vt[VT_SETVS], (void *)h_SetVS);
    o_SetPS = (SetDword_t)PatchPointer(&vt[VT_SETPS], (void *)h_SetPS);
    o_SetStream = (SetStream_t)PatchPointer(&vt[VT_SETSTREAM], (void *)h_SetStream);
    o_SetIndices = (SetIndices_t)PatchPointer(&vt[VT_SETINDICES], (void *)h_SetIndices);
    Log("rendu : appels de dessin interceptes (ombres du soleil %s)", g_cfg.sunShadows ? "activees" : "desactivees");
}

// Le peripherique est remis a zero (changement de resolution) : nos ressources en memoire video sont perdues.
void GfxBeforeReset()
{
    ReleaseRecords();
    Release(g_shadowSurf); Release(g_shadowTex); Release(g_shadowDs);
    g_shadowSurf = g_shadowTex = g_shadowDs = NULL;
    g_shadowReady = false;
    g_shadowValid = false;
}
