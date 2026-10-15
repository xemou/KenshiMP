#include "Session.h"

#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <deque>
#include <map>
#include <stdio.h>
#include <stdlib.h>

#include "Accounts.h"
#include "Reliable.h"
#include "StateCodec.h"
#include "MovementGuard.h"

#pragma comment(lib, "ws2_32.lib")

namespace mp {

namespace {

struct Lock
{
    CRITICAL_SECTION* c;
    explicit Lock(CRITICAL_SECTION& cs) : c(&cs) { EnterCriticalSection(c); }
    ~Lock() { LeaveCriticalSection(c); }
};

struct Outgoing
{
    int target;             // player id or BROADCAST
    uint8_t type;
    Bytes body;
};

// UDP session datagrams (link datagrams of core/Reliable.h use kinds 1..3).
enum { UDP_CONNECT = 16, UDP_ACCEPT = 17, UDP_CLOSE = 18, UDP_INFO_REQ = 19, UDP_INFO = 20 };
const uint32_t UDP_MAGIC = 0x32504D4B;   // "KMP2"
const unsigned UDP_CONNECT_RESEND_MS = 250;

struct Conn
{
    SOCKET s;               // TCP socket (INVALID_SOCKET for UDP peers: they share the session's datagram socket)
    Bytes in, out;
    uint8_t playerId;
    bool helloDone;
    bool closeAfterFlush;
    bool dead;
    std::string reason;     // why it died (for EV_PLAYER_LEFT / EV_DISCONNECTED)
    DWORD lastRecv;
    bool connecting;        // client: connection not established yet (checked by the net thread)
    DWORD connectDeadline;
    std::string target;     // client: "address" for messages
    std::string ip;         // host: the peer's address
    // UDP
    bool udp;
    sockaddr_in addr;
    ReliableLink link;
    std::vector<Bytes> dgrams;      // unreliable datagrams to send
    DWORD lastConnectSent;
    uint32_t token;
    // dedicated server login
    bool authPending, authed;
    std::string pendingName, pendingFaction;
    AccountStore::Challenge challenge;
    // counters / limits
    uint64_t bytesIn, bytesOut;
    uint32_t dropped;
    DWORD rateStart; uint32_t rateBytes;
    DWORD closeDeadline;
    MovementGuard moves;    // server: positions this player reports for its characters
    std::map<uint32_t, DWORD> lastJump;
    uint32_t refused;
    DWORD lastRefusedWarn;
    Conn() : s(INVALID_SOCKET), playerId(0), helloDone(false), closeAfterFlush(false), dead(false), lastRecv(GetTickCount()),
             connecting(false), connectDeadline(0), udp(false), lastConnectSent(0), token(0), authPending(false), authed(false),
             bytesIn(0), bytesOut(0), dropped(0), rateStart(GetTickCount()), rateBytes(0), closeDeadline(0), moves(80.f, 50.f, 500), refused(0), lastRefusedWarn(0) { memset(&addr, 0, sizeof(addr)); }
    void kill(const char* why) { if (!dead) { dead = true; reason = why; } }
    size_t backlog() const { return out.size() + (udp ? link.backlog() : 0); }
};

// High-rate messages that a newer one supersedes: safe to skip for a congested peer (and sent unreliably over UDP).
bool isDroppable(uint8_t type) { return type == MSG_ENTITY_STATE; }

void appendFrame(Bytes& out, const Bytes& payload)
{
    uint32_t n = (uint32_t)payload.size();
    const uint8_t* p = (const uint8_t*)&n;
    out.insert(out.end(), p, p + 4);
    out.insert(out.end(), payload.begin(), payload.end());
}

Bytes relayedPayload(uint8_t type, uint8_t sender, const Bytes& body)
{
    Bytes p;
    p.reserve(body.size() + 2);
    p.push_back(type);
    p.push_back(sender);
    p.insert(p.end(), body.begin(), body.end());
    return p;
}

Bytes controlPayload(uint8_t type, const ByteWriter& w)
{
    Bytes p; p.push_back(type); p.insert(p.end(), w.data.begin(), w.data.end());
    return p;
}

std::string lastWsaError(const char* what)
{
    char buf[128];
    sprintf_s(buf, sizeof(buf), "%s failed (WSA error %d)", what, WSAGetLastError());
    return buf;
}

void setNonBlocking(SOCKET s, bool tcp = true)
{
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
    if (!tcp) return;
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
}

std::string addrText(const sockaddr_in& a)
{
    char buf[64];
    const uint8_t* b = (const uint8_t*)&a.sin_addr.s_addr;
    sprintf_s(buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}
bool sameAddr(const sockaddr_in& a, const sockaddr_in& b) { return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }

bool resolve(const std::string& addr, int port, int socktype, sockaddr_in& out, std::string& err)
{
    addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = socktype;
    char portStr[16]; sprintf_s(portStr, sizeof(portStr), "%d", port);
    addrinfo* res = NULL;
    if (getaddrinfo(addr.c_str(), portStr, &hints, &res) != 0 || !res) { err = "cannot resolve " + addr; return false; }
    memcpy(&out, res->ai_addr, sizeof(out));
    freeaddrinfo(res);
    return true;
}

std::string shorten(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n) + "..."; }

std::vector<std::string> splitMods(const std::string& s)
{
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size())
    {
        size_t b = s.find(';', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

// Human readable difference between the host's and a player's mod list.
std::string modDiff(const std::string& host, const std::string& player)
{
    std::vector<std::string> h = splitMods(host), p = splitMods(player);
    std::string missing, extra;
    for (size_t i = 0; i < h.size(); ++i)
        if (std::find(p.begin(), p.end(), h[i]) == p.end()) missing += (missing.empty() ? "" : ", ") + h[i];
    for (size_t i = 0; i < p.size(); ++i)
        if (std::find(h.begin(), h.end(), p[i]) == h.end()) extra += (extra.empty() ? "" : ", ") + p[i];
    std::string out;
    if (!missing.empty()) out += "Missing (enable them): " + shorten(missing, 400) + ". ";
    if (!extra.empty()) out += "Not on the host (disable them): " + shorten(extra, 400) + ". ";
    if (out.empty()) out = "Same mods but a different load order: copy the host's order. ";
    return out;
}

struct Delayed { DWORD at; sockaddr_in to; Bytes data; };

} // namespace

class SessionImpl
{
public:
    SessionImpl() : thread(NULL), running(false), hosting(false), localId(0), ping(0), strictMods(true), passwordHash(0),
                    transport(TRANSPORT_TCP), hostServer(false), hostFlags(0), lossPct(0), lagMs(0),
                    listenSock(INVALID_SOCKET), udpSock(INVALID_SOCKET), wsaStarted(false), lastPing(0), rng(12345), lastPeers(0), welcomed(false)
    { InitializeCriticalSection(&cs); }
    ~SessionImpl() { stop(); DeleteCriticalSection(&cs); }

    // ---- shared with game thread (guarded by cs) ----
    mutable CRITICAL_SECTION cs;
    std::deque<NetEvent> events;
    std::vector<Outgoing> outbox;
    std::vector<PlayerInfo> players;
    HANDLE thread;
    volatile bool running;
    bool hosting;
    uint8_t localId;
    int ping;
    std::string mods;
    bool strictMods;
    uint32_t passwordHash;                    // 0 = no password
    std::string accountPassword;              // client
    std::map<std::string, uint8_t> knownIds;  // host: name -> slot, kept for the session
    NetStats stats;
    Transport transport;
    ServerOptions server;
    bool hostServer;                          // client: the host said it is a dedicated server
    uint8_t hostFlags;
    std::vector<std::pair<uint8_t, std::string> > kicks;   // game thread -> net thread
    std::vector<PeerStats> peers;             // refreshed by the net thread
    AccountStore accounts;
    BanList banList;
    std::map<uint8_t, std::string> addresses; // host: player -> IP
    std::map<uint8_t, bool> adminFlags;
    int lossPct, lagMs;

    // ---- net thread only ----
    SOCKET listenSock, udpSock;
    std::vector<Conn*> conns;
    PlayerInfo self;
    bool wsaStarted;
    DWORD lastPing;
    std::deque<Delayed> delayed;
    unsigned rng;
    DWORD lastPeers;
    bool welcomed;                            // client: WELCOME received
    std::vector<Bytes> heldBack;              // client: game messages sent before the WELCOME (net thread)

    bool startWsa(std::string& err)
    {
        if (wsaStarted) return true;
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) { err = "WSAStartup failed"; return false; }
        wsaStarted = true;
        return true;
    }

    int slotLimit() const { int m = server.maxPlayers; return m < 1 ? 1 : (m > MAX_PLAYERS - 1 ? MAX_PLAYERS - 1 : m); }

    bool startHost(int port, const std::string& name, const std::string& faction, std::string& err)
    {
        if (!startWsa(err)) return false;
        sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((u_short)port);
        if (transport == TRANSPORT_UDP)
        {
            udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (udpSock == INVALID_SOCKET) { err = lastWsaError("socket"); return false; }
            if (bind(udpSock, (sockaddr*)&a, sizeof(a)) != 0)
            {
                err = lastWsaError("bind") + " - UDP port already used by another program or server?";
                closesocket(udpSock); udpSock = INVALID_SOCKET;
                return false;
            }
            setNonBlocking(udpSock, false);
            int big = 4 * 1024 * 1024;
            setsockopt(udpSock, SOL_SOCKET, SO_RCVBUF, (const char*)&big, sizeof(big));
            setsockopt(udpSock, SOL_SOCKET, SO_SNDBUF, (const char*)&big, sizeof(big));
        }
        else
        {
            listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listenSock == INVALID_SOCKET) { err = lastWsaError("socket"); return false; }
            // No SO_REUSEADDR: on Windows it would let a second host silently share the port.
            if (bind(listenSock, (sockaddr*)&a, sizeof(a)) != 0 || listen(listenSock, 16) != 0)
            {
                err = lastWsaError("bind/listen") + " - port already used by another program or game?";
                closesocket(listenSock); listenSock = INVALID_SOCKET;
                return false;
            }
            setNonBlocking(listenSock);
        }
        if (server.accounts)
        {
            std::string dir = server.dataDir.empty() ? std::string(".") : server.dataDir;
            makeDirs(dir);
            accounts.load(dir + "\\accounts.txt");
        }
        if (!server.dataDir.empty()) banList.load(server.dataDir + "\\bans.txt");
        self.id = HOST_ID; self.name = name; self.faction = faction;
        hosting = true; localId = HOST_ID;
        players.clear(); players.push_back(self);
        launch();
        return true;
    }

    bool startClient(const std::string& addr, int port, const std::string& name,
                     const std::string& faction, std::string& err)
    {
        if (!startWsa(err)) return false;
        hostServer = false; hostFlags = 0; welcomed = false; heldBack.clear();
        Conn* c = new Conn();
        c->playerId = HOST_ID; c->helloDone = true;
        c->connectDeadline = GetTickCount() + CONNECT_TIMEOUT_MS; c->target = addr;
        if (transport == TRANSPORT_UDP)
        {
            if (!resolve(addr, port, SOCK_DGRAM, c->addr, err)) { delete c; return false; }
            udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (udpSock == INVALID_SOCKET) { delete c; err = lastWsaError("socket"); return false; }
            setNonBlocking(udpSock, false);
            int big = 2 * 1024 * 1024;
            setsockopt(udpSock, SOL_SOCKET, SO_RCVBUF, (const char*)&big, sizeof(big));
            c->udp = true; c->connecting = true;
            c->token = (uint32_t)GetTickCount() ^ ((uint32_t)(size_t)c << 3) ^ 0x5bd1e995;
        }
        else
        {
            sockaddr_in sa;
            if (!resolve(addr, port, SOCK_STREAM, sa, err)) { delete c; return false; }
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) { delete c; err = lastWsaError("socket"); return false; }
            setNonBlocking(s);
            int rc = connect(s, (sockaddr*)&sa, sizeof(sa));
            if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) { err = lastWsaError("connect"); closesocket(s); delete c; return false; }
            // The connection completes (or fails) on the network thread: the game never waits for an
            // unreachable host. A failure arrives as EV_DISCONNECTED.
            c->s = s; c->connecting = rc != 0;
        }
        conns.push_back(c);
        hosting = false;
        self.name = name; self.faction = faction;

        ByteWriter w2;
        w2.u32(PROTOCOL_VERSION); w2.str(name); w2.str(faction); w2.str(mods); w2.u32(passwordHash);
        appendFrame(c->out, controlPayload(MSG_HELLO, w2));
        launch();
        return true;
    }

    std::string connectFailure(Conn* c)
    {
        return "connection to " + c->target + " timed out / refused (host not started, wrong IP, wrong transport (TCP/UDP), or port closed on the host's router/firewall?)";
    }

    void launch()
    {
        running = true;
        lastPing = GetTickCount();
        thread = CreateThread(NULL, 0, &SessionImpl::threadMain, this, 0, NULL);
    }

    static DWORD WINAPI threadMain(LPVOID p) { ((SessionImpl*)p)->loop(); return 0; }

    void stop()
    {
        running = false;
        if (thread) { WaitForSingleObject(thread, 3000); CloseHandle(thread); thread = NULL; }
        for (size_t i = 0; i < conns.size(); ++i)
        {
            if (conns[i]->udp && udpSock != INVALID_SOCKET && !conns[i]->dead) { Bytes b(1, (uint8_t)UDP_CLOSE); rawSend(conns[i]->addr, b); }
            if (conns[i]->s != INVALID_SOCKET) closesocket(conns[i]->s);
            delete conns[i];
        }
        conns.clear();
        delayed.clear();
        if (listenSock != INVALID_SOCKET) { closesocket(listenSock); listenSock = INVALID_SOCKET; }
        if (udpSock != INVALID_SOCKET) { closesocket(udpSock); udpSock = INVALID_SOCKET; }
        if (wsaStarted) { WSACleanup(); wsaStarted = false; }
    }

    // ------------------------------------------------------------------ net thread
    // The game thread may not poll for a while (loading screens): cap the backlog by dropping
    // superseded state messages first.
    void push(const NetEvent& e)
    {
        Lock l(cs);
        if (events.size() > 20000 && e.kind == NetEvent::EV_MESSAGE && isDroppable(e.msgType)) return;
        events.push_back(e);
    }

    void queue(Conn* c, const Bytes& payload, bool droppable)
    {
        if (c->dead) return;
        if (droppable && c->backlog() > SOFT_BACKLOG) { ++c->dropped; Lock l(cs); ++stats.statesDropped; return; }   // superseded soon anyway
        if (c->udp && droppable)
        {
            // Before the handshake (client: no WELCOME yet) a state could overtake the HELLO on the stream: dropped.
            if (c->connecting || (hosting ? !c->helloDone : !welcomed)) return;
            c->link.sendUnreliable(payload, c->dgrams);
            return;
        }
        if (c->backlog() + payload.size() > HARD_BACKLOG) { c->kill("too slow (send backlog overflow)"); return; }
        appendFrame(c->out, payload);
    }

    Conn* findConn(uint8_t id)
    {
        for (size_t i = 0; i < conns.size(); ++i)
            if (conns[i]->helloDone && !conns[i]->dead && conns[i]->playerId == id) return conns[i];
        return NULL;
    }

    void drainOutbox()
    {
        std::vector<Outgoing> box;
        std::vector<std::pair<uint8_t, std::string> > kk;
        { Lock l(cs); box.swap(outbox); kk.swap(kicks); }
        for (size_t i = 0; i < kk.size(); ++i)
            for (size_t k = 0; k < conns.size(); ++k)
                if (conns[k]->helloDone && !conns[k]->dead && conns[k]->playerId == kk[i].first && conns[k]->playerId != HOST_ID)
                    reject(conns[k], kk[i].second.c_str());
        for (size_t i = 0; i < box.size(); ++i)
        {
            const Outgoing& o = box[i];
            bool drop = isDroppable(o.type);
            if (hosting)
            {
                Bytes p = relayedPayload(o.type, HOST_ID, o.body);
                for (size_t k = 0; k < conns.size(); ++k)
                {
                    Conn* c = conns[k];
                    if (!c->helloDone || c->dead) continue;
                    if (o.target == BROADCAST || o.target == c->playerId) queue(c, p, drop);
                }
            }
            else if (!conns.empty())
            {
                Bytes p; p.push_back(o.type); p.insert(p.end(), o.body.begin(), o.body.end());
                // Game messages wait for the end of the login (HELLO, account challenge, WELCOME): the host expects
                // nothing else before. Superseded states are not kept.
                if (!welcomed) { if (!drop && heldBack.size() < 4096) heldBack.push_back(p); continue; }
                queue(conns[0], p, drop);
            }
        }
    }

    // --- datagrams ---
    void rawSend(const sockaddr_in& to, const Bytes& d)
    {
        if (d.empty() || udpSock == INVALID_SOCKET) return;
        int r = sendto(udpSock, (const char*)&d[0], (int)d.size(), 0, (const sockaddr*)&to, sizeof(to));
        if (r > 0) { Lock l(cs); stats.bytesOut += r; }
    }
    void sendDatagram(const sockaddr_in& to, const Bytes& d)
    {
        int loss, lag; { Lock l(cs); loss = lossPct; lag = lagMs; }
        if (loss > 0) { rng = rng * 1103515245u + 12345u; if ((int)((rng >> 16) % 100) < loss) return; }
        if (lag > 0) { Delayed x; x.at = GetTickCount() + lag; x.to = to; x.data = d; delayed.push_back(x); return; }
        rawSend(to, d);
    }
    void flushDelayed()
    {
        DWORD now = GetTickCount();
        while (!delayed.empty() && (int)(now - delayed.front().at) >= 0) { rawSend(delayed.front().to, delayed.front().data); delayed.pop_front(); }
    }
    void pumpUdpConn(Conn* c, DWORD now)
    {
        if (c->connecting)
        {
            if (!hosting && now - c->lastConnectSent >= UDP_CONNECT_RESEND_MS)
            {
                c->lastConnectSent = now;
                ByteWriter w; w.u8(UDP_CONNECT); w.u32(UDP_MAGIC); w.u32(c->token);
                sendDatagram(c->addr, w.data);
            }
            return;
        }
        while (!c->out.empty())
        {
            size_t n = c->link.writeStream(&c->out[0], c->out.size());
            if (!n) break;
            c->out.erase(c->out.begin(), c->out.begin() + n);
        }
        std::vector<Bytes> d;
        d.swap(c->dgrams);
        c->link.poll(now, d);
        for (size_t i = 0; i < d.size(); ++i) { c->bytesOut += d[i].size(); sendDatagram(c->addr, d[i]); }
    }
    Conn* findUdp(const sockaddr_in& from)
    {
        for (size_t i = 0; i < conns.size(); ++i) if (conns[i]->udp && !conns[i]->dead && sameAddr(conns[i]->addr, from)) return conns[i];
        return NULL;
    }
    int countFromIp(const std::string& ip)
    {
        int n = 0;
        for (size_t i = 0; i < conns.size(); ++i) if (!conns[i]->dead && conns[i]->ip == ip) ++n;
        return n;
    }
    void readUdp()
    {
        char buf[2048];
        for (int guard = 0; guard < 4096; ++guard)
        {
            sockaddr_in from; int fl = sizeof(from);
            int r = recvfrom(udpSock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (r < 0)
            {
                int e = WSAGetLastError();
                if (e == WSAECONNRESET) continue;   // ICMP "port unreachable" for an earlier datagram: ignore
                break;
            }
            if (r == 0) continue;
            { Lock l(cs); stats.bytesIn += r; }
            const uint8_t* p = (const uint8_t*)buf;
            uint8_t kind = p[0];
            Conn* c = findUdp(from);
            if (hosting)
            {
                if (kind == UDP_INFO_REQ)
                {
                    ByteReader rr(p + 1, r - 1); uint32_t token = rr.u32();
                    ByteWriter w; w.u8(UDP_INFO); w.u32(token); serverInfo().write(w);
                    sendDatagram(from, w.data);
                    continue;
                }
                if (kind == UDP_CONNECT)
                {
                    ByteReader rr(p + 1, r - 1); uint32_t magic = rr.u32(); uint32_t token = rr.u32();
                    if (!rr.ok() || magic != UDP_MAGIC) continue;
                    if (!c)
                    {
                        std::string ip = addrText(from);
                        bool banned; { Lock l(cs); banned = banList.banned("", ip); }
                        if (banned || (server.dedicated && countFromIp(ip) >= server.maxPerAddress) || conns.size() > (size_t)(slotLimit() + 16))
                        { Bytes b(1, (uint8_t)UDP_CLOSE); sendDatagram(from, b); continue; }
                        c = new Conn(); c->udp = true; c->addr = from; c->ip = ip; c->token = token;
                        conns.push_back(c);
                    }
                    if (c->token != token) continue;
                    c->lastRecv = GetTickCount();
                    ByteWriter w; w.u8(UDP_ACCEPT); w.u32(token);
                    sendDatagram(from, w.data);
                    continue;
                }
            }
            if (!c) continue;
            if (kind == UDP_ACCEPT)
            {
                ByteReader rr(p + 1, r - 1); uint32_t token = rr.u32();
                if (rr.ok() && token == c->token && c->connecting) { c->connecting = false; c->lastRecv = GetTickCount(); }
                continue;
            }
            if (kind == UDP_CLOSE)
            {
                ByteReader rr(p + 1, r - 1); std::string why = rr.str();
                std::string text = hosting ? std::string("connection closed") : std::string("connection closed by the host") + (rr.ok() && !why.empty() ? " (" + why + ")" : std::string());
                c->kill(text.c_str());
                continue;
            }
            if (kind < DGRAM_RELIABLE || kind > DGRAM_ACK || c->connecting) continue;
            c->lastRecv = GetTickCount();
            c->bytesIn += r;
            if (!rateOk(c, r)) continue;
            Bytes stream; std::vector<Bytes> msgs;
            if (!c->link.receive(p, r, GetTickCount(), stream, msgs)) continue;
            if (!stream.empty()) { c->in.insert(c->in.end(), stream.begin(), stream.end()); parseFrames(c); }
            for (size_t i = 0; i < msgs.size() && !c->dead; ++i)
            {
                if (msgs[i].empty()) continue;
                if (hosting && !c->helloDone) continue;   // only states travel unreliably: meaningless before the login
                if (msgs[i].size() > maxMessage()) { c->kill("protocol error (message too large)"); break; }
                if (hosting) handleHostPacket(c, msgs[i]); else handleClientPacket(c, msgs[i]);
            }
        }
    }
    uint32_t maxMessage() const { return hosting && server.maxMessage ? server.maxMessage : MAX_PACKET; }
    bool rateOk(Conn* c, int n)
    {
        if (!hosting || !server.dedicated) return true;
        DWORD now = GetTickCount();
        if (now - c->rateStart >= 1000) { c->rateStart = now; c->rateBytes = 0; }
        c->rateBytes += n;
        if (c->rateBytes > server.maxBytesPerSec) { c->kill("flooding (too much data per second)"); return false; }
        return true;
    }

    void loop()
    {
        while (running)
        {
            drainOutbox();
            DWORD now = GetTickCount();

            if (!hosting && !conns.empty() && !conns[0]->connecting && welcomed && now - lastPing > PING_MS)
            {
                lastPing = now;
                Bytes p; p.push_back(MSG_PING);
                uint32_t t = lastPing; const uint8_t* tp = (const uint8_t*)&t;
                p.insert(p.end(), tp, tp + 4);
                queue(conns[0], p, false);
            }

            // Silent peers are dead (a live client pings, a live host answers).
            for (size_t i = 0; i < conns.size(); ++i)
            {
                Conn* c = conns[i];
                if (c->dead) continue;
                if (c->connecting && !hosting) { if ((int)(now - c->connectDeadline) >= 0) c->kill(connectFailure(c).c_str()); }
                else if (now - c->lastRecv > TIMEOUT_MS) c->kill("timed out (no data for 15 s)");
                else if (!c->helloDone && hosting && now - c->lastRecv > CONNECT_TIMEOUT_MS * 2) c->kill("no login");
                if (c->closeAfterFlush && !c->dead && c->closeDeadline && (int)(now - c->closeDeadline) >= 0) c->kill("rejected");
            }

            if (transport == TRANSPORT_UDP)
            {
                for (size_t i = 0; i < conns.size(); ++i) if (!conns[i]->dead) pumpUdpConn(conns[i], now);
                flushDelayed();
                fd_set rs; FD_ZERO(&rs);
                if (udpSock != INVALID_SOCKET) FD_SET(udpSock, &rs);
                timeval tv; tv.tv_sec = 0; tv.tv_usec = 2000;
                if (udpSock == INVALID_SOCKET || select(0, &rs, NULL, NULL, &tv) < 0) Sleep(2);
                else if (FD_ISSET(udpSock, &rs)) readUdp();
                for (size_t i = 0; i < conns.size(); ++i)
                {
                    Conn* c = conns[i];
                    if (!c->dead && c->closeAfterFlush && c->out.empty() && c->link.idle()) c->kill("rejected");
                }
            }
            else
            {
                fd_set rs, ws, es; FD_ZERO(&rs); FD_ZERO(&ws); FD_ZERO(&es);
                int n = 0;
                if (hosting && listenSock != INVALID_SOCKET) { FD_SET(listenSock, &rs); ++n; }
                for (size_t i = 0; i < conns.size() && n < FD_SETSIZE - 1; ++i)
                {
                    if (conns[i]->dead) continue;
                    if (conns[i]->connecting) { FD_SET(conns[i]->s, &ws); FD_SET(conns[i]->s, &es); ++n; continue; }
                    FD_SET(conns[i]->s, &rs); ++n;
                    if (!conns[i]->out.empty()) FD_SET(conns[i]->s, &ws);
                }
                if (n > 0)
                {
                    // 2 ms: outgoing messages queued by the game thread leave within ~2 ms.
                    timeval tv; tv.tv_sec = 0; tv.tv_usec = 2000;
                    if (select(0, &rs, &ws, &es, &tv) < 0) Sleep(2);
                    else
                    {
                        if (hosting && listenSock != INVALID_SOCKET && FD_ISSET(listenSock, &rs))
                        {
                            sockaddr_in from; int fl = sizeof(from);
                            SOCKET ns = accept(listenSock, (sockaddr*)&from, &fl);
                            if (ns != INVALID_SOCKET)
                            {
                                std::string ip = addrText(from);
                                bool banned; { Lock l(cs); banned = banList.banned("", ip); }
                                if (banned || (server.dedicated && countFromIp(ip) >= server.maxPerAddress) || conns.size() > (size_t)(slotLimit() + 16))
                                    closesocket(ns);
                                else
                                {
                                    setNonBlocking(ns);
                                    Conn* c = new Conn(); c->s = ns; c->ip = ip; conns.push_back(c);
                                }
                            }
                        }
                        for (size_t i = 0; i < conns.size(); ++i)
                        {
                            Conn* c = conns[i];
                            if (c->dead) continue;
                            if (c->connecting)
                            {
                                if (FD_ISSET(c->s, &es)) c->kill(connectFailure(c).c_str());
                                else if (FD_ISSET(c->s, &ws)) { c->connecting = false; c->lastRecv = GetTickCount(); }
                                continue;
                            }
                            if (FD_ISSET(c->s, &rs)) readConn(c);
                            if (!c->dead && !c->out.empty() && FD_ISSET(c->s, &ws)) writeConn(c);
                            if (!c->dead && c->closeAfterFlush && c->out.empty()) c->kill("rejected");
                        }
                    }
                }
                else Sleep(2);
            }

            // reap dead connections
            for (size_t i = 0; i < conns.size();)
            {
                if (conns[i]->dead)
                {
                    onDisconnect(conns[i]);
                    if (conns[i]->udp && conns[i]->reason.compare(0, 17, "connection closed") != 0)
                    {
                        ByteWriter w; w.u8(UDP_CLOSE); w.str(conns[i]->reason.substr(0, 300));
                        rawSend(conns[i]->addr, w.data);
                    }
                    if (conns[i]->s != INVALID_SOCKET) closesocket(conns[i]->s);
                    delete conns[i]; conns.erase(conns.begin() + i);
                }
                else ++i;
            }
            refreshPeers();
            if (!hosting && conns.empty()) { running = false; break; }
        }
    }

    void refreshPeers()
    {
        DWORD now = GetTickCount();
        if (now - lastPeers < 500) return;
        lastPeers = now;
        std::vector<PeerStats> v;
        uint32_t resent = 0, lost = 0;
        for (size_t i = 0; i < conns.size(); ++i)
        {
            Conn* c = conns[i];
            resent += c->link.resent(); lost += c->link.lost();
            if (!c->helloDone || c->dead) continue;
            PeerStats p; p.id = c->playerId; p.address = c->ip; p.bytesIn = c->bytesIn; p.bytesOut = c->bytesOut; p.statesDropped = c->dropped;
            p.rttMs = c->udp ? c->link.rtt() : 0;
            p.refused = c->refused;
            v.push_back(p);
        }
        Lock l(cs);
        for (size_t i = 0; i < v.size(); ++i)
        {
            for (size_t k = 0; k < players.size(); ++k) if (players[k].id == v[i].id) v[i].name = players[k].name;
            std::map<uint8_t, bool>::iterator a = adminFlags.find(v[i].id);
            v[i].admin = a != adminFlags.end() && a->second;
        }
        peers.swap(v);
        stats.resent = resent; stats.lost = lost;
    }

    void readConn(Conn* c)
    {
        char buf[65536];
        int r = recv(c->s, buf, sizeof(buf), 0);
        if (r == 0) { c->kill("connection closed"); return; }
        if (r < 0) { if (WSAGetLastError() != WSAEWOULDBLOCK) c->kill("connection reset"); return; }
        c->lastRecv = GetTickCount();
        c->bytesIn += r;
        { Lock l(cs); stats.bytesIn += r; }
        if (!rateOk(c, r)) return;
        c->in.insert(c->in.end(), buf, buf + r);
        parseFrames(c);
    }

    void parseFrames(Conn* c)
    {
        size_t pos = 0;
        uint32_t limit = maxMessage();
        while (c->in.size() - pos >= 4)
        {
            uint32_t len; memcpy(&len, &c->in[pos], 4);
            if (len == 0 || len > limit) { c->kill("protocol error (bad frame)"); return; }
            if (c->in.size() - pos - 4 < len) break;
            Bytes payload(c->in.begin() + pos + 4, c->in.begin() + pos + 4 + len);
            pos += 4 + len;
            if (hosting) handleHostPacket(c, payload); else handleClientPacket(c, payload);
            if (c->dead) return;
        }
        c->in.erase(c->in.begin(), c->in.begin() + pos);
    }

    void writeConn(Conn* c)
    {
        int r = send(c->s, (const char*)&c->out[0], (int)c->out.size(), 0);
        if (r < 0) { if (WSAGetLastError() != WSAEWOULDBLOCK) c->kill("connection reset"); return; }
        c->bytesOut += r;
        { Lock l(cs); stats.bytesOut += r; }
        c->out.erase(c->out.begin(), c->out.begin() + r);
    }

    ServerInfo serverInfo()
    {
        ServerInfo si;
        Lock l(cs);
        si.name = server.name.empty() ? self.name : server.name;
        si.description = server.description;
        si.protocol = PROTOCOL_VERSION;
        size_t clients = players.size() > 0 ? players.size() - 1 : 0;
        si.players = (uint8_t)(clients > 255 ? 255 : clients);
        si.maxPlayers = (uint8_t)slotLimit();
        si.flags = (uint8_t)((server.accounts ? ServerInfo::ACCOUNTS : 0) | (passwordHash ? ServerInfo::PASSWORD : 0) |
                             (transport == TRANSPORT_UDP ? ServerInfo::UDP : 0) | (server.dedicated ? ServerInfo::DEDICATED : 0));
        Bytes m(mods.begin(), mods.end());
        si.modsHash = hashBytes(m);
        return si;
    }

    // ---------------------------------------------------------------- host logic
    void handleHostPacket(Conn* c, const Bytes& p)
    {
        uint8_t type = p[0];
        Bytes body(p.begin() + 1, p.end());

        if (!c->helloDone)
        {
            if (type == MSG_SERVER_INFO_REQ && !c->authPending)
            {
                ByteWriter w; serverInfo().write(w);
                queue(c, controlPayload(MSG_SERVER_INFO, w), false);
                c->closeAfterFlush = true; c->closeDeadline = GetTickCount() + 2000;
                return;
            }
            if (c->authPending)
            {
                if (isRelayed(type) || type == MSG_PING) return;   // a game message / ping while the account is checked: ignored
                if (type != MSG_AUTH_PROOF) { c->kill("protocol error (no login proof)"); return; }
                ByteReader r(body);
                std::string proof = r.str();
                if (!r.ok()) { c->kill("protocol error (bad proof)"); return; }
                std::string why;
                bool ok;
                { Lock l(cs); ok = accounts.verify(c->pendingName, c->ip, c->challenge, Bytes(proof.begin(), proof.end()), why, GetTickCount()); }
                ByteWriter w; w.u8(ok ? 1 : 0); w.str(ok ? std::string() : why);
                queue(c, controlPayload(MSG_AUTH_RESULT, w), false);
                if (!ok)
                {
                    NetEvent e; e.kind = NetEvent::EV_WARNING; e.text = "login refused for '" + c->pendingName + "' from " + c->ip + ": " + why; push(e);
                    reject(c, ("login refused: " + why).c_str());
                    return;
                }
                c->authPending = false; c->authed = true;
                acceptPlayer(c, c->pendingName, c->pendingFaction);
                return;
            }
            if (type != MSG_HELLO) { c->kill("protocol error (no hello)"); return; }
            ByteReader r(body);
            uint32_t ver = r.u32(); std::string name = r.str(); std::string faction = r.str();
            if (!r.ok()) { c->kill("protocol error (bad hello)"); return; }
            if (ver != PROTOCOL_VERSION)
            {
                // Plain words for the player (the version is the protocol number, shown in the
                // multiplayer window's title), and who has to update. The host is told too.
                char msg[320];
                sprintf_s(msg, sizeof(msg), "KenshiMP version mismatch: the host has v%u, you have v%u. %s", PROTOCOL_VERSION, ver,
                          ver < PROTOCOL_VERSION ? "Update KenshiMP: restart Steam so it downloads the new version (or unsubscribe and subscribe again)."
                                                 : "The host must update KenshiMP (restart Steam, or unsubscribe and subscribe again).");
                reject(c, msg);
                char host[200];
                sprintf_s(host, sizeof(host), "%s could not join: KenshiMP version mismatch (they have v%u, you have v%u)",
                          name.empty() ? "A player" : name.substr(0, 40).c_str(), ver, PROTOCOL_VERSION);
                NetEvent w; w.kind = NetEvent::EV_WARNING; w.text = host; push(w);
                return;
            }
            std::string theirMods = r.str();
            uint32_t theirPass = r.u32();
            if (!r.ok()) { c->kill("protocol error (bad hello)"); return; }
            if (name.empty() || name.size() > 40) { reject(c, "invalid player name (1 to 40 characters)"); return; }
            std::string myMods;
            bool strict;
            uint32_t myPass;
            bool banned;
            { Lock l(cs); myMods = mods; strict = strictMods; myPass = passwordHash; banned = banList.banned(name, c->ip); }
            if (banned) { reject(c, "you are banned from this server"); return; }
            if (myPass && theirPass != myPass) { reject(c, "wrong session password"); return; }
            if (theirMods != myMods)
            {
                std::string text = "mod lists differ (player '" + name + "'). " + modDiff(myMods, theirMods);
                if (strict) { reject(c, ("Your mods must match the host's. " + text).c_str()); return; }
                text += "Playing anyway: what comes from a mod the other side lacks is replaced or skipped.";
                NetEvent w; w.kind = NetEvent::EV_WARNING; w.text = text; push(w);
            }

            // A player named like the host would share its name (labels, chat, saved slots): it picks another one.
            // (A second client under an existing client's name is a reconnecting player: it takes its slot back, below.)
            { bool host_ = false; { Lock l(cs); host_ = (name == self.name); }
              if (host_) { reject(c, ("A player named '" + name + "' is already connected (the host): choose another player name (MULTIPLAYER window, ADVANCED SETTINGS).").c_str()); return; } }

            if (server.accounts)
            {
                bool locked;
                { Lock l(cs); locked = accounts.lockedOut(name, c->ip, GetTickCount()); c->challenge = accounts.challenge(name); }
                if (locked) { reject(c, "too many wrong passwords: try again in a minute"); return; }
                c->authPending = true; c->pendingName = name; c->pendingFaction = faction;
                ByteWriter w; w.u8(c->challenge.fresh ? 1 : 0);
                w.str(std::string(c->challenge.salt.begin(), c->challenge.salt.end())); w.u32(c->challenge.iterations);
                w.str(std::string(c->challenge.nonce.begin(), c->challenge.nonce.end()));
                queue(c, controlPayload(MSG_AUTH_CHALLENGE, w), false);
                return;
            }
            acceptPlayer(c, name, faction);
            return;
        }

        if (type == MSG_PING)
        {
            Bytes pong; pong.push_back(MSG_PONG); pong.insert(pong.end(), body.begin(), body.end());
            queue(c, pong, false);
            return;
        }
        if (!isRelayed(type)) return;

        uint8_t sender = c->playerId;
        if (type == MSG_DAMAGE || type == MSG_BUILDING_DAMAGE || type == MSG_ITEM_TAKE || type == MSG_ITEM_GIVE || type == MSG_GROUND_TAKE || type == MSG_TRADE || type == MSG_ITEM_UNDO || type == MSG_ZONE_MODE || type == MSG_WORLD_STATE_REPORT || type == MSG_WORLD_BUILDING_REPORT || type == MSG_BOUNTY_CRIME || type == MSG_WORLD_CONTAINER || type == MSG_INVENTORY_REQUEST || type == MSG_HEAL || type == MSG_CARRIED || type == MSG_ESCAPED || type == MSG_AUTH_HANDOFF || type == MSG_BAG_FINGERPRINT || type == MSG_SERVER_NOTICE || type == MSG_ADMIN)
        {
            if (body.empty()) return;
            uint8_t target = body[0];
            if (target == HOST_ID) deliver(sender, type, body);
            else if (type == MSG_SERVER_NOTICE || type == MSG_ADMIN || type == MSG_BAG_FINGERPRINT) return;   // server -> client only / for the server only
            else if (Conn* t = findConn(target)) queue(t, relayedPayload(type, sender, body), false);
            return;
        }

        if (type == MSG_ENTITY_STATE && server.dedicated && server.checkMoves && !statesAcceptable(c, body)) return;
        deliver(sender, type, body);
        Bytes out = relayedPayload(type, sender, body);
        bool drop = isDroppable(type);
        for (size_t i = 0; i < conns.size(); ++i)
            if (conns[i] != c && conns[i]->helloDone && !conns[i]->dead) queue(conns[i], out, drop);
    }

    // Server: a player only moves its own characters (or NPCs the host handed to it), at a believable speed. A teleport
    // (a single jump) passes once (regroup, reload); a sustained impossible speed is refused: not relayed, not applied.
    bool statesAcceptable(Conn* c, const Bytes& body)
    {
        ByteReader r(body);
        uint32_t clock = r.u32();
        std::vector<EntityState> st;
        StateCodec::readAll(r, st);
        const char* why = NULL;
        for (size_t i = 0; i < st.size() && !why; ++i)
        {
            const EntityState& s = st[i];
            if (isNpcNetId(s.netId)) continue;                       // handed NPCs: the authority table decides (receivers check)
            if (netIdOwner(s.netId) != c->playerId) { why = "someone else's character"; break; }
            if (s.flags & (EntityState::DEAD | EntityState::RAGDOLL)) continue;
            float amount = 0;
            MovementGuard::Verdict v = c->moves.observe(s.netId, clock, s.x, s.z, &amount);
            if (v == MovementGuard::FAST || c->moves.isFast(s.netId)) why = "impossible speed";
            else if (v == MovementGuard::JUMP)
            {
                // one teleport now and then is normal (regroup, world reload); a series is not
                DWORD& last = c->lastJump[s.netId];
                DWORD now = GetTickCount();
                if (last && now - last < 3000) why = "repeated teleports";
                last = now;
            }
        }
        if (!why) return true;
        ++c->refused;
        DWORD now = GetTickCount();
        if (!c->lastRefusedWarn || now - c->lastRefusedWarn > 30000)
        {
            c->lastRefusedWarn = now;
            char buf[200];
            sprintf_s(buf, "refused states from player %d (%s), %u so far", (int)c->playerId, why, c->refused);
            NetEvent w; w.kind = NetEvent::EV_WARNING; w.text = buf; push(w);
        }
        if (server.kickAfterRefusals > 0 && c->refused >= (uint32_t)server.kickAfterRefusals) reject(c, "disconnected: impossible movement (cheat or very bad connection)");
        return false;
    }

    void acceptPlayer(Conn* c, const std::string& name, const std::string& faction)
    {
        // The same player reconnecting before we noticed that the old connection died (its
        // game restarted, its network dropped for a moment): end the old one now, so the
        // player gets its slot back instead of a new number with ghosts shown twice.
        {
            uint8_t oldId = 0; bool found = false;
            { Lock l(cs);
              std::map<std::string, uint8_t>::iterator prev = knownIds.find(name);
              if (prev != knownIds.end())
                  for (size_t i = 0; i < players.size(); ++i) if (players[i].id == prev->second && players[i].name == name) { oldId = prev->second; found = true; } }
            if (found)
                for (size_t i = 0; i < conns.size(); ++i)
                {
                    Conn* o = conns[i];
                    if (o == c || !o->helloDone || o->dead || o->playerId != oldId) continue;
                    o->kill("replaced by a new connection of the same player");
                    onDisconnect(o);          // announced now, before the new join
                    o->helloDone = false;     // ... and not a second time when it is reaped
                }
        }

        int id = -1;
        int limit = slotLimit();
        { Lock l(cs);
          // Same name as earlier in this session -> same slot (mirror faction, relations, ids).
          std::map<std::string, uint8_t>::iterator prev = knownIds.find(name);
          if (prev != knownIds.end() && (int)players.size() - 1 < limit)
          {
              bool used = false;
              for (size_t i = 0; i < players.size(); ++i) if (players[i].id == prev->second) used = true;
              if (!used) id = prev->second;
          }
          // New name: prefer slots nobody used before (keep returning players' slots),
          // fall back to any free slot when the session is nearly full.
          int connected = (int)players.size() - 1;
          for (int pass = 0; pass < 2 && id < 0 && connected < limit; ++pass)
              for (int cand = 1; cand < MAX_PLAYERS && id < 0; ++cand)
              {
                  bool used = false;
                  for (size_t i = 0; i < players.size(); ++i) if (players[i].id == cand) used = true;
                  if (pass == 0)
                      for (std::map<std::string, uint8_t>::iterator k = knownIds.begin(); k != knownIds.end(); ++k)
                          if (k->second == cand && k->first != name) used = true;
                  if (!used) id = cand;
              } }
        if (id < 0) { reject(c, "server full"); return; }

        PlayerInfo pi; pi.id = (uint8_t)id; pi.name = name; pi.faction = faction;
        c->playerId = pi.id; c->helloDone = true;
        bool admin = false;
        if (c->authed) for (size_t i = 0; i < server.admins.size(); ++i) if (server.admins[i] == name) admin = true;
        { Lock l(cs); knownIds[name] = pi.id; addresses[pi.id] = c->ip; adminFlags[pi.id] = admin || (c->authed && accounts.isAdmin(name)); }

        ByteWriter w;
        { Lock l(cs);
          players.push_back(pi);
          w.u8(pi.id); w.u8((uint8_t)players.size());
          for (size_t i = 0; i < players.size(); ++i) { w.u8(players[i].id); w.str(players[i].name); w.str(players[i].faction); }
          w.u8((uint8_t)((server.dedicated ? WELCOME_DEDICATED : 0) | (server.accounts ? WELCOME_ACCOUNTS : 0))); }
        queue(c, controlPayload(MSG_WELCOME, w), false);

        ByteWriter j; j.u8(pi.id); j.str(pi.name); j.str(pi.faction);
        Bytes join = controlPayload(MSG_PLAYER_JOIN, j);
        for (size_t i = 0; i < conns.size(); ++i)
            if (conns[i] != c && conns[i]->helloDone && !conns[i]->dead) queue(conns[i], join, false);

        NetEvent e; e.kind = NetEvent::EV_PLAYER_JOINED; e.player = pi; push(e);
    }

    void reject(Conn* c, const char* reason)
    {
        ByteWriter w; w.str(reason);
        queue(c, controlPayload(MSG_REJECT, w), false);
        c->closeAfterFlush = true;
        c->closeDeadline = GetTickCount() + 3000;
    }

    void deliver(uint8_t sender, uint8_t type, const Bytes& body)
    {
        NetEvent e; e.kind = NetEvent::EV_MESSAGE; e.sender = sender; e.msgType = type; e.body = body;
        push(e);
    }

    void onDisconnect(Conn* c)
    {
        if (!hosting)
        {
            NetEvent e; e.kind = NetEvent::EV_DISCONNECTED; e.text = c->reason.empty() ? "connection lost" : c->reason; push(e);
            return;
        }
        if (!c->helloDone) return;
        { Lock l(cs);
          for (size_t i = 0; i < players.size(); ++i) if (players[i].id == c->playerId) { players.erase(players.begin() + i); break; }
          addresses.erase(c->playerId); adminFlags.erase(c->playerId); }
        Bytes leave; leave.push_back(MSG_PLAYER_LEAVE); leave.push_back(c->playerId);
        for (size_t i = 0; i < conns.size(); ++i)
            if (conns[i] != c && conns[i]->helloDone && !conns[i]->dead) queue(conns[i], leave, false);
        NetEvent e; e.kind = NetEvent::EV_PLAYER_LEFT; e.player.id = c->playerId; e.text = c->reason; push(e);
    }

    // -------------------------------------------------------------- client logic
    void handleClientPacket(Conn* c, const Bytes& p)
    {
        uint8_t type = p[0];
        Bytes body(p.begin() + 1, p.end());
        ByteReader r(body);

        switch (type)
        {
        case MSG_AUTH_CHALLENGE:
        {
            uint8_t fresh = r.u8(); std::string salt = r.str(); uint32_t iterations = r.u32(); std::string nonce = r.str();
            if (!r.ok() || salt.empty() || nonce.empty()) { c->kill("protocol error (bad login challenge)"); return; }
            if (iterations < MIN_ITERATIONS || iterations > 2000000) { c->kill("the server offers a weak login check: refused (your password is not sent)"); return; }
            std::string pw; { Lock l(cs); pw = accountPassword; }
            if (pw.empty()) { c->kill("this server needs an account password: set one in the MULTIPLAYER window (advanced settings, Password). The first login creates your account."); return; }
            Bytes key = authClientKey(pw, Bytes(salt.begin(), salt.end()), iterations);
            Bytes proof = fresh ? key : authProof(key, Bytes(nonce.begin(), nonce.end()), self.name);
            ByteWriter w; w.str(std::string(proof.begin(), proof.end()));
            queue(c, controlPayload(MSG_AUTH_PROOF, w), false);
            c->authed = true;
            break;
        }
        case MSG_AUTH_RESULT:
        {
            uint8_t ok = r.u8(); std::string why = r.str();
            if (r.ok() && !ok) { c->reason = "login refused: " + why; c->kill(c->reason.c_str()); }
            break;
        }
        case MSG_WELCOME:
        {
            uint8_t me = r.u8(); uint8_t count = r.u8();
            std::vector<PlayerInfo> list;
            for (int i = 0; i < count && r.ok(); ++i)
            {
                PlayerInfo pi; pi.id = r.u8(); pi.name = r.str(); pi.faction = r.str(); list.push_back(pi);
            }
            if (!r.ok()) { c->kill("protocol error (bad welcome)"); return; }
            uint8_t flags = r.remaining() ? r.u8() : 0;
            std::string pw; { Lock l(cs); pw = accountPassword; }
            if ((flags & WELCOME_DEDICATED) && !pw.empty() && !c->authed)
            { c->kill("this server did not ask for your account password (no secure login): refused"); return; }
            { Lock l(cs); localId = me; players = list; hostFlags = flags; hostServer = (flags & WELCOME_DEDICATED) != 0; }
            welcomed = true;
            for (size_t i = 0; i < heldBack.size(); ++i) queue(c, heldBack[i], false);
            heldBack.clear();
            NetEvent e; e.kind = NetEvent::EV_CONNECTED; e.player.id = me; push(e);
            break;
        }
        case MSG_PLAYER_JOIN:
        {
            PlayerInfo pi; pi.id = r.u8(); pi.name = r.str(); pi.faction = r.str();
            if (!r.ok()) return;
            { Lock l(cs); players.push_back(pi); }
            NetEvent e; e.kind = NetEvent::EV_PLAYER_JOINED; e.player = pi; push(e);
            break;
        }
        case MSG_PLAYER_LEAVE:
        {
            uint8_t id = r.u8();
            { Lock l(cs); for (size_t i = 0; i < players.size(); ++i) if (players[i].id == id) { players.erase(players.begin() + i); break; } }
            NetEvent e; e.kind = NetEvent::EV_PLAYER_LEFT; e.player.id = id; push(e);
            break;
        }
        case MSG_PONG:
        {
            uint32_t t = r.u32();
            if (r.ok()) { Lock l(cs); ping = (int)(GetTickCount() - t); }
            break;
        }
        case MSG_REJECT:
            c->reason = r.str();
            if (c->reason.empty()) c->reason = "rejected by host";
            c->kill(c->reason.c_str());
            break;
        default:
            if (isRelayed(type) && !body.empty())
            {
                Bytes rest(body.begin() + 1, body.end());
                deliver(body[0], type, rest);
            }
        }
    }
};

// ------------------------------------------------------------------ public API
Session::Session() : impl_(new SessionImpl()) {}
Session::~Session() { delete impl_; }

void Session::setPassword(const std::string& password)
{
    Bytes b(password.begin(), password.end());
    Lock l(impl_->cs);
    impl_->passwordHash = password.empty() ? 0 : (hashBytes(b) | 1u);   // never 0 when set
}

void Session::setAccountPassword(const std::string& password) { Lock l(impl_->cs); impl_->accountPassword = password; }
void Session::setTransport(Transport t) { Lock l(impl_->cs); impl_->transport = t; }
Transport Session::transport() const { Lock l(impl_->cs); return impl_->transport; }
void Session::setServer(const ServerOptions& o) { Lock l(impl_->cs); impl_->server = o; }
void Session::setNetSimulation(int lossPercent, int lagMs) { Lock l(impl_->cs); impl_->lossPct = lossPercent; impl_->lagMs = lagMs; }

void Session::setModList(const std::string& mods, bool strict)
{
    Lock l(impl_->cs);
    impl_->mods = mods;
    impl_->strictMods = strict;
}

bool Session::host(int port, const std::string& name, const std::string& faction, std::string& err)
{
    stop();
    return impl_->startHost(port, name, faction, err);
}

bool Session::join(const std::string& address, int port, const std::string& name,
                   const std::string& faction, std::string& err)
{
    stop();
    return impl_->startClient(address, port, name, faction, err);
}

void Session::stop()
{
    impl_->stop();
    Lock l(impl_->cs);
    impl_->events.clear(); impl_->outbox.clear(); impl_->players.clear(); impl_->kicks.clear(); impl_->peers.clear();
    impl_->addresses.clear(); impl_->adminFlags.clear();
    impl_->hosting = false; impl_->localId = 0; impl_->ping = 0; impl_->hostServer = false;
}

bool Session::active() const { return impl_->running; }
bool Session::isHost() const { Lock l(impl_->cs); return impl_->hosting; }
uint8_t Session::localId() const { Lock l(impl_->cs); return impl_->localId; }
int Session::pingMs() const { Lock l(impl_->cs); return impl_->ping; }
NetStats Session::stats() const { Lock l(impl_->cs); return impl_->stats; }
std::vector<PlayerInfo> Session::players() const { Lock l(impl_->cs); return impl_->players; }
bool Session::hostIsServer() const { Lock l(impl_->cs); return impl_->hostServer; }

void Session::kick(uint8_t player, const std::string& reason)
{
    Lock l(impl_->cs);
    impl_->kicks.push_back(std::make_pair(player, reason.empty() ? std::string("kicked by the server") : reason));
}

bool Session::ban(const std::string& what)
{
    if (what.empty()) return false;
    std::vector<uint8_t> victims;
    {
        Lock l(impl_->cs);
        bool ip = what.find_first_not_of("0123456789.") == std::string::npos && what.find('.') != std::string::npos;
        if (ip) impl_->banList.banIp(what); else impl_->banList.banName(what);
        for (size_t i = 0; i < impl_->players.size(); ++i)
        {
            const PlayerInfo& p = impl_->players[i];
            if (p.id == HOST_ID) continue;
            std::map<uint8_t, std::string>::iterator a = impl_->addresses.find(p.id);
            if (p.name == what || (a != impl_->addresses.end() && a->second == what)) victims.push_back(p.id);
        }
    }
    for (size_t i = 0; i < victims.size(); ++i) kick(victims[i], "banned from this server");
    return true;
}
bool Session::unban(const std::string& what) { Lock l(impl_->cs); return impl_->banList.unban(what); }
std::string Session::bans() const { Lock l(impl_->cs); return impl_->banList.list(); }
bool Session::isAdmin(uint8_t player) const
{
    Lock l(impl_->cs);
    std::map<uint8_t, bool>::const_iterator it = impl_->adminFlags.find(player);
    return it != impl_->adminFlags.end() && it->second;
}
std::string Session::address(uint8_t player) const
{
    Lock l(impl_->cs);
    std::map<uint8_t, std::string>::const_iterator it = impl_->addresses.find(player);
    return it == impl_->addresses.end() ? std::string() : it->second;
}
std::vector<PeerStats> Session::peerStats() const { Lock l(impl_->cs); return impl_->peers; }
size_t Session::accountCount() const { Lock l(impl_->cs); return impl_->accounts.size(); }
bool Session::resetAccount(const std::string& name) { Lock l(impl_->cs); return impl_->accounts.remove(name); }

bool Session::pollEvent(NetEvent& out)
{
    Lock l(impl_->cs);
    if (impl_->events.empty()) return false;
    out = impl_->events.front();
    impl_->events.pop_front();
    return true;
}

void Session::send(uint8_t msgType, const Bytes& body)
{
    if (!impl_->running) return;
    Outgoing o; o.target = BROADCAST; o.type = msgType; o.body = body;
    Lock l(impl_->cs); impl_->outbox.push_back(o);
}

void Session::setKnownSlots(const std::map<std::string, uint8_t>& slots)
{
    Lock l(impl_->cs);
    for (std::map<std::string, uint8_t>::const_iterator it = slots.begin(); it != slots.end(); ++it)
        if (it->second > 0 && it->second < MAX_PLAYERS && !it->first.empty() && !impl_->knownIds.count(it->first))
            impl_->knownIds[it->first] = it->second;
}

std::map<std::string, uint8_t> Session::knownSlots() const
{
    Lock l(impl_->cs);
    return impl_->knownIds;
}

void Session::sendTo(uint8_t player, uint8_t msgType, const Bytes& body)
{
    if (!impl_->running) return;
    Outgoing o; o.target = player; o.type = msgType; o.body = body;
    Lock l(impl_->cs); impl_->outbox.push_back(o);
}

// Blocking: the server list asks each server from a worker thread.
bool Session::queryInfo(const std::string& address, int port, Transport t, ServerInfo& out, int& pingMs, std::string& err, int timeoutMs)
{
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) { err = "WSAStartup failed"; return false; }
    bool ok = false;
    sockaddr_in sa;
    DWORD start = GetTickCount();
    if (!resolve(address, port, t == TRANSPORT_UDP ? SOCK_DGRAM : SOCK_STREAM, sa, err)) { WSACleanup(); return false; }
    if (t == TRANSPORT_UDP)
    {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET)
        {
            uint32_t token = GetTickCount() * 2654435761u;
            DWORD lastSend = 0;
            while (!ok && GetTickCount() - start < (DWORD)timeoutMs)
            {
                if (GetTickCount() - lastSend >= 400)
                {
                    lastSend = GetTickCount();
                    ByteWriter w; w.u8(UDP_INFO_REQ); w.u32(token);
                    sendto(s, (const char*)&w.data[0], (int)w.data.size(), 0, (sockaddr*)&sa, sizeof(sa));
                }
                fd_set rs; FD_ZERO(&rs); FD_SET(s, &rs);
                timeval tv; tv.tv_sec = 0; tv.tv_usec = 50000;
                if (select(0, &rs, NULL, NULL, &tv) > 0)
                {
                    char buf[2048];
                    int r = recvfrom(s, buf, sizeof(buf), 0, NULL, NULL);
                    if (r > 5 && (uint8_t)buf[0] == UDP_INFO)
                    {
                        ByteReader rr((const uint8_t*)buf + 1, r - 1);
                        if (rr.u32() == token) { out.read(rr); ok = rr.ok(); }
                    }
                }
            }
            closesocket(s);
        }
    }
    else
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET)
        {
            setNonBlocking(s);
            connect(s, (sockaddr*)&sa, sizeof(sa));
            fd_set ws; FD_ZERO(&ws); FD_SET(s, &ws);
            timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
            if (select(0, NULL, &ws, NULL, &tv) > 0)
            {
                Bytes frame; Bytes req(1, (uint8_t)MSG_SERVER_INFO_REQ); appendFrame(frame, req);
                ::send(s, (const char*)&frame[0], (int)frame.size(), 0);
                Bytes in;
                while (!ok && GetTickCount() - start < (DWORD)timeoutMs)
                {
                    fd_set rs; FD_ZERO(&rs); FD_SET(s, &rs);
                    timeval t2; t2.tv_sec = 0; t2.tv_usec = 50000;
                    if (select(0, &rs, NULL, NULL, &t2) <= 0) continue;
                    char buf[4096];
                    int r = recv(s, buf, sizeof(buf), 0);
                    if (r <= 0) break;
                    in.insert(in.end(), buf, buf + r);
                    if (in.size() >= 4)
                    {
                        uint32_t len; memcpy(&len, &in[0], 4);
                        if (len > 65536) break;
                        if (in.size() >= 4 + len && len > 1 && in[4] == MSG_SERVER_INFO)
                        {
                            ByteReader rr(&in[5], len - 1); out.read(rr); ok = rr.ok();
                        }
                    }
                }
            }
            closesocket(s);
        }
    }
    pingMs = (int)(GetTickCount() - start);
    if (!ok && err.empty()) err = "no answer";
    WSACleanup();
    return ok;
}

bool Session::httpGet(const std::string& url, std::string& body, std::string& err, int timeoutMs)
{
    if (url.compare(0, 7, "http://") != 0) { err = "only http:// addresses are supported"; return false; }
    std::string rest = url.substr(7);
    size_t slash = rest.find('/');
    std::string hp = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = hp.find(':');
    std::string host = colon == std::string::npos ? hp : hp.substr(0, colon);
    int port = colon == std::string::npos ? 80 : atoi(hp.c_str() + colon + 1);
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) { err = "WSAStartup failed"; return false; }
    bool ok = false;
    sockaddr_in sa;
    if (resolve(host, port, SOCK_STREAM, sa, err))
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET)
        {
            DWORD to = (DWORD)timeoutMs;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
            if (connect(s, (sockaddr*)&sa, sizeof(sa)) == 0)
            {
                std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: KenshiMP\r\nConnection: close\r\n\r\n";
                ::send(s, req.data(), (int)req.size(), 0);
                std::string resp;
                char buf[4096];
                int r;
                while ((r = recv(s, buf, sizeof(buf), 0)) > 0 && resp.size() < 1024 * 1024) resp.append(buf, r);
                size_t hdr = resp.find("\r\n\r\n");
                if (resp.compare(0, 9, "HTTP/1.0 ") == 0 || resp.compare(0, 9, "HTTP/1.1 ") == 0)
                {
                    int code = atoi(resp.c_str() + 9);
                    ok = code == 200 && hdr != std::string::npos;
                    if (ok) body = resp.substr(hdr + 4);
                    else { char c[64]; sprintf_s(c, "HTTP status %d", code); err = c; }
                }
                else err = resp.empty() ? "no answer" : "not an HTTP answer";
            }
            else err = lastWsaError("connect");
            closesocket(s);
        }
    }
    WSACleanup();
    return ok;
}

} // namespace mp
