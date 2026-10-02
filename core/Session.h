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
#pragma once
#include "Protocol.h"

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
    NetStats() : bytesIn(0), bytesOut(0), statesDropped(0) {}
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

    bool pollEvent(NetEvent& out);

    // Send to everybody (host: all clients; client: via the host, which relays).
    void send(uint8_t msgType, const Bytes& body);
    // Send to one player (DAMAGE-style targeted messages). Body layout is up to the caller;
    // for MSG_DAMAGE / MSG_BUILDING_DAMAGE the first byte must already be the target player id.
    void sendTo(uint8_t player, uint8_t msgType, const Bytes& body);

private:
    Session(const Session&);
    Session& operator=(const Session&);
    SessionImpl* impl_;
};

} // namespace mp
