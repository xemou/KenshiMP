// Bounties: a crime seen in the host's world puts the bounty on the real character.
//
// Kenshi keeps bounties per character (Character::crimes: enforcing faction -> amount + crimes).
// In the shared world the witnesses are the host's NPCs, so a client's crime lands on the host's
// ghost of that character, which its owner never saw:
//  Host   : every second, compares the bounty of each player ghost with what its owner last said;
//           an increase (crime witnessed here) goes to the owner (MSG_BOUNTY_CRIME).
//  Owner  : adds it to its real character (the game's own UI, guards, prisons... then apply), and
//           sends the bounty table of its characters whenever it changes (MSG_BOUNTIES).
//  Others : copy that table onto their ghost of the character, so their guards know who is wanted.
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/GameData.h>
#include <kenshi/Character.h>
#include <kenshi/Faction.h>
#include <kenshi/BountyManager.h>
#include <kenshi/PlayerInterface.h>

#include "Shared.h"

#include <map>
#include <vector>
#include <stdio.h>

using namespace mp;

namespace kmp {

namespace
{
    const DWORD CHECK_MS = 1000;
    const uint16_t MAX_ENTRIES = 256;

    struct Entry
    {
        int32_t amount;
        uint32_t crimes;    // CrimeEnum bits (shown in the game's character panel)
        Entry() : amount(0), crimes(0) {}
        Entry(int32_t a, uint32_t c) : amount(a), crimes(c) {}
        bool operator!=(const Entry& o) const { return amount != o.amount || crimes != o.crimes; }
    };
    typedef std::map<std::string, Entry> Table;   // enforcing faction sid -> bounty

    std::map<uint32_t, uint32_t> g_sentHash;      // our characters: last table sent
    std::map<uint32_t, Table> g_known;            // ghosts: what their owner last said (or we reported)
    DWORD g_lastCheck = 0;
    bool g_forceSend = false;

    // ---- engine ----------------------------------------------------------------------------
    struct RawBounty { Faction* f; int amount; unsigned crimes; };
    bool safeRead(Character* c, std::vector<RawBounty>* out)
    {
        __try
        {
            ogre_unordered_map<Faction*, Bounty>::type& m = c->crimes.bounties;
            for (ogre_unordered_map<Faction*, Bounty>::type::iterator it = m.begin(); it != m.end(); ++it)
            {
                RawBounty b; b.f = it->first; b.amount = it->second.amount; b.crimes = it->second.crimes;
                if (b.f) out->push_back(b);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeFactionSid(Faction* f, std::string* out)
    {
        __try { GameData* d = f->getData(); if (!d) return false; *out = d->stringID; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool readTable(Character* c, Table& t)
    {
        std::vector<RawBounty> raw;
        if (!safeRead(c, &raw)) return false;
        for (size_t i = 0; i < raw.size(); ++i)
        {
            if (raw[i].amount <= 0) continue;
            std::string sid;
            if (safeFactionSid(raw[i].f, &sid) && !sid.empty()) t[sid] = Entry(raw[i].amount, raw[i].crimes);
        }
        return true;
    }

    bool safeSetCrimes(Character* c, Faction* f, unsigned bits, bool orBits)
    {
        __try
        {
            ogre_unordered_map<Faction*, Bounty>::type& m = c->crimes.bounties;
            for (ogre_unordered_map<Faction*, Bounty>::type::iterator it = m.begin(); it != m.end(); ++it)
                if (it->first == f) { it->second.crimes = orBits ? (it->second.crimes | bits) : bits; return true; }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeAdd(Character* c, Faction* f, int amount)
    {
        __try { c->crimes.unfairAddToBounty(f, amount); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeClear(Character* c, Faction* f)
    {
        __try { c->crimes.clearBounty(f); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    Faction* factionBySid(const std::string& sid)
    {
        if (!ou || !ou->factionMgr || sid.empty()) return NULL;
        return ou->factionMgr->getFactionByStringID(sid);
    }

    // Ghost := owner's table.
    void applyTable(Character* c, const Table& want)
    {
        Table cur;
        if (!readTable(c, cur)) return;
        for (Table::const_iterator it = cur.begin(); it != cur.end(); ++it)
            if (!want.count(it->first)) if (Faction* f = factionBySid(it->first)) safeClear(c, f);
        for (Table::const_iterator it = want.begin(); it != want.end(); ++it)
        {
            Table::const_iterator o = cur.find(it->first);
            if (o != cur.end() && !(o->second != it->second)) continue;
            Faction* f = factionBySid(it->first);
            if (!f) continue;
            if (o == cur.end() || o->second.amount != it->second.amount) { safeClear(c, f); safeAdd(c, f, it->second.amount); }
            safeSetCrimes(c, f, it->second.crimes, false);
        }
    }

    // ---- wire ------------------------------------------------------------------------------
    void writeTable(ByteWriter& w, const Table& t)
    {
        uint16_t n = (uint16_t)(t.size() < MAX_ENTRIES ? t.size() : MAX_ENTRIES), k = 0;
        w.u16(n);
        for (Table::const_iterator it = t.begin(); it != t.end() && k < n; ++it, ++k)
        { w.str(it->first); w.u32((uint32_t)it->second.amount); w.u32(it->second.crimes); }
    }
    bool readTableMsg(ByteReader& r, Table& t)
    {
        uint16_t n = r.u16();
        if (!r.ok() || n > MAX_ENTRIES) return false;
        for (uint16_t i = 0; i < n && r.ok(); ++i)
        {
            std::string sid = r.str(); int32_t a = (int32_t)r.u32(); uint32_t c = r.u32();
            if (!r.ok()) return false;
            if (sid.empty()) continue;
            if (a < 0) a = 0;
            if (a > 1000000) a = 1000000;
            t[sid] = Entry(a, c & 0xFFFFF);
        }
        return r.ok();
    }

    // Our characters' tables -> everybody (on change).
    void sendOwn(bool force)
    {
        std::vector<std::pair<uint32_t, Character*> > mine;
        chars_localCharacters(mine);
        for (size_t i = 0; i < mine.size(); ++i)
        {
            Table t;
            if (!readTable(mine[i].second, t)) continue;
            ByteWriter w; w.u32(mine[i].first); writeTable(w, t);
            uint32_t h = hashBytes(w.data);
            std::map<uint32_t, uint32_t>::iterator s = g_sentHash.find(mine[i].first);
            if (!force && s != g_sentHash.end() && s->second == h) continue;
            if (s == g_sentHash.end() && t.empty() && !force) { g_sentHash[mine[i].first] = h; continue; }   // nothing to tell yet
            g_sentHash[mine[i].first] = h;
            g_session.send(MSG_BOUNTIES, w.data);
        }
    }

    // Host: crimes our NPCs saw other players' characters commit -> their owners.
    void reportWitnessed()
    {
        std::vector<std::pair<uint32_t, Character*> > ghosts;
        chars_playerGhosts(ghosts);
        std::map<uint8_t, ByteWriter> perOwner;
        std::map<uint8_t, int> counts;
        for (size_t i = 0; i < ghosts.size(); ++i)
        {
            uint32_t id = ghosts[i].first;
            uint8_t owner = netIdOwner(id);
            if (owner == g_session.localId() || npcs_clientOwnWorld(owner)) continue;   // its crimes there are its own world's
            Table now;
            if (!readTable(ghosts[i].second, now)) continue;
            Table& known = g_known[id];
            Table delta;
            for (Table::const_iterator it = now.begin(); it != now.end(); ++it)
            {
                Table::iterator k = known.find(it->first);
                int before = k == known.end() ? 0 : k->second.amount;
                if (it->second.amount <= before) continue;
                uint32_t newCrimes = it->second.crimes & ~(k == known.end() ? 0u : k->second.crimes);
                delta[it->first] = Entry(it->second.amount - before, newCrimes);
                known[it->first] = it->second;
            }
            if (delta.empty()) continue;
            ByteWriter& w = perOwner[owner];
            if (w.data.empty()) { w.u8(owner); w.u16(0); }   // count patched below
            for (Table::const_iterator it = delta.begin(); it != delta.end(); ++it)
            {
                w.u32(id); w.str(it->first); w.u32((uint32_t)it->second.amount); w.u32(it->second.crimes);
                ++counts[owner];
                log("bounty: %s's character %08x wanted by %s (+%d) in our world", playerName(owner).c_str(), id, it->first.c_str(), it->second.amount);
            }
        }
        for (std::map<uint8_t, ByteWriter>::iterator it = perOwner.begin(); it != perOwner.end(); ++it)
        {
            uint16_t n = (uint16_t)counts[it->first];
            it->second.data[1] = (uint8_t)(n & 0xFF); it->second.data[2] = (uint8_t)(n >> 8);
            g_session.sendTo(it->first, MSG_BOUNTY_CRIME, it->second.data);
        }
    }
}

void bounties_tick(DWORD now)
{
    if (!ready() || (now - g_lastCheck < CHECK_MS && !g_forceSend)) return;
    g_lastCheck = now;
    bool force = g_forceSend;
    g_forceSend = false;
    sendOwn(force);
    if (g_session.isHost()) reportWitnessed();
}

void bounties_onMessage(const NetEvent& e)
{
    ByteReader r(e.body);
    if (e.msgType == MSG_BOUNTIES)
    {
        uint32_t id = r.u32();
        Table t;
        if (!r.ok() || !readTableMsg(r, t) || isNpcNetId(id) || netIdOwner(id) != e.sender) return;
        bool changed = false;
        {
            std::map<uint32_t, Table>::iterator k = g_known.find(id);
            if (k == g_known.end()) changed = !t.empty();
            else if (k->second.size() != t.size()) changed = true;
            else for (Table::const_iterator it = t.begin(); it != t.end() && !changed; ++it)
            { Table::const_iterator o = k->second.find(it->first); changed = o == k->second.end() || o->second != it->second; }
        }
        g_known[id] = t;
        if (Character* c = chars_byNetId(id))
            if (chars_isGhost(c))
            {
                applyTable(c, t);
                if (changed)
                {
                    Table now; readTable(c, now);
                    int total = 0; for (Table::const_iterator it = now.begin(); it != now.end(); ++it) total += it->second.amount;
                    log("bounty: %s's character %08x, owner's table applied: %u faction(s), total %d here", playerName(e.sender).c_str(), id, (unsigned)t.size(), total);
                }
            }
    }
    else if (e.msgType == MSG_BOUNTY_CRIME)
    {
        uint8_t target = r.u8();
        uint16_t n = r.u16();
        if (!r.ok() || target != g_session.localId() || e.sender != HOST_ID || n > MAX_ENTRIES) return;
        for (uint16_t i = 0; i < n && r.ok(); ++i)
        {
            uint32_t id = r.u32(); std::string sid = r.str(); int32_t amount = (int32_t)r.u32(); uint32_t crimes = r.u32();
            if (!r.ok() || netIdOwner(id) != g_session.localId() || amount <= 0 || amount > 1000000) continue;
            Character* c = chars_byNetId(id);
            Faction* f = factionBySid(sid);
            if (!c || chars_isGhost(c) || !f) continue;
            if (safeAdd(c, f, amount))
            {
                safeSetCrimes(c, f, crimes & 0xFFFFF, true);
                std::string who = c->getName(), fname = f->getName();
                log("bounty: %s now wanted by %s (+%d, seen in the host's world)", who.c_str(), sid.c_str(), amount);
                chat_notice(TF("%s: bounty from %s (+%d), seen in the host's world.", who.c_str(), fname.c_str(), amount));
            }
        }
        g_forceSend = true;   // our new table goes out at once
    }
}

void bounties_resendAll() { g_sentHash.clear(); g_forceSend = true; }

void bounties_onPlayerLeft(uint8_t id)
{
    for (std::map<uint32_t, Table>::iterator it = g_known.begin(); it != g_known.end();)
        if (netIdOwner(it->first) == id) g_known.erase(it++); else ++it;
}

void bounties_onWorldReload()
{
    g_sentHash.clear(); g_known.clear(); g_forceSend = true;
}

// Test (autotest): a world faction that enforces laws (bounties from lawless factions are ignored
// by the game), base game first.
std::string bounties_debugFaction()
{
    if (!ou || !ou->factionMgr) return "";
    const lektor<Faction*>* all = ou->factionMgr->getAllFactions();
    if (!all) return "";
    // name parts, English and French (the game's language may be either)
    const char* lawful[] = { "United Cities", "Unies", "Holy Nation", "Sainte", "Shek Kingdom", "Royaume Shek" };
    for (int k = 0; k < 6; ++k)
        for (uint32_t i = 0; i < all->size(); ++i)
        {
            Faction* f = (*all)[i];
            if (f && f->getData() && !f->isThePlayer() && f->getName().find(lawful[k]) != std::string::npos
                && f->getData()->stringID.find("gamedata.base") != std::string::npos)
                return f->getData()->stringID;
        }
    // Else the first faction the game keeps a bounty for, tried on our own first character
    // (test games only) and cleared at once.
    std::vector<std::pair<uint32_t, Character*> > mine;
    chars_localCharacters(mine);
    if (mine.empty() && ou->player && ou->player->playerCharacters.size()) mine.push_back(std::make_pair(0u, ou->player->playerCharacters[0]));
    if (mine.empty()) return "";
    for (uint32_t i = 0; i < all->size() && i < 300; ++i)
    {
        Faction* f = (*all)[i];
        if (!f || !f->getData() || f->isThePlayer()) continue;
        if (!safeAdd(mine[0].second, f, 1)) continue;
        Table t; readTable(mine[0].second, t);
        bool kept = t.count(f->getData()->stringID) != 0;
        safeClear(mine[0].second, f);
        if (kept) return f->getData()->stringID;
    }
    return "";
}

// Test (autotest, host): our NPCs "saw" the first other player's character commit a crime.
std::string bounties_debugCrimeOnGhost(const std::string& factionSid)
{
    std::vector<std::pair<uint32_t, Character*> > ghosts;
    chars_playerGhosts(ghosts);
    if (ghosts.empty() || !ou || !ou->factionMgr) return "no ghost";
    // The named faction first, else the first faction for which the game keeps the bounty
    // (factions without laws ignore it).
    std::vector<Faction*> tries;
    if (Faction* f = factionBySid(factionSid)) tries.push_back(f);
    const lektor<Faction*>* all = ou->factionMgr->getAllFactions();
    for (uint32_t i = 0; all && i < all->size() && tries.size() < 200; ++i)
        if ((*all)[i] && !(*all)[i]->isThePlayer() && (*all)[i]->getData()) tries.push_back((*all)[i]);
    for (size_t k = 0; k < tries.size(); ++k)
    {
        Faction* f = tries[k];
        if (!safeAdd(ghosts[0].second, f, 500)) continue;
        Table now; readTable(ghosts[0].second, now);
        Table::const_iterator it = now.find(f->getData()->stringID);
        if (it == now.end() || it->second.amount <= 0) continue;
        char buf[160]; sprintf_s(buf, "+500 on ghost %08x for %s (%s), its bounty there is now %d", ghosts[0].first, f->getName().c_str(), f->getData()->stringID.c_str(), it->second.amount);
        return buf;
    }
    return "no faction kept the bounty";
}

} // namespace kmp
