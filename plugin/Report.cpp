// Bug report (chat /report or /rapport, or the BUG REPORT button of the multiplayer window):
// everything needed to understand a problem, in one folder on the desktop that the player sends:
//   session.txt         what this PC sees right now (version, game, mods, options, players, ping,
//                       characters and where the other players are)
//   RE_Kenshi_log.txt   the game's log with the "KenshiMP:" lines (copied while the game writes it)
//   KenshiMP_loader.log the loader's log (players without RE_Kenshi)
//   kenshimp.cfg        the settings, password removed
//   players.cfg         host: player numbers kept between sessions
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
#include <shlobj.h>

#include "Shared.h"

#include <stdio.h>
#include <string.h>

using namespace mp;

namespace kmp {

namespace
{
    std::string desktopDir()
    {
        char p[MAX_PATH] = { 0 };
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, p)) && p[0]) return p;
        char home[MAX_PATH] = { 0 };
        size_t n = 0;
        if (getenv_s(&n, home, sizeof(home), "USERPROFILE") == 0 && n > 1) return std::string(home) + "\\Desktop";
        return std::string();
    }

    // Copies a file the game may still be writing (its log): opened with every sharing mode.
    bool copyShared(const std::string& from, const std::string& to)
    {
        HANDLE in = CreateFileA(from.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (in == INVALID_HANDLE_VALUE) return false;
        HANDLE out = CreateFileA(to.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return false; }
        char buf[65536];
        DWORD got = 0, put = 0;
        bool ok = true;
        while (ReadFile(in, buf, sizeof(buf), &got, NULL) && got > 0)
            if (!WriteFile(out, buf, got, &put, NULL) || put != got) { ok = false; break; }
        CloseHandle(in); CloseHandle(out);
        return ok;
    }

    // kenshimp.cfg without the session password.
    bool copySettings(const std::string& from, const std::string& to)
    {
        FILE* in = NULL;
        FILE* out = NULL;
        if (fopen_s(&in, from.c_str(), "r") != 0 || !in) return false;
        if (fopen_s(&out, to.c_str(), "w") != 0 || !out) { fclose(in); return false; }
        char line[1024];
        while (fgets(line, sizeof(line), in))
        {
            const char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (strncmp(p, "password", 8) == 0 && strchr(p, '=')) fputs("password=(removed from the report)\n", out);
            else fputs(line, out);
        }
        fclose(in); fclose(out);
        return true;
    }

    const char* yesNo(bool b) { return b ? "yes" : "no"; }

    void writeSummary(FILE* f, const std::string& when)
    {
        fprintf(f, "KenshiMP bug report - %s\n\n", when.c_str());
        fprintf(f, "KenshiMP version   : v%u\n", (unsigned)PROTOCOL_VERSION);
        fprintf(f, "Game and mods      : %s\n", mp_modList().c_str());
        fprintf(f, "RE_Kenshi          : %s\n", yesNo(GetModuleHandleA("RE_Kenshi.dll") != NULL));
        fprintf(f, "Interface language : %s\n", lang_french() ? "French" : "English");
        fprintf(f, "\n[settings]\n");
        fprintf(f, "mode=%s name=%s faction=%s address=%s port=%d password=%s\n", g_cfg.mode.c_str(), g_cfg.name.c_str(),
                g_cfg.faction.c_str(), g_cfg.address.c_str(), g_cfg.port, g_cfg.password.empty() ? "none" : "set");
        fprintf(f, "strict_mods=%d npc_sync=%d town_sync=%d load_sharing=%d player_names=%d render_smoothing=%d ghost_no_collide=%d debug_keys=%d\n",
                (int)g_cfg.strictMods, (int)g_cfg.npcSync, (int)g_cfg.townSync, (int)g_cfg.loadSharing, (int)g_cfg.playerNames,
                (int)g_cfg.renderSmoothing, (int)g_cfg.ghostNoCollide, (int)g_cfg.debugKeys);

        fprintf(f, "\n[session]\n");
        bool active = g_session.active();
        fprintf(f, "active=%s role=%s ready=%s my number=%d\n", yesNo(active), !active ? "-" : g_session.isHost() ? "host" : "client",
                yesNo(ready()), (int)g_session.localId());
        if (active)
        {
            NetStats st = g_session.stats();
            fprintf(f, "ping=%d ms, received %.1f MB, sent %.1f MB, states dropped %u\n", g_session.pingMs(),
                    st.bytesIn / 1048576.0, st.bytesOut / 1048576.0, (unsigned)st.statesDropped);
            if (!g_session.isHost()) fprintf(f, "own world (load sharing, far from the host): %s\n", yesNo(npcs_ownWorld()));
        }
        std::vector<PlayerInfo> ps = g_session.players();
        for (size_t i = 0; i < ps.size(); ++i)
            fprintf(f, "player %d: %s (faction %s)%s\n", (int)ps[i].id, ps[i].name.c_str(), ps[i].faction.c_str(),
                    ps[i].id == g_session.localId() ? "  <- this PC" : "");

        fprintf(f, "\n[world]\n");
        if (!mp_worldLoaded()) { fprintf(f, "no game loaded (title screen or loading)\n"); return; }
        lektor<Character*>& mine = ou->player->playerCharacters;
        fprintf(f, "our characters: %u, other characters shown here: %d\n", (unsigned)mine.size(), chars_ghostTotal());
        Ogre::Vector3 me = mine.size() ? mine[0]->getPosition() : Ogre::Vector3::ZERO;
        for (uint32_t i = 0; i < mine.size() && i < 12; ++i)
            if (Character* c = mine[i])
            {
                Ogre::Vector3 p = c->getPosition();
                fprintf(f, "  %s at (%.0f, %.0f, %.0f)\n", c->getName().c_str(), p.x, p.y, p.z);
            }
        for (size_t i = 0; i < ps.size(); ++i)
        {
            if (ps[i].id == g_session.localId()) continue;
            Ogre::Vector3 at;
            if (chars_playerPosition(ps[i].id, at))
                fprintf(f, "%s: a character at (%.0f, %.0f, %.0f), %.0f units from our first one\n", ps[i].name.c_str(), at.x, at.y, at.z, at.distance(me));
            else fprintf(f, "%s: position not received yet\n", ps[i].name.c_str());
        }
    }
}

std::string report_write()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[64], when[64];
    sprintf_s(stamp, "KenshiMP_report_%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    sprintf_s(when, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

    // The desktop, or next to kenshimp.cfg if it cannot be written.
    std::string base = desktopDir(), dir;
    if (!base.empty()) { dir = base + "\\" + stamp; if (!CreateDirectoryA(dir.c_str(), NULL)) dir.clear(); }
    if (dir.empty()) { dir = mp_configDir() + stamp; if (!CreateDirectoryA(dir.c_str(), NULL)) return T("Bug report: could not create the folder."); }

    log("bug report requested (%s)", stamp);   // so the copied log ends with this moment
    FILE* f = NULL;
    if (fopen_s(&f, (dir + "\\session.txt").c_str(), "w") == 0 && f)
    {
        try { writeSummary(f, when); } catch (...) { fprintf(f, "\n(summary interrupted)\n"); }
        fclose(f);
    }
    int files = 1;
    if (copyShared("RE_Kenshi_log.txt", dir + "\\RE_Kenshi_log.txt")) ++files;
    if (copyShared("KenshiMP_loader.log", dir + "\\KenshiMP_loader.log")) ++files;
    if (copySettings(mp_configDir() + "kenshimp.cfg", dir + "\\kenshimp.cfg")) ++files;
    if (copyShared(mp_configDir() + "players.cfg", dir + "\\players.cfg")) ++files;
    log("bug report written: %s (%d files)", dir.c_str(), files);
    return TF("Bug report saved (%d files): %s - send this folder (zip it) with a few words about what happened.", files, dir.c_str());
}

} // namespace kmp
