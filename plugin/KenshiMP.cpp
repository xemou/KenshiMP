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
            else if (k == "autotest_load") g_cfg.autotestLoad = v;
            else if (k == "autotest") g_cfg.autotest = toBool(v);
            else if (k == "lobby_key")
            {
                // F1..F12
                if ((v[0] == 'F' || v[0] == 'f') && atoi(v.c_str() + 1) >= 1 && atoi(v.c_str() + 1) <= 12)
                { g_cfg.lobbyKey = VK_F1 + atoi(v.c_str() + 1) - 1; g_cfg.lobbyKeyName = "F" + v.substr(1); }
            }
            else if (k == "password") g_cfg.password = v;
        }
        fclose(f);
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
        const char* keys[] = { "mode", "address", "port", "name", "faction", "password" };
        std::string vals[] = { g_cfg.mode, g_cfg.address, port, g_cfg.name, g_cfg.faction, g_cfg.password };
        for (int k = 0; k < 6; ++k)
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
                g_players[e.player.id] = e.player;
                if (g_session.isHost()) saveSlots();
                g_relationPending.insert(e.player.id);
                break;
            case NetEvent::EV_PLAYER_LEFT:
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
            onPlayerLeft(e.player.id);
            showMessage(TF("%s left the game", who.c_str()) + (e.text.empty() ? "" : " (" + lang_reason(e.text) + ")"));
            chat_notice(TF("%s left the game", who.c_str()));
            break;
        }
        case NetEvent::EV_MESSAGE:
            // No game world yet (title screen, loading): world messages cannot be applied.
            // They are not lost: once our world exists we send MSG_RESYNC and get them again.
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
        if (step == 87 && s >= 110) { step = -1; chars_logHitStats(); log("autotest: lobby open after close -> %d; done", (int)lobby_isOpen()); }
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
        g_session.setModList(modList(), g_cfg.strictMods);
        g_session.setPassword(g_cfg.password);
        std::string err;
        bool ok = false;
        if (g_cfg.mode == "host")
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
            PlayerInfo me; me.id = HOST_ID; me.name = g_cfg.name; me.faction = g_cfg.faction;
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
            log("autotest: unique %s", towns_debugUnique().c_str());
            log("autotest: town override %s", towns_debugOverride().c_str());
            log("autotest: far town %s", towns_debugFarTown().c_str());
            log("autotest: faction %s", bounties_debugFaction().c_str());
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

        // Fast-forward is disabled for now: everybody simulates at x1.
        if (ou->getFrameSpeedMultiplier() > 1.0f) ou->setGameSpeed(1.0f, false);

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
            if (!mine.empty() && mine[0].second && chars_playerPosition(HOST_ID, hostAt))
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
        items_tick(now);
        trade_tick(now);
        weather_tick(now);
        towns_tick(now);
        bounties_tick(now);
        ground_tick(now);

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
    va_list a; va_start(a, fmt); vsprintf_s(buf, sizeof(buf), fmt, a); va_end(a);
    DebugLog(std::string("KenshiMP: ") + buf);
}

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
    if (!g_started) { g_started = true; startSession(); }
    if (GetTickCount() - g_lastLobbyTick > 100) { lobby_titleButton(false); lobbyFrame(); }   // in game (title screen hook idle)
    if (ou && ou->isPaused()) lobby_pauseButton();
    autotestClicks();   // character editor of a new test game
    steamFrame();       // Steam callbacks, invitations accepted

    bool live = ready() && worldLoaded();
    if (live) chars_preFrame();
    pump();
    mainLoop_orig(thisptr, time);
    // The engine has updated and placed every body for this frame: now draw ghosts on their
    // smoothed network path (render layer), right before the frame is presented.
    if (live) chars_renderTick(GetTickCount());
    if (live) chars_showPlayerNames(); else chars_clearPlayerNames();
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
    if (ok) DebugLog("KenshiMP: plugin loaded (mode=" + g_cfg.mode + ")");
}
