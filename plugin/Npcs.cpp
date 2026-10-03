// World NPC synchronisation: the host's world is the shared world.
//
// Host   : for every client, the NPCs (humans and animals of any non-player faction) within
//          INTEREST_RADIUS of that client's characters are replicated to it, with the same
//          complete ghost pipeline as player squads (spawn in their real faction, appearance,
//          gear, stats, 5 Hz state with health/combat/tasks). They are dropped for that client
//          once all its characters are further than DROP_RADIUS.
//          Hits from clients on those NPCs arrive as MSG_DAMAGE and are applied here.
// Client : the local world population is switched off (no squad spawns, no town residents,
//          existing NPCs near our characters are removed); only the host's NPCs are shown.
#include <Debug.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
#include <kenshi/Faction.h>
#include <kenshi/GameData.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/ZoneManager.h>
#include <core/Functions.h>

#include "Shared.h"
#include "../core/LoadSharing.h"
#include "CharSync.h"

#include <map>
#include <set>
#include <vector>
#include <string.h>
#include <math.h>

using namespace mp;

namespace kmp {

namespace
{
    const float INTEREST_RADIUS = 600.f;
    const float DROP_RADIUS = 750.f;
    const float QUERY_MERGE = 40.f;          // skip area queries whose centre is this close to one done
    const DWORD STATE_PERIOD_MS = 50;       // scheduler tick; each NPC has its own LOD rate below

    // Level of detail, like a game server's entity tracker: close NPCs update often.
    DWORD lodPeriod(float dist) { return dist < 80.f ? 100 : (dist < 250.f ? 200 : 500); }
    const DWORD SCAN_PERIOD_MS = 1000;
    const DWORD LOOKS_PERIOD_MS = 3000;
    const DWORD CLEANUP_PERIOD_MS = 1000;
    const float CLEANUP_RADIUS = 400.f;
    const float CLAIM_RANGE = 60.f;

    struct Npc
    {
        hand h;
        Character* ptr;
        uint32_t id;
        std::set<uint8_t> clients;           // who currently has a ghost of it
        uint32_t appearanceHash, equipmentHash, statsHash, inventoryHash, backpackHash;
        EntityState lastSent;                // for delta suppression
        DWORD lastSentAt;
        bool everSent;
        float nearestPlayer;                 // distance to the closest remote player (LOD)
        Npc() : ptr(NULL), id(0), appearanceHash(0), equipmentHash(0), statsHash(0), inventoryHash(0), backpackHash(0), lastSentAt(0), everSent(false), nearestPlayer(0) {}
    };

    const DWORD STATE_REFRESH_MS = 2000;     // unchanged NPCs are still refreshed this often

    bool nearlySame(const EntityState& a, const EntityState& b)
    {
        if (a.flags != b.flags || a.moveSpeed != b.moveSpeed || a.task != b.task) return false;
        if (a.combatTarget != b.combatTarget || a.taskSubject != b.taskSubject) return false;
        float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        if (dx * dx + dy * dy + dz * dz > 0.05f * 0.05f) return false;
        if (fabsf(a.qw - b.qw) > 0.01f || fabsf(a.qy - b.qy) > 0.01f) return false;
        if (fabsf(a.blood - b.blood) > 1.f || a.flesh.size() != b.flesh.size()) return false;
        for (size_t i = 0; i < a.flesh.size(); ++i) if (fabsf(a.flesh[i] - b.flesh[i]) > 0.5f) return false;
        return true;
    }
    std::map<Character*, Npc> g_npcs;        // host only (g_lock for lookups from hooks)
    uint32_t g_nextNpc = 1;
    DWORD g_lastState = 0, g_lastScan = 0, g_lastLooks = 0, g_lastCleanup = 0;
    bool g_purgedAll = false;

    // Load sharing. The host simulates the world only around itself and the clients near it;
    // a client far away simulates its own surroundings (its own NPCs, its own CPU). The host
    // decides, with a wide gap between the two distances so it does not flip back and forth.
    enum { MODE_UNKNOWN = SHARE_UNKNOWN, MODE_APART = SHARE_APART, MODE_SHARED = SHARE_SHARED };   // core/LoadSharing.h
    std::map<uint8_t, int> g_clientMode;     // host: per client
    int g_zoneMode = MODE_UNKNOWN;           // client: as told by the host

    bool enabled() { return g_cfg.npcSync; }
    // Client: the local population stays off unless the host said we are on our own.
    bool suppressLocalWorld() { return enabled() && ready() && !g_session.isHost() && g_zoneMode != MODE_APART; }

    bool isPlayerMirrorFaction(Faction* f)
    {
        GameData* d = f ? f->getData() : NULL;
        return d && strncmp(d->stringID.c_str(), "kenshimp_player_", 16) == 0;
    }

    // A world NPC = any living-world character that is not a player's (ours or a mirror).
    bool isWorldNpc(Character* c)
    {
        if (!c) return false;
        Faction* f = c->getFaction();
        if (!f || f->isThePlayer() || isPlayerMirrorFaction(f)) return false;
        return !chars_isGhost(c);
    }

    int g_diagQueries = 0, g_diagFound = 0, g_diagFactions = 0;

    void collectAround(const std::vector<Ogre::Vector3>& centres, float radius, std::set<Character*>& out)
    {
        std::vector<Character*> all;
        cs_allActiveCharacters(all);
        g_diagFactions = (int)all.size();
        for (size_t n = 0; n < all.size(); ++n)
        {
            Character* c = all[n];
            if (!isWorldNpc(c)) continue;
            Ogre::Vector3 p = c->getPosition();
            for (size_t i = 0; i < centres.size(); ++i)
            {
                ++g_diagQueries;
                if (centres[i].distance(p) <= radius) { out.insert(c); ++g_diagFound; break; }
            }
        }
    }

    float nearest(const Ogre::Vector3& p, const std::vector<Ogre::Vector3>& centres)
    {
        float best = 1e30f;
        for (size_t i = 0; i < centres.size(); ++i) { float d = centres[i].distance(p); if (d < best) best = d; }
        return best;
    }

    void sendSpawnTo(uint8_t client, Npc& n, Character* c)
    {
        EntitySpawn sp;
        sp.netId = n.id;
        sp.kind = KIND_CHARACTER;
        sp.gameDataName = c->data ? c->data->stringID : std::string();
        sp.displayName = c->getName();
        Ogre::Vector3 p = c->getPosition();
        sp.x = p.x; sp.y = p.y; sp.z = p.z;
        Faction* f = c->getFaction();
        sp.factionSid = f && f->getData() ? f->getData()->stringID : std::string();
        ByteWriter w; sp.write(w);
        g_session.sendTo(client, MSG_ENTITY_SPAWN, w.data);
        Bytes a = cs_appearanceMsg(c, n.id), e = cs_equipmentMsg(c, n.id);
        Bytes inv = items_inventoryMsg(CONTAINER_CHARACTER, n.id, c->inventory, items_moneyOf(c));
        Bytes bp = items_backpackMsg(n.id, c);
        Bytes st = cs_statsMsg(c, n.id);
        g_session.sendTo(client, MSG_APPEARANCE, a);
        g_session.sendTo(client, MSG_EQUIPMENT, e);
        g_session.sendTo(client, MSG_INVENTORY, inv);
        if (!bp.empty()) g_session.sendTo(client, MSG_INVENTORY, bp);
        if (!st.empty()) g_session.sendTo(client, MSG_STATS, st);
        // First spawn of this NPC: its looks check compares against what was just sent (a change
        // in the next seconds is then sent, instead of being taken as the starting point).
        if (!n.appearanceHash)
        {
            n.appearanceHash = hashBytes(a); n.equipmentHash = hashBytes(e); n.statsHash = hashBytes(st);
            n.inventoryHash = hashBytes(inv); n.backpackHash = hashBytes(bp);
        }
    }

    void sendDespawnTo(uint8_t client, uint32_t id)
    {
        ByteWriter w; w.u32(id);
        g_session.sendTo(client, MSG_ENTITY_DESPAWN, w.data);
    }

    // Host: the engine only loads/simulates zones around the host's own characters. Keep the
    // zones around every remote player active too, otherwise a client far from the host would
    // walk through an empty, frozen world.
    const DWORD ZONE_KEEPALIVE_MS = 3000;
    DWORD g_lastZoneKeep = 0;
    int g_zoneFailures = 0;

    bool safeActivateZone(const Ogre::Vector3* p)
    {
        __try
        {
            if (!ou->zoneMgr) return false;
            ZoneMap* z = ou->zoneMgr->getZoneMap(*p);
            if (!z) return false;
            return ou->zoneMgr->activateZoneMap(z->coordinates, 1, ACTIVATION_PLAYER_CHARACTER, true);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ++g_zoneFailures; return false; }
    }

    void keepRemoteZonesLoaded()
    {
        if (g_zoneFailures > 5) return;   // engine refuses: stop trying, log once
        std::vector<PlayerInfo> players = g_session.players();
        for (size_t p = 0; p < players.size(); ++p)
        {
            if (players[p].id == HOST_ID) continue;
            std::map<uint8_t, int>::iterator m = g_clientMode.find(players[p].id);
            if (m != g_clientMode.end() && m->second == MODE_APART) continue;   // it simulates its own surroundings
            std::vector<Ogre::Vector3> centres;
            cs_ghostPositions(players[p].id, centres);
            for (size_t i = 0; i < centres.size(); ++i) safeActivateZone(&centres[i]);
        }
        if (g_zoneFailures > 5) log("zone keep-alive disabled (engine exceptions)");
    }

    // Host: shared world (true) or the client's own (false); tells the client when it changes.
    bool updateClientMode(uint8_t client, const std::vector<Ogre::Vector3>& centres)
    {
        int mode = MODE_SHARED;
        std::map<uint8_t, int>::iterator prev = g_clientMode.find(client);
        if (g_cfg.loadSharing)
        {
            std::vector<std::pair<uint32_t, Character*> > mine;
            chars_localCharacters(mine);
            float d = 1e30f;
            for (size_t i = 0; i < mine.size(); ++i)
                if (mine[i].second) { float n = nearest(mine[i].second->getPosition(), centres); if (n < d) d = n; }
            mode = shareMode(prev == g_clientMode.end() ? MODE_UNKNOWN : prev->second, d);
            if (prev == g_clientMode.end() || prev->second != mode)
                log("load sharing: player %d is %s (%.0f units from us)", (int)client, mode == MODE_SHARED ? "in our world" : "on its own", d);
        }
        if (prev == g_clientMode.end() || prev->second != mode)
        {
            g_clientMode[client] = mode;
            ByteWriter w; w.u8(client); w.u8((uint8_t)mode);
            g_session.sendTo(client, MSG_ZONE_MODE, w.data);
        }
        return mode == MODE_SHARED;
    }

    // Host: decide, per client, which NPCs it must see.
    void hostScan()
    {
        std::vector<PlayerInfo> players = g_session.players();

        // Drop NPCs that no longer exist, or stopped being world NPCs (recruited by the host:
        // they now travel as the host's own squad members).
        for (std::map<Character*, Npc>::iterator it = g_npcs.begin(); it != g_npcs.end();)
        {
            if (it->second.h.getCharacter() == it->first && isWorldNpc(it->first)) { ++it; continue; }
            for (std::set<uint8_t>::iterator c = it->second.clients.begin(); c != it->second.clients.end(); ++c)
                sendDespawnTo(*c, it->second.id);
            Lock l;
            g_npcs.erase(it++);
        }

        for (std::map<Character*, Npc>::iterator it = g_npcs.begin(); it != g_npcs.end(); ++it) it->second.nearestPlayer = 1e30f;

        for (size_t p = 0; p < players.size(); ++p)
        {
            uint8_t client = players[p].id;
            if (client == HOST_ID) continue;
            std::vector<Ogre::Vector3> centres;
            cs_ghostPositions(client, centres);
            if (centres.empty()) continue;

            if (!updateClientMode(client, centres))
            {
                // On its own: it simulates its surroundings, we neither send nor simulate them.
                for (std::map<Character*, Npc>::iterator it = g_npcs.begin(); it != g_npcs.end(); ++it)
                    if (it->second.clients.erase(client)) sendDespawnTo(client, it->second.id);
                continue;
            }

            std::set<Character*> wanted;
            g_diagQueries = g_diagFound = 0;
            collectAround(centres, INTEREST_RADIUS, wanted);
            static DWORD lastDiag = 0;
            if (GetTickCount() - lastDiag > 10000)
            {
                lastDiag = GetTickCount();
                log("npc scan for player %d: %d centre(s), %d active char(s), %d test(s), %d in range, %d wanted, %d tracked",
                    (int)client, (int)centres.size(), g_diagFactions, g_diagQueries, g_diagFound, (int)wanted.size(), (int)g_npcs.size());
                // Where things are: centre, and the nearest few world NPCs.
                std::vector<Character*> all; cs_allActiveCharacters(all);
                float best = 1e30f; std::string bestName; int npcs = 0;
                for (size_t n = 0; n < all.size(); ++n)
                {
                    if (!isWorldNpc(all[n])) continue;
                    ++npcs;
                    float d = all[n]->getPosition().distance(centres[0]);
                    if (d < best) { best = d; bestName = all[n]->getName(); }
                }
                log("   centre (%.0f, %.0f, %.0f), %d world NPC(s), nearest '%s' at %.0f m",
                    centres[0].x, centres[0].y, centres[0].z, npcs, bestName.c_str(), best);
            }

            for (std::set<Character*>::iterator w = wanted.begin(); w != wanted.end(); ++w)
            {
                std::map<Character*, Npc>::iterator it = g_npcs.find(*w);
                if (it == g_npcs.end())
                {
                    Npc n; n.h = hand(*w); n.ptr = *w; n.id = makeNetId(HOST_ID, NPC_ID_FLAG | (g_nextNpc++ & 0x7FFFFF));
                    Lock l;
                    it = g_npcs.insert(std::make_pair(*w, n)).first;
                }
                float d = nearest((*w)->getPosition(), centres);
                if (d < it->second.nearestPlayer) it->second.nearestPlayer = d;
                if (!it->second.clients.count(client))
                {
                    sendSpawnTo(client, it->second, *w);
                    it->second.clients.insert(client);
                }
            }

            // Hysteresis: forget an NPC for this client only once it is clearly out of range.
            for (std::map<Character*, Npc>::iterator it = g_npcs.begin(); it != g_npcs.end(); ++it)
            {
                if (!it->second.clients.count(client) || wanted.count(it->first)) continue;
                if (nearest(it->first->getPosition(), centres) > DROP_RADIUS)
                {
                    sendDespawnTo(client, it->second.id);
                    it->second.clients.erase(client);
                }
            }
        }
    }

    void hostStates()
    {
        std::map<uint8_t, ByteWriter> bodies;
        std::map<uint8_t, uint16_t> counts;
        for (std::map<Character*, Npc>::iterator it = g_npcs.begin(); it != g_npcs.end(); ++it)
        {
            if (it->second.clients.empty()) continue;
            Character* c = it->second.h.getCharacter();
            if (c != it->first) continue;
            EntityState s;
            cs_captureState(c, it->second.id, s);
            DWORD now = GetTickCount();
            Npc& n = it->second;
            if (n.everSent && now - n.lastSentAt < lodPeriod(n.nearestPlayer)) continue;
            if (n.everSent && now - n.lastSentAt < STATE_REFRESH_MS && nearlySame(s, n.lastSent)) continue;
            n.lastSent = s; n.lastSentAt = now; n.everSent = true;
            ByteWriter one; s.write(one);
            for (std::set<uint8_t>::iterator cl = it->second.clients.begin(); cl != it->second.clients.end(); ++cl)
            {
                bodies[*cl].bytes(one.data);
                ++counts[*cl];
                if (counts[*cl] >= 150)   // keep packets reasonable
                {
                    ByteWriter w; w.u32(GetTickCount()); w.u16(counts[*cl]); w.bytes(bodies[*cl].data);
                    g_session.sendTo(*cl, MSG_ENTITY_STATE, w.data);
                    bodies[*cl] = ByteWriter(); counts[*cl] = 0;
                }
            }
        }
        for (std::map<uint8_t, uint16_t>::iterator it = counts.begin(); it != counts.end(); ++it)
        {
            if (!it->second) continue;
            ByteWriter w; w.u32(GetTickCount()); w.u16(it->second); w.bytes(bodies[it->first].data);
            g_session.sendTo(it->first, MSG_ENTITY_STATE, w.data);
        }
    }

    // Looks of every NPC are checked once per LOOKS_PERIOD_MS, a slice per call (LOOKS_SLICES
    // calls per period): building them all at once made a small hitch every 3 s on a busy host.
    const int LOOKS_SLICES = 10;
    Character* g_looksCursor = NULL;   // next NPC to check (map order)
    void hostLooks()
    {
        size_t budget = (g_npcs.size() + LOOKS_SLICES - 1) / LOOKS_SLICES;
        std::map<Character*, Npc>::iterator it = g_npcs.lower_bound(g_looksCursor);
        for (size_t done = 0; done < budget && !g_npcs.empty(); ++done, ++it)
        {
            if (it == g_npcs.end()) it = g_npcs.begin();
            g_looksCursor = it->first;
            Npc& n = it->second;
            Character* c = n.h.getCharacter();
            if (c != it->first || n.clients.empty()) continue;
            Bytes a = cs_appearanceMsg(c, n.id), e = cs_equipmentMsg(c, n.id), s = cs_statsMsg(c, n.id);
            Bytes inv = items_inventoryMsg(CONTAINER_CHARACTER, n.id, c->inventory, items_moneyOf(c));
            Bytes bp = items_backpackMsg(n.id, c);
            uint32_t ha = hashBytes(a), he = hashBytes(e), hs = hashBytes(s), hi = hashBytes(inv), hb = hashBytes(bp);
            bool first = !n.appearanceHash;
            for (std::set<uint8_t>::iterator cl = n.clients.begin(); cl != n.clients.end(); ++cl)
            {
                if (!first && ha != n.appearanceHash) g_session.sendTo(*cl, MSG_APPEARANCE, a);
                if (!first && he != n.equipmentHash) g_session.sendTo(*cl, MSG_EQUIPMENT, e);
                if (!first && hs != n.statsHash && !s.empty()) g_session.sendTo(*cl, MSG_STATS, s);
                if (!first && hi != n.inventoryHash) g_session.sendTo(*cl, MSG_INVENTORY, inv);
                if (!first && hb != n.backpackHash && !bp.empty()) g_session.sendTo(*cl, MSG_INVENTORY, bp);
            }
            n.appearanceHash = ha; n.equipmentHash = he; n.statsHash = hs; n.inventoryHash = hi; n.backpackHash = hb;
        }
        g_looksCursor = it == g_npcs.end() ? NULL : it->first;
    }

    // Client: remove the local population around our characters (or everywhere, once).
    void clientCleanup(bool everywhere)
    {
        std::vector<Ogre::Vector3> centres;
        if (everywhere) centres.push_back(Ogre::Vector3::ZERO);
        else cs_localPositions(centres);
        if (centres.empty()) return;
        std::set<Character*> locals;
        collectAround(centres, everywhere ? 1.0e9f : CLEANUP_RADIUS, locals);
        int n = 0;
        for (std::set<Character*>::iterator it = locals.begin(); it != locals.end(); ++it)
        {
            // Keep what we took over (recruited/carried/looted NPCs) and our captives.
            Character* lc = *it;
            if (chars_isClaimed(lc) || lc->isBeingCarried() || lc->isSlave() == IS_SLAVE) continue;
            if (everywhere && n < 5) log("world sync: removing local '%s'", (*it)->getName().c_str());
            if (cs_destroy(*it)) ++n;
        }
        // Aggregate: towns keep trying to repopulate, one line per second would flood the log.
        static int pending = 0;
        static DWORD lastLog = 0;
        pending += n;
        if (everywhere && n) { log("world sync: removed %d local NPC(s) (initial purge)", n); pending = 0; }
        else if (pending && GetTickCount() - lastLog > 30000)
        {
            lastLog = GetTickCount();
            log("world sync: removed %d local NPC(s) in the last 30 s", pending);
            pending = 0;
        }
    }
}

uint32_t npcs_netIdOf(Character* c)
{
    Lock l;
    std::map<Character*, Npc>::iterator it = g_npcs.find(c);
    return it == g_npcs.end() ? 0 : it->second.id;
}

Character* npcs_byNetId(uint32_t id)
{
    Lock l;
    for (std::map<Character*, Npc>::iterator it = g_npcs.begin(); it != g_npcs.end(); ++it)
        if (it->second.id == id) return it->second.h.getCharacter();
    return NULL;
}

void npcs_tick(DWORD now)
{
    if (!enabled()) return;
    if (g_session.isHost())
    {
        if (now - g_lastScan >= SCAN_PERIOD_MS) { g_lastScan = now; hostScan(); }
        if (now - g_lastState >= STATE_PERIOD_MS) { g_lastState = now; hostStates(); }
        if (now - g_lastLooks >= LOOKS_PERIOD_MS / LOOKS_SLICES) { g_lastLooks = now; hostLooks(); }
        if (now - g_lastZoneKeep >= ZONE_KEEPALIVE_MS) { g_lastZoneKeep = now; keepRemoteZonesLoaded(); }
        return;
    }
    if (g_zoneMode != MODE_SHARED) return;   // our own world (far from the host), or not told yet
    if (!g_purgedAll) { g_purgedAll = true; clientCleanup(true); }
    if (now - g_lastCleanup >= CLEANUP_PERIOD_MS) { g_lastCleanup = now; clientCleanup(false); }
}

// A client recruited one of our NPCs: it now lives on the client; remove ours everywhere.
void npcs_onClaim(uint8_t client, const Bytes& body)
{
    if (!g_session.isHost()) return;
    ByteReader r(body);
    uint32_t id = r.u32();
    if (!r.ok() || !isNpcNetId(id)) return;
    Character* c = NULL;
    Npc n;
    {
        Lock l;
        for (std::map<Character*, Npc>::iterator it = g_npcs.begin(); it != g_npcs.end(); ++it)
            if (it->second.id == id) { c = it->first; n = it->second; g_npcs.erase(it); break; }
    }
    if (!c) return;
    // The claimer must actually be next to that NPC (recruiting happens face to face).
    {
        std::vector<Ogre::Vector3> centres;
        cs_ghostPositions(client, centres);
        if (n.h.getCharacter() != c || nearest(c->getPosition(), centres) > CLAIM_RANGE)
        {
            Lock l;
            g_npcs[c] = n;                   // not handed over: keep replicating it
            log("refused NPC claim %08x from %s (not next to it)", id, playerName(client).c_str());
            return;
        }
    }
    for (std::set<uint8_t>::iterator cl = n.clients.begin(); cl != n.clients.end(); ++cl)
        if (*cl != client) sendDespawnTo(*cl, id);
    if (n.h.getCharacter() == c) cs_destroy(c);
    log("NPC %08x handed over to %s", id, playerName(client).c_str());
    showMessage(TF("%s recruited an NPC.", playerName(client).c_str()));
}

// World reloaded or session ended: every NPC pointer is stale, and the next client session must
// remove the local population again.
void npcs_reset()
{
    Lock l;
    g_npcs.clear();
    g_looksCursor = NULL;
    g_purgedAll = false;
    g_clientMode.clear();
    g_zoneMode = MODE_UNKNOWN;
}

bool npcs_ownWorld() { return g_session.active() && !g_session.isHost() && g_zoneMode == MODE_APART; }

bool npcs_clientOwnWorld(uint8_t client)
{
    std::map<uint8_t, int>::iterator m = g_clientMode.find(client);
    return m != g_clientMode.end() && m->second == MODE_APART;
}

void npcs_onZoneMode(const NetEvent& e)
{
    ByteReader r(e.body);
    uint8_t target = r.u8(), mode = r.u8();
    if (!r.ok() || e.sender != HOST_ID || target != g_session.localId() || mode > MODE_SHARED) return;
    if (mode == g_zoneMode) return;
    bool entering = mode == MODE_SHARED;
    g_zoneMode = mode;
    if (entering) g_purgedAll = false;   // the host's people replace ours around us (next tick)
    log("load sharing: %s", entering ? "the host simulates the world around us" : "we simulate our own surroundings (far from the host)");
    if (g_cfg.debugKeys) chat_notice(entering ? "World shared with the host" : "Own world (far from the host)");
}

void npcs_onPlayerLeft(uint8_t id)
{
    g_clientMode.erase(id);
    for (std::map<Character*, Npc>::iterator it = g_npcs.begin(); it != g_npcs.end(); ++it)
        it->second.clients.erase(id);
}

} // namespace kmp

using namespace kmp;

// ============================================================================ client-side hooks
// While connected as a client with npc_sync on, the local world does not create its own people.

// Faction::_spawnASquad (protected): random squads roaming the world.
namespace
{
    typedef bool (Faction::*SpawnSquadFn)(const std::string&, float);
    struct FactionAccess : Faction { static SpawnSquadFn address() { return &FactionAccess::_spawnASquad; } };
}
bool (*spawnSquad_orig)(Faction*, const std::string&, float) = NULL;
bool spawnSquad_hook(Faction* self, const std::string& list, float mult)
{
    if (suppressLocalWorld()) return false;
    return spawnSquad_orig(self, list, mult);
}

// Town residents / shopkeepers / guards populated into buildings.
void (*charForBuilding_orig)(RootObjectFactory*, Building*) = NULL;
void charForBuilding_hook(RootObjectFactory* self, Building* b)
{
    if (suppressLocalWorld()) return;
    charForBuilding_orig(self, b);
}
void (*populate_orig)(RootObjectFactory*, Building*) = NULL;
void populate_hook(RootObjectFactory* self, Building* b)
{
    if (suppressLocalWorld()) return;
    populate_orig(self, b);
}

namespace kmp {
bool npcs_install()
{
    bool ok = true;
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(FactionAccess::address()), spawnSquad_hook, &spawnSquad_orig))
    { ErrorLog("KenshiMP: could not hook Faction::_spawnASquad!"); ok = false; }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&RootObjectFactory::createCharacterForBuilding), charForBuilding_hook, &charForBuilding_orig))
    { ErrorLog("KenshiMP: could not hook createCharacterForBuilding!"); ok = false; }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&RootObjectFactory::populateBuilding), populate_hook, &populate_orig))
    { ErrorLog("KenshiMP: could not hook populateBuilding!"); ok = false; }
    return ok;
}
}
