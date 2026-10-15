// Host/client session. Star topology: the host accepts clients and relays their messages.
// All public methods are called from the game thread; networking runs on its own thread and
// hands results over through a locked event queue (poll it once per frame).
//
// Robustness:
//  - dead peers are detected by silence (TIMEOUT_MS) : clients ping every PING_MS, the host
//    answers, so both sides always see traffic from a live peer;
//  - a slow peer never makes memory grow without bound: above SOFT_BACKLOG bytes pending, the
//    superseded high-rate messages (entity states) are skipped for it; above HARD_BACKLOG it
//    is disconnected;
//  - the handshake carries the protocol version and the list of active game mods; the host
//    refuses mismatches (or only warns, see setModList).
//
// Transports: TCP (default, games between friends, Steam tunnel) or UDP (dedicated server): a reliable ordered
// stream plus unreliable sequenced entity states (core/Reliable.h), so one lost datagram never stalls the rest.
// Dedicated server (setServer): accounts with a challenge/response login (core/Accounts.h), bans, per-address and
// flood limits, more slots, kick, per-player traffic counters, and a "server info" answer for the server list.
#pragma once
#include "Protocol.h"
#include <map>

namespace mp {

const unsigned PING_MS      = 2000;
const unsigned TIMEOUT_MS   = 15000;
const unsigned CONNECT_TIMEOUT_MS = 5000;   // client: host unreachable after this long
const size_t   SOFT_BACKLOG = 512 * 1024;
const size_t   HARD_BACKLOG = 8 * 1024 * 1024;

struct PlayerInfo
{
    uint8_t id;
    std::string name;
    std::string faction;
    PlayerInfo() : id(0) {}
};

struct NetEvent
{
    enum Kind
    {
        EV_CONNECTED,       // client only: handshake done, localId() valid
        EV_DISCONNECTED,    // client only: connection lost / rejected (text = reason)
        EV_PLAYER_JOINED,   // player = info
        EV_PLAYER_LEFT,     // player.id, text = reason
        EV_MESSAGE,         // msgType, sender, body
        EV_WARNING          // text (e.g. mod list differs but strict mode is off)
    };
    Kind kind;
    uint8_t msgType;
    uint8_t sender;
    PlayerInfo player;
    std::string text;
    Bytes body;
    NetEvent() : kind(EV_MESSAGE), msgType(0), sender(0) {}
};

// Cumulative traffic counters (for bandwidth display).
struct NetStats
{
    uint64_t bytesIn, bytesOut;
    uint32_t statesDropped;      // superseded states skipped for a congested peer
    uint32_t resent, lost;       // UDP: reliable segments sent again, unreliable messages that never completed
    NetStats() : bytesIn(0), bytesOut(0), statesDropped(0), resent(0), lost(0) {}
};

// Per connected player (server console, budget logs).
struct PeerStats
{
    uint8_t id;
    std::string name, address;
    uint64_t bytesIn, bytesOut;
    uint32_t statesDropped;
    unsigned rttMs;              // UDP: smoothed round trip
    bool admin;
    uint32_t refused;            // server: states refused (impossible movement, someone else's character)
    PeerStats() : id(0), bytesIn(0), bytesOut(0), statesDropped(0), rttMs(0), admin(false), refused(0) {}
};

enum Transport { TRANSPORT_TCP = 0, TRANSPORT_UDP = 1 };

// Dedicated server settings (Session::setServer).
struct ServerOptions
{
    bool dedicated;              // the host is a server, not a player (told to clients in WELCOME)
    bool accounts;               // name + password per player (dataDirccounts.txt)
    std::string dataDir;         // accounts.txt, bans.txt
    std::string name, description;
    int maxPlayers;              // 1..MAX_PLAYERS-1 (clients)
    int maxPerAddress;           // connections from one IP address
    uint32_t maxMessage;         // largest accepted message (bytes)
    uint32_t maxBytesPerSec;     // incoming per connection; above = disconnected (flood)
    std::vector<std::string> admins;   // account names allowed to use admin commands
    bool checkMoves;             // states of a player's characters moving impossibly fast are not relayed (counted per player)
    int kickAfterRefusals;       // that many refused states -> disconnected (0 = never)
    ServerOptions() : dedicated(false), accounts(false), maxPlayers(HOST_SLOTS - 1), maxPerAddress(4), maxMessage(MAX_PACKET),
                      maxBytesPerSec(4 * 1024 * 1024), checkMoves(false), kickAfterRefusals(0) {}
};

class SessionImpl;

class Session
{
public:
    Session();
    ~Session();

    // Content identity checked at handshake (e.g. "mod1.mod;mod2.mod"). strict: the host
    // rejects players whose list differs; otherwise both sides get an EV_WARNING.
    void setModList(const std::string& mods, bool strict);
    // Optional shared secret: the host refuses players without the same password ("" = open).
    void setPassword(const std::string& password);
    // Client: password of our account on dedicated servers (the first login creates the account). With one set, a
    // dedicated server that does not ask for it securely is refused.
    void setAccountPassword(const std::string& password);
    // Before host()/join(). Both sides must use the same transport.
    void setTransport(Transport t);
    Transport transport() const;
    // Host: dedicated server mode (before host()).
    void setServer(const ServerOptions& o);
    // Tests: simulated network on our outgoing UDP datagrams (percent lost, added delay).
    void setNetSimulation(int lossPercent, int lagMs);

    bool host(int port, const std::string& name, const std::string& faction, std::string& err);
    // Returns at once; the connection completes on the network thread (EV_CONNECTED), or fails
    // with EV_DISCONNECTED (host unreachable, refused...). false only for immediate errors.
    bool join(const std::string& address, int port, const std::string& name,
              const std::string& faction, std::string& err);
    void stop();

    bool active() const;
    bool isHost() const;
    uint8_t localId() const;
    int  pingMs() const;                       // client: last measured round-trip, host: 0
    NetStats stats() const;
    std::vector<PlayerInfo> players() const;   // includes the local player once known
    // Client: the host is a dedicated server (no player of its own).
    bool hostIsServer() const;

    // ---- host / dedicated server ----
    void kick(uint8_t player, const std::string& reason);
    bool ban(const std::string& nameOrAddress);          // also kicks a connected player of that name / address
    bool unban(const std::string& nameOrAddress);
    std::string bans() const;
    bool isAdmin(uint8_t player) const;
    std::string address(uint8_t player) const;
    std::vector<PeerStats> peerStats() const;
    size_t accountCount() const;
    bool resetAccount(const std::string& name);

    // Blocking query of a server's description (server list): connects, asks, closes. pingMs = round trip.
    static bool queryInfo(const std::string& address, int port, Transport t, ServerInfo& out, int& pingMs, std::string& err, int timeoutMs = 2000);
    // Blocking plain HTTP/1.0 GET of http://host[:port]/path (server list service). body = the response body.
    static bool httpGet(const std::string& url, std::string& body, std::string& err, int timeoutMs = 5000);

    bool pollEvent(NetEvent& out);

    // Send to everybody (host: all clients; client: via the host, which relays).
    void send(uint8_t msgType, const Bytes& body);
    // Send to one player (DAMAGE-style targeted messages). Body layout is up to the caller;
    // for MSG_DAMAGE / MSG_BUILDING_DAMAGE the first byte must already be the target player id.
    void sendTo(uint8_t player, uint8_t msgType, const Bytes& body);

    // Host: name -> player number of everybody who joined (returning players get their number
    // back). Kept by the plugin across sessions, given back before host() or right after it.
    void setKnownSlots(const std::map<std::string, uint8_t>& slots);
    std::map<std::string, uint8_t> knownSlots() const;

private:
    Session(const Session&);
    Session& operator=(const Session&);
    SessionImpl* impl_;
};

} // namespace mp
