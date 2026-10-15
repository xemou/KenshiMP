// Dedicated server: what it keeps per account between sessions (DEDICATED_SERVER_PLAN.md, phase 3): the player's
// characters as last seen (name, race, position, inventory and its hash) and squad money. One file per account in
// <dataDir>\players\, written atomically with the previous versions kept (<file>.1 .. .3) to roll back a bad state.
#pragma once
#include "Protocol.h"
#include "FileUtil.h"
#include <time.h>

namespace mp {

struct PlayerRecord
{
    struct Char
    {
        uint32_t netId;
        std::string name, race;
        float x, y, z;
        uint32_t bagHash;
        std::vector<InvItem> items;
        Char() : netId(0), x(0), y(0), z(0), bagHash(0) {}
    };
    std::string name;
    uint32_t money;
    uint32_t lastSeen;     // unix time
    std::vector<Char> chars;
    PlayerRecord() : money(0), lastSeen(0) {}

    void write(ByteWriter& w) const
    {
        w.u32(0x31524D4B);   // "KMR1"
        w.str(name); w.u32(money); w.u32(lastSeen);
        w.u16((uint16_t)chars.size());
        for (size_t i = 0; i < chars.size(); ++i)
        {
            const Char& c = chars[i];
            w.u32(c.netId); w.str(c.name); w.str(c.race); w.f32(c.x); w.f32(c.y); w.f32(c.z); w.u32(c.bagHash);
            w.u16((uint16_t)c.items.size()); for (size_t k = 0; k < c.items.size(); ++k) c.items[k].write(w);
        }
    }
    bool read(ByteReader& r)
    {
        if (r.u32() != 0x31524D4B) return false;
        name = r.str(); money = r.u32(); lastSeen = r.u32();
        uint16_t n = r.u16(); chars.clear();
        for (int i = 0; i < n && r.ok(); ++i)
        {
            Char c; c.netId = r.u32(); c.name = r.str(); c.race = r.str(); c.x = r.f32(); c.y = r.f32(); c.z = r.f32(); c.bagHash = r.u32();
            uint16_t m = r.u16();
            for (int k = 0; k < m && k < MAX_INV_ITEMS && r.ok(); ++k) { InvItem it; it.read(r); if (it.sanitize()) c.items.push_back(it); }
            chars.push_back(c);
        }
        return r.ok();
    }
};

// Order-independent hash of an inventory (the client computes the same over its real items).
inline uint32_t bagHash(const std::vector<InvItem>& items)
{
    uint32_t sum = 0, x = 0;
    for (size_t i = 0; i < items.size(); ++i)
    {
        ByteWriter w; w.str(items[i].item); w.str(items[i].manufacturer); w.str(items[i].material); w.u32((uint32_t)items[i].quantity);
        uint32_t h = hashBytes(w.data);
        sum += h; x ^= h * 2654435761u;
    }
    return (sum ^ (x << 1) ^ (uint32_t)items.size()) | 1u;   // never 0 (0 = unknown)
}

class PlayerRecordStore
{
public:
    explicit PlayerRecordStore(const std::string& dataDir = std::string()) { setDir(dataDir); }
    void setDir(const std::string& dataDir) { dir_ = dataDir.empty() ? std::string() : dataDir + "\\players"; if (!dir_.empty()) makeDirs(dir_); }
    bool save(PlayerRecord& r)
    {
        if (dir_.empty() || r.name.empty()) return false;
        if (!r.lastSeen) r.lastSeen = (uint32_t)time(NULL);
        ByteWriter w; r.write(w);
        return writeFileAtomic(path(r.name), std::string(w.data.begin(), w.data.end()), 3);
    }
    bool load(const std::string& name, PlayerRecord& r) const { return loadFile(path(name), r); }
    bool loadBackup(const std::string& name, int n, PlayerRecord& r) const { char s[8]; sprintf_s(s, ".%d", n); return loadFile(path(name) + s, r); }
    bool remove(const std::string& name) const { return DeleteFileA(path(name).c_str()) != 0; }
private:
    std::string path(const std::string& name) const { return dir_ + "\\" + safeFileName(name) + ".rec"; }
    static bool loadFile(const std::string& p, PlayerRecord& r)
    {
        std::string text;
        if (!readFile(p, text) || text.empty()) return false;
        ByteReader rd((const uint8_t*)text.data(), text.size());
        return r.read(rd);
    }
    std::string dir_;
};

} // namespace mp
