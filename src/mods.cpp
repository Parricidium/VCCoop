// Mods partages ("streaming" a la FiveM) : ModsPartages=1.
// Dossier VCCoop\mods\ (sous-dossiers libres, leur nom ne sert qu'a s'y retrouver) :
//  - nom.dff / nom.txd / nom.col / nom.ifp : remplacent le modele "nom" du jeu (voiture, arme, batiment...) ;
//  - handling.cfg (ou *.handling) : lignes de conduite, remplacent la ligne du jeu dont le premier mot (nom du
//    vehicule) est le meme ; carcols.dat : pareil pour les couleurs.
// A l'initialisation du jeu, un vccmods.img/.dir (a la racine du jeu : le jeu ne garde que 15 caracteres de nom
// d'image) est fabrique et ajoute en DERNIER : CStreaming::LoadCdDirectory lit les images de la derniere a la
// premiere et ne garde que la premiere entree d'un nom, la notre gagne donc sur celle de gta3.img.
// Les fichiers de conduite / couleurs fusionnes sont ecrits dans VCCoop\cache\ et le jeu les lit a la place des siens
// (CFileMgr::OpenFile detourne).
// Distribution : l'hote sert le dossier en TCP (meme numero de port que l'UDP) ; chaque invite compare le manifeste
// (chemin, taille, empreinte) au sien et telecharge ce qui manque dans son VCCoop\mods\, avant que la partie
// commence (le salon attend). Il ne charge que les fichiers du manifeste de l'hote : memes modeles chez tous.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

using namespace game;

struct ModFile { std::string rel; uint32_t size, hash; };
static std::vector<ModFile> g_local;      // ce qu'il y a dans notre dossier
static std::vector<ModFile> g_manifest;   // invite : ce que l'hote sert (sinon = g_local)
static CRITICAL_SECTION g_lock;
static volatile long g_modsTotal, g_modsDone, g_modsFailed;
static volatile bool g_manifestKnown;    // invite : manifeste recu (meme vide)
static HANDLE g_server, g_client;

static std::string ModsDir() { return std::string(GameDir()) + "VCCoop\\mods\\"; }
static std::string CacheDir() { return std::string(GameDir()) + "VCCoop\\cache\\"; }

static uint32_t Fnv(const uint8_t *d, size_t n, uint32_t h = 2166136261u)
{
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
    return h;
}

static bool HashFile(const std::string &path, uint32_t &size, uint32_t &hash)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    static uint8_t buf[65536];
    uint32_t h = 2166136261u, n = 0;
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) { h = Fnv(buf, r, h); n += (uint32_t)r; }
    fclose(f);
    size = n; hash = h;
    return true;
}

static void Scan(const std::string &dir, const std::string &rel, std::vector<ModFile> &out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        std::string r = rel + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { Scan(dir + fd.cFileName + "\\", r + "\\", out); continue; }
        ModFile m;
        m.rel = r;
        if (r.size() < 200 && HashFile(dir + fd.cFileName, m.size, m.hash) && m.size < 64u * 1024 * 1024) out.push_back(m);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void ScanLocal()
{
    std::vector<ModFile> v;
    Scan(ModsDir(), "", v);
    EnterCriticalSection(&g_lock);
    g_local = v;
    LeaveCriticalSection(&g_lock);
}

static bool SafeRel(const std::string &r)
{
    if (r.empty() || r.size() > 200 || r[0] == '\\' || r[0] == '/' || r.find(':') != std::string::npos || r.find("..") != std::string::npos) return false;
    for (char c : r) if ((unsigned char)c < 32) return false;
    return true;
}

static void MakeDirs(const std::string &path)   // dossiers parents d'un fichier
{
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '\\') CreateDirectoryA(path.substr(0, i).c_str(), NULL);
}

// ======================================================================= Chargement par le jeu
static std::string Lower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string BaseName(const std::string &rel) { size_t p = rel.rfind('\\'); return p == std::string::npos ? rel : rel.substr(p + 1); }
static std::string Ext(const std::string &name) { size_t p = name.rfind('.'); return p == std::string::npos ? "" : Lower(name.substr(p)); }

// Fichiers a utiliser : le manifeste de l'hote chez un invite (ceux qu'on a, a la bonne empreinte), sinon le dossier.
static std::vector<ModFile> Active()
{
    EnterCriticalSection(&g_lock);
    std::vector<ModFile> out = g_manifestKnown ? g_manifest : g_local;
    std::vector<ModFile> local = g_local;
    LeaveCriticalSection(&g_lock);
    if (!g_manifestKnown) return out;
    std::vector<ModFile> have;
    for (auto &m : out)
        for (auto &l : local) if (l.rel == m.rel && l.hash == m.hash && l.size == m.size) { have.push_back(m); break; }
    return have;
}

static bool g_imgReady;

static bool BuildImg(const std::vector<ModFile> &files)
{
    std::string cache = CacheDir(), root = GameDir();
    CreateDirectoryA((root + "VCCoop").c_str(), NULL);
    CreateDirectoryA(cache.c_str(), NULL);
    FILE *img = fopen((root + "vccmods.img").c_str(), "wb");
    FILE *dir = fopen((root + "vccmods.dir").c_str(), "wb");
    if (!img || !dir) { if (img) fclose(img); if (dir) fclose(dir); Log("mods : impossible d'ecrire vccmods.img dans %s", root.c_str()); return false; }
    std::vector<std::string> seen;
    uint32_t block = 0;
    int n = 0;
    static uint8_t buf[65536];
    for (auto &m : files) {
        std::string name = BaseName(m.rel), e = Ext(name);
        if (e != ".dff" && e != ".txd" && e != ".col" && e != ".ifp") continue;
        if (name.size() > 23) { Log("mods : nom trop long pour le jeu (23 max) : %s", name.c_str()); continue; }
        bool dup = false;
        for (auto &s : seen) if (Lower(s) == Lower(name)) dup = true;
        if (dup) { Log("mods : %s en double, le premier est garde", m.rel.c_str()); continue; }
        FILE *f = fopen((ModsDir() + m.rel).c_str(), "rb");
        if (!f) continue;
        seen.push_back(name);
        uint32_t written = 0;
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0) { fwrite(buf, 1, r, img); written += (uint32_t)r; }
        fclose(f);
        uint32_t blocks = (written + 2047) / 2048;
        for (uint32_t pad = written; pad < blocks * 2048; pad++) fputc(0, img);
        uint8_t entry[32] = {};
        memcpy(entry, &block, 4);
        memcpy(entry + 4, &blocks, 4);
        memcpy(entry + 8, name.c_str(), name.size());
        fwrite(entry, 1, 32, dir);
        block += blocks;
        n++;
    }
    fclose(img);
    fclose(dir);
    Log("mods : %d modeles dans vccmods.img (%u Ko)", n, block * 2);
    return n > 0;
}

// Conduite (handling.cfg) et couleurs (carcols.dat) : lignes du mod fusionnees par nom de vehicule.
static bool ReadLines(const std::string &path, std::vector<std::string> &lines)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string all;
    char b[4096];
    size_t r;
    while ((r = fread(b, 1, sizeof(b), f)) > 0) all.append(b, r);
    fclose(f);
    size_t p = 0;
    while (p <= all.size()) {
        size_t e = all.find('\n', p);
        if (e == std::string::npos) e = all.size();
        std::string l = all.substr(p, e - p);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        p = e + 1;
    }
    return true;
}

static std::string FirstWord(const std::string &l, bool carcols)
{
    size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) i++;
    size_t j = i;
    while (j < l.size() && l[j] != ' ' && l[j] != '\t' && !(carcols && l[j] == ',')) j++;
    return Lower(l.substr(i, j - i));
}

static int MergeData(const char *gameFile, const std::vector<ModFile> &files, bool carcols, const char *outName)
{
    std::vector<std::string> mod;
    for (auto &m : files) {
        std::string name = Lower(BaseName(m.rel));
        bool ok = carcols ? name == "carcols.dat" : (name == "handling.cfg" || name == "handling.txt" || Ext(name) == ".handling");
        if (!ok) continue;
        std::vector<std::string> l;
        if (ReadLines(ModsDir() + m.rel, l)) for (auto &x : l) if (!x.empty() && x[0] != ';' && x[0] != '#') mod.push_back(x);
    }
    if (mod.empty()) return 0;
    std::vector<std::string> game;
    if (!ReadLines(std::string(GameDir()) + gameFile, game)) return 0;
    int replaced = 0;
    for (auto &g : game) {
        std::string w = FirstWord(g, carcols);
        if (w.empty() || w == "end" || w == "car" || w == "col") continue;
        for (auto &m : mod) if (FirstWord(m, carcols) == w) { g = m; replaced++; break; }
    }
    FILE *f = fopen((CacheDir() + outName).c_str(), "wb");
    if (!f) return 0;
    for (auto &g : game) { fputs(g.c_str(), f); fputs("\r\n", f); }
    fclose(f);
    Log("mods : %s, %d lignes remplacees", outName, replaced);
    return replaced;
}

static bool g_handling, g_carcols;

static void Build()
{
    if (!g_cfg.sharedMods) return;
    ScanLocal();
    std::vector<ModFile> files = Active();
    g_imgReady = BuildImg(files);
    g_handling = MergeData("data\\handling.cfg", files, false, "handling.cfg") > 0;
    g_carcols = MergeData("data\\carcols.dat", files, true, "carcols.dat") > 0;
}

// CStreaming::LoadCdDirectory() (0x40FE00, toutes les images, a chaque initialisation du jeu) : juste avant, notre
// image est ajoutee en dernier (CdStreamAddImage 0x4081E0 ; nombre d'images 0x6F76C8, noms 0x6F7488 + 16 * i).
typedef void(__cdecl *LoadDirs_t)();
static LoadDirs_t o_LoadDirs;
static void __cdecl h_LoadDirs()
{
    Build();
    if (g_imgReady) {
        int n = *(int *)0x6F76C8;
        bool have = false;
        for (int i = 0; i < n; i++) if (_stricmp((const char *)(0x6F7488 + i * 0x10), "vccmods.img") == 0) have = true;
        if (!have) { ((int(__cdecl *)(const char *))0x4081E0)("vccmods.img"); Log("mods : vccmods.img ajoute (image %d)", n); }
    }
    o_LoadDirs();
}

// CFileMgr::OpenFile (0x48DF90) : conduite et couleurs fusionnees a la place des fichiers du jeu.
typedef void *(__cdecl *OpenFile_t)(const char *, const char *);
static OpenFile_t o_OpenFile;
static void *__cdecl h_OpenFile(const char *name, const char *mode)
{
    if (name) {
        size_t n = strlen(name);
        if (g_handling && n >= 12 && _stricmp(name + n - 12, "handling.cfg") == 0) return o_OpenFile("VCCoop\\cache\\handling.cfg", mode);
        if (g_carcols && n >= 11 && _stricmp(name + n - 11, "carcols.dat") == 0) return o_OpenFile("VCCoop\\cache\\carcols.dat", mode);
    }
    return o_OpenFile(name, mode);
}

// ======================================================================= Distribution (TCP)
static bool SendAll(SOCKET s, const void *d, int n)
{
    const char *p = (const char *)d;
    while (n > 0) { int r = send(s, p, n, 0); if (r <= 0) return false; p += r; n -= r; }
    return true;
}
static bool RecvAll(SOCKET s, void *d, int n)
{
    char *p = (char *)d;
    while (n > 0) { int r = recv(s, p, n, 0); if (r <= 0) return false; p += r; n -= r; }
    return true;
}

static DWORD WINAPI ServeClient(void *param)
{
    SOCKET s = (SOCKET)param;
    DWORD to = 30000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof(to));
    char magic[4];
    if (!RecvAll(s, magic, 4) || memcmp(magic, "VCM1", 4)) { closesocket(s); return 0; }
    EnterCriticalSection(&g_lock);
    std::vector<ModFile> list = g_local;
    LeaveCriticalSection(&g_lock);
    uint32_t count = (uint32_t)list.size();
    SendAll(s, &count, 4);
    for (auto &m : list) {
        uint16_t len = (uint16_t)m.rel.size();
        SendAll(s, &len, 2); SendAll(s, m.rel.data(), len); SendAll(s, &m.size, 4); SendAll(s, &m.hash, 4);
    }
    static uint8_t buf[65536];
    for (;;) {
        char cmd[4];
        if (!RecvAll(s, cmd, 4) || memcmp(cmd, "GET ", 4)) break;
        uint16_t len;
        if (!RecvAll(s, &len, 2) || len > 200) break;
        std::string rel(len, 0);
        if (!RecvAll(s, &rel[0], len) || !SafeRel(rel)) break;
        FILE *f = fopen((ModsDir() + rel).c_str(), "rb");
        uint32_t size = 0;
        if (f) { fseek(f, 0, SEEK_END); size = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET); }
        if (!SendAll(s, &size, 4)) break;
        bool ok = true;
        if (f) {
            size_t r;
            while (ok && (r = fread(buf, 1, sizeof(buf), f)) > 0) ok = SendAll(s, buf, (int)r);
            fclose(f);
        }
        if (!ok) break;
        Log("mods : %s envoye (%u octets)", rel.c_str(), size);
    }
    closesocket(s);
    return 0;
}

static DWORD WINAPI ServerThread(void *)
{
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons((u_short)g_cfg.port);
    if (bind(ls, (sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 4) != 0) { Log("mods : port TCP %d indisponible (%d)", g_cfg.port, WSAGetLastError()); closesocket(ls); return 0; }
    Log("mods : serveur TCP sur le port %d (%d fichiers)", g_cfg.port, (int)g_local.size());
    for (;;) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        CloseHandle(CreateThread(NULL, 0, ServeClient, (void *)c, 0, NULL));
    }
    closesocket(ls);
    return 0;
}

static DWORD WINAPI ClientThread(void *)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD to = 30000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof(to));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_cfg.port);
    addrinfo hints = {}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_cfg.address, NULL, &hints, &res) != 0 || !res) { g_modsFailed = 1; return 0; }
    a.sin_addr = ((sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    if (connect(s, (sockaddr *)&a, sizeof(a)) != 0 || !SendAll(s, "VCM1", 4)) {
        Log("mods : pas de serveur de mods chez l'hote (%d) : on joue sans", WSAGetLastError());
        closesocket(s);
        g_manifestKnown = true;   // manifeste vide : pas de mods
        g_modsFailed = 1;
        return 0;
    }
    uint32_t count;
    std::vector<ModFile> list;
    if (!RecvAll(s, &count, 4) || count > 4096) { closesocket(s); g_modsFailed = 1; return 0; }
    for (uint32_t i = 0; i < count; i++) {
        uint16_t len;
        ModFile m;
        if (!RecvAll(s, &len, 2) || len > 200) { closesocket(s); g_modsFailed = 1; return 0; }
        m.rel.resize(len);
        if (!RecvAll(s, &m.rel[0], len) || !RecvAll(s, &m.size, 4) || !RecvAll(s, &m.hash, 4)) { closesocket(s); g_modsFailed = 1; return 0; }
        if (SafeRel(m.rel)) list.push_back(m);
    }
    ScanLocal();
    std::vector<ModFile> want;
    EnterCriticalSection(&g_lock);
    g_manifest = list;
    for (auto &m : list) {
        bool have = false;
        for (auto &l : g_local) if (l.rel == m.rel && l.hash == m.hash && l.size == m.size) have = true;
        if (!have) want.push_back(m);
    }
    LeaveCriticalSection(&g_lock);
    g_manifestKnown = true;
    g_modsTotal = (long)want.size();
    g_modsDone = 0;
    Log("mods : manifeste de l'hote : %u fichiers, %d a telecharger", count, (int)want.size());
    static uint8_t buf[65536];
    for (auto &m : want) {
        uint16_t len = (uint16_t)m.rel.size();
        if (!SendAll(s, "GET ", 4) || !SendAll(s, &len, 2) || !SendAll(s, m.rel.data(), len)) break;
        uint32_t size;
        if (!RecvAll(s, &size, 4)) break;
        std::string path = ModsDir() + m.rel;
        MakeDirs(path);
        FILE *f = fopen((path + ".part").c_str(), "wb");
        bool ok = true;
        uint32_t left = size;
        while (ok && left > 0) {
            int n = left > sizeof(buf) ? (int)sizeof(buf) : (int)left;
            ok = RecvAll(s, buf, n);
            if (ok && f) fwrite(buf, 1, n, f);
            left -= n;
        }
        if (f) fclose(f);
        if (!ok) { DeleteFileA((path + ".part").c_str()); break; }
        DeleteFileA(path.c_str());
        MoveFileA((path + ".part").c_str(), path.c_str());
        InterlockedIncrement(&g_modsDone);
        Log("mods : %s recu (%u octets)", m.rel.c_str(), size);
    }
    SendAll(s, "END ", 4);
    closesocket(s);
    ScanLocal();
    if (g_modsDone < g_modsTotal) { g_modsFailed = 1; Log("mods : telechargement incomplet (%ld/%ld)", g_modsDone, g_modsTotal); }
    else Log("mods : a jour");
    return 0;
}

// ======================================================================= Etat pour le reste du mod
bool ModsReady()
{
    if (!g_cfg.sharedMods || g_cfg.host) return true;
    return g_manifestKnown && (g_modsFailed || g_modsDone >= g_modsTotal);
}

int ModsPercent()
{
    if (!g_cfg.sharedMods || g_cfg.host) return 100;
    if (!g_manifestKnown) return 0;
    if (g_modsTotal <= 0) return 100;
    return (int)(g_modsDone * 100 / g_modsTotal);
}

void ModsFrame()
{
    if (!g_cfg.sharedMods) return;
    if (g_cfg.host && CoopNetworkStarted() && !g_server) { ScanLocal(); g_server = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL); }
    if (!g_cfg.host && g_localId > 0 && !g_client) g_client = CreateThread(NULL, 0, ClientThread, NULL, 0, NULL);
}

void InstallMods()
{
    InitializeCriticalSection(&g_lock);
    if (!g_cfg.sharedMods) return;
    CreateDirectoryA((std::string(GameDir()) + "VCCoop").c_str(), NULL);
    CreateDirectoryA(ModsDir().c_str(), NULL);
    static const uint8_t dirsPro[] = { 0x53, 0xC7, 0x05, 0x30, 0x15, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t openPro[] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x4C, 0x24, 0x08 };
    o_LoadDirs = (LoadDirs_t)MakeDetour(0x40FE00, dirsPro, sizeof(dirsPro), (void *)h_LoadDirs);
    o_OpenFile = (OpenFile_t)MakeDetour(0x48DF90, openPro, sizeof(openPro), (void *)h_OpenFile);
    Log("mods : dossier %s", ModsDir().c_str());
}
