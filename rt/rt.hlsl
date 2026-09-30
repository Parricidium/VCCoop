// Rayons de vcrt64.exe (DXR, lib_6_3). Compile par build.cmd (dxc -T lib_6_3 -Fh rt\rt_dxil.h).
//
// Par pixel de l'image demandee : un rayon de camera trouve la surface (memes triangles et memes matrices que ce que
// le jeu a dessine), puis :
//   - rayons vers le disque du soleil : visibilite (ombres douces) ;
//   - rayons courts dans l'hemisphere : occlusion ambiante ;
//   - un rayon diffus : lumiere renvoyee par le decor (lumiere indirecte, 1 rebond) ;
//   - un rayon miroir sur les carrosseries et les sols mouilles : reflet.
// Accumulation d'une image a l'autre (reprojection avec la camera precedente) : moins de bruit.
// Feuillages et grillages : test alpha de leur texture dans le "any hit".

struct Frame {
    row_major float4x4 invViewProj;   // clip -> monde (vecteurs lignes)
    row_major float4x4 prevViewProj;  // monde -> clip de l'image precedente
    float4 camPos;                    // w : distance max des rayons de camera
    float4 camFwd;                    // axe de vue ; w : sol mouille (0..1)
    float4 sun;                       // vers le soleil, w = force
    float4 sunColor;                  // w : portee de l'occlusion (m)
    float4 ambient;                   // w : 1 si l'historique est utilisable
    float4 skyTop, skyBottom;
    float4 params;                    // x = demi-angle du soleil, y = rayons par pixel, z = numero d'image, w = lissage (1 normal)
    uint4 size;                       // largeur, hauteur, fonctions (1 soleil, 2 occlusion, 4 reflets, 8 lumiere indirecte)
};
ConstantBuffer<Frame> gFrame : register(b0);

struct InstInfo {
    uint idxOff, uvOff, posOff, colOff;   // indices (uint), uv (float2), positions (float3), couleurs (D3DCOLOR)
    uint tex;                             // case + 1 (0 : aucune)
    float alphaRef;
    uint flags;                           // 1 : test alpha, 2 : vehicule, 0x100 : maillage de l'image, 0x200 : personnage
    uint tint;                            // couleur de la matiere (D3DCOLOR)
    uint nrmOff, pad0, pad1, pad2;        // normales des sommets (float3 ; 0,0,0 : aucune)
};

RaytracingAccelerationStructure gScene : register(t0);
StructuredBuffer<InstInfo> gInst : register(t1);
ByteAddressBuffer gHistIn : register(t2);
RWByteAddressBuffer gOut : register(u0);
RWByteAddressBuffer gHistOut : register(u1);
Texture2D gTex[8192] : register(t0, space1);
ByteAddressBuffer gGeo[10] : register(t0, space2);   // 0-4 : indices, uv, positions, couleurs, normales ; 5-9 : ceux de l'image
SamplerState gSamp : register(s0);

struct PrimaryPayload { float t; float3 n; float3 sn; uint flags; };   // n : face, sn : normale lissee
struct ShadowPayload { float vis; };
struct RadiancePayload { float3 color; float t; };

// ---------------------------------------------------------------- utilitaires
uint Hash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
float Rand(inout uint s) { s = Hash(s); return (s & 0xFFFFFF) / 16777216.0; }
float3 Unpack(uint c) { return float3((c >> 16) & 255, (c >> 8) & 255, c & 255) / 255.0; }   // D3DCOLOR -> rgb
uint PackBgra(float3 c, float a) {
    uint3 v = (uint3)(saturate(c) * 255 + 0.5);
    return v.z | (v.y << 8) | (v.x << 16) | ((uint)(saturate(a) * 255 + 0.5) << 24);   // A8R8G8B8 en memoire
}
uint4 HistWord(float vis, float ao, float depth, float3 gi, uint count) {
    return uint4(f32tof16(vis) | (f32tof16(ao) << 16), asuint(depth), f32tof16(gi.r) | (f32tof16(gi.g) << 16), f32tof16(gi.b) | (count << 16));
}

uint GeoBase(InstInfo ii) { return (ii.flags & 0x100) ? 5 : 0; }
uint3 TriIndices(InstInfo ii, uint prim) { return gGeo[GeoBase(ii)].Load3((ii.idxOff + prim * 3) * 4); }
float2 VertUv(InstInfo ii, uint v) { return asfloat(gGeo[GeoBase(ii) + 1].Load2((ii.uvOff + v) * 8)); }
float3 VertPos(InstInfo ii, uint v) { return asfloat(gGeo[GeoBase(ii) + 2].Load3((ii.posOff + v) * 12)); }
float3 VertCol(InstInfo ii, uint v) { return Unpack(gGeo[GeoBase(ii) + 3].Load((ii.colOff + v) * 4)); }
float3 VertNrm(InstInfo ii, uint v) { return asfloat(gGeo[GeoBase(ii) + 4].Load3((ii.nrmOff + v) * 12)); }

float2 HitUv(InstInfo ii, uint3 t, float2 bary) {
    float3 w = float3(1 - bary.x - bary.y, bary.x, bary.y);
    return VertUv(ii, t.x) * w.x + VertUv(ii, t.y) * w.y + VertUv(ii, t.z) * w.z;
}

bool AlphaPass(float2 bary) {
    InstInfo ii = gInst[InstanceID()];
    if (!(ii.flags & 1) || ii.tex == 0) return true;
    uint3 t = TriIndices(ii, PrimitiveIndex());
    float a = gTex[NonUniformResourceIndex(ii.tex - 1)].SampleLevel(gSamp, HitUv(ii, t, bary), 0).a;
    return a >= ii.alphaRef;
}

float3 GeomNormal(InstInfo ii, uint3 t) {
    float3 p0 = VertPos(ii, t.x), p1 = VertPos(ii, t.y), p2 = VertPos(ii, t.z);
    float3 n = mul(ObjectToWorld3x4(), float4(cross(p1 - p0, p2 - p0), 0));
    float len = length(n);
    n = len > 1e-12 ? n / len : -WorldRayDirection();
    return dot(n, WorldRayDirection()) > 0 ? -n : n;
}

// Normale lissee (sommets du jeu) ; sans normales : la face. Tournee comme la face (vers l'arrivee du rayon).
float3 SmoothNormal(InstInfo ii, uint3 t, float2 bary, float3 face) {
    float3 w = float3(1 - bary.x - bary.y, bary.x, bary.y);
    float3 n = VertNrm(ii, t.x) * w.x + VertNrm(ii, t.y) * w.y + VertNrm(ii, t.z) * w.z;
    if (dot(n, n) < 1e-8) return face;
    n = normalize(mul(ObjectToWorld3x4(), float4(n, 0)));
    return dot(n, face) < 0 ? -n : n;
}

float3 Sky(float3 d) { return lerp(gFrame.skyBottom.rgb, gFrame.skyTop.rgb, saturate(d.z * 1.6 + 0.05)); }

float3 ConeDir(float3 axis, float angle, inout uint seed) {
    float3 up = abs(axis.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tx = normalize(cross(up, axis)), ty = cross(axis, tx);
    float r = sqrt(Rand(seed)) * tan(angle), phi = Rand(seed) * 6.2831853;
    return normalize(axis + (tx * cos(phi) + ty * sin(phi)) * r);
}
float3 CosineDir(float3 n, inout uint seed) {
    float3 up = abs(n.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tx = normalize(cross(up, n)), ty = cross(n, tx);
    float r = sqrt(Rand(seed)), phi = Rand(seed) * 6.2831853;
    return normalize(tx * (r * cos(phi)) + ty * (r * sin(phi)) + n * sqrt(max(0, 1 - r * r)));
}

float ShadowRay(float3 o, float3 d, float tmax, uint mask) {
    RayDesc s;
    s.Origin = o; s.Direction = d; s.TMin = 0.001; s.TMax = tmax;
    ShadowPayload sp; sp.vis = 0;
    TraceRay(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, mask, 1, 3, 1, s, sp);
    return sp.vis;
}

// ---------------------------------------------------------------- rayons de camera
[shader("closesthit")]
void PrimaryHit(inout PrimaryPayload p, in BuiltInTriangleIntersectionAttributes a) {
    InstInfo ii = gInst[InstanceID()];
    uint3 t = TriIndices(ii, PrimitiveIndex());
    p.t = RayTCurrent();
    p.n = GeomNormal(ii, t);
    p.sn = SmoothNormal(ii, t, a.barycentrics, p.n);
    p.flags = ii.flags;
}
[shader("anyhit")]
void PrimaryAny(inout PrimaryPayload p, in BuiltInTriangleIntersectionAttributes a) {
    if (!AlphaPass(a.barycentrics)) IgnoreHit();
}
[shader("miss")]
void PrimaryMiss(inout PrimaryPayload p) { p.t = -1; }

// ---------------------------------------------------------------- rayons d'ombre (et d'occlusion)
[shader("anyhit")]
void ShadowAny(inout ShadowPayload p, in BuiltInTriangleIntersectionAttributes a) {
    if (!AlphaPass(a.barycentrics)) IgnoreHit();
}
[shader("miss")]
void ShadowMiss(inout ShadowPayload p) { p.vis = 1; }

// ---------------------------------------------------------------- rayons de couleur (reflets, lumiere renvoyee)
// Couleur d'un point du decor : texture x couleur des sommets (eclairage "cuit" du jeu) x matiere (peinture), eclairee
// par le soleil s'il le voit.
[shader("closesthit")]
void RadianceHit(inout RadiancePayload p, in BuiltInTriangleIntersectionAttributes a) {
    InstInfo ii = gInst[InstanceID()];
    uint3 t = TriIndices(ii, PrimitiveIndex());
    float3 w = float3(1 - a.barycentrics.x - a.barycentrics.y, a.barycentrics.x, a.barycentrics.y);
    float3 vcol = VertCol(ii, t.x) * w.x + VertCol(ii, t.y) * w.y + VertCol(ii, t.z) * w.z;
    float3 albedo = Unpack(ii.tint);
    if (ii.tex) albedo *= gTex[NonUniformResourceIndex(ii.tex - 1)].SampleLevel(gSamp, HitUv(ii, t, a.barycentrics), 0).rgb;
    float3 face = GeomNormal(ii, t);
    float3 n = SmoothNormal(ii, t, a.barycentrics, face);
    float3 P = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float sunK = gFrame.sun.w;
    float ndl = dot(n, gFrame.sun.xyz);
    float direct = 0;
    if (sunK > 0.01 && ndl > 0) direct = ndl * ShadowRay(P + face * 0.03, gFrame.sun.xyz, 400, 0xFF);
    float base = 0.35 + 0.65 * (1 - saturate(sunK));   // de nuit : l'eclairage cuit du jeu (neons, fenetres) seul
    p.color = albedo * (vcol * base + gFrame.ambient.rgb * 0.25 + gFrame.sunColor.rgb * sunK * direct * 0.9);
    p.t = RayTCurrent();
}
[shader("miss")]
void RadianceMiss(inout RadiancePayload p) { p.color = Sky(WorldRayDirection()); p.t = -1; }

float3 RadianceRay(float3 o, float3 d, float tmax, out float hitT) {
    RayDesc r;
    r.Origin = o; r.Direction = d; r.TMin = 0.002; r.TMax = tmax;
    RadiancePayload rp; rp.color = 0; rp.t = -1;
    TraceRay(gScene, RAY_FLAG_NONE, 0xFF, 2, 3, 2, r, rp);
    hitT = rp.t;
    return rp.color;
}

// ---------------------------------------------------------------- generation
[shader("raygeneration")]
void RayGen() {
    uint2 px = DispatchRaysIndex().xy;
    uint W = gFrame.size.x, H = gFrame.size.y, feat = gFrame.size.z;
    uint idx = px.y * W + px.x;
    uint plane1 = W * H * 8, plane2 = W * H * 12;
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
    pp.t = -1; pp.n = 0; pp.sn = 0; pp.flags = 0;
    TraceRay(gScene, RAY_FLAG_NONE, 0xFF, 0, 3, 0, r, pp);

    if (pp.t < 0) {   // ciel
        gOut.Store2(idx * 8, uint2(f32tof16(1.0), f32tof16(1.0)));
        gOut.Store(plane1 + idx * 4, 0);
        gOut.Store(plane2 + idx * 4, 0);
        gHistOut.Store4(idx * 16, uint4(0, 0, 0, 0));
        return;
    }
    float3 P = r.Origin + r.Direction * pp.t;
    float3 N = pp.n;
    float depth = dot(P - gFrame.camPos.xyz, gFrame.camFwd.xyz);
    bool ped = (pp.flags & 0x200) != 0, vehicle = (pp.flags & 2) != 0;
    uint seed = Hash(px.x * 1973 + px.y * 9277 + (uint)gFrame.params.z * 26699);
    float eps = 0.01 + pp.t * 0.0015;
    uint mask = ped ? 0x01 : 0xFF;   // personnages : ni auto-ombre ni auto-occlusion (facettes)

    // Soleil. Personnages : peu de triangles, lisses par l'eclairage du jeu : pas de test de face.
    float vis = 1;
    float3 L = gFrame.sun.xyz;
    if ((feat & 1) && gFrame.sun.w > 0.001) {
        float ndl = dot(N, L);
        if (ndl <= 0 && !ped) vis = 0;
        else {
            uint n = max((uint)gFrame.params.y, 1);
            float3 origin = P + (ped ? L * 0.05 : N * eps);
            float sum = 0;
            for (uint i = 0; i < n; i++) sum += ShadowRay(origin, ConeDir(L, gFrame.params.x, seed), 600, mask);
            vis = sum / n;
        }
    }

    // Occlusion ambiante : 4 rayons courts.
    float ao = 1;
    if (feat & 2) {
        float R = gFrame.sunColor.w, sum = 0;
        for (uint i = 0; i < 4; i++) sum += ShadowRay(P + N * eps, CosineDir(N, seed), R, mask);
        ao = sum / 4;
    }

    // Lumiere renvoyee par le decor (le ciel est deja la lumiere ambiante du jeu : un rayon qui s'echappe ne compte pas).
    // 2 rayons ; un impact tres lumineux (neon, enseigne) est plafonne : sinon des points brillants isoles scintillent
    // ("lucioles", JD 30/09 : facade de l'Ocean View la nuit couverte de grains).
    float3 gi = 0;
    if (feat & 8) {
        for (uint i = 0; i < 2; i++) {
            float ht;
            float3 c = RadianceRay(P + N * eps, CosineDir(N, seed), 80, ht);
            if (ht > 0) {
                float l = dot(c, float3(0.3, 0.59, 0.11));
                gi += l > 0.6 ? c * (0.6 / l) : c;
            }
        }
        gi *= 0.5;
    }

    // Accumulation : meme point dans l'image precedente (profondeur coherente) -> moyenne glissante.
    uint count = 1;
    if (gFrame.ambient.w > 0.5) {
        float4 pc = mul(float4(P, 1), gFrame.prevViewProj);
        float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
        if (pc.w > 0.05 && all(puv > 0) && all(puv < 1)) {
            uint2 q = min((uint2)(puv * float2(W, H)), uint2(W - 1, H - 1));
            uint4 h = gHistIn.Load4((q.y * W + q.x) * 16);
            uint hc = h.w >> 16;
            if (hc > 0 && abs(asfloat(h.y) - pc.w) < pc.w * 0.03 + 0.08) {
                float k = 1.0 / (hc + 1), hs = gFrame.params.w;   // lissage choisi (plus petit : plus long)
                vis = lerp(f16tof32(h.x), vis, max(k, (ped || vehicle ? 0.5 : 0.25) * hs));   // objets qui bougent : peu d'historique
                ao = lerp(f16tof32(h.x >> 16), ao, max(k, (ped || vehicle ? 0.25 : 0.08) * hs));
                gi = lerp(float3(f16tof32(h.z), f16tof32(h.z >> 16), f16tof32(h.w)), gi, max(k, (ped || vehicle ? 0.2 : 0.04) * hs));
                count = min(hc + 1, 64);
            }
        }
    }
    gHistOut.Store4(idx * 16, HistWord(vis, ao, depth, gi, count));

    // Reflets : carrosseries (vernis), sols mouilles sous la pluie.
    float3 refl = 0; float rw = 0;
    if (feat & 4) {
        float3 SN = pp.sn;
        float cosT = saturate(dot(-r.Direction, SN));
        float F = 0.04 + 0.96 * pow(1 - cosT, 5);
        if (vehicle) rw = 0.12 + 0.7 * F;
        else if (!ped && gFrame.camFwd.w > 0.01 && N.z > 0.85) rw = gFrame.camFwd.w * (0.06 + 0.85 * F);
        if (rw > 0.01) {
            float ht;
            float3 R = reflect(r.Direction, SN);
            if (dot(R, N) < 0.02) R = normalize(R + N * (0.02 - dot(R, N)));   // (normale lissee : ne pas repartir dans la surface)
            refl = RadianceRay(P + N * eps, R, 400, ht);
        }
    }

    gOut.Store2(idx * 8, uint2(f32tof16(vis) | (f32tof16(depth) << 16), f32tof16(ao)));
    gOut.Store(plane1 + idx * 4, PackBgra(refl, rw));
    gOut.Store(plane2 + idx * 4, PackBgra(gi / 4, 1));
}
