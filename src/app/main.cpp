// TeleFiles - client Telegram per Windows dedicato a upload e download di file
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "../core/Engine.h"
#include "../core/FileIO.h"
#include "MainWindow.h"
#include "Text.h"
#include "Version.h"

using namespace tf;

static std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) r = wideToUtf8(p);
    CoTaskMemFree(p);
    return r;
}

static std::string exeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s = buf;
    return wideToUtf8(s.substr(0, s.find_last_of(L"\\/")));
}

// api_id/api_hash: da api.ini (accanto all'exe o in %APPDATA%\TeleFiles) oppure fissati in compilazione.
static bool readApiIni(const std::string& path, int& id, std::string& hash) {
    std::ifstream in(u8p(path));
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        if (k == "api_id") id = atoi(v.c_str());
        if (k == "api_hash") hash = v;
    }
    return id > 0 && !hash.empty();
}

static std::string windowsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v{sizeof v};
    if (auto f = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) f(&v);
    return std::string(v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10") + " (build " +
           std::to_string(v.dwBuildNumber) + ")";
}

// Portable se compilato con TELEFILES_PORTABLE oppure se accanto all'exe c'è "TeleFiles.portable".
static bool isPortable(const std::string& dir) {
#ifdef TF_PORTABLE
    (void)dir;
    return true;
#else
    return fileExists(joinPath(dir, "TeleFiles.portable"));
#endif
}

static uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    const std::string base = exeDir();
    const bool portable = isPortable(base);

    Engine::Config cfg;
    std::string apiDir;  // dove cercare/indicare api.ini
    if (portable) {
        // tutto accanto all'exe: sessione, impostazioni, coda, temporanei e download
        std::string data = joinPath(base, "Data");
        cfg.dataDir = data;
        cfg.tempDir = joinPath(data, "upload-temp");
        cfg.defaultDownloadDir = joinPath(base, "Download");
        cfg.portableBase = base;
        apiDir = base;
    } else {
        std::string local = joinPath(knownFolder(FOLDERID_LocalAppData), "TeleFiles");
        cfg.dataDir = local;
        cfg.tempDir = joinPath(local, "upload-temp");
        cfg.defaultDownloadDir = joinPath(knownFolder(FOLDERID_Downloads), "TeleFiles");
        apiDir = joinPath(knownFolder(FOLDERID_RoamingAppData), "TeleFiles");
    }
    makeDirs(cfg.dataDir);
    if (!portable) makeDirs(apiDir);

    // una sola istanza per cartella dati: TDLib non permette due processi sullo stesso database
    // (due copie portable in cartelle diverse possono convivere)
    std::wstring mutexName = L"TeleFiles.SingleInstance." + std::to_wstring(fnv1a(cfg.dataDir));
    HANDLE single = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(L"TeleFilesMain", nullptr)) {
            ShowWindow(w, IsIconic(w) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(w);
        }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    cfg.appVersion = TELEFILES_VERSION;
    cfg.systemVersion = windowsVersion();
    cfg.deviceModel = portable ? "PC Windows (portable)" : "PC Windows";

    std::wstring configError;
    if (!readApiIni(joinPath(base, "api.ini"), cfg.apiId, cfg.apiHash) &&
        !(!portable && readApiIni(joinPath(apiDir, "api.ini"), cfg.apiId, cfg.apiHash))) {
#if defined(TF_API_ID) && defined(TF_API_HASH)
        cfg.apiId = TF_API_ID;
        cfg.apiHash = TF_API_HASH;
#endif
    }
    if (cfg.apiId <= 0 || cfg.apiHash.empty())
        configError = L"Mancano le credenziali API di Telegram.\n\nCreale gratis su my.telegram.org "
                      L"(API development tools) e salvale nel file\n" +
                      utf8ToWide(joinPath(apiDir, "api.ini")) +
                      L"\n\ncon queste due righe:\napi_id=123456\napi_hash=0123456789abcdef...\n\nPoi riapri TeleFiles.";

    Engine engine(cfg);
#ifdef TF_SIM_BUILD
    // solo per la build di simulazione: limite di divisione ridotto per vedere le parti
    if (const char* l = std::getenv("TF_SPLIT_LIMIT")) engine.setSplitLimitForTesting(std::atoll(l));
#endif
    MainWindow win(engine, configError, portable);
    engine.setNotify([&win] { if (win.hwnd()) win.requestRefresh(); });
    if (!win.create(inst, show)) return 1;
    if (configError.empty()) engine.start();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    engine.setNotify(nullptr);
    engine.shutdown();
    CoUninitialize();
    ReleaseMutex(single);
    return 0;
}
