// KenshiMP - RE_Kenshi plugin entry point: config, session, player/faction management, main loop.
//
// Each player simulates his own squad, faction and buildings. Remote squads and buildings are
// mirrored locally as "ghosts" owned by one local faction per remote player (Characters.cpp,
// Buildings.cpp). Game speed is locked to x1.
//
// Hotkeys: F9 = declare war on every other player, F10 = make peace.
#include <Debug.h>
#include <kenshi/GameWorld.h>
#include <kenshi/SaveManager.h>
#include <kenshi/gui/TitleScreen.h>
#include <kenshi/Globals.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
#include <kenshi/Faction.h>
#include <kenshi/FactionRelations.h>
#include <kenshi/GameData.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/Enums.h>
#include <kenshi/ModInfo.h>
#include <kenshi/Kenshi.h>
#include <core/Functions.h>

#include "Shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <map>
#include <set>

using namespace mp;

namespace kmp {

Session g_session;
Config  g_cfg;
CRITICAL_SECTION g_lock;

namespace
{
    const float WAR = -100.f, PEACE = 0.f;

    bool g_started = false;
    bool g_ready = false;
    const DWORD REGROUP_HINT_DELAY_MS = 20000;   // the host's positions have arrived by then
    const float REGROUP_HINT_DISTANCE = 1500.f;
    DWORD g_regroupHintAt = 0;

    // A game world exists: we have a squad (not on the title screen / character editor / loading).
    bool worldLoaded() { return ou && ou->player && ou->player->playerCharacters.size() > 0; }
    bool g_purgePending = true;
    bool g_permanentFailure = false;
    DWORD g_nextReconnect = 0;
    const DWORD RECONNECT_DELAY_MS = 10000;
    std::map<uint8_t, PlayerInfo> g_players;
    std::map<uint8_t, Faction*> g_playerFactions;

    std::string trim(const std::string& s)
    {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }

    bool toBool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

    // Where the settings live. The mod's own folder can be anywhere (Kenshi/mods/KenshiMP, or the
    // Steam Workshop folder, which Steam overwrites on every update), so the player's settings are
    // kept in %LOCALAPPDATA%\kenshi\KenshiMP\kenshimp.cfg, next to the game's saves. The first time,
    // that file is copied from the one shipped with the mod (or from an older install's).
    std::string modDir()
    {
        HMODULE self = NULL;
        char path[MAX_PATH] = { 0 };
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)&modDir, &self) || !GetModuleFileNameA(self, path, MAX_PATH)) return "mods\\KenshiMP\\";
        std::string p = path;
        size_t slash = p.find_last_of("\\/");
        return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
    }

    std::string userConfigPath()
    {
        char base[MAX_PATH] = { 0 };
        size_t n = 0;
        if (getenv_s(&n, base, sizeof(base), "LOCALAPPDATA") != 0 || n <= 1) return "mods\\KenshiMP\\kenshimp.cfg";
        std::string dir = std::string(base) + "\\kenshi";
        CreateDirectoryA(dir.c_str(), NULL);
        dir += "\\KenshiMP";
        CreateDirectoryA(dir.c_str(), NULL);
        return dir + "\\kenshimp.cfg";
    }

    bool fileExists(const std::string& p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

    // Host: player numbers by name, kept between sessions (players.cfg next to kenshimp.cfg). A
    // player's number names its faction and characters in the host's save, so a friend coming
    // back after the host restarted gets the same number, not the one of whoever joined first.
    std::string slotsPath()
    {
        std::string p = userConfigPath();
        size_t slash = p.find_last_of("\\/");
        return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "players.cfg";
    }
    void loadSlots()
    {
        FILE* f = NULL;
        if (fopen_s(&f, slotsPath().c_str(), "r") != 0 || !f) return;
        std::map<std::string, uint8_t> slots;
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            // "<number> <name>"
            char* sp = strchr(line, ' ');
            if (!sp || line[0] == '#') continue;
            int id = atoi(line);
            std::string name(sp + 1);
            while (!name.empty() && (name[name.size() - 1] == '\n' || name[name.size() - 1] == '\r')) name.erase(name.size() - 1);
            if (id > 0 && id < MAX_PLAYERS && !name.empty()) slots[name] = (uint8_t)id;
        }
        fclose(f);
        g_session.setKnownSlots(slots);
        log("player numbers of earlier sessions: %u", (unsigned)slots.size());
    }
    void saveSlots()
    {
        std::map<std::string, uint8_t> slots = g_session.knownSlots();
        FILE* f = NULL;
        if (fopen_s(&f, slotsPath().c_str(), "w") != 0 || !f) return;
        fprintf(f, "# KenshiMP host: player number and name of everybody who joined (kept for returning players)\n");
        for (std::map<std::string, uint8_t>::iterator it = slots.begin(); it != slots.end(); ++it)
            if (it->first.find('\n') == std::string::npos) fprintf(f, "%d %s\n", (int)it->second, it->first.c_str());
        fclose(f);
    }

    // The player's settings file, created from a template the first time.
    std::string configPath()
    {
        std::string user = userConfigPath();
        if (fileExists(user)) return user;
        const std::string templates[] = { "mods\\KenshiMP\\kenshimp.cfg", modDir() + "kenshimp.cfg", "kenshimp.cfg" };
        for (int i = 0; i < 3; ++i)
            if (fileExists(templates[i]))
            {
                if (CopyFileA(templates[i].c_str(), user.c_str(), TRUE)) log("settings created: %s (from %s)", user.c_str(), templates[i].c_str());
                return fileExists(user) ? user : templates[i];
            }
        return user;
    }

    // kenshimp.cfg (key=value, '#' comments), see configPath().
    void loadConfig()
    {
        FILE* f = NULL;
        std::string path = configPath();
        if (fopen_s(&f, path.c_str(), "r") != 0 || !f) { ErrorLog("KenshiMP: kenshimp.cfg not found (" + path + "), multiplayer disabled"); return; }
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            std::string l = trim(line);
            if (l.empty() || l[0] == '#') continue;
            size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
            if (k == "mode") g_cfg.mode = v;
            else if (k == "address") g_cfg.address = v;
            else if (k == "port") g_cfg.port = atoi(v.c_str());
            else if (k == "name") g_cfg.name = v;
            else if (k == "faction") g_cfg.faction = v;
            else if (k == "relation") g_cfg.relation = (float)atof(v.c_str());
            else if (k == "ghost_ai") g_cfg.ghostAI = v;
            else if (k == "sync_appearance") g_cfg.syncAppearance = toBool(v);
            else if (k == "sync_equipment") g_cfg.syncEquipment = toBool(v);
            else if (k == "sync_buildings") g_cfg.syncBuildings = toBool(v);
            else if (k == "debug_keys") g_cfg.debugKeys = toBool(v);
            else if (k == "npc_sync") g_cfg.npcSync = toBool(v);
            else if (k == "strict_mods") g_cfg.strictMods = toBool(v);
            else if (k == "auto_reconnect") g_cfg.autoReconnect = toBool(v);
            else if (k == "pause_sync") g_cfg.pauseSync = toBool(v);
            else if (k == "render_smoothing") g_cfg.renderSmoothing = toBool(v);
            else if (k == "weather_sync") g_cfg.weatherSync = toBool(v);
            else if (k == "language") g_cfg.language = v;
            else if (k == "show_players_on_map") g_cfg.playersOnMap = toBool(v);
            else if (k == "tune_speed_match") g_cfg.tuneSpeedMatch = toBool(v);
            else if (k == "tune_trail") g_cfg.tuneTrail = toBool(v);
            else if (k == "tune_repath") g_cfg.tuneRepath = toBool(v);
            else if (k == "tune_unblock") g_cfg.tuneUnblock = toBool(v);
            else if (k == "tune_onscreen") g_cfg.tuneOnScreen = toBool(v);
            else if (k == "ghost_no_collide") g_cfg.ghostNoCollide = toBool(v);
            else if (k == "load_sharing") g_cfg.loadSharing = toBool(v);
            else if (k == "assault_hostility") g_cfg.assaultHostility = toBool(v);
            else if (k == "town_sync") g_cfg.townSync = toBool(v);
            else if (k == "backup_saves") { int n = atoi(v.c_str()); g_cfg.backupSaves = n < 0 ? 0 : (n > 50 ? 50 : n); }
            else if (k == "player_names") g_cfg.playerNames = toBool(v);
            else if (k == "player_names_range") { int n = atoi(v.c_str()); g_cfg.playerNamesRange = n < 0 ? 0 : n; }
            else if (k == "speed_vote") g_cfg.speedVote = toBool(v);
            else if (k == "autotest_load") g_cfg.autotestLoad = v;
            else if (k == "autotest") g_cfg.autotest = toBool(v);
        else if (k == "autotest_goto_shop") g_cfg.autotestGotoShop = toBool(v);
        else if (k == "autotest_carry") g_cfg.autotestCarry = toBool(v);
        else if (k == "autotest_build") g_cfg.autotestBuild = toBool(v);
        else if (k == "npc_range") g_cfg.npcRange = atoi(v.c_str());
        else if (k == "autotest_shots") g_cfg.autotestShots = atoi(v.c_str());
            else if (k == "lobby_key")
            {
                // F1..F12
                if ((v[0] == 'F' || v[0] == 'f') && atoi(v.c_str() + 1) >= 1 && atoi(v.c_str() + 1) <= 12)
                { g_cfg.lobbyKey = VK_F1 + atoi(v.c_str() + 1) - 1; g_cfg.lobbyKeyName = "F" + v.substr(1); }
            }
            else if (k == "password") g_cfg.password = v;
            else if (k == "transport") g_cfg.transport = (v == "udp" || v == "UDP") ? "udp" : "tcp";
            else if (k == "account_password") g_cfg.accountPassword = v;
            else if (k == "server_name") g_cfg.serverName = v.substr(0, 60);
            else if (k == "server_description") g_cfg.serverDescription = v.substr(0, 200);
            else if (k == "server_motd") g_cfg.serverMotd = v.substr(0, 400);
            else if (k == "max_players") { int n = atoi(v.c_str()); g_cfg.maxPlayers = n < 1 ? 1 : (n > MAX_PLAYERS - 1 ? MAX_PLAYERS - 1 : n); }
            else if (k == "server_accounts") g_cfg.serverAccounts = toBool(v);
            else if (k == "server_admins") { std::string a = v; for (size_t i = 0; i < a.size(); ++i) if (a[i] == ',' || a[i] == ';') a[i] = ' '; g_cfg.serverAdmins = a; }
            else if (k == "server_data_dir") g_cfg.serverDataDir = v;
            else if (k == "server_console") g_cfg.serverConsole = toBool(v);
            else if (k == "server_window") g_cfg.serverWindow = v;
            else if (k == "server_render") g_cfg.serverRender = toBool(v);
            else if (k == "server_fps") g_cfg.serverFps = atoi(v.c_str());
            else if (k == "server_autosave_min") g_cfg.serverAutosaveMin = atoi(v.c_str());
            else if (k == "server_save") g_cfg.serverSave = v;
            else if (k == "server_start_save") g_cfg.serverStartSave = v;
            else if (k == "server_world") g_cfg.serverWorld = v;
            else if (k == "server_list_url") g_cfg.serverListUrl = v;
            else if (k == "server_public_address") g_cfg.serverPublicAddress = v;
            else if (k == "server_max_per_address") g_cfg.serverMaxPerAddress = atoi(v.c_str());
            else if (k == "server_npc_cap") g_cfg.serverNpcCap = atoi(v.c_str());
            else if (k == "server_npc_budget") g_cfg.serverNpcBudget = atoi(v.c_str());
            else if (k == "log_max_mb") g_cfg.logMaxMB = atoi(v.c_str());
            else if (k == "server_kick_refusals") g_cfg.serverKickRefusals = atoi(v.c_str());
        }
        fclose(f);
        // A dedicated server gets into its world by itself: CONTINUE (its last save) or a NEW GAME, pressed by name.
        if (g_cfg.mode == "server" && g_cfg.autotestLoad.empty()) g_cfg.autotestLoad = g_cfg.serverWorld.empty() ? "continue" : g_cfg.serverWorld;
    }

    // Rewrites the connection keys in kenshimp.cfg, keeping every other line and comment.
    void saveConfig()
    {
        std::string cfg = configPath();
        const char* path = cfg.c_str();
        std::vector<std::string> lines;
        FILE* f = NULL;
        if (fopen_s(&f, path, "r") == 0 && f)
        {
            char line[512];
            while (fgets(line, sizeof(line), f))
            {
                std::string l = line;
                while (!l.empty() && (l[l.size() - 1] == 10 || l[l.size() - 1] == 13)) l.erase(l.size() - 1);
                lines.push_back(l);
            }
            fclose(f);
        }
        char port[16]; sprintf_s(port, "%d", g_cfg.port);
        const char* keys[] = { "mode", "address", "port", "name", "faction", "password", "transport" };
        std::string vals[] = { g_cfg.mode, g_cfg.address, port, g_cfg.name, g_cfg.faction, g_cfg.password, g_cfg.transport };
        for (int k = 0; k < 7; ++k)
        {
            bool found = false;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                std::string l = trim(lines[i]);
                if (l.empty() || l[0] == '#') continue;
                size_t eq = l.find('=');
                if (eq != std::string::npos && trim(l.substr(0, eq)) == keys[k]) { lines[i] = std::string(keys[k]) + "=" + vals[k]; found = true; }
            }
            if (!found) lines.push_back(std::string(keys[k]) + "=" + vals[k]);
        }
        if (fopen_s(&f, path, "w") != 0 || !f) { ErrorLog("KenshiMP: cannot write kenshimp.cfg"); return; }
        for (size_t i = 0; i < lines.size(); ++i) fprintf(f, "%s\n", lines[i].c_str());
        fclose(f);
    }

    // Relation per player name for the whole session: a player who drops and comes back finds the
    // same war / peace / alliance, not the default.
    std::map<std::string, float> g_relationByName;
    float relationFor(uint8_t player)
    {
        std::map<std::string, float>::iterator it = g_relationByName.find(playerName(player));
        return it != g_relationByName.end() ? it->second : g_cfg.relation;
    }

    void setRelationWith(uint8_t player, float value)
    {
        g_relationByName[playerName(player)] = value;
        Faction* mine = ou && ou->player ? ou->player->getFaction() : NULL;
        Faction* theirs = factionFor(player);
        if (!mine || !theirs) return;
        if (mine->relations) mine->relations->setRelation(theirs, value);
        if (theirs->relations) theirs->relations->setRelation(mine, value);
    }

    // ---- standing with the world's factions -------------------------------------------------
    // The host's NPC AI judges a remote player's characters through his mirror faction, so that
    // faction must carry the player's real relations (bandits hostile, allies friendly, ...).
    bool isMirrorFaction(Faction* f)
    {
        GameData* d = f ? f->getData() : NULL;
        return d && d->stringID.compare(0, 16, "kenshimp_player_") == 0;
    }

    uint32_t g_lastTableHash = 0;
    DWORD g_lastTableCheck = 0;

    void sendFactionTable(bool force)
    {
        Faction* mine = ou->player->getFaction();
        const lektor<Faction*>* all = ou->factionMgr->getAllFactions();
        if (!mine || !mine->relations || !all) return;
        ByteWriter body; uint16_t n = 0;
        for (uint32_t i = 0; i < all->size(); ++i)
        {
            Faction* f = (*all)[i];
            if (!f || f == mine || f->isThePlayer() || isMirrorFaction(f) || !f->getData()) continue;
            body.str(f->getData()->stringID);
            body.f32(mine->relations->getFactionRelation(f));
            ++n;
        }
        ByteWriter w; w.u16(n); w.bytes(body.data);
        uint32_t h = hashBytes(w.data);
        if (!force && h == g_lastTableHash) return;
        g_lastTableHash = h;
        g_session.send(MSG_FACTION_TABLE, w.data);
    }

    void applyFactionTable(uint8_t sender, const Bytes& data)
    {
        Faction* mirror = factionFor(sender);
        if (!mirror || !mirror->relations) return;
        ByteReader r(data);
        uint16_t n = r.u16();
        int applied = 0;
        for (int i = 0; i < n && r.ok(); ++i)
        {
            std::string sid = r.str();
            float rel = clampF(r.f32(), -100.f, 100.f);
            if (!r.ok()) break;
            Faction* f = ou->factionMgr->getFactionByStringID(sid);
            if (!f || f->isThePlayer() || isMirrorFaction(f)) continue;
            mirror->relations->setRelation(f, rel);
            if (f->relations) f->relations->setRelation(mirror, rel);
            ++applied;
        }
        log("faction standings of %s updated (%d factions)", playerName(sender).c_str(), applied);
    }

    // F9/F10: towards the player whose character is selected, or everybody if none is.
    void broadcastRelation(float value)
    {
        int only = -1;
        lektor<RootObject*> sel;
        ou->player->getAllSelectedObjects(sel, CHARACTER);
        std::vector<Character*> cands;
        for (uint32_t i = 0; i < sel.size(); ++i) { hand h(sel[i]); if (Character* c = h.getCharacter()) cands.push_back(c); }
        if (Character* c = ou->player->selectedCharacter.getCharacter()) cands.push_back(c);
        for (size_t i = 0; i < cands.size() && only < 0; ++i)
        {
            uint32_t id = chars_isGhost(cands[i]) ? chars_netIdOf(cands[i]) : 0;
            if (id && !isNpcNetId(id)) only = netIdOwner(id);
        }
        diplomacy_set(only, value);
    }
}

float diplomacy_relation(uint8_t player)
{
    if (!ou || !ou->player || ou->player->playerCharacters.size() == 0) return 0.f;
    Faction* mine = ou->player->getFaction();
    Faction* theirs = factionFor(player);
    if (!mine || !mine->relations || !theirs) return 0.f;
    return mine->relations->getFactionRelation(theirs);
}

// assault_hostility=1: like Kenshi's own factions, a player whose characters hurt ours while we are
// not at war loses standing with us (-25 per assault, at most one every 3 s); below zero it is war.
// Allies are not exempt (an ally who attacks is no ally), but the war only starts below zero.
void diplomacy_assaulted(uint8_t player)
{
    if (!g_cfg.assaultHostility || !g_players.count(player)) return;
    static std::map<uint8_t, DWORD> last;
    DWORD now = GetTickCount();
    if (last.count(player) && now - last[player] < 3000) return;
    last[player] = now;
    float rel = diplomacy_relation(player);
    if (rel < 0) return;   // already at war: fighting is expected
    float next = rel - 25.f;
    if (next < 0)
    {
        diplomacy_set(player, -100.f);
        chat_notice(TF("%s attacked your squad: you are now at war.", playerName(player).c_str()));
        log("assault by %s: war", playerName(player).c_str());
        return;
    }
    setRelationWith(player, next);
    ByteWriter w; w.u8(player); w.f32(next);
    g_session.send(MSG_FACTION_RELATION, w.data);
    chat_notice(TF("%s attacked your squad (relation %.0f).", playerName(player).c_str(), next));
    log("assault by %s: relation %.0f -> %.0f", playerName(player).c_str(), rel, next);
}

void diplomacy_set(int only, float value)
{
    {
        for (std::map<uint8_t, PlayerInfo>::iterator it = g_players.begin(); it != g_players.end(); ++it)
        {
            if (it->first == g_session.localId()) continue;
            if (only >= 0 && it->first != only) continue;
            setRelationWith(it->first, value);
            ByteWriter w; w.u8(it->first); w.f32(value);
            g_session.send(MSG_FACTION_RELATION, w.data);
        }
        const char* fmt = value < 0 ? "You declared war on %s." : value > 50 ? "You allied with %s." : "You made peace with %s.";
        std::string who = only >= 0 ? playerName((uint8_t)only) : std::string(T("the other players"));
        showMessage(TF(fmt, who.c_str()));
        chat_notice(TF(fmt, who.c_str()));
    }
}

namespace
{

    void resendAll()
    {
        chars_resendAll();
        builds_resendAll();
        ground_resendAll();
        towns_resendAll();
        bounties_resendAll();
        if (ou && ou->player) sendFactionTable(true);
    }

    void handleMessage(const NetEvent& e)
    {
        ByteReader r(e.body);
        switch (e.msgType)
        {
        case MSG_FACTION_RELATION:
        {
            uint8_t other = r.u8(); float rel = r.f32();
            if (!r.ok() || other != g_session.localId()) break;
            setRelationWith(e.sender, rel);
            log("%s set its relation with us to %.0f", playerName(e.sender).c_str(), rel);
            showMessage(TF(rel < 0 ? "%s declared war on you!" : rel > 50 ? "%s allied with you." : "%s made peace with you.", playerName(e.sender).c_str()));
            chat_notice(TF(rel < 0 ? "%s declared war on you!" : rel > 50 ? "%s allied with you." : "%s made peace with you.", playerName(e.sender).c_str()));
            break;
        }
        case MSG_CHAT:
        {
            std::string text = chat_clean(r.str());
            if (r.ok() && !text.empty()) { chat_received(e.sender, text); log("chat %s: %s", playerName(e.sender).c_str(), text.c_str()); }
        }
            break;
        case MSG_BUILDING_STATE:
        case MSG_BUILDING_REMOVE:
        case MSG_BUILDING_DAMAGE:
        case MSG_WORLD_BUILDINGS:
        case MSG_WORLD_BUILDING_REPORT:
            builds_onMessage(e);
            break;
        case MSG_SPEED:
            world_onSpeed(e);
            break;
        case MSG_WORLD_SYNC:
            world_onMessage(e);
            break;
        case MSG_WEATHER:
            weather_onMessage(e);
            break;
        case MSG_WORLD_STATES:
        case MSG_WORLD_STATE_REPORT:
            towns_onMessage(e);
            break;
        case MSG_BOUNTIES:
        case MSG_BOUNTY_CRIME:
            bounties_onMessage(e);
            break;
        case MSG_GROUND_ITEM:
        case MSG_GROUND_REMOVE:
        case MSG_GROUND_TAKE:
            ground_onMessage(e);
            break;
        case MSG_FACTION_TABLE:
            applyFactionTable(e.sender, e.body);
            break;
        case MSG_INVENTORY:
        case MSG_INVENTORY_REQUEST:
        case MSG_ITEM_TAKE:
        case MSG_ITEM_GIVE:
        case MSG_ITEM_UNDO:
            items_onMessage(e);
            break;
        case MSG_TRADE:
            trade_onMessage(e);
            break;
        case MSG_WORLD_CONTAINER:
            items_onMessage(e);
            break;
        case MSG_REGROUP:
        {
            uint8_t target = r.u8();
            if (r.ok()) chars_onRegroup(e.sender, target);
            break;
        }
        case MSG_ZONE_MODE:
            npcs_onZoneMode(e);
            break;
        case MSG_NPC_CLAIM:
            npcs_onClaim(e.sender, e.body);
            break;
        case MSG_AUTH_TABLE:
        case MSG_AUTH_HANDOFF:
            auth_onMessage(e);
            break;
        case MSG_RESYNC:
            log("%s reloaded its world, resending everything", playerName(e.sender).c_str());
            chars_forgetMovement(e.sender);
            npcs_onPlayerLeft(e.sender);   // host: forget what that player had, NPCs get re-spawned
            ground_onPlayerLeft(e.sender);  // its old drops are not in its new world: our copies go
            items_worldForget(e.sender);    // the town containers it had open (or, from the host: we had open) start over
            resendAll();
            if (g_session.isHost()) { world_sendNow(); weather_sendNow(); }   // its clock and weather right away
            break;
        default:
            chars_onMessage(e);
        }
    }

    void onPlayerLeft(uint8_t id)
    {
        chars_onPlayerLeft(id);
        world_onPlayerLeft(id);
        builds_onPlayerLeft(id);
        npcs_onPlayerLeft(id);
        items_onPlayerLeft(id);
        trade_onPlayerLeft(id);
        ground_onPlayerLeft(id);
        bounties_onPlayerLeft(id);
        g_players.erase(id);
    }

    std::set<uint8_t> g_relationPending;   // players met on the title screen: relation set once the world exists

    // Title screen: no world yet, so only the session bookkeeping is done (the world part follows
    // the load: resync + relations). Keeps the lobby window informed (connected / refused).
    void titleEvents()
    {
        NetEvent e;
        while (g_session.pollEvent(e))
        {
            switch (e.kind)
            {
            case NetEvent::EV_CONNECTED:
            {
                log("connected, I am player %d (title screen)", (int)e.player.id);
                g_ready = true;
                std::vector<PlayerInfo> ps = g_session.players();
                for (size_t i = 0; i < ps.size(); ++i) { g_players[ps[i].id] = ps[i]; if (ps[i].id != e.player.id) g_relationPending.insert(ps[i].id); }
                break;
            }
            case NetEvent::EV_DISCONNECTED:
                ErrorLog("KenshiMP: disconnected (" + e.text + ")");
                g_ready = false;
                g_players.clear();
                g_relationPending.clear();
                g_permanentFailure = e.text.find("must match") != std::string::npos || e.text.find("version") != std::string::npos ||
                                     e.text.find("full") != std::string::npos || e.text.find("password") != std::string::npos;
                g_nextReconnect = GetTickCount() + RECONNECT_DELAY_MS;
                lobby_setError(TF("Disconnected: %s", lang_reason(e.text).c_str()));
                break;
            case NetEvent::EV_PLAYER_JOINED:
                log("%s joined (title screen)", e.player.name.c_str());
                server_onPlayerJoined(e.player.id, e.player.name);
                g_players[e.player.id] = e.player;
                if (g_session.isHost()) saveSlots();
                g_relationPending.insert(e.player.id);
                break;
            case NetEvent::EV_PLAYER_LEFT:
                server_onPlayerLeft(e.player.id);
                g_players.erase(e.player.id);
                g_relationPending.erase(e.player.id);
                break;
            case NetEvent::EV_WARNING:
                lobby_setError(TF("Warning: %s", lang_reason(e.text).c_str()));
                break;
            default:
                break;   // world messages: resent after our MSG_RESYNC once a game is loaded
            }
        }
    }

    // Session over: remove every ghost of every player (host included), the local world takes over.
    void forgetEveryone()
    {
        g_ready = false;
        std::vector<uint8_t> ids;
        for (std::map<uint8_t, PlayerInfo>::iterator it = g_players.begin(); it != g_players.end(); ++it) ids.push_back(it->first);
        ids.push_back(HOST_ID);
        for (size_t i = 0; i < ids.size(); ++i) onPlayerLeft(ids[i]);
        g_players.clear();
        // Out of session a save may be loaded without us noticing: no engine pointer survives.
        g_playerFactions.clear();
        npcs_reset();
        world_reset();
        towns_onWorldReload();
    }

    void handleEvent(const NetEvent& e)
    {
        switch (e.kind)
        {
        case NetEvent::EV_CONNECTED:
        {
            log("connected, I am player %d", (int)e.player.id);
            g_ready = true;
            std::vector<PlayerInfo> ps = g_session.players();
            for (size_t i = 0; i < ps.size(); ++i) g_players[ps[i].id] = ps[i];
            for (size_t i = 0; i < ps.size(); ++i)
                if (ps[i].id != e.player.id) setRelationWith(ps[i].id, relationFor(ps[i].id));
            resendAll();
            showMessage(T("Connected to the multiplayer session."));
            chat_notice(T("Connected to the multiplayer session."));
            break;
        }
        case NetEvent::EV_DISCONNECTED:
        {
            ErrorLog("KenshiMP: disconnected (" + e.text + ")");
            forgetEveryone();
            // A refusal (wrong version/mods, server full) will not fix itself: no retry loop.
            g_permanentFailure = e.text.find("must match") != std::string::npos || e.text.find("version") != std::string::npos ||
                                 e.text.find("full") != std::string::npos || e.text.find("password") != std::string::npos;
            lobby_setError(TF("Disconnected: %s", lang_reason(e.text).c_str()));
            showMessage(TF("Multiplayer disconnected: %s", lang_reason(e.text).c_str()) +
                        (g_permanentFailure || !g_cfg.autoReconnect ? "" : T(" - reconnecting automatically...")));
            g_nextReconnect = GetTickCount() + RECONNECT_DELAY_MS;
            break;
        }
        case NetEvent::EV_WARNING:
            ErrorLog("KenshiMP: warning: " + e.text);
            showMessage(TF("Multiplayer warning: %s", lang_reason(e.text).c_str()));
            break;
        case NetEvent::EV_PLAYER_JOINED:
            log("%s joined (faction %s)", e.player.name.c_str(), e.player.faction.c_str());
            server_onPlayerJoined(e.player.id, e.player.name);
            g_players[e.player.id] = e.player;
            setRelationWith(e.player.id, relationFor(e.player.id));
            resendAll();   // let the newcomer see our squad, gear and town
            if (g_session.isHost()) { world_sendNow(); weather_sendNow(); saveSlots(); }
            showMessage(TF("%s joined the game", e.player.name.c_str()));
            chat_notice(TF("%s joined the game", e.player.name.c_str()));
            break;
        case NetEvent::EV_PLAYER_LEFT:
        {
            std::string who = playerName(e.player.id);
            log("player %d left (%s)", (int)e.player.id, e.text.c_str());
            server_onPlayerLeft(e.player.id);
            onPlayerLeft(e.player.id);
            showMessage(TF("%s left the game", who.c_str()) + (e.text.empty() ? "" : " (" + lang_reason(e.text) + ")"));
            chat_notice(TF("%s left the game", who.c_str()));
            break;
        }
        case NetEvent::EV_MESSAGE:
            // No game world yet (title screen, loading): world messages cannot be applied.
            // They are not lost: once our world exists we send MSG_RESYNC and get them again.
            if (server_onMessage(e)) break;   // admin command, bag check, server notice
            if (!worldLoaded() && e.msgType != MSG_CHAT && e.msgType != MSG_FACTION_RELATION && e.msgType != MSG_RESYNC) break;
            handleMessage(e);
            break;
        }
    }

    // True when a window of this process (the game) has the keyboard focus.
    bool safeLoadSaveInner(const std::string* name)
    {
        __try
        {
            SaveManager* sm = SaveManager::getSingleton();
            if (!sm) return false;
            sm->load(*name);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeLoadSave(const std::string& name) { return safeLoadSaveInner(&name); }

    // ---------------------------------------------------------------- automatic test run
    // autotest=1: once another player (the test bot) is in the session, run the checks that
    // otherwise need the debug keys, without any keyboard or mouse input (the game can stay in
    // the background). Every result is logged as "autotest: ...".
    void autotestTick(DWORD now)
    {
        static DWORD t0 = 0;
        static int step = 0;
        if (g_cfg.autotest && server_isServer())   // a server has no squad to test with: only the authority lines (AUTHORITY_PLAN step A)
        {
            static DWORD lastAuth = 0;
            if (now - lastAuth >= 2000) { lastAuth = now; npcs_debugAuthority(); }
            return;
        }
        if (!g_cfg.autotest || step < 0) return;
        uint8_t other = 0; bool found = false;
        for (std::map<uint8_t, PlayerInfo>::iterator it = g_players.begin(); it != g_players.end(); ++it)
            if (it->first != g_session.localId()) { other = it->first; found = true; break; }
        if (!found) return;
        if (!t0) { t0 = now; log("autotest: start (other player %d)", (int)other); }
        DWORD s = (now - t0) / 1000;
        std::vector<std::pair<uint32_t, Character*> > locals;
        chars_localCharacters(locals);
        Character* me = locals.empty() ? NULL : locals[0].second;
        if (!me) return;
        // Step A of AUTHORITY_PLAN.md: every 2 s, one "auth:" line per entity (own / copy) for the offline comparison.
        { static DWORD lastAuth = 0;
          if (now - lastAuth >= 2000) { lastAuth = now; chars_debugAuthority(); npcs_debugAuthority(); } }
        // Frames of what the player sees, saved by the game itself (autotest_shots=N: every N seconds after the start).
        if (g_cfg.autotestShots > 0)
        {
            static int shotNo = 0; static DWORD lastShot = 0;
            if (s >= 40 && (lastShot == 0 || now - lastShot >= (DWORD)g_cfg.autotestShots * 1000) && shotNo < 18)
            {
                lastShot = now;
                char f[64]; sprintf_s(f, "kmp_shot_%s_%d.png", g_session.isHost() ? "host" : "client", ++shotNo);
                log("autotest: screenshot %s -> %s", f, chars_screenshot(f) ? "saved" : "failed");
                if (shotNo == 3 && g_cfg.autotestGotoShop) log("autotest: goto shop (shots) -> %s", builds_debugGotoShop().c_str());
                if (shotNo >= 2) chars_debugNpcCensus();
                if (shotNo >= 5 && shotNo <= 8) chars_debugShopView();
                // treatment: the HOST is wounded (shot 2), the CLIENT heals the copy it sees (shot 4), the host looks at itself (shots 4-7)
                if (shotNo == 2 && g_session.isHost()) log("autotest: wound -> %s", chars_debugWoundSelf(me).c_str());
                if (shotNo == 4 && !g_session.isHost()) log("autotest: heal -> %s", chars_debugHealGhost(me).c_str());
                if (shotNo == 6 && g_session.isHost()) log("autotest: heal control -> %s", chars_debugHealSelf(me).c_str());
                if (shotNo >= 3 && shotNo <= 7 && g_session.isHost()) log("autotest: flesh -> %s", chars_debugFleshSelf(me).c_str());
                if (shotNo >= 3 && shotNo <= 7 && !g_session.isHost()) log("autotest: the copy's medical state -> %s", chars_debugGhostMedical().c_str());
                // carry: the HOST is knocked out (shot 9), the CLIENT picks the copy up (shot 10); frames of both games follow
                if (shotNo == 9 && g_session.isHost()) log("autotest: host knocked out -> %s", chars_debugKnockOutSelf(me, 70.f).c_str());
                // the client picks the copy up as soon as it sees it knocked out (the two games' clocks are not aligned)
                { static bool lifted = false;
                  if (shotNo >= 6 && !g_session.isHost() && !lifted)
                  { std::string r = chars_debugCarryGhost(me); if (r.compare(0, 5, "order") == 0) { lifted = true; log("autotest: lift -> %s", r.c_str()); } } }
                if (shotNo >= 7 && shotNo <= 15 && !g_session.isHost()) log("autotest: carry state -> %s", chars_debugCarryState().c_str());
                if (shotNo >= 9) chars_debugFocus(me, 70.f, shotNo == 11 ? 90.f : (shotNo == 13 ? -90.f : 0.f));   // 11 / 13: the character high / low on the screen (name label check)   // the carrying scene, beside the interface windows
                { static bool claimed = false;   // step C: the client takes over a host NPC; the host must answer with a grant
                  if (shotNo >= 10 && !g_session.isHost() && !claimed)
                  { std::string r = chars_debugClaimNpc(me); if (r.compare(0, 8, "claiming") == 0) claimed = true; log("autotest: claim NPC -> %s", r.c_str()); } }
                if (shotNo == 5) log("autotest: talk -> %s", chars_debugTalk(me).c_str());
                if (shotNo == 6 || shotNo == 7) log("autotest: talk state -> %s", chars_debugTalkState().c_str());
            }
        }
        // autotest_build=1: its own short script. Each game places a finished building and a blueprint next to its first
        // character, then advances the blueprint; both report their own buildings and the copies of the other's. Then the
        // world map is opened (frames with the players' names on it) and closed.
        if (g_cfg.autotestBuild)
        {
            static int bs = 0;
            float side = g_session.isHost() ? 10.f : -10.f;
            const char* role = g_session.isHost() ? "host" : "client";
            if (bs == 0 && s >= 10) { bs = 1; log("autotest: build -> %s", builds_debugPlace(true, side).c_str()); log("autotest: build -> %s", builds_debugPlace(false, side).c_str()); }
            if (bs == 1 && s >= 20) { bs = 2; log("autotest: build progress -> %s", builds_debugProgress(0.5f).c_str()); }
            { static DWORD lastRep = 0; if (bs >= 1 && now - lastRep >= 10000) { lastRep = now; log("autotest: buildings -> %s", builds_debugReport().c_str()); } }
            if (bs == 2 && s >= 35) { bs = 3; chars_debugFocus(me, 0.f, 0.f); }
            if (bs == 3 && s >= 38) { bs = 4; char f[64]; sprintf_s(f, "kmp_shot_%s_build.png", role); log("autotest: screenshot %s -> %s", f, chars_screenshot(f) ? "saved" : "failed"); }
            if (bs == 4 && s >= 45) { bs = 5; log("autotest: map -> %s", chars_debugOpenMap(true).c_str()); }
            if (bs == 5 && s >= 50) { bs = 6; char f[64]; sprintf_s(f, "kmp_shot_%s_map.png", role); log("autotest: screenshot %s -> %s", f, chars_screenshot(f) ? "saved" : "failed"); }
            if (bs == 6 && s >= 55) { bs = 7; log("autotest: map -> %s", chars_debugOpenMap(false).c_str()); log("autotest: build; done"); }
            return;
        }
        // autotest_carry=1: its own short script (other players' knocked-out characters carried, ours carried).
        if (g_cfg.autotestCarry)
        {
            // Game speed vote: alone, x3 does not apply; once the other player chose x3 as well (bot, KMP_TEST_SPEED), it does.
            static int vs = 0;
            if (vs == 0 && s >= 8)  { vs = 1; ou->setGameSpeed(3.0f, false); log("autotest: speed x3 asked"); }
            if (vs == 1 && s >= 13) { vs = 2; log("autotest: speed while the other player does not agree -> x%.1f", ou->getFrameSpeedMultiplier()); }
            if (vs == 2 && s >= 26) { vs = 3; log("autotest: speed once the other player agrees (vote) / changed it (free) -> x%.1f (mode %s)", ou->getFrameSpeedMultiplier(), g_cfg.speedVote ? "vote" : "free"); }
            if (vs == 3 && s >= 28) { vs = 4; ou->setGameSpeed(1.0f, false); log("autotest: speed back to x1"); }
            static int cs = 0;
            if (cs == 0 && s >= 35) { cs = 1; log("autotest: carry -> %s", chars_debugCarryGhost(me).c_str()); }
            if (cs == 1 && s >= 48) { cs = 2; log("autotest: carry state -> %s", chars_debugCarryState().c_str()); }
            if (cs == 2 && s >= 55) { cs = 3; log("autotest: carry state later -> %s", chars_debugCarryState().c_str()); }
            if (cs == 3 && s >= 62) { cs = 4; log("autotest: knocked out self -> %s", chars_debugKnockOutSelf(me, 18.f).c_str()); }
            if (cs == 4 && s >= 68) { cs = 5; log("autotest: held report -> %s", chars_debugHeldReport(me).c_str()); }
            if (cs == 5 && s >= 74) { cs = 6; log("autotest: held report later -> %s", chars_debugHeldReport(me).c_str()); }
            if (cs == 6 && s >= 80) { cs = 7; log("autotest: held report at the end -> %s", chars_debugHeldReport(me).c_str()); }
            // Caged: knocked out again, the bot cages the character (state 3); awake again, the player orders it to pick the lock.
            if (cs == 7 && s >= 84) { cs = 8; log("autotest: knocked out again -> %s", chars_debugKnockOutSelf(me, 4.f).c_str()); }
            if (cs == 8 && s >= 92) { cs = 9; log("autotest: caged -> %s", chars_debugHeldReport(me).c_str()); log("autotest: pick lock -> %s", chars_debugPickLock(me).c_str()); }
            if (cs == 9 && s >= 108) { cs = 10; log("autotest: pick lock again -> %s", chars_debugPickLock(me).c_str()); }
            if (cs == 10 && s >= 126) { cs = 11; log("autotest: after the lock pick -> %s", chars_debugHeldReport(me).c_str()); log("autotest: carry; done"); }
            return;
        }
        static bool crimeDone = false;   // host: our NPCs "saw" the other player's character commit a crime
        if (!crimeDone && s >= 30 && g_session.isHost())
        { crimeDone = true; log("autotest: crime on ghost -> %s", bounties_debugCrimeOnGhost(bounties_debugFaction()).c_str()); }
        if (step == 0 && s >= 2)
        {
            step = 1;
            if (steam_available())
            {
                std::vector<SteamFriend> fr; steam_friends(fr);
                int online = 0, kenshi = 0;
                for (size_t i = 0; i < fr.size(); ++i) { if (fr[i].online) ++online; if (fr[i].inKenshi) ++kenshi; }
                log("autotest: steam ok, %d friend(s), %d online, %d in Kenshi", (int)fr.size(), online, kenshi);
            }
            else log("autotest: steam not available");
            std::string p = configPath();
            log("autotest: settings file %s exists=%d", p.c_str(), (int)fileExists(p));
        }
        if (step == 1 && s >= 4)  { step = 11; log("autotest: one item for the race -> %s", items_debugGiveOne(me).c_str()); }
        if (step == 11 && s >= 6)  { step = 12; log("autotest: crossbow -> %s", items_debugEquipCrossbow(me).c_str()); }
        if (step == 12 && s >= 8)  { step = 2; trade_request(other); log("autotest: trade requested"); }
        if (step == 2 && s >= 12) { step = 3; log("autotest: trade loot -> %s", items_debugLoot(me).c_str()); }
        if (step == 3 && s >= 15) { step = 4; trade_endAll(); }
        if (step == 4 && s >= 17) { step = 5; log("autotest: backpack take -> %s", items_debugBackpack(me).c_str()); }
        if (step == 5 && s >= 25) { step = 51; builds_debugSpawn(); log("autotest: test building placed"); }
        if (step == 51 && s >= 40) { step = 52; log("autotest: our shot -> %s", chars_debugShoot(me).c_str()); }
        if (step == 52 && s >= 44) { step = 53; log("autotest: our shot -> %s", chars_debugShoot(me).c_str()); }
        if (step == 53 && s >= 56) { step = 6; log("autotest: ground pickup -> %s", ground_debugPickup(me).c_str()); }
        if (step == 6 && s >= 60) { step = 7; log("autotest: bag given -> %s", ground_debugGiveBag(me).c_str()); }
        if (step == 7 && s >= 63) { step = 8; log("autotest: bag dropped -> %s", ground_debugDropBag(me).c_str()); }
        if (step == 8 && s >= 66) { step = 81; log("autotest: factions screen opened -> %d", (int)lobby_debugOpenFactions(other, true)); }
        if (step == 81 && s >= 69) { step = 82; log("autotest: ALLY pressed in the factions screen -> %d", (int)lobby_pressTitleButton("KMP_Dip_1_a")); }
        if (step == 82 && s >= 72) { step = 83; log("autotest: relation with %s now %.0f", playerName(other).c_str(), diplomacy_relation(other)); lobby_debugOpenFactions(other, false); }
        if (step == 83 && s >= 74) { step = 84; lobby_toggle(); log("autotest: lobby window opened -> %d", (int)lobby_isOpen()); }
        if (step == 84 && s >= 82) { step = 85; log("autotest: Advanced settings pressed -> %d", (int)lobby_pressTitleButton("KMP_Advanced")); }
        if (step == 85 && s >= 102) { step = 86; log("autotest: Advanced settings pressed again -> %d", (int)lobby_pressTitleButton("KMP_Advanced")); }
        if (step == 86 && s >= 107) { step = 87; lobby_toggle(); log("autotest: lobby window closing"); }
        if (step == 87 && s >= 110) { step = 88; log("autotest: shop trade opened (test) -> %s", items_debugShopTrade().c_str()); }
        if (step == 88 && s >= 113) { step = 89; log("autotest: shop buy -> %s", items_debugBuyFromShelf(me).c_str()); }
        // Speed lock: fast-forward is refused while connected (everybody simulates at x1).
        if (step == 89 && s >= 118) { step = 90; ou->setGameSpeed(3.0f, false); log("autotest: game speed set to x3"); }
        if (step == 90 && s >= 121) { step = 91; log("autotest: speed lock -> game speed is x%.1f (was set to x3)", ou->getFrameSpeedMultiplier()); }
        // Client only: GO TO the host's character (what the multiplayer window's GO TO button does).
        if (step == 91 && s >= 123) { step = 92; log("autotest: goto -> %s", g_session.isHost() ? "skipped (host)" : chars_regroupTo(other).c_str()); }
        // (with autotest_shots the frames go on after the script's end: step 93 does nothing)
        if (step == 92 && s >= 150) { step = g_cfg.autotestShots > 0 ? 93 : -1; chars_logHitStats(); log("autotest: lobby open after close -> %d; done", (int)lobby_isOpen()); }
    }

    bool gameFocused()
    {
        HWND w = GetForegroundWindow();
        DWORD pid = 0;
        if (w) GetWindowThreadProcessId(w, &pid);
        return pid == GetCurrentProcessId();
    }

    bool keyPressed(int vk, bool& wasDown)
    {
        // GetAsyncKeyState is global: ignore keys typed in other applications.
        if (!gameFocused()) { GetAsyncKeyState(vk); wasDown = false; return false; }
        // Polled once per frame: also honour the "pressed since last call" bit so a quick tap
        // between two frames is not lost.
        SHORT s = GetAsyncKeyState(vk);
        bool down = (s & 0x8000) != 0;
        bool pressed = (down && !wasDown) || (!down && (s & 1));
        wasDown = down;
        return pressed;
    }

    // Identity of the game content: every player must run the same mods in the same order,
    // otherwise FCS ids (characters, items, buildings) do not resolve the same way.
    std::string modList()
    {
        // Game build first: Steam/GOG share data, but a different Kenshi version does not.
        std::string s = "game " + KenshiLib::GetKenshiVersion().GetVersion();
        if (!ou) return s;
        for (uint32_t i = 0; i < ou->activeMods.size(); ++i)
        {
            ModInfo* m = ou->activeMods[i];
            if (!m) continue;
            if (!s.empty()) s += ";";
            s += m->name;
        }
        return s;
    }

    void startSession()
    {
        if (!mp_hooksOk())
        {
            static bool told = false;
            if (!told) { told = true; lobby_setError(T("Multiplayer is off: this version of Kenshi is not supported (game functions not found). Your saves are untouched.")); }
            return;
        }
        g_session.setModList(modList(), g_cfg.strictMods);
        g_session.setPassword(g_cfg.password);
        server_configureSession();
        std::string err;
        bool ok = false;
        if (g_cfg.mode == "server")
        {
            loadSlots();
            ok = g_session.host(g_cfg.port, g_cfg.serverName, "", err);
        }
        else if (g_cfg.mode == "host")
        {
            loadSlots();
            ok = g_session.host(g_cfg.port, g_cfg.name, g_cfg.faction, err);
            if (ok) steam_onHosting(true, g_cfg.port);   // Steam friends can join too
        }
        else if (g_cfg.mode == "join")
        {
            // "steam:<id>": through Steam's network (tunnel to that player), otherwise an IP address.
            if (g_cfg.address.compare(0, 6, "steam:") == 0)
            {
                int local = steam_tunnelTo(_strtoui64(g_cfg.address.c_str() + 6, NULL, 10));
                if (local) ok = g_session.join("127.0.0.1", local, g_cfg.name, g_cfg.faction, err);
                else err = "Steam is not available (start Kenshi from Steam) or the host could not be reached";
            }
            else ok = g_session.join(g_cfg.address, g_cfg.port, g_cfg.name, g_cfg.faction, err);
        }
        else return;
        if (!ok)
        {
            ErrorLog("KenshiMP: cannot start session: " + err);
            showMessage(TF("Multiplayer: %s", lang_reason(err).c_str()));
            lobby_setError(lang_reason(err));
            g_nextReconnect = GetTickCount() + RECONNECT_DELAY_MS;
            return;
        }
        log("session started as %s", g_cfg.mode.c_str());
        if (g_session.isHost())
        {
            PlayerInfo me; me.id = HOST_ID; me.name = server_isServer() ? g_cfg.serverName : g_cfg.name; me.faction = g_cfg.faction;
            g_players[HOST_ID] = me;
            g_ready = true;
        }
    }

    // Called every frame from the game's main loop.
    void pump()
    {
        if (!ou || !ou->player) return;
        static bool hinted = false;
        if (!hinted && worldLoaded()) { hinted = true; if ((g_session.active() || g_cfg.mode == "join") && !g_ready) { showMessage(T("Connecting to the host...")); log("hint: 'Connecting to the host...' shown"); } else if (!g_session.active()) showMessage(TF("KenshiMP: %s = multiplayer window (host / join), Enter = chat.", g_cfg.lobbyKeyName.c_str())); }

        // Test runs: targets for the town tests, once the world exists (connected or not).
        // (10 s after the squad appears: the town around it is loaded by then)
        static bool townTargets = false;
        static DWORD worldSeenAt = 0;
        if (g_cfg.autotest && !townTargets && worldLoaded() && !worldSeenAt) worldSeenAt = GetTickCount();
        if (g_cfg.autotest && !townTargets && worldSeenAt && GetTickCount() - worldSeenAt > 10000)
        {
            townTargets = true;
            log("autotest: town door %s", builds_debugTownDoor().c_str());
            log("autotest: shop %s", builds_debugShop().c_str());
            if (g_cfg.autotestGotoShop) log("autotest: goto shop -> %s", builds_debugGotoShop().c_str());
            log("autotest: unique %s", towns_debugUnique().c_str());
            log("autotest: town override %s", towns_debugOverride().c_str());
            log("autotest: far town %s", towns_debugFarTown().c_str());
            log("autotest: faction %s", bounties_debugFaction().c_str());
        }

        // Test runs: a census of the surroundings every 10 s (NPCs, ghosts, zone, furnished buildings),
        // connected or not, so a run shows what the world looked like before, during and after joining.
        if ((g_cfg.autotest || g_cfg.debugKeys) && worldLoaded())
        {
            static DWORD lastCensus = 0;
            if (GetTickCount() - lastCensus >= 10000)
            {
                lastCensus = GetTickCount();
                std::string c = npcs_debugCensus();
                if (!c.empty()) log("%s", c.c_str());
                log("census: paused=%d, game speed x%.1f", (int)ou->isPaused(), ou->getFrameSpeedMultiplier());
                std::vector<std::string> b = builds_debugCensus(250.f);
                for (size_t i = 0; i < b.size(); ++i) log("%s", b[i].c_str());
            }
        }

        // Events are drained even when the session just died, so the disconnect is handled.
        NetEvent e;
        while (g_session.pollEvent(e)) handleEvent(e);

        if (!g_session.active())
        {
            // Client: retry until it works (the host may start later or restart).
            if (g_cfg.mode == "join" && g_cfg.autoReconnect && !g_permanentFailure && GetTickCount() >= g_nextReconnect)
            {
                g_nextReconnect = GetTickCount() + RECONNECT_DELAY_MS;
                log("reconnecting to %s:%d...", g_cfg.address.c_str(), g_cfg.port);
                startSession();
            }
            return;
        }

        // Game speed: by vote (every player on the same one, x1 otherwise) or, speed_vote=0 on the host, anybody's change.
        world_speedTick(GetTickCount());

        if (!g_ready) return;

        // A (re)loaded game brings back whatever the save contained: purge stale ghosts again.
        static uint32_t lastSquadSize = 0;
        uint32_t squadSize = ou->player->playerCharacters.size();
        if (lastSquadSize == 0 && squadSize > 0) g_purgePending = true;
        lastSquadSize = squadSize;
        if (!worldLoaded()) return;   // nothing to sync on the title screen / while loading

        chat_tick();
        backup_tick();
        // Chat: opened when Enter is released, so that key press does not also reach the new box.
        static bool enter = false, openOnRelease = false;
        // Decided when Enter goes down: an Enter that validates a game dialog (save, rename...) is not for us.
        if (!chat_isOpen() && keyPressed(VK_RETURN, enter)) openOnRelease = chat_mayOpen();
        if (openOnRelease && !(GetAsyncKeyState(VK_RETURN) & 0x8000)) { openOnRelease = false; chat_open(); }
        static bool f9 = false, f10 = false;
        if (keyPressed(VK_F9, f9)) broadcastRelation(WAR);
        if (keyPressed(VK_F10, f10)) broadcastRelation(PEACE);
        static bool f11 = false;
        if (keyPressed(VK_F11, f11) && g_cfg.debugKeys) builds_debugSpawn();
        static bool f8 = false;
        if (keyPressed(VK_F8, f8) && g_cfg.debugKeys) world_debugShift();
        autotestTick(GetTickCount());
        lobby_factionsTick();
        // F6 (debug): trade with the first other player (ask, or accept their offer).
        static bool f6 = false;
        if (keyPressed(VK_F6, f6) && g_cfg.debugKeys)
        {
            std::vector<PlayerInfo> ps = g_session.players();
            for (size_t i = 0; i < ps.size(); ++i) if (ps[i].id != g_session.localId()) { trade_request(ps[i].id); break; }
        }
        // F5 (debug): reload the save "kmptest" (world reload while connected).
        static bool f5 = false;
        if (keyPressed(VK_F5, f5) && g_cfg.debugKeys)
        {
            log("debug: loading save 'kmptest'");
            if (!safeLoadSave("kmptest")) log("debug: load failed");
        }
        // F1 (debug): our selected character attacks the first remote character.
        static bool f1 = false;
        if (keyPressed(VK_F1, f1) && g_cfg.debugKeys)
        {
            Character* me = NULL;
            lektor<RootObject*> sel;
            ou->player->getAllSelectedObjects(sel, CHARACTER);
            for (uint32_t i = 0; i < sel.size() && !me; ++i) { hand h(sel[i]); Character* c = h.getCharacter(); if (c && !chars_isGhost(c)) me = c; }
            if (!me && ou->player->playerCharacters.size()) me = ou->player->playerCharacters[0];
            Character* target = chars_nextGhost(NULL);
            log("debug attack: %s", chars_debugAttack(me, target) ? "ordered" : "failed");
        }
        // F2 (debug): take the first item of the remote inventory open on screen (as a loot would).
        static bool f2 = false;
        if (keyPressed(VK_F2, f2) && g_cfg.debugKeys)
        {
            Character* me = NULL;
            lektor<RootObject*> sel;
            ou->player->getAllSelectedObjects(sel, CHARACTER);
            for (uint32_t i = 0; i < sel.size() && !me; ++i) { hand h(sel[i]); Character* c = h.getCharacter(); if (c && !chars_isGhost(c)) me = c; }
            if (!me) me = ou->player->selectedCharacter.getCharacter();
            std::string r = me && !chars_isGhost(me) ? items_debugLoot(me) : std::string("select one of your characters first");
            if (r == "no remote inventory is open" && me)
            {
                std::string b = items_debugBackpack(me);
                r = b != "no remote backpack within reach" ? "backpack: " + b : "ground: " + ground_debugPickup(me);
            }
            log("debug loot: %s", r.c_str());
        }
        // F3 (debug): select the next remote character and centre the camera on it.
        static bool f3 = false;
        static Character* lastFocus = NULL;
        if (keyPressed(VK_F3, f3) && g_cfg.debugKeys)
        {
            if (Character* g = chars_nextGhost(lastFocus))
            {
                lastFocus = g;
                ou->player->selectObject(g, false);
                ou->player->focusCamera(g->getPosition());
                log("debug: camera on ghost %08x", chars_netIdOf(g));
            }
        }

        // Client: a squad that arrived far from the host is told how to join it (once per load).
        if (g_regroupHintAt && (int)(GetTickCount() - g_regroupHintAt) >= 0)
        {
            g_regroupHintAt = 0;
            Ogre::Vector3 hostAt;
            std::vector<std::pair<uint32_t, Character*> > mine;
            chars_localCharacters(mine);
            if (!g_session.hostIsServer() && !mine.empty() && mine[0].second && chars_playerPosition(HOST_ID, hostAt))
            {
                float d = mine[0].second->getPosition().distance(hostAt);
                if (d > REGROUP_HINT_DISTANCE)
                {
                    log("regroup hint: %.0f units from the host", d);
                    std::string hint = TF("You are far from %s: %s > Go to, or type /goto, to travel to them.", playerName(HOST_ID).c_str(), g_cfg.lobbyKeyName.c_str());
                    showMessage(hint);
                    chat_notice(hint);
                }
            }
        }

        if (g_purgePending)
        {
            // Ghosts that a previous session left in the savegame come back as orphans.
            g_purgePending = false;
            onWorldReload();
            chars_purgeStale();
            builds_purgeStale();
            for (std::set<uint8_t>::iterator it = g_relationPending.begin(); it != g_relationPending.end(); ++it)
                if (g_players.count(*it)) setRelationWith(*it, relationFor(*it));
            g_relationPending.clear();
            resendAll();
            g_session.send(MSG_RESYNC, Bytes());   // and ask everybody for their state
            log("world ready, resync requested");
            g_regroupHintAt = g_session.isHost() ? 0 : GetTickCount() + REGROUP_HINT_DELAY_MS;
            if (g_session.isHost()) backup_hostSave();   // its save holds the shared world: a copy first
            if (g_cfg.debugKeys)
            {
                // Test data: a few prosthetic limb ids of this game (for the bot's limb test).
                lektor<GameData*> limbs;
                ou->gamedata.getDataOfType(limbs, LIMB_REPLACEMENT);
                for (uint32_t i = 0; i < limbs.size() && i < 6; ++i)
                    if (limbs[i]) log("prosthetic available: %s (%s)", limbs[i]->stringID.c_str(), limbs[i]->name.c_str());
                lektor<GameData*> bows;
                ou->gamedata.getDataOfType(bows, CROSSBOW);
                int shown = 0;
                for (int pass = 0; pass < 2; ++pass)   // base game ones first (mods' may not fit a ghost)
                    for (uint32_t i = 0; i < bows.size() && shown < 3; ++i)
                        if (bows[i] && (bows[i]->stringID.find("gamedata.base") != std::string::npos) == (pass == 0))
                        { log("crossbow available: %s (%s)", bows[i]->stringID.c_str(), bows[i]->name.c_str()); ++shown; }
                lektor<GameData*> packs;
                ou->gamedata.getDataOfType(packs, CONTAINER);
                for (uint32_t i = 0; i < packs.size(); ++i)
                    if (packs[i] && packs[i]->name.find("Backpack") != std::string::npos) log("backpack available: %s (%s)", packs[i]->stringID.c_str(), packs[i]->name.c_str());
            }
        }

        DWORD now = GetTickCount();
        if (now - g_lastTableCheck > 5000) { g_lastTableCheck = now; sendFactionTable(false); }
        chars_tick(now);
        builds_tick(now);
        world_tick(now);
        npcs_tick(now);
        auth_tick(now);
        items_tick(now);
        trade_tick(now);
        weather_tick(now);
        towns_tick(now);
        bounties_tick(now);
        ground_tick(now);
        server_clientTick(now);

        // Network diagnostics (F7 in game, log every 30 s): ping, bandwidth, ghosts.
        static DWORD lastRate = now;
        static NetStats prevStats;
        static double inKBs = 0, outKBs = 0;
        if (now - lastRate >= 1000)
        {
            NetStats st = g_session.stats();
            double secs = (now - lastRate) / 1000.0;
            inKBs = (st.bytesIn - prevStats.bytesIn) / 1024.0 / secs;
            outKBs = (st.bytesOut - prevStats.bytesOut) / 1024.0 / secs;
            prevStats = st;
            lastRate = now;
        }
        static bool f7 = false;
        static DWORD lastStats = now;
        bool showNow = keyPressed(VK_F7, f7);
        if (showNow || now - lastStats > 30000)
        {
            char buf[256];
            sprintf_s(buf, sizeof(buf), "Ping %d ms | in %.1f KB/s | out %.1f KB/s | %d ghost(s) | %d player(s) | %u state(s) skipped",
                      g_session.pingMs(), inKBs, outKBs, chars_ghostTotal(), (int)g_players.size(), prevStats.statesDropped);
            if (showNow) showMessage(buf);
            else { lastStats = now; log("%s", buf); chars_logHitStats(); chars_logRenderStats(); }
        }
    }
}

bool ready() { return g_ready && g_session.active(); }
bool server_loadSave(const std::string& name) { return safeLoadSave(name); }
bool mp_worldLoaded() { return worldLoaded(); }
std::string mp_modList() { return modList(); }
std::string mp_configDir()
{
    std::string p = userConfigPath();
    size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
}

void log(const char* fmt, ...)
{
    char buf[512];
    va_list a; va_start(a, fmt); _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, a); va_end(a);   // truncates instead of aborting the game on a long line
    DebugLog(std::string("KenshiMP: ") + buf);
    if (g_cfg.mode == "server") server_logLine(buf);
}

static std::vector<std::string> g_problems;
static std::map<std::string, DWORD> g_problemAt;
static bool g_hooksOk = true;
bool mp_hooksOk() { return g_hooksOk; }
void problem(const char* key, const char* fmt, ...)
{
    DWORD now = GetTickCount();
    std::map<std::string, DWORD>::iterator it = g_problemAt.find(key);
    if (it != g_problemAt.end() && now - it->second < 60000) return;   // once a minute per kind
    g_problemAt[key] = now;
    char buf[400];
    va_list a; va_start(a, fmt); vsprintf_s(buf, sizeof(buf), fmt, a); va_end(a);
    char line[460]; sprintf_s(line, "[%lu s] %s", (unsigned long)(now / 1000), buf);
    g_problems.push_back(line);
    if (g_problems.size() > 60) g_problems.erase(g_problems.begin());
    DebugLog(std::string("KenshiMP: PROBLEM: ") + buf);
}
std::vector<std::string> problems_recent() { return g_problems; }

void showMessage(const std::string& s) { if (ou) ou->showPlayerAMessage(s, false); }

std::string playerName(uint8_t id)
{
    std::map<uint8_t, PlayerInfo>::iterator it = g_players.find(id);
    return it != g_players.end() ? it->second.name : std::string("A player");
}

namespace
{
    // A faction needs real FCS data behind it (platoons/AI read it), so we create a FACTION
    // GameData first and let the FactionManager build the Faction from it.
    Faction* safeMakeFaction(const std::string* sid, const std::string* name)
    {
        __try
        {
            Faction* f = ou->factionMgr->getFactionByStringID(*sid);
            if (f) return f;
            GameData* d = ou->gamedata.getData(*sid);
            if (!d) d = ou->gamedata.createNewData(FACTION, *sid, *name);
            if (!d) return NULL;
            return ou->factionMgr->getOrCreateFaction(d);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }

    // Factions created after world load miss the relations setup the game does at startup;
    // without it relations->me stays NULL and the first combat crashes in FactionRelations.
    int safeSetupRelations(Faction* f)
    {
        __try
        {
            if (!f->relations) return -1;
            if (f->relations->me) return 0;
            f->relations->setupPhase1(f);
            f->relations->setupPhase2();
            return f->relations->me ? 1 : -2;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -3; }
    }
}

Faction* factionFor(uint8_t player)
{
    std::map<uint8_t, Faction*>::iterator it = g_playerFactions.find(player);
    if (it != g_playerFactions.end()) return it->second;
    if (!ou || !ou->factionMgr) return NULL;
    if (player == HOST_ID && g_session.hostIsServer()) return NULL;   // a dedicated server has no characters, no faction to mirror

    char buf[32]; sprintf_s(buf, sizeof(buf), "kenshimp_player_%d", (int)player);
    std::string sid(buf);
    std::map<uint8_t, PlayerInfo>::iterator p = g_players.find(player);
    std::string name = p != g_players.end() ? p->second.faction : sid;
    Faction* f = safeMakeFaction(&sid, &name);
    if (!f) { log("could not create faction for player %d", (int)player); return NULL; }
    int rel = safeSetupRelations(f);
    log("faction relations setup for player %d: %d (1=fixed, 0=already ok, <0=error)", (int)player, rel);
    if (rel < 0) return NULL;
    // Player ids follow join order, so this slot may have belonged to someone else in a
    // previous session (the faction is saved in savegames): show the current owner's name.
    if (f->getName() != name) { log("mirror faction %s renamed '%s' -> '%s'", sid.c_str(), f->getName().c_str(), name.c_str()); f->setName(name); }
    g_playerFactions[player] = f;
    log("faction '%s' ready for player %d (data=%p)", name.c_str(), (int)player, f->getData());
    return f;
}

// The world was (re)loaded: every cached engine pointer is stale.
void onWorldReload()
{
    g_playerFactions.clear();
    npcs_reset();
    chars_onWorldReload();
    items_onWorldReload();
    trade_onWorldReload();
    weather_onWorldReload();
    towns_onWorldReload();
    bounties_onWorldReload();
    ground_onWorldReload();
    chat_onWorldReload();
    builds_onWorldReload();
}

// An invitation accepted (or "Join game" on a friend's profile, or Kenshi started by Steam for
// it): join that player through Steam.
void steamFrame()
{
    if (!steam_init()) return;
    unsigned long long host = 0;
    if (!steam_takeJoinRequest(host)) return;
    char addr[40]; sprintf_s(addr, "steam:%llu", host);
    g_cfg.address = addr;
    log("steam: joining %s", addr);
    showMessage(T("Joining your friend's game through Steam..."));
    mp_start("join");
}

void mp_start(const std::string& mode)
{
    if (g_session.active()) { g_session.stop(); forgetEveryone(); }
    if (mode != "host") steam_onHosting(false, 0);
    g_cfg.mode = mode;
    g_permanentFailure = false;
    saveConfig();
    startSession();
}

void mp_leave()
{
    if (g_session.active()) { g_session.stop(); forgetEveryone(); }
    steam_leave();
    g_cfg.mode = "off";   // no automatic reconnection
    saveConfig();
    showMessage(T("Multiplayer: you left the session."));
}

} // namespace kmp

using namespace kmp;

// --- lobby window (lobby_key): driven by the title screen's update, then by the game loop ------------
namespace kmp { namespace {
    DWORD g_lastLobbyTick = 0;
    void lobbyFrame()
    {
        lobby_tick();
        static bool down = false;
        if (keyPressed(g_cfg.lobbyKey, down)) lobby_toggle();
        g_lastLobbyTick = GetTickCount();
    }
} }
void (*titleUpdate_orig)(TitleScreen*) = NULL;
// Test runs: get into a game without any mouse or keyboard input, by pressing the game's own
// buttons by name (as a click would). autotest_load=new: NEW GAME, BEGIN (default start), then
// CONFIRM in the character editor. Any other value: CONTINUE (the test script points settings.cfg's
// "continue" at the test save). Loading a save directly from the title screen crashes the game:
// these buttons prepare the world first.
static void autotestClicks()
{
    static int step = 0;
    static DWORD last = 0, started = 0;
    if (g_cfg.autotestLoad.empty() || step < 0) return;
    DWORD now = GetTickCount();
    if (!started) started = now;
    if (now - started < 5000 || now - last < 1000) return;
    last = now;
    static const char* const newGame[] = { "NewGameButton", "BeginButton", "ConfirmButton" };
    static const char* const resume[] = { "ContinueButton" };
    bool fresh = g_cfg.autotestLoad == "new";
    const char* const* seq = fresh ? newGame : resume;
    int n = fresh ? 3 : 1;
    if (lobby_pressTitleButton(seq[step]))
    {
        log("autotest: pressed %s", seq[step]);
        if (++step >= n) step = -1;
    }
    else if (now - started > 240000) { log("autotest: gave up waiting for %s", seq[step]); step = -1; }
}

void titleUpdate_hook(TitleScreen* self)
{
    titleUpdate_orig(self);
    titleEvents();
    lobby_titleButton(true);
    lobbyFrame();
    autotestClicks();
    steamFrame();
}

// --- hook: GameWorld::mainLoop_GPUSensitiveStuff (once per frame) -----------------------------
void (*mainLoop_orig)(GameWorld*, float) = NULL;
// Debug: frame time statistics every 30 s (a periodic hitch shows as regular slow frames).
static void frameStats()
{
    static LARGE_INTEGER freq = { 0 }, last = { 0 };
    static double sum = 0, worst = 0; static int frames = 0, slow = 0; static DWORD since = 0;
    static std::vector<double> times;
    if (!g_cfg.debugKeys) return;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (last.QuadPart)
    {
        double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
        sum += ms; ++frames; if (ms > worst) worst = ms; if (ms > 50.0) ++slow;
        times.push_back(ms);
    }
    last = now;
    if (!since) since = GetTickCount();
    if (GetTickCount() - since >= 30000 && frames > 0)
    {
        std::sort(times.begin(), times.end());
        double p99 = times[(size_t)(times.size() * 0.99)];
        log("frames: %d in 30 s, mean %.1f ms, p99 %.1f ms, worst %.1f ms, %d over 50 ms", frames, sum / frames, p99, worst, slow);
        sum = worst = 0; frames = slow = 0; times.clear(); since = GetTickCount();
    }
}

void mainLoop_hook(GameWorld* thisptr, float time)
{
    frameStats();
    server_frame();
    if (!g_started) { g_started = true; startSession(); }
    if (GetTickCount() - g_lastLobbyTick > 100) { lobby_titleButton(false); lobbyFrame(); }   // in game (title screen hook idle)
    if (ou && ou->isPaused()) lobby_pauseButton();
    autotestClicks();   // character editor of a new test game
    steamFrame();       // Steam callbacks, invitations accepted

    bool live = ready() && worldLoaded();
    if (live) chars_preFrame();
    pump();
    server_tick(GetTickCount());
    mainLoop_orig(thisptr, time);
    // The engine has updated and placed every body for this frame: now draw ghosts on their
    // smoothed network path (render layer), right before the frame is presented.
    if (live) chars_renderTick(GetTickCount());
    if (live) chars_showPlayerNames(); else chars_clearPlayerNames();
    if (live) chars_updateMapNames();
}

__declspec(dllexport) void startPlugin()
{
    InitializeCriticalSection(&g_lock);
    loadConfig();
    log("KenshiMP v%u starting (the version is the network protocol number)", (unsigned)PROTOCOL_VERSION);
    lang_init();
    bool ok = true;
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
                                                 mainLoop_hook, &mainLoop_orig))
    { ErrorLog("KenshiMP: could not hook the main loop!"); ok = false; }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&TitleScreen::_NV_update), titleUpdate_hook, &titleUpdate_orig))
    { ErrorLog("KenshiMP: could not hook the title screen (multiplayer window only in game)"); }
    if (!chars_install()) ok = false;
    if (g_cfg.syncBuildings && !builds_install()) ok = false;
    world_install();
    if (!npcs_install()) ok = false;
    if (!items_install()) ok = false;
    weather_install();
    towns_install();
    if (!ok)
    {
        g_hooksOk = false;
        problem("hooks", "some game functions were not found (this Kenshi version is not supported): multiplayer is switched off to protect your saves");
    }
    if (ok) DebugLog("KenshiMP: plugin loaded (mode=" + g_cfg.mode + ")");
}
