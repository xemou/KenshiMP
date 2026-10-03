// Test bot: joins a KenshiMP host as a second player and mirrors the host's squad back as its
// own (offset by a few metres), so a single machine can exercise ghosts, appearance, gear,
// buildings and damage routing.
//
// usage: bot.exe [address] [port] [seconds] [offsetX|d<ms>] [recordFile|-] [modList] [ko]
//   ko[seconds]: 15 s (or the given delay) after connecting our copies fall unconscious and carry a lootable item; loot
//       requests (MSG_ITEM_TAKE) addressed to us are applied to that inventory.
//   Every DAMAGE the host sends to our (mirrored) characters is logged.
//   After 60 s the bot sends one small hit to every host character it knows (damage routing).
//   Feature script (seconds after connecting): 20 chat, 30 takes one item from a host character,
//   40 gives it back, 45 drops an item on the ground next to its copy, 50 declares war, 75 makes peace. Its mirrored characters lose their left arm
//   (limb sync) and carry the host's inventory (inventory mirror). Weather from the host is logged.
#include "../core/Session.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <string.h>
#include <map>
#include <set>
#include <vector>

using namespace mp;

static uint32_t mine(uint32_t hostId) { return makeNetId(1, hostId & 0xFFFFFF); }

// Mirror a reference: host objects become the bot's copies and vice versa, so if the host's
// character fights the bot's ghost, the ghost fights the host's character back.
static void swapRef(TargetRef& t)
{
    if (t.kind != TargetRef::NET_CHARACTER && t.kind != TargetRef::NET_BUILDING) return;
    t.netId = netIdOwner(t.netId) == 1 ? makeNetId(HOST_ID, t.netId & 0xFFFFFF) : mine(t.netId);
}

// First net id carried by an entity message (to tell world-NPC traffic apart).
static uint32_t firstId(uint8_t type, const Bytes& body)
{
    ByteReader r(body);
    if (type == MSG_ENTITY_STATE) { r.u32(); if (r.u16() == 0) return 0; }
    else if (type == MSG_INVENTORY) r.u8();
    else if (type != MSG_ENTITY_SPAWN && type != MSG_ENTITY_DESPAWN && type != MSG_APPEARANCE &&
             type != MSG_EQUIPMENT && type != MSG_STATS) return 0;
    uint32_t id = r.u32();
    return r.ok() ? id : 0;
}

struct Rec { DWORD ms; uint8_t type; Bytes body; };

// KMP_TEST_NEAR=1: the recorded NPCs are moved next to the client's character (its first state):
// they then stand within the client's "near" radius (off-screen immunity test).
static bool nearMode = false, haveClient = false, haveShift = false;
static float clientPos[3], shift[3];
static Bytes moveNear(uint8_t type, const Bytes& body)
{
    ByteReader r(body);
    if (type == MSG_ENTITY_SPAWN)
    {
        EntitySpawn sp; sp.read(r);
        if (!r.ok()) return body;
        if (!haveShift) { shift[0] = clientPos[0] + 20 - sp.x; shift[1] = clientPos[1] - sp.y; shift[2] = clientPos[2] + 20 - sp.z; haveShift = true; }
        sp.x += shift[0]; sp.y += shift[1]; sp.z += shift[2];
        ByteWriter w; sp.write(w); return w.data;
    }
    if (type == MSG_ENTITY_STATE && haveShift)
    {
        uint32_t clock = r.u32(); uint16_t n = r.u16();
        ByteWriter w; w.u32(clock); w.u16(n);
        for (int i = 0; i < n && r.ok(); ++i)
        {
            EntityState st; st.read(r);
            st.x += shift[0]; st.y += shift[1]; st.z += shift[2]; st.tx += shift[0]; st.ty += shift[1]; st.tz += shift[2];
            st.write(w);
        }
        return r.ok() ? w.data : body;
    }
    return body;
}

// Host mode: serve a recorded world-NPC stream to whoever joins (tests the client side).
static int hostReplay(const char* file, int port, int seconds, const char* mods)
{
    { char nm[8]; size_t nl = 0; nearMode = getenv_s(&nl, nm, sizeof(nm), "KMP_TEST_NEAR") == 0 && nl > 1; }
    std::vector<Rec> recs;
    FILE* f = NULL;
    if (fopen_s(&f, file, "rb") != 0 || !f) { printf("cannot open %s\n", file); return 1; }
    for (;;)
    {
        Rec r; uint32_t len = 0;
        if (fread(&r.ms, 4, 1, f) != 1 || fread(&r.type, 1, 1, f) != 1 || fread(&len, 4, 1, f) != 1) break;
        r.body.resize(len);
        if (len && fread(&r.body[0], 1, len, f) != len) break;
        recs.push_back(r);
    }
    fclose(f);
    printf("[host] %d recorded message(s) loaded\n", (int)recs.size());

    Session s; std::string err;
    s.setModList(mods, true);
    if (!s.host(port, "ReplayHost", "Host World", err)) { printf("host failed: %s\n", err.c_str()); return 1; }
    DWORD t0 = GetTickCount(), start = 0;
    size_t next = 0;
    int damages = 0;
    // KMP_TEST_ZONE=a,b: tell the client it is on its own at a s, back in our world at b s (load sharing).
    int zoneA = -1, zoneB = -1, zoneStep = -1; uint8_t joined = 0;
    {
        char z[32]; size_t zl = 0;
        if (getenv_s(&zl, z, sizeof(z), "KMP_TEST_ZONE") == 0 && zl > 1) sscanf_s(z, "%d,%d", &zoneA, &zoneB);
    }
    while (GetTickCount() - t0 < (DWORD)seconds * 1000)
    {
        NetEvent e;
        while (s.pollEvent(e))
        {
            if (e.kind == NetEvent::EV_PLAYER_JOINED) { printf("[host] %s joined, replaying\n", e.player.name.c_str()); start = GetTickCount(); next = 0; joined = e.player.id; zoneStep = 0; }
            else if (e.kind == NetEvent::EV_MESSAGE && e.msgType == MSG_ENTITY_STATE && !haveClient)
            {
                ByteReader r(e.body); r.u32(); uint16_t n = r.u16();
                if (n) { EntityState st; st.read(r); if (r.ok()) { clientPos[0] = st.x; clientPos[1] = st.y; clientPos[2] = st.z; haveClient = true; printf("[host] client character at (%.0f, %.0f, %.0f)\n", st.x, st.y, st.z); } }
            }
            else if (e.kind == NetEvent::EV_MESSAGE && e.msgType == MSG_DAMAGE)
            {
                ByteReader r(e.body); r.u8(); DamageMsg d; d.read(r);
                if (isNpcNetId(d.victimNetId)) printf("[host] client hit NPC %08x part %d blunt=%.1f cut=%.1f\n", d.victimNetId, (int)d.bodyPart, d.blunt, d.cut);
                ++damages;
            }
        }
        if (start && zoneA >= 0 && zoneStep >= 0)
        {
            DWORD since = (GetTickCount() - start) / 1000;
            int mode = -1;
            if (zoneStep == 0) { mode = 1; zoneStep = 1; }
            else if (zoneStep == 1 && (int)since >= zoneA) { mode = 0; zoneStep = 2; }
            else if (zoneStep == 2 && (int)since >= zoneB) { mode = 1; zoneStep = 3; }
            if (mode >= 0)
            {
                ByteWriter w; w.u8(joined); w.u8((uint8_t)mode);
                s.sendTo(joined, MSG_ZONE_MODE, w.data);
                printf("[host] zone mode for player %d: %s\n", (int)joined, mode ? "shared" : "on its own");
            }
        }
        if (start)
            while (next < recs.size() && GetTickCount() - start >= recs[next].ms)
            {
                if (nearMode && haveClient) { s.send(recs[next].type, moveNear(recs[next].type, recs[next].body)); ++next; continue; }
                if (nearMode && !haveClient) break;   // wait until we know where the client is
                s.send(recs[next].type, recs[next].body);
                ++next;
                if (next == recs.size()) printf("[host] replay finished\n");
            }
        Sleep(5);
    }
    printf("[host] done, %d damage message(s) received\n", damages);
    s.stop();
    return 0;
}

// Path recording: with KMP_PATH_RECORD=<file> in the environment, everything the bot sends for its
// own copies (spawn, looks, states) is written there with its time. --replay-path plays it back.
static FILE* g_pathRec = NULL;
static DWORD g_pathT0 = 0;
static void sendOwn(Session& s, uint8_t type, const Bytes& body)
{
    s.send(type, body);
    if (!g_pathRec) return;
    DWORD ms = GetTickCount() - g_pathT0; uint32_t len = (uint32_t)body.size();
    fwrite(&ms, 4, 1, g_pathRec); fwrite(&type, 1, 1, g_pathRec); fwrite(&len, 4, 1, g_pathRec);
    if (len) fwrite(&body[0], 1, len, g_pathRec);
}

// Client mode: replay a recorded path (same movement every run, for comparable measurements).
static int replayPath(const char* file, int loops, const char* mods)
{
    std::vector<Rec> recs;
    FILE* f = NULL;
    if (fopen_s(&f, file, "rb") != 0 || !f) { printf("cannot open %s\n", file); return 1; }
    for (;;)
    {
        Rec r; uint32_t len = 0;
        if (fread(&r.ms, 4, 1, f) != 1 || fread(&r.type, 1, 1, f) != 1 || fread(&len, 4, 1, f) != 1) break;
        r.body.resize(len);
        if (len && fread(&r.body[0], 1, len, f) != len) break;
        recs.push_back(r);
    }
    fclose(f);
    size_t firstState = 0;
    while (firstState < recs.size() && recs[firstState].type != MSG_ENTITY_STATE) ++firstState;
    printf("[path] %d message(s), first state at %d\n", (int)recs.size(), (int)firstState);
    if (firstState == recs.size()) return 1;

    Session s; std::string err;
    s.setModList(mods, true);
    if (!s.join("127.0.0.1", 47000, "Bot", "Bot Nation", err)) { printf("join failed: %s\n", err.c_str()); return 1; }
    bool connected = false;
    while (!connected)
    {
        NetEvent e;
        while (s.pollEvent(e))
        {
            if (e.kind == NetEvent::EV_CONNECTED) connected = true;
            if (e.kind == NetEvent::EV_DISCONNECTED) { printf("[path] disconnected: %s\n", e.text.c_str()); return 2; }
        }
        Sleep(10);
    }
    printf("[path] connected, replaying %d time(s)\n", loops);
    // Spawn and looks once, then the states, looped.
    for (size_t i = 0; i < firstState; ++i) s.send(recs[i].type, recs[i].body);
    Sleep(3000);
    DWORD lastLooks = GetTickCount();
    for (int loop = 0; loop < loops; ++loop)
    {
        DWORD start = GetTickCount(), base = recs[firstState].ms;
        for (size_t i = firstState; i < recs.size(); ++i)
        {
            while (GetTickCount() - start < recs[i].ms - base)
            {
                NetEvent e;
                while (s.pollEvent(e)) if (e.kind == NetEvent::EV_DISCONNECTED) { printf("[path] disconnected: %s\n", e.text.c_str()); return 2; }
                Sleep(2);
            }
            Bytes b = recs[i].body;
            if (recs[i].type == MSG_ENTITY_STATE && b.size() >= 4) { DWORD t = GetTickCount(); memcpy(&b[0], &t, 4); }
            if (recs[i].type == MSG_ENTITY_SPAWN || recs[i].type == MSG_ENTITY_STATE || GetTickCount() - lastLooks > 5000)
                s.send(recs[i].type, b);
        }
        printf("[path] loop %d done\n", loop + 1);
        Sleep(3000);
    }
    s.stop();
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   // logs are tailed while running
    if (argc > 2 && strcmp(argv[1], "--host-replay") == 0)
        return hostReplay(argv[2], argc > 3 ? atoi(argv[3]) : 47000, argc > 4 ? atoi(argv[4]) : 600,
                          argc > 5 ? argv[5] : "game 1.0.65;base;Newwworld;Dialogue;rebirth;KenshiMP");
    if (argc > 2 && strcmp(argv[1], "--replay-path") == 0)
        return replayPath(argv[2], argc > 3 ? atoi(argv[3]) : 3, argc > 4 ? argv[4] : "game 1.0.65;base;Newwworld;Dialogue;rebirth;KenshiMP");
    {
        char path[512]; size_t n = 0;
        if (getenv_s(&n, path, sizeof(path), "KMP_PATH_RECORD") == 0 && n > 1) { fopen_s(&g_pathRec, path, "wb"); g_pathT0 = GetTickCount(); }
    }

    const char* addr = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 47000;
    int seconds = argc > 3 ? atoi(argv[3]) : 300;
    // offsetX, or "d<ms>": follow the host's exact path that many ms behind (no side offset).
    float offset = 4.0f; DWORD delayMs = 0;
    // KMP_TEST_FAR=a,b: from a to b seconds after connecting, our copy stands 4000 units away
    // (load sharing: the host must hand our surroundings over to us, then take them back).
    int farFrom = -1, farTo = -1;
    {
        char farEnv[32]; size_t fl = 0;
        if (getenv_s(&fl, farEnv, sizeof(farEnv), "KMP_TEST_FAR") == 0 && fl > 1) sscanf_s(farEnv, "%d,%d", &farFrom, &farTo);
    }
    if (argc > 4) { if (argv[4][0] == 'd') { delayMs = (DWORD)atoi(argv[4] + 1); offset = 0.f; } else offset = (float)atof(argv[4]); }
    struct Delayed { DWORD due; Bytes body; };
    std::vector<Delayed> delayed;
    FILE* record = NULL;
    if (argc > 5 && strcmp(argv[5], "-") != 0) fopen_s(&record, argv[5], "wb");
    std::set<uint32_t> npcSeen;
    int npcStates = 0, npcDespawns = 0, npcInventories = 0;

    Session s;
    std::string err;
    // Must equal the host's active mod list (see the rejection message if it differs).
    s.setModList(argc > 6 ? argv[6] : "game 1.0.65;base;Newwworld;Dialogue;rebirth;KenshiMP", true);
    char botName[32] = "Bot"; size_t bnl = 0;
    if (getenv_s(&bnl, botName, sizeof(botName), "KMP_BOT_NAME") != 0 || bnl <= 1) strcpy_s(botName, "Bot");
    if (!s.join(addr, port, botName, std::string(botName) + " Nation", err)) { printf("join failed: %s\n", err.c_str()); return 1; }

    // "ko" or "ko<seconds>": our copies fall unconscious that long after connecting (default 15).
    bool koMode = argc > 7 && strncmp(argv[7], "ko", 2) == 0;
    DWORD koAfter = koMode && argv[7][2] ? (DWORD)atoi(argv[7] + 2) : 15;
    std::map<uint32_t, std::vector<InvItem> > myInv;   // our copies' loose items (ko mode)
    std::map<uint32_t, std::vector<InvItem> > myPack;  // our copies' backpack content
    bool packDirty = false;
    std::string spoofTemplate;   // a valid character template (spoof test)
    std::map<uint32_t, uint32_t> npcMoney;   // host NPCs that carry money (traders)
    std::set<uint32_t> hostChars;
    int states = 0, spawns = 0, looks = 0, builds = 0, damages = 0;
    bool hitSent = false, lastCombat = false, worldSeen = false;
    uint16_t lastTask = 0xFFFF;
    DWORD t0 = GetTickCount(), lastReport = t0, connectedAt = 0;
    std::map<uint32_t, std::vector<InvItem> > hostInv;   // host character -> its loose items
    uint32_t tookFrom = 0; InvItem took;
    std::map<uint32_t, std::vector<ItemRef> > hostGear;   // host character -> equipped items
    float copyX = 0, copyY = 0, copyZ = 0; bool copyKnown = false;   // where our first copy stands
    std::set<uint32_t> myGround;                           // our announced ground items
    int script = 0;
    bool weatherSeen = false;

    while (GetTickCount() - t0 < (DWORD)seconds * 1000)
    {
        NetEvent e;
        while (s.pollEvent(e))
        {
            if (e.kind == NetEvent::EV_CONNECTED) { printf("[bot] connected as player %d\n", (int)e.player.id); connectedAt = GetTickCount(); }
            else if (e.kind == NetEvent::EV_DISCONNECTED) { printf("[bot] disconnected: %s\n", e.text.c_str()); return 2; }
            else if (e.kind == NetEvent::EV_PLAYER_JOINED) printf("[bot] player joined: %s\n", e.player.name.c_str());
            if (e.kind == NetEvent::EV_MESSAGE && (e.msgType == MSG_ITEM_TAKE || e.msgType == MSG_ITEM_GIVE))
            {
                ByteReader r(e.body);
                uint8_t target = r.u8(), kind = r.u8(); uint32_t id = r.u32(); InvItem it; it.read(r);
                if (!r.ok() || target != 1) continue;
                printf("[bot] %s from player %d on %08x: %d x %s\n", e.msgType == MSG_ITEM_TAKE ? "LOOTED" : "RECEIVED",
                       (int)e.sender, id, it.quantity, it.item.c_str());
                // KMP_TEST_UNDO=1: takes from our backpack are refused (the taker must undo them).
                char undoEnv[8]; size_t undoLen = 0;
                if (e.msgType == MSG_ITEM_TAKE && kind == CONTAINER_BACKPACK &&
                    getenv_s(&undoLen, undoEnv, sizeof(undoEnv), "KMP_TEST_UNDO") == 0 && undoLen > 1)
                {
                    ByteWriter u; u.u8(e.sender); u.u8(UNDO_REMOVE); u.u8(kind); u.u32(id); it.write(u);
                    s.send(MSG_ITEM_UNDO, u.data);
                    printf("[bot] REFUSED the take of %d x %s (undo sent)\n", it.quantity, it.item.c_str());
                    continue;
                }
                std::vector<InvItem>& inv = kind == CONTAINER_BACKPACK ? myPack[id] : myInv[id];
                if (e.msgType == MSG_ITEM_TAKE)
                {
                    for (size_t i = 0; i < inv.size(); ++i)
                        if (inv[i].sameKind(it)) { inv[i].quantity -= it.quantity; if (inv[i].quantity <= 0) inv.erase(inv.begin() + i); break; }
                }
                else inv.push_back(it);
                ByteWriter w; w.u8(kind); w.u32(id); w.u16((uint16_t)inv.size());
                for (size_t i = 0; i < inv.size(); ++i) inv[i].write(w);
                s.send(MSG_INVENTORY, w.data);
                printf("[bot] our copy now carries %d stack(s)\n", (int)inv.size());
                continue;
            }
            if (e.kind == NetEvent::EV_MESSAGE && e.msgType == MSG_SHOT)
            {
                ByteReader r(e.body); uint32_t shooter = r.u32(); TargetRef t; t.read(r);
                if (r.ok()) printf("[bot] SHOT received from player %d: %08x at target kind %d id %08x\n", (int)e.sender, shooter, (int)t.kind, t.netId);
                continue;
            }
            if (e.kind == NetEvent::EV_MESSAGE && e.msgType == MSG_ITEM_UNDO)
            {
                ByteReader r(e.body);
                uint8_t target = r.u8(), action = r.u8(), kind = r.u8(); uint32_t id = r.u32(); InvItem it; it.read(r);
                if (r.ok() && target == 1)
                    printf("[bot] UNDO from player %d: %s %d x %s (%08x, kind %d)\n", (int)e.sender, action == UNDO_REMOVE ? "remove" : "give back", it.quantity, it.item.c_str(), id, (int)kind);
                continue;
            }
            if (e.kind == NetEvent::EV_MESSAGE && e.msgType == MSG_TRADE)
            {
                // Trade offers are accepted at once (the player's side opens its trade window).
                ByteReader r(e.body);
                uint8_t target = r.u8(), action = r.u8(); uint32_t theirs = r.u32(), ours = r.u32();
                if (!r.ok() || target != 1) continue;
                printf("[bot] trade %s from player %d (%08x <-> our %08x)\n", action == 0 ? "offer" : action == 1 ? "accept" : "end", (int)e.sender, theirs, ours);
                if (action == 0)
                {
                    ByteWriter w; w.u8(e.sender); w.u8(1); w.u32(ours); w.u32(theirs);
                    s.send(MSG_TRADE, w.data);
                }
                continue;
            }
            if (e.kind == NetEvent::EV_MESSAGE && e.msgType == MSG_GROUND_TAKE)
            {
                ByteReader r(e.body); uint8_t target = r.u8(); uint32_t id = r.u32();
                if (!r.ok() || target != 1 || !myGround.count(id)) continue;
                printf("[bot] ground item %08x picked up by player %d: removed here, everybody told\n", id, (int)e.sender);
                myGround.erase(id);
                ByteWriter w; w.u32(id); s.send(MSG_GROUND_REMOVE, w.data);
                continue;
            }
            if (e.kind != NetEvent::EV_MESSAGE || e.sender != HOST_ID) continue;

            // Weather and clock are recorded too, so a replay host serves them.
            if (record && (e.msgType == MSG_WEATHER || e.msgType == MSG_WORLD_SYNC))
            {
                DWORD ms = GetTickCount() - t0; uint32_t len = (uint32_t)e.body.size();
                fwrite(&ms, 4, 1, record); fwrite(&e.msgType, 1, 1, record); fwrite(&len, 4, 1, record);
                if (len) fwrite(&e.body[0], 1, len, record);
            }
            // World NPC traffic: count / record it, never mirror it back.
            uint32_t fid = firstId(e.msgType, e.body);
            if (fid && isNpcNetId(fid))
            {
                if (e.msgType == MSG_ENTITY_SPAWN && !npcSeen.count(fid))
                {
                    ByteReader rs(e.body); EntitySpawn sp; sp.read(rs);
                    npcSeen.insert(fid);
                    if (npcSeen.size() <= 12) printf("[bot] NPC %08x '%s' faction=%s\n", fid, sp.displayName.c_str(), sp.factionSid.c_str());
                }
                if (e.msgType == MSG_ENTITY_STATE) ++npcStates;
                if (e.msgType == MSG_INVENTORY)
                {
                    ++npcInventories;
                    // remember it (and its money, if it trades) for the money test
                    ByteReader ri(e.body); uint8_t kind = ri.u8(); uint32_t id = ri.u32(); uint16_t n = ri.u16();
                    for (int i = 0; i < n && ri.ok(); ++i) { InvItem it; it.read(ri); }
                    uint32_t money = 0;
                    if (ri.ok() && ri.remaining() && ri.u8() == 1) money = ri.u32();
                    if (ri.ok() && kind == CONTAINER_CHARACTER) npcMoney[id] = money;
                }
                if (e.msgType == MSG_ENTITY_DESPAWN) ++npcDespawns;
                if (record)
                {
                    DWORD ms = GetTickCount() - t0; uint32_t len = (uint32_t)e.body.size();
                    fwrite(&ms, 4, 1, record); fwrite(&e.msgType, 1, 1, record); fwrite(&len, 4, 1, record);
                    if (len) fwrite(&e.body[0], 1, len, record);
                }
                continue;
            }

            ByteReader r(e.body);
            switch (e.msgType)
            {
            case MSG_ENTITY_SPAWN:
            {
                EntitySpawn sp; sp.read(r);
                if (!r.ok()) break;
                if (!hostChars.count(sp.netId)) printf("[bot] host character %08x '%s' template=%s\n", sp.netId, sp.displayName.c_str(), sp.gameDataName.c_str());
                if (spoofTemplate.empty()) spoofTemplate = sp.gameDataName;
                hostChars.insert(sp.netId);
                ++spawns;
                sp.netId = mine(sp.netId); sp.displayName = "Bot " + sp.displayName; sp.x += offset;
                ByteWriter w; sp.write(w); sendOwn(s, MSG_ENTITY_SPAWN, w.data);
                break;
            }
            case MSG_ENTITY_STATE:
            {
                r.u32();                                  // host clock: we re-stamp with ours
                uint16_t n = r.u16();
                ByteWriter w; w.u32(GetTickCount()); w.u16(n);
                for (int i = 0; i < n && r.ok(); ++i)
                {
                    EntityState st; st.read(r);
                    uint32_t fightTarget = 0;
                    // KMP_TEST_TASK=a,b,task: from a to b s our copy stands still doing that task.
                    // KMP_TEST_FIGHT=a,b: from a to b s it fights the host's first character.
                    {
                        static int taskA = -2, taskB = 0, taskId = 0, fightA = -2, fightB = 0, fightC = 0, floorA = -2, floorB = 0;
                        if (floorA == -2) { char e3[32]; size_t l3 = 0; floorA = -1;
                            if (getenv_s(&l3, e3, sizeof(e3), "KMP_TEST_FLOOR") == 0 && l3 > 1) sscanf_s(e3, "%d,%d", &floorA, &floorB); }
                        if (taskA == -2) { char e1[32]; size_t l1 = 0; taskA = -1;
                            if (getenv_s(&l1, e1, sizeof(e1), "KMP_TEST_TASK") == 0 && l1 > 1) sscanf_s(e1, "%d,%d,%d", &taskA, &taskB, &taskId); }
                        if (fightA == -2) { char e2[32]; size_t l2 = 0; fightA = -1;
                            if (getenv_s(&l2, e2, sizeof(e2), "KMP_TEST_FIGHT") == 0 && l2 > 1) sscanf_s(e2, "%d,%d,%d", &fightA, &fightB, &fightC); }
                        int sec = connectedAt ? (int)((GetTickCount() - connectedAt) / 1000) : -1;
                        static float holdX = 0, holdY = 0, holdZ = 0; static bool holding = false;
                        if (i == 0 && taskA >= 0 && sec >= taskA && sec < taskB)
                        {
                            if (!holding) { holding = true; holdX = st.x + offset; holdY = st.y; holdZ = st.z; printf("[bot] task test: standing still, task %d\n", taskId); }
                            st.x = holdX - offset; st.y = holdY; st.z = holdZ; st.tx = st.x; st.ty = st.y; st.tz = st.z; st.vx = st.vy = st.vz = 0;
                            st.flags &= ~EntityState::RUNNING; st.task = (uint16_t)taskId; st.taskSubject = TargetRef();
                        }
                        if (i == 0 && floorA >= 0 && sec >= floorA && sec < floorB)
                        {
                            static bool saidF = false; if (!saidF) { saidF = true; printf("[bot] floor test: our copy says it is on floor 1 (no building here)\n"); }
                            st.floor = 1;
                        }
                        int fightEnd = fightC > fightB ? fightC : fightB;
                        if (i == 0 && fightA >= 0 && sec >= fightA && sec < fightEnd)
                        {
                            bool refusedPhase = fightC > fightB && sec < fightB;   // first phase: an order that is not an attack
                            static int said = -1; if (said != (int)refusedPhase) { said = (int)refusedPhase; printf("[bot] fight test: attacking %08x with task %d\n", st.netId, refusedPhase ? 3 : 262); }
                            st.flags |= EntityState::IN_COMBAT; st.task = refusedPhase ? 3 : 262;   // PICKUP (must be refused), then RANGED_ATTACK
                            if (!refusedPhase) { st.x += 60.f; st.tx += 60.f; st.vx = st.vy = st.vz = 0; }   // shooting distance
                            fightTarget = st.netId;   // the host's own character (set after swapRef below)
                        }
                    }
                    st.netId = mine(st.netId); { float o = offset; DWORD sinceC = connectedAt ? (GetTickCount() - connectedAt) / 1000 : 0;
                      if (farFrom >= 0 && connectedAt && (int)sinceC >= farFrom && (int)sinceC < farTo) o = 4000.f;
                      st.x += o; st.tx += o; }
                    if (i == 0) { copyX = st.x; copyY = st.y; copyZ = st.z; copyKnown = true; }
                    if (koMode && connectedAt && GetTickCount() - connectedAt > koAfter * 1000)
                    {
                        // A real knock-out: wounded body parts (the ghost copies them), not just a flag.
                        st.flags |= EntityState::UNCONSCIOUS; st.combatTarget = TargetRef(); st.task = 0xFFFF;
                        for (size_t f = 0; f < st.flesh.size(); ++f) if (st.flesh[f] > -8.f) st.flesh[f] = -8.f;
                    }
                    swapRef(st.combatTarget); swapRef(st.taskSubject);
                    if (fightTarget && i == 0)
                    {
                        st.combatTarget = TargetRef(); st.combatTarget.kind = TargetRef::NET_CHARACTER; st.combatTarget.netId = fightTarget;
                        static DWORD lastShot = 0;
                        if (GetTickCount() - lastShot > 2000)   // our copy "fires" every 2 s: its ghost on the host must repeat it
                        {
                            lastShot = GetTickCount();
                            ByteWriter sw; sw.u32(st.netId); st.combatTarget.write(sw); sw.u8(0); sw.f32(st.x - 60.f); sw.f32(st.y + 15.f); sw.f32(st.z);
                            s.send(MSG_SHOT, sw.data);
                            printf("[bot] shot sent (our %08x at %08x)\n", st.netId, fightTarget);
                        }
                        fightTarget = 0;
                    }
                    if (st.combatTarget.kind != TargetRef::NONE && !lastCombat)
                        printf("[bot] host character in combat, target kind %d id %08x -> ghost mirrors it\n", (int)st.combatTarget.kind, st.combatTarget.netId);
                    lastCombat = st.combatTarget.kind != TargetRef::NONE;
                    if (st.task != lastTask) { printf("[bot] host task -> %d\n", (int)st.task); lastTask = st.task; }
                    st.write(w);
                }
                if (r.ok())
                {
                    if (delayMs) { Delayed dl; dl.due = GetTickCount() + delayMs; dl.body = w.data; delayed.push_back(dl); }
                    else sendOwn(s, MSG_ENTITY_STATE, w.data);
                    ++states;
                }
                break;
            }
            case MSG_EQUIPMENT:
            {
                // Mirror, but our copy lost its left arm (limb sync test).
                uint32_t id = r.u32(); uint8_t n = r.u8();
                std::vector<ItemRef> items;
                for (int i = 0; i < n && r.ok(); ++i) { ItemRef it; it.read(r); items.push_back(it); }
                uint8_t st[4] = { 0, 0, 0, 0 }; ItemRef li[4];
                if (r.remaining() && r.u8() == 4) for (int i = 0; i < 4; ++i) { st[i] = r.u8(); li[i].read(r); }
                if (!r.ok()) break;
                hostGear[id] = items;
                st[0] = 1; li[0] = ItemRef();                    // LEFT_ARM = LIMB_STUMP
                {
                    // KMP_TEST_LIMB=<prosthetic id>: our copy wears it as its right arm (prosthetic test).
                    char limb[256]; size_t ln = 0;
                    if (getenv_s(&ln, limb, sizeof(limb), "KMP_TEST_LIMB") == 0 && ln > 1) { st[1] = 2; li[1] = ItemRef(); li[1].item = limb; }
                    // KMP_TEST_CROSSBOW=<id>: our copy carries that crossbow (ranged fight test).
                    char bow[256]; size_t bl = 0;
                    if (getenv_s(&bl, bow, sizeof(bow), "KMP_TEST_CROSSBOW") == 0 && bl > 1) { ItemRef cb; cb.item = bow; items.push_back(cb); ++n; }
                    // KMP_TEST_BACKPACK=<backpack id>: our copy wears it, filled with 3 of the host's first item.
                    char pack[256]; size_t pn = 0;
                    if (getenv_s(&pn, pack, sizeof(pack), "KMP_TEST_BACKPACK") == 0 && pn > 1)
                    {
                        ItemRef bp; bp.item = pack; items.push_back(bp); ++n;
                        std::vector<InvItem>& content = myPack[mine(id)];
                        if (content.empty() && !hostInv[id].empty()) { content.push_back(hostInv[id][0]); content[0].quantity = 3; }
                        if (content.empty() && !items.empty() && items[0].item != pack)
                        { InvItem x; x.item = items[0].item; x.manufacturer = items[0].manufacturer; x.material = items[0].material; x.quantity = 1; x.quality = 50.f; content.push_back(x); }
                        packDirty = true;
                    }
                }
                ByteWriter w; w.u32(mine(id)); w.u8(n);
                for (size_t i = 0; i < items.size(); ++i) items[i].write(w);
                w.u8(4); for (int i = 0; i < 4; ++i) { w.u8(st[i]); li[i].write(w); }
                sendOwn(s, MSG_EQUIPMENT, w.data);
                ++looks;
                printf("[bot] equipment of %08x: %d item(s), limbs %d/%d/%d/%d -> our copy has no left arm\n", id, (int)n, st[0], st[1], st[2], st[3]);
                break;
            }
            case MSG_INVENTORY:
            {
                uint8_t kind = r.u8(); uint32_t id = r.u32(); uint16_t n = r.u16();
                std::vector<InvItem> items;
                for (int i = 0; i < n && r.ok(); ++i) { InvItem it; it.read(r); items.push_back(it); }
                if (!r.ok()) break;
                if (kind == CONTAINER_CHARACTER) hostInv[id] = items;

                printf("[bot] inventory of %s %08x: %d item(s)%s%s\n", kind ? "building" : "character", id, (int)n,
                       n ? ", first: " : "", n ? items[0].item.c_str() : "");
                if (koMode && kind == CONTAINER_CHARACTER)
                {
                    if (!myInv.count(mine(id))) myInv[mine(id)] = items;
                    break;   // our own list is sent by the script below
                }
                ByteWriter w; w.u8(kind); w.u32(mine(id)); w.u16(n);
                for (size_t i = 0; i < items.size(); ++i) items[i].write(w);
                s.send(MSG_INVENTORY, w.data);
                break;
            }
            case MSG_GROUND_ITEM:
            {
                uint32_t id = r.u32(); InvItem it; it.read(r); float x = r.f32(), y = r.f32(), z = r.f32();
                int inside = -1;
                if (r.ok() && r.remaining() && r.u8() == 1) { inside = r.u16(); for (int i = 0; i < inside && r.ok(); ++i) { InvItem c; c.read(r); printf("[bot]    inside: %d x %s\n", c.quantity, c.item.c_str()); } }
                if (r.ok()) printf("[bot] host dropped %d x %s on the ground at (%.0f, %.0f, %.0f) as %08x%s\n", it.quantity, it.item.c_str(), x, y, z, id,
                                   inside >= 0 ? " (a bag with content)" : "");
                break;
            }
            case MSG_GROUND_REMOVE:
            {
                uint32_t id = r.u32();
                if (r.ok()) printf("[bot] host ground item %08x removed\n", id);
                break;
            }
            case MSG_CHAT:
            {
                std::string text = r.str();
                if (!r.ok()) break;
                printf("[bot] chat from host: %s\n", text.c_str());
                if (text.compare(0, 10, "Bot heard:") != 0) { ByteWriter w; w.str("Bot heard: " + text); s.send(MSG_CHAT, w.data); }
                break;
            }
            case MSG_WEATHER:
            {
                uint16_t n = r.u16();
                if (weatherSeen || !r.ok()) break;
                weatherSeen = true;
                printf("[bot] weather from host: %d region(s)\n", (int)n);
                for (int i = 0; i < n && i < 6; ++i)
                {
                    std::string key = r.str(); uint32_t season = r.u32(); r.u32(); std::string weather = r.str();
                    float strength = r.f32(); for (int k = 0; k < 7; ++k) r.f32(); r.u32(); r.u32();
                    if (!r.ok()) break;
                    printf("[bot]   region [%s] season %u weather '%s' strength %.2f\n", key.substr(0, 60).c_str(), season, weather.c_str(), strength);
                }
                break;
            }
            case MSG_APPEARANCE:
            case MSG_STATS:
            {
                uint32_t id = r.u32();
                ByteWriter w; w.u32(mine(id));
                Bytes rest(e.body.begin() + 4, e.body.end());
                w.bytes(rest);
                sendOwn(s, e.msgType, w.data);
                ++looks;
                if (e.msgType == MSG_STATS) printf("[bot] stats of %08x: %d bytes\n", id, (int)e.body.size());
                else printf("[bot] appearance of %08x: %d bytes\n", id, (int)e.body.size());
                break;
            }
            case MSG_BUILDING_STATE:
            {
                uint16_t n = r.u16();
                ByteWriter w; w.u16(n);
                for (int i = 0; i < n && r.ok(); ++i)
                {
                    BuildingMsg m; m.read(r);
                    m.netId = mine(m.netId); m.x += offset * 5;
                    m.write(w);
                }
                if (r.ok()) { s.send(MSG_BUILDING_STATE, w.data); builds += n; }
                if (connectedAt) printf("[bot] t=%lus building state: %d building(s)\n", (GetTickCount() - connectedAt) / 1000, (int)n);
                break;
            }
            case MSG_DAMAGE:
            {
                uint8_t target = r.u8();
                DamageMsg d; d.read(r);
                ++damages;
                printf("[bot] DAMAGE for player %d on %08x part %d cut=%.1f blunt=%.1f\n", (int)target, d.victimNetId, (int)d.bodyPart, d.cut, d.blunt);
                break;
            }
            case MSG_BUILDING_DAMAGE:
            {
                uint8_t target = r.u8();
                BuildingDamageMsg d; d.read(r);
                printf("[bot] BUILDING_DAMAGE for player %d on %08x door %d blunt=%.1f dismantle=%.2f\n",
                       (int)target, d.buildingNetId, (int)d.door, d.blunt, d.dismantle);
                break;
            }
            case MSG_WORLD_SYNC:
            {
                double h = r.f64();
                if (!worldSeen) { printf("[bot] world sync: host time %.3f h\n", h); worldSeen = true; }
                break;
            }
            case MSG_FACTION_RELATION:
            {
                uint8_t other = r.u8(); float rel = r.f32();
                printf("[bot] relation towards player %d set to %.0f\n", (int)other, rel);
                break;
            }
            }
        }

        DWORD now = GetTickCount();
        while (!delayed.empty() && now >= delayed.front().due)
        {
            // Re-stamp with the send time: the receiver sees a normal live stream.
            Bytes b = delayed.front().body;
            DWORD t = now; memcpy(&b[0], &t, 4);
            sendOwn(s, MSG_ENTITY_STATE, b);
            delayed.erase(delayed.begin());
        }
        DWORD since = connectedAt ? (now - connectedAt) / 1000 : 0;
        if (packDirty)
        {
            packDirty = false;
            for (std::map<uint32_t, std::vector<InvItem> >::iterator it = myPack.begin(); it != myPack.end(); ++it)
            {
                ByteWriter w; w.u8(CONTAINER_BACKPACK); w.u32(it->first); w.u16((uint16_t)it->second.size());
                for (size_t i = 0; i < it->second.size(); ++i) it->second[i].write(w);
                s.send(MSG_INVENTORY, w.data);
                printf("[bot] backpack of %08x: %d stack(s)%s%s\n", it->first, (int)it->second.size(),
                       it->second.empty() ? "" : ", first: ", it->second.empty() ? "" : it->second[0].item.c_str());
            }
        }
        static bool koItems = false;
        if (koMode && !koItems && connectedAt && since >= koAfter)
        {
            koItems = true;
            for (std::map<uint32_t, std::vector<ItemRef> >::iterator it = hostGear.begin(); it != hostGear.end(); ++it)
            {
                if (it->second.empty()) continue;
                InvItem loot; loot.item = it->second[0].item; loot.manufacturer = it->second[0].manufacturer;
                loot.material = it->second[0].material; loot.quantity = 1; loot.quality = 60.f;
                std::vector<InvItem>& inv = myInv[mine(it->first)];
                inv.push_back(loot);
                ByteWriter w; w.u8(CONTAINER_CHARACTER); w.u32(mine(it->first)); w.u16((uint16_t)inv.size());
                for (size_t i = 0; i < inv.size(); ++i) inv[i].write(w);
                s.send(MSG_INVENTORY, w.data);
                printf("[bot] ko: our copy %08x is unconscious and carries %s\n", mine(it->first), loot.item.c_str());
            }
        }
        // ko mode: re-announce our copies' inventory every 10 s with a changed quality (content hash
        // changes, items do not): the receiver must not pile up copies.
        static DWORD lastReannounce = 0;
        if (koMode && koItems && now - lastReannounce > 10000)
        {
            lastReannounce = now;
            for (std::map<uint32_t, std::vector<InvItem> >::iterator it = myInv.begin(); it != myInv.end(); ++it)
            {
                for (size_t i = 0; i < it->second.size(); ++i) it->second[i].quality += 0.5f;
                ByteWriter w; w.u8(CONTAINER_CHARACTER); w.u32(it->first); w.u16((uint16_t)it->second.size());
                for (size_t i = 0; i < it->second.size(); ++i) it->second[i].write(w);
                s.send(MSG_INVENTORY, w.data);
            }
        }
        static bool spoofed = false;
        char spoofEnv[8]; size_t spoofLen = 0;
        if (connectedAt && !spoofed && since >= 85 && getenv_s(&spoofLen, spoofEnv, sizeof(spoofEnv), "KMP_TEST_SPOOF") == 0 && spoofLen > 1 && copyKnown)
        {
            spoofed = true;
            EntitySpawn sp; sp.netId = makeNetId(1, 0x77); sp.kind = KIND_CHARACTER; sp.displayName = "Spoof";
            if (!hostChars.empty()) { /* any valid template: reuse the host's first character's */ }
            sp.gameDataName = spoofTemplate; sp.factionSid = "kenshimp_player_0"; sp.x = copyX + 3; sp.y = copyY; sp.z = copyZ;
            ByteWriter w; sp.write(w); s.send(MSG_ENTITY_SPAWN, w.data);
            printf("[bot] spoof test: spawned %08x claiming the host's faction\n", sp.netId);
        }
        static bool moneyAsked = false;
        char moneyEnv[8]; size_t moneyLen = 0;
        if (connectedAt && !moneyAsked && since >= 66 && getenv_s(&moneyLen, moneyEnv, sizeof(moneyEnv), "KMP_TEST_MONEY") == 0 && moneyLen > 1)
        {
            moneyAsked = true;
            if (npcMoney.empty()) printf("[bot] money test: no NPC with money seen\n");
            else
            {
                std::map<uint32_t, uint32_t>::iterator m = npcMoney.begin();
                for (std::map<uint32_t, uint32_t>::iterator i = npcMoney.begin(); i != npcMoney.end(); ++i) if (i->second > m->second) m = i;   // the richest
                InvItem pay; pay.item = "$money"; pay.quantity = 50;
                ByteWriter w; w.u8(HOST_ID); w.u8(CONTAINER_CHARACTER); w.u32(m->first); pay.write(w);
                s.send(MSG_ITEM_TAKE, w.data);
                printf("[bot] money test: asked 50 from NPC %08x (it has %u)\n", m->first, m->second);
            }
        }
        static bool gearGiven = false;
        char gearEnv[8]; size_t gearLen = 0;
        if (connectedAt && !gearGiven && since >= 62 && getenv_s(&gearLen, gearEnv, sizeof(gearEnv), "KMP_TEST_GEAR") == 0 && gearLen > 1)
        {
            gearGiven = true;
            for (std::map<uint32_t, std::vector<ItemRef> >::iterator it = hostGear.begin(); it != hostGear.end(); ++it)
                if (!it->second.empty())
                {
                    InvItem g; g.item = it->second[0].item; g.manufacturer = it->second[0].manufacturer; g.material = it->second[0].material;
                    g.quantity = 2; g.quality = 50.f;
                    ByteWriter w; w.u8(HOST_ID); w.u8(CONTAINER_CHARACTER); w.u32(it->first); g.write(w);
                    s.send(MSG_ITEM_GIVE, w.data);
                    printf("[bot] script: gave 2 x %s to %08x (gear test)\n", g.item.c_str(), it->first);
                    break;
                }
        }
        if (connectedAt && script == 0 && since >= 20 && !koMode)
        {
            script = 1;
            ByteWriter w; w.str("Hello from Bot! (chat test)"); s.send(MSG_CHAT, w.data);
            printf("[bot] script: chat sent\n");
        }
        if (script == 1 && since >= 30)
        {
            script = 2;
            for (std::map<uint32_t, std::vector<InvItem> >::iterator it = hostInv.begin(); it != hostInv.end() && !tookFrom; ++it)
                if (!it->second.empty()) { tookFrom = it->first; took = it->second[0]; took.quantity = 1; }
            if (tookFrom)
            {
                ByteWriter w; w.u8(HOST_ID); w.u8(CONTAINER_CHARACTER); w.u32(tookFrom); took.write(w);
                s.send(MSG_ITEM_TAKE, w.data);
                printf("[bot] script: took 1 x %s from %08x\n", took.item.c_str(), tookFrom);
            }
            else
            {
                // Empty backpack: give a copy of an equipped item now, take it back at 40 s.
                for (std::map<uint32_t, std::vector<ItemRef> >::iterator it = hostGear.begin(); it != hostGear.end() && !tookFrom; ++it)
                    if (!it->second.empty())
                    {
                        tookFrom = it->first;
                        took.item = it->second[0].item; took.manufacturer = it->second[0].manufacturer; took.material = it->second[0].material;
                        took.quantity = 1; took.quality = 50.f;
                    }
                if (tookFrom)
                {
                    ByteWriter w; w.u8(HOST_ID); w.u8(CONTAINER_CHARACTER); w.u32(tookFrom); took.write(w);
                    s.send(MSG_ITEM_GIVE, w.data);
                    printf("[bot] script: backpack empty, gave 1 x %s to %08x\n", took.item.c_str(), tookFrom);
                    script = 20;   // take it back next
                }
                else printf("[bot] script: nothing to exchange\n");
            }
        }
        if (script == 2 && since >= 40)
        {
            script = 3;
            if (tookFrom)
            {
                ByteWriter w; w.u8(HOST_ID); w.u8(CONTAINER_CHARACTER); w.u32(tookFrom); took.write(w);
                s.send(MSG_ITEM_GIVE, w.data);
                printf("[bot] script: gave %s back to %08x\n", took.item.c_str(), tookFrom);
            }
        }
        if (script == 20 && since >= 40)
        {
            script = 3;
            ByteWriter w; w.u8(HOST_ID); w.u8(CONTAINER_CHARACTER); w.u32(tookFrom); took.write(w);
            s.send(MSG_ITEM_TAKE, w.data);
            printf("[bot] script: took %s back from %08x\n", took.item.c_str(), tookFrom);
        }
        static bool dropped = false;
        if (!dropped && since >= 45 && copyKnown && !hostGear.empty() && !hostGear.begin()->second.empty())
        {
            dropped = true;
            InvItem it; const ItemRef& g = hostGear.begin()->second[0];
            it.item = g.item; it.manufacturer = g.manufacturer; it.material = g.material; it.quantity = 1; it.quality = 40.f;
            uint32_t id = makeNetId(1, 0x7000);
            myGround.insert(id);
            ByteWriter w; w.u32(id); it.write(w); w.f32(copyX); w.f32(copyY); w.f32(copyZ);
            s.send(MSG_GROUND_ITEM, w.data);
            printf("[bot] script: dropped %s on the ground as %08x\n", it.item.c_str(), id);
        }
        if (script == 3 && since >= 50)
        {
            script = 4;
            char nowar[8]; size_t nwl = 0;   // KMP_TEST_NOWAR=1: stay at peace (assault test)
            ByteWriter w; w.u8(HOST_ID); w.f32(getenv_s(&nwl, nowar, sizeof(nowar), "KMP_TEST_NOWAR") == 0 && nwl > 1 ? 0.f : -100.f); s.send(MSG_FACTION_RELATION, w.data);
            printf("[bot] script: war declared\n");
        }
        if (script == 4 && since >= 75)
        {
            script = 5;
            ByteWriter w; w.u8(HOST_ID); w.f32(0.f); s.send(MSG_FACTION_RELATION, w.data);
            printf("[bot] script: peace made\n");
        }
        if (!hitSent && now - t0 > 60000 && !hostChars.empty())
        {
            hitSent = true;
            for (std::set<uint32_t>::iterator it = hostChars.begin(); it != hostChars.end(); ++it)
            {
                DamageMsg d; d.victimNetId = *it; d.attackerNetId = makeNetId(1, 0); d.bodyPart = 2; d.blunt = 5.f;
                ByteWriter w; w.u8(HOST_ID); d.write(w);
                s.send(MSG_DAMAGE, w.data);
            }
            printf("[bot] sent a 5 blunt hit to %d host character(s)\n", (int)hostChars.size());
        }
        if (now - lastReport > 10000)
        {
            lastReport = now;
            printf("[bot] t=%lus spawns=%d states=%d looks=%d buildings=%d damages=%d ping=%dms | NPCs seen=%d states=%d despawns=%d inventories=%d\n",
                   (now - t0) / 1000, spawns, states, looks, builds, damages, s.pingMs(), (int)npcSeen.size(), npcStates, npcDespawns, npcInventories);
            if (record) fflush(record);
            fflush(stdout);
        }
        Sleep(10);
    }
    s.stop();
    if (g_pathRec) fclose(g_pathRec);
    return 0;
}
