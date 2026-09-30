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
// Packs complets (ex. NextGen Cars Pack) :
//  - nom.img + nom.dir : l'archive entiere est ajoutee telle quelle (lien vccpkN.img/.dir a la racine du jeu, sans
//    copie), avant vccmods.img : les fichiers isoles gagnent sur le pack, le pack sur gta3.img ;
//  - fichiers ranges sous ...\models\... ou ...\data\... (vehicles.col, generic\wheels.dff...) : remplacent le
//    fichier du jeu au meme chemin (CreateFileA du jeu detourne) ; default.ide : lignes fusionnees par numero ;
//    gta_vc.dat et le "limit adjuster" ne servent pas : la memoire de chargement est relevee par nous (MemoireChargement).
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
static volatile bool g_needRestart;      // invite : recu en jeu des fichiers que le jeu ne lit qu'au demarrage (pack)
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
        if (r.size() >= 5 && !_stricmp(r.c_str() + r.size() - 5, ".part")) continue;
        if (r.size() < 200 && HashFile(dir + fd.cFileName, m.size, m.hash) && m.size < 1536u * 1024 * 1024) out.push_back(m);
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

static std::string SwapKey(const std::string &rel);
static std::string Ext(const std::string &name);
static uint64_t g_looseBytes;   // fichiers isoles regroupes dans vccmods.img (un pack de voitures HD en vrac : 560 Mo)

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
        if (!SwapKey(m.rel).empty()) continue;   // fichier du jeu remplace a son chemin (vehicles.col, generic\wheels.dff)
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
    g_looseBytes = (uint64_t)block * 2048;
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
        bool ide = !strcmp(outName, "default.ide");
        bool ok = ide ? name == "default.ide" : carcols ? name == "carcols.dat" : (name == "handling.cfg" || name == "handling.txt" || Ext(name) == ".handling");
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

static bool g_handling, g_carcols, g_ide;

// --- Archives de pack (.img + .dir) ---
static int g_packCount;
static uint64_t g_packBytes;
static int g_memFloorMb;   // memoire de chargement minimale (Mo), lue par interp.cpp
int ModsMemoryFloorMb() { return g_memFloorMb; }

static uint64_t FileSize64(const std::string &p)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &a)) return (uint64_t)-1;
    return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

// Le jeu ne garde que 15 caracteres du nom d'une image (relatif au dossier du jeu) : un lien dur vccpkN.img/.dir a la
// racine pointe sur l'archive du dossier des mods (meme disque : pas de copie ; sinon, copie).
static bool LinkPack(const std::string &src, const std::string &dst)
{
    if (FileSize64(dst) == FileSize64(src) && FileSize64(src) != (uint64_t)-1) return true;   // deja la
    DeleteFileA(dst.c_str());
    if (CreateHardLinkA(dst.c_str(), src.c_str(), NULL)) return true;
    Log("mods : lien %s impossible (%lu), copie", dst.c_str(), GetLastError());
    return CopyFileA(src.c_str(), dst.c_str(), FALSE) != 0;
}

static void BuildPacks(const std::vector<ModFile> &files)
{
    std::string root = GameDir();
    g_packCount = 0;
    g_packBytes = 0;
    for (auto &m : files) {
        if (Ext(m.rel) != ".img" || g_packCount >= 4) continue;
        std::string base = m.rel.substr(0, m.rel.size() - 4), dirRel;
        for (auto &d : files) if (Lower(d.rel) == Lower(base + ".dir")) dirRel = d.rel;
        if (dirRel.empty()) { Log("mods : %s sans son .dir, ignore", m.rel.c_str()); continue; }
        char name[16];
        wsprintfA(name, "vccpk%d", g_packCount + 1);
        if (!LinkPack(ModsDir() + m.rel, root + name + ".img") || !LinkPack(ModsDir() + dirRel, root + name + ".dir")) {
            Log("mods : %s : impossible de le poser a la racine du jeu", m.rel.c_str());
            continue;
        }
        g_packCount++;
        g_packBytes += m.size;
        Log("mods : archive %s (%u Mo) -> %s.img", m.rel.c_str(), m.size >> 20, name);
    }
    for (int k = g_packCount + 1; k <= 4; k++) {   // anciennes archives (pack retire)
        char name[32];
        wsprintfA(name, "vccpk%d.img", k); DeleteFileA((root + name).c_str());
        wsprintfA(name, "vccpk%d.dir", k); DeleteFileA((root + name).c_str());
    }
}

// Memoire de chargement (CStreaming::ms_memoryAvailable) selon TOUS les modeles moddes : archives de packs et fichiers
// isoles de vccmods.img. Avant, seules les archives comptaient : un pack de voitures HD en vrac (560 Mo) gardait la
// memoire d'origine, la circulation la remplissait et le jeu ne chargeait plus les batiments detailles (decor en
// modeles lointains, invite de JD, 30/09). 256 Mo + la moitie des mods, 768 Mo au plus (jeu 32 bits).
static void ComputeMemFloor()
{
    int mb = GetPrivateProfileIntA("VCCoop", "MemoireChargement", 0, IniPath());
    uint64_t total = g_packBytes + g_looseBytes;
    if (mb <= 0) { uint64_t want = 256 + (total >> 21); mb = total > (50u << 20) ? (int)(want > 768 ? 768 : want) : 0; }
    g_memFloorMb = mb;
    if (mb) Log("mods : memoire de chargement %d Mo (archives %u Mo, fichiers isoles %u Mo)", mb, (unsigned)(g_packBytes >> 20), (unsigned)(g_looseBytes >> 20));
}

// --- Fichiers du jeu remplaces par chemin (vehicles.col, generic\wheels.dff...) ---
// Cle : le chemin a partir de ...\models\ ou ...\data\, seulement si le jeu a ce fichier sur le disque (sinon c'est un
// modele isole ordinaire, pour vccmods.img : vehicles.col y passait pour une collision de decor, plantage 0x62A8EE).
static std::string SwapKey(const std::string &rel)
{
    std::string low = "\\" + Lower(rel);
    size_t p = low.find("\\models\\"), q = low.find("\\data\\");
    if (q != std::string::npos && (p == std::string::npos || q < p)) p = q;
    if (p == std::string::npos) return "";
    std::string key = low.substr(p + 1);
    if (GetFileAttributesA((std::string(GameDir()) + key).c_str()) == INVALID_FILE_ATTRIBUTES) return "";
    return key;
}
struct PathSwap { std::string key, path; };
static std::vector<PathSwap> g_swaps;
static CRITICAL_SECTION g_swapLock;

static void BuildSwaps(const std::vector<ModFile> &files)
{
    std::vector<PathSwap> v;
    for (auto &m : files) {
        std::string e = Ext(m.rel), name = Lower(BaseName(m.rel)), key = SwapKey(m.rel);
        if (key.empty()) continue;
        if (e == ".img" || e == ".dir" || name == "gta_vc.dat" || name == "handling.cfg" || name == "carcols.dat" || name == "default.ide") continue;
        if (e == ".asi" || e == ".dll" || e == ".exe" || e == ".ini") continue;
        PathSwap s = { key, ModsDir() + m.rel };
        bool dup = false;
        for (auto &o : v) if (o.key == s.key) dup = true;
        if (dup) continue;
        v.push_back(s);
        Log("mods : %s remplace par %s", s.key.c_str(), m.rel.c_str());
    }
    EnterCriticalSection(&g_swapLock);
    g_swaps.swap(v);
    LeaveCriticalSection(&g_swapLock);
}

// Chemin du jeu (relatif ou absolu dans son dossier) -> fichier du mod, ou vide.
static bool SwapFor(const char *name, std::string &out)
{
    if (!name || !name[0]) return false;
    char full[MAX_PATH];
    if (!GetFullPathNameA(name, MAX_PATH, full, NULL)) return false;   // relatif au dossier courant (SetDir("DATA")...)
    std::string n = Lower(full);
    for (char &c : n) if (c == '/') c = '\\';
    std::string root = Lower(GameDir());
    if (n.compare(0, root.size(), root) == 0) n = n.substr(root.size());
    while (n.size() > 2 && n[0] == '.' && n[1] == '\\') n = n.substr(2);
    bool found = false;
    EnterCriticalSection(&g_swapLock);
    for (auto &s : g_swaps) if (s.key == n) { out = s.path; found = true; break; }
    LeaveCriticalSection(&g_swapLock);
    return found;
}

typedef HANDLE(WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileA_t o_CreateFileA;
static HANDLE WINAPI h_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    std::string swap;
    if (!(acc & GENERIC_WRITE) && SwapFor(name, swap)) {
        static int logged;
        if (logged++ < 40) Log("mods : le jeu ouvre %s -> %s", name, swap.c_str());
        return o_CreateFileA(swap.c_str(), acc, share, sa, disp, flags, tmpl);
    }
    return o_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
}

static void Build()
{
    if (!g_cfg.sharedMods) return;
    ScanLocal();
    std::vector<ModFile> files = Active();
    BuildPacks(files);
    BuildSwaps(files);
    g_looseBytes = 0;
    g_imgReady = BuildImg(files);
    ComputeMemFloor();
    g_handling = MergeData("data\\handling.cfg", files, false, "handling.cfg") > 0;
    g_carcols = MergeData("data\\carcols.dat", files, true, "carcols.dat") > 0;
    g_ide = MergeData("data\\default.ide", files, false, "default.ide") > 0;
}

// CStreaming::LoadCdDirectory() (0x40FE00, toutes les images, a chaque initialisation du jeu) : juste avant, notre
// image est ajoutee en dernier (CdStreamAddImage 0x4081E0 ; nombre d'images 0x6F76C8, noms 0x6F7488 + 16 * i).
typedef void(__cdecl *LoadDirs_t)();
static LoadDirs_t o_LoadDirs;
static void __cdecl h_LoadDirs()
{
    Build();
    for (int k = 1; k <= g_packCount; k++) {   // archives de pack d'abord : vccmods.img (fichiers isoles) passe devant
        char name[16];
        wsprintfA(name, "vccpk%d.img", k);
        int n = *(int *)0x6F76C8;
        bool have = false;
        for (int i = 0; i < n; i++) if (_stricmp((const char *)(0x6F7488 + i * 0x10), name) == 0) have = true;
        if (!have && n < 30) { ((int(__cdecl *)(const char *))0x4081E0)(name); Log("mods : %s ajoute (image %d)", name, n); }
    }
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
        // Chemins absolus : le jeu change de dossier courant avant certains fichiers (CFileMgr::SetDir("DATA") puis
        // "CARCOLS.DAT") ; un chemin relatif a la racine du jeu n'y etait plus trouve (plantage 0x652AA0, fichier nul).
        if (g_handling && n >= 12 && _stricmp(name + n - 12, "handling.cfg") == 0) return o_OpenFile((CacheDir() + "handling.cfg").c_str(), mode);
        if (g_carcols && n >= 11 && _stricmp(name + n - 11, "carcols.dat") == 0) return o_OpenFile((CacheDir() + "carcols.dat").c_str(), mode);
        if (g_ide && n >= 11 && _stricmp(name + n - 11, "default.ide") == 0) return o_OpenFile((CacheDir() + "default.ide").c_str(), mode);
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
        std::string e = Ext(m.rel);
        if (e == ".img" || e == ".dir" || !SwapKey(m.rel).empty() || Lower(BaseName(m.rel)) == "handling.cfg" || Lower(BaseName(m.rel)) == "default.ide")
            g_needRestart = true;
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
    // Pack recu en jeu (rejoindre sans le salon du lanceur) : collisions, roues, conduite sont lues au demarrage.
    static bool told;
    if (g_needRestart && !told && ModsReady() && g_onNotice) {
        told = true;
        g_onNotice("pack de mods de l'hote recu : relancez le jeu pour qu'il soit complet", "host's mod pack received: restart the game to apply it fully", 0);
    }
    if (g_cfg.host && CoopNetworkStarted() && !g_server) { ScanLocal(); g_server = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL); }
    if (!g_cfg.host && g_localId > 0 && !g_client) g_client = CreateThread(NULL, 0, ClientThread, NULL, 0, NULL);
}

void InstallMods()
{
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_swapLock);
    if (!g_cfg.sharedMods) return;
    CreateDirectoryA((std::string(GameDir()) + "VCCoop").c_str(), NULL);
    CreateDirectoryA(ModsDir().c_str(), NULL);
    static const uint8_t dirsPro[] = { 0x53, 0xC7, 0x05, 0x30, 0x15, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t openPro[] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x4C, 0x24, 0x08 };
    o_LoadDirs = (LoadDirs_t)MakeDetour(0x40FE00, dirsPro, sizeof(dirsPro), (void *)h_LoadDirs);
    o_OpenFile = (OpenFile_t)MakeDetour(0x48DF90, openPro, sizeof(openPro), (void *)h_OpenFile);
    o_CreateFileA = (CreateFileA_t)HookImport("kernel32.dll", "CreateFileA", (void *)h_CreateFileA);
    if (!o_CreateFileA) Log("mods : CreateFileA du jeu introuvable (remplacements par chemin indisponibles)");
    // Tout est prepare des maintenant : le jeu lit gta_vc.dat (vehicles.col, generic\wheels.dff), la conduite et les
    // couleurs AVANT CStreaming::Init, ou l'on ne passait qu'ensuite (les remplacements arrivaient trop tard).
    Build();
    Log("mods : dossier %s", ModsDir().c_str());
}
