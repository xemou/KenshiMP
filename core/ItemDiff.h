// Content of a container as (item kind -> total quantity) and the difference between two contents.
// Used to detect item transfers made through the game's inventory windows (Items.cpp), whatever
// engine function the window used.
#pragma once
#include "Protocol.h"
#include <map>
#include <string>
#include <vector>

namespace mp {

inline std::string itemKindKey(const InvItem& d) { return d.item + "|" + d.manufacturer + "|" + d.material; }

struct ItemContent
{
    std::map<std::string, int> qty;
    std::map<std::string, InvItem> desc;   // one description per kind (quality of the last one seen)
    void add(const InvItem& d)
    {
        std::string k = itemKindKey(d);
        qty[k] += d.quantity;
        desc[k] = d;
    }
    bool operator==(const ItemContent& o) const { return qty == o.qty; }
    bool operator!=(const ItemContent& o) const { return qty != o.qty; }
};

// What left (`taken`) and what arrived (`given`) between two contents, with quantities.
inline void diffContent(const ItemContent& before, const ItemContent& now, std::vector<InvItem>& taken, std::vector<InvItem>& given)
{
    for (std::map<std::string, int>::const_iterator it = before.qty.begin(); it != before.qty.end(); ++it)
    {
        std::map<std::string, int>::const_iterator n = now.qty.find(it->first);
        int gone = it->second - (n == now.qty.end() ? 0 : n->second);
        if (gone <= 0) continue;
        InvItem d = before.desc.find(it->first)->second; d.quantity = gone;
        taken.push_back(d);
    }
    for (std::map<std::string, int>::const_iterator it = now.qty.begin(); it != now.qty.end(); ++it)
    {
        std::map<std::string, int>::const_iterator b = before.qty.find(it->first);
        int added = it->second - (b == before.qty.end() ? 0 : b->second);
        if (added <= 0) continue;
        InvItem d = now.desc.find(it->first)->second; d.quantity = added;
        given.push_back(d);
    }
}

} // namespace mp
