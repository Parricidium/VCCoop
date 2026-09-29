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
#include <windows.h>
#include <algorithm>
using std::min;
using std::max;
#include <objidl.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <atomic>
#include <stdio.h>
#include <math.h>

using namespace Gdiplus;

static const wchar_t *kRepoApi = L"https://api.github.com/repos/Parricidium/VCCoop/releases/latest";
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
static Bitmap *g_bg;
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

static std::wstring g_testLog;          // /testfenetre : fenetre hors ecran, sans activation, journal puis sortie
static int g_ulwOk = -1, g_frames;

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
            if (!MoveFileExW(g_self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) { ok = false; *failed = n; continue; }
            *selfReplaced = true;
        }
        SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!CopyFileW(s.c_str(), d.c_str(), FALSE)) { ok = false; *failed = n; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
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
enum { B_HOST, B_JOIN, B_PLAY, B_EXE, B_CLOSE, B_MIN, B_COUNT };
struct Button { RectF r; float hover; bool visible, enabled; };
static Button g_btn[B_COUNT];
static int g_hot = -1, g_pressed = -1;

static void Layout()
{
    g_fields[0].r = RectF(76, 286, 304, 36); g_fields[0].maxLen = 23; g_fields[0].address = false;
    g_fields[1].r = RectF(76, 352, 304, 36); g_fields[1].maxLen = 63; g_fields[1].address = true;
    g_btn[B_HOST].r = RectF(76, 408, 148, 46);
    g_btn[B_JOIN].r = RectF(232, 408, 148, 46);
    g_btn[B_PLAY].r = RectF(76, 464, 304, 34);
    g_btn[B_EXE].r = RectF(250, 502, 130, 20);
    g_btn[B_CLOSE].r = RectF(938, 76, 28, 28);
    g_btn[B_MIN].r = RectF(904, 76, 28, 28);
}

static void UpdateButtons()
{
    bool menu = g_state == ST_IDLE, exeOk = g_exeKind == EXE_OK, busy = g_busy;
    for (int i = 0; i < B_COUNT; i++) g_btn[i].visible = true;
    g_btn[B_HOST].visible = g_btn[B_JOIN].visible = g_btn[B_PLAY].visible = g_btn[B_EXE].visible = menu;
    g_btn[B_HOST].enabled = g_btn[B_JOIN].enabled = g_btn[B_PLAY].enabled = menu && exeOk && !busy && !g_localVer.empty();
    g_btn[B_EXE].enabled = menu && !busy;
    g_btn[B_CLOSE].enabled = g_btn[B_MIN].enabled = true;
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

static const Color kInk(255, 52, 40, 62), kGrey(255, 128, 112, 130), kPink(255, 255, 79, 139), kOrange(255, 255, 138, 91);

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
        SolidBrush fill(Mix(Color((BYTE)(215 * a), 255, 255, 255), Color((BYTE)(240 * a), 255, 236, 244), b.hover));
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
    SolidBrush fill(Color(235, 255, 255, 255));
    g.FillPath(&fill, &p);
    Pen pen(g_focus == i ? kPink : Color(255, 226, 206, 216), g_focus == i ? 2.0f : 1.2f);
    g.DrawPath(&pen, &p);
    RectF tr(f.r.X + 12, f.r.Y, f.r.Width - 24, f.r.Height);
    std::wstring shown = f.text;
    bool placeholder = shown.empty() && g_focus != i;
    if (placeholder) shown = f.address ? L"ex. 26.12.34.56" : L"Tommy";
    Text(g, shown, tr, 15, FontStyleRegular, placeholder ? Color(255, 190, 176, 190) : kInk, StringAlignmentNear);
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
    for (int id : { B_MIN, B_CLOSE }) {
        Button &b = g_btn[id];
        SolidBrush cb(Mix(Color(150, 255, 255, 255), Color(235, 255, 255, 255), b.hover));
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
        Text(g, status, RectF(60, 214, 336, 22), 13, FontStyleBold, sc);
        if (prog != -1.0f) DrawBar(g, RectF(96, 242, 264, 5), prog);
        DrawField(g, 0, T(L"PSEUDO", L"NICKNAME"));
        DrawField(g, 1, T(L"ADRESSE DE L'H\u00D4TE", L"HOST ADDRESS"));
        DrawButton(g, B_HOST, T(L"H\u00C9BERGER", L"HOST"), true);
        DrawButton(g, B_JOIN, T(L"REJOINDRE", L"JOIN"), false);
        DrawButton(g, B_PLAY, T(L"JOUER (MENU COOP)", L"PLAY (COOP MENU)"), false);
        // exe cible
        std::wstring exeLine;
        Color ec = kGrey;
        if (g_exeKind == EXE_OK) { exeLine = L"gta-vc.exe 1.0 \u2713"; ec = Color(255, 38, 150, 96); }
        else if (g_exeKind == EXE_MISSING) { exeLine = T(L"gta-vc.exe introuvable", L"gta-vc.exe not found"); ec = Color(255, 214, 48, 72); }
        else { exeLine = T(L"exe pas en 1.0", L"exe is not 1.0"); ec = Color(255, 214, 48, 72); }
        Text(g, exeLine, RectF(78, 502, 170, 20), 12, FontStyleBold, ec, StringAlignmentNear);
        Button &eb = g_btn[B_EXE];
        const wchar_t *el = g_exeKind == EXE_OK ? T(L"Changer d'exe", L"Change exe") : T(L"Choisir l'exe\u2026", L"Choose exe\u2026");
        Color lc = Mix(g_exeKind == EXE_OK ? kGrey : kPink, kPink, eb.hover);
        Text(g, el, eb.r, 12, g_exeKind == EXE_OK ? FontStyleUnderline : FontStyleBold | FontStyleUnderline, WithA(lc, eb.enabled ? 1.0f : 0.4f), StringAlignmentFar);
    }
    const Color lg(200, 128, 112, 130);
    Text(g, T(L"Mod non officiel et non commercial.", L"Unofficial, non-commercial mod."), RectF(56, 527, 344, 14), 10, FontStyleRegular, lg);
    Text(g, T(L"Non affili\u00E9 \u00E0 Rockstar Games ni \u00E0 Take-Two.", L"Not affiliated with Rockstar Games or Take-Two."), RectF(56, 540, 344, 14), 10, FontStyleRegular, lg);
    Text(g, T(L"N\u00E9cessite une copie l\u00E9gale de GTA: Vice City.", L"Requires a legal copy of GTA: Vice City."), RectF(56, 553, 344, 14), 10, FontStyleRegular, lg);
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
    if (g_bg) g.DrawImage(g_bg, RectF(0, 0, kImgW, kImgH));
    else {   // pas d'image : carte simple
        GraphicsPath p;
        RoundRect(p, RectF(20, 60, 960, 540), 26);
        LinearGradientBrush lg(RectF(20, 60, 960, 540), Color(255, 255, 222, 236), Color(255, 255, 178, 158), LinearGradientModeVertical);
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

static void Launch(int mode)
{
    if (g_exeKind != EXE_OK || g_busy) return;
    std::wstring addr = Trim(g_fields[1].text);
    if (mode == 2 && addr.empty()) {
        SetStatus(K_ERR, T(L"Entre l'adresse de l'h\u00F4te", L"Enter the host address"));
        g_focus = 1;
        return;
    }
    SavePlayer();
    std::wstring cmd = L"\"" + g_exe + L"\"";
    if (mode == 1) cmd += L" -vccoop hote";
    else if (mode == 2) cmd += L" -vccoop invite " + addr;
    std::vector<wchar_t> c(cmd.begin(), cmd.end());
    c.push_back(0);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(g_exe.c_str(), c.data(), NULL, NULL, FALSE, 0, NULL, g_gameDir.c_str(), &si, &pi)) {
        SetStatus(K_ERR, T(L"Impossible de lancer gta-vc.exe (erreur %lu)", L"Could not start gta-vc.exe (error %lu)"), GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    g_pid = pi.dwProcessId;
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

static void OnButton(int id)
{
    switch (id) {
    case B_HOST: Launch(1); break;
    case B_JOIN: Launch(2); break;
    case B_PLAY: Launch(0); break;
    case B_EXE: ChooseExe(); break;
    case B_CLOSE: g_state = ST_CLOSING; break;
    case B_MIN: ShowWindow(g_wnd, SW_MINIMIZE); break;
    }
}

static void Tick()
{
    static DWORD last = GetTickCount();
    DWORD now = GetTickCount();
    float dt = min((now - last) / 1000.0f, 0.1f);
    last = now;
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
    if (g_state != ST_IDLE) return -1;
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

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_TIMER:
        if (wp == 2) {
            FILE *f = _wfopen(g_testLog.c_str(), L"w, ccs=UTF-8");
            RECT r;
            GetWindowRect(h, &r);
            if (f) { fwprintf(f, L"ulw=%d images=%d alpha=%.2f taille=%dx%d echelle=%.2f exe=%d local=%s\n%s\n", g_ulwOk, g_frames, g_alpha,
                              r.right - r.left, r.bottom - r.top, g_scale, (int)g_exeKind, g_localVer.c_str(), g_status.c_str()); fclose(f); }
            DestroyWindow(h);
            return 0;
        }
        Tick();
        return 0;
    case WM_MOUSEMOVE: {
        float x = (short)LOWORD(lp) / g_scale, y = (short)HIWORD(lp) / g_scale;
        g_hot = HitButton(x, y);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        SetCursor(LoadCursor(NULL, (g_hot >= 0 && g_btn[g_hot].enabled) ? IDC_HAND : HitField(x, y) >= 0 ? IDC_IBEAM : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE: g_hot = -1; return 0;
    case WM_SETCURSOR: return TRUE;
    case WM_LBUTTONDOWN: {
        float x = (short)LOWORD(lp) / g_scale, y = (short)HIWORD(lp) / g_scale;
        int b = HitButton(x, y), f = HitField(x, y);
        if (b >= 0) { g_pressed = b; SetCapture(h); return 0; }
        if (f >= 0) { g_focus = f; g_time = 0; return 0; }
        g_focus = -1;
        ReleaseCapture();
        SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);   // glisser la fenetre
        return 0;
    }
    case WM_LBUTTONUP: {
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
    case WM_DESTROY: PostQuitMessage(0); return 0;
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

static void LoadBackground()
{
    for (const std::wstring &d : { g_dir, g_gameDir }) {
        if (d.empty()) continue;
        std::wstring p = d + L"VCCoop\\interface\\launcher.png";
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
            g_bg = copy;
            return;
        }
        delete b;
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    InitializeCriticalSection(&g_cs);
    g_fr = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    g_self = self;
    g_dir = DirOf(g_self);
    g_iniLauncher = g_dir + L"vccoop-launcher.ini";
    DeleteFileW((g_self + L".old").c_str());   // reste d'une mise a jour du lanceur

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && !_wcsicmp(argv[1], L"/check")) return (int)CheckExe(argv[2]);
    for (int i = 1; i + 1 < argc; i++) {
        if (!_wcsicmp(argv[i], L"/lang")) g_fr = !_wcsicmp(argv[i + 1], L"fr");
        if (!_wcsicmp(argv[i], L"/testfenetre")) g_testLog = argv[i + 1];
    }

    GdiplusStartupInput gin;
    ULONG_PTR gtok;
    GdiplusStartup(&gtok, &gin, NULL);
    Layout();

    wchar_t saved[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"Lanceur", L"Exe", L"", saved, MAX_PATH, g_iniLauncher.c_str());
    std::wstring start = (saved[0] && FileExists(saved)) ? saved : g_dir + L"gta-vc.exe";
    SetExe(start);
    LoadBackground();
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

    // Capture d'un etat, sans fenetre ni reseau (verification du rendu)
    if (argc >= 4 && !_wcsicmp(argv[1], L"/capture")) {
        std::wstring st = argv[3];
        g_alpha = 1;
        g_time = 0.3f;
        if (st == L"attente") { g_state = ST_LAUNCH; g_launchInfo = L"Tommy h\u00E9berge la partie"; g_time = 1.3f; }
        else if (st == L"sansexe") { g_exeKind = EXE_STEAM; g_localVer.clear(); SetStatus(K_ERR, T(L"Ce gta-vc.exe n'est pas la version 1.0", L"This gta-vc.exe is not version 1.0")); }
        else if (st == L"maj") { g_busy = true; g_progress = 0.42f; SetStatus(K_NORMAL, T(L"T\u00E9l\u00E9chargement de VCCoop %s\u2026", L"Downloading VCCoop %s\u2026"), L"2026.09.29h"); g_focus = 0; g_time = 0.2f; }
        else { g_localVer = g_localVer.empty() ? L"2026.09.29h" : g_localVer; SetStatus(K_OK, T(L"VCCoop %s \u00B7 \u00E0 jour", L"VCCoop %s \u00B7 up to date"), g_localVer.c_str()); g_hot = B_HOST; g_btn[B_HOST].hover = 1; }
        int rc = 1;
        {
            Bitmap out((INT)kImgW, (INT)kImgH, PixelFormat32bppPARGB);
            RenderTo(out, 1.0f);
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
    if (test) SetTimer(g_wnd, 2, 5000, NULL);
    if (g_exeKind == EXE_OK) StartUpdate();
    else if (g_exeKind != EXE_MISSING && !test) BadExeMessage(g_exeKind);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_proc) CloseHandle(g_proc);
    delete g_bg;
    GdiplusShutdown(gtok);
    return 0;
}
