// Who simulates what: the single owner of every network entity (AUTHORITY_PLAN.md, steps B and C).
// Default owner: the player a character's id was made by; world NPCs belong to the host. Only the exceptions
// (an NPC handed to a client, ...) are stored. The host is the only writer; clients keep a copy of its table.
#pragma once
#include "Protocol.h"
#include "Buffer.h"
#include <map>

namespace mp {

enum HandoffAction { HANDOFF_GRANT = 0, HANDOFF_REFUSE = 1, HANDOFF_GONE = 2 };   // host -> claimer, answer to MSG_NPC_CLAIM

inline uint8_t defaultOwner(uint32_t id) { return isNpcNetId(id) ? HOST_ID : netIdOwner(id); }

struct AuthEntry
{
    uint8_t owner;
    uint16_t epoch;       // grows with every change of owner: a stale update can never win
    AuthEntry() : owner(0), epoch(0) {}
};

class AuthTable
{
public:
    uint8_t owner(uint32_t id) const
    {
        std::map<uint32_t, AuthEntry>::const_iterator it = m_.find(id);
        return it == m_.end() ? defaultOwner(id) : it->second.owner;
    }
    uint16_t epoch(uint32_t id) const
    {
        std::map<uint32_t, AuthEntry>::const_iterator it = m_.find(id);
        return it == m_.end() ? 0 : it->second.epoch;
    }
    // Host: give the entity to `owner` (a new epoch). Returns it.
    uint16_t assign(uint32_t id, uint8_t owner)
    {
        AuthEntry& e = m_[id];
        e.owner = owner;
        e.epoch = (uint16_t)(e.epoch + 1);
        if (e.owner == defaultOwner(id)) { uint16_t ep = e.epoch; m_.erase(id); return ep; }
        return e.epoch;
    }
    void forget(uint32_t id) { m_.erase(id); }
    // The player left: what it owned is no longer anybody's exception (its NPCs are gone with it).
    int dropOwner(uint8_t player)
    {
        int n = 0;
        for (std::map<uint32_t, AuthEntry>::iterator it = m_.begin(); it != m_.end();)
            if (it->second.owner == player) { m_.erase(it++); ++n; } else ++it;
        return n;
    }
    size_t size() const { return m_.size(); }
    void clear() { m_.clear(); }
    // Whole table (the exceptions): u16 n, {u32 id, u8 owner, u16 epoch}*. The receiver replaces its own.
    void write(ByteWriter& w) const
    {
        w.u16((uint16_t)(m_.size() > 4000 ? 4000 : m_.size()));
        uint16_t k = 0;
        for (std::map<uint32_t, AuthEntry>::const_iterator it = m_.begin(); it != m_.end() && k < 4000; ++it, ++k)
        { w.u32(it->first); w.u8(it->second.owner); w.u16(it->second.epoch); }
    }
    bool read(ByteReader& r)
    {
        uint16_t n = r.u16();
        std::map<uint32_t, AuthEntry> fresh;
        for (uint16_t i = 0; i < n && r.ok(); ++i)
        {
            uint32_t id = r.u32(); AuthEntry e; e.owner = r.u8(); e.epoch = r.u16();
            if (r.ok()) fresh[id] = e;
        }
        if (!r.ok()) return false;
        m_.swap(fresh);
        return true;
    }
    bool operator==(const AuthTable& o) const
    {
        if (m_.size() != o.m_.size()) return false;
        for (std::map<uint32_t, AuthEntry>::const_iterator a = m_.begin(), b = o.m_.begin(); a != m_.end(); ++a, ++b)
            if (a->first != b->first || a->second.owner != b->second.owner || a->second.epoch != b->second.epoch) return false;
        return true;
    }
private:
    std::map<uint32_t, AuthEntry> m_;
};

} // namespace mp
