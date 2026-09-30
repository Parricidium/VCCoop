// Rendu moderne sur Direct3D 9 (derriere le pont bridge.cpp).
//
// Ombres du soleil (OmbresSoleil=1) :
//  - chaque dessin 3D du jeu vers l'image est note (tampons, matrice monde, texture, test alpha). Les tampons
//    dynamiques (personnages, effets) sont recopies tout de suite : le jeu les reecrit dans l'image ;
//  - juste avant le premier dessin 2D qui suit la scene (interface, halos), dans la MEME image :
//      1. carte d'ombre en 4 cascades (atlas 2x2 de R32F) vue du soleil du jeu (CTimeCycle, 0x792C70) : nette de
//         pres, jusqu'a ~220 m ; chaque cascade couvre une sphere fixe (rotation de la camera sans effet) calee sur
//         la grille de ses texels (pas de scintillement quand la camera bouge) ;
//      2. profondeur de la scene vue de la camera (R32F, meme taille que l'image) ;
//      3. passe plein ecran : position de chaque pixel, normale deduite de la profondeur, decalage le long de la
//         normale (pas d'acne), 25 echantillons ponderes (filtre 4x4 lisse), fondu entre cascades ; l'image est
//         multipliee par la couleur d'ombre. Un seul passage sur l'image : plus de doubles dessins qui clignotent.
//  - intensite : celle du cycle du jour du jeu (meteo, heure : 0xA10A34), hauteur du soleil, brouillard du jeu.
// Shaders HLSL compiles au lancement par le compilateur de Windows (d3dcompiler_47.dll).
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include "bridge.h"
#include <d3d9.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include <xmmintrin.h>

using namespace game;

// Etat flottant du jeu (x87 : precision, arrondi, exceptions ; SSE : MXCSR) garde tel quel autour de notre code :
// le compilateur HLSL et certains pilotes le modifient, et le jeu calcule alors faux (pointeurs -1, plantages).
static unsigned short GetFpuCw()
{
    unsigned short c;
    __asm {
        fnstcw c
    }
    return c;
}
static void SetFpuCw(unsigned short c)
{
    __asm {
        fnclex
        fldcw c
    }
}
struct FpuGuard {
    unsigned short cw;
    unsigned int csr;
    FpuGuard() : cw(GetFpuCw()), csr(_mm_getcsr()) {}
    ~FpuGuard() { SetFpuCw(cw); _mm_setcsr(csr); }
};

static IDirect3DDevice9 *g_dev;
static IDirect3DSurface9 *g_captureBefore;   // image juste avant le masque d'ombre (capture "sans")
static int g_captureStage;                    // 1 : capture demandee pour cette image
static int g_captureIndex;
static UINT g_width, g_height;
static bool g_msaa;
static float SkyChan(uintptr_t a);
static bool g_effectsPass, g_depthReady;
static void *g_curEntity;   // entite en cours de dessin (CRenderer::RenderOneNonRoad)   // pendant RenderEffects ; profondeur de la scene prete (particules douces)

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
static bool Invert(const M4 &a, M4 &out)
{
    const float *m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-12f) return false;
    for (int i = 0; i < 16; i++) out.m[i] = inv[i] / det;
    return true;
}
static Vec3 Transform(const M4 &m, float x, float y, float z, float w)
{
    float r[4];
    for (int j = 0; j < 4; j++) r[j] = x * m.m[j] + y * m.m[4 + j] + z * m.m[8 + j] + w * m.m[12 + j];
    return { r[0] / r[3], r[1] / r[3], r[2] / r[3] };
}
static float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 NormV(Vec3 a);
static Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static Vec3 Norm(Vec3 a) { float l = sqrtf(Dot(a, a)); return l > 0 ? Vec3{ a.x / l, a.y / l, a.z / l } : a; }
static Vec3 Sub(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static Vec3 NormV(Vec3 a) { return Norm(a); }
static M4 Identity4() { M4 r = {}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
// Vue d'une lumiere (main gauche, comme Direct3D) et projection en perspective.
static M4 LookDir(Vec3 eye, Vec3 dir)
{
    Vec3 z = Norm(dir);
    Vec3 up = fabsf(z.z) > 0.9f ? Vec3{ 0, 1, 0 } : Vec3{ 0, 0, 1 };
    Vec3 x = Norm(Cross(up, z));
    Vec3 y = Cross(z, x);
    M4 r = {};
    r.m[0] = x.x; r.m[4] = x.y; r.m[8] = x.z;  r.m[12] = -Dot(x, eye);
    r.m[1] = y.x; r.m[5] = y.y; r.m[9] = y.z;  r.m[13] = -Dot(y, eye);
    r.m[2] = z.x; r.m[6] = z.y; r.m[10] = z.z; r.m[14] = -Dot(z, eye);
    r.m[15] = 1;
    return r;
}
static M4 Perspective(float fovDeg, float zn, float zf)
{
    float ys = 1.0f / tanf(fovDeg * 0.5f * 3.14159265f / 180.0f);
    M4 r = {};
    r.m[0] = ys; r.m[5] = ys; r.m[10] = zf / (zf - zn); r.m[11] = 1; r.m[14] = -zn * zf / (zf - zn);
    return r;
}

// ======================================================================= Shaders
static const char kShaders[] = R"HLSL(
row_major float4x4 gMat : register(c0);
float4 gVsParams : register(c4);   // x = echelle de profondeur (vue camera)

struct VIn { float4 pos : POSITION;
#ifndef NOUV
  float2 uv : TEXCOORD0;
#endif
};
struct VOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float depth : TEXCOORD1; };

VOut VsLight(VIn i) {
  VOut o; o.pos = mul(float4(i.pos.xyz, 1), gMat);
#ifndef NOUV
  o.uv = i.uv;
#else
  o.uv = 0;
#endif
  o.depth = o.pos.z; return o;
}
// Carte d'ombre d'une lumiere : distance a la lumiere / portee.
row_major float4x4 gWorldS : register(c8);
float4 gLPos : register(c12);   // position de la lumiere, w = 1 / portee
VOut VsSpot(VIn i) {
  VOut o; o.pos = mul(float4(i.pos.xyz, 1), gMat);
#ifndef NOUV
  o.uv = i.uv;
#else
  o.uv = 0;
#endif
  float3 w = mul(float4(i.pos.xyz, 1), gWorldS).xyz;
  o.depth = length(w - gLPos.xyz) * gLPos.w; return o;
}
VOut VsView(VIn i) {
  VOut o; o.pos = mul(float4(i.pos.xyz, 1), gMat);
#ifndef NOUV
  o.uv = i.uv;
#else
  o.uv = 0;
#endif
  o.depth = o.pos.w * gVsParams.x; return o;
}

sampler2D sTex : register(s0);
float4 gAlpha : register(c0);   // x = seuil, y = 1 si test alpha
float4 PsDepth(VOut i) : COLOR {
#ifndef NOUV
  if (gAlpha.y > 0) clip(tex2D(sTex, i.uv).a - gAlpha.x);
#endif
  return float4(i.depth, 0, 0, 1);
}

// ---- passe plein ecran
struct QIn { float4 pos : POSITION; };
struct QOut { float4 pos : POSITION; };
QOut VsQuad(QIn i) { QOut o; o.pos = i.pos; return o; }

sampler2D sDepth : register(s0);
sampler2D sAtlas : register(s1);
float4 gScreen : register(c0);               // largeur, hauteur, 1/largeur, 1/hauteur
row_major float4x4 gInvVP : register(c1);    // clip -> monde
float4 gProj : register(c5);                 // P33/P34, P43, echelle de profondeur
row_major float4x4 gLight[4] : register(c6); // monde -> cascade (xy -1..1, z 0..1)
float4 gSplits : register(c22);              // fin de chaque cascade (m)
float4 gSun : register(c23);                 // vers le soleil, w = force
float4 gShadowCol : register(c24);           // couleur d'ombre
float4 gAtlas : register(c25);               // taille d'une cascade (texels), 1/taille de l'atlas, zone de fondu (fraction)
float4 gFog : register(c26);                 // debut du brouillard, fin, distance max des ombres, debut du fondu
float4 gCam : register(c27);                 // position de la camera
float4 gTexelWorld : register(c28);          // taille d'un texel (m) par cascade
float4 gBias : register(c29);                // biais de profondeur par cascade (unites de la carte)

float3 WorldAt(float2 uv, float d) {
  float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
  float w = d * gProj.z;
  float4 clipP = float4(ndc * w, w * gProj.x + gProj.y, w);
  float4 p = mul(clipP, gInvVP);
  return p.xyz / p.w;
}

// Filtre lisse 4x4 (25 lectures, poids en tente) dans la cascade c.
float Pcf(float2 uv, float z, float c) {
  float2 tile = float2(fmod(c, 2), floor(c / 2));
  float size = gAtlas.x;
  float2 st = uv * size - 0.5;
  float2 f = frac(st);
  float2 base = floor(st) - 1;
  float sum = 0;
  float wx[5] = { 1 - f.x, 1, 1, 1, f.x };
  float wy[5] = { 1 - f.y, 1, 1, 1, f.y };
  [unroll] for (int y = 0; y < 5; y++) {
    [unroll] for (int x = 0; x < 5; x++) {
      float2 t = clamp(base + float2(x, y) + 0.5, 0.5, size - 0.5);
      float m = tex2Dlod(sAtlas, float4((t + tile * size) * gAtlas.y, 0, 0)).r;
      sum += (z <= m ? 1.0 : 0.0) * wx[x] * wy[y];
    }
  }
  return sum / 16;
}

float Cascade(float3 P, float3 N, float ndl, float c) {
  float4x4 L = gLight[0];
  float tw = gTexelWorld.x, b = gBias.x;
  if (c > 2.5) { L = gLight[3]; tw = gTexelWorld.w; b = gBias.w; }
  else if (c > 1.5) { L = gLight[2]; tw = gTexelWorld.z; b = gBias.z; }
  else if (c > 0.5) { L = gLight[1]; tw = gTexelWorld.y; b = gBias.y; }
  // Decalage le long de la normale, plus grand quand le soleil est rasant (sol plat en fin de journee : sinon des
  // anneaux d'auto-ombre apparaissaient au loin), et vers le soleil (surfaces tournees vers lui seulement).
  float slope = sqrt(saturate(1 - ndl * ndl)) / max(abs(ndl), 0.1);
  float3 Q = P + N * tw * (1.5 + min(slope, 6) * 0.75) + gSun.xyz * tw * 1.0;
  float4 lp = mul(float4(Q, 1), L);
  float2 uv = lp.xy * float2(0.5, -0.5) + 0.5;
  if (any(uv < 0) || any(uv > 1) || lp.z > 1) return 1;
  return Pcf(uv, lp.z - b, c);
}

float4 PsMask(float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
  if (d >= 0.999) return 1;
  float3 P = WorldAt(uv, d);
  // Normale : voisins de gauche/droite et haut/bas, le plus proche en profondeur (pas de faux bord aux silhouettes).
  float2 dx = float2(gScreen.z, 0), dy = float2(0, gScreen.w);
  float dl = tex2Dlod(sDepth, float4(uv - dx, 0, 0)).r, dr = tex2Dlod(sDepth, float4(uv + dx, 0, 0)).r;
  float du = tex2Dlod(sDepth, float4(uv - dy, 0, 0)).r, dd = tex2Dlod(sDepth, float4(uv + dy, 0, 0)).r;
  float3 ex = abs(dr - d) < abs(d - dl) ? WorldAt(uv + dx, dr) - P : P - WorldAt(uv - dx, dl);
  float3 ey = abs(dd - d) < abs(d - du) ? WorldAt(uv + dy, dd) - P : P - WorldAt(uv - dy, du);
  float3 N = normalize(cross(ey, ex));
  float3 V = gCam.xyz - P;
  if (dot(N, V) < 0) N = -N;
  float dist = length(V);
  if (dist > gFog.z) return 1;

  float ndl = dot(N, gSun.xyz);
  float c = dist < gSplits.x ? 0 : dist < gSplits.y ? 1 : dist < gSplits.z ? 2 : 3;
  float lit = Cascade(P, N, ndl, c);
  // Fondu vers la cascade suivante sur la fin de celle-ci.
  float cEnd = c < 0.5 ? gSplits.x : c < 1.5 ? gSplits.y : c < 2.5 ? gSplits.z : gSplits.w;
  float cStart = c < 0.5 ? 0 : c < 1.5 ? gSplits.x : c < 2.5 ? gSplits.y : gSplits.z;
  float blend = saturate((dist - (cEnd - (cEnd - cStart) * gAtlas.z)) / ((cEnd - cStart) * gAtlas.z));
  [branch] if (blend > 0 && c < 2.5) lit = lerp(lit, Cascade(P, N, ndl, c + 1), blend);

  // Faces tournees a l'oppose du soleil : a l'ombre. Seulement de pres (au loin, la normale deduite de la
  // profondeur est bruitee sur les petits objets : passants noirs).
  float facing = saturate(ndl * 4 + 0.15);
  lit = min(lit, lerp(facing, 1, saturate((dist - 60) / 60)));
  float k = gSun.w;
  k *= 1 - saturate((dist - gFog.x) / max(gFog.y - gFog.x, 1));   // brouillard du jeu
  k *= 1 - saturate((dist - gFog.w) / max(gFog.z - gFog.w, 1));   // fin des ombres
  float3 col = lerp(gShadowCol.rgb, 1, lit);
  if (gAtlas.w > 0.5) return float4(lit, lit, lit, 1) * 0.999 + 0.0005 * c;   // OmbresDebug=1 : masque brut
  return float4(lerp(1, col, k), 1);
}

// ---- ray tracing (Rendu=12) : visibilite du soleil tracee par vcrt64.exe (x), avec sa profondeur (y, m).
// Image tracee plus petite que l'ecran et bruitee (quelques rayons par pixel) : filtre 4x4 pondere par la distance et
// par l'ecart de profondeur (pas de melange a travers les bords des objets).
sampler2D sRT : register(s4);
float4 gRT : register(c191);   // largeur, hauteur de l'image tracee, 1/largeur, 1/hauteur
float4 PsRTMask(float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
  if (d >= 0.999) return 1;
  float z = d * gProj.z;
  float2 st = uv * gRT.xy - 0.5;
  float2 base = floor(st);
  float2 f = st - base;
  float sum = 0, wsum = 0, nearLit = 1, nearErr = 1e9;
  [unroll] for (int y = -1; y <= 2; y++) {
    [unroll] for (int x = -1; x <= 2; x++) {
      float4 s = tex2Dlod(sRT, float4((base + float2(x, y) + 0.5) * gRT.zw, 0, 0));
      float2 o = float2(x, y) - f;
      float err = abs(s.y - z) / (z * 0.02 + 0.05);
      float w = exp(-dot(o, o) * 0.9) * exp(-err * err) * (s.y > 0 ? 1 : 0);
      sum += s.x * w; wsum += w;
      if (s.y > 0 && err < nearErr) { nearErr = err; nearLit = s.x; }
    }
  }
  float lit = wsum > 0.02 ? sum / wsum : nearLit;
  float3 P = WorldAt(uv, d);
  float dist = length(gCam.xyz - P);
  float k = gSun.w;
  k *= 1 - saturate((dist - gFog.x) / max(gFog.y - gFog.x, 1));   // brouillard du jeu
  k *= 1 - saturate((dist - gFog.w) / max(gFog.z - gFog.w, 1));   // fin des ombres
  if (gAtlas.w > 0.5) return float4(lit, lit, lit, 1);           // OmbresDebug=1 : masque brut
  float3 col = lerp(gShadowCol.rgb, 1, lit);
  return float4(lerp(1, col, k), 1);
}

// ---- lumieres dynamiques : l'image est multipliee par (1 + lumiere recue)
float4 gLightPos[48] : register(c30);    // position + portee
float4 gLightCol[48] : register(c78);    // couleur + genre (1 : phare, en cone)
float4 gLightDir[48] : register(c126);   // direction + cosinus du cone
float4 gLightCount : register(c29);   // nombre, intensite, nombre de lumieres a ombre
row_major float4x4 gLightVP[4] : register(c174);   // lumieres a ombre : monde -> carte (perspective)
float4 gLShadow : register(c190);    // taille d'une case (texels), 1 / taille de l'atlas
sampler2D sLightAtlas : register(s2);
sampler2D sScene : register(s3);   // l'image avant les lumieres

// Ombre de la lumiere k (cases 2x2 de l'atlas) : filtre 3x3 lisse (16 lectures).
float LightShadow(int k, float3 P, float3 N, float4 lpos) {
  float4 lp = mul(float4(P + N * 0.05, 1), gLightVP[k]);
  if (lp.w <= 0.02) return 1;
  float2 uv = lp.xy / lp.w * float2(0.5, -0.5) + 0.5;
  if (any(uv < 0.002) || any(uv > 0.998)) return 1;
  float z = length(P - lpos.xyz) / lpos.w - 0.012;
  float2 tile = float2(k % 2, k / 2);
  float size = gLShadow.x;
  float2 st = uv * size - 0.5;
  float2 f = frac(st);
  float2 base = floor(st) - 1;
  float wx[4] = { 1 - f.x, 1, 1, f.x };
  float wy[4] = { 1 - f.y, 1, 1, f.y };
  float sum = 0;
  [unroll] for (int y = 0; y < 4; y++) {
    [unroll] for (int x = 0; x < 4; x++) {
      float2 t = clamp(base + float2(x, y) + 0.5, 0.5, size - 0.5);
      float m = tex2Dlod(sLightAtlas, float4((t + tile * size) * gLShadow.y, 0, 0)).r;
      sum += (z <= m ? 1.0 : 0.0) * wx[x] * wy[y];
    }
  }
  return sum / 9;
}

float3 OneLight(float4 a, float4 b, float4 c, float3 P, float3 N, out float inRange) {
  float3 L = a.xyz - P;
  float dist = length(L);
  inRange = dist < a.w ? 1 : 0;
  L /= max(dist, 0.001);
  float att = saturate(1 - dist / a.w); att *= att;
  float ndl = saturate(dot(N, L) * 0.8 + 0.2);
  float spot = b.w > 0.5 ? smoothstep(c.w, c.w + (1 - c.w) * 0.6, dot(-L, c.xyz)) : 1;
  return b.rgb * att * ndl * spot;
}

float4 PsLights(float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float3 scene = tex2Dlod(sScene, float4(uv, 0, 0)).rgb;
  float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
  if (d >= 0.999) return float4(gLShadow.z > 0.5 ? 0 : scene, 1);   // ciel, feuillages : image inchangee
  float3 P = WorldAt(uv, d);
  float2 dx = float2(gScreen.z, 0), dy = float2(0, gScreen.w);
  float dl = tex2Dlod(sDepth, float4(uv - dx, 0, 0)).r, dr = tex2Dlod(sDepth, float4(uv + dx, 0, 0)).r;
  float du = tex2Dlod(sDepth, float4(uv - dy, 0, 0)).r, dd = tex2Dlod(sDepth, float4(uv + dy, 0, 0)).r;
  float3 ex = abs(dr - d) < abs(d - dl) ? WorldAt(uv + dx, dr) - P : P - WorldAt(uv - dx, dl);
  float3 ey = abs(dd - d) < abs(d - du) ? WorldAt(uv + dy, dd) - P : P - WorldAt(uv - dy, du);
  float3 N = normalize(cross(ey, ex));
  if (dot(N, gCam.xyz - P) < 0) N = -N;
  float3 sum = 0;
  float inRange;
  // Les premieres lumieres (au plus 4) ont une carte d'ombre.
  [unroll] for (int k = 0; k < 4; k++) {
    [branch] if (k < gLightCount.z) {
      float3 l = OneLight(gLightPos[k], gLightCol[k], gLightDir[k], P, N, inRange);
      [branch] if (inRange > 0 && dot(l, 1) > 0.001) sum += l * LightShadow(k, P, N, gLightPos[k]);
    }
  }
  [loop] for (int i = 0; i < 48; i++) {
    if (i >= gLightCount.x) break;
    if (i < gLightCount.z) continue;
    float3 l = OneLight(gLightPos[i], gLightCol[i], gLightDir[i], P, N, inRange);
    sum += l;
  }
  float3 light = sum * gLightCount.y;
  if (gLShadow.z > 0.5) return float4(light, 1);   // OmbresDebug=2 : lumiere recue seule
  // Couleur propre de la surface : l'image divisee par la clarte ambiante (la nuit, tout est assombri), en partie
  // desaturee (sinon un neon rose teintait tout ce qu'un phare eclaire).
  float lum = dot(scene, float3(0.3, 0.59, 0.11));
  float3 albedo = saturate(lerp(scene, lum, 0.45) / gLightCount.w);
  return float4(scene + albedo * light, 1);
}

)HLSL" R"HLSL(
// ---- eau moderne
row_major float4x4 gWorld : register(c4);
struct WIn { float4 pos : POSITION; };
struct WOut { float4 pos : POSITION; float3 world : TEXCOORD0; float depth : TEXCOORD1; };
WOut VsWater(WIn i) {
  WOut o; o.pos = mul(float4(i.pos.xyz, 1), gMat);
  o.world = mul(float4(i.pos.xyz, 1), gWorld).xyz;
  o.depth = o.pos.w; return o;
}

sampler2D sRefract : register(s1);
float4 gWCam : register(c1);        // camera, w = temps (s)
float4 gWSun : register(c2);        // vers le soleil, w = force du reflet
float4 gSkyTop : register(c3);
float4 gSkyBottom : register(c4);
float4 gWFog : register(c5);        // debut, fin du brouillard, echelle de profondeur
float4 gFogCol : register(c6);
float4 gShallow : register(c7);
float4 gDeep : register(c8);
float4 gWLight : register(c9);      // x = clarte du jour

float Hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float Noise(float2 p) {
  float2 i = floor(p), f = frac(p);
  float2 u = f * f * (3 - 2 * f);
  return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), u.x), u.y);
}
// Pente de la surface : vagues directionnelles, plus petites au loin (pas de scintillement).
float2 WaveSlope(float2 p, float t, float detail) {
  float2 s = 0;
  const float2 dirs[6] = { float2(0.8, 0.6), float2(-0.5, 0.86), float2(0.3, -0.95), float2(-0.9, -0.2), float2(0.6, -0.4), float2(-0.2, 0.7) };
  const float freq[6] = { 0.12, 0.21, 0.37, 0.61, 1.3, 2.4 };
  const float amp[6] = { 0.30, 0.22, 0.14, 0.09, 0.045, 0.025 };
  [unroll] for (int k = 0; k < 6; k++) {
    float w = k < 4 ? 1 : detail;
    float ph = dot(dirs[k], p) * freq[k] + t * (0.9 + k * 0.35);
    s += dirs[k] * cos(ph) * freq[k] * amp[k] * w;
  }
  float2 q = p * 0.9 + t * float2(0.35, 0.2);
  s += (float2(Noise(q + float2(0.01, 0)) - Noise(q - float2(0.01, 0)), Noise(q + float2(0, 0.01)) - Noise(q - float2(0, 0.01))) * 1.6) * detail;
  return s;
}

// ---- reflet : la scene redessinee en miroir sous la surface de l'eau (demi-resolution)
row_major float4x4 gRWorld : register(c4);
float4 gRSun : register(c8);      // vers le soleil, w = part ambiante
struct RIn { float4 pos : POSITION;
#ifdef HASNORMAL
  float3 nrm : NORMAL;
#endif
#ifdef HASCOLOR
  float4 col : COLOR0;
#endif
#ifdef HASUV
  float2 uv : TEXCOORD0;
#endif
};
struct ROut { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float wz : TEXCOORD1; };
ROut VsRefl(RIn i) {
  ROut o; o.pos = mul(float4(i.pos.xyz, 1), gMat);
  o.wz = mul(float4(i.pos.xyz, 1), gRWorld).z;
#ifdef HASUV
  o.uv = i.uv;
#else
  o.uv = 0;
#endif
  float4 c = 1;
#ifdef HASCOLOR
  c = i.col;
#endif
#ifdef HASNORMAL
  float3 n = normalize(mul(i.nrm, (float3x3)gRWorld));
  c.rgb *= gRSun.w + (1 - gRSun.w) * saturate(dot(n, gRSun.xyz));
#endif
  o.col = c; return o;
}
float4 gRAlpha : register(c0);    // seuil, test alpha, texture presente
float4 gRPlane : register(c1);    // hauteur de l'eau
float4 PsRefl(ROut i) : COLOR {
  clip(i.wz - gRPlane.x + 0.1);   // rien de ce qui est sous l'eau
  float4 t = gRAlpha.z > 0.5 ? tex2D(sTex, i.uv) : 1;
  if (gRAlpha.y > 0) clip(t.a - gRAlpha.x);
  return float4((t * i.col).rgb, 1);
}

sampler2D sRefl : register(s2);
float4 gWRefl : register(c10);    // x = reflet disponible, y = force, z = hauteur du plan du miroir

float4 PsWater(WOut i, float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float3 V = gWCam.xyz - i.world;
  float dist = length(V); V /= dist;
  float detail = saturate(1 - dist / 120);
  float2 sl = WaveSlope(i.world.xy, gWCam.w, detail);
  float3 N = normalize(float3(-sl * (0.35 + 0.65 * detail), 1));
  // Profondeur de l'eau sous ce point (le fond de la scene, vu par la camera).
  float sceneD = tex2Dlod(sDepth, float4(uv, 0, 0)).r * gWFog.z;
  float thick = max(sceneD - i.depth, 0);
  float2 ruv = uv + N.xy * 0.03 * saturate(thick / 4) * detail;
  float sceneD2 = tex2Dlod(sDepth, float4(ruv, 0, 0)).r * gWFog.z;
  if (sceneD2 < i.depth) { ruv = uv; sceneD2 = sceneD; }
  float thick2 = max(sceneD2 - i.depth, 0);
  float3 refr = tex2Dlod(sRefract, float4(ruv, 0, 0)).rgb;
  // Couleur de l'eau : turquoise en eau peu profonde, bleu profond au large.
  float depthMix = saturate(thick2 / 14);
  float3 body = lerp(gShallow.rgb, gDeep.rgb, depthMix) * gWLight.x;
  // Lumiere diffusee sous la surface : les flancs des vagues tournes vers le soleil s'eclaircissent en turquoise.
  body += gShallow.rgb * gWLight.x * 0.25 * saturate(dot(N.xy, gWSun.xy) * 3 + 0.3) * gWSun.w;
  float clarity = exp(-thick2 * 0.2);
  float3 col = lerp(body, refr * lerp(float3(1, 1, 1), gShallow.rgb * 1.35, 0.6), clarity * 0.85);
  // Reflet du ciel (Fresnel, moderee : l'eau garde sa couleur) et du soleil.
  float3 R = reflect(-V, N);
  float3 sky = lerp(gSkyBottom.rgb, gSkyTop.rgb, saturate(R.z * 1.6));
  float fres = 0.02 + 0.98 * pow(1 - saturate(dot(N, V)), 5);
  float3 refl = sky;
  float reflK = 0.6;
  if (gWRefl.x > 0.5) {
    // Reflet de la scene (batiments, palmiers, bateaux, voitures), deforme par les vagues ; le ciel la ou il n'y a rien.
    // Deformation faible et qui diminue au loin : plus forte, le reflet d'un rocher ou d'un quai se decollait de
    // l'objet et tremblait a cote de lui.
    float4 rs = tex2Dlod(sRefl, float4(uv + N.xy * 0.010 * detail, 0, 0));
    float onPlane = saturate(1 - abs(i.world.z - gWRefl.z) / 0.6);   // piscine, bassin : autre hauteur, ciel seul
    refl = lerp(sky, rs.rgb, rs.a * onPlane * saturate(thick2 / 1.2));   // pas sur le liseré du rivage (plage reflétée en escalier)
    reflK = gWRefl.y;
  }
  col = lerp(col, refl, saturate(fres * reflK));
  float spec = pow(saturate(dot(R, gWSun.xyz)), 280) * 3 + pow(saturate(dot(R, gWSun.xyz)), 40) * 0.12;
  col += spec * gWSun.w * float3(1, 0.95, 0.85);
  // Ecume le long des rives et autour des objets dans l'eau.
  float foamEdge = saturate(1 - thick / 0.7);
  float foamNoise = Noise(i.world.xy * 2.2 + gWCam.w * 0.6) * Noise(i.world.xy * 0.7 - gWCam.w * 0.3);
  float foam = saturate(foamEdge * (0.35 + foamNoise * 1.3)) * detail;
  col = lerp(col, float3(0.95, 0.97, 1) * max(gWLight.x, 0.15), foam * 0.75);
  // Brouillard du jeu.
  float fog = saturate((dist - gWFog.x) / max(gWFog.y - gWFog.x, 1));
  col = lerp(col, gFogCol.rgb, fog);
  return float4(col, 1);
}
)HLSL" R"HLSL(
// ---- occlusion ambiante : coins, pieds des murs, dessous des voitures, des bancs et des arbres assombris
row_major float4x4 gAoVP : register(c30);   // monde -> clip (camera principale)
float4 gAo : register(c34);                  // rayon (m), force, distance de fin (m)
sampler2D sAO : register(s1);

float4 PsAO(float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
  if (d >= 0.999) return 1;
  float w0 = d * gProj.z;
  if (w0 > gAo.z) return 1;
  float3 P = WorldAt(uv, d);
  float2 dx = float2(gScreen.z, 0), dy = float2(0, gScreen.w);
  float dl = tex2Dlod(sDepth, float4(uv - dx, 0, 0)).r, dr = tex2Dlod(sDepth, float4(uv + dx, 0, 0)).r;
  float du = tex2Dlod(sDepth, float4(uv - dy, 0, 0)).r, dd = tex2Dlod(sDepth, float4(uv + dy, 0, 0)).r;
  float3 ex = abs(dr - d) < abs(d - dl) ? WorldAt(uv + dx, dr) - P : P - WorldAt(uv - dx, dl);
  float3 ey = abs(dd - d) < abs(d - du) ? WorldAt(uv + dy, dd) - P : P - WorldAt(uv - dy, du);
  float3 N = normalize(cross(ey, ex));
  if (dot(N, gCam.xyz - P) < 0) N = -N;
  // Rotation differente sur chaque case d'une grille 4x4 : le flou 4x4 qui suit fait la moyenne des 16.
  float2 cell = fmod(floor(vpos), 4);
  float idx = cell.x * 4 + cell.y;
  float rot = idx * 0.39269908;
  float jit = frac(idx * 0.618034);
  float3 T = normalize(abs(N.z) < 0.9 ? cross(N, float3(0, 0, 1)) : cross(N, float3(1, 0, 0)));
  float3 B = cross(N, T);
  float R = gAo.x;
  float occ = 0;
  [unroll] for (int k = 0; k < 12; k++) {
    float u = (k + jit) / 12;
    float a = rot + k * 2.3999632;
    float3 dir = (T * cos(a) + B * sin(a)) * sqrt(u) + N * sqrt(1 - u);   // hemisphere, plus dense vers la normale
    float sc = lerp(0.15, 1, ((k + 1) / 12.0) * ((k + 1) / 12.0));      // plus d'echantillons pres du point
    float4 c = mul(float4(P + N * 0.03 + dir * R * sc, 1), gAoVP);
    float2 su = c.xy / max(c.w, 0.05) * float2(0.5, -0.5) + 0.5;
    float sw = tex2Dlod(sDepth, float4(su, 0, 0)).r * gProj.z;
    float hit = (c.w - sw > 0.04 && c.w > 0.05 && all(su > 0) && all(su < 1)) ? 1 : 0;
    occ += hit * saturate(R / max(abs(w0 - sw), 0.001));   // pas d'ombre d'un objet loin devant
  }
  float ao = 1 - occ / 12 * gAo.y;
  ao = lerp(ao, 1, saturate((w0 - gAo.z * 0.6) / (gAo.z * 0.4)));   // s'efface au loin
  return float4(ao, ao, ao, 1);
}

// Flou 4x4 qui respecte les bords (profondeur) : enleve le motif des rotations sans baver sur les silhouettes.
float4 PsAOBlur(float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float d0 = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
  if (d0 >= 0.999) return 1;
  float sum = 0, wsum = 0;
  [unroll] for (int y = -2; y < 2; y++) {
    [unroll] for (int x = -2; x < 2; x++) {
      float2 o = uv + float2(x, y) * gScreen.zw;
      float dk = tex2Dlod(sDepth, float4(o, 0, 0)).r;
      float wk = saturate(1 - abs(dk - d0) / (d0 * 0.03 + 0.0002));
      sum += tex2Dlod(sAO, float4(o, 0, 0)).r * wk;
      wsum += wk;
    }
  }
  float ao = sum / max(wsum, 0.001);
  return float4(ao, ao, ao, 1);
}

// ---- sols mouilles (pluie) et sols brillants (interieurs) : reflet de l'image le long du rayon reflechi
float4 gWet : register(c30);                  // mouillage, temps (s), brillance des interieurs, force
row_major float4x4 gWetVP : register(c31);    // monde -> clip (camera principale)
float4 gWetSky : register(c35);               // couleur du ciel bas (reflet quand le rayon ne touche rien) ; w : masque pret
sampler2D sDynMask : register(s5);            // personnages et vehicules (pas de sol mouille ni brillant sur eux)
float3 NormalAt(float2 uv, float d, float3 P) {
  float2 dx = float2(gScreen.z, 0), dy = float2(0, gScreen.w);
  float dl = tex2Dlod(sDepth, float4(uv - dx, 0, 0)).r, dr = tex2Dlod(sDepth, float4(uv + dx, 0, 0)).r;
  float du = tex2Dlod(sDepth, float4(uv - dy, 0, 0)).r, dd = tex2Dlod(sDepth, float4(uv + dy, 0, 0)).r;
  float3 ex = abs(dr - d) < abs(d - dl) ? WorldAt(uv + dx, dr) - P : P - WorldAt(uv - dx, dl);
  float3 ey = abs(dd - d) < abs(d - du) ? WorldAt(uv + dy, dd) - P : P - WorldAt(uv - dy, du);
  float3 N = normalize(cross(ey, ex));
  if (dot(N, gCam.xyz - P) < 0) N = -N;
  return N;
}
float3 TraceScreen(float3 P, float3 R, float2 vpos, float3 fallback, out float found);
float4 PsWet(float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float3 scene = tex2Dlod(sScene, float4(uv, 0, 0)).rgb;
  float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
  if (d >= 0.999) return float4(scene, 1);
  if (gWetSky.w > 0.5 && tex2Dlod(sDynMask, float4(uv, 0, 0)).r > 0.5) return float4(scene, 1);
  float3 P = WorldAt(uv, d);
  float3 N = NormalAt(uv, d, P);
  float3 V = gCam.xyz - P;
  float dist = length(V); V /= dist;
  if (N.z < 0.8 || dist > 160) return float4(scene, 1);
  float puddle = saturate(Noise(P.xy * 0.16) * 1.7 - 0.45);        // flaques
  float wet = gWet.z > 0 ? gWet.z : gWet.x * lerp(0.5, 1.0, puddle);
  // Ondes des gouttes dans les flaques.
  float2 rip = (float2(Noise(P.xy * 3.0 + gWet.y * 2.3), Noise(P.xy * 3.1 - gWet.y * 1.9)) - 0.5) * 0.05 * gWet.x * puddle;
  float3 Nw = normalize(float3(rip, 1));
  float3 R = reflect(-V, Nw);
  float found;
  float3 refl = TraceScreen(P + Nw * 0.03, R, vpos, gWetSky.rgb, found);
  float fres = 0.04 + 0.96 * pow(1 - saturate(dot(Nw, V)), 5);
  float3 dark = scene * (1 - 0.32 * wet * (gWet.z > 0 ? 0 : 1));    // un sol mouille est plus sombre
  float mixK = saturate(0.18 + fres * 1.5) * wet * gWet.w * saturate(1 - dist / 160);
  return float4(lerp(dark, refl, mixK), 1);
}


// Rayon reflechi suivi dans l'image (reflets a l'ecran) : couleur touchee, found = 0..1 (bords de l'image adoucis).
float3 TraceScreen(float3 P, float3 R, float2 vpos, float3 fallback, out float found) {
  float3 refl = fallback;
  found = 0;
  float t = 0.3;
  [loop] for (int k = 0; k < 28; k++) {
    float3 Q = P + R * t;
    float4 c = mul(float4(Q, 1), gWetVP);
    if (c.w < 0.1) break;
    float2 su = c.xy / c.w * float2(0.5, -0.5) + 0.5;
    if (any(su < 0) || any(su > 1)) break;
    float sw = tex2Dlod(sDepth, float4(su, 0, 0)).r * gProj.z;
    if (c.w > sw + 0.05 && c.w < sw + 1.2 + t * 0.12) {
      // Affinage (dichotomie entre le pas precedent et celui-ci) : plus de pointilles.
      float lo = t / 1.22, hi = t;
      [unroll] for (int r = 0; r < 5; r++) {
        float mid = (lo + hi) * 0.5;
        float4 cm = mul(float4(P + R * mid, 1), gWetVP);
        float2 sm = cm.xy / cm.w * float2(0.5, -0.5) + 0.5;
        float swm = tex2Dlod(sDepth, float4(sm, 0, 0)).r * gProj.z;
        if (cm.w > swm + 0.05) { hi = mid; su = sm; } else lo = mid;
      }
      float2 edge = saturate(min(su, 1 - su) * 12);
      found = edge.x * edge.y;
      refl = lerp(fallback, tex2Dlod(sScene, float4(su, 0, 0)).rgb, found);
      break;
    }
    t *= 1.22;
  }
  return refl;
}
// ---- carrosseries : reflet de la rue, des neons, du ciel (masque des vehicules en s1)
sampler2D sCarMask : register(s1);
float4 PsCarRefl(float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float3 scene = tex2Dlod(sScene, float4(uv, 0, 0)).rgb;
  float mask = tex2Dlod(sCarMask, float4(uv, 0, 0)).r;
  float shiny = saturate((mask - 0.18) / 0.22);   // pneus (texture sombre) : pas de reflet
  if (shiny <= 0) return float4(scene, 1);
  float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
  if (d >= 0.999) return float4(scene, 1);
  float3 P = WorldAt(uv, d);
  float3 N = NormalAt(uv, d, P);
  float3 V = normalize(gCam.xyz - P);
  float3 R = reflect(-V, N);
  float3 sky = lerp(gWetSky.rgb, gWetSky.rgb * 1.4 + 0.08, saturate(R.z * 2));
  float found;
  float3 refl = TraceScreen(P + N * 0.05, R, vpos, sky, found);
  float fres = 0.04 + 0.96 * pow(1 - saturate(dot(N, V)), 5);
  float k = (0.10 + fres * 0.55) * gWet.x * shiny;
  return float4(lerp(scene, refl, saturate(k)), 1);
}
// ---- lumiere indirecte : une facade eclairee teinte ses voisines (8 echantillons autour du point)
float4 PsGI(float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float3 scene = tex2Dlod(sScene, float4(uv, 0, 0)).rgb;
  float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
  if (d >= 0.999) return float4(scene, 1);
  float w0 = d * gProj.z;
  if (w0 > 70) return float4(scene, 1);
  float3 P = WorldAt(uv, d);
  float3 N = NormalAt(uv, d, P);
  float2 cell = fmod(floor(vpos), 4);
  float idx = cell.x * 4 + cell.y;
  float3 T = normalize(abs(N.z) < 0.9 ? cross(N, float3(0, 0, 1)) : cross(N, float3(1, 0, 0)));
  float3 B = cross(N, T);
  float Rr = gWet.y;
  float3 sum = 0;
  [unroll] for (int k = 0; k < 8; k++) {
    float u = (k + frac(idx * 0.618034)) / 8;
    float a = idx * 0.39269908 + k * 2.3999632;
    float3 dir = (T * cos(a) + B * sin(a)) * sqrt(u) + N * sqrt(1 - u);
    float4 c = mul(float4(P + N * 0.05 + dir * Rr * (0.25 + 0.75 * u), 1), gWetVP);
    float2 su = c.xy / max(c.w, 0.05) * float2(0.5, -0.5) + 0.5;
    if (c.w < 0.05 || any(su < 0) || any(su > 1)) continue;
    float sw = tex2Dlod(sDepth, float4(su, 0, 0)).r * gProj.z;
    float hit = c.w > sw + 0.03 ? saturate(1 - abs(c.w - sw) / Rr) : 0;
    sum += tex2Dlod(sScene, float4(su, 0, 0)).rgb * hit;
  }
  float fade = saturate(1 - w0 / 70);
  return float4(sum / 8 * fade, 1);   // lumiere renvoyee (floutee et appliquee par PsGIApply)
}
// Flou 4x4 qui respecte les bords (comme l'occlusion), puis image + couleur propre x lumiere renvoyee.
sampler2D sGI : register(s1);
float4 PsGIApply(float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float3 scene = tex2Dlod(sScene, float4(uv, 0, 0)).rgb;
  float d0 = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
  if (d0 >= 0.999) return float4(scene, 1);
  float3 sum = 0; float wsum = 0;
  [unroll] for (int y = -2; y < 2; y++) {
    [unroll] for (int x = -2; x < 2; x++) {
      float2 o = uv + float2(x, y) * gScreen.zw;
      float dk = tex2Dlod(sDepth, float4(o, 0, 0)).r;
      float wk = saturate(1 - abs(dk - d0) / (d0 * 0.03 + 0.0002));
      sum += tex2Dlod(sGI, float4(o, 0, 0)).rgb * wk;
      wsum += wk;
    }
  }
  float3 bounce = sum / max(wsum, 0.001);
  float lum = dot(scene, float3(0.3, 0.59, 0.11));
  float3 albedo = saturate(lerp(scene, lum.xxx, 0.3) * 1.4);
  return float4(scene + albedo * bounce * gWet.x, 1);
}
// masque des vehicules
// Masque des vehicules = clarte de leur texture : la peinture est sur une texture claire (la teinte vient du
// materiau, voitures noires comprises), les jantes aussi ; le caoutchouc des pneus est une texture sombre.
float4 PsFlag(VOut i) : COLOR {
#ifndef NOUV
  float l = max(dot(tex2D(sTex, i.uv).rgb, float3(0.3, 0.59, 0.11)), 0.02);
#else
  float l = 1;
#endif
  return float4(l, l, l, 1);
}
// ---- brume : la ville se fond au loin dans une teinte qui suit l'heure (doree, violette, bleutee)
float4 gHaze : register(c30);    // couleur, densite (par m)
float4 gHaze2 : register(c31);   // distance ou elle commence, chute avec la hauteur (par m), part sur l'horizon du ciel
float4 PsHaze(float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
  if (d >= 0.999) {
    float3 dir = normalize(WorldAt(uv, 0.5) - gCam.xyz);
    return float4(gHaze.rgb, gHaze2.z * saturate(1 - abs(dir.z) * 5));   // bande claire sur l'horizon
  }
  float3 P = WorldAt(uv, d);
  float dist = length(P - gCam.xyz);
  float f = 1 - exp(-max(dist - gHaze2.x, 0) * gHaze.w);
  f *= exp(-max(P.z - 6, 0) * gHaze2.y);   // moins dense en hauteur (toits, collines)
  return float4(gHaze.rgb, saturate(f) * 0.8);
}

// ---- faisceaux des phares : cones lumineux visibles dans la pluie, le brouillard, la nuit
row_major float4x4 gBeamVP : register(c0);
struct BIn { float4 pos : POSITION; float4 col : COLOR0; };
struct BOut { float4 pos : POSITION; float4 col : COLOR0; float w : TEXCOORD0; };
BOut VsBeam(BIn i) { BOut o; o.pos = mul(float4(i.pos.xyz, 1), gBeamVP); o.col = i.col; o.w = o.pos.w; return o; }
float4 gBeam : register(c30);   // densite de l'air (pluie, brouillard), echelle de profondeur
float4 PsBeam(BOut i, float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float sw = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
  sw = sw >= 0.999 ? 1e6 : sw * gBeam.y;
  float soft = saturate((sw - i.w) / 1.5);   // s'efface contre le decor au lieu de le couper
  return float4(i.col.rgb * i.col.a * gBeam.x * soft, 1);
}
)HLSL";

typedef HRESULT(WINAPI *D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
static D3DCompile_t g_compile;

static IUnknown *CompileShader(const char *entry, const char *target, bool noUv);
// Vertex shader du reflet pour un format de sommets : normale (1), couleur (2), uv (4).
static IUnknown *CompileReflShader(int flags)
{
    if (!g_compile) return NULL;
    D3D_SHADER_MACRO defs[4] = {};
    int n = 0;
    if (flags & 1) defs[n++] = { "HASNORMAL", "1" };
    if (flags & 2) defs[n++] = { "HASCOLOR", "1" };
    if (flags & 4) defs[n++] = { "HASUV", "1" };
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = g_compile(kShaders, sizeof(kShaders) - 1, "vccoop", defs, NULL, "VsRefl", "vs_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err) { if (FAILED(hr)) Log("rendu : shader VsRefl(%d) refuse : %s", flags, (const char *)err->GetBufferPointer()); err->Release(); }
    if (FAILED(hr)) return NULL;
    IDirect3DVertexShader9 *vs = NULL;
    g_dev->CreateVertexShader((const DWORD *)code->GetBufferPointer(), &vs);
    code->Release();
    return vs;
}

static IUnknown *CompileShader(const char *entry, const char *target, bool noUv)
{
    if (!g_compile) return NULL;
    D3D_SHADER_MACRO defs[] = { { "NOUV", "1" }, { NULL, NULL } };
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = g_compile(kShaders, sizeof(kShaders) - 1, "vccoop", noUv ? defs : defs + 1, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        Log("rendu : shader %s refuse : %s", entry, err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return NULL;
    }
    if (err) err->Release();
    IUnknown *out = NULL;
    if (target[0] == 'v') g_dev->CreateVertexShader((const DWORD *)code->GetBufferPointer(), (IDirect3DVertexShader9 **)&out);
    else g_dev->CreatePixelShader((const DWORD *)code->GetBufferPointer(), (IDirect3DPixelShader9 **)&out);
    code->Release();
    if (!out) Log("rendu : shader %s non cree", entry);
    return out;
}

// ======================================================================= Ressources
enum { CASCADES = 4 };
static IDirect3DVertexShader9 *g_vsLight[2], *g_vsView[2], *g_vsQuad;   // [0] avec uv, [1] sans
static IDirect3DPixelShader9 *g_psDepth[2], *g_psMask, *g_psLights, *g_psWater, *g_psRTMask;
static IDirect3DVertexShader9 *g_vsWater, *g_vsSpot[2];
static IDirect3DTexture9 *g_lightAtlas;
static IDirect3DSurface9 *g_lightAtlasSurf, *g_lightAtlasDs;
static IDirect3DVertexShader9 *g_vsRefl[8];   // selon le format : normale (1), couleur (2), uv (4)
static IDirect3DPixelShader9 *g_psRefl;
static IDirect3DPixelShader9 *g_psAO, *g_psAOBlur, *g_psWet, *g_psHaze, *g_psBeam, *g_psCarRefl, *g_psGI, *g_psGIApply, *g_psFlag[2];
static IDirect3DTexture9 *g_carMask, *g_dynMask;
static IDirect3DSurface9 *g_carMaskSurf, *g_dynMaskSurf;
static bool g_dynMaskReady;
static IDirect3DVertexShader9 *g_vsBeam;
static IDirect3DTexture9 *g_ao;          // occlusion ambiante avant le flou
static IDirect3DSurface9 *g_aoSurf;
static IDirect3DTexture9 *g_refl;
static IDirect3DSurface9 *g_reflSurf, *g_reflDs;
enum { LIGHT_TILE = 1024 };
static IDirect3DTexture9 *g_atlas, *g_screenDepth, *g_refract;
static IDirect3DSurface9 *g_atlasSurf, *g_atlasDs, *g_screenSurf, *g_screenDs, *g_refractSurf;
static IDirect3DVertexBuffer9 *g_replayVb;
static IDirect3DIndexBuffer9 *g_replayIb;
static UINT g_replayVbSize, g_replayIbSize;
static IDirect3DStateBlock9 *g_state;
static int g_cascadeSize;
static int g_debugMask;   // OmbresDebug=1 : l'image est remplacee par le masque d'ombre
static bool g_shadersTried, g_shadersOk, g_resourcesOk, g_resourcesFailed;

template <class T> static void SafeRelease(T *&p) { if (p) { p->Release(); p = NULL; } }

static bool CreateShaders()
{
    if (g_shadersTried) return g_shadersOk;
    g_shadersTried = true;
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    g_compile = m ? (D3DCompile_t)GetProcAddress(m, "D3DCompile") : NULL;
    if (!g_compile) { Log("rendu : d3dcompiler_47.dll introuvable, ombres modernes coupees"); return false; }
    for (int v = 0; v < 2; v++) {
        g_vsLight[v] = (IDirect3DVertexShader9 *)CompileShader("VsLight", "vs_3_0", v == 1);
        g_vsView[v] = (IDirect3DVertexShader9 *)CompileShader("VsView", "vs_3_0", v == 1);
        g_psDepth[v] = (IDirect3DPixelShader9 *)CompileShader("PsDepth", "ps_3_0", v == 1);
    }
    g_vsQuad = (IDirect3DVertexShader9 *)CompileShader("VsQuad", "vs_3_0", false);
    g_psMask = (IDirect3DPixelShader9 *)CompileShader("PsMask", "ps_3_0", false);
    if (g_cfg.renderer == 12) g_psRTMask = (IDirect3DPixelShader9 *)CompileShader("PsRTMask", "ps_3_0", false);
    g_psLights = (IDirect3DPixelShader9 *)CompileShader("PsLights", "ps_3_0", false);
    for (int v = 0; v < 2; v++) g_vsSpot[v] = (IDirect3DVertexShader9 *)CompileShader("VsSpot", "vs_3_0", v == 1);
    for (int v = 0; v < 8; v++) g_vsRefl[v] = (IDirect3DVertexShader9 *)CompileReflShader(v);
    g_psRefl = (IDirect3DPixelShader9 *)CompileShader("PsRefl", "ps_3_0", false);
    g_psAO = (IDirect3DPixelShader9 *)CompileShader("PsAO", "ps_3_0", false);
    g_psAOBlur = (IDirect3DPixelShader9 *)CompileShader("PsAOBlur", "ps_3_0", false);
    g_psWet = (IDirect3DPixelShader9 *)CompileShader("PsWet", "ps_3_0", false);
    g_psCarRefl = (IDirect3DPixelShader9 *)CompileShader("PsCarRefl", "ps_3_0", false);
    g_psGI = (IDirect3DPixelShader9 *)CompileShader("PsGI", "ps_3_0", false);
    g_psGIApply = (IDirect3DPixelShader9 *)CompileShader("PsGIApply", "ps_3_0", false);
    for (int v = 0; v < 2; v++) g_psFlag[v] = (IDirect3DPixelShader9 *)CompileShader("PsFlag", "ps_3_0", v == 1);
    g_psHaze = (IDirect3DPixelShader9 *)CompileShader("PsHaze", "ps_3_0", false);
    g_psBeam = (IDirect3DPixelShader9 *)CompileShader("PsBeam", "ps_3_0", false);
    g_vsBeam = (IDirect3DVertexShader9 *)CompileShader("VsBeam", "vs_3_0", false);
    g_vsWater = (IDirect3DVertexShader9 *)CompileShader("VsWater", "vs_3_0", false);
    g_psWater = (IDirect3DPixelShader9 *)CompileShader("PsWater", "ps_3_0", false);
    g_shadersOk = g_vsLight[0] && g_vsLight[1] && g_vsView[0] && g_vsView[1] && g_psDepth[0] && g_psDepth[1] && g_vsQuad && g_psMask;
    Log("rendu : shaders HLSL %s", g_shadersOk ? "prets (vs_3_0 / ps_3_0)" : "en echec");
    return g_shadersOk;
}

static void ReleaseResources()
{
    SafeRelease(g_atlasSurf); SafeRelease(g_atlas); SafeRelease(g_atlasDs);
    SafeRelease(g_screenSurf); SafeRelease(g_screenDepth); SafeRelease(g_screenDs);
    SafeRelease(g_refractSurf); SafeRelease(g_refract);
    SafeRelease(g_lightAtlasSurf); SafeRelease(g_lightAtlas); SafeRelease(g_lightAtlasDs);
    SafeRelease(g_reflSurf); SafeRelease(g_refl); SafeRelease(g_reflDs);
    SafeRelease(g_aoSurf); SafeRelease(g_ao);
    SafeRelease(g_carMaskSurf); SafeRelease(g_carMask);
    SafeRelease(g_dynMaskSurf); SafeRelease(g_dynMask);
    SafeRelease(g_replayVb); SafeRelease(g_replayIb);
    SafeRelease(g_state);
    g_replayVbSize = g_replayIbSize = 0;
    g_resourcesOk = false;
}

static bool CreateResources()
{
    if (g_resourcesOk) return true;
    if (g_resourcesFailed || !CreateShaders()) return false;
    g_resourcesFailed = true;
    // OmbresResolution : taille de l'atlas des 4 cascades (4096 : cascades de 2048, 64 Mo ; 8192 : 4096, 256 Mo).
    g_cascadeSize = g_cfg.shadowRes >= 8192 ? 4096 : g_cfg.shadowRes >= 4096 ? 2048 : 1024;
    for (;;) {
        UINT a = g_cascadeSize * 2;
        if (SUCCEEDED(g_dev->CreateTexture(a, a, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g_atlas, NULL)) &&
            SUCCEEDED(g_dev->CreateDepthStencilSurface(a, a, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &g_atlasDs, NULL))) break;
        SafeRelease(g_atlas); SafeRelease(g_atlasDs);
        if (g_cascadeSize <= 1024) { Log("rendu : atlas d'ombre impossible a creer"); return false; }
        g_cascadeSize /= 2;
    }
    g_atlas->GetSurfaceLevel(0, &g_atlasSurf);
    if (FAILED(g_dev->CreateTexture(g_width, g_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g_screenDepth, NULL)) ||
        FAILED(g_dev->CreateDepthStencilSurface(g_width, g_height, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &g_screenDs, NULL))) {
        Log("rendu : profondeur de l'ecran impossible a creer");
        ReleaseResources();
        return false;
    }
    g_screenDepth->GetSurfaceLevel(0, &g_screenSurf);
    if (SUCCEEDED(g_dev->CreateTexture(g_width, g_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_refract, NULL)))
        g_refract->GetSurfaceLevel(0, &g_refractSurf);
    if (SUCCEEDED(g_dev->CreateTexture(LIGHT_TILE * 2, LIGHT_TILE * 2, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g_lightAtlas, NULL)) &&
        SUCCEEDED(g_dev->CreateDepthStencilSurface(LIGHT_TILE * 2, LIGHT_TILE * 2, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &g_lightAtlasDs, NULL)))
        g_lightAtlas->GetSurfaceLevel(0, &g_lightAtlasSurf);
    else { SafeRelease(g_lightAtlas); SafeRelease(g_lightAtlasDs); Log("rendu : atlas des ombres des lumieres impossible"); }
    if (SUCCEEDED(g_dev->CreateTexture(g_width / 2, g_height / 2, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_refl, NULL)) &&
        SUCCEEDED(g_dev->CreateDepthStencilSurface(g_width / 2, g_height / 2, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &g_reflDs, NULL)))
        g_refl->GetSurfaceLevel(0, &g_reflSurf);
    else { SafeRelease(g_refl); SafeRelease(g_reflDs); Log("rendu : cible des reflets impossible"); }
    if (SUCCEEDED(g_dev->CreateTexture(g_width, g_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_ao, NULL)))
        g_ao->GetSurfaceLevel(0, &g_aoSurf);
    else Log("rendu : cible de l'occlusion ambiante impossible");
    if (SUCCEEDED(g_dev->CreateTexture(g_width, g_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_carMask, NULL)))
        g_carMask->GetSurfaceLevel(0, &g_carMaskSurf);
    if (SUCCEEDED(g_dev->CreateTexture(g_width, g_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_dynMask, NULL)))
        g_dynMask->GetSurfaceLevel(0, &g_dynMaskSurf);
    if (FAILED(g_dev->CreateStateBlock(D3DSBT_ALL, &g_state))) { Log("rendu : bloc d'etats impossible"); ReleaseResources(); return false; }
    g_resourcesFailed = false;
    g_resourcesOk = true;
    Log("rendu : ombres pretes (4 cascades de %dx%d, ecran %ux%u) ; eau %s, lumieres %s", g_cascadeSize, g_cascadeSize, g_width, g_height,
        g_psWater && g_refract ? "prete" : "indisponible", g_psLights ? "pretes" : "indisponibles");
    return true;
}

// ======================================================================= Dessins notes
struct Rec {
    IDirect3DVertexBuffer9 *vb;
    IDirect3DIndexBuffer9 *ib;
    IDirect3DBaseTexture9 *tex;
    UINT stride;
    bool replayVb, replayIb, receiver, mainView;
    bool recvCand;        // recevrait ombres et lumieres s'il est vu par la camera principale
    uint8_t viewIdx;      // sa camera (g_views)
    bool caster, water;   // projette une ombre ; surface de l'eau (dessinee par nous)
    bool vehicle;         // dessin d'un vehicule (reflets des carrosseries)
    bool dynamic;         // vehicule ou personnage (pas de sol mouille / brillant dessus)
    GfxDraw d;
    DWORD fvf;
    float world[16];
    float alphaRef;
    bool alphaTest;
};
static std::vector<Rec> g_recs;
static std::vector<BYTE> g_cpuVb;
static std::vector<WORD> g_cpuIb;
static float g_mainView[16], g_mainProj[16];
static bool g_haveMain, g_applied;
// Cameras des dessins de l'image (vue + projection). La principale est celle du plus grand nombre de dessins : avant,
// c'etait celle du PREMIER dessin, et selon la direction regardee le premier etait l'horizon (projection a part) :
// presque rien ne "recevait", les ombres sautaient.
struct ViewEntry { float view[16], proj[16]; int count; };
static ViewEntry g_views[8];
static int g_viewCount, g_mainIdx = -1;
static int ViewIndex(const float *view, const float *proj)
{
    for (int i = 0; i < g_viewCount; i++)
        if (!memcmp(g_views[i].view, view, 64) && !memcmp(g_views[i].proj, proj, 64)) return i;
    if (g_viewCount >= 8) return 7;
    ViewEntry &v = g_views[g_viewCount];
    memcpy(v.view, view, 64); memcpy(v.proj, proj, 64); v.count = 0;
    return g_viewCount++;
}
struct Rec;
static void ChooseMainView();
static IDirect3DSurface9 *g_backBuffer;   // tampon arriere de l'image (pour ne noter que les dessins vers lui)
static int g_after3d;                     // diagnostic : dessins 3D apres la pose du masque
static int g_why[8];                      // diagnostic : dessins 3D refuses (primitive, z, ecriture z, cible, tampon, dynamique)

static void ReleaseRecs()
{
    for (Rec &r : g_recs) { SafeRelease(r.vb); SafeRelease(r.ib); SafeRelease(r.tex); }
    g_recs.clear();
    g_cpuVb.clear();
    g_cpuIb.clear();
    g_haveMain = false;
    g_viewCount = 0;
    g_mainIdx = -1;
}

static UINT VertexCount(UINT type, UINT count)
{
    switch (type) { case D3DPT_TRIANGLELIST: return count * 3; case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return count + 2; }
    return 0;
}

static bool Outdoors()
{
    void *me = FindPlayerPed();
    return !me || AreaCode(me) == 0;
}
static bool ShadowsWanted() { return (g_cfg.sunShadows || g_cfg.moonShadows) && GameState() == GS_PLAYING && Outdoors(); }   // interieurs : pas de soleil
static bool LightsWanted() { return g_cfg.dynLights && GameState() == GS_PLAYING; }
static bool WaterWanted() { return g_cfg.modernWater && GameState() == GS_PLAYING && g_shadersOk && g_psWater; }
static bool AoWanted() { return g_cfg.ambientOcclusion && GameState() == GS_PLAYING; }
static bool RecordingWanted() { return ShadowsWanted() || LightsWanted() || WaterWanted() || AoWanted(); }

static float g_sunK;          // force des ombres de cette image (0 : rien a faire)
static Vec3 g_sun;            // vers le soleil, ou la lune la nuit (lisse)
static bool g_moon;           // la lumiere des ombres est la lune

static void UpdateSun()
{
    int idx = *(int *)0xA0CFF8;
    Vec3 s = *(Vec3 *)(0x792C70 + 12 * (idx & 15));   // CTimeCycle::m_VectorToSun[m_CurrentStoredValue]
    s = Norm(s);
    // Le jeu avance son soleil par minute de jeu (une seconde) : on le suit en douceur.
    static bool init;
    if (!init || Dot(s, g_sun) < 0.99f) { g_sun = s; init = true; }
    else g_sun = Norm({ g_sun.x + (s.x - g_sun.x) * 0.05f, g_sun.y + (s.y - g_sun.y) * 0.05f, g_sun.z + (s.z - g_sun.z) * 0.05f });
    float strength = *(short *)0xA10A34 / 255.0f;   // CTimeCycle::m_nCurrentShadowStrength (heure, meteo)
    if (strength > 1) strength = 1;
    float elev = (s.z - 0.04f) / 0.16f;             // apparaissent avec le soleil au-dessus de l'horizon
    if (elev < 0) elev = 0; if (elev > 1) elev = 1;
    g_sunK = g_cfg.sunShadows ? strength * elev : 0.0f;
    g_moon = false;
    // La nuit : la lune du jeu (CClouds::Render : fixe a (0, -100, 15) de la camera, visible de 0 h a 6 h, pleine a
    // 3 h, voilee par les nuages, la pluie et le brouillard). Ombres plus faibles et bleutees.
    if (g_sunK <= 0.01f && g_cfg.moonShadows) {
        float minute = ClockHours() * 60.0f + ClockMinutes();
        float fade = fabsf(minute - 180.0f);
        int w = NewWeather();
        float cover = (w == 0 || w == 4) ? 0.0f : w == 1 ? 0.5f : 1.0f;
        float bright = fade < 180 ? (1 - cover) * (180 - fade) / 180.0f : 0.0f;
        bright = bright * 1.6f; if (bright > 1) bright = 1;
        if (bright > 0.02f) {
            Vec3 m = Norm({ 0, -100, 15 });
            if (Dot(m, g_sun) < 0.99f) g_sun = m;
            else g_sun = Norm({ g_sun.x + (m.x - g_sun.x) * 0.05f, g_sun.y + (m.y - g_sun.y) * 0.05f, g_sun.z + (m.z - g_sun.z) * 0.05f });
            g_sunK = 0.55f * bright;
            g_moon = true;
        }
    }
}

static void ChooseMainView()
{
    int best = -1;
    for (int i = 0; i < g_viewCount; i++) if (best < 0 || g_views[i].count > g_views[best].count) best = i;
    g_mainIdx = best;
    g_haveMain = best >= 0;
    if (best < 0) return;
    memcpy(g_mainView, g_views[best].view, 64);
    memcpy(g_mainProj, g_views[best].proj, 64);
    for (Rec &r : g_recs) { r.mainView = r.viewIdx == best; r.receiver = r.recvCand && r.mainView; }
}

// ======================================================================= Projeteurs hors champ
// Le jeu ne dessine que ce que voit la camera : un immeuble juste derriere elle n'avait pas d'ombre, qui apparaissait
// d'un coup en tournant. Juste apres le dessin du monde (RenderScene 0x4A6570 appelle RenderEverythingBarRoads en
// 0x4A6584), on fait dessiner par le jeu les batiments proches hors du champ (pool 0x97F240, 7000 x 100 octets), en
// mode "projeteur seulement" : le pont les note sans les afficher.
static bool SphereInView(const M4 &vp, Vec3 c, float r)
{
    for (int k = 0; k < 6; k++) {
        float p[4];
        int col = k >> 1;   // 0 : x, 1 : y, 2 : z
        float sgn = (k & 1) ? -1.0f : 1.0f;
        for (int i = 0; i < 4; i++) {
            float w = vp.m[i * 4 + 3];
            if (k == 4) p[i] = vp.m[i * 4 + 2];                  // proche : z >= 0
            else if (k == 5) p[i] = w - vp.m[i * 4 + 2];         // lointain : z <= w
            else p[i] = w + sgn * vp.m[i * 4 + col];
        }
        float len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (len < 1e-6f) continue;
        if ((p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3]) / len < -r) return false;
    }
    return true;
}

static int g_extraCasters, g_extraFaults;

// Un batiment : dessine s'il est proche et hors du champ. Protege : une entite abimee ne doit pas faire planter.
static bool CasterOf(uint8_t *e, const M4 &vp, Vec3 cam, float reach)
{
    __try {
        if (!*(void **)(e + 0x4C)) return false;                          // pas de modele charge
        if (!(e[0x52] & 0x04) || (e[0x52] & 0x40) || (e[0x54] & 0x40)) return false;   // invisible, LOD lointain, ne pas dessiner
        int area = e[0x5F];
        if (area != 0 && area != 13) return false;
        int model = *(short *)(e + 0x5C);
        if (model < 0 || model >= 6500) return false;
        uint8_t *mi = (uint8_t *)ModelInfo(model);
        if (!mi) return false;
        const float *col = *(const float **)(mi + 0x1C);   // CBaseModelInfo::m_colModel (nom sur 21 octets)
        if ((uintptr_t)col < 0x10000) return false;
        const float *m = (const float *)(e + 4);   // CMatrix : right, up, at, pos (lignes de 4)
        Vec3 c = { m[12] + col[0] * m[0] + col[1] * m[4] + col[2] * m[8],
                   m[13] + col[0] * m[1] + col[1] * m[5] + col[2] * m[9],
                   m[14] + col[0] * m[2] + col[1] * m[6] + col[2] * m[10] };
        float r = col[3];
        if (!(r > 0 && r < 2000)) return false;
        Vec3 dc = Sub(c, cam);
        if (sqrtf(Dot(dc, dc)) - r > reach) return false;
        if (SphereInView(vp, c, r)) return false;   // dans le champ : deja dessine par le jeu
        ((void(__thiscall *)(void *))(*(void ***)e)[13])(e);   // CEntity::Render
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_extraFaults++;
        return false;
    }
}

static void ExtraCasters()
{
    g_extraCasters = 0;
    ChooseMainView();
    if (!g_dev || g_applied || !g_haveMain || !g_cfg.sunShadows || !ShadowsWanted()) return;
    UpdateSun();
    if (g_sunK <= 0.01f) return;
    M4 view, proj, invView;
    memcpy(view.m, g_mainView, 64); memcpy(proj.m, g_mainProj, 64);
    if (!Invert(view, invView)) return;
    M4 vp = Mul(view, proj);
    Vec3 cam = { invView.m[12], invView.m[13], invView.m[14] };
    const float reach = 100.0f;
    Pool *pool = *(Pool **)0x97F240;   // CPools::ms_pBuildingPool
    if (!pool) return;
    g_bridgeCasterOnly = true;
    for (int i = 0; i < pool->size; i++) {
        if (pool->flags[i] & 0x80) continue;
        if (CasterOf(pool->objects + i * 100, vp, cam, reach)) g_extraCasters++;
    }
    g_bridgeCasterOnly = false;
    static int loggedFaults;
    if (g_extraFaults != loggedFaults) { loggedFaults = g_extraFaults; Log("rendu : %d batiments illisibles ignores (projeteurs hors champ)", g_extraFaults); }
}

static bool g_sceneHooked;
static void Apply();
// Fin de RenderScene (appel en 0x4A604A dans Idle) : monde, eau et bateaux dessines ; les effets (particules, halos,
// interface) viennent apres et ne recoivent pas d'ombre.
static void __cdecl h_RenderScene()
{
    ((void(__cdecl *)())0x4A6570)();
    if (!g_applied && g_recs.size() >= 20) Apply();
}

static void __cdecl h_RenderEverythingBarRoads()
{
    ((void(__cdecl *)())0x4C9F40)();
    FpuGuard fpu;
    ExtraCasters();
}

static void ReleasePostResources();
static bool g_postResFailed;
void Gfx9SettingsChanged()
{
    if (!g_dev) return;
    ReleaseResources();
    ReleasePostResources();
    g_postResFailed = false;
    g_resourcesFailed = false;
}


// ======================================================================= Appels du pont
void Gfx9DeviceCreated(IDirect3DDevice9 *dev, UINT width, UINT height, bool msaa)
{
    g_dev = dev; g_width = width; g_height = height; g_msaa = msaa;
    Log("rendu : Direct3D 9 actif (%ux%u%s)", width, height, msaa ? ", anticrenelage" : "");
    FpuGuard fpu;
    g_debugMask = GetPrivateProfileIntA("VCCoop", "OmbresDebug", 0, IniPath());
    if (g_cfg.renderer == 12) RtStart();   // ray tracing : vcrt64.exe (Direct3D 12 + DXR, 64 bits)
    if (g_cfg.sunShadows || g_cfg.modernWater || g_cfg.dynLights) CreateShaders();   // au lancement (pas au milieu d'une image de jeu)
}
static void ReleasePostResources();
void Gfx9BeforeReset() { RtBeforeReset(); SafeRelease(g_captureBefore); ReleaseRecs(); SafeRelease(g_backBuffer); ReleaseResources(); g_resourcesFailed = false; ReleasePostResources(); g_postResFailed = false; }
void Gfx9AfterReset(UINT width, UINT height, bool msaa) { g_width = width; g_height = height; g_msaa = msaa; }

void Gfx9BeginScene()
{
    if (!g_backBuffer && g_dev) g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_backBuffer);
}
void Gfx9EndScene();

static bool DrawingToBackBuffer()
{
    IDirect3DSurface9 *rt = NULL;
    if (FAILED(g_dev->GetRenderTarget(0, &rt)) || !rt) return false;
    bool main = rt == g_backBuffer;
    rt->Release();
    return main;
}

static bool BuildRec(DWORD fvf, const GfxDraw &d, Rec &r);

void Gfx9AfterDraw(DWORD fvf, const GfxDraw &d)
{
    if (g_applied || !g_dev || (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ) return;
    if (!(d.type == D3DPT_TRIANGLELIST || d.type == D3DPT_TRIANGLESTRIP || d.type == D3DPT_TRIANGLEFAN)) { g_why[0]++; return; }
    if (!RecordingWanted() || g_recs.size() >= 6000) return;
    DWORD zen = 0, zw = 0, blend = 0, at = 0, aref = 0;
    g_dev->GetRenderState(D3DRS_ZENABLE, &zen);
    g_dev->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
    if (!zen && !g_bridgeCasterOnly) { g_why[1]++; return; }
    g_dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    // Sans ecriture de profondeur : feuillages, grillages, vitres et personnages en fondu (Tommy tout pres de la
    // camera) projettent quand meme leur ombre ; pas les effets (fumee, eau, ombres du jeu : tampons dynamiques
    // sans normales).
    if (!zw && !g_bridgeCasterOnly && !(blend && (fvf & D3DFVF_NORMAL))) {
        IDirect3DVertexBuffer9 *vb = NULL; UINT o = 0, st = 0;
        bool dynamic = SUCCEEDED(g_dev->GetStreamSource(0, &vb, &o, &st)) && vb && BridgeVertexMirror(vb);
        if (vb) vb->Release();
        if (!blend || dynamic) {
            g_why[2]++;
            static uint32_t lastDiag; static int n;
            if (g_cfg.logScripts && GetTickCount() - lastDiag > 10000) { if (++n > 6) { lastDiag = GetTickCount(); n = 0; } Log("rendu : refuse (sans ecriture z) fvf %X, melange %lu, dynamique %d, %u triangles", fvf, blend, dynamic, d.count); }
            return;
        }
    }
    if (!DrawingToBackBuffer()) { g_why[3]++; return; }
    g_dev->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
    g_dev->GetRenderState(D3DRS_ALPHAREF, &aref);
    Rec r = {};
    if (!BuildRec(fvf, d, r)) return;
    r.alphaTest = at || blend;
    r.alphaRef = blend ? 0.5f : (at ? (aref & 255) / 255.0f : 0.0f);
    if (r.alphaTest && r.alphaRef < 0.02f) r.alphaRef = 0.02f;
    r.recvCand = !blend && zw && !g_bridgeCasterOnly;
    r.vehicle = g_curEntity && (((uint8_t *)g_curEntity)[0x50] & 7) == 2;
    r.dynamic = g_curEntity && ((((uint8_t *)g_curEntity)[0x50] & 7) == 2 || (((uint8_t *)g_curEntity)[0x50] & 7) == 3);
    r.receiver = false;   // fixe par ChooseMainView
    r.caster = true;
    g_recs.push_back(r);
}

// Tampons, matrices et texture du dessin en cours ; les parties dynamiques (le jeu les reecrit dans l'image) sont
// recopiees tout de suite.
static bool BuildRec(DWORD fvf, const GfxDraw &d, Rec &r)
{
    UINT off = 0;
    if (FAILED(g_dev->GetStreamSource(0, &r.vb, &off, &r.stride)) || !r.vb) { g_why[4]++; return false; }
    if (d.indexed && (FAILED(g_dev->GetIndices(&r.ib)) || !r.ib)) { r.vb->Release(); r.vb = NULL; return false; }
    r.d = d; r.fvf = fvf;
    g_dev->GetTransform(D3DTS_WORLD, (D3DMATRIX *)r.world);
    float view[16], proj[16];
    g_dev->GetTransform(D3DTS_VIEW, (D3DMATRIX *)view);
    g_dev->GetTransform(D3DTS_PROJECTION, (D3DMATRIX *)proj);
    r.viewIdx = (uint8_t)ViewIndex(view, proj);
    if (!g_bridgeCasterOnly) { g_views[r.viewIdx].count++; g_haveMain = true; }
    r.mainView = false;
    if (fvf & D3DFVF_TEXCOUNT_MASK) g_dev->GetTexture(0, &r.tex);
    // Tampons dynamiques (le jeu les reecrit dans l'image) : on garde tout de suite la partie dessinee.
    const BYTE *vm = BridgeVertexMirror(r.vb);
    const BYTE *im = r.ib ? BridgeIndexMirror(r.ib) : NULL;
    if (im) {
        D3DINDEXBUFFER_DESC idesc;
        r.ib->GetDesc(&idesc);
        UINT n = VertexCount(d.type, d.count);
        if (idesc.Format != D3DFMT_INDEX16 || (d.start + n) * 2 > idesc.Size) { g_why[5]++; SafeRelease(r.vb); SafeRelease(r.ib); SafeRelease(r.tex); return false; }
        UINT q = (UINT)g_cpuIb.size();
        g_cpuIb.insert(g_cpuIb.end(), (const WORD *)im + d.start, (const WORD *)im + d.start + n);
        r.d.start = q;
        r.replayIb = true;
    }
    if (vm) {
        D3DVERTEXBUFFER_DESC vdesc;
        r.vb->GetDesc(&vdesc);
        UINT first = d.indexed ? d.baseVertex + d.minIndex : d.start;
        UINT n = d.indexed ? d.numVerts : VertexCount(d.type, d.count);
        if ((first + n) * r.stride > vdesc.Size || !r.stride) { g_why[6]++; SafeRelease(r.vb); SafeRelease(r.ib); SafeRelease(r.tex); return false; }
        UINT pos = ((UINT)g_cpuVb.size() + r.stride - 1) / r.stride * r.stride;
        g_cpuVb.resize(pos + n * r.stride);
        memcpy(g_cpuVb.data() + pos, vm + first * r.stride, n * r.stride);
        if (d.indexed) r.d.baseVertex = pos / r.stride - d.minIndex;
        else r.d.start = pos / r.stride;
        r.replayVb = true;
    }
    return true;
}

// Recopie des parties dynamiques dans nos tampons.
static bool UploadReplay()
{
    if (!g_cpuVb.empty()) {
        UINT need = (UINT)g_cpuVb.size();
        if (need > g_replayVbSize) {
            SafeRelease(g_replayVb);
            g_replayVbSize = need + need / 2 + 65536;
            if (FAILED(g_dev->CreateVertexBuffer(g_replayVbSize, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_replayVb, NULL))) { g_replayVbSize = 0; return false; }
        }
        void *p;
        if (FAILED(g_replayVb->Lock(0, need, &p, D3DLOCK_DISCARD))) return false;
        memcpy(p, g_cpuVb.data(), need);
        g_replayVb->Unlock();
    }
    if (!g_cpuIb.empty()) {
        UINT need = (UINT)g_cpuIb.size() * 2;
        if (need > g_replayIbSize) {
            SafeRelease(g_replayIb);
            g_replayIbSize = need + need / 2 + 65536;
            if (FAILED(g_dev->CreateIndexBuffer(g_replayIbSize, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &g_replayIb, NULL))) { g_replayIbSize = 0; return false; }
        }
        void *p;
        if (FAILED(g_replayIb->Lock(0, need, &p, D3DLOCK_DISCARD))) return false;
        memcpy(p, g_cpuIb.data(), need);
        g_replayIb->Unlock();
    }
    return true;
}

static void DrawRec(const Rec &r, const M4 &mat, bool viewPass)
{
    int v = (r.fvf & D3DFVF_TEXCOUNT_MASK) ? 0 : 1;
    g_dev->SetVertexShader(viewPass ? g_vsView[v] : g_vsLight[v]);
    g_dev->SetPixelShader(g_psDepth[v]);
    g_dev->SetFVF(r.fvf);
    g_dev->SetVertexShaderConstantF(0, mat.m, 4);
    float alpha[4] = { r.alphaRef, r.alphaTest && r.tex ? 1.0f : 0.0f, 0, 0 };
    g_dev->SetPixelShaderConstantF(0, alpha, 1);
    g_dev->SetTexture(0, r.tex);
    g_dev->SetStreamSource(0, r.replayVb ? g_replayVb : r.vb, 0, r.stride);
    if (r.d.indexed) {
        g_dev->SetIndices(r.replayIb ? g_replayIb : r.ib);
        g_dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)r.d.type, (INT)r.d.baseVertex, r.d.minIndex, r.d.numVerts, r.d.start, r.d.count);
    } else g_dev->DrawPrimitive((D3DPRIMITIVETYPE)r.d.type, r.d.start, r.d.count);
}

// Cascade : sphere fixe autour d'une tranche du champ de vision, vue du soleil, calee sur la grille des texels.
struct CascadeInfo { M4 light; float texelWorld, bias; };
static CascadeInfo MakeCascade(Vec3 cam, Vec3 fwd, float tanX, float tanY, float a, float b, int size)
{
    // Centre et rayon de la sphere qui contient la tranche [a, b].
    float k = tanX * tanX + tanY * tanY;
    float c = (a + b) * 0.5f * (1 + k);
    if (c > b) c = b;
    float ra = sqrtf((c - a) * (c - a) + a * a * k), rb = sqrtf((b - c) * (b - c) + b * b * k);
    float r = ra > rb ? ra : rb;
    r = ceilf(r);   // rayon stable
    Vec3 center = { cam.x + fwd.x * c, cam.y + fwd.y * c, cam.z + fwd.z * c };
    Vec3 f = { -g_sun.x, -g_sun.y, -g_sun.z };   // du soleil vers la scene
    Vec3 up0 = fabsf(f.z) > 0.99f ? Vec3{ 0, 1, 0 } : Vec3{ 0, 0, 1 };
    Vec3 right = Norm(Cross(up0, f));
    Vec3 up = Cross(f, right);
    float texel = 2 * r / size;
    float cx = floorf(Dot(center, right) / texel) * texel;
    float cy = floorf(Dot(center, up) / texel) * texel;
    const float ext = 400.0f;   // hauteur au-dessus de la scene ou les objets projettent encore
    float zmin = Dot(center, f) - r - ext, zr = 2 * r + ext;
    CascadeInfo ci;
    M4 &m = ci.light;
    m.m[0] = right.x / r; m.m[1] = up.x / r; m.m[2] = f.x / zr; m.m[3] = 0;
    m.m[4] = right.y / r; m.m[5] = up.y / r; m.m[6] = f.y / zr; m.m[7] = 0;
    m.m[8] = right.z / r; m.m[9] = up.z / r; m.m[10] = f.z / zr; m.m[11] = 0;
    m.m[12] = -cx / r; m.m[13] = -cy / r; m.m[14] = -zmin / zr; m.m[15] = 1;
    ci.texelWorld = texel;
    ci.bias = (texel * 1.0f + 0.03f) / zr;
    return ci;
}

// ======================================================================= Lumieres du jeu (CPointLights)
// CPointLights::AddLight (0x567700) : phares, lampadaires, explosions, feux, tirs. Le jeu n'en garde que 32 a moins de
// 22 m (pour eclairer les personnages et vehicules) ; on les note aussi jusqu'a 150 m pour l'eclairage par pixel.
struct DynLight { float x, y, z, dx, dy, dz, radius, r, g, b; int type; float cone; };
enum { MAX_LIGHTS = 48 };
static DynLight g_lightList[MAX_LIGHTS];
static int g_lightCount;
typedef void(__cdecl *AddLight_t)(int, float, float, float, float, float, float, float, float, float, float, int, int);
static AddLight_t o_AddLight;
static void __cdecl h_AddLight(int type, float x, float y, float z, float dx, float dy, float dz, float radius, float r, float g, float b, int fog, int extra)
{
    int t = type & 0xFF;
    // Le phare du joueur (genre 1) part du centre de sa voiture : sa carrosserie masquait tout. Les phares sont
    // refaits a l'avant de chaque vehicule (h_StoreCarLight).
    if (t == 0 && g_lightCount < MAX_LIGHTS && radius > 0.1f && r + g + b > 0.02f) {
        const float *cam = (const float *)0x7E46B8;   // TheCamera : position
        float ex = x - cam[0], ey = y - cam[1], ez = z - cam[2];
        if (ex * ex + ey * ey + ez * ez < 150.0f * 150.0f) g_lightList[g_lightCount++] = { x, y, z, dx, dy, dz, radius, r, g, b, t, 0.55f };
    }
    o_AddLight(type, x, y, z, dx, dy, dz, radius, r, g, b, fog, extra);
}

// Phares : le jeu n'en fait une vraie lumiere que pour la voiture du joueur ; les autres ne sont qu'une tache
// lumineuse peinte au sol (CShadows::StoreCarLightShadow 0x56DCD0, texture des phares 0xA1073C). Chaque vehicule
// phares allumes recoit ici un projecteur a l'avant (qui eclaire et projette des ombres), et la tache peinte est retiree.
typedef void(__cdecl *StoreCarLight_t)(void *, int, void *, float *, float, float, float, float, int, int, int, float);
static StoreCarLight_t o_StoreCarLight;
static bool g_lightsLive;   // lumieres dynamiques actives a l'image precedente (sinon la tache du jeu reste)
static float g_night = 1;   // 1 la nuit, 0 en plein jour (ciel du cycle du jour) : les phares portent plus la nuit
static float SkyLum()
{
    int c[3] = { *(int *)0xA0D958, *(int *)0x97F208, *(int *)0x9B6DF4 };   // CTimeCycle : bas du ciel
    float l = (c[0] * 0.3f + c[1] * 0.59f + c[2] * 0.11f) / 255.0f * 1.25f;
    return l < 0 ? 0 : l > 1 ? 1 : l;
}
static void __cdecl h_StoreCarLight(void *car, int id, void *tex, float *pos, float fx, float fy, float sx, float sy, int r, int g, int b, float maxAngle)
{
    // Phare avant : identifiant vehicule + 22 (voitures : texture des phares ; motos, CBike::PreRender : texture
    // d'explosion, elles etaient donc oubliees). + 25 = la tache rouge des feux arriere, laissee au jeu.
    bool head = tex && (tex == *(void **)0xA1073C || id == (int)((uintptr_t)car + 22));
    if (car && pos && head && g_lightsLive) {
        const float *m = (const float *)((uint8_t *)car + 4);   // right, forward, up, position (lignes de 4)
        Vec3 p = { m[12], m[13], m[14] }, fwd = { m[4], m[5], m[6] }, up = { m[8], m[9], m[10] };
        // La tache est posee 6 m devant les phares : on retrouve l'avant du vehicule.
        float front = (pos[0] - p.x) * fwd.x + (pos[1] - p.y) * fwd.y + (pos[2] - p.z) * fwd.z - 6.0f;
        if (front < 0.8f) front = 0.8f;
        if (front > 4.0f) front = 4.0f;
        const float *cam = (const float *)0x7E46B8;
        float ex = p.x - cam[0], ey = p.y - cam[1];
        if (g_lightCount < MAX_LIGHTS && ex * ex + ey * ey < 150.0f * 150.0f) {
            Vec3 o = { p.x + fwd.x * (front + 0.6f) + up.x * 0.15f, p.y + fwd.y * (front + 0.6f) + up.y * 0.15f, p.z + fwd.z * (front + 0.6f) + up.z * 0.15f };   // devant le pare-chocs
            Vec3 d = Norm({ fwd.x - up.x * 0.12f, fwd.y - up.y * 0.12f, fwd.z - up.z * 0.12f });
            // L'image est multipliee par (1 + lumiere) : sur une route sombre la nuit, il faut beaucoup plus de lumiere
            // pour retrouver la tache du jeu (et plus).
            float k = 2.5f * (1.0f + 0.6f * g_night);   // (la passe applique x0,4 a toutes les lumieres)
            g_lightList[g_lightCount++] = { o.x, o.y, o.z, d.x, d.y, d.z, 14.0f, 1.15f * k, 1.1f * k, 0.95f * k, 1, 0.80f };
            static bool bikeLogged;
            if (!bikeLogged && tex != *(void **)0xA1073C) { bikeLogged = true; Log("rendu : phare de moto (%s) eclaire la route", ModelName(ModelIndex(car))); }
        }
        return;
    }
    o_StoreCarLight(car, id, tex, pos, fx, fy, sx, sy, r, g, b, maxAngle);
}

// ======================================================================= Phare d'Ocean Beach
// Son faisceau d'origine est un objet (od_lightbeam, 474 -1718 60) que CMovingThings fait tourner de 20 h a 5 h (un tour
// en 0x3FFF ms du chronometre du jeu, direction (cos a, sin a, 0), reVC Fluff.cpp). Avec LampadairesEclairent, l'objet
// n'est plus dessine (h_RenderOneNonRoad) : a sa place un vrai projecteur tournant (eclaire le sol, les facades, la
// mer ; ombre s'il est parmi les plus importants) et un long faisceau doux dans l'air (DrawBeams). L'eblouissement du
// jeu quand il passe face a la camera reste.
static Vec3 g_lhPos = { 474.3353f, -1717.672f, 60.0871f };
static Vec3 g_lhDir;
static bool g_lhActive;
static int g_lhModel = -2;
static bool IsLightBeam(int model)
{
    if (g_lhModel == -2) {
        g_lhModel = -1;
        for (int i = 0; i < 6500; i++) if (ModelInfo(i) && !_stricmp(ModelName(i), "od_lightbeam")) { g_lhModel = i; break; }
        Log("rendu : faisceau du phare : modele %d", g_lhModel);
    }
    return model >= 0 && model == g_lhModel;
}
static void AddLighthouse()
{
    g_lhActive = false;
    int h = ClockHours();
    if (!g_cfg.lampLights || !g_lightsLive || (h < 20 && h >= 5) || g_lightCount >= MAX_LIGHTS || !Outdoors()) return;
    const float *cam = (const float *)0x7E46B8;
    float ex = g_lhPos.x - cam[0], ey = g_lhPos.y - cam[1];
    if (ex * ex + ey * ey > 1200.0f * 1200.0f) return;
    uint32_t t = *(uint32_t *)0x974B2C;   // CTimer::m_snTimeInMilliseconds
    float a = (t % 0x3FFF) * 6.2831853f / 0x3FFF;
    g_lhDir = Norm({ cosf(a), sinf(a), -0.12f });
    g_lightList[g_lightCount++] = { g_lhPos.x + g_lhDir.x * 1.2f, g_lhPos.y + g_lhDir.y * 1.2f, g_lhPos.z, g_lhDir.x, g_lhDir.y, g_lhDir.z,
                                    95.0f, 3.2f, 3.1f, 2.7f, 1, 0.985f };
    g_lhActive = true;
}

// ======================================================================= Lampadaires, neons, enseignes
// Les lumieres des batiments et objets (2dEffect "light", CEntity::ProcessLightsForEntity 0x541590) ne sont que des
// halos (CCoronas::RegisterCorona, variante a texture 0x542490) et, sous les lampadaires, une tache lumineuse peinte
// au sol (CShadows::StoreStaticShadow 0x56E780, type 2 additif). Chacune devient ici une vraie lumiere (couleur de
// l'effet, portee selon la taille du halo ou de la tache) qui eclaire rue, voitures et personnages, avec ombre pour
// les plus proches (meme systeme que les phares) ; la tache peinte est retiree. Les candidates (jusqu'a 256) sont
// triees par importance et completent la liste des lumieres de l'image.
#include <intrin.h>
enum { MAX_LAMPS = 256 };
static DynLight g_lamps[MAX_LAMPS];
static int g_lampCount;
static bool FromEntityLights(void *ret) { uintptr_t a = (uintptr_t)ret; return a >= 0x541590 && a < 0x541F00; }

static void AddLamp(float x, float y, float z, float radius, float r, float g, float b)
{
    if (r + g + b < 0.03f || radius <= 0.5f) return;
    const float *cam = (const float *)0x7E46B8;
    float ex = x - cam[0], ey = y - cam[1], ez = z - cam[2];
    if (ex * ex + ey * ey + ez * ez > 160.0f * 160.0f) return;
    for (int i = 0; i < g_lampCount; i++) {   // halo et tache de la meme lampe : une seule lumiere
        DynLight &l = g_lamps[i];
        float dx = l.x - x, dy = l.y - y, dz = l.z - z;
        if (dx * dx + dy * dy + dz * dz < 1.5f * 1.5f) {
            if (radius > l.radius) l.radius = radius;
            if (r + g + b > l.r + l.g + l.b) { l.r = r; l.g = g; l.b = b; }
            return;
        }
    }
    if (g_lampCount < MAX_LAMPS) g_lamps[g_lampCount++] = { x, y, z, 0, 0, -1, radius, r, g, b, 0, 0.0f };
}

typedef void(__cdecl *StoreStaticShadow_t)(uint32_t, int, void *, const float *, float, float, float, float, int, int, int, int, float, float, float, int, float);
static StoreStaticShadow_t o_StoreStaticShadow;
static void __cdecl h_StoreStaticShadow(uint32_t id, int type, void *tex, const float *pos, float fx, float fy, float sx, float sy,
                                        int intensity, int r, int g, int b, float zDist, float scale, float drawDist, int temporary, float upDist)
{
    if (g_cfg.lampLights && g_lightsLive && (type & 0xFF) == 2 && pos && FromEntityLights(_ReturnAddress())) {
        float size = fabsf(fx) > fabsf(sy) ? fabsf(fx) : fabsf(sy);
        float k = 2.2f / 255.0f;
        AddLamp(pos[0], pos[1], pos[2], size * 2.4f < 5.0f ? 5.0f : size * 2.4f > 18.0f ? 18.0f : size * 2.4f,
                (r & 0xFF) * k, (g & 0xFF) * k, (b & 0xFF) * k);
        return;   // plus de tache peinte : la lumiere eclaire vraiment
    }
    o_StoreStaticShadow(id, type, tex, pos, fx, fy, sx, sy, intensity, r, g, b, zDist, scale, drawDist, temporary, upDist);
}

typedef void(__cdecl *RegisterCoronaTex_t)(uint32_t, int, int, int, int, const float *, float, float, void *, int, int, int, int, float, int, float);
static RegisterCoronaTex_t o_RegisterCoronaTex;
static void __cdecl h_RegisterCoronaTex(uint32_t id, int r, int g, int b, int a, const float *pos, float size, float drawDist, void *tex,
                                        int flare, int refl, int los, int streak, float angle, int longDist, float nearDist)
{
    if (g_cfg.lampLights && g_lightsLive && pos && (a & 0xFF) > 20 && FromEntityLights(_ReturnAddress())) {
        float k = 1.1f * (a & 0xFF) / (255.0f * 255.0f);
        float rad = size * 4.5f;
        AddLamp(pos[0], pos[1], pos[2], rad < 2.5f ? 2.5f : rad > 12.0f ? 12.0f : rad, (r & 0xFF) * k, (g & 0xFF) * k, (b & 0xFF) * k);
    }
    o_RegisterCoronaTex(id, r, g, b, a, pos, size, drawDist, tex, flare, refl, los, streak, angle, longDist, nearDist);
}

// Les lampes les plus importantes (vues d'ici) completent la liste des lumieres ; pas de doublon avec une lumiere
// deja donnee par le jeu (CPointLights::AddLight d'un lampadaire).
static void MergeLamps()
{
    if (!g_lampCount) return;
    const float *cam = (const float *)0x7E46B8;
    static float score[MAX_LAMPS];
    for (int i = 0; i < g_lampCount; i++) {
        const DynLight &l = g_lamps[i];
        float dx = l.x - cam[0], dy = l.y - cam[1], dz = l.z - cam[2];
        score[i] = (l.r + l.g + l.b) * l.radius / (6.0f + sqrtf(dx * dx + dy * dy + dz * dz));
    }
    int added = 0;
    while (g_lightCount < MAX_LIGHTS) {
        int best = -1;
        for (int i = 0; i < g_lampCount; i++) if (score[i] > 0 && (best < 0 || score[i] > score[best])) best = i;
        if (best < 0) break;
        score[best] = 0;
        const DynLight &l = g_lamps[best];
        bool dup = false;
        for (int j = 0; j < g_lightCount && !dup; j++) {
            float dx = g_lightList[j].x - l.x, dy = g_lightList[j].y - l.y, dz = g_lightList[j].z - l.z;
            dup = g_lightList[j].type == 0 && dx * dx + dy * dy + dz * dz < 2.0f * 2.0f;
        }
        if (dup) continue;
        g_lightList[g_lightCount++] = l;
        added++;
    }
    static uint32_t lastLog;
    if (GetTickCount() - lastLog > 10000) { lastLog = GetTickCount(); Log("rendu : %d lampes et enseignes vues, %d eclairent", g_lampCount, added); }
}

static void SetCommonStates()
{
    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    g_dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
    g_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    g_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    for (int s = 0; s < 2; s++) {
        g_dev->SetSamplerState(s, D3DSAMP_MINFILTER, s == 0 ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        g_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, s == 0 ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        g_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, s == 0 ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSU, s == 0 ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSV, s == 0 ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }
}

// Profondeur de la scene vue de la camera (R32F, metres / 1000) : ce que le jeu a dessine d'opaque jusqu'ici.
static const float kDepthScale = 1.0f / 1000.0f;
static void RenderScreenDepth(const M4 &vp, bool withWater)
{
    g_dev->SetRenderTarget(0, g_screenSurf);
    g_dev->SetDepthStencilSurface(g_screenDs);
    g_dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0);
    float vsParams[4] = { kDepthScale, 0, 0, 0 };
    g_dev->SetVertexShaderConstantF(4, vsParams, 1);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    for (const Rec &r : g_recs) {
        if (!r.receiver || (r.water && !withWater)) continue;
        M4 w; memcpy(w.m, r.world, 64);
        DrawRec(r, Mul(w, vp), true);
    }
}

// Les lumieres qui meritent une ombre (fortes, grandes, proches de la camera) passent en tete de liste ; chacune a sa
// carte (perspective depuis la lumiere : les lampadaires regardent vers le bas en grand angle, les phares vers l'avant).
static int g_shadowLights;
static M4 g_lightVP[4];
static float LightRange(const DynLight &l) { return l.type == 1 ? l.radius * 2.2f : l.radius * 1.6f; }
static void PrepareLightShadows(Vec3 cam)
{
    if (!g_cfg.lightShadows || !g_lightAtlasSurf || !g_vsSpot[0] || !g_vsSpot[1]) return;
    int want = g_cfg.lightShadows < g_lightCount ? g_cfg.lightShadows : g_lightCount;
    for (int k = 0; k < want; k++) {
        int best = -1; float bestScore = 0;
        for (int i = k; i < g_lightCount; i++) {
            const DynLight &l = g_lightList[i];
            Vec3 d = Sub({ l.x, l.y, l.z }, cam);
            float dist = sqrtf(Dot(d, d));
            if (dist > 60.0f + LightRange(l)) continue;
            float score = (l.r + l.g + l.b) * LightRange(l) / (4.0f + dist);
            if (score > bestScore) { bestScore = score; best = i; }
        }
        if (best < 0) break;
        DynLight t = g_lightList[k]; g_lightList[k] = g_lightList[best]; g_lightList[best] = t;
        g_shadowLights = k + 1;
    }
    if (!g_shadowLights) return;
    g_dev->SetRenderTarget(0, g_lightAtlasSurf);
    g_dev->SetDepthStencilSurface(g_lightAtlasDs);
    g_dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0);
    for (int k = 0; k < g_shadowLights; k++) {
        const DynLight &l = g_lightList[k];
        bool spot = l.type == 1;
        float range = LightRange(l);
        Vec3 pos = { l.x, l.y, l.z };
        Vec3 dir = spot ? Norm({ l.dx, l.dy, l.dz }) : Vec3{ 0, 0, -1 };
        if (spot && Dot(dir, dir) < 0.5f) dir = { 0, 0, -1 };
        g_lightVP[k] = Mul(LookDir(pos, dir), Perspective(spot ? 110.0f : 150.0f, 0.05f, range));
        D3DVIEWPORT9 vpt = { (DWORD)((k & 1) * LIGHT_TILE), (DWORD)((k >> 1) * LIGHT_TILE), LIGHT_TILE, LIGHT_TILE, 0, 1 };
        g_dev->SetViewport(&vpt);
        float lpos[4] = { l.x, l.y, l.z, 1.0f / range };
        g_dev->SetVertexShaderConstantF(12, lpos, 1);
        for (const Rec &r : g_recs) {
            if (!r.caster) continue;
            // Objets loin de la lumiere : ignores (position de l'objet ; les dessins sans matrice sont des effets).
            float ox = r.world[12] - l.x, oy = r.world[13] - l.y, oz = r.world[14] - l.z;
            if (ox * ox + oy * oy + oz * oz > (range + 45.0f) * (range + 45.0f)) continue;
            int v = (r.fvf & D3DFVF_TEXCOUNT_MASK) ? 0 : 1;
            M4 w; memcpy(w.m, r.world, 64);
            M4 wvp = Mul(w, g_lightVP[k]);
            g_dev->SetVertexShader(g_vsSpot[v]);
            g_dev->SetPixelShader(g_psDepth[v]);
            g_dev->SetFVF(r.fvf);
            g_dev->SetVertexShaderConstantF(0, wvp.m, 4);
            g_dev->SetVertexShaderConstantF(8, w.m, 4);
            float alpha[4] = { r.alphaRef, r.alphaTest && r.tex ? 1.0f : 0.0f, 0, 0 };
            g_dev->SetPixelShaderConstantF(0, alpha, 1);
            g_dev->SetTexture(0, r.tex);
            g_dev->SetStreamSource(0, r.replayVb ? g_replayVb : r.vb, 0, r.stride);
            if (r.d.indexed) {
                g_dev->SetIndices(r.replayIb ? g_replayIb : r.ib);
                g_dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)r.d.type, (INT)r.d.baseVertex, r.d.minIndex, r.d.numVerts, r.d.start, r.d.count);
            } else g_dev->DrawPrimitive((D3DPRIMITIVETYPE)r.d.type, r.d.start, r.d.count);
        }
    }
}


// ======================================================================= Ambiance (dans Apply, apres les lumieres)
static M4 g_lastVP;          // camera de la derniere image (rayons de soleil, dans le post-traitement)
static bool g_carMaskReady;
static Vec3 g_lastCam;
static uint32_t g_lastVPAt;
// Projection a l'ecran avec la vraie camera de la derniere image (pseudos des joueurs) : celle du jeu
// (CSprite::CalcScreenCoors) garde la camera habituelle quand un script impose une camera fixe.
bool Gfx9Project(float x, float y, float z, float *sx, float *sy, float *dist)
{
    if (!g_lastVPAt || GetTickCount() - g_lastVPAt > 250) return false;
    float cl[4];
    for (int j = 0; j < 4; j++) cl[j] = x * g_lastVP.m[j] + y * g_lastVP.m[4 + j] + z * g_lastVP.m[8 + j] + g_lastVP.m[12 + j];
    if (cl[3] < 0.3f) return false;
    float u = cl[0] / cl[3], v = cl[1] / cl[3];
    if (u < -1.1f || u > 1.1f || v < -1.1f || v > 1.1f) return false;
    int w = *(int *)0x9B48DC, h = *(int *)0x9B48E0;
    *sx = (u * 0.5f + 0.5f) * w;
    *sy = (0.5f - v * 0.5f) * h;
    float dx = x - g_lastCam.x, dy = y - g_lastCam.y, dz = z - g_lastCam.z;
    *dist = sqrtf(dx * dx + dy * dy + dz * dz);
    return true;
}
static float WeatherF(uintptr_t a) { float v = *(float *)a; return v == v && v > 0 ? (v > 1.5f ? 1.5f : v) : 0.0f; }
static float SunsetK()
{
    if (g_moon || g_sun.z <= -0.05f || g_sun.z >= 0.35f) return 0;
    float k = 1.0f - fabsf(g_sun.z - 0.12f) / 0.23f;
    return k < 0 ? 0 : k;
}
static const float kFullTri[12] = { -1, -1, 0.5f, 1, -1, 3, 0.5f, 1, 3, -1, 0.5f, 1 };

// Cones des phares (lumieres de genre 1), additifs, testes contre la profondeur du jeu, adoucis contre le decor.
static void DrawBeams(const M4 &vp, Vec3 cam, IDirect3DSurface9 *oldDs)
{
    float rain = WeatherF(0x975340), fog = WeatherF(0x94DDC0);
    float dens = (0.12f + 0.9f * rain + 1.0f * fog) * (0.4f + 0.6f * g_night);
    struct BV { float x, y, z; DWORD c; };
    static BV v[(MAX_LIGHTS + 1) * 9 * 24];
    int nv = 0;
    const int cap = (int)(sizeof(v) / sizeof(v[0]));
    // Cone : sommet o, direction dir, longueur len, rayon au bout rad ; couleur (0..1), alpha au sommet et au tiers.
    auto addCone = [&](Vec3 o, Vec3 dir, float len, float rad, float r, float g, float b, int aTip, int aMid, int seg) {
        Vec3 up = fabsf(dir.z) > 0.9f ? Vec3{ 1, 0, 0 } : Vec3{ 0, 0, 1 };
        Vec3 ax = Norm(Cross(dir, up)), bx = Cross(dir, ax);
        int cr = (int)(255 * r), cg = (int)(255 * g), cb = (int)(255 * b);
        cr = cr > 255 ? 255 : cr; cg = cg > 255 ? 255 : cg; cb = cb > 255 ? 255 : cb;
        DWORD tip = D3DCOLOR_ARGB(aTip, cr, cg, cb), mid = D3DCOLOR_ARGB(aMid, cr, cg, cb), end = D3DCOLOR_ARGB(0, cr, cg, cb);
        for (int k = 0; k < seg && nv + 9 <= cap; k++) {
            float a0 = k * 6.2831853f / seg, a1 = (k + 1) * 6.2831853f / seg;
            Vec3 r0 = { ax.x * cosf(a0) + bx.x * sinf(a0), ax.y * cosf(a0) + bx.y * sinf(a0), ax.z * cosf(a0) + bx.z * sinf(a0) };
            Vec3 r1 = { ax.x * cosf(a1) + bx.x * sinf(a1), ax.y * cosf(a1) + bx.y * sinf(a1), ax.z * cosf(a1) + bx.z * sinf(a1) };
            auto P = [&](float t, Vec3 rr) { return Vec3{ o.x + dir.x * len * t + rr.x * rad * t, o.y + dir.y * len * t + rr.y * rad * t, o.z + dir.z * len * t + rr.z * rad * t }; };
            Vec3 m0 = P(0.35f, r0), m1 = P(0.35f, r1), e0 = P(1, r0), e1 = P(1, r1);
            v[nv++] = { o.x, o.y, o.z, tip }; v[nv++] = { m0.x, m0.y, m0.z, mid }; v[nv++] = { m1.x, m1.y, m1.z, mid };
            v[nv++] = { m0.x, m0.y, m0.z, mid }; v[nv++] = { e0.x, e0.y, e0.z, end }; v[nv++] = { e1.x, e1.y, e1.z, end };
            v[nv++] = { m0.x, m0.y, m0.z, mid }; v[nv++] = { e1.x, e1.y, e1.z, end }; v[nv++] = { m1.x, m1.y, m1.z, mid };
        }
    };
    if (dens >= 0.02f) {
        for (int i = 0; i < g_lightCount; i++) {
            const DynLight &l = g_lightList[i];
            if (l.type != 1 || l.cone > 0.95f) continue;   // (le phare a son propre faisceau)
            Vec3 o = { l.x, l.y, l.z }, dir = Norm({ l.dx, l.dy, l.dz });
            Vec3 dc = Sub(o, cam);
            if (Dot(dc, dc) > 80.0f * 80.0f || Dot(dir, dir) < 0.5f) continue;
            float lum = l.r + l.g + l.b; if (lum < 0.01f) continue;
            addCone(o, dir, 13.0f, 13.0f * 0.42f, l.r / lum * 1.2f, l.g / lum * 1.2f, l.b / lum * 1.2f, 150, 70, 18);
        }
    }
    // Faisceau du phare : visible toute la nuit (plus dense sous la pluie et dans le brouillard).
    float lhDens = 0;
    if (g_lhActive) {
        // Trois cones emboites : coeur etroit et lumineux, bords de plus en plus diffus (pas un coin a bord net).
        addCone(g_lhPos, g_lhDir, 160.0f, 160.0f * 0.035f, 1.0f, 0.97f, 0.85f, 120, 70, 16);
        addCone(g_lhPos, g_lhDir, 150.0f, 150.0f * 0.07f, 1.0f, 0.97f, 0.85f, 70, 35, 16);
        addCone(g_lhPos, g_lhDir, 140.0f, 140.0f * 0.11f, 1.0f, 0.97f, 0.85f, 35, 15, 16);
        lhDens = 0.35f + 0.8f * rain + 1.0f * fog;
    }
    int nLh = g_lhActive ? 3 * 16 * 9 : 0;   // (ajoute en dernier)
    if (!nv) return;
    g_dev->SetDepthStencilSurface(oldDs);
    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_dev->SetVertexShaderConstantF(0, vp.m, 4);
    g_dev->SetVertexShader(g_vsBeam);
    g_dev->SetPixelShader(g_psBeam);
    g_dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    int nCars = nv - nLh;
    if (nCars > 0) {
        float bc[4] = { dens, 1.0f / kDepthScale, 0, 0 };
        g_dev->SetPixelShaderConstantF(30, bc, 1);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, nCars / 3, v, sizeof(BV));
    }
    if (nLh > 0) {
        float bc[4] = { lhDens, 1.0f / kDepthScale, 0, 0 };
        g_dev->SetPixelShaderConstantF(30, bc, 1);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, nLh / 3, v + nCars, sizeof(BV));
    }
    g_dev->SetDepthStencilSurface(NULL);
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetVertexShader(g_vsQuad);
    g_dev->SetFVF(D3DFVF_XYZW);
}

static void AmbiencePasses(const M4 &vp, Vec3 cam, IDirect3DSurface9 *oldRt, IDirect3DSurface9 *oldDs, bool lights)
{
    if (g_debugMask) return;
    bool outdoors = Outdoors();
    float wetRoads = WeatherF(0x9B6A9C);   // CWeather::WetRoads
    float sky[3] = { SkyChan(0xA0D958), SkyChan(0x97F208), SkyChan(0x9B6DF4) };
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    float vpc[6 * 4] = {};
    memcpy(vpc + 4, vp.m, 64);
    vpc[20] = sky[0]; vpc[21] = sky[1]; vpc[22] = sky[2]; vpc[23] = 1;
    // Lumiere indirecte (de jour, et un peu la nuit sous les neons).
    if (g_cfg.indirectLight && g_psGI && g_psGIApply && g_refractSurf && g_aoSurf) {
        g_dev->StretchRect(oldRt, NULL, g_refractSurf, NULL, D3DTEXF_NONE);
        g_dev->SetTexture(3, g_refract);
        g_dev->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        vpc[0] = 0.22f + 0.13f * (1 - g_night); vpc[1] = 2.5f; vpc[2] = 0; vpc[3] = 0;
        g_dev->SetPixelShaderConstantF(30, vpc, 6);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        g_dev->SetRenderTarget(0, g_aoSurf);            // lumiere renvoyee (la cible de l'occlusion est libre)
        g_dev->SetPixelShader(g_psGI);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, kFullTri, 16);
        g_dev->SetRenderTarget(0, oldRt);
        D3DVIEWPORT9 fullVp = { 0, 0, g_width, g_height, 0, 1 };
        g_dev->SetViewport(&fullVp);
        g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
        g_dev->SetTexture(1, g_ao);
        g_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        g_dev->SetPixelShader(g_psGIApply);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, kFullTri, 16);
        g_dev->SetTexture(1, NULL);
    }
    // Carrosseries.
    if (g_cfg.carReflections && g_carMaskReady && g_psCarRefl && g_refractSurf) {
        g_dev->StretchRect(oldRt, NULL, g_refractSurf, NULL, D3DTEXF_NONE);
        g_dev->SetTexture(3, g_refract);
        g_dev->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        g_dev->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        g_dev->SetTexture(1, g_carMask);
        g_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        vpc[0] = 1.0f; vpc[1] = 0; vpc[2] = 0; vpc[3] = 0;
        g_dev->SetPixelShaderConstantF(30, vpc, 6);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_dev->SetPixelShader(g_psCarRefl);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, kFullTri, 16);
        g_dev->SetTexture(1, NULL);
    }
    // Sols mouilles (pluie) ; sols brillants des interieurs (Malibu, centre commercial...).
    if (g_cfg.wetRoads && g_psWet && g_refractSurf && (wetRoads > 0.03f || !outdoors)) {
        g_dev->StretchRect(oldRt, NULL, g_refractSurf, NULL, D3DTEXF_NONE);
        g_dev->SetTexture(3, g_refract);
        g_dev->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        g_dev->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        float wc[6 * 4] = {};
        wc[0] = outdoors ? wetRoads : 0; wc[1] = (float)(GetTickCount() % 600000) / 1000.0f; wc[2] = outdoors ? 0 : 0.28f; wc[3] = 0.85f;
        memcpy(wc + 4, vp.m, 64);
        wc[20] = sky[0]; wc[21] = sky[1]; wc[22] = sky[2]; wc[23] = g_dynMaskReady ? 1.0f : 0.0f;
        g_dev->SetPixelShaderConstantF(30, wc, 6);
        g_dev->SetTexture(5, g_dynMaskReady ? g_dynMask : NULL);
        g_dev->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_dev->SetPixelShader(g_psWet);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, kFullTri, 16);
        g_dev->SetTexture(5, NULL);
    }
    // Brume : teinte du jour (bleu pale), du coucher (doree), du soir et de la nuit (violette) ; pluie et brouillard : grise.
    if (g_cfg.haze && g_psHaze && outdoors) {
        float sunset = SunsetK(), rain = WeatherF(0x975340), fog = WeatherF(0x94DDC0);
        float col[3] = { 0.72f, 0.82f, 0.95f };
        const float gold[3] = { 1.0f, 0.70f, 0.52f }, violet[3] = { 0.30f, 0.25f, 0.44f };
        DWORD fogCol = 0;
        g_dev->GetRenderState(D3DRS_FOGCOLOR, &fogCol);
        float gray[3] = { ((fogCol >> 16) & 255) / 255.0f, ((fogCol >> 8) & 255) / 255.0f, (fogCol & 255) / 255.0f };
        float wx = rain * 0.8f + fog; if (wx > 1) wx = 1;
        for (int k = 0; k < 3; k++) {
            col[k] = col[k] + (gold[k] - col[k]) * sunset;
            col[k] = col[k] + (violet[k] - col[k]) * g_night;
            col[k] = col[k] + (gray[k] - col[k]) * wx;
        }
        float hc[2 * 4] = { col[0], col[1], col[2], 0.0015f + 0.002f * sunset + 0.004f * fog + 0.0015f * rain,
                            70.0f - 40.0f * fog, 0.025f, 0.15f + 0.35f * sunset, 0 };
        g_dev->SetPixelShaderConstantF(30, hc, 2);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        g_dev->SetPixelShader(g_psHaze);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, kFullTri, 16);
    }
    if (g_cfg.beams && lights && g_psBeam && g_vsBeam && outdoors) DrawBeams(vp, cam, oldDs);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

static void Apply()
{
    FpuGuard fpu;
    g_applied = true;
    UpdateSun();
    ChooseMainView();
    if (LightsWanted()) { AddLighthouse(); MergeLamps(); }
    bool shadows = ShadowsWanted() && g_sunK > 0.01f;
    bool lights = LightsWanted() && g_lightCount > 0;
    bool aoWanted = AoWanted();
    bool ambience = g_cfg.wetRoads || g_cfg.haze || g_cfg.beams || g_cfg.sunRays || g_cfg.softParticles || g_cfg.carReflections || g_cfg.indirectLight;
    if (g_recs.empty() || !g_haveMain || (!shadows && !lights && !aoWanted && !ambience) || !CreateResources()) return;
    if (lights && !g_psLights) lights = false;
    bool ao = aoWanted && g_psAO && g_psAOBlur && g_aoSurf;
    int receivers = 0;
    for (const Rec &r : g_recs) receivers += r.receiver;
    if (receivers < 20 || (!shadows && !lights && !ao && !ambience)) return;

    // Camera principale.
    M4 view, proj, vp, invVp;
    memcpy(view.m, g_mainView, 64); memcpy(proj.m, g_mainProj, 64);
    vp = Mul(view, proj);
    if (!Invert(vp, invVp)) return;
    M4 invView;
    if (!Invert(view, invView)) return;
    Vec3 cam = { invView.m[12], invView.m[13], invView.m[14] };
    Vec3 ahead = Transform(invVp, 0, 0, 0.5f, 1);
    Vec3 fwd = Norm(Sub(ahead, cam));
    float tanX = fabsf(1.0f / proj.m[0]), tanY = fabsf(1.0f / proj.m[5]);
    float p34 = proj.m[11] != 0 ? proj.m[11] : 1.0f;

    IDirect3DSurface9 *oldRt = NULL, *oldDs = NULL;
    g_dev->GetRenderTarget(0, &oldRt);
    g_dev->GetDepthStencilSurface(&oldDs);
    g_state->Capture();
    if (!UploadReplay()) { SafeRelease(oldRt); SafeRelease(oldDs); return; }

    // Etats communs a nos passes.
    SetCommonStates();

    // 0. Ray tracing (Rendu=12) : visibilite du soleil tracee par vcrt64.exe (memes dessins, memes matrices), a la
    // place des cascades. Sans reponse (carte sans DXR, programme arrete) : cascades.
    bool rtShadow = false;
    if (shadows && g_cfg.renderer == 12 && g_cfg.rtShadows && g_psRTMask && RtReady()) {
        static std::vector<RtDraw> draws;
        draws.clear();
        for (const Rec &r : g_recs) {
            if ((int)r.viewIdx != g_mainIdx || !r.caster) continue;
            RtDraw d;
            d.vb = r.vb; d.ib = r.ib; d.tex = r.tex;
            d.vbData = r.replayVb ? g_cpuVb.data() : NULL;
            d.ibData = r.replayIb ? (const WORD *)g_cpuIb.data() : NULL;
            d.stride = r.stride; d.fvf = r.fvf; d.d = r.d; d.world = r.world;
            d.alphaRef = r.alphaRef; d.alphaTest = r.alphaTest; d.vehicle = r.vehicle; d.dynamic = r.dynamic;
            draws.push_back(d);
        }
        float sun4[4] = { g_sun.x, g_sun.y, g_sun.z, g_sunK };
        UINT tw = g_width * g_cfg.rtScale / 100, th = g_height * g_cfg.rtScale / 100;
        rtShadow = RtTrace(draws.data(), (int)draws.size(), g_mainView, g_mainProj, sun4, g_moon ? 0.012f : 0.02f, 700.0f, tw, th) && RtResult();
        static bool logged;
        if (rtShadow && !logged) { logged = true; Log("rendu : ombres tracees (ray tracing) a la place des cascades"); }
    }

    // 1. Cascades.
    static const float splits[CASCADES] = { 12.0f, 35.0f, 90.0f, 220.0f };
    CascadeInfo casc[CASCADES] = {};
    int casters = 0;
    if (shadows && !rtShadow) {
    g_dev->SetRenderTarget(0, g_atlasSurf);
    g_dev->SetDepthStencilSurface(g_atlasDs);
    g_dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0);
    for (int c = 0; c < CASCADES; c++) {
        casc[c] = MakeCascade(cam, fwd, tanX, tanY, c ? splits[c - 1] : 0.3f, splits[c], g_cascadeSize);
        D3DVIEWPORT9 vpt = { (DWORD)((c & 1) * g_cascadeSize), (DWORD)((c >> 1) * g_cascadeSize), (DWORD)g_cascadeSize, (DWORD)g_cascadeSize, 0, 1 };
        g_dev->SetViewport(&vpt);
        for (const Rec &r : g_recs) {
            if (!r.caster) continue;
            M4 w; memcpy(w.m, r.world, 64);
            DrawRec(r, Mul(w, casc[c].light), false);
            casters++;
        }
    }
    }

    // 1b. Cartes d'ombre des lumieres les plus importantes (au plus 4, dans l'atlas 2x2).
    g_shadowLights = 0;
    if (lights) PrepareLightShadows(cam);

    // 2. Profondeur de la scene vue de la camera (l'eau comprise : elle recoit ombres et lumieres).
    const float depthScale = kDepthScale;
    RenderScreenDepth(vp, true);
    g_depthReady = true;
    // 2b. Masque des carrosseries (memes sommets et matrices : meme profondeur, test "inferieur ou egal").
    g_carMaskReady = false;
    if (g_cfg.carReflections && g_carMaskSurf && g_psFlag[0] && g_psFlag[1]) {
        g_dev->SetRenderTarget(0, g_carMaskSurf);
        g_dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        for (const Rec &r : g_recs) {
            if (!r.receiver || !r.vehicle) continue;
            int v = (r.fvf & D3DFVF_TEXCOUNT_MASK) ? 0 : 1;
            M4 w; memcpy(w.m, r.world, 64);
            DrawRec(r, Mul(w, vp), true);
            g_dev->SetPixelShader(g_psFlag[v]);
            if (r.d.indexed) g_dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)r.d.type, (INT)r.d.baseVertex, r.d.minIndex, r.d.numVerts, r.d.start, r.d.count);
            else g_dev->DrawPrimitive((D3DPRIMITIVETYPE)r.d.type, r.d.start, r.d.count);
            g_carMaskReady = true;
        }
        g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    }
    // 2c. Masque des personnages et vehicules : le sol mouille (pluie) et les sols brillants des interieurs ne
    // s'appliquaient qu'aux surfaces tournees vers le haut... y compris les epaules et la tete des passagers vus par
    // les vitres, et les toits des voitures (JD, 29/09).
    g_dynMaskReady = false;
    if (g_cfg.wetRoads && g_dynMaskSurf && g_psFlag[1]) {
        g_dev->SetRenderTarget(0, g_dynMaskSurf);
        g_dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        for (const Rec &r : g_recs) {
            if (!r.receiver || !r.dynamic) continue;
            M4 w; memcpy(w.m, r.world, 64);
            DrawRec(r, Mul(w, vp), true);
            g_dev->SetPixelShader(g_psFlag[1]);
            if (r.d.indexed) g_dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)r.d.type, (INT)r.d.baseVertex, r.d.minIndex, r.d.numVerts, r.d.start, r.d.count);
            else g_dev->DrawPrimitive((D3DPRIMITIVETYPE)r.d.type, r.d.start, r.d.count);
        }
        g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        g_dynMaskReady = true;
    }

    // 3. Masque d'ombre sur l'image.
    if (g_captureStage == 1 && !g_captureBefore && SUCCEEDED(g_dev->CreateRenderTarget(g_width, g_height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_captureBefore, NULL)))
        g_dev->StretchRect(oldRt, NULL, g_captureBefore, NULL, D3DTEXF_NONE);
    g_dev->SetRenderTarget(0, oldRt);
    g_dev->SetDepthStencilSurface(NULL);
    D3DVIEWPORT9 full = { 0, 0, g_width, g_height, 0, 1 };
    g_dev->SetViewport(&full);
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    if (g_debugMask) g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetTexture(0, g_screenDepth);
    g_dev->SetTexture(1, g_atlas);
    g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    float pc[30 * 4] = {};
    float *c0 = pc;
    c0[0] = (float)g_width; c0[1] = (float)g_height; c0[2] = 1.0f / g_width; c0[3] = 1.0f / g_height;
    memcpy(pc + 4, invVp.m, 64);
    float *c5 = pc + 20; c5[0] = proj.m[10] / p34; c5[1] = proj.m[14]; c5[2] = 1.0f / depthScale;
    for (int c = 0; c < CASCADES; c++) memcpy(pc + 24 + c * 16, casc[c].light.m, 64);
    float *c22 = pc + 88; for (int c = 0; c < CASCADES; c++) c22[c] = splits[c];
    float *c23 = pc + 92; c23[0] = g_sun.x; c23[1] = g_sun.y; c23[2] = g_sun.z; c23[3] = g_sunK;
    float *c24 = pc + 96;
    if (g_moon) { c24[0] = 0.40f; c24[1] = 0.46f; c24[2] = 0.66f; }   // lune : ombre bleutee
    else { c24[0] = 0.42f; c24[1] = 0.46f; c24[2] = 0.58f; }
    c24[3] = 1;
    float *c25 = pc + 100; c25[0] = (float)g_cascadeSize; c25[1] = 1.0f / (2 * g_cascadeSize); c25[2] = 0.15f; c25[3] = g_debugMask == 1 ? 1.0f : 0.0f;
    float fogStart = *(float *)0x978660, farClip = *(float *)0x9B6A6C;   // CTimeCycle : brouillard et distance de vue
    if (farClip < fogStart + 1) farClip = fogStart + 1;
    float *c26 = pc + 104; c26[0] = fogStart; c26[1] = farClip; c26[2] = splits[CASCADES - 1]; c26[3] = splits[CASCADES - 1] * 0.8f;
    float *c27 = pc + 108; c27[0] = cam.x; c27[1] = cam.y; c27[2] = cam.z; c27[3] = 1;
    float *c28 = pc + 112; float *c29 = pc + 116;
    for (int c = 0; c < CASCADES; c++) { c28[c] = casc[c].texelWorld; c29[c] = casc[c].bias; }
    g_dev->SetPixelShaderConstantF(0, pc, 30);
    g_dev->SetVertexShader(g_vsQuad);
    g_dev->SetFVF(D3DFVF_XYZW);
    static const float tri[12] = { -1, -1, 0.5f, 1, -1, 3, 0.5f, 1, 3, -1, 0.5f, 1 };
    if (shadows && g_debugMask != 2 && g_debugMask != 3) {
        if (rtShadow) {
            float rs[4]; RtResultSize(&rs[0], &rs[1]); rs[2] = 1.0f / rs[0]; rs[3] = 1.0f / rs[1];
            g_dev->SetPixelShaderConstantF(191, rs, 1);
            float fogRt[4] = { fogStart, farClip, 600.0f, 480.0f };   // ombres tracees jusqu'a 600 m
            g_dev->SetPixelShaderConstantF(26, fogRt, 1);
            g_dev->SetTexture(4, RtResult());
            g_dev->SetSamplerState(4, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            g_dev->SetSamplerState(4, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            g_dev->SetSamplerState(4, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            g_dev->SetSamplerState(4, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            g_dev->SetSamplerState(4, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            g_dev->SetPixelShader(g_psRTMask);
            g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 16);
            g_dev->SetTexture(4, NULL);
            g_dev->SetPixelShaderConstantF(26, c26, 1);
        } else {
            g_dev->SetPixelShader(g_psMask);
            g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 16);
        }
    }

    // 3b. Occlusion ambiante : calculee dans g_ao (rotations en grille 4x4), puis floutee en multipliant l'image.
    if (ao) {
        float ac[5 * 4] = {};
        memcpy(ac, vp.m, 64);
        ac[16] = 1.2f; ac[17] = 0.85f; ac[18] = 90.0f;   // rayon (m), force, fin (m)
        g_dev->SetPixelShaderConstantF(30, ac, 5);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        g_dev->SetRenderTarget(0, g_aoSurf);
        g_dev->SetPixelShader(g_psAO);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 16);
        g_dev->SetRenderTarget(0, oldRt);
        g_dev->SetViewport(&full);
        g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, g_debugMask == 3 ? FALSE : TRUE);
        g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
        g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
        g_dev->SetTexture(1, g_ao);
        g_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        g_dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        g_dev->SetPixelShader(g_psAOBlur);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 16);
    }

    // 4. Lumieres dynamiques : image x (1 + lumiere recue), les plus proches avec leur ombre.
    if (lights && g_debugMask != 3) {
        float amb = 0.22f + 0.78f * SkyLum();   // clarte ambiante de la scene (nuit : ~0,25)
        float lc[4] = { (float)g_lightCount, 0.4f, (float)g_shadowLights, amb };
        g_dev->SetPixelShaderConstantF(29, lc, 1);
        if (g_shadowLights) {
            g_dev->SetPixelShaderConstantF(174, g_lightVP[0].m, 4 * g_shadowLights);
            float ls[4] = { (float)LIGHT_TILE, 1.0f / (2 * LIGHT_TILE), g_debugMask == 2 ? 1.0f : 0.0f, 0 };
            g_dev->SetPixelShaderConstantF(190, ls, 1);
            g_dev->SetTexture(2, g_lightAtlas);
            g_dev->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            g_dev->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            g_dev->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            g_dev->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            g_dev->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        float lp[MAX_LIGHTS * 4], lcol[MAX_LIGHTS * 4], ldir[MAX_LIGHTS * 4];
        for (int i = 0; i < g_lightCount; i++) {
            const DynLight &l = g_lightList[i];
            bool spot = l.type == 1;
            float range = spot ? l.radius * 2.2f : l.radius * 1.6f;
            float *q = lp + i * 4; q[0] = l.x; q[1] = l.y; q[2] = l.z; q[3] = range;
            q = lcol + i * 4; q[0] = l.r; q[1] = l.g; q[2] = l.b; q[3] = spot ? 1.0f : 0.0f;
            Vec3 dir = Norm({ l.dx, l.dy, l.dz });
            q = ldir + i * 4; q[0] = dir.x; q[1] = dir.y; q[2] = dir.z; q[3] = l.cone;
        }
        g_dev->SetPixelShaderConstantF(30, lp, g_lightCount);
        g_dev->SetPixelShaderConstantF(78, lcol, g_lightCount);
        g_dev->SetPixelShaderConstantF(126, ldir, g_lightCount);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
        g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        if (!g_shadowLights) { float ls[4] = { (float)LIGHT_TILE, 1.0f / (2 * LIGHT_TILE), g_debugMask == 2 ? 1.0f : 0.0f, 0 }; g_dev->SetPixelShaderConstantF(190, ls, 1); }
        // L'image actuelle (ombres comprises) sert de couleur des surfaces ; la passe la remplace.
        if (g_refractSurf) g_dev->StretchRect(oldRt, NULL, g_refractSurf, NULL, D3DTEXF_NONE);
        g_dev->SetTexture(3, g_refract);
        g_dev->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        g_dev->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_dev->SetPixelShader(g_psLights);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 16);
    }

    // 5. Ambiance : sols mouilles / brillants, brume, faisceaux des phares.
    AmbiencePasses(vp, cam, oldRt, oldDs, lights);
    g_lastVP = vp;
    g_lastCam = cam;
    g_lastVPAt = GetTickCount();

    // Retour a l'etat du jeu.
    g_dev->SetTexture(0, NULL);
    g_dev->SetTexture(1, NULL);
    g_dev->SetTexture(2, NULL);
    g_dev->SetTexture(3, NULL);
    g_dev->SetRenderTarget(0, oldRt);
    g_dev->SetDepthStencilSurface(oldDs);
    g_state->Apply();
    SafeRelease(oldRt);
    SafeRelease(oldDs);

    static uint32_t lastLog;
    if (GetTickCount() - lastLog > 10000) {
        lastLog = GetTickCount();
        int dyn = 0;
        for (const Rec &r : g_recs) dyn += r.replayVb;
        Log("rendu : %d cameras (principale %d dessins) ; occlusion %s ; %d lumieres (%d avec ombre) ; %s ; ombres : %d projeteurs hors champ ; %d dessins (%d recepteurs, %d dynamiques), %d dans les cascades, soleil %.2f %.2f %.2f force %.2f, brouillard %.0f-%.0f ; %d dessins 3D apres le masque ; refus %d/%d/%d/%d/%d/%d/%d, UP 3D %d",
            g_viewCount, g_mainIdx >= 0 ? g_views[g_mainIdx].count : 0, ao ? "oui" : "non", lights ? g_lightCount : 0, g_shadowLights, g_moon ? "lune" : "soleil", g_extraCasters, (int)g_recs.size(), receivers, dyn, casters, g_sun.x, g_sun.y, g_sun.z, g_sunK, fogStart, farClip, g_after3d,
            g_why[0], g_why[1], g_why[2], g_why[3], g_why[4], g_why[5], g_why[6], g_why[7]);
    }
}

// ======================================================================= Eau moderne
// CWaterLevel::RenderWater (appel 0x4A6594) et RenderTransparentWater (0x4A65AE) : les surfaces d'eau du jeu (texture
// du premier dessin de RenderWater) sont notees sans etre affichees ; les oiseaux et bateaux a l'horizon restent. Apres
// RenderTransparentWater (bateaux et objets sous l'eau deja dessines), l'eau est dessinee avec notre shader :
// turquoise selon la profondeur, fond vu a travers (refraction de l'image), reflet du ciel du cycle du jour, soleil,
// ecume sur les rives, vagues calculees par pixel.
static int g_waterPass;          // 1 : RenderWater, 2 : RenderTransparentWater
static bool g_waterTexSet;
static void *g_waterTex;
static int g_waterDraws;

static void MaybeBindSoft(DWORD fvf);

// ======================================================================= Vegetation au vent
// CRenderer::RenderOneNonRoad (0x4C9DA0, aussi pour les entites en fondu) : on retient l'entite dessinee. Palmiers,
// arbres, buissons (nom du modele) : juste avant chacun de leurs dessins, la matrice monde est cisaillee depuis la base
// de l'objet (x += a (z - base)) selon le vent du jeu (CWeather::Wind 0x97533C), avec une phase par objet et des
// rafales ; retablie juste apres (Gfx9DrawDone). Les ombres (dessins notes) suivent le meme mouvement.
typedef void(__cdecl *RenderOne_t)(void *);
static RenderOne_t o_RenderOneNonRoad;
static void __cdecl h_RenderOneNonRoad(void *e)
{
    if (g_cfg.lampLights && g_lightsLive && e && IsLightBeam(*(short *)((uint8_t *)e + 0x5C))) {
        g_lhPos = { *(float *)((uint8_t *)e + 0x34), *(float *)((uint8_t *)e + 0x38), *(float *)((uint8_t *)e + 0x3C) };
        return;   // cone d'origine remplace par la vraie lumiere tournante
    }
    void *prev = g_curEntity;
    g_curEntity = e;
    o_RenderOneNonRoad(e);
    g_curEntity = prev;
}
static uint8_t g_vegFlag[6600];   // 0 inconnu, 1 vegetation, 2 non
static bool IsVegetation(int model)
{
    if (model < 0 || model >= 6600) return false;
    if (!g_vegFlag[model]) {
        const char *n = ModelName(model);
        static const char *const keys[] = { "palm", "tree", "veg_", "bush", "plant", "hedge", "shrub", "fern", "flower", "banana", "cypress", "leaves" };
        bool veg = false;
        if (n && n[0]) {
            char low[32]; int i = 0;
            for (; n[i] && i < 31; i++) low[i] = (char)((n[i] >= 'A' && n[i] <= 'Z') ? n[i] + 32 : n[i]);
            low[i] = 0;
            for (const char *k : keys) if (strstr(low, k)) veg = true;
        }
        g_vegFlag[model] = veg ? 1 : 2;
    }
    return g_vegFlag[model] == 1;
}
static bool g_windBound;
static D3DMATRIX g_windSaved;
static int g_windDraws;
static void MaybeWind()
{
    if (!g_cfg.windPlants || !g_curEntity || !g_dev) return;
    uint8_t *e = (uint8_t *)g_curEntity;
    int type = e[0x50] & 7;
    if (type != 1 && type != 4 && type != 5) return;   // batiment, objet, decor
    if (!IsVegetation(*(short *)(e + 0x5C))) return;
    float bx = *(float *)(e + 0x34), by = *(float *)(e + 0x38), bz = *(float *)(e + 0x3C);
    float t = (GetTickCount() % 3600000) / 1000.0f;
    float wind = *(float *)0x97533C;
    if (!(wind >= 0 && wind < 3)) wind = 0;
    float amp = 0.006f + 0.022f * wind;
    float ph = bx * 0.13f + by * 0.17f;
    float gust = 0.75f + 0.25f * sinf(t * 0.37f + ph * 0.3f);
    float sx = amp * gust * (sinf(t * 1.3f + ph) + 0.35f * sinf(t * 3.1f + ph * 1.7f));
    float sy = amp * gust * 0.6f * sinf(t * 1.1f + ph * 0.7f + 1.0f);
    M4 w;
    g_dev->GetTransform(D3DTS_WORLD, (D3DMATRIX *)w.m);
    memcpy(&g_windSaved, w.m, 64);
    M4 a = Identity4();
    a.m[8] = sx; a.m[9] = sy;                     // ligne z : x += sx z, y += sy z
    a.m[12] = -sx * bz; a.m[13] = -sy * bz;       // ... mesure depuis la base de l'objet
    M4 w2 = Mul(w, a);
    g_dev->SetTransform(D3DTS_WORLD, (D3DMATRIX *)w2.m);
    g_windBound = true;
    g_windDraws++;
    (void)bx; (void)by;
}
static void RestoreWind()
{
    if (!g_windBound) return;
    g_windBound = false;
    g_dev->SetTransform(D3DTS_WORLD, &g_windSaved);
}

extern bool g_hideOwnGlass;   // camera.cpp : vehicule du joueur en vue a la premiere personne
static bool InterceptInner(DWORD fvf, const GfxDraw &d, bool up);
bool Gfx9Intercept(DWORD fvf, const GfxDraw &d, bool up)
{
    MaybeWind();
    bool r = InterceptInner(fvf, d, up);
    if (r) RestoreWind();   // dessin saute : la matrice est rendue tout de suite
    return r;
}
static bool InterceptInner(DWORD fvf, const GfxDraw &d, bool up)
{
    if (g_hideOwnGlass && g_dev) {   // ses vitres (dessins transparents) ne sont pas dessinees
        DWORD blend = 0;
        g_dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
        if (blend) return true;
    }
    if (g_bridgeCasterOnly) { if (!up) Gfx9AfterDraw(fvf, d); return true; }
    if (g_effectsPass && g_dev) { MaybeBindSoft(fvf); return false; }
    if (!g_waterPass || !g_dev || !WaterWanted()) return false;
    // Sur PC, RenderWater ne dessine que la mer au loin ; l'eau proche vient de RenderTransparentWater (meme texture),
    // suivie d'un masque (autre texture) qu'on retire aussi.
    IDirect3DBaseTexture9 *t = NULL;
    g_dev->GetTexture(0, &t);
    if (t) t->Release();
    if (!g_waterTexSet) { g_waterTexSet = true; g_waterTex = t; }
    if (t != g_waterTex) return g_waterPass == 2;
    if (up || (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ || !(d.type == D3DPT_TRIANGLELIST || d.type == D3DPT_TRIANGLESTRIP || d.type == D3DPT_TRIANGLEFAN)) return true;
    if (g_recs.size() >= 6000) return true;
    Rec r = {};
    if (BuildRec(fvf, d, r)) {
        r.water = true;
        r.recvCand = true;
        g_recs.push_back(r);
        g_waterDraws++;
    }
    return true;
}

static float SkyChan(uintptr_t a) { int v = *(int *)a; return (v < 0 ? 0 : v > 255 ? 255 : v) / 255.0f; }

// Hauteur de l'eau (plan du miroir) : 6 m tant qu'aucun sommet d'eau n'est lisible.
static float g_waterLevel = 6.0f;
static void UpdateWaterLevel(const Vec3 &cam)
{
    // Hauteur la plus frequente (cases de 25 cm), en coordonnees du monde (matrice du dessin appliquee : les sommets
    // bruts de la mer au loin sont locaux, le plan tombait 6 m trop bas et les reflets flottaient a cote des objets),
    // l'eau proche de la camera comptant davantage (c'est elle qu'on voit refleter).
    static float bins[1000], sumZ[1000];
    memset(bins, 0, sizeof(bins));
    memset(sumZ, 0, sizeof(sumZ));
    int n = 0;
    for (const Rec &r : g_recs) {
        if (!r.water || !r.replayVb || r.stride < 12) continue;
        const float *m = r.world;
        UINT first = r.d.indexed ? r.d.baseVertex + r.d.minIndex : r.d.start;
        UINT cnt = r.d.indexed ? r.d.numVerts : VertexCount(r.d.type, r.d.count);
        for (UINT k = 0; k < cnt && n < 6000 && (first + k + 1) * r.stride <= g_cpuVb.size(); k += 3) {
            const float *v = (const float *)(g_cpuVb.data() + (first + k) * r.stride);
            float x = v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12];
            float y = v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13];
            float z = v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14];
            if (z <= -50 || z >= 200) continue;
            float dx = x - cam.x, dy = y - cam.y;
            float wgt = 1.0f / (1.0f + sqrtf(dx * dx + dy * dy) / 40.0f);
            int b = (int)((z + 50.0f) * 4.0f);
            bins[b] += wgt;
            sumZ[b] += z * wgt;
            n++;
        }
    }
    if (n <= 8) return;
    int best = 0;
    for (int i = 1; i < 1000; i++) if (bins[i] > bins[best]) best = i;
    // Les sommets de l'eau du jeu montent et descendent avec ses vagues : moyenne sur +-1 m autour du pic, puis
    // lissee dans le temps (sinon le plan sautait de 25 cm d'une image a l'autre et tout le reflet tremblait).
    float w = 0, sz = 0;
    for (int i = best - 4; i <= best + 4; i++) if (i >= 0 && i < 1000) { w += bins[i]; sz += sumZ[i]; }
    float level = sz / w;
    static uint32_t last;
    uint32_t now = GetTickCount();
    float dt = (now - last) / 1000.0f;
    last = now;
    if (fabsf(level - g_waterLevel) > 1.5f || dt > 1.0f) g_waterLevel = level;   // autre plan d'eau : tout de suite
    else g_waterLevel += (level - g_waterLevel) * (dt < 0.5f ? dt * 2.0f : 1.0f);
}

// Reflet : la scene notee jusqu'ici redessinee vue en miroir sous la surface (z -> 2h - z), a demi-resolution ; alpha 0
// la ou il n'y a rien (le ciel du cycle du jour y est mis par le shader de l'eau).
static bool RenderReflection(const M4 &vp, const Vec3 &cam)
{
    if (!g_cfg.waterReflections || !g_reflSurf || !g_psRefl) return false;
    UpdateWaterLevel(cam);
    float h = g_waterLevel;
    M4 mirror = Identity4();
    mirror.m[10] = -1; mirror.m[14] = 2 * h;
    M4 rvp = Mul(mirror, vp);
    g_dev->SetRenderTarget(0, g_reflSurf);
    g_dev->SetDepthStencilSurface(g_reflDs);
    D3DVIEWPORT9 half = { 0, 0, g_width / 2, g_height / 2, 0, 1 };
    g_dev->SetViewport(&half);
    g_dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00000000, 1.0f, 0);
    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    float sun[4] = { g_sun.x, g_sun.y, g_sun.z, 0.55f };
    float plane[4] = { h, 0, 0, 0 };
    g_dev->SetVertexShaderConstantF(8, sun, 1);
    g_dev->SetPixelShaderConstantF(1, plane, 1);
    g_dev->SetPixelShader(g_psRefl);
    int drawn = 0;
    for (const Rec &r : g_recs) {
        if (r.water || !r.mainView) continue;
        int v = ((r.fvf & D3DFVF_NORMAL) ? 1 : 0) | ((r.fvf & D3DFVF_DIFFUSE) ? 2 : 0) | ((r.fvf & D3DFVF_TEXCOUNT_MASK) ? 4 : 0);
        if (!g_vsRefl[v]) continue;
        M4 w; memcpy(w.m, r.world, 64);
        M4 wm = Mul(w, rvp);
        g_dev->SetVertexShader(g_vsRefl[v]);
        g_dev->SetFVF(r.fvf);
        g_dev->SetVertexShaderConstantF(0, wm.m, 4);
        g_dev->SetVertexShaderConstantF(4, w.m, 4);
        float alpha[4] = { r.alphaTest ? r.alphaRef : 0.0f, r.alphaTest && r.tex ? 1.0f : 0.0f, r.tex ? 1.0f : 0.0f, 0 };
        g_dev->SetPixelShaderConstantF(0, alpha, 1);
        g_dev->SetTexture(0, r.tex);
        g_dev->SetStreamSource(0, r.replayVb ? g_replayVb : r.vb, 0, r.stride);
        if (r.d.indexed) {
            g_dev->SetIndices(r.replayIb ? g_replayIb : r.ib);
            g_dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)r.d.type, (INT)r.d.baseVertex, r.d.minIndex, r.d.numVerts, r.d.start, r.d.count);
        } else g_dev->DrawPrimitive((D3DPRIMITIVETYPE)r.d.type, r.d.start, r.d.count);
        drawn++;
    }
    g_dev->SetTexture(0, NULL);
    return drawn > 0;
}

static void DrawModernWater()
{
    FpuGuard fpu;
    int n = 0;
    for (const Rec &r : g_recs) n += r.water;
    ChooseMainView();
    if (!n || !g_haveMain || !CreateResources() || !g_refractSurf || !g_psWater) return;
    UpdateSun();
    M4 view, proj, vp, invView;
    memcpy(view.m, g_mainView, 64); memcpy(proj.m, g_mainProj, 64);
    vp = Mul(view, proj);
    if (!Invert(view, invView)) return;
    Vec3 cam = { invView.m[12], invView.m[13], invView.m[14] };
    DWORD fogCol = 0;
    g_dev->GetRenderState(D3DRS_FOGCOLOR, &fogCol);

    IDirect3DSurface9 *oldRt = NULL, *oldDs = NULL;
    g_dev->GetRenderTarget(0, &oldRt);
    g_dev->GetDepthStencilSurface(&oldDs);
    g_state->Capture();
    if (!UploadReplay()) { SafeRelease(oldRt); SafeRelease(oldDs); return; }
    SetCommonStates();
    RenderScreenDepth(vp, false);                                   // le fond, sans l'eau
    g_dev->StretchRect(oldRt, NULL, g_refractSurf, NULL, D3DTEXF_LINEAR);   // l'image sous l'eau
    bool reflected = RenderReflection(vp, cam);

    g_dev->SetRenderTarget(0, oldRt);
    g_dev->SetDepthStencilSurface(oldDs);
    D3DVIEWPORT9 full = { 0, 0, g_width, g_height, 0, 1 };
    g_dev->SetViewport(&full);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    g_dev->SetTexture(0, g_screenDepth);
    g_dev->SetTexture(1, g_refract);
    g_dev->SetTexture(2, reflected ? g_refl : NULL);
    for (int s = 0; s < 3; s++) {
        g_dev->SetSamplerState(s, D3DSAMP_MINFILTER, s ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        g_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, s ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        g_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    // Couleurs du ciel du cycle du jour (CTimeCycle : haut 0xA0CE98.., bas 0xA0D958..).
    float top[3] = { SkyChan(0xA0CE98), SkyChan(0xA0FD70), SkyChan(0x978D1C) };
    float bot[3] = { SkyChan(0xA0D958), SkyChan(0x97F208), SkyChan(0x9B6DF4) };
    float day = (bot[0] * 0.3f + bot[1] * 0.59f + bot[2] * 0.11f) * 1.25f;
    if (day < 0.07f) day = 0.07f; if (day > 1) day = 1;
    float fogStart = *(float *)0x978660, farClip = *(float *)0x9B6A6C;
    if (farClip < fogStart + 1) farClip = fogStart + 1;
    float elev = (g_sun.z - 0.02f) / 0.12f; if (elev < 0) elev = 0; if (elev > 1) elev = 1;
    float pc[10 * 4] = {};
    pc[0] = (float)g_width; pc[1] = (float)g_height; pc[2] = 1.0f / g_width; pc[3] = 1.0f / g_height;
    pc[4] = cam.x; pc[5] = cam.y; pc[6] = cam.z; pc[7] = (float)(GetTickCount() % 600000) / 1000.0f;
    pc[8] = g_sun.x; pc[9] = g_sun.y; pc[10] = g_sun.z; pc[11] = elev * (0.35f + 0.65f * (g_sunK > 0.3f ? 1.0f : g_sunK / 0.3f));
    pc[12] = top[0]; pc[13] = top[1]; pc[14] = top[2];
    pc[16] = bot[0]; pc[17] = bot[1]; pc[18] = bot[2];
    pc[20] = fogStart; pc[21] = farClip; pc[22] = 1.0f / kDepthScale;
    pc[24] = ((fogCol >> 16) & 255) / 255.0f; pc[25] = ((fogCol >> 8) & 255) / 255.0f; pc[26] = (fogCol & 255) / 255.0f;
    pc[28] = 0.10f; pc[29] = 0.82f; pc[30] = 0.76f;   // eau peu profonde : turquoise clair
    pc[32] = 0.02f; pc[33] = 0.38f; pc[34] = 0.50f;   // au large : bleu-vert des Caraibes
    pc[36] = day;
    float wr[4] = { reflected ? 1.0f : 0.0f, 0.85f, g_waterLevel, 0 };
    g_dev->SetPixelShaderConstantF(0, pc, 10);
    g_dev->SetPixelShaderConstantF(10, wr, 1);
    g_dev->SetVertexShader(g_vsWater);
    g_dev->SetPixelShader(g_psWater);
    for (const Rec &r : g_recs) {
        if (!r.water) continue;
        M4 w; memcpy(w.m, r.world, 64);
        M4 wvp = Mul(w, vp);
        g_dev->SetVertexShaderConstantF(0, wvp.m, 4);
        g_dev->SetVertexShaderConstantF(4, w.m, 4);
        g_dev->SetFVF(r.fvf);
        g_dev->SetStreamSource(0, r.replayVb ? g_replayVb : r.vb, 0, r.stride);
        if (r.d.indexed) {
            g_dev->SetIndices(r.replayIb ? g_replayIb : r.ib);
            g_dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)r.d.type, (INT)r.d.baseVertex, r.d.minIndex, r.d.numVerts, r.d.start, r.d.count);
        } else g_dev->DrawPrimitive((D3DPRIMITIVETYPE)r.d.type, r.d.start, r.d.count);
    }
    g_dev->SetTexture(0, NULL);
    g_dev->SetTexture(1, NULL);
    g_dev->SetTexture(2, NULL);
    g_dev->SetRenderTarget(0, oldRt);
    g_dev->SetDepthStencilSurface(oldDs);
    g_state->Apply();
    SafeRelease(oldRt);
    SafeRelease(oldDs);
    static uint32_t lastLog;
    if (GetTickCount() - lastLog > 10000) {
        lastLog = GetTickCount();
        Log("rendu : reflets %s, eau a %.2f m", reflected ? "oui" : "non", g_waterLevel);
        // Point d'eau le plus proche de la camera (tests : ou regarder).
        float best = 1e12f; Vec3 at = { 0, 0, 0 };
        for (const Rec &r : g_recs) {
            if (!r.water || !r.replayVb || r.stride < 12) continue;
            UINT first = r.d.indexed ? r.d.baseVertex + r.d.minIndex : r.d.start;
            UINT cnt = r.d.indexed ? r.d.numVerts : VertexCount(r.d.type, r.d.count);
            for (UINT k = 0; k < cnt && (first + k + 1) * r.stride <= g_cpuVb.size(); k++) {
                const float *v = (const float *)(g_cpuVb.data() + (first + k) * r.stride);
                float dx = v[0] - cam.x, dy = v[1] - cam.y, d2 = dx * dx + dy * dy;
                if (d2 < best) { best = d2; at = { v[0], v[1], v[2] }; }
            }
        }
        Log("rendu : eau moderne : %d dessins, clarte %.2f ; eau la plus proche en %.0f %.0f %.1f (%.0f m)", n, day, at.x, at.y, at.z, sqrtf(best));
    }
}

static void __cdecl h_RenderWater()
{
    g_waterPass = 1;
    ((void(__cdecl *)())0x5C1710)();
    g_waterPass = 0;
}
static void __cdecl h_RenderTransparentWater()
{
    g_waterPass = 2;
    ((void(__cdecl *)())0x5BFF00)();
    g_waterPass = 0;
    if (WaterWanted() && !g_applied) DrawModernWater();
}

void Gfx9BeforeDraw(DWORD fvf, bool up)
{
    if (!g_dev) return;
    if (up && !g_applied && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZ) g_why[7]++;
    if (g_applied) {
        if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZ) g_after3d++;
        return;
    }
    // Secours (appel de RenderScene introuvable) : premier dessin 2D apres la scene 3D. Normalement les ombres se
    // posent a la fin de RenderScene (h_RenderScene) : sous la pluie, le jeu dessine des effets 2D en pleine scene
    // (reflets des halos sur la route mouillee) et, selon ce qui etait dans le champ, le masque se posait trop tot.
    if (!g_sceneHooked && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW && g_recs.size() >= 20) Apply();
}

void Gfx9EndScene() { if (!g_applied && g_recs.size() >= 20) Apply(); }

// ======================================================================= Post-traitement (avant l'interface)
// Juste avant Render2dStuff (appel 0x4A608E dans Idle : la scene, les particules, les halos et le flou de mouvement
// du jeu sont dessines ; l'interface pas encore) :
//  1. SMAA 1x (SMAA 2.8, licence MIT, smaa/) : bords en escalier (palmiers, fils, carrosseries) ;
//  2. eclat : zones claires (neons, soleil, phares, reflets) extraites a 1/4 de la taille, floutees a 1/4, 1/8, 1/16 ;
//  3. passe finale : nettete adaptative (contraste local, a la maniere de CAS), eclat ajoute, etalonnage (Original,
//     Vice, Film) teinte selon l'heure du jeu (coucher : roses et turquoises ; nuit : bleutee), vignette.
#include "smaa/smaa_src.h"
#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"

static const char kPostShaders[] = R"HLSL(
float4 gTexel : register(c0);      // 1/largeur, 1/hauteur de la source, largeur, hauteur
float4 gBloomP : register(c1);     // seuil, genou, direction du flou (x, y)
float4 gGrade0 : register(c2);     // saturation, contraste, exposition, vignette
float4 gGradeLo : register(c3);    // teinte des ombres (rgb), courbe filmique (w)
float4 gGradeHi : register(c4);    // teinte des lumieres (rgb), force de l'eclat (w)
float4 gSharp : register(c5);      // x = nettete (0..1)
sampler2D sSrc : register(s0);
sampler2D sB1 : register(s1);
sampler2D sB2 : register(s2);
sampler2D sB3 : register(s3);
sampler2D sRays : register(s4);
float4 gRay : register(c6);        // soleil a l'ecran (uv), force, rapport largeur / hauteur
struct PIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
PIn VsPost(PIn i) { return i; }

float3 Box4(float2 uv) {
  float2 t = gTexel.xy;
  return (tex2D(sSrc, uv + t * float2(-0.5, -0.5)).rgb + tex2D(sSrc, uv + t * float2(0.5, -0.5)).rgb +
          tex2D(sSrc, uv + t * float2(-0.5, 0.5)).rgb + tex2D(sSrc, uv + t * float2(0.5, 0.5)).rgb) * 0.25;
}
float4 PsBright(PIn i) : COLOR {
  float3 c = Box4(i.uv);
  float l = max(c.r, max(c.g, c.b));
  float knee = gBloomP.y;
  float soft = clamp(l - gBloomP.x + knee, 0, 2 * knee);
  soft = soft * soft / (4 * knee + 1e-4);
  float k = max(soft, l - gBloomP.x) / max(l, 1e-4);
  return float4(c * k, 1);
}
float4 PsDown(PIn i) : COLOR { return float4(Box4(i.uv), 1); }
float4 PsBlur(PIn i) : COLOR {
  float2 d = gBloomP.zw * gTexel.xy;
  const float w[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };
  float3 c = tex2D(sSrc, i.uv).rgb * w[0];
  [unroll] for (int k = 1; k < 5; k++) c += (tex2D(sSrc, i.uv + d * k).rgb + tex2D(sSrc, i.uv - d * k).rgb) * w[k];
  return float4(c, 1);
}
// Rayons de soleil : ciel clair pres du soleil (profondeur en s1), puis flou radial vers lui.
float4 PsRayMask(PIn i) : COLOR {
  float d = tex2Dlod(sB1, float4(i.uv, 0, 0)).r;
  float sky = d >= 0.999 ? 1 : 0;
  float3 c = tex2Dlod(sSrc, float4(i.uv, 0, 0)).rgb;
  float l = dot(c, float3(0.3, 0.59, 0.11));
  float2 dv = i.uv - gRay.xy; dv.x *= gRay.w;
  float fall = saturate(1 - length(dv) * 1.1);
  return float4(c * sky * saturate(l * 1.6 - 0.35) * fall * fall, 1);
}
float4 PsRayBlur(PIn i) : COLOR {
  float2 delta = (i.uv - gRay.xy) * (0.92 / 40);
  float2 uv = i.uv;
  float3 sum = 0;
  float decay = 1;
  [unroll] for (int k = 0; k < 40; k++) { uv -= delta; sum += tex2Dlod(sSrc, float4(uv, 0, 0)).rgb * decay; decay *= 0.965; }
  return float4(sum / 22, 1);
}
float3 Hable(float3 x) {
  const float A = 0.22, B = 0.30, C = 0.10, D = 0.20, E = 0.01, F = 0.30;
  return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
float4 PsFinal(PIn i) : COLOR {
  float2 t = gTexel.xy;
  float3 c = tex2Dlod(sSrc, float4(i.uv, 0, 0)).rgb;
  [branch] if (gSharp.x > 0) {
    float3 n = tex2Dlod(sSrc, float4(i.uv + float2(0, -t.y), 0, 0)).rgb, s = tex2Dlod(sSrc, float4(i.uv + float2(0, t.y), 0, 0)).rgb;
    float3 e = tex2Dlod(sSrc, float4(i.uv + float2(t.x, 0), 0, 0)).rgb, w = tex2Dlod(sSrc, float4(i.uv + float2(-t.x, 0), 0, 0)).rgb;
    float3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));
    float3 amp = sqrt(saturate(min(mn, 1 - mx) / max(mx, 1e-4)));   // peu de contraste local : plus de nettete
    float3 wgt = -amp * gSharp.x * 0.2;
    c = saturate((c + (n + s + e + w) * wgt) / (1 + 4 * wgt));
  }
  c += (tex2D(sB1, i.uv).rgb * 0.55 + tex2D(sB2, i.uv).rgb * 0.75 + tex2D(sB3, i.uv).rgb * 0.95) * gGradeHi.w;
  c += tex2D(sRays, i.uv).rgb * gRay.z * float3(1.0, 0.9, 0.72);
  c *= gGrade0.z;
  if (gGradeLo.w > 0.5) c = Hable(c * 3.2) / Hable(3.2);   // courbe "film" : hautes lumieres adoucies
  float l = dot(c, float3(0.2126, 0.7152, 0.0722));
  c = lerp(l.xxx, c, gGrade0.x);
  c = (c - 0.5) * gGrade0.y + 0.5;
  c *= lerp(gGradeLo.rgb, gGradeHi.rgb, smoothstep(0.05, 0.85, l));   // ombres / lumieres teintees
  float2 v = i.uv - 0.5;
  c *= 1 - dot(v, v) * gGrade0.w;
  return float4(saturate(c), 1);
}
)HLSL";

static const char kSmaaWrap[] = R"HLSL(
float4 gRtMetrics : register(c0);
#define SMAA_RT_METRICS gRtMetrics
#define SMAA_HLSL_3 1
#define SMAA_PRESET_HIGH 1
)HLSL";
static const char kSmaaEntries[] = R"HLSL(
sampler2D sColor : register(s0);
sampler2D sEdges : register(s1);
sampler2D sArea : register(s2);
sampler2D sSearch : register(s3);
struct SIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct SEdge { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 o0 : TEXCOORD1; float4 o1 : TEXCOORD2; float4 o2 : TEXCOORD3; };
SEdge VsSmaaEdge(SIn i) { SEdge o; o.pos = i.pos; o.uv = i.uv; float4 off[3]; SMAAEdgeDetectionVS(i.uv, off); o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2]; return o; }
float4 PsSmaaEdge(SEdge i) : COLOR { float4 off[3] = { i.o0, i.o1, i.o2 }; return float4(SMAALumaEdgeDetectionPS(i.uv, off, sColor), 0, 0); }
struct SWeight { float4 pos : POSITION; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o0 : TEXCOORD2; float4 o1 : TEXCOORD3; float4 o2 : TEXCOORD4; };
SWeight VsSmaaWeight(SIn i) { SWeight o; o.pos = i.pos; o.uv = i.uv; float4 off[3]; SMAABlendingWeightCalculationVS(i.uv, o.pix, off); o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2]; return o; }
float4 PsSmaaWeight(SWeight i) : COLOR { float4 off[3] = { i.o0, i.o1, i.o2 }; return SMAABlendingWeightCalculationPS(i.uv, i.pix, off, sEdges, sArea, sSearch, 0); }
struct SBlend { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };
SBlend VsSmaaBlend(SIn i) { SBlend o; o.pos = i.pos; o.uv = i.uv; SMAANeighborhoodBlendingVS(i.uv, o.o); return o; }
float4 PsSmaaBlend(SBlend i) : COLOR { return SMAANeighborhoodBlendingPS(i.uv, i.o, sColor, sEdges); }
)HLSL";

static IDirect3DVertexShader9 *g_vsPost, *g_vsSmaa[3];
static IDirect3DPixelShader9 *g_psSmaa[3], *g_psBright, *g_psDown, *g_psBlur, *g_psFinal, *g_psRayMask, *g_psRayBlur;
static IDirect3DTexture9 *g_rayTex[2];
static IDirect3DTexture9 *g_postA, *g_postB, *g_smaaEdge, *g_smaaWeight, *g_areaTex, *g_searchTex, *g_bloomTex[3][2];
static IDirect3DStateBlock9 *g_postState;
static bool g_postTried, g_postShadersOk, g_postResOk;
static UINT g_bloomW[3], g_bloomH[3];

static IUnknown *CompileFrom(const std::string &source, const char *entry, const char *target)
{
    if (!g_compile) return NULL;
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = g_compile(source.data(), source.size(), "vccoop-post", NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        Log("rendu : shader %s refuse : %.600s", entry, err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return NULL;
    }
    if (err) err->Release();
    IUnknown *out = NULL;
    if (target[0] == 'v') g_dev->CreateVertexShader((const DWORD *)code->GetBufferPointer(), (IDirect3DVertexShader9 **)&out);
    else g_dev->CreatePixelShader((const DWORD *)code->GetBufferPointer(), (IDirect3DPixelShader9 **)&out);
    code->Release();
    return out;
}

static bool CreatePostShaders()
{
    if (g_postTried) return g_postShadersOk;
    g_postTried = true;
    CreateShaders();
    if (!g_compile) return false;
    std::string post(kPostShaders);
    g_vsPost = (IDirect3DVertexShader9 *)CompileFrom(post, "VsPost", "vs_3_0");
    g_psBright = (IDirect3DPixelShader9 *)CompileFrom(post, "PsBright", "ps_3_0");
    g_psDown = (IDirect3DPixelShader9 *)CompileFrom(post, "PsDown", "ps_3_0");
    g_psBlur = (IDirect3DPixelShader9 *)CompileFrom(post, "PsBlur", "ps_3_0");
    g_psFinal = (IDirect3DPixelShader9 *)CompileFrom(post, "PsFinal", "ps_3_0");
    g_psRayMask = (IDirect3DPixelShader9 *)CompileFrom(post, "PsRayMask", "ps_3_0");
    g_psRayBlur = (IDirect3DPixelShader9 *)CompileFrom(post, "PsRayBlur", "ps_3_0");
    std::string smaa = std::string(kSmaaWrap) + kSmaaSource + kSmaaEntries;
    static const char *vs[3] = { "VsSmaaEdge", "VsSmaaWeight", "VsSmaaBlend" }, *ps[3] = { "PsSmaaEdge", "PsSmaaWeight", "PsSmaaBlend" };
    for (int k = 0; k < 3; k++) {
        g_vsSmaa[k] = (IDirect3DVertexShader9 *)CompileFrom(smaa, vs[k], "vs_3_0");
        g_psSmaa[k] = (IDirect3DPixelShader9 *)CompileFrom(smaa, ps[k], "ps_3_0");
    }
    g_postShadersOk = g_vsPost && g_psFinal;
    Log("rendu : post-traitement %s (SMAA %s, eclat %s)", g_postShadersOk ? "pret" : "en echec",
        g_vsSmaa[0] && g_vsSmaa[1] && g_vsSmaa[2] && g_psSmaa[0] && g_psSmaa[1] && g_psSmaa[2] ? "oui" : "non",
        g_psBright && g_psDown && g_psBlur ? "oui" : "non");
    return g_postShadersOk;
}

static void ReleasePostResources()
{
    SafeRelease(g_postA); SafeRelease(g_postB); SafeRelease(g_smaaEdge); SafeRelease(g_smaaWeight);
    for (auto &l : g_bloomTex) { SafeRelease(l[0]); SafeRelease(l[1]); }
    SafeRelease(g_rayTex[0]); SafeRelease(g_rayTex[1]);
    SafeRelease(g_postState);
    g_postResOk = false;
}

static bool RenderTargetTex(UINT w, UINT h, IDirect3DTexture9 **t)
{
    return SUCCEEDED(g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, t, NULL));
}

static bool CreatePostResources()
{
    if (g_postResOk) return true;
    if (g_postResFailed || !CreatePostShaders()) return false;
    g_postResFailed = true;
    if (!RenderTargetTex(g_width, g_height, &g_postA) || !RenderTargetTex(g_width, g_height, &g_postB) ||
        !RenderTargetTex(g_width, g_height, &g_smaaEdge) || !RenderTargetTex(g_width, g_height, &g_smaaWeight)) {
        Log("rendu : cibles du post-traitement impossibles"); ReleasePostResources(); return false;
    }
    for (int l = 0; l < 3; l++) {
        g_bloomW[l] = g_width >> (2 + l); g_bloomH[l] = g_height >> (2 + l);
        if (g_bloomW[l] < 8) g_bloomW[l] = 8;
        if (g_bloomH[l] < 8) g_bloomH[l] = 8;
        if (!RenderTargetTex(g_bloomW[l], g_bloomH[l], &g_bloomTex[l][0]) || !RenderTargetTex(g_bloomW[l], g_bloomH[l], &g_bloomTex[l][1])) {
            Log("rendu : cibles de l'eclat impossibles"); ReleasePostResources(); return false;
        }
    }
    if (!RenderTargetTex(g_bloomW[0], g_bloomH[0], &g_rayTex[0]) || !RenderTargetTex(g_bloomW[0], g_bloomH[0], &g_rayTex[1])) {
        Log("rendu : cibles des rayons impossibles"); ReleasePostResources(); return false;
    }
    // Tables du SMAA (gardees d'une remise a zero a l'autre) : aire en A8L8 (R -> L, G -> A : lue .ra), recherche en L8.
    if (!g_areaTex && SUCCEEDED(g_dev->CreateTexture(AREATEX_WIDTH, AREATEX_HEIGHT, 1, 0, D3DFMT_A8L8, D3DPOOL_MANAGED, &g_areaTex, NULL))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_areaTex->LockRect(0, &lr, NULL, 0))) {
            for (int y = 0; y < AREATEX_HEIGHT; y++) memcpy((BYTE *)lr.pBits + y * lr.Pitch, areaTexBytes + y * AREATEX_PITCH, AREATEX_PITCH);
            g_areaTex->UnlockRect(0);
        }
    }
    if (!g_searchTex && SUCCEEDED(g_dev->CreateTexture(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &g_searchTex, NULL))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_searchTex->LockRect(0, &lr, NULL, 0))) {
            for (int y = 0; y < SEARCHTEX_HEIGHT; y++) memcpy((BYTE *)lr.pBits + y * lr.Pitch, searchTexBytes + y * SEARCHTEX_PITCH, SEARCHTEX_PITCH);
            g_searchTex->UnlockRect(0);
        }
    }
    if (FAILED(g_dev->CreateStateBlock(D3DSBT_ALL, &g_postState))) { ReleasePostResources(); return false; }
    g_postResFailed = false;
    g_postResOk = true;
    return true;
}

// Rectangle plein ecran sur une cible de w x h : demi-texel de Direct3D 9 compense.
static void PostQuadTo(IDirect3DSurface9 *s, UINT w, UINT h)
{
    g_dev->SetRenderTarget(0, s);
    D3DVIEWPORT9 vp = { 0, 0, w, h, 0, 1 };
    g_dev->SetViewport(&vp);
    float ox = -1.0f / w, oy = 1.0f / h;
    const float q[4 * 6] = {
        -1 + ox, 1 + oy, 0.5f, 1, 0, 0,
         1 + ox, 1 + oy, 0.5f, 1, 1, 0,
        -1 + ox, -1 + oy, 0.5f, 1, 0, 1,
         1 + ox, -1 + oy, 0.5f, 1, 1, 1,
    };
    g_dev->SetFVF(D3DFVF_XYZW | D3DFVF_TEX1);
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, 24);
}
static void PostQuad(IDirect3DTexture9 *target, UINT w, UINT h)
{
    IDirect3DSurface9 *s = NULL;
    target->GetSurfaceLevel(0, &s);
    PostQuadTo(s, w, h);
    s->Release();
}
static void Sampler(int s, IDirect3DBaseTexture9 *t, bool linear)
{
    g_dev->SetTexture(s, t);
    D3DTEXTUREFILTERTYPE f = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    g_dev->SetSamplerState(s, D3DSAMP_MINFILTER, f);
    g_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, f);
    g_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    g_dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g_dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g_dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
}

static void SaveCapture(IDirect3DSurface9 *from, const char *tag);
static bool PostWanted()
{
    return ModernRenderer() && GameState() == GS_PLAYING && (g_cfg.smaa || g_cfg.bloom || g_cfg.grade || g_cfg.sharpen > 0);
}

static void PostProcess()
{
    if (!g_dev || !PostWanted() || !CreatePostResources()) return;
    FpuGuard fpu;
    IDirect3DSurface9 *bb = NULL, *oldRt = NULL, *oldDs = NULL;
    if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    g_dev->GetRenderTarget(0, &oldRt);
    g_dev->GetDepthStencilSurface(&oldDs);
    g_postState->Capture();
    IDirect3DSurface9 *a = NULL;
    g_postA->GetSurfaceLevel(0, &a);
    g_dev->StretchRect(bb, NULL, a, NULL, D3DTEXF_NONE);   // (resout le MSAA)
    if (g_captureStage == 1) SaveCapture(a, "-brut");   // tests : l'image avant le post-traitement
    a->Release();
    SetCommonStates();
    g_dev->SetDepthStencilSurface(NULL);
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    for (int s = 0; s < 4; s++) g_dev->SetTexture(s, NULL);

    IDirect3DTexture9 *src = g_postA;
    // 1. SMAA.
    bool smaa = g_cfg.smaa && g_vsSmaa[0] && g_vsSmaa[1] && g_vsSmaa[2] && g_psSmaa[0] && g_psSmaa[1] && g_psSmaa[2] && g_areaTex && g_searchTex;
    if (smaa) {
        float rtm[4] = { 1.0f / g_width, 1.0f / g_height, (float)g_width, (float)g_height };
        g_dev->SetVertexShaderConstantF(0, rtm, 1);
        g_dev->SetPixelShaderConstantF(0, rtm, 1);
        IDirect3DSurface9 *s = NULL;
        g_smaaEdge->GetSurfaceLevel(0, &s); g_dev->SetRenderTarget(0, s); g_dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1, 0); s->Release();
        g_dev->SetVertexShader(g_vsSmaa[0]); g_dev->SetPixelShader(g_psSmaa[0]);
        Sampler(0, g_postA, true);
        PostQuad(g_smaaEdge, g_width, g_height);
        g_smaaWeight->GetSurfaceLevel(0, &s); g_dev->SetRenderTarget(0, s); g_dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1, 0); s->Release();
        g_dev->SetVertexShader(g_vsSmaa[1]); g_dev->SetPixelShader(g_psSmaa[1]);
        Sampler(0, NULL, true); Sampler(1, g_smaaEdge, true); Sampler(2, g_areaTex, true); Sampler(3, g_searchTex, false);
        PostQuad(g_smaaWeight, g_width, g_height);
        g_dev->SetVertexShader(g_vsSmaa[2]); g_dev->SetPixelShader(g_psSmaa[2]);
        Sampler(0, g_postA, true); Sampler(1, g_smaaWeight, true); Sampler(2, NULL, true); Sampler(3, NULL, true);
        PostQuad(g_postB, g_width, g_height);
        src = g_postB;
    }
    g_dev->SetVertexShader(g_vsPost);
    // 2. Eclat.
    bool bloom = g_cfg.bloom && g_psBright && g_psDown && g_psBlur;
    float night = g_night;
    if (bloom) {
        // Seuil plus bas la nuit : neons et phares ressortent sur une image sombre.
        float bp[4] = { 0.90f - 0.28f * night, 0.10f, 0, 0 };
        for (int l = 0; l < 3; l++) {
            IDirect3DTexture9 *from = l == 0 ? src : g_bloomTex[l - 1][0];
            UINT fw = l == 0 ? g_width : g_bloomW[l - 1], fh = l == 0 ? g_height : g_bloomH[l - 1];
            float tx[4] = { 1.0f / fw, 1.0f / fh, (float)fw, (float)fh };
            g_dev->SetPixelShaderConstantF(0, tx, 1);
            g_dev->SetPixelShaderConstantF(1, bp, 1);
            Sampler(0, from, true);
            g_dev->SetPixelShader(l == 0 ? g_psBright : g_psDown);
            PostQuad(g_bloomTex[l][0], g_bloomW[l], g_bloomH[l]);
            float bt[4] = { 1.0f / g_bloomW[l], 1.0f / g_bloomH[l], (float)g_bloomW[l], (float)g_bloomH[l] };
            g_dev->SetPixelShaderConstantF(0, bt, 1);
            g_dev->SetPixelShader(g_psBlur);
            for (int pass = 0; pass < 2; pass++) {
                float dir[4] = { bp[0], bp[1], pass == 0 ? 1.5f : 0.0f, pass == 0 ? 0.0f : 1.5f };
                g_dev->SetPixelShaderConstantF(1, dir, 1);
                Sampler(0, g_bloomTex[l][pass], true);
                PostQuad(g_bloomTex[l][1 - pass], g_bloomW[l], g_bloomH[l]);
            }
        }
    }
    // 2b. Rayons de soleil : le soleil devant la camera, au-dessus de l'horizon, dehors.
    float rayK = 0, rayUv[2] = { 0.5f, 0.5f };
    if (g_cfg.sunRays && g_psRayMask && g_psRayBlur && g_depthReady && g_screenDepth && !g_moon && g_sun.z > 0.02f && g_sunK > 0.05f && Outdoors()) {
        Vec3 sp = { g_lastCam.x + g_sun.x * 800, g_lastCam.y + g_sun.y * 800, g_lastCam.z + g_sun.z * 800 };
        float cl[4];
        for (int j = 0; j < 4; j++) cl[j] = sp.x * g_lastVP.m[j] + sp.y * g_lastVP.m[4 + j] + sp.z * g_lastVP.m[8 + j] + g_lastVP.m[12 + j];
        if (cl[3] > 1) {
            rayUv[0] = cl[0] / cl[3] * 0.5f + 0.5f; rayUv[1] = -cl[1] / cl[3] * 0.5f + 0.5f;
            float off = fmaxf(fmaxf(-rayUv[0], rayUv[0] - 1), fmaxf(-rayUv[1], rayUv[1] - 1));   // hors de l'image : s'efface
            float vis = off <= 0 ? 1.0f : fmaxf(0.0f, 1.0f - off * 2.5f);
            float sunsetR = 0;
            if (g_sun.z < 0.35f) sunsetR = 1.0f - fabsf(g_sun.z - 0.12f) / 0.23f;
            if (sunsetR < 0) sunsetR = 0;
            rayK = vis * g_sunK * (0.28f + 0.45f * sunsetR);
        }
        if (rayK > 0.01f) {
            float rc[4] = { rayUv[0], rayUv[1], rayK, (float)g_width / g_height };
            float tx[4] = { 1.0f / g_width, 1.0f / g_height, (float)g_width, (float)g_height };
            g_dev->SetPixelShaderConstantF(0, tx, 1);
            g_dev->SetPixelShaderConstantF(6, rc, 1);
            Sampler(0, src, true); Sampler(1, g_screenDepth, false);
            g_dev->SetPixelShader(g_psRayMask);
            PostQuad(g_rayTex[0], g_bloomW[0], g_bloomH[0]);
            Sampler(1, NULL, true);
            Sampler(0, g_rayTex[0], true);
            g_dev->SetPixelShader(g_psRayBlur);
            PostQuad(g_rayTex[1], g_bloomW[0], g_bloomH[0]);
        }
    }
    // 3. Passe finale vers l'image.
    float sunset = 0;   // soleil bas sur l'horizon (lever, coucher)
    if (g_sun.z > -0.05f && g_sun.z < 0.35f && !g_moon) sunset = 1.0f - fabsf(g_sun.z - 0.12f) / 0.23f;
    if (sunset < 0) sunset = 0;
    float c[6 * 4] = {};
    c[0] = 1.0f / g_width; c[1] = 1.0f / g_height; c[2] = (float)g_width; c[3] = (float)g_height;
    float *g0 = c + 8, *lo = c + 12, *hi = c + 16;
    for (int k = 0; k < 3; k++) { lo[k] = 1; hi[k] = 1; }
    g0[0] = 1; g0[1] = 1; g0[2] = 1; g0[3] = 0;
    if (g_cfg.grade == 1) {   // Vice : saturation, contraste, ombres turquoise, lumieres roses au coucher, nuits bleutees
        g0[0] = 1.15f; g0[1] = 1.05f; g0[2] = 1.0f; g0[3] = 0.18f;
        lo[0] = 0.97f - 0.02f * sunset - 0.04f * night; lo[1] = 1.0f + 0.02f * sunset; lo[2] = 1.03f + 0.03f * sunset + 0.07f * night;
        hi[0] = 1.02f + 0.06f * sunset; hi[1] = 0.99f - 0.03f * sunset; hi[2] = 1.0f + 0.03f * sunset + 0.03f * night;
    } else if (g_cfg.grade == 2) {   // Film : courbe douce, couleurs un peu retenues, chaleur legere
        g0[0] = 0.92f; g0[1] = 1.0f; g0[2] = 1.05f; g0[3] = 0.30f;
        lo[0] = 0.98f; lo[1] = 0.99f; lo[2] = 1.03f; lo[3] = 1;
        hi[0] = 1.03f; hi[1] = 1.0f; hi[2] = 0.95f;
    }
    hi[3] = bloom ? (0.35f + 0.55f * night) : 0.0f;
    c[20] = g_cfg.sharpen / 100.0f;
    g_dev->SetPixelShaderConstantF(0, c, 6);
    float rc2[4] = { rayUv[0], rayUv[1], rayK > 0.01f ? rayK : 0.0f, (float)g_width / g_height };
    g_dev->SetPixelShaderConstantF(6, rc2, 1);
    Sampler(4, rayK > 0.01f ? g_rayTex[1] : NULL, true);
    Sampler(0, src, false);
    for (int l = 0; l < 3; l++) Sampler(1 + l, bloom ? g_bloomTex[l][0] : NULL, true);
    g_dev->SetPixelShader(g_psFinal);
    PostQuadTo(bb, g_width, g_height);

    for (int s = 0; s < 5; s++) g_dev->SetTexture(s, NULL);
    g_dev->SetRenderTarget(0, oldRt);
    g_dev->SetDepthStencilSurface(oldDs);
    g_postState->Apply();
    SafeRelease(oldRt); SafeRelease(oldDs); bb->Release();
    static uint32_t lastLog;
    if (GetTickCount() - lastLog > 10000) {
        lastLog = GetTickCount();
        Log("rendu : post-traitement : SMAA %s, eclat %s, etalonnage %d, nettete %d%%, coucher %.2f, nuit %.2f", smaa ? "oui" : "non",
            bloom ? "oui" : "non", g_cfg.grade, g_cfg.sharpen, sunset, night);
    }
}

// Appele par le crochet de Render2dStuff (players.cpp, qui dessine aussi les pseudos) : avant toute l'interface.
void Gfx9BeforeHud() { PostProcess(); }

// ======================================================================= Particules douces
// Fumee, poussiere, eclaboussures, explosions (dessins de RenderEffects, appel 0x4A604F : sommets XYZ + couleur +
// texture, melange, sans ecriture de profondeur) : la ou la particule rejoint le decor, elle s'efface en douceur
// (profondeur de la scene deja calculee par Apply) au lieu de couper net en ligne droite contre le sol et les murs.
// Nos shaders sont poses juste avant le dessin du jeu et retires juste apres (Gfx9DrawDone).
static const char kSoftShaders[] = R"HLSL(
row_major float4x4 gMat : register(c0);
struct SIn { float4 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
struct SOut { float4 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; float w : TEXCOORD1; };
SOut VsSoft(SIn i) { SOut o; o.pos = mul(float4(i.pos.xyz, 1), gMat); o.col = i.col; o.uv = i.uv; o.w = o.pos.w; return o; }
sampler2D sPTex : register(s0);
sampler2D sPDepth : register(s1);
float4 gSoft : register(c0);     // 1/largeur, 1/hauteur, distance de fondu (m), echelle de profondeur
float4 gPFog : register(c1);     // debut, fin du brouillard, brouillard actif, melange additif
float4 gPFogCol : register(c2);
float4 PsSoft(SOut i, float2 vpos : VPOS) : COLOR {
  float4 c = tex2D(sPTex, i.uv) * i.col;
  float d = tex2Dlod(sPDepth, float4((vpos + 0.5) * gSoft.xy, 0, 0)).r;
  float sceneW = d >= 0.999 ? 1e6 : d * gSoft.w;
  c.a *= saturate((sceneW - i.w) / gSoft.z);
  if (gPFog.z > 0.5) {
    float f = saturate((gPFog.y - i.w) / max(gPFog.y - gPFog.x, 1));
    if (gPFog.w > 0.5) c.rgb *= f; else c.rgb = lerp(gPFogCol.rgb, c.rgb, f);
  }
  if (gPFog.w > 0.5) c.rgb *= c.a;   // additif (source x 1) : la transparence doit aussi eteindre la couleur
  return c;
}
)HLSL";
static IDirect3DVertexShader9 *g_vsSoft;
static IDirect3DPixelShader9 *g_psSoft;
static bool g_softTried, g_softBound;
static int g_softDraws;

static bool SoftReady()
{
    if (!g_softTried) {
        g_softTried = true;
        CreateShaders();
        std::string src(kSoftShaders);
        g_vsSoft = (IDirect3DVertexShader9 *)CompileFrom(src, "VsSoft", "vs_3_0");
        g_psSoft = (IDirect3DPixelShader9 *)CompileFrom(src, "PsSoft", "ps_3_0");
        Log("rendu : particules douces %s", g_vsSoft && g_psSoft ? "pretes" : "indisponibles");
    }
    return g_vsSoft && g_psSoft;
}

// Avant un dessin du jeu : particule candidate -> nos shaders.
static void MaybeBindSoft(DWORD fvf)
{
    if (!g_effectsPass || !g_cfg.softParticles || !g_depthReady || !g_screenDepth || fvf != (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)) return;
    DWORD blend = 0, zw = 0, destBlend = 0, fogOn = 0, fogCol = 0;
    g_dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    g_dev->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
    if (!blend || zw) return;
    IDirect3DBaseTexture9 *t = NULL;
    g_dev->GetTexture(0, &t);
    if (!t) return;
    t->Release();
    if (!SoftReady()) return;
    FpuGuard fpu;
    M4 w, v, p;
    g_dev->GetTransform(D3DTS_WORLD, (D3DMATRIX *)w.m);
    g_dev->GetTransform(D3DTS_VIEW, (D3DMATRIX *)v.m);
    g_dev->GetTransform(D3DTS_PROJECTION, (D3DMATRIX *)p.m);
    M4 wvp = Mul(Mul(w, v), p);
    g_dev->GetRenderState(D3DRS_DESTBLEND, &destBlend);
    g_dev->GetRenderState(D3DRS_FOGENABLE, &fogOn);
    g_dev->GetRenderState(D3DRS_FOGCOLOR, &fogCol);
    float fs = *(float *)0x978660, fe = *(float *)0x9B6A6C;   // brouillard du cycle du jour
    float c[3 * 4] = { 1.0f / g_width, 1.0f / g_height, 0.9f, 1.0f / kDepthScale,
                       fs, fe, fogOn ? 1.0f : 0.0f, destBlend == D3DBLEND_ONE ? 1.0f : 0.0f,
                       ((fogCol >> 16) & 255) / 255.0f, ((fogCol >> 8) & 255) / 255.0f, (fogCol & 255) / 255.0f, 1 };
    g_dev->SetVertexShaderConstantF(0, wvp.m, 4);
    g_dev->SetPixelShaderConstantF(0, c, 3);
    g_dev->SetTexture(1, g_screenDepth);
    g_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g_dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    g_dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g_dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g_dev->SetVertexShader(g_vsSoft);
    g_dev->SetPixelShader(g_psSoft);
    g_softBound = true;
    g_softDraws++;
}

void Gfx9DrawDone()
{
    RestoreWind();
    if (!g_softBound) return;
    g_softBound = false;
    g_dev->SetVertexShader(NULL);
    g_dev->SetPixelShader(NULL);
    g_dev->SetTexture(1, NULL);
}

static void __cdecl h_RenderEffects()
{
    g_effectsPass = true;
    ((void(__cdecl *)())0x4A6510)();
    g_effectsPass = false;
    static uint32_t lastLog;
    if (g_softDraws && GetTickCount() - lastLog > 10000) { lastLog = GetTickCount(); Log("rendu : %d dessins de particules adoucis", g_softDraws); }
    g_softDraws = 0;
}

// ======================================================================= Captures (tests : CaptureRendu=N)
// L'image du tampon arriere, enregistree en BMP dans captures\ du dossier du jeu : les captures de fenetre des
// instances de test (hors ecran) sortent noires.
static void SaveCapture(IDirect3DSurface9 *from, const char *tag)
{
    IDirect3DSurface9 *bb = from, *rt = NULL, *mem = NULL;
    if (!bb && FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    if (from) bb->AddRef();
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    if (SUCCEEDED(g_dev->CreateRenderTarget(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL)) &&
        SUCCEEDED(g_dev->StretchRect(bb, NULL, rt, NULL, D3DTEXF_NONE)) &&
        SUCCEEDED(g_dev->CreateOffscreenPlainSurface(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &mem, NULL)) &&
        SUCCEEDED(g_dev->GetRenderTargetData(rt, mem))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(mem->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
            char dir[MAX_PATH], path[MAX_PATH];
            wsprintfA(dir, "%scaptures", GameDir());
            CreateDirectoryA(dir, NULL);
            wsprintfA(path, "%s\\rendu-%d%s.bmp", dir, g_captureIndex, tag);
            FILE *f = fopen(path, "wb");
            if (f) {
                int rowBytes = d.Width * 3, pad = (4 - rowBytes % 4) % 4;
                BITMAPFILEHEADER fh = {};
                BITMAPINFOHEADER ih = {};
                ih.biSize = sizeof(ih); ih.biWidth = d.Width; ih.biHeight = d.Height; ih.biPlanes = 1; ih.biBitCount = 24;
                ih.biSizeImage = (rowBytes + pad) * d.Height;
                fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + ih.biSizeImage;
                fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
                BYTE *row = new BYTE[rowBytes + pad]();
                for (int y = (int)d.Height - 1; y >= 0; y--) {
                    const BYTE *src = (const BYTE *)lr.pBits + y * lr.Pitch;
                    for (UINT x = 0; x < d.Width; x++) { row[x * 3] = src[x * 4]; row[x * 3 + 1] = src[x * 4 + 1]; row[x * 3 + 2] = src[x * 4 + 2]; }
                    fwrite(row, rowBytes + pad, 1, f);
                }
                delete[] row;
                fclose(f);
                Log("rendu : capture %s", path);
            }
            mem->UnlockRect();
        }
    }
    if (mem) mem->Release();
    if (rt) rt->Release();
    bb->Release();
}

void Gfx9BeforePresent()
{
    static uint32_t last;
    if (g_captureStage == 1) {
        g_captureStage = 0;
        if (g_captureBefore) { SaveCapture(g_captureBefore, "-sans"); SafeRelease(g_captureBefore); }
        SaveCapture(NULL, "");
        g_captureIndex++;
    }
    if (g_cfg.captureSecs > 0 && (GameState() == GS_PLAYING || GameState() == GS_FRONTEND) && GetTickCount() - last > (uint32_t)g_cfg.captureSecs * 1000) {
        last = GetTickCount();
        g_captureStage = 1;   // l'image suivante : avant et apres les ombres
    }
    // Image suivante.
    ReleaseRecs();
    g_applied = false;
    g_after3d = 0;
    g_lightCount = 0;
    g_lampCount = 0;
    g_depthReady = false;
    {
        static uint32_t lastWindLog;
        if (g_windDraws && GetTickCount() - lastWindLog > 10000) {
            lastWindLog = GetTickCount();
            Log("rendu : %d dessins de vegetation au vent (vent %.2f, pluie %.2f, sol mouille %.2f)", g_windDraws, *(float *)0x97533C, *(float *)0x975340, *(float *)0x9B6A9C);
        }
        g_windDraws = 0;
    }
    g_waterDraws = 0;
    g_lightsLive = LightsWanted() && g_shadersOk && g_psLights;
    g_night = 1.0f - SkyLum();
    if (g_night < 0) g_night = 0;
    g_waterTexSet = false;
    memset(g_why, 0, sizeof(g_why));
    SafeRelease(g_backBuffer);
}

void InstallGfx9Hooks()
{
    static bool done;
    if (done) return;
    done = true;
    if (*(uint8_t *)0x4A6584 == 0xE8 && *(int32_t *)0x4A6585 == 0x4C9F40 - 0x4A6589) PatchCall(0x4A6584, (void *)h_RenderEverythingBarRoads);
    else Log("rendu : appel de RenderEverythingBarRoads introuvable (projeteurs hors champ coupes)");
    if (*(uint8_t *)0x4A6594 == 0xE8 && *(int32_t *)0x4A6595 == 0x5C1710 - 0x4A6599 &&
        *(uint8_t *)0x4A65AE == 0xE8 && *(int32_t *)0x4A65AF == 0x5BFF00 - 0x4A65B3) {
        PatchCall(0x4A6594, (void *)h_RenderWater);
        PatchCall(0x4A65AE, (void *)h_RenderTransparentWater);
    } else Log("rendu : appels de l'eau introuvables (eau moderne coupee)");
    static const uint8_t roneProl[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x08 };
    o_RenderOneNonRoad = (RenderOne_t)MakeDetour(0x4C9DA0, roneProl, sizeof(roneProl), (void *)h_RenderOneNonRoad);
    if (!o_RenderOneNonRoad) Log("rendu : RenderOneNonRoad introuvable (vegetation au vent coupee)");
    if (*(uint8_t *)0x4A604F == 0xE8 && *(int32_t *)0x4A6050 == 0x4A6510 - 0x4A6054) PatchCall(0x4A604F, (void *)h_RenderEffects);
    else Log("rendu : appel de RenderEffects introuvable (particules douces coupees)");
    if (*(uint8_t *)0x4A604A == 0xE8 && *(int32_t *)0x4A604B == 0x4A6570 - 0x4A604F) { PatchCall(0x4A604A, (void *)h_RenderScene); g_sceneHooked = true; }
    else Log("rendu : appel de RenderScene introuvable (ombres posees au premier dessin 2D)");
    static const uint8_t carLightPro[] = { 0xBA, 0xB8, 0x46, 0x7E, 0x00 };   // mov edx, 7E46B8h (TheCamera)
    o_StoreCarLight = (StoreCarLight_t)MakeDetour(0x56DCD0, carLightPro, sizeof(carLightPro), (void *)h_StoreCarLight);
    if (!o_StoreCarLight) Log("rendu : CShadows::StoreCarLightShadow introuvable (phares : tache du jeu gardee)");
    static const uint8_t addLightPro[] = { 0xD9, 0xEE, 0xD9, 0xEE, 0x83, 0xEC, 0x18 };   // fldz ; fldz ; sub esp, 18h
    o_AddLight = (AddLight_t)MakeDetour(0x567700, addLightPro, sizeof(addLightPro), (void *)h_AddLight);
    if (!o_AddLight) Log("rendu : CPointLights::AddLight introuvable (lumieres dynamiques coupees)");
    static const uint8_t staticShadowPro[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x10 };
    o_StoreStaticShadow = (StoreStaticShadow_t)MakeDetour(0x56E780, staticShadowPro, sizeof(staticShadowPro), (void *)h_StoreStaticShadow);
    static const uint8_t coronaTexPro[] = { 0x53, 0x56, 0x55, 0xBB, 0xB8, 0x46, 0x7E, 0x00 };
    o_RegisterCoronaTex = (RegisterCoronaTex_t)MakeDetour(0x542490, coronaTexPro, sizeof(coronaTexPro), (void *)h_RegisterCoronaTex);
    if (!o_StoreStaticShadow || !o_RegisterCoronaTex) Log("rendu : lampes du decor introuvables (elles n'eclairent pas)");
}
