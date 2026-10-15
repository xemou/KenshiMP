// Dedicated server mode (DEDICATED_SERVER_PLAN.md): mode=server in kenshimp.cfg.
//
// The server is a Kenshi game with this plugin hosting the session without a player of its own: its squad is never
// sent to anybody, it simulates the world (NPCs, towns, weather...) around the connected players, and it adds what a
// public host needs:
//   - accounts (name + password, core/Accounts.h), bans, per-address and flood limits, more slots (core/Session.cpp);
//   - an admin console: commands typed in the server's console window (server_console=1), written into
//     server_commands.txt (one per line, the file is emptied once read), or sent by an admin player in the chat (!cmd)
//     or with /admin <cmd>; answers go to the log, the console and that admin;
//   - a record per account of the characters it brought (name, race, last position, inventory and its hash, money) in
//     <data>\players, used to check the client's bags (MSG_BAG_FINGERPRINT) and kept between restarts;
//   - periodic world saves (and a timestamped copy of the save folder), clean stop (save, then quit);
//   - headless options: window hidden or minimised, rendering switched off, frame rate cap;
//   - load counters every 30 s (players, KB/s per player, NPCs sent / skipped, frame time, CPU, memory);
//   - announces itself to a server list (server_list_url) every 30 s.
// Client side (any game joining a server): sends the fingerprint of its bags every 30 s and shows server notices.
#include <Debug.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
#include <kenshi/Inventory.h>
#include <kenshi/SaveManager.h>
#include <ogre/OgreRoot.h>
#include <ogre/Compositor/OgreCompositorManager2.h>
#include <ogre/Compositor/OgreCompositorWorkspace.h>

#include "Shared.h"
#include "../core/StateCodec.h"
#include "../core/ServerStore.h"
#include "../core/FileUtil.h"

#include <psapi.h>
#include <ctype.h>
#include <share.h>
#include <stdio.h>
#include <map>
#include <set>
#include <algorithm>

#pragma comment(lib, "psapi.lib")

using namespace mp;

namespace kmp {

namespace
{
    PlayerRecordStore g_records;
    struct Tracked { uint8_t owner; PlayerRecord::Char c; uint32_t money; bool hasMoney; Tracked() : owner(0), money(0), hasMoney(false) {} };
    std::map<uint32_t, Tracked> g_tracked;            // server: client characters seen (spawn, states, inventories)
    std::map<uint8_t, PlayerRecord> g_previous;       // server: what each connected account had last time
    std::map<uint8_t, int> g_mismatch;                // server: bag fingerprints that did not match, per player
    std::map<uint8_t, DWORD> g_joinedAt;
    DWORD g_lastRecords = 0, g_lastSave = 0, g_lastCounters = 0, g_lastAnnounce = 0, g_lastCommands = 0, g_lastPrint = 0;
    DWORD g_stopAt = 0, g_backupAt = 0, g_startedAt = 0;
    bool g_headlessDone = false;
    HANDLE g_consoleThread = NULL;
    CRITICAL_SECTION g_cmdLock;
    bool g_cmdLockInit = false;
    std::vector<std::string> g_consoleLines;          // typed in the console (console thread -> game thread)
    int g_npcSent = 0, g_npcSkipped = 0;              // counters fed by Npcs.cpp
    int g_bagChecks = 0, g_bagMismatches = 0;         // bag fingerprints compared / different (since start)
    std::map<std::string, uint32_t> g_recordHash;     // last record written per account (unchanged: not written again)
    double g_frameMsSum = 0; int g_frames = 0; double g_frameWorst = 0;

    std::string dataDir()
    {
        std::string d = g_cfg.serverDataDir.empty() ? mp_configDir() + "server" : g_cfg.serverDataDir;
        return d;
    }

    // ---- console window (server_console=1) ----
    DWORD WINAPI consoleThread(LPVOID)
    {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        char line[512];
        while (true)
        {
            DWORD n = 0;
            if (!ReadFile(in, line, sizeof(line) - 1, &n, NULL) || n == 0) { Sleep(200); continue; }
            line[n] = 0;
            std::string s(line);
            while (!s.empty() && (s[s.size() - 1] == '\n' || s[s.size() - 1] == '\r')) s.erase(s.size() - 1);
            if (s.empty()) continue;
            EnterCriticalSection(&g_cmdLock);
            g_consoleLines.push_back(s);
            LeaveCriticalSection(&g_cmdLock);
        }
        return 0;
    }
    void consolePrint(const std::string& s)
    {
        if (!g_consoleThread) return;
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        std::string l = s + "\r\n";
        DWORD w; WriteFile(out, l.data(), (DWORD)l.size(), &w, NULL);
    }

    // Answer to whoever typed the command (console/file: log + console; a player: a notice to it).
    void reply(int to, const std::string& text)
    {
        log("server: %s", text.c_str());
        consolePrint(text);
        if (to >= 0)
        {
            ByteWriter w; w.u8((uint8_t)to); w.str(text.substr(0, 1500));
            g_session.sendTo((uint8_t)to, MSG_SERVER_NOTICE, w.data);
        }
    }

    // ---- window / rendering ----
    struct FindWin { DWORD pid; HWND best; int area; };
    BOOL CALLBACK enumWin(HWND h, LPARAM p)
    {
        FindWin* f = (FindWin*)p;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid != f->pid || !IsWindowVisible(h)) return TRUE;
        RECT r; GetWindowRect(h, &r);
        int a = (r.right - r.left) * (r.bottom - r.top);
        if (a > f->area) { f->area = a; f->best = h; }
        return TRUE;
    }
    HWND gameWindow()
    {
        FindWin f; f.pid = GetCurrentProcessId(); f.best = NULL; f.area = 0;
        EnumWindows(enumWin, (LPARAM)&f);
        return f.best;
    }
    // Ogre 2 draws through compositor workspaces: a disabled workspace renders nothing (the frame loop, the AI and the
    // physics go on). The list is protected: reached through a derived type.
    struct WorkspacesOf : Ogre::CompositorManager2
    {
        static size_t count(Ogre::CompositorManager2* m) { return static_cast<WorkspacesOf*>(m)->mWorkspaces.size(); }
        static Ogre::CompositorWorkspace* at(Ogre::CompositorManager2* m, size_t i) { return static_cast<WorkspacesOf*>(m)->mWorkspaces[i]; }
    };
    int setRenderInner(bool on)
    {
        __try
        {
            Ogre::Root* root = Ogre::Root::getSingletonPtr();
            Ogre::CompositorManager2* cm = root ? root->getCompositorManager2() : NULL;
            if (!cm) return -1;
            size_t n = WorkspacesOf::count(cm);
            for (size_t i = 0; i < n; ++i)
                if (Ogre::CompositorWorkspace* w = WorkspacesOf::at(cm, i)) w->setEnabled(on);
            return (int)n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
    }
    void goHeadless()
    {
        if (g_headlessDone) return;
        g_headlessDone = true;
        if (g_cfg.serverWindow == "hide" || g_cfg.serverWindow == "minimize")
        {
            HWND w = gameWindow();
            if (w) ShowWindow(w, g_cfg.serverWindow == "hide" ? SW_HIDE : SW_MINIMIZE);
            log("server: game window %s (%p)", g_cfg.serverWindow == "hide" ? "hidden" : "minimised", (void*)w);
        }
        if (!g_cfg.serverRender)
        {
            int n = setRenderInner(false);
            log("server: rendering switched off (%d window(s))", n);
        }
    }

    // ---- process load ----
    double cpuPercent()
    {
        static ULONGLONG lastCpu = 0, lastWall = 0;
        FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        ULONGLONG cpu = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
        FILETIME now; GetSystemTimeAsFileTime(&now);
        ULONGLONG wall = (ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime;
        double pct = 0;
        if (lastWall && wall > lastWall) pct = 100.0 * (cpu - lastCpu) / (double)(wall - lastWall);   // 100 = one full core
        lastCpu = cpu; lastWall = wall;
        return pct;
    }
    double memoryMB()
    {
        PROCESS_MEMORY_COUNTERS pm; memset(&pm, 0, sizeof(pm));
        if (!GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm))) return 0;
        return pm.WorkingSetSize / (1024.0 * 1024.0);
    }

    // ---- saves ----
    bool saveInner(const std::string* name)
    {
        __try
        {
            SaveManager* sm = SaveManager::getSingleton();
            if (!sm) return false;
            sm->save(*name, false);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // The game's CONTINUE loads the save named in settings.cfg ("continue="): pointed at the server's world, so a restart
    // (crash, stop, run_server.ps1) continues the same world.
    void pointContinueAt(const std::string& save)
    {
        std::string text;
        if (!readFile("settings.cfg", text)) return;
        std::vector<std::string> lines = splitLines(text);
        bool found = false;
        std::string out;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (lines[i].compare(0, 9, "continue=") == 0) { if (lines[i] == "continue=" + save) return; out += "continue=" + save + "\r\n"; found = true; }
            else out += lines[i] + "\r\n";
        }
        if (!found) out += "continue=" + save + "\r\n";
        if (writeFileAtomic("settings.cfg", out)) log("server: the game's CONTINUE now loads '%s' (settings.cfg)", save.c_str());
    }

    bool saveWorld()
    {
        std::string name = g_cfg.serverSave.empty() ? std::string("kenshimp_server") : g_cfg.serverSave;
        bool ok = saveInner(&name);
        log("server: world saved as '%s' -> %s", name.c_str(), ok ? "ok" : "FAILED");
        g_lastSave = GetTickCount();
        if (ok) { g_backupAt = GetTickCount() + 60000; pointContinueAt(name); }   // a timestamped copy once the game has written it
        return ok;
    }

    // ---- player records ----
    std::string accountOf(uint8_t id)
    {
        std::vector<PlayerInfo> ps = g_session.players();
        for (size_t i = 0; i < ps.size(); ++i) if (ps[i].id == id) return ps[i].name;
        return std::string();
    }
    void saveRecord(uint8_t owner)
    {
        std::string name = accountOf(owner);
        if (name.empty()) return;
        PlayerRecord r; r.name = name;
        for (std::map<uint32_t, Tracked>::iterator it = g_tracked.begin(); it != g_tracked.end(); ++it)
        {
            if (it->second.owner != owner) continue;
            r.chars.push_back(it->second.c);
            if (it->second.hasMoney) r.money = it->second.money;
        }
        if (r.chars.empty()) return;
        // Same content as last time (lastSeen aside): no new file, so the kept versions (.1 .. .3) stay real earlier states.
        ByteWriter w; PlayerRecord h = r; h.lastSeen = 0; h.write(w);
        uint32_t hash = hashBytes(w.data);
        if (g_recordHash.count(name) && g_recordHash[name] == hash) return;
        g_recordHash[name] = hash;
        g_records.save(r);
    }
    void saveAllRecords()
    {
        std::vector<PlayerInfo> ps = g_session.players();
        for (size_t i = 0; i < ps.size(); ++i) if (ps[i].id != HOST_ID) saveRecord(ps[i].id);
    }
    // Same items list as the owner's MSG_INVENTORY body (u8 kind, u32 id, u16 n, InvItem*, [u8 1, u32 money]).
    bool parseInventory(const Bytes& body, uint8_t& kind, uint32_t& id, std::vector<InvItem>& items, bool& hasMoney, uint32_t& money)
    {
        ByteReader r(body);
        kind = r.u8(); id = r.u32(); uint16_t n = r.u16();
        for (int i = 0; i < n && i < MAX_INV_ITEMS && r.ok(); ++i) { InvItem it; it.read(r); if (r.ok()) items.push_back(it); }
        hasMoney = false; money = 0;
        if (r.ok() && r.remaining() >= 5 && r.u8() == 1) { money = r.u32(); hasMoney = r.ok(); }
        return r.ok();
    }

    // ---- server list ----
    DWORD WINAPI announceThread(LPVOID p)
    {
        std::string* url = (std::string*)p;
        std::string body, err;
        if (!Session::httpGet(*url, body, err, 5000)) log("server: announce to the server list failed (%s)", err.c_str());
        delete url;
        return 0;
    }
    std::string urlEncode(const std::string& s)
    {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i)
        {
            unsigned char c = (unsigned char)s[i];
            if (isalnum(c) || c == '-' || c == '_' || c == '.') o += (char)c;
            else { char h[4]; sprintf_s(h, "%%%02X", c); o += h; }
        }
        return o;
    }
    void announce()
    {
        std::string base = g_cfg.serverListUrl;
        if (base.compare(0, 7, "http://") != 0) return;
        if (base[base.size() - 1] != '/') base += "/";
        char q[1024];
        int players = (int)g_session.players().size() - 1;
        sprintf_s(q, "announce?name=%s&port=%d&transport=%s&players=%d&max=%d&version=%u&address=%s",
                  urlEncode(g_cfg.serverName).c_str(), g_cfg.port, g_cfg.transport.c_str(), players < 0 ? 0 : players, g_cfg.maxPlayers,
                  (unsigned)PROTOCOL_VERSION, urlEncode(g_cfg.serverPublicAddress).c_str());
        std::string* a = new std::string(base + q);
        HANDLE h = CreateThread(NULL, 0, announceThread, a, 0, NULL);
        if (h) CloseHandle(h); else delete a;
    }

    // ---- admin commands ----
    int findPlayerId(const std::string& name)
    {
        std::vector<PlayerInfo> ps = g_session.players();
        for (size_t i = 0; i < ps.size(); ++i) if (ps[i].id != HOST_ID && ps[i].name == name) return ps[i].id;
        for (size_t i = 0; i < ps.size(); ++i)   // unique prefix
            if (ps[i].id != HOST_ID && ps[i].name.compare(0, name.size(), name) == 0) return ps[i].id;
        return -1;
    }

    std::string statusLine()
    {
        char buf[512];
        double fps = g_frames ? 1000.0 * g_frames / (g_frameMsSum > 0 ? g_frameMsSum : 1) : 0;
        DWORD up = (GetTickCount() - g_startedAt) / 1000;
        sprintf_s(buf, "%d player(s), %s, %.0f fps (worst frame %.0f ms), CPU %.0f%% of a core, %.0f MB, up %luh%02lum, %d NPC state(s) sent / %d skipped (budget) in the last period, %d account(s), bags checked %d (%d different)",
                  (int)g_session.players().size() - 1, g_cfg.transport.c_str(), fps, g_frameWorst, cpuPercent(), memoryMB(),
                  (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60), g_npcSent, g_npcSkipped, (int)g_session.accountCount(), g_bagChecks, g_bagMismatches);
        return buf;
    }

    void runCommand(const std::string& line, int from)
    {
        std::vector<std::string> w = splitWords(line);
        if (w.empty()) return;
        std::string cmd = w[0];
        if (!cmd.empty() && (cmd[0] == '!' || cmd[0] == '/')) cmd = cmd.substr(1);
        for (size_t i = 0; i < cmd.size(); ++i) cmd[i] = (char)tolower(cmd[i]);
        std::string arg = line.size() > w[0].size() ? line.substr(line.find(w[0]) + w[0].size()) : std::string();
        while (!arg.empty() && arg[0] == ' ') arg.erase(0, 1);
        std::string first = w.size() > 1 ? w[1] : std::string();
        std::string rest;
        if (w.size() > 2) { size_t p = arg.find(first); rest = arg.substr(p + first.size()); while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1); }
        log("server: command '%s' from %s", line.c_str(), from < 0 ? "the console" : playerName((uint8_t)from).c_str());

        if (cmd == "help")
            reply(from, "commands: players, status, say <text>, kick <player> [reason], ban <player|ip>, unban <player|ip>, bans, save, stop, "
                        "resetpassword <account>, record <account>, give <player> <item id> [quantity], resetworld, admin <account> (toggle)");
        else if (cmd == "players")
        {
            std::vector<PeerStats> ps = g_session.peerStats();
            if (ps.empty()) { reply(from, "no player connected"); return; }
            for (size_t i = 0; i < ps.size(); ++i)
            {
                char b[256];
                sprintf_s(b, "#%d %s (%s)%s in %.0f KB out %.0f KB, rtt %u ms, %u state(s) skipped, %d bag mismatch(es)", (int)ps[i].id, ps[i].name.c_str(),
                          ps[i].address.c_str(), ps[i].admin ? " admin," : "", ps[i].bytesIn / 1024.0, ps[i].bytesOut / 1024.0, ps[i].rttMs, ps[i].statesDropped,
                          g_mismatch.count(ps[i].id) ? g_mismatch[ps[i].id] : 0);
                reply(from, b);
            }
        }
        else if (cmd == "status") reply(from, statusLine());
        else if (cmd == "say")
        {
            if (arg.empty()) { reply(from, "say <text>"); return; }
            ByteWriter n; n.u8(BROADCAST); n.str(arg.substr(0, 400));
            g_session.send(MSG_SERVER_NOTICE, n.data);
            reply(from, "said: " + arg);
        }
        else if (cmd == "kick")
        {
            int id = findPlayerId(first);
            if (id < 0) { reply(from, "no player '" + first + "'"); return; }
            g_session.kick((uint8_t)id, rest.empty() ? "kicked by an admin" : "kicked: " + rest);
            reply(from, "kicked " + first);
        }
        else if (cmd == "ban") { if (first.empty()) reply(from, "ban <player|ip>"); else { g_session.ban(first); reply(from, "banned " + first); } }
        else if (cmd == "unban") reply(from, g_session.unban(first) ? "unbanned " + first : first + " was not banned");
        else if (cmd == "bans") reply(from, "bans: " + g_session.bans());
        else if (cmd == "save") reply(from, saveWorld() ? "world saved" : "save FAILED");
        else if (cmd == "stop")
        {
            ByteWriter n; n.u8(BROADCAST); n.str("The server is stopping (saving the world)...");
            g_session.send(MSG_SERVER_NOTICE, n.data);
            saveAllRecords();
            saveWorld();
            g_stopAt = GetTickCount() + 15000;   // the game writes its save meanwhile
            reply(from, "stopping in 15 s");
        }
        else if (cmd == "resetpassword") reply(from, g_session.resetAccount(first) ? "account '" + first + "' removed: its next login sets a new password" : "no account '" + first + "'");
        else if (cmd == "admin")
        {
            reply(from, "admins are listed in kenshimp.cfg (server_admins=name1,name2); restart the server after a change");
        }
        else if (cmd == "record")
        {
            PlayerRecord r;
            if (!g_records.load(arg, r)) { reply(from, "no record for '" + arg + "'"); return; }
            char b[200]; sprintf_s(b, "%s: %d character(s), money %u, last seen %u", r.name.c_str(), (int)r.chars.size(), r.money, r.lastSeen);
            reply(from, b);
            for (size_t i = 0; i < r.chars.size(); ++i)
            {
                sprintf_s(b, "  %s (%s) at (%.0f, %.0f, %.0f), %d item stack(s)", r.chars[i].name.c_str(), r.chars[i].race.c_str(), r.chars[i].x, r.chars[i].y, r.chars[i].z, (int)r.chars[i].items.size());
                reply(from, b);
            }
        }
        else if (cmd == "give")
        {
            int id = findPlayerId(first);
            std::vector<std::string> ww = splitWords(rest);
            if (id < 0 || ww.empty()) { reply(from, "give <player> <item id> [quantity]"); return; }
            uint32_t target = 0;
            for (std::map<uint32_t, Tracked>::iterator it = g_tracked.begin(); it != g_tracked.end() && !target; ++it)
                if (it->second.owner == id) target = it->first;
            if (!target) { reply(from, "no character of that player known yet"); return; }
            InvItem item; item.item = ww[0]; item.quantity = ww.size() > 1 ? atoi(ww[1].c_str()) : 1;
            if (!item.sanitize()) { reply(from, "bad item"); return; }
            ByteWriter g; g.u8((uint8_t)id); g.u8(CONTAINER_CHARACTER); g.u32(target); item.write(g);
            g_session.sendTo((uint8_t)id, MSG_ITEM_GIVE, g.data);
            reply(from, "gave " + item.item + " to " + first);
        }
        else if (cmd == "resetworld")
        {
            if (g_cfg.serverStartSave.empty()) { reply(from, "set server_start_save=<save name> (a copy of the starting world) first"); return; }
            ByteWriter n; n.u8(BROADCAST); n.str("The world is being reset; your characters and money stay yours.");
            g_session.send(MSG_SERVER_NOTICE, n.data);
            saveAllRecords();
            reply(from, server_loadSave(g_cfg.serverStartSave) ? "loading the starting world '" + g_cfg.serverStartSave + "'" : "load FAILED");
        }
        else reply(from, "unknown command '" + cmd + "' (help)");
    }

    void readCommandFile()
    {
        std::string path = dataDir() + "\\server_commands.txt";
        std::string text;
        if (!readFile(path, text) || text.empty()) return;
        writeFileAtomic(path, std::string());
        std::vector<std::string> lines = splitLines(text);
        for (size_t i = 0; i < lines.size(); ++i) if (!lines[i].empty() && lines[i][0] != '#') runCommand(lines[i], -1);
    }
}

bool server_isServer() { return g_cfg.mode == "server"; }

// Session settings for this game: transport of both sides, the server's options.
void server_configureSession()
{
    g_session.setTransport(g_cfg.transport == "udp" ? TRANSPORT_UDP : TRANSPORT_TCP);
    g_session.setAccountPassword(g_cfg.accountPassword.empty() ? g_cfg.password : g_cfg.accountPassword);
    ServerOptions o;
    if (server_isServer())
    {
        o.dedicated = true;
        o.accounts = g_cfg.serverAccounts;
        o.dataDir = dataDir();
        o.name = g_cfg.serverName;
        o.description = g_cfg.serverDescription;
        o.maxPlayers = g_cfg.maxPlayers;
        o.maxPerAddress = g_cfg.serverMaxPerAddress;
        o.maxMessage = 1u << 20;
        o.maxBytesPerSec = 2u * 1024 * 1024;
        o.checkMoves = true;
        o.kickAfterRefusals = g_cfg.serverKickRefusals;
        std::vector<std::string> a = splitWords(g_cfg.serverAdmins);
        for (size_t i = 0; i < a.size(); ++i) o.admins.push_back(a[i]);
        makeDirs(o.dataDir);
        g_records.setDir(o.dataDir);
        g_startedAt = GetTickCount();
        if (g_cfg.serverConsole && !g_consoleThread)
        {
            if (!g_cmdLockInit) { InitializeCriticalSection(&g_cmdLock); g_cmdLockInit = true; }
            if (AllocConsole())
            {
                SetConsoleTitleA(("KenshiMP server - " + g_cfg.serverName).c_str());
                g_consoleThread = CreateThread(NULL, 0, consoleThread, NULL, 0, NULL);
                consolePrint("KenshiMP dedicated server. Type 'help' for the commands.");
            }
        }
        log("server: '%s' on %s port %d, %d slot(s), accounts %s, data in %s", g_cfg.serverName.c_str(), g_cfg.transport.c_str(), g_cfg.port,
            g_cfg.maxPlayers, g_cfg.serverAccounts ? "on" : "off", o.dataDir.c_str());
    }
    else o.maxPlayers = HOST_SLOTS - 1;
    g_session.setServer(o);
}

// Every frame (before the game's own update): frame cap and frame statistics.
void server_frame()
{
    if (!server_isServer()) return;
    static LARGE_INTEGER freq = { 0 }, last = { 0 };
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (last.QuadPart)
    {
        double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
        if (g_cfg.serverFps > 0)
        {
            double target = 1000.0 / g_cfg.serverFps;
            if (ms < target) { Sleep((DWORD)(target - ms)); QueryPerformanceCounter(&now); ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart; }
        }
        g_frameMsSum += ms; ++g_frames; if (ms > g_frameWorst) g_frameWorst = ms;
    }
    last = now;
}

void server_tick(DWORD now)
{
    if (!server_isServer()) return;
    if (mp_worldLoaded()) goHeadless();
    static DWORD lastRender = 0;
    if (g_headlessDone && !g_cfg.serverRender && now - lastRender > 5000) { lastRender = now; setRenderInner(false); }
    if (g_stopAt && (int)(now - g_stopAt) >= 0)
    {
        log("server: stopped by an admin");
        writeFileAtomic(dataDir() + "\\server_stopped.flag", "stopped by an admin\n");   // tools\server\run_server.ps1: do not restart
        g_session.stop();
        ExitProcess(0);
    }
    if (now - g_lastCommands >= 1000)
    {
        g_lastCommands = now;
        readCommandFile();
        std::vector<std::string> lines;
        if (g_cmdLockInit) { EnterCriticalSection(&g_cmdLock); lines.swap(g_consoleLines); LeaveCriticalSection(&g_cmdLock); }
        for (size_t i = 0; i < lines.size(); ++i) runCommand(lines[i], -1);
    }
    if (g_backupAt && (int)(now - g_backupAt) >= 0) { g_backupAt = 0; backup_serverSave(); }
    if (!mp_worldLoaded()) return;
    if (!g_lastSave) g_lastSave = now;
    if (g_cfg.serverAutosaveMin > 0 && now - g_lastSave >= (DWORD)g_cfg.serverAutosaveMin * 60000) saveWorld();
    if (now - g_lastRecords >= 60000) { g_lastRecords = now; saveAllRecords(); }
    if (!g_cfg.serverListUrl.empty() && (!g_lastAnnounce || now - g_lastAnnounce >= 30000)) { g_lastAnnounce = now; announce(); }
    if (!g_lastCounters) g_lastCounters = now;
    if (now - g_lastCounters >= 30000)
    {
        double secs = (now - g_lastCounters) / 1000.0;
        g_lastCounters = now;
        log("server load: %s", statusLine().c_str());
        static std::map<uint8_t, std::pair<uint64_t, uint64_t> > prev;
        std::vector<PeerStats> ps = g_session.peerStats();
        for (size_t i = 0; i < ps.size(); ++i)
        {
            std::pair<uint64_t, uint64_t>& p = prev[ps[i].id];
            double inK = (ps[i].bytesIn - (ps[i].bytesIn >= p.first ? p.first : 0)) / 1024.0 / secs;
            double outK = (ps[i].bytesOut - (ps[i].bytesOut >= p.second ? p.second : 0)) / 1024.0 / secs;
            log("   player %d %s: in %.1f KB/s, out %.1f KB/s, rtt %u ms, %u state(s) skipped", (int)ps[i].id, ps[i].name.c_str(), inK, outK, ps[i].rttMs, ps[i].statesDropped);
            p.first = ps[i].bytesIn; p.second = ps[i].bytesOut;
        }
        g_npcSent = g_npcSkipped = 0;
        g_frameMsSum = 0; g_frames = 0; g_frameWorst = 0;
    }
}

std::map<uint8_t, int> g_refusals;
void server_refused(uint8_t player, const char* why)
{
    int n = ++g_refusals[player];
    static std::map<uint8_t, DWORD> lastLog;
    DWORD now = GetTickCount();
    if (!lastLog[player] || now - lastLog[player] > 10000 || n <= 3)
    {
        lastLog[player] = now;
        log("server: refused from %s: %s (%d so far)", playerName(player).c_str(), why, n);
    }
    if (g_cfg.serverKickRefusals > 0 && n >= g_cfg.serverKickRefusals)
    {
        g_session.kick(player, "too many refused actions (cheat or broken game?)");
        g_refusals.erase(player);
    }
}

// Npcs.cpp: what the per-client budget let through.
void server_countNpcStates(int sent, int skipped) { g_npcSent += sent; g_npcSkipped += skipped; }

// Server: watch what clients send about their own characters (records, bag checks), and admin commands.
// Returns true when the message was for the server only (no further handling).
bool server_onMessage(const NetEvent& e)
{
    if (!server_isServer())
    {
        if (e.msgType == MSG_SERVER_NOTICE)
        {
            ByteReader r(e.body); r.u8(); std::string text = r.str();
            if (r.ok() && !text.empty()) { showMessage("[server] " + text); chat_notice("[server] " + text); log("server notice: %s", text.c_str()); }
            return true;
        }
        return false;
    }
    if (e.sender == HOST_ID) return false;
    ByteReader r(e.body);
    switch (e.msgType)
    {
    case MSG_ADMIN:
    {
        r.u8(); std::string line = r.str();
        if (!r.ok()) return true;
        if (!g_session.isAdmin(e.sender)) { reply(e.sender, "you are not an admin of this server"); log("server: refused admin command from %s", playerName(e.sender).c_str()); return true; }
        runCommand(line, e.sender);
        return true;
    }
    case MSG_CHAT:
    {
        std::string text = r.str();
        if (r.ok() && text.size() > 1 && text[0] == '!' && g_session.isAdmin(e.sender)) { runCommand(text, e.sender); return true; }
        return false;
    }
    case MSG_ENTITY_SPAWN:
    {
        EntitySpawn s; s.read(r);
        if (!r.ok() || netIdOwner(s.netId) != e.sender || s.kind != KIND_CHARACTER) return false;
        Tracked& t = g_tracked[s.netId];
        t.owner = e.sender; t.c.netId = s.netId; t.c.name = s.displayName; t.c.race = s.gameDataName; t.c.x = s.x; t.c.y = s.y; t.c.z = s.z;
        return false;
    }
    case MSG_ENTITY_DESPAWN:
    {
        uint32_t id = r.u32();
        if (r.ok() && netIdOwner(id) == e.sender) g_tracked.erase(id);
        return false;
    }
    case MSG_ENTITY_STATE:
    {
        r.u32();
        std::vector<EntityState> st; StateCodec::readAll(r, st);
        for (size_t i = 0; i < st.size(); ++i)
        {
            std::map<uint32_t, Tracked>::iterator it = g_tracked.find(st[i].netId);
            if (it != g_tracked.end() && it->second.owner == e.sender && validPos(st[i].x, st[i].y, st[i].z)) { it->second.c.x = st[i].x; it->second.c.y = st[i].y; it->second.c.z = st[i].z; }
        }
        return false;
    }
    case MSG_INVENTORY:
    {
        uint8_t kind; uint32_t id; std::vector<InvItem> items; bool hasMoney; uint32_t money;
        if (!parseInventory(e.body, kind, id, items, hasMoney, money) || kind != CONTAINER_CHARACTER || netIdOwner(id) != e.sender) return false;
        Tracked& t = g_tracked[id];
        t.owner = e.sender; t.c.netId = id; t.c.items = items; t.c.bagHash = bagHash(items);
        if (hasMoney) { t.money = money; t.hasMoney = true; }
        return false;
    }
    case MSG_BAG_FINGERPRINT:
    {
        r.u8(); uint16_t n = r.u16();
        for (int i = 0; i < n && r.ok(); ++i)
        {
            uint32_t id = r.u32(), h = r.u32();
            if (!r.ok() || netIdOwner(id) != e.sender) continue;
            std::map<uint32_t, Tracked>::iterator it = g_tracked.find(id);
            if (it == g_tracked.end() || !it->second.c.bagHash) continue;   // its inventory has not arrived yet
            ++g_bagChecks;
            if (it->second.c.bagHash == h) continue;
            ++g_bagMismatches;
            int& m = g_mismatch[e.sender];
            ++m;
            log("server: bag of %08x (%s, %s) differs from the server's copy (%08x vs %08x), asking for it again (%d so far)",
                id, it->second.c.name.c_str(), playerName(e.sender).c_str(), h, it->second.c.bagHash, m);
            ByteWriter q; q.u8(e.sender); q.u8(CONTAINER_CHARACTER); q.u32(id);
            g_session.sendTo(e.sender, MSG_INVENTORY_REQUEST, q.data);
        }
        return true;
    }
    default:
        return false;
    }
}

void server_onPlayerJoined(uint8_t id, const std::string& name)
{
    if (!server_isServer()) return;
    g_joinedAt[id] = GetTickCount();
    g_mismatch.erase(id);
    PlayerRecord r;
    if (g_records.load(name, r))
    {
        g_previous[id] = r;
        log("server: %s is back (%d character(s) last time, money %u)", name.c_str(), (int)r.chars.size(), r.money);
    }
    else log("server: %s joins for the first time", name.c_str());
    std::string addr = g_session.address(id);
    log("server: player %d '%s' from %s%s", (int)id, name.c_str(), addr.c_str(), g_session.isAdmin(id) ? " (admin)" : "");
    if (!g_cfg.serverMotd.empty())
    {
        ByteWriter w; w.u8(id); w.str(g_cfg.serverMotd);
        g_session.sendTo(id, MSG_SERVER_NOTICE, w.data);
    }
}

// Their record is written before forgetting them; what came back compared with last time is logged.
void server_onPlayerLeft(uint8_t id)
{
    if (!server_isServer()) return;
    saveRecord(id);
    std::map<uint8_t, PlayerRecord>::iterator prev = g_previous.find(id);
    if (prev != g_previous.end())
    {
        int same = 0;
        for (size_t i = 0; i < prev->second.chars.size(); ++i)
            for (std::map<uint32_t, Tracked>::iterator it = g_tracked.begin(); it != g_tracked.end(); ++it)
                if (it->second.owner == id && it->second.c.name == prev->second.chars[i].name) ++same;
        log("server: %s left; %d of its %d previous character(s) seen again this time", prev->second.name.c_str(), same, (int)prev->second.chars.size());
        g_previous.erase(prev);
    }
    for (std::map<uint32_t, Tracked>::iterator it = g_tracked.begin(); it != g_tracked.end();)
        if (it->second.owner == id) g_tracked.erase(it++); else ++it;
    g_joinedAt.erase(id);
}

// Client of a dedicated server: the fingerprint of our bags every 30 s (same items as our MSG_INVENTORY).
void server_clientTick(DWORD now)
{
    if (server_isServer() || !g_session.hostIsServer() || !ready()) return;
    static DWORD last = 0;
    if (now - last < 30000) return;
    last = now;
    std::vector<std::pair<uint32_t, Character*> > mine;
    chars_localCharacters(mine);
    ByteWriter w; w.u8(HOST_ID);
    ByteWriter list; uint16_t n = 0;
    for (size_t i = 0; i < mine.size(); ++i)
    {
        Character* c = mine[i].second;
        if (!c || !c->inventory) continue;
        Bytes body = items_inventoryMsg(CONTAINER_CHARACTER, mine[i].first, c->inventory, items_moneyOf(c));
        uint8_t kind; uint32_t id; std::vector<InvItem> items; bool hasMoney; uint32_t money;
        if (body.empty() || !parseInventory(body, kind, id, items, hasMoney, money)) continue;
        list.u32(mine[i].first); list.u32(bagHash(items)); ++n;
    }
    if (!n) return;
    w.u16(n); w.bytes(list.data);
    g_session.send(MSG_BAG_FINGERPRINT, w.data);
}

// Server log: <data>\server.log, renamed to server.log.1 above log_max_mb (so two files at most).
namespace { struct LogLock { CRITICAL_SECTION c; LogLock() { InitializeCriticalSection(&c); } } g_logLock; FILE* g_logFile = NULL; long g_logSize = 0; bool g_logFailed = false; }
void server_logLine(const char* line)
{
    if (g_logFailed) return;
    EnterCriticalSection(&g_logLock.c);
    std::string path = dataDir() + "\\server.log";
    if (!g_logFile)
    {
        makeDirs(dataDir());
        g_logFile = _fsopen(path.c_str(), "a", _SH_DENYNO);   // readable (tail) while the server runs
        if (!g_logFile) { g_logFailed = true; LeaveCriticalSection(&g_logLock.c); return; }
        fseek(g_logFile, 0, SEEK_END);
        g_logSize = ftell(g_logFile);
    }
    SYSTEMTIME t; GetLocalTime(&t);
    int n = fprintf(g_logFile, "%04d-%02d-%02d %02d:%02d:%02d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, line);
    fflush(g_logFile);
    if (n > 0) g_logSize += n;
    long limit = (long)(g_cfg.logMaxMB > 0 ? g_cfg.logMaxMB : 8) * 1024 * 1024;
    if (g_logSize > limit)
    {
        fclose(g_logFile); g_logFile = NULL;
        MoveFileExA(path.c_str(), (path + ".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    LeaveCriticalSection(&g_logLock.c);
}

} // namespace kmp
