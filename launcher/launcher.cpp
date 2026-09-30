// VCCoop - lanceur (VCCoop.exe, a poser dans le dossier du jeu, livre dans le paquet).
//
//  - Fenetre sans cadre, forme et transparence prises du PNG VCCoop\interface\launcher.png (fenetre "layered",
//    alpha par pixel) ; textes, champs et boutons dessines par-dessus avec GDI+.
//  - Cible gta-vc.exe : celui du dossier, ou celui choisi (retenu dans vccoop-launcher.ini). Seul l'exe 1.0 est
//    accepte (meme signature que dllmain.cpp IsVersion10) ; Steam, GOG, 1.1 : message, sans lien.
//  - A chaque lancement : derniere version publiee sur GitHub (Parricidium/VCCoop, releases/latest). Plus recente
//    que VCCoop\version.txt (ou mod absent) : telechargement du zip, extraction (tar.exe de Windows), copie dans le
//    dossier du jeu. vccoop.ini de l'utilisateur garde ses valeurs (seules les cles nouvelles sont ajoutees) ;
//    vccoop-joueur.ini n'est pas dans le paquet. Le lanceur se remplace lui-meme (renomme en .old) puis se relance.
//  - Heberger / Rejoindre / Jouer : lance le jeu (-vccoop hote | -vccoop invite <adresse> | rien) puis reste affiche
//    en ecran d'attente jusqu'a ce que la fenetre du jeu apparaisse.
//
// Options de ligne de commande (tests) : /capture <png> <menu|attente|sansexe|maj> ; /check <exe> (code de sortie :
// 0 = 1.0, 1 = absent, 2 = autre, 3 = Steam, 4 = GOG).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
using std::min;
using std::max;
#include <objidl.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <atomic>
#include <stdio.h>
#include <math.h>
#include "model3d.h"
#include <map>

using namespace Gdiplus;

static const wchar_t *kRepoApi = L"https://api.github.com/repos/Parricidium/VCCoop/releases/latest";
static const wchar_t *kStoreUrl = L"https://store.rockstargames.com/fr/game/buy-grand-theft-auto-the-trilogy";   // (meme lien que le README)
static const float kImgW = 1000, kImgH = 620;   // mise en page (coordonnees de launcher.png)

// ---------------------------------------------------------------- etat
enum { ST_IDLE, ST_BUSY, ST_LAUNCH, ST_CLOSING };
enum { K_NORMAL, K_OK, K_WARN, K_ERR };
enum ExeKind { EXE_OK, EXE_MISSING, EXE_OTHER, EXE_STEAM, EXE_GOG };

static HWND g_wnd;
static bool g_fr;
static std::wstring g_dir, g_self, g_iniLauncher;   // dossier du lanceur (avec \), chemin du lanceur
static std::wstring g_exe, g_gameDir;               // gta-vc.exe choisi, son dossier (avec \)
static ExeKind g_exeKind = EXE_MISSING;
static std::wstring g_localVer;
static int g_state = ST_IDLE;
static float g_scale = 1, g_alpha = 0, g_time = 0;
static Bitmap *g_bg, *g_bgDark;
static int g_winW, g_winH;
static HDC g_memDC;
static HBITMAP g_dib;
static void *g_bits;

static CRITICAL_SECTION g_cs;
static std::wstring g_status;
static int g_statusKind = K_NORMAL;
static std::atomic<float> g_progress(-1.0f);   // -1 = pas de barre, -2 = indeterminee, 0..1
static std::atomic<bool> g_busy(false);

static HANDLE g_proc;
static DWORD g_pid, g_launchT, g_winSeenT;
static int g_launchMode;
static std::wstring g_launchInfo;

#define WM_APP_RELAUNCH (WM_APP + 1)
#define WM_APP_GO (WM_APP + 2)          // invite : l'hote a lance la partie
#define WM_APP_LOBBYEND (WM_APP + 3)    // invite : salon ferme ou refuse (wParam : 1 = refuse)

// Salon : etat partage entre la fenetre et les fils reseau (sous g_lcs)
enum { LB_NONE, LB_HOST, LB_CONNECTING, LB_GUEST };
struct LobbyPeer { int id; std::string name, skin; bool ready; int mods; int ping; };   // mods : -1 sans objet, 0..100, -2 erreur

// Sons du salon : arrivee, depart, pret, plus pret. Petites notes synthetisees (WAV en memoire, PlaySound).
enum { SND_JOIN, SND_LEAVE, SND_READY, SND_UNREADY, SND_COUNT };
static std::vector<uint8_t> g_snd[SND_COUNT];
static void MakeSound(std::vector<uint8_t> &w, std::initializer_list<float> notes, float noteMs, float vol)
{
    const int rate = 22050, per = (int)(rate * noteMs / 1000.0f), tail = rate / 5;
    int total = per * (int)notes.size() + tail;
    std::vector<float> buf(total, 0.0f);
    int k = 0;
    for (float f : notes) {
        int start = k++ * per;
        for (int i = 0; i < per + tail && start + i < total; i++) {
            float t = i / (float)rate;
            float env = (i < rate / 200 ? i / (rate / 200.0f) : 1.0f) * expf(-t * 9.0f);
            buf[start + i] += env * (sinf(6.2831853f * f * t) + 0.25f * sinf(6.2831853f * f * 2 * t));
        }
    }
    uint32_t data = total * 2;
    w.resize(44 + data);
    uint8_t *p = w.data();
    auto u32 = [&](int at, uint32_t v) { memcpy(p + at, &v, 4); };
    auto u16 = [&](int at, uint16_t v) { memcpy(p + at, &v, 2); };
    memcpy(p, "RIFF", 4); u32(4, 36 + data); memcpy(p + 8, "WAVEfmt ", 8); u32(16, 16); u16(20, 1); u16(22, 1);
    u32(24, rate); u32(28, rate * 2); u16(32, 2); u16(34, 16); memcpy(p + 36, "data", 4); u32(40, data);
    for (int i = 0; i < total; i++) {
        float v = buf[i] * vol;
        v = v > 1 ? 1 : v < -1 ? -1 : v;
        int16_t sv = (int16_t)(v * 32000);
        memcpy(p + 44 + i * 2, &sv, 2);
    }
}
static void LobbySound(int which)
{
    if (g_snd[0].empty()) {
        MakeSound(g_snd[SND_JOIN], { 523.3f, 659.3f, 784.0f }, 90, 0.30f);     // do mi sol : quelqu'un arrive
        MakeSound(g_snd[SND_LEAVE], { 784.0f, 659.3f, 523.3f }, 90, 0.26f);    // sol mi do : il part
        MakeSound(g_snd[SND_READY], { 987.8f, 1318.5f }, 70, 0.24f);           // si mi aigus : pret
        MakeSound(g_snd[SND_UNREADY], { 659.3f, 493.9f }, 80, 0.22f);          // mi si graves : plus pret
    }
    PlaySoundW((LPCWSTR)g_snd[which].data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
static std::atomic<int> g_lobby(LB_NONE);
static CRITICAL_SECTION g_lcs;
static std::vector<LobbyPeer> g_peers;
static int g_myId, g_lobbyChoice;                  // choix de l'hote : 0 = nouvelle partie, sinon index dans g_saves + 1
static std::string g_lobbyChoiceLabel;              // invite : sauvegarde choisie par l'hote ("" = nouvelle partie)
static std::atomic<int> g_myMods(-1);
static bool g_imgOk;                                // tenues : catalogue du jeu lu (onglet TENUE)

static std::wstring g_testLog;          // /testfenetre : fenetre hors ecran, sans activation, journal puis sortie
static int g_ulwOk = -1, g_frames;
static int g_testLaunch = -1;           // /testlancer <0|1|2> <journal> : idem, et lance le jeu tout seul au bout de 3 s

static const wchar_t *T(const wchar_t *fr, const wchar_t *en) { return g_fr ? fr : en; }

static void SetStatus(int kind, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_cs);
    g_status = buf;
    g_statusKind = kind;
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------- utilitaires
static std::wstring Widen(const std::string &s, UINT cp = CP_UTF8)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string Narrow(const std::wstring &w, UINT cp = CP_ACP)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(cp, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(cp, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
static bool FileExists(const std::wstring &p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static std::wstring DirOf(const std::wstring &p) { size_t k = p.find_last_of(L"\\/"); return k == std::wstring::npos ? L"" : p.substr(0, k + 1); }
static bool ReadAll(const std::wstring &p, std::vector<unsigned char> &out)
{
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, NULL), got = 0;
    out.resize(size);
    bool ok = size == INVALID_FILE_SIZE ? false : (size == 0 || (ReadFile(f, out.data(), size, &got, NULL) && got == size));
    CloseHandle(f);
    return ok;
}
static std::wstring Trim(std::wstring s)
{
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    size_t k = 0;
    while (k < s.size() && (s[k] == L' ' || s[k] == L'\t' || s[k] == 0xFEFF)) k++;
    return s.substr(k);
}

// ---------------------------------------------------------------- gta-vc.exe 1.0
// Octets de tete de CRunningScript::ProcessOneCommand (inc word [CTheScripts::CommandsExecuted]) a 0x44FBE0 : n'existent
// qu'a cette adresse dans le 1.0 (meme test que le mod, dllmain.cpp).
static ExeKind CheckExe(const std::wstring &path)
{
    std::vector<unsigned char> d;
    if (path.empty() || !ReadAll(path, d) || d.size() < 0x400) return EXE_MISSING;
    static const unsigned char sig[] = { 0x66, 0xFF, 0x05, 0x66, 0x0A, 0xA1, 0x00 };
    bool steamSection = false;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)d.data();
    if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 && (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) < d.size()) {
        const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(d.data() + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386) {
            const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
            DWORD rva = 0x44FBE0 - nt->OptionalHeader.ImageBase;
            for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
                if ((const unsigned char *)(sec + i + 1) > d.data() + d.size()) break;
                if (!memcmp(sec[i].Name, ".bind", 5)) steamSection = true;   // enveloppe SteamStub
                DWORD va = sec[i].VirtualAddress, sz = max(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
                if (nt->OptionalHeader.ImageBase == 0x400000 && rva >= va && rva < va + sz) {
                    size_t off = sec[i].PointerToRawData + (rva - va);
                    if (off + sizeof(sig) <= d.size() && rva - va + sizeof(sig) <= sec[i].SizeOfRawData && !memcmp(d.data() + off, sig, sizeof(sig)))
                        return EXE_OK;
                }
            }
        }
    }
    std::wstring dir = DirOf(path);
    if (steamSection || FileExists(dir + L"steam_api.dll") || FileExists(dir + L"steam_appid.txt")) return EXE_STEAM;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"goggame-*.info").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { FindClose(h); return EXE_GOG; }
    if (FileExists(dir + L"GalaxyEngine.dll") || FileExists(dir + L"gog.ico")) return EXE_GOG;
    return EXE_OTHER;
}

static void BadExeMessage(ExeKind k)
{
    const wchar_t *first = k == EXE_STEAM ? T(L"Ce gta-vc.exe est la version Steam.", L"This gta-vc.exe is the Steam version.")
                         : k == EXE_GOG   ? T(L"Ce gta-vc.exe est la version GOG.", L"This gta-vc.exe is the GOG version.")
                                          : T(L"Ce gta-vc.exe n'est pas la version 1.0.", L"This gta-vc.exe is not version 1.0.");
    std::wstring msg = first;
    msg += T(L"\n\nVCCoop ne fonctionne qu'avec le gta-vc.exe de la version 1.0 (exe \u00AB downgrade \u00BB 1.0). "
             L"Les exe Steam, GOG et 1.1 ne sont pas compatibles.\n\n"
             L"Remplace l'exe du jeu par celui de la version 1.0, puis choisis-le dans le lanceur.",
             L"\n\nVCCoop only works with the version 1.0 gta-vc.exe (1.0 \"downgrade\" exe). "
             L"The Steam, GOG and 1.1 exes are not compatible.\n\n"
             L"Replace the game's exe with the version 1.0 one, then pick it in the launcher.");
    MessageBoxW(g_wnd, msg.c_str(), L"VCCoop", MB_ICONWARNING | MB_OK);
}

// ---------------------------------------------------------------- reglages
struct Field { std::wstring text; RectF r; size_t maxLen; bool address; };
static Field g_fields[2];   // 0 = pseudo, 1 = adresse
static int g_focus = -1;

static std::wstring PlayerIni() { return g_gameDir + L"vccoop-joueur.ini"; }

static void LoadPlayer()
{
    char v[128];
    std::string pj = Narrow(PlayerIni()), main = Narrow(g_gameDir + L"vccoop.ini");
    GetPrivateProfileStringA("VCCoop", "Pseudo", "Tommy", v, sizeof(v), main.c_str());
    GetPrivateProfileStringA("VCCoop", "Pseudo", v, v, sizeof(v), pj.c_str());
    g_fields[0].text = Widen(v, CP_ACP);
    GetPrivateProfileStringA("VCCoop", "Adresse", "127.0.0.1", v, sizeof(v), main.c_str());
    GetPrivateProfileStringA("VCCoop", "Adresse", v, v, sizeof(v), pj.c_str());
    g_fields[1].text = Widen(v, CP_ACP);
}

static void SavePlayer()
{
    if (g_gameDir.empty()) return;
    std::string pj = Narrow(PlayerIni());
    std::wstring name = Trim(g_fields[0].text);
    if (name.empty()) name = L"Tommy";
    WritePrivateProfileStringA("VCCoop", "Pseudo", Narrow(name).c_str(), pj.c_str());
    WritePrivateProfileStringA("VCCoop", "Adresse", Narrow(Trim(g_fields[1].text)).c_str(), pj.c_str());
}

static void LoadLocalVersion()
{
    std::vector<unsigned char> d;
    g_localVer.clear();
    if (!g_gameDir.empty() && ReadAll(g_gameDir + L"VCCoop\\version.txt", d))
        g_localVer = Trim(Widen(std::string(d.begin(), d.end())));
    if (!g_gameDir.empty() && !FileExists(g_gameDir + L"dinput8.dll")) g_localVer.clear();   // mod pas installe ici
}

static void SetExe(const std::wstring &path)
{
    g_exe = path;
    g_exeKind = CheckExe(path);
    g_gameDir = g_exeKind == EXE_OK ? DirOf(path) : L"";
    LoadLocalVersion();
    if (!g_gameDir.empty()) LoadPlayer();
}

// "2026.09.29h" : date, puis suffixe (a < b < ... < z < za < ... < zz ; plus long = plus recent)
static int CmpVer(std::wstring a, std::wstring b)
{
    if (!a.empty() && (a[0] == L'v' || a[0] == L'V')) a.erase(0, 1);
    if (!b.empty() && (b[0] == L'v' || b[0] == L'V')) b.erase(0, 1);
    std::wstring da = a.substr(0, min<size_t>(10, a.size())), db = b.substr(0, min<size_t>(10, b.size()));
    if (da != db) return da < db ? -1 : 1;
    std::wstring sa = a.size() > 10 ? a.substr(10) : L"", sb = b.size() > 10 ? b.substr(10) : L"";
    if (sa.size() != sb.size()) return sa.size() < sb.size() ? -1 : 1;
    return sa == sb ? 0 : (sa < sb ? -1 : 1);
}

// ---------------------------------------------------------------- HTTP (WinHTTP)
// GET sur une URL https ; corps dans out (ou dans le fichier toFile), progression 0..1 si onProgress.
static bool HttpGet(const std::wstring &url, std::string *out, const std::wstring &toFile, bool progress)
{
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = _countof(path);
    wchar_t extra[1024] = {};
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = _countof(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    std::wstring full = std::wstring(path) + extra;

    HINTERNET s = WinHttpOpen(L"VCCoop-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!s) s = WinHttpOpen(L"VCCoop-Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 8000, 8000, 15000, 30000);
    bool ok = false;
    HANDLE f = INVALID_HANDLE_VALUE;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", full.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    // en-tete de l'API seulement pour l'API (le zip : requete nue, comme un navigateur)
    const wchar_t *hdr = toFile.empty() ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (r && WinHttpSendRequest(r, hdr, toFile.empty() ? (DWORD)-1 : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
          && WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        DWORD total = 0; len = sizeof(total);
        if (!WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total, &len, WINHTTP_NO_HEADER_INDEX)) total = 0;
        if (code == 200) {
            if (!toFile.empty()) f = CreateFileW(toFile.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            ok = toFile.empty() || f != INVALID_HANDLE_VALUE;
            DWORD got = 0;
            std::vector<char> buf(64 * 1024);
            while (ok) {
                DWORD n = 0;
                if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n)) { ok = false; break; }
                if (!n) break;
                if (f != INVALID_HANDLE_VALUE) { DWORD w = 0; if (!WriteFile(f, buf.data(), n, &w, NULL) || w != n) ok = false; }
                else out->append(buf.data(), n);
                got += n;
                if (progress && total) g_progress = (float)got / total;
            }
            if (ok && total && got != total) ok = false;
        }
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

static std::string JsonString(const std::string &json, const char *key, size_t from = 0, size_t *at = NULL)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k, from);
    if (p == std::string::npos) return "";
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = json.find('"', p);
    if (p == std::string::npos) return "";
    std::string v;
    for (size_t i = p + 1; i < json.size() && json[i] != '"'; i++) {
        if (json[i] == '\\' && i + 1 < json.size()) i++;
        v += json[i];
    }
    if (at) *at = p;
    return v;
}

// ---------------------------------------------------------------- installation
static bool RunHidden(const std::wstring &cmd, DWORD *exitCode)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    std::vector<wchar_t> c(cmd.begin(), cmd.end());
    c.push_back(0);
    if (!CreateProcessW(NULL, c.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void DeleteTree(const std::wstring &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p);
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// vccoop.ini deja la : garde les valeurs de l'utilisateur, ajoute seulement les cles apparues dans la nouvelle version.
static void MergeIni(const std::wstring &newIni, const std::wstring &userIni)
{
    std::vector<unsigned char> d;
    if (!ReadAll(newIni, d)) return;
    std::string text(d.begin(), d.end()), section, u = Narrow(userIni);
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { section = line.substr(1, line.find(']') - 1); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos || section.empty()) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        char cur[8];
        GetPrivateProfileStringA(section.c_str(), key.c_str(), "\x01", cur, sizeof(cur), u.c_str());
        if (cur[0] == 1 && !cur[1]) WritePrivateProfileStringA(section.c_str(), key.c_str(), val.c_str(), u.c_str());
    }
}

// Copie l'arbre extrait dans le dossier du jeu. selfReplaced : le lanceur en cours a ete remplace.
static DWORD g_copyError;
static bool CopyTree(const std::wstring &src, const std::wstring &dst, bool *selfReplaced, std::wstring *failed)
{
    CreateDirectoryW(dst.c_str(), NULL);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring s = src + L"\\" + n, d = dst + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ok = CopyTree(s, d, selfReplaced, failed) && ok; continue; }
        if (!_wcsicmp(d.c_str(), (g_gameDir + L"vccoop.ini").c_str()) && FileExists(d)) { MergeIni(s, d); continue; }
        if (!_wcsicmp(d.c_str(), (g_gameDir + L"VCCoop\\version.txt").c_str())) continue;   // en dernier (UpdateThread)
        if (!_wcsicmp(d.c_str(), g_self.c_str())) {
            std::wstring old = g_self + L".old";
            DeleteFileW(old.c_str());
            if (!MoveFileExW(g_self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) { g_copyError = GetLastError(); ok = false; *failed = n; continue; }
            *selfReplaced = true;
        }
        SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!CopyFileW(s.c_str(), d.c_str(), FALSE)) { g_copyError = GetLastError(); ok = false; *failed = n; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

// Le dossier accepte-t-il l'ecriture (Program Files sans droits, par exemple) ?
static bool IsWritableDir(const std::wstring &dir)
{
    std::wstring p = dir + L"vccoop-ecriture.tmp";
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    CloseHandle(f);
    return true;
}

static DWORD WINAPI UpdateThread(void *)
{
    SetStatus(K_NORMAL, T(L"Recherche de mises \u00E0 jour\u2026", L"Checking for updates\u2026"));
    g_progress = -2;
    std::string json;
    std::wstring local = g_localVer;
    if (!HttpGet(kRepoApi, &json, L"", false)) {
        g_progress = -1;
        if (local.empty()) SetStatus(K_ERR, T(L"Hors ligne : VCCoop n'est pas install\u00E9 ici", L"Offline: VCCoop is not installed here"));
        else SetStatus(K_WARN, T(L"Hors ligne \u00B7 VCCoop %s", L"Offline \u00B7 VCCoop %s"), local.c_str());
        g_busy = false;
        return 0;
    }
    std::wstring tag = Widen(JsonString(json, "tag_name")), zipUrl;
    for (size_t at = 0;;) {
        std::string u = JsonString(json, "browser_download_url", at, &at);
        if (u.empty()) break;
        at++;
        if (u.size() > 4 && !_stricmp(u.c_str() + u.size() - 4, ".zip")) { zipUrl = Widen(u); break; }
    }
    std::wstring remote = (!tag.empty() && (tag[0] == L'v' || tag[0] == L'V')) ? tag.substr(1) : tag;
    if (remote.empty() || zipUrl.empty() || (!local.empty() && CmpVer(remote, local) <= 0)) {
        g_progress = -1;
        SetStatus(K_OK, T(L"VCCoop %s \u00B7 \u00E0 jour", L"VCCoop %s \u00B7 up to date"), local.empty() ? L"?" : local.c_str());
        g_busy = false;
        return 0;
    }

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring work = std::wstring(tmp) + L"VCCoop-maj";
    DeleteTree(work);
    CreateDirectoryW(work.c_str(), NULL);
    std::wstring zip = work + L"\\VCCoop.zip", ext = work + L"\\x";
    SetStatus(K_NORMAL, T(L"T\u00E9l\u00E9chargement de VCCoop %s\u2026", L"Downloading VCCoop %s\u2026"), remote.c_str());
    g_progress = 0;
    if (!HttpGet(zipUrl, NULL, zip, true)) {
        g_progress = -1;
        SetStatus(K_ERR, T(L"Mise \u00E0 jour impossible (t\u00E9l\u00E9chargement)", L"Update failed (download)"));
        DeleteTree(work);
        g_busy = false;
        return 0;
    }
    SetStatus(K_NORMAL, T(L"Installation de VCCoop %s\u2026", L"Installing VCCoop %s\u2026"), remote.c_str());
    g_progress = -2;
    CreateDirectoryW(ext.c_str(), NULL);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    DWORD code = 1;
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + ext + L"\"";
    if (!RunHidden(cmd, &code) || code != 0) {
        g_progress = -1;
        SetStatus(K_ERR, T(L"Mise \u00E0 jour impossible (archive)", L"Update failed (archive)"));
        DeleteTree(work);
        g_busy = false;
        return 0;
    }
    bool self = false;
    std::wstring failed;
    std::wstring gd = g_gameDir.substr(0, g_gameDir.size() - 1);
    bool ok = CopyTree(ext, gd, &self, &failed);
    // version.txt seulement si tout est en place : une installation interrompue sera reprise au prochain lancement
    if (ok && !CopyFileW((ext + L"\\VCCoop\\version.txt").c_str(), (g_gameDir + L"VCCoop\\version.txt").c_str(), FALSE)) { ok = false; failed = L"version.txt"; }
    DeleteTree(work);
    g_progress = -1;
    if (!ok && g_copyError == ERROR_ACCESS_DENIED && !IsWritableDir(g_gameDir)) {
        SetStatus(K_ERR, T(L"Dossier du jeu prot\u00E9g\u00E9 : lance VCCoop.exe en administrateur pour mettre \u00E0 jour", L"Game folder is protected: run VCCoop.exe as administrator to update"));
        g_busy = false;
        return 0;
    }
    if (!ok) {
        SetStatus(K_ERR, T(L"%s est occup\u00E9 : ferme le jeu, puis relance le lanceur", L"%s is in use: close the game, then restart the launcher"), failed.c_str());
        g_busy = false;
        return 0;
    }
    LoadLocalVersion();
    SetStatus(K_OK, T(L"Mis \u00E0 jour \u00B7 VCCoop %s", L"Updated \u00B7 VCCoop %s"), g_localVer.empty() ? remote.c_str() : g_localVer.c_str());
    g_busy = false;
    if (self) PostMessageW(g_wnd, WM_APP_RELAUNCH, 0, 0);
    return 0;
}

static void StartUpdate()
{
    if (g_exeKind != EXE_OK || g_busy) return;
    g_busy = true;
    HANDLE t = CreateThread(NULL, 0, UpdateThread, NULL, 0, NULL);
    if (t) CloseHandle(t); else g_busy = false;
}

// ---------------------------------------------------------------- boutons
enum { B_HOST, B_JOIN, B_PLAY, B_EXE, B_BUY, B_THEME, B_CLOSE, B_MIN, B_COUNT };
struct Button { RectF r; float hover; bool visible, enabled; };
static Button g_btn[B_COUNT];
static int g_hot = -1, g_pressed = -1;

static void Layout()
{
    g_fields[0].r = RectF(76, 276, 304, 36); g_fields[0].maxLen = 23; g_fields[0].address = false;
    g_fields[1].r = RectF(76, 338, 304, 36); g_fields[1].maxLen = 63; g_fields[1].address = true;
    g_btn[B_HOST].r = RectF(76, 390, 148, 46);
    g_btn[B_JOIN].r = RectF(232, 390, 148, 46);
    g_btn[B_PLAY].r = RectF(0, 0, 0, 0);          // (retire : le salon suffit, le menu COOP du jeu reste)
    g_btn[B_EXE].r = RectF(250, 452, 130, 20);
    g_btn[B_BUY].r = RectF(236, 554, 144, 26);
    g_btn[B_CLOSE].r = RectF(938, 76, 28, 28);
    g_btn[B_MIN].r = RectF(904, 76, 28, 28);
    g_btn[B_THEME].r = RectF(62, 100, 26, 26);   // coin du panneau, a gauche du logo
}

static void UpdateButtons()
{
    bool menu = g_state == ST_IDLE, exeOk = g_exeKind == EXE_OK, busy = g_busy;
    for (int i = 0; i < B_COUNT; i++) g_btn[i].visible = true;
    g_btn[B_HOST].visible = g_btn[B_JOIN].visible = g_btn[B_PLAY].visible = g_btn[B_EXE].visible = menu;
    g_btn[B_HOST].enabled = g_btn[B_JOIN].enabled = g_btn[B_PLAY].enabled = menu && exeOk && !busy && !g_localVer.empty();
    if (g_lobby != LB_NONE) {
        extern bool LobbyCanStart();
        g_btn[B_PLAY].visible = false;
        g_btn[B_EXE].enabled = false;
        g_btn[B_JOIN].enabled = menu;
        g_btn[B_HOST].enabled = menu && (g_lobby == LB_HOST ? LobbyCanStart() : g_lobby == LB_GUEST);
    }
    g_btn[B_EXE].enabled = menu && !busy;
    g_btn[B_PLAY].visible = false;
    g_btn[B_CLOSE].enabled = g_btn[B_MIN].enabled = g_btn[B_BUY].enabled = g_btn[B_THEME].enabled = true;
}

// ---------------------------------------------------------------- dessin
static void RoundRect(GraphicsPath &p, RectF r, float rad)
{
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

static const Color kPink(255, 255, 79, 139), kOrange(255, 255, 138, 91);
// Themes clair et sombre (bouton lune / soleil ; Theme=clair|sombre dans vccoop-launcher.ini, sinon celui de Windows).
struct Theme {
    Color ink, grey, panel, panelBorder, sep, card, cardSel, cardBorder, choiceBorder, toggleOff, field, fieldBorder, placeholder,
          tab, tabHot, pill, dash, dashText, thumbBg, prevA, prevB, btn2, btn2Hot, circle, circleHot, fallA, fallB;
};
static const Theme kLight = {
    Color(255, 52, 40, 62), Color(255, 128, 112, 130), Color(255, 252, 249, 251), Color(150, 255, 255, 255), Color(255, 240, 214, 226),
    Color(255, 255, 255, 255), Color(255, 255, 236, 244), Color(255, 240, 226, 232), Color(255, 240, 196, 214), Color(255, 222, 210, 218),
    Color(235, 255, 255, 255), Color(255, 226, 206, 216), Color(255, 190, 176, 190), Color(185, 255, 255, 255), Color(240, 255, 255, 255),
    Color(235, 52, 40, 62), Color(255, 236, 214, 224), Color(255, 200, 186, 196), Color(255, 248, 236, 242), Color(255, 255, 244, 248),
    Color(255, 255, 222, 214), Color(215, 255, 255, 255), Color(240, 255, 236, 244), Color(150, 255, 255, 255), Color(235, 255, 255, 255),
    Color(255, 255, 222, 236), Color(255, 255, 178, 158) };
static const Theme kDark = {
    Color(255, 240, 232, 248), Color(255, 172, 156, 188), Color(255, 30, 24, 42), Color(90, 255, 120, 190), Color(255, 66, 52, 82),
    Color(255, 44, 36, 60), Color(255, 76, 40, 72), Color(255, 72, 60, 92), Color(255, 118, 66, 104), Color(255, 84, 72, 102),
    Color(235, 40, 32, 56), Color(255, 88, 72, 108), Color(255, 124, 108, 140), Color(200, 44, 36, 60), Color(240, 66, 50, 88),
    Color(235, 96, 70, 128), Color(255, 82, 68, 102), Color(255, 124, 108, 140), Color(255, 56, 44, 74), Color(255, 50, 36, 66),
    Color(255, 76, 36, 62), Color(215, 44, 36, 60), Color(240, 76, 42, 72), Color(170, 44, 36, 60), Color(235, 70, 52, 92),
    Color(255, 40, 26, 62), Color(255, 70, 30, 62) };
static bool g_dark;
#define TH(x) ((g_dark ? kDark : kLight).x)
#define kInk TH(ink)
#define kGrey TH(grey)

static Color Mix(Color a, Color b, float t)
{
    auto L = [&](BYTE x, BYTE y) { return (BYTE)(x + (y - x) * t); };
    return Color(L(a.GetA(), b.GetA()), L(a.GetR(), b.GetR()), L(a.GetG(), b.GetG()), L(a.GetB(), b.GetB()));
}
static Color WithA(Color c, float a) { return Color((BYTE)(c.GetA() * a), c.GetR(), c.GetG(), c.GetB()); }

static void Text(Graphics &g, const std::wstring &s, RectF r, float px, int style, Color c, StringAlignment h = StringAlignmentCenter)
{
    FontFamily fam(L"Segoe UI");
    Font font(&fam, px, style, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, &font, r, &sf, &b);
}

static void DrawButton(Graphics &g, int id, const wchar_t *label, bool primary)
{
    Button &b = g_btn[id];
    if (!b.visible) return;
    float a = b.enabled ? 1.0f : 0.38f;
    RectF r = b.r;
    if (g_pressed == id && g_hot == id) r.Offset(0, 1);
    GraphicsPath p;
    RoundRect(p, r, 12);
    if (primary) {
        // ombre coloree
        GraphicsPath sp;
        RoundRect(sp, RectF(r.X + 2, r.Y + 5, r.Width - 4, r.Height), 12);
        SolidBrush sb(Color((BYTE)(55 * a), 255, 80, 120));
        g.FillPath(&sb, &sp);
        LinearGradientBrush lg(r, WithA(kPink, a), WithA(kOrange, a), LinearGradientModeHorizontal);
        g.FillPath(&lg, &p);
        SolidBrush hi(Color((BYTE)(60 * b.hover * a), 255, 255, 255));
        g.FillPath(&hi, &p);
        Text(g, label, r, 15, FontStyleBold, Color((BYTE)(255 * a), 255, 255, 255));
    } else {
        SolidBrush fill(Mix(WithA(TH(btn2), a), WithA(TH(btn2Hot), a), b.hover));
        g.FillPath(&fill, &p);
        Pen pen(WithA(kPink, a), 1.6f);
        g.DrawPath(&pen, &p);
        Text(g, label, r, id == B_PLAY ? 12.5f : 15, FontStyleBold, WithA(kPink, a));
    }
}

static void DrawField(Graphics &g, int i, const wchar_t *label)
{
    Field &f = g_fields[i];
    Text(g, label, RectF(f.r.X + 2, f.r.Y - 18, f.r.Width, 16), 10.5f, FontStyleBold, kGrey, StringAlignmentNear);
    GraphicsPath p;
    RoundRect(p, f.r, 9);
    SolidBrush fill(TH(field));
    g.FillPath(&fill, &p);
    Pen pen(g_focus == i ? kPink : TH(fieldBorder), g_focus == i ? 2.0f : 1.2f);
    g.DrawPath(&pen, &p);
    RectF tr(f.r.X + 12, f.r.Y, f.r.Width - 24, f.r.Height);
    std::wstring shown = f.text;
    bool placeholder = shown.empty() && g_focus != i;
    if (placeholder) shown = f.address ? L"ex. 26.12.34.56" : L"Tommy";
    Text(g, shown, tr, 15, FontStyleRegular, placeholder ? TH(placeholder) : kInk, StringAlignmentNear);
    if (g_focus == i && fmodf(g_time, 1.0f) < 0.55f) {
        FontFamily fam(L"Segoe UI");
        Font font(&fam, 15, FontStyleRegular, UnitPixel);
        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetFormatFlags(StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
        RectF box;
        g.MeasureString(f.text.c_str(), -1, &font, PointF(0, 0), &sf, &box);
        float x = min(tr.X + box.Width + 1, tr.X + tr.Width);
        Pen cp(kPink, 1.6f);
        g.DrawLine(&cp, x, f.r.Y + 9, x, f.r.Y + f.r.Height - 9);
    }
}

static void DrawBar(Graphics &g, RectF r, float p)
{
    GraphicsPath bg;
    RoundRect(bg, r, r.Height / 2);
    SolidBrush b(Color(70, 255, 79, 139));
    g.FillPath(&b, &bg);
    RectF fr = r;
    if (p >= 0) fr.Width = max(r.Height, r.Width * min(p, 1.0f));
    else {   // indeterminee : un segment qui glisse
        float w = r.Width * 0.3f, t = fmodf(g_time * 0.8f, 1.0f);
        fr.X = r.X - w + (r.Width + w) * t;
        fr.Width = w;
        g.SetClip(&bg);
    }
    GraphicsPath fp;
    RoundRect(fp, fr, r.Height / 2);
    LinearGradientBrush lg(RectF(fr.X - 1, fr.Y, fr.Width + 2, fr.Height), kPink, kOrange, LinearGradientModeHorizontal);
    g.FillPath(&lg, &fp);
    g.ResetClip();
}


// ---------------------------------------------------------------- options (vccoop.ini du jeu)
// Les memes cles que le menu COOP du jeu (menu.cpp SaveIni) et que dllmain.cpp LoadConfig, memes valeurs par defaut ;
// ecrites tout de suite, prises au prochain lancement. Onglets RENDU et EFFETS : seulement avec Rendu=9 (Direct3D 9).
enum { TAB_VIDEO, TAB_RENDER, TAB_FX, TAB_COOP, TAB_LOBBY, TAB_SKIN, TAB_MODS, TAB_NOTES, TAB_RT, TAB_COUNT };
static void DrawNotes(Graphics &g);
static float NotesMaxScroll();
static bool NotesUnseen();
static void NotesMarkSeen();
static void DrawMods(Graphics &g);
static float ModsMaxScroll();
static void ModsScan();
static void DrawLobby(Graphics &g);
static bool LobbyClick(float x, float y);
static void DrawSkin(Graphics &g);
static void SkinsInit();
static float SkinMaxScroll();
enum { O_TOGGLE, O_CHOICE };
enum { W_ALL, W_HOST, W_GUEST };
struct Opt {
    int tab; const char *key; int def; int kind; std::vector<int> vals;
    const wchar_t *fr, *en;
    std::vector<const wchar_t *> labFr, labEn;   // vide : la valeur + suffixe
    const wchar_t *suffix; int who;
    const wchar_t *dFr, *dEn;
    int rend;   // 0 : tout moteur ; 1 : Direct3D 9 sans ray tracing (cascades) ; 2 : ray tracing seulement (Rendu=12)
};
static std::vector<Opt> g_opts;
static int g_tab = -1, g_optHot = -1, g_optPart = 0, g_tabHot = -1;
static float g_scroll[TAB_COUNT];
static RectF g_tabR[TAB_COUNT];
static const RectF kOptPanel(440, 116, 512, 472), kOptList(452, 128, 488, 396);
static const float kRowH = 34;

static std::vector<int> Range(int a, int b, int step) { std::vector<int> v; for (int i = a; i <= b; i += step) v.push_back(i); return v; }

static void BuildOptions()
{
    auto T2 = [](int tab, const char *key, int def, const wchar_t *fr, const wchar_t *en, const wchar_t *dFr, const wchar_t *dEn, int who = W_ALL) {
        Opt o; o.tab = tab; o.key = key; o.def = def; o.kind = O_TOGGLE; o.vals = { 0, 1 }; o.fr = fr; o.en = en; o.suffix = L""; o.who = who; o.dFr = dFr; o.dEn = dEn; o.rend = 0;
        g_opts.push_back(o);
    };
    auto C = [](int tab, const char *key, int def, std::vector<int> vals, const wchar_t *fr, const wchar_t *en, std::vector<const wchar_t *> lf,
                std::vector<const wchar_t *> le, const wchar_t *suffix, const wchar_t *dFr, const wchar_t *dEn, int who = W_ALL) {
        Opt o; o.tab = tab; o.key = key; o.def = def; o.kind = O_CHOICE; o.vals = vals; o.fr = fr; o.en = en; o.labFr = lf; o.labEn = le;
        o.suffix = suffix; o.who = who; o.dFr = dFr; o.dEn = dEn; o.rend = 0;
        g_opts.push_back(o);
    };
    // VIDEO
    C(TAB_VIDEO, "Rendu", 9, { 12, 9, 8 }, L"Moteur de rendu", L"Renderer", { L"Ray tracing (DXR)", L"Direct3D 9", L"Direct3D 8" },
      { L"Ray tracing (DXR)", L"Direct3D 9", L"Direct3D 8" }, L"",
      L"Ray tracing : rendu moderne + vrais rayons (RTX 20, RX 6000 et plus r\u00E9centes). Direct3D 9 : rendu moderne. Direct3D 8 : d'origine.",
      L"Ray tracing: modern rendering + real rays (RTX 20, RX 6000 and newer). Direct3D 9: modern rendering. Direct3D 8: original.");
    C(TAB_VIDEO, "Fenetre", 1, { 2, 1, 0 }, L"Affichage", L"Display", { L"Plein \u00E9cran fen\u00EAtr\u00E9", L"Fen\u00EAtre", L"Plein \u00E9cran" },
      { L"Borderless", L"Windowed", L"Fullscreen" }, L"",
      L"Plein \u00E9cran fen\u00EAtr\u00E9 (conseill\u00E9) : \u00E0 la r\u00E9solution du bureau. Fen\u00EAtre : 1280x720.",
      L"Borderless (recommended): desktop resolution. Windowed: 1280x720.");
    T2(TAB_VIDEO, "GrandEcran", 1, L"Grand \u00E9cran", L"Widescreen", L"Image et interface au vrai format de l'\u00E9cran (16:9, 21:9, 32:9) au lieu du 4:3 \u00E9tir\u00E9.",
       L"Picture and HUD at the screen's real aspect ratio (16:9, 21:9, 32:9) instead of stretched 4:3.");
    C(TAB_VIDEO, "ImagesParSeconde", 30, { 30, 45, 60, 90, 120, 144 }, L"Images par seconde", L"Frame rate", {}, {}, L" i/s",
      L"30 = jeu d'origine (conseill\u00E9). Au-dessus, bogues du jeu d'origine (par ex. monter dans les v\u00E9hicules \u00E0 60).",
      L"30 = original game (recommended). Above it, original game bugs (e.g. entering vehicles at 60).");
    C(TAB_VIDEO, "DistanceAffichage", 200, Range(100, 400, 25), L"Distance d'affichage", L"Draw distance", {}, {}, L" %",
      L"D\u00E9tails, passants et v\u00E9hicules plus loin. 100 % = jeu d'origine.", L"Details, pedestrians and vehicles further away. 100% = original game.");
    C(TAB_VIDEO, "Anticrenelage", 4, { 0, 2, 4, 8 }, L"Anticr\u00E9nelage", L"Anti-aliasing", { L"Non", L"2x", L"4x", L"8x" }, { L"Off", L"2x", L"4x", L"8x" }, L"",
      L"Bords des objets lisses (MSAA).", L"Smooth object edges (MSAA).");
    T2(TAB_VIDEO, "FiltrageAnisotrope", 1, L"Filtrage anisotrope", L"Anisotropic filtering", L"Textures nettes au loin (16x, trilin\u00E9aire).", L"Sharp textures in the distance (16x, trilinear).");
    T2(TAB_VIDEO, "SansIntro", 1, L"Passer les vid\u00E9os", L"Skip intro videos", L"Pas de vid\u00E9os Rockstar au d\u00E9marrage.", L"No Rockstar videos at startup.");
    T2(TAB_VIDEO, "VuePremierePersonne", 1, L"Vue \u00E0 la 1re personne (F6)", L"First-person view (F6)", L"F6 passe en vue depuis la t\u00EAte de Tommy, \u00E0 pied et en v\u00E9hicule.",
       L"F6 switches to a view from Tommy's head, on foot and in vehicles.");
    T2(TAB_VIDEO, "CameraLibre", 1, L"Cam\u00E9ra libre", L"Free camera", L"Cam\u00E9ra \u00E0 la souris autour du v\u00E9hicule, vis\u00E9e au clic droit (comme GTA V).",
       L"Mouse camera around the vehicle, aim with right click (like GTA V).");
    C(TAB_VIDEO, "SensibiliteCamera", 100, Range(25, 300, 25), L"Sensibilit\u00E9 de la cam\u00E9ra", L"Camera sensitivity", {}, {}, L" %",
      L"Vitesse de la cam\u00E9ra libre \u00E0 la souris.", L"Speed of the free mouse camera.");
    T2(TAB_VIDEO, "AfficherPseudos", 1, L"Pseudos des joueurs", L"Player names", L"Pseudo des autres joueurs au-dessus de leur t\u00EAte.", L"Other players' names above their heads.");
    T2(TAB_VIDEO, "CorpsMous", 1, L"Corps mous", L"Ragdolls", L"Un personnage tu\u00E9 ou percut\u00E9 s'effondre et roule comme un vrai corps.",
       L"A killed or run-over character collapses and tumbles like a real body.");
    T2(TAB_VIDEO, "ChargerASI", 1, L"Mods .asi", L".asi mods", L"Charge les mods .asi du dossier du jeu, de scripts et de plugins.", L"Loads .asi mods from the game, scripts and plugins folders.");
    // RENDU MODERNE
    T2(TAB_RENDER, "OmbresSoleil", 1, L"Ombres du soleil", L"Sun shadows", L"B\u00E2timents, palmiers, v\u00E9hicules et personnages projettent une ombre qui suit le soleil.",
       L"Buildings, palm trees, vehicles and characters cast shadows that follow the sun.");
    C(TAB_RENDER, "OmbresResolution", 4096, { 2048, 4096, 8192 }, L"Qualit\u00E9 des ombres", L"Shadow quality", { L"Moyenne", L"Haute", L"Ultra" },
      { L"Medium", L"High", L"Ultra" }, L"", L"Ultra : carte graphique r\u00E9cente.", L"Ultra: recent graphics card.");
    g_opts.back().rend = 1;   // (ray tracing : pas de cartes d'ombre)
    // RAY TRACING (Rendu=12) : onglet a part, seulement avec ce moteur (memes cles que l'onglet RAY TRACING du panneau F10)
    T2(TAB_RT, "RTOmbres", 1, L"Ombres trac\u00E9es", L"Ray-traced shadows", L"Ombres du soleil et de la lune par de vrais rayons, \u00E0 la place des cascades.",
       L"Sun and moon shadows from real rays, instead of the cascades.");
    C(TAB_RT, "RTRayons", 4, { 1, 2, 4, 8, 16 }, L"Rayons d'ombre par pixel", L"Shadow rays per pixel", {}, {}, L"",
      L"Plus de rayons : p\u00E9nombre plus lisse, carte plus sollicit\u00E9e.", L"More rays: smoother penumbra, more GPU work.");
    C(TAB_RT, "RTDouceur", 1, { 0, 1, 2 }, L"Douceur des ombres", L"Shadow softness", { L"Nettes", L"Douces", L"Tr\u00E8s douces" }, { L"Sharp", L"Soft", L"Very soft" }, L"",
      L"Taille apparente du soleil : p\u00E9nombre plus ou moins large loin des objets.", L"Apparent size of the sun: wider or narrower penumbra away from objects.");
    C(TAB_RT, "RTDistance", 600, { 200, 400, 600, 1000, 1500 }, L"Port\u00E9e des ombres", L"Shadow distance", {}, {}, L" m",
      L"Au-del\u00E0, les ombres s'effacent.", L"Beyond it, shadows fade out.");
    C(TAB_RT, "RTResolution", 50, { 50, 100 }, L"R\u00E9solution des rayons", L"Ray resolution", { L"Demie", L"Pleine" }, { L"Half", L"Full" }, L"",
      L"Pleine : plus fin (bords des reflets), quatre fois plus de rayons.", L"Full: finer (reflection edges), four times as many rays.");
    C(TAB_RT, "RTLissage", 1, { 0, 1, 2 }, L"Lissage du bruit", L"Noise smoothing", { L"Faible", L"Moyen", L"Fort" }, { L"Low", L"Medium", L"High" }, L"",
      L"Fort : image plus calme, un peu de tra\u00EEn\u00E9e en bougeant. Faible : plus r\u00E9actif, plus de grain.",
      L"High: calmer picture, slight trailing when moving. Low: more responsive, more grain.");
    T2(TAB_RT, "RTReflets", 1, L"Reflets trac\u00E9s", L"Ray-traced reflections", L"La rue, le ciel et les voitures se refl\u00E8tent vraiment sur les carrosseries et les sols mouill\u00E9s.",
       L"The street, the sky and cars really reflect on car bodies and wet ground.");
    C(TAB_RT, "RTRefletsForce", 100, { 25, 50, 75, 100, 150, 200 }, L"Force des reflets", L"Reflection strength", {}, {}, L" %",
      L"Intensit\u00E9 des reflets.", L"Reflection intensity.");
    C(TAB_RT, "RTRefletsSol", 0, { 0, 1, 2 }, L"Sols brillants", L"Shiny ground", { L"Pluie", L"L\u00E9gers", L"Miroirs" }, { L"Rain", L"Light", L"Mirrors" }, L"",
      L"Pluie : le sol ne refl\u00E8te que mouill\u00E9. L\u00E9gers : toujours un peu. Miroirs : comme une chauss\u00E9e d'apr\u00E8s l'averse.",
      L"Rain: the ground only reflects when wet. Light: always a little. Mirrors: like a road after a downpour.");
    T2(TAB_RT, "RTLumiere", 1, L"Lumi\u00E8re trac\u00E9e", L"Ray-traced lighting", L"Les murs et le sol \u00E9clair\u00E9s renvoient leur lumi\u00E8re et leur couleur autour d'eux (un rebond).",
       L"Lit walls and ground bounce their light and color around them (one bounce).");
    C(TAB_RT, "RTLumiereForce", 100, { 25, 50, 75, 100, 150, 200 }, L"Force de la lumi\u00E8re", L"Lighting strength", {}, {}, L" %",
      L"Intensit\u00E9 de la lumi\u00E8re renvoy\u00E9e.", L"Intensity of the bounced light.");
    T2(TAB_RT, "RTLampes", 1, L"Lampes trac\u00E9es", L"Ray-traced lamps", L"Lampadaires, n\u00E9ons et phares \u00E9clairent avec de vraies ombres, toutes les lampes.",
       L"Street lamps, neons and headlights light up with real shadows, every lamp.");
    T2(TAB_RT, "RTOcclusion", 1, L"Occlusion trac\u00E9e", L"Ray-traced occlusion", L"Coins, pieds des murs et dessous des voitures assombris par de vrais rayons.",
       L"Corners, wall bases and car undersides darkened by real rays.");
    C(TAB_RT, "RTOcclusionForce", 100, { 25, 50, 75, 100, 150 }, L"Force de l'occlusion", L"Occlusion strength", {}, {}, L" %",
      L"Assombrissement des coins.", L"How dark corners get.");
    T2(TAB_RENDER, "EauModerne", 1, L"Eau moderne", L"Modern water", L"Turquoise selon la profondeur, fond visible, vagues et \u00E9cume sur les rives.",
       L"Turquoise by depth, visible sea floor, waves and foam on the shores.");
    T2(TAB_RENDER, "RefletsEau", 1, L"Reflets sur l'eau", L"Water reflections", L"Quais, bateaux, palmiers et immeubles se refl\u00E8tent dans l'eau.",
       L"Docks, boats, palm trees and buildings reflect in the water.");
    T2(TAB_RENDER, "LumieresDynamiques", 1, L"Lumi\u00E8res dynamiques", L"Dynamic lights", L"Lampadaires, n\u00E9ons, phares, explosions et tirs \u00E9clairent le d\u00E9cor.",
       L"Street lamps, neons, headlights, explosions and gunfire light up the scene.");
    C(TAB_RENDER, "OmbresLumieres", 4, Range(0, 4, 1), L"Ombres des lumi\u00E8res", L"Light shadows", {}, {}, L"",
      L"Nombre de lumi\u00E8res proches (phares, lampadaires) qui projettent une ombre.", L"Number of nearby lights (headlights, street lamps) casting shadows.");
    T2(TAB_RENDER, "OmbresLune", 1, L"Ombres de la lune", L"Moon shadows", L"La lune (0 h \u00E0 6 h) projette des ombres plus faibles.", L"The moon (midnight to 6 am) casts fainter shadows.");
    T2(TAB_RENDER, "OcclusionAmbiante", 1, L"Occlusion ambiante", L"Ambient occlusion", L"Coins, pieds des murs et dessous des voitures un peu assombris.",
       L"Corners, wall bases and car undersides slightly darkened.");
    g_opts.back().rend = 1;   // (ray tracing : occlusion tracee)
    // EFFETS
    T2(TAB_FX, "SMAA", 1, L"SMAA", L"SMAA", L"Bords en escalier liss\u00E9s sur toute l'image.", L"Jagged edges smoothed over the whole picture.");
    T2(TAB_FX, "Eclat", 1, L"\u00C9clat", L"Bloom", L"N\u00E9ons, soleil et phares d\u00E9bordent en lumi\u00E8re douce.", L"Neons, sun and headlights glow softly.");
    C(TAB_FX, "Etalonnage", 1, { 0, 1, 2 }, L"\u00C9talonnage", L"Color grading", { L"Original", L"Vice", L"Film" }, { L"Original", L"Vice", L"Film" }, L"",
      L"Vice : couleurs \u00AB Miami 80 \u00BB. Film : contraste de cin\u00E9ma.", L"Vice: \"Miami 80s\" colors. Film: cinema contrast.");
    C(TAB_FX, "Nettete", 40, Range(0, 100, 10), L"Nettet\u00E9", L"Sharpening", {}, {}, L" %", L"Renforce les d\u00E9tails de l'image.", L"Brings out picture detail.");
    T2(TAB_FX, "LampadairesEclairent", 1, L"R\u00E9verb\u00E8res et n\u00E9ons", L"Street lamps and neons", L"Ils \u00E9clairent vraiment la rue la nuit ; phare d'Ocean Beach tournant.",
       L"They really light the street at night; rotating Ocean Beach lighthouse.");
    T2(TAB_FX, "ParticulesDouces", 1, L"Particules douces", L"Soft particles", L"Fum\u00E9e et explosions sans coupure nette contre le d\u00E9cor.", L"Smoke and explosions without hard edges against the scene.");
    T2(TAB_FX, "RoutesMouillees", 1, L"Routes mouill\u00E9es", L"Wet roads", L"Reflets des n\u00E9ons et des phares, flaques sous la pluie ; sols brillants.",
       L"Neon and headlight reflections, puddles in the rain; shiny floors.");
    T2(TAB_FX, "RayonsSoleil", 1, L"Rayons de soleil", L"Sun rays", L"Rayons du soleil \u00E0 travers le d\u00E9cor.", L"Sun rays through the scenery.");
    T2(TAB_FX, "VegetationVent", 1, L"Palmiers au vent", L"Palm trees in the wind", L"Palmiers et arbres bougent avec le vent.", L"Palm trees and trees sway in the wind.");
    T2(TAB_FX, "FaisceauxPhares", 1, L"Faisceaux des phares", L"Headlight beams", L"Faisceaux visibles dans la pluie et le brouillard.", L"Beams visible in rain and fog.");
    T2(TAB_FX, "Brume", 1, L"Brume", L"Haze", L"Brume au loin qui suit l'heure.", L"Distant haze that follows the time of day.");
    T2(TAB_FX, "RefletsVoitures", 1, L"Reflets des voitures", L"Car reflections", L"La ville se refl\u00E8te sur les carrosseries (pas sur les pneus).", L"The city reflects on car bodies (not on tyres).");
    g_opts.back().rend = 1;   // (ray tracing : reflets traces)
    T2(TAB_FX, "LumiereIndirecte", 1, L"Lumi\u00E8re indirecte", L"Indirect light", L"Les surfaces color\u00E9es renvoient leur couleur autour d'elles.", L"Colored surfaces bounce their color around.");
    g_opts.back().rend = 1;   // (ray tracing : lumiere tracee)
    // COOP
    T2(TAB_COOP, "TirAmi", 1, L"Tir ami", L"Friendly fire", L"Les joueurs peuvent se blesser entre eux (coups, balles, voiture).", L"Players can hurt each other (punches, bullets, cars).", W_HOST);
    T2(TAB_COOP, "ArgentPartage", 1, L"Argent partag\u00E9", L"Shared money", L"L'argent des missions va aussi aux invit\u00E9s.", L"Mission money also goes to guests.", W_HOST);
    T2(TAB_COOP, "RecherchePartagee", 0, L"\u00C9toiles communes", L"Shared wanted level", L"Tous les joueurs partagent les m\u00EAmes \u00E9toiles de police.", L"All players share the same wanted stars.", W_HOST);
    T2(TAB_COOP, "PoliceHote", 1, L"Police de l'h\u00F4te", L"Host's police", L"Pr\u00E8s de l'h\u00F4te, seule sa police existe et elle poursuit aussi les invit\u00E9s recherch\u00E9s.",
       L"Near the host, only their police exists and it also chases wanted guests.", W_HOST);
    C(TAB_COOP, "ZonePopulation", 150, Range(100, 200, 10), L"Zone de population", L"Population range", {}, {}, L" %",
      L"Passants et voitures naissent plus loin et restent tant qu'on est dans la zone.", L"Pedestrians and cars spawn further away and stay while you are in range.", W_HOST);
    C(TAB_COOP, "DensitePopulation", 150, Range(50, 300, 25), L"Densit\u00E9 de population", L"Population density", {}, {}, L" %",
      L"Nombre de passants et de voitures en m\u00EAme temps. 100 % = jeu d'origine.", L"Number of pedestrians and cars at once. 100% = original game.", W_HOST);
    T2(TAB_COOP, "ModsPartages", 1, L"Mods partag\u00E9s", L"Shared mods", L"Les fichiers de VCCoop\\mods remplacent ceux du jeu et sont envoy\u00E9s aux invit\u00E9s.",
       L"Files in VCCoop\\mods replace the game's and are sent to guests.");
    T2(TAB_COOP, "GarderArmes", 1, L"Garder ses armes", L"Keep weapons", L"Apr\u00E8s une mort ou une arrestation, l'invit\u00E9 garde ses armes et son argent.",
       L"After dying or being busted, the guest keeps weapons and money.", W_GUEST);
    T2(TAB_COOP, "ReapparitionHote", 0, L"R\u00E9appara\u00EEtre pr\u00E8s de l'h\u00F4te", L"Respawn near the host", L"Au lieu de l'h\u00F4pital (d\u00E9conseill\u00E9).",
       L"Instead of the hospital (not recommended).", W_GUEST);
}

static std::string GameIni() { return Narrow(g_gameDir + L"vccoop.ini"); }
static int OptGet(const Opt &o) { return GetPrivateProfileIntA("VCCoop", o.key, o.def, GameIni().c_str()); }
static void OptSet(const Opt &o, int v) { char b[16]; wsprintfA(b, "%d", v); WritePrivateProfileStringA("VCCoop", o.key, b, GameIni().c_str()); }
static int RenderVal() { return g_gameDir.empty() ? 9 : GetPrivateProfileIntA("VCCoop", "Rendu", 9, GameIni().c_str()); }
static bool Modern() { int r = RenderVal(); return r == 9 || r == 12; }
// Carte capable de ray tracing (DXR) : VCCoop\vcrt64.exe --probe au demarrage (-1 : pas encore su, 0 : non, 1 : oui).
static volatile int g_rtProbe = -1;
static DWORD WINAPI RtProbeThread(void *)
{
    std::wstring exe = g_gameDir + L"VCCoop\\vcrt64.exe";
    if (!FileExists(exe)) { g_rtProbe = 0; return 0; }
    std::wstring cmd = L"\"" + exe + L"\" --probe";
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { g_rtProbe = 0; return 0; }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    g_rtProbe = code == 0 ? 1 : 0;
    return 0;
}
static bool OptShown(const Opt &o) { return !o.rend || (o.rend == 2) == (RenderVal() == 12); }
static bool TabVisible(int t)
{
    if (t == TAB_LOBBY) return g_lobby != LB_NONE;
    if (t == TAB_SKIN) return g_imgOk;
    if (t == TAB_MODS) return !g_gameDir.empty() && g_lobby != LB_GUEST && g_lobby != LB_CONNECTING;   // (l'invite prend ceux de l'hote)
    if (t == TAB_NOTES) return true;
    if (t == TAB_RT) return !g_gameDir.empty() && RenderVal() == 12;
    return (t != TAB_RENDER && t != TAB_FX) || Modern();
}
static const wchar_t *TabName(int t)
{
    static const wchar_t *fr[] = { L"VID\u00C9O", L"RENDU", L"EFFETS", L"COOP", L"SALON", L"TENUE", L"MODS", L"NOUVEAUT\u00C9S", L"RAY TRACING" },
                         *en[] = { L"VIDEO", L"RENDERING", L"EFFECTS", L"CO-OP", L"LOBBY", L"OUTFIT", L"MODS", L"UPDATES", L"RAY TRACING" };
    return g_fr ? fr[t] : en[t];
}
static float MeasureW(Graphics &g, const std::wstring &s, float px, int style);
static float g_tabFont = 11.5f;   // police des onglets (plus petite quand ils ne tiennent pas sur la ligne)
static void LayoutTabs()
{
    float x = 440;
    static const int order[] = { TAB_LOBBY, TAB_SKIN, TAB_MODS, TAB_VIDEO, TAB_RENDER, TAB_RT, TAB_FX, TAB_COOP, TAB_NOTES };
    // Largeur de chaque libelle mesuree ; trop d'onglets pour la place (jusqu'aux boutons reduire / fermer, x 898) :
    // marges et ecarts resserres.
    float tw[TAB_COUNT] = {}, total = 0, pad = 12, gap = 6;
    int n = 0;
    {
        Bitmap bm(1, 1);
        Graphics mg(&bm);
        for (int t : order) if (TabVisible(t)) { tw[t] = MeasureW(mg, TabName(t), 11.5f, FontStyleBold); total += tw[t]; n++; }
    }
    while (pad > 4 && total + n * pad + (n - 1) * gap > 458) { pad -= 2; gap = 4; }
    g_tabFont = 11.5f;
    float room = 458 - n * pad - (n - 1) * gap;
    if (total > room && total > 0) {   // (ray tracing + salon : neuf onglets) police reduite
        float k = room / total;
        if (k < 0.72f) k = 0.72f;
        g_tabFont = 11.5f * k;
        for (int t = 0; t < TAB_COUNT; t++) tw[t] *= k;
    }
    for (int t : order) {
        if (!TabVisible(t)) { g_tabR[t] = RectF(0, 0, 0, 0); continue; }
        float w = pad + tw[t];
        g_tabR[t] = RectF(x, 78, w, 26);
        x += w + gap;
    }
    if (g_tab >= 0 && !TabVisible(g_tab)) g_tab = -1;
}
static std::vector<int> TabRows(int t) { std::vector<int> r; for (int i = 0; i < (int)g_opts.size(); i++) if (g_opts[i].tab == t && OptShown(g_opts[i])) r.push_back(i); return r; }
static float MaxScroll(int t) { return t == TAB_LOBBY ? 0.0f : t == TAB_SKIN ? SkinMaxScroll() : t == TAB_MODS ? ModsMaxScroll() : t == TAB_NOTES ? NotesMaxScroll() : max(0.0f, TabRows(t).size() * kRowH - kOptList.Height); }

static int ValueIndex(const Opt &o, int v)
{
    for (int i = 0; i < (int)o.vals.size(); i++) if (o.vals[i] == v) return i;
    for (int i = 0; i < (int)o.vals.size(); i++) if (o.vals[i] > v) return i;   // valeur hors liste : la suivante
    return (int)o.vals.size() - 1;
}
static std::wstring ValueText(const Opt &o, int v)
{
    int i = ValueIndex(o, v);
    const std::vector<const wchar_t *> &lab = g_fr ? o.labFr : o.labEn;
    if (!strcmp(o.key, "Rendu") && v == 12 && g_rtProbe == 0) return g_fr ? L"Ray tracing (carte incompatible)" : L"Ray tracing (unsupported GPU)";
    if (!lab.empty()) return lab[i];
    wchar_t b[32];
    swprintf_s(b, L"%d%s", v, (!wcscmp(o.suffix, L" i/s") && !g_fr) ? L" fps" : o.suffix);
    return b;
}
static void OptStep(int idx, int dir)
{
    const Opt &o = g_opts[idx];
    int v = OptGet(o);
    if (o.kind == O_TOGGLE) { OptSet(o, v ? 0 : 1); return; }
    int i = ValueIndex(o, v);
    if (o.vals[i] != v && dir > 0) i--;   // hors liste : un cran vers le haut = la valeur de la liste juste au-dessus
    i = (i + dir + (int)o.vals.size()) % (int)o.vals.size();
    if (!strcmp(o.key, "Rendu") && o.vals[i] == 12 && g_rtProbe == 0) i = (i + dir + (int)o.vals.size()) % (int)o.vals.size();   // carte sans DXR
    OptSet(o, o.vals[i]);
    if (!strcmp(o.key, "Rendu")) { LayoutTabs(); g_scroll[TAB_RENDER] = 0; }
}

static float MeasureW(Graphics &g, const std::wstring &s, float px, int style)
{
    FontFamily fam(L"Segoe UI");
    Font font(&fam, px, style, UnitPixel);
    RectF box;
    g.MeasureString(s.c_str(), -1, &font, PointF(0, 0), &box);
    return box.Width;
}

static void DrawTabs(Graphics &g)
{
    if (g_gameDir.empty()) return;
    for (int t = 0; t < TAB_COUNT; t++) {
        if (!TabVisible(t)) continue;
        RectF r = g_tabR[t];
        GraphicsPath p;
        RoundRect(p, r, r.Height / 2);
        bool on = g_tab == t, hot = g_tabHot == t;
        if (on) { LinearGradientBrush lg(r, kPink, kOrange, LinearGradientModeHorizontal); g.FillPath(&lg, &p); }
        else { SolidBrush b(hot ? TH(tabHot) : TH(tab)); g.FillPath(&b, &p); }
        Text(g, TabName(t), r, g_tabFont, FontStyleBold, on ? Color(255, 255, 255, 255) : Mix(kInk, kPink, hot ? 1.0f : 0.0f));
        if (t == TAB_NOTES && !on && NotesUnseen()) {   // pastille : des notes pas encore lues
            SolidBrush dot(kPink);
            g.FillEllipse(&dot, r.X + r.Width - 7, r.Y - 1, 8.0f, 8.0f);
        }
    }
}

static void DrawOptions(Graphics &g)
{
    if (g_tab < 0 || g_gameDir.empty()) return;
    if (g_tab == TAB_LOBBY) { DrawLobby(g); return; }
    if (g_tab == TAB_SKIN) { DrawSkin(g); return; }
    if (g_tab == TAB_MODS) { DrawMods(g); return; }
    if (g_tab == TAB_NOTES) { DrawNotes(g); return; }
    GraphicsPath pp;
    RoundRect(pp, kOptPanel, 18);
    SolidBrush bg(TH(panel));
    g.FillPath(&bg, &pp);
    Pen border(TH(panelBorder), 1.5f);
    g.DrawPath(&border, &pp);

    std::vector<int> rows = TabRows(g_tab);
    float sc = g_scroll[g_tab];
    g.SetClip(kOptList);
    for (int k = 0; k < (int)rows.size(); k++) {
        const Opt &o = g_opts[rows[k]];
        RectF r(kOptList.X, kOptList.Y + k * kRowH - sc, kOptList.Width - 10, kRowH);
        if (r.Y + r.Height < kOptList.Y || r.Y > kOptList.Y + kOptList.Height) continue;
        bool hot = g_optHot == rows[k];
        if (hot) { GraphicsPath hp; RoundRect(hp, RectF(r.X, r.Y + 2, r.Width, r.Height - 4), 9); SolidBrush hb(TH(cardSel)); g.FillPath(&hb, &hp); }
        std::wstring label = g_fr ? o.fr : o.en;
        Text(g, label, RectF(r.X + 12, r.Y, 280, r.Height), 13.5f, FontStyleRegular, kInk, StringAlignmentNear);
        if (o.who != W_ALL) {   // pastille HOTE / INVITE
            float lw = MeasureW(g, label, 13.5f, FontStyleRegular);
            const wchar_t *bt = o.who == W_HOST ? T(L"H\u00D4TE", L"HOST") : T(L"INVIT\u00C9", L"GUEST");
            RectF br(r.X + 12 + lw + 6, r.Y + 10, 12 + 6.2f * (float)wcslen(bt), 15);
            GraphicsPath bp; RoundRect(bp, br, 7.5f);
            SolidBrush bb(o.who == W_HOST ? Color(40, 255, 79, 139) : Color(40, 90, 110, 200));
            g.FillPath(&bb, &bp);
            Text(g, bt, br, 9, FontStyleBold, o.who == W_HOST ? kPink : Color(255, 80, 96, 180));
        }
        int v = OptGet(o);
        if (o.kind == O_TOGGLE) {
            RectF tr(r.X + r.Width - 54, r.Y + 7, 42, 20);
            GraphicsPath tp; RoundRect(tp, tr, 10);
            if (v) { LinearGradientBrush lg(tr, kPink, kOrange, LinearGradientModeHorizontal); g.FillPath(&lg, &tp); }
            else { SolidBrush ob(TH(toggleOff)); g.FillPath(&ob, &tp); }
            SolidBrush knob(Color(255, 255, 255, 255));
            g.FillEllipse(&knob, v ? tr.X + 24 : tr.X + 2, tr.Y + 2, 16.0f, 16.0f);
        } else {
            RectF cr(r.X + r.Width - 190, r.Y + 5, 178, 24);
            GraphicsPath cp; RoundRect(cp, cr, 12);
            SolidBrush cb(TH(card)); g.FillPath(&cb, &cp);
            Pen cpen(TH(choiceBorder), 1.2f); g.DrawPath(&cpen, &cp);
            Color al = (hot && g_optPart < 0) ? kPink : Color(200, 255, 79, 139), ar = (hot && g_optPart > 0) ? kPink : Color(200, 255, 79, 139);
            Text(g, L"\u2039", RectF(cr.X + 4, cr.Y - 2, 18, cr.Height), 18, FontStyleBold, al);
            Text(g, L"\u203A", RectF(cr.X + cr.Width - 22, cr.Y - 2, 18, cr.Height), 18, FontStyleBold, ar);
            Text(g, ValueText(o, v), RectF(cr.X + 20, cr.Y, cr.Width - 40, cr.Height), 12.5f, FontStyleBold, kInk);
        }
    }
    g.ResetClip();
    float ms = MaxScroll(g_tab);
    if (ms > 0) {   // barre de defilement
        float h = kOptList.Height * kOptList.Height / (kOptList.Height + ms), y = kOptList.Y + (kOptList.Height - h) * sc / ms;
        GraphicsPath sp; RoundRect(sp, RectF(kOptList.X + kOptList.Width - 5, y, 4, h), 2);
        SolidBrush sb(Color(120, 255, 79, 139)); g.FillPath(&sb, &sp);
    }
    // description de la ligne survolee
    Pen sep(TH(sep), 1);
    g.DrawLine(&sep, kOptPanel.X + 18, 532.0f, kOptPanel.X + kOptPanel.Width - 18, 532.0f);
    std::wstring d = g_optHot >= 0 ? (g_fr ? g_opts[g_optHot].dFr : g_opts[g_optHot].dEn)
                                   : T(L"Pris au prochain lancement du jeu. Le menu COOP du jeu modifie les m\u00EAmes r\u00E9glages.",
                                       L"Applied the next time the game starts. The game's COOP menu changes the same settings.");
    FontFamily fam(L"Segoe UI");
    Font font(&fam, 12, FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisWord);
    SolidBrush db(kGrey);
    g.DrawString(d.c_str(), -1, &font, RectF(kOptPanel.X + 20, 536, kOptPanel.Width - 40, 44), &sf, &db);
}

// Survol : ligne d'option et cote du selecteur (-1 gauche, +1 droite, 0 libelle)
static void HitOption(float x, float y, int *row, int *part)
{
    *row = -1; *part = 0;
    if (g_tab < 0 || g_tab == TAB_LOBBY || g_tab == TAB_SKIN || g_tab == TAB_MODS || g_tab == TAB_NOTES || !kOptList.Contains(x, y)) return;
    std::vector<int> rows = TabRows(g_tab);
    int k = (int)((y - kOptList.Y + g_scroll[g_tab]) / kRowH);
    if (k < 0 || k >= (int)rows.size()) return;
    *row = rows[k];
    const Opt &o = g_opts[*row];
    float right = kOptList.X + kOptList.Width - 10;
    if (o.kind == O_CHOICE && x >= right - 190) *part = x < right - 190 + 89 ? -1 : 1;
}
static int HitTab(float x, float y)
{
    if (g_state != ST_IDLE || g_gameDir.empty()) return -1;
    for (int t = 0; t < TAB_COUNT; t++) if (TabVisible(t) && g_tabR[t].Contains(x, y)) return t;
    return -1;
}

// ---------------------------------------------------------------- onglet NOUVEAUTES / UPDATES
// Notes des versions publiees sur GitHub (releases, 40 dernieres) : le texte de chaque release est en francais,
// puis une ligne "---", puis en anglais (dist\make-release.ps1 -Notes). Gardees dans VCCoop\notes-maj.json pour
// les lire hors ligne. Pastille sur l'onglet tant que la plus recente n'a pas ete vue (NotesVues du lanceur).
struct Note { std::wstring ver, date, fr, en; };
static std::vector<Note> g_notes;
static volatile bool g_notesDone;
static float g_notesH;

// Chaine JSON a partir de son guillemet ouvrant, echappements decodes (en UTF-8).
static std::string JsonDecode(const std::string &json, size_t q)
{
    std::string v;
    for (size_t i = q + 1; i < json.size() && json[i] != '"'; i++) {
        char c = json[i];
        if (c != '\\' || i + 1 >= json.size()) { v += c; continue; }
        char e = json[++i];
        if (e == 'n') v += '\n';
        else if (e == 'r') {}
        else if (e == 't') v += ' ';
        else if (e == 'u' && i + 4 < json.size()) {
            unsigned cp = strtoul(json.substr(i + 1, 4).c_str(), NULL, 16);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDFFF) continue;   // emoji (paires) : laisses
            if (cp < 0x80) v += (char)cp;
            else if (cp < 0x800) { v += (char)(0xC0 | (cp >> 6)); v += (char)(0x80 | (cp & 0x3F)); }
            else { v += (char)(0xE0 | (cp >> 12)); v += (char)(0x80 | ((cp >> 6) & 0x3F)); v += (char)(0x80 | (cp & 0x3F)); }
        } else v += e;
    }
    return v;
}
static std::string JsonField(const std::string &json, const char *key, size_t from, size_t to)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k, from);
    if (p == std::string::npos || p >= to) return "";
    p = json.find(':', p + k.size());
    size_t q = p == std::string::npos ? p : json.find_first_not_of(" \t\r\n", p + 1);
    return q != std::string::npos && json[q] == '"' ? JsonDecode(json, q) : "";
}
// Markdown simple : titres, gras et code retires ; puces "- " -> "•".
static std::wstring CleanNote(const std::wstring &s)
{
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        bool lineStart = i == 0 || s[i - 1] == L'\n';
        if (s[i] == L'`') continue;
        if (s[i] == L'*' && i + 1 < s.size() && s[i + 1] == L'*') { i++; continue; }
        if (lineStart && s[i] == L'#') { while (i < s.size() && (s[i] == L'#' || s[i] == L' ')) i++; i--; continue; }
        if (lineStart && (s[i] == L'-' || s[i] == L'*') && i + 1 < s.size() && s[i + 1] == L' ') { o += L"\u2022"; continue; }
        o += s[i];
    }
    size_t b = o.find_first_not_of(L"\n "), e = o.find_last_not_of(L"\n ");
    return b == std::wstring::npos ? L"" : o.substr(b, e - b + 1);
}
static void ParseNotes(const std::string &json)
{
    std::vector<Note> list;
    for (size_t at = 0;;) {
        size_t p = json.find("\"tag_name\"", at);
        if (p == std::string::npos) break;
        size_t next = json.find("\"tag_name\"", p + 10), end = next == std::string::npos ? json.size() : next;
        Note n;
        n.ver = Widen(JsonField(json, "tag_name", p, end));
        if (!n.ver.empty() && (n.ver[0] == L'v' || n.ver[0] == L'V')) n.ver.erase(0, 1);
        std::string d = JsonField(json, "published_at", p, end);
        if (d.size() >= 10) n.date = Widen(d.substr(8, 2) + "/" + d.substr(5, 2) + "/" + d.substr(0, 4));
        std::wstring body = Widen(JsonField(json, "body", p, end));
        size_t sep = body.find(L"\n---");
        if (sep == std::wstring::npos) n.fr = n.en = CleanNote(body);
        else {
            size_t enAt = body.find(L'\n', sep + 1);
            n.fr = CleanNote(body.substr(0, sep));
            n.en = CleanNote(enAt == std::wstring::npos ? L"" : body.substr(enAt + 1));
            if (n.en.empty()) n.en = n.fr;
        }
        if (!n.ver.empty()) list.push_back(n);
        at = end;
    }
    EnterCriticalSection(&g_cs);
    g_notes = list;
    LeaveCriticalSection(&g_cs);
}
static void NotesFetch()
{
    std::wstring cache = g_dir + L"VCCoop\\notes-maj.json";
    std::string json;
    if (HttpGet(L"https://api.github.com/repos/Parricidium/VCCoop/releases?per_page=40", &json, L"", false) && json.find("\"tag_name\"") != std::string::npos) {
        HANDLE f = CreateFileW(cache.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, json.data(), (DWORD)json.size(), &w, NULL); CloseHandle(f); }
    } else {
        json.clear();
        HANDLE f = CreateFileW(cache.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(f, NULL), r = 0;
            if (size != INVALID_FILE_SIZE && size < 8u << 20) { json.resize(size); ReadFile(f, &json[0], size, &r, NULL); json.resize(r); }
            CloseHandle(f);
        }
    }
    if (!json.empty()) ParseNotes(json);
    g_notesDone = true;
}
static DWORD WINAPI NotesThread(void *) { NotesFetch(); return 0; }

static std::wstring NewestNote()
{
    EnterCriticalSection(&g_cs);
    std::wstring v = g_notes.empty() ? L"" : g_notes[0].ver;
    LeaveCriticalSection(&g_cs);
    return v;
}
static bool NotesUnseen()
{
    std::wstring v = NewestNote();
    if (v.empty()) return false;
    wchar_t seen[64] = {};
    GetPrivateProfileStringW(L"Lanceur", L"NotesVues", L"", seen, 64, g_iniLauncher.c_str());
    return !seen[0] || CmpVer(v, seen) > 0;
}
static void NotesMarkSeen()
{
    std::wstring v = NewestNote();
    if (!v.empty()) WritePrivateProfileStringW(L"Lanceur", L"NotesVues", v.c_str(), g_iniLauncher.c_str());
}

static RectF NotesArea() { return RectF(kOptList.X, kOptList.Y, kOptList.Width, kOptPanel.Y + kOptPanel.Height - 14 - kOptList.Y); }
static float NotesMaxScroll() { return max(0.0f, g_notesH - NotesArea().Height); }

static void DrawNotes(Graphics &g)
{
    GraphicsPath pp;
    RoundRect(pp, kOptPanel, 18);
    SolidBrush bg(TH(panel));
    g.FillPath(&bg, &pp);
    Pen border(TH(panelBorder), 1.5f);
    g.DrawPath(&border, &pp);
    RectF area = NotesArea();
    std::vector<Note> notes;
    EnterCriticalSection(&g_cs);
    notes = g_notes;
    LeaveCriticalSection(&g_cs);
    if (notes.empty()) {
        Text(g, g_notesDone ? T(L"Notes de version indisponibles (hors ligne).", L"Release notes unavailable (offline).")
                            : T(L"Chargement des notes de version\u2026", L"Loading release notes\u2026"), area, 13, FontStyleRegular, kGrey);
        return;
    }
    FontFamily fam(L"Segoe UI");
    Font fh(&fam, 14.5f, FontStyleBold, UnitPixel), fd(&fam, 11.5f, FontStyleRegular, UnitPixel), fb(&fam, 12.5f, FontStyleRegular, UnitPixel);
    StringFormat sf;
    SolidBrush ink(kInk), grey(kGrey), pink(kPink);
    Pen sep(TH(sep), 1);
    float sc = g_scroll[TAB_NOTES], y = area.Y - sc, w = area.Width - 14;
    g.SetClip(area);
    for (size_t i = 0; i < notes.size(); i++) {
        const Note &n = notes[i];
        const std::wstring &body = g_fr ? n.fr : n.en;
        RectF box;
        g.MeasureString(body.c_str(), -1, &fb, RectF(0, 0, w - 16, 100000), &sf, &box);
        float h = 26 + (body.empty() ? 0 : box.Height) + 16;
        if (y + h >= area.Y && y <= area.Y + area.Height) {
            std::wstring title = L"VCCoop " + n.ver;
            g.DrawString(title.c_str(), -1, &fh, PointF(area.X + 6, y), &pink);
            float tw = MeasureW(g, title, 14.5f, FontStyleBold);
            g.DrawString(n.date.c_str(), -1, &fd, PointF(area.X + 12 + tw, y + 3), &grey);
            int cmp = g_localVer.empty() ? 1 : CmpVer(n.ver, g_localVer);
            if (cmp >= 0 && !g_localVer.empty()) {   // version installee, ou plus recente (a venir)
                const wchar_t *lab = cmp == 0 ? T(L"INSTALL\u00C9E", L"INSTALLED") : T(L"NOUVELLE", L"NEW");
                RectF br(area.X + w - 12 - 7.0f * (float)wcslen(lab), y + 2, 12 + 7.0f * (float)wcslen(lab), 17);
                GraphicsPath bp; RoundRect(bp, br, 8.5f);
                SolidBrush bb(cmp == 0 ? Color(45, 38, 150, 96) : Color(45, 255, 79, 139));
                g.FillPath(&bb, &bp);
                Text(g, lab, br, 9.5f, FontStyleBold, cmp == 0 ? Color(255, 38, 150, 96) : kPink);
            }
            if (!body.empty()) g.DrawString(body.c_str(), -1, &fb, RectF(area.X + 10, y + 26, w - 16, box.Height + 4), &sf, &ink);
            if (i + 1 < notes.size()) g.DrawLine(&sep, area.X + 6, y + h - 8, area.X + w, y + h - 8);
        }
        y += h;
    }
    g.ResetClip();
    g_notesH = y + sc - area.Y;
    float ms = NotesMaxScroll();
    if (ms > 0) {
        float bh = area.Height * area.Height / (area.Height + ms), by = area.Y + (area.Height - bh) * sc / ms;
        GraphicsPath sp; RoundRect(sp, RectF(area.X + area.Width - 5, by, 4, bh), 2);
        SolidBrush sb(Color(120, 255, 79, 139)); g.FillPath(&sb, &sp);
    }
}

// Salon : arrivees, departs et changements "pret" entendus (compare la liste d'une image a l'autre).
static void LobbySoundsTick()
{
    static std::vector<std::pair<int, bool>> prev;
    static bool had;
    std::vector<std::pair<int, bool>> now;
    if (g_lobby == LB_HOST || g_lobby == LB_GUEST) {
        EnterCriticalSection(&g_lcs);
        for (auto &p : g_peers) now.push_back({ p.id, p.ready });
        LeaveCriticalSection(&g_lcs);
    }
    if (!had || (g_lobby != LB_HOST && g_lobby != LB_GUEST)) { prev = now; had = g_lobby == LB_HOST || g_lobby == LB_GUEST; return; }
    int sound = -1;
    for (auto &n : now) {
        bool found = false;
        for (auto &o : prev) if (o.first == n.first) { found = true; if (o.second != n.second && sound < 0) sound = n.second ? SND_READY : SND_UNREADY; }
        if (!found) sound = SND_JOIN;
    }
    for (auto &o : prev) {
        bool still = false;
        for (auto &n : now) still |= n.first == o.first;
        if (!still && sound != SND_JOIN) sound = SND_LEAVE;
    }
    prev = now;
    if (sound >= 0 && g_testLog.empty()) LobbySound(sound);   // (modes de test : silencieux)
}

static void DrawUI(Graphics &g)
{
    UpdateButtons();
    std::wstring status;
    int kind;
    EnterCriticalSection(&g_cs);
    status = g_status; kind = g_statusKind;
    LeaveCriticalSection(&g_cs);
    Color sc = kind == K_OK ? Color(255, 38, 150, 96) : kind == K_WARN ? Color(255, 205, 120, 30) : kind == K_ERR ? Color(255, 214, 48, 72) : kGrey;
    float prog = g_progress;

    // fermer / reduire (sur l'image, en haut a droite de la carte)
    {   // theme : lune (passer en sombre) ou soleil (passer en clair)
        Button &b = g_btn[B_THEME];
        SolidBrush cb(Mix(TH(circle), TH(circleHot), b.hover));
        g.FillEllipse(&cb, b.r);
        Color ic = Mix(kInk, kPink, b.hover);
        float cx = b.r.X + b.r.Width / 2, cy = b.r.Y + b.r.Height / 2;
        if (!g_dark) {
            SolidBrush moon(ic);
            GraphicsPath mp;
            mp.AddEllipse(cx - 6.5f, cy - 6.5f, 13.0f, 13.0f);
            Region rg(&mp);
            GraphicsPath cut;
            cut.AddEllipse(cx - 2.5f, cy - 9.0f, 13.0f, 13.0f);
            rg.Exclude(&cut);
            g.FillRegion(&moon, &rg);
        } else {
            SolidBrush sun(ic);
            g.FillEllipse(&sun, cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
            Pen ray(ic, 1.6f);
            ray.SetStartCap(LineCapRound); ray.SetEndCap(LineCapRound);
            for (int k = 0; k < 8; k++) { float a = k * 0.7854f; g.DrawLine(&ray, cx + cosf(a) * 6.5f, cy + sinf(a) * 6.5f, cx + cosf(a) * 9.0f, cy + sinf(a) * 9.0f); }
        }
    }
    for (int id : { B_MIN, B_CLOSE }) {
        Button &b = g_btn[id];
        SolidBrush cb(Mix(TH(circle), TH(circleHot), b.hover));
        g.FillEllipse(&cb, b.r);
        Pen pen(Mix(kInk, kPink, b.hover), 1.8f);
        float cx = b.r.X + b.r.Width / 2, cy = b.r.Y + b.r.Height / 2;
        if (id == B_CLOSE) { g.DrawLine(&pen, cx - 5, cy - 5, cx + 5, cy + 5); g.DrawLine(&pen, cx + 5, cy - 5, cx - 5, cy + 5); }
        else g.DrawLine(&pen, cx - 5, cy, cx + 5, cy);
    }

    if (g_state == ST_LAUNCH || g_state == ST_CLOSING) {
        // ecran d'attente
        int dots = (int)(g_time * 2.5f) % 4;
        std::wstring title = T(L"Vice City se lance", L"Vice City is starting");
        title += std::wstring(dots, L'.') + std::wstring(3 - dots, L' ');
        Text(g, title, RectF(60, 300, 336, 40), 24, FontStyleBold, kInk);
        Text(g, g_launchInfo, RectF(60, 340, 336, 26), 14, FontStyleRegular, kGrey);
        DrawBar(g, RectF(96, 390, 264, 6), -2);
        Text(g, T(L"La fen\u00EAtre du jeu va appara\u00EEtre.", L"The game window will appear shortly."), RectF(60, 410, 336, 24), 12.5f, FontStyleRegular, kGrey);
    } else {
        DrawTabs(g);
        DrawOptions(g);
        Text(g, status, RectF(60, 212, 336, 22), 13, FontStyleBold, sc);
        if (prog != -1.0f) DrawBar(g, RectF(96, 238, 264, 5), prog);
        DrawField(g, 0, T(L"PSEUDO", L"NICKNAME"));
        DrawField(g, 1, T(L"ADRESSE DE L'H\u00D4TE", L"HOST ADDRESS"));
        extern bool LobbyMeReady();
        extern bool g_joinFallback;
        const wchar_t *hostLabel = T(L"H\u00C9BERGER", L"HOST"), *joinLabel = g_joinFallback ? T(L"REJOINDRE EN JEU", L"JOIN IN GAME") : T(L"REJOINDRE", L"JOIN");
        if (g_lobby == LB_HOST) { hostLabel = T(L"LANCER", L"START"); joinLabel = T(L"FERMER LE SALON", L"CLOSE LOBBY"); }
        else if (g_lobby == LB_GUEST) { hostLabel = LobbyMeReady() ? T(L"PR\u00CAT \u2713", L"READY \u2713") : T(L"PR\u00CAT ?", L"READY?"); joinLabel = T(L"QUITTER", L"LEAVE"); }
        else if (g_lobby == LB_CONNECTING) { hostLabel = T(L"CONNEXION\u2026", L"CONNECTING\u2026"); joinLabel = T(L"ANNULER", L"CANCEL"); }
        DrawButton(g, B_HOST, hostLabel, true);
        DrawButton(g, B_JOIN, joinLabel, false);
        // exe cible
        std::wstring exeLine;
        Color ec = kGrey;
        if (g_exeKind == EXE_OK) { exeLine = L"gta-vc.exe 1.0 \u2713"; ec = Color(255, 38, 150, 96); }
        else if (g_exeKind == EXE_MISSING) { exeLine = T(L"gta-vc.exe introuvable", L"gta-vc.exe not found"); ec = Color(255, 214, 48, 72); }
        else { exeLine = T(L"exe pas en 1.0", L"exe is not 1.0"); ec = Color(255, 214, 48, 72); }
        Text(g, exeLine, RectF(78, 452, 170, 20), 12, FontStyleBold, ec, StringAlignmentNear);
        Button &eb = g_btn[B_EXE];
        const wchar_t *el = g_exeKind == EXE_OK ? T(L"Changer d'exe", L"Change exe") : T(L"Choisir l'exe\u2026", L"Choose exe\u2026");
        Color lc = Mix(g_exeKind == EXE_OK ? kGrey : kPink, kPink, eb.hover);
        Text(g, el, eb.r, 12, g_exeKind == EXE_OK ? FontStyleUnderline : FontStyleBold | FontStyleUnderline, WithA(lc, eb.enabled ? 1.0f : 0.4f), StringAlignmentFar);
    }
    const Color lg = WithA(kGrey, 0.8f);
    Text(g, T(L"Mod non officiel et non commercial.", L"Unofficial, non-commercial mod."), RectF(56, 510, 344, 14), 10, FontStyleRegular, lg);
    Text(g, T(L"Non affili\u00E9 \u00E0 Rockstar Games ni \u00E0 Take-Two.", L"Not affiliated with Rockstar Games or Take-Two."), RectF(56, 523, 344, 14), 10, FontStyleRegular, lg);
    Text(g, T(L"N\u00E9cessite une copie l\u00E9gale de GTA: Vice City.", L"Requires a legal copy of GTA: Vice City."), RectF(56, 536, 344, 14), 10, FontStyleRegular, lg);
    // Acheter le jeu : pastille sombre avec un panier, vers la boutique Rockstar
    {
        Button &b = g_btn[B_BUY];
        Text(g, T(L"Achetez GTA: Vice City :", L"Buy GTA: Vice City:"), RectF(76, b.r.Y, b.r.X - 76 - 8, b.r.Height), 12, FontStyleBold, kInk, StringAlignmentFar);
        GraphicsPath p;
        RoundRect(p, b.r, b.r.Height / 2);
        SolidBrush fill(Mix(TH(pill), kPink, b.hover));
        g.FillPath(&fill, &p);
        Pen cart(Color(255, 255, 255, 255), 1.6f);
        cart.SetLineJoin(LineJoinRound);
        cart.SetStartCap(LineCapRound);
        cart.SetEndCap(LineCapRound);
        float x = b.r.X + 13, y = b.r.Y + 7;
        PointF basket[] = { PointF(x - 2, y), PointF(x + 1, y), PointF(x + 3.5f, y + 9), PointF(x + 12, y + 9), PointF(x + 14, y + 3), PointF(x + 2.2f, y + 3) };
        g.DrawLines(&cart, basket, 6);
        SolidBrush white(Color(255, 255, 255, 255));
        g.FillEllipse(&white, x + 3.2f, y + 10.4f, 3.2f, 3.2f);
        g.FillEllipse(&white, x + 9.8f, y + 10.4f, 3.2f, 3.2f);
        Text(g, L"Rockstar Store", RectF(b.r.X + 30, b.r.Y, b.r.Width - 36, b.r.Height), 12, FontStyleBold, Color(255, 255, 255, 255), StringAlignmentNear);
    }
}

static void RenderTo(Bitmap &target, float scale)
{
    Graphics g(&target);
    g.Clear(Color(0, 0, 0, 0));
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.ScaleTransform(scale, scale);
    Bitmap *bgi = (g_dark && g_bgDark) ? g_bgDark : g_bg;
    if (bgi) g.DrawImage(bgi, RectF(0, 0, kImgW, kImgH));
    else {   // pas d'image : carte simple
        GraphicsPath p;
        RoundRect(p, RectF(20, 60, 960, 540), 26);
        LinearGradientBrush lg(RectF(20, 60, 960, 540), TH(fallA), TH(fallB), LinearGradientModeVertical);
        g.FillPath(&lg, &p);
    }
    DrawUI(g);
}

static void Present()
{
    if (!g_wnd || !g_memDC) return;
    {
        Bitmap frame(g_winW, g_winH, g_winW * 4, PixelFormat32bppPARGB, (BYTE *)g_bits);
        RenderTo(frame, g_scale);
    }
    GdiFlush();
    RECT wr;
    GetWindowRect(g_wnd, &wr);
    POINT dst = { wr.left, wr.top }, src = { 0, 0 };
    SIZE sz = { g_winW, g_winH };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(255 * min(max(g_alpha, 0.0f), 1.0f)), AC_SRC_ALPHA };
    HDC screen = GetDC(NULL);
    BOOL ok = UpdateLayeredWindow(g_wnd, screen, &dst, &sz, g_memDC, &src, 0, &bf, ULW_ALPHA);
    if (g_ulwOk != 0) g_ulwOk = ok ? 1 : 0;
    g_frames++;
    ReleaseDC(NULL, screen);
}

// ---------------------------------------------------------------- actions
static void ChooseExe()
{
    wchar_t file[MAX_PATH] = L"gta-vc.exe";
    OPENFILENAMEW of = { sizeof(of) };
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"gta-vc.exe\0gta-vc*.exe\0*.exe\0*.exe\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    std::wstring init = !g_exe.empty() ? DirOf(g_exe) : g_dir;
    of.lpstrInitialDir = init.c_str();
    of.lpstrTitle = T(L"Choisir gta-vc.exe (version 1.0)", L"Choose gta-vc.exe (version 1.0)");
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&of)) return;
    ExeKind k = CheckExe(file);
    if (k != EXE_OK) { BadExeMessage(k); return; }
    SetExe(file);
    SkinsInit();
    WritePrivateProfileStringW(L"Lanceur", L"Exe", file, g_iniLauncher.c_str());
    StartUpdate();
}

static BOOL CALLBACK FindGameWindow(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    RECT r;
    if (pid == g_pid && IsWindowVisible(h) && GetWindowRect(h, &r) && r.right - r.left >= 320) { *(bool *)lp = true; return FALSE; }
    return TRUE;
}

static void Launch(int mode, const std::wstring &extra = L"")
{
    if (g_exeKind != EXE_OK || g_busy) return;
    std::wstring addr = Trim(g_fields[1].text);
    if (mode == 2 && addr.empty()) {
        SetStatus(K_ERR, T(L"Entre l'adresse de l'h\u00F4te", L"Enter the host address"));
        g_focus = 1;
        return;
    }
    SavePlayer();
    std::wstring args;
    if (mode == 1) args = L"-vccoop hote";
    else if (mode == 2) args = L"-vccoop invite " + addr;
    if (!extra.empty()) args += L" " + extra;
    // Lance comme un double-clic dans l'explorateur : les modes de compatibilite de l'exe (Windows XP, "executer en
    // tant qu'administrateur") exigent parfois l'administrateur ; CreateProcess echoue alors (erreur 740), ShellExecuteEx
    // affiche la demande de Windows.
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = g_wnd;
    sei.lpVerb = L"open";
    sei.lpFile = g_exe.c_str();
    sei.lpParameters = args.empty() ? NULL : args.c_str();
    sei.lpDirectory = g_gameDir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        DWORD e = GetLastError();
        if (e == ERROR_CANCELLED) SetStatus(K_WARN, T(L"Lancement annul\u00E9 (demande d'administrateur refus\u00E9e)", L"Launch cancelled (administrator prompt declined)"));
        else SetStatus(K_ERR, T(L"Impossible de lancer gta-vc.exe (erreur %lu)", L"Could not start gta-vc.exe (error %lu)"), e);
        return;
    }
    g_proc = sei.hProcess;
    g_pid = GetProcessId(sei.hProcess);
    g_launchT = GetTickCount();
    g_winSeenT = 0;
    g_launchMode = mode;
    std::wstring name = Trim(g_fields[0].text);
    wchar_t info[160];
    if (mode == 1) swprintf_s(info, T(L"%s h\u00E9berge la partie", L"%s is hosting"), name.c_str());
    else if (mode == 2) swprintf_s(info, T(L"%s rejoint %s", L"%s joins %s"), name.c_str(), addr.c_str());
    else swprintf_s(info, T(L"Menu COOP du jeu", L"Game COOP menu"));
    g_launchInfo = info;
    g_focus = -1;
    g_state = ST_LAUNCH;
}


// ---------------------------------------------------------------- salon
// Le lanceur de l'hote ouvre un salon en TCP sur le port du jeu (celui de vccoop-joueur.ini / vccoop.ini). Chaque
// connexion commence par 4 octets :
//  - "VCL1" : salon. Messages [u16 longueur][u8 type][...] : HELLO (version, pseudo, tenue) -> WELCOME (numero) ou
//    REJECT (raison) ; STATE (joueurs : pret, % des mods, ping ; choix de partie) ; READY ; PROGRESS ; PING / PONG ;
//    GO (l'hote lance : chaque lanceur demarre son jeu, l'hote avec -vccoop-partie, les invites avec -vccoop invite).
//  - "VCM1" : mods partages, meme protocole que le mod en jeu (mods.cpp) : manifeste (chemin, taille, empreinte FNV-1a)
//    puis "GET " par fichier. L'invite telecharge dans son VCCoop\mods avant le lancement ; son % part a l'hote, qui le
//    montre a tous.
// Le lanceur ferme le port juste avant de lancer le jeu (qui ouvre les memes).
enum { LB_PROTO = 1, M_HELLO = 1, M_WELCOME, M_REJECT, M_STATE, M_READY, M_PROGRESS, M_GO, M_PING, M_PONG, M_SKIN };
struct SaveInfo { int slot; std::string label; };
static std::vector<SaveInfo> g_saves;
static SOCKET g_listen = INVALID_SOCKET, g_guestSock = INVALID_SOCKET;
struct Conn { SOCKET s; int id; DWORD pingAt; };
static std::vector<Conn> g_conns;                   // hote : invites du salon (sous g_lcs)
static std::atomic<bool> g_meReady(false), g_goSent(false);
bool g_joinFallback;                                 // invite : pas de salon chez l'hote -> "Rejoindre en jeu"
static std::wstring g_lobbyAddr;
static int g_lobbyPort = 7790;
static std::wstring g_testSalon;                     // /testsalon hote|invite <journal>
static std::wstring g_testSalonLog;

static void TestLog(const char *fmt, ...)
{
    if (g_testSalonLog.empty()) return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(b, _countof(b), _TRUNCATE, fmt, ap);
    va_end(ap);
    FILE *f = _wfopen(g_testSalonLog.c_str(), L"a");
    if (f) { fprintf(f, "[%lu] %s\n", GetTickCount(), b); fclose(f); }
}

struct Wr {
    std::string d;
    void u8(int v) { d += (char)(uint8_t)v; }
    void u16(int v) { uint16_t x = (uint16_t)v; d.append((const char *)&x, 2); }
    void u32(uint32_t v) { d.append((const char *)&v, 4); }
    void str(const std::string &s) { size_t n = min<size_t>(s.size(), 255); u8((int)n); d.append(s.data(), n); }
};
struct Rd {
    const std::string &d; size_t p = 0; bool ok = true;
    Rd(const std::string &x) : d(x) {}
    int u8() { if (p + 1 > d.size()) { ok = false; return 0; } return (uint8_t)d[p++]; }
    int u16() { if (p + 2 > d.size()) { ok = false; return 0; } uint16_t x; memcpy(&x, d.data() + p, 2); p += 2; return x; }
    uint32_t u32() { if (p + 4 > d.size()) { ok = false; return 0; } uint32_t x; memcpy(&x, d.data() + p, 4); p += 4; return x; }
    std::string str() { int n = u8(); if (!ok || p + n > d.size()) { ok = false; return ""; } std::string s = d.substr(p, n); p += n; return s; }
};

static bool SendAllS(SOCKET s, const void *d, int n)
{
    const char *p = (const char *)d;
    while (n > 0) { int r = send(s, p, n, 0); if (r <= 0) return false; p += r; n -= r; }
    return true;
}
static bool RecvAllS(SOCKET s, void *d, int n)
{
    char *p = (char *)d;
    while (n > 0) { int r = recv(s, p, n, 0); if (r <= 0) return false; p += r; n -= r; }
    return true;
}
static bool SendMsg(SOCKET s, const Wr &w)
{
    uint16_t n = (uint16_t)w.d.size();
    return SendAllS(s, &n, 2) && SendAllS(s, w.d.data(), n);
}
static bool RecvMsg(SOCKET s, std::string &out)
{
    uint16_t n;
    if (!RecvAllS(s, &n, 2)) return false;
    out.resize(n);
    return n == 0 || RecvAllS(s, &out[0], n);
}

static int LobbyPort()
{
    std::string pj = Narrow(PlayerIni()), main = Narrow(g_gameDir + L"vccoop.ini");
    int port = GetPrivateProfileIntA("VCCoop", "Port", 7790, main.c_str());
    return GetPrivateProfileIntA("VCCoop", "Port", port, pj.c_str());
}
static std::string MySkin()
{
    char v[64];
    std::string pj = Narrow(PlayerIni()), main = Narrow(g_gameDir + L"vccoop.ini");
    GetPrivateProfileStringA("VCCoop", "Tenue", "", v, sizeof(v), main.c_str());
    GetPrivateProfileStringA("VCCoop", "Tenue", v, v, sizeof(v), pj.c_str());
    return v;
}
static std::string MyName() { std::wstring n = Trim(g_fields[0].text); return Narrow(n.empty() ? L"Tommy" : n); }
static bool SharedModsOn() { return GetPrivateProfileIntA("VCCoop", "ModsPartages", 1, Narrow(g_gameDir + L"vccoop.ini").c_str()) != 0; }

// --- sauvegardes (hote) : <jeu>\GTA Vice City User Files (SauvegardesLocales=1) ou Mes documents\GTA Vice City User Files
static void ReadSaves()
{
    g_saves.clear();
    std::wstring dir;
    if (GetPrivateProfileIntA("VCCoop", "SauvegardesLocales", 1, Narrow(g_gameDir + L"vccoop.ini").c_str())) dir = g_gameDir + L"GTA Vice City User Files\\";
    else {
        wchar_t docs[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs))) dir = std::wstring(docs) + L"\\GTA Vice City User Files\\";
    }
    for (int slot = 1; slot <= 8; slot++) {
        std::vector<unsigned char> d;
        wchar_t name[32];
        swprintf_s(name, L"GTAVCsf%d.b", slot);
        if (!ReadAll(dir + name, d) || d.size() < 0x44) continue;
        std::wstring title((const wchar_t *)(d.data() + 4), 24);   // nom de la sauvegarde (24 caracteres larges)
        title = title.c_str();
        const SYSTEMTIME *st = (const SYSTEMTIME *)(d.data() + 0x34);
        wchar_t lab[128];
        swprintf_s(lab, L"%d \u00B7 %s \u00B7 %02d/%02d %02d:%02d", slot, Trim(title).c_str(), st->wDay, st->wMonth, st->wHour, st->wMinute);
        g_saves.push_back({ slot, Narrow(lab, CP_UTF8) });
    }
}
static std::string ChosenSave()   // sauvegarde choisie ("" = nouvelle partie)
{
    if (g_lobby == LB_GUEST) return g_lobbyChoiceLabel;
    if (g_lobbyChoice <= 0 || g_lobbyChoice > (int)g_saves.size()) return "";
    return g_saves[g_lobbyChoice - 1].label;
}
static std::string ChoiceLabel()   // dans la langue de chacun
{
    std::string save = ChosenSave();
    if (save.empty()) return Narrow(T(L"Nouvelle partie", L"New game"), CP_UTF8);
    return Narrow(T(L"Charger ", L"Load "), CP_UTF8) + save;
}

// --- mods (meme empreinte que mods.cpp)
struct ModFile { std::string rel; uint32_t size, hash; };
static std::string ModsDirA() { return Narrow(g_gameDir) + "VCCoop\\mods\\"; }
static bool HashFileA(const std::string &path, uint32_t &size, uint32_t &hash)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    static uint8_t buf[65536];
    uint32_t h = 2166136261u, n = 0;
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) { for (size_t i = 0; i < r; i++) { h ^= buf[i]; h *= 16777619u; } n += (uint32_t)r; }
    fclose(f);
    size = n; hash = h;
    return true;
}
static void ScanMods(const std::string &dir, const std::string &rel, std::vector<ModFile> &out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        std::string r = rel + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ScanMods(dir + fd.cFileName + "\\", r + "\\", out); continue; }
        size_t n = strlen(fd.cFileName);
        if (n > 5 && !_stricmp(fd.cFileName + n - 5, ".part")) continue;
        ModFile m;
        m.rel = r;
        if (r.size() < 200 && HashFileA(dir + fd.cFileName, m.size, m.hash) && m.size < 1536u * 1024 * 1024) out.push_back(m);   // (archives de pack : 400 Mo et plus)
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static bool SafeRelA(const std::string &r)
{
    if (r.empty() || r.size() > 200 || r[0] == '\\' || r[0] == '/' || r.find(':') != std::string::npos || r.find("..") != std::string::npos) return false;
    for (char c : r) if ((unsigned char)c < 32) return false;
    return true;
}
static std::vector<ModFile> g_hostMods;   // hote : manifeste servi
static void ServeMods(SOCKET s)
{
    std::vector<ModFile> list;
    EnterCriticalSection(&g_lcs);
    list = g_hostMods;
    LeaveCriticalSection(&g_lcs);
    uint32_t count = (uint32_t)list.size();
    SendAllS(s, &count, 4);
    for (auto &m : list) {
        uint16_t len = (uint16_t)m.rel.size();
        SendAllS(s, &len, 2); SendAllS(s, m.rel.data(), len); SendAllS(s, &m.size, 4); SendAllS(s, &m.hash, 4);
    }
    std::vector<uint8_t> buf(65536);
    for (;;) {
        char cmd[4];
        if (!RecvAllS(s, cmd, 4) || memcmp(cmd, "GET ", 4)) break;
        uint16_t len;
        if (!RecvAllS(s, &len, 2) || len > 200) break;
        std::string rel(len, 0);
        if (!RecvAllS(s, &rel[0], len) || !SafeRelA(rel)) break;
        bool listed = false;
        for (auto &m : list) if (m.rel == rel) listed = true;   // seulement les fichiers du manifeste
        FILE *f = listed ? fopen((ModsDirA() + rel).c_str(), "rb") : NULL;
        uint32_t size = 0;
        if (f) { fseek(f, 0, SEEK_END); size = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET); }
        if (!SendAllS(s, &size, 4)) { if (f) fclose(f); break; }
        bool ok = true;
        if (f) {
            size_t r;
            while (ok && (r = fread(buf.data(), 1, buf.size(), f)) > 0) ok = SendAllS(s, buf.data(), (int)r);
            fclose(f);
        }
        if (!ok) break;
    }
}

// --- hote
static void BroadcastState()
{
    Wr w;
    w.u8(M_STATE);
    EnterCriticalSection(&g_lcs);
    w.u8((int)g_peers.size());
    for (auto &p : g_peers) { w.u8(p.id); w.str(p.name); w.str(p.skin); w.u8(p.ready); w.u8(p.mods + 2); w.u16(min(p.ping, 9999)); }
    w.str(ChosenSave());
    for (auto &c : g_conns) SendMsg(c.s, w);
    LeaveCriticalSection(&g_lcs);
}
static LobbyPeer *PeerById(int id) { for (auto &p : g_peers) if (p.id == id) return &p; return NULL; }
static void DropConn(SOCKET s)
{
    EnterCriticalSection(&g_lcs);
    bool listed = false;
    for (size_t i = 0; i < g_conns.size(); i++)
        if (g_conns[i].s == s) {
            listed = true;
            int id = g_conns[i].id;
            g_conns.erase(g_conns.begin() + i);
            for (size_t k = 0; k < g_peers.size(); k++) if (g_peers[k].id == id) { TestLog("salon : %s part", g_peers[k].name.c_str()); g_peers.erase(g_peers.begin() + k); break; }
            break;
        }
    LeaveCriticalSection(&g_lcs);
    (void)listed;
    closesocket(s);
}
static void LobbySession(SOCKET s)
{
    std::string m;
    if (!RecvMsg(s, m)) { closesocket(s); return; }
    Rd r(m);
    int type = r.u8(), proto = r.u8();
    std::string ver = r.str(), name = r.str(), skin = r.str();
    auto reject = [&](const std::string &why) { Wr w; w.u8(M_REJECT); w.str(why); SendMsg(s, w); closesocket(s); };
    if (!r.ok || type != M_HELLO || proto != LB_PROTO) { reject("proto"); return; }
    std::string mine = Narrow(g_localVer, CP_UTF8);
    if (ver != mine) { reject("version " + mine); return; }
    if (g_goSent || g_lobby != LB_HOST) { reject("started"); return; }
    int id = -1;
    EnterCriticalSection(&g_lcs);
    for (int k = 1; k < 4 && id < 0; k++) if (!PeerById(k)) id = k;
    if (id > 0) {
        g_peers.push_back({ id, name, skin, false, SharedModsOn() ? 0 : -1, 0 });
        g_conns.push_back({ s, id, 0 });
    }
    LeaveCriticalSection(&g_lcs);
    if (id < 0) { reject("full"); return; }
    { Wr w; w.u8(M_WELCOME); w.u8(id); SendMsg(s, w); }
    TestLog("salon : %s arrive (joueur %d)", name.c_str(), id);
    BroadcastState();
    DWORD to = 60000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
    while (RecvMsg(s, m)) {
        Rd q(m);
        int t = q.u8();
        EnterCriticalSection(&g_lcs);
        LobbyPeer *p = PeerById(id);
        if (p && t == M_READY) { p->ready = q.u8() != 0; TestLog("salon : joueur %d pret=%d", id, (int)p->ready); }
        else if (p && t == M_PROGRESS) { int v = q.u8(); p->mods = v == 255 ? -2 : v; }
        else if (p && t == M_PONG) { uint32_t sent = q.u32(); p->ping = (int)(GetTickCount() - sent); }
        else if (p && t == M_SKIN) p->skin = q.str();
        LeaveCriticalSection(&g_lcs);
        if (t == M_READY || t == M_PROGRESS || t == M_SKIN) BroadcastState();
    }
    DropConn(s);   // (ferme la socket)
    BroadcastState();
}
static DWORD WINAPI ConnThread(void *param)
{
    SOCKET s = (SOCKET)param;
    DWORD to = 30000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof(to));
    char magic[4];
    if (RecvAllS(s, magic, 4)) {
        if (!memcmp(magic, "VCM1", 4)) { ServeMods(s); closesocket(s); return 0; }
        if (!memcmp(magic, "VCL1", 4)) {
            DWORD sto = 3000;   // salon : un invite bloque ne fige pas la fenetre (envois sous g_lcs)
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&sto, sizeof(sto));
            LobbySession(s);   // (ferme la socket)
            return 0;
        }
    }
    closesocket(s);
    return 0;
}
static DWORD WINAPI AcceptThread(void *)
{
    for (;;) {
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        HANDLE t = CreateThread(NULL, 0, ConnThread, (void *)c, 0, NULL);
        if (t) CloseHandle(t); else closesocket(c);
    }
    return 0;
}
static std::wstring LocalAddresses()
{
    char host[256];
    std::wstring out;
    if (gethostname(host, sizeof(host)) != 0) return out;
    addrinfo hints = {}, *res = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return out;
    int n = 0;
    for (addrinfo *a = res; a && n < 3; a = a->ai_next) {
        char ip[64];
        inet_ntop(AF_INET, &((sockaddr_in *)a->ai_addr)->sin_addr, ip, sizeof(ip));
        if (!strncmp(ip, "127.", 4)) continue;
        if (!out.empty()) out += L" \u00B7 ";
        out += Widen(ip);
        n++;
    }
    freeaddrinfo(res);
    return out;
}
static std::wstring g_myAddresses;

static void LobbyHost()
{
    SavePlayer();
    g_lobbyPort = LobbyPort();
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons((u_short)g_lobbyPort);
    if (bind(g_listen, (sockaddr *)&a, sizeof(a)) != 0 || listen(g_listen, 8) != 0) {
        int e = WSAGetLastError();
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        SetStatus(K_ERR, T(L"Port %d occup\u00E9 (le jeu tourne d\u00E9j\u00E0 ? erreur %d)", L"Port %d in use (is the game already running? error %d)"), g_lobbyPort, e);
        return;
    }
    std::vector<ModFile> mods;
    if (SharedModsOn()) ScanMods(ModsDirA(), "", mods);
    ReadSaves();
    EnterCriticalSection(&g_lcs);
    g_hostMods = mods;
    g_peers.clear();
    g_conns.clear();
    g_peers.push_back({ 0, MyName(), MySkin(), true, -1, 0 });
    g_lobbyChoice = 0;
    LeaveCriticalSection(&g_lcs);
    g_goSent = false;
    g_myId = 0;
    g_myAddresses = LocalAddresses();
    g_lobby = LB_HOST;
    g_tab = TAB_LOBBY;
    LayoutTabs();
    HANDLE t = CreateThread(NULL, 0, AcceptThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    SetStatus(K_OK, T(L"Salon ouvert \u00B7 port %d", L"Lobby open \u00B7 port %d"), g_lobbyPort);
    TestLog("salon : ouvert sur le port %d, %d mods, %d sauvegardes", g_lobbyPort, (int)mods.size(), (int)g_saves.size());
}

bool LobbyCanStart()
{
    if (g_lobby != LB_HOST || g_goSent) return false;
    bool ok = true;
    EnterCriticalSection(&g_lcs);
    for (auto &p : g_peers) if (p.id != 0 && (!p.ready || (p.mods != -1 && p.mods != 100))) ok = false;
    LeaveCriticalSection(&g_lcs);
    return ok;
}
bool LobbyMeReady() { return g_meReady; }

static void LobbyClose()
{
    int was = g_lobby.exchange(LB_NONE);
    if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
    EnterCriticalSection(&g_lcs);
    for (auto &c : g_conns) shutdown(c.s, SD_BOTH);   // les fils des sessions ferment leurs sockets
    g_conns.clear();
    g_peers.clear();
    if (g_guestSock != INVALID_SOCKET) shutdown(g_guestSock, SD_BOTH);   // GuestThread la ferme
    LeaveCriticalSection(&g_lcs);
    g_meReady = false;
    g_myMods = -1;
    if (g_tab == TAB_LOBBY) g_tab = -1;
    LayoutTabs();
    (void)was;
}

static void HostStart()
{
    if (!LobbyCanStart()) return;
    int slot = (g_lobbyChoice > 0 && g_lobbyChoice <= (int)g_saves.size()) ? g_saves[g_lobbyChoice - 1].slot : 0;
    g_goSent = true;
    Wr w;
    w.u8(M_GO);
    w.u8(slot);
    EnterCriticalSection(&g_lcs);
    for (auto &c : g_conns) SendMsg(c.s, w);
    LeaveCriticalSection(&g_lcs);
    TestLog("salon : GO (emplacement %d)", slot);
    // Les invites ferment les premiers en recevant GO : leurs connexions quittent la liste. On attend (1,5 s au plus)
    // pour que ce soit eux qui gardent le TIME_WAIT, et que le serveur de mods du jeu de l'hote retrouve son port.
    for (int i = 0; i < 30; i++) {
        EnterCriticalSection(&g_lcs);
        bool empty = g_conns.empty();
        LeaveCriticalSection(&g_lcs);
        if (empty) break;
        Sleep(50);
    }
    LobbyClose();
    wchar_t extra[32];
    if (slot > 0) swprintf_s(extra, L"-vccoop-partie %d", slot); else wcscpy_s(extra, L"-vccoop-partie nouvelle");
    Launch(1, extra);
}

// --- invite
static void GuestSend(const Wr &w)
{
    EnterCriticalSection(&g_lcs);
    if (g_guestSock != INVALID_SOCKET) SendMsg(g_guestSock, w);
    LeaveCriticalSection(&g_lcs);
}
static void SendProgress(int pct)
{
    g_myMods = pct;
    Wr w; w.u8(M_PROGRESS); w.u8(pct < 0 ? 255 : pct);
    GuestSend(w);
}
static SOCKET ConnectTo(const std::wstring &addr, int port, int timeoutMs)
{
    addrinfo hints = {}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(Narrow(addr).c_str(), NULL, &hints, &res) != 0 || !res) return INVALID_SOCKET;
    sockaddr_in a = *(sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    a.sin_port = htons((u_short)port);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (sockaddr *)&a, sizeof(a));
    fd_set wr, ex;
    FD_ZERO(&wr); FD_SET(s, &wr);
    FD_ZERO(&ex); FD_SET(s, &ex);
    timeval tv = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    int err = 0, len = sizeof(err);
    if (select(0, NULL, &wr, &ex, &tv) <= 0 || !FD_ISSET(s, &wr) || getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0 || err) { closesocket(s); return INVALID_SOCKET; }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    DWORD to = 30000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof(to));
    return s;
}
static DWORD WINAPI ModsDownloadThread(void *)
{
    if (!SharedModsOn()) { SendProgress(-1); return 0; }
    SOCKET s = ConnectTo(g_lobbyAddr, g_lobbyPort, 5000);
    DWORD to = 30000;
    if (s != INVALID_SOCKET) setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
    uint32_t count = 0;
    std::vector<ModFile> list, want;
    bool ok = s != INVALID_SOCKET && SendAllS(s, "VCM1", 4) && RecvAllS(s, &count, 4) && count <= 4096;
    for (uint32_t i = 0; ok && i < count; i++) {
        uint16_t len;
        ModFile m;
        ok = RecvAllS(s, &len, 2) && len <= 200;
        if (!ok) break;
        m.rel.resize(len);
        ok = RecvAllS(s, &m.rel[0], len) && RecvAllS(s, &m.size, 4) && RecvAllS(s, &m.hash, 4);
        if (ok && SafeRelA(m.rel)) list.push_back(m);
    }
    if (!ok) { if (s != INVALID_SOCKET) closesocket(s); SendProgress(-2); TestLog("mods : echec du manifeste"); return 0; }
    std::vector<ModFile> local;
    ScanMods(ModsDirA(), "", local);
    uint64_t total = 0, done = 0;
    for (auto &m : list) {
        bool have = false;
        for (auto &l : local) if (l.rel == m.rel && l.hash == m.hash && l.size == m.size) have = true;
        if (!have) { want.push_back(m); total += m.size; }
    }
    TestLog("mods : manifeste %u fichiers, %d a telecharger (%llu octets)", count, (int)want.size(), total);
    SendProgress(want.empty() ? 100 : 0);
    std::vector<uint8_t> buf(65536);
    int lastPct = 0;
    DWORD lastSend = GetTickCount();
    for (auto &m : want) {
        uint16_t len = (uint16_t)m.rel.size();
        uint32_t size;
        if (!SendAllS(s, "GET ", 4) || !SendAllS(s, &len, 2) || !SendAllS(s, m.rel.data(), len) || !RecvAllS(s, &size, 4)) { ok = false; break; }
        std::string path = ModsDirA() + m.rel;
        for (size_t i = 1; i < path.size(); i++) if (path[i] == '\\') CreateDirectoryA(path.substr(0, i).c_str(), NULL);
        FILE *f = fopen((path + ".part").c_str(), "wb");
        uint32_t left = size;
        while (ok && left > 0) {
            int n = left > buf.size() ? (int)buf.size() : (int)left;
            ok = RecvAllS(s, buf.data(), n);
            if (ok && f) fwrite(buf.data(), 1, n, f);
            left -= n;
            done += n;
            int pct = total ? (int)(done * 100 / total) : 100;
            if (pct >= 100) pct = 99;
            if (pct != lastPct && GetTickCount() - lastSend > 150) { SendProgress(pct); lastPct = pct; lastSend = GetTickCount(); }
        }
        if (f) fclose(f);
        if (!ok || !f) { DeleteFileA((path + ".part").c_str()); ok = false; break; }
        DeleteFileA(path.c_str());
        MoveFileA((path + ".part").c_str(), path.c_str());
    }
    SendAllS(s, "END ", 4);
    closesocket(s);
    SendProgress(ok ? 100 : -2);
    TestLog("mods : %s (%llu/%llu octets)", ok ? "a jour" : "ECHEC", done, total);
    return 0;
}
static DWORD WINAPI GuestThread(void *)
{
    SOCKET s = ConnectTo(g_lobbyAddr, g_lobbyPort, 5000);
    if (s == INVALID_SOCKET) { TestLog("salon : pas de salon chez l'hote"); PostMessageW(g_wnd, WM_APP_LOBBYEND, 2, 0); return 0; }
    Wr hello;
    hello.u8(M_HELLO); hello.u8(LB_PROTO); hello.str(Narrow(g_localVer, CP_UTF8)); hello.str(MyName()); hello.str(MySkin());
    std::string m;
    if (!SendAllS(s, "VCL1", 4) || !SendMsg(s, hello) || !RecvMsg(s, m)) { closesocket(s); PostMessageW(g_wnd, WM_APP_LOBBYEND, 2, 0); return 0; }
    Rd r(m);
    int t = r.u8();
    if (t == M_REJECT) {
        static std::string why;
        why = r.str();
        closesocket(s);
        TestLog("salon : refuse (%s)", why.c_str());
        PostMessageW(g_wnd, WM_APP_LOBBYEND, 1, (LPARAM)why.c_str());
        return 0;
    }
    if (t != M_WELCOME) { closesocket(s); PostMessageW(g_wnd, WM_APP_LOBBYEND, 2, 0); return 0; }
    g_myId = r.u8();
    EnterCriticalSection(&g_lcs);
    g_guestSock = s;
    LeaveCriticalSection(&g_lcs);
    g_lobby = LB_GUEST;
    SetStatus(K_OK, T(L"Dans le salon de %s", L"In %s's lobby"), g_lobbyAddr.c_str());
    TestLog("salon : entre (joueur %d)", g_myId);
    HANDLE mt = CreateThread(NULL, 0, ModsDownloadThread, NULL, 0, NULL);
    if (mt) CloseHandle(mt);
    DWORD to = 60000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
    bool go = false;
    while (!go && RecvMsg(s, m)) {
        Rd q(m);
        int type = q.u8();
        if (type == M_STATE) {
            int n = q.u8();
            std::vector<LobbyPeer> peers;
            for (int i = 0; i < n; i++) {
                LobbyPeer p;
                p.id = q.u8(); p.name = q.str(); p.skin = q.str(); p.ready = q.u8() != 0; p.mods = q.u8() - 2; p.ping = q.u16();
                peers.push_back(p);
            }
            std::string label = q.str();
            if (q.ok) { EnterCriticalSection(&g_lcs); g_peers = peers; g_lobbyChoiceLabel = label; LeaveCriticalSection(&g_lcs); }
        } else if (type == M_PING) {
            Wr w; w.u8(M_PONG); w.u32(q.u32());
            GuestSend(w);
        } else if (type == M_GO) {
            go = true;
            TestLog("salon : GO recu");
            PostMessageW(g_wnd, WM_APP_GO, q.u8(), 0);
        }
    }
    EnterCriticalSection(&g_lcs);
    g_guestSock = INVALID_SOCKET;
    LeaveCriticalSection(&g_lcs);
    closesocket(s);
    if (!go) PostMessageW(g_wnd, WM_APP_LOBBYEND, 0, 0);
    return 0;
}
static void LobbyJoin()
{
    std::wstring addr = Trim(g_fields[1].text);
    if (addr.empty()) { SetStatus(K_ERR, T(L"Entre l'adresse de l'h\u00F4te", L"Enter the host address")); g_focus = 1; return; }
    size_t colon = addr.find(L':');   // adresse:port accepte
    g_lobbyPort = LobbyPort();
    g_lobbyAddr = addr;
    if (colon != std::wstring::npos) { g_lobbyAddr = addr.substr(0, colon); g_lobbyPort = _wtoi(addr.c_str() + colon + 1); }
    SavePlayer();
    g_meReady = false;
    g_myMods = -1;
    EnterCriticalSection(&g_lcs);
    g_peers.clear();
    g_lobbyChoiceLabel.clear();
    LeaveCriticalSection(&g_lcs);
    g_lobby = LB_CONNECTING;
    g_tab = TAB_LOBBY;
    LayoutTabs();
    SetStatus(K_NORMAL, T(L"Connexion au salon de %s\u2026", L"Connecting to %s's lobby\u2026"), g_lobbyAddr.c_str());
    HANDLE t = CreateThread(NULL, 0, GuestThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
static void GuestToggleReady()
{
    g_meReady = !g_meReady;
    Wr w; w.u8(M_READY); w.u8(g_meReady ? 1 : 0);
    GuestSend(w);
}

// Toutes les 2 s, l'hote mesure le ping de chacun.
static void LobbyTick()
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (g_lobby != LB_HOST || now - last < 2000) return;
    last = now;
    Wr w; w.u8(M_PING); w.u32(now);
    EnterCriticalSection(&g_lcs);
    for (auto &c : g_conns) SendMsg(c.s, w);
    LeaveCriticalSection(&g_lcs);
    BroadcastState();
}

// --- dessin du salon (panneau de droite)
static const Color kPlayerCol[4] = { Color(255, 70, 130, 230), Color(255, 255, 150, 50), Color(255, 60, 180, 90), Color(255, 160, 90, 210) };
static const RectF kChoiceR(456, 452, 480, 30);

static void DrawLobby(Graphics &g)
{
    GraphicsPath pp;
    RoundRect(pp, kOptPanel, 18);
    SolidBrush bg(TH(panel));
    g.FillPath(&bg, &pp);
    Pen border(TH(panelBorder), 1.5f);
    g.DrawPath(&border, &pp);

    std::vector<LobbyPeer> peers;
    std::string choice;
    EnterCriticalSection(&g_lcs);
    peers = g_peers;
    choice = ChoiceLabel();
    LeaveCriticalSection(&g_lcs);
    int lobby = g_lobby;

    wchar_t head[96];
    swprintf_s(head, T(L"%d / 4 joueurs", L"%d / 4 players"), (int)peers.size());
    Text(g, T(L"SALON", L"LOBBY"), RectF(460, 126, 200, 26), 17, FontStyleBold, kInk, StringAlignmentNear);
    Text(g, head, RectF(700, 126, 232, 26), 13, FontStyleBold, kGrey, StringAlignmentFar);
    std::wstring sub;
    if (lobby == LB_HOST) sub = std::wstring(T(L"Adresse \u00E0 donner : ", L"Address to share: ")) + (g_myAddresses.empty() ? L"?" : g_myAddresses) + L" \u00B7 port " + std::to_wstring(g_lobbyPort);
    else sub = std::wstring(T(L"H\u00F4te : ", L"Host: ")) + g_lobbyAddr + L":" + std::to_wstring(g_lobbyPort);
    Text(g, sub, RectF(460, 150, 476, 20), 11.5f, FontStyleRegular, kGrey, StringAlignmentNear);

    if (lobby == LB_CONNECTING) {
        Text(g, T(L"Connexion au salon\u2026", L"Connecting to the lobby\u2026"), RectF(460, 280, 476, 30), 16, FontStyleBold, kInk);
        DrawBar(g, RectF(560, 320, 276, 5), -2);
    }
    for (int i = 0; i < 4 && lobby != LB_CONNECTING; i++) {
        RectF r(456, 178 + i * 64.0f, 480, 56);
        GraphicsPath rp;
        RoundRect(rp, r, 12);
        if (i >= (int)peers.size()) {
            Pen dash(TH(dash), 1.4f);
            dash.SetDashStyle(DashStyleDash);
            g.DrawPath(&dash, &rp);
            Text(g, T(L"En attente d'un joueur\u2026", L"Waiting for a player\u2026"), r, 12.5f, FontStyleRegular, TH(dashText));
            continue;
        }
        const LobbyPeer &p = peers[i];
        bool me = p.id == g_myId;
        SolidBrush rb(me ? TH(cardSel) : TH(card));
        g.FillPath(&rb, &rp);
        Pen rpen(me ? Color(255, 255, 170, 200) : TH(cardBorder), 1.2f);
        g.DrawPath(&rpen, &rp);
        // portrait de sa tenue (sinon initiale), dans un cercle a la couleur du joueur en jeu
        std::wstring nm = Widen(p.name, CP_UTF8);
        extern void DrawAvatar(Graphics &g, RectF r, const std::string &skin, const std::wstring &name, Color col);
        DrawAvatar(g, RectF(r.X + 8, r.Y + 6, 44, 44), p.skin, nm, kPlayerCol[p.id & 3]);
        Text(g, nm, RectF(r.X + 60, r.Y + 7, 220, 22), 15, FontStyleBold, kInk, StringAlignmentNear);
        std::wstring line;
        if (p.id == 0) line = T(L"H\u00F4te", L"Host");
        else line = p.ready ? T(L"Pr\u00EAt \u2713", L"Ready \u2713") : T(L"Pas pr\u00EAt", L"Not ready");
        Text(g, line, RectF(r.X + 60, r.Y + 29, 90, 20), 12, FontStyleBold, p.id == 0 ? kPink : p.ready ? Color(255, 38, 150, 96) : kGrey, StringAlignmentNear);
        // mods
        if (p.id != 0) {
            std::wstring ml;
            if (p.mods == -1) ml = T(L"Mods : non", L"Mods: off");
            else if (p.mods == -2) ml = T(L"Mods : \u00E9chec", L"Mods: failed");
            else if (p.mods >= 100) ml = T(L"Mods \u00E0 jour", L"Mods up to date");
            else { wchar_t b[48]; swprintf_s(b, T(L"Mods %d %%", L"Mods %d%%"), p.mods); ml = b; }
            Text(g, ml, RectF(r.X + 160, r.Y + 29, 110, 20), 12, FontStyleRegular, p.mods == -2 ? Color(255, 214, 48, 72) : kGrey, StringAlignmentNear);
            if (p.mods >= 0 && p.mods < 100) DrawBar(g, RectF(r.X + 270, r.Y + 37, 90, 5), p.mods / 100.0f);
        }
        if (p.id != 0 || lobby == LB_GUEST) {
            wchar_t pb[32];
            swprintf_s(pb, L"%d ms", p.ping);
            if (p.id != 0) Text(g, pb, RectF(r.X + r.Width - 90, r.Y + 8, 78, 40), 12, FontStyleRegular, kGrey, StringAlignmentFar);
        }
    }
    // partie
    Text(g, T(L"PARTIE", L"GAME"), RectF(460, 432, 200, 18), 10.5f, FontStyleBold, kGrey, StringAlignmentNear);
    GraphicsPath cp;
    RoundRect(cp, kChoiceR, 15);
    SolidBrush cb(TH(card));
    g.FillPath(&cb, &cp);
    Pen cpen(TH(choiceBorder), 1.2f);
    g.DrawPath(&cpen, &cp);
    bool host = lobby == LB_HOST;
    if (host && (int)g_saves.size() > 0) {
        Text(g, L"\u2039", RectF(kChoiceR.X + 6, kChoiceR.Y - 2, 20, kChoiceR.Height), 20, FontStyleBold, kPink);
        Text(g, L"\u203A", RectF(kChoiceR.X + kChoiceR.Width - 26, kChoiceR.Y - 2, 20, kChoiceR.Height), 20, FontStyleBold, kPink);
    }
    Text(g, choice.empty() ? L"\u2026" : Widen(choice, CP_UTF8), RectF(kChoiceR.X + 28, kChoiceR.Y, kChoiceR.Width - 56, kChoiceR.Height), 13, FontStyleBold, kInk);
    // mods de l'hote / les miens
    std::wstring info;
    if (host) {
        EnterCriticalSection(&g_lcs);
        size_t n = g_hostMods.size();
        uint64_t bytes = 0;
        for (auto &m : g_hostMods) bytes += m.size;
        LeaveCriticalSection(&g_lcs);
        wchar_t b[128];
        if (!SharedModsOn()) wcscpy_s(b, T(L"Mods partag\u00E9s d\u00E9sactiv\u00E9s", L"Shared mods disabled"));
        else swprintf_s(b, T(L"Mods partag\u00E9s : %d fichiers (%.1f Mo)", L"Shared mods: %d files (%.1f MB)"), (int)n, bytes / 1048576.0);
        info = b;
    }
    Text(g, info, RectF(460, 490, 476, 20), 12, FontStyleRegular, kGrey, StringAlignmentNear);
    Pen sep(TH(sep), 1);
    g.DrawLine(&sep, kOptPanel.X + 18, 532.0f, kOptPanel.X + kOptPanel.Width - 18, 532.0f);
    const wchar_t *hint = host ? T(L"Quand tout le monde est pr\u00EAt et a les mods, \u00AB Lancer \u00BB d\u00E9marre le jeu de chacun, directement en partie.",
                                   L"Once everyone is ready and has the mods, \"Start\" launches everyone's game, straight into the session.")
                               : T(L"Clique sur \u00AB Pr\u00EAt \u00BB. Ton jeu d\u00E9marre tout seul quand l'h\u00F4te lance la partie.",
                                   L"Click \"Ready\". Your game starts by itself when the host starts the session.");
    FontFamily fam(L"Segoe UI");
    Font font(&fam, 12, FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush db(kGrey);
    g.DrawString(hint, -1, &font, RectF(kOptPanel.X + 20, 536, kOptPanel.Width - 40, 44), &sf, &db);
}

static bool LobbyClick(float x, float y)
{
    if (g_lobby != LB_HOST || !kChoiceR.Contains(x, y) || g_saves.empty()) return kOptPanel.Contains(x, y);
    int n = (int)g_saves.size() + 1;
    int dir = x < kChoiceR.X + kChoiceR.Width / 2 ? -1 : 1;
    EnterCriticalSection(&g_lcs);
    g_lobbyChoice = (g_lobbyChoice + dir + n) % n;
    LeaveCriticalSection(&g_lcs);
    BroadcastState();
    return true;
}

// ---------------------------------------------------------------- tenues : apercu 3D et portraits
// Les modeles viennent du jeu du joueur (model3d.cpp). Deux fils : l'apercu (le modele choisi, qui tourne, rendu a la
// taille de l'ecran) et les portraits (toutes les tenues, une fois, du plus demande au reste).
static std::vector<std::string> g_skinList;
static std::atomic<int> g_skinSel(0);
static CRITICAL_SECTION g_scs;
struct Portrait { int size; std::vector<uint32_t> px; };
static std::map<std::string, Portrait> g_portraits;
static std::vector<std::string> g_portraitWanted;    // demandes du salon (tenues d'autres joueurs)
static std::atomic<int> g_skinGen(0);
static std::atomic<float> g_prevYaw(0.0f);
static std::atomic<int> g_prevW(0), g_prevH(0);
static std::vector<uint32_t> g_prevFrame;
static int g_prevFrameW, g_prevFrameH;
static bool g_skinDrag;
static float g_skinDragX;
static const RectF kPrevR(452, 150, 250, 376), kGridR(712, 150, 228, 376);
static const float kTile = 68, kTileStepX = 80, kTileStepY = 78;
static const int kPortraitPx = 104;

static std::string LowerA(std::string v) { for (auto &c : v) c = (char)tolower((unsigned char)c); return v; }
static std::string SkinOrDefault(const std::string &skin) { return skin.empty() ? std::string("player") : LowerA(skin); }

static std::wstring SkinDisplayName(const std::string &n)
{
    static const struct { const char *k; const wchar_t *v; } known[] = {
        { "player", L"Tommy Vercetti" }, { "igken", L"Ken Rosenberg" }, { "igsonny", L"Sonny Forelli" }, { "igdiaz", L"Ricardo Diaz" },
        { "igmerc", L"Mercedes" }, { "igphil", L"Phil Cassidy" }, { "igcandy", L"Candy Suxxx" }, { "igcolon", L"Colonel Cortez" },
        { "igpercy", L"Percy" }, { "ighlary", L"Hilary King" }, { "igbuddy", L"Lance Vance" }, { "iggonz", L"Gonzalez" }, { "igjezz", L"Jezz Torrent" } };
    for (auto &k : known) if (n == k.k) return k.v;
    for (auto &k : known) {   // variantes numerotees (igmerc2, igphil3...)
        size_t l = strlen(k.k);
        if (n.size() > l && !n.compare(0, l, k.k) && isdigit((unsigned char)n[l])) return std::wstring(k.v) + L" " + Widen(n.substr(l));
    }
    if (!n.compare(0, 4, "play") && n.size() > 4 && isdigit((unsigned char)n[4])) return std::wstring(T(L"Tommy \u00B7 tenue ", L"Tommy \u00B7 outfit ")) + Widen(n.substr(4));
    std::wstring w = Widen(n);
    for (auto &c : w) c = towupper(c);
    return w;
}

struct ModEntry { std::wstring name, dff, txd; bool on; int files; uint64_t bytes; std::vector<uint32_t> thumb; bool thumbDone; bool pack = false; };
static std::vector<ModEntry> g_modList;
static std::atomic<int> g_modSel(0), g_modGen(0);
static const int kThumbPx = 64;

static DWORD WINAPI PreviewThread(void *)
{
    std::wstring loaded;
    Model3D *model = NULL;
    int gen = -1, style = 0;
    std::vector<uint32_t> buf;
    for (;;) {
        bool skins = g_tab == TAB_SKIN && g_imgOk, mods = g_tab == TAB_MODS;
        if ((!skins && !mods) || g_state != ST_IDLE) { Sleep(60); continue; }
        std::wstring want, txd;
        int wantGen;
        EnterCriticalSection(&g_scs);
        if (skins) {
            int sel = g_skinSel;
            if (sel >= 0 && sel < (int)g_skinList.size()) want = L"t:" + Widen(g_skinList[sel]);
            wantGen = g_skinGen;
        } else {
            int sel = g_modSel;
            if (sel >= 0 && sel < (int)g_modList.size() && !g_modList[sel].dff.empty()) { want = L"m:" + g_modList[sel].dff; txd = g_modList[sel].txd; }
            wantGen = 1000000 + g_modGen;
        }
        LeaveCriticalSection(&g_scs);
        if (want != loaded || gen != wantGen) {
            ModelFree(model);
            model = want.empty() ? NULL : want[0] == L't' ? ModelLoad(Narrow(want.substr(2))) : ModelLoadPath(want.substr(2), txd);
            loaded = want; gen = wantGen; style = skins ? 0 : 2;
        }
        int w = g_prevW, h = g_prevH;
        if (w <= 0 || h <= 0) { Sleep(30); continue; }
        buf.resize((size_t)w * h);
        DWORD t0 = GetTickCount();
        ModelRender(model, buf.data(), w, h, g_prevYaw, style);
        DWORD spent = GetTickCount() - t0;
        EnterCriticalSection(&g_scs);
        g_prevFrame.swap(buf);
        g_prevFrameW = w; g_prevFrameH = h;
        LeaveCriticalSection(&g_scs);
        Sleep(spent < 25 ? 33 - spent : 8);   // ~30 images/s au plus
    }
}

static DWORD WINAPI PortraitThread(void *)
{
    for (;;) {
        int gen = g_skinGen;
        std::string next;
        EnterCriticalSection(&g_scs);
        for (auto &n : g_portraitWanted) if (!g_portraits.count(n)) { next = n; break; }
        if (next.empty()) for (auto &n : g_skinList) if (!g_portraits.count(n)) { next = n; break; }
        LeaveCriticalSection(&g_scs);
        if (next.empty()) {   // portraits faits : miniatures des mods
            std::wstring dff, txd;
            int idx = -1, mg = g_modGen;
            EnterCriticalSection(&g_scs);
            for (int i = 0; i < (int)g_modList.size(); i++) if (!g_modList[i].thumbDone) { idx = i; dff = g_modList[i].dff; txd = g_modList[i].txd; break; }
            LeaveCriticalSection(&g_scs);
            if (idx >= 0) {
                std::vector<uint32_t> px((size_t)kThumbPx * kThumbPx, 0);
                if (!dff.empty()) { Model3D *m = ModelLoadPath(dff, txd); if (m) ModelRender(m, px.data(), kThumbPx, kThumbPx, 0.7f, 2); ModelFree(m); }
                EnterCriticalSection(&g_scs);
                if (mg == g_modGen && idx < (int)g_modList.size()) { g_modList[idx].thumb.swap(px); g_modList[idx].thumbDone = true; }
                LeaveCriticalSection(&g_scs);
                continue;
            }
        }
        if (next.empty() || !g_imgOk) { Sleep(200); continue; }
        Portrait pr;
        pr.size = kPortraitPx;
        pr.px.assign((size_t)kPortraitPx * kPortraitPx, 0);
        Model3D *m = ModelLoad(next);
        if (m) ModelRender(m, pr.px.data(), kPortraitPx, kPortraitPx, 0.3f, true);
        ModelFree(m);
        EnterCriticalSection(&g_scs);
        if (gen == g_skinGen) g_portraits[next] = std::move(pr);
        LeaveCriticalSection(&g_scs);
    }
}

// Catalogue du jeu choisi : liste des tenues, selection = Tenue de vccoop-joueur.ini.
static void SkinsInit()
{
    static bool threads;
    bool ok = !g_gameDir.empty() && ImgOpen(g_gameDir);
    std::vector<std::string> list = ok ? SkinList() : std::vector<std::string>();
    EnterCriticalSection(&g_scs);
    g_skinList = list;
    g_portraits.clear();
    g_portraitWanted.clear();
    int sel = 0;
    std::string mine = SkinOrDefault(g_gameDir.empty() ? "" : MySkin());
    for (int i = 0; i < (int)list.size(); i++) if (list[i] == mine) sel = i;
    g_skinSel = sel;
    g_skinGen++;
    LeaveCriticalSection(&g_scs);
    g_imgOk = ok && !list.empty();
    if (!g_gameDir.empty()) ModsScan();
    if (g_imgOk && !threads) {
        threads = true;
        HANDLE a = CreateThread(NULL, 0, PreviewThread, NULL, 0, NULL), b = CreateThread(NULL, 0, PortraitThread, NULL, 0, NULL);
        if (a) { SetThreadPriority(a, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(a); }
        if (b) { SetThreadPriority(b, THREAD_PRIORITY_LOWEST); CloseHandle(b); }
    }
    LayoutTabs();
}

static void SkinSelect(int i)
{
    std::string name;
    EnterCriticalSection(&g_scs);
    if (i < 0 || i >= (int)g_skinList.size()) { LeaveCriticalSection(&g_scs); return; }
    g_skinSel = i;
    name = g_skinList[i];
    LeaveCriticalSection(&g_scs);
    WritePrivateProfileStringA("VCCoop", "Tenue", name.c_str(), Narrow(PlayerIni()).c_str());   // celle du jeu (F7)
    if (g_lobby == LB_GUEST) { Wr w; w.u8(M_SKIN); w.str(name); GuestSend(w); }
    else if (g_lobby == LB_HOST) {
        EnterCriticalSection(&g_lcs);
        if (LobbyPeer *p = PeerById(0)) p->skin = name;
        LeaveCriticalSection(&g_lcs);
        BroadcastState();
    }
}

static float SkinMaxScroll()
{
    EnterCriticalSection(&g_scs);
    int n = (int)g_skinList.size();
    LeaveCriticalSection(&g_scs);
    int rows = (n + 2) / 3;
    return max(0.0f, rows * kTileStepY - 10 - kGridR.Height);
}

// Portrait d'une tenue dans un cercle (salon) ; initiale si le portrait n'est pas (encore) la.
void DrawAvatar(Graphics &g, RectF r, const std::string &skin, const std::wstring &name, Color col)
{
    std::string k = SkinOrDefault(skin);
    EnterCriticalSection(&g_scs);
    auto it = g_portraits.find(k);
    bool have = it != g_portraits.end();
    if (!have) { bool asked = false; for (auto &w : g_portraitWanted) asked |= w == k; if (!asked && g_portraitWanted.size() < 16) g_portraitWanted.push_back(k); }
    SolidBrush bg(have ? TH(thumbBg) : col);
    g.FillEllipse(&bg, r);
    if (have) {
        GraphicsPath clip;
        clip.AddEllipse(r);
        g.SetClip(&clip);
        Bitmap b(it->second.size, it->second.size, it->second.size * 4, PixelFormat32bppPARGB, (BYTE *)it->second.px.data());
        g.DrawImage(&b, RectF(r.X - r.Width * 0.08f, r.Y - r.Height * 0.02f, r.Width * 1.16f, r.Height * 1.16f));
        g.ResetClip();
    }
    LeaveCriticalSection(&g_scs);
    if (have) { Pen ring(col, 2.4f); g.DrawEllipse(&ring, r); }
    else Text(g, name.empty() ? L"?" : name.substr(0, 1), r, 18, FontStyleBold, Color(255, 255, 255, 255));
}

static int g_tileHot = -1, g_arrowHot = 0;

static void DrawSkin(Graphics &g)
{
    GraphicsPath pp;
    RoundRect(pp, kOptPanel, 18);
    SolidBrush bg(TH(panel));
    g.FillPath(&bg, &pp);
    Pen border(TH(panelBorder), 1.5f);
    g.DrawPath(&border, &pp);

    std::vector<std::string> list;
    EnterCriticalSection(&g_scs);
    list = g_skinList;
    LeaveCriticalSection(&g_scs);
    int sel = g_skinSel;
    std::string cur = sel >= 0 && sel < (int)list.size() ? list[sel] : "";
    Text(g, T(L"TENUE", L"OUTFIT"), RectF(460, 122, 200, 26), 17, FontStyleBold, kInk, StringAlignmentNear);
    wchar_t cnt[32];
    swprintf_s(cnt, L"%d / %d", sel + 1, (int)list.size());
    Text(g, cnt, RectF(700, 122, 236, 26), 13, FontStyleBold, kGrey, StringAlignmentFar);

    // apercu : fond doux, ombre au sol, modele rendu a la taille de l'ecran (fil PreviewThread)
    GraphicsPath vp;
    RoundRect(vp, kPrevR, 14);
    LinearGradientBrush vb(kPrevR, TH(prevA), TH(prevB), LinearGradientModeVertical);
    g.FillPath(&vb, &vp);
    SolidBrush shadow(Color(40, 120, 40, 80));
    g.FillEllipse(&shadow, kPrevR.X + kPrevR.Width / 2 - 52, kPrevR.Y + kPrevR.Height - 34, 104.0f, 16.0f);
    g_prevW = (int)(kPrevR.Width * g_scale);
    g_prevH = (int)((kPrevR.Height - 34) * g_scale);
    EnterCriticalSection(&g_scs);
    if (!g_prevFrame.empty() && g_prevFrameW > 0) {
        Bitmap b(g_prevFrameW, g_prevFrameH, g_prevFrameW * 4, PixelFormat32bppPARGB, (BYTE *)g_prevFrame.data());
        g.DrawImage(&b, RectF(kPrevR.X, kPrevR.Y + 8, kPrevR.Width, kPrevR.Height - 34));
    }
    LeaveCriticalSection(&g_scs);
    Text(g, SkinDisplayName(cur), RectF(kPrevR.X + 30, kPrevR.Y + kPrevR.Height - 30, kPrevR.Width - 60, 24), 14, FontStyleBold, kInk);
    for (int side = -1; side <= 1; side += 2) {   // tenue precedente / suivante
        RectF a(side < 0 ? kPrevR.X + 6 : kPrevR.X + kPrevR.Width - 34, kPrevR.Y + kPrevR.Height - 34, 28, 28);
        SolidBrush ab(g_arrowHot == side ? kPink : TH(card));
        g.FillEllipse(&ab, a);
        Text(g, side < 0 ? L"\u2039" : L"\u203A", RectF(a.X, a.Y - 2, a.Width, a.Height), 20, FontStyleBold, g_arrowHot == side ? Color(255, 255, 255, 255) : kPink);
    }

    // grille des portraits
    float sc = g_scroll[TAB_SKIN];
    g.SetClip(kGridR);
    for (int i = 0; i < (int)list.size(); i++) {
        RectF t(kGridR.X + (i % 3) * kTileStepX, kGridR.Y + (i / 3) * kTileStepY - sc, kTile, kTile);
        if (t.Y + t.Height < kGridR.Y || t.Y > kGridR.Y + kGridR.Height) continue;
        GraphicsPath tp;
        RoundRect(tp, t, 12);
        SolidBrush tb(i == sel ? TH(cardSel) : TH(card));
        g.FillPath(&tb, &tp);
        EnterCriticalSection(&g_scs);
        auto it = g_portraits.find(list[i]);
        if (it != g_portraits.end()) {
            Region old;
            g.GetClip(&old);
            g.SetClip(&tp, CombineModeIntersect);
            Bitmap b(it->second.size, it->second.size, it->second.size * 4, PixelFormat32bppPARGB, (BYTE *)it->second.px.data());
            g.DrawImage(&b, t);
            g.SetClip(&old);
        }
        LeaveCriticalSection(&g_scs);
        Pen tpen(i == sel ? kPink : i == g_tileHot ? Color(255, 255, 170, 200) : TH(cardBorder), i == sel ? 2.4f : 1.2f);
        g.DrawPath(&tpen, &tp);
    }
    g.ResetClip();
    float ms = SkinMaxScroll();
    if (ms > 0) {
        float h = kGridR.Height * kGridR.Height / (kGridR.Height + ms), y = kGridR.Y + (kGridR.Height - h) * sc / ms;
        GraphicsPath sp; RoundRect(sp, RectF(kGridR.X + kGridR.Width + 4, y, 4, h), 2);
        SolidBrush sb(Color(120, 255, 79, 139)); g.FillPath(&sb, &sp);
    }
    Pen sep(TH(sep), 1);
    g.DrawLine(&sep, kOptPanel.X + 18, 536.0f, kOptPanel.X + kOptPanel.Width - 18, 536.0f);
    FontFamily fam(L"Segoe UI");
    Font font(&fam, 12, FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush db(kGrey);
    g.DrawString(T(L"Ta tenue en jeu (la m\u00EAme que F7). Clique sur un portrait ; fais tourner le mod\u00E8le \u00E0 la souris.",
                   L"Your in-game outfit (same as F7). Click a portrait; drag the model to turn it."), -1, &font, RectF(kOptPanel.X + 20, 540, kOptPanel.Width - 40, 42), &sf, &db);
}

static int SkinTileAt(float x, float y)
{
    if (!kGridR.Contains(x, y)) return -1;
    float gx = x - kGridR.X, gy = y - kGridR.Y + g_scroll[TAB_SKIN];
    int col = (int)(gx / kTileStepX), row = (int)(gy / kTileStepY);
    if (col > 2 || gx - col * kTileStepX > kTile || gy - row * kTileStepY > kTile) return -1;
    int i = row * 3 + col;
    EnterCriticalSection(&g_scs);
    int n = (int)g_skinList.size();
    LeaveCriticalSection(&g_scs);
    return i < n ? i : -1;
}
static int SkinArrowAt(float x, float y)
{
    RectF l(kPrevR.X + 6, kPrevR.Y + kPrevR.Height - 34, 28, 28), r(kPrevR.X + kPrevR.Width - 34, kPrevR.Y + kPrevR.Height - 34, 28, 28);
    return l.Contains(x, y) ? -1 : r.Contains(x, y) ? 1 : 0;
}
// Clic dans l'onglet TENUE : vrai si traite ; *drag : commence a tourner le modele.
static bool SkinMouseDown(float x, float y, bool *drag)
{
    *drag = false;
    int a = SkinArrowAt(x, y);
    if (a) {
        EnterCriticalSection(&g_scs);
        int n = (int)g_skinList.size();
        LeaveCriticalSection(&g_scs);
        if (n) SkinSelect((g_skinSel + a + n) % n);
        return true;
    }
    int t = SkinTileAt(x, y);
    if (t >= 0) { SkinSelect(t); return true; }
    if (kPrevR.Contains(x, y)) { *drag = true; g_skinDrag = true; g_skinDragX = x; return true; }
    return kOptPanel.Contains(x, y);
}

// ---------------------------------------------------------------- mods partages (hote)
// Un mod = une entree du dossier VCCoop\mods (sous-dossier ou fichier). Desactive = deplace dans VCCoop\mods-off : le
// jeu (mods.cpp) et le salon ne lisent que VCCoop\mods. Apercu 3D : le premier .dff du mod, avec son .txd (ou celui
// du jeu du meme nom).
static std::wstring ModsDirW(bool on) { return g_gameDir + (on ? L"VCCoop\\mods\\" : L"VCCoop\\mods-off\\"); }
// Archive de pack (nom.img + nom.dir) : un vehicule connu en est extrait (dossier temporaire) pour l'apercu 3D.
static bool PackPreview(const std::wstring &img, const std::wstring &dirFile, ModEntry &e)
{
    FILE *fd = _wfopen(dirFile.c_str(), L"rb");
    if (!fd) return false;
    struct Ent { uint32_t off, size; char name[24]; };
    std::vector<Ent> ents;
    Ent x;
    while (fread(&x, 1, 32, fd) == 32) { x.name[23] = 0; ents.push_back(x); }
    fclose(fd);
    auto find = [&](const char *n) -> const Ent * { for (auto &en : ents) if (!_stricmp(en.name, n)) return &en; return nullptr; };
    static const char *pref[] = { "infernus", "cheetah", "banshee", "stinger", "sentinel", "admiral", "voodoo", "pcj600" };
    const Ent *dff = nullptr, *txd = nullptr;
    for (const char *p : pref) {
        char a[32], b[32];
        sprintf_s(a, "%s.dff", p); sprintf_s(b, "%s.txd", p);
        if ((dff = find(a)) && (txd = find(b))) break;
        dff = txd = nullptr;
    }
    for (size_t i = 0; !dff && i < ents.size(); i++) {
        size_t l = strlen(ents[i].name);
        if (l < 5 || _stricmp(ents[i].name + l - 4, ".dff")) continue;
        std::string t = std::string(ents[i].name, l - 4) + ".txd";
        if ((txd = find(t.c_str()))) dff = &ents[i];
    }
    if (!dff) return false;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring out = std::wstring(tmp) + L"VCCoop-apercu\\";
    CreateDirectoryW(out.c_str(), NULL);
    out += std::to_wstring(std::hash<std::wstring>()(img) & 0xFFFFFF) + L"_";
    FILE *fi = _wfopen(img.c_str(), L"rb");
    if (!fi) return false;
    bool ok = true;
    for (const Ent *en : { dff, txd }) {
        std::wstring path = out + Widen(en->name);
        std::vector<char> buf((size_t)en->size * 2048);
        if (_fseeki64(fi, (int64_t)en->off * 2048, SEEK_SET) != 0 || fread(buf.data(), 1, buf.size(), fi) != buf.size()) { ok = false; break; }
        FILE *fo = _wfopen(path.c_str(), L"wb");
        if (!fo) { ok = false; break; }
        fwrite(buf.data(), 1, buf.size(), fo);
        fclose(fo);
        (en == dff ? e.dff : e.txd) = path;
    }
    fclose(fi);
    return ok;
}

static void WalkMod(const std::wstring &dir, ModEntry &e)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> dffs, txds;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { WalkMod(dir + n + L"\\", e); continue; }
        e.files++;
        e.bytes += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        std::wstring low = n;
        for (auto &c : low) c = towlower(c);
        std::wstring ldir = dir;
        for (auto &c : ldir) c = towlower(c);
        bool generic = ldir.find(L"\\generic\\") != std::wstring::npos;   // roues, avion lointain : pas un apercu
        if (!generic && low.size() > 4 && !low.compare(low.size() - 4, 4, L".dff")) dffs.push_back(dir + n);
        if (!generic && low.size() > 4 && !low.compare(low.size() - 4, 4, L".txd")) txds.push_back(dir + n);
        if (low.size() > 4 && !low.compare(low.size() - 4, 4, L".img") && !e.pack) {
            std::wstring d = dir + n.substr(0, n.size() - 4) + L".dir";
            if (GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES) { e.pack = true; e.dff.clear(); e.txd.clear(); PackPreview(dir + n, d, e); }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (e.dff.empty() && !dffs.empty()) {
        e.dff = dffs[0];
        std::wstring base = e.dff.substr(0, e.dff.size() - 4);
        for (auto &t : txds) if (!_wcsicmp(t.substr(0, t.size() - 4).c_str(), base.c_str())) e.txd = t;
    }
}
static void ModsScan()
{
    std::vector<ModEntry> list;
    for (int on = 1; on >= 0; on--) {
        std::wstring dir = ModsDirW(on != 0);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || !_wcsicmp(n.c_str(), L"LISEZMOI.txt")) continue;
            ModEntry e;
            e.name = n; e.on = on != 0; e.files = 0; e.bytes = 0; e.thumbDone = false;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) WalkMod(dir + n + L"\\", e);
            else {
                e.files = 1;
                e.bytes = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                size_t l = n.size();
                if (l > 4 && !_wcsicmp(n.c_str() + l - 4, L".dff")) {
                    e.dff = dir + n;
                    if (GetFileAttributesW((dir + n.substr(0, l - 4) + L".txd").c_str()) != INVALID_FILE_ATTRIBUTES) e.txd = dir + n.substr(0, l - 4) + L".txd";
                }
            }
            list.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(list.begin(), list.end(), [](const ModEntry &a, const ModEntry &b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    EnterCriticalSection(&g_scs);
    std::wstring selName = g_modSel >= 0 && g_modSel < (int)g_modList.size() ? g_modList[g_modSel].name : L"";
    // (les miniatures deja faites sont gardees)
    for (auto &e : list) for (auto &o : g_modList) if (o.name == e.name && o.thumbDone && o.dff.substr(o.dff.find_last_of(L'\\') + 1) == e.dff.substr(e.dff.find_last_of(L'\\') + 1)) { e.thumb = o.thumb; e.thumbDone = true; }
    g_modList.swap(list);
    int sel = 0;
    for (int i = 0; i < (int)g_modList.size(); i++) if (g_modList[i].name == selName) sel = i;
    g_modSel = sel;
    g_modGen++;
    LeaveCriticalSection(&g_scs);
}
static void ModToggle(int i)
{
    if (g_lobby != LB_NONE) { SetStatus(K_WARN, T(L"Ferme le salon pour changer les mods", L"Close the lobby to change mods")); return; }
    ModEntry e;
    EnterCriticalSection(&g_scs);
    bool ok = i >= 0 && i < (int)g_modList.size();
    if (ok) e = g_modList[i];
    LeaveCriticalSection(&g_scs);
    if (!ok) return;
    CreateDirectoryW((g_gameDir + L"VCCoop").c_str(), NULL);
    CreateDirectoryW(ModsDirW(false).c_str(), NULL);
    CreateDirectoryW(ModsDirW(true).c_str(), NULL);
    std::wstring from = ModsDirW(e.on) + e.name, to = ModsDirW(!e.on) + e.name;
    if (!MoveFileW(from.c_str(), to.c_str())) {
        SetStatus(K_ERR, T(L"Impossible de d\u00E9placer %s (erreur %lu)", L"Could not move %s (error %lu)"), e.name.c_str(), GetLastError());
        return;
    }
    SetStatus(K_OK, e.on ? T(L"Mod d\u00E9sactiv\u00E9 : %s", L"Mod disabled: %s") : T(L"Mod activ\u00E9 : %s", L"Mod enabled: %s"), e.name.c_str());
    ModsScan();
}
static float ModsMaxScroll()
{
    EnterCriticalSection(&g_scs);
    int n = (int)g_modList.size();
    LeaveCriticalSection(&g_scs);
    return max(0.0f, n * 52.0f - kGridR.Height);
}
static const RectF kModsFolderR(826, 124, 110, 22);
static int g_modRowHot = -1;

static void DrawMods(Graphics &g)
{
    GraphicsPath pp;
    RoundRect(pp, kOptPanel, 18);
    SolidBrush bg(TH(panel));
    g.FillPath(&bg, &pp);
    Pen border(TH(panelBorder), 1.5f);
    g.DrawPath(&border, &pp);
    Text(g, L"MODS", RectF(460, 122, 120, 26), 17, FontStyleBold, kInk, StringAlignmentNear);
    EnterCriticalSection(&g_scs);
    int n = (int)g_modList.size(), on = 0;
    for (auto &e : g_modList) on += e.on;
    int sel = g_modSel;
    std::wstring selName = sel >= 0 && sel < n ? g_modList[sel].name : L"";
    bool selHas3d = sel >= 0 && sel < n && !g_modList[sel].dff.empty();
    LeaveCriticalSection(&g_scs);
    wchar_t cnt[64];
    swprintf_s(cnt, T(L"%d actif(s) / %d", L"%d enabled / %d"), on, n);
    Text(g, cnt, RectF(560, 122, 180, 26), 12.5f, FontStyleBold, kGrey, StringAlignmentNear);
    Text(g, T(L"Ouvrir le dossier", L"Open folder"), kModsFolderR, 12, FontStyleUnderline, g_btn[B_EXE].hover > 2 ? kPink : kPink, StringAlignmentFar);

    // apercu
    GraphicsPath vp;
    RoundRect(vp, kPrevR, 14);
    LinearGradientBrush vb(kPrevR, TH(prevA), TH(prevB), LinearGradientModeVertical);
    g.FillPath(&vb, &vp);
    g_prevW = (int)(kPrevR.Width * g_scale);
    g_prevH = (int)((kPrevR.Height - 34) * g_scale);
    if (selHas3d) {
        SolidBrush shadow(Color(34, 120, 40, 80));
        g.FillEllipse(&shadow, kPrevR.X + 34, kPrevR.Y + kPrevR.Height * 0.5f + 18, kPrevR.Width - 68, 30.0f);
        EnterCriticalSection(&g_scs);
        if (!g_prevFrame.empty() && g_prevFrameW > 0) {
            Bitmap b(g_prevFrameW, g_prevFrameH, g_prevFrameW * 4, PixelFormat32bppPARGB, (BYTE *)g_prevFrame.data());
            g.DrawImage(&b, RectF(kPrevR.X, kPrevR.Y + 8, kPrevR.Width, kPrevR.Height - 34));
        }
        LeaveCriticalSection(&g_scs);
    } else if (n) Text(g, T(L"Pas de mod\u00E8le 3D\n(conduite, couleurs\u2026)", L"No 3D model\n(handling, colours\u2026)"), RectF(kPrevR.X, kPrevR.Y + 120, kPrevR.Width, 60), 13, FontStyleRegular, kGrey);
    else Text(g, T(L"Aucun mod dans\nVCCoop\\mods", L"No mods in\nVCCoop\\mods"), RectF(kPrevR.X, kPrevR.Y + 120, kPrevR.Width, 60), 13, FontStyleRegular, kGrey);
    Text(g, selName, RectF(kPrevR.X + 10, kPrevR.Y + kPrevR.Height - 30, kPrevR.Width - 20, 24), 14, FontStyleBold, kInk);

    // liste
    float sc = g_scroll[TAB_MODS];
    g.SetClip(kGridR);
    EnterCriticalSection(&g_scs);
    for (int i = 0; i < n; i++) {
        const ModEntry &e = g_modList[i];
        RectF r(kGridR.X, kGridR.Y + i * 52.0f - sc, kGridR.Width - 6, 46);
        if (r.Y + r.Height < kGridR.Y || r.Y > kGridR.Y + kGridR.Height) continue;
        GraphicsPath rp;
        RoundRect(rp, r, 10);
        SolidBrush rb(i == sel ? TH(cardSel) : TH(card));
        g.FillPath(&rb, &rp);
        Pen rpen(i == sel ? kPink : i == g_modRowHot ? Color(255, 255, 170, 200) : TH(cardBorder), i == sel ? 2.0f : 1.2f);
        g.DrawPath(&rpen, &rp);
        RectF th(r.X + 4, r.Y + 3, 40, 40);
        if (e.thumbDone && !e.thumb.empty()) {
            Bitmap b(kThumbPx, kThumbPx, kThumbPx * 4, PixelFormat32bppPARGB, (BYTE *)e.thumb.data());
            ImageAttributes ia;
            ColorMatrix cm = { { { 1, 0, 0, 0, 0 }, { 0, 1, 0, 0, 0 }, { 0, 0, 1, 0, 0 }, { 0, 0, 0, e.on ? 1.0f : 0.35f, 0 }, { 0, 0, 0, 0, 1 } } };
            ia.SetColorMatrix(&cm);
            g.DrawImage(&b, th, 0, 0, (REAL)kThumbPx, (REAL)kThumbPx, UnitPixel, &ia);
        } else {
            SolidBrush tb(TH(thumbBg));
            g.FillEllipse(&tb, th);
        }
        Text(g, e.name, RectF(r.X + 50, r.Y + 4, r.Width - 104, 20), 12.5f, FontStyleBold, e.on ? kInk : kGrey, StringAlignmentNear);
        wchar_t info[64];
        swprintf_s(info, T(L"%d fichier(s) \u00B7 %.1f Mo", L"%d file(s) \u00B7 %.1f MB"), e.files, e.bytes / 1048576.0);
        Text(g, info, RectF(r.X + 50, r.Y + 23, r.Width - 104, 18), 10.5f, FontStyleRegular, kGrey, StringAlignmentNear);
        RectF tr(r.X + r.Width - 50, r.Y + 13, 42, 20);
        GraphicsPath tp; RoundRect(tp, tr, 10);
        if (e.on) { LinearGradientBrush lg(tr, kPink, kOrange, LinearGradientModeHorizontal); g.FillPath(&lg, &tp); }
        else { SolidBrush ob(TH(toggleOff)); g.FillPath(&ob, &tp); }
        SolidBrush knob(Color(255, 255, 255, 255));
        g.FillEllipse(&knob, e.on ? tr.X + 24 : tr.X + 2, tr.Y + 2, 16.0f, 16.0f);
    }
    LeaveCriticalSection(&g_scs);
    g.ResetClip();
    float ms = ModsMaxScroll();
    if (ms > 0) {
        float h = kGridR.Height * kGridR.Height / (kGridR.Height + ms), y = kGridR.Y + (kGridR.Height - h) * sc / ms;
        GraphicsPath sp; RoundRect(sp, RectF(kGridR.X + kGridR.Width + 4, y, 4, h), 2);
        SolidBrush sb(Color(120, 255, 79, 139)); g.FillPath(&sb, &sp);
    }
    Pen sep(TH(sep), 1);
    g.DrawLine(&sep, kOptPanel.X + 18, 536.0f, kOptPanel.X + kOptPanel.Width - 18, 536.0f);
    FontFamily fam(L"Segoe UI");
    Font font(&fam, 12, FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush db(kGrey);
    const wchar_t *hint = g_lobby != LB_NONE ? T(L"Salon ouvert : ferme-le pour activer ou d\u00E9sactiver des mods.", L"Lobby open: close it to enable or disable mods.")
                                             : T(L"Les mods actifs remplacent ceux du jeu et sont envoy\u00E9s aux invit\u00E9s. D\u00E9sactiv\u00E9s : rang\u00E9s dans VCCoop\\mods-off.",
                                                 L"Enabled mods replace the game's and are sent to guests. Disabled ones are moved to VCCoop\\mods-off.");
    g.DrawString(hint, -1, &font, RectF(kOptPanel.X + 20, 540, kOptPanel.Width - 40, 42), &sf, &db);
}

static int ModRowAt(float x, float y, bool *onToggle)
{
    *onToggle = false;
    if (!kGridR.Contains(x, y)) return -1;
    int i = (int)((y - kGridR.Y + g_scroll[TAB_MODS]) / 52.0f);
    float within = y - kGridR.Y + g_scroll[TAB_MODS] - i * 52.0f;
    EnterCriticalSection(&g_scs);
    int n = (int)g_modList.size();
    LeaveCriticalSection(&g_scs);
    if (i < 0 || i >= n || within > 46) return -1;
    *onToggle = x > kGridR.X + kGridR.Width - 60;
    return i;
}
static bool ModsMouseDown(float x, float y, bool *drag)
{
    *drag = false;
    if (kModsFolderR.Contains(x, y)) {
        CreateDirectoryW((g_gameDir + L"VCCoop").c_str(), NULL);
        CreateDirectoryW(ModsDirW(true).c_str(), NULL);
        ShellExecuteW(g_wnd, L"open", ModsDirW(true).c_str(), NULL, NULL, SW_SHOWNORMAL);
        return true;
    }
    bool tog;
    int i = ModRowAt(x, y, &tog);
    if (i >= 0) { if (tog) ModToggle(i); else g_modSel = i; return true; }
    if (kPrevR.Contains(x, y)) { *drag = true; g_skinDrag = true; g_skinDragX = x; return true; }
    return kOptPanel.Contains(x, y);
}

static void OnButton(int id)
{
    switch (id) {
    case B_HOST:
        if (g_lobby == LB_HOST) HostStart();
        else if (g_lobby == LB_GUEST) GuestToggleReady();
        else if (g_lobby == LB_NONE) LobbyHost();
        break;
    case B_JOIN:
        if (g_lobby != LB_NONE) { LobbyClose(); SetStatus(K_NORMAL, L"VCCoop %s", g_localVer.c_str()); }
        else if (g_joinFallback) { g_joinFallback = false; Launch(2); }
        else LobbyJoin();
        break;
    case B_PLAY: Launch(0); break;
    case B_EXE: ChooseExe(); break;
    case B_CLOSE: g_state = ST_CLOSING; break;
    case B_MIN: ShowWindow(g_wnd, SW_MINIMIZE); break;
    case B_THEME: g_dark = !g_dark; WritePrivateProfileStringW(L"Lanceur", L"Theme", g_dark ? L"sombre" : L"clair", g_iniLauncher.c_str()); break;
    case B_BUY: ShellExecuteW(g_wnd, L"open", kStoreUrl, NULL, NULL, SW_SHOWNORMAL); break;
    }
}

static void Tick()
{
    LobbyTick();
    static DWORD last = GetTickCount();
    DWORD now = GetTickCount();
    float dt = min((now - last) / 1000.0f, 0.1f);
    last = now;
    if ((g_tab == TAB_SKIN || g_tab == TAB_MODS) && !g_skinDrag) g_prevYaw = g_prevYaw + dt * 0.55f;
    g_time += dt;
    for (int i = 0; i < B_COUNT; i++) {
        float want = (g_hot == i && g_btn[i].enabled) ? 1.0f : 0.0f;
        g_btn[i].hover += (want - g_btn[i].hover) * min(dt * 12, 1.0f);
    }
    if (g_state == ST_CLOSING) {
        g_alpha -= dt * 4;
        if (g_alpha <= 0) { DestroyWindow(g_wnd); return; }
    } else if (g_alpha < 1) g_alpha = min(g_alpha + dt * 5, 1.0f);

    if (g_state == ST_LAUNCH) {
        if (WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0) {
            CloseHandle(g_proc); g_proc = NULL;
            g_state = ST_IDLE;
            SetStatus(K_ERR, T(L"Le jeu s'est ferm\u00E9 au d\u00E9marrage (voir vccoop.log)", L"The game closed on startup (see vccoop.log)"));
        } else {
            bool seen = false;
            EnumWindows(FindGameWindow, (LPARAM)&seen);
            if (seen && !g_winSeenT) g_winSeenT = now;
            if ((g_winSeenT && now - g_winSeenT > 1200) || now - g_launchT > 120000) g_state = ST_CLOSING;
        }
    }
    Present();
}

// ---------------------------------------------------------------- fenetre
static int HitButton(float x, float y)
{
    for (int i = 0; i < B_COUNT; i++)
        if (g_btn[i].visible && g_btn[i].r.Contains(x, y)) return i;
    return -1;
}
static int HitField(float x, float y)
{
    if (g_state != ST_IDLE || g_lobby != LB_NONE) return -1;
    for (int i = 0; i < 2; i++) if (g_fields[i].r.Contains(x, y)) return i;
    return -1;
}

static void TypeChar(wchar_t ch)
{
    if (g_focus < 0) return;
    Field &f = g_fields[g_focus];
    if (f.text.size() >= f.maxLen) return;
    if (f.address) { if (!(iswalnum(ch) && ch < 128) && ch != L'.' && ch != L':' && ch != L'-' && ch != L'_') return; }
    else if (ch < 32 || ch > 126) return;   // police du jeu : ASCII
    f.text += ch;
}

// /testsalon hote|invite : l'hote ouvre le salon et lance des que tout le monde est pret ; l'invite rejoint, attend
// ses mods, se met pret. Abandon au bout de 90 s.
static void TestSalonStep()
{
    static DWORD start = GetTickCount(), allReadySince;
    DWORD t = GetTickCount() - start;
    if (g_state != ST_IDLE) return;
    if (t > 90000) { TestLog("test : abandon (90 s)"); DestroyWindow(g_wnd); return; }
    if (g_busy) return;
    if (g_testSalon == L"hote") {
        if (g_lobby == LB_NONE && t > 1500) { LobbyHost(); if (!g_saves.empty()) g_lobbyChoice = 1; }   // teste aussi le chargement
        EnterCriticalSection(&g_lcs);
        size_t n = g_peers.size();
        LeaveCriticalSection(&g_lcs);
        bool can = n >= 2 && LobbyCanStart();
        if (!can) allReadySince = 0;
        else if (!allReadySince) allReadySince = GetTickCount();
        else if (GetTickCount() - allReadySince > 2000) HostStart();
    } else {
        if (g_lobby == LB_NONE && t > 3000 && !g_joinFallback) LobbyJoin();
        if (g_lobby == LB_GUEST && g_myMods == 100 && !g_meReady) { TestLog("test : mods a jour, pret"); GuestToggleReady(); }
    }
}

static void WriteTestLog(HWND h)
{
    FILE *f = _wfopen(g_testLog.c_str(), L"w, ccs=UTF-8");
    RECT r;
    GetWindowRect(h, &r);
    if (f) { fwprintf(f, L"ulw=%d images=%d alpha=%.2f taille=%dx%d echelle=%.2f exe=%d local=%s pid=%lu fenetre_jeu=%lu ms\n%s\n", g_ulwOk, g_frames, g_alpha,
                      r.right - r.left, r.bottom - r.top, g_scale, (int)g_exeKind, g_localVer.c_str(), g_pid,
                      g_winSeenT ? g_winSeenT - g_launchT : 0, g_status.c_str()); fclose(f); }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_TIMER:
        if (wp == 3) { KillTimer(h, 3); Launch(g_testLaunch); return 0; }
        if (wp == 4) { TestSalonStep(); return 0; }
        if (wp == 2) { WriteTestLog(h); DestroyWindow(h); return 0; }
        LobbySoundsTick();
        Tick();
        return 0;
    case WM_MOUSEMOVE: {
        float x = (short)LOWORD(lp) / g_scale, y = (short)HIWORD(lp) / g_scale;
        g_hot = HitButton(x, y);
        g_tabHot = HitTab(x, y);
        HitOption(x, y, &g_optHot, &g_optPart);
        if (g_skinDrag) { g_prevYaw = g_prevYaw + (x - g_skinDragX) * 0.018f; g_skinDragX = x; }
        g_tileHot = g_tab == TAB_SKIN ? SkinTileAt(x, y) : -1;
        g_arrowHot = g_tab == TAB_SKIN ? SkinArrowAt(x, y) : 0;
        { bool tg; g_modRowHot = g_tab == TAB_MODS ? ModRowAt(x, y, &tg) : -1; }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        SetCursor(LoadCursor(NULL, (g_skinDrag || ((g_tab == TAB_SKIN || g_tab == TAB_MODS) && kPrevR.Contains(x, y))) ? IDC_SIZEWE
                                   : ((g_hot >= 0 && g_btn[g_hot].enabled) || g_tabHot >= 0 || g_optHot >= 0 || g_tileHot >= 0 || g_arrowHot || g_modRowHot >= 0
                                      || (g_tab == TAB_MODS && kModsFolderR.Contains(x, y))) ? IDC_HAND
                                   : HitField(x, y) >= 0 ? IDC_IBEAM : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE: g_hot = -1; g_tabHot = -1; g_optHot = -1; g_tileHot = -1; g_arrowHot = 0; return 0;
    case WM_MOUSEWHEEL:
        if (g_tab >= 0) {
            float step = -(short)HIWORD(wp) / 120.0f * kRowH * 1.5f;
            g_scroll[g_tab] = min(max(g_scroll[g_tab] + step, 0.0f), MaxScroll(g_tab));
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            ScreenToClient(h, &pt);
            HitOption(pt.x / g_scale, pt.y / g_scale, &g_optHot, &g_optPart);
        }
        return 0;
    case WM_SETCURSOR: return TRUE;
    case WM_LBUTTONDOWN: {
        float x = (short)LOWORD(lp) / g_scale, y = (short)HIWORD(lp) / g_scale;
        int b = HitButton(x, y), f = HitField(x, y);
        if (b >= 0) { g_pressed = b; SetCapture(h); return 0; }
        if (f >= 0) { g_focus = f; g_time = 0; return 0; }
        g_focus = -1;
        int t = HitTab(x, y);
        if (t >= 0) { g_tab = g_tab == t ? -1 : t; g_optHot = -1; if (g_tab == TAB_MODS) ModsScan(); if (g_tab == TAB_NOTES) NotesMarkSeen(); return 0; }   // un 2e clic referme
        int row, part;
        HitOption(x, y, &row, &part);
        if (row >= 0) { OptStep(row, part < 0 ? -1 : 1); return 0; }
        if (g_tab == TAB_LOBBY && LobbyClick(x, y)) return 0;
        if (g_tab == TAB_SKIN) { bool drag; if (SkinMouseDown(x, y, &drag)) { if (drag) SetCapture(h); return 0; } }
        if (g_tab == TAB_MODS) { bool drag; if (ModsMouseDown(x, y, &drag)) { if (drag) SetCapture(h); return 0; } }
        if (g_tab >= 0 && kOptPanel.Contains(x, y)) return 0;
        ReleaseCapture();
        SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);   // glisser la fenetre
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_skinDrag) { g_skinDrag = false; ReleaseCapture(); return 0; }
        int p = g_pressed;
        g_pressed = -1;
        ReleaseCapture();
        float x = (short)LOWORD(lp) / g_scale, y = (short)HIWORD(lp) / g_scale;
        if (p >= 0 && HitButton(x, y) == p && g_btn[p].enabled) OnButton(p);
        return 0;
    }
    case WM_CHAR:
        if (g_state != ST_IDLE) return 0;
        if (wp == 8) { if (g_focus >= 0 && !g_fields[g_focus].text.empty()) g_fields[g_focus].text.pop_back(); }
        else if (wp == 127) { if (g_focus >= 0) g_fields[g_focus].text.clear(); }   // Ctrl+Retour arriere
        else if (wp == 22 && g_focus >= 0 && OpenClipboard(h)) {   // Ctrl+V
            HANDLE d = GetClipboardData(CF_UNICODETEXT);
            const wchar_t *s = d ? (const wchar_t *)GlobalLock(d) : NULL;
            if (s) { for (; *s && *s != L'\r' && *s != L'\n'; s++) TypeChar(*s); GlobalUnlock(d); }
            CloseClipboard();
        } else if (wp == 9) { g_focus = g_focus == 0 ? 1 : 0; g_time = 0; }
        else if (wp == 13) { if (g_focus == 1) Launch(2); else if (g_focus == 0) { g_focus = 1; g_time = 0; } }
        else if (wp == 27) g_focus = -1;
        else if (wp >= 32) TypeChar((wchar_t)wp);
        g_time = 0.2f;
        return 0;
    case WM_APP_GO:
        LobbyClose();
        Launch(2);
        return 0;
    case WM_APP_LOBBYEND:
        if (g_lobby == LB_NONE) return 0;
        LobbyClose();
        if (wp == 1) {
            std::string why = lp ? (const char *)lp : "";
            if (!why.compare(0, 8, "version ")) SetStatus(K_ERR, T(L"Version différente de l'hôte (%S)", L"Different version from the host (%S)"), why.c_str() + 8);
            else if (why == "full") SetStatus(K_ERR, T(L"Salon complet (4 joueurs)", L"Lobby is full (4 players)"));
            else if (why == "started") { SetStatus(K_WARN, T(L"Partie déjà lancée : « Rejoindre en jeu »", L"Session already started: \"Join in game\"")); g_joinFallback = true; }
            else SetStatus(K_ERR, T(L"Refusé par l'hôte", L"Refused by the host"));
        } else if (wp == 2) {
            SetStatus(K_WARN, T(L"Pas de salon chez l'hôte : « Rejoindre en jeu » s'il joue déjà", L"No lobby at the host: \"Join in game\" if they are already playing"));
            g_joinFallback = true;
        } else SetStatus(K_WARN, T(L"L'hôte a fermé le salon", L"The host closed the lobby"));
        return 0;
    case WM_APP_RELAUNCH: {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        std::wstring cmd = L"\"" + g_self + L"\"";
        std::vector<wchar_t> c(cmd.begin(), cmd.end());
        c.push_back(0);
        if (CreateProcessW(g_self.c_str(), c.data(), NULL, NULL, FALSE, 0, NULL, g_dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            DestroyWindow(h);
        }
        return 0;
    }
    case WM_CLOSE: g_state = ST_CLOSING; return 0;
    case WM_DESTROY:
        if (g_testLaunch >= 0) WriteTestLog(h);   // test de lancement : fermeture apres la fenetre du jeu
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static bool EncoderClsid(const wchar_t *mime, CLSID *out)
{
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (!size) return false;
    std::vector<BYTE> buf(size);
    ImageCodecInfo *info = (ImageCodecInfo *)buf.data();
    GetImageEncoders(n, size, info);
    for (UINT i = 0; i < n; i++) if (!wcscmp(info[i].MimeType, mime)) { *out = info[i].Clsid; return true; }
    return false;
}

static Bitmap *LoadPng(const wchar_t *file)
{
    for (const std::wstring &d : { g_dir, g_gameDir }) {
        if (d.empty()) continue;
        std::wstring p = d + L"VCCoop\\interface\\" + file;
        if (!FileExists(p)) continue;
        // Copie en memoire : Bitmap::FromFile garde le fichier ouvert, et la mise a jour doit pouvoir le remplacer.
        Bitmap *b = Bitmap::FromFile(p.c_str());
        if (b && b->GetLastStatus() == Ok) {
            Bitmap *copy = new Bitmap(b->GetWidth(), b->GetHeight(), PixelFormat32bppPARGB);
            {
                Graphics g(copy);
                g.SetCompositingMode(CompositingModeSourceCopy);
                g.DrawImage(b, 0, 0, b->GetWidth(), b->GetHeight());
            }
            delete b;
            return copy;
        }
        delete b;
    }
    return NULL;
}
static void LoadBackground()
{
    g_bg = LoadPng(L"launcher.png");
    g_bgDark = LoadPng(L"launcher-sombre.png");
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_lcs);
    InitializeCriticalSection(&g_scs);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_fr = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    g_self = self;
    g_dir = DirOf(g_self);
    g_iniLauncher = g_dir + L"vccoop-launcher.ini";
    DeleteFileW((g_self + L".old").c_str());   // reste d'une mise a jour du lanceur
    // Langue : francais si Windows est en francais, anglais pour toute autre langue ; Langue=fr|en pour forcer.
    wchar_t lang[8] = L"";
    GetPrivateProfileStringW(L"Lanceur", L"Langue", L"", lang, 8, g_iniLauncher.c_str());
    if (!_wcsicmp(lang, L"fr")) g_fr = true;
    else if (!_wcsicmp(lang, L"en")) g_fr = false;
    // Theme : Theme=clair|sombre, sinon celui des applications de Windows
    {
        wchar_t th[16] = L"";
        GetPrivateProfileStringW(L"Lanceur", L"Theme", L"", th, 16, g_iniLauncher.c_str());
        if (!_wcsicmp(th, L"sombre") || !_wcsicmp(th, L"dark")) g_dark = true;
        else if (!_wcsicmp(th, L"clair") || !_wcsicmp(th, L"light")) g_dark = false;
        else {
            DWORD v = 1, sz = sizeof(v);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS) g_dark = v == 0;
        }
    }

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && !_wcsicmp(argv[1], L"/check")) return (int)CheckExe(argv[2]);
    for (int i = 1; i + 1 < argc; i++) {
        if (!_wcsicmp(argv[i], L"/lang")) g_fr = !_wcsicmp(argv[i + 1], L"fr");
        if (!_wcsicmp(argv[i], L"/theme")) g_dark = !_wcsicmp(argv[i + 1], L"sombre");
        if (!_wcsicmp(argv[i], L"/echelle")) g_scale = (float)_wtof(argv[i + 1]);   // captures : rendu agrandi
        if (!_wcsicmp(argv[i], L"/testfenetre")) g_testLog = argv[i + 1];
        if (!_wcsicmp(argv[i], L"/testlancer") && i + 2 < argc) { g_testLaunch = _wtoi(argv[i + 1]); g_testLog = argv[i + 2]; }
        if (!_wcsicmp(argv[i], L"/testsalon") && i + 2 < argc) { g_testSalon = argv[i + 1]; g_testSalonLog = argv[i + 2]; g_testLog = g_testSalonLog + L".fin"; g_testLaunch = 99; }
    }

    GdiplusStartupInput gin;
    ULONG_PTR gtok;
    GdiplusStartup(&gtok, &gin, NULL);
    Layout();
    BuildOptions();

    wchar_t saved[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"Lanceur", L"Exe", L"", saved, MAX_PATH, g_iniLauncher.c_str());
    std::wstring start = (saved[0] && FileExists(saved)) ? saved : g_dir + L"gta-vc.exe";
    SetExe(start);
    LoadBackground();
    SkinsInit();
    if (g_exeKind == EXE_MISSING) SetStatus(K_ERR, T(L"Choisis ton gta-vc.exe (version 1.0)", L"Choose your gta-vc.exe (version 1.0)"));
    else if (g_exeKind != EXE_OK) SetStatus(K_ERR, T(L"Ce gta-vc.exe n'est pas la version 1.0", L"This gta-vc.exe is not version 1.0"));
    else SetStatus(K_NORMAL, L"VCCoop %s", g_localVer.empty() ? L"" : g_localVer.c_str());

    // /maj <exe> <journal> : verification + mise a jour sans fenetre (tests) ; journal = etat final
    if (argc >= 4 && !_wcsicmp(argv[1], L"/maj")) {
        SetExe(argv[2]);
        if (g_exeKind == EXE_OK) { g_busy = true; UpdateThread(NULL); }
        FILE *f = _wfopen(argv[3], L"w, ccs=UTF-8");
        if (f) { fwprintf(f, L"exe=%d local=%s\n%s\n", (int)g_exeKind, g_localVer.c_str(), g_status.c_str()); fclose(f); }
        delete g_bg;
        GdiplusShutdown(gtok);
        return 0;
    }

    // /testmodele <dossier du jeu\> <sortie.png> : planche des tenues (6 de face, 6 de trois quarts, 6 portraits)
    if (argc >= 4 && !_wcsicmp(argv[1], L"/testmodele")) {
        int rc = 1;
        if (ImgOpen(argv[2])) {
            std::vector<std::string> skins = SkinList();
            FILE *lf = _wfopen((std::wstring(argv[3]) + L".txt").c_str(), L"w");
            if (lf) { fprintf(lf, "%d tenues :", (int)skins.size()); for (auto &k : skins) fprintf(lf, " %s", k.c_str()); fprintf(lf, "\n"); }
            const int CW = 200, CH = 300;
            Bitmap sheet(CW * 6, CH * 2 + 140, PixelFormat32bppARGB);
            {
                Graphics g(&sheet);
                g.Clear(Color(255, 40, 70, 110));
                std::vector<uint32_t> buf(CW * CH);
                const char *pick[6] = { "player", "play3", "igken", "hfyst", "cop", "wmybe" };
                for (int i = 0; i < 12; i++) {
                    std::string name = i < 6 ? pick[i] : skins[min((size_t)(i * 11), skins.size() - 1)];
                    DWORD t0 = GetTickCount();
                    Model3D *m = ModelLoad(name);
                    DWORD t1 = GetTickCount();
                    ModelRender(m, buf.data(), CW, CH, i < 6 ? 0.0f : 0.7f, false);
                    DWORD t2 = GetTickCount();
                    { float lo[3], hi[3]; ModelBounds(m, lo, hi); if (lf) fprintf(lf, "%s : %s, chargement %lu ms, rendu %lu ms, boite %.2f %.2f %.2f / %.2f %.2f %.2f\n", name.c_str(), m ? "ok" : "ECHEC", t1 - t0, t2 - t1, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]); }
                    Bitmap b(CW, CH, CW * 4, PixelFormat32bppPARGB, (BYTE *)buf.data());
                    g.DrawImage(&b, (i % 6) * CW, (i / 6) * CH, CW, CH);
                    Text(g, Widen(name), RectF((float)(i % 6) * CW, (float)(i / 6) * CH + CH - 22, (float)CW, 20), 13, FontStyleBold, Color(255, 255, 255, 255));
                    if (i < 6) {
                        std::vector<uint32_t> pb(128 * 128);
                        ModelRender(m, pb.data(), 128, 128, 0.25f, true);
                        Bitmap pbm(128, 128, 128 * 4, PixelFormat32bppPARGB, (BYTE *)pb.data());
                        g.DrawImage(&pbm, i * CW + 36, CH * 2 + 6, 128, 128);
                    }
                    ModelFree(m);
                }
            }
            CLSID png;
            if (EncoderClsid(L"image/png", &png) && sheet.Save(argv[3], &png, NULL) == Ok) rc = 0;
            if (lf) fclose(lf);
        }
        delete g_bg;
        GdiplusShutdown(gtok);
        return rc;
    }

    // /testmod <jeu\> <dff> <txd|-> <png> : rendu d'un modele de mod (objet) et sa boite
    if (argc >= 6 && !_wcsicmp(argv[1], L"/testmod")) {
        ImgOpen(argv[2]);
        DWORD t0 = GetTickCount();
        Model3D *m = ModelLoadPath(argv[3], wcscmp(argv[4], L"-") ? argv[4] : L"");
        DWORD t1 = GetTickCount();
        float lo[3], hi[3];
        ModelBounds(m, lo, hi);
        std::vector<uint32_t> px(400 * 300);
        ModelRender(m, px.data(), 400, 300, argc >= 7 ? (float)_wtof(argv[6]) : 0.6f, 2);
        DWORD t2 = GetTickCount();
        int painted = 0;
        for (uint32_t c : px) painted += c != 0;
        FILE *f = _wfopen((std::wstring(argv[5]) + L".txt").c_str(), L"w");
        if (f) { fprintf(f, "modele %s, chargement %lu ms, rendu %lu ms, boite %.2f %.2f %.2f / %.2f %.2f %.2f, pixels %d\n%s", m ? "ok" : "ECHEC", t1 - t0, t2 - t1, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], painted, ModelInfo(m).c_str()); fclose(f); }
        {
            Bitmap out(400, 300, 400 * 4, PixelFormat32bppPARGB, (BYTE *)px.data());
            CLSID png;
            if (EncoderClsid(L"image/png", &png)) out.Save(argv[5], &png, NULL);
        }
        ModelFree(m);
        delete g_bg;
        GdiplusShutdown(gtok);
        return 0;
    }

    // /testoptions <journal> : fait avancer quelques options (test de l'ecriture dans vccoop.ini)
    if (argc >= 3 && !_wcsicmp(argv[1], L"/testoptions")) {
        FILE *f = _wfopen(argv[2], L"w, ccs=UTF-8");
        auto find = [](const char *k) { for (int i = 0; i < (int)g_opts.size(); i++) if (!strcmp(g_opts[i].key, k)) return i; return -1; };
        const char *keys[] = { "DistanceAffichage", "Rendu", "Anticrenelage", "SMAA", "Fenetre" };
        for (const char *k : keys) {
            int i = find(k), a = OptGet(g_opts[i]);
            OptStep(i, 1);
            int b = OptGet(g_opts[i]);
            OptStep(i, -1);
            if (f) fwprintf(f, L"%S : %d -> %d -> %d ; onglet rendu visible=%d\n", k, a, b, OptGet(g_opts[i]), (int)TabVisible(TAB_RENDER));
        }
        if (f) fclose(f);
        delete g_bg;
        GdiplusShutdown(gtok);
        return 0;
    }

    // Capture d'un etat, sans fenetre ni reseau (verification du rendu)
    if (argc >= 4 && !_wcsicmp(argv[1], L"/capture")) {
        std::wstring st = argv[3];
        g_alpha = 1;
        g_time = 0.3f;
        if (st == L"attente") { g_state = ST_LAUNCH; g_launchInfo = L"Tommy h\u00E9berge la partie"; g_time = 1.3f; }
        else if (st == L"sansexe") { g_exeKind = EXE_STEAM; g_localVer.clear(); SetStatus(K_ERR, T(L"Ce gta-vc.exe n'est pas la version 1.0", L"This gta-vc.exe is not version 1.0")); }
        else if (st == L"options" || st == L"coop") { g_tab = st == L"coop" ? TAB_COOP : TAB_VIDEO; g_optHot = g_tab == TAB_COOP ? TabRows(TAB_COOP)[0] : TabRows(TAB_VIDEO)[4]; g_optPart = 1; g_tabHot = TAB_FX; }
        else if (st == L"salon" || st == L"saloninvite") {
            bool host = st == L"salon";
            g_lobby = host ? LB_HOST : LB_GUEST;
            g_myId = host ? 0 : 2;
            g_lobbyPort = 7790;
            g_lobbyAddr = L"26.12.34.56";
            g_myAddresses = L"192.168.1.20 \u00B7 26.12.34.56";
            g_saves = { { 1, "1 \xC2\xB7 AU DEBUT... \xC2\xB7 25/09 21:51" } };
            g_lobbyChoice = 1;
            g_lobbyChoiceLabel = "1 \xC2\xB7 AU DEBUT... \xC2\xB7 25/09 21:51";
            g_peers = { { 0, "JD", "", true, -1, 0 }, { 1, "GG", "igken", true, 100, 34 }, { 2, "Sam", "hfyst", false, 42, 61 } };
            g_meReady = false;
            g_tab = TAB_LOBBY;
            LayoutTabs();
            SetStatus(K_OK, host ? T(L"Salon ouvert \u00B7 port %d", L"Lobby open \u00B7 port %d") : T(L"Dans le salon de %s", L"In %s's lobby"), host ? L"7790" : L"26.12.34.56");
            if (host) SetStatus(K_OK, T(L"Salon ouvert \u00B7 port 7790", L"Lobby open \u00B7 port 7790"));
            g_localVer = L"2026.09.29m";
        }
        else if (st == L"mods") { g_tab = TAB_MODS; ModsScan(); g_prevYaw = 0.6f; }
        else if (st == L"rendu") { g_rtProbe = 1; g_tab = TAB_RT; LayoutTabs(); g_optHot = TabRows(TAB_RT)[2]; }
        else if (st == L"notes") { NotesFetch(); g_tab = TAB_NOTES; LayoutTabs(); }
        else if (st == L"tenue") { g_tab = TAB_SKIN; g_skinSel = 0; g_prevYaw = 0.35f; g_tileHot = 4; }
        else if (st == L"maj") { g_busy = true; g_progress = 0.42f; SetStatus(K_NORMAL, T(L"T\u00E9l\u00E9chargement de VCCoop %s\u2026", L"Downloading VCCoop %s\u2026"), L"2026.09.29h"); g_focus = 0; g_time = 0.2f; }
        else { g_localVer = g_localVer.empty() ? L"2026.09.29h" : g_localVer; SetStatus(K_OK, T(L"VCCoop %s \u00B7 \u00E0 jour", L"VCCoop %s \u00B7 up to date"), g_localVer.c_str()); g_hot = B_HOST; g_btn[B_HOST].hover = 1; }
        int rc = 1;
        // tenues : attendre l'apercu et les portraits (fils de fond)
        if (g_imgOk && (st == L"tenue" || st == L"salon" || st == L"saloninvite" || st == L"mods")) {
            g_prevW = (int)(kPrevR.Width * g_scale); g_prevH = (int)((kPrevR.Height - 34) * g_scale);
            int st0 = g_state; g_state = ST_IDLE;
            if (st == L"salon" || st == L"saloninvite") { EnterCriticalSection(&g_scs); g_portraitWanted = { "player", "igken", "hfyst" }; LeaveCriticalSection(&g_scs); }
            for (int i = 0; i < 100; i++) {
                EnterCriticalSection(&g_scs);
                size_t np = g_portraits.size();
                bool frame = !g_prevFrame.empty();
                LeaveCriticalSection(&g_scs);
                bool thumbs = true;
                for (auto &e : g_modList) thumbs &= e.thumbDone;
                if (np >= 15 && (frame || (st != L"tenue" && st != L"mods")) && (thumbs || st != L"mods")) break;
                Sleep(100);
            }
            g_state = st0;
        }
        {
            Bitmap out((INT)(kImgW * g_scale), (INT)(kImgH * g_scale), PixelFormat32bppPARGB);
            RenderTo(out, g_scale);
            CLSID png;
            if (EncoderClsid(L"image/png", &png) && out.Save(argv[2], &png, NULL) == Ok) rc = 0;
        }   // (detruit avant GdiplusShutdown)
        delete g_bg;
        GdiplusShutdown(gtok);
        return rc;
    }
    LocalFree(argv);

    // Taille : l'image a l'echelle de l'ecran (PPP), sans depasser 90 % de la zone de travail.
    HDC sdc = GetDC(NULL);
    g_scale = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f;
    ReleaseDC(NULL, sdc);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    float fit = min((work.right - work.left) * 0.9f / kImgW, (work.bottom - work.top) * 0.9f / kImgH);
    g_scale = max(0.5f, min(g_scale, fit));
    g_winW = (int)(kImgW * g_scale);
    g_winH = (int)(kImgH * g_scale);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    wc.lpszClassName = L"VCCoopLauncher";
    RegisterClassExW(&wc);
    int x = work.left + (work.right - work.left - g_winW) / 2, y = work.top + (work.bottom - work.top - g_winH) / 2;
    bool test = !g_testLog.empty();
    if (test) x = y = -5000;
    g_wnd = CreateWindowExW(WS_EX_LAYERED | (test ? WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW : WS_EX_APPWINDOW), wc.lpszClassName, L"VCCoop", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                            x, y, g_winW, g_winH, NULL, NULL, inst, NULL);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = g_winW;
    bi.bmiHeader.biHeight = -g_winH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    g_memDC = CreateCompatibleDC(NULL);
    g_dib = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    SelectObject(g_memDC, g_dib);

    Present();
    ShowWindow(g_wnd, test ? SW_SHOWNOACTIVATE : SW_SHOW);
    SetTimer(g_wnd, 1, 16, NULL);
    if (test && g_testSalon.empty()) SetTimer(g_wnd, g_testLaunch >= 0 ? 3 : 2, g_testLaunch >= 0 ? 3000 : 5000, NULL);
    if (test && !g_testSalon.empty()) SetTimer(g_wnd, 4, 500, NULL);
    if (g_exeKind == EXE_OK) StartUpdate();
    else if (g_exeKind != EXE_MISSING && !test) BadExeMessage(g_exeKind);
    { HANDLE nt = CreateThread(NULL, 0, NotesThread, NULL, 0, NULL); if (nt) CloseHandle(nt); }
    if (!g_gameDir.empty()) { HANDLE pt = CreateThread(NULL, 0, RtProbeThread, NULL, 0, NULL); if (pt) CloseHandle(pt); }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_proc) CloseHandle(g_proc);
    delete g_bg;
    GdiplusShutdown(gtok);
    return 0;
}
