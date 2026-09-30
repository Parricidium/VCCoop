// Test de vcrt64.exe sans le jeu (32 bits, comme le jeu) : sol + cube + soleil, image en caracteres.
//   build\rttest.exe   (vcrt64.exe a cote)
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <vector>
#include "rtshared.h"

static BYTE *g_base;
static uint32_t g_off;
static void *Cmd(uint32_t type, uint32_t bytes)
{
    RtCmd *c = (RtCmd *)(g_base + RT_CMD_OFFSET + g_off);
    c->type = type; c->bytes = bytes;
    g_off += sizeof(RtCmd) + ((bytes + 15) & ~15u);
    return c + 1;
}
static void Mesh(uint32_t id, const std::vector<float> &pos, const std::vector<uint32_t> &idx)
{
    uint32_t nv = (uint32_t)pos.size() / 3, nt = (uint32_t)idx.size() / 3;
    BYTE *p = (BYTE *)Cmd(RT_CMD_MESH, sizeof(RtMesh) + nv * RT_VERTEX_BYTES + nt * 12);
    RtMesh *m = (RtMesh *)p; m->id = id; m->vertices = nv; m->triangles = nt; m->flags = 0;
    memcpy(p + sizeof(RtMesh), pos.data(), nv * 12);
    memset(p + sizeof(RtMesh) + nv * 12, 0, nv * 20);         // pas de normales, uv 0
    memset(p + sizeof(RtMesh) + nv * 32, 0xFF, nv * 4);       // sommets blancs
    memcpy(p + sizeof(RtMesh) + nv * 36, idx.data(), nt * 12);
}

int main()
{
    DWORD pid = GetCurrentProcessId();
    char name[64];
    sprintf_s(name, "Local\\VCCoopRT_%lu", pid);
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, RT_MAP_SIZE, name);
    g_base = (BYTE *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, RT_MAP_SIZE);
    if (!g_base) { printf("memoire partagee impossible\n"); return 1; }
    RtHeader *h = (RtHeader *)g_base;
    h->magic = RT_MAGIC; h->version = RT_PROTOCOL;
    sprintf_s(name, "Local\\VCCoopRT_go_%lu", pid);
    HANDLE go = CreateEventA(NULL, FALSE, FALSE, name);
    sprintf_s(name, "Local\\VCCoopRT_done_%lu", pid);
    HANDLE done = CreateEventA(NULL, FALSE, FALSE, name);

    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(strrchr(exe, '\\') + 1, "vcrt64.exe");
    char cmd[MAX_PATH + 32]; sprintf_s(cmd, "\"%s\" %lu", exe, pid);
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { printf("lancement impossible\n"); return 1; }
    for (int i = 0; i < 200 && h->helperState == 0; i++) Sleep(50);
    printf("etat %u : %s %s\n", h->helperState, h->adapter, h->error);
    if (h->helperState != 1) return 1;

    // Sol 100 x 100 (z = 0), cube 2 m (0..2 en z).
    Mesh(1, { -50, -50, 0, 50, -50, 0, 50, 50, 0, -50, 50, 0 }, { 0, 1, 2, 0, 2, 3 });
    std::vector<float> bp; std::vector<uint32_t> bi;
    for (int i = 0; i < 8; i++) { bp.push_back(i & 1 ? 1.f : -1.f); bp.push_back(i & 2 ? 1.f : -1.f); bp.push_back(i & 4 ? 1.f : -1.f); }
    uint32_t f[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
    for (auto &q : f) { bi.insert(bi.end(), { q[0], q[1], q[2], q[0], q[2], q[3] }); }
    Mesh(2, bp, bi);

    // Camera (conventions Direct3D, vecteurs lignes, main gauche comme RenderWare/le jeu : vue LookAt).
    float eye[3] = { 0, -14, 7 }, at[3] = { 0, 0, 0 }, up[3] = { 0, 0, 1 };
    float z[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
    float zl = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]); for (float &v : z) v /= zl;
    float x[3] = { up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0] };
    float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); for (float &v : x) v /= xl;
    float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    float view[16] = { x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
                       -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]), -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]), -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]), 1 };
    float fov = 1.0f, aspect = 4.0f / 3, zn = 0.5f, zf = 500;
    float ys = 1 / tanf(fov / 2), xs = ys / aspect;
    float proj[16] = { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0 };

    const uint32_t W = 64, H = 32;
    RtFrame *fr = (RtFrame *)Cmd(RT_CMD_FRAME, sizeof(RtFrame) + 2 * sizeof(RtInstance));
    memset(fr, 0, sizeof *fr);
    memcpy(fr->view, view, 64); memcpy(fr->proj, proj, 64);
    float s[3] = { 1, 1, 1.2f }; float sl = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    fr->sun[0] = s[0] / sl; fr->sun[1] = s[1] / sl; fr->sun[2] = s[2] / sl; fr->sun[3] = 1;
    fr->sunAngle = 0.02f; fr->outW = W; fr->outH = H; fr->features = RT_FEAT_SUN | RT_FEAT_AO | RT_FEAT_REFL | RT_FEAT_GI; fr->raysPerPixel = 4;
    fr->instanceCount = 2; fr->maxDistance = 500; fr->aoRadius = 1.5f; fr->wetness = 1;
    for (int k = 0; k < 3; k++) { fr->sunColor[k] = 1; fr->ambient[k] = 0.4f; fr->skyTop[k] = 0.3f; fr->skyBottom[k] = 0.8f; }
    RtInstance *in = (RtInstance *)(fr + 1);
    memset(in, 0, 2 * sizeof(RtInstance));
    float id[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    memcpy(in[0].transform, id, 48); in[0].mesh = 1;
    memcpy(in[1].transform, id, 48); in[1].transform[11] = 1; in[1].mesh = 2;   // cube pose sur le sol
    in[0].tint = 0xFFFFFFFF; in[1].tint = 0xFFFF2020; in[1].flags = RT_INST_VEHICLE;   // cube rouge, "carrosserie" 

    h->cmdBytes = g_off;
    h->frameSeq = 1;
    SetEvent(go);
    DWORD w = WaitForSingleObject(done, 20000);
    printf("image : %s, doneSeq %u, %.2f ms, %u maillages ; %s\n", w == WAIT_OBJECT_0 ? "ok" : "PAS DE REPONSE", h->doneSeq, h->gpuMs, h->meshCount, h->error);
    const uint16_t *px = (const uint16_t *)(g_base + RT_OUT_OFFSET);
    auto half = [](uint16_t v) { uint32_t s = (v >> 15) & 1, e = (v >> 10) & 31, m = v & 1023; float f = e ? ldexpf(1.0f + m / 1024.0f, (int)e - 15) : ldexpf(m / 1024.0f, -14); return s ? -f : f; };
    for (uint32_t yy = 0; yy < H; yy++) {
        for (uint32_t xx = 0; xx < W; xx++) {
            const uint16_t *p = px + (yy * W + xx) * 4;
            float vis = half(p[0]), d = half(p[1]);
            putchar(d <= 0 ? ' ' : vis > 0.75f ? '.' : vis > 0.25f ? '+' : '#');
        }
        putchar('\n');
    }
    const uint16_t *c = px + ((H / 2) * W + W / 2) * 4;
    printf("centre : visibilite %.2f, profondeur %.2f (attendu ~%.1f), occlusion %.2f\n", half(c[0]), half(c[1]), zl, half(c[2]));
    const BYTE *refl = g_base + RT_OUT_OFFSET + W * H * 8, *gi = refl + W * H * 4;
    uint32_t ci = (H / 2) * W + W / 2, gp = (H - 4) * W + W / 2 - 6;   // cube ; sol pres du cube
    printf("reflet du cube : %u %u %u poids %u ; lumiere renvoyee au sol pres du cube : %u %u %u\n",
           refl[ci * 4 + 2], refl[ci * 4 + 1], refl[ci * 4], refl[ci * 4 + 3], gi[gp * 4 + 2], gi[gp * 4 + 1], gi[gp * 4]);
    // occlusion en caracteres
    for (uint32_t yy = 0; yy < H; yy++) {
        for (uint32_t xx = 0; xx < W; xx++) { float a = half(px[(yy * W + xx) * 4 + 2]); putchar(a > 0.9f ? '.' : a > 0.6f ? '+' : '#'); }
        putchar('\n');
    }
    TerminateProcess(pi.hProcess, 0);
    return 0;
}
