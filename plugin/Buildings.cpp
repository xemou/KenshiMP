// Town replication.
//
// Owner side : RootObjectFactory::createBuilding is hooked; every top-level building created for
//              the player faction (placed blueprint or loaded from the save) is tracked. Its state
//              (template, transform, construction progress, destroyed flag, door states) is sent
//              when it changes, plus a full resend every 10 s and when someone joins.
// Ghost side : buildings are created in the remote player's local faction and follow the owner.
//              Furniture, doors and interiors are built by the game from the parent building.
// Damage     : hits on a ghost building's door and dismantling of a ghost building are forwarded
//              to the owner, who applies them for real; the result comes back through the state.
#include <Debug.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/Faction.h>
#include <kenshi/GameData.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Building/Building.h>
#include <kenshi/Building/DoorStuff.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
#include <kenshi/Damages.h>
#include <kenshi/ZoneManager.h>
#include <kenshi/FactionRelations.h>
#include <core/Functions.h>

#include "Shared.h"

#include <map>
#include <vector>

using namespace mp;

namespace kmp {

namespace
{
    const DWORD CHANGE_PERIOD_MS = 1000;
    const DWORD FULL_PERIOD_MS = 60000;   // safety net only: joins and resyncs force a full resend (builds_resendAll)

    struct LocalBuilding
    {
        hand h;
        Building* ptr;
        uint32_t id;          // 0 until we are ready (client id unknown before the handshake)
        BuildingMsg lastSent;
        bool sent;
        uint32_t inventoryHash;   // storage content (MSG_INVENTORY)
        LocalBuilding() : ptr(NULL), id(0), sent(false), inventoryHash(0) {}
    };
    std::vector<LocalBuilding> g_local;      // appended from the factory hook (g_lock)
    uint32_t g_nextLocal = 1;
    DWORD g_lastChange = 0, g_lastFull = 0;
    bool g_forceFull = false;

    struct GhostBuilding
    {
        hand h;
        Building* ptr;
        bool completed, destroyed;
        std::vector<uint8_t> doors;
        GhostBuilding() : ptr(NULL), completed(false), destroyed(false) {}
    };
    std::map<uint32_t, GhostBuilding> g_ghosts;           // g_lock for lookups from hooks
    volatile bool g_creatingGhost = false;   // our own createBuilding calls are not tracked
    std::vector<hand> g_rejected;            // blueprints refused (overlap), destroyed next tick
    const float OVERLAP_RADIUS = 3.f;

    bool overlapsGhostBuilding(const Ogre::Vector3& p)
    {
        Lock l;
        for (std::map<uint32_t, GhostBuilding>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
        {
            Building* g = it->second.h.getBuilding();
            if (g && g->getPosition().distance(p) < OVERLAP_RADIUS) return true;
        }
        return false;
    }
    volatile bool g_applyingSync = false;    // our own damage/dismantle calls pass the hooks

    Building* safeCreate(GameData* gd, const Ogre::Vector3* p, Faction* f, const Ogre::Quaternion* q, bool completed)
    {
        __try { return ou->theFactory->createBuilding(gd, *p, NULL, f, *q, NULL, NULL, NULL, NULL, NULL, false, completed, false, 0, false); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    bool safeSetProgress(Building* b, float progress, bool complete)
    {
        __try
        {
            b->setConstructionProgress(progress);
            if (complete) b->notifyConstructionComplete();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeSetDestroyed(Building* b, bool d)
    {
        __try { b->setDestroyed(d); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeDoorState(DoorStuff* d, uint8_t bits)
    {
        __try
        {
            bool broken = (bits & BuildingMsg::DOOR_BROKEN) != 0, locked = (bits & BuildingMsg::DOOR_LOCKED) != 0;
            if (d->isBroken() != broken) d->setBroken(broken);
            if (d->isLocked() != locked) { if (locked) d->lockDoor(); else d->unlockDoor(); }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeDestroyInner(Building* b)
    {
        __try { return ou->destroy(b, false, "kenshimp"); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeDestroy(Building* b) { ++g_itemsMute; ++g_itemsEpoch; bool r = safeDestroyInner(b); --g_itemsMute; return r; }
    bool safeDismantle(Building* b, float amount)
    {
        __try { b->addDismantleProgress(amount); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    DoorStuff* doorAt(Building* b, uint32_t i)
    {
        if (!b || i >= b->doors.size()) return NULL;
        Building* d = b->doors[i];
        return d && d->isDoor() ? static_cast<DoorStuff*>(d) : NULL;
    }

    bool capture(Building* b, uint32_t id, BuildingMsg& m)
    {
        if (!b->data) return false;
        m.netId = id;
        m.gameDataName = b->data->stringID;
        Ogre::Vector3 p = b->getPosition();
        Ogre::Quaternion q = b->getOrientation();
        m.x = p.x; m.y = p.y; m.z = p.z;
        m.qx = q.x; m.qy = q.y; m.qz = q.z; m.qw = q.w;
        Building::ConstructionState* s = b->getBuildState();
        m.buildProgress = s ? s->constructionProgress : 0.f;
        m.completed = s ? (s->isComplete ? 1 : 0) : 1;
        m.destroyed = b->isDestroyed() ? 1 : 0;
        for (uint32_t i = 0; i < b->doors.size() && i < 64; ++i)
        {
            DoorStuff* d = doorAt(b, i);
            m.doors.push_back(d ? (uint8_t)((d->isBroken() ? BuildingMsg::DOOR_BROKEN : 0) | (d->isLocked() ? BuildingMsg::DOOR_LOCKED : 0)) : 0);
        }
        return true;
    }

    bool changed(const BuildingMsg& a, const BuildingMsg& b)
    {
        return a.completed != b.completed || a.buildProgress != b.buildProgress || a.destroyed != b.destroyed ||
               a.doors != b.doors || a.x != b.x || a.y != b.y || a.z != b.z;
    }

    void sendBatch(std::vector<BuildingMsg>& batch)
    {
        const size_t PER_MSG = 200;
        for (size_t start = 0; start < batch.size(); start += PER_MSG)
        {
            size_t end = start + PER_MSG < batch.size() ? start + PER_MSG : batch.size();
            ByteWriter w; w.u16((uint16_t)(end - start));
            for (size_t k = start; k < end; ++k) batch[k].write(w);
            g_session.send(MSG_BUILDING_STATE, w.data);
        }
        batch.clear();
    }

    void sync(bool full)
    {
        std::vector<BuildingMsg> batch;
        std::vector<uint32_t> removed;
        std::vector<Bytes> inventories;
        {
            Lock l;
            for (size_t k = g_local.size(); k-- > 0;)
            {
                LocalBuilding& lb = g_local[k];
                Building* b = lb.h.getBuilding();
                if (!b || b != lb.ptr)
                {
                    if (lb.id) removed.push_back(lb.id);
                    g_local.erase(g_local.begin() + k);
                    continue;
                }
                if (lb.id && netIdOwner(lb.id) != g_session.localId()) { lb.id = 0; lb.sent = false; lb.inventoryHash = 0; }   // player number changed
                if (!lb.id) lb.id = makeNetId(g_session.localId(), g_nextLocal++);
                BuildingMsg m;
                if (!capture(b, lb.id, m)) continue;
                if (full || !lb.sent || changed(m, lb.lastSent)) { batch.push_back(m); lb.lastSent = m; lb.sent = true; }
                if (Inventory* inv = m.completed ? b->getInventory() : NULL)
                {
                    Bytes body = items_inventoryMsg(CONTAINER_BUILDING, lb.id, inv);
                    uint32_t h = hashBytes(body);
                    if (full || h != lb.inventoryHash) { lb.inventoryHash = h; inventories.push_back(body); }
                }
            }
        }
        for (size_t k = 0; k < inventories.size(); ++k) g_session.send(MSG_INVENTORY, inventories[k]);
        for (size_t k = 0; k < removed.size(); ++k)
        {
            ByteWriter w; w.u32(removed[k]);
            g_session.send(MSG_BUILDING_REMOVE, w.data);
        }
        sendBatch(batch);
    }

    void applyGhost(uint8_t owner, const BuildingMsg& m)
    {
        GhostBuilding g;
        { Lock l; std::map<uint32_t, GhostBuilding>::iterator it = g_ghosts.find(m.netId); if (it != g_ghosts.end()) g = it->second; }
        Building* b = g.h.getBuilding();
        if (!b)
        {
            Faction* f = factionFor(owner);
            GameData* gd = m.gameDataName.empty() ? NULL : ou->gamedata.getData(m.gameDataName);
            if (!f || !gd) { log("cannot create building '%s' (missing mod?)", m.gameDataName.c_str()); return; }
            Ogre::Vector3 p(m.x, m.y, m.z);
            Ogre::Quaternion q(m.qw, m.qx, m.qy, m.qz);
            g_creatingGhost = true;
            b = safeCreate(gd, &p, f, &q, m.completed != 0);
            g_creatingGhost = false;
            if (!b) { log("createBuilding failed for '%s'", m.gameDataName.c_str()); return; }
            g.h = hand(b); g.ptr = b;
            g.completed = m.completed != 0;
            if (!g.completed) safeSetProgress(b, m.buildProgress, false);
        }
        else
        {
            g.ptr = b;
            bool nowComplete = m.completed && !g.completed;
            if (!g.completed || nowComplete)
                if (!safeSetProgress(b, m.buildProgress, nowComplete)) log("could not update building progress");
            g.completed = m.completed != 0;
        }

        g_applyingSync = true;
        if ((m.destroyed != 0) != g.destroyed)
            if (safeSetDestroyed(b, m.destroyed != 0)) g.destroyed = m.destroyed != 0;
        if (m.doors != g.doors)
        {
            for (uint32_t i = 0; i < m.doors.size(); ++i)
                if (DoorStuff* d = doorAt(b, i)) safeDoorState(d, m.doors[i]);
            g.doors = m.doors;
        }
        g_applyingSync = false;

        Lock l;
        g_ghosts[m.netId] = g;
    }

    void removeGhost(uint32_t id)
    {
        Building* b = NULL;
        {
            Lock l;
            std::map<uint32_t, GhostBuilding>::iterator it = g_ghosts.find(id);
            if (it == g_ghosts.end()) return;
            b = it->second.h.getBuilding();
            g_ghosts.erase(it);
        }
        if (b && !safeDestroy(b)) log("could not destroy ghost building %08x", id);
    }

    uint32_t ghostIdOf(Building* b)
    {
        if (!b) return 0;
        Lock l;
        for (std::map<uint32_t, GhostBuilding>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
            if (it->second.ptr == b && it->second.h.getBuilding() == b) return it->first;
        return 0;
    }

    uint32_t localIdOf(Building* b)
    {
        if (!b) return 0;
        Lock l;
        for (size_t i = 0; i < g_local.size(); ++i) if (g_local[i].ptr == b) return g_local[i].id;
        return 0;
    }

    void applyIncomingDamage(const BuildingDamageMsg& d, uint8_t sender);
}

uint32_t builds_netIdOf(Building* b)
{
    if (uint32_t g = ghostIdOf(b)) return g;
    return localIdOf(b);
}

Building* builds_byNetId(uint32_t id)
{
    Lock l;
    if (netIdOwner(id) == g_session.localId())
    {
        for (size_t i = 0; i < g_local.size(); ++i) if (g_local[i].id == id) return g_local[i].h.getBuilding();
        return NULL;
    }
    std::map<uint32_t, GhostBuilding>::iterator it = g_ghosts.find(id);
    return it == g_ghosts.end() ? NULL : it->second.h.getBuilding();
}

Building* builds_findWorld(const std::string& sid, float x, float y, float z)
{
    GameData* d = sid.empty() ? NULL : ou->gamedata.getData(sid);
    if (!d || !ou->zoneMgr) return NULL;
    lektor<Building*> list;
    ou->zoneMgr->getBuildingsThatLinkTo(list, d);
    Ogre::Vector3 p(x, y, z);
    Building* best = NULL; float bestD = 3.f;
    for (uint32_t i = 0; i < list.size(); ++i)
    {
        float dd = list[i]->getPosition().distance(p);
        if (dd < bestD) { bestD = dd; best = list[i]; }
    }
    return best;
}

void builds_tick(DWORD now)
{
    if (!g_cfg.syncBuildings) return;
    std::vector<hand> rejected;
    { Lock l; rejected.swap(g_rejected); }
    for (size_t i = 0; i < rejected.size(); ++i)
        if (Building* b = rejected[i].getBuilding())
            if (safeDestroy(b)) showMessage(T("Cannot build there: another player's building is in the way."));
    bool full = g_forceFull || now - g_lastFull >= FULL_PERIOD_MS;
    if (full || now - g_lastChange >= CHANGE_PERIOD_MS)
    {
        g_lastChange = now;
        if (full) { g_lastFull = now; g_forceFull = false; }
        sync(full);
    }
}

void builds_resendAll() { g_forceFull = true; }

// World reloaded: ghost buildings died with the old world (owners resend a full state every
// 10 s). Local entries validate themselves by handle in sync(); the ones created while the
// save loaded are already tracked by the factory hook.
void builds_onWorldReload()
{
    Lock l;
    g_ghosts.clear();
    g_forceFull = true;
}

void builds_onPlayerLeft(uint8_t id)
{
    std::vector<uint32_t> gone;
    {
        Lock l;
        for (std::map<uint32_t, GhostBuilding>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
            if (netIdOwner(it->first) == id) gone.push_back(it->first);
    }
    for (size_t i = 0; i < gone.size(); ++i) removeGhost(gone[i]);
}

// Ghost buildings saved into the savegame by a previous session: remove them.
void builds_purgeStale()
{
    if (!ou->zoneMgr) return;
    for (int p = 0; p < MAX_PLAYERS; ++p)
    {
        char buf[32]; sprintf_s(buf, sizeof(buf), "kenshimp_player_%d", p);
        Faction* f = ou->factionMgr->getFactionByStringID(buf);
        if (!f) continue;
        lektor<Building*> list;
        ou->zoneMgr->findAllBuildings(list, NULL, f, false, 0, NULL);
        int n = 0;
        for (uint32_t i = 0; i < list.size(); ++i)
            if (!ghostIdOf(list[i]) && !list[i]->isFurnitureOrDoor() && safeDestroy(list[i])) ++n;
        if (n) log("purged %d stale ghost building(s) of %s", n, buf);
    }
}

void builds_onMessage(const NetEvent& e)
{
    if (!g_cfg.syncBuildings) return;
    ByteReader r(e.body);
    if (e.msgType == MSG_BUILDING_STATE)
    {
        uint16_t n = r.u16();
        for (int i = 0; i < n && r.ok(); ++i)
        {
            BuildingMsg m; m.read(r);
            if (r.ok() && netIdOwner(m.netId) == e.sender && m.sanitize()) applyGhost(e.sender, m);
        }
    }
    else if (e.msgType == MSG_BUILDING_REMOVE)
    {
        uint32_t id = r.u32();
        if (r.ok() && netIdOwner(id) == e.sender) removeGhost(id);
    }
    else if (e.msgType == MSG_BUILDING_DAMAGE)
    {
        uint8_t target = r.u8();
        BuildingDamageMsg d; d.read(r);
        d.sanitize();
        if (r.ok() && target == g_session.localId()) applyIncomingDamage(d, e.sender);
    }
}

// Debug (F11 with debug_keys=1): place a finished building of the player faction next to the
// selected character, exercising the whole replication path without the build menus.
void builds_debugSpawn()
{
    if (!ou || !ou->player) return;
    Character* c = ou->player->selectedCharacter.getCharacter();
    Faction* f = ou->player->getFaction();
    if (!c || !f) { log("debug building: select a character first"); return; }

    lektor<GameData*> list;
    ou->gamedata.getDataOfType(list, BUILDING);
    GameData* pick = NULL;
    for (uint32_t i = 0; i < list.size() && !pick; ++i)
        if (list[i] && (list[i]->name.find("Storage") != std::string::npos || list[i]->name.find("storage") != std::string::npos))
            pick = list[i];
    if (!pick && list.size()) pick = list[0];
    if (!pick) { log("debug building: no building data"); return; }

    Ogre::Vector3 p = c->getPosition() + Ogre::Vector3(0, 0, 6);
    Ogre::Quaternion q = Ogre::Quaternion::IDENTITY;
    Building* b = safeCreate(pick, &p, f, &q, true);
    log("debug building '%s' (%s) -> %p", pick->name.c_str(), pick->stringID.c_str(), b);
}

} // namespace kmp

using namespace kmp;

// --- hook: RootObjectFactory::createBuilding -------------------------------------------------
typedef Building* (*CreateBuildingFn)(RootObjectFactory*, GameData*, Ogre::Vector3, TownBase*, Faction*, Ogre::Quaternion,
                                      FactoryCallbackInterface*, Layout*, Building*, GameSaveState*, Building*,
                                      bool, bool, bool, int, bool);
CreateBuildingFn createBuilding_orig = NULL;
Building* createBuilding_hook(RootObjectFactory* self, GameData* data, Ogre::Vector3 position, TownBase* t, Faction* owner,
                              Ogre::Quaternion rotation, FactoryCallbackInterface* cb, Layout* furnitureOf, Building* isDoorOf,
                              GameSaveState* saveState, Building* isIndoorsOf, bool invisible, bool completed,
                              bool isFoliage, int floorNumber, bool isOutsideFurniture)
{
    Building* b = createBuilding_orig(self, data, position, t, owner, rotation, cb, furnitureOf, isDoorOf, saveState,
                                      isIndoorsOf, invisible, completed, isFoliage, floorNumber, isOutsideFurniture);
    if (b && !g_creatingGhost && owner && owner->isThePlayer() && !furnitureOf && !isDoorOf && !isIndoorsOf && !isFoliage)
    {
        // A new blueprint (not loaded from a save) right on top of another player's building:
        // refuse it (destroyed next tick: the engine still uses it right after creation).
        if (!saveState && !completed && ready() && overlapsGhostBuilding(position))
        {
            Lock l;
            g_rejected.push_back(hand(b));
            return b;
        }
        LocalBuilding lb; lb.h = hand(b); lb.ptr = b;
        Lock l;
        g_local.push_back(lb);
    }
    return b;
}

// --- hook: Building::addDismantleProgress ----------------------------------------------------
bool (*dismantle_orig)(Building*, float) = NULL;
bool dismantle_hook(Building* self, float amount)
{
    if (!g_applyingSync && ready())
        if (uint32_t id = ghostIdOf(self))
        {
            BuildingDamageMsg d; d.buildingNetId = id; d.door = 0xFF; d.dismantle = amount;
            ByteWriter w; w.u8(netIdOwner(id)); d.write(w);
            g_session.send(MSG_BUILDING_DAMAGE, w.data);
            return false;
        }
    return dismantle_orig(self, amount);
}

// --- hook: DoorStuff::hitByMeleeAttack -------------------------------------------------------
// Door of a ghost building: forward the hit to the owner, play the hit locally with no damage.
// Door of our own building hit by a ghost: no local damage either, the real hit comes by network.
HitMaterialType (*doorHit_orig)(DoorStuff*, CutDirection, Damages&, Character*, CombatTechniqueData*, int) = NULL;
HitMaterialType doorHit_hook(DoorStuff* self, CutDirection dir, Damages& dmg, Character* who, CombatTechniqueData* attack, int combo)
{
    if (!g_applyingSync && ready() && self && self->parent)
    {
        bool attackerGhost = chars_isGhost(who);
        uint32_t parentGhost = ghostIdOf(self->parent);
        if (parentGhost && !attackerGhost)
        {
            BuildingDamageMsg d; d.buildingNetId = parentGhost; d.door = 0xFF;
            for (uint32_t i = 0; i < self->parent->doors.size(); ++i) if (self->parent->doors[i] == self) { d.door = (uint8_t)i; break; }
            d.attackerNetId = chars_netIdOf(who);
            d.cut = dmg.cut; d.blunt = dmg.blunt; d.pierce = dmg.pierce;
            ByteWriter w; w.u8(netIdOwner(parentGhost)); d.write(w);
            g_session.send(MSG_BUILDING_DAMAGE, w.data);
        }
        if (parentGhost || (attackerGhost && localIdOf(self->parent)))
        {
            Damages none(0, 0, 0, 0, 0);
            return doorHit_orig(self, dir, none, who, attack, combo);
        }
    }
    return doorHit_orig(self, dir, dmg, who, attack, combo);
}

namespace kmp { namespace {
    HitMaterialType safeDoorHit(DoorStuff* door, Damages* dmg, Character* who, bool* ok)
    {
        *ok = true;
        __try { return doorHit_orig(door, (CutDirection)0, *dmg, who, NULL, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { *ok = false; return (HitMaterialType)0; }
    }

    // Only a player we are hostile with may damage or dismantle our buildings (a modified or
    // careless client cannot tear down a friend's town).
    bool hostileTo(uint8_t sender)
    {
        Faction* mine = ou && ou->player ? ou->player->getFaction() : NULL;
        Faction* theirs = factionFor(sender);
        if (!mine || !theirs || !mine->relations) return false;
        return mine->relations->getFactionRelation(theirs) < 0.f;
    }

    void applyIncomingDamage(const BuildingDamageMsg& d, uint8_t sender)
    {
        Building* b = builds_byNetId(d.buildingNetId);
        if (!b || netIdOwner(d.buildingNetId) != g_session.localId()) return;
        if (!hostileTo(sender))
        {
            static DWORD lastWarn = 0;
            if (GetTickCount() - lastWarn > 5000) { lastWarn = GetTickCount(); log("ignored building damage from %s (not at war)", playerName(sender).c_str()); }
            return;
        }
        g_applyingSync = true;
        if (d.door == 0xFF)
        {
            if (d.dismantle != 0 && !safeDismantle(b, d.dismantle)) log("remote dismantle failed on %08x", d.buildingNetId);
        }
        else if (DoorStuff* door = doorAt(b, d.door))
        {
            Damages dmg(d.cut, d.blunt, d.pierce, 0, 0);
            bool ok = false;
            safeDoorHit(door, &dmg, chars_byNetId(d.attackerNetId), &ok);
            if (!ok) log("remote door hit failed on %08x door %d", d.buildingNetId, (int)d.door);
        }
        g_applyingSync = false;
    }
} }

namespace kmp {
bool builds_install()
{
    bool ok = true;
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&RootObjectFactory::createBuilding),
                                                 createBuilding_hook, &createBuilding_orig))
    { ErrorLog("KenshiMP: could not hook createBuilding!"); ok = false; }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Building::_NV_addDismantleProgress),
                                                 dismantle_hook, &dismantle_orig))
    { ErrorLog("KenshiMP: could not hook addDismantleProgress!"); ok = false; }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&DoorStuff::_NV_hitByMeleeAttack),
                                                 doorHit_hook, &doorHit_orig))
    { ErrorLog("KenshiMP: could not hook DoorStuff::hitByMeleeAttack!"); ok = false; }
    return ok;
}
}
