// Items: inventories of replicated containers and transfers between players.
//
// Mirror   : the owner of a container (a player's character or storage building, or the host for
//            world NPCs such as traders) sends its content (MSG_INVENTORY) whenever it changes.
//            Ghosts show that content; equipped gear comes from MSG_EQUIPMENT as before.
// Transfer : when WE take an item from a ghost's inventory, or put one into it (looting a body,
//            trading, filling another player's chest), the change is forwarded to the owner
//            (MSG_ITEM_TAKE / MSG_ITEM_GIVE), who validates it (item present, taker standing
//            next to the container) and applies it to the real container. The owner's next
//            MSG_INVENTORY then corrects every copy. Items therefore exist once: a looted item
//            disappears from the owner's world instead of being duplicated.
// Detection does not depend on which engine function the inventory window uses (drag, right-click
// auto-trade, trade screen...): each remote container keeps a snapshot of its content; while its
// window is open on our screen, any difference is a transfer made by the player and is forwarded.
// Our own changes (mirror, re-dress, despawn) only refresh the snapshot.
// Town containers (chests, shop storage... of the host's world, CONTAINER_WORLD): a client in the
// host's world has its own copy of them from its save. When it opens one, it tells the host
// (MSG_WORLD_CONTAINER), its copy is emptied and then mirrors the host's content for as long as
// the window stays open; what the player takes or puts goes to the host like any transfer. So a
// town chest holds the same things for everybody and an item looted by one player is gone for all.
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/Character.h>
#include <kenshi/CharStats.h>
#include <kenshi/GameData.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Inventory.h>
#include <kenshi/Platoon.h>
#include <kenshi/Item.h>
#include <kenshi/Building/Building.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Enums.h>
#include <kenshi/util/hand.h>
#include <core/Functions.h>

#include "Shared.h"
#include "../core/ItemDiff.h"
#include "CharSync.h"

#include <algorithm>
#include <map>
#include <math.h>
#include <stdio.h>
#include <set>
#include <vector>

using namespace mp;

namespace kmp {

volatile long g_itemsMute = 0;
volatile long g_itemsEpoch = 0;

namespace
{
    const float TRANSFER_RANGE = 45.f;        // taker must have a character this close (as seen by the owner, so with some lag)
    const DWORD PENDING_TTL_MS = 30000;       // inventories of ghosts not spawned yet wait this long

    std::string sidOf(GameData* d) { return d ? d->stringID : std::string(); }
    GameData* dataOf(const std::string& sid) { return sid.empty() ? NULL : ou->gamedata.getData(sid); }

    // ------------------------------------------------------------------ SEH-guarded engine calls
    Item* safeCreate(GameData* gd, GameData* man, GameData* mat, const hand* h)
    {
        __try { return ou->theFactory->createItem(gd, *h, man, mat, -1, NULL); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    bool safeAdd(Inventory* inv, Item* it, int qty)
    {
        __try { return inv->addItem(it, qty, false, true); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeRemove(Inventory* inv, Item* it, int qty)
    {
        __try { return inv->removeItemAutoDestroy(it, qty); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeVisible(Inventory* inv)
    {
        __try { return inv->isVisible(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Squad money of a character (NPC traders pay and get paid from their squad).
    bool safeGetMoney(Character* c, int* out)
    {
        __try
        {
            Ownerships* o = c->getOwnerships();
            if (!o) return false;
            *out = o->getMoney();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeSetMoney(Character* c, int value)
    {
        __try
        {
            Ownerships* o = c->getOwnerships();
            if (!o) return false;
            o->setMoney(value < 0 ? 0 : value);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    const char* MONEY_ITEM = "$money";   // pseudo item of MSG_ITEM_TAKE/GIVE: squad money

    // What the player reads: the item's name from the game data (localised by the game), not its id.
    std::string itemName(const std::string& sid)
    {
        GameData* d = dataOf(sid);
        return d && !d->name.empty() ? d->name : sid;
    }

    InvItem describe(Item* it)
    {
        InvItem r;
        r.item = sidOf(it->data);
        r.manufacturer = sidOf(it->manufacturerData);
        r.material = sidOf(it->materialData);
        r.quantity = it->quantity > 0 ? it->quantity : 1;
        r.quality = it->quality;
        return r;
    }

    Item* create(const InvItem& d)
    {
        GameData* gd = dataOf(d.item);
        if (!gd) return NULL;
        GameData* man = dataOf(d.manufacturer);
        GameData* mat = dataOf(d.material);
        hand none;
        Item* it = NULL;
        // Weapons take (manufacturer, weapon) instead of (weapon, manufacturer) (see applyEquipment).
        if (gd->type == WEAPON && man) it = safeCreate(man, gd, mat, &none);
        if (!it) it = safeCreate(gd, man, mat, &none);
        if (it) { it->quantity = d.quantity; it->quality = d.quality; }
        return it;
    }

    // Gear never stacks in Kenshi: a transfer of "2 x sword" (two stacks added up by the diff) is
    // two items of quantity 1, not one item of quantity 2.
    bool stacks(const InvItem& d)
    {
        GameData* gd = dataOf(d.item);
        if (!gd) return true;
        switch (gd->type)
        {
        case WEAPON: case ARMOUR: case CROSSBOW: case CONTAINER: case LIMB_REPLACEMENT: case BLUEPRINT: case MAP_ITEM:
            return false;
        default:
            return true;
        }
    }

    // ------------------------------------------------------------------ containers
    // A worn backpack is an item with its own inventory (not part of the wearer's).
    Inventory* safeBackpack(Character* c)
    {
        __try { ContainerItem* b = c->hasABackpackOn(); return b ? b->inventory : NULL; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    Item* safeWornBag(Character* c)
    {
        __try { return c->hasABackpackOn(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    // A character's worn bag is not in its inventory's lists: it is part of what can be looted.
    Item* wornBagOf(uint8_t kind, uint32_t id)
    {
        if (kind != CONTAINER_CHARACTER) return NULL;
        Character* c = chars_byNetId(id);
        return c ? safeWornBag(c) : NULL;
    }
    // ------------------------------------------------------------------ town containers
    struct WorldBox
    {
        std::string sid; float x, y, z;   // the building (furniture) as both worlds know it
        hand h;                           // our copy, once found
        std::set<uint8_t> watchers;       // host: clients with its window open
        uint32_t sentHash;                // host: content last sent to them
        bool open;                        // client: its window is open on our screen
        WorldBox() : x(0), y(0), z(0), sentHash(0), open(false) {}
    };
    std::map<uint32_t, WorldBox> g_boxes;
    const size_t MAX_BOXES = 2048;

    // A container of the world (not a player's building, not a ghost), and its inventory.
    Inventory* safeBoxInventory(Building* b, Faction* mine)
    {
        __try
        {
            if (!b->data || (mine && b->owner == mine)) return NULL;
            return b->getInventory();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    Inventory* worldInventory(Building* b)
    {
        if (!b || builds_netIdOf(b)) return NULL;
        return safeBoxInventory(b, ou && ou->player ? ou->player->getFaction() : NULL);
    }
    Building* boxBuilding(WorldBox& w)
    {
        Building* b = w.h.getBuilding();
        if (!b && !w.sid.empty())
        {
            b = builds_findWorld(w.sid, w.x, w.y, w.z);
            if (b) w.h = hand(b);
        }
        return b;
    }

    Inventory* inventoryOf(uint8_t kind, uint32_t id)
    {
        if (kind == CONTAINER_WORLD)
        {
            std::map<uint32_t, WorldBox>::iterator w = g_boxes.find(id);
            return w == g_boxes.end() ? NULL : worldInventory(boxBuilding(w->second));
        }
        if (kind == CONTAINER_BACKPACK) { Character* c = chars_byNetId(id); return c ? safeBackpack(c) : NULL; }
        if (kind == CONTAINER_CHARACTER) { Character* c = chars_byNetId(id); return c ? c->inventory : NULL; }
        if (kind == CONTAINER_BUILDING) { Building* b = builds_byNetId(id); return b ? b->getInventory() : NULL; }
        return NULL;
    }
    bool positionOf(uint8_t kind, uint32_t id, Ogre::Vector3& out)
    {
        if (kind == CONTAINER_CHARACTER || kind == CONTAINER_BACKPACK) { Character* c = chars_byNetId(id); if (c) { out = c->getPosition(); return true; } }
        if (kind == CONTAINER_BUILDING) { Building* b = builds_byNetId(id); if (b) { out = b->getPosition(); return true; } }
        if (kind == CONTAINER_WORLD)
        {
            std::map<uint32_t, WorldBox>::iterator w = g_boxes.find(id);
            if (w != g_boxes.end()) { out = Ogre::Vector3(w->second.x, w->second.y, w->second.z); return true; }
        }
        return false;
    }
    void forward(uint8_t type, uint8_t kind, uint32_t id, const InvItem& d)
    {
        ByteWriter w; w.u8(netIdOwner(id)); w.u8(kind); w.u32(id); d.write(w);
        g_session.send(type, w.data);
    }

    // ------------------------------------------------------------------ ghost side: apply mirror
    struct Pending { uint8_t kind; uint32_t id; std::vector<InvItem> items; DWORD since; int money; Pending() : money(-1) {} };
    std::map<uint64_t, Pending> g_pending;
    std::map<uint64_t, uint32_t> g_appliedHash;          // skip identical re-sends
    uint64_t key(uint8_t kind, uint32_t id) { return ((uint64_t)kind << 32) | id; }

    // Items the mirror created in each container: the engine may drop one into a free equipment
    // slot, so they are tracked (and removed on the next update) wherever they ended up.
    std::map<uint64_t, std::vector<hand> > g_created;
    std::map<uint64_t, Pending> g_lastMirror;   // last content applied, re-applied after a re-dress

    bool safeSnapshot(Inventory* inv, std::vector<Item*>* out);

    // Returns false while the container does not exist here yet.
    bool applyMirror(const Pending& p)
    {
        Inventory* inv = inventoryOf(p.kind, p.id);
        if (!inv) return false;
        std::vector<hand>& created = g_created[key(p.kind, p.id)];
        ++g_itemsMute; ++g_itemsEpoch;
        std::vector<Item*> old;
        const lektor<Item*>& all = inv->getAllItems();
        for (uint32_t i = 0; i < all.size(); ++i) if (all[i] && !all[i]->isEquipped) old.push_back(all[i]);   // spawn leftovers
        // Items we created earlier, wherever the engine put them in THIS inventory (a free equipment
        // slot...). One the player has looted meanwhile is in his own inventory now: leave it there.
        std::vector<Item*> here;
        safeSnapshot(inv, &here);
        for (size_t i = 0; i < created.size(); ++i)
            if (Item* it = created[i].getItem())
                if (std::find(here.begin(), here.end(), it) != here.end() && std::find(old.begin(), old.end(), it) == old.end()) old.push_back(it);
        for (size_t i = 0; i < old.size(); ++i) safeRemove(inv, old[i], old[i]->quantity > 0 ? old[i]->quantity : 1);
        created.clear();
        int failed = 0;
        for (size_t k = 0; k < p.items.size(); ++k)
        {
            Item* it = create(p.items[k]);
            hand h;
            if (it) h = hand(it);
            if (!it || !safeAdd(inv, it, p.items[k].quantity)) ++failed;
            else created.push_back(h);
        }
        --g_itemsMute;
        {
            static std::map<uint64_t, bool> logged;
            if (!logged[key(p.kind, p.id)]) { logged[key(p.kind, p.id)] = true; log("%s of %08x mirrored: %d item(s), %d failed", p.kind == CONTAINER_BACKPACK ? "backpack" : "inventory", p.id, (int)p.items.size(), failed); }
        }
        if (failed)
        {
            static DWORD lastWarn = 0;
            if (GetTickCount() - lastWarn > 10000) { lastWarn = GetTickCount(); log("inventory %08x: %d item(s) could not be mirrored (unknown item or no room)", p.id, failed); }
        }
        return true;
    }

    // ------------------------------------------------------------------ owner side: validate transfers
    // A transfer we cannot apply has already happened on the requester's screen (the item is in
    // its character's hands, or gone from them): tell it to undo that, or the item would be
    // duplicated (refused take) or lost (refused give).
    void sendUndo(uint8_t to, uint8_t action, uint8_t kind, uint32_t id, const InvItem& d)
    {
        if (to == g_session.localId()) return;
        ByteWriter w; w.u8(to); w.u8(action); w.u8(kind); w.u32(id); d.write(w);
        g_session.send(MSG_ITEM_UNDO, w.data);
        log("transfer of %d x %s by %s undone (%s)", d.quantity, d.item.c_str(), playerName(to).c_str(), action == UNDO_REMOVE ? "take" : "give");
    }
    bool takerIsNear(uint8_t sender, uint8_t kind, uint32_t id)
    {
        Ogre::Vector3 c;
        if (!positionOf(kind, id, c)) return false;
        std::vector<Ogre::Vector3> ghosts;
        cs_ghostPositions(sender, ghosts);
        for (size_t i = 0; i < ghosts.size(); ++i) if (ghosts[i].distance(c) <= TRANSFER_RANGE) return true;
        return false;
    }

    // A trade with one of our NPCs (host) moved money on the trader's side.
    // Money the trader did not pay (delta < 0: the player got it) or did not get (delta > 0: the
    // player paid it): the player's side is put back.
    void undoMoney(uint8_t to, uint32_t id, int delta)
    {
        InvItem m; m.item = MONEY_ITEM; m.quantity = delta < 0 ? -delta : delta;
        if (m.quantity > 0) sendUndo(to, delta < 0 ? UNDO_REMOVE : UNDO_GIVE_BACK, CONTAINER_CHARACTER, id, m);
    }

    void applyMoney(uint8_t sender, uint32_t id, int delta)
    {
        Character* c = chars_byNetId(id);
        int have = 0;
        if (!c || !safeGetMoney(c, &have)) { undoMoney(sender, id, delta); return; }
        if (!takerIsNear(sender, CONTAINER_CHARACTER, id))
        {
            log("refused money change by %s on %08x (not next to it)", playerName(sender).c_str(), id);
            undoMoney(sender, id, delta);
            return;
        }
        int now = have + delta;
        if (now < 0) { undoMoney(sender, id, now); now = 0; }   // it paid more than it had
        safeSetMoney(c, now);
        log("trade with %s: %08x money %d -> %d", playerName(sender).c_str(), id, have, now);
    }

    void applyTake(uint8_t sender, uint8_t kind, uint32_t id, const InvItem& d)
    {
        if (d.item == MONEY_ITEM) { if (kind == CONTAINER_CHARACTER) applyMoney(sender, id, -d.quantity); return; }
        Inventory* inv = inventoryOf(kind, id);
        if (!inv) { sendUndo(sender, UNDO_REMOVE, kind, id, d); return; }
        if (!takerIsNear(sender, kind, id))
        {
            log("refused item take by %s from %08x (not next to it)", playerName(sender).c_str(), id);
            sendUndo(sender, UNDO_REMOVE, kind, id, d);
            return;
        }
        // The amount may span several stacks (two identical swords, food in three piles): take from
        // loose stacks first, then worn gear (looting a knocked-out body), then the worn bag.
        std::vector<Item*> all;
        if (!safeSnapshot(inv, &all)) return;
        if (Item* bag = wornBagOf(kind, id)) all.push_back(bag);
        std::vector<Item*> loose, worn;
        for (size_t i = 0; i < all.size(); ++i)
        {
            Item* it = all[i];
            if (!it || !describe(it).sameKind(d)) continue;
            if (std::find(loose.begin(), loose.end(), it) != loose.end() || std::find(worn.begin(), worn.end(), it) != worn.end()) continue;
            (it->isEquipped ? worn : loose).push_back(it);
        }
        loose.insert(loose.end(), worn.begin(), worn.end());
        if (loose.empty())
        {
            log("item take by %s: '%s' no longer in %08x", playerName(sender).c_str(), d.item.c_str(), id);
            sendUndo(sender, UNDO_REMOVE, kind, id, d);   // someone else was faster
            return;
        }
        int left = d.quantity > 0 ? d.quantity : 1, n = 0;
        for (size_t i = 0; i < loose.size() && left > 0; ++i)
        {
            int have = loose[i]->quantity > 0 ? loose[i]->quantity : 1;
            int part = left < have ? left : have;
            if (safeRemove(inv, loose[i], part)) { left -= part; n += part; }
        }
        if (left > 0)
        {
            log("item take by %s: only %d of %d x %s were left in %08x", playerName(sender).c_str(), n, d.quantity, d.item.c_str(), id);
            InvItem rest = d; rest.quantity = left;
            sendUndo(sender, UNDO_REMOVE, kind, id, rest);
        }
        log("%s took %d x %s from %08x", playerName(sender).c_str(), n, d.item.c_str(), id);
        if ((kind == CONTAINER_CHARACTER || kind == CONTAINER_BACKPACK) && netIdOwner(id) == g_session.localId() && !isNpcNetId(id))
            showMessage(TF("%s took %s from your squad.", playerName(sender).c_str(), itemName(d.item).c_str()));
    }

    void applyGive(uint8_t sender, uint8_t kind, uint32_t id, const InvItem& d)
    {
        if (d.item == MONEY_ITEM) { if (kind == CONTAINER_CHARACTER) applyMoney(sender, id, d.quantity); return; }
        Inventory* inv = inventoryOf(kind, id);
        if (!inv) { sendUndo(sender, UNDO_GIVE_BACK, kind, id, d); return; }   // gone (despawned, destroyed)
        if (!takerIsNear(sender, kind, id))
        {
            log("refused item give by %s to %08x (not next to it)", playerName(sender).c_str(), id);
            sendUndo(sender, UNDO_GIVE_BACK, kind, id, d);
            return;
        }
        InvItem one = d;
        int count = 1;
        if (!stacks(d) && d.quantity > 1) { count = d.quantity; one.quantity = 1; }
        int added = 0;
        for (int i = 0; i < count; ++i)
        {
            Item* it = create(one);
            if (it && safeAdd(inv, it, one.quantity)) added += one.quantity;
        }
        if (added < d.quantity)
        {
            log("item give by %s: could not add %d x '%s'", playerName(sender).c_str(), d.quantity - added, d.item.c_str());
            InvItem rest = d; rest.quantity = d.quantity - added;
            sendUndo(sender, UNDO_GIVE_BACK, kind, id, rest);
        }
        else log("%s gave %d x %s to %08x (%d item(s) created)", playerName(sender).c_str(), d.quantity, d.item.c_str(), id, count);
    }

    // ------------------------------------------------------------------ transfer detection
    // Content of a container as (item kind -> total quantity), loose items and worn gear.
    typedef ItemContent Content;   // core/ItemDiff.h

    // (SEH functions cannot own objects with destructors: the lists belong to the caller.)
    bool safeLists(Inventory* inv, const lektor<Item*>** loose, lektor<Item*>* weapons, lektor<Item*>* armour)
    {
        __try
        {
            *loose = &inv->getAllItems();
            inv->getEquippedWeapons(*weapons);
            inv->getEquippedArmour(*armour);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeSnapshot(Inventory* inv, std::vector<Item*>* out)
    {
        const lektor<Item*>* loose = NULL;
        lektor<Item*> weapons, armour;
        if (!safeLists(inv, &loose, &weapons, &armour)) return false;
        for (uint32_t i = 0; i < loose->size(); ++i) out->push_back((*loose)[i]);
        for (uint32_t i = 0; i < weapons.size(); ++i) out->push_back(weapons[i]);
        for (uint32_t i = 0; i < armour.size(); ++i) out->push_back(armour[i]);
        return true;
    }
    bool snapshot(Inventory* inv, Content& c, Item* extra = NULL)
    {
        std::vector<Item*> items;
        if (!safeSnapshot(inv, &items)) return false;
        if (extra) items.push_back(extra);
        std::set<Item*> seen;
        for (size_t i = 0; i < items.size(); ++i)
        {
            Item* it = items[i];
            if (!it || !seen.insert(it).second) continue;
            InvItem d = describe(it);
            if (!d.sanitize()) continue;
            c.add(d);
        }
        return true;
    }

    std::map<uint64_t, Content> g_baseline;    // remote containers we know (main thread)

    void forwardDiff(uint8_t kind, uint32_t id, const Content& before, const Content& now)
    {
        std::vector<InvItem> taken, given;
        diffContent(before, now, taken, given);
        for (size_t i = 0; i < taken.size(); ++i)
        {
            forward(MSG_ITEM_TAKE, kind, id, taken[i]);
            log("took %d x %s from %08x (sent to its owner)", taken[i].quantity, taken[i].item.c_str(), id);
        }
        for (size_t i = 0; i < given.size(); ++i)
        {
            forward(MSG_ITEM_GIVE, kind, id, given[i]);
            log("put %d x %s into %08x (sent to its owner)", given[i].quantity, given[i].item.c_str(), id);
        }
    }

    // Remote NPCs' money (their squads here are shared per faction, so the trader's own amount is put
    // in place when its window opens, and changes during the trade are sent to the host).
    std::map<uint64_t, int> g_announcedMoney;   // from MSG_INVENTORY
    std::map<uint64_t, int> g_moneyBase;         // while its window is open
    void detectMoney(uint64_t k, uint32_t id, bool visible)
    {
        std::map<uint64_t, int>::iterator ann = g_announcedMoney.find(k);
        if (ann == g_announcedMoney.end()) return;
        Character* c = chars_byNetId(id);
        if (!c) return;
        std::map<uint64_t, int>::iterator base = g_moneyBase.find(k);
        if (!visible) { if (base != g_moneyBase.end()) g_moneyBase.erase(base); return; }
        if (base == g_moneyBase.end())
        {
            if (safeSetMoney(c, ann->second)) g_moneyBase[k] = ann->second;   // window just opened
            return;
        }
        int now = 0;
        if (!safeGetMoney(c, &now) || now == base->second) return;
        int delta = now - base->second;
        InvItem d; d.item = MONEY_ITEM; d.quantity = delta > 0 ? delta : -delta;
        forward(delta > 0 ? MSG_ITEM_GIVE : MSG_ITEM_TAKE, CONTAINER_CHARACTER, id, d);
        log("trade: %08x money %+d (sent to the host)", id, delta);
        base->second = now;
        ann->second = now;
    }

    // Every tick: compare each OPEN remote container with its snapshot. Closed ones are not read
    // at all (hundreds of host NPCs on a client): when a window opens, its content at that moment
    // becomes the reference, so changes made while it was closed are never taken for transfers.
    std::set<uint64_t> g_open;   // containers whose window was open at the last check
    void detectTransfers()
    {
        // Our own changes (mirror, re-dress) re-take their container's reference right away
        // (rememberContainer), so only a change still in progress needs muting: a mirror elsewhere
        // must not swallow the player's transfer.
        bool ownChange = g_itemsMute != 0;
        bool live = ready() && g_session.active();
        for (std::map<uint64_t, Content>::iterator it = g_baseline.begin(); it != g_baseline.end();)
        {
            uint8_t kind = (uint8_t)(it->first >> 32);
            uint32_t id = (uint32_t)it->first;
            bool npc = kind == CONTAINER_CHARACTER && isNpcNetId(id);
            Inventory* inv = inventoryOf(kind, id);
            if (!inv) { g_open.erase(it->first); g_moneyBase.erase(it->first); g_baseline.erase(it++); continue; }   // despawned (a new mirror re-creates it)
            bool visible = safeVisible(inv);
            if (kind == CONTAINER_BACKPACK && !visible)   // shown beside its wearer's inventory
                if (Inventory* wearer = inventoryOf(CONTAINER_CHARACTER, id)) visible = safeVisible(wearer);
            bool wasOpen = g_open.count(it->first) != 0;
            if (!visible)
            {
                if (wasOpen) { g_open.erase(it->first); if (npc && live) detectMoney(it->first, id, false); }
                ++it; continue;
            }
            Content now;
            if (!snapshot(inv, now, wornBagOf(kind, id))) { ++it; continue; }
            if (!wasOpen) g_open.insert(it->first);   // just opened: reference only
            else if (!ownChange && now != it->second && live) forwardDiff(kind, id, it->second, now);
            if (npc && live) detectMoney(it->first, id, true);
            it->second = now;
            ++it;
        }
    }
    // An open container is about to be changed by us (mirror, re-dress): first forward whatever the
    // player did since the last check, or the change would erase it.
    void flushOpen(uint8_t kind, uint32_t id)
    {
        uint64_t k = key(kind, id);
        if (!g_open.count(k) || g_itemsMute || !ready() || !g_session.active()) return;
        std::map<uint64_t, Content>::iterator b = g_baseline.find(k);
        Inventory* inv = inventoryOf(kind, id);
        Content now;
        if (b == g_baseline.end() || !inv || !snapshot(inv, now, wornBagOf(kind, id))) return;
        if (now != b->second) forwardDiff(kind, id, b->second, now);
        b->second = now;
    }
    void rememberContainer(uint8_t kind, uint32_t id)
    {
        Inventory* inv = inventoryOf(kind, id);
        Content c;
        if (inv && snapshot(inv, c, wornBagOf(kind, id))) g_baseline[key(kind, id)] = c;
    }
}

Inventory* items_backpackOf(Character* c)
{
    return c ? safeBackpack(c) : NULL;
}

Bytes items_backpackMsg(uint32_t id, Character* c)
{
    Inventory* bp = items_backpackOf(c);
    return bp ? items_inventoryMsg(CONTAINER_BACKPACK, id, bp) : Bytes();
}

int items_moneyOf(Character* c)
{
    int m = -1;
    if (!c || !safeGetMoney(c, &m)) return -1;
    return m;
}

Bytes items_inventoryMsg(uint8_t kind, uint32_t id, Inventory* inv, int money)
{
    ByteWriter w; w.u8(kind); w.u32(id);
    std::vector<InvItem> items;
    if (inv)
    {
        const lektor<Item*>& all = inv->getAllItems();
        for (uint32_t i = 0; i < all.size() && items.size() < MAX_INV_ITEMS; ++i)
        {
            Item* it = all[i];
            if (!it || it->isEquipped) continue;
            InvItem d = describe(it);
            if (d.sanitize()) items.push_back(d);
        }
    }
    w.u16((uint16_t)items.size());
    for (size_t k = 0; k < items.size(); ++k) items[k].write(w);
    if (money >= 0) { w.u8(1); w.u32((uint32_t)money); }   // optional trailer: squad money
    return w.data;
}

namespace
{
    bool safeRemoveFromCharacter(Character* c, Item* it, int qty)
    {
        __try { return c->inventory && c->inventory->removeItemAutoDestroy(it, qty); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeGiveTo(Character* c, Item* it)
    {
        __try { return c->giveItem(it, false, true); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // A trade's money was refused by the host: our squad's money and the trader copy's money go
    // back to what they really are.
    void undoMoneyHere(uint8_t owner, uint8_t action, uint32_t traderId, int amount)
    {
        std::vector<std::pair<uint32_t, Character*> > locals;
        chars_localCharacters(locals);
        Character* me = locals.empty() ? NULL : locals[0].second;
        int have = 0;
        if (me && safeGetMoney(me, &have))
        {
            int now = action == UNDO_REMOVE ? have - amount : have + amount;
            safeSetMoney(me, now < 0 ? 0 : now);
            log("undo from %s: our money %d -> %d", playerName(owner).c_str(), have, now < 0 ? 0 : now);
        }
        uint64_t k = key(CONTAINER_CHARACTER, traderId);
        std::map<uint64_t, int>::iterator ann = g_announcedMoney.find(k);
        Character* trader = chars_byNetId(traderId);
        if (trader && ann != g_announcedMoney.end())
        {
            int real = ann->second + (action == UNDO_REMOVE ? amount : -amount);   // the copy had moved by the refused amount
            if (real < 0) real = 0;
            ann->second = real;
            safeSetMoney(trader, real);
            std::map<uint64_t, int>::iterator base = g_moneyBase.find(k);
            if (base != g_moneyBase.end()) base->second = real;
        }
        showMessage(TF("%s refused the payment (too far from the trader?).", playerName(owner).c_str()));
    }

    // The owner refused one of our transfers: put our side back as it was.
    void applyUndo(uint8_t owner, uint8_t action, uint8_t kind, uint32_t id, const InvItem& d)
    {
        std::vector<std::pair<uint32_t, Character*> > locals;
        chars_localCharacters(locals);
        Ogre::Vector3 where;
        bool hasWhere = kind != CONTAINER_GROUND && positionOf(kind, id, where);
        // Our characters, nearest to the container first (the one that did it).
        std::vector<std::pair<float, Character*> > order;
        for (size_t i = 0; i < locals.size(); ++i)
            if (Character* c = locals[i].second)
                order.push_back(std::make_pair(hasWhere ? c->getPosition().distance(where) : 0.f, c));
        std::sort(order.begin(), order.end());
        ++g_itemsMute; ++g_itemsEpoch;
        int done = 0;
        if (action == UNDO_REMOVE)
        {
            int left = d.quantity > 0 ? d.quantity : 1;
            for (size_t i = 0; i < order.size() && left > 0; ++i)
            {
                std::vector<Item*> items;
                if (!order[i].second->inventory || !safeSnapshot(order[i].second->inventory, &items)) continue;
                for (size_t k = 0; k < items.size() && left > 0; ++k)
                {
                    Item* it = items[k];
                    if (!it || !describe(it).sameKind(d)) continue;
                    int have = it->quantity > 0 ? it->quantity : 1;
                    int part = left < have ? left : have;
                    if (safeRemoveFromCharacter(order[i].second, it, part)) { left -= part; done += part; }
                }
            }
            if (left > 0) log("undo: %d x %s could not be found on our characters any more", left, d.item.c_str());
        }
        else if (!order.empty())
        {
            InvItem one = d;
            int count = 1;
            if (!stacks(d) && d.quantity > 1) { count = d.quantity; one.quantity = 1; }
            for (int i = 0; i < count; ++i)
            {
                Item* it = create(one);
                if (it && safeGiveTo(order[0].second, it)) done += one.quantity;
            }
            if (done < d.quantity) log("undo: could not give %d x %s back", d.quantity - done, d.item.c_str());
        }
        --g_itemsMute;
        // The other player's container, as we see it, goes back to what its owner last said.
        if (kind != CONTAINER_GROUND)
        {
            uint64_t k = key(kind, id);
            g_appliedHash.erase(k);
            std::map<uint64_t, Pending>::iterator m = g_lastMirror.find(k);
            if (m != g_lastMirror.end() && applyMirror(m->second)) rememberContainer(kind, id);
        }
        log("undo from %s: %s %d x %s", playerName(owner).c_str(), action == UNDO_REMOVE ? "removed" : "gave back", done, d.item.c_str());
        showMessage(action == UNDO_REMOVE ? TF("%s kept %s (someone was faster, or you were too far).", playerName(owner).c_str(), itemName(d.item).c_str())
                                          : TF("%s could not take %s: it is back in your inventory.", playerName(owner).c_str(), itemName(d.item).c_str()));
    }
}

void items_sendUndo(uint8_t to, uint8_t action, uint8_t kind, uint32_t id, const InvItem& d) { sendUndo(to, action, kind, id, d); }

// ---------------------------------------------------------------------- town containers
namespace
{
    const DWORD BOX_SCAN_MS = 300, BOX_PUSH_MS = 500;
    const float BOX_SCAN_RADIUS = 40.f;     // around our characters: the container whose window is open
    DWORD g_lastBoxScan = 0, g_lastBoxPush = 0;

    bool sharedWorldClient() { return g_cfg.townSync && ready() && !g_session.isHost() && !npcs_ownWorld(); }

    bool safeBoxPosition(Building* b, Ogre::Vector3* at)
    {
        __try { if (!b->data) return false; *at = b->getPosition(); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // The id a client gives a town container: host-owned net id (top byte 0, NPC bit clear),
    // from its type and position. The host keeps whatever id the client chose for that building.
    uint32_t boxId(const std::string& sid, const Ogre::Vector3& at)
    {
        char buf[64];
        sprintf_s(buf, "@%d,%d,%d", (int)floorf(at.x + 0.5f), (int)floorf(at.y + 0.5f), (int)floorf(at.z + 0.5f));
        std::string k = sid + buf;
        uint32_t h = hashBytes(Bytes(k.begin(), k.end())) & 0x7FFFFF;
        return makeNetId(HOST_ID, h ? h : 1);
    }

    void sendBoxEvent(uint32_t id, const WorldBox& w, bool open)
    {
        ByteWriter m; m.u8(HOST_ID); m.u8(open ? 1 : 0); m.u32(id); m.str(w.sid); m.f32(w.x); m.f32(w.y); m.f32(w.z);
        g_session.sendTo(HOST_ID, MSG_WORLD_CONTAINER, m.data);
    }

    // Client: which town container windows are open on our screen (opened / closed since last time).
    void scanBoxes()
    {
        std::vector<std::pair<uint32_t, Character*> > mine;
        chars_localCharacters(mine);
        std::set<uint32_t> openNow;
        std::vector<Ogre::Vector3> done;
        for (size_t i = 0; i < mine.size(); ++i)
        {
            if (!mine[i].second) continue;
            Ogre::Vector3 p = mine[i].second->getPosition();
            bool covered = false;   // (not "near": a Windows macro)
            for (size_t k = 0; k < done.size() && !covered; ++k) covered = done[k].squaredDistance(p) < 20.f * 20.f;
            if (covered) continue;
            done.push_back(p);
            lektor<RootObject*> list;
            ou->getObjectsWithinSphere(list, p, BOX_SCAN_RADIUS, BUILDING, 64, NULL);
            for (uint32_t k = 0; k < list.size(); ++k)
            {
                Building* b = static_cast<Building*>(list[k]);
                Inventory* inv = worldInventory(b);
                if (!inv || !safeVisible(inv)) continue;
                Ogre::Vector3 at;
                if (!safeBoxPosition(b, &at)) continue;
                std::string sid = b->data->stringID;
                uint32_t id = boxId(sid, at);
                openNow.insert(id);
                if (!g_boxes.count(id) && g_boxes.size() >= MAX_BOXES) continue;
                WorldBox& w = g_boxes[id];
                if (w.open) continue;
                w.sid = sid; w.x = at.x; w.y = at.y; w.z = at.z; w.h = hand(b); w.open = true;
                // Our copy (from our save) is not the town's: emptied until the host's content
                // arrives, and not taken for a transfer of the player (no reference until then).
                uint64_t k2 = key(CONTAINER_WORLD, id);
                g_baseline.erase(k2); g_open.erase(k2); g_appliedHash.erase(k2);
                Pending blank; blank.kind = CONTAINER_WORLD; blank.id = id; blank.since = GetTickCount();
                applyMirror(blank);
                sendBoxEvent(id, w, true);
                log("town container %s opened: asking the host for its content (%08x)", sid.c_str(), id);
            }
        }
        for (std::map<uint32_t, WorldBox>::iterator it = g_boxes.begin(); it != g_boxes.end(); ++it)
            if (it->second.open && !openNow.count(it->first))
            {
                it->second.open = false;
                sendBoxEvent(it->first, it->second, false);
            }
    }

    // Host: content of a container to the clients that have it open (on change, or to one now).
    void pushBox(uint32_t id, WorldBox& w, int onlyTo)
    {
        Inventory* inv = worldInventory(boxBuilding(w));
        if (!inv) return;
        Bytes body = items_inventoryMsg(CONTAINER_WORLD, id, inv);
        uint32_t h = hashBytes(body);
        if (onlyTo >= 0) { g_session.sendTo((uint8_t)onlyTo, MSG_INVENTORY, body); return; }
        if (h == w.sentHash) return;
        w.sentHash = h;
        for (std::set<uint8_t>::iterator p = w.watchers.begin(); p != w.watchers.end(); ++p) g_session.sendTo(*p, MSG_INVENTORY, body);
    }

    // Host: a client opened / closed one.
    void onBoxEvent(const NetEvent& e)
    {
        if (!g_session.isHost()) return;
        ByteReader r(e.body);
        uint8_t target = r.u8(), open = r.u8();
        uint32_t id = r.u32();
        std::string sid = r.str();
        float x = r.f32(), y = r.f32(), z = r.f32();
        if (!r.ok() || target != HOST_ID || netIdOwner(id) != HOST_ID || (id & NPC_ID_FLAG) || !validPos(x, y, z) || sid.empty()) return;
        if (!open)
        {
            std::map<uint32_t, WorldBox>::iterator it = g_boxes.find(id);
            if (it != g_boxes.end()) it->second.watchers.erase(e.sender);
            return;
        }
        if (npcs_clientOwnWorld(e.sender)) return;   // its own world: its containers are its own
        std::map<uint32_t, WorldBox>::iterator it = g_boxes.find(id);
        if (it == g_boxes.end())
        {
            if (g_boxes.size() >= MAX_BOXES) return;
            it = g_boxes.insert(std::make_pair(id, WorldBox())).first;
            it->second.sid = sid; it->second.x = x; it->second.y = y; it->second.z = z;
        }
        WorldBox& w = it->second;
        if (w.sid != sid || Ogre::Vector3(w.x, w.y, w.z).distance(Ogre::Vector3(x, y, z)) > 3.f)
        {
            log("town container %08x from %s: id already used by another container here, ignored", id, playerName(e.sender).c_str());
            return;
        }
        if (!worldInventory(boxBuilding(w)))
        {
            log("town container %s opened by %s: not loaded here", sid.c_str(), playerName(e.sender).c_str());
            return;
        }
        w.watchers.insert(e.sender);
        pushBox(id, w, e.sender);
        log("town container %s opened by %s: content sent", sid.c_str(), playerName(e.sender).c_str());
    }
}

// That player reloaded its world / left. Host: the town containers it had open are closed.
// Client, about the host: it forgot who watches what, open windows are asked for again.
void items_worldForget(uint8_t player)
{
    for (std::map<uint32_t, WorldBox>::iterator it = g_boxes.begin(); it != g_boxes.end(); ++it)
    {
        it->second.watchers.erase(player);
        if (player == HOST_ID && !g_session.isHost()) it->second.open = false;
    }
}

void items_onMessage(const NetEvent& e)
{
    if (e.msgType == MSG_WORLD_CONTAINER) { onBoxEvent(e); return; }
    ByteReader r(e.body);
    if (e.msgType == MSG_INVENTORY)
    {
        Pending p;
        p.kind = r.u8(); p.id = r.u32();
        uint16_t n = r.u16();
        if (!r.ok() || n > MAX_INV_ITEMS || netIdOwner(p.id) != e.sender) return;   // only the owner describes it
        for (uint16_t i = 0; i < n && r.ok(); ++i) { InvItem d; d.read(r); if (d.sanitize()) p.items.push_back(d); }
        if (!r.ok()) return;
        if (r.remaining() && r.u8() == 1)
        {
            uint32_t m = r.u32();
            if (r.ok() && m <= 100000000u) { p.money = (int)m; g_announcedMoney[key(p.kind, p.id)] = p.money; }
        }
        uint64_t k = key(p.kind, p.id);
        uint32_t h = hashBytes(e.body);
        std::map<uint64_t, uint32_t>::iterator ah = g_appliedHash.find(k);
        if (ah != g_appliedHash.end() && ah->second == h) return;
        p.since = GetTickCount();
        flushOpen(p.kind, p.id);
        if (applyMirror(p))
        {
            g_appliedHash[k] = h; g_pending.erase(k); g_lastMirror[k] = p; rememberContainer(p.kind, p.id);
            if (g_cfg.debugKeys)
            {
                std::map<uint64_t, Content>::iterator b = g_baseline.find(k);
                int total = 0;
                if (b != g_baseline.end()) for (std::map<std::string, int>::iterator q = b->second.qty.begin(); q != b->second.qty.end(); ++q) total += q->second;
                log("inventory of %08x updated: %d stack(s) announced, %d item(s) now carried (worn included)", p.id, (int)p.items.size(), total);
            }
        }
        else g_pending[k] = p;
        return;
    }
    if (e.msgType == MSG_ITEM_UNDO)
    {
        uint8_t target = r.u8(), action = r.u8(), kind = r.u8();
        uint32_t id = r.u32();
        InvItem d; d.read(r);
        if (!r.ok() || target != g_session.localId() || netIdOwner(id) != e.sender || !d.sanitize()) return;
        if (action > UNDO_GIVE_BACK || kind > CONTAINER_WORLD) return;
        if (d.item == MONEY_ITEM) { if (kind == CONTAINER_CHARACTER) undoMoneyHere(e.sender, action, id, d.quantity); return; }
        applyUndo(e.sender, action, kind, id, d);
        return;
    }
    if (e.msgType == MSG_ITEM_TAKE || e.msgType == MSG_ITEM_GIVE)
    {
        uint8_t target = r.u8(), kind = r.u8();
        uint32_t id = r.u32();
        InvItem d; d.read(r);
        if (!r.ok() || target != g_session.localId() || netIdOwner(id) != target || !d.sanitize()) return;
        if (e.msgType == MSG_ITEM_TAKE) applyTake(e.sender, kind, id, d);
        else applyGive(e.sender, kind, id, d);
    }
}

void items_tick(DWORD now)
{
    for (std::map<uint64_t, Pending>::iterator it = g_pending.begin(); it != g_pending.end();)
    {
        if (applyMirror(it->second)) { g_lastMirror[it->first] = it->second; rememberContainer(it->second.kind, it->second.id); g_appliedHash.erase(it->first); g_pending.erase(it++); }
        else if (now - it->second.since > PENDING_TTL_MS) g_pending.erase(it++);
        else ++it;
    }
    static DWORD lastDetect = 0;
    if (now - lastDetect >= 100) { lastDetect = now; detectTransfers(); }
    if (sharedWorldClient() && now - g_lastBoxScan >= BOX_SCAN_MS) { g_lastBoxScan = now; scanBoxes(); }
    if (g_session.isHost() && now - g_lastBoxPush >= BOX_PUSH_MS)
    {
        g_lastBoxPush = now;
        for (std::map<uint32_t, WorldBox>::iterator it = g_boxes.begin(); it != g_boxes.end(); ++it)
            if (!it->second.watchers.empty()) pushBox(it->first, it->second, -1);
    }
}

void items_onPlayerLeft(uint8_t id)
{
    items_worldForget(id);
    if (id == HOST_ID && !g_session.isHost()) g_boxes.clear();   // the host's town containers: ours again
    for (std::map<uint64_t, Pending>::iterator it = g_pending.begin(); it != g_pending.end();)
        if (netIdOwner(it->second.id) == id) g_pending.erase(it++); else ++it;
    for (std::map<uint64_t, uint32_t>::iterator it = g_appliedHash.begin(); it != g_appliedHash.end();)
        if (netIdOwner((uint32_t)it->first) == id) g_appliedHash.erase(it++); else ++it;
    for (std::map<uint64_t, Content>::iterator it = g_baseline.begin(); it != g_baseline.end();)
        if (netIdOwner((uint32_t)it->first) == id) g_baseline.erase(it++); else ++it;
    for (std::set<uint64_t>::iterator it = g_open.begin(); it != g_open.end();)
        if (netIdOwner((uint32_t)*it) == id) g_open.erase(it++); else ++it;
    for (std::map<uint64_t, int>::iterator it = g_announcedMoney.begin(); it != g_announcedMoney.end();)
        if (netIdOwner((uint32_t)it->first) == id) g_announcedMoney.erase(it++); else ++it;
    for (std::map<uint64_t, int>::iterator it = g_moneyBase.begin(); it != g_moneyBase.end();)
        if (netIdOwner((uint32_t)it->first) == id) g_moneyBase.erase(it++); else ++it;
    for (std::map<uint64_t, Pending>::iterator it = g_lastMirror.begin(); it != g_lastMirror.end();)
        if (netIdOwner((uint32_t)it->first) == id) g_lastMirror.erase(it++); else ++it;
    for (std::map<uint64_t, std::vector<hand> >::iterator it = g_created.begin(); it != g_created.end();)
        if (netIdOwner((uint32_t)it->first) == id) g_created.erase(it++); else ++it;
}

void items_redressed(uint32_t id)
{
    // Re-dressing recreated its gear: its loose items and the content of its (new) backpack.
    uint8_t kinds[2] = { CONTAINER_CHARACTER, CONTAINER_BACKPACK };
    g_created.erase(key(CONTAINER_BACKPACK, id));   // those died with the old backpack
    for (int n = 0; n < 2; ++n)
    {
        uint64_t k = key(kinds[n], id);
        std::map<uint64_t, Pending>::iterator it = g_lastMirror.find(k);
        if (it != g_lastMirror.end()) applyMirror(it->second);
        if (g_baseline.count(k) || it != g_lastMirror.end()) rememberContainer(kinds[n], id);   // our change, not a transfer
    }
}

// A ghost was removed: its containers died with it. What the owner last announced is kept
// (re-applied when it comes back), but the "already applied" marks go, so the next identical
// announcement is applied to the new body.
void items_onDespawn(uint32_t id)
{
    uint8_t kinds[2] = { CONTAINER_CHARACTER, CONTAINER_BACKPACK };
    for (int n = 0; n < 2; ++n)
    {
        uint64_t k = key(kinds[n], id);
        g_appliedHash.erase(k); g_created.erase(k); g_moneyBase.erase(k); g_open.erase(k); g_baseline.erase(k);
    }
}

void items_onWorldReload()
{
    g_boxes.clear();
    g_announcedMoney.clear();
    g_moneyBase.clear();
    g_lastMirror.clear();
    g_created.clear();
    g_baseline.clear();
    g_open.clear();
    g_pending.clear();
    g_appliedHash.clear();
}

namespace
{
    Item* safeTakeOut(Inventory* inv, Item* it)
    {
        __try { return inv->removeItemDontDestroy_returnsItem(it, it->quantity > 0 ? it->quantity : 1, false); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    bool safeGive(Character* c, Item* it)
    {
        __try { return c->giveItem(it, false, true); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

std::string items_debugLoot(Character* taker)
{
    for (std::map<uint64_t, Content>::iterator it = g_baseline.begin(); it != g_baseline.end(); ++it)
    {
        Inventory* inv = inventoryOf((uint8_t)(it->first >> 32), (uint32_t)it->first);
        if (!inv || !safeVisible(inv)) continue;
        std::vector<Item*> items;
        if (!safeSnapshot(inv, &items) || items.empty()) return "the open inventory is empty";
        Item* item = items[0];
        if (!item) return "the open inventory has an empty slot first";
        std::string name = describe(item).item;
        Item* out = safeTakeOut(inv, item);
        if (!out) return "could not take " + name;
        if (!safeGive(taker, out)) return "took " + name + " but could not give it";
        return "moved " + name;
    }
    return "no remote inventory is open";
}

// Test runs: a crossbow of the base game equipped on c (its ghost on the other side gets it from
// the equipment sync: ranged fight test). Tries each one until the engine accepts it.
bool safeEquipOn(Character* c, Item* it)
{
    __try
    {
        if (!c->giveItem(it, false, true)) return false;   // as the ghosts get their gear (it takes the ranged slot)
        if (!it->isEquipped && c->inventory) c->inventory->equipItem(it);
        return it->isEquipped;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string items_debugEquipCrossbow(Character* c)
{
    lektor<GameData*> bows;
    ou->gamedata.getDataOfType(bows, CROSSBOW);
    std::string tried;
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < bows.size(); ++i)
        {
            GameData* g = bows[i];
            if (!g || (g->stringID.find("gamedata.base") != std::string::npos) != (pass == 0)) continue;
            InvItem d; d.item = g->stringID; d.quantity = 1;
            Item* it = create(d);
            if (!it) continue;
            if (safeEquipOn(c, it))
            {
                if (c->stats) c->stats->rangedMode = true;   // shoot, do not close in
                return g->name + " (" + g->stringID + ") equipped, ranged mode on";
            }
            tried += g->name + "; ";
            if (tried.size() > 300) break;
        }
    return "no crossbow could be equipped: " + tried;
}

// Test runs: one single item in c's inventory (two players then try to take it at once).
std::string items_debugGiveOne(Character* c)
{
    lektor<GameData*> things;
    ou->gamedata.getDataOfType(things, ITEM);
    if (things.size() == 0 || !things[0] || !c->inventory) return "no item data";
    InvItem d; d.item = things[0]->stringID; d.quantity = 1;
    Item* it = create(d);
    if (!it || !safeAdd(c->inventory, it, 1)) return "could not add " + d.item;
    return "1 x " + d.item;
}

std::string items_debugBackpack(Character* taker)
{
    // Debug: take the first item of a remote backpack within reach, as if dragged out of it.
    for (std::map<uint64_t, Content>::iterator it = g_baseline.begin(); it != g_baseline.end(); ++it)
    {
        uint8_t kind = (uint8_t)(it->first >> 32);
        uint32_t id = (uint32_t)it->first;
        if (kind != CONTAINER_BACKPACK) continue;
        Character* wearer = chars_byNetId(id);
        Inventory* inv = inventoryOf(kind, id);
        if (!wearer || !inv || wearer->getPosition().distance(taker->getPosition()) > 60.f) continue;
        std::vector<Item*> items;
        if (!safeSnapshot(inv, &items) || items.empty() || !items[0]) continue;
        std::string name = describe(items[0]).item;
        Item* out = safeTakeOut(inv, items[0]);
        if (!out) return "could not take " + name + " from the backpack";
        if (!safeGive(taker, out)) return "took " + name + " from the backpack but could not give it";
        Content now;
        if (snapshot(inv, now)) { forwardDiff(kind, id, it->second, now); it->second = now; }
        return "moved " + name + " out of the backpack of " + wearer->getName();
    }
    return "no remote backpack within reach";
}

bool items_install()
{
    return true;   // no engine hook: transfers are detected by comparing snapshots (items_tick)
}

} // namespace kmp
