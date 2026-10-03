// Host save backup: the host's save holds the shared world, so a multiplayer bug could damage it.
// Each time a hosted game is ready (loaded, players resynced), its save folder is copied once to
// %LOCALAPPDATA%\kenshi\KenshiMP\backups\<save>_<date>\ (backup_saves=N keeps the last N copies of
// each save, 0 = off). The copy runs on its own thread (saves are tens of MB): the game never waits.
// Only fields of the engine's SaveManager are read (no extra engine function, so nothing to add to
// the address tables); the folder is looked up in both places Kenshi keeps saves.
#include <kenshi/SaveManager.h>

#include "Shared.h"

#include <algorithm>
#include <stdio.h>
#include <string>
#include <vector>

namespace kmp {

namespace
{
    volatile LONG g_running = 0;     // a copy is in progress
    volatile LONG g_finished = 0;    // 1: done (result below), read and cleared by backup_tick
    std::string g_result;            // written by the copy thread before g_finished = 1
    std::string g_lastBackedUp;      // save already copied during this game session

    bool isDir(const std::string& p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
    std::string withSlash(const std::string& p) { return p.empty() || p[p.size() - 1] == '\\' || p[p.size() - 1] == '/' ? p : p + "\\"; }

    // Recursive copy; returns the number of files copied (-1 on a failure).
    int copyTree(const std::string& from, const std::string& to)
    {
        if (!CreateDirectoryA(to.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return -1;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((from + "\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        int n = 0;
        do
        {
            std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            std::string s = from + "\\" + name, d = to + "\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                int k = copyTree(s, d);
                if (k < 0) { n = -1; break; }
                n += k;
            }
            else if (CopyFileA(s.c_str(), d.c_str(), FALSE)) ++n;
            else { n = -1; break; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return n;
    }
    void deleteTree(const std::string& dir)
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                std::string name = fd.cFileName;
                if (name == "." || name == "..") continue;
                std::string p = dir + "\\" + name;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) deleteTree(p);
                else { SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileA(p.c_str()); }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryA(dir.c_str());
    }
    // Only the newest `keep` backups of that save stay (names end with a sortable date).
    void prune(const std::string& root, const std::string& save, int keep)
    {
        std::vector<std::string> mine;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((root + save + "_*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            std::string name = fd.cFileName;
            // "<save>_YYYY-MM-DD_HH-MM-SS" exactly (another save may start with the same name)
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name.size() == save.size() + 20) mine.push_back(name);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        std::sort(mine.begin(), mine.end());
        for (size_t i = 0; i + keep < mine.size(); ++i) deleteTree(root + mine[i]);
    }

    struct Job { std::string from, root, save, stamp; int keep; };
    DWORD WINAPI copyThread(LPVOID p)
    {
        Job* j = (Job*)p;
        std::string to = j->root + j->save + "_" + j->stamp;
        int n = copyTree(j->from, to);
        char buf[600];
        if (n < 0) { deleteTree(to); sprintf_s(buf, "save backup of '%s' failed (copy error), nothing kept", j->save.c_str()); }
        else
        {
            prune(j->root, j->save, j->keep);
            sprintf_s(buf, "save '%s' backed up: %d file(s) in %s", j->save.c_str(), n, to.c_str());
        }
        g_result = buf;
        delete j;
        InterlockedExchange(&g_finished, 1);
        InterlockedExchange(&g_running, 0);
        return 0;
    }

    // (SEH and C++ exceptions cannot share a function.)
    SaveManager* safeSaveManager()
    {
        __try { return SaveManager::getSingleton(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    bool saveInfo(std::string* game, std::string* userPath, std::string* localPath)
    {
        SaveManager* sm = safeSaveManager();
        if (!sm) return false;
        try { *game = sm->currentGame; *userPath = sm->userSavePath; *localPath = sm->localSavePath; return true; }
        catch (...) { return false; }
    }
}

// Host: the game is ready (called once per load). Starts the copy of its save folder.
void backup_hostSave()
{
    if (g_cfg.backupSaves <= 0 || !g_session.isHost() || g_running) return;
    std::string game, userPath, localPath;
    if (!saveInfo(&game, &userPath, &localPath) || game.empty()) { log("save backup: no save name yet (new game not saved)"); return; }
    if (game == g_lastBackedUp) return;
    std::string from;
    const std::string bases[2] = { userPath, localPath };
    for (int i = 0; i < 2 && from.empty(); ++i)
        if (!bases[i].empty() && isDir(withSlash(bases[i]) + game)) from = withSlash(bases[i]) + game;
    if (from.empty()) { log("save backup: folder of '%s' not found (%s | %s)", game.c_str(), userPath.c_str(), localPath.c_str()); return; }

    std::string root = mp_configDir() + "backups";
    CreateDirectoryA(root.c_str(), NULL);
    SYSTEMTIME t; GetLocalTime(&t);
    char stamp[32];
    sprintf_s(stamp, "%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    Job* j = new Job;
    j->from = from; j->root = withSlash(root); j->save = game; j->stamp = stamp; j->keep = g_cfg.backupSaves;
    g_lastBackedUp = game;
    InterlockedExchange(&g_running, 1);
    HANDLE h = CreateThread(NULL, 0, copyThread, j, 0, NULL);
    if (!h) { InterlockedExchange(&g_running, 0); delete j; log("save backup: could not start the copy"); return; }
    CloseHandle(h);
    log("save backup of '%s' started (%s)", game.c_str(), from.c_str());
}

// Main thread: reports a finished copy.
void backup_tick()
{
    if (!g_finished) return;
    InterlockedExchange(&g_finished, 0);
    log("%s", g_result.c_str());
    if (g_result.find("failed") == std::string::npos) chat_notice(T("Your save was backed up (KenshiMP\\backups)."));
}

} // namespace kmp
