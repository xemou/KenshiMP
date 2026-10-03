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
    std::string mode;      // "off", "host" or "join"
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
    bool townSync;         // host's world states (unique NPCs dead/jailed) and town changes apply to everybody
    std::string autotestLoad;  // test: save loaded automatically from the title screen
    bool autotest;             // test: scripted checks once another player is there (log "autotest:")
    std::string lobbyKeyName;
    Config() : mode("off"), address("127.0.0.1"), port(47000), name("Player"), faction("My Faction"),
               relation(0), ghostAI("none"), syncAppearance(true), syncEquipment(true), syncBuildings(true),
               debugKeys(false), npcSync(true), strictMods(true), autoReconnect(true), pauseSync(true),
               renderSmoothing(true), weatherSync(true), lobbyKey(VK_F4), lobbyKeyName("F4"), language("auto"), playersOnMap(true),
               tuneSpeedMatch(true), tuneTrail(true), tuneRepath(true), tuneUnblock(true), tuneOnScreen(true), ghostNoCollide(false), loadSharing(false), assaultHostility(false), playerNames(true), townSync(true), autotest(false) {}
};

extern mp::Session g_session;
extern Config g_cfg;

// True once we have a valid player id (host: immediately, client: after the handshake).
bool ready();

void log(const char* fmt, ...);

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
void chars_showPlayerNames();            // other players' name tags stay visible (player_names)
void chars_logRenderStats();
void chars_releaseGhost(uint32_t id);
void chars_claimNpc(uint32_t id, Character* c);   // take over a world NPC (host drops its copy)
void chars_claimWhenNear(uint32_t id, Character* c);  // same, once one of our characters is next to it (host range)
bool chars_isClaimed(Character* c);
Character* chars_nextGhost(Character* after);
std::string chars_debugShoot(Character* me);   // test: our character fires at a ghost   // cycles through the ghosts (debug camera)
void chars_localCharacters(std::vector<std::pair<uint32_t, Character*> >& out);   // our replicated squad
void chars_playerGhosts(std::vector<std::pair<uint32_t, Character*> >& out);      // other players' characters here
Character* chars_ghostNear(uint8_t owner, const Ogre::Vector3& pos, float maxDist); // that player's nearest ghost
bool chars_debugAttack(Character* attacker, Character* target);   // debug: attack order

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
bool npcs_ownWorld();                    // client: we simulate our own surroundings (load sharing, far from the host)
bool npcs_clientOwnWorld(uint8_t client); // host: that client simulates its own surroundings

// Items.cpp: inventories and item transfers
bool items_install();
void items_tick(DWORD now);
void items_onMessage(const mp::NetEvent& e);
void items_onPlayerLeft(uint8_t id);
void items_onWorldReload();
mp::Bytes items_inventoryMsg(uint8_t kind, uint32_t id, Inventory* inv, int money = -1);   // body of MSG_INVENTORY
int items_moneyOf(Character* c);
Inventory* items_backpackOf(Character* c);                 // worn backpack's own inventory, NULL if none
mp::Bytes items_backpackMsg(uint32_t id, Character* c);    // MSG_INVENTORY body of it, empty if none   // squad money (NPC traders), -1 if unknown
std::string items_debugLoot(Character* taker);
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

// KenshiMP.cpp: diplomacy with one player (id) or everybody (-1); value -100 war .. 100 allies
void diplomacy_set(int player, float value);
float diplomacy_relation(uint8_t player);
void diplomacy_assaulted(uint8_t player);   // that player hurt one of our characters (assault_hostility)   // our relation with that player's faction (0 if unknown)

// Weather.cpp: weather from the host
bool weather_install();
void weather_tick(DWORD now);
void weather_onMessage(const mp::NetEvent& e);
void weather_onWorldReload();

// World.cpp: time of day / weather from the host
bool world_install();
void world_tick(DWORD now);
void world_reset();
void world_onMessage(const mp::NetEvent& e);
void world_debugShift();

// Towns.cpp: world states (unique NPCs dead / jailed) and town changes from the host
bool towns_install();
void towns_tick(DWORD now);
void towns_onMessage(const mp::NetEvent& e);
void towns_resendAll();
void towns_onWorldReload();
std::string towns_debugUnique();      // test: a unique NPC's id
std::string towns_debugOverride();    // test: "townSid;overrideSid" for a far town that has another version
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
