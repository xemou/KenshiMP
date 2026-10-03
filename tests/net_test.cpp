// Loopback test: 1 host + 2 clients. Checks handshake, relaying, targeted damage and disconnects.
#include "../core/Session.h"
#include "../core/MovementGuard.h"
#include "../core/ItemDiff.h"
#include "../core/LoadSharing.h"
#include "../core/Interp.h"
#include "../core/Tunnel.h"
#include <deque>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

using namespace mp;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

// Poll until predicate-event arrives or timeout.
static bool waitEvent(Session& s, NetEvent::Kind kind, NetEvent& out, int ms = 3000, int msgType = -1)
{
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < (DWORD)ms)
    {
        NetEvent e;
        while (s.pollEvent(e))
            if (e.kind == kind && (msgType < 0 || e.msgType == msgType)) { out = e; return true; }
        Sleep(5);
    }
    return false;
}

static void serialisationTests()
{
    AppearanceBlob a;
    a.s.push_back(std::make_pair(std::string("hair"), std::string("mohawk")));
    a.i.push_back(std::make_pair(std::string("age"), 42));
    a.f.push_back(std::make_pair(std::string("height"), 1.25f));
    a.b.push_back(std::make_pair(std::string("female"), (uint8_t)1));
    std::vector<AppearanceBlob::Ref> refs;
    AppearanceBlob::Ref ref; ref.sid = "12-race.mod"; ref.v0 = 1; ref.v1 = -2; ref.v2 = 3; refs.push_back(ref);
    a.lists.push_back(std::make_pair(std::string("race"), refs));
    ByteWriter w; a.write(w);
    ByteReader r(w.data);
    AppearanceBlob b; b.read(r);
    CHECK(r.ok() && r.remaining() == 0);
    CHECK(b.s.size() == 1 && b.s[0].second == "mohawk");
    CHECK(b.i[0].second == 42 && b.f[0].second == 1.25f && b.b[0].second == 1);
    CHECK(b.lists.size() == 1 && b.lists[0].second[0].sid == "12-race.mod" && b.lists[0].second[0].v1 == -2);

    BuildingMsg m; m.netId = makeNetId(3, 9); m.gameDataName = "wall"; m.x = 1; m.qw = 0.5f; m.buildProgress = 0.7f; m.completed = 1;
    ByteWriter w2; m.write(w2);
    ByteReader r2(w2.data);
    BuildingMsg m2; m2.read(r2);
    CHECK(r2.ok() && m2.netId == m.netId && m2.gameDataName == "wall" && m2.qw == 0.5f && m2.buildProgress == 0.7f && m2.completed == 1);

    ItemRef it; it.item = "katana"; it.manufacturer = "edge"; it.material = "steel";
    ByteWriter w3; it.write(w3);
    ByteReader r3(w3.data);
    ItemRef it2; it2.read(r3);
    CHECK(r3.ok() && it2 == it);

    EntityState es; es.netId = 7; es.flags = EntityState::IN_COMBAT | EntityState::RUNNING; es.moveSpeed = 2; es.blood = 180;
    es.flesh.push_back(90); es.flesh.push_back(-20.5f);
    es.combatTarget.kind = TargetRef::NET_CHARACTER; es.combatTarget.netId = makeNetId(2, 5);
    es.task = 87; es.taskSubject.kind = TargetRef::WORLD_BUILDING; es.taskSubject.sid = "123-gamedata.base"; es.taskSubject.x = 4;
    ByteWriter w5; es.write(w5);
    ByteReader r5(w5.data);
    EntityState es2; es2.read(r5);
    CHECK(r5.ok() && r5.remaining() == 0 && es2.flesh.size() == 2 && es2.flesh[1] == -20.5f && es2.blood == 180);
    CHECK(es2.combatTarget == es.combatTarget && es2.taskSubject == es.taskSubject && es2.task == 87 && es2.moveSpeed == 2);

    BuildingMsg bm; bm.netId = 1; bm.doors.push_back(BuildingMsg::DOOR_BROKEN); bm.doors.push_back(BuildingMsg::DOOR_LOCKED); bm.destroyed = 1;
    ByteWriter w6; bm.write(w6);
    ByteReader r6(w6.data);
    BuildingMsg bm2; bm2.read(r6);
    CHECK(r6.ok() && bm2.doors == bm.doors && bm2.destroyed == 1);

    // Truncated input must fail cleanly, not crash.
    Bytes cut(w.data.begin(), w.data.begin() + w.data.size() / 2);
    ByteReader r4(cut);
    AppearanceBlob c; c.read(r4);
    CHECK(!r4.ok());
}

// ---------------------------------------------------------------- robustness tests
// A raw TCP peer, to misbehave in ways a Session never would.
struct RawPeer
{
    SOCKET s;
    RawPeer() : s(INVALID_SOCKET) {}
    ~RawPeer() { close(); }
    bool connectTo(int port)
    {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a; memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return connect(s, (sockaddr*)&a, sizeof(a)) == 0;
    }
    void sendFrame(const Bytes& payload)
    {
        uint32_t n = (uint32_t)payload.size();
        ::send(s, (const char*)&n, 4, 0);
        if (n) ::send(s, (const char*)&payload[0], (int)n, 0);
    }
    void sendHello(uint32_t version, const char* name, const char* mods)
    {
        ByteWriter w; w.u8(MSG_HELLO); w.u32(version); w.str(name); w.str("Raw Faction"); w.str(mods); w.u32(0);
        sendFrame(w.data);
    }
    // Reads one frame (blocking up to ms). Returns false on timeout / closed.
    bool readFrame(Bytes& out, int ms)
    {
        DWORD t0 = GetTickCount();
        Bytes buf;
        while (GetTickCount() - t0 < (DWORD)ms)
        {
            fd_set rs; FD_ZERO(&rs); FD_SET(s, &rs);
            timeval tv; tv.tv_sec = 0; tv.tv_usec = 50000;
            if (select(0, &rs, NULL, NULL, &tv) > 0)
            {
                char tmp[4096];
                int r = recv(s, tmp, sizeof(tmp), 0);
                if (r <= 0) return false;
                buf.insert(buf.end(), tmp, tmp + r);
            }
            if (buf.size() >= 4)
            {
                uint32_t n; memcpy(&n, &buf[0], 4);
                if (buf.size() >= 4 + n) { out.assign(buf.begin() + 4, buf.begin() + 4 + n); return true; }
            }
        }
        return false;
    }
    // True once the peer closed the connection (within ms).
    bool closedWithin(int ms)
    {
        DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < (DWORD)ms)
        {
            fd_set rs; FD_ZERO(&rs); FD_SET(s, &rs);
            timeval tv; tv.tv_sec = 0; tv.tv_usec = 50000;
            if (select(0, &rs, NULL, NULL, &tv) > 0)
            {
                char tmp[4096];
                int r = recv(s, tmp, sizeof(tmp), 0);
                if (r <= 0) return true;
            }
        }
        return false;
    }
    void close() { if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; } }
};

static void sanitizeTests()
{
    float nan = 0.f; nan = nan / nan;
    EntityState s; s.x = nan;
    CHECK(!s.sanitize());
    EntityState t; t.x = 1; t.y = 2; t.z = 3; t.qx = nan; t.blood = -5; t.tx = 1e9f;
    t.flesh.push_back(nan); t.flesh.push_back(1e9f);
    CHECK(t.sanitize());
    CHECK(t.qw == 1 && t.qx == 0 && t.blood == 0 && t.tx == 1 && t.flesh[0] == -1000.f && t.flesh[1] == 1000.f);
    EntityState outside; outside.x = 5e6f;
    CHECK(!outside.sanitize());

    DamageMsg d; d.cut = 1e9f; d.blunt = -3; d.pierce = nan;
    d.sanitize();
    CHECK(d.cut == MAX_HIT && d.blunt == 0 && d.pierce == 0);

    BuildingMsg b; b.gameDataName = "x"; b.y = nan;
    CHECK(!b.sanitize());
    BuildingMsg b2; b2.gameDataName = "x"; b2.qw = 0; b2.qx = 0; b2.buildProgress = nan;
    CHECK(b2.sanitize() && b2.qw == 1 && b2.buildProgress == 0);

    EntitySpawn sp; sp.gameDataName = "tmpl"; sp.x = nan;
    CHECK(!sp.sanitize());
    EntitySpawn sp2; sp2.x = 1;
    CHECK(!sp2.sanitize());                                  // no template id

    InvItem it; CHECK(!it.sanitize());                        // no item id
    InvItem it2; it2.item = "sword"; it2.quantity = -4; it2.quality = nan;
    CHECK(it2.sanitize() && it2.quantity == 1 && it2.quality == 0);
    InvItem it3; it3.item = "ore"; it3.quantity = 5000000;
    CHECK(it3.sanitize() && it3.quantity == 100000);
    InvItem a1; a1.item = "x"; a1.material = "m"; InvItem a2 = a1; a2.quantity = 9;
    CHECK(a1.sameKind(a2)); a2.material = "n"; CHECK(!a1.sameKind(a2));
}

static void modListTests()
{
    std::string err;
    NetEvent e;
    {   // strict: refused, with a readable reason on the client
        Session host, c;
        host.setModList("base;modA", true);
        c.setModList("base;modB", true);
        CHECK(host.host(47110, "H", "HF", err));
        CHECK(c.join("127.0.0.1", 47110, "C", "CF", err));
        CHECK(waitEvent(c, NetEvent::EV_DISCONNECTED, e));
        CHECK(e.text.find("must match") != std::string::npos && e.text.find("modA") != std::string::npos);
        CHECK(e.text.find("Missing (enable them): modA") != std::string::npos);
        CHECK(e.text.find("Not on the host (disable them): modB") != std::string::npos);
        Sleep(100);
        CHECK(!c.active());
        CHECK(host.players().size() == 1);
    }
    {   // same mods, other order
        Session host, c;
        host.setModList("base;modA;modB", true);
        c.setModList("base;modB;modA", true);
        CHECK(host.host(47112, "H", "HF", err));
        CHECK(c.join("127.0.0.1", 47112, "C", "CF", err));
        CHECK(waitEvent(c, NetEvent::EV_DISCONNECTED, e));
        CHECK(e.text.find("load order") != std::string::npos);
    }
    {   // lenient: accepted, both warned on the host side
        Session host, c;
        host.setModList("base;modA", false);
        c.setModList("base;modB", false);
        CHECK(host.host(47111, "H", "HF", err));
        CHECK(c.join("127.0.0.1", 47111, "C", "CF", err));
        CHECK(waitEvent(c, NetEvent::EV_CONNECTED, e));
        CHECK(waitEvent(host, NetEvent::EV_WARNING, e));
        CHECK(e.text.find("modB") != std::string::npos);
    }
    {   // identical lists: plain connect
        Session host, c;
        host.setModList("base;modA", true);
        c.setModList("base;modA", true);
        CHECK(host.host(47112, "H", "HF", err));
        CHECK(c.join("127.0.0.1", 47112, "C", "CF", err));
        CHECK(waitEvent(c, NetEvent::EV_CONNECTED, e));
    }
}

static void passwordAndIdentityTests()
{
    std::string err;
    NetEvent e;
    Session host;
    host.setPassword("kenshi");
    CHECK(host.host(47130, "H", "HF", err));
    {   // wrong password: refused with a clear reason
        Session c; c.setPassword("nope");
        CHECK(c.join("127.0.0.1", 47130, "Intruder", "IF", err));
        CHECK(waitEvent(c, NetEvent::EV_DISCONNECTED, e));
        CHECK(e.text.find("password") != std::string::npos);
    }
    {   // no password at all: refused too
        Session c;
        CHECK(c.join("127.0.0.1", 47130, "Intruder2", "IF", err));
        CHECK(waitEvent(c, NetEvent::EV_DISCONNECTED, e));
    }
    // Right password: accepted. Reconnecting keeps the same slot even if someone joined meanwhile.
    uint8_t firstId = 0;
    {
        Session bob; bob.setPassword("kenshi");
        CHECK(bob.join("127.0.0.1", 47130, "Bob", "BF", err));
        CHECK(waitEvent(bob, NetEvent::EV_CONNECTED, e));
        firstId = bob.localId();
    }
    CHECK(waitEvent(host, NetEvent::EV_PLAYER_LEFT, e, 5000));
    Session carol; carol.setPassword("kenshi");
    CHECK(carol.join("127.0.0.1", 47130, "Carol", "CF", err));
    CHECK(waitEvent(carol, NetEvent::EV_CONNECTED, e));
    CHECK(carol.localId() != firstId);                       // Bob's slot is kept for Bob
    Session bob2; bob2.setPassword("kenshi");
    CHECK(bob2.join("127.0.0.1", 47130, "Bob", "BF", err));
    CHECK(waitEvent(bob2, NetEvent::EV_CONNECTED, e));
    CHECK(bob2.localId() == firstId);
    printf("  password + stable slot: Bob back in slot %d, Carol in %d\n", (int)bob2.localId(), (int)carol.localId());
}

// The host restarts (new Session): the numbers it saved (players.cfg) are given back, so a returning
// player gets its old number even if someone else connects first.
static void savedSlotsTests()
{
    std::string err;
    NetEvent e;
    std::map<std::string, uint8_t> saved;
    {
        Session host;
        CHECK(host.host(47140, "H", "HF", err));
        Session a, b;
        CHECK(a.join("127.0.0.1", 47140, "Ann", "AF", err));
        CHECK(waitEvent(a, NetEvent::EV_CONNECTED, e));
        CHECK(b.join("127.0.0.1", 47140, "Ben", "BF", err));
        CHECK(waitEvent(b, NetEvent::EV_CONNECTED, e));
        CHECK(a.localId() == 1 && b.localId() == 2);
        saved = host.knownSlots();
        CHECK(saved.size() == 2 && saved["Ben"] == 2);
    }
    Session host;
    host.setKnownSlots(saved);
    CHECK(host.host(47141, "H", "HF", err));
    Session newcomer, ben;
    CHECK(newcomer.join("127.0.0.1", 47141, "Cid", "CF", err));
    CHECK(waitEvent(newcomer, NetEvent::EV_CONNECTED, e));
    CHECK(ben.join("127.0.0.1", 47141, "Ben", "BF", err));
    CHECK(waitEvent(ben, NetEvent::EV_CONNECTED, e));
    CHECK(ben.localId() == 2);                                // Ben's number survived the restart
    CHECK(newcomer.localId() != 1 && newcomer.localId() != 2); // free slots first, Ann's kept for her
    printf("  saved slots after a host restart: Ben back in slot %d, newcomer in %d\n", (int)ben.localId(), (int)newcomer.localId());
}

static void misbehavingPeerTests()
{
    std::string err;
    NetEvent e;
    Session host;
    CHECK(host.host(47120, "H", "HF", err));

    // Wrong protocol version: rejected with an explicit reason, then closed.
    {
        RawPeer p; CHECK(p.connectTo(47120));
        p.sendHello(PROTOCOL_VERSION + 100, "Old", "");
        Bytes f;
        CHECK(p.readFrame(f, 3000) && !f.empty() && f[0] == MSG_REJECT);
        ByteReader r(f); r.u8();
        std::string why = r.str();
        CHECK(why.find("version") != std::string::npos);
        CHECK(p.closedWithin(3000));
    }
    // Garbage frame (absurd length): dropped without affecting the host.
    {
        RawPeer p; CHECK(p.connectTo(47120));
        uint32_t huge = 0xFFFFFFF0u;
        ::send(p.s, (const char*)&huge, 4, 0);
        CHECK(p.closedWithin(3000));
    }
    // Something that is not a hello first: dropped.
    {
        RawPeer p; CHECK(p.connectTo(47120));
        ByteWriter w; w.u8(MSG_CHAT); w.str("hi");
        p.sendFrame(w.data);
        CHECK(p.closedWithin(3000));
    }
    // Host still serves normal players afterwards.
    {
        Session c;
        CHECK(c.join("127.0.0.1", 47120, "Normal", "NF", err));
        CHECK(waitEvent(c, NetEvent::EV_CONNECTED, e));
        c.stop();
        CHECK(waitEvent(host, NetEvent::EV_PLAYER_LEFT, e));
    }
    while (host.pollEvent(e)) {}

    // Silent peer: dropped after the timeout, with the reason reported.
    {
        RawPeer p; CHECK(p.connectTo(47120));
        p.sendHello(PROTOCOL_VERSION, "Mute", "");
        CHECK(waitEvent(host, NetEvent::EV_PLAYER_JOINED, e));
        DWORD t0 = GetTickCount();
        CHECK(waitEvent(host, NetEvent::EV_PLAYER_LEFT, e, TIMEOUT_MS + 5000));
        DWORD took = GetTickCount() - t0;
        CHECK(took >= TIMEOUT_MS - 1000 && e.text.find("timed out") != std::string::npos);
        printf("  silent peer dropped after %lu ms (%s)\n", took, e.text.c_str());
    }

    // Peer that never reads: superseded states are skipped (no disconnect), then a flood of
    // reliable data overflows the hard limit and it is disconnected instead of eating memory.
    {
        RawPeer p; CHECK(p.connectTo(47120));
        int small = 1; setsockopt(p.s, SOL_SOCKET, SO_RCVBUF, (const char*)&small, sizeof(small));
        p.sendHello(PROTOCOL_VERSION, "Slow", "");
        CHECK(waitEvent(host, NetEvent::EV_PLAYER_JOINED, e));
        Bytes state(60000, 7);
        for (int i = 0; i < 400; ++i) host.send(MSG_ENTITY_STATE, state);   // 24 MB of droppable data
        Sleep(1500);
        NetEvent left;
        CHECK(!waitEvent(host, NetEvent::EV_PLAYER_LEFT, left, 500));        // still connected
        Bytes chat(100000, 'x');
        for (int i = 0; i < 150; ++i) host.send(MSG_CHAT, chat);            // 15 MB reliable
        CHECK(waitEvent(host, NetEvent::EV_PLAYER_LEFT, left, 10000));
        CHECK(left.text.find("backlog") != std::string::npos);
        printf("  slow peer: %s\n", left.text.c_str());
    }

    // Host going away: the client is told, and its session stops.
    {
        Session c;
        CHECK(c.join("127.0.0.1", 47120, "Leaver", "LF", err));
        CHECK(waitEvent(c, NetEvent::EV_CONNECTED, e));
        host.stop();
        CHECK(waitEvent(c, NetEvent::EV_DISCONNECTED, e));
        Sleep(100);
        CHECK(!c.active());
        printf("  host gone: client told '%s'\n", e.text.c_str());
    }
}

// Every decoder must survive arbitrary bytes: fail cleanly (r.ok() == false), never crash,
// never allocate absurdly.
static InvItem inv(const char* sid, int q, const char* mat = "")
{
    InvItem d; d.item = sid; d.material = mat; d.quantity = q; return d;
}

static void itemDiffTests()
{
    // Loot window: took 2 of 5 bread and the sword, put 10 arrows.
    ItemContent before, now;
    before.add(inv("bread", 5)); before.add(inv("sword", 1, "steel"));
    now.add(inv("bread", 3)); now.add(inv("arrow", 10));
    std::vector<InvItem> taken, given;
    diffContent(before, now, taken, given);
    CHECK(taken.size() == 2 && given.size() == 1);
    bool bread = false, sword = false;
    for (size_t i = 0; i < taken.size(); ++i)
    {
        if (taken[i].item == "bread" && taken[i].quantity == 2) bread = true;
        if (taken[i].item == "sword" && taken[i].material == "steel" && taken[i].quantity == 1) sword = true;
    }
    CHECK(bread && sword);
    CHECK(given[0].item == "arrow" && given[0].quantity == 10);
    // Two stacks of the same kind count as one total; moving items around changes nothing.
    ItemContent a, b;
    a.add(inv("bread", 2)); a.add(inv("bread", 3));
    b.add(inv("bread", 5));
    CHECK(a == b);
    taken.clear(); given.clear();
    diffContent(a, b, taken, given);
    CHECK(taken.empty() && given.empty());
    // Same item, other material: a different kind.
    ItemContent c, d2;
    c.add(inv("sword", 1, "steel")); d2.add(inv("sword", 1, "iron"));
    CHECK(c != d2);
    printf("  item diff: loot/put detected per kind, restacking ignored\n");
}

static void movementGuardTests()
{
    // 20 Hz samples of a fast runner (45 u/s): never flagged.
    MovementGuard g;
    int flags = 0;
    for (int i = 0; i <= 200; ++i) if (g.observe(1, 1000 + i * 50, i * 45.f * 0.05f, 0.f) != MovementGuard::OK) ++flags;
    CHECK(flags == 0);
    // Speed hack at 120 u/s: flagged as FAST.
    MovementGuard h;
    float v = 0; bool fast = false;
    for (int i = 0; i <= 40; ++i) if (h.observe(2, i * 50, i * 120.f * 0.05f, 0.f, &v) == MovementGuard::FAST) fast = true;
    CHECK(fast && v > 100.f);
    // Teleport: 200 units in one step.
    MovementGuard j;
    j.observe(3, 0, 0, 0); j.observe(3, 50, 1, 0);
    float jump = 0;
    CHECK(j.observe(3, 100, 201, 0, &jump) == MovementGuard::JUMP && jump > 150.f);
    // A long pause (no samples for 5 s) then a far position: not a teleport.
    MovementGuard p;
    p.observe(4, 0, 0, 0);
    CHECK(p.observe(4, 5000, 300, 0) == MovementGuard::OK);
    // Forgetting a player (world reload): its next far position is accepted.
    MovementGuard f;
    f.observe(makeNetId(2, 1), 0, 0, 0); f.observe(makeNetId(2, 1), 50, 0, 0);
    f.forgetOwner(2);
    CHECK(f.tracked() == 0);
    CHECK(f.observe(makeNetId(2, 1), 100, 500, 0) == MovementGuard::OK);
    printf("  movement guard: runner 45 u/s ok, 120 u/s flagged (%.0f), teleport flagged (%.0f)\n", v, jump);
}

static void fuzzTests()
{
    srand(12345);
    int decoded = 0;
    for (int round = 0; round < 20000; ++round)
    {
        Bytes b(rand() % 300);
        for (size_t i = 0; i < b.size(); ++i) b[i] = (uint8_t)(rand() & 0xFF);
        // Bias some rounds towards "almost valid": small counts at the front.
        if (!b.empty() && (round & 3) == 0) b[0] = (uint8_t)(rand() % 4);
        { ByteReader r(b); EntityState s; s.read(r); if (r.ok()) { s.sanitize(); ++decoded; } }
        { ByteReader r(b); EntitySpawn s; s.read(r); if (r.ok()) s.sanitize(); }
        { ByteReader r(b); BuildingMsg m; m.read(r); if (r.ok()) m.sanitize(); }
        { ByteReader r(b); DamageMsg d; d.read(r); d.sanitize(); }
        { ByteReader r(b); BuildingDamageMsg d; d.read(r); d.sanitize(); }
        { ByteReader r(b); AppearanceBlob a; a.read(r); }
        { ByteReader r(b); ItemRef it; it.read(r); }
        { ByteReader r(b); InvItem it; it.read(r); if (r.ok()) it.sanitize(); }
        {   // MSG_GROUND_ITEM body, decoded like ground_onMessage does
            ByteReader r(b); r.u32(); InvItem it; it.read(r); float x = r.f32(), y = r.f32(), z = r.f32();
            if (r.ok()) { it.sanitize(); validPos(x, y, z); }
        }
        {   // MSG_INVENTORY body, decoded like items_onMessage does
            ByteReader r(b); r.u8(); r.u32(); uint16_t n = r.u16();
            if (r.ok() && n <= MAX_INV_ITEMS) for (uint16_t i = 0; i < n && r.ok(); ++i) { InvItem it; it.read(r); if (r.ok()) it.sanitize(); }
        }
        { ByteReader r(b); TargetRef t; t.read(r); }
    }
    printf("  fuzz: 20000 random buffers through every decoder (%d decoded as states), no crash\n", decoded);
}

// ---------------------------------------------------------------- smoothness simulation
// True path: running zig-zag at 6 units/s, turning every 1.5 s.
static void truePos(double tMs, float& x, float& z)
{
    double t = tMs / 1000.0;
    int leg = (int)(t / 1.5);
    double into = t - leg * 1.5;
    x = 0; z = 0;
    for (int i = 0; i <= leg; ++i)
    {
        double len = (i < leg ? 1.5 : into) * 6.0;
        double ang = (i % 2) ? 0.6 : -0.6;
        x += (float)(sin(ang) * len); z += (float)(cos(ang) * len);
    }
}

struct SmoothResult { double meanErr, maxErr, maxSpeed, stallFrames; };

// Simulates sender -> TCP network (delay + jitter + spikes, order preserved) -> receiver rendering
// at 60 fps. interpolated=false models "jump to the latest received position".
static SmoothResult simulateMovement(bool interpolated, double sendHz, unsigned seed, int jitterMax = 60, int spikePct = 2, int spikeMs = 200)
{
    srand(seed);
    const double sendMs = 1000.0 / sendHz;
    const uint32_t senderClockOffset = 123456789u;    // unrelated clocks
    struct Pkt { double arrival; uint32_t senderMs; float x, z, vx, vz; };
    std::vector<Pkt> pkts;
    double lastArrival = 0;
    float px = 0, pz = 0;
    for (double t = 0; t < 20000; t += sendMs)
    {
        float x, z; truePos(t, x, z);
        Pkt p;
        p.senderMs = (uint32_t)t + senderClockOffset;
        p.x = x; p.z = z;
        p.vx = t > 0 ? (float)((x - px) * 1000.0 / sendMs) : 0;
        p.vz = t > 0 ? (float)((z - pz) * 1000.0 / sendMs) : 0;
        px = x; pz = z;
        double delay = 40 + (rand() % (jitterMax + 1));
        if (rand() % 100 < spikePct) delay += spikeMs;      // latency spikes
        p.arrival = t + delay;
        if (p.arrival < lastArrival) p.arrival = lastArrival;   // TCP keeps order
        lastArrival = p.arrival;
        pkts.push_back(p);
    }

    ClockSync clock;
    InterpBuffer buf;
    PlaybackClock playback;
    SmoothFollower follow;
    size_t next = 0;
    float latestX = 0, latestZ = 0;
    double errSum = 0, errMax = 0, maxSpeed = 0, stalls = 0;
    int n = 0;
    float prevX = 0, prevZ = 0;
    bool havePrev = false;
    for (double now = 0; now < 20000; now += 1000.0 / 60)
    {
        while (next < pkts.size() && pkts[next].arrival <= now)
        {
            const Pkt& p = pkts[next++];
            clock.observe(p.senderMs, (uint32_t)p.arrival);
            Snapshot s; memset(&s, 0, sizeof(s));
            s.t = clock.toLocal(p.senderMs); s.x = p.x; s.z = p.z; s.vx = p.vx; s.vz = p.vz; s.qw = 1;
            buf.push(s);
            latestX = p.x; latestZ = p.z;
        }
        if (now < 1000 || buf.empty()) continue;
        float sx, sz;
        double shownT;   // sender-time the displayed position should correspond to
        if (interpolated)
        {
            double delay = renderDelayMs(sendMs, clock.jitterMs());
            double rt = playback.update(now, now - delay);
            Snapshot o;
            buf.sample(rt, o);
            follow.update(o, 1000.0 / 60);
            sx = follow.x; sz = follow.z;
            shownT = rt - clock.toLocal(senderClockOffset);   // local -> sender-relative
        }
        else
        {
            sx = latestX; sz = latestZ;
            shownT = now - 70;   // average network delay
        }
        float tx, tz; truePos(shownT, tx, tz);
        double err = sqrt((double)(sx - tx) * (sx - tx) + (double)(sz - tz) * (sz - tz));
        errSum += err; if (err > errMax) errMax = err; ++n;
        if (havePrev)
        {
            double step = sqrt((double)(sx - prevX) * (sx - prevX) + (double)(sz - prevZ) * (sz - prevZ));
            double speed = step * 60.0;
            if (speed > maxSpeed) maxSpeed = speed;
            if (step < 1e-4) ++stalls;                     // frozen frame while it should run
        }
        prevX = sx; prevZ = sz; havePrev = true;
    }
    SmoothResult r; r.meanErr = errSum / n; r.maxErr = errMax; r.maxSpeed = maxSpeed; r.stallFrames = stalls * 100.0 / n;
    return r;
}

static void smoothnessTests()
{
    SmoothResult naive = simulateMovement(false, 10, 7);
    SmoothResult smooth = simulateMovement(true, 20, 7);
    printf("  movement (true speed 6.0 u/s, 40-100 ms latency + 2%% spikes of +200 ms):\n");
    printf("    naive 10 Hz : max apparent speed %5.1f u/s, frozen frames %4.1f %%, mean err %.2f, max err %.2f\n",
           naive.maxSpeed, naive.stallFrames, naive.meanErr, naive.maxErr);
    printf("    interp 20 Hz: max apparent speed %5.1f u/s, frozen frames %4.1f %%, mean err %.2f, max err %.2f\n",
           smooth.maxSpeed, smooth.stallFrames, smooth.meanErr, smooth.maxErr);
    // Smooth motion: never faster than ~1.5x the real speed, practically never frozen.
    CHECK(smooth.maxSpeed < 9.0);
    CHECK(smooth.stallFrames < 1.0);
    CHECK(smooth.meanErr < 0.5);
    CHECK(naive.stallFrames > 50.0);                         // the old behaviour stutters

    SmoothResult bad = simulateMovement(true, 20, 11, 150, 5, 400);
    printf("    bad wifi    : max apparent speed %5.1f u/s, frozen frames %4.1f %%, mean err %.2f, max err %.2f  (40-190 ms, 5%% spikes +400 ms)\n",
           bad.maxSpeed, bad.stallFrames, bad.meanErr, bad.maxErr);
    CHECK(bad.maxSpeed < 9.0);
    CHECK(bad.stallFrames < 3.0);

    // ClockSync with wrapping clocks.
    ClockSync c;
    c.observe(0xFFFFFF00u, 1000); c.observe(0x00000010u, 1300);
    CHECK(fabs(c.toLocal(0x00000010u) - 1300.0) < 20.0 || fabs(c.toLocal(0xFFFFFF00u) - 1000.0) < 20.0);
}

static void loadSharingTests()
{
    // First decision: shared only when close.
    CHECK(shareMode(SHARE_UNKNOWN, 500.f) == SHARE_SHARED);
    CHECK(shareMode(SHARE_UNKNOWN, 2500.f) == SHARE_APART);
    // Hysteresis: once shared, stays shared up to SHARE_LEAVE; once apart, needs SHARE_ENTER.
    CHECK(shareMode(SHARE_SHARED, 2500.f) == SHARE_SHARED);
    CHECK(shareMode(SHARE_SHARED, 3100.f) == SHARE_APART);
    CHECK(shareMode(SHARE_APART, 2500.f) == SHARE_APART);
    CHECK(shareMode(SHARE_APART, 1900.f) == SHARE_SHARED);
    // Walking back and forth along one limit never flips more than once.
    int mode = SHARE_UNKNOWN, flips = 0;
    for (int i = 0; i < 200; ++i)
    {
        float d = 2900.f + (i % 2 ? 150.f : -150.f);   // 2750 .. 3050
        int m = shareMode(mode, d);
        if (mode != SHARE_UNKNOWN && m != mode) ++flips;
        mode = m;
    }
    CHECK(flips <= 1);
}

// In-process stand-in for Steam's P2P network: two endpoints, reliable and ordered.
struct FakeP2P
{
    CRITICAL_SECTION cs;
    std::deque<std::pair<uint64_t, Bytes> > toA, toB;   // (from, message)
    FakeP2P() { InitializeCriticalSection(&cs); }
    ~FakeP2P() { DeleteCriticalSection(&cs); }
};
class FakeEnd : public P2PTransport
{
public:
    FakeEnd(FakeP2P* n, bool a) : net(n), isA(a), closes(0) {}
    bool send(uint64_t peer, const uint8_t* d, uint32_t size)
    {
        EnterCriticalSection(&net->cs);
        (isA ? net->toB : net->toA).push_back(std::make_pair(isA ? (uint64_t)1 : (uint64_t)2, Bytes(d, d + size)));
        LeaveCriticalSection(&net->cs);
        return true;
    }
    bool receive(uint64_t& peer, Bytes& out)
    {
        EnterCriticalSection(&net->cs);
        std::deque<std::pair<uint64_t, Bytes> >& q = isA ? net->toA : net->toB;
        bool any = !q.empty();
        if (any) { peer = q.front().first; out = q.front().second; q.pop_front(); }
        LeaveCriticalSection(&net->cs);
        return any;
    }
    void close(uint64_t) { ++closes; }
    FakeP2P* net; bool isA; volatile long closes;
};

// The session over the tunnel: host endpoint A (peer id 1), client endpoint B (peer id 2).
static void tunnelTests()
{
    FakeP2P net;
    FakeEnd hostEnd(&net, true), clientEnd(&net, false);
    Session host, client;
    std::string err;
    CHECK(host.host(47160, "TunnelHost", "Host Faction", err));
    TunnelHost th; TunnelClient tc;
    CHECK(th.start(&hostEnd, 47160, err));
    CHECK(tc.start(&clientEnd, 1, err));
    CHECK(tc.localPort() > 0);
    CHECK(client.join("127.0.0.1", tc.localPort(), "TunnelClient", "Client Faction", err));
    NetEvent e;
    CHECK(waitEvent(client, NetEvent::EV_CONNECTED, e, 5000));
    CHECK(waitEvent(host, NetEvent::EV_PLAYER_JOINED, e, 5000) && e.player.name == "TunnelClient");
    CHECK(th.peers() == 1);
    // both ways, and a big message (fragmented into many tunnel chunks) arrives intact
    { ByteWriter w; w.str("hello through the tunnel"); client.send(MSG_CHAT, w.data); }
    CHECK(waitEvent(host, NetEvent::EV_MESSAGE, e, 3000, MSG_CHAT));
    { ByteReader r(e.body); CHECK(r.str() == "hello through the tunnel"); }
    ByteWriter big; for (int i = 0; i < 50000; ++i) big.u32((uint32_t)i * 2654435761u);
    host.send(MSG_CHAT, big.data);
    CHECK(waitEvent(client, NetEvent::EV_MESSAGE, e, 5000, MSG_CHAT));
    CHECK(e.body == big.data);
    // client leaves: the host sees it, the tunnel stream is closed
    client.stop();
    CHECK(waitEvent(host, NetEvent::EV_PLAYER_LEFT, e, 5000));
    Sleep(200);
    CHECK(th.peers() == 0);
    // the host leaves: the client's tunnel is told (BYE)
    Session again;
    CHECK(again.join("127.0.0.1", tc.localPort(), "TunnelClient", "Client Faction", err));
    CHECK(waitEvent(again, NetEvent::EV_CONNECTED, e, 5000));
    th.stop();
    CHECK(waitEvent(again, NetEvent::EV_DISCONNECTED, e, 5000));
    Sleep(200);
    CHECK(tc.hostClosed());
    again.stop(); tc.stop(); host.stop();
}

int main()
{
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    smoothnessTests();
    fuzzTests();
    movementGuardTests();
    itemDiffTests();
    loadSharingTests();
    tunnelTests();
    sanitizeTests();
    modListTests();
    passwordAndIdentityTests();
    savedSlotsTests();
    misbehavingPeerTests();
    serialisationTests();

    Session host, a, b;
    std::string err;
    CHECK(host.host(47100, "Alice", "Alice's Hive", err));
    CHECK(a.join("127.0.0.1", 47100, "Bob", "Bob's Holy Nation", err));

    NetEvent e;
    CHECK(waitEvent(a, NetEvent::EV_CONNECTED, e));
    CHECK(a.localId() == 1);
    CHECK(waitEvent(host, NetEvent::EV_PLAYER_JOINED, e));
    CHECK(e.player.name == "Bob");

    CHECK(b.join("127.0.0.1", 47100, "Carol", "Carol's Shek", err));
    CHECK(waitEvent(b, NetEvent::EV_CONNECTED, e));
    CHECK(b.localId() == 2);
    CHECK(b.players().size() == 3);
    CHECK(waitEvent(a, NetEvent::EV_PLAYER_JOINED, e));   // a is told about Carol
    CHECK(e.player.id == 2);

    // Entity state from Bob is relayed to host AND Carol, tagged with sender 1.
    {
        EntityState st; st.netId = makeNetId(1, 7); st.x = 10; st.y = 20; st.z = 30; st.health = 0.5f;
        ByteWriter w; w.u32(777); w.u16(1); st.write(w);
        a.send(MSG_ENTITY_STATE, w.data);

        NetEvent hostEv, carolEv;
        CHECK(waitEvent(host, NetEvent::EV_MESSAGE, hostEv, 3000, MSG_ENTITY_STATE));
        CHECK(waitEvent(b, NetEvent::EV_MESSAGE, carolEv, 3000, MSG_ENTITY_STATE));
        CHECK(hostEv.sender == 1 && carolEv.sender == 1);
        ByteReader r(carolEv.body);
        CHECK(r.u32() == 777);
        CHECK(r.u16() == 1);
        EntityState got; got.read(r);
        CHECK(r.ok() && got.netId == makeNetId(1, 7) && got.x == 10 && got.z == 30 && got.health == 0.5f);
    }

    // Damage from Carol to Bob's character: only Bob must receive it.
    {
        DamageMsg d; d.victimNetId = makeNetId(1, 7); d.attackerNetId = makeNetId(2, 1); d.cut = 12.f;
        ByteWriter w; w.u8(1); d.write(w);          // target player = Bob
        b.send(MSG_DAMAGE, w.data);

        NetEvent bob;
        CHECK(waitEvent(a, NetEvent::EV_MESSAGE, bob, 3000, MSG_DAMAGE));
        CHECK(bob.sender == 2);
        ByteReader r(bob.body);
        CHECK(r.u8() == 1);
        DamageMsg got; got.read(r);
        CHECK(r.ok() && got.victimNetId == makeNetId(1, 7) && got.cut == 12.f);
        NetEvent none;
        CHECK(!waitEvent(host, NetEvent::EV_MESSAGE, none, 300, MSG_DAMAGE));   // host not involved
    }

    // World states: a client's report goes to the host only; the host's table reaches everybody.
    {
        ByteWriter w; w.u8(HOST_ID); w.u16(1); w.str("1234-gamedata.base"); w.u8(0); w.u8(1);
        b.send(MSG_WORLD_STATE_REPORT, w.data);
        NetEvent got, none;
        CHECK(waitEvent(host, NetEvent::EV_MESSAGE, got, 3000, MSG_WORLD_STATE_REPORT));
        CHECK(got.sender == 2);
        ByteReader r(got.body);
        CHECK(r.u8() == HOST_ID && r.u16() == 1 && r.str() == "1234-gamedata.base" && r.u8() == 0 && r.u8() == 1 && r.ok());
        CHECK(!waitEvent(a, NetEvent::EV_MESSAGE, none, 300, MSG_WORLD_STATE_REPORT));   // Bob not involved

        ByteWriter t; t.u16(0); t.u16(1); t.str("town-a"); t.str("town-a-ruins");
        host.send(MSG_WORLD_STATES, t.data);
        NetEvent ea, eb;
        CHECK(waitEvent(a, NetEvent::EV_MESSAGE, ea, 3000, MSG_WORLD_STATES));
        CHECK(waitEvent(b, NetEvent::EV_MESSAGE, eb, 3000, MSG_WORLD_STATES));
        CHECK(ea.sender == HOST_ID && eb.sender == HOST_ID);
        ByteReader rt(eb.body);
        CHECK(rt.u16() == 0 && rt.u16() == 1 && rt.str() == "town-a" && rt.str() == "town-a-ruins" && rt.ok());
    }

    // Town buildings: a client's door hit goes to the host only; the host's states reach everybody.
    {
        WorldBuildingState ref; ref.sid = "gate-1"; ref.x = 100.f; ref.y = 5.f; ref.z = -20.f;
        BuildingDamageMsg d; d.door = 2; d.attackerNetId = makeNetId(2, 1); d.blunt = 30.f;
        ByteWriter w; w.u8(HOST_ID); w.u8(0); ref.write(w); d.write(w);
        b.send(MSG_WORLD_BUILDING_REPORT, w.data);
        NetEvent got, none;
        CHECK(waitEvent(host, NetEvent::EV_MESSAGE, got, 3000, MSG_WORLD_BUILDING_REPORT));
        ByteReader r(got.body);
        CHECK(r.u8() == HOST_ID && r.u8() == 0);
        WorldBuildingState ref2; ref2.read(r); BuildingDamageMsg d2; d2.read(r);
        CHECK(r.ok() && ref2.sanitize() && ref2.sid == "gate-1" && ref2.x == 100.f && d2.door == 2 && d2.blunt == 30.f);
        CHECK(!waitEvent(a, NetEvent::EV_MESSAGE, none, 300, MSG_WORLD_BUILDING_REPORT));

        WorldBuildingState st = ref; st.destroyed = 0; st.brokenDoors.push_back(0); st.brokenDoors.push_back(1);
        ByteWriter t; t.u16(1); st.write(t);
        host.send(MSG_WORLD_BUILDINGS, t.data);
        NetEvent ea, eb;
        CHECK(waitEvent(a, NetEvent::EV_MESSAGE, ea, 3000, MSG_WORLD_BUILDINGS));
        CHECK(waitEvent(b, NetEvent::EV_MESSAGE, eb, 3000, MSG_WORLD_BUILDINGS));
        ByteReader rt(eb.body);
        WorldBuildingState back;
        CHECK(rt.u16() == 1); back.read(rt);
        CHECK(rt.ok() && back.sameState(st) && back.brokenDoors.size() == 2 && back.brokenDoors[1] == 1);
        WorldBuildingState bad; bad.sid = "x"; bad.x = 1e30f; CHECK(!bad.sanitize());
    }

    // Bounty: a crime seen by the host's NPCs goes to the character's owner only.
    {
        ByteWriter w; w.u8(2); w.u16(1); w.u32(makeNetId(2, 4)); w.str("faction-x"); w.u32(300); w.u32(1u << 5);
        host.sendTo(2, MSG_BOUNTY_CRIME, w.data);
        NetEvent got, none;
        CHECK(waitEvent(b, NetEvent::EV_MESSAGE, got, 3000, MSG_BOUNTY_CRIME));
        ByteReader r(got.body);
        CHECK(got.sender == HOST_ID && r.u8() == 2 && r.u16() == 1 && r.u32() == makeNetId(2, 4) && r.str() == "faction-x" && r.u32() == 300 && r.ok());
        CHECK(!waitEvent(a, NetEvent::EV_MESSAGE, none, 300, MSG_BOUNTY_CRIME));
    }

    // Item transfer: Carol loots one of Bob's characters -> only Bob gets it.
    {
        InvItem it; it.item = "katana"; it.manufacturer = "smith"; it.material = "steel"; it.quantity = 1; it.quality = 42.f;
        ByteWriter w; w.u8(1); w.u8(CONTAINER_CHARACTER); w.u32(makeNetId(1, 3)); it.write(w);
        b.send(MSG_ITEM_TAKE, w.data);
        NetEvent got;
        CHECK(waitEvent(a, NetEvent::EV_MESSAGE, got, 3000, MSG_ITEM_TAKE));
        CHECK(got.sender == 2);
        ByteReader r(got.body);
        CHECK(r.u8() == 1 && r.u8() == CONTAINER_CHARACTER && r.u32() == makeNetId(1, 3));
        InvItem back; back.read(r);
        CHECK(r.ok() && back.sameKind(it) && back.quantity == 1 && back.quality == 42.f);
        NetEvent none;
        CHECK(!waitEvent(host, NetEvent::EV_MESSAGE, none, 300, MSG_ITEM_TAKE));
    }
    // Ground item: announced to everybody; a pickup goes to the owner only.
    {
        InvItem it; it.item = "bread"; it.quantity = 3;
        ByteWriter w; w.u32(makeNetId(1, 5)); it.write(w); w.f32(100.f); w.f32(20.f); w.f32(-40.f);
        a.send(MSG_GROUND_ITEM, w.data);
        NetEvent eh, eb;
        CHECK(waitEvent(host, NetEvent::EV_MESSAGE, eh, 3000, MSG_GROUND_ITEM));
        CHECK(waitEvent(b, NetEvent::EV_MESSAGE, eb, 3000, MSG_GROUND_ITEM));
        ByteReader r(eb.body);
        CHECK(r.u32() == makeNetId(1, 5));
        InvItem got; got.read(r);
        CHECK(r.ok() && got.item == "bread" && got.quantity == 3 && r.f32() == 100.f);

        ByteWriter t; t.u8(1); t.u32(makeNetId(1, 5));
        b.send(MSG_GROUND_TAKE, t.data);
        NetEvent ta;
        CHECK(waitEvent(a, NetEvent::EV_MESSAGE, ta, 3000, MSG_GROUND_TAKE));
        CHECK(ta.sender == 2);
        NetEvent none;
        CHECK(!waitEvent(host, NetEvent::EV_MESSAGE, none, 300, MSG_GROUND_TAKE));
    }
    // Trade offer: routed to the target player only, sender set by the host.
    {
        ByteWriter w; w.u8(2); w.u8(0); w.u32(makeNetId(1, 3)); w.u32(makeNetId(2, 4));
        a.send(MSG_TRADE, w.data);
        NetEvent got, none;
        CHECK(waitEvent(b, NetEvent::EV_MESSAGE, got, 3000, MSG_TRADE));
        CHECK(got.sender == 1 && got.body == w.data);
        CHECK(!waitEvent(host, NetEvent::EV_MESSAGE, none, 300, MSG_TRADE));
        // An accept addressed to the host reaches the host only.
        ByteWriter acc; acc.u8(0); acc.u8(1); acc.u32(makeNetId(2, 4)); acc.u32(makeNetId(0, 1));
        b.send(MSG_TRADE, acc.data);
        CHECK(waitEvent(host, NetEvent::EV_MESSAGE, got, 3000, MSG_TRADE));
        CHECK(got.sender == 2);
        CHECK(!waitEvent(a, NetEvent::EV_MESSAGE, none, 300, MSG_TRADE));
    }
    // Refused transfer: the owner's undo reaches the requester only.
    {
        InvItem it; it.item = "bread"; it.quantity = 2;
        ByteWriter w; w.u8(2); w.u8(UNDO_REMOVE); w.u8(CONTAINER_CHARACTER); w.u32(makeNetId(1, 3)); it.write(w);
        a.send(MSG_ITEM_UNDO, w.data);
        NetEvent got, none;
        CHECK(waitEvent(b, NetEvent::EV_MESSAGE, got, 3000, MSG_ITEM_UNDO));
        CHECK(got.sender == 1 && got.body == w.data);
        CHECK(!waitEvent(host, NetEvent::EV_MESSAGE, none, 300, MSG_ITEM_UNDO));
    }
    // Backpack content: an ordinary inventory broadcast with its own container kind.
    {
        ByteWriter w; w.u8(CONTAINER_BACKPACK); w.u32(makeNetId(1, 3)); w.u16(1);
        InvItem i1; i1.item = "dried meat"; i1.quantity = 4; i1.write(w);
        a.send(MSG_INVENTORY, w.data);
        NetEvent eb;
        CHECK(waitEvent(b, NetEvent::EV_MESSAGE, eb, 3000, MSG_INVENTORY));
        ByteReader r(eb.body);
        CHECK(r.u8() == CONTAINER_BACKPACK && r.u32() == makeNetId(1, 3) && r.u16() == 1);
        NetEvent eh; CHECK(waitEvent(host, NetEvent::EV_MESSAGE, eh, 3000, MSG_INVENTORY));
    }
    // Inventory mirror: broadcast to everybody.
    {
        ByteWriter w; w.u8(CONTAINER_BUILDING); w.u32(makeNetId(1, 9)); w.u16(2);
        InvItem i1; i1.item = "bread"; i1.quantity = 12; i1.write(w);
        InvItem i2; i2.item = "iron"; i2.quantity = 40; i2.write(w);
        a.send(MSG_INVENTORY, w.data);
        NetEvent eh, eb;
        CHECK(waitEvent(host, NetEvent::EV_MESSAGE, eh, 3000, MSG_INVENTORY));
        CHECK(waitEvent(b, NetEvent::EV_MESSAGE, eb, 3000, MSG_INVENTORY));
        CHECK(eb.sender == 1 && eb.body == w.data);
    }

    // Host -> everyone (chat).
    {
        ByteWriter w; w.str("hello war");
        host.send(MSG_CHAT, w.data);
        NetEvent ea, eb;
        CHECK(waitEvent(a, NetEvent::EV_MESSAGE, ea, 3000, MSG_CHAT));
        CHECK(waitEvent(b, NetEvent::EV_MESSAGE, eb, 3000, MSG_CHAT));
        CHECK(ea.sender == HOST_ID);
        ByteReader r(ea.body);
        CHECK(r.str() == "hello war");
    }

    // Ping.
    Sleep(2300);
    CHECK(a.pingMs() >= 0 && a.pingMs() < 200);

    // Same player reconnecting while the host still holds its old connection: same slot, old
    // connection closed, the others see one leave then one join (no duplicate player).
    {
        Session d1, d2;
        CHECK(d1.join("127.0.0.1", 47100, "Dave", "Dave's Swamp", err));
        NetEvent ev;
        CHECK(waitEvent(d1, NetEvent::EV_CONNECTED, ev));
        uint8_t first = d1.localId();
        CHECK(waitEvent(host, NetEvent::EV_PLAYER_JOINED, ev));
        CHECK(d2.join("127.0.0.1", 47100, "Dave", "Dave's Swamp", err));
        CHECK(waitEvent(d2, NetEvent::EV_CONNECTED, ev));
        CHECK(d2.localId() == first);
        CHECK(waitEvent(d1, NetEvent::EV_DISCONNECTED, ev, 3000));
        CHECK(waitEvent(host, NetEvent::EV_PLAYER_LEFT, ev) && ev.player.id == first);
        CHECK(waitEvent(host, NetEvent::EV_PLAYER_JOINED, ev) && ev.player.id == first);
        int daves = 0;
        std::vector<PlayerInfo> ps = host.players();
        for (size_t i = 0; i < ps.size(); ++i) if (ps[i].name == "Dave") ++daves;
        CHECK(daves == 1);
        d2.stop(); d1.stop();
        CHECK(waitEvent(host, NetEvent::EV_PLAYER_LEFT, ev));
        // drain what a and b were told about Dave
        while (a.pollEvent(ev)) {}
        while (b.pollEvent(ev)) {}
    }

    // Disconnect of Carol is announced.
    b.stop();
    CHECK(waitEvent(host, NetEvent::EV_PLAYER_LEFT, e));
    CHECK(e.player.id == 2);
    CHECK(waitEvent(a, NetEvent::EV_PLAYER_LEFT, e));

    // Wrong host: join returns at once, the failure arrives as an event (the game never waits).
    Session c;
    DWORD t0 = GetTickCount();
    bool started = c.join("127.0.0.1", 47199, "X", "X", err);
    CHECK(GetTickCount() - t0 < 500);
    if (started)
    {
        CHECK(waitEvent(c, NetEvent::EV_DISCONNECTED, e, 7000));
        CHECK(e.text.find("timed out / refused") != std::string::npos);
        c.stop();
    }

    host.stop(); a.stop();
    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
