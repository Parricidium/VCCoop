// Pont Direct3D 8 -> Direct3D 9, ecrit pour VCCoop (aucun code tiers).
//
// Le jeu (RenderWare) ne parle que Direct3D 8 : fonctions fixes, 5 formats de sommets (0x42, 0x112, 0x142, 0x144,
// 0x152), aucun shader. On lui donne nos propres objets IDirect3D8 / IDirect3DDevice8 / ressources, qui traduisent
// chaque appel vers Direct3D 9. Le jeu dessine exactement comme avant ; le rendu moderne (gfx9.cpp) travaille ensuite
// en Direct3D 9 : shaders 3.0, cibles flottantes, compilateur HLSL de Windows.
//
// Enveloppes : chaque objet Direct3D 9 rendu au jeu a une seule enveloppe (table g_wrap), qui a son propre compteur
// et garde une reference sur l'objet reel tant qu'elle vit. Les vtables suivent exactement l'ordre de d3d8.h :
// methodes virtuelles __stdcall declarees dans le meme ordre, pas de destructeur virtuel, pas de surcharge.
#include "util.h"
#include "vccoop.h"
#include "bridge.h"
#include <initguid.h>
#include <d3d9.h>
#include <stddef.h>
#include <string.h>
#include <unordered_map>
#include <vector>

// --- Types propres a Direct3D 8 ---
struct D3DPRESENT_PARAMETERS8 {
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
};
struct D3DADAPTER_IDENTIFIER8 {
    char Driver[512], Description[512];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId, DeviceId, SubSysId, Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
};
struct D3DSURFACE_DESC8 { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; D3DMULTISAMPLE_TYPE MultiSampleType; UINT Width, Height; };
struct D3DVOLUME_DESC8 { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; UINT Width, Height, Depth; };
enum { D3DCAPS8_SIZE = offsetof(D3DCAPS9, PixelShader1xMaxValue) + sizeof(float) };
enum { SWAPEFFECT8_COPY_VSYNC = 4, ENUM8_NO_WHQL_LEVEL = 2 };
enum { RS8_LINEPATTERN = 10, RS8_ZVISIBLE = 30, RS8_EDGEANTIALIAS = 40, RS8_ZBIAS = 47, RS8_SOFTWAREVP = 153, RS8_PATCHSEGMENTS = 164 };

typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);

class D3D8;
class Device8;
class Surface8;
class Texture8;
class CubeTexture8;
class VolumeTexture8;
class Volume8;
class VertexBuffer8;
class IndexBuffer8;
class SwapChain8;

// ======================================================================= Table des enveloppes
static std::unordered_map<void *, void *> g_wrap;   // objet Direct3D 9 -> son enveloppe
static CRITICAL_SECTION g_wrapLock;
struct WrapLock { WrapLock() { EnterCriticalSection(&g_wrapLock); } ~WrapLock() { LeaveCriticalSection(&g_wrapLock); } };

// Toutes les enveloppes : [vtable][objet reel][compteur][peripherique][genre].
// Surfaces : RenderWare garde le pointeur de la surface de profondeur (et de l'image) apres l'avoir relachee, ce que
// Direct3D 8 permet (le peripherique la garde en vie). Une enveloppe de surface n'est donc jamais detruite : a zero
// elle rend sa reference et reste dans la table ("en sommeil") ; elle resert si le meme objet revient.
struct WrapHeader { void *vt; IUnknown *real; ULONG ref; Device8 *dev; int kind; };
enum { KIND_SURFACE = 1, KIND_VOLUME, KIND_TEXTURE, KIND_CUBE, KIND_VOLTEX, KIND_VB, KIND_IB, KIND_SWAP };
template <class T> static T *Real(void *wrapper) { return wrapper ? (T *)((WrapHeader *)wrapper)->real : NULL; }

// L'objet reel arrive avec une reference a nous (Create / Get) : enveloppe existante (on rend la reference en trop)
// ou nouvelle.
template <class W, class R> static W *Wrap(R *real, Device8 *dev)
{
    if (!real) return NULL;
    WrapLock lock;
    auto it = g_wrap.find(real);
    if (it != g_wrap.end() && ((WrapHeader *)it->second)->kind == W::KIND) {
        W *w = (W *)it->second;
        WrapHeader *h = (WrapHeader *)w;
        if (h->ref++ > 0) real->Release();   // en sommeil (compteur a 0) : elle reprend la reference qui arrive
        return w;
    }
    W *w = new W(real, dev);
    g_wrap[real] = w;
    return w;
}

static ULONG WrapRelease(void *self)
{
    WrapHeader *h = (WrapHeader *)self;
    WrapLock lock;
    if (h->ref == 0) return 0;   // surface en sommeil relachee une fois de trop
    ULONG r = --h->ref;
    if (r == 0) {
        if (h->kind != KIND_SURFACE) g_wrap.erase(h->real);
        h->real->Release();
    }
    return r;
}

// Echecs de creation (le jeu ne verifie pas toujours : modele sans texture, objet de cinematique sans modele...).
static HRESULT Fail(HRESULT hr, const char *what, UINT a, UINT b, UINT c, UINT d)
{
    static int n;
    if (FAILED(hr) && n < 60) { n++; Log("pont : %s refuse (0x%08lX) : %u %u %u %u", what, hr, a, b, c, d); }
    return hr;
}

// Taille d'une surface (D3D8 la donne dans la description).
static UINT FormatBits(D3DFORMAT f)
{
    switch ((DWORD)f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_G16R16: case D3DFMT_A2R10G10B10: case D3DFMT_D32: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4:
    case D3DFMT_R32F: case D3DFMT_G16R16F: case D3DFMT_X8L8V8U8: case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: return 32;
    case D3DFMT_R8G8B8: return 24;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8R3G3B2: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8P8: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_D16: case D3DFMT_D15S1: case D3DFMT_D16_LOCKABLE: case D3DFMT_R16F: return 16;
    case D3DFMT_R3G3B2: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4: return 8;
    case D3DFMT_DXT1: return 4;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return 8;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: return 64;
    case D3DFMT_A32B32G32R32F: return 128;
    }
    return 32;
}
static UINT SurfaceSize(D3DFORMAT f, UINT w, UINT h, UINT d = 1)
{
    if (f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5)
        return ((w + 3) / 4) * ((h + 3) / 4) * (f == D3DFMT_DXT1 ? 8 : 16) * d;
    return w * h * d * FormatBits(f) / 8;
}
static void ConvertDesc(const D3DSURFACE_DESC &s, D3DSURFACE_DESC8 *d)
{
    d->Format = s.Format; d->Type = s.Type; d->Usage = s.Usage; d->Pool = s.Pool; d->MultiSampleType = s.MultiSampleType;
    d->Width = s.Width; d->Height = s.Height; d->Size = SurfaceSize(s.Format, s.Width, s.Height);
}
static void ConvertPresent(const D3DPRESENT_PARAMETERS8 *s, D3DPRESENT_PARAMETERS *d)
{
    memset(d, 0, sizeof(*d));
    d->BackBufferWidth = s->BackBufferWidth; d->BackBufferHeight = s->BackBufferHeight;
    d->BackBufferFormat = s->BackBufferFormat; d->BackBufferCount = s->BackBufferCount;
    d->MultiSampleType = s->MultiSampleType; d->MultiSampleQuality = 0;
    d->SwapEffect = s->SwapEffect == (D3DSWAPEFFECT)SWAPEFFECT8_COPY_VSYNC ? D3DSWAPEFFECT_COPY : s->SwapEffect;
    d->hDeviceWindow = s->hDeviceWindow; d->Windowed = s->Windowed;
    d->EnableAutoDepthStencil = s->EnableAutoDepthStencil; d->AutoDepthStencilFormat = s->AutoDepthStencilFormat;
    d->Flags = s->Flags;
    d->FullScreen_RefreshRateInHz = s->Windowed ? 0 : s->FullScreen_RefreshRateInHz;
    // Direct3D 8 en fenetre : pas d'attente de synchro (le limiteur d'images de VCCoop cadence) ; COPY_VSYNC : attente.
    if (s->Windowed) d->PresentationInterval = s->SwapEffect == (D3DSWAPEFFECT)SWAPEFFECT8_COPY_VSYNC ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    else d->PresentationInterval = s->FullScreen_PresentationInterval;
    if (d->MultiSampleType) d->Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
}

#define STDM virtual HRESULT __stdcall
#define UNKNOWN_METHODS(Cls) \
    STDM QueryInterface(REFIID riid, void **out) { \
        if (!out) return E_POINTER; \
        if (riid == IID_IUnknown) { *out = this; AddRef(); return S_OK; } \
        *out = NULL; Log("pont : QueryInterface refuse sur " #Cls); return E_NOINTERFACE; } \
    virtual ULONG __stdcall AddRef() { WrapLock lock; if (m_ref == 0) m_real->AddRef(); return ++m_ref; } \
    virtual ULONG __stdcall Release() { ULONG r = WrapRelease(this); if (!r && m_kind != KIND_SURFACE) delete this; return r; }
#define RESOURCE_METHODS(R9) \
    STDM GetDevice(Device8 **out); \
    STDM SetPrivateData(REFGUID g, const void *d, DWORD n, DWORD f) { return m_obj->SetPrivateData(g, d, n, f); } \
    STDM GetPrivateData(REFGUID g, void *d, DWORD *n) { return m_obj->GetPrivateData(g, d, n); } \
    STDM FreePrivateData(REFGUID g) { return m_obj->FreePrivateData(g); } \
    virtual DWORD __stdcall SetPriority(DWORD p) { return m_obj->SetPriority(p); } \
    virtual DWORD __stdcall GetPriority() { return m_obj->GetPriority(); } \
    virtual void __stdcall PreLoad() { m_obj->PreLoad(); } \
    virtual D3DRESOURCETYPE __stdcall GetType() { return m_obj->GetType(); }
#define WRAP_FIELDS(R9) \
    union { IUnknown *m_real; R9 *m_obj; }; ULONG m_ref; Device8 *m_dev; int m_kind;

// ======================================================================= Surfaces et volumes
class Surface8 {
public:
    WRAP_FIELDS(IDirect3DSurface9)
    enum { KIND = KIND_SURFACE };
    Surface8(IDirect3DSurface9 *s, Device8 *d) : m_obj(s), m_ref(1), m_dev(d), m_kind(KIND_SURFACE) {}
    UNKNOWN_METHODS(Surface8)
    STDM GetDevice(Device8 **out);
    STDM SetPrivateData(REFGUID g, const void *d, DWORD n, DWORD f) { return m_obj->SetPrivateData(g, d, n, f); }
    STDM GetPrivateData(REFGUID g, void *d, DWORD *n) { return m_obj->GetPrivateData(g, d, n); }
    STDM FreePrivateData(REFGUID g) { return m_obj->FreePrivateData(g); }
    STDM GetContainer(REFIID riid, void **out);
    STDM GetDesc(D3DSURFACE_DESC8 *d) { D3DSURFACE_DESC s; HRESULT hr = m_obj->GetDesc(&s); if (SUCCEEDED(hr)) ConvertDesc(s, d); return hr; }
    STDM LockRect(D3DLOCKED_RECT *r, const RECT *rc, DWORD f) { return m_obj->LockRect(r, rc, f); }
    STDM UnlockRect() { return m_obj->UnlockRect(); }
};

class Volume8 {
public:
    WRAP_FIELDS(IDirect3DVolume9)
    enum { KIND = KIND_VOLUME };
    Volume8(IDirect3DVolume9 *s, Device8 *d) : m_obj(s), m_ref(1), m_dev(d), m_kind(KIND_VOLUME) {}
    UNKNOWN_METHODS(Volume8)
    STDM GetDevice(Device8 **out);
    STDM SetPrivateData(REFGUID g, const void *d, DWORD n, DWORD f) { return m_obj->SetPrivateData(g, d, n, f); }
    STDM GetPrivateData(REFGUID g, void *d, DWORD *n) { return m_obj->GetPrivateData(g, d, n); }
    STDM FreePrivateData(REFGUID g) { return m_obj->FreePrivateData(g); }
    STDM GetContainer(REFIID riid, void **out) { if (out) *out = NULL; return E_NOINTERFACE; }
    STDM GetDesc(D3DVOLUME_DESC8 *d)
    {
        D3DVOLUME_DESC s; HRESULT hr = m_obj->GetDesc(&s);
        if (SUCCEEDED(hr)) { d->Format = s.Format; d->Type = s.Type; d->Usage = s.Usage; d->Pool = s.Pool; d->Width = s.Width; d->Height = s.Height; d->Depth = s.Depth; d->Size = SurfaceSize(s.Format, s.Width, s.Height, s.Depth); }
        return hr;
    }
    STDM LockBox(D3DLOCKED_BOX *b, const D3DBOX *box, DWORD f) { return m_obj->LockBox(b, box, f); }
    STDM UnlockBox() { return m_obj->UnlockBox(); }
};

// ======================================================================= Textures
class Texture8 {
public:
    WRAP_FIELDS(IDirect3DTexture9)
    enum { KIND = KIND_TEXTURE };
    Texture8(IDirect3DTexture9 *t, Device8 *d) : m_obj(t), m_ref(1), m_dev(d), m_kind(KIND_TEXTURE) {}
    UNKNOWN_METHODS(Texture8)
    RESOURCE_METHODS(IDirect3DTexture9)
    virtual DWORD __stdcall SetLOD(DWORD l) { return m_obj->SetLOD(l); }
    virtual DWORD __stdcall GetLOD() { return m_obj->GetLOD(); }
    virtual DWORD __stdcall GetLevelCount() { return m_obj->GetLevelCount(); }
    STDM GetLevelDesc(UINT l, D3DSURFACE_DESC8 *d) { D3DSURFACE_DESC s; HRESULT hr = m_obj->GetLevelDesc(l, &s); if (SUCCEEDED(hr)) ConvertDesc(s, d); return hr; }
    STDM GetSurfaceLevel(UINT l, Surface8 **out)
    {
        IDirect3DSurface9 *s = NULL;
        HRESULT hr = m_obj->GetSurfaceLevel(l, &s);
        *out = SUCCEEDED(hr) ? Wrap<Surface8>(s, m_dev) : NULL;
        return hr;
    }
    STDM LockRect(UINT l, D3DLOCKED_RECT *r, const RECT *rc, DWORD f) { return Fail(m_obj->LockRect(l, r, rc, f), "Texture LockRect", l, f, 0, 0); }
    STDM UnlockRect(UINT l) { return m_obj->UnlockRect(l); }
    STDM AddDirtyRect(const RECT *r) { return m_obj->AddDirtyRect(r); }
};

class CubeTexture8 {
public:
    WRAP_FIELDS(IDirect3DCubeTexture9)
    enum { KIND = KIND_CUBE };
    CubeTexture8(IDirect3DCubeTexture9 *t, Device8 *d) : m_obj(t), m_ref(1), m_dev(d), m_kind(KIND_CUBE) {}
    UNKNOWN_METHODS(CubeTexture8)
    RESOURCE_METHODS(IDirect3DCubeTexture9)
    virtual DWORD __stdcall SetLOD(DWORD l) { return m_obj->SetLOD(l); }
    virtual DWORD __stdcall GetLOD() { return m_obj->GetLOD(); }
    virtual DWORD __stdcall GetLevelCount() { return m_obj->GetLevelCount(); }
    STDM GetLevelDesc(UINT l, D3DSURFACE_DESC8 *d) { D3DSURFACE_DESC s; HRESULT hr = m_obj->GetLevelDesc(l, &s); if (SUCCEEDED(hr)) ConvertDesc(s, d); return hr; }
    STDM GetCubeMapSurface(D3DCUBEMAP_FACES f, UINT l, Surface8 **out)
    {
        IDirect3DSurface9 *s = NULL;
        HRESULT hr = m_obj->GetCubeMapSurface(f, l, &s);
        *out = SUCCEEDED(hr) ? Wrap<Surface8>(s, m_dev) : NULL;
        return hr;
    }
    STDM LockRect(D3DCUBEMAP_FACES f, UINT l, D3DLOCKED_RECT *r, const RECT *rc, DWORD fl) { return m_obj->LockRect(f, l, r, rc, fl); }
    STDM UnlockRect(D3DCUBEMAP_FACES f, UINT l) { return m_obj->UnlockRect(f, l); }
    STDM AddDirtyRect(D3DCUBEMAP_FACES f, const RECT *r) { return m_obj->AddDirtyRect(f, r); }
};

class VolumeTexture8 {
public:
    WRAP_FIELDS(IDirect3DVolumeTexture9)
    enum { KIND = KIND_VOLTEX };
    VolumeTexture8(IDirect3DVolumeTexture9 *t, Device8 *d) : m_obj(t), m_ref(1), m_dev(d), m_kind(KIND_VOLTEX) {}
    UNKNOWN_METHODS(VolumeTexture8)
    RESOURCE_METHODS(IDirect3DVolumeTexture9)
    virtual DWORD __stdcall SetLOD(DWORD l) { return m_obj->SetLOD(l); }
    virtual DWORD __stdcall GetLOD() { return m_obj->GetLOD(); }
    virtual DWORD __stdcall GetLevelCount() { return m_obj->GetLevelCount(); }
    STDM GetLevelDesc(UINT l, D3DVOLUME_DESC8 *d)
    {
        D3DVOLUME_DESC s; HRESULT hr = m_obj->GetLevelDesc(l, &s);
        if (SUCCEEDED(hr)) { d->Format = s.Format; d->Type = s.Type; d->Usage = s.Usage; d->Pool = s.Pool; d->Width = s.Width; d->Height = s.Height; d->Depth = s.Depth; d->Size = SurfaceSize(s.Format, s.Width, s.Height, s.Depth); }
        return hr;
    }
    STDM GetVolumeLevel(UINT l, Volume8 **out)
    {
        IDirect3DVolume9 *v = NULL;
        HRESULT hr = m_obj->GetVolumeLevel(l, &v);
        *out = SUCCEEDED(hr) ? Wrap<Volume8>(v, m_dev) : NULL;
        return hr;
    }
    STDM LockBox(UINT l, D3DLOCKED_BOX *b, const D3DBOX *box, DWORD f) { return m_obj->LockBox(l, b, box, f); }
    STDM UnlockBox(UINT l) { return m_obj->UnlockBox(l); }
    STDM AddDirtyBox(const D3DBOX *b) { return m_obj->AddDirtyBox(b); }
};

// Texture de base -> son enveloppe selon son type.
static void *WrapBaseTexture(IDirect3DBaseTexture9 *t, Device8 *d)
{
    if (!t) return NULL;
    switch (t->GetType()) {
    case D3DRTYPE_TEXTURE: return Wrap<Texture8>((IDirect3DTexture9 *)t, d);
    case D3DRTYPE_CUBETEXTURE: return Wrap<CubeTexture8>((IDirect3DCubeTexture9 *)t, d);
    case D3DRTYPE_VOLUMETEXTURE: return Wrap<VolumeTexture8>((IDirect3DVolumeTexture9 *)t, d);
    default: t->Release(); return NULL;
    }
}

// ======================================================================= Tampons
// Tampon dynamique : le jeu ecrit dans une copie en memoire (rendue au tampon reel au deverrouillage), que le rendu
// moderne relit pour rejouer ses dessins plus tard dans l'image.
struct Mirror {
    std::vector<BYTE> data;
    BYTE *locked;
    UINT offset, size;
};
static bool Lockable(DWORD usage) { return (usage & D3DUSAGE_DYNAMIC) != 0; }

class VertexBuffer8 {
public:
    WRAP_FIELDS(IDirect3DVertexBuffer9)
    Mirror m_mirror;
    enum { KIND = KIND_VB };
    VertexBuffer8(IDirect3DVertexBuffer9 *b, Device8 *d) : m_obj(b), m_ref(1), m_dev(d), m_kind(KIND_VB)
    {
        m_mirror.locked = NULL;
        D3DVERTEXBUFFER_DESC desc;
        if (SUCCEEDED(b->GetDesc(&desc)) && Lockable(desc.Usage)) m_mirror.data.resize(desc.Size);
    }
    UNKNOWN_METHODS(VertexBuffer8)
    RESOURCE_METHODS(IDirect3DVertexBuffer9)
    STDM Lock(UINT off, UINT size, BYTE **out, DWORD f)
    {
        if (m_mirror.data.empty()) return m_obj->Lock(off, size, (void **)out, f);
        if (size == 0 || off + size > m_mirror.data.size()) size = (UINT)m_mirror.data.size() - off;
        void *p = NULL;
        HRESULT hr = m_obj->Lock(off, size, &p, f);
        if (FAILED(hr)) return hr;
        m_mirror.locked = (BYTE *)p; m_mirror.offset = off; m_mirror.size = size;
        *out = m_mirror.data.data() + off;
        return hr;
    }
    STDM Unlock()
    {
        if (m_mirror.locked) { memcpy(m_mirror.locked, m_mirror.data.data() + m_mirror.offset, m_mirror.size); m_mirror.locked = NULL; }
        return m_obj->Unlock();
    }
    STDM GetDesc(D3DVERTEXBUFFER_DESC *d) { return m_obj->GetDesc(d); }
};

class IndexBuffer8 {
public:
    WRAP_FIELDS(IDirect3DIndexBuffer9)
    Mirror m_mirror;
    enum { KIND = KIND_IB };
    IndexBuffer8(IDirect3DIndexBuffer9 *b, Device8 *d) : m_obj(b), m_ref(1), m_dev(d), m_kind(KIND_IB)
    {
        m_mirror.locked = NULL;
        D3DINDEXBUFFER_DESC desc;
        if (SUCCEEDED(b->GetDesc(&desc)) && Lockable(desc.Usage)) m_mirror.data.resize(desc.Size);
    }
    UNKNOWN_METHODS(IndexBuffer8)
    RESOURCE_METHODS(IDirect3DIndexBuffer9)
    STDM Lock(UINT off, UINT size, BYTE **out, DWORD f)
    {
        if (m_mirror.data.empty()) return m_obj->Lock(off, size, (void **)out, f);
        if (size == 0 || off + size > m_mirror.data.size()) size = (UINT)m_mirror.data.size() - off;
        void *p = NULL;
        HRESULT hr = m_obj->Lock(off, size, &p, f);
        if (FAILED(hr)) return hr;
        m_mirror.locked = (BYTE *)p; m_mirror.offset = off; m_mirror.size = size;
        *out = m_mirror.data.data() + off;
        return hr;
    }
    STDM Unlock()
    {
        if (m_mirror.locked) { memcpy(m_mirror.locked, m_mirror.data.data() + m_mirror.offset, m_mirror.size); m_mirror.locked = NULL; }
        return m_obj->Unlock();
    }
    STDM GetDesc(D3DINDEXBUFFER_DESC *d) { return m_obj->GetDesc(d); }
};

const BYTE *BridgeVertexMirror(IDirect3DVertexBuffer9 *vb)
{
    WrapLock lock;
    auto it = g_wrap.find(vb);
    if (it == g_wrap.end()) return NULL;
    VertexBuffer8 *w = (VertexBuffer8 *)it->second;
    return w->m_mirror.data.empty() ? NULL : w->m_mirror.data.data();
}
const BYTE *BridgeIndexMirror(IDirect3DIndexBuffer9 *ib)
{
    WrapLock lock;
    auto it = g_wrap.find(ib);
    if (it == g_wrap.end()) return NULL;
    IndexBuffer8 *w = (IndexBuffer8 *)it->second;
    return w->m_mirror.data.empty() ? NULL : w->m_mirror.data.data();
}

// ======================================================================= Chaine d'echange
class SwapChain8 {
public:
    WRAP_FIELDS(IDirect3DSwapChain9)
    enum { KIND = KIND_SWAP };
    SwapChain8(IDirect3DSwapChain9 *s, Device8 *d) : m_obj(s), m_ref(1), m_dev(d), m_kind(KIND_SWAP) {}
    UNKNOWN_METHODS(SwapChain8)
    STDM Present(const RECT *s, const RECT *d, HWND w, const RGNDATA *r) { return m_obj->Present(s, d, w, r, 0); }
    STDM GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE t, Surface8 **out)
    {
        IDirect3DSurface9 *s = NULL;
        HRESULT hr = m_obj->GetBackBuffer(i, t, &s);
        *out = SUCCEEDED(hr) ? Wrap<Surface8>(s, m_dev) : NULL;
        return hr;
    }
};

// ======================================================================= Peripherique
bool g_bridgeCasterOnly;
static Device8 *g_device8;
static IDirect3DDevice9 *g_device9;

class Device8 {
public:
    WRAP_FIELDS(IDirect3DDevice9)
    D3D8 *m_d3d8;
    UINT m_baseVertex;
    DWORD m_fvf;
    DWORD m_rs8[256];        // etats de rendu propres a D3D8 (relus par GetRenderState)
    DWORD m_vsHandle;
    Device8(IDirect3DDevice9 *d, D3D8 *parent) : m_obj(d), m_ref(1), m_dev(this), m_kind(0), m_d3d8(parent), m_baseVertex(0), m_fvf(0), m_vsHandle(0)
    {
        memset(m_rs8, 0, sizeof(m_rs8));
        m_rs8[RS8_LINEPATTERN] = 0; m_rs8[RS8_ZVISIBLE] = 0; m_rs8[RS8_EDGEANTIALIAS] = 0; m_rs8[RS8_ZBIAS] = 0; m_rs8[RS8_PATCHSEGMENTS] = 0x3F800000;
    }

    STDM QueryInterface(REFIID riid, void **out) { if (!out) return E_POINTER; if (riid == IID_IUnknown) { *out = this; AddRef(); return S_OK; } *out = NULL; return E_NOINTERFACE; }
    virtual ULONG __stdcall AddRef() { return ++m_ref; }
    virtual ULONG __stdcall Release()
    {
        ULONG r = --m_ref;
        if (!r) {
            Log("pont : peripherique libere");
            if (g_device8 == this) { g_device8 = NULL; g_device9 = NULL; }
            m_obj->Release();
            delete this;
        }
        return r;
    }

    STDM TestCooperativeLevel() { return m_obj->TestCooperativeLevel(); }
    virtual UINT __stdcall GetAvailableTextureMem() { return m_obj->GetAvailableTextureMem(); }
    STDM ResourceManagerDiscardBytes(DWORD) { return m_obj->EvictManagedResources(); }
    STDM GetDirect3D(D3D8 **out);
    STDM GetDeviceCaps(D3DCAPS9 *caps8);
    STDM GetDisplayMode(D3DDISPLAYMODE *m) { return m_obj->GetDisplayMode(0, m); }
    STDM GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *p) { return m_obj->GetCreationParameters(p); }
    STDM SetCursorProperties(UINT x, UINT y, Surface8 *s) { return m_obj->SetCursorProperties(x, y, Real<IDirect3DSurface9>(s)); }
    virtual void __stdcall SetCursorPosition(int x, int y, DWORD f) { m_obj->SetCursorPosition(x, y, f); }
    virtual BOOL __stdcall ShowCursor(BOOL b) { return m_obj->ShowCursor(b); }
    STDM CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS8 *pp, SwapChain8 **out)
    {
        D3DPRESENT_PARAMETERS p9; ConvertPresent(pp, &p9);
        IDirect3DSwapChain9 *s = NULL;
        HRESULT hr = m_obj->CreateAdditionalSwapChain(&p9, &s);
        *out = SUCCEEDED(hr) ? Wrap<SwapChain8>(s, this) : NULL;
        return hr;
    }
    STDM Reset(D3DPRESENT_PARAMETERS8 *pp)
    {
        D3DPRESENT_PARAMETERS p9; ConvertPresent(pp, &p9);
        Gfx9BeforeReset();
        HRESULT hr = m_obj->Reset(&p9);
        if (SUCCEEDED(hr)) {
            m_baseVertex = 0;
            Gfx9AfterReset(p9.BackBufferWidth, p9.BackBufferHeight, p9.MultiSampleType != 0);
        } else Log("pont : Reset refuse (0x%08lX)", hr);
        return hr;
    }
    STDM Present(const RECT *s, const RECT *d, HWND w, const RGNDATA *r) { Gfx9BeforePresent(); return m_obj->Present(s, d, w, r); }
    STDM GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE t, Surface8 **out)
    {
        IDirect3DSurface9 *s = NULL;
        HRESULT hr = m_obj->GetBackBuffer(0, i, t, &s);
        *out = SUCCEEDED(hr) ? Wrap<Surface8>(s, this) : NULL;
        return hr;
    }
    STDM GetRasterStatus(D3DRASTER_STATUS *r) { return m_obj->GetRasterStatus(0, r); }
    virtual void __stdcall SetGammaRamp(DWORD f, const D3DGAMMARAMP *r) { m_obj->SetGammaRamp(0, f, r); }
    virtual void __stdcall GetGammaRamp(D3DGAMMARAMP *r) { m_obj->GetGammaRamp(0, r); }
    STDM CreateTexture(UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, Texture8 **out)
    {
        IDirect3DTexture9 *t = NULL;
        HRESULT hr = Fail(m_obj->CreateTexture(w, h, l, u, f, p, &t, NULL), "CreateTexture", w, h, (UINT)f, (UINT)p | (u << 8));
        *out = SUCCEEDED(hr) ? Wrap<Texture8>(t, this) : NULL;
        return hr;
    }
    STDM CreateVolumeTexture(UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, VolumeTexture8 **out)
    {
        IDirect3DVolumeTexture9 *t = NULL;
        HRESULT hr = Fail(m_obj->CreateVolumeTexture(w, h, d, l, u, f, p, &t, NULL), "CreateVolumeTexture", w, h, (UINT)f, (UINT)p);
        *out = SUCCEEDED(hr) ? Wrap<VolumeTexture8>(t, this) : NULL;
        return hr;
    }
    STDM CreateCubeTexture(UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, CubeTexture8 **out)
    {
        IDirect3DCubeTexture9 *t = NULL;
        HRESULT hr = Fail(m_obj->CreateCubeTexture(e, l, u, f, p, &t, NULL), "CreateCubeTexture", e, l, (UINT)f, (UINT)p);
        *out = SUCCEEDED(hr) ? Wrap<CubeTexture8>(t, this) : NULL;
        return hr;
    }
    STDM CreateVertexBuffer(UINT len, DWORD u, DWORD fvf, D3DPOOL p, VertexBuffer8 **out)
    {
        IDirect3DVertexBuffer9 *b = NULL;
        HRESULT hr = Fail(m_obj->CreateVertexBuffer(len, u, fvf, p, &b, NULL), "CreateVertexBuffer", len, u, fvf, (UINT)p);
        *out = SUCCEEDED(hr) ? Wrap<VertexBuffer8>(b, this) : NULL;
        return hr;
    }
    STDM CreateIndexBuffer(UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IndexBuffer8 **out)
    {
        IDirect3DIndexBuffer9 *b = NULL;
        HRESULT hr = Fail(m_obj->CreateIndexBuffer(len, u, f, p, &b, NULL), "CreateIndexBuffer", len, u, (UINT)f, (UINT)p);
        *out = SUCCEEDED(hr) ? Wrap<IndexBuffer8>(b, this) : NULL;
        return hr;
    }
    STDM CreateRenderTarget(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, BOOL lockable, Surface8 **out)
    {
        IDirect3DSurface9 *s = NULL;
        HRESULT hr = Fail(m_obj->CreateRenderTarget(w, h, f, ms, 0, ms ? FALSE : lockable, &s, NULL), "CreateRenderTarget", w, h, (UINT)f, (UINT)ms);
        *out = SUCCEEDED(hr) ? Wrap<Surface8>(s, this) : NULL;
        return hr;
    }
    STDM CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, Surface8 **out)
    {
        IDirect3DSurface9 *s = NULL;
        HRESULT hr = Fail(m_obj->CreateDepthStencilSurface(w, h, f, ms, 0, FALSE, &s, NULL), "CreateDepthStencilSurface", w, h, (UINT)f, (UINT)ms);
        *out = SUCCEEDED(hr) ? Wrap<Surface8>(s, this) : NULL;
        return hr;
    }
    STDM CreateImageSurface(UINT w, UINT h, D3DFORMAT f, Surface8 **out)
    {
        IDirect3DSurface9 *s = NULL;
        HRESULT hr = Fail(m_obj->CreateOffscreenPlainSurface(w, h, f, D3DPOOL_SYSTEMMEM, &s, NULL), "CreateImageSurface", w, h, (UINT)f, 0);
        *out = SUCCEEDED(hr) ? Wrap<Surface8>(s, this) : NULL;
        return hr;
    }
    STDM CopyRects(Surface8 *src, const RECT *rects, UINT n, Surface8 *dst, const POINT *pts);
    STDM UpdateTexture(void *src, void *dst) { return m_obj->UpdateTexture(Real<IDirect3DBaseTexture9>(src), Real<IDirect3DBaseTexture9>(dst)); }
    STDM GetFrontBuffer(Surface8 *dst) { return m_obj->GetFrontBufferData(0, Real<IDirect3DSurface9>(dst)); }
    STDM SetRenderTarget(Surface8 *rt, Surface8 *ds)
    {
        HRESULT hr = D3D_OK;
        if (rt) hr = m_obj->SetRenderTarget(0, Real<IDirect3DSurface9>(rt));
        if (SUCCEEDED(hr)) hr = m_obj->SetDepthStencilSurface(Real<IDirect3DSurface9>(ds));
        return hr;
    }
    STDM GetRenderTarget(Surface8 **out)
    {
        IDirect3DSurface9 *s = NULL;
        HRESULT hr = m_obj->GetRenderTarget(0, &s);
        *out = SUCCEEDED(hr) ? Wrap<Surface8>(s, this) : NULL;
        return hr;
    }
    STDM GetDepthStencilSurface(Surface8 **out)
    {
        IDirect3DSurface9 *s = NULL;
        HRESULT hr = m_obj->GetDepthStencilSurface(&s);
        *out = SUCCEEDED(hr) ? Wrap<Surface8>(s, this) : NULL;
        return hr;
    }
    STDM BeginScene() { HRESULT hr = m_obj->BeginScene(); if (SUCCEEDED(hr)) Gfx9BeginScene(); return hr; }
    STDM EndScene() { Gfx9EndScene(); return m_obj->EndScene(); }
    STDM Clear(DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c, float z, DWORD s) { return m_obj->Clear(n, r, f, c, z, s); }
    STDM SetTransform(D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) { return m_obj->SetTransform(t, m); }
    STDM GetTransform(D3DTRANSFORMSTATETYPE t, D3DMATRIX *m) { return m_obj->GetTransform(t, m); }
    STDM MultiplyTransform(D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) { return m_obj->MultiplyTransform(t, m); }
    STDM SetViewport(const D3DVIEWPORT9 *v) { return m_obj->SetViewport(v); }
    STDM GetViewport(D3DVIEWPORT9 *v) { return m_obj->GetViewport(v); }
    STDM SetMaterial(const D3DMATERIAL9 *m) { return m_obj->SetMaterial(m); }
    STDM GetMaterial(D3DMATERIAL9 *m) { return m_obj->GetMaterial(m); }
    STDM SetLight(DWORD i, const D3DLIGHT9 *l) { return m_obj->SetLight(i, l); }
    STDM GetLight(DWORD i, D3DLIGHT9 *l) { return m_obj->GetLight(i, l); }
    STDM LightEnable(DWORD i, BOOL b) { return m_obj->LightEnable(i, b); }
    STDM GetLightEnable(DWORD i, BOOL *b) { return m_obj->GetLightEnable(i, b); }
    STDM SetClipPlane(DWORD i, const float *p) { return m_obj->SetClipPlane(i, p); }
    STDM GetClipPlane(DWORD i, float *p) { return m_obj->GetClipPlane(i, p); }
    STDM SetRenderState(DWORD s, DWORD v)
    {
        switch (s) {
        case RS8_LINEPATTERN: case RS8_ZVISIBLE: case RS8_EDGEANTIALIAS: case RS8_PATCHSEGMENTS: m_rs8[s] = v; return D3D_OK;
        case RS8_ZBIAS: {
            m_rs8[s] = v;
            float bias = -(float)v * 0.000002f;   // ZBIAS 0..16 -> decalage de profondeur (vers la camera)
            return m_obj->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD *)&bias);
        }
        case RS8_SOFTWAREVP: m_rs8[s] = v; return m_obj->SetSoftwareVertexProcessing(v);
        }
        return m_obj->SetRenderState((D3DRENDERSTATETYPE)s, v);
    }
    STDM GetRenderState(DWORD s, DWORD *v)
    {
        switch (s) {
        case RS8_LINEPATTERN: case RS8_ZVISIBLE: case RS8_EDGEANTIALIAS: case RS8_PATCHSEGMENTS: case RS8_ZBIAS: *v = m_rs8[s]; return D3D_OK;
        case RS8_SOFTWAREVP: *v = m_obj->GetSoftwareVertexProcessing(); return D3D_OK;
        }
        return m_obj->GetRenderState((D3DRENDERSTATETYPE)s, v);
    }
    STDM BeginStateBlock() { return m_obj->BeginStateBlock(); }
    STDM EndStateBlock(DWORD *token) { IDirect3DStateBlock9 *sb = NULL; HRESULT hr = m_obj->EndStateBlock(&sb); *token = (DWORD)(uintptr_t)sb; return hr; }
    STDM ApplyStateBlock(DWORD token) { return token ? ((IDirect3DStateBlock9 *)(uintptr_t)token)->Apply() : D3DERR_INVALIDCALL; }
    STDM CaptureStateBlock(DWORD token) { return token ? ((IDirect3DStateBlock9 *)(uintptr_t)token)->Capture() : D3DERR_INVALIDCALL; }
    STDM DeleteStateBlock(DWORD token) { if (token) ((IDirect3DStateBlock9 *)(uintptr_t)token)->Release(); return D3D_OK; }
    STDM CreateStateBlock(D3DSTATEBLOCKTYPE t, DWORD *token) { IDirect3DStateBlock9 *sb = NULL; HRESULT hr = m_obj->CreateStateBlock(t, &sb); *token = (DWORD)(uintptr_t)sb; return hr; }
    STDM SetClipStatus(const D3DCLIPSTATUS9 *c) { return m_obj->SetClipStatus(c); }
    STDM GetClipStatus(D3DCLIPSTATUS9 *c) { return m_obj->GetClipStatus(c); }
    STDM GetTexture(DWORD stage, void **out)
    {
        IDirect3DBaseTexture9 *t = NULL;
        HRESULT hr = m_obj->GetTexture(stage, &t);
        *out = SUCCEEDED(hr) ? WrapBaseTexture(t, this) : NULL;
        return hr;
    }
    STDM SetTexture(DWORD stage, void *t) { return m_obj->SetTexture(stage, Real<IDirect3DBaseTexture9>(t)); }
    // Etats d'etage : filtrage, adressage, bordure et biais sont des etats d'echantillonneur en D3D9.
    static int SamplerOf(DWORD t)
    {
        switch (t) {
        case 13: return D3DSAMP_ADDRESSU; case 14: return D3DSAMP_ADDRESSV; case 25: return D3DSAMP_ADDRESSW;
        case 15: return D3DSAMP_BORDERCOLOR; case 16: return D3DSAMP_MAGFILTER; case 17: return D3DSAMP_MINFILTER;
        case 18: return D3DSAMP_MIPFILTER; case 19: return D3DSAMP_MIPMAPLODBIAS; case 20: return D3DSAMP_MAXMIPLEVEL;
        case 21: return D3DSAMP_MAXANISOTROPY;
        }
        return 0;
    }
    STDM GetTextureStageState(DWORD stage, DWORD t, DWORD *v)
    {
        int s = SamplerOf(t);
        if (s) return m_obj->GetSamplerState(stage, (D3DSAMPLERSTATETYPE)s, v);
        return m_obj->GetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)t, v);
    }
    STDM SetTextureStageState(DWORD stage, DWORD t, DWORD v)
    {
        int s = SamplerOf(t);
        if (s) return m_obj->SetSamplerState(stage, (D3DSAMPLERSTATETYPE)s, v);
        return m_obj->SetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)t, v);
    }
    STDM ValidateDevice(DWORD *n) { return m_obj->ValidateDevice(n); }
    STDM GetInfo(DWORD, void *, DWORD) { return S_FALSE; }
    STDM SetPaletteEntries(UINT n, const PALETTEENTRY *e) { return m_obj->SetPaletteEntries(n, e); }
    STDM GetPaletteEntries(UINT n, PALETTEENTRY *e) { return m_obj->GetPaletteEntries(n, e); }
    STDM SetCurrentTexturePalette(UINT n) { return m_obj->SetCurrentTexturePalette(n); }
    STDM GetCurrentTexturePalette(UINT *n) { return m_obj->GetCurrentTexturePalette(n); }
    STDM DrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT count)
    {
        GfxDraw d = { false, (UINT)t, 0, 0, 0, start, count };
        if (Gfx9Intercept(m_fvf, d, false)) return D3D_OK;
        Gfx9BeforeDraw(m_fvf, false);
        HRESULT hr = m_obj->DrawPrimitive(t, start, count);
        Gfx9DrawDone();
        Gfx9AfterDraw(m_fvf, d);
        return hr;
    }
    STDM DrawIndexedPrimitive(D3DPRIMITIVETYPE t, UINT minIdx, UINT numVerts, UINT start, UINT count)
    {
        GfxDraw d = { true, (UINT)t, m_baseVertex, minIdx, numVerts, start, count };
        if (Gfx9Intercept(m_fvf, d, false)) return D3D_OK;
        Gfx9BeforeDraw(m_fvf, false);
        HRESULT hr = m_obj->DrawIndexedPrimitive(t, m_baseVertex, minIdx, numVerts, start, count);
        Gfx9DrawDone();
        Gfx9AfterDraw(m_fvf, d);
        return hr;
    }
    STDM DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT count, const void *data, UINT stride)
    {
        GfxDraw d = { false, (UINT)t, 0, 0, 0, 0, count };
        if (Gfx9Intercept(m_fvf, d, true)) return D3D_OK;
        Gfx9BeforeDraw(m_fvf, true);
        HRESULT hr = m_obj->DrawPrimitiveUP(t, count, data, stride);
        Gfx9DrawDone();
        return hr;
    }
    STDM DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT minIdx, UINT numVerts, UINT count, const void *idx, D3DFORMAT fmt, const void *data, UINT stride)
    {
        GfxDraw d = { true, (UINT)t, 0, minIdx, numVerts, 0, count };
        if (Gfx9Intercept(m_fvf, d, true)) return D3D_OK;
        Gfx9BeforeDraw(m_fvf, true);
        HRESULT hr = m_obj->DrawIndexedPrimitiveUP(t, minIdx, numVerts, count, idx, fmt, data, stride);
        Gfx9DrawDone();
        return hr;
    }
    STDM ProcessVertices(UINT src, UINT dst, UINT n, VertexBuffer8 *buf, DWORD f) { return m_obj->ProcessVertices(src, dst, n, Real<IDirect3DVertexBuffer9>(buf), NULL, f); }
    // Le jeu ne cree aucun shader : seulement des formats de sommets fixes (FVF).
    STDM CreateVertexShader(const DWORD *, const DWORD *, DWORD *h, DWORD)
    {
        static bool logged;
        if (!logged) { logged = true; Log("pont : CreateVertexShader refuse (non pris en charge)"); }
        if (h) *h = 0;
        return D3DERR_INVALIDCALL;
    }
    STDM SetVertexShader(DWORD h)
    {
        m_vsHandle = h;
        m_fvf = h;
        m_obj->SetVertexShader(NULL);
        return m_obj->SetFVF(h);
    }
    STDM GetVertexShader(DWORD *h) { *h = m_vsHandle; return D3D_OK; }
    STDM DeleteVertexShader(DWORD) { return D3DERR_INVALIDCALL; }
    STDM SetVertexShaderConstant(DWORD r, const void *d, DWORD n) { return m_obj->SetVertexShaderConstantF(r, (const float *)d, n); }
    STDM GetVertexShaderConstant(DWORD r, void *d, DWORD n) { return m_obj->GetVertexShaderConstantF(r, (float *)d, n); }
    STDM GetVertexShaderDeclaration(DWORD, void *, DWORD *) { return D3DERR_INVALIDCALL; }
    STDM GetVertexShaderFunction(DWORD, void *, DWORD *) { return D3DERR_INVALIDCALL; }
    STDM SetStreamSource(UINT n, VertexBuffer8 *b, UINT stride) { return m_obj->SetStreamSource(n, Real<IDirect3DVertexBuffer9>(b), 0, stride); }
    STDM GetStreamSource(UINT n, VertexBuffer8 **out, UINT *stride)
    {
        IDirect3DVertexBuffer9 *b = NULL; UINT off = 0;
        HRESULT hr = m_obj->GetStreamSource(n, &b, &off, stride);
        *out = SUCCEEDED(hr) ? Wrap<VertexBuffer8>(b, this) : NULL;
        return hr;
    }
    STDM SetIndices(IndexBuffer8 *b, UINT base) { m_baseVertex = base; return m_obj->SetIndices(Real<IDirect3DIndexBuffer9>(b)); }
    STDM GetIndices(IndexBuffer8 **out, UINT *base)
    {
        IDirect3DIndexBuffer9 *b = NULL;
        HRESULT hr = m_obj->GetIndices(&b);
        *out = SUCCEEDED(hr) ? Wrap<IndexBuffer8>(b, this) : NULL;
        if (base) *base = m_baseVertex;
        return hr;
    }
    STDM CreatePixelShader(const DWORD *f, DWORD *h)
    {
        IDirect3DPixelShader9 *ps = NULL;
        HRESULT hr = m_obj->CreatePixelShader(f, &ps);
        *h = SUCCEEDED(hr) ? (DWORD)(uintptr_t)ps : 0;
        return hr;
    }
    STDM SetPixelShader(DWORD h) { return m_obj->SetPixelShader((IDirect3DPixelShader9 *)(uintptr_t)h); }
    STDM GetPixelShader(DWORD *h)
    {
        IDirect3DPixelShader9 *ps = NULL;
        HRESULT hr = m_obj->GetPixelShader(&ps);
        if (ps) ps->Release();   // le jeton reste valide : la reference est gardee par la creation
        *h = (DWORD)(uintptr_t)ps;
        return hr;
    }
    STDM DeletePixelShader(DWORD h) { if (h) ((IDirect3DPixelShader9 *)(uintptr_t)h)->Release(); return D3D_OK; }
    STDM SetPixelShaderConstant(DWORD r, const void *d, DWORD n) { return m_obj->SetPixelShaderConstantF(r, (const float *)d, n); }
    STDM GetPixelShaderConstant(DWORD r, void *d, DWORD n) { return m_obj->GetPixelShaderConstantF(r, (float *)d, n); }
    STDM GetPixelShaderFunction(DWORD h, void *d, DWORD *n) { return h ? ((IDirect3DPixelShader9 *)(uintptr_t)h)->GetFunction(d, (UINT *)n) : D3DERR_INVALIDCALL; }
    STDM DrawRectPatch(UINT, const float *, const void *) { return D3DERR_INVALIDCALL; }
    STDM DrawTriPatch(UINT, const float *, const void *) { return D3DERR_INVALIDCALL; }
    STDM DeletePatch(UINT) { return D3DERR_INVALIDCALL; }
};

// CopyRects (D3D8) : selon les pools, UpdateSurface (memoire -> carte), StretchRect (carte -> carte),
// GetRenderTargetData (cible -> memoire), sinon copie ligne a ligne entre surfaces verrouillables.
HRESULT Device8::CopyRects(Surface8 *src8, const RECT *rects, UINT n, Surface8 *dst8, const POINT *pts)
{
    IDirect3DSurface9 *src = Real<IDirect3DSurface9>(src8), *dst = Real<IDirect3DSurface9>(dst8);
    if (!src || !dst) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC sd, dd;
    src->GetDesc(&sd); dst->GetDesc(&dd);
    RECT whole = { 0, 0, (LONG)sd.Width, (LONG)sd.Height };
    if (!rects || !n) { rects = &whole; n = 1; }
    HRESULT hr = D3D_OK;
    if (sd.Pool == D3DPOOL_DEFAULT && dd.Pool == D3DPOOL_SYSTEMMEM && sd.Width == dd.Width && sd.Height == dd.Height && sd.Format == dd.Format && !sd.MultiSampleType)
        return m_obj->GetRenderTargetData(src, dst);
    for (UINT i = 0; i < n; i++) {
        RECT r = rects[i];
        POINT p = pts ? pts[i] : POINT{ r.left, r.top };
        RECT dr = { p.x, p.y, p.x + (r.right - r.left), p.y + (r.bottom - r.top) };
        if (sd.Pool == D3DPOOL_SYSTEMMEM && dd.Pool == D3DPOOL_DEFAULT) hr = m_obj->UpdateSurface(src, &r, dst, &p);
        else if (sd.Pool == D3DPOOL_DEFAULT && dd.Pool == D3DPOOL_DEFAULT) hr = m_obj->StretchRect(src, &r, dst, &dr, D3DTEXF_NONE);
        else {
            D3DLOCKED_RECT sl, dl;
            if (FAILED(hr = src->LockRect(&sl, &r, D3DLOCK_READONLY))) break;
            if (FAILED(hr = dst->LockRect(&dl, &dr, 0))) { src->UnlockRect(); break; }
            UINT rows = r.bottom - r.top, bytes = (r.right - r.left) * FormatBits(sd.Format) / 8;
            if (sd.Format == D3DFMT_DXT1 || sd.Format == D3DFMT_DXT2 || sd.Format == D3DFMT_DXT3 || sd.Format == D3DFMT_DXT4 || sd.Format == D3DFMT_DXT5) {
                rows = (rows + 3) / 4;
                bytes = ((r.right - r.left + 3) / 4) * (sd.Format == D3DFMT_DXT1 ? 8 : 16);
            }
            for (UINT y = 0; y < rows; y++) memcpy((BYTE *)dl.pBits + y * dl.Pitch, (BYTE *)sl.pBits + y * sl.Pitch, bytes);
            dst->UnlockRect(); src->UnlockRect();
        }
        if (FAILED(hr)) break;
    }
    if (FAILED(hr)) Log("pont : CopyRects refuse (0x%08lX, pools %d -> %d)", hr, sd.Pool, dd.Pool);
    return hr;
}

// ======================================================================= IDirect3D8
class D3D8 {
public:
    IDirect3D9 *m_d3d;
    ULONG m_ref;
    std::vector<D3DDISPLAYMODE> m_modes[4];
    bool m_modesBuilt[4];
    D3D8(IDirect3D9 *d) : m_d3d(d), m_ref(1) { memset(m_modesBuilt, 0, sizeof(m_modesBuilt)); }

    STDM QueryInterface(REFIID riid, void **out) { if (!out) return E_POINTER; if (riid == IID_IUnknown) { *out = this; AddRef(); return S_OK; } *out = NULL; return E_NOINTERFACE; }
    virtual ULONG __stdcall AddRef() { return ++m_ref; }
    virtual ULONG __stdcall Release() { ULONG r = --m_ref; if (!r) { m_d3d->Release(); delete this; } return r; }
    STDM RegisterSoftwareDevice(void *f) { return m_d3d->RegisterSoftwareDevice(f); }
    virtual UINT __stdcall GetAdapterCount() { return m_d3d->GetAdapterCount(); }
    STDM GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER8 *id)
    {
        D3DADAPTER_IDENTIFIER9 i9;
        HRESULT hr = m_d3d->GetAdapterIdentifier(a, (f & ENUM8_NO_WHQL_LEVEL) ? 0 : D3DENUM_WHQL_LEVEL, &i9);
        if (FAILED(hr)) return hr;
        memcpy(id->Driver, i9.Driver, 512); memcpy(id->Description, i9.Description, 512);
        id->DriverVersion = i9.DriverVersion; id->VendorId = i9.VendorId; id->DeviceId = i9.DeviceId;
        id->SubSysId = i9.SubSysId; id->Revision = i9.Revision; id->DeviceIdentifier = i9.DeviceIdentifier; id->WHQLLevel = i9.WHQLLevel;
        return hr;
    }
    // D3D8 enumere tous les formats d'un coup ; D3D9 format par format.
    std::vector<D3DDISPLAYMODE> &Modes(UINT a)
    {
        UINT i = a < 4 ? a : 3;
        if (!m_modesBuilt[i]) {
            m_modesBuilt[i] = true;
            static const D3DFORMAT fmts[] = { D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5 };
            for (D3DFORMAT f : fmts) {
                UINT n = m_d3d->GetAdapterModeCount(a, f);
                for (UINT k = 0; k < n; k++) { D3DDISPLAYMODE m; if (SUCCEEDED(m_d3d->EnumAdapterModes(a, f, k, &m))) m_modes[i].push_back(m); }
            }
        }
        return m_modes[i];
    }
    virtual UINT __stdcall GetAdapterModeCount(UINT a) { return (UINT)Modes(a).size(); }
    STDM EnumAdapterModes(UINT a, UINT m, D3DDISPLAYMODE *out) { std::vector<D3DDISPLAYMODE> &v = Modes(a); if (m >= v.size()) return D3DERR_INVALIDCALL; *out = v[m]; return D3D_OK; }
    STDM GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE *m) { return m_d3d->GetAdapterDisplayMode(a, m); }
    STDM CheckDeviceType(UINT a, D3DDEVTYPE t, D3DFORMAT df, D3DFORMAT bf, BOOL w) { return m_d3d->CheckDeviceType(a, t, df, bf, w); }
    STDM CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u, D3DRESOURCETYPE r, D3DFORMAT f) { return m_d3d->CheckDeviceFormat(a, t, af, u, r, f); }
    STDM CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE t, D3DFORMAT f, BOOL w, D3DMULTISAMPLE_TYPE ms) { return m_d3d->CheckDeviceMultiSampleType(a, t, f, w, ms, NULL); }
    STDM CheckDepthStencilMatch(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df) { return m_d3d->CheckDepthStencilMatch(a, t, af, rf, df); }
    STDM GetDeviceCaps(UINT a, D3DDEVTYPE t, D3DCAPS9 *caps8)
    {
        D3DCAPS9 c;
        HRESULT hr = m_d3d->GetDeviceCaps(a, t, &c);
        if (SUCCEEDED(hr)) FixCaps(c, caps8);
        return hr;
    }
    static void FixCaps(D3DCAPS9 &c, D3DCAPS9 *caps8)
    {
        // Le jeu ne doit pas croire aux shaders (on ne traduit pas ceux de D3D8) : versions a 0.
        c.VertexShaderVersion = 0;
        c.PixelShaderVersion = 0;
        c.MaxVertexShaderConst = 0;
        memcpy(caps8, &c, D3DCAPS8_SIZE);
    }
    virtual HMONITOR __stdcall GetAdapterMonitor(UINT a) { return m_d3d->GetAdapterMonitor(a); }
    STDM CreateDevice(UINT a, D3DDEVTYPE t, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS8 *pp, Device8 **out)
    {
        D3DPRESENT_PARAMETERS p9; ConvertPresent(pp, &p9);
        flags &= ~D3DCREATE_PUREDEVICE;   // le rendu moderne relit les etats (Get...)
        IDirect3DDevice9 *d = NULL;
        HRESULT hr = m_d3d->CreateDevice(a, t, focus, flags, &p9, &d);
        if (FAILED(hr) && (flags & D3DCREATE_HARDWARE_VERTEXPROCESSING) == 0 && (flags & D3DCREATE_MIXED_VERTEXPROCESSING) == 0) {
            DWORD f2 = (flags & ~D3DCREATE_SOFTWARE_VERTEXPROCESSING) | D3DCREATE_HARDWARE_VERTEXPROCESSING;
            hr = m_d3d->CreateDevice(a, t, focus, f2, &p9, &d);
        }
        Log("pont : CreateDevice Direct3D 9 %ux%u fmt=%u ms=%u fenetre=%d flags=%08lX -> 0x%08lX", p9.BackBufferWidth, p9.BackBufferHeight,
            p9.BackBufferFormat, p9.MultiSampleType, p9.Windowed, flags, hr);
        if (FAILED(hr)) { *out = NULL; return hr; }
        Device8 *dev = new Device8(d, this);
        AddRef();
        g_device8 = dev;
        g_device9 = d;
        *out = dev;
        Gfx9DeviceCreated(d, p9.BackBufferWidth, p9.BackBufferHeight, p9.MultiSampleType != 0);
        InstallGfx9Hooks();
        return hr;
    }
};

// ======================================================================= Methodes differees
HRESULT Device8::GetDirect3D(D3D8 **out) { *out = m_d3d8; m_d3d8->AddRef(); return D3D_OK; }
HRESULT Device8::GetDeviceCaps(D3DCAPS9 *caps8)
{
    D3DCAPS9 c;
    HRESULT hr = m_obj->GetDeviceCaps(&c);
    if (SUCCEEDED(hr)) D3D8::FixCaps(c, caps8);
    return hr;
}
#define GETDEVICE(Cls) HRESULT Cls::GetDevice(Device8 **out) { *out = m_dev; m_dev->AddRef(); return D3D_OK; }
GETDEVICE(Surface8) GETDEVICE(Volume8) GETDEVICE(Texture8) GETDEVICE(CubeTexture8) GETDEVICE(VolumeTexture8)
GETDEVICE(VertexBuffer8) GETDEVICE(IndexBuffer8)

HRESULT Surface8::GetContainer(REFIID riid, void **out)
{
    // Le jeu demande au plus la texture qui contient la surface : on rend son enveloppe s'il y en a une.
    IDirect3DTexture9 *t = NULL;
    if (SUCCEEDED(m_obj->GetContainer(IID_IDirect3DTexture9, (void **)&t)) && t) { *out = Wrap<Texture8>(t, m_dev); return S_OK; }
    (void)riid;
    *out = NULL;
    return E_NOINTERFACE;
}

// ======================================================================= Entree
static bool g_inited;
void *BridgeCreate8(UINT sdk)
{
    if (!g_inited) { g_inited = true; InitializeCriticalSection(&g_wrapLock); }
    HMODULE m = LoadLibraryA("d3d9.dll");
    Direct3DCreate9_t create = m ? (Direct3DCreate9_t)GetProcAddress(m, "Direct3DCreate9") : NULL;
    IDirect3D9 *d3d = create ? create(D3D_SDK_VERSION) : NULL;
    Log("pont : Direct3DCreate8(%u) -> Direct3D 9 %s", sdk, d3d ? "pret" : "indisponible (Direct3D 8 d'origine)");
    return d3d ? new D3D8(d3d) : NULL;
}

bool IsBridgeDevice(void *dev8) { return dev8 && dev8 == (void *)g_device8; }
IDirect3DDevice9 *BridgeDevice9() { return g_device9; }
