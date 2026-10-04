// KenshiMP_Loader.dll: starts KenshiMP without RE_Kenshi.
//
// Kenshi's interface library (MyGUI) loads the plugins listed in its settings files and calls their
// dllStartPlugin(); the mod's gui\core\core_settings.xml lists this loader (Kenshi reads the gui
// folder of every enabled mod), so ticking KenshiMP in the launcher is enough. The same entry point
// also works from Ogre's Plugins_x64.cfg. The loader then:
//  1. does nothing if RE_Kenshi is in the game (RE_Kenshi starts KenshiMP itself, RE_Kenshi.json);
//  2. recognises the executable (MD5): the stock Steam 1.0.68 build uses the address table shipped
//     with KenshiMP (rva\RE_Kenshi\RVAs\Steam_1.0.65.br, made by matching the 1.0.65 functions in
//     the 1.0.68 executable - see tools/README.md); a 1.0.65 build uses RE_Kenshi's own tables;
//  3. loads the KenshiLib.dll next to it, hands it that table (KenshiLib reads
//     "RE_Kenshi/RVAs/<platform>_<version>.br" from the current folder, so the loader points the
//     current folder at its rva folder while KenshiLib::Init runs), then loads KenshiMP.dll and
//     calls its startPlugin().
// Anything unexpected: a line in KenshiMP_loader.log (game folder) and the game starts without
// multiplayer - never a crash.
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string>

#define KENSHILIB_INTERNAL   // Init / OverrideKenshiVersion: what RE_Kenshi normally calls
#include <core/Functions.h>
#include <kenshi/Kenshi.h>

namespace
{
    FILE* g_log = NULL;
    void log(const char* fmt, ...)
    {
        if (!g_log) fopen_s(&g_log, "KenshiMP_loader.log", "w");
        if (!g_log) return;
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(g_log, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
        va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
        fprintf(g_log, "\n"); fflush(g_log);
    }

    std::wstring moduleDir()
    {
        HMODULE me = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&moduleDir, &me);
        wchar_t path[MAX_PATH] = { 0 };
        GetModuleFileNameW(me, path, MAX_PATH);
        std::wstring p(path);
        size_t s = p.find_last_of(L"\\/");
        return s == std::wstring::npos ? L"." : p.substr(0, s);
    }

    std::string narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

    // MD5 of the running executable, lowercase hex ("" on error).
    std::string exeMd5()
    {
        wchar_t path[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, path, MAX_PATH);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) return "";
        HCRYPTPROV prov = 0; HCRYPTHASH hash = 0;
        std::string out;
        if (CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) && CryptCreateHash(prov, CALG_MD5, 0, 0, &hash))
        {
            static BYTE buf[1 << 16];
            DWORD n = 0; bool ok = true;
            while (ReadFile(f, buf, sizeof(buf), &n, NULL) && n) if (!CryptHashData(hash, buf, n, 0)) { ok = false; break; }
            BYTE md[16]; DWORD len = 16;
            if (ok && CryptGetHashParam(hash, HP_HASHVAL, md, &len, 0))
            {
                char hex[33];
                for (int i = 0; i < 16; ++i) sprintf_s(hex + 2 * i, 3, "%02x", md[i]);
                out = hex;
            }
        }
        if (hash) CryptDestroyHash(hash);
        if (prov) CryptReleaseContext(prov, 0);
        CloseHandle(f);
        return out;
    }

    bool fileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

    // Delay-loaded KenshiLib calls, isolated so a missing DLL / bad table never takes the game down.
    bool initKenshiLib(bool overrideVersion)
    {
        if (overrideVersion && !KenshiLib::OverrideKenshiVersion(KenshiLib::BinaryVersion(KenshiLib::BinaryVersion::STEAM, "1.0.65")))
            return false;
        return KenshiLib::Init();
    }
    bool safeInit(bool overrideVersion)
    {
        __try { return initKenshiLib(overrideVersion); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    typedef void (*StartPluginFn)();
    bool safeStart(StartPluginFn f)
    {
        __try { f(); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void start()
    {
        if (GetModuleHandleW(L"RE_Kenshi.dll"))
        {
            log("RE_Kenshi is loaded: it starts KenshiMP itself, nothing to do here.");
            return;
        }
        // Started once only: the loader can be reached twice (MyGUI plugin list of the mod's
        // gui folder and Plugins_x64.cfg, or a local copy and the Workshop copy).
        if (GetModuleHandleW(L"KenshiMP.dll"))
        {
            log("KenshiMP is already started (another copy of the loader), nothing to do here.");
            return;
        }
        std::wstring dir = moduleDir();
        std::string md5 = exeMd5();
        log("KenshiMP loader in %s, game executable md5 %s", narrow(dir).c_str(), md5.c_str());

        // Known builds. The stock Steam build is only supported through our own table.
        const char* STEAM_1068 = "8a03c256f0da1555d9cceb939b41530a";
        const char* STEAM_1065 = "df4a5a7ef8a29deb24b70e7b7f4a222a";
        const char* GOG_1065 = "213349bf76a0a758067f9ed0aef2ab01";
        bool ours = md5 == STEAM_1068;
        if (!ours && md5 != STEAM_1065 && md5 != GOG_1065)
        {
            log("This Kenshi build is not supported without RE_Kenshi (Steam 1.0.68 or 1.0.65 only). Install RE_Kenshi to play KenshiMP.");
            return;
        }
        std::wstring table = dir + L"\\rva\\RE_Kenshi\\RVAs\\Steam_1.0.65.br";
        if (ours && !fileExists(table)) { log("missing address table %s", narrow(table).c_str()); return; }
        if (!ours && !fileExists(L"RE_Kenshi\\RVAs\\Steam_1.0.65.br") && !fileExists(L"RE_Kenshi\\RVAs\\GOG_1.0.65.br"))
        { log("1.0.65 build without RE_Kenshi's address tables: install RE_Kenshi."); return; }

        // KenshiLib: ours (same version as the table), unless one is already loaded.
        if (!GetModuleHandleW(L"KenshiLib.dll"))
        {
            std::wstring lib = dir + L"\\KenshiLib.dll";
            if (!LoadLibraryExW(lib.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH))
            { log("cannot load %s (error %lu)", narrow(lib).c_str(), GetLastError()); return; }
        }

        wchar_t cwd[MAX_PATH] = { 0 };
        GetCurrentDirectoryW(MAX_PATH, cwd);
        if (ours) SetCurrentDirectoryW((dir + L"\\rva").c_str());
        bool ok = safeInit(ours);
        SetCurrentDirectoryW(cwd);
        if (!ok) { log("KenshiLib could not start (address table)."); return; }
        log("KenshiLib ready (%s table)", ours ? "KenshiMP's Steam 1.0.68" : "RE_Kenshi's 1.0.65");

        std::wstring mp = dir + L"\\KenshiMP.dll";
        HMODULE m = LoadLibraryExW(mp.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) { log("cannot load %s (error %lu)", narrow(mp).c_str(), GetLastError()); return; }
        StartPluginFn f = (StartPluginFn)GetProcAddress(m, "?startPlugin@@YAXXZ");
        if (!f) { log("KenshiMP.dll has no startPlugin"); return; }
        log(safeStart(f) ? "KenshiMP started" : "KenshiMP startPlugin failed");
    }
}

// Ogre plugin entry points (Plugins_x64.cfg: Plugin=<folder>/KenshiMP_Loader).
extern "C" __declspec(dllexport) void dllStartPlugin() { start(); }
extern "C" __declspec(dllexport) void dllStopPlugin() {}
