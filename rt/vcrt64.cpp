// vcrt64.exe : ray tracing materiel (Direct3D 12 + DXR) pour VCCoop, a cote du jeu.
//
// Le pilote refuse DXR aux processus 32 bits (le jeu) : ce programme 64 bits recoit par memoire partagee les
// maillages, les textures et les dessins de chaque image (rtshared.h), trace les rayons (rt.hlsl) et renvoie une
// image. Lance par le jeu (rt.cpp) avec son numero de processus ; s'arrete avec lui.
//   vcrt64.exe <pid>     : service du jeu
//   vcrt64.exe --probe   : code de sortie 0 si une carte sait faire DXR (le lanceur grise sinon le choix)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <map>
#include <unordered_map>
#include <vector>
#include "rtshared.h"
#include "rt_dxil.h"
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// ---------------------------------------------------------------- journal
static FILE *g_log;
static void Log(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

static RtHeader *g_hdr;
static void Fatal(const char *fmt, ...)
{
    char b[256];
    va_list a; va_start(a, fmt); _vsnprintf_s(b, sizeof b, _TRUNCATE, fmt, a); va_end(a);
    Log("ECHEC : %s", b);
    if (g_hdr) { strncpy_s(g_hdr->error, b, _TRUNCATE); MemoryBarrier(); g_hdr->helperState = 2; }
    exit(1);
}
#define CK(x) do { HRESULT _h = (x); if (FAILED(_h)) Fatal("%s : %08lX (ligne %d)", #x, _h, __LINE__); } while (0)

// ---------------------------------------------------------------- allocateur (premier bloc libre, fusion)
struct FreeList {
    std::map<uint64_t, uint64_t> free;   // debut -> taille
    uint64_t total = 0;
    void Init(uint64_t size) { free.clear(); free[0] = size; total = size; }
    // Renvoie ~0 si plus de place.
    uint64_t Alloc(uint64_t size, uint64_t align)
    {
        for (auto it = free.begin(); it != free.end(); ++it) {
            uint64_t start = (it->first + align - 1) / align * align, end = it->first + it->second;
            if (start + size > end) continue;
            uint64_t b = it->first, e = end;
            free.erase(it);
            if (start > b) free[b] = start - b;
            if (start + size < e) free[start + size] = e - (start + size);
            return start;
        }
        return ~0ull;
    }
    void Free(uint64_t off, uint64_t size)
    {
        if (!size) return;
        auto it = free.emplace(off, size).first;
        auto next = std::next(it);
        if (next != free.end() && it->first + it->second == next->first) { it->second += next->second; free.erase(next); }
        if (it != free.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second == it->first) { prev->second += it->second; free.erase(it); }
        }
    }
};

// ---------------------------------------------------------------- Direct3D 12
static ID3D12Device5 *g_dev;
static ID3D12CommandQueue *g_queue;
static ID3D12CommandAllocator *g_alloc;
static ID3D12GraphicsCommandList4 *g_cl;
static ID3D12Fence *g_fence;
static UINT64 g_fenceValue;
static HANDLE g_fenceEvent;
static ID3D12RootSignature *g_rootSig;
static ID3D12StateObject *g_pso;
static ID3D12Resource *g_shaderTable;
static ID3D12DescriptorHeap *g_heap;
static UINT g_descSize;
static std::vector<IUnknown *> g_releaseAfterFrame;

static ID3D12Resource *Buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES st, bool uav = false)
{
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *r = NULL;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, IID_PPV_ARGS(&r));
    if (FAILED(hr)) { Log("tampon de %llu octets refuse (%08lX)", size, hr); return NULL; }
    return r;
}
static void *MapAll(ID3D12Resource *r) { void *p = NULL; D3D12_RANGE none = { 0, 0 }; r->Map(0, &none, &p); return p; }
static void Barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x = {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r; x.Transition.StateBefore = a; x.Transition.StateAfter = b;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_cl->ResourceBarrier(1, &x);
}
static void UavBarrier() { D3D12_RESOURCE_BARRIER x = {}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; g_cl->ResourceBarrier(1, &x); }

static void WaitGpu()
{
    g_queue->Signal(g_fence, ++g_fenceValue);
    if (g_fence->GetCompletedValue() < g_fenceValue) {
        g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 10000);
    }
    HRESULT r = g_dev->GetDeviceRemovedReason();
    if (FAILED(r)) Fatal("carte graphique perdue (%08lX)", r);
}

static IDXGIAdapter1 *PickAdapter(char *name, size_t nameLen)
{
    IDXGIFactory4 *fac = NULL;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) return NULL;
    IDXGIAdapter1 *best = NULL; SIZE_T bestMem = 0;
    IDXGIAdapter1 *a = NULL;
    for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        bool ok = false;
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            ID3D12Device5 *dev = NULL;
            if (SUCCEEDED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) {
                D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
                ok = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5)) && o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
                dev->Release();
            }
        }
        if (ok && d.DedicatedVideoMemory > bestMem) {
            if (best) best->Release();
            best = a; bestMem = d.DedicatedVideoMemory;
            if (name) { size_t n; wcstombs_s(&n, name, nameLen, d.Description, _TRUNCATE); }
        } else a->Release();
    }
    fac->Release();
    return best;
}

// ---------------------------------------------------------------- geometrie
// Arenes : indices (uint), uv (float2), positions (float3), couleurs (D3DCOLOR), permanentes et "de l'image"
// (maillages transients).
enum { GEO_IDX, GEO_UV, GEO_POS, GEO_COL, GEO_NRM, GEO_COUNT };
static const UINT kGeoStride[GEO_COUNT] = { 4, 8, 12, 4, 12 };
static const UINT64 kGeoSize[GEO_COUNT] = { 96ull << 20, 64ull << 20, 96ull << 20, 32ull << 20, 96ull << 20 };
static const UINT64 kTGeoSize[GEO_COUNT] = { 16ull << 20, 12ull << 20, 16ull << 20, 6ull << 20, 16ull << 20 };
static ID3D12Resource *g_geo[GEO_COUNT], *g_tgeo[GEO_COUNT];
static FreeList g_geoFree[GEO_COUNT];
static UINT64 g_tgeoUsed[GEO_COUNT];
static const UINT64 kBlasSize = 768ull << 20, kTBlasSize = 128ull << 20;
static ID3D12Resource *g_blas, *g_tblas;
static FreeList g_blasFree;
static UINT64 g_tblasUsed;
static ID3D12Resource *g_scratch;
static UINT64 g_scratchSize;

struct Mesh {
    bool transient;
    UINT vertices, triangles;
    UINT64 off[GEO_COUNT];   // en elements
    UINT64 blasOff, blasSize;
    bool built;
};
static std::unordered_map<uint32_t, Mesh> g_meshes;

struct PendingBuild { uint32_t id; D3D12_RAYTRACING_GEOMETRY_DESC geom; UINT64 scratch; bool transient; };
static std::vector<PendingBuild> g_builds;

static void FreeMesh(Mesh &m)
{
    if (m.transient) return;
    for (int g = 0; g < GEO_COUNT; g++) g_geoFree[g].Free(m.off[g] * kGeoStride[g], (UINT64)(g == GEO_IDX ? m.triangles * 3 : m.vertices) * kGeoStride[g]);
    if (m.blasSize) g_blasFree.Free(m.blasOff, m.blasSize);
}

// ---------------------------------------------------------------- textures
enum { MAX_TEX = 8192, HEAP_GEO = MAX_TEX, HEAP_SIZE = MAX_TEX + 2 * GEO_COUNT };
struct Tex { ID3D12Resource *res; UINT slot; };
static std::unordered_map<uint32_t, Tex> g_texs;
static std::vector<UINT> g_freeSlots;

static void NullTexSrv(UINT slot)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; d.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)slot * g_descSize;
    g_dev->CreateShaderResourceView(NULL, &d, h);
}
static void RawSrv(UINT slot, ID3D12Resource *r, UINT64 bytes)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_TYPELESS; d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.NumElements = (UINT)(bytes / 4); d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)slot * g_descSize;
    g_dev->CreateShaderResourceView(r, &d, h);
}

// ---------------------------------------------------------------- tampons de l'image
static const UINT64 kStagingSize = 96ull << 20;
static ID3D12Resource *g_staging; static BYTE *g_stagingPtr; static UINT64 g_stagingUsed;
enum { MAX_INST = 32768 };
static ID3D12Resource *g_instDescs, *g_instInfo, *g_consts;
static D3D12_RAYTRACING_INSTANCE_DESC *g_instDescPtr;
struct InstInfo { uint32_t idxOff, uvOff, posOff, colOff, tex; float alphaRef; uint32_t flags, tint, nrmOff, pad[3]; };   // (rt.hlsl)
static InstInfo *g_instInfoPtr;
static BYTE *g_constsPtr;
static ID3D12Resource *g_tlas, *g_tlasScratch; static UINT64 g_tlasSize, g_tlasScratchSize;
static ID3D12Resource *g_out, *g_readback; static UINT64 g_outSize;
// Historique (accumulation d'une image a l'autre) : deux tampons, l'un lu (image precedente), l'autre ecrit.
static ID3D12Resource *g_hist[2]; static UINT64 g_histSize; static int g_histCur; static bool g_histValid;
static float g_prevVP[16];

static UINT64 Stage(const void *data, UINT64 bytes, UINT64 align)
{
    UINT64 off = (g_stagingUsed + align - 1) / align * align;
    if (off + bytes > kStagingSize) return ~0ull;
    if (data) memcpy(g_stagingPtr + off, data, (size_t)bytes);
    g_stagingUsed = off + bytes;
    return off;
}

static void Replace(ID3D12Resource *&r, UINT64 &cur, UINT64 need, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES st, bool uav)
{
    if (r && cur >= need) return;
    if (r) g_releaseAfterFrame.push_back(r);
    cur = need + need / 2 + (1 << 20);
    r = Buffer(cur, heap, st, uav);
    if (!r) Fatal("memoire video insuffisante (%llu Mo)", cur >> 20);
}

// ---------------------------------------------------------------- initialisation
static void CreatePipeline()
{
    D3D12_ROOT_PARAMETER rp[8] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[0].Descriptor.ShaderRegister = 0;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[1].Descriptor.ShaderRegister = 1;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[2].Descriptor.ShaderRegister = 0;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[3].Descriptor.ShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE r4 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MAX_TEX, 0, 1, 0 };
    D3D12_DESCRIPTOR_RANGE r5 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2 * GEO_COUNT, 0, 2, 0 };
    rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[4].DescriptorTable.NumDescriptorRanges = 1; rp[4].DescriptorTable.pDescriptorRanges = &r4;
    rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[5].DescriptorTable.NumDescriptorRanges = 1; rp[5].DescriptorTable.pDescriptorRanges = &r5;
    rp[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[6].Descriptor.ShaderRegister = 2;
    rp[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[7].Descriptor.ShaderRegister = 1;
    for (auto &p : rp) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    ss.MaxLOD = D3D12_FLOAT32_MAX; ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 8, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *sig = NULL, *err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        Fatal("signature racine : %s", err ? (const char *)err->GetBufferPointer() : "?");
    CK(g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_rootSig)));
    sig->Release();

    D3D12_DXIL_LIBRARY_DESC lib = {};
    lib.DXILLibrary.pShaderBytecode = g_rtDxil; lib.DXILLibrary.BytecodeLength = sizeof(g_rtDxil);
    D3D12_HIT_GROUP_DESC hg0 = {}, hg1 = {}, hg2 = {};
    hg0.HitGroupExport = L"HgPrimary"; hg0.ClosestHitShaderImport = L"PrimaryHit"; hg0.AnyHitShaderImport = L"PrimaryAny"; hg0.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hg1.HitGroupExport = L"HgShadow"; hg1.AnyHitShaderImport = L"ShadowAny"; hg1.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hg2.HitGroupExport = L"HgRadiance"; hg2.ClosestHitShaderImport = L"RadianceHit"; hg2.AnyHitShaderImport = L"PrimaryAny"; hg2.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    D3D12_RAYTRACING_SHADER_CONFIG sc = { 32, 8 };   // (charge la plus grosse : rayon de camera, 32 octets)
    D3D12_RAYTRACING_PIPELINE_CONFIG pc = { 2 };
    D3D12_GLOBAL_ROOT_SIGNATURE gr = { g_rootSig };
    D3D12_STATE_SUBOBJECT so[] = {
        { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib }, { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg0 }, { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg1 },
        { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg2 },
        { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc }, { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc },
        { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gr } };
    D3D12_STATE_OBJECT_DESC sod = { D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)(sizeof so / sizeof so[0]), so };
    CK(g_dev->CreateStateObject(&sod, IID_PPV_ARGS(&g_pso)));
    ID3D12StateObjectProperties *props = NULL;
    CK(g_pso->QueryInterface(IID_PPV_ARGS(&props)));
    // Table : generation (0), echecs (64 : camera, 96 : ombre, 128 : couleur), impacts (192, 224, 256).
    BYTE table[320] = {};
    const wchar_t *names[7] = { L"RayGen", L"PrimaryMiss", L"ShadowMiss", L"RadianceMiss", L"HgPrimary", L"HgShadow", L"HgRadiance" };
    const UINT offs[7] = { 0, 64, 96, 128, 192, 224, 256 };
    for (int i = 0; i < 7; i++) {
        void *id = props->GetShaderIdentifier(names[i]);
        if (!id) Fatal("shader %ls introuvable", names[i]);
        memcpy(table + offs[i], id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    }
    props->Release();
    g_shaderTable = Buffer(sizeof table, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    memcpy(MapAll(g_shaderTable), table, sizeof table);
}

static void CreateResources()
{
    for (int g = 0; g < GEO_COUNT; g++) {
        g_geo[g] = Buffer(kGeoSize[g], D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_tgeo[g] = Buffer(kTGeoSize[g], D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!g_geo[g] || !g_tgeo[g]) Fatal("memoire video insuffisante (geometrie)");
        g_geoFree[g].Init(kGeoSize[g]);
    }
    g_blas = Buffer(kBlasSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
    g_tblas = Buffer(kTBlasSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
    if (!g_blas || !g_tblas) Fatal("memoire video insuffisante (structures d'acceleration)");
    g_blasFree.Init(kBlasSize);
    g_staging = Buffer(kStagingSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    g_instDescs = Buffer(MAX_INST * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    g_instInfo = Buffer(MAX_INST * sizeof(InstInfo), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    g_consts = Buffer(512, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!g_staging || !g_instDescs || !g_instInfo || !g_consts) Fatal("memoire insuffisante (tampons d'envoi)");
    g_stagingPtr = (BYTE *)MapAll(g_staging);
    g_instDescPtr = (D3D12_RAYTRACING_INSTANCE_DESC *)MapAll(g_instDescs);
    g_instInfoPtr = (InstInfo *)MapAll(g_instInfo);
    g_constsPtr = (BYTE *)MapAll(g_consts);

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, HEAP_SIZE, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    CK(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)));
    g_descSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = 0; i < MAX_TEX; i++) { NullTexSrv(i); g_freeSlots.push_back(MAX_TEX - 1 - i); }
    for (int g = 0; g < GEO_COUNT; g++) { RawSrv(HEAP_GEO + g, g_geo[g], kGeoSize[g]); RawSrv(HEAP_GEO + GEO_COUNT + g, g_tgeo[g], kTGeoSize[g]); }
}

// ---------------------------------------------------------------- commandes
static bool g_geoCopyState;   // arenes en COPY_DEST
static void GeoToCopy()
{
    if (g_geoCopyState) return;
    g_geoCopyState = true;
    for (int g = 0; g < GEO_COUNT; g++) { Barrier(g_geo[g], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST); Barrier(g_tgeo[g], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST); }
}
static void GeoToRead()
{
    if (!g_geoCopyState) return;
    g_geoCopyState = false;
    for (int g = 0; g < GEO_COUNT; g++) { Barrier(g_geo[g], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); Barrier(g_tgeo[g], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); }
}

static int g_meshRefused;
static void CmdMesh(const BYTE *p, uint32_t bytes)
{
    const RtMesh *h = (const RtMesh *)p;
    if (bytes < sizeof(RtMesh)) return;
    UINT64 need = sizeof(RtMesh) + (UINT64)h->vertices * RT_VERTEX_BYTES + (UINT64)h->triangles * 12;
    if (need > bytes || !h->vertices || !h->triangles) return;
    auto old = g_meshes.find(h->id);
    if (old != g_meshes.end()) { FreeMesh(old->second); g_meshes.erase(old); }
    const BYTE *pos = p + sizeof(RtMesh), *nrm = pos + h->vertices * 12, *uv = nrm + h->vertices * 12, *col = uv + h->vertices * 8, *idx = col + h->vertices * 4;
    Mesh m = {};
    m.transient = (h->flags & RT_MESH_TRANSIENT) != 0;
    m.vertices = h->vertices; m.triangles = h->triangles;
    UINT64 sizes[GEO_COUNT] = { (UINT64)h->triangles * 12, (UINT64)h->vertices * 8, (UINT64)h->vertices * 12, (UINT64)h->vertices * 4, (UINT64)h->vertices * 12 };
    const BYTE *srcs[GEO_COUNT] = { idx, uv, pos, col, nrm };
    UINT64 byteOff[GEO_COUNT];
    for (int g = 0; g < GEO_COUNT; g++) {
        if (m.transient) {
            byteOff[g] = (g_tgeoUsed[g] + 47) / 48 * 48;   // multiple de 16 (copie) et de la taille d'un element (12, 8, 4)
            if (byteOff[g] + sizes[g] > kTGeoSize[g]) { g_meshRefused++; return; }
            g_tgeoUsed[g] = byteOff[g] + sizes[g];
        } else {
            byteOff[g] = g_geoFree[g].Alloc(sizes[g], 48);
            if (byteOff[g] == ~0ull) {
                for (int k = 0; k < g; k++) g_geoFree[k].Free(byteOff[k], sizes[k]);
                g_meshRefused++; return;
            }
        }
    }
    GeoToCopy();
    for (int g = 0; g < GEO_COUNT; g++) {
        UINT64 s = Stage(srcs[g], sizes[g], 16);
        if (s == ~0ull) { if (!m.transient) for (int k = 0; k < GEO_COUNT; k++) g_geoFree[k].Free(byteOff[k], sizes[k]); g_meshRefused++; return; }
        g_cl->CopyBufferRegion(m.transient ? g_tgeo[g] : g_geo[g], byteOff[g], g_staging, s, sizes[g]);
        m.off[g] = byteOff[g] / kGeoStride[g];
    }
    // Structure d'acceleration : taille connue tout de suite, construite apres les copies.
    PendingBuild b = {};
    b.id = h->id; b.transient = m.transient;
    D3D12_RAYTRACING_GEOMETRY_DESC &gd = b.geom;
    gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    gd.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    ID3D12Resource *pa = m.transient ? g_tgeo[GEO_POS] : g_geo[GEO_POS], *ia = m.transient ? g_tgeo[GEO_IDX] : g_geo[GEO_IDX];
    gd.Triangles.VertexBuffer.StartAddress = pa->GetGPUVirtualAddress() + byteOff[GEO_POS];
    gd.Triangles.VertexBuffer.StrideInBytes = 12;
    gd.Triangles.VertexCount = m.vertices;
    gd.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    gd.Triangles.IndexBuffer = ia->GetGPUVirtualAddress() + byteOff[GEO_IDX];
    gd.Triangles.IndexCount = m.triangles * 3;
    gd.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = m.transient ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; in.NumDescs = 1; in.pGeometryDescs = &gd;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    UINT64 size = (info.ResultDataMaxSizeInBytes + 255) / 256 * 256;
    if (m.transient) {
        m.blasOff = (g_tblasUsed + 255) / 256 * 256;
        if (m.blasOff + size > kTBlasSize) { g_meshRefused++; return; }
        g_tblasUsed = m.blasOff + size;
    } else {
        m.blasOff = g_blasFree.Alloc(size, 256);
        if (m.blasOff == ~0ull) { FreeMesh(m); g_meshRefused++; return; }
    }
    m.blasSize = m.transient ? 0 : size;
    b.scratch = (info.ScratchDataSizeInBytes + 255) / 256 * 256;
    g_meshes[h->id] = m;
    g_builds.push_back(b);
}

static void CmdMeshDel(uint32_t id)
{
    auto it = g_meshes.find(id);
    if (it == g_meshes.end()) return;
    FreeMesh(it->second);
    g_meshes.erase(it);
}

static void CmdTex(const BYTE *p, uint32_t bytes)
{
    const RtTex *h = (const RtTex *)p;
    if (bytes < sizeof(RtTex) || !h->width || !h->height || h->width > 4096 || h->height > 4096) return;
    DXGI_FORMAT fmt = h->format == RT_TEX_BC1 ? DXGI_FORMAT_BC1_UNORM : h->format == RT_TEX_BC2 ? DXGI_FORMAT_BC2_UNORM :
                      h->format == RT_TEX_BC3 ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    bool bc = h->format != RT_TEX_BGRA8;
    if (bc && ((h->width & 3) || (h->height & 3))) return;
    UINT rows = bc ? h->height / 4 : h->height;
    UINT rowBytes = bc ? (h->width / 4) * (h->format == RT_TEX_BC1 ? 8 : 16) : h->width * 4;
    if (sizeof(RtTex) + (UINT64)rows * rowBytes > bytes) return;
    auto old = g_texs.find(h->id);
    if (old != g_texs.end()) { g_releaseAfterFrame.push_back(old->second.res); NullTexSrv(old->second.slot); g_freeSlots.push_back(old->second.slot); g_texs.erase(old); }
    if (g_freeSlots.empty()) return;

    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = h->width; d.Height = h->height; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT nRows; UINT64 rowSize, total;
    g_dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &nRows, &rowSize, &total);
    UINT64 s = Stage(NULL, total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (s == ~0ull) return;
    const BYTE *src = p + sizeof(RtTex);
    for (UINT y = 0; y < rows; y++) memcpy(g_stagingPtr + s + (UINT64)y * fp.Footprint.RowPitch, src + (UINT64)y * rowBytes, rowBytes);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    ID3D12Resource *t = NULL;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&t)))) return;
    fp.Offset = s;
    D3D12_TEXTURE_COPY_LOCATION dst = {}, sl = {};
    dst.pResource = t; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    sl.pResource = g_staging; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; sl.PlacedFootprint = fp;
    g_cl->CopyTextureRegion(&dst, 0, 0, 0, &sl, NULL);
    Barrier(t, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    UINT slot = g_freeSlots.back(); g_freeSlots.pop_back();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = fmt; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE hh = g_heap->GetCPUDescriptorHandleForHeapStart(); hh.ptr += (SIZE_T)slot * g_descSize;
    g_dev->CreateShaderResourceView(t, &sv, hh);
    g_texs[h->id] = { t, slot };
}

static void CmdTexDel(uint32_t id)
{
    auto it = g_texs.find(id);
    if (it == g_texs.end()) return;
    g_releaseAfterFrame.push_back(it->second.res);
    NullTexSrv(it->second.slot);
    g_freeSlots.push_back(it->second.slot);
    g_texs.erase(it);
}

static void BuildPending()
{
    if (g_builds.empty()) return;
    UINT64 need = 0;
    for (auto &b : g_builds) need += b.scratch;
    if (need > (512ull << 20)) need = 512ull << 20;   // au-dela : constructions en plusieurs fois dans l'image
    Replace(g_scratch, g_scratchSize, need, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    UINT64 used = 0;
    for (auto &b : g_builds) {
        auto it = g_meshes.find(b.id);
        if (it == g_meshes.end()) continue;
        Mesh &m = it->second;
        if (b.scratch > g_scratchSize) continue;
        if (used + b.scratch > g_scratchSize) { UavBarrier(); used = 0; }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
        bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bd.Inputs.Flags = m.transient ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; bd.Inputs.NumDescs = 1; bd.Inputs.pGeometryDescs = &b.geom;
        bd.DestAccelerationStructureData = (m.transient ? g_tblas : g_blas)->GetGPUVirtualAddress() + m.blasOff;
        bd.ScratchAccelerationStructureData = g_scratch->GetGPUVirtualAddress() + used;
        g_cl->BuildRaytracingAccelerationStructure(&bd, 0, NULL);
        used += b.scratch;
        m.built = true;
    }
    g_builds.clear();
    UavBarrier();
}

// ---------------------------------------------------------------- image
static void Render(const RtFrame &f, const RtInstance *inst)
{
    BuildPending();

    // Dessins -> instances (maillages absents : pas encore envoyes, ou refuses faute de place).
    UINT n = 0;
    for (UINT i = 0; i < f.instanceCount && n < MAX_INST; i++) {
        const RtInstance &s = inst[i];
        auto it = g_meshes.find(s.mesh);
        if (it == g_meshes.end() || !it->second.built) continue;
        const Mesh &m = it->second;
        D3D12_RAYTRACING_INSTANCE_DESC &d = g_instDescPtr[n];
        memcpy(d.Transform, s.transform, sizeof d.Transform);
        bool ped = (s.flags & RT_INST_DYNAMIC) && !(s.flags & RT_INST_VEHICLE);
        d.InstanceID = n; d.InstanceMask = ped ? 0x02 : 0x01; d.InstanceContributionToHitGroupIndex = 0;
        auto tx = s.tex ? g_texs.find(s.tex) : g_texs.end();
        bool alpha = (s.flags & RT_INST_ALPHA) && tx != g_texs.end();
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | (alpha ? 0 : D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE);
        d.AccelerationStructure = (m.transient ? g_tblas : g_blas)->GetGPUVirtualAddress() + m.blasOff;
        InstInfo &ii = g_instInfoPtr[n];
        ii.idxOff = (uint32_t)m.off[GEO_IDX]; ii.uvOff = (uint32_t)m.off[GEO_UV]; ii.posOff = (uint32_t)m.off[GEO_POS]; ii.colOff = (uint32_t)m.off[GEO_COL];
        ii.tex = tx != g_texs.end() ? tx->second.slot + 1 : 0;
        ii.alphaRef = s.alphaRef;
        ii.flags = (alpha ? 1 : 0) | ((s.flags & RT_INST_VEHICLE) ? 2 : 0) | (m.transient ? 0x100 : 0) | (ped ? 0x200 : 0);
        ii.tint = s.tint;
        ii.nrmOff = (uint32_t)m.off[GEO_NRM];
        n++;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti = {};
    ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    ti.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; ti.NumDescs = n; ti.InstanceDescs = g_instDescs->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tp = {};
    g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&ti, &tp);
    Replace(g_tlas, g_tlasSize, tp.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
    Replace(g_tlasScratch, g_tlasScratchSize, tp.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = {};
    td.Inputs = ti; td.DestAccelerationStructureData = g_tlas->GetGPUVirtualAddress(); td.ScratchAccelerationStructureData = g_tlasScratch->GetGPUVirtualAddress();
    g_cl->BuildRaytracingAccelerationStructure(&td, 0, NULL);
    UavBarrier();

    // Constantes (voir Frame dans rt.hlsl).
    float vp[16], inv[16];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { float s = 0; for (int k = 0; k < 4; k++) s += f.view[r * 4 + k] * f.proj[k * 4 + c]; vp[r * 4 + c] = s; }
    {   // inversion 4x4 (Gauss-Jordan)
        double a[4][8];
        for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? vp[r * 4 + c] : (c - 4 == r ? 1 : 0);
        for (int c = 0; c < 4; c++) {
            int p = c; for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
            if (p != c) for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
            double d = a[c][c]; if (fabs(d) < 1e-20) d = 1e-20;
            for (int k = 0; k < 8; k++) a[c][k] /= d;
            for (int r = 0; r < 4; r++) if (r != c) { double m = a[r][c]; for (int k = 0; k < 8; k++) a[r][k] -= m * a[c][k]; }
        }
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) inv[r * 4 + c] = (float)a[r][c + 4];
    }
    // Camera : inverse de la vue (lignes : droite, haut, avant, position).
    float iv[16];
    {
        double a[4][8];
        for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? f.view[r * 4 + c] : (c - 4 == r ? 1 : 0);
        for (int c = 0; c < 4; c++) {
            int p = c; for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
            if (p != c) for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
            double d = a[c][c]; if (fabs(d) < 1e-20) d = 1e-20;
            for (int k = 0; k < 8; k++) a[c][k] /= d;
            for (int r = 0; r < 4; r++) if (r != c) { double m = a[r][c]; for (int k = 0; k < 8; k++) a[r][k] -= m * a[c][k]; }
        }
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) iv[r * 4 + c] = (float)a[r][c + 4];
    }
    // Historique : meme taille d'image, pas de coupure demandee par le jeu.
    UINT64 histBytes = (UINT64)f.outW * f.outH * 16;
    if (!g_hist[0] || g_histSize != histBytes) {
        for (auto &h : g_hist) { if (h) g_releaseAfterFrame.push_back(h); h = Buffer(histBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true); if (!h) Fatal("memoire insuffisante (historique)"); }
        g_histSize = histBytes; g_histValid = false;
    }
    bool histOk = g_histValid && !f.reset;

    struct Consts { float invVP[16], prevVP[16]; float camPos[4], camFwd[4], sun[4], sunColor[4], ambient[4], skyTop[4], skyBottom[4], params[4]; uint32_t size[4]; } c = {};
    static_assert(sizeof(Consts) <= 512, "constantes");
    memcpy(c.invVP, inv, 64);
    memcpy(c.prevVP, g_prevVP, 64);
    c.camPos[0] = iv[12]; c.camPos[1] = iv[13]; c.camPos[2] = iv[14]; c.camPos[3] = f.maxDistance;
    float fl = sqrtf(iv[8] * iv[8] + iv[9] * iv[9] + iv[10] * iv[10]); if (fl < 1e-6f) fl = 1;
    c.camFwd[0] = iv[8] / fl; c.camFwd[1] = iv[9] / fl; c.camFwd[2] = iv[10] / fl; c.camFwd[3] = f.wetness;
    memcpy(c.sun, f.sun, 16);
    memcpy(c.sunColor, f.sunColor, 12); c.sunColor[3] = f.aoRadius;
    memcpy(c.ambient, f.ambient, 12); c.ambient[3] = histOk ? 1.0f : 0.0f;
    memcpy(c.skyTop, f.skyTop, 16); memcpy(c.skyBottom, f.skyBottom, 16);
    c.params[0] = f.sunAngle; c.params[1] = (float)f.raysPerPixel; c.params[2] = (float)(f.frameIndex & 0xFFFF);
    c.size[0] = f.outW; c.size[1] = f.outH; c.size[2] = f.features;
    memcpy(g_constsPtr, &c, sizeof c);
    memcpy(g_prevVP, vp, 64);
    g_histValid = true;

    UINT64 outBytes = (UINT64)f.outW * f.outH * RT_OUT_BPP;
    if (!g_out || g_outSize < outBytes) {
        if (g_out) { g_releaseAfterFrame.push_back(g_out); g_releaseAfterFrame.push_back(g_readback); }
        g_outSize = outBytes;
        g_out = Buffer(outBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        g_readback = Buffer(outBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!g_out || !g_readback) Fatal("memoire insuffisante (image %ux%u)", f.outW, f.outH);
    }

    ID3D12DescriptorHeap *heaps[] = { g_heap };
    g_cl->SetDescriptorHeaps(1, heaps);
    g_cl->SetComputeRootSignature(g_rootSig);
    g_cl->SetComputeRootShaderResourceView(0, g_tlas->GetGPUVirtualAddress());
    g_cl->SetComputeRootShaderResourceView(1, g_instInfo->GetGPUVirtualAddress());
    g_cl->SetComputeRootUnorderedAccessView(2, g_out->GetGPUVirtualAddress());
    g_cl->SetComputeRootConstantBufferView(3, g_consts->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_heap->GetGPUDescriptorHandleForHeapStart();
    g_cl->SetComputeRootDescriptorTable(4, gh);
    gh.ptr += (UINT64)HEAP_GEO * g_descSize;
    g_cl->SetComputeRootDescriptorTable(5, gh);
    ID3D12Resource *histIn = g_hist[g_histCur ^ 1], *histOut = g_hist[g_histCur];
    Barrier(histIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_cl->SetComputeRootShaderResourceView(6, histIn->GetGPUVirtualAddress());
    g_cl->SetComputeRootUnorderedAccessView(7, histOut->GetGPUVirtualAddress());
    g_cl->SetPipelineState1(g_pso);
    D3D12_DISPATCH_RAYS_DESC dr = {};
    D3D12_GPU_VIRTUAL_ADDRESS t = g_shaderTable->GetGPUVirtualAddress();
    dr.RayGenerationShaderRecord = { t, 64 };
    dr.MissShaderTable = { t + 64, 96, 32 };
    dr.HitGroupTable = { t + 192, 96, 32 };
    dr.Width = f.outW; dr.Height = f.outH; dr.Depth = 1;
    g_cl->DispatchRays(&dr);
    Barrier(histIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_histCur ^= 1;
    Barrier(g_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_cl->CopyBufferRegion(g_readback, 0, g_out, 0, outBytes);
    Barrier(g_out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// ---------------------------------------------------------------- boucle
static bool g_probe;
int main(int argc, char **argv)
{
    char exeDir[MAX_PATH]; GetModuleFileNameA(NULL, exeDir, MAX_PATH);
    char *slash = strrchr(exeDir, '\\'); if (slash) slash[1] = 0;
    g_probe = argc > 1 && !strcmp(argv[1], "--probe");
    if (g_probe) {
        char name[128] = "";
        IDXGIAdapter1 *a = PickAdapter(name, sizeof name);
        printf("%s\n", a ? name : "aucune carte DXR");
        return a ? 0 : 1;
    }
    if (argc < 2) return 2;
    DWORD pid = strtoul(argv[1], NULL, 10);
    char path[MAX_PATH]; sprintf_s(path, "%svcrt64.log", exeDir);
    if (fopen_s(&g_log, path, "w")) {   // dossier du jeu protege (Program Files) : journal dans %TEMP%
        char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
        sprintf_s(path, "%svcrt64.log", tmp);
        fopen_s(&g_log, path, "w");
    }
    Log("vcrt64 : jeu %lu", pid);

    char name[64];
    sprintf_s(name, "Local\\VCCoopRT_%lu", pid);
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map) Fatal("memoire partagee %s introuvable", name);
    BYTE *base = (BYTE *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, RT_MAP_SIZE);
    if (!base) Fatal("memoire partagee illisible");
    g_hdr = (RtHeader *)base;
    if (g_hdr->magic != RT_MAGIC || g_hdr->version != RT_PROTOCOL) Fatal("version du protocole differente (%08X %u)", g_hdr->magic, g_hdr->version);
    sprintf_s(name, "Local\\VCCoopRT_go_%lu", pid);
    HANDLE go = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    sprintf_s(name, "Local\\VCCoopRT_done_%lu", pid);
    HANDLE done = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!go || !done || !game) Fatal("evenements ou processus du jeu introuvables");

    if (getenv("VCRT_DEBUG")) { ID3D12Debug *dbg; if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); dbg->Release(); } }
    char adapter[128] = "";
    IDXGIAdapter1 *ad = PickAdapter(adapter, sizeof adapter);
    if (!ad) Fatal("aucune carte graphique ne sait faire le ray tracing (DXR)");
    CK(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dev)));
    D3D12_COMMAND_QUEUE_DESC qd = {};
    CK(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)));
    CK(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc)));
    CK(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, NULL, IID_PPV_ARGS(&g_cl)));
    g_cl->Close();
    CK(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    g_fenceEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    CreatePipeline();
    CreateResources();
    strncpy_s(g_hdr->adapter, adapter, _TRUNCATE);
    MemoryBarrier();
    g_hdr->helperState = 1;
    Log("pret : %s", adapter);

    BYTE *cmds = base + RT_CMD_OFFSET, *out = base + RT_OUT_OFFSET;
    uint32_t frames = 0; DWORD lastLog = GetTickCount(); double msSum = 0;
    HANDLE waits[2] = { go, game };
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) { Log("le jeu s'est arrete"); break; }
        LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq);
        uint32_t seq = g_hdr->frameSeq;
        uint32_t total = g_hdr->cmdBytes;
        if (total > RT_CMD_SIZE) total = 0;
        g_alloc->Reset();
        g_cl->Reset(g_alloc, NULL);
        g_stagingUsed = 0;
        for (int g = 0; g < GEO_COUNT; g++) g_tgeoUsed[g] = 0;
        g_tblasUsed = 0;
        // Maillages de l'image precedente : oublies.
        for (auto it = g_meshes.begin(); it != g_meshes.end();) { if (it->second.transient) it = g_meshes.erase(it); else ++it; }
        const RtFrame *frame = NULL; const RtInstance *inst = NULL;
        uint32_t off = 0;
        while (off + sizeof(RtCmd) <= total) {
            const RtCmd *c = (const RtCmd *)(cmds + off);
            const BYTE *p = cmds + off + sizeof(RtCmd);
            if (off + sizeof(RtCmd) + c->bytes > total) break;
            switch (c->type) {
            case RT_CMD_MESH: CmdMesh(p, c->bytes); break;
            case RT_CMD_MESH_DEL: if (c->bytes >= 4) CmdMeshDel(*(const uint32_t *)p); break;
            case RT_CMD_TEX: CmdTex(p, c->bytes); break;
            case RT_CMD_TEX_DEL: if (c->bytes >= 4) CmdTexDel(*(const uint32_t *)p); break;
            case RT_CMD_FRAME:
                if (c->bytes >= sizeof(RtFrame)) {
                    frame = (const RtFrame *)p;
                    if (sizeof(RtFrame) + (UINT64)frame->instanceCount * sizeof(RtInstance) > c->bytes) frame = NULL;
                    else inst = (const RtInstance *)(p + sizeof(RtFrame));
                }
                break;
            }
            off += sizeof(RtCmd) + ((c->bytes + 15) & ~15u);
        }
        GeoToRead();
        bool render = frame && frame->outW && frame->outH && frame->outW <= RT_MAX_OUT_W && frame->outH <= RT_MAX_OUT_H;
        if (render) Render(*frame, inst);
        else BuildPending();
        CK(g_cl->Close());
        ID3D12CommandList *lists[] = { g_cl };
        g_queue->ExecuteCommandLists(1, lists);
        WaitGpu();
        for (IUnknown *u : g_releaseAfterFrame) u->Release();
        g_releaseAfterFrame.clear();
        if (render) {
            void *p = NULL; D3D12_RANGE rr = { 0, (SIZE_T)frame->outW * frame->outH * RT_OUT_BPP };
            if (SUCCEEDED(g_readback->Map(0, &rr, &p))) { memcpy(out, p, rr.End); D3D12_RANGE none = { 0, 0 }; g_readback->Unmap(0, &none); }
            g_hdr->outW = frame->outW; g_hdr->outH = frame->outH;
        }
        QueryPerformanceCounter(&t1);
        double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
        g_hdr->gpuMs = (float)ms;
        g_hdr->meshCount = (uint32_t)g_meshes.size(); g_hdr->texCount = (uint32_t)g_texs.size();
        MemoryBarrier();
        g_hdr->doneSeq = seq;
        SetEvent(done);
        frames++; msSum += ms;
        if (GetTickCount() - lastLog > 10000) {
            lastLog = GetTickCount();
            Log("%u images, %.2f ms en moyenne ; %zu maillages, %zu textures, %d refuses ; %u dessins dans la derniere ; blas %.0f Mo libres",
                frames, msSum / frames, g_meshes.size(), g_texs.size(), g_meshRefused, frame ? frame->instanceCount : 0,
                [] { UINT64 s = 0; for (auto &f : g_blasFree.free) s += f.second; return s / 1048576.0; }());
            frames = 0; msSum = 0;
        }
    }
    return 0;
}
