// World towns: the host's world states and town changes are everybody's.
//
// How Kenshi changes a town (found by disassembling 1.0.65, KenshiLib does not expose it):
//  - a hidden global table holds the "world state" of every unique NPC (GameData of the
//    character -> DEAD / ALIVE / IMPRISONED, plus a "player involvement" flag). Kenshi writes it
//    when a unique dies (Character::declareDead) or is jailed / freed
//    (Character::uniqueStateUpdate) and saves it with the game. A missing entry means ALIVE.
//  - a town's FCS data lists "override town" entries, each with "world state" conditions
//    (WorldEventStateQuery: such NPC dead / imprisoned / alive...). When a zone activates,
//    ZoneManager::checkForRepopulateTown evaluates them and, if one is true, calls
//    Town::notifyRepopulation(override): Town::replacementTown is set, the town's faction / type /
//    public flag follow it, and the buildings and residents are spawned from it. That is how a
//    town gets destroyed, abandoned or taken by another faction.
//
// Sync:
//  Host   : every 2 s, reads its state table and the towns that have a replacement; on change
//           (and on resync) sends both to everybody (MSG_WORLD_STATES).
//  Client : writes the host's states into its own table, so its zones choose the same town
//           versions, dialogues and campaigns see the same world; town replacements the host
//           has are applied to towns whose zones are not loaded (never under the player's eyes:
//           the change shows next time the zone loads, as in the base game). Towns are never
//           reverted (the base game never reverts them either).
//           A client that simulates its own surroundings (load sharing, far from the host)
//           reports the states that change in its world (a unique killed there) to the host
//           (MSG_WORLD_STATE_REPORT); the host adopts them and everybody gets them.
//
// The table is reached through two engine functions found by a byte pattern in
// Character::uniqueStateUpdate (the getter of the table and its operator[]); if the pattern is
// not found the feature stays off (logged), nothing else is affected.
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/GameData.h>
#include <kenshi/Character.h>
#include <kenshi/Town.h>
#include <kenshi/SharedKing.h>
#include <kenshi/ZoneManager.h>
#include <core/Functions.h>

#include "Shared.h"

#include <map>
#include <vector>
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace mp;

namespace kmp {

namespace
{
    const DWORD CHECK_PERIOD_MS = 2000;
    const DWORD REFRESH_MS = 60000;          // host: unchanged table still resent this often
    const uint16_t MAX_ENTRIES = 4096;

    enum { ST_DEAD = 0, ST_ALIVE = 1, ST_IMPRISONED = 2 };   // WorldStateEnum

    struct Entry
    {
        uint8_t state;
        uint8_t involved;      // the player was involved (Kenshi's own flag, kept as is)
        Entry() : state(ST_ALIVE), involved(0) {}
        Entry(uint8_t s, uint8_t i) : state(s), involved(i) {}
        bool operator==(const Entry& o) const { return state == o.state && involved == o.involved; }
        bool operator!=(const Entry& o) const { return !(*this == o); }
        bool isDefault() const { return state == ST_ALIVE && !involved; }
    };
    typedef std::map<std::string, Entry> States;          // unique NPC sid -> state (defaults left out)
    typedef std::map<std::string, std::string> TownReps;  // town sid -> replacement town sid

    // ---- engine access -------------------------------------------------------------------
    typedef void* (*GetTableFn)();
    typedef char* (*TableIndexFn)(void* table, GameData** key);   // returns the key/value pair
    GetTableFn g_getTable = NULL;
    TableIndexFn g_tableIndex = NULL;
    // Layout (boost::unordered_map<GameData*, UniqueState> of the game's build).
    const size_t MAP_BUCKET_COUNT = 0x18, MAP_SIZE = 0x20, MAP_BUCKETS = 0x38;
    const size_t NODE_KEY = 0x10, NODE_STATE = 0x40, NODE_INVOLVED = 0x44;   // node = next, hash, pair
    const size_t PAIR_STATE = 0x30, PAIR_INVOLVED = 0x34;

    // Follows "jmp rel32" thunks (incremental-link style jump tables of the exe).
    const unsigned char* followJumps(const unsigned char* p)
    {
        for (int i = 0; i < 4 && p && p[0] == 0xE9; ++i) p = p + 5 + *(const int32_t*)(p + 1);
        return p;
    }
    const unsigned char* callTarget(const unsigned char* call) { return call + 5 + *(const int32_t*)(call + 1); }

    // In uniqueStateUpdate (jailed unique -> IMPRISONED):
    //   call getTable ; lea rdx,[rsp+??] ; mov rcx,rax ; mov rdi,rax ; call operator[] ; cmp dword ptr [rax+30h],0
    bool scanPattern(const unsigned char* f)
    {
        static const int LEN = 24;
        for (int i = 0; i < 0x500; ++i)
        {
            const unsigned char* p = f + i;
            if (p[0] == 0xC3 && p[1] == 0xCC) break;   // end of the function
            if (p[0] != 0xE8 || p[5] != 0x48 || p[6] != 0x8D || p[7] != 0x54 || p[8] != 0x24) continue;
            if (p[10] != 0x48 || p[11] != 0x8B || p[12] != 0xC8 || p[13] != 0x48 || p[14] != 0x8B) continue;
            if (p[16] != 0xE8 || p[21] != 0x83 || p[22] != 0x78 || p[23] != PAIR_STATE || p[LEN] != 0x00) continue;
            g_getTable = (GetTableFn)followJumps(callTarget(p));
            g_tableIndex = (TableIndexFn)followJumps(callTarget(p + 16));
            return true;
        }
        return false;
    }
    bool safeScan(const unsigned char* f) { __try { return scanPattern(f); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

    void* table()
    {
        if (!g_getTable) return NULL;
        __try
        {
            char* m = (char*)g_getTable();
            if (!m) return NULL;
            size_t buckets = *(size_t*)(m + MAP_BUCKET_COUNT);
            if (buckets == 0 || (buckets & (buckets - 1)) != 0 || buckets > (1u << 24)) return NULL;   // not the layout we know
            return m;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }

    // Reads the whole table (GameData pointers; names are taken outside the SEH frame).
    struct RawEntry { GameData* gd; int state; bool involved; };
    bool safeReadTable(char* m, std::vector<RawEntry>* out)
    {
        __try
        {
            size_t count = *(size_t*)(m + MAP_SIZE);
            char** buckets = *(char***)(m + MAP_BUCKETS);
            if (count == 0 || !buckets) return true;
            size_t bc = *(size_t*)(m + MAP_BUCKET_COUNT);
            char* node = buckets[bc];   // the extra bucket holds the start of the node list
            for (size_t n = 0; node && n < count + 16 && n < 100000; ++n)
            {
                RawEntry e;
                e.gd = *(GameData**)(node + NODE_KEY);
                e.state = *(int*)(node + NODE_STATE);
                e.involved = *(bool*)(node + NODE_INVOLVED);
                if (e.gd) out->push_back(e);
                node = *(char**)node;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool readStates(States& out)
    {
        char* m = (char*)table();
        if (!m) return false;
        std::vector<RawEntry> raw;
        if (!safeReadTable(m, &raw)) return false;
        for (size_t i = 0; i < raw.size(); ++i)
        {
            if (raw[i].state < ST_DEAD || raw[i].state > ST_IMPRISONED) continue;
            Entry e((uint8_t)raw[i].state, raw[i].involved ? 1 : 0);
            if (e.isDefault()) continue;
            const std::string& sid = raw[i].gd->stringID;
            if (!sid.empty()) out[sid] = e;
        }
        return true;
    }

    bool safeWrite(void* m, GameData* gd, const Entry* e)
    {
        __try
        {
            char* pair = g_tableIndex(m, &gd);
            if (!pair) return false;
            *(int*)(pair + PAIR_STATE) = e->state;
            *(bool*)(pair + PAIR_INVOLVED) = e->involved != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool writeState(const std::string& sid, const Entry& e)
    {
        void* m = table();
        GameData* gd = (m && ou) ? ou->gamedata.getData(sid) : NULL;
        if (!gd)
        {
            static DWORD warn = 0;
            if (m && GetTickCount() - warn > 30000) { warn = GetTickCount(); log("world state: unique '%s' unknown here (missing mod?)", sid.c_str()); }
            return false;
        }
        return safeWrite(m, gd, &e);
    }

    // ---- towns ---------------------------------------------------------------------------
    bool safeTowns(std::vector<Town*>* out)
    {
        __try
        {
            if (!shou || !shou->townList) return false;
            lektor<RootObject*>& all = shou->townList->getAllTowns();
            for (uint32_t i = 0; i < all.size(); ++i)
            {
                RootObject* o = all[i];
                if (!o || o->getDataType() != TOWN) continue;
                Town* t = static_cast<TownBase*>(o)->isTown();
                if (t) out->push_back(t);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeTownData(Town* t, GameData** orig, GameData** rep)
    {
        __try { *orig = t->getOriginalGameData(); *rep = t->replacementTown; return *orig != NULL; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void readTowns(TownReps& out, std::map<std::string, Town*>* bySid)
    {
        std::vector<Town*> ts;
        if (!safeTowns(&ts)) return;
        for (size_t i = 0; i < ts.size(); ++i)
        {
            GameData *orig = NULL, *rep = NULL;
            if (!safeTownData(ts[i], &orig, &rep)) continue;
            if (bySid) (*bySid)[orig->stringID] = ts[i];
            if (rep && rep != orig) out[orig->stringID] = rep->stringID;
        }
    }

    // A zone of that town is loaded: its buildings and residents exist, leave it alone.
    bool safeTownLoadedIn(Town* t, lektor<ZoneMap*>* zones, bool* loaded)
    {
        __try
        {
            *loaded = true;
            if (!ou || !ou->zoneMgr) return false;
            ou->zoneMgr->getZonesTouchingTown(*zones, t);
            bool any = false;
            for (uint32_t i = 0; i < zones->size(); ++i)
                if ((*zones)[i] && ((*zones)[i]->isActive() || (*zones)[i]->isLoadedMT())) { any = true; break; }
            *loaded = any;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeTownLoaded(Town* t, bool* loaded)
    {
        lektor<ZoneMap*> zones;
        return safeTownLoadedIn(t, &zones, loaded);
    }
    bool safeRepopulate(Town* t, GameData* d)
    {
        __try { t->notifyRepopulation(d); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ---- wire ----------------------------------------------------------------------------
    void writeStates(ByteWriter& w, const States& s)
    {
        w.u16((uint16_t)(s.size() < MAX_ENTRIES ? s.size() : MAX_ENTRIES));
        uint16_t n = 0;
        for (States::const_iterator it = s.begin(); it != s.end() && n < MAX_ENTRIES; ++it, ++n)
        { w.str(it->first); w.u8(it->second.state); w.u8(it->second.involved); }
    }
    bool readStatesMsg(ByteReader& r, States& s)
    {
        uint16_t n = r.u16();
        if (!r.ok() || n > MAX_ENTRIES) return false;
        for (uint16_t i = 0; i < n && r.ok(); ++i)
        {
            std::string sid = r.str(); uint8_t st = r.u8(); uint8_t inv = r.u8();
            if (!r.ok()) return false;
            if (sid.empty() || st > ST_IMPRISONED) continue;
            s[sid] = Entry(st, inv ? 1 : 0);
        }
        return r.ok();
    }

    // ---- state ---------------------------------------------------------------------------
    bool g_enabled = false;              // engine functions found
    DWORD g_lastCheck = 0, g_lastSend = 0;
    uint32_t g_lastHash = 0;             // host: what was last sent
    bool g_forceSend = false;

    // Client
    bool g_haveHost = false;             // a table arrived since the world was loaded
    States g_hostStates;                 // as last received
    TownReps g_hostTowns;
    States g_baseline;                   // our table right after we last synced it (own-world reports)

    bool ownWorld() { return !g_session.isHost() && npcs_ownWorld(); }

    uint32_t hashTables(const States& s, const TownReps& t)
    {
        ByteWriter w; writeStates(w, s);
        w.u16((uint16_t)t.size());
        for (TownReps::const_iterator it = t.begin(); it != t.end(); ++it) { w.str(it->first); w.str(it->second); }
        return hashBytes(w.data);
    }

    void hostSend(DWORD now, bool force)
    {
        States s; TownReps t;
        if (!readStates(s)) return;
        readTowns(t, NULL);
        uint32_t h = hashTables(s, t);
        if (h == g_lastHash && !force && now - g_lastSend < REFRESH_MS) return;
        if (h != g_lastHash) log("world states: %u uniques not alive, %u towns changed -> players", (unsigned)s.size(), (unsigned)t.size());
        g_lastHash = h; g_lastSend = now;
        ByteWriter w;
        writeStates(w, s);
        w.u16((uint16_t)t.size());
        for (TownReps::const_iterator it = t.begin(); it != t.end(); ++it) { w.str(it->first); w.str(it->second); }
        g_session.send(MSG_WORLD_STATES, w.data);
    }

    // Client: our table := wanted (entries absent from it are ALIVE).
    int enforce(const States& wanted, const States& local)
    {
        int written = 0;
        for (States::const_iterator it = wanted.begin(); it != wanted.end(); ++it)
        {
            States::const_iterator l = local.find(it->first);
            if (l == local.end() || l->second != it->second) { if (writeState(it->first, it->second)) ++written; }
        }
        for (States::const_iterator it = local.begin(); it != local.end(); ++it)
            if (!wanted.count(it->first) && writeState(it->first, Entry())) ++written;
        return written;
    }

    // Client: host's town replacements for towns not loaded right now.
    void applyTowns(DWORD now)
    {
        if (g_hostTowns.empty() || !ou) return;
        TownReps mine; std::map<std::string, Town*> bySid;
        readTowns(mine, &bySid);
        int applied = 0;
        std::string lastName;
        for (TownReps::const_iterator it = g_hostTowns.begin(); it != g_hostTowns.end(); ++it)
        {
            TownReps::const_iterator m = mine.find(it->first);
            if (m != mine.end() && m->second == it->second) continue;   // already that version
            std::map<std::string, Town*>::iterator t = bySid.find(it->first);
            if (t == bySid.end()) continue;                              // no such town here (mods)
            GameData* d = ou->gamedata.getData(it->second);
            if (!d) { static DWORD warn = 0; if (now - warn > 30000) { warn = now; log("town version '%s' unknown here (missing mod?)", it->second.c_str()); } continue; }
            bool loaded = true;
            if (!safeTownLoaded(t->second, &loaded) || loaded) continue;   // retried at the next check
            if (!safeRepopulate(t->second, d)) { log("town %s: notifyRepopulation failed", it->first.c_str()); continue; }
            ++applied; lastName = d->name;
            log("town %s -> %s (%s), as in the host's world", it->first.c_str(), it->second.c_str(), d->name.c_str());
        }
        if (applied == 1) chat_notice(TF("The world changed: %s (host's world).", lastName.c_str()));
        else if (applied > 1) chat_notice(TF("%d towns changed to match the host's world.", applied));
    }

    void clientCheck(DWORD now)
    {
        if (!g_haveHost) return;
        States local;
        if (!readStates(local)) return;
        if (ownWorld())
        {
            // Our own world: what changed here since the last sync goes to the host.
            States changes;
            for (States::const_iterator it = local.begin(); it != local.end(); ++it)
            {
                States::const_iterator b = g_baseline.find(it->first);
                if (b == g_baseline.end() || b->second != it->second) changes[it->first] = it->second;
            }
            for (States::const_iterator it = g_baseline.begin(); it != g_baseline.end(); ++it)
                if (!local.count(it->first)) changes[it->first] = Entry();
            if (!changes.empty())
            {
                ByteWriter w; w.u8(HOST_ID); writeStates(w, changes);
                g_session.sendTo(HOST_ID, MSG_WORLD_STATE_REPORT, w.data);
                log("world states: %u changes in our own world reported to the host", (unsigned)changes.size());
            }
            g_baseline = local;
        }
        else
        {
            // Shared world: the host decides; undo anything our side changed on its own.
            int n = enforce(g_hostStates, local);
            static DWORD lastLog = 0;
            if (n && now - lastLog > 30000) { lastLog = now; log("world states: %d entries put back to the host's", n); }
            g_baseline = g_hostStates;
        }
        applyTowns(now);
    }
}

bool towns_install()
{
    const unsigned char* f = (const unsigned char*)KenshiLib::GetRealAddress(&Character::uniqueStateUpdate);
    if (!f || !safeScan(followJumps(f)))
    {
        log("world states: engine table not found (town and unique NPC sync off)");
        return false;
    }
    g_enabled = true;
    log("world states: table getter %p, operator[] %p", (void*)g_getTable, (void*)g_tableIndex);
    return true;
}

void towns_tick(DWORD now)
{
    if (!g_enabled || !g_cfg.townSync || !ready()) return;
    if (now - g_lastCheck < CHECK_PERIOD_MS && !g_forceSend) return;
    g_lastCheck = now;
    bool force = g_forceSend;
    g_forceSend = false;   // even if the table cannot be read now (the periodic check retries)
    if (g_session.isHost()) hostSend(now, force);
    else clientCheck(now);
}

void towns_onMessage(const NetEvent& e)
{
    if (!g_enabled || !g_cfg.townSync) return;
    ByteReader r(e.body);
    if (e.msgType == MSG_WORLD_STATES)
    {
        if (g_session.isHost() || e.sender != HOST_ID) return;
        States s; TownReps t;
        if (!readStatesMsg(r, s)) return;
        uint16_t n = r.u16();
        if (!r.ok() || n > MAX_ENTRIES) return;
        for (uint16_t i = 0; i < n && r.ok(); ++i) { std::string a = r.str(); std::string b = r.str(); if (r.ok() && !a.empty() && !b.empty()) t[a] = b; }
        if (!r.ok()) return;

        States local;
        if (!readStates(local)) return;
        int written = 0;
        if (!g_haveHost || !ownWorld()) written = enforce(s, local);
        else
        {
            // Own world: only what the host changed since its last table; our own changes stand
            // (they were reported and come back in a later table).
            for (States::const_iterator it = s.begin(); it != s.end(); ++it)
            {
                States::const_iterator o = g_hostStates.find(it->first);
                if ((o == g_hostStates.end() || o->second != it->second) && writeState(it->first, it->second)) ++written;
            }
            for (States::const_iterator it = g_hostStates.begin(); it != g_hostStates.end(); ++it)
                if (!s.count(it->first) && writeState(it->first, Entry())) ++written;
        }
        if (written || !g_haveHost) log("world states from the host: %u uniques not alive, %u towns changed, %d written here", (unsigned)s.size(), (unsigned)t.size(), written);
        g_hostStates = s; g_hostTowns = t; g_haveHost = true;
        States after;
        if (readStates(after)) g_baseline = after;
        if (g_cfg.autotest)
            for (States::const_iterator it = s.begin(); it != s.end(); ++it)
            {
                States::const_iterator a = after.find(it->first);
                log("autotest: unique %s now %d here (host says %d)", it->first.c_str(), a == after.end() ? (int)ST_ALIVE : (int)a->second.state, (int)it->second.state);
            }
        applyTowns(GetTickCount());
    }
    else if (e.msgType == MSG_WORLD_STATE_REPORT)
    {
        // A client simulating its own surroundings: what happened to uniques there.
        if (!g_session.isHost()) return;
        uint8_t target = r.u8();
        States s;
        if (target != HOST_ID || !readStatesMsg(r, s)) return;
        if (!npcs_clientOwnWorld(e.sender))
        { log("world states from %s ignored (it is in the host's world)", playerName(e.sender).c_str()); return; }
        int written = 0;
        for (States::const_iterator it = s.begin(); it != s.end(); ++it)
            if (writeState(it->first, it->second)) { ++written; log("world state from %s: %s -> %d", playerName(e.sender).c_str(), it->first.c_str(), (int)it->second.state); }
        if (written) g_forceSend = true;   // everybody gets it at the next tick
    }
}

void towns_resendAll()
{
    if (g_session.isHost()) g_forceSend = true;
}

void towns_onWorldReload()
{
    g_haveHost = false;
    g_hostStates.clear(); g_hostTowns.clear(); g_baseline.clear();
    g_lastHash = 0;
    g_forceSend = g_session.isHost();   // clients get the table again after their MSG_RESYNC
}

} // namespace kmp

namespace kmp {
// Test (autotest): a unique NPC of the game data (base game first).
std::string towns_debugUnique()
{
    if (!ou) return "";
    lektor<GameData*> list;
    ou->gamedata.getDataOfType(list, CHARACTER);
    std::string other;
    for (uint32_t i = 0; i < list.size(); ++i)
    {
        GameData* d = list[i];
        if (!d) continue;
        auto it = d->bdata.find("unique");
        if (it == d->bdata.end() || !it->second) continue;
        if (d->stringID.find("gamedata.base") != std::string::npos) return d->stringID;
        if (other.empty()) other = d->stringID;
    }
    return other;
}

// Test (autotest): "name;x,y,z" of a town 8000-20000 units from us (the one nearest 12000), where
// the test bot stands as a far client (traders, residents: does it get a living town?).
std::string towns_debugFarTown()
{
    if (!ou || !ou->player || ou->player->playerCharacters.size() == 0) return "";
    Ogre::Vector3 me = ou->player->playerCharacters[0]->getPosition();
    std::vector<Town*> ts;
    if (!safeTowns(&ts)) return "";
    Town* best = NULL; float bestScore = 1e30f;
    for (size_t i = 0; i < ts.size(); ++i)
    {
        float d = ts[i]->getPosition().distance(me);
        if (d < 8000.f || d > 20000.f) continue;
        float score = fabsf(d - 12000.f);
        if (score < bestScore) { bestScore = score; best = ts[i]; }
    }
    if (!best) return "";
    Ogre::Vector3 p = best->getPosition();
    char buf[96]; sprintf_s(buf, ";%.0f,%.0f,%.0f", p.x, p.y, p.z);
    return best->getName() + buf;
}

// Test (autotest): "townSid;overrideSid" for the town farthest from us that has an "override
// town" version and is not replaced yet (its area is surely not loaded).
std::string towns_debugOverride()
{
    if (!ou || !ou->player || ou->player->playerCharacters.size() == 0) return "";
    Ogre::Vector3 me = ou->player->playerCharacters[0]->getPosition();
    std::vector<Town*> ts;
    if (!safeTowns(&ts)) return "";
    std::string best; float bestD = -1.f;
    for (size_t i = 0; i < ts.size(); ++i)
    {
        GameData *orig = NULL, *rep = NULL;
        if (!safeTownData(ts[i], &orig, &rep) || (rep && rep != orig)) continue;
        const Ogre::vector<GameDataReference>::type* refs = orig->getReferenceListIfExists("override town");
        if (!refs || refs->empty() || (*refs)[0].sid.empty() || !ou->gamedata.getData((*refs)[0].sid)) continue;
        float d = ts[i]->getPosition().squaredDistance(me);
        if (d > bestD) { bestD = d; best = orig->stringID + ";" + (*refs)[0].sid; }
    }
    return best;
}
}
