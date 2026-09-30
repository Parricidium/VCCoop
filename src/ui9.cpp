// Interface moderne du menu en jeu (F10), dessinee sur le peripherique Direct3D 9 du pont (voir ui9.h).
//
// Une liste de quadrilateres par image, dessinee d'un coup a la fin de PanelDraw (dans la scene du jeu, apres son
// interface) par un seul shader :
//   - formes : rectangle arrondi par distance signee (bords lisses au pixel), degrade, liserÃ©, ombre douce ;
//   - verre : l'image du jeu, reduite au quart et floutee (4 passes), teintee sous le panneau ;
//   - texte : police Segoe UI (normale et grasse) rendue par Windows (GDI) dans une texture au lancement, avec
//     ses niveaux reduits (lisible de 12 a 40 pixels).
#include "util.h"
#include "vccoop.h"
#include "bridge.h"
#include "ui9.h"
#include <d3d9.h>
#include <d3dcompiler.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <xmmintrin.h>
#pragma comment(lib, "gdi32.lib")   // police : rendue par GDI

// Etat flottant du jeu garde tel quel (le compilateur HLSL et certains pilotes le changent : voir gfx9.cpp).
static unsigned short GetFpuCw() { unsigned short c; __asm { fnstcw c } return c; }
static void SetFpuCw(unsigned short c) { __asm { fnclex } __asm { fldcw c } }
struct FpuGuard {
    unsigned short cw; unsigned int csr;
    FpuGuard() : cw(GetFpuCw()), csr(_mm_getcsr()) {}
    ~FpuGuard() { SetFpuCw(cw); _mm_setcsr(csr); }
};

struct UiVtx {
    float x, y, z, w;
    DWORD c0, c1;          // remplissage, liserÃ©
    float lx, ly;          // position par rapport au centre de la forme (pixels)
    float hw, hh, r, bw;   // demi-largeur, demi-hauteur, rayon, epaisseur du liserÃ©
    float mode, spread, u, v;   // mode : 0 forme, 1 verre, 2 texte, 3 aplat, 4 ombre
};
static const DWORD kFvf = D3DFVF_XYZW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX3 |
                          D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE4(1) | D3DFVF_TEXCOORDSIZE4(2);

static std::vector<UiVtx> g_v;
static bool g_glass;
static IDirect3DDevice9 *g_dev;
static IDirect3DVertexShader9 *g_vs;
static IDirect3DPixelShader9 *g_ps, *g_psBlur;
static IDirect3DTexture9 *g_font;
static IDirect3DTexture9 *g_bg[2];
static IDirect3DSurface9 *g_bgSurf[2];
static UINT g_bgW, g_bgH;
static IDirect3DStateBlock9 *g_state;
static bool g_failed, g_sharedOk;

// ---------------------------------------------------------------- police
enum { ATLAS = 1024, MIPS = 3 };
static const float kEm = 40;   // (2 graisses x 224 caracteres dans 1024 x 1024)
struct Glyph { float u0, v0, u1, v1, w, h, ox, adv; };
static Glyph g_gl[2][256];
static float g_cellH = 64;

static bool BuildFont(std::vector<DWORD> &px)
{
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return false;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = ATLAS; bi.bmiHeader.biHeight = -ATLAS;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { DeleteDC(dc); return false; }
    HGDIOBJ oldBm = SelectObject(dc, bm);
    memset(bits, 0, ATLAS * ATLAS * 4);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    int x = 2, y = 2;
    bool ok = true;
    for (int wgt = 0; wgt < 2 && ok; wgt++) {
        HFONT f = CreateFontW(-(int)kEm, 0, 0, 0, wgt ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldF = SelectObject(dc, f);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        int cellH = tm.tmHeight;
        g_cellH = (float)cellH;
        if (x > 2) { x = 2; y += cellH + 6; }
        for (int c = 32; c < 256; c++) {
            char ch = (char)c;
            wchar_t wc = 0;
            MultiByteToWideChar(1252, 0, &ch, 1, &wc, 1);
            ABC abc = {};
            if (!GetCharABCWidthsW(dc, wc, wc, &abc)) { SIZE s; GetTextExtentPoint32W(dc, &wc, 1, &s); abc.abcA = 0; abc.abcB = s.cx; abc.abcC = 0; }
            int cellW = (int)abc.abcB + 4;
            if (x + cellW + 2 > ATLAS) { x = 2; y += cellH + 6; }
            if (y + cellH + 2 > ATLAS) { ok = false; break; }
            TextOutW(dc, x + 2 - abc.abcA, y, &wc, 1);
            Glyph &g = g_gl[wgt][c];
            g.u0 = (float)x / ATLAS; g.v0 = (float)y / ATLAS;
            g.u1 = (float)(x + cellW) / ATLAS; g.v1 = (float)(y + cellH) / ATLAS;
            g.w = (float)cellW; g.h = (float)cellH;
            g.ox = (float)abc.abcA - 2;
            g.adv = (float)(abc.abcA + (int)abc.abcB + abc.abcC);
            x += cellW + 6;   // (marge : niveaux reduits sans bavure entre lettres)
        }
        SelectObject(dc, oldF);
        DeleteObject(f);
    }
    GdiFlush();
    px.resize(ATLAS * ATLAS);
    const BYTE *b = (const BYTE *)bits;
    for (int i = 0; i < ATLAS * ATLAS; i++) {
        BYTE a = b[i * 4] > b[i * 4 + 1] ? b[i * 4] : b[i * 4 + 1];
        if (b[i * 4 + 2] > a) a = b[i * 4 + 2];
        px[i] = ((DWORD)a << 24) | 0xFFFFFF;
    }
    SelectObject(dc, oldBm);
    DeleteObject(bm);
    DeleteDC(dc);
    return ok;
}

static bool CreateFontTexture()
{
    std::vector<DWORD> px;
    if (!BuildFont(px)) { Log("interface : police Segoe UI impossible"); return false; }
    if (FAILED(g_dev->CreateTexture(ATLAS, ATLAS, MIPS, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_font, NULL))) return false;
    int size = ATLAS;
    std::vector<DWORD> cur = px, next;
    for (int l = 0; l < MIPS; l++) {
        D3DLOCKED_RECT lr;
        if (FAILED(g_font->LockRect(l, &lr, NULL, 0))) return false;
        for (int y = 0; y < size; y++) memcpy((BYTE *)lr.pBits + y * lr.Pitch, &cur[y * size], size * 4);
        g_font->UnlockRect(l);
        int ns = size / 2;
        next.assign(ns * ns, 0);
        for (int y = 0; y < ns; y++)
            for (int x = 0; x < ns; x++) {
                DWORD a = (cur[(2 * y) * size + 2 * x] >> 24) + (cur[(2 * y) * size + 2 * x + 1] >> 24) +
                          (cur[(2 * y + 1) * size + 2 * x] >> 24) + (cur[(2 * y + 1) * size + 2 * x + 1] >> 24);
                next[y * ns + x] = ((a / 4) << 24) | 0xFFFFFF;
            }
        cur.swap(next);
        size = ns;
    }
    return true;
}

// ---------------------------------------------------------------- shaders
static const char kUiHlsl[] = R"HLSL(
float4 gScreen : register(c0);   // largeur, hauteur, 1/largeur, 1/hauteur
float4 gDir : register(c1);      // flou : pas (texels)
struct VIn { float4 pos : POSITION; float4 c0 : COLOR0; float4 c1 : COLOR1; float2 lp : TEXCOORD0; float4 rect : TEXCOORD1; float4 m : TEXCOORD2; };
struct VOut { float4 pos : POSITION; float4 c0 : COLOR0; float4 c1 : COLOR1; float2 lp : TEXCOORD0; float4 rect : TEXCOORD1; float4 m : TEXCOORD2; };
VOut Vs(VIn i) {
  VOut o;
  o.pos = float4((i.pos.x - 0.5) * gScreen.z * 2 - 1, 1 - (i.pos.y - 0.5) * gScreen.w * 2, 0, 1);
  o.c0 = i.c0; o.c1 = i.c1; o.lp = i.lp; o.rect = i.rect; o.m = i.m;
  return o;
}

sampler2D sFont : register(s0);
sampler2D sGlass : register(s1);
float Sd(float2 p, float2 b, float r) { float2 q = abs(p) - b + r; return length(max(q, 0)) + min(max(q.x, q.y), 0) - r; }

float4 Ps(float4 c0 : COLOR0, float4 c1 : COLOR1, float2 lp : TEXCOORD0, float4 rect : TEXCOORD1, float4 m : TEXCOORD2, float2 vpos : VPOS) : COLOR {
  if (m.x > 1.5 && m.x < 2.5) return float4(c0.rgb, c0.a * tex2D(sFont, m.zw).a);   // texte
  if (m.x > 2.5 && m.x < 3.5) return c0;                                              // aplat
  float d = Sd(lp, rect.xy, rect.z);
  if (m.x > 3.5) { float s = saturate(1 - max(d, 0) / m.y); return float4(c0.rgb, c0.a * s * s); }   // ombre
  float4 col = c0;
  if (m.x > 0.5) {   // verre : image floutee, teintee ; reflet doux en haut
    float3 bg = tex2D(sGlass, (vpos + 0.5) * gScreen.zw).rgb;
    col = float4(lerp(bg, c0.rgb, c0.a), 1);
    float t = saturate((lp.y + rect.y) / max(rect.y * 2, 1));
    col.rgb += (1 - t) * (1 - t) * 0.07;
  }
  if (rect.w > 0) {   // liserÃ©
    float e = saturate(0.5 - (abs(d + rect.w * 0.5) - rect.w * 0.5));
    col.rgb = lerp(col.rgb, c1.rgb, e * c1.a);
    col.a = max(col.a, e * c1.a);
  }
  col.a *= saturate(0.5 - d);
  return col;
}

// Flou gaussien (9 lectures) le long de gDir.
float4 PsBlur(float2 vpos : VPOS) : COLOR {
  float2 uv = (vpos + 0.5) * gScreen.zw;
  float w[5] = { 0.2270, 0.1945, 0.1216, 0.0540, 0.0162 };
  float3 s = tex2D(sFont, uv).rgb * w[0];
  [unroll] for (int k = 1; k < 5; k++) {
    s += tex2D(sFont, uv + gDir.xy * k).rgb * w[k];
    s += tex2D(sFont, uv - gDir.xy * k).rgb * w[k];
  }
  return float4(s, 1);
}
)HLSL";

typedef HRESULT(WINAPI *D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
static IUnknown *Compile(D3DCompile_t comp, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    if (FAILED(comp(kUiHlsl, sizeof(kUiHlsl) - 1, "vccoop-ui", NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        Log("interface : shader %s refuse : %.400s", entry, err ? (const char *)err->GetBufferPointer() : "?");
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

// Ressources durables (shaders, police) : une fois ; cibles du flou : a la taille de l'ecran, refaites apres Reset.
static bool EnsureShared()
{
    if (g_sharedOk) return true;
    if (g_failed) return false;
    g_dev = BridgeDevice9();
    if (!g_dev) return false;
    FpuGuard fpu;
    g_failed = true;
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompile_t comp = m ? (D3DCompile_t)GetProcAddress(m, "D3DCompile") : NULL;
    if (!comp) { Log("interface : d3dcompiler_47.dll introuvable, menu d'origine"); return false; }
    g_vs = (IDirect3DVertexShader9 *)Compile(comp, "Vs", "vs_3_0");
    g_ps = (IDirect3DPixelShader9 *)Compile(comp, "Ps", "ps_3_0");
    g_psBlur = (IDirect3DPixelShader9 *)Compile(comp, "PsBlur", "ps_3_0");
    if (!g_vs || !g_ps || !g_psBlur || !CreateFontTexture()) { Log("interface : ressources impossibles, menu d'origine"); return false; }
    g_failed = false;
    g_sharedOk = true;
    Log("interface : menu moderne pret (verre, Segoe UI)");
    return true;
}

bool UiReady() { return ModernRenderer() && BridgeDevice9() && EnsureShared(); }

void UiRelease()
{
    for (int i = 0; i < 2; i++) { if (g_bgSurf[i]) { g_bgSurf[i]->Release(); g_bgSurf[i] = NULL; } if (g_bg[i]) { g_bg[i]->Release(); g_bg[i] = NULL; } }
    if (g_state) { g_state->Release(); g_state = NULL; }
    g_bgW = g_bgH = 0;
}

// ---------------------------------------------------------------- liste
static DWORD Argb(uint32_t rgba) { return ((rgba & 0xFF) << 24) | (rgba >> 8); }

void UiBegin() { g_v.clear(); g_glass = false; }

static void Push(float x, float y, DWORD c0, DWORD c1, float lx, float ly, float hw, float hh, float r, float bw, float mode, float spread, float u, float v)
{
    UiVtx t = { x, y, 0, 1, c0, c1, lx, ly, hw, hh, r, bw, mode, spread, u, v };
    g_v.push_back(t);
}

static void Shape(float x0, float y0, float x1, float y1, float r, DWORD tl, DWORD tr, DWORD bl, DWORD br, DWORD border, float bw, float mode, float pad, float spread)
{
    if (x1 <= x0 || y1 <= y0) return;
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f;
    float mr = hw < hh ? hw : hh;
    if (r > mr) r = mr;
    float X0 = x0 - pad, Y0 = y0 - pad, X1 = x1 + pad, Y1 = y1 + pad;
    Push(X0, Y0, tl, border, X0 - cx, Y0 - cy, hw, hh, r, bw, mode, spread, 0, 0);
    Push(X1, Y0, tr, border, X1 - cx, Y0 - cy, hw, hh, r, bw, mode, spread, 0, 0);
    Push(X0, Y1, bl, border, X0 - cx, Y1 - cy, hw, hh, r, bw, mode, spread, 0, 0);
    Push(X0, Y1, bl, border, X0 - cx, Y1 - cy, hw, hh, r, bw, mode, spread, 0, 0);
    Push(X1, Y0, tr, border, X1 - cx, Y0 - cy, hw, hh, r, bw, mode, spread, 0, 0);
    Push(X1, Y1, br, border, X1 - cx, Y1 - cy, hw, hh, r, bw, mode, spread, 0, 0);
}

void UiRect(float x0, float y0, float x1, float y1, float r, uint32_t top, uint32_t bottom, uint32_t border, float bw)
{
    Shape(x0, y0, x1, y1, r, Argb(top), Argb(top), Argb(bottom), Argb(bottom), Argb(border), bw, 0, 1, 0);
}
void UiRectH(float x0, float y0, float x1, float y1, float r, uint32_t left, uint32_t right, uint32_t border, float bw)
{
    Shape(x0, y0, x1, y1, r, Argb(left), Argb(right), Argb(left), Argb(right), Argb(border), bw, 0, 1, 0);
}
void UiGlass(float x0, float y0, float x1, float y1, float r, uint32_t tint, uint32_t border, float bw)
{
    g_glass = true;
    Shape(x0, y0, x1, y1, r, Argb(tint), Argb(tint), Argb(tint), Argb(tint), Argb(border), bw, 1, 1, 0);
}
void UiShadow(float x0, float y0, float x1, float y1, float r, float spread, uint32_t color)
{
    Shape(x0, y0, x1, y1, r, Argb(color), Argb(color), Argb(color), Argb(color), 0, 0, 4, spread, spread);
}
void UiTri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color)
{
    DWORD c = Argb(color);
    Push(x0, y0, c, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0);
    Push(x1, y1, c, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0);
    Push(x2, y2, c, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0);
}

float UiTextWidth(const char *s, float px, bool bold)
{
    float k = px / g_cellH, w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p >= 32) w += g_gl[bold][*p].adv * k;
    return w;
}

float UiText(float x, float y, float px, uint32_t color, int align, const char *s, bool bold)
{
    if (!s || !*s || !g_sharedOk) return 0;
    float k = px / g_cellH, w = UiTextWidth(s, px, bold);
    if (align == UI_CENTER) x -= w * 0.5f;
    else if (align == UI_RIGHT) x -= w;
    x = floorf(x + 0.5f); y = floorf(y + 0.5f);
    DWORD c = Argb(color);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 32) continue;
        const Glyph &g = g_gl[bold][*p];
        if (*p != ' ') {
            float gx0 = x + g.ox * k, gy0 = y, gx1 = gx0 + g.w * k, gy1 = y + g.h * k;
            Push(gx0, gy0, c, 0, 0, 0, 0, 0, 0, 0, 2, 0, g.u0, g.v0);
            Push(gx1, gy0, c, 0, 0, 0, 0, 0, 0, 0, 2, 0, g.u1, g.v0);
            Push(gx0, gy1, c, 0, 0, 0, 0, 0, 0, 0, 2, 0, g.u0, g.v1);
            Push(gx0, gy1, c, 0, 0, 0, 0, 0, 0, 0, 2, 0, g.u0, g.v1);
            Push(gx1, gy0, c, 0, 0, 0, 0, 0, 0, 0, 2, 0, g.u1, g.v0);
            Push(gx1, gy1, c, 0, 0, 0, 0, 0, 0, 0, 2, 0, g.u1, g.v1);
        }
        x += g.adv * k;
    }
    return w;
}

// ---------------------------------------------------------------- dessin
static void FullQuad(float w, float h)
{
    UiVtx q[6] = {};
    float xy[6][2] = { { 0, 0 }, { w, 0 }, { 0, h }, { 0, h }, { w, 0 }, { w, h } };
    for (int i = 0; i < 6; i++) { q[i].x = xy[i][0]; q[i].y = xy[i][1]; q[i].w = 1; }
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(UiVtx));
}

// Image du jeu derriere le panneau : au quart, floutee deux fois dans chaque sens.
static bool BlurBackground(IDirect3DSurface9 *rt, UINT W, UINT H)
{
    UINT bw = W / 4 ? W / 4 : 1, bh = H / 4 ? H / 4 : 1;
    if (!g_bg[0] || g_bgW != bw || g_bgH != bh) {
        for (int i = 0; i < 2; i++) { if (g_bgSurf[i]) { g_bgSurf[i]->Release(); g_bgSurf[i] = NULL; } if (g_bg[i]) { g_bg[i]->Release(); g_bg[i] = NULL; } }
        for (int i = 0; i < 2; i++) {
            if (FAILED(g_dev->CreateTexture(bw, bh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_bg[i], NULL))) return false;
            g_bg[i]->GetSurfaceLevel(0, &g_bgSurf[i]);
        }
        g_bgW = bw; g_bgH = bh;
    }
    if (FAILED(g_dev->StretchRect(rt, NULL, g_bgSurf[0], NULL, D3DTEXF_LINEAR))) return false;
    g_dev->SetPixelShader(g_psBlur);
    float sc[4] = { (float)bw, (float)bh, 1.0f / bw, 1.0f / bh };
    g_dev->SetVertexShaderConstantF(0, sc, 1);
    g_dev->SetPixelShaderConstantF(0, sc, 1);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    for (int pass = 0; pass < 4; pass++) {
        int src = pass & 1;
        g_dev->SetRenderTarget(0, g_bgSurf[src ^ 1]);
        g_dev->SetTexture(0, g_bg[src]);
        float dir[4] = { (pass & 1) ? 0.0f : 1.5f / bw, (pass & 1) ? 1.5f / bh : 0.0f, 0, 0 };
        g_dev->SetPixelShaderConstantF(1, dir, 1);
        FullQuad((float)bw, (float)bh);
    }
    g_dev->SetRenderTarget(0, rt);
    return true;
}

void UiEnd()
{
    if (g_v.empty()) return;
    if (!g_sharedOk || !g_dev) { g_v.clear(); return; }
    FpuGuard fpu;
    if (!g_state && FAILED(g_dev->CreateStateBlock(D3DSBT_ALL, &g_state))) { g_v.clear(); return; }
    g_state->Capture();
    IDirect3DSurface9 *rt = NULL;
    if (FAILED(g_dev->GetRenderTarget(0, &rt)) || !rt) { g_v.clear(); return; }
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    UINT W = d.Width, H = d.Height;

    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    g_dev->SetVertexShader(g_vs);
    g_dev->SetFVF(kFvf);
    for (int s = 0; s < 2; s++) {
        g_dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        g_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        g_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, s == 0 ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    bool glass = g_glass && BlurBackground(rt, W, H);

    D3DVIEWPORT9 vp = { 0, 0, W, H, 0, 1 };
    g_dev->SetViewport(&vp);
    float sc[4] = { (float)W, (float)H, 1.0f / W, 1.0f / H };
    g_dev->SetVertexShaderConstantF(0, sc, 1);
    g_dev->SetPixelShaderConstantF(0, sc, 1);
    g_dev->SetPixelShader(g_ps);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    float bias = -0.4f;
    g_dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, *(DWORD *)&bias);
    g_dev->SetTexture(0, g_font);
    g_dev->SetTexture(1, glass ? g_bg[0] : NULL);
    const UINT chunk = 3 * 20000;
    for (UINT i = 0; i < g_v.size(); i += chunk) {
        UINT n = (UINT)g_v.size() - i;
        if (n > chunk) n = chunk;
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, &g_v[i], sizeof(UiVtx));
    }
    g_dev->SetTexture(0, NULL);
    g_dev->SetTexture(1, NULL);
    g_state->Apply();
    rt->Release();
    g_v.clear();
    g_glass = false;
}
