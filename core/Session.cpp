#include "Session.h"

#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <deque>
#include <map>
#include <stdio.h>

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

struct Conn
{
    SOCKET s;
    Bytes in, out;
    uint8_t playerId;
    bool helloDone;
    bool closeAfterFlush;
    bool dead;
    std::string reason;     // why it died (for EV_PLAYER_LEFT / EV_DISCONNECTED)
    DWORD lastRecv;
    bool connecting;        // client: TCP connection not established yet (checked by the net thread)
    DWORD connectDeadline;
    std::string target;     // client: "address" for messages
    Conn() : s(INVALID_SOCKET), playerId(0), helloDone(false), closeAfterFlush(false), dead(false), lastRecv(GetTickCount()),
             connecting(false), connectDeadline(0) {}
    void kill(const char* why) { if (!dead) { dead = true; reason = why; } }
};

// High-rate messages that a newer one supersedes: safe to skip for a congested peer.
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

std::string lastWsaError(const char* what)
{
    char buf[128];
    sprintf_s(buf, sizeof(buf), "%s failed (WSA error %d)", what, WSAGetLastError());
    return buf;
}

void setNonBlocking(SOCKET s)
{
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
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

} // namespace

class SessionImpl
{
public:
    SessionImpl() : thread(NULL), running(false), hosting(false), localId(0), ping(0), strictMods(true), passwordHash(0),
                    listenSock(INVALID_SOCKET), wsaStarted(false), lastPing(0) { InitializeCriticalSection(&cs); }
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
    std::map<std::string, uint8_t> knownIds;  // host: name -> slot, kept for the session
    NetStats stats;

    // ---- net thread only ----
    SOCKET listenSock;
    std::vector<Conn*> conns;
    PlayerInfo self;
    bool wsaStarted;
    DWORD lastPing;

    bool startWsa(std::string& err)
    {
        if (wsaStarted) return true;
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) { err = "WSAStartup failed"; return false; }
        wsaStarted = true;
        return true;
    }

    bool startHost(int port, const std::string& name, const std::string& faction, std::string& err)
    {
        if (!startWsa(err)) return false;
        listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) { err = lastWsaError("socket"); return false; }
        // No SO_REUSEADDR: on Windows it would let a second host silently share the port.
        sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((u_short)port);
        if (bind(listenSock, (sockaddr*)&a, sizeof(a)) != 0 || listen(listenSock, 8) != 0)
        {
            err = lastWsaError("bind/listen") + " - port already used by another program or game?";
            closesocket(listenSock); listenSock = INVALID_SOCKET;
            return false;
        }
        setNonBlocking(listenSock);
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
        addrinfo hints; memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        char portStr[16]; sprintf_s(portStr, sizeof(portStr), "%d", port);
        addrinfo* res = NULL;
        if (getaddrinfo(addr.c_str(), portStr, &hints, &res) != 0 || !res) { err = "cannot resolve " + addr; return false; }

        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { freeaddrinfo(res); err = lastWsaError("socket"); return false; }
        setNonBlocking(s);
        int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
        freeaddrinfo(res);
        if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) { err = lastWsaError("connect"); closesocket(s); return false; }

        // The connection completes (or fails) on the network thread: the game never waits for an
        // unreachable host. A failure arrives as EV_DISCONNECTED.
        Conn* c = new Conn();
        c->s = s; c->playerId = HOST_ID; c->helloDone = true;
        c->connecting = rc != 0; c->connectDeadline = GetTickCount() + CONNECT_TIMEOUT_MS; c->target = addr;
        conns.push_back(c);
        hosting = false;
        self.name = name; self.faction = faction;

        ByteWriter w2;
        w2.u32(PROTOCOL_VERSION); w2.str(name); w2.str(faction); w2.str(mods); w2.u32(passwordHash);
        Bytes hello; hello.push_back(MSG_HELLO); hello.insert(hello.end(), w2.data.begin(), w2.data.end());
        appendFrame(c->out, hello);
        launch();
        return true;
    }

    std::string connectFailure(Conn* c)
    {
        return "connection to " + c->target + " timed out / refused (host not started, wrong IP, or port closed on the host's router/firewall?)";
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
        for (size_t i = 0; i < conns.size(); ++i) { if (conns[i]->s != INVALID_SOCKET) closesocket(conns[i]->s); delete conns[i]; }
        conns.clear();
        if (listenSock != INVALID_SOCKET) { closesocket(listenSock); listenSock = INVALID_SOCKET; }
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
        if (droppable && c->out.size() > SOFT_BACKLOG) { Lock l(cs); ++stats.statesDropped; return; }   // superseded soon anyway
        if (c->out.size() + payload.size() > HARD_BACKLOG) { c->kill("too slow (send backlog overflow)"); return; }
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
        { Lock l(cs); box.swap(outbox); }
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
                queue(conns[0], p, drop);
            }
        }
    }

    void loop()
    {
        while (running)
        {
            drainOutbox();
            DWORD now = GetTickCount();

            if (!hosting && !conns.empty() && now - lastPing > PING_MS)
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
                if (c->connecting) { if ((int)(now - c->connectDeadline) >= 0) c->kill(connectFailure(c).c_str()); }
                else if (now - c->lastRecv > TIMEOUT_MS) c->kill("timed out (no data for 15 s)");
            }

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
                        SOCKET ns = accept(listenSock, NULL, NULL);
                        if (ns != INVALID_SOCKET)
                        {
                            setNonBlocking(ns);
                            Conn* c = new Conn(); c->s = ns; conns.push_back(c);
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

            // reap dead connections
            for (size_t i = 0; i < conns.size();)
            {
                if (conns[i]->dead) { onDisconnect(conns[i]); closesocket(conns[i]->s); delete conns[i]; conns.erase(conns.begin() + i); }
                else ++i;
            }
            if (!hosting && conns.empty()) { running = false; break; }
        }
    }

    void readConn(Conn* c)
    {
        char buf[65536];
        int r = recv(c->s, buf, sizeof(buf), 0);
        if (r == 0) { c->kill("connection closed"); return; }
        if (r < 0) { if (WSAGetLastError() != WSAEWOULDBLOCK) c->kill("connection reset"); return; }
        c->lastRecv = GetTickCount();
        { Lock l(cs); stats.bytesIn += r; }
        c->in.insert(c->in.end(), buf, buf + r);

        size_t pos = 0;
        while (c->in.size() - pos >= 4)
        {
            uint32_t len; memcpy(&len, &c->in[pos], 4);
            if (len == 0 || len > MAX_PACKET) { c->kill("protocol error (bad frame)"); return; }
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
        { Lock l(cs); stats.bytesOut += r; }
        c->out.erase(c->out.begin(), c->out.begin() + r);
    }

    // ---------------------------------------------------------------- host logic
    void handleHostPacket(Conn* c, const Bytes& p)
    {
        uint8_t type = p[0];
        Bytes body(p.begin() + 1, p.end());

        if (!c->helloDone)
        {
            if (type != MSG_HELLO) { c->kill("protocol error (no hello)"); return; }
            ByteReader r(body);
            uint32_t ver = r.u32(); std::string name = r.str(); std::string faction = r.str();
            if (!r.ok()) { c->kill("protocol error (bad hello)"); return; }
            if (ver != PROTOCOL_VERSION)
            {
                char msg[128];
                sprintf_s(msg, sizeof(msg), "KenshiMP version mismatch (host protocol %u, yours %u): install the same mod version", PROTOCOL_VERSION, ver);
                reject(c, msg);
                return;
            }
            std::string theirMods = r.str();
            uint32_t theirPass = r.u32();
            if (!r.ok()) { c->kill("protocol error (bad hello)"); return; }
            std::string myMods;
            bool strict;
            uint32_t myPass;
            { Lock l(cs); myMods = mods; strict = strictMods; myPass = passwordHash; }
            if (myPass && theirPass != myPass) { reject(c, "wrong session password"); return; }
            if (theirMods != myMods)
            {
                std::string text = "mod lists differ (player '" + name + "'). " + modDiff(myMods, theirMods);
                if (strict) { reject(c, ("Your mods must match the host's. " + text).c_str()); return; }
                NetEvent w; w.kind = NetEvent::EV_WARNING; w.text = text; push(w);
            }

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
            { Lock l(cs);
              // Same name as earlier in this session -> same slot (mirror faction, relations, ids).
              std::map<std::string, uint8_t>::iterator prev = knownIds.find(name);
              if (prev != knownIds.end())
              {
                  bool used = false;
                  for (size_t i = 0; i < players.size(); ++i) if (players[i].id == prev->second) used = true;
                  if (!used) id = prev->second;
              }
              // New name: prefer slots nobody used before (keep returning players' slots),
              // fall back to any free slot when the session is nearly full.
              for (int pass = 0; pass < 2 && id < 0; ++pass)
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
            { Lock l(cs); knownIds[name] = pi.id; }

            ByteWriter w;
            { Lock l(cs);
              players.push_back(pi);
              w.u8(pi.id); w.u8((uint8_t)players.size());
              for (size_t i = 0; i < players.size(); ++i) { w.u8(players[i].id); w.str(players[i].name); w.str(players[i].faction); } }
            Bytes welcome; welcome.push_back(MSG_WELCOME); welcome.insert(welcome.end(), w.data.begin(), w.data.end());
            queue(c, welcome, false);

            ByteWriter j; j.u8(pi.id); j.str(pi.name); j.str(pi.faction);
            Bytes join; join.push_back(MSG_PLAYER_JOIN); join.insert(join.end(), j.data.begin(), j.data.end());
            for (size_t i = 0; i < conns.size(); ++i)
                if (conns[i] != c && conns[i]->helloDone && !conns[i]->dead) queue(conns[i], join, false);

            NetEvent e; e.kind = NetEvent::EV_PLAYER_JOINED; e.player = pi; push(e);
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
        if (type == MSG_DAMAGE || type == MSG_BUILDING_DAMAGE || type == MSG_ITEM_TAKE || type == MSG_ITEM_GIVE || type == MSG_GROUND_TAKE || type == MSG_TRADE || type == MSG_ITEM_UNDO || type == MSG_ZONE_MODE)
        {
            if (body.empty()) return;
            uint8_t target = body[0];
            if (target == HOST_ID) deliver(sender, type, body);
            else if (Conn* t = findConn(target)) queue(t, relayedPayload(type, sender, body), false);
            return;
        }

        deliver(sender, type, body);
        Bytes out = relayedPayload(type, sender, body);
        bool drop = isDroppable(type);
        for (size_t i = 0; i < conns.size(); ++i)
            if (conns[i] != c && conns[i]->helloDone && !conns[i]->dead) queue(conns[i], out, drop);
    }

    void reject(Conn* c, const char* reason)
    {
        ByteWriter w; w.str(reason);
        Bytes p; p.push_back(MSG_REJECT); p.insert(p.end(), w.data.begin(), w.data.end());
        queue(c, p, false);
        c->closeAfterFlush = true;
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
          for (size_t i = 0; i < players.size(); ++i) if (players[i].id == c->playerId) { players.erase(players.begin() + i); break; } }
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
        case MSG_WELCOME:
        {
            uint8_t me = r.u8(); uint8_t count = r.u8();
            std::vector<PlayerInfo> list;
            for (int i = 0; i < count && r.ok(); ++i)
            {
                PlayerInfo pi; pi.id = r.u8(); pi.name = r.str(); pi.faction = r.str(); list.push_back(pi);
            }
            if (!r.ok()) { c->kill("protocol error (bad welcome)"); return; }
            { Lock l(cs); localId = me; players = list; }
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
    impl_->events.clear(); impl_->outbox.clear(); impl_->players.clear();
    impl_->hosting = false; impl_->localId = 0; impl_->ping = 0;
}

bool Session::active() const { return impl_->running; }
bool Session::isHost() const { Lock l(impl_->cs); return impl_->hosting; }
uint8_t Session::localId() const { Lock l(impl_->cs); return impl_->localId; }
int Session::pingMs() const { Lock l(impl_->cs); return impl_->ping; }
NetStats Session::stats() const { Lock l(impl_->cs); return impl_->stats; }
std::vector<PlayerInfo> Session::players() const { Lock l(impl_->cs); return impl_->players; }

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

void Session::sendTo(uint8_t player, uint8_t msgType, const Bytes& body)
{
    if (!impl_->running) return;
    Outgoing o; o.target = player; o.type = msgType; o.body = body;
    Lock l(impl_->cs); impl_->outbox.push_back(o);
}

} // namespace mp
