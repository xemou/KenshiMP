// State and helpers shared by the plugin modules (KenshiMP.cpp, Characters.cpp, Buildings.cpp).
#pragma once
#include "../core/Session.h"

#include <windows.h>
#include <string>
#include <vector>
#include <utility>
#include <ogre/OgreVector3.h>

class Faction;
class Character;
class Building;
class RootObject;
class RootObjectBase;
class Inventory;

namespace kmp {

struct Config
{
    std::string mode;      // "off", "host", "join" or "server" (dedicated server, Server.cpp)
    std::string address;
    int port;
    std::string name;
    std::string faction;
    float relation;        // initial relation towards other players (-100 war .. 100 allies)
    std::string ghostAI;   // "none" (only execute our move orders), "full" (vanilla AI)
    bool syncAppearance;
    bool syncEquipment;
    bool syncBuildings;
    bool debugKeys;        // F11 = spawn a test building, F8 = clock +3 h
    bool npcSync;          // host's world NPCs replace the clients' own
    bool strictMods;       // host refuses players whose mod list differs
    bool autoReconnect;    // client retries every 10 s after a disconnect
    bool pauseSync;        // host's pause applies to everybody
    bool renderSmoothing;  // draw remote characters on their smoothed path (render layer)
    std::string password;  // optional session password (host and clients must match)
    bool weatherSync;      // host's weather applies to everybody
    int lobbyKey;          // virtual key of the multiplayer window (default F4; CheatMenu uses F6)
    std::string language;  // "auto" (the game's language), "fr" or "en"
    bool playersOnMap;     // other players' squads on the world map (when not at war)
    // Ghost movement tuning switches (A/B measurements; all on by default).
    bool tuneSpeedMatch, tuneTrail, tuneRepath, tuneUnblock, tuneOnScreen;
    bool ghostNoCollide;   // experimental (off): ghosts are not pushed aside by nearby bodies
    bool loadSharing;      // host: a client far from the host simulates its own surroundings
    bool assaultHostility; // attacking a player's characters outside a war lowers their faction's relation
    bool playerNames;      // other players' characters always show their name above their head
    int playerNamesRange;  // how far from the camera those names show (0 = any distance)
    bool speedVote;        // host: a game speed applies only when every player chose it (0 = anybody's change applies to all)
    bool townSync;         // host's world states (unique NPCs dead/jailed) and town changes apply to everybody
    int backupSaves;       // host: backups kept per save (copied when a hosted game is ready), 0 = off
    std::string autotestLoad;  // test: save loaded automatically from the title screen
    bool autotest;             // test: scripted checks once another player is there (log "autotest:")
    int npcRange;              // host: how far around a player the host's NPCs are shown to it (0 = the game's own "npc range", 3250 by default)
    int autotestShots;         // test: save a frame every N seconds (kmp_shot_<role>_<n>.png)
    bool autotestCarry;        // test: carry a knocked-out ghost, then get knocked out and carried
    bool autotestBuild;        // test: buildings placed on each side, seen on the other; world map opened with the players' names
    bool autotestGotoShop;     // test: the squad is moved next to the nearest shop (shop shelves in reach)
    std::string lobbyKeyName;
    // Network transport and dedicated servers (DEDICATED_SERVER_PLAN.md)
    std::string transport;         // "tcp" (default, games between friends) or "udp" (both sides must use the same)
    std::string accountPassword;   // client: our account password on dedicated servers ("" = use password=)
    std::string serverName, serverDescription, serverMotd;
    int maxPlayers;                // server: player slots
    bool serverAccounts;           // server: name + password per player
    std::string serverAdmins;      // server: admin account names, separated by commas or spaces
    std::string serverDataDir;     // server: accounts, bans, player records, command file ("" = <config dir>\server)
    bool serverConsole;            // server: a console window for admin commands
    std::string serverWindow;      // server: "normal", "minimize" or "hide" once the world is loaded
    bool serverRender;             // server: false = the game draws nothing (saves the GPU)
    int serverFps;                 // server: frame rate cap (0 = none)
    int serverAutosaveMin;         // server: world saved every N minutes (0 = never automatically)
    std::string serverSave;        // server: save name used for the world
    std::string serverStartSave;   // server: save of the starting world ("resetworld")
    std::string serverWorld;       // server: "continue" (its last save, default) or "new" (a new game) at start
    std::string serverListUrl;     // server list service (http://host:port/), servers announce there, clients read it
    std::string serverPublicAddress; // server: address shown in the list ("" = the one the list sees)
    int serverMaxPerAddress;       // server: connections from one IP address
    int serverNpcCap;              // server: most NPCs sent to one client (closest first)
    int serverNpcBudget;           // server: KB per second of NPC states per client (0 = no budget)
    int logMaxMB;                  // log file rotated above this size (2 files kept)
    int serverKickRefusals;        // server: that many refused actions (impossible moves, hits from afar) -> kicked (0 = never)
    Config() : mode("off"), address("127.0.0.1"), port(47000), name("Player"), faction("My Faction"),
               relation(0), ghostAI("none"), syncAppearance(true), syncEquipment(true), syncBuildings(true),
               debugKeys(false), npcSync(true), strictMods(false), autoReconnect(true), pauseSync(true),
               renderSmoothing(true), weatherSync(true), lobbyKey(VK_F4), lobbyKeyName("F4"), language("auto"), playersOnMap(true),
               tuneSpeedMatch(true), tuneTrail(true), tuneRepath(true), tuneUnblock(true), tuneOnScreen(true), ghostNoCollide(false), loadSharing(false), assaultHostility(false), playerNames(true), playerNamesRange(0), speedVote(true), townSync(true), backupSaves(5), autotest(false), autotestGotoShop(false), autotestCarry(false), autotestBuild(false), autotestShots(0), npcRange(0),
               transport("tcp"), serverName("KenshiMP server"), maxPlayers(16), serverAccounts(true), serverConsole(false), serverWindow("normal"),
               serverRender(true), serverFps(0), serverAutosaveMin(10), serverSave("kenshimp_server"), serverMaxPerAddress(4), serverNpcCap(256),
               serverNpcBudget(48), logMaxMB(8), serverKickRefusals(0) {}
};

extern mp::Session g_session;
extern Config g_cfg;

// True once we have a valid player id (host: immediately, client: after the handshake).
bool ready();

// Server.cpp: dedicated server (mode=server) and the client side of it
bool server_isServer();                          // this game is a dedicated server (it hosts, it has no player of its own)
void server_configureSession();                  // before host()/join(): transport, accounts, limits
void server_frame();                             // every frame: frame cap, frame statistics
void server_tick(DWORD now);                     // console, saves, records, counters, server list
bool server_onMessage(const mp::NetEvent& e);    // true = consumed (admin command, bag check, notice)
void server_onPlayerJoined(uint8_t id, const std::string& name);
void server_onPlayerLeft(uint8_t id);
void server_clientTick(DWORD now);               // client of a dedicated server: bag fingerprints
void server_countNpcStates(int sent, int skipped);
void server_refused(uint8_t player, const char* why);   // server: something a player sent was refused (counted, logged)
const float MAX_HIT_DISTANCE = 1200.f;                  // server: farther than this from its victim, a hit is refused (longest crossbows ~ 1000)
bool server_loadSave(const std::string& name);   // KenshiMP.cpp: load a save (resetworld)
void backup_serverSave();                        // Backup.cpp: copy the current save folder now (timestamped)
void server_logLine(const char* line);           // server: log file of its own (<data>\server.log, rotated)

void log(const char* fmt, ...);
void problem(const char* key, const char* fmt, ...);   // a check that failed: "PROBLEM:" line in the log (once a minute per key), kept for the bug report
std::vector<std::string> problems_recent();            // the last ones (bug report)
bool mp_hooksOk();                                     // false: some game functions were not found, multiplayer stays off
bool mp_worldLoaded();                   // a game is loaded (our squad exists)
std::string mp_modList();                // "game <version>;mod1;mod2..." as checked at handshake
std::string mp_configDir();              // %LOCALAPPDATA%\kenshi\KenshiMP\ (with the final backslash)
std::string report_write();              // Report.cpp: bug report folder on the desktop, returns the text to show
void backup_hostSave();                  // Backup.cpp: host, game ready: copy its save folder (background thread)
void backup_tick();                      // main thread: reports a finished copy

// Lang.cpp: the mod speaks the game's language (texts are written in English, translated here)
void lang_init();
bool lang_french();
const char* T(const char* english);                 // translated text
std::string TF(const char* englishFormat, ...);     // translated printf-style format
std::string lang_reason(const std::string& text);   // network refusal/disconnect reasons
void showMessage(const std::string& s);
std::string playerName(uint8_t id);

// Local faction mirroring a remote player (created on demand).
Faction* factionFor(uint8_t player);
// A save was loaded / the world rebuilt: drop every cached engine pointer.
void onWorldReload();
void chars_onWorldReload();
void builds_onWorldReload();

// Guards data touched from engine hooks that may run outside the main loop.
extern CRITICAL_SECTION g_lock;
struct Lock
{
    Lock() { EnterCriticalSection(&g_lock); }
    ~Lock() { LeaveCriticalSection(&g_lock); }
};

// Characters.cpp
bool chars_install();
void chars_tick(DWORD now);
void chars_onMessage(const mp::NetEvent& e);
void chars_onPlayerLeft(uint8_t id);
void chars_forgetMovement(uint8_t id);   // that player reloaded / joined: its next positions may jump
void chars_resendAll();
void chars_purgeStale();
void chars_logHitStats();
uint32_t chars_netIdOf(Character* c);      // local player character or ghost, 0 otherwise
Character* chars_byNetId(uint32_t id);
bool chars_isGhost(Character* c);
int chars_ghostTotal();
void chars_preFrame();                   // before the engine's frame update
void chars_renderTick(DWORD now);        // after it, before drawing: render-layer smoothing
void chars_showPlayerNames();            // other players' names drawn above their characters (player_names)
std::string chars_debugOpenMap(bool open);  // test: management screen opened on its map tab (or closed)
void chars_updateMapNames();            // every other player named on the world map at their last known position
void chars_clearPlayerNames();           // remove those labels (out of game, world reloaded)
void chars_logRenderStats();
void chars_releaseGhost(uint32_t id);
void chars_claimNpc(uint32_t id, Character* c);   // take over a world NPC (host drops its copy)
void chars_claimWhenNear(uint32_t id, Character* c);  // same, once one of our characters is next to it (host range)
bool chars_isClaimed(Character* c);
Character* chars_nextGhost(Character* after);
std::string chars_debugShoot(Character* me);   // test: our character fires at a ghost   // cycles through the ghosts (debug camera)
void chars_localCharacters(std::vector<std::pair<uint32_t, Character*> >& out);   // our replicated squad
void chars_npcGhostsNear(const std::vector<Ogre::Vector3>& centres, float radius, std::vector<uint32_t>& out);   // host NPC ghosts within radius of a centre
void chars_traderPositions(std::vector<Ogre::Vector3>& out);   // the host shopkeepers' ghosts we see (their shop shelves are mirrored)
void chars_playerGhosts(std::vector<std::pair<uint32_t, Character*> >& out);      // other players' characters here
bool chars_playerPosition(uint8_t owner, Ogre::Vector3& out);   // last known position of one of its characters
std::string chars_regroupTo(uint8_t owner);   // move our selected characters next to that player's (text to show)
void chars_onRegroup(uint8_t who, uint8_t target);   // MSG_REGROUP received
Character* chars_ghostNear(uint8_t owner, const Ogre::Vector3& pos, float maxDist); // that player's nearest ghost
bool chars_debugAttack(Character* attacker, Character* target);   // debug: attack order
std::string chars_debugCarryGhost(Character* me);      // test: pick up the first knocked-out ghost
std::string chars_debugCarryState();                  // test: state of the ghosts we may have picked up
std::string chars_debugKnockOutSelf(Character* me, float seconds);   // test: our character is knocked out
std::string chars_debugHeldReport(Character* me);     // test: our characters held by another player
std::string chars_debugTalk(Character* me);             // test: talk to the nearest NPC with dialogue
std::string chars_debugTalkState();                   // test: what became of that conversation
std::string chars_debugWoundSelf(Character* me);       // test: wound our character
std::string chars_debugHealGhost(Character* me);       // test: first aid on the copy of the other player's character
std::string chars_debugHealSelf(Character* me);        // test: control, first aid on our own character
std::string chars_debugGhostMedical();                 // test: medical state of the player copy
std::string chars_debugFleshSelf(Character* me);       // test: our wounded part
void chars_debugShopView();                           // test: the shop inventory the trade window would show
std::string chars_debugClaimNpc(Character* me);         // test (AUTHORITY_PLAN step C): claim the nearest host NPC copy
void chars_debugAuthority();                          // test (AUTHORITY_PLAN step A): one 'auth:' log line per own character and per copy
void chars_debugNpcCensus();                          // test: NPCs around us: copy or real, trader, dialogue
bool chars_screenshot(const std::string& file);        // test: the game's frame saved to a PNG
void chars_debugFocus(Character* c, float rightOffset, float forwardOffset = 0.f);   // test: camera on c, c drawn right of the interface windows
std::string chars_debugPickLock(Character* me);       // test: order our caged character to pick the lock

// Buildings.cpp
bool builds_install();
void builds_tick(DWORD now);
void builds_onMessage(const mp::NetEvent& e);
void builds_onPlayerLeft(uint8_t id);
void builds_resendAll();
void builds_purgeStale();
void builds_debugSpawn();
uint32_t builds_netIdOf(Building* b);      // local tracked building or ghost, 0 otherwise
Building* builds_byNetId(uint32_t id);
Building* builds_findWorld(const std::string& sid, float x, float y, float z);   // nearest of that type within 3 m

// Targets (Characters.cpp): engine object <-> network reference
mp::TargetRef makeTargetRef(RootObjectBase* o);
RootObject* resolveTargetRef(const mp::TargetRef& t);

// Npcs.cpp: world NPCs from the host
bool npcs_install();
void npcs_tick(DWORD now);
void npcs_onPlayerLeft(uint8_t id);
void npcs_reset();                       // world reloaded / session ended
void npcs_onZoneMode(const mp::NetEvent& e);   // client: the host says who simulates the world around us
void npcs_onClaim(uint8_t client, const mp::Bytes& body);
// Authority table (core/Authority.h; Npcs.cpp): who simulates each entity. Host writes, clients mirror.
uint8_t auth_owner(uint32_t netId);
uint16_t auth_epoch(uint32_t netId);
size_t auth_size();
void auth_tick(DWORD now);
bool auth_checkSender(uint32_t netId, uint8_t sender);   // false (and logged) when somebody but the owner sent its state
long auth_violations();
void auth_onMessage(const mp::NetEvent& e);
void auth_onPlayerLeft(uint8_t player);
void chars_onHandoff(uint32_t netId, uint8_t action, uint16_t epoch);   // client: the host's answer to our claim
bool npcs_ownWorld();                    // client: we simulate our own surroundings (load sharing, far from the host)
bool npcs_clientOwnWorld(uint8_t client); // host: that client simulates its own surroundings

// Items.cpp: inventories and item transfers
bool items_install();
void items_tick(DWORD now);
void items_onMessage(const mp::NetEvent& e);
void items_onPlayerLeft(uint8_t id);
void items_onWorldReload();
void items_worldForget(uint8_t player);   // that player reloaded / left: its open town containers are forgotten
mp::Bytes items_inventoryMsg(uint8_t kind, uint32_t id, Inventory* inv, int money = -1);   // body of MSG_INVENTORY
int items_moneyOf(Character* c);
Inventory* items_backpackOf(Character* c);                 // worn backpack's own inventory, NULL if none
mp::Bytes items_backpackMsg(uint32_t id, Character* c);    // MSG_INVENTORY body of it, empty if none   // squad money (NPC traders), -1 if unknown
std::string items_debugLoot(Character* taker);
std::string items_debugShopTrade();                  // test: pretend a trade with a shopkeeper is open for a few seconds
std::string items_debugBuyFromShelf(Character* taker); // test: the first item of a mirrored shop shelf moves to taker
std::string items_debugBackpack(Character* taker);
std::string items_debugGiveOne(Character* c);      // test: one item in c's inventory
std::string items_debugEquipCrossbow(Character* c); // test: a crossbow in c's hands
void items_sendUndo(uint8_t to, uint8_t action, uint8_t kind, uint32_t id, const mp::InvItem& d);   // owner refused a transfer

// Trade.cpp: direct trade between players in the game's trade window
void trade_request(uint8_t player);          // ask that player, or accept their offer
bool trade_pendingFrom(uint8_t player);      // they offered a trade (not answered yet)
void trade_onMessage(const mp::NetEvent& e);
void trade_tick(DWORD now);
void trade_onPlayerLeft(uint8_t id);
void trade_onWorldReload();
void trade_endAll();
bool lobby_pressTitleButton(const char* suffix);   // test runs: click a title screen button by name
void lobby_factionsTick();

// Steam.cpp: connections through Steam (no IP / port / VPN), invitations, "Join game"
struct SteamFriend { unsigned long long id; std::string name; bool online, inKenshi; SteamFriend() : id(0), online(false), inKenshi(false) {} };
bool steam_init();                                   // lazy, once Steam is up (no-op on GOG)
bool steam_available();
unsigned long long steam_myId();
std::string steam_personaName();
void steam_friends(std::vector<SteamFriend>& out);
bool steam_invite(unsigned long long friendId);
void steam_onHosting(bool hosting, int port);
int steam_tunnelTo(unsigned long long host);         // local port for the session to join (0: failed)
void steam_leave();
bool steam_takeJoinRequest(unsigned long long& host);   // runs Steam callbacks; an invitation accepted?                          // diplomacy buttons in the game's Factions screen
bool lobby_debugOpenFactions(uint8_t player, bool open);   // test: show that player's faction there
std::string ground_debugGiveBag(Character* c);     // test: a backpack holding two items, put in c's inventory
std::string ground_debugDropBag(Character* c);     // test: drop that backpack on the ground                         // close our trade window (and tell the partner)   // debug: first item out of a nearby remote backpack
std::string ground_debugPickup(Character* taker);   // debug: nearest ground item -> taker
void items_onDespawn(uint32_t ghostId);         // a ghost body was removed: forget per-body item state
void items_redressed(uint32_t ghostId);        // Characters.cpp re-dressed that ghost: put its backpack back   // debug: first item of an open remote inventory -> taker
// While non-zero, inventory changes are our own mirroring, never forwarded as transfers.
extern volatile long g_itemsMute;
extern volatile long g_itemsEpoch;   // bumped by every change of our own to ghost inventories

// Lobby.cpp: connection window (lobby_key, default F4) and the title screen button
void lobby_titleButton(bool show);
void lobby_pauseButton();   // adds MULTIPLAYER under the pause menu when it is open
void lobby_toggle();
bool lobby_isOpen();
void lobby_tick();
void lobby_setError(const std::string& e);
// KenshiMP.cpp: (re)start the session as "host" or "join" with g_cfg, or leave it; both save kenshimp.cfg.
void mp_start(const std::string& mode);
void mp_leave();

// Ground.cpp: items dropped on the ground
void ground_tick(DWORD now);
void ground_onMessage(const mp::NetEvent& e);
void ground_resendAll();
void ground_onPlayerLeft(uint8_t id);
void ground_onWorldReload();

// Chat.cpp: chat box (Enter) and commands
void chat_open();
bool chat_mayOpen();   // no text field of the game has the keyboard right now
void chat_received(uint8_t from, const std::string& cleanText);   // add a line to the chat history
void chat_notice(const std::string& text);                          // grey line (joins, wars...)
bool chat_isOpen();
void chat_tick();
void chat_onWorldReload();
std::string chat_clean(const std::string& in);   // strip control characters, escape colour codes
const char* chat_playerColour(uint8_t id);       // MyGUI colour code of that player (chat, name labels)

// KenshiMP.cpp: diplomacy with one player (id) or everybody (-1); value -100 war .. 100 allies
void diplomacy_set(int player, float value);
float diplomacy_relation(uint8_t player);
void diplomacy_assaulted(uint8_t player);   // that player hurt one of our characters (assault_hostility)   // our relation with that player's faction (0 if unknown)

// Weather.cpp: weather from the host
bool weather_install();
void weather_tick(DWORD now);
void weather_onMessage(const mp::NetEvent& e);
void weather_onWorldReload();
void weather_sendNow();                  // host: send it at the next tick (join, resync)

// World.cpp: time of day / weather from the host
bool world_install();
void world_tick(DWORD now);
void world_reset();
void world_sendNow();                    // host: send the clock at the next tick (join, resync)
void world_onMessage(const mp::NetEvent& e);
void world_onSpeed(const mp::NetEvent& e);      // another player's chosen game speed
void world_onPlayerLeft(uint8_t id);                // forget its chosen speed
void world_speedTick(DWORD now);                    // vote: everybody on the same speed applies, else x1; free: the latest change applies
float world_debugSpeedWish();                       // test: the speed we asked for
void world_debugShift();

// Towns.cpp: world states (unique NPCs dead / jailed) and town changes from the host
bool towns_install();
void towns_tick(DWORD now);
void towns_onMessage(const mp::NetEvent& e);
void towns_resendAll();
void towns_onWorldReload();
std::string towns_debugUnique();      // test: a unique NPC's id
std::string towns_debugFarTown();     // test: "name;x,y,z" of a town 8000-20000 units away
std::string towns_debugOverride();    // test: "townSid;overrideSid" for a far town that has another version
std::vector<std::string> builds_debugCensus(float radius);   // test: furnished / empty structures around our first character
void npcs_debugAuthority();                                 // test (AUTHORITY_PLAN step A): one 'auth:' log line per NPC the host serves
std::string npcs_debugCensus();                               // test: local NPCs, host NPC ghosts, zone state around our first character
std::string builds_debugShop();       // test: "sid;x;y;z" of the nearest shop building
std::string builds_debugGotoShop();   // test: moves our squad next to it
std::string builds_debugPlace(bool completed, float side);   // test: a building / blueprint of ours next to our first character
std::string builds_debugProgress(float progress);           // test: our blueprints get that construction progress
std::string builds_debugReport();                           // test: our buildings and the copies of others'
std::string builds_debugTownDoor();   // test: "sid;x;y;z;door;doorCount" of a nearby town building

// Bounties.cpp: crimes seen in the host's world reach the real character
void bounties_tick(DWORD now);
void bounties_onMessage(const mp::NetEvent& e);
void bounties_resendAll();
void bounties_onPlayerLeft(uint8_t id);
void bounties_onWorldReload();
std::string bounties_debugFaction();
std::string bounties_debugCrimeOnGhost(const std::string& factionSid);

} // namespace kmp
