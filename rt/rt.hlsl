// Rayons de vcrt64.exe (DXR, lib_6_3). Compile par build.cmd (dxc -T lib_6_3 -Fh rt\rt_dxil.h).
//
// Par pixel de l'image demandee : un rayon de camera trouve la surface (memes triangles et memes matrices que ce que
// le jeu a dessine), puis des rayons vers le disque du soleil donnent sa visibilite (ombres douces). Feuillages et
// grillages : test alpha de leur texture dans le "any hit".

struct Frame {
    row_major float4x4 invViewProj;   // clip -> monde (vecteurs lignes)
    float4 camPos;          // w : distance max des rayons de camera
    float4 camFwd;          // axe de vue (profondeur rendue au jeu)
    float4 sun;             // vers le soleil, w = force
    float4 params;          // x = demi-angle du soleil, y = rayons par pixel, z = numero d'image, w = rien
    uint4 size;             // largeur, hauteur, fonctions (bits), rien
};
ConstantBuffer<Frame> gFrame : register(b0);

struct InstInfo {
    uint idxOff, uvOff, posOff, tex;   // indices (uint), uv (float2), positions (float3) ; tex : case + 1 (0 : aucune)
    float alphaRef;
    uint flags;                        // 1 : test alpha, 0x100 : maillage de l'image (tampons "transients"), 0x200 : personnage
    uint pad0, pad1;
};

RaytracingAccelerationStructure gScene : register(t0);
StructuredBuffer<InstInfo> gInst : register(t1);
RWByteAddressBuffer gOut : register(u0);
Texture2D gTex[8192] : register(t0, space1);
ByteAddressBuffer gGeo[6] : register(t0, space2);   // 0-2 : indices, uv, positions permanents ; 3-5 : ceux de l'image
SamplerState gSamp : register(s0);

struct PrimaryPayload { float t; float3 n; uint flags; };
struct ShadowPayload { float vis; };

// ---------------------------------------------------------------- utilitaires
uint Hash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
float Rand(inout uint s) { s = Hash(s); return (s & 0xFFFFFF) / 16777216.0; }

uint3 TriIndices(InstInfo ii, uint prim) {
    uint b = (ii.flags & 0x100) ? 3 : 0;
    return gGeo[b].Load3((ii.idxOff + prim * 3) * 4);
}
float2 VertUv(InstInfo ii, uint v) {
    uint b = (ii.flags & 0x100) ? 4 : 1;
    return asfloat(gGeo[b].Load2((ii.uvOff + v) * 8));
}
float3 VertPos(InstInfo ii, uint v) {
    uint b = (ii.flags & 0x100) ? 5 : 2;
    return asfloat(gGeo[b].Load3((ii.posOff + v) * 12));
}

bool AlphaPass(float2 bary) {
    InstInfo ii = gInst[InstanceID()];
    if (!(ii.flags & 1) || ii.tex == 0) return true;
    uint3 t = TriIndices(ii, PrimitiveIndex());
    float3 w = float3(1 - bary.x - bary.y, bary.x, bary.y);
    float2 uv = VertUv(ii, t.x) * w.x + VertUv(ii, t.y) * w.y + VertUv(ii, t.z) * w.z;
    float a = gTex[NonUniformResourceIndex(ii.tex - 1)].SampleLevel(gSamp, uv, 0).a;
    return a >= ii.alphaRef;
}

// ---------------------------------------------------------------- rayons de camera
[shader("closesthit")]
void PrimaryHit(inout PrimaryPayload p, in BuiltInTriangleIntersectionAttributes a) {
    InstInfo ii = gInst[InstanceID()];
    uint3 t = TriIndices(ii, PrimitiveIndex());
    float3 p0 = VertPos(ii, t.x), p1 = VertPos(ii, t.y), p2 = VertPos(ii, t.z);
    float3 n = cross(p1 - p0, p2 - p0);
    n = mul(ObjectToWorld3x4(), float4(n, 0));
    float len = length(n);
    n = len > 1e-12 ? n / len : -WorldRayDirection();
    if (dot(n, WorldRayDirection()) > 0) n = -n;
    p.t = RayTCurrent();
    p.n = n;
    p.flags = ii.flags;
}
[shader("anyhit")]
void PrimaryAny(inout PrimaryPayload p, in BuiltInTriangleIntersectionAttributes a) {
    if (!AlphaPass(a.barycentrics)) IgnoreHit();
}
[shader("miss")]
void PrimaryMiss(inout PrimaryPayload p) { p.t = -1; }

// ---------------------------------------------------------------- rayons d'ombre
[shader("anyhit")]
void ShadowAny(inout ShadowPayload p, in BuiltInTriangleIntersectionAttributes a) {
    if (!AlphaPass(a.barycentrics)) IgnoreHit();
}
[shader("miss")]
void ShadowMiss(inout ShadowPayload p) { p.vis = 1; }

float3 ConeDir(float3 axis, float angle, inout uint seed) {
    float3 up = abs(axis.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tx = normalize(cross(up, axis)), ty = cross(axis, tx);
    float r = sqrt(Rand(seed)) * tan(angle), phi = Rand(seed) * 6.2831853;
    return normalize(axis + (tx * cos(phi) + ty * sin(phi)) * r);
}

[shader("raygeneration")]
void RayGen() {
    uint2 px = DispatchRaysIndex().xy;
    uint W = gFrame.size.x, H = gFrame.size.y;
    float2 uv = (px + 0.5) / float2(W, H);
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float4 a = mul(float4(ndc, 0, 1), gFrame.invViewProj);
    float4 b = mul(float4(ndc, 1, 1), gFrame.invViewProj);
    float3 o = a.xyz / a.w, e = b.xyz / b.w;

    RayDesc r;
    r.Origin = o;
    r.Direction = normalize(e - o);
    r.TMin = 0;
    r.TMax = gFrame.camPos.w;
    PrimaryPayload pp;
    pp.t = -1; pp.n = 0; pp.flags = 0;
    TraceRay(gScene, RAY_FLAG_NONE, 0xFF, 0, 2, 0, r, pp);

    float vis = 1, depth = 0;
    if (pp.t >= 0) {
        float3 P = r.Origin + r.Direction * pp.t;
        depth = dot(P - gFrame.camPos.xyz, gFrame.camFwd.xyz);
        float3 L = gFrame.sun.xyz;
        if ((gFrame.size.z & 1) && gFrame.sun.w > 0.001) {
            // Personnages : peu de triangles, lisses par l'eclairage du jeu. Leurs faces a l'oppose du soleil et leur
            // propre ombre les decoupaient en facettes noires : pas de test de face, et leurs rayons ignorent les
            // personnages (masque 1 : decor et vehicules).
            bool ped = (pp.flags & 0x200) != 0;
            float ndl = dot(pp.n, L);
            if (ndl <= 0 && !ped) vis = 0;
            else {
                uint seed = Hash(px.x * 1973 + px.y * 9277 + (uint)gFrame.params.z * 26699);
                uint n = max((uint)gFrame.params.y, 1);
                float3 origin = P + (ped ? L * 0.05 : pp.n * (0.01 + pp.t * 0.0015));
                float sum = 0;
                for (uint i = 0; i < n; i++) {
                    RayDesc s;
                    s.Origin = origin;
                    s.Direction = ConeDir(L, gFrame.params.x, seed);
                    s.TMin = 0.001;
                    s.TMax = 600;
                    ShadowPayload sp;
                    sp.vis = 0;
                    TraceRay(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, ped ? 0x01 : 0xFF, 1, 2, 1, s, sp);
                    sum += sp.vis;
                }
                vis = sum / n;
            }
        }
    }
    uint lo = f32tof16(vis) | (f32tof16(depth) << 16);
    gOut.Store2((px.y * W + px.x) * 8, uint2(lo, 0));
}
