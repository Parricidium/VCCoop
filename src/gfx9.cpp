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

// ---- lumieres dynamiques : l'image est multipliee par (1 + lumiere recue)
float4 gLightPos[48] : register(c30);    // position + portee
float4 gLightCol[48] : register(c78);    // couleur + genre (1 : phare, en cone)
float4 gLightDir[48] : register(c126);   // direction + cosinus du cone
float4 gLightCount : register(c29);   // nombre, intensite, nombre de lumieres a ombre
row_major float4x4 gLightVP[4] : register(c174);   // lumieres a ombre : monde -> carte (perspective)
float4 gLShadow : register(c190);    // taille d'une case (texels), 1 / taille de l'atlas
sampler2D sLightAtlas : register(s2);

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
  float spot = b.w > 0.5 ? smoothstep(c.w, c.w + 0.15, dot(-L, c.xyz)) : 1;
  return b.rgb * att * ndl * spot;
}

float4 PsLights(float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
  if (d >= 0.999) return 0;
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
  return float4(sum * gLightCount.y, 1);
}

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
  col = lerp(col, sky, saturate(fres * 0.6));
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
)HLSL";

typedef HRESULT(WINAPI *D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
static D3DCompile_t g_compile;

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
static IDirect3DPixelShader9 *g_psDepth[2], *g_psMask, *g_psLights, *g_psWater;
static IDirect3DVertexShader9 *g_vsWater, *g_vsSpot[2];
static IDirect3DTexture9 *g_lightAtlas;
static IDirect3DSurface9 *g_lightAtlasSurf, *g_lightAtlasDs;
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
    g_psLights = (IDirect3DPixelShader9 *)CompileShader("PsLights", "ps_3_0", false);
    for (int v = 0; v < 2; v++) g_vsSpot[v] = (IDirect3DVertexShader9 *)CompileShader("VsSpot", "vs_3_0", v == 1);
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
    bool caster, water;   // projette une ombre ; surface de l'eau (dessinee par nous)
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
static bool RecordingWanted() { return ShadowsWanted() || LightsWanted() || WaterWanted(); }

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

static void __cdecl h_RenderEverythingBarRoads()
{
    ((void(__cdecl *)())0x4C9F40)();
    FpuGuard fpu;
    ExtraCasters();
}

void Gfx9SettingsChanged()
{
    if (!g_dev) return;
    ReleaseResources();
    g_resourcesFailed = false;
}


// ======================================================================= Appels du pont
void Gfx9DeviceCreated(IDirect3DDevice9 *dev, UINT width, UINT height, bool msaa)
{
    g_dev = dev; g_width = width; g_height = height; g_msaa = msaa;
    Log("rendu : Direct3D 9 actif (%ux%u%s)", width, height, msaa ? ", anticrenelage" : "");
    FpuGuard fpu;
    g_debugMask = GetPrivateProfileIntA("VCCoop", "OmbresDebug", 0, IniPath());
    if (g_cfg.sunShadows || g_cfg.modernWater || g_cfg.dynLights) CreateShaders();   // au lancement (pas au milieu d'une image de jeu)
}
void Gfx9BeforeReset() { SafeRelease(g_captureBefore); ReleaseRecs(); SafeRelease(g_backBuffer); ReleaseResources(); g_resourcesFailed = false; }
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
    r.receiver = !blend && zw && r.mainView && !g_bridgeCasterOnly;
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
    if (!g_haveMain && !g_bridgeCasterOnly) { memcpy(g_mainView, view, 64); memcpy(g_mainProj, proj, 64); g_haveMain = true; }
    r.mainView = memcmp(view, g_mainView, 64) == 0 && memcmp(proj, g_mainProj, 64) == 0;
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
struct DynLight { float x, y, z, dx, dy, dz, radius, r, g, b; int type; };
enum { MAX_LIGHTS = 48 };
static DynLight g_lightList[MAX_LIGHTS];
static int g_lightCount;
typedef void(__cdecl *AddLight_t)(int, float, float, float, float, float, float, float, float, float, float, int, int);
static AddLight_t o_AddLight;
static void __cdecl h_AddLight(int type, float x, float y, float z, float dx, float dy, float dz, float radius, float r, float g, float b, int fog, int extra)
{
    int t = type & 0xFF;
    if ((t == 0 || t == 1) && g_lightCount < MAX_LIGHTS && radius > 0.1f && r + g + b > 0.02f) {
        const float *cam = (const float *)0x7E46B8;   // TheCamera : position
        float ex = x - cam[0], ey = y - cam[1], ez = z - cam[2];
        if (ex * ex + ey * ey + ez * ez < 150.0f * 150.0f) g_lightList[g_lightCount++] = { x, y, z, dx, dy, dz, radius, r, g, b, t };
    }
    o_AddLight(type, x, y, z, dx, dy, dz, radius, r, g, b, fog, extra);
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

static void Apply()
{
    FpuGuard fpu;
    g_applied = true;
    UpdateSun();
    bool shadows = ShadowsWanted() && g_sunK > 0.01f;
    bool lights = LightsWanted() && g_lightCount > 0;
    if (g_recs.empty() || !g_haveMain || (!shadows && !lights) || !CreateResources()) return;
    if (lights && !g_psLights) lights = false;
    int receivers = 0;
    for (const Rec &r : g_recs) receivers += r.receiver;
    if (receivers < 20 || (!shadows && !lights)) return;

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

    // 1. Cascades.
    static const float splits[CASCADES] = { 12.0f, 35.0f, 90.0f, 220.0f };
    CascadeInfo casc[CASCADES] = {};
    int casters = 0;
    if (shadows) {
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
    if (shadows && g_debugMask != 2) {
        g_dev->SetPixelShader(g_psMask);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 16);
    }

    // 4. Lumieres dynamiques : image x (1 + lumiere recue), les plus proches avec leur ombre.
    if (lights) {
        float lc[4] = { (float)g_lightCount, 1.25f, (float)g_shadowLights, 0 };
        g_dev->SetPixelShaderConstantF(29, lc, 1);
        if (g_shadowLights) {
            g_dev->SetPixelShaderConstantF(174, g_lightVP[0].m, 4 * g_shadowLights);
            float ls[4] = { (float)LIGHT_TILE, 1.0f / (2 * LIGHT_TILE), 0, 0 };
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
            q = ldir + i * 4; q[0] = dir.x; q[1] = dir.y; q[2] = dir.z; q[3] = 0.55f;
        }
        g_dev->SetPixelShaderConstantF(30, lp, g_lightCount);
        g_dev->SetPixelShaderConstantF(78, lcol, g_lightCount);
        g_dev->SetPixelShaderConstantF(126, ldir, g_lightCount);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
        g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        if (g_debugMask == 2) g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);   // OmbresDebug=2 : lumiere recue seule
        g_dev->SetPixelShader(g_psLights);
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 16);
    }

    // Retour a l'etat du jeu.
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
        int dyn = 0;
        for (const Rec &r : g_recs) dyn += r.replayVb;
        Log("rendu : %d lumieres (%d avec ombre) ; %s ; ombres : %d projeteurs hors champ ; %d dessins (%d recepteurs, %d dynamiques), %d dans les cascades, soleil %.2f %.2f %.2f force %.2f, brouillard %.0f-%.0f ; %d dessins 3D apres le masque ; refus %d/%d/%d/%d/%d/%d/%d, UP 3D %d",
            lights ? g_lightCount : 0, g_shadowLights, g_moon ? "lune" : "soleil", g_extraCasters, (int)g_recs.size(), receivers, dyn, casters, g_sun.x, g_sun.y, g_sun.z, g_sunK, fogStart, farClip, g_after3d,
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

bool Gfx9Intercept(DWORD fvf, const GfxDraw &d, bool up)
{
    if (g_bridgeCasterOnly) { if (!up) Gfx9AfterDraw(fvf, d); return true; }
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
        r.receiver = r.mainView;
        g_recs.push_back(r);
        g_waterDraws++;
    }
    return true;
}

static float SkyChan(uintptr_t a) { int v = *(int *)a; return (v < 0 ? 0 : v > 255 ? 255 : v) / 255.0f; }

static void DrawModernWater()
{
    FpuGuard fpu;
    int n = 0;
    for (const Rec &r : g_recs) n += r.water;
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

    g_dev->SetRenderTarget(0, oldRt);
    g_dev->SetDepthStencilSurface(oldDs);
    D3DVIEWPORT9 full = { 0, 0, g_width, g_height, 0, 1 };
    g_dev->SetViewport(&full);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    g_dev->SetTexture(0, g_screenDepth);
    g_dev->SetTexture(1, g_refract);
    for (int s = 0; s < 2; s++) {
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
    g_dev->SetPixelShaderConstantF(0, pc, 10);
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
    g_dev->SetRenderTarget(0, oldRt);
    g_dev->SetDepthStencilSurface(oldDs);
    g_state->Apply();
    SafeRelease(oldRt);
    SafeRelease(oldDs);
    static uint32_t lastLog;
    if (GetTickCount() - lastLog > 10000) {
        lastLog = GetTickCount();
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
    // Premier dessin 2D apres la scene 3D (interface, halos) : les ombres se posent maintenant.
    if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW && g_recs.size() >= 20) Apply();
}

void Gfx9EndScene() { if (!g_applied && g_recs.size() >= 20) Apply(); }

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
    if (g_cfg.captureSecs > 0 && GameState() == GS_PLAYING && GetTickCount() - last > (uint32_t)g_cfg.captureSecs * 1000) {
        last = GetTickCount();
        g_captureStage = 1;   // l'image suivante : avant et apres les ombres
    }
    // Image suivante.
    ReleaseRecs();
    g_applied = false;
    g_after3d = 0;
    g_lightCount = 0;
    g_waterDraws = 0;
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
    static const uint8_t addLightPro[] = { 0xD9, 0xEE, 0xD9, 0xEE, 0x83, 0xEC, 0x18 };   // fldz ; fldz ; sub esp, 18h
    o_AddLight = (AddLight_t)MakeDetour(0x567700, addLightPro, sizeof(addLightPro), (void *)h_AddLight);
    if (!o_AddLight) Log("rendu : CPointLights::AddLight introuvable (lumieres dynamiques coupees)");
}
