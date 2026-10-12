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
#include <kenshi/Platoon.h>
#include <kenshi/Inventory.h>
#include <kenshi/Building/Building.h>
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

    // Level of detail, like a game server's entity tracker: close NPCs update often. Everything stays under
    // 100 ms (the scheduler runs every STATE_PERIOD_MS = 50 ms, so a period of 99 means "every second tick").
    DWORD lodPeriod(float dist) { return dist < 250.f ? 50 : 99; }
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

    // Test runs: what makes a shopkeeper a shopkeeper (reviews: "nothing to buy at the trader", "I sell and
    // get no money"). Logged once per trader the host replicates.
    bool safeTraderProbe(Character* c, int* isTrader, int* platoonTrader, float* priceMult, char* squad, int squadLen, int* looseItems, int* sections)
    {
        __try
        {
            *isTrader = c->isATrader() ? 1 : 0;
            ActivePlatoon* ap = c->getPlatoon();
            *platoonTrader = ap ? (ap->getIsTrader() ? 1 : 0) : -1;
            *priceMult = (ap && ap->me) ? ap->me->priceMultWhenITrade : -1.f;
            squad[0] = 0;
            if (ap && ap->me && ap->me->data) strncpy_s(squad, squadLen, ap->me->data->stringID.c_str(), _TRUNCATE);
            if (ap && ap->me && ap->me->squadTemplate) { size_t l = strlen(squad); _snprintf_s(squad + l, squadLen - l, _TRUNCATE, " / template '%s'", ap->me->squadTemplate->stringID.c_str()); }
            *looseItems = c->inventory ? (int)c->inventory->getAllItems().size() : -1;
            *sections = c->inventory ? (int)c->inventory->getNumItems() : -1;   // all items, whatever the section
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeBuildingsAround(lektor<RootObject*>* out, const Ogre::Vector3* p, float r)
    {
        __try { ou->getObjectsWithinSphere(*out, *p, r, BUILDING, 200, NULL); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeBuildingProbe(Building* b, int* items, int* func, char* sid, int sidLen)
    {
        __try
        {
            Inventory* inv = b->getInventory();
            *items = inv ? (int)inv->getAllItems().size() : 0;
            *func = (int)b->getSpecialFunction();
            sid[0] = 0;
            if (b->data) strncpy_s(sid, sidLen, b->data->stringID.c_str(), _TRUNCATE);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // "name:items" of each inventory section of c.
    bool safeSectionNames(Character* c, char* out, int len, int* count)
    {
        __try
        {
            out[0] = 0; *count = 0;
            if (!c->inventory) return false;
            lektor<InventorySection*>& all = c->inventory->getAllSections();
            *count = (int)all.size();
            for (uint32_t i = 0; i < all.size() && i < 24; ++i)
            {
                InventorySection* s = all[i];
                if (!s) continue;
                char one[80]; sprintf_s(one, "%s%s:%d", i ? ", " : "", s->name.c_str(), (int)s->getItems().size());
                strcat_s(out, len, one);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void logStockBuildings(Character* c, const char* when)
    {
        Ogre::Vector3 at = c->getPosition();
        lektor<RootObject*> around;
        if (!safeBuildingsAround(&around, &at, 80.f)) return;
        int shown = 0;
        for (uint32_t i = 0; i < around.size() && shown < 10; ++i)
        {
            Building* b = static_cast<Building*>(around[i]);
            int n = 0, func = -1; char sid[120];
            if (!safeBuildingProbe(b, &n, &func, sid, sizeof(sid)) || n <= 0) continue;
            ++shown;
            log("   [%s] building '%s' holds %d item(s), special function %d, %.0f units away", when, sid, n, func, b->getPosition().distance(at));
        }
        if (!shown) log("   [%s] no building holding items within 80 units (%d building(s) checked)", when, (int)around.size());
    }
    bool g_probedRestock = false;
    bool safeRestock(Character* c, int* itemsAfter, int* vendorList, int* specialList)
    {
        __try
        {
            ActivePlatoon* ap = c->getPlatoon();
            if (!ap) return false;
            *vendorList = ap->getHasVendorList() ? 1 : 0;
            *specialList = ap->getHasSpecialItemsList() ? 1 : 0;
            ap->refreshInventory(true);
            *itemsAfter = c->inventory ? (int)c->inventory->getAllItems().size() : -1;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void logTrader(Character* c, uint32_t id)
    {
        if (!g_cfg.autotest && !g_cfg.debugKeys) return;
        int isTrader = 0, platTrader = 0, items = 0, sections = 0; float mult = 0; char squad[160];
        if (!safeTraderProbe(c, &isTrader, &platTrader, &mult, squad, sizeof(squad), &items, &sections)) return;
        if (!isTrader && platTrader != 1) return;
        static int logged = 0;
        if (logged++ >= 12) return;
        log("trader NPC '%s' (%08x): isATrader=%d, platoon isTrader=%d, platoon template '%s', price multiplier %.2f, %d loose item(s), %d item(s) in all sections, money %d",
            c->getName().c_str(), id, isTrader, platTrader, squad, mult, items, sections, items_moneyOf(c));
        {
            char names[400]; int n = 0;
            if (safeSectionNames(c, names, sizeof(names), &n)) log("   inventory sections (%d): %s", n, names);
        }
        // Where is its stock? Buildings around it that hold items (shop counter, storage...).
        logStockBuildings(c, "before restock");
        // Experiment (test runs only): does the squad's own restock put the stock somewhere?
        if (items == 0 && !g_probedRestock)
        {
            g_probedRestock = true;
            int before = items, after = -2, vendor = -1, special = -1;
            bool called = safeRestock(c, &after, &vendor, &special);
            log("   restock experiment: refreshInventory(true) %s; loose items %d -> %d (has vendor list %d, special items list %d)",
                called ? "called" : "FAILED", before, after, vendor, special);
            logStockBuildings(c, "after restock");
        }
    }

    // A shopkeeper's squad, for the ghost on the client (protocol 20): see EntitySpawn.
    bool safeTradeSpawn(Character* c, char* squad, int squadLen, float* mult, int* flags)
    {
        __try
        {
            ActivePlatoon* ap = c->getPlatoon();
            if (!ap || !ap->me || !ap->getIsTrader()) return false;
            // The squad's own data is a numbered copy ("Marchands Libres_0"); the template is what to recreate.
            GameData* tmpl = ap->me->squadTemplate ? ap->me->squadTemplate : ap->me->data;
            if (!tmpl) return false;
            strncpy_s(squad, squadLen, tmpl->stringID.c_str(), _TRUNCATE);
            *mult = ap->me->priceMultWhenITrade;
            *flags = (ap->me->_iBuyStolenGoods ? 1 : 0) | (ap->me->_iBuyIllegalGoods ? 2 : 0);
            return squad[0] != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void sendSpawnTo(uint8_t client, Npc& n, Character* c)
    {
        logTrader(c, n.id);
        EntitySpawn sp;
        sp.netId = n.id;
        sp.kind = KIND_CHARACTER;
        sp.gameDataName = c->data ? c->data->stringID : std::string();
        sp.displayName = c->getName();
        Ogre::Vector3 p = c->getPosition();
        sp.x = p.x; sp.y = p.y; sp.z = p.z;
        Faction* f = c->getFaction();
        sp.factionSid = f && f->getData() ? f->getData()->stringID : std::string();
        {
            char squad[160]; float mult = 1.f; int flags = 0;
            if (safeTradeSpawn(c, squad, sizeof(squad), &mult, &flags)) { sp.hasTrade = true; sp.squadSid = squad; sp.priceMult = mult; sp.tradeFlags = (uint8_t)flags; }
        }
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

    // Empty-town safeguard. A client in the shared world sees only what the host simulates. If the host's
    // zone around that client never loads (engine refuses, far corner of the map...), the client would
    // stand in an empty town: no residents, no prisoners, nobody to talk to. After ZONE_GRACE_MS without
    // a loaded zone the client is moved to its own world instead (its game simulates its surroundings,
    // exactly as when it is far from the host). Released once the zone loads or the client moves away.
    const DWORD ZONE_GRACE_MS = 25000;
    const float ZONE_RELEASE_DIST = 2500.f;
    std::map<uint8_t, DWORD> g_zoneBadSince;                       // host: client -> first check without a loaded zone
    std::map<uint8_t, Ogre::Vector3> g_forcedApart;                // host: client -> where it was when moved to its own world

    bool safeZoneLoaded(const Ogre::Vector3* p)
    {
        __try { return ou->zoneMgr && ou->zoneMgr->isZoneLoadedT(*p); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void watchRemoteZones()
    {
        if (!g_cfg.loadSharing) return;
        DWORD now = GetTickCount();
        std::vector<PlayerInfo> players = g_session.players();
        for (size_t p = 0; p < players.size(); ++p)
        {
            uint8_t id = players[p].id;
            if (id == HOST_ID) continue;
            std::vector<Ogre::Vector3> centres;
            cs_ghostPositions(id, centres);
            if (centres.empty()) continue;
            bool loaded = false;
            for (size_t i = 0; i < centres.size() && !loaded; ++i) loaded = safeZoneLoaded(&centres[i]);
            // Host NPCs shown to that client prove the world works there, whatever the zone query says.
            for (std::map<Character*, Npc>::iterator n = g_npcs.begin(); n != g_npcs.end() && !loaded; ++n)
                loaded = n->second.clients.count(id) != 0;
            std::map<uint8_t, Ogre::Vector3>::iterator forced = g_forcedApart.find(id);
            if (forced != g_forcedApart.end())
            {
                if (loaded || forced->second.distance(centres[0]) > ZONE_RELEASE_DIST)
                {
                    log("zone watch: %s released from its own world (%s)", playerName(id).c_str(), loaded ? "the host's zone is loaded there" : "it moved away");
                    g_forcedApart.erase(forced);
                    g_zoneBadSince.erase(id);
                }
                continue;
            }
            std::map<uint8_t, int>::iterator m = g_clientMode.find(id);
            if (m == g_clientMode.end() || m->second != MODE_SHARED || loaded) { g_zoneBadSince.erase(id); continue; }
            std::map<uint8_t, DWORD>::iterator bad = g_zoneBadSince.find(id);
            if (bad == g_zoneBadSince.end()) { g_zoneBadSince[id] = now; continue; }
            if (now - bad->second < ZONE_GRACE_MS) continue;
            g_forcedApart[id] = centres[0];
            g_zoneBadSince.erase(id);
            log("zone watch: the host's zone around %s (%.0f, %.0f, %.0f) is not loaded after %lu s: it simulates its own surroundings instead",
                playerName(id).c_str(), centres[0].x, centres[0].y, centres[0].z, (unsigned long)(ZONE_GRACE_MS / 1000));
        }
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
            if (g_forcedApart.count(client)) mode = MODE_APART;   // zone watch: the host's zone there never loaded
            if (prev == g_clientMode.end() || prev->second != mode)
                log("load sharing: player %d is %s (%.0f units from us)", (int)client, mode == MODE_SHARED ? "in our world" : "on its own", d);
        }
        if (prev == g_clientMode.end() || prev->second != mode)
        {
            g_clientMode[client] = mode;
            if (mode == MODE_SHARED) g_lastZoneKeep = 0;   // client (back) in our world: load the zones around it now, not in up to 3 s
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

    // Client: has the host sent any NPC next to our characters yet?
    const DWORD PURGE_GRACE_MS = 45000;
    DWORD g_sharedSince = 0;                 // client: when the shared world began (GetTickCount)
    bool g_waitLogged = false, g_graceDone = false;
    bool hostPeopleArrived()
    {
        std::vector<Ogre::Vector3> centres;
        cs_localPositions(centres);
        if (centres.empty()) return true;    // nothing to wait for
        std::vector<uint32_t> ids;
        chars_npcGhostsNear(centres, INTEREST_RADIUS, ids);   // what the host sends us (its interest radius)
        return !ids.empty();
    }

    // Client: the local people removed while the host's world is shown are remembered, and brought back
    // when we are on our own again (far from the host), on top of what the zones create by themselves.
    const size_t STASH_MAX = 3000;
    const DWORD STASH_MAX_AGE_MS = 10 * 60 * 1000;
    const float RESTORE_RADIUS = 1500.f;
    const float STASH_DUPLICATE_DIST = 30.f;   // a repopulated copy of someone already remembered
    std::vector<StashedNpc> g_stash;
    std::vector<StashedNpc> g_restoreQueue;
    int g_restored = 0, g_restoreSkipped = 0, g_restoreFailed = 0;

    void rememberNpc(const StashedNpc& s)
    {
        if (g_stash.size() >= STASH_MAX) return;
        for (size_t i = 0; i < g_stash.size(); ++i)
            if (g_stash[i].tmpl == s.tmpl && g_stash[i].faction == s.faction && g_stash[i].pos.distance(s.pos) < STASH_DUPLICATE_DIST) return;
        g_stash.push_back(s);
    }

    // We are on our own again: queue the remembered people who were close enough, then drop the memory.
    void queueRestore()
    {
        g_restoreQueue.clear();
        g_restored = g_restoreSkipped = g_restoreFailed = 0;
        std::vector<Ogre::Vector3> centres;
        cs_localPositions(centres);
        DWORD now = GetTickCount();
        size_t tooOld = 0, tooFar = 0;   // ('far' / 'near' are windows.h macros)
        for (size_t i = 0; i < g_stash.size(); ++i)
        {
            if (now - g_stash[i].at > STASH_MAX_AGE_MS) { ++tooOld; continue; }
            if (nearest(g_stash[i].pos, centres) > RESTORE_RADIUS) { ++tooFar; continue; }
            g_restoreQueue.push_back(g_stash[i]);
        }
        log("load sharing: %d remembered NPC(s): %d to bring back, %d too far from us, %d too old", (int)g_stash.size(), (int)g_restoreQueue.size(), (int)tooFar, (int)tooOld);
        g_stash.clear();
    }

    // A few per call, so bringing back a whole town does not make the game hitch.
    void restoreStep()
    {
        for (int k = 0; k < 12 && !g_restoreQueue.empty(); ++k)
        {
            StashedNpc s = g_restoreQueue.back();
            g_restoreQueue.pop_back();
            Ogre::Vector3 p = s.pos;
            if (!safeZoneLoaded(&p)) { ++g_restoreSkipped; }   // creating people in an unloaded zone only makes churn
            else if (cs_restoreNpc(s)) ++g_restored;
            else ++g_restoreFailed;
            if (g_restoreQueue.empty())
                log("load sharing: %d NPC(s) brought back, %d skipped (zone not loaded), %d failed", g_restored, g_restoreSkipped, g_restoreFailed);
        }
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
            if (everywhere && n < (g_cfg.autotest ? 80 : 5)) log("world sync: removing local '%s'", (*it)->getName().c_str());
            StashedNpc keep; bool described = cs_stashNpc(lc, keep);   // before it is gone
            if (cs_destroy(*it))
            {
                ++n;
                if (described) rememberNpc(keep);
            }
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

// Test runs: what stands around our first character, in one line (see the autotest census in KenshiMP.cpp).
std::string npcs_debugCensus()
{
    std::vector<Ogre::Vector3> centres;
    cs_localPositions(centres);
    if (centres.empty()) return "";
    std::vector<Ogre::Vector3> mine(1, centres[0]);
    const float radii[3] = { 100.f, 400.f, 1500.f };
    int local[3] = { 0, 0, 0 }, ghosts[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; ++i)
    {
        std::set<Character*> locals; collectAround(mine, radii[i], locals); local[i] = (int)locals.size();
        std::vector<uint32_t> ids; chars_npcGhostsNear(mine, radii[i], ids); ghosts[i] = (int)ids.size();
    }
    // Traders in the surroundings (replicated or not): probe each one once (what makes a shop work).
    {
        static std::set<Character*> probed;
        std::set<Character*> around; collectAround(mine, 1500.f, around);
        for (std::set<Character*>::iterator it = around.begin(); it != around.end(); ++it)
        {
            if (probed.count(*it)) continue;
            int isT = 0, platT = 0, items = 0, sect = 0; float mult = 0; char squad[8];
            if (!safeTraderProbe(*it, &isT, &platT, &mult, squad, sizeof(squad), &items, &sect) || (!isT && platT != 1)) continue;
            probed.insert(*it);
            logTrader(*it, 0);
            if (probed.size() >= 6) break;
        }
    }
    const char* mode = g_session.isHost() ? "host" : (!g_session.active() ? "not connected" : (g_zoneMode == MODE_SHARED ? "shared world" : (g_zoneMode == MODE_APART ? "own world" : "mode unknown")));
    char buf[400];
    sprintf_s(buf, "census at (%.0f, %.0f, %.0f) [%s]: zone loaded=%d; local NPCs %d/%d/%d, host NPC ghosts with a body %d/%d/%d (within 100/400/1500), %d ghost(s) alive in total",
              mine[0].x, mine[0].y, mine[0].z, mode, safeZoneLoaded(&mine[0]) ? 1 : 0, local[0], local[1], local[2], ghosts[0], ghosts[1], ghosts[2], chars_ghostTotal());
    return buf;
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
        if (now - g_lastZoneKeep >= ZONE_KEEPALIVE_MS) { g_lastZoneKeep = now; { keepRemoteZonesLoaded(); watchRemoteZones(); } }
        return;
    }
    if (g_zoneMode == MODE_APART)
    {
        static DWORD lastRestore = 0;
        if (!g_restoreQueue.empty() && now - lastRestore >= 100) { lastRestore = now; restoreStep(); }
    }
    if (g_zoneMode != MODE_SHARED) return;   // our own world (far from the host), or not told yet
    // Coming back from our own world (or joining): our local people are removed only once the host's
    // have arrived around us. The host needs a few seconds to load the zone again; removing ours at
    // once left the town empty ("my friend came back and the town was empty").
    if (now - g_lastCleanup < CLEANUP_PERIOD_MS) return;
    g_lastCleanup = now;
    bool arrived = hostPeopleArrived();
    if (arrived) g_graceDone = true;   // the host's people have been here once: no more waiting until the next change of world
    if (!g_graceDone && now - g_sharedSince < PURGE_GRACE_MS)
    {
        if (!g_waitLogged)
        {
            g_waitLogged = true;
            std::vector<Ogre::Vector3> centres; cs_localPositions(centres);
            std::set<Character*> locals; collectAround(centres, CLEANUP_RADIUS, locals);
            log("load sharing: keeping the %d local NPC(s) around us until the host's arrive (at most %lu s)", (int)locals.size(), (unsigned long)(PURGE_GRACE_MS / 1000));
        }
        return;
    }
    if (g_waitLogged) log("load sharing: %s after %lu ms: removing the local NPCs now", arrived ? "the host's NPCs arrived" : "grace period over", (unsigned long)(now - g_sharedSince));
    g_waitLogged = false;
    g_graceDone = true;
    if (!g_purgedAll) { g_purgedAll = true; clientCleanup(true); }
    clientCleanup(false);
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
    g_zoneBadSince.clear();
    g_forcedApart.clear();
    g_stash.clear(); g_restoreQueue.clear();
    g_sharedSince = 0;
    g_waitLogged = false; g_graceDone = false;
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
    if (entering) { g_purgedAll = false; g_sharedSince = GetTickCount(); g_waitLogged = false; g_graceDone = false; g_restoreQueue.clear(); }   // the host's people replace ours around us (once they arrive)
    else queueRestore();   // on our own again: the people removed meanwhile come back
    log("load sharing: %s", entering ? "the host simulates the world around us" : "we simulate our own surroundings (far from the host)");
    if (g_cfg.debugKeys) chat_notice(entering ? "World shared with the host" : "Own world (far from the host)");
}

void npcs_onPlayerLeft(uint8_t id)
{
    g_clientMode.erase(id);
    g_zoneBadSince.erase(id);
    g_forcedApart.erase(id);
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
// populateBuilding also furnishes the building (tables, shop counters, cells...): blocking it left the towns of
// a client who joined before creating its world completely empty (reviews: "the town was empty, no cells in
// the tower", no furniture, no shops). It always runs now; the people it creates come through
// createCharacterForBuilding (blocked above) and the local NPCs that slip through are removed by clientCleanup.
void (*populate_orig)(RootObjectFactory*, Building*) = NULL;
void populate_hook(RootObjectFactory* self, Building* b)
{
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
