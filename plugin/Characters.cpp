// Squad replication.
//
// Owner side : every player character gets a stable netId. A full state is streamed at 10 Hz:
//              transform, per-body-part health, blood, KO/death, move speed, combat target and
//              current task. Spawn/appearance/equipment/stats are (re)sent on change or on join.
// Ghost side : remote characters are real engine characters in the remote player's faction.
//              They walk/run to the streamed position, fight the same target (real combat
//              animations), perform the same whitelisted task (build, operate, sleep, sit...),
//              wear the same gear, and mirror health, stats, KO and death. Their own decision
//              making (AI::periodicUpdate) is off: they only do what their owner does.
// Combat     : damage authority = the attacker's machine, applied by the victim's owner.
//              - a local character hitting a ghost: forwarded to the ghost's owner;
//              - a ghost hitting anything replicated (local player or other ghost): dropped,
//                because the real hit is computed on the attacker's own machine;
//              - world NPCs are local on every machine and take damage normally.
#include <Debug.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
#include <kenshi/CharMovement.h>
#include <kenshi/CharBody.h>
#include <kenshi/CharStats.h>
#include <kenshi/Tasker.h>
#include <kenshi/Faction.h>
#include <kenshi/GameData.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/Damages.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/Appearance.h>
#include <kenshi/AI/AI.h>
#include <kenshi/Platoon.h>
#include <kenshi/gui/ManagementScreen.h>
#include <kenshi/gui/MapScreen.h>
#include <kenshi/FactionRelations.h>
#include <kenshi/Building/Building.h>
#include <kenshi/combat/CombatClass.h>
#include <kenshi/GunClass.h>
#include <kenshi/Animation/AnimationClass.h>
#include <core/Functions.h>

#include "Shared.h"
#include "CharSync.h"
#include "../core/Interp.h"
#include "../core/MovementGuard.h"

#include <map>
#include <deque>
#include <set>
#include <vector>

using namespace mp;

namespace kmp {

namespace
{
    const DWORD STATE_PERIOD_MS = 50;     // 20 Hz, like a game server tick
    const DWORD SPAWN_PERIOD_MS = 5000;
    const DWORD LOOKS_PERIOD_MS = 1000;   // appearance / gear / stats change detection
    const float TELEPORT_DISTANCE = 15.f;
    const float TASK_RANGE = 3.f;         // ghost must be this close to its owner's spot to mirror a task
    const float COMBAT_LEASH = 8.f;       // while fighting, only pull the ghost back if it drifts further
    const float MELEE_SANITY_RANGE = 30.f;  // generous: interpolation delay + lunges
    const float SPAWN_RANGE = 2500.f;       // ghosts further than this from all our characters wait

    // ------------------------------------------------------------------ SEH-guarded engine calls
    // (Functions containing __try cannot hold C++ objects with destructors.)
    bool safeTeleport(Character* c, const Ogre::Vector3* p, int floor = 0)
    {
        __try { c->getMovement()->_setPositionAndTeleport(*p, floor); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Render layer: moves only what is drawn (model/scene node), not the simulated body.
    bool safeVisual(Character* c, const Ogre::Vector3* p, const Ogre::Quaternion* q)
    {
        __try { c->teleportVisuallyOnly(*p, *q); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeDrawnPos(Character* c, Ogre::Vector3* out)
    {
        __try
        {
            AnimationClass* a = c->animation;
            if (!a) return false;
            *out = a->getSceneNodePosition();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Small positional nudge (no teleport effects): used every frame to close the residual gap
    // the engine's locomotion leaves (arrival tolerance, speed differences).
    bool safeNudge(Character* c, const Ogre::Vector3* p)
    {
        __try { c->getMovement()->_setPositionSimple(*p); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Direct locomotion order: Character::setDestination goes through the character's AI (which is
    // off for ghosts) and was silently ignored; CharMovement::setDestination drives the body itself.
    bool safeMoveTo(Character* c, const Ogre::Vector3* p)
    {
        __try
        {
            CharMovement* m = c->getMovement();
            if (!m) return false;
            m->setDestination(*p, HIGH_PRIORITY, false);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // A ghost must be able to run as fast as its owner (stats, load, wounds can differ here).
    bool safeMatchSpeed(Character* c, float speed, float* maxBefore)
    {
        __try
        {
            CharMovement* m = c->getMovement();
            if (!m) return false;
            *maxBefore = m->getMaxSpeed();
            if (speed > *maxBefore) m->setMaxSpeed(speed);
            m->setDesiredSpeed(speed);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Diagnostics: what the engine does with the ghost's speed.
    struct SpeedInfo { float current, max, standard; int orders; bool running, pathOk, pathFailed; };
    bool safeSpeedInfo(Character* c, SpeedInfo* s)
    {
        __try
        {
            CharMovement* m = c->getMovement();
            if (!m) return false;
            s->current = m->getCurrentSpeed(); s->max = m->getMaxSpeed(); s->standard = m->getStandardWalkSpeed();
            s->orders = (int)m->getSpeedOrders(); s->running = m->isRunning();
            s->pathOk = m->pathOk(); s->pathFailed = m->pathFailed();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeRestoreSpeed(Character* c, float baseMax)
    {
        __try
        {
            CharMovement* m = c->getMovement();
            if (!m) return false;
            if (baseMax > 0) m->setMaxSpeed(baseMax);
            m->restoreDesiredSpeed();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Engine locomotion state: is it actually stalled (stopped, or no path)?
    bool safeStalled(Character* c)
    {
        __try
        {
            CharMovement* m = c->getMovement();
            return !m || m->getCurrentSpeed() < 1.f || m->pathFailed();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    }
    bool safeSetSpeed(Character* c, int speed)
    {
        __try { c->getMovement()->setDesiredSpeedOrders((MoveSpeed)speed); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeAttack(Character* c, Character* target)
    {
        __try { c->attackTarget(target); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeOrder(Character* c, int task, RootObject* subject, const Ogre::Vector3* loc)
    {
        __try { c->addOrder(NULL, (TaskType)task, subject, false, true, *loc); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeRefreshAppearance(Character* c)
    {
        __try
        {
            AppearanceBase* a = c->getAppearance();
            if (!a) return false;
            a->notifyDirty();
            a->reload();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // *crashed is set when the engine threw (as opposed to returning NULL).
    Item* safeCreateItem(GameData* gd, GameData* man, GameData* mat, const hand* h, int level, bool* crashed)
    {
        *crashed = false;
        __try { return ou->theFactory->createItem(gd, *h, man, mat, level, NULL); }
        __except (EXCEPTION_EXECUTE_HANDLER) { *crashed = true; return NULL; }
    }
    bool safeEquip(Character* c, Item* it)
    {
        __try
        {
            if (!c->giveItem(it, false, true)) return false;
            if (!it->isEquipped && c->inventory) c->inventory->equipItem(it);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeRemoveItem(Character* c, Item* it)
    {
        __try { return c->inventory && c->inventory->removeItemAutoDestroy(it, it->quantity > 0 ? it->quantity : 1); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Limbs (RobotLimbs::Limb order: left arm, right arm, left leg, right leg).
    bool safeLimbs(Character* c, int* states, Item** items)
    {
        __try
        {
            RobotLimbs* rl = c->medical.robotLimbs;
            if (!rl) return false;
            for (int i = 0; i < 4; ++i) { states[i] = (int)rl->states[i]; items[i] = rl->items[i]; }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeAmputate(Character* c, int limb)
    {
        __try { c->medical.amputate((RobotLimbs::Limb)limb, false, Ogre::Vector3::ZERO); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeCrush(Character* c, int limb)
    {
        __try { c->medical.crushLimb((RobotLimbs::Limb)limb); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeSetRobotLimb(Character* c, int limb, Item* it)
    {
        __try { c->medical.setRobotLimbItem((RobotLimbs::Limb)limb, it, false); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // The owner stopped a task / a fight: the ghost must let go too (otherwise it stays seated, at
    // its machine or in its fight while its owner walks away: frozen, then teleported).
    bool safeReleaseOrders(Character* c, const Ogre::Vector3* at)
    {
        __try
        {
            c->clearAllAIGoals();
            c->addOrder(NULL, IDLE, NULL, false, true, *at);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Other players' characters read "Name [Player]" and show their name tag, so everybody knows
    // whose squad is whose.
    bool safeLabel(Character* c, const std::string* name)
    {
        __try { c->setName(*name); c->setNameTagVisible(true); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    RootObject* safeSpawn(Faction* f, const Ogre::Vector3* p, GameData* tmpl, RootObjectContainer* container)
    {
        __try { return ou->theFactory->createRandomCharacter(f, *p, container, tmpl, NULL, 1.0f); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    Platoon* safeNewPlatoon(Faction* f, GameData* squadTemplate, const Ogre::Vector3* p)
    {
        __try { return f->createNewEmptyActivePlatoon(squadTemplate, true, *p); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    ActivePlatoon* safeActive(Platoon* p)
    {
        __try { return p->getActivePlatoon(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    // GameWorld::destroy is the engine's real removal (Faction::destroyObject only detaches).
    bool safeDestroy(Character* c)
    {
        __try { return ou->destroy(c, false, "kenshimp"); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeMirrorHealth(Character* c, const float* flesh, int n, float blood)
    {
        __try
        {
            MedicalSystem& m = c->medical;
            for (int i = 0; i < n && i < (int)m.anatomy.size(); ++i)
            {
                MedicalSystem::HealthPartStatus* p = m.anatomy[i];
                if (p->flesh != flesh[i]) { p->flesh = flesh[i]; p->updateDerivedHealths(); }
            }
            m.blood = blood;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    std::string sidOf(GameData* d) { return d ? d->stringID : std::string(); }
    GameData* dataOf(const std::string& sid) { return sid.empty() ? NULL : ou->gamedata.getData(sid); }

    // Tasks a ghost may copy: mostly visual, and any side effect lands on replicated state
    // (building progress, health, stats) that the owner overwrites anyway.
    bool isMirroredTask(int t)
    {
        switch (t)
        {
        case BUILD: case JOB_BUILDER: case ADD_MATERIALS_TO_BUILDING: case REPAIR:
        case CROUCH: case STAND_UP: case GET_UP_STAND_UP:
        case OPERATE_MACHINERY: case PRETEND_TO_OPERATE_MACHINERY: case OPERATE_AUTOMATIC_MACHINERY:
        case USE_TRAINING_DUMMY: case USE_BED: case USE_BED_ORDER: case SLEEP_ON_FLOOR:
        case USE_TURRET: case MAN_A_TURRET: case MAN_A_TURRET_PLAYER_JOB: case MAN_A_TURRET_ON_BUILDING:
        case SIT_ON_THRONE: case SIT_AROUND: case HOLD_POSITION:
        case JOB_MEDIC: case JOB_REPAIR_ROBOT: case UNJAM_MACHINE: case UNJAM_ALL_MACHINES:
        case FILL_MACHINE: case EMPTY_MACHINE_OUTPUTS: case EMPTYING_MACHINE:
        case AUTO_LABOURING_MINES: case AUTO_LABOURING_MINES_PRETEND:
            return true;
        default:
            return false;
        }
    }

    // Attack orders a ghost may receive from its owner's combat state (anything else, e.g. carry or
    // cage, would be run on its combat target, possibly one of our characters).
    bool isAttackTask(int t)
    {
        switch (t)
        {
        case MELEE_ATTACK: case FOCUSED_MELEE_ATTACK: case UNPROVOKED_FOCUSED_MELEE_ATTACK: case MELEE_ATTACK_ANIMAL:
        case SHOOT_AT_TARGET: case RANGED_ATTACK: case RANGED_ATTACK_FOCUSED: case RANGED_ATTACK_FOCUSED_UNPROVOKED:
            return true;
        default:
            return false;
        }
    }

    // ------------------------------------------------------------------ appearance, gear, stats
    AppearanceBlob captureAppearance(GameData* d)
    {
        AppearanceBlob b;
        if (!d) return b;
        for (auto it = d->sdata.begin(); it != d->sdata.end(); ++it) b.s.push_back(std::make_pair(it->first, it->second));
        for (auto it = d->idata.begin(); it != d->idata.end(); ++it) b.i.push_back(std::make_pair(it->first, (int32_t)it->second));
        for (auto it = d->fdata.begin(); it != d->fdata.end(); ++it) b.f.push_back(std::make_pair(it->first, it->second));
        for (auto it = d->bdata.begin(); it != d->bdata.end(); ++it) b.b.push_back(std::make_pair(it->first, (uint8_t)(it->second ? 1 : 0)));
        for (auto it = d->objectReferences.begin(); it != d->objectReferences.end(); ++it)
        {
            std::vector<AppearanceBlob::Ref> refs;
            for (size_t k = 0; k < it->second.size(); ++k)
            {
                const GameDataReference& g = it->second[k];
                AppearanceBlob::Ref r; r.sid = g.sid; r.v0 = g.values.value[0]; r.v1 = g.values.value[1]; r.v2 = g.values.value[2];
                refs.push_back(r);
            }
            b.lists.push_back(std::make_pair(it->first, refs));
        }
        return b;
    }

    // Same CRT (msvcr100) and Ogre allocator as the game, so writing into its maps is safe.
    void applyAppearance(GameData* d, const AppearanceBlob& b)
    {
        for (size_t k = 0; k < b.s.size(); ++k) d->sdata[b.s[k].first] = b.s[k].second;
        for (size_t k = 0; k < b.i.size(); ++k) d->idata[b.i[k].first] = b.i[k].second;
        for (size_t k = 0; k < b.f.size(); ++k) d->fdata[b.f[k].first] = b.f[k].second;
        for (size_t k = 0; k < b.b.size(); ++k) d->bdata[b.b[k].first] = b.b[k].second != 0;
        for (size_t k = 0; k < b.lists.size(); ++k)
        {
            const std::string& name = b.lists[k].first;
            d->clearList(name);
            const std::vector<AppearanceBlob::Ref>& refs = b.lists[k].second;
            for (size_t n = 0; n < refs.size(); ++n) d->addToList(name, refs[n].sid, refs[n].v0, refs[n].v1, refs[n].v2);
        }
    }

    bool isProsthetic(Item* it) { return it->data && it->data->type == LIMB_REPLACEMENT; }

    std::vector<ItemRef> captureEquipment(Character* c)
    {
        std::vector<ItemRef> out;
        if (!c->inventory) return out;
        lektor<Item*> weapons, armour;
        c->inventory->getEquippedWeapons(weapons);
        c->inventory->getEquippedArmour(armour);
        const lektor<Item*>* lists[2] = { &weapons, &armour };
        for (int l = 0; l < 2; ++l)
            for (uint32_t k = 0; k < lists[l]->size(); ++k)
            {
                Item* it = (*lists[l])[k];
                if (!it || isProsthetic(it)) continue;   // limbs travel in the limbs trailer
                ItemRef r; r.item = sidOf(it->data); r.manufacturer = sidOf(it->manufacturerData); r.material = sidOf(it->materialData);
                if (!r.item.empty()) out.push_back(r);
            }
        return out;
    }

    // Skills/attributes: the contiguous float block CharStats::_strength .. polearms.
    float* statsBlock(Character* c, int& count)
    {
        CharStats* st = c->stats;
        if (!st) { count = 0; return NULL; }
        count = (int)((&st->polearms - &st->_strength) + 1);
        return &st->_strength;
    }

    // ------------------------------------------------------------------ local squad
    struct LocalChar
    {
        hand h;               // validates that ptr is still alive
        Character* ptr;
        uint32_t id;
        uint32_t appearanceHash, equipmentHash, statsHash, inventoryHash, backpackHash;
        LocalChar() : ptr(NULL), id(0), appearanceHash(0), equipmentHash(0), statsHash(0), inventoryHash(0), backpackHash(0) {}
    };
    std::vector<LocalChar> g_local;
    uint32_t g_nextLocal = 1;
    DWORD g_lastState = 0, g_lastSpawn = 0, g_lastLooks = 0;
    bool g_forceLooks = false;

    LocalChar* findLocal(uint32_t id)
    {
        for (size_t i = 0; i < g_local.size(); ++i) if (g_local[i].id == id) return &g_local[i];
        return NULL;
    }

    void refreshLocalCharacters()
    {
        // Our ids carry our player number: after a reconnect under another number (or hosting
        // after having been a client), the old ids would be refused by everyone. Re-key them.
        if (ready())
        {
            uint8_t me = g_session.localId();
            for (size_t k = 0; k < g_local.size(); ++k)
                if (netIdOwner(g_local[k].id) != me)
                {
                    uint32_t old = g_local[k].id;
                    Lock l;
                    g_local[k].id = makeNetId(me, g_nextLocal++);
                    g_local[k].appearanceHash = g_local[k].equipmentHash = g_local[k].statsHash = 0;
                    g_local[k].inventoryHash = g_local[k].backpackHash = 0;
                    log("local character %08x is now %08x (player number changed)", old, g_local[k].id);
                }
        }
        const lektor<Character*>& list = ou->player->playerCharacters;
        std::vector<bool> seen(g_local.size(), false);
        for (uint32_t i = 0; i < list.size(); ++i)
        {
            Character* c = list[i];
            if (!c) continue;
            // Match by pointer: comparing hands frame to frame proved unreliable.
            bool known = false;
            for (size_t k = 0; k < g_local.size(); ++k)
                if (g_local[k].ptr == c) { seen[k] = true; known = true; break; }
            if (known) continue;
            LocalChar lc; lc.h = hand(c); lc.ptr = c; lc.id = makeNetId(g_session.localId(), g_nextLocal++);
            { Lock l; g_local.push_back(lc); }
            seen.push_back(true);
            log("tracking local character %08x (%s)", lc.id, c->getName().c_str());
        }
        for (size_t k = g_local.size(); k-- > 0;)
        {
            if (seen[k] && g_local[k].h.getCharacter() == g_local[k].ptr) continue;
            log("local character %08x gone", g_local[k].id);
            ByteWriter w; w.u32(g_local[k].id);
            g_session.send(MSG_ENTITY_DESPAWN, w.data);
            Lock l;
            g_local.erase(g_local.begin() + k);
        }
    }

    void sendSpawns()
    {
        for (size_t k = 0; k < g_local.size(); ++k)
        {
            Character* c = g_local[k].h.getCharacter();
            if (!c) continue;
            EntitySpawn sp;
            sp.netId = g_local[k].id;
            sp.kind = KIND_CHARACTER;
            sp.gameDataName = sidOf(c->data);
            sp.displayName = c->getName();
            Ogre::Vector3 p = c->getPosition();
            sp.x = p.x; sp.y = p.y; sp.z = p.z;
            ByteWriter w; sp.write(w);
            g_session.send(MSG_ENTITY_SPAWN, w.data);
        }
    }

    void sendLooks(bool force)
    {
        for (size_t k = 0; k < g_local.size(); ++k)
        {
            LocalChar& lc = g_local[k];
            Character* c = lc.h.getCharacter();
            if (!c) continue;

            if (g_cfg.syncAppearance)
            {
                Bytes b = cs_appearanceMsg(c, lc.id);
                uint32_t h = hashBytes(b);
                if (force || h != lc.appearanceHash) { lc.appearanceHash = h; g_session.send(MSG_APPEARANCE, b); }
            }
            if (g_cfg.syncEquipment)
            {
                Bytes b = cs_equipmentMsg(c, lc.id);
                uint32_t h = hashBytes(b);
                if (force || h != lc.equipmentHash) { lc.equipmentHash = h; g_session.send(MSG_EQUIPMENT, b); }
                Bytes inv = items_inventoryMsg(CONTAINER_CHARACTER, lc.id, c->inventory);
                uint32_t hi = hashBytes(inv);
                if (force || hi != lc.inventoryHash) { lc.inventoryHash = hi; g_session.send(MSG_INVENTORY, inv); }
                Bytes bp = items_backpackMsg(lc.id, c);
                uint32_t hb = hashBytes(bp);
                if (!bp.empty() && (force || hb != lc.backpackHash)) g_session.send(MSG_INVENTORY, bp);
                lc.backpackHash = hb;
            }
            Bytes b = cs_statsMsg(c, lc.id);
            uint32_t h = hashBytes(b);
            if (!b.empty() && (force || h != lc.statsHash)) { lc.statsHash = h; g_session.send(MSG_STATS, b); }
        }
    }

    // Velocity by finite difference of successive captures (smoothed), keyed by net id.
    // Timed with the performance counter: GetTickCount's ~16 ms steps make 50 ms deltas noisy.
    struct PrevPos { Ogre::Vector3 p, v; double t; };
    std::map<uint32_t, PrevPos> g_prevPos;

    double preciseMs()
    {
        static LARGE_INTEGER freq = { 0 };
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
    }

    // Millisecond clock with sub-millisecond resolution, on the same epoch as GetTickCount (which
    // only advances in ~15.6 ms steps and made the playback target move in jerks).
    double preciseTick()
    {
        static double offset = -1;
        double q = preciseMs();
        if (offset < 0) offset = (double)GetTickCount() - q;
        return q + offset;
    }

    Ogre::Vector3 velocityOf(uint32_t id, const Ogre::Vector3& p)
    {
        double now = preciseMs();
        if (g_prevPos.size() > 4000)   // forget entities not captured for a while
            for (std::map<uint32_t, PrevPos>::iterator it = g_prevPos.begin(); it != g_prevPos.end();)
                if (now - it->second.t > 10000) g_prevPos.erase(it++); else ++it;
        std::map<uint32_t, PrevPos>::iterator it = g_prevPos.find(id);
        if (it == g_prevPos.end()) { PrevPos pp; pp.p = p; pp.v = Ogre::Vector3::ZERO; pp.t = now; g_prevPos[id] = pp; return pp.v; }
        PrevPos& pp = it->second;
        double dt = now - pp.t;
        if (dt < 20) return pp.v;                    // too close to measure, keep last
        Ogre::Vector3 v = (p - pp.p) * (float)(1000.0 / dt);
        // Stale sample or teleport. Fast runners reach ~45 u/s in Kenshi (measured), so the
        // teleport threshold must stay well above that.
        if (dt > 1500 || v.length() > 100.f) v = Ogre::Vector3::ZERO;
        pp.v = pp.v * 0.3f + v * 0.7f;
        pp.p = p; pp.t = now;
        return pp.v;
    }

    void captureState(Character* c, uint32_t id, EntityState& s)
    {
        s.netId = id;
        Ogre::Vector3 p = c->getPosition();
        Ogre::Quaternion q = c->getOrientation();
        s.x = p.x; s.y = p.y; s.z = p.z;
        Ogre::Vector3 v = velocityOf(id, p);
        s.vx = v.x; s.vy = v.y; s.vz = v.z;
        int fl = c->getFloor();
        s.floor = (int8_t)(fl < 0 ? 0 : (fl > 20 ? 20 : fl));
        s.qx = q.x; s.qy = q.y; s.qz = q.z; s.qw = q.w;
        MedicalSystem& m = c->medical;
        s.health = m.getOverallHealthRating();
        s.blood = m.blood;
        for (uint32_t i = 0; i < m.anatomy.size() && i < 32; ++i) s.flesh.push_back(m.anatomy[i]->flesh);

        bool fighting = c->isInCombatMode(true, true);
        s.flags = (c->isDead() ? EntityState::DEAD : 0) | (m.isUnconcious() ? EntityState::UNCONSCIOUS : 0) |
                  (fighting ? EntityState::IN_COMBAT : 0);
        s.moveSpeed = (uint8_t)c->getMovementSpeedOrders();
        if (c->getMovement() && c->getMovement()->isRunning()) s.flags |= EntityState::RUNNING;

        if (fighting)
            if (CombatClass* cc = c->getCombatClass())
                s.combatTarget = makeTargetRef(cc->_getAttackTarget().getRootObjectBase());

        if (c->body)
            if (Tasker* t = c->body->getCurrentAction())
            {
                s.task = (uint16_t)t->key();
                s.taskSubject = makeTargetRef(t->subject.getRootObjectBase());
                Ogre::Vector3 loc = t->getLocation();
                s.tx = loc.x; s.ty = loc.y; s.tz = loc.z;
            }
    }

    void sendStates()
    {
        if (g_local.empty()) return;
        ByteWriter body;
        uint16_t n = 0;
        for (size_t k = 0; k < g_local.size(); ++k)
        {
            Character* c = g_local[k].h.getCharacter();
            if (!c) continue;
            EntityState s;
            captureState(c, g_local[k].id, s);
            s.write(body);
            ++n;
            static DWORD lastSpeedLog = 0;
            if (g_cfg.debugKeys && k == 0 && GetTickCount() - lastSpeedLog > 2000)
            {
                lastSpeedLog = GetTickCount();
                log("own %08x: engine speed %.1f, measured %.1f u/s, flags %d, moveSpeed %d, pos (%.1f,%.1f,%.1f)", g_local[k].id,
                    c->getMovementSpeed(), sqrtf(s.vx * s.vx + s.vz * s.vz), (int)s.flags, (int)s.moveSpeed, s.x, s.y, s.z);
            }
        }
        if (!n) return;
        ByteWriter w; w.u32(GetTickCount()); w.u16(n); w.bytes(body.data);
        g_session.send(MSG_ENTITY_STATE, w.data);
    }

    void applyRealDamage(Character* c, MedicalSystem::HealthPartStatus* part, const Damages& dmg);

    Character* localByNetId(uint32_t id);

    void applyIncomingDamage(const DamageMsg& d)
    {
        Character* c = localByNetId(d.victimNetId);   // our squad member or (host) a world NPC
        if (!c || d.bodyPart >= c->medical.anatomy.size()) return;
        // Sanity check against modified clients: a melee blow (attacker known) must come from
        // someone standing next to the victim, as seen here.
        if (d.attackerNetId)
            if (Character* a = chars_byNetId(d.attackerNetId))
            {
                float dist = a->getPosition().distance(c->getPosition());
                if (dist > MELEE_SANITY_RANGE)
                {
                    static DWORD lastWarn = 0;
                    if (GetTickCount() - lastWarn > 5000) { lastWarn = GetTickCount(); log("ignored melee damage from %08x at %.0f units (too far)", d.attackerNetId, dist); }
                    return;
                }
            }
        Damages dmg(d.cut, d.blunt, d.pierce, 1.0f, 0.0f);
        applyRealDamage(c, c->medical.anatomy[d.bodyPart], dmg);
    }

    // ------------------------------------------------------------------ ghosts
    long g_floorFixes = 0, g_floorGiveUps = 0;   // ghost floor corrections (stats)
    struct Ghost
    {
        hand h;
        EntityState last;
        std::vector<hand> givenItems;   // gear we created on this ghost
        bool hasPendingAppearance, hasPendingEquipment, hasPendingStats;
        AppearanceBlob pendingAppearance;
        std::vector<ItemRef> pendingEquipment;
        bool hasLimbs;                  // prosthetics / missing limbs sent with the equipment
        uint8_t limbState[4];
        ItemRef limbItem[4];
        std::vector<float> pendingStats;
        float pendingHunger;            // owner's hunger (-1: not sent)
        int pendingRanged;              // owner's "ranged" toggle (-1: not sent)
        Character* combatTarget;        // what we last told it to attack
        int mirroredTask;               // TaskType we last ordered (-1 none)
        TargetRef mirroredSubject;
        int lastSpeed;
        // --- smooth movement (see core/Interp.h)
        enum Mode { MOVE, COMBAT, TASK, FROZEN };
        Mode mode;
        InterpBuffer interp;
        PlaybackClock playback;
        SmoothFollower follow;
        double intervalMs;              // measured spacing of its snapshots (players 50, far NPCs 500)
        double lastSnapT;
        DWORD lastMoveCmd, lastFrame;
        // --- render layer: what is drawn, decoupled from the simulated body
        bool hasVisual;
        Ogre::Vector3 visualPos;        // smoothed path position (this frame)
        Ogre::Quaternion visualRot;     // owner's interpolated orientation
        float blend;                    // 0 = draw the body where the engine has it, 1 = on the path
        Ogre::Vector3 lastDrawn;        // what we set, to measure engine overrides
        Ogre::Vector3 lastBody;         // body position last frame (freeze measurement)
        Ogre::Vector3 lastDest;         // last destination given to the engine
        bool hasDest;
        std::deque<Ogre::Vector3> trail;  // where the owner actually went (walkable waypoints)
        double lastFrameP, lastMoveAt;  // precise clock: previous frame, last time the body moved
        Ogre::Vector3 prevFrameBody;    // body position last frame (its real speed)
        DWORD settleUntil;              // after a teleport: no snap/unblock checks until then
        DWORD lastSpeedCmd;             // speed matching: last command
        float speedCmd, baseMaxSpeed;   // value sent; the body's own max (restored when slow)
        double freezeMs;                // current streak: owner moving, body not
        DWORD stuckSince;               // owner still, ghost still far: since when
        Ogre::Vector3 stuckTarget;      // last target we unstuck it to (no teleport loop on a blocked spot)
        int floorTries; int floorFor;   // floor corrections made for the owner's current floor
        Ghost() : floorTries(0), floorFor(-1), hasPendingAppearance(false), hasPendingEquipment(false), hasLimbs(false), hasPendingStats(false), pendingHunger(-1.f), pendingRanged(-1),
                  combatTarget(NULL), mirroredTask(-1), lastSpeed(-1), mode(MOVE), intervalMs(100),
                  lastSnapT(0), lastMoveCmd(0), lastFrame(0), hasVisual(false), visualPos(Ogre::Vector3::ZERO),
                  visualRot(Ogre::Quaternion::IDENTITY), blend(0), lastDrawn(Ogre::Vector3::ZERO),
                  stuckSince(0), stuckTarget(Ogre::Vector3::ZERO), lastBody(Ogre::Vector3::ZERO), freezeMs(0), lastDest(Ogre::Vector3::ZERO), hasDest(false),
                  lastFrameP(0), lastMoveAt(0), prevFrameBody(Ogre::Vector3::ZERO), settleUntil(0), lastSpeedCmd(0), speedCmd(0), baseMaxSpeed(0) {}
    };
    std::map<uint8_t, ClockSync> g_clocks;   // per remote player: its clock -> ours
    long g_released = 0;                     // ghosts released from a finished task / fight (stats)

    // Host: warn (never block) when a player's characters move impossibly fast (MovementGuard.h).
    MovementGuard g_moveGuard;
    std::map<uint8_t, DWORD> g_lastMoveWarn;
    void checkMovement(uint8_t player, const EntityState& s, uint32_t senderMs)
    {
        if (s.flags & (EntityState::DEAD | EntityState::RAGDOLL)) return;   // bodies thrown around
        float amount = 0;
        MovementGuard::Verdict v = g_moveGuard.observe(s.netId, senderMs, s.x, s.z, &amount);
        if (v == MovementGuard::OK) return;
        DWORD now = GetTickCount();
        DWORD& last = g_lastMoveWarn[player];
        if (last && now - last < 60000) return;
        last = now;
        const char* what = v == MovementGuard::JUMP ? "jumped %.0f units" : "moved at %.0f units/s";
        char buf[96]; sprintf_s(buf, what, amount);
        log("suspicious movement: %s's character %08x %s", playerName(player).c_str(), s.netId, buf);
        std::string shown = TF(what, amount);
        showMessage("KenshiMP: " + TF("%s's character %s (speed hack or lag?).", playerName(player).c_str(), shown.c_str()));
    }
    std::map<uint32_t, Ghost> g_ghosts;
    int g_npcGhostsSpawned = 0;
    // Engine protection: a buggy or malicious peer cannot flood us with characters.
    const int MAX_GHOSTS_PER_PLAYER = 400;   // squads are <= ~256; the host also streams NPCs

    int ghostCount(uint8_t owner)
    {
        int n = 0;
        for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
            if (netIdOwner(it->first) == owner) ++n;
        return n;
    }
    std::map<Character*, uint32_t> g_ghostByChar;   // rebuilt every tick, read by the hooks (g_lock)
    std::map<Character*, bool> g_ghostFighting;      // ghosts currently mirroring a fight (g_lock)
    volatile bool g_applyingSync = false;          // lets our own damage through the hooks

    void rebuildGhostIndex()
    {
        std::map<Character*, uint32_t> idx;
        std::map<Character*, bool> fighting;
        for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
            if (Character* c = it->second.h.getCharacter())
            {
                idx[c] = it->first;
                if (it->second.last.flags & EntityState::IN_COMBAT) fighting[c] = true;
            }
        Lock l;
        g_ghostByChar.swap(idx);
        g_ghostFighting.swap(fighting);
    }

    bool ghostFighting(Character* c)
    {
        Lock l;
        return g_ghostFighting.find(c) != g_ghostFighting.end();
    }

    uint32_t ghostIdOf(Character* c)
    {
        Lock l;
        std::map<Character*, uint32_t>::iterator it = g_ghostByChar.find(c);
        return it == g_ghostByChar.end() ? 0 : it->second;
    }

    void applyEquipment(Ghost& g, Character* c, const std::vector<ItemRef>& eq)
    {
        struct Mute { Mute() { ++g_itemsMute; ++g_itemsEpoch; } ~Mute() { --g_itemsMute; } } mute;
        // Strip everything it wears (including the template's default clothes), then dress it.
        if (c->inventory)
        {
            lektor<Item*> weapons, armour;
            c->inventory->getEquippedWeapons(weapons);
            c->inventory->getEquippedArmour(armour);
            for (uint32_t k = 0; k < weapons.size(); ++k) if (weapons[k]) safeRemoveItem(c, weapons[k]);
            for (uint32_t k = 0; k < armour.size(); ++k) if (armour[k] && !isProsthetic(armour[k])) safeRemoveItem(c, armour[k]);
        }
        g.givenItems.clear();

        for (size_t k = 0; k < eq.size(); ++k)
        {
            GameData* gd = dataOf(eq[k].item);
            if (!gd) { log("unknown item '%s' (missing mod?)", eq[k].item.c_str()); continue; }
            hand none;
            GameData* man = dataOf(eq[k].manufacturer);
            GameData* mat = dataOf(eq[k].material);
            bool crashed = false;
            Item* it = NULL;
            // Weapons need (manufacturer, weapon) instead of (weapon, manufacturer): tried first.
            if (gd->type == WEAPON && man) it = safeCreateItem(man, gd, mat, &none, -1, &crashed);
            if (!it) it = safeCreateItem(gd, man, mat, &none, -1, &crashed);
            if (!it) { log("createItem('%s' type=%d) %s", eq[k].item.c_str(), (int)gd->type, crashed ? "threw" : "returned NULL"); continue; }
            if (safeEquip(c, it)) g.givenItems.push_back(hand(it));
            else log("could not equip '%s' on ghost", eq[k].item.c_str());
        }
    }

    // Missing, crushed and robotic limbs, as the owner has them. A limb the ghost lost cannot grow
    // back (neither can the owner's), so only ORIGINAL -> anything and REPLACED <-> REPLACED move.
    void applyLimbs(Ghost& g, Character* c)
    {
        struct Mute { Mute() { ++g_itemsMute; ++g_itemsEpoch; } ~Mute() { --g_itemsMute; } } mute;
        int have[4]; Item* items[4];
        if (!safeLimbs(c, have, items))
            for (int i = 0; i < 4; ++i) { have[i] = LIMB_ORIGINAL; items[i] = NULL; }   // never lost a limb yet
        for (int i = 0; i < 4; ++i)
            if (g.limbState[i] != have[i]) log("limbs: ghost limb %d %d -> %d", i, have[i], (int)g.limbState[i]);
        for (int i = 0; i < 4; ++i)
        {
            int want = g.limbState[i];
            ItemRef cur;
            if (items[i]) { cur.item = sidOf(items[i]->data); cur.manufacturer = sidOf(items[i]->manufacturerData); cur.material = sidOf(items[i]->materialData); }
            if (want == have[i] && (want != LIMB_REPLACED || cur == g.limbItem[i])) continue;
            if (want == LIMB_STUMP)
            {
                if (have[i] == LIMB_REPLACED) safeSetRobotLimb(c, i, NULL);
                else if (have[i] == LIMB_ORIGINAL) safeAmputate(c, i);
            }
            else if (want == LIMB_CRUSHED && have[i] == LIMB_ORIGINAL) safeCrush(c, i);
            else if (want == LIMB_REPLACED)
            {
                GameData* gd = dataOf(g.limbItem[i].item);
                if (!gd) { log("unknown prosthetic '%s' (missing mod?)", g.limbItem[i].item.c_str()); continue; }
                if (have[i] == LIMB_ORIGINAL) safeAmputate(c, i);
                hand none; bool crashed = false;
                Item* it = safeCreateItem(gd, dataOf(g.limbItem[i].manufacturer), dataOf(g.limbItem[i].material), &none, -1, &crashed);
                if (!it || !safeSetRobotLimb(c, i, it)) log("could not fit prosthetic '%s' on ghost", g.limbItem[i].item.c_str());
            }
        }
    }

    void flushPending(uint32_t id, Ghost& g, Character* c)
    {
        if (g.hasPendingAppearance)
        {
            g.hasPendingAppearance = false;
            if (GameData* d = c->getAppearanceData())
            {
                applyAppearance(d, g.pendingAppearance);
                if (!safeRefreshAppearance(c)) log("appearance refresh failed");
            }
        }
        if (g.hasPendingEquipment)
        {
            g.hasPendingEquipment = false;
            applyEquipment(g, c, g.pendingEquipment);
            if (g.hasLimbs) applyLimbs(g, c);
            items_redressed(id);   // (the ghost index may not list a body that was just spawned)
        }
        if (g.hasPendingStats)
        {
            g.hasPendingStats = false;
            int n = 0;
            if (float* st = statsBlock(c, n))
                for (int i = 0; i < n && i < (int)g.pendingStats.size(); ++i) st[i] = g.pendingStats[i];
            if (g.pendingRanged >= 0 && c->stats && c->stats->rangedMode != (g.pendingRanged == 1))
            {
                c->stats->rangedMode = g.pendingRanged == 1;
                if (g_cfg.debugKeys) log("ghost %08x ranged mode %s (as its owner)", id, g.pendingRanged ? "on" : "off");
            }
            if (g.pendingHunger >= 0.f)
            {
                if (g_cfg.debugKeys && fabsf(c->medical.hunger - g.pendingHunger) >= 0.05f)
                    log("ghost %08x hunger %.2f -> %.2f", id, c->medical.hunger, g.pendingHunger);
                c->medical.hunger = g.pendingHunger;   // shown in its health panel
            }
        }
    }

    float hdist(const Ogre::Vector3& a, const Ogre::Vector3& b) { float dx = a.x - b.x, dz = a.z - b.z; return sqrtf(dx * dx + dz * dz); }

    // Ghosts live in engine squads (the factory needs a container): one per (owner, faction).
    std::map<std::pair<uint8_t, Faction*>, Platoon*> g_ghostSquads;

    // ------------------------------------------------------------------ world map
    // Other players' squads appear on the game's own map (same markers and colours as any squad)
    // while we are not at war with them: enemies stay in the fog of war.
    std::set<Platoon*> g_onMap;
    bool safeMapSquad(Platoon* p, bool add)
    {
        __try
        {
            ManagementScreen* ms = ManagementScreen::getSingleton();
            if (!ms || !ms->mapScreen) return false;
            if (add) ms->mapScreen->addSquad(p); else ms->mapScreen->removeSquad(p);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void mapRemove(Platoon* p)
    {
        if (g_onMap.erase(p)) safeMapSquad(p, false);
    }
    void updateMapSquads()
    {
        Faction* mine = ou && ou->player ? ou->player->getFaction() : NULL;
        if (!mine || !mine->relations) return;
        for (std::map<std::pair<uint8_t, Faction*>, Platoon*>::iterator it = g_ghostSquads.begin(); it != g_ghostSquads.end(); ++it)
        {
            Platoon* p = it->second;
            Faction* f = it->first.second;
            if (!p || f != factionFor(it->first.first)) continue;      // NPC squads of the host: not shown
            bool want = g_cfg.playersOnMap && mine->relations->getFactionRelation(f) >= 0.f;
            bool shown = g_onMap.count(p) != 0;
            if (want && !shown)
            {
                if (safeMapSquad(p, true)) { g_onMap.insert(p); log("map: squad of player %d shown", it->first.first); }
                else log("map: could not show squad of player %d", it->first.first);
            }
            else if (!want && shown) { mapRemove(p); log("map: squad of player %d hidden", it->first.first); }
        }
    }

    ActivePlatoon* ghostSquadFor(uint8_t owner, Faction* f, const Ogre::Vector3& pos)
    {
        std::pair<uint8_t, Faction*> key(owner, f);
        std::map<std::pair<uint8_t, Faction*>, Platoon*>::iterator it = g_ghostSquads.find(key);
        if (it != g_ghostSquads.end())
            if (ActivePlatoon* a = safeActive(it->second)) return a;
        // Reuse the local player's squad template: any valid squad GameData will do.
        GameData* squadTemplate = ou->player->currentPlatoon ? ou->player->currentPlatoon->data : NULL;
        Platoon* p = safeNewPlatoon(f, squadTemplate, &pos);
        if (!p) { log("createNewEmptyActivePlatoon failed"); return NULL; }
        g_ghostSquads[key] = p;
        return safeActive(p);
    }

    void spawnGhost(uint8_t owner, const EntitySpawn& sp)
    {
        Ghost& g = g_ghosts[sp.netId];
        if (g.h.getCharacter()) return;

        // Player squads go to the player's mirror faction, world NPCs to their real faction
        // (so relations, bounties and town allegiances behave as on the host).
        // Only the host's world NPCs carry a faction of their own; a player's characters always go to
        // that player's mirror faction (never ours, whatever the message says).
        bool worldNpc = owner == HOST_ID && isNpcNetId(sp.netId) && !sp.factionSid.empty();
        if (!worldNpc && !sp.factionSid.empty()) log("spawn %08x from %s: faction '%s' ignored (players' characters go to their own faction)", sp.netId, playerName(owner).c_str(), sp.factionSid.c_str());
        Faction* f = worldNpc ? ou->factionMgr->getFactionByStringID(sp.factionSid) : factionFor(owner);
        if (worldNpc && f && (f->isThePlayer() || f == ou->player->getFaction())) f = NULL;
        GameData* tmpl = dataOf(sp.gameDataName);
        if (!f || !tmpl)
        {
            log("cannot spawn ghost %08x (template '%s', faction '%s')", sp.netId, sp.gameDataName.c_str(), sp.factionSid.c_str());
            return;
        }

        Ogre::Vector3 pos(sp.x, sp.y, sp.z);
        // Far from all our characters, the zone is not loaded here: a character created there
        // would be unloaded again at once (spawn/destroy churn). The owner re-announces its
        // spawns every few seconds, so the ghost appears when we get close.
        {
            float nearestLocal = 1e30f;
            for (size_t i = 0; i < g_local.size(); ++i)
                if (Character* lc = g_local[i].h.getCharacter())
                {
                    float d = lc->getPosition().distance(pos);
                    if (d < nearestLocal) nearestLocal = d;
                }
            // Keep the (bodiless) entry: it holds appearance/gear already received for it.
            if (!g_local.empty() && nearestLocal > SPAWN_RANGE) return;
        }
        ActivePlatoon* squad = ghostSquadFor(owner, f, pos);
        if (!squad) { log("no ghost squad for player %d", (int)owner); return; }
        Character* c = static_cast<Character*>(safeSpawn(f, &pos, tmpl, squad));
        if (!c) { log("factory returned no character for %08x", sp.netId); return; }
        g.h = hand(c);
        g.last.netId = sp.netId;
        g.combatTarget = NULL; g.mirroredTask = -1; g.lastSpeed = -1;
        if (!isNpcNetId(sp.netId))
        {
            log("ghost %08x spawned (%s)", sp.netId, sp.displayName.c_str());
            std::string label = sp.displayName + " [" + playerName(owner) + "]";
            safeLabel(c, &label);
        }
        else ++g_npcGhostsSpawned;
        flushPending(sp.netId, g, c);
        rebuildGhostIndex();
    }

    Faction* safeFactionOf(Character* c)
    {
        __try { return c->getFaction(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }

    void despawnGhost(uint32_t id)
    {
        std::map<uint32_t, Ghost>::iterator it = g_ghosts.find(id);
        if (it == g_ghosts.end()) return;
        Character* c = it->second.h.getCharacter();
        Faction* f = c ? safeFactionOf(c) : NULL;
        g_ghosts.erase(it);
        rebuildGhostIndex();
        items_onDespawn(id);
        // Last ghost of its squad: the engine may free the squad with it, so forget our cached
        // pointer (off the world map first, while it still exists). A later ghost gets a new one.
        if (f)
        {
            std::pair<uint8_t, Faction*> k(netIdOwner(id), f);
            std::map<std::pair<uint8_t, Faction*>, Platoon*>::iterator sq = g_ghostSquads.find(k);
            bool last = sq != g_ghostSquads.end();
            for (std::map<uint32_t, Ghost>::iterator g = g_ghosts.begin(); g != g_ghosts.end() && last; ++g)
                if (netIdOwner(g->first) == k.first)
                    if (Character* o = g->second.h.getCharacter())
                        if (safeFactionOf(o) == f) last = false;
            if (last) { mapRemove(sq->second); g_ghostSquads.erase(sq); }
        }
        if (c && !safeDestroy(c)) log("could not destroy ghost %08x", id);
    }

    // What the ghost does (health, KO, combat, task). Where it is shown is decided every frame
    // by driveGhosts() from the interpolation buffer fed here.
    void applyGhostState(const EntityState& s, double snapLocalMs)
    {
        std::map<uint32_t, Ghost>::iterator it = g_ghosts.find(s.netId);
        if (it == g_ghosts.end()) return;   // spawn not received yet
        Ghost& g = it->second;
        Character* c = g.h.getCharacter();
        if (!c) return;
        g.last = s;

        Snapshot snap;
        snap.t = snapLocalMs;
        snap.x = s.x; snap.y = s.y; snap.z = s.z;
        snap.qx = s.qx; snap.qy = s.qy; snap.qz = s.qz; snap.qw = s.qw;
        snap.vx = s.vx; snap.vy = s.vy; snap.vz = s.vz;
        if (g.lastSnapT > 0 && snapLocalMs > g.lastSnapT)
        {
            double gap = snapLocalMs - g.lastSnapT;
            if (gap < 2000) g.intervalMs += (gap - g.intervalMs) * 0.2;   // NPC LOD changes the rate
        }
        g.lastSnapT = snapLocalMs;
        g.interp.push(snap);
        {
            Ogre::Vector3 p(s.x, s.y, s.z);
            if (g.trail.empty() || g.trail.back().distance(p) > 2.f) g.trail.push_back(p);   // 2 units apart
            while (g.trail.size() > 128) g.trail.pop_front();
        }
        g.mode = Ghost::MOVE;

        // --- health: exact per-part mirror (limping, crippled arms, bleeding look...)
        if (!s.flesh.empty() && !safeMirrorHealth(c, &s.flesh[0], (int)s.flesh.size(), s.blood))
            log("health mirror failed on %08x", s.netId);

        bool ownerDead = (s.flags & EntityState::DEAD) != 0, ownerKO = (s.flags & EntityState::UNCONSCIOUS) != 0;
        if (ownerDead && !c->isDead())
        {
            g_applyingSync = true;
            Damages kill(1000, 1000, 0, 0, 0);
            for (uint32_t i = 0; i < c->medical.anatomy.size(); ++i) c->medical.anatomy[i]->applyDamage(kill);
            g_applyingSync = false;
            g.mode = Ghost::FROZEN;
            return;
        }
        if (ownerDead || ownerKO)
        {
            if (ownerKO && !c->medical.isUnconcious()) c->medical.knockoutForceTimer(1.0f);
            g.mode = Ghost::FROZEN;
            return;
        }

        // --- walk / jog / run like the owner
        if (s.moveSpeed != g.lastSpeed) { g.lastSpeed = s.moveSpeed; safeSetSpeed(c, s.moveSpeed); }

        Ogre::Vector3 target(s.x, s.y, s.z);
        float dist = c->getPosition().distance(target);

        // --- combat: fight the same opponent, with the real combat animations
        if (s.flags & EntityState::IN_COMBAT)
        {
            g.mode = Ghost::COMBAT;
            RootObject* o = resolveTargetRef(s.combatTarget);
            Character* enemy = (s.combatTarget.kind == TargetRef::NET_CHARACTER || s.combatTarget.kind == TargetRef::WORLD_CHARACTER)
                               ? static_cast<Character*>(o) : NULL;
            if (enemy && enemy != c && enemy != g.combatTarget)
            {
                // Explicit order + attackTarget; the ghost's AI is re-enabled while it fights
                // (combat decisions live in AI::periodicUpdate, see aiPeriodic_hook).
                Ogre::Vector3 at = enemy->getPosition();
                int order = isAttackTask(s.task) ? s.task : (int)FOCUSED_MELEE_ATTACK;
                if (g_cfg.debugKeys && s.task != 0xFFFF && order != s.task) log("ghost %08x: combat order %d refused, attacks instead", s.netId, (int)s.task);
                bool ordered = safeOrder(c, order, enemy, &at);
                bool attacked = safeAttack(c, enemy);
                if (ordered || attacked) g.combatTarget = enemy;
                log("ghost %08x engages %08x (order=%d attack=%d)", s.netId, s.combatTarget.netId, (int)ordered, (int)attacked);
                g.mirroredTask = -1;
            }
            return;
        }
        if (g.combatTarget)
        {
            g.combatTarget = NULL;
            safeReleaseOrders(c, &target);
            ++g_released;
        }

        // --- tasks: build, operate a machine, sleep, sit... (whitelist)
        if (s.task != 0xFFFF && isMirroredTask(s.task) && dist < TASK_RANGE && s.vx * s.vx + s.vz * s.vz < 1.f)
        {
            g.mode = Ghost::TASK;
            if ((int)s.task != g.mirroredTask || s.taskSubject != g.mirroredSubject)
            {
                RootObject* subject = resolveTargetRef(s.taskSubject);
                Ogre::Vector3 loc(s.tx, s.ty, s.tz);
                if ((subject || s.taskSubject.kind == TargetRef::NONE) && safeOrder(c, s.task, subject, &loc))
                {
                    if (g_cfg.debugKeys) log("ghost %08x copies its owner's task %d", s.netId, (int)s.task);
                    g.mirroredTask = s.task;
                    g.mirroredSubject = s.taskSubject;
                }
            }
            return;
        }
        if (g.mirroredTask != -1)
        {
            g.mirroredTask = -1;
            safeReleaseOrders(c, &target);
            ++g_released;
        }
    }

    // Every frame: move each ghost along the interpolated, error-smoothed path of its owner.
    // The engine walks it (real walk/run animations); we steer it towards a point slightly ahead
    // on the owner's path so it never stops between two updates, and only teleport on big gaps.
    // Tracking quality (how far engine-driven ghosts lag behind their smoothed target).
    double g_trackErrSum = 0, g_trackErrMax = 0;
    long g_trackErrN = 0;
    int g_snaps = 0;
    double g_frozenMs = 0, g_movingMs = 0, g_longestFreezeMs = 0;

    const float STUCK_DISTANCE = 3.f;      // owner still, ghost further than this ...
    const DWORD STUCK_MS = 2500;           // ... for this long: placed next to him
    long g_unstucks = 0;
    const float SNAP_DISTANCE = 20.f;      // beyond this the ghost is teleported (spawn, real jumps); below it catches up by sliding
    const float LEAD_SECONDS = 0.6f;       // steer this far ahead along the velocity
    const float MIN_LEAD = 8.f;            // lead never shorter (engine arrival distance ~4)
    const float MAX_LEAD = 30.f;           // ... nor longer (corners)
    const float REPATH_NEAR = 6.f;         // next waypoint once this close to the current one
    const float REPATH_FAR = 15.f;         // ... or when the wanted destination moved this much
    const DWORD SETTLE_MS = 400;           // after a teleport: no snap/floor/stuck checks
    const float SNAP_FAR = 150.f;          // beyond this a running ghost is placed anyway
    const double UNBLOCK_HARD_MS = 1000;   // body still this long while the owner runs: placed, even if the engine thinks it walks (pushing on an obstacle)
    const DWORD UNBLOCK_COOLDOWN_MS = 1500;
    const double FREEZE_COUNT_MS = 300;    // still longer than this while the owner moves: frozen
    const float NUDGE_MAX_GAP = 8.f;       // straight-line correction only this close to the target
    const float TRAIL_REACHED = 8.f;       // waypoint considered reached (next one is given)
    const float TRAIL_LOOKAHEAD = 20.f;    // the ghost walks to the trail point this far ahead
    const float REPATH_DISTANCE = 3.f;     // new destination only if it moved this much (and see below)
    const DWORD REPATH_MAX_MS = 1000;      // ... or this long after the previous one
    const double BLOCKED_MS = 300;         // owner moving, ghost body still this long: blocked
    const double UNBLOCK_TELEPORT_MS = 800;  // real stall this long: back on the path
    long g_unblocks = 0;
    long g_trailGlides = 0;
    const float NUDGE_BASE_SPEED = 1.5f;   // units/s of correction even for a tiny gap
    const float NUDGE_GAIN = 2.5f;         // + this per unit of gap (4 units -> 11.5 u/s)
    bool g_nudgeBroken = false;
    const DWORD STEER_PERIOD_MS = 250;     // re-issue move orders at most this often (keeps momentum)

    void driveGhosts(DWORD now)
    {
        for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
        {
            Ghost& g = it->second;
            Character* c = g.h.getCharacter();
            if (!c || g.interp.empty()) continue;

            ClockSync& clock = g_clocks[netIdOwner(it->first)];
            double delay = renderDelayMs(g.intervalMs, clock.jitterMs());
            double pnow = preciseTick();
            double rt = g.playback.update(pnow, pnow - delay);
            Snapshot smp;
            if (!g.interp.sample(rt, smp)) continue;
            double frameMs = g.lastFrameP > 0 ? pnow - g.lastFrameP : 16.0;
            if (frameMs > 200) frameMs = 200;
            if (frameMs < 0) frameMs = 0;
            g.lastFrameP = pnow;
            g.lastFrame = now;
            {
                // Correction and snap distance grow with speed: at 70 u/s a fixed 2.5 u/s correction
                // and a 6-unit snap made the target jump on every sharp turn.
                float sp = sqrtf(smp.vx * smp.vx + smp.vz * smp.vz);
                g.follow.update(smp, frameMs, 2.5f + sp * 0.5f, 6.f + sp * 0.15f);
            }
            Ogre::Vector3 target(g.follow.x, g.follow.y, g.follow.z);
            g.hasVisual = true;
            g.visualPos = target;
            g.visualRot = Ogre::Quaternion(smp.qw, smp.qx, smp.qy, smp.qz);

            if (g.mode == Ghost::FROZEN) continue;
            // Horizontal error: the height is the ground's business (slopes, stairs).
            Ogre::Vector3 gpos = c->getPosition();
            float dist = sqrtf((gpos.x - target.x) * (gpos.x - target.x) + (gpos.z - target.z) * (gpos.z - target.z));
            float speedNow = sqrtf(smp.vx * smp.vx + smp.vz * smp.vz);
            if (g.mode == Ghost::MOVE && speedNow > 0.5f) { g_trackErrSum += dist; ++g_trackErrN; if (dist > g_trackErrMax) g_trackErrMax = dist; }
            // Freeze, in real time: how long since the body last moved, while its owner moves. (The
            // engine updates other factions' bodies less often than we draw: a still frame or two
            // is normal, only a long still period is a freeze.)
            if (gpos.distance(g.lastBody) > 0.01f || g.lastMoveAt <= 0) { g.lastMoveAt = pnow; g.lastBody = gpos; }
            g.freezeMs = speedNow > 2.f ? pnow - g.lastMoveAt : 0;
            if (speedNow <= 2.f) g.lastMoveAt = pnow;
            if (!isNpcNetId(it->first) && speedNow > 2.f)
            {
                g_movingMs += frameMs;
                if (g.freezeMs > FREEZE_COUNT_MS)
                {
                    g_frozenMs += frameMs;
                    if (g.freezeMs > g_longestFreezeMs) g_longestFreezeMs = g.freezeMs;
                    if (g_cfg.debugKeys && g.freezeMs >= 500 && g.freezeMs - frameMs < 500)
                        log("freeze %08x: mode %d dist %.1f target speed %.1f ghost speed %.1f | ghost (%.1f,%.1f,%.1f) target (%.1f,%.1f,%.1f)",
                            it->first, (int)g.mode, dist, speedNow, c->getMovementSpeed(), gpos.x, gpos.y, gpos.z, target.x, target.y, target.z);
                }
            }
            bool settling = now < g.settleUntil;   // just teleported: let the engine take the new position
            // Teleport only if it is really lost: the other player never sees the original, only
            // this copy, so running a little behind on the right path is invisible while a
            // teleport is not. A ghost that is running after its target is left to catch up
            // (speed boost); a stalled one, or one absurdly far, is placed.
            if (!settling && (dist > SNAP_FAR || (dist > SNAP_DISTANCE + speedNow * 0.6f && safeStalled(c))))
            {
                ++g_snaps;
                static DWORD lastSnapLog = 0;
                if (g_cfg.debugKeys && now - lastSnapLog > 2000)
                {
                    lastSnapLog = now;
                    log("snap %08x: dist %.1f ghost (%.1f,%.1f,%.1f) -> target (%.1f,%.1f,%.1f), mode %d",
                        it->first, dist, gpos.x, gpos.y, gpos.z, target.x, target.y, target.z, (int)g.mode);
                }
                safeTeleport(c, &target, g.last.floor);
                g.follow.reset(); g.combatTarget = NULL; g.mirroredTask = -1;
                g.trail.clear(); g.hasDest = false; g.settleUntil = now + SETTLE_MS; g.lastMoveAt = pnow;
                continue;
            }
            int ghostFloor = c->getFloor();
            if (ghostFloor < 0) ghostFloor = 0;              // the sender clamps the same way
            if (g.floorFor != g.last.floor) { g.floorFor = g.last.floor; g.floorTries = 0; }
            if (!settling && ghostFloor != g.last.floor && (ghostFloor > 0 || g.last.floor > 0) && dist < SNAP_DISTANCE && speedNow < 2.f && g.floorTries < 2)
            {
                // Changing floor (stairs inside a building): place it on the owner's floor. Twice at
                // most: if that floor does not exist here (building not loaded / different), the
                // ghost would otherwise be teleported again every 400 ms.
                if (++g.floorTries == 2) ++g_floorGiveUps;
                ++g_floorFixes;
                safeTeleport(c, &target, g.last.floor);
                g.follow.reset(); g.settleUntil = now + SETTLE_MS; g.lastMoveAt = pnow;
                continue;
            }
            // In a fight the AI moves it; but a fight on the run is driven like any move.
            if (g.mode == Ghost::COMBAT && speedNow <= 5.f)
            {
                if (dist > COMBAT_LEASH && now - g.lastMoveCmd > STEER_PERIOD_MS) { g.lastMoveCmd = now; safeMoveTo(c, &target); }
                continue;
            }
            if (g.mode == Ghost::TASK) continue;

            float speed = speedNow;
            // Stuck: the owner stands still but the ghost cannot reach him (blocked path, pushed
            // away by another body). Place it once; never loop on a spot the engine rejects.
            if (speed < 0.3f && dist > STUCK_DISTANCE)
            {
                if (!g.stuckSince) g.stuckSince = now;
                else if (!settling && now - g.stuckSince > STUCK_MS && target.distance(g.stuckTarget) > 1.f)
                {
                    ++g_unstucks;
                    safeTeleport(c, &target, g.last.floor);
                    g.stuckTarget = target; g.stuckSince = 0; g.follow.reset(); g.settleUntil = now + SETTLE_MS;
                    continue;
                }
            }
            else g.stuckSince = 0;
            static DWORD lastDiag = 0;
            if (g_cfg.debugKeys && now - lastDiag > 3000 && !isNpcNetId(it->first))
            {
                lastDiag = now;
                SpeedInfo si;
                if (safeSpeedInfo(c, &si))
                    log("drive %08x: target speed %.1f, offset %.2f | engine current %.1f max %.1f orders %d running %d pathOk %d pathFailed %d | owner flags %d delay %.0f ms",
                        it->first, speed, dist, si.current, si.max, si.orders, (int)si.running, (int)si.pathOk, (int)si.pathFailed, (int)g.last.flags, delay);
            }
            // Blocked while its owner moves (wedged on a corner): stop pushing in a straight line and
            // walk to the owner's exact spot. Only a long, real stall (engine stopped or no path)
            // puts it back on the path, with a cooldown: a teleport restarts the locomotion from
            // zero, so teleporting too eagerly would only feed the freeze.
            bool blocked = g_cfg.tuneUnblock && speedNow > 2.f && g.freezeMs > BLOCKED_MS;
            if (g_cfg.tuneUnblock && !settling && speedNow > 2.f && dist > 2.f && (g.freezeMs > UNBLOCK_HARD_MS || (g.freezeMs > UNBLOCK_TELEPORT_MS && safeStalled(c))))
            {
                ++g_unblocks;
                safeTeleport(c, &target, g.last.floor);
                g.follow.reset(); g.lastMoveAt = pnow; g.lastBody = target;
                g.trail.clear(); g.hasDest = false; g.settleUntil = now + UNBLOCK_COOLDOWN_MS;
                continue;
            }
            // Trail: drop the waypoints already reached or overtaken.
            while (!g.trail.empty() && hdist(gpos, g.trail.front()) < TRAIL_REACHED * 0.5f) g.trail.pop_front();
            while (g.trail.size() >= 2 && hdist(gpos, g.trail[1]) < hdist(gpos, g.trail[0])) g.trail.pop_front();
            // Speed of the owner (+ a margin to close the gap), whatever this body's own limit;
            // sent at most every 250 ms and only when it changes, restored when the owner slows.
            if (g_cfg.tuneSpeedMatch && now - g.lastSpeedCmd >= STEER_PERIOD_MS)
            {
                if (speed >= 2.f)
                {
                    float want = speed * 1.15f + (dist > 3.f ? dist * 1.5f : 0.f);
                    if (want > 150.f) want = 150.f;
                    if (g.speedCmd <= 0 || fabsf(want - g.speedCmd) > g.speedCmd * 0.1f)
                    {
                        float before = 0;
                        if (safeMatchSpeed(c, want, &before))
                        {
                            if (g.baseMaxSpeed <= 0) g.baseMaxSpeed = before;
                            g.speedCmd = want; g.lastSpeedCmd = now;
                        }
                    }
                }
                else if (g.speedCmd > 0)
                {
                    safeRestoreSpeed(c, g.baseMaxSpeed);
                    g.speedCmd = 0; g.lastSpeedCmd = now;
                }
            }
            // 1) The engine walks/runs the ghost (real animations) towards a point ahead on the path.
            //    The lead grows with speed and always exceeds the engine's arrival distance (~4), so
            //    it never stops between two orders. Every new destination makes the engine plan a
            //    path again: a new one only when the current waypoint is nearly reached, when it is
            //    far off, or once a second.
            if (now - g.lastMoveCmd >= STEER_PERIOD_MS)
            {
                Ogre::Vector3 dest = target;
                bool wantMove = dist > 1.5f;
                if (speed >= 0.3f && !blocked)
                {
                    if (g_cfg.tuneTrail && dist > TRAIL_REACHED && !g.trail.empty())
                    {
                        dest = g.trail.back();
                        for (size_t i = 0; i < g.trail.size(); ++i)
                            if (hdist(gpos, g.trail[i]) >= TRAIL_LOOKAHEAD) { dest = g.trail[i]; break; }
                    }
                    else
                    {
                        Ogre::Vector3 dir(smp.vx, 0, smp.vz);
                        float len = dir.length();
                        float lead = speed * 0.5f;
                        if (lead < MIN_LEAD) lead = MIN_LEAD;
                        if (lead > MAX_LEAD) lead = MAX_LEAD;
                        dest = len > 1e-3f ? target + dir * (lead / len) : target;
                    }
                    wantMove = true;
                }
                bool nearlyThere = hdist(gpos, g.lastDest) < REPATH_NEAR;
                bool changed = !g.hasDest || (dest.distance(g.lastDest) > REPATH_DISTANCE && (nearlyThere || dest.distance(g.lastDest) > REPATH_FAR));
                if (!g_cfg.tuneRepath) changed = true;   // old behaviour: a new destination every steer
                if (wantMove && (changed || now - g.lastMoveCmd >= REPATH_MAX_MS))
                {
                    g.lastMoveCmd = now;
                    g.lastDest = dest; g.hasDest = true;
                    safeMoveTo(c, &dest);
                }
                else if (!wantMove) g.hasDest = false;
            }
            // 1b) Sprint start / slow acceleration: the engine takes a moment to reach a fast runner's
            //     speed. Top it up along the owner's own trail (walkable, so no cutting through walls),
            //     by the missing speed only.
            {
                float frameSec = (float)(frameMs / 1000.0);
                float bodySpeed = frameSec > 0 ? hdist(gpos, g.prevFrameBody) / frameSec : 0.f;
                g.prevFrameBody = gpos;
                if (g_cfg.tuneTrail && speedNow > 10.f && dist >= NUDGE_MAX_GAP && dist < SNAP_FAR && !g.trail.empty() && !blocked &&
                    bodySpeed < speedNow * 0.8f && frameSec > 0 && !g_nudgeBroken)
                {
                    Ogre::Vector3 wp = g.trail.front();
                    Ogre::Vector3 dir = wp - gpos; dir.y = 0;
                    float len = dir.length();
                    float step = (speedNow * 1.1f - bodySpeed) * frameSec;
                    if (step > len) step = len;
                    if (len > 1e-3f && step > 0)
                    {
                        Ogre::Vector3 np = gpos + dir * (step / len);
                        if (!safeNudge(c, &np)) { g_nudgeBroken = true; log("position nudge unavailable, using locomotion only"); }
                        else ++g_trailGlides;
                    }
                }
            }
            // 2) Close what the locomotion leaves (arrival tolerance ~4 units, speed mismatch) with a
            //    bounded per-frame nudge: fast enough to never drift, slow enough to look natural.
            //    Not while blocked: a straight push would only wedge it further.
            if (dist > 0.15f && (dist < NUDGE_MAX_GAP || !g_cfg.tuneTrail) && !g_nudgeBroken && !blocked)
            {
                float frameSec = (float)(frameMs / 1000.0);
                float maxStep = (NUDGE_BASE_SPEED + dist * NUDGE_GAIN + speedNow * 0.25f) * frameSec;
                Ogre::Vector3 pos = c->getPosition();
                Ogre::Vector3 dir = target - pos;
                dir.y = 0;                                          // the ground decides the height
                float len = dir.length();
                if (len > 1e-4f)
                {
                    float step = len < maxStep ? len : maxStep;
                    Ogre::Vector3 np = pos + dir * (step / len);
                    if (!safeNudge(c, &np)) { g_nudgeBroken = true; log("position nudge unavailable, using locomotion only"); }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------- targets
// Translate an engine object into something every machine can resolve, and back.
TargetRef makeTargetRef(RootObjectBase* o)
{
    TargetRef t;
    if (!o) return t;
    hand h(o);
    if (h.type == CHARACTER)
    {
        Character* c = h.getCharacter();
        if (!c) return t;
        uint32_t id = chars_netIdOf(c);
        if (id) { t.kind = TargetRef::NET_CHARACTER; t.netId = id; return t; }
        Ogre::Vector3 p = c->getPosition();
        t.kind = TargetRef::WORLD_CHARACTER; t.x = p.x; t.y = p.y; t.z = p.z;
        return t;
    }
    if (h.type == BUILDING)
    {
        Building* b = h.getBuilding();
        if (!b) return t;
        uint32_t id = builds_netIdOf(b);
        if (id) { t.kind = TargetRef::NET_BUILDING; t.netId = id; return t; }
        if (!b->data) return t;
        Ogre::Vector3 p = b->getPosition();
        t.kind = TargetRef::WORLD_BUILDING; t.sid = b->data->stringID; t.x = p.x; t.y = p.y; t.z = p.z;
    }
    return t;
}

RootObject* resolveTargetRef(const TargetRef& t)
{
    switch (t.kind)
    {
    case TargetRef::NET_CHARACTER: return chars_byNetId(t.netId);
    case TargetRef::NET_BUILDING:  return builds_byNetId(t.netId);
    case TargetRef::WORLD_CHARACTER:
    {
        // World NPCs exist on every machine at the same place: take the nearest one.
        Ogre::Vector3 p(t.x, t.y, t.z);
        Character* best = NULL; float bestD = 3.f;
        const lektor<Faction*>* all = ou->factionMgr->getAllFactions();
        for (uint32_t f = 0; all && f < all->size(); ++f)
        {
            std::vector<Character*> found;
            cs_charactersNear((*all)[f], p, 3.f, found);
            for (size_t i = 0; i < found.size(); ++i)
            {
                Character* c = found[i];
                float d = c->getPosition().distance(p);
                if (d < bestD) { bestD = d; best = c; }
            }
        }
        return best;
    }
    case TargetRef::WORLD_BUILDING:
        return builds_findWorld(t.sid, t.x, t.y, t.z);
    }
    return NULL;
}

// ---------------------------------------------------------------------- module API
uint32_t chars_netIdOf(Character* c)
{
    if (!c) return 0;
    if (uint32_t g = ghostIdOf(c)) return g;
    {
        Lock l;
        for (size_t i = 0; i < g_local.size(); ++i) if (g_local[i].ptr == c) return g_local[i].id;
    }
    return npcs_netIdOf(c);
}

bool chars_isGhost(Character* c) { return c && ghostIdOf(c) != 0; }

bool chars_debugAttack(Character* attacker, Character* target)
{
    // A player's character acts through its task list (what a right-click on an enemy issues).
    if (!attacker || !target) return false;
    Ogre::Vector3 at = target->getPosition();
    return safeOrder(attacker, MELEE_ATTACK, target, &at);
}

void chars_localCharacters(std::vector<std::pair<uint32_t, Character*> >& out)
{
    Lock l;
    for (size_t i = 0; i < g_local.size(); ++i)
    {
        Character* c = g_local[i].h.getCharacter();
        if (c && c == g_local[i].ptr && g_local[i].id) out.push_back(std::make_pair(g_local[i].id, c));
    }
}

Character* chars_ghostNear(uint8_t owner, const Ogre::Vector3& pos, float maxDist)
{
    Character* best = NULL;
    float bestD = maxDist;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        if (netIdOwner(it->first) != owner || isNpcNetId(it->first)) continue;
        Character* c = it->second.h.getCharacter();
        if (!c) continue;
        float d = c->getPosition().distance(pos);
        if (d <= bestD) { bestD = d; best = c; }
    }
    return best;
}

Character* chars_nextGhost(Character* after)
{
    Character* first = NULL;
    bool take = after == NULL;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        Character* c = it->second.h.getCharacter();
        if (!c) continue;
        if (!first) first = c;
        if (take) return c;
        if (c == after) take = true;
    }
    return first;
}

int chars_ghostTotal() { return (int)g_ghosts.size(); }

// World reloaded: the engine destroyed every ghost with the old world. Forget them (their owners
// re-announce spawns every few seconds, so they come back) and the squads that held them.
// Our own characters need nothing: refreshLocalCharacters validates each one by handle.
static void clearDrawOverrides();
void chars_onWorldReload()
{
    clearDrawOverrides();   // their animation objects died with the old world
    g_ghosts.clear();
    g_onMap.clear();   // the map was rebuilt with the world
    g_ghostSquads.clear();
    rebuildGhostIndex();
    g_forceLooks = true;
}

// Stop treating a ghost as a ghost: it becomes an ordinary local character (recruitment).
void chars_releaseGhost(uint32_t id)
{
    items_onDespawn(id);   // its inventory is ours now, no longer a mirror
    g_ghosts.erase(id);
    rebuildGhostIndex();
}

// World NPCs we took over (recruited, carried, caged, looted): simulated here from now on and
// never removed by the client-side world cleanup.
static std::map<Character*, hand> g_claimed;

void chars_claimNpc(uint32_t id, Character* c)
{
    chars_releaseGhost(id);
    if (c) { Lock l; g_claimed[c] = hand(c); }
    ByteWriter w; w.u32(id);
    g_session.sendTo(HOST_ID, MSG_NPC_CLAIM, w.data);
    log("world NPC %08x claimed (now simulated here)", id);
}

// Carry / cage orders are often given from afar, but the host only hands an NPC over to a client
// standing next to it: the claim waits until one of our characters is close enough.
static const float CLAIM_NEAR = 50.f;          // host accepts within 60
static const DWORD CLAIM_WAIT_MS = 120000;
static std::map<uint32_t, DWORD> g_pendingClaims;   // NPC net id -> when the order was given

static bool ourCharacterNear(Character* c, float range)
{
    Ogre::Vector3 p = c->getPosition();
    for (size_t k = 0; k < g_local.size(); ++k)
        if (Character* m = g_local[k].h.getCharacter())
            if (m == g_local[k].ptr && m->getPosition().distance(p) <= range) return true;
    return false;
}

void chars_claimWhenNear(uint32_t id, Character* c)
{
    if (ourCharacterNear(c, CLAIM_NEAR)) { g_pendingClaims.erase(id); chars_claimNpc(id, c); return; }
    g_pendingClaims[id] = GetTickCount();
    log("world NPC %08x will be claimed when one of our characters reaches it", id);
}

static void processPendingClaims(DWORD now)
{
    for (std::map<uint32_t, DWORD>::iterator it = g_pendingClaims.begin(); it != g_pendingClaims.end();)
    {
        std::map<uint32_t, Ghost>::iterator g = g_ghosts.find(it->first);
        Character* c = g != g_ghosts.end() ? g->second.h.getCharacter() : NULL;
        if (!c || now - it->second > CLAIM_WAIT_MS) { g_pendingClaims.erase(it++); continue; }   // gone, or given up
        if (ourCharacterNear(c, CLAIM_NEAR)) { uint32_t id = it->first; g_pendingClaims.erase(it++); chars_claimNpc(id, c); continue; }
        ++it;
    }
}

bool chars_isClaimed(Character* c)
{
    Lock l;
    std::map<Character*, hand>::iterator it = g_claimed.find(c);
    if (it == g_claimed.end()) return false;
    if (it->second.getCharacter() != c) { g_claimed.erase(it); return false; }   // gone / reused
    return true;
}

namespace { Character* localByNetId(uint32_t id)
{
    if (isNpcNetId(id)) return npcs_byNetId(id);
    LocalChar* lc = findLocal(id);
    return lc ? lc->h.getCharacter() : NULL;
} }

Character* chars_byNetId(uint32_t id)
{
    if (netIdOwner(id) == g_session.localId()) return localByNetId(id);
    std::map<uint32_t, Ghost>::iterator it = g_ghosts.find(id);
    return it == g_ghosts.end() ? NULL : it->second.h.getCharacter();
}

// --- helpers shared with Npcs.cpp (CharSync.h) ---
void cs_captureState(Character* c, uint32_t id, EntityState& s) { captureState(c, id, s); }

Bytes cs_appearanceMsg(Character* c, uint32_t id)
{
    ByteWriter w; w.u32(id);
    captureAppearance(c->getAppearanceData()).write(w);
    return w.data;
}

Bytes cs_equipmentMsg(Character* c, uint32_t id)
{
    std::vector<ItemRef> eq = captureEquipment(c);
    ByteWriter w; w.u32(id); w.u8((uint8_t)(eq.size() > 255 ? 255 : eq.size()));
    for (size_t n = 0; n < eq.size() && n < 255; ++n) eq[n].write(w);
    int states[4]; Item* items[4];
    if (safeLimbs(c, states, items))
    {
        w.u8(4);
        for (int i = 0; i < 4; ++i)
        {
            ItemRef r;
            if (states[i] == LIMB_REPLACED && items[i]) { r.item = sidOf(items[i]->data); r.manufacturer = sidOf(items[i]->manufacturerData); r.material = sidOf(items[i]->materialData); }
            w.u8((uint8_t)states[i]); r.write(w);
        }
    }
    return w.data;
}

Bytes cs_statsMsg(Character* c, uint32_t id)
{
    int n = 0;
    float* st = statsBlock(c, n);
    if (!st) return Bytes();
    ByteWriter w; w.u32(id); w.u16((uint16_t)n);
    for (int i = 0; i < n; ++i) w.f32(st[i]);
    // Trailer: hunger, to the point the health panel shows (x100), so the message rarely changes.
    w.u8(1); w.f32(floorf(c->medical.hunger * 100.f) / 100.f);
    // Trailer 2: the "ranged" toggle (shoot with the crossbow instead of closing in to melee).
    if (c->stats) { w.u8(2); w.u8(c->stats->rangedMode ? 1 : 0); }
    return w.data;
}

void cs_ghostPositions(uint8_t owner, std::vector<Ogre::Vector3>& out)
{
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
        if (netIdOwner(it->first) == owner && !isNpcNetId(it->first))
            if (Character* c = it->second.h.getCharacter()) out.push_back(c->getPosition());
}

void cs_localPositions(std::vector<Ogre::Vector3>& out)
{
    for (size_t i = 0; i < g_local.size(); ++i)
        if (Character* c = g_local[i].h.getCharacter()) out.push_back(c->getPosition());
}

bool cs_destroy(Character* c) { ++g_itemsMute; ++g_itemsEpoch; bool r = safeDestroy(c); --g_itemsMute; return r; }

namespace {
    // Copies a squad's members; guarded because squads can be half-torn-down while unloading.
    int safeSquadThings(Platoon* p, RootObject** buf, int cap)
    {
        __try
        {
            if (!p || !p->activePlatoon) return 0;
            lektor<RootObject*>* things = p->activePlatoon->getThings();
            if (!things) return 0;
            int n = 0;
            for (uint32_t i = 0; i < things->size() && n < cap; ++i) buf[n++] = (*things)[i];
            return n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
}

namespace {
    int safeActiveList(Character** buf, int cap)
    {
        __try
        {
            int n = 0;
            for (auto it = ou->charUpdateListMain.begin(); it != ou->charUpdateListMain.end() && n < cap; ++it) buf[n++] = *it;
            return n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
}

void cs_allActiveCharacters(std::vector<Character*>& out)
{
    static Character* buf[8192];
    int n = safeActiveList(buf, 8192);
    out.assign(buf, buf + n);
}

void cs_charactersNear(Faction* f, const Ogre::Vector3& pos, float radius, std::vector<Character*>& out)
{
    if (!f) return;
    const lektor<Platoon*>* squads = f->getActivePlatoons();
    if (!squads) return;
    RootObject* buf[128];
    for (uint32_t s = 0; s < squads->size(); ++s)
    {
        int n = safeSquadThings((*squads)[s], buf, 128);
        for (int i = 0; i < n; ++i)
        {
            hand h(buf[i]);
            if (h.type != CHARACTER && h.type != ANIMAL_CHARACTER) continue;
            Character* c = static_cast<Character*>(buf[i]);
            if (c->getPosition().distance(pos) <= radius) out.push_back(c);
        }
    }
}

// ---------------------------------------------------------------------- render layer
// Called right after the engine's frame update (its simulation has placed every body), before
// the frame is drawn: draw walking ghosts exactly on their smoothed network path.
namespace
{
    const float BLEND_IN_MS = 200.f;         // fade between "engine body" and "network path"
    const float MAX_VISUAL_OFFSET = 6.f;     // never draw a model far from its body (combat, clicks)
    long g_visFrames = 0, g_visKept = 0;     // measurement: did our drawn position survive?
    double g_visOffsetSum = 0;
    DWORD g_lastRender = 0;
}

// What each ghost's model must show this frame, keyed by its AnimationClass (read by the
// AnimationClass::setPosition hook, which runs inside the engine's frame update).
struct DrawOverride { Ogre::Vector3 pos; Ogre::Quaternion rot; float blend; };
static std::map<AnimationClass*, DrawOverride> g_draw;   // g_lock
static std::map<AnimationClass*, Ogre::Vector3> g_applied;   // g_lock: last position our hook gave the engine
static void clearDrawOverrides() { kmp::Lock l; g_draw.clear(); g_applied.clear(); }
static long g_animHookHits = 0;

static void publishDrawOverrides()
{
    std::map<AnimationClass*, DrawOverride> next;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        Ghost& g = it->second;
        Character* c = g.h.getCharacter();
        if (!c || !c->animation || g.blend <= 0.f || !g.hasVisual) continue;
        Ogre::Vector3 body = c->getPosition();
        DrawOverride d;
        d.pos = body + (g.visualPos - body) * g.blend;
        d.rot = g.visualRot;
        d.blend = g.blend;
        next[c->animation] = d;
    }
    kmp::Lock l;
    g_draw.swap(next);
    for (std::map<AnimationClass*, Ogre::Vector3>::iterator it = g_applied.begin(); it != g_applied.end();)
        if (!g_draw.count(it->first)) g_applied.erase(it++); else ++it;
}

void chars_preFrame()
{
    // Before the engine updates: is the model still where we drew it last frame?
    if (!g_cfg.renderSmoothing) return;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        Ghost& g = it->second;
        if (g.blend <= 0.f) continue;
        Character* c = g.h.getCharacter();
        Ogre::Vector3 drawn;
        if (!c || !c->animation || !safeDrawnPos(c, &drawn)) continue;
        Ogre::Vector3 applied;
        {
            kmp::Lock l;
            std::map<AnimationClass*, Ogre::Vector3>::iterator a = g_applied.find(c->animation);
            if (a == g_applied.end()) continue;
            applied = a->second;
        }
        ++g_visFrames;
        // Nothing moved the model after our hook. Horizontal only: the scene node sits a fixed
        // ~1.3 units below the position given (model origin); allow one frame of running.
        float hx = drawn.x - applied.x, hz = drawn.z - applied.z;
        if (hx * hx + hz * hz < 1.0f) ++g_visKept;
        static DWORD lastDiag = 0;
        if (g_cfg.debugKeys && GetTickCount() - lastDiag > 3000)
        {
            lastDiag = GetTickCount();
            Ogre::Vector3 b = c->getPosition();
            log("render %08x: drawn (%.2f,%.2f,%.2f) applied (%.2f,%.2f,%.2f) body (%.2f,%.2f,%.2f)", it->first,
                drawn.x, drawn.y, drawn.z, applied.x, applied.y, applied.z, b.x, b.y, b.z);
        }
    }
}

void chars_renderTick(DWORD now)
{
    if (!g_cfg.renderSmoothing) return;
    float dt = g_lastRender ? (float)(now - g_lastRender) : 16.f;
    if (dt > 200.f) dt = 200.f;
    g_lastRender = now;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        Ghost& g = it->second;
        Character* c = g.h.getCharacter();
        if (!c || !g.hasVisual) continue;
        Ogre::Vector3 body = c->getPosition();
        // Only while simply moving: in combat, tasks, KO... the body's exact place matters.
        bool want = g.mode == Ghost::MOVE && body.distance(g.visualPos) < MAX_VISUAL_OFFSET;
        g.blend += (want ? 1.f : -1.f) * dt / BLEND_IN_MS;
        if (g.blend < 0.f) g.blend = 0.f;
        if (g.blend > 1.f) g.blend = 1.f;
        if (g.blend <= 0.f) continue;
        Ogre::Vector3 pos = body + (g.visualPos - body) * g.blend;
        g_visOffsetSum += body.distance(pos);
        g.lastDrawn = pos;
        // Fallback only: if the engine never routes through AnimationClass::setPosition for this
        // model, place it directly (may be overwritten before drawing, see the stats).
        if (g_animHookHits == 0)
        {
            Ogre::Quaternion rot = g.blend > 0.5f ? g.visualRot : c->getOrientation();
            if (!safeVisual(c, &pos, &rot)) { g_cfg.renderSmoothing = false; log("render smoothing unavailable (engine exception), disabled"); return; }
        }
    }
    // Overrides for the next engine update (its AnimationClass::setPosition calls).
    publishDrawOverrides();
}

void chars_logRenderStats()
{
    if (!g_visFrames) return;
    log("render layer: model on smoothed path in %.0f %% of %ld samples, mean body->model offset %.2f, %ld engine placements overridden",
        100.0 * g_visKept / g_visFrames, g_visFrames, g_visOffsetSum / g_visFrames, g_animHookHits);
    g_visFrames = g_visKept = 0; g_visOffsetSum = 0; g_animHookHits = 0;
}

// Host NPC ghosts within NPC_FULL_UPDATE_RANGE of one of our characters (refreshed each second).
static std::set<Character*> g_nearNpcs;   // g_lock
static const float NPC_FULL_UPDATE_RANGE = 150.f;
static bool npcNearUs(Character* c)
{
    kmp::Lock l;
    return g_nearNpcs.count(c) != 0;
}
static void refreshNearNpcs(DWORD now)
{
    static DWORD last = 0;
    if (now - last < 1000) return;
    last = now;
    std::vector<Ogre::Vector3> ours;
    cs_localPositions(ours);
    std::set<Character*> near_;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        if (!isNpcNetId(it->first)) continue;
        Character* c = it->second.h.getCharacter();
        if (!c) continue;
        Ogre::Vector3 p = c->getPosition();
        for (size_t i = 0; i < ours.size(); ++i)
            if (ours[i].distance(p) < NPC_FULL_UPDATE_RANGE) { near_.insert(c); break; }
    }
    kmp::Lock l;
    g_nearNpcs.swap(near_);
}

void chars_tick(DWORD now)
{
    driveGhosts(now);   // every frame
    if (g_cfg.tuneOnScreen) refreshNearNpcs(now);
    if (now - g_lastState >= STATE_PERIOD_MS)
    {
        g_lastState = now;
        refreshLocalCharacters();
        sendStates();
        processPendingClaims(now);
    }
    if (now - g_lastSpawn >= SPAWN_PERIOD_MS) { g_lastSpawn = now; sendSpawns(); updateMapSquads(); }
    if (g_forceLooks || now - g_lastLooks >= LOOKS_PERIOD_MS)
    {
        g_lastLooks = now;
        sendLooks(g_forceLooks);
        g_forceLooks = false;
    }
    rebuildGhostIndex();
}

void chars_resendAll()
{
    if (!ou || !ou->player) return;
    refreshLocalCharacters();
    sendSpawns();
    g_lastSpawn = GetTickCount();
    g_forceLooks = true;
}

void chars_forgetMovement(uint8_t id) { g_moveGuard.forgetOwner(id); }

void chars_onPlayerLeft(uint8_t id)
{
    g_moveGuard.forgetOwner(id);
    std::vector<uint32_t> gone;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
        if (netIdOwner(it->first) == id) gone.push_back(it->first);
    // Off the map first, while the squads still exist (destroying their last ghosts may free them).
    for (std::map<std::pair<uint8_t, Faction*>, Platoon*>::iterator it = g_ghostSquads.begin(); it != g_ghostSquads.end();)
        if (it->first.first == id) { mapRemove(it->second); g_ghostSquads.erase(it++); log("player %d left: squad taken off the map before its characters", (int)id); } else ++it;
    for (size_t i = 0; i < gone.size(); ++i) despawnGhost(gone[i]);
    g_clocks.erase(id);   // a reconnecting player restarts its clock
}

// Ghosts saved into the savegame by a previous session come back as orphans: remove them.
void chars_purgeStale()
{
    for (int p = 0; p < MAX_PLAYERS; ++p)
    {
        char buf[32]; sprintf_s(buf, sizeof(buf), "kenshimp_player_%d", p);
        Faction* f = ou->factionMgr->getFactionByStringID(buf);
        if (!f) continue;
        std::vector<Character*> chars;
        cs_charactersNear(f, Ogre::Vector3::ZERO, 1.0e9f, chars);
        int n = 0;
        for (size_t i = 0; i < chars.size(); ++i)
        {
            Character* c = chars[i];
            if (chars_isGhost(c)) continue;
            if (safeDestroy(c)) ++n;
        }
        if (n) log("purged %d stale ghost character(s) of %s", n, buf);
    }
}

void chars_onMessage(const NetEvent& e)
{
    ByteReader r(e.body);
    switch (e.msgType)
    {
    case MSG_ENTITY_SPAWN:
    {
        EntitySpawn sp; sp.read(r);
        if (!r.ok() || netIdOwner(sp.netId) != e.sender || !sp.sanitize()) break;
        if (!g_ghosts.count(sp.netId) && ghostCount(e.sender) >= MAX_GHOSTS_PER_PLAYER)
        {
            static DWORD lastWarn = 0;
            if (GetTickCount() - lastWarn > 10000) { lastWarn = GetTickCount(); log("ghost cap reached for player %d, spawn ignored", (int)e.sender); }
            break;
        }
        spawnGhost(e.sender, sp);
        break;
    }
    case MSG_ENTITY_DESPAWN:
    {
        uint32_t id = r.u32();
        if (r.ok() && netIdOwner(id) == e.sender) despawnGhost(id);
        break;
    }
    case MSG_ENTITY_STATE:
    {
        uint32_t senderMs = r.u32();
        uint16_t n = r.u16();
        if (!r.ok()) break;
        ClockSync& clock = g_clocks[e.sender];
        clock.observe(senderMs, (uint32_t)preciseTick());
        double snapT = clock.toLocal(senderMs);
        for (int i = 0; i < n && r.ok(); ++i)
        {
            EntityState s; s.read(r);
            if (!r.ok() || netIdOwner(s.netId) != e.sender || !s.sanitize()) continue;
            if (g_session.isHost() && !isNpcNetId(s.netId)) checkMovement(e.sender, s, senderMs);
            applyGhostState(s, snapT);
        }
        break;
    }
    case MSG_APPEARANCE:
    {
        uint32_t id = r.u32();
        AppearanceBlob b; b.read(r);
        if (!r.ok() || netIdOwner(id) != e.sender || !g_cfg.syncAppearance) break;
        Ghost& g = g_ghosts[id];
        g.pendingAppearance = b; g.hasPendingAppearance = true;
        if (Character* c = g.h.getCharacter()) flushPending(id, g, c);
        break;
    }
    case MSG_EQUIPMENT:
    {
        uint32_t id = r.u32();
        uint8_t n = r.u8();
        std::vector<ItemRef> eq;
        for (int i = 0; i < n && r.ok(); ++i) { ItemRef it; it.read(r); eq.push_back(it); }
        if (!r.ok() || netIdOwner(id) != e.sender || !g_cfg.syncEquipment) break;
        Ghost& g = g_ghosts[id];
        g.pendingEquipment = eq; g.hasPendingEquipment = true;
        if (r.remaining() && r.u8() == 4)   // optional limbs trailer
        {
            uint8_t st[4]; ItemRef li[4];
            for (int i = 0; i < 4; ++i) { st[i] = r.u8(); li[i].read(r); }
            if (r.ok())
            {
                g.hasLimbs = true;
                for (int i = 0; i < 4; ++i) { g.limbState[i] = st[i] <= LIMB_CRUSHED ? st[i] : LIMB_ORIGINAL; g.limbItem[i] = li[i]; }
            }
        }
        if (Character* c = g.h.getCharacter()) flushPending(id, g, c);
        break;
    }
    case MSG_STATS:
    {
        uint32_t id = r.u32();
        uint16_t n = r.u16();
        std::vector<float> st;
        for (int i = 0; i < n && r.ok(); ++i) st.push_back(clampF(r.f32(), 0.f, 1000.f));
        if (!r.ok() || netIdOwner(id) != e.sender) break;
        float hunger = -1.f; int ranged = -1;
        while (r.ok() && r.remaining())   // optional tagged trailers
        {
            uint8_t tag = r.u8();
            if (tag == 1) { float h = r.f32(); if (r.ok()) hunger = clampF(h, 0.f, 1000.f); }
            else if (tag == 2) { uint8_t v = r.u8(); if (r.ok()) ranged = v ? 1 : 0; }
            else break;   // unknown (newer sender): ignore the rest
        }
        Ghost& g = g_ghosts[id];
        g.pendingStats = st; g.hasPendingStats = true; g.pendingHunger = hunger; g.pendingRanged = ranged;
        if (Character* c = g.h.getCharacter()) flushPending(id, g, c);
        break;
    }
    case MSG_DAMAGE:
    {
        uint8_t target = r.u8();
        DamageMsg d; d.read(r);
        d.sanitize();
        if (r.ok() && target == g_session.localId()) applyIncomingDamage(d);
        break;
    }
    }
}

} // namespace kmp

using namespace kmp;

// ============================================================================ damage hooks
// Who is hitting right now (set around Character::hitByMeleeAttack / CombatClass::_getHit,
// read by the medical hooks).
static RootObject* g_hitAttacker = NULL;
static int g_hitsInsideGetHit = 0, g_hitsOutsideGetHit = 0;
// Projectiles hit later, without attacker info: each ghost shot is remembered on its target
// until it lands or its flight time runs out. A piercing hit on that target uses up one shot:
// a real archer's arrow arriving at the same time is no longer mistaken for the ghost's.
static std::map<RootObject*, std::deque<DWORD> > g_ghostShots;   // target -> expiry of each shot in flight (g_lock)
static const DWORD GHOST_SHOT_WINDOW_MS = 4000;                   // longest flight considered
static const DWORD GHOST_SHOT_MIN_MS = 800;

static bool takeGhostShot(Character* victim)
{
    kmp::Lock l;
    std::map<RootObject*, std::deque<DWORD> >::iterator it = g_ghostShots.find(victim);
    if (it == g_ghostShots.end()) return false;
    DWORD now = GetTickCount();
    std::deque<DWORD>& q = it->second;
    while (!q.empty() && (int)(q.front() - now) <= 0) q.pop_front();   // missed or long gone
    if (q.empty()) { g_ghostShots.erase(it); return false; }
    q.pop_front();
    return true;
}
// Depth of the damage hooks: MedicalSystem::applyDamage calls HealthPartStatus::applyDamage,
// and one hit must be judged (and use up a shot) only once.
static int g_judgeDepth = 0;

void (*applyDamage_orig)(MedicalSystem::HealthPartStatus*, const Damages&) = NULL;
void (*medApply_orig)(MedicalSystem*, MedicalSystem::HealthPartStatus*, const Damages&, bool, bool, const Ogre::Vector3&) = NULL;

namespace
{
    typedef void (MedicalSystem::*MedApplyFn)(MedicalSystem::HealthPartStatus*, const Damages&, bool, bool, const Ogre::Vector3&);
    // Deriving grants access to the protected member's address.
    struct MedicalAccess : MedicalSystem
    {
        static MedApplyFn address() { return &MedicalAccess::applyDamage; }
    };

    enum HitVerdict { HIT_APPLY, HIT_FORWARDED, HIT_DROP };

    // Decides what happens to a hit on `victim` (see the authority rules at the top of the file).
    HitVerdict judgeHit(Character* victim, MedicalSystem::HealthPartStatus* part, const Damages& dmg)
    {
        if (g_applyingSync || !victim || !ready()) return HIT_APPLY;
        if (g_hitAttacker) ++g_hitsInsideGetHit; else ++g_hitsOutsideGetHit;
        if (g_cfg.debugKeys)
        {
            // Who hits whom (debug): first 60 hits involving a replicated character.
            static int logged = 0;
            Character* a = g_hitAttacker ? static_cast<Character*>(g_hitAttacker) : NULL;
            uint32_t vid = chars_netIdOf(victim), aid = a ? chars_netIdOf(a) : 0;
            if ((vid || aid) && logged < 60)
            {
                ++logged;
                log("hit: %s (%08x%s) -> %s (%08x%s) cut %.1f blunt %.1f pierce %.1f",
                    a ? a->getName().c_str() : "?", aid, a && chars_isGhost(a) ? " ghost" : "",
                    victim->getName().c_str(), vid, chars_isGhost(victim) ? " ghost" : "", dmg.cut, dmg.blunt, dmg.pierce);
            }
        }

        bool attackerIsGhost = g_hitAttacker ? chars_isGhost(static_cast<Character*>(g_hitAttacker))
                                             : (dmg.pierce > 0 && takeGhostShot(victim));
        uint32_t ghostId = ghostIdOf(victim);
        if (ghostId)
        {
            if (attackerIsGhost) return HIT_DROP;     // ghost vs ghost: the owners sort it out
            MedicalSystem& med = victim->medical;
            uint8_t index = 0;
            for (uint32_t i = 0; i < med.anatomy.size(); ++i) if (med.anatomy[i] == part) { index = (uint8_t)i; break; }
            DamageMsg d;
            d.victimNetId = ghostId;
            d.attackerNetId = g_hitAttacker ? chars_netIdOf(static_cast<Character*>(g_hitAttacker)) : 0;
            d.bodyPart = index;
            d.cut = dmg.cut; d.blunt = dmg.blunt; d.pierce = dmg.pierce;
            ByteWriter w; w.u8(netIdOwner(ghostId)); d.write(w);
            g_session.send(MSG_DAMAGE, w.data);
            return HIT_FORWARDED;
        }
        // A ghost hitting one of our replicated characters: the real hit arrives by network.
        if (attackerIsGhost && chars_netIdOf(victim))
        {
            if (g_cfg.debugKeys && !g_hitAttacker) log("hit on %s dropped: a ghost's projectile (its owner sends the real hit)", victim->getName().c_str());
            return HIT_DROP;
        }
        return HIT_APPLY;
    }
}

namespace kmp { namespace {
    // Full medical path (wounds, bleeding, severing, KO) when available.
    void applyRealDamage(Character* c, MedicalSystem::HealthPartStatus* part, const Damages& dmg)
    {
        g_applyingSync = true;
        if (medApply_orig) medApply_orig(&c->medical, part, dmg, false, true, Ogre::Vector3::ZERO);
        else part->applyDamage(dmg);
        g_applyingSync = false;
    }
} }

void applyDamage_hook(MedicalSystem::HealthPartStatus* part, const Damages& dmg)
{
    if (!g_judgeDepth && part && judgeHit(part->me, part, dmg) != HIT_APPLY) return;   // inside medApply: already judged
    applyDamage_orig(part, dmg);
}

void medApply_hook(MedicalSystem* self, MedicalSystem::HealthPartStatus* part, const Damages& dmg, bool loading, bool canSever,
                   const Ogre::Vector3& force)
{
    if (!loading && !g_judgeDepth && self && judgeHit(self->me, part, dmg) != HIT_APPLY) return;
    struct Depth { Depth() { ++g_judgeDepth; } ~Depth() { --g_judgeDepth; } } depth;   // also undone if the engine throws
    medApply_orig(self, part, dmg, loading, canSever, force);
}

// CombatClass::_getHit: remembers the attacker for the medical hooks above.
void (*getHit_orig)(CombatClass*, CutDirection, const Damages&, RootObject*, bool) = NULL;
void getHit_hook(CombatClass* self, CutDirection dir, const Damages& dmg, RootObject* who, bool stumble)
{
    RootObject* prev = g_hitAttacker;
    g_hitAttacker = who;
    getHit_orig(self, dir, dmg, who, stumble);
    g_hitAttacker = prev;
}

// Character::hitByMeleeAttack: the melee entry point, knows the attacker. A ghost's blow on a
// replicated character still plays (block/stagger/sound) but with zero damage.
HitMaterialType (*charHit_orig)(Character*, CutDirection, Damages&, Character*, CombatTechniqueData*, int) = NULL;
HitMaterialType charHit_hook(Character* self, CutDirection dir, Damages& dmg, Character* who, CombatTechniqueData* attack, int combo)
{
    RootObject* prev = g_hitAttacker;
    g_hitAttacker = who;
    HitMaterialType res;
    if (ready() && who && chars_isGhost(who) && chars_netIdOf(self))
    {
        Damages none(0, 0, 0, 0, 0);
        res = charHit_orig(self, dir, none, who, attack, combo);
    }
    else
        res = charHit_orig(self, dir, dmg, who, attack, combo);
    g_hitAttacker = prev;
    return res;
}

// GunClass::shoot: remember targets of ghost shooters (see recentlyShotByGhost).
void (*gunShoot_orig)(GunClass*, Character*, RootObject*, StatsEnumerated, const Ogre::Vector3&) = NULL;
void gunShoot_hook(GunClass* self, Character* me, RootObject* target, StatsEnumerated stat, const Ogre::Vector3& aim)
{
    if (target && me && chars_isGhost(me))
    {
        // Flight time from the distance and the weapon's shot speed, plus a margin.
        DWORD window = GHOST_SHOT_WINDOW_MS;
        float speed = self ? self->shotSpeed : 0.f;
        if (speed > 1.f)
        {
            float ms = target->getPosition().distance(me->getPosition()) / speed * 1000.f + (float)GHOST_SHOT_MIN_MS;
            window = ms < (float)GHOST_SHOT_MIN_MS ? GHOST_SHOT_MIN_MS : ms > (float)GHOST_SHOT_WINDOW_MS ? GHOST_SHOT_WINDOW_MS : (DWORD)ms;
        }
        kmp::Lock l;
        DWORD now = GetTickCount();
        if (g_ghostShots.size() > 256)   // forget targets with nothing in flight (and stale pointers)
            for (std::map<RootObject*, std::deque<DWORD> >::iterator it = g_ghostShots.begin(); it != g_ghostShots.end();)
                if (it->second.empty() || (int)(it->second.back() - now) <= 0) g_ghostShots.erase(it++); else ++it;
        std::deque<DWORD>& q = g_ghostShots[target];
        if (q.size() < 16) q.push_back(now + window);
        if (g_cfg.debugKeys) log("ghost shot at a target (flight window %u ms, %d in flight)", window, (int)q.size());
    }
    gunShoot_orig(self, me, target, stat, aim);
}

// --- hook: CombatMovementController::checkWeDontCollideWithCharacters (experimental) ------------
// The engine pushes a moving character's next position away from nearby bodies. A ghost already
// follows a path its owner's game resolved; being pushed by the bodies here (our squad crowding
// it) only blocks it and causes catch-up teleports. Installed only with ghost_no_collide=1.
void (*noCollide_orig)(CombatMovementController*, const Ogre::Vector3&, Ogre::Vector3&) = NULL;
static bool moverIsGhost(CombatMovementController* self)
{
    __try { return self->movement && self->movement->character && chars_isGhost(self->movement->character); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void noCollide_hook(CombatMovementController* self, const Ogre::Vector3& pos, Ogre::Vector3& newpos)
{
    if (ready() && self && moverIsGhost(self)) return;   // newpos kept as planned
    noCollide_orig(self, pos, newpos);
}

// --- hook: Character::setFaction ------------------------------------------------------------
// Recruitment of a ghost. A world-NPC ghost recruited by a client becomes a real character of
// that client (the host is asked to hand the NPC over and removes its copy); another player's
// character can never be recruited.
void (*setFaction_orig)(Character*, Faction*, ActivePlatoon*) = NULL;
void setFaction_hook(Character* self, Faction* f, ActivePlatoon* a)
{
    if (ready() && self && f && f->isThePlayer())
        if (uint32_t id = ghostIdOf(self))
        {
            if (!isNpcNetId(id))
            {
                log("blocked recruiting another player's character (%08x)", id);
                showMessage(T("You cannot recruit another player's character."));
                return;
            }
            chars_claimNpc(id, self);                      // ours from now on
            log("recruited world NPC %08x", id);
        }
    setFaction_orig(self, f, a);
}

// --- hooks: AnimationClass::setPosition (3 overloads) ------------------------------------------
// The engine places each character's model here every frame. For a ghost we substitute the
// smoothed network path, so what is drawn is always fluid whatever the body's locomotion does.
static bool drawOverrideFor(AnimationClass* a, DrawOverride& out)
{
    if (!g_cfg.renderSmoothing || !kmp::ready()) return false;   // out of session: entries may name freed objects
    kmp::Lock l;
    std::map<AnimationClass*, DrawOverride>::iterator it = g_draw.find(a);
    if (it == g_draw.end()) return false;
    out = it->second;
    g_applied[a] = out.pos;
    ++g_animHookHits;
    return true;
}

typedef void (AnimationClass::*SetPos1)(const Ogre::Vector3&);
typedef void (AnimationClass::*SetPosQ)(const Ogre::Vector3&, const Ogre::Quaternion&);
typedef void (AnimationClass::*SetPosV)(const Ogre::Vector3&, const Ogre::Vector3&);

void (*animSetPos1_orig)(AnimationClass*, const Ogre::Vector3&) = NULL;
void animSetPos1_hook(AnimationClass* self, const Ogre::Vector3& pos)
{
    DrawOverride d;
    if (drawOverrideFor(self, d)) { animSetPos1_orig(self, d.pos); return; }
    animSetPos1_orig(self, pos);
}
void (*animSetPosQ_orig)(AnimationClass*, const Ogre::Vector3&, const Ogre::Quaternion&) = NULL;
void animSetPosQ_hook(AnimationClass* self, const Ogre::Vector3& pos, const Ogre::Quaternion& q)
{
    DrawOverride d;
    if (drawOverrideFor(self, d)) { animSetPosQ_orig(self, d.pos, d.blend > 0.5f ? d.rot : q); return; }
    animSetPosQ_orig(self, pos, q);
}
void (*animSetPosV_orig)(AnimationClass*, const Ogre::Vector3&, const Ogre::Vector3&) = NULL;
void animSetPosV_hook(AnimationClass* self, const Ogre::Vector3& pos, const Ogre::Vector3& dir)
{
    DrawOverride d;
    // Keep the engine's facing here (its axis convention for `dir` is unverified).
    if (drawOverrideFor(self, d)) { animSetPosV_orig(self, d.pos, dir); return; }
    animSetPosV_orig(self, pos, dir);
}

// --- hook: Character::isImmuneToOffscreenMode -----------------------------------------------
// Off screen the engine runs characters in a cheap mode (coarse, jerky updates). Other players'
// characters are driven by the network every frame: they must always get the full update, or
// they move in jerks, fall behind and get placed.
namespace
{
    typedef bool (Character::*ImmuneFn)();
    struct CharacterAccess : Character
    {
        static ImmuneFn address() { return &CharacterAccess::isImmuneToOffscreenMode; }
    };
}
bool (*immune_orig)(Character*) = NULL;
long g_immuneCalls = 0, g_immuneNpcCalls = 0;
bool immune_hook(Character* self)
{
    if (g_cfg.tuneOnScreen && self && ready())
    {
        uint32_t id = ghostIdOf(self);
        if (id && !isNpcNetId(id)) { ++g_immuneCalls; return true; }
        if (id && npcNearUs(self)) { ++g_immuneNpcCalls; return true; }   // host NPCs around our characters (fights, chases)
    }
    return immune_orig(self);
}

// --- hook: Character::addOrder ---------------------------------------------------------------
// Orders our characters receive against ghosts. Physically taking a body (carry, cage, bed)
// cannot be shared: another player's character is off limits; a world NPC is claimed first
// (ownership moves to us, the host drops its copy), then the order runs normally here.
// Looting is not in the list: it only moves items, which the owner validates (Items.cpp).
static bool isTakeBodyTask(int t)
{
    switch (t)
    {
    case PICKUP: case USE_CAGE: case PUT_SOMEONE_IN_BED: case FIND_BED_AND_PUT_IN:
    case KNOCKOUT_PRISONER: case CARRY_WOUNDED_SLAVES: case LIFT_OBJECT_BUT_HEAL_FIRST:
        return true;
    default:
        return false;
    }
}

void (*addOrder_orig)(Character*, Building*, TaskType, RootObject*, bool, bool, const Ogre::Vector3&) = NULL;
void addOrder_hook(Character* self, Building* dest, TaskType t, RootObject* subject, bool shift, bool clear, const Ogre::Vector3& loc)
{
    if (ready() && self && subject && isTakeBodyTask(t) && !chars_isGhost(self))
    {
        hand h(subject);
        Character* target = (h.type == CHARACTER || h.type == ANIMAL_CHARACTER) ? h.getCharacter() : NULL;
        if (uint32_t id = target ? ghostIdOf(target) : 0)
        {
            if (!isNpcNetId(id))
            {
                showMessage(T("You cannot carry or cage another player's character."));
                return;
            }
            chars_claimWhenNear(id, target);
        }
    }
    addOrder_orig(self, dest, t, subject, shift, clear, loc);
}

// --- hook: AI::periodicUpdate --------------------------------------------------------------
// Out of combat, ghosts never pick their own tasks: they only execute what we mirror from their
// owner. In combat the AI runs (it drives the actual fighting), but a ghost's hits are judged by
// judgeHit so they never deal damage to replicated characters.
void (*aiPeriodic_orig)(AI*, float) = NULL;
void aiPeriodic_hook(AI* ai, float time)
{
    if (ai && ai->me && g_cfg.ghostAI != "full" && ghostIdOf(ai->me) && !ghostFighting(ai->me)) return;
    aiPeriodic_orig(ai, time);
}

namespace kmp {
void chars_logHitStats()
{
    log("damage hooks: %d hit(s) with known attacker, %d without; %d NPC ghost(s) spawned, %d ghost(s) alive",
        g_hitsInsideGetHit, g_hitsOutsideGetHit, g_npcGhostsSpawned, (int)g_ghosts.size());
    if (g_trackErrN)
        log("ghost tracking: mean offset %.2f, max %.2f (units) over %ld frame-samples, %d snap(s)",
            g_trackErrSum / g_trackErrN, g_trackErrMax, g_trackErrN, g_snaps);
    if (g_unstucks) log("ghost tracking: %ld stuck ghost(s) placed next to their owner", g_unstucks);
    if (g_immuneCalls) { log("ghost tracking: off-screen mode refused %ld time(s)", g_immuneCalls); g_immuneCalls = 0; }
    if (g_immuneNpcCalls) { log("ghost tracking: off-screen mode refused %ld time(s) for host NPCs near us", g_immuneNpcCalls); g_immuneNpcCalls = 0; }
    if (g_floorFixes) { log("ghost tracking: %ld floor correction(s), %ld given up (owner's floor not reachable here)", g_floorFixes, g_floorGiveUps); g_floorFixes = g_floorGiveUps = 0; }
    if (g_trailGlides) { log("ghost tracking: %ld frame(s) helped along the trail (sprint start)", g_trailGlides); g_trailGlides = 0; }
    if (g_movingMs > 0)
        log("ghost tracking: frozen %.1f %% of the time their owner moved (longest %.0f ms), %ld unblock(s), %ld task/fight release(s)",
            100.0 * g_frozenMs / g_movingMs, g_longestFreezeMs, g_unblocks, g_released);
    g_frozenMs = g_movingMs = g_longestFreezeMs = 0; g_released = 0; g_unblocks = 0;
    g_trackErrSum = 0; g_trackErrMax = 0; g_trackErrN = 0; g_snaps = 0; g_unstucks = 0;
}

bool chars_install()
{
    bool ok = true;
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&MedicalSystem::HealthPartStatus::applyDamage),
                                                 applyDamage_hook, &applyDamage_orig))
    { ErrorLog("KenshiMP: could not hook applyDamage!"); ok = false; }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(MedicalAccess::address()),
                                                 medApply_hook, &medApply_orig))
    { ErrorLog("KenshiMP: could not hook MedicalSystem::applyDamage!"); ok = false; }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&CombatClass::_getHit),
                                                 getHit_hook, &getHit_orig))
    { ErrorLog("KenshiMP: could not hook CombatClass::_getHit!"); ok = false; }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::_NV_hitByMeleeAttack),
                                                 charHit_hook, &charHit_orig))
    { ErrorLog("KenshiMP: could not hook Character::hitByMeleeAttack!"); ok = false; }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&GunClass::shoot),
                                                 gunShoot_hook, &gunShoot_orig))
    { ErrorLog("KenshiMP: could not hook GunClass::shoot!"); ok = false; }
    if (g_cfg.ghostNoCollide)
    {
        if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&CombatMovementController::checkWeDontCollideWithCharacters),
                                                     noCollide_hook, &noCollide_orig))
            log("hook checkWeDontCollideWithCharacters failed (ghost_no_collide ignored)");
        else log("experimental: ghosts ignore body collisions (ghost_no_collide=1)");
    }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(CharacterAccess::address()), immune_hook, &immune_orig))
        log("hook Character::isImmuneToOffscreenMode failed (off-screen ghosts update coarsely)");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::addOrder),
                                                 addOrder_hook, &addOrder_orig))
    { ErrorLog("KenshiMP: could not hook Character::addOrder!"); ok = false; }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::_NV_setFaction),
                                                 setFaction_hook, &setFaction_orig))
    { ErrorLog("KenshiMP: could not hook Character::setFaction!"); ok = false; }
    // Render layer (not fatal if missing: falls back to teleportVisuallyOnly).
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((SetPos1)&AnimationClass::setPosition), animSetPos1_hook, &animSetPos1_orig))
        ErrorLog("KenshiMP: could not hook AnimationClass::setPosition(pos)");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((SetPosQ)&AnimationClass::setPosition), animSetPosQ_hook, &animSetPosQ_orig))
        ErrorLog("KenshiMP: could not hook AnimationClass::setPosition(pos, quat)");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((SetPosV)&AnimationClass::setPosition), animSetPosV_hook, &animSetPosV_orig))
        ErrorLog("KenshiMP: could not hook AnimationClass::setPosition(pos, dir)");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&AI::_NV_periodicUpdate),
                                                 aiPeriodic_hook, &aiPeriodic_orig))
    { ErrorLog("KenshiMP: could not hook AI::periodicUpdate!"); ok = false; }
    return ok;
}
}
