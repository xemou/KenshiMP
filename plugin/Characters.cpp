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
#include <kenshi/combat/RangedCombatClass.h>
#include <kenshi/Gear.h>
#include <kenshi/Animation/AnimationClass.h>
#include <kenshi/CameraClass.h>
#include <kenshi/Dialogue.h>
#include <kenshi/ShopTrader.h>
#include <kenshi/Building/UseableStuff.h>
#include <kenshi/Enums.h>
#include <kenshi/gui/ScreenLabel.h>
#include <ogre/OgreCamera.h>
#include <mygui/MyGUI.h>
#include <core/Functions.h>

#include "Shared.h"
#include "../core/StateCodec.h"
#include "CharSync.h"
#include "../core/Authority.h"
#include "../core/Interp.h"
#include "../core/MovementGuard.h"

#include <map>
#include <deque>
#include <set>
#include <math.h>
#include <vector>

using namespace mp;

static bool g_replayingShot = false;   // a ghost firing because its owner did (MSG_SHOT)

namespace kmp {

namespace
{
    const DWORD STATE_PERIOD_MS = 50;     // 20 Hz, like a game server tick
    const DWORD SPAWN_PERIOD_MS = 5000;
    const DWORD LOOKS_PERIOD_MS = 1000;   // appearance / gear / stats change detection
    const float TELEPORT_DISTANCE = 15.f;
    long g_tmFar = 0, g_tmMoving = 0, g_tmWanted = 0, g_tmNoSubject = 0, g_tmOrderFail = 0;   // task mirroring statistics (test)
    const float NPC_TASK_RANGE = 45.f;    // a still world NPC this close to its owner's spot is sent to do the same task
    const float TASK_RANGE = 3.f;         // ghost must be this close to its owner's spot to mirror a task
    const float COMBAT_LEASH = 8.f;       // while fighting, only pull the ghost back if it drifts further
    const float MELEE_SANITY_RANGE = 30.f;  // generous: interpolation delay + lunges
    const float SPAWN_RANGE = 3300.f;       // ghosts further than this from all our characters wait (= the host's NPC reach, 3250: everything it shows is made here too)

    // ------------------------------------------------------------------ SEH-guarded engine calls
    // (Functions containing __try cannot hold C++ objects with destructors.)
    bool safeTeleport(Character* c, const Ogre::Vector3* p, int floor = 0)
    {
        __try { c->getMovement()->_setPositionAndTeleport(*p, floor); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeDead(Character* c) { __try { return c->isDead(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
    bool safeUnconscious(Character* c) { __try { return c->medical.isUnconcious(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
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
    int safeAroundBuildings(lektor<RootObject*>* out, const Ogre::Vector3* p, float r)
    {
        __try { ou->getObjectsWithinSphere(*out, *p, r, BUILDING, 64, NULL); return (int)out->size(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
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
    // A ghost repeats its owner's shot with its own weapon (1 fired, 0 no weapon, -1 failed).
    GunClass* safeGunOf(Character* c)
    {
        __try
        {
            if (c->rangedCombat) if (GunClass* g = c->rangedCombat->getGun()) return g;
            Crossbow* cb = c->getRangedWeapon();
            return cb ? cb->gunClass : NULL;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    bool safeShoot(GunClass* g, Character* c, RootObject* target, int stat, const Ogre::Vector3* aim)
    {
        __try
        {
            if (g->numShotsCurrent <= 0) g->numShotsCurrent = g->numShotsMax > 0 ? g->numShotsMax : 1;   // the owner had a loaded weapon
            g->shoot(c, target, (StatsEnumerated)stat, *aim);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    int replayShot(Character* ghost, RootObject* target, int stat, const Ogre::Vector3* aim)
    {
        GunClass* g = safeGunOf(ghost);
        if (!g) return 0;
        g_replayingShot = true;
        bool ok = safeShoot(g, ghost, target, stat, aim);
        g_replayingShot = false;
        return ok ? 1 : -1;
    }

    // Debug: what decides whether a character shoots.
    void safeRangedInfo(Character* c, int* bow, int* rangedMode, int* inRanged, int* wants)
    {
        __try
        {
            *bow = c->getRangedWeapon() ? 1 : 0;
            *rangedMode = c->stats ? (c->stats->rangedMode ? 1 : 0) : -1;
            *inRanged = c->isInRangedCombatMode() ? 1 : 0;
            *wants = c->shouldUseRangedWeapons() ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
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
    bool safeMirrorHealth(Character* c, const float* flesh, int n, float blood, const float* bandage = NULL, int nb = 0)
    {
        __try
        {
            MedicalSystem& m = c->medical;
            for (int i = 0; i < n && i < (int)m.anatomy.size(); ++i)
            {
                MedicalSystem::HealthPartStatus* p = m.anatomy[i];
                if (p->flesh != flesh[i]) { p->flesh = flesh[i]; p->updateDerivedHealths(); }
                if (i < nb && p->bandaging != bandage[i]) p->bandaging = bandage[i];   // treated wounds show as treated, here too
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
        // REPLACE, not merge: a ghost is spawned from a character template and gets its own random
        // looks (face, hair, build...). Keys the source character does not have would keep that random
        // value, so the ghost looked different from the real one (and differently after each reload).
        if (!b.s.empty() || !b.i.empty() || !b.f.empty() || !b.b.empty() || !b.lists.empty())
        {
            d->sdata.clear(); d->idata.clear(); d->fdata.clear(); d->bdata.clear();
            std::vector<std::string> stale;
            for (auto it = d->objectReferences.begin(); it != d->objectReferences.end(); ++it)
            {
                bool kept = false;
                for (size_t k = 0; k < b.lists.size() && !kept; ++k) kept = b.lists[k].first == it->first;
                if (!kept) stale.push_back(it->first);
            }
            for (size_t k = 0; k < stale.size(); ++k) d->clearList(stale[k]);
        }
        for (size_t k = 0; k < b.s.size(); ++k) d->sdata[b.s[k].first] = b.s[k].second;
        for (size_t k = 0; k < b.i.size(); ++k) d->idata[b.i[k].first] = b.i[k].second;
        for (size_t k = 0; k < b.f.size(); ++k) d->fdata[b.f[k].first] = b.f[k].second;
        for (size_t k = 0; k < b.b.size(); ++k) d->bdata[b.b[k].first] = b.b[k].second != 0;
        for (size_t k = 0; k < b.lists.size(); ++k)
        {
            const std::string& name = b.lists[k].first;
            d->clearList(name);
            const std::vector<AppearanceBlob::Ref>& refs = b.lists[k].second;
            for (size_t n = 0; n < refs.size(); ++n)
            {
                // Races and body parts from a mod we do not have: skip the reference instead of handing the engine a name it cannot resolve.
                if (!refs[n].sid.empty() && !dataOf(refs[n].sid))
                {
                    static std::set<std::string> told;
                    if (told.size() < 50 && told.insert(refs[n].sid).second) log("appearance: '%s' (%s) is unknown here (missing mod?), skipped", refs[n].sid.c_str(), name.c_str());
                    continue;
                }
                d->addToList(name, refs[n].sid, refs[n].v0, refs[n].v1, refs[n].v2);
            }
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
        // A dedicated server has no player of its own: its squad (the game needs one) is never shown to anybody.
        if (server_isServer()) { if (!g_local.empty()) { Lock l; g_local.clear(); } return; }
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
        for (uint32_t i = 0; i < m.anatomy.size() && i < 32; ++i) { s.flesh.push_back(m.anatomy[i]->flesh); s.bandage.push_back(m.anatomy[i]->bandaging); }

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

    void applyIncomingDamage(const DamageMsg& d, uint8_t sender)
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
        // A player hurt one of OUR characters (not a world NPC we host): an assault, Kenshi-style.
        if (!isNpcNetId(d.victimNetId) && sender != g_session.localId()) diplomacy_assaulted(sender);
    }

    // ------------------------------------------------------------------ treatment of copies
    // First aid / doctoring given to the copy of somebody else's character changes nothing for good (the owner's
    // state overwrites the copy): what the care did is measured on the copy, sent to the owner, and applied there.
    struct HealAcc { float d[32], b[32], r[32]; float blood; bool any; HealAcc() : blood(0), any(false) { for (int i = 0; i < 32; ++i) d[i] = b[i] = r[i] = 0; } };
    std::map<uint32_t, HealAcc> g_heal;
    DWORD g_lastHealSend = 0;
    long g_healsSent = 0, g_healsApplied = 0;

    bool safeApplyHeal(Character* c, const uint8_t* idx, const float* d, const float* bd, const float* rg, int n, float blood)
    {
        __try
        {
            MedicalSystem& m = c->medical;
            for (int i = 0; i < n; ++i)
            {
                if (idx[i] >= m.anatomy.size()) continue;
                MedicalSystem::HealthPartStatus* p = m.anatomy[idx[i]];
                float cap = p->maxHealth(), nf = p->flesh + d[i];
                if (nf > cap) nf = cap > p->flesh ? cap : p->flesh;
                if (nf > p->flesh) { p->flesh = nf; p->updateDerivedHealths(); }
                if (bd[i] > 0) p->bandaging += bd[i];     // what first aid does: the part heals and stops bleeding from it
                if (rg[i] > 0) p->juryRigging += rg[i];   // robot repair
            }
            if (blood > 0)
            {
                float mb = m.getMaxBlood(), nb = m.blood + blood;
                if (nb > mb) nb = mb > m.blood ? mb : m.blood;
                m.blood = nb;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void applyIncomingHeal(const HealMsg& h)
    {
        Character* c = localByNetId(h.victimNetId);   // our squad member or (host) a world NPC
        if (!c || h.parts.empty() && h.blood <= 0) return;
        uint8_t idx[32]; float d[32], bd[32], rg[32]; int n = 0;
        for (size_t i = 0; i < h.parts.size() && n < 32; ++i) { idx[n] = h.parts[i].idx; d[n] = h.parts[i].flesh; bd[n] = h.parts[i].bandage; rg[n] = h.parts[i].rig; ++n; }
        float before = 0, after = 0, bandBefore = 0, bandAfter = 0;
        if (g_cfg.debugKeys && n > 0 && idx[0] < c->medical.anatomy.size()) { before = c->medical.anatomy[idx[0]]->flesh; bandBefore = c->medical.anatomy[idx[0]]->bandaging; }
        bool ok = safeApplyHeal(c, idx, d, bd, rg, n, h.blood);
        if (ok) ++g_healsApplied;
        if (g_cfg.debugKeys)
        {
            if (n > 0 && idx[0] < c->medical.anatomy.size()) { after = c->medical.anatomy[idx[0]]->flesh; bandAfter = c->medical.anatomy[idx[0]]->bandaging; }
            log("treatment received for %08x: %d part(s), blood +%.1f, first part flesh %.2f -> %.2f, bandaging %.2f -> %.2f (%s)", h.victimNetId, n, h.blood, before, after, bandBefore, bandAfter, ok ? "applied" : "failed");
        }
    }

    // Every 200 ms: what the care changed since, one message per treated copy.
    void flushHeals(DWORD now)
    {
        if (g_heal.empty() || now - g_lastHealSend < 200) return;
        g_lastHealSend = now;
        for (std::map<uint32_t, HealAcc>::iterator it = g_heal.begin(); it != g_heal.end();)
        {
            HealAcc& a = it->second;
            if (a.any)
            {
                HealMsg m; m.victimNetId = it->first; m.blood = a.blood;
                for (int i = 0; i < 32; ++i)
                    if (a.d[i] > 0.001f || a.b[i] > 0.001f || a.r[i] > 0.001f) { HealPart hp; hp.idx = (uint8_t)i; hp.flesh = a.d[i]; hp.bandage = a.b[i]; hp.rig = a.r[i]; m.parts.push_back(hp); }
                ByteWriter w; w.u8(netIdOwner(it->first)); m.write(w);
                g_session.send(MSG_HEAL, w.data);
                ++g_healsSent;
                a = HealAcc();
                ++it;
            }
            else g_heal.erase(it++);
        }
    }

    // ------------------------------------------------------------------ carried characters
    // Our character, knocked out, carried / caged / put to bed by another player: that player's game moves the
    // copy and sends us where our real character must be. It stays ours: we only follow, for as long as the messages
    // keep coming (3 s without one: let go).
    struct HeldChar { uint32_t carrier; DWORD liftTry; DWORD liftedAt; uint8_t by; uint8_t state; Ogre::Vector3 pos; Ogre::Quaternion rot; int8_t floor; DWORD last, lastKo, pickUntil; bool escaping; };
    std::map<uint32_t, HeldChar> g_held;
    std::map<uint32_t, DWORD> g_heldFarSince;   // carried flag set but body far from the carrier: since when
    long g_heldMessages = 0;

    bool safeKnockOut(Character* c, float seconds)
    {
        __try { c->medical.knockoutForceTimer(seconds); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeForceKo(Character* c)
    {
        __try { c->medical.knockoutForceTimer(1.0f); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // What the engine says about a ghost that one of our characters took: 0 free, 1 carried, 2 in bed, 3 in a cage.
    int safeHeldState(Character* c)
    {
        __try
        {
            if (c->isBeingCarried()) return CarriedMsg::CARRIED;
            if (c->inSomething == IN_BED) return CarriedMsg::IN_BED;
            if (c->inSomething == IN_PRISON) return CarriedMsg::IN_CAGE;
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    // The carrier's copy on our side picks our character up (the engine then shows it carrying, and moves it).
    bool tryGhostLift(uint32_t carrier, Character* victim, const Ogre::Vector3* at);
    float ghostDistanceTo(uint32_t ghostId, Character* c);
    void dropByGhost(uint32_t carrier, Character* victim);
    bool safeBeingCarried(Character* c) { __try { return c->isBeingCarried(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
    void releaseHeld(uint32_t id, Character* c, const HeldChar& h)
    {
        if (c && h.carrier) dropByGhost(h.carrier, c);
        if (c) { Ogre::Vector3 p = h.pos; if (c->getPosition().distance(p) > 2.f) safeTeleport(c, &p, h.floor); }
        log("character %08x let go by %s", id, playerName(h.by).c_str());
    }
    void applyIncomingCarried(CarriedMsg m, uint8_t sender)
    {
        if (netIdOwner(m.netId) != g_session.localId() || sender == g_session.localId()) return;
        Character* c = localByNetId(m.netId);
        if (!c) return;
        std::map<uint32_t, HeldChar>::iterator it = g_held.find(m.netId);
        if (m.state == CarriedMsg::FREE)
        {
            if (it != g_held.end() && it->second.by == sender)
            {
                it->second.pos = Ogre::Vector3(m.x, m.y, m.z); it->second.floor = m.floor;
                releaseHeld(m.netId, c, it->second);
                g_held.erase(it);
            }
            return;
        }
        if (it == g_held.end())
        {
            if (safeDead(c) || !safeUnconscious(c)) return;   // only a knocked-out character can be taken
            HeldChar h; h.by = sender; h.state = m.state; h.last = h.lastKo = GetTickCount(); h.pickUntil = 0; h.carrier = 0; h.liftTry = 0; h.liftedAt = 0; h.floor = m.floor; h.escaping = false;
            h.pos = Ogre::Vector3(m.x, m.y, m.z); h.rot = Ogre::Quaternion(m.qw, m.qx, m.qy, m.qz);
            it = g_held.insert(std::make_pair(m.netId, h)).first;
            showMessage(TF("%s is carrying your knocked-out character.", playerName(sender).c_str()));
            log("character %08x taken by %s (state %d)", m.netId, playerName(sender).c_str(), (int)m.state);
        }
        else if (it->second.by != sender) return;   // the first one to take it keeps it
        HeldChar& h = it->second;
        if (h.state == CarriedMsg::CARRIED && m.state != CarriedMsg::CARRIED && h.carrier) { dropByGhost(h.carrier, c); h.carrier = 0; h.liftedAt = 0; }
        if (m.state == CarriedMsg::CARRIED && m.carrier) h.carrier = m.carrier;
        h.state = m.state; h.pos = Ogre::Vector3(m.x, m.y, m.z); h.rot = Ogre::Quaternion(m.qw, m.qx, m.qy, m.qz);
        h.floor = m.floor; h.last = GetTickCount();
        ++g_heldMessages;
    }
    // A character locked in another player's cage: the player orders it to pick the lock, like on a normal cage or chest.
    // The work takes 6 to 14 s (the better its lockpicking, the faster and the likelier: 25 % at 0, 90 % at 100);
    // a failure can be tried again.
    float safeLockpicking(Character* c)
    {
        __try { return c->stats ? c->stats->getStat(STAT_LOCKPICKING, false) : 0.f; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
    }
    // The cage the copy is in lets it go (our engine's own release of a prisoner).
    bool safeFreeFromCage(Character* c)
    {
        __try
        {
            if (c->inSomething != IN_PRISON) return false;
            Building* b = c->inWhat.getBuilding();
            UseableStuff* us = b ? b->getUseableStuff() : NULL;
            c->setPrisonMode(false, us);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void startPick(uint32_t id, Character* c, HeldChar& h, DWORD now)
    {
        float skill = safeLockpicking(c);
        if (skill < 0.f) skill = 0.f; if (skill > 100.f) skill = 100.f;
        h.pickUntil = now + 6000 + (DWORD)((100.f - skill) * 80.f);
        log("character %08x starts picking the lock of the cage (lockpicking %.0f, %lu ms)", id, skill, (unsigned long)(h.pickUntil - now));
        showMessage(T("Your character is picking the lock..."));
    }
    void resolvePick(uint32_t id, Character* c, HeldChar& h)
    {
        h.pickUntil = 0;
        float skill = safeLockpicking(c);
        if (skill < 0.f) skill = 0.f; if (skill > 100.f) skill = 100.f;
        float chance = 0.25f + skill / 100.f * 0.65f;
        bool ok = (rand() % 10000) < (int)(chance * 10000.f);
        static int testAttempts = 0;
        if (g_cfg.autotestCarry) ok = ++testAttempts >= 2;   // test: the first try fails, the second succeeds
        log("character %08x picked the lock of the cage (lockpicking %.0f, chance %.0f %%): %s", id, skill, chance * 100.f, ok ? "got out" : "failed");
        if (!ok) { showMessage(T("Your character failed to pick the lock.")); return; }
        h.escaping = true;
        ByteWriter w; w.u8(h.by); w.u32(id);
        g_session.send(MSG_ESCAPED, w.data);
        showMessage(T("Your character picked the lock and got out of the cage!"));
    }
    // Every frame: our carried characters follow the carrier's copy.

    void updateHeld(DWORD now)
    {
        for (std::map<uint32_t, HeldChar>::iterator it = g_held.begin(); it != g_held.end();)
        {
            Character* c = localByNetId(it->first);
            HeldChar& h = it->second;
            if (!c || safeDead(c) || now - h.last > 3000) { if (c && !safeDead(c)) releaseHeld(it->first, c, h); g_held.erase(it++); continue; }
            if (h.state != CarriedMsg::IN_CAGE && now - h.lastKo > 500)   // a carried or bedded character stays down (a prisoner may wake up)
            {
                h.lastKo = now;
                if (!safeUnconscious(c)) safeForceKo(c);
            }
            if (h.pickUntil && now >= h.pickUntil && !h.escaping) resolvePick(it->first, c, h);
            if (h.state == CarriedMsg::CARRIED && h.carrier)
            {
                if (safeBeingCarried(c))
                {
                    // The carrier's copy holds it: the engine moves it along (and draws it carried).
                    if (!h.liftedAt) { h.liftedAt = now; log("character %08x is carried by the copy of %08x (engine carry)", it->first, h.carrier); }
                    {   // pose check (a character that lies has its up axis near horizontal; standing = up.y near 1)
                        static DWORD lastPose = 0;
                        if (g_cfg.autotest && now - lastPose > 2000)
                        {
                            lastPose = now;
                            Ogre::Quaternion q = c->getOrientation();
                            Ogre::Vector3 up = q * Ogre::Vector3::UNIT_Y;
                            float dist = ghostDistanceTo(h.carrier, c);
                            log("carried pose: %08x up.y %.2f, unconscious %d, %.1f units from its carrier", it->first, up.y, (int)safeUnconscious(c), dist);
                        }
                    }
                    // "Carried" in the engine's flags but not attached to the carrier (it walks off, we stay on the ground):
                    // let it go and pick up again with the game's own pickup.
                    float apart = ghostDistanceTo(h.carrier, c);
                    if (apart > 10.f)
                    {
                        DWORD& fs = g_heldFarSince[it->first];
                        if (!fs) { fs = now; ++it; continue; }
                        if (now - fs < 1500) { ++it; continue; }
                        fs = 0;
                        log("carry: %08x is flagged carried but %.0f units from its carrier: picking it up again", it->first, apart);
                        dropByGhost(h.carrier, c);
                        h.liftedAt = 0; h.liftTry = 0;
                    }
                    else { g_heldFarSince.erase(it->first); ++it; continue; }
                }
                if (h.liftedAt) { h.liftedAt = 0; h.liftTry = now; }   // it slipped: we place it ourselves until the next try
                if (now - h.liftTry > 3000) { h.liftTry = now; tryGhostLift(h.carrier, c, &h.pos); }
            }
            Ogre::Vector3 cur = c->getPosition();
            float d = cur.distance(h.pos);
            if (d > 12.f) safeTeleport(c, &h.pos, h.floor);
            else if (d > 0.05f) { Ogre::Vector3 np = cur + (h.pos - cur) * 0.5f; safeNudge(c, &np); }
            ++it;
        }
    }
    // Which of our characters carries this copy (so its owner's game shows the carrier carrying it).
    uint32_t carrierOf(Character* victim);
    void sendCarried(uint32_t id, int state, Character* c)
    {
        CarriedMsg m; m.netId = id; m.state = (uint8_t)state;
        if (state == CarriedMsg::CARRIED) m.carrier = carrierOf(c);
        Ogre::Vector3 p = c->getPosition(); Ogre::Quaternion q = c->getOrientation();
        m.x = p.x; m.y = p.y; m.z = p.z; m.qx = q.x; m.qy = q.y; m.qz = q.z; m.qw = q.w;
        int fl = c->getFloor(); m.floor = (int8_t)(fl < 0 ? 0 : (fl > 20 ? 20 : fl));
        ByteWriter w; w.u8(netIdOwner(id)); m.write(w);
        g_session.send(MSG_CARRIED, w.data);
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
        Ogre::Vector3 prevTarget, measVel;   // target position last frame, and the velocity it implies (smoothed)
        bool hasPrevTarget;
        DWORD stallSince; bool unstickSent;   // stalled far from the owner: since when, unstick order given
        Ogre::Vector3 placedFrom; int placeFails; DWORD noPlaceUntil;   // placements the engine ignored
        DWORD lastTpAt; float prevDist; Ogre::Vector3 prevJTarget, prevBody;   // our last placement; offset and target one frame ago
        bool held; int heldState; DWORD lastCarrySend;                    // one of our characters carries / cages / beds this copy
        Ogre::Vector3 prevJ; bool hasPrevJ; float lastNudge;             // frame-to-frame jump measure (tracking stats)
        DWORD settleUntil;            // after a teleport: no snap/unblock checks until then
        DWORD lastSpeedCmd;             // speed matching: last command
        float speedCmd, baseMaxSpeed;   // value sent; the body's own max (restored when slow)
        double freezeMs;                // current streak: owner moving, body not
        DWORD stuckSince;               // owner still, ghost still far: since when
        Ogre::Vector3 stuckTarget;      // last target we unstuck it to (no teleport loop on a blocked spot)
        int floorTries; int floorFor;   // floor corrections made for the owner's current floor
        Ogre::Vector3 knownPos;         // owner's last reported position, body or not (regroup)
        DWORD knownAt;                  // when (0: never)
        EntitySpawn spawn; bool hasSpawn; // the spawn message, to create the body later when it could not be made (or was too far)
        DWORD lastBodyTry; int bodyTries;
        Ghost() : knownPos(Ogre::Vector3::ZERO), knownAt(0), hasSpawn(false), lastBodyTry(0), bodyTries(0), floorTries(0), floorFor(-1), hasPendingAppearance(false), hasPendingEquipment(false), hasLimbs(false), hasPendingStats(false), pendingHunger(-1.f), pendingRanged(-1),
                  combatTarget(NULL), mirroredTask(-1), lastSpeed(-1), mode(MOVE), intervalMs(100),
                  lastSnapT(0), lastMoveCmd(0), lastFrame(0), hasVisual(false), visualPos(Ogre::Vector3::ZERO),
                  visualRot(Ogre::Quaternion::IDENTITY), blend(0), lastDrawn(Ogre::Vector3::ZERO),
                  stuckSince(0), stuckTarget(Ogre::Vector3::ZERO), lastBody(Ogre::Vector3::ZERO), freezeMs(0), lastDest(Ogre::Vector3::ZERO), hasDest(false), prevTarget(Ogre::Vector3::ZERO), measVel(Ogre::Vector3::ZERO), hasPrevTarget(false), stallSince(0), unstickSent(false), placedFrom(Ogre::Vector3(1e9f, 0, 1e9f)), placeFails(0), noPlaceUntil(0), held(false), heldState(0), lastCarrySend(0), lastTpAt(0), prevDist(0), prevJTarget(Ogre::Vector3::ZERO), prevBody(Ogre::Vector3::ZERO), prevJ(Ogre::Vector3::ZERO), hasPrevJ(false), lastNudge(0),
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
    const int MAX_GHOSTS_PER_PLAYER = 1200;  // squads are <= ~256; the host also streams the NPCs of its whole zone (npc range)

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

    // A host shopkeeper's ghost lives in its own squad made from the host's squad template, with the host's
    // price multiplier: that is what makes the game treat it as a trader (reviews: "nothing to buy at the
    // trader", "I sell and get no money").
    std::map<uint32_t, Platoon*> g_traderSquads;
    bool safeSetTrade(Platoon* p, float mult, int flags)
    {
        __try { p->priceMultWhenITrade = mult; p->_iBuyStolenGoods = (flags & 1) != 0; p->_iBuyIllegalGoods = (flags & 2) != 0; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    GameData* safeDataByName(const std::string* name, itemType category)
    {
        __try { return ou->gamedata.getDataByName(*name, category); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    ActivePlatoon* traderSquadFor(const EntitySpawn& sp, Faction* f, const Ogre::Vector3& pos)
    {
        std::map<uint32_t, Platoon*>::iterator it = g_traderSquads.find(sp.netId);
        if (it != g_traderSquads.end())
            if (ActivePlatoon* a = safeActive(it->second)) return a;
        // The host's squad is a numbered copy of its template ("Marchands Libres_0"): try the name as is, then without the number.
        // The id is the numbered copy's name, not a template id: look the template up by id, then by name
        // (with and without the number) among the squad templates.
        GameData* squadData = dataOf(sp.squadSid);
        std::string bare = sp.squadSid;
        {
            size_t us = bare.find_last_of('_');
            if (us != std::string::npos && us + 1 < bare.size() && bare.find_first_not_of("0123456789", us + 1) == std::string::npos) bare = bare.substr(0, us);
        }
        const itemType cats[3] = { SQUAD_TEMPLATE, UNIQUE_SQUAD_TEMPLATE, PLATOON };
        for (int i = 0; i < 3 && !squadData; ++i) squadData = safeDataByName(&sp.squadSid, cats[i]);
        for (int i = 0; i < 3 && !squadData; ++i) squadData = safeDataByName(&bare, cats[i]);
        if (squadData) log("trader %08x: squad '%s' -> template '%s' (%s)", sp.netId, sp.squadSid.c_str(), squadData->name.c_str(), squadData->stringID.c_str());
        if (!squadData) { log("trader %08x: unknown squad template '%s' (missing mod?), ghost created as a plain NPC", sp.netId, sp.squadSid.c_str()); return NULL; }
        Platoon* p = safeNewPlatoon(f, squadData, &pos);
        if (!p) return NULL;
        safeSetTrade(p, sp.priceMult, sp.tradeFlags);
        g_traderSquads[sp.netId] = p;
        return safeActive(p);
    }
    // After the body exists: what the engine thinks of it (and, in test runs, what a trade would show).
    bool safeTraderState(Character* c, int* isTrader, int* vendorList, int* platoonTrader)
    {
        __try
        {
            ActivePlatoon* ap = c->getPlatoon();
            *isTrader = c->isATrader() ? 1 : 0;
            *vendorList = ap ? (ap->getHasVendorList() ? 1 : 0) : -1;
            *platoonTrader = ap ? (ap->getIsTrader() ? 1 : 0) : -1;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeSetupTrader(Character* c, bool restock)
    {
        __try
        {
            ActivePlatoon* ap = c->getPlatoon();
            if (!ap) return false;
            ap->setupTraderBuildings();
            if (restock) ap->refreshInventory(true);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeShelfQuery(lektor<RootObject*>* out, const Ogre::Vector3* at, float radius)
    {
        __try { ou->getObjectsWithinSphere(*out, *at, radius, BUILDING, 200, NULL); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    int safeShelfBuildingItems(Building* b)
    {
        __try
        {
            if (!b || b->getSpecialFunction() != BF_SHOP) return 0;
            Inventory* inv = b->getInventory();
            return inv ? (int)inv->getAllItems().size() : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    int safeShelfItems(Character* c, float radius)   // items held by shop buildings within radius (test probe)
    {
        Ogre::Vector3 at = c->getPosition();
        lektor<RootObject*> around;
        if (!safeShelfQuery(&around, &at, radius)) return -1;
        int total = 0;
        for (uint32_t i = 0; i < around.size(); ++i) total += safeShelfBuildingItems(static_cast<Building*>(around[i]));
        return total;
    }

    bool safeGhostDrop(Character* carrier, Character* victim)
    {
        __try
        {
            if (carrier->getCarryingObject() == victim) carrier->dropCarriedObject(true, false);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // The game's own way: the carrier picks the other up (Character::pickupObject, which calls victim->getPickedUp(carrier)):
    // the engine then sets the carried state, the pose and the attachment to the carrier's body, as when an NPC carries.
    std::map<uint32_t, int> g_directPickups;   // carrier -> direct pickups tried since it last held somebody
    bool safePickup(Character* carrier, Character* victim)
    {
        __try
        {
            if (carrier->isCarryingSomething || carrier->isBeingCarried()) return false;
            carrier->pickupObject(victim);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool tryGhostLift(uint32_t carrier, Character* victim, const Ogre::Vector3* at)
    {
        std::map<uint32_t, Ghost>::iterator g = g_ghosts.find(carrier);
        if (g == g_ghosts.end()) return false;
        Character* cc = g->second.h.getCharacter();
        if (!cc || safeDead(cc) || safeUnconscious(cc)) return false;
        if (g_directPickups[carrier] < 3)
        {
            ++g_directPickups[carrier];
            bool picked = safePickup(cc, victim);
            log("carry: the copy of %08x picks our character up (engine pickupObject, try %d) -> %s", carrier, g_directPickups[carrier], picked ? "called" : "refused");
            if (picked) return true;
        }
        bool ok = safeOrder(cc, LIFT_PERSON_SNATCHING_ALLOWED, victim, at);
        log("carry: the copy of %08x is told to lift our character -> %s", carrier, ok ? "ordered" : "refused");
        return ok;
    }
    float ghostDistanceTo(uint32_t ghostId, Character* c)
    {
        std::map<uint32_t, Ghost>::iterator g = g_ghosts.find(ghostId);
        Character* gc = g != g_ghosts.end() ? g->second.h.getCharacter() : NULL;
        return gc ? gc->getPosition().distance(c->getPosition()) : -1.f;
    }
    void dropByGhost(uint32_t carrier, Character* victim)
    {
        g_directPickups.erase(carrier);
        std::map<uint32_t, Ghost>::iterator g = g_ghosts.find(carrier);
        if (g == g_ghosts.end()) return;
        Character* cc = g->second.h.getCharacter();
        if (cc) safeGhostDrop(cc, victim);
    }
    // The carrying character of one of our (host-side) copies: which of our characters holds it.
    bool safeHolds(Character* lc, Character* victim)
    {
        __try { return lc->getCarryingObject() == victim; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    uint32_t carrierOf(Character* victim)
    {
        for (size_t i = 0; i < g_local.size(); ++i)
            if (Character* lc = g_local[i].h.getCharacter()) if (safeHolds(lc, victim)) return g_local[i].id;
        return 0;
    }

    // The shop counters of the town, as our game created them, carry a junk "shop owner" (a non-empty hand that points
    // to nothing); on the host it is empty. The game's trade window only lists the counters without a valid owner (or
    // owned by the trader), so the copy of a trader showed an empty shop. We empty it.
    bool fixCounterOwner(Building* b)
    {
        __try
        {
            if (b->getSpecialFunction() != BF_SHOP) return false;
            UseableStuff* us = b->getUseableStuff();
            if (!us) return false;
            hand none;
            if (us->shopOwner == none || us->shopOwner.getRootObject()) return false;   // empty already, or a real owner
            us->shopOwner = none;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    int fixShopOwnersAround(Character* c)
    {
        Ogre::Vector3 at = c->getPosition();
        lektor<RootObject*> around;
        if (!safeShelfQuery(&around, &at, 120.f)) return 0;
        int n = 0;
        for (uint32_t i = 0; i < around.size(); ++i) if (fixCounterOwner(static_cast<Building*>(around[i]))) ++n;
        return n;
    }
    // A real trader's squad has a home town and a home building (its bar, its shop): that is how the game finds the
    // shop counters for its trade window. The copy's squad has neither, so its window was empty. We give it the
    // town and the building it stands in.
    struct HomeLink { int did, shops; char home[80]; };
    void linkHomeInner(Character* c, lektor<RootObject*>* around, HomeLink* hl)
    {
        Ownerships* o = c->getOwnerships();
        ActivePlatoon* ap = c->getPlatoon();
        if (!o || !ap) return;
        if (o->_homeTown) { hl->did = 2; return; }                         // already has one
        Building* counter = NULL; float best = 1e30f; Ogre::Vector3 at = c->getPosition();
        for (uint32_t i = 0; i < around->size(); ++i)
        {
            Building* b = static_cast<Building*>((*around)[i]);
            if (b->getSpecialFunction() != BF_SHOP) continue;
            float d = b->getPosition().distance(at);
            if (d < best) { best = d; counter = b; }
        }
        if (!counter) return;
        TownBase* town = counter->getTown();
        if (!town) return;
        hand home = c->isIndoors();
        if (!home.getBuilding()) home = hand(counter);
        o->setHomeTown(town, SQ_RESIDENT);
        o->setHomeBuilding(home, SQ_RESIDENT);
        ap->setupTraderBuildings();
        lektor<Building*> a; o->getBuildingsWithFunction(a, BF_SHOP); hl->shops = (int)a.size();
        strncpy_s(hl->home, home.toString().c_str(), _TRUNCATE);
        hl->did = 1;
    }
    bool safeLinkHome(Character* c, lektor<RootObject*>* around, HomeLink* hl)
    {
        __try { linkHomeInner(c, around, hl); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool linkTraderHome(Character* c)   // true once the copy has a home
    {
        Ogre::Vector3 at = c->getPosition();
        lektor<RootObject*> around;
        if (!safeShelfQuery(&around, &at, 80.f)) return false;
        HomeLink hl; memset(&hl, 0, sizeof(hl));
        if (!safeLinkHome(c, &around, &hl)) { problem("shophome", "the shop of a trader could not be linked"); return false; }
        if (hl.did == 1) log("trader copy '%s' linked to its shop: home building [%s], the game finds %d shop building(s) for it now", c->getName().c_str(), hl.home, hl.shops);
        return hl.did != 0;
    }
    void fixShopOwners(DWORD now)
    {
        static DWORD last = 0;
        if (now - last < 2000) return;
        last = now;
        for (std::map<uint32_t, Platoon*>::iterator it = g_traderSquads.begin(); it != g_traderSquads.end(); ++it)
        {
            std::map<uint32_t, Ghost>::iterator g = g_ghosts.find(it->first);
            if (g == g_ghosts.end()) continue;
            Character* c = g->second.h.getCharacter();
            if (!c) continue;
            linkTraderHome(c);
            int n = fixShopOwnersAround(c);
            static int logged = 0;
            if (n && logged++ < 6) log("shop counters around trader copy '%s': %d owner(s) cleared (was junk)", c->getName().c_str(), n);
        }
    }

    void spawnGhost(uint8_t owner, const EntitySpawn& sp)
    {
        Ghost& g = g_ghosts[sp.netId];
        g.knownPos = Ogre::Vector3(sp.x, sp.y, sp.z); g.knownAt = GetTickCount();
        g.spawn = sp; g.hasSpawn = true;
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
        // Tolerant to other mod lists: a faction or a character template from a mod we do not have is replaced by the
        // nearest thing we do have (the sender's mirror faction, one of our own characters' template) instead of
        // dropping the character; its looks, gear and name still come from the owner.
        if (worldNpc && !f)
        {
            f = factionFor(owner);
            if (f) { static int n = 0; if (n++ < 10) log("faction '%s' of %08x is unknown here (missing mod?): using the sender's faction", sp.factionSid.c_str(), sp.netId); }
        }
        if (!tmpl && sp.kind == KIND_CHARACTER)
        {
            for (size_t i = 0; i < g_local.size() && !tmpl; ++i)
                if (Character* lc = g_local[i].h.getCharacter()) tmpl = lc->data;
            if (tmpl) { static int n = 0; if (n++ < 10) log("template '%s' of %08x is unknown here (missing mod?): using '%s'", sp.gameDataName.c_str(), sp.netId, tmpl->stringID.c_str()); }
        }
        if (!f || !tmpl)
        {
            log("cannot spawn ghost %08x (template '%s', faction '%s')", sp.netId, sp.gameDataName.c_str(), sp.factionSid.c_str());
            problem("spawn", "a character of the other player(s) could not be created here (template '%s'): a mod they use may be missing or different", sp.gameDataName.c_str());
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
        ActivePlatoon* squad = NULL;
        bool trader = worldNpc && sp.hasTrade;
        if (trader) squad = traderSquadFor(sp, f, pos);
        if (!squad) { trader = false; squad = ghostSquadFor(owner, f, pos); }
        if (!squad) { log("no ghost squad for player %d", (int)owner); return; }
        Character* c = static_cast<Character*>(safeSpawn(f, &pos, tmpl, squad));
        if (!c && sp.kind == KIND_CHARACTER && g.bodyTries >= 2)
        {
            // The factory keeps refusing this template (a unique character the world believes exists elsewhere, a template
            // that needs its own squad...): make it from one of our own characters' template so it is at least there,
            // dressed and named like the original (its looks and gear arrive next). Better than an invisible person.
            // The template comes from a world NPC copy that was made fine (a generic one, never one of our own characters'
            // data: creating from that adds a character to our own squad, in a loop).
            GameData* alt = NULL;
            for (std::map<uint32_t, Ghost>::iterator o = g_ghosts.begin(); o != g_ghosts.end() && !alt; ++o)
            {
                if (!isNpcNetId(o->first) || o->first == sp.netId) continue;
                Character* oc = o->second.h.getCharacter();
                if (oc && oc->data && oc->data != tmpl && !oc->data->stringID.empty()) alt = oc->data;
            }
            static int madeFallback = 0;
            if (alt && madeFallback < 60)
            {
                c = static_cast<Character*>(safeSpawn(f, &pos, alt, squad));
                if (c) ++madeFallback;
                static int told = 0;
                if (c && told++ < 8) log("template '%s' of %08x is refused by the factory here: made from '%s' instead", sp.gameDataName.c_str(), sp.netId, alt->stringID.c_str());
            }
        }
        if (!c) { static int shown = 0; if (shown++ < 8) log("factory returned no character for %08x (will try again)", sp.netId); return; }
        if (trader)
        {
            int isT = -1, vend = -1, platT = -1;
            safeTraderState(c, &isT, &vend, &platT);
            bool probe = g_cfg.autotest || g_cfg.debugKeys;
            int shelvesBefore = probe ? safeShelfItems(c, 80.f) : 0;
            bool set = safeSetupTrader(c, probe);   // (the restock is a test-run experiment for now)
            static int logged = 0;
            if (logged++ < 8)
                log("trader ghost %08x '%s': squad '%s', isATrader=%d, vendor list=%d, squad isTrader=%d, price x%.2f; setupTraderBuildings %s; shop items within 80 units %d -> %d",
                    sp.netId, sp.displayName.c_str(), sp.squadSid.c_str(), isT, vend, platT, sp.priceMult, set ? "called" : "FAILED", shelvesBefore, probe ? safeShelfItems(c, 80.f) : 0);
        }
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
        g.knownPos = Ogre::Vector3(s.x, s.y, s.z); g.knownAt = GetTickCount();
        Character* c = g.h.getCharacter();
        if (!c) return;
        if (g_cfg.debugKeys && !isNpcNetId(s.netId) && g.last.flags != s.flags)
            log("ghost %08x: owner flags %d -> %d (received)", s.netId, (int)g.last.flags, (int)s.flags);
        g.last = s;

        Snapshot snap;
        snap.t = snapLocalMs;
        snap.x = s.x; snap.y = s.y; snap.z = s.z;
        snap.qx = s.qx; snap.qy = s.qy; snap.qz = s.qz; snap.qw = s.qw;
        snap.vx = s.vx; snap.vy = s.vy; snap.vz = s.vz;
        if (g.lastSnapT > 0 && snapLocalMs > g.lastSnapT)
        {
            double gap = snapLocalMs - g.lastSnapT;
            // NPC LOD changes the rate. A still body refreshes rarely: that long gap says nothing about
            // how fast it will be sent once it moves, so it is ignored; a shorter gap is believed at once.
            bool still = fabsf(s.vx) + fabsf(s.vz) < 0.5f;
            if (gap < 2000 && !(still && gap > 300)) g.intervalMs += (gap - g.intervalMs) * (gap < g.intervalMs ? 0.6 : 0.2);
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
        if (!s.flesh.empty() && !safeMirrorHealth(c, &s.flesh[0], (int)s.flesh.size(), s.blood, s.bandage.empty() ? NULL : &s.bandage[0], (int)s.bandage.size()))
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
                if (g_cfg.debugKeys)
                {
                    int bow = -1, rangedMode = -1, inRanged = -1, wants = -1;
                    safeRangedInfo(c, &bow, &rangedMode, &inRanged, &wants);
                    log("ghost %08x ranged state: weapon %d, ranged toggle %d, in ranged mode %d, should use ranged %d, distance %.0f",
                        s.netId, bow, rangedMode, inRanged, wants, c->getPosition().distance(enemy->getPosition()));
                }
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
        // A player's ghost must stand on its owner's exact spot (an order would otherwise walk it away from the owner's path).
        // A world NPC is not steered by us when it is still: it may be a few steps from its chair or machine, the order
        // sends it there and the engine sits it down (it stayed standing at the table before: 3 units is a hand's breadth).
        const float taskRange = isNpcNetId(s.netId) ? NPC_TASK_RANGE : TASK_RANGE;
        if (s.task != 0xFFFF && isMirroredTask(s.task))
        {   // why a whitelisted task is not copied (test statistics, logged with the authority dump)
            if (dist >= taskRange) ++g_tmFar; else if (s.vx * s.vx + s.vz * s.vz >= 1.f) ++g_tmMoving; else ++g_tmWanted;
        }
        if (s.task != 0xFFFF && isMirroredTask(s.task) && dist < taskRange && s.vx * s.vx + s.vz * s.vz < 1.f)
        {
            g.mode = Ghost::TASK;
            if ((int)s.task != g.mirroredTask || s.taskSubject != g.mirroredSubject)
            {
                RootObject* subject = resolveTargetRef(s.taskSubject);
                Ogre::Vector3 loc(s.tx, s.ty, s.tz);
                if (!subject && s.taskSubject.kind != TargetRef::NONE) { ++g_tmNoSubject; if (g_tmNoSubject <= 6)
                {
                    std::string near_;
                    lektor<RootObject*> around; Ogre::Vector3 sp(s.taskSubject.x, s.taskSubject.y, s.taskSubject.z);
                    int n = safeAroundBuildings(&around, &sp, 12.f);
                    for (int i = 0; i < n && i < 6; ++i)
                    {
                        Building* bb = static_cast<Building*>(around[i]);
                        char b[120]; sprintf_s(b, "%s@%.1f ", bb && bb->data ? bb->data->stringID.c_str() : "?", bb ? bb->getPosition().distance(sp) : -1.f); near_ += b;
                    }
                    log("task %d of %08x: subject kind %d '%s' at (%.0f,%.0f,%.0f) not found here; %d building(s) within 12 units: %s", (int)s.task, s.netId, (int)s.taskSubject.kind, s.taskSubject.sid.c_str(), sp.x, sp.y, sp.z, n, near_.c_str());
                } }
                bool orderOk = (subject || s.taskSubject.kind == TargetRef::NONE) ? safeOrder(c, s.task, subject, &loc) : false;
                if ((subject || s.taskSubject.kind == TargetRef::NONE) && !orderOk) ++g_tmOrderFail;
                if (orderOk)
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
    long g_microJumps = 0, g_bigJumps = 0, g_jumpFrames = 0; double g_jumpMax = 0;
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
    const float SNAP_FAR = 250.f;          // beyond this a running ghost is placed anyway
    const DWORD STALL_UNSTICK_MS = 1000;   // standing still and far: a direct move order first
    const DWORD STALL_PLACE_MS = 3000;     // still not moving after that long: placed
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
    long g_unblocks = 0, g_recreated = 0;
    long g_trailGlides = 0;
    const float NUDGE_BASE_SPEED = 1.5f;   // units/s of correction even for a tiny gap
    const float NUDGE_GAIN = 2.5f;         // + this per unit of gap (4 units -> 11.5 u/s)
    bool g_nudgeBroken = false;
    const DWORD STEER_PERIOD_MS = 250;     // re-issue move orders at most this often (keeps momentum)

    void driveGhosts(DWORD now)
    {
        std::vector<uint32_t> recreate;
        for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
        {
            Ghost& g = it->second;
            Character* c = g.h.getCharacter();
            if (!c || g.interp.empty()) continue;

            // Taken by one of our characters (knocked out, carried / caged / in bed): our engine moves the copy;
            // its owner is told where it is, and our own tracking leaves it alone.
            if (!isNpcNetId(it->first))
            {
                int hs = safeHeldState(c);
                if (hs)
                {
                    if (!g.held) log("ghost %08x taken (state %d)", it->first, hs);
                    g.held = true; g.heldState = hs; g.blend = 0.f;
                    if (now - g.lastCarrySend >= 100) { g.lastCarrySend = now; sendCarried(it->first, hs, c); }
                    continue;
                }
                if (g.held)
                {
                    g.held = false; g.heldState = 0;
                    sendCarried(it->first, 0, c);
                    log("ghost %08x let go", it->first);
                    g.follow.reset(); g.trail.clear(); g.hasDest = false; g.settleUntil = now + SETTLE_MS; g.lastMoveAt = preciseTick();
                }
            }

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
            // The owner's reported velocity can be missing (a stale or sparse sample reads 0 while the
            // body clearly moves): then the speed is what the target itself does, so the engine still
            // gets move orders and walks the ghost with its animation instead of it being placed.
            {
                if (g.hasPrevTarget && frameMs > 1.0)
                {
                    float fs = (float)(frameMs / 1000.0);
                    Ogre::Vector3 mv((target.x - g.prevTarget.x) / fs, 0, (target.z - g.prevTarget.z) / fs);
                    if (mv.length() > 120.f) mv = Ogre::Vector3::ZERO;   // a jump, not a walk
                    g.measVel = g.measVel * 0.85f + mv * 0.15f;
                }
                g.prevTarget = target; g.hasPrevTarget = true;
                float rep = sqrtf(smp.vx * smp.vx + smp.vz * smp.vz);
                if (rep < 0.5f && g.measVel.length() > 2.f) { smp.vx = g.measVel.x; smp.vz = g.measVel.z; }
            }
            g.hasVisual = true;
            g.visualPos = target;
            g.visualRot = Ogre::Quaternion(smp.qw, smp.qx, smp.qy, smp.qz);

            if (g.mode == Ghost::FROZEN) continue;
            // Horizontal error: the height is the ground's business (slopes, stairs).
            Ogre::Vector3 gpos = c->getPosition();
            float dist = sqrtf((gpos.x - target.x) * (gpos.x - target.x) + (gpos.z - target.z) * (gpos.z - target.z));
            float speedNow = sqrtf(smp.vx * smp.vx + smp.vz * smp.vz);
            // Visible jumps of the body from one frame to the next that neither the engine's own speed nor
            // a placement of ours explains (statistics + the first few in the log, to find their cause).
            {
                bool settlingJ = now < g.settleUntil;
                if (g.hasPrevJ && !settlingJ && frameMs > 0 && fabsf(g.prevJ.x) + fabsf(g.prevJ.z) > 100.f)   // (not drawn yet: at the origin)
                {
                    float fs = (float)(frameMs / 1000.0);
                    // What is DRAWN is what the player sees (the render layer smooths the body's position).
                    Ogre::Vector3 shown = gpos; safeDrawnPos(c, &shown);
                    float disp = sqrtf((shown.x - g.prevJ.x) * (shown.x - g.prevJ.x) + (shown.z - g.prevJ.z) * (shown.z - g.prevJ.z));
                    float bodyDisp = sqrtf((gpos.x - g.prevBody.x) * (gpos.x - g.prevBody.x) + (gpos.z - g.prevBody.z) * (gpos.z - g.prevBody.z));
                    float eng = c->getMovementSpeed(); if (eng < speedNow) eng = speedNow;
                    float allowed = (eng * 1.4f + 8.f) * fs + 1.2f;
                    ++g_jumpFrames;
                    if (disp > allowed)
                    {
                        if (disp >= 20.f) ++g_bigJumps; else ++g_microJumps;
                        if (disp > g_jumpMax) g_jumpMax = disp;
                        static int shown = 0, shownBig = 0;
                        if (g_cfg.debugKeys && (disp >= 20.f ? shownBig < 60 : shown < 25))
                        {
                            if (disp >= 20.f) ++shownBig; else ++shown;
                            log("jump %08x%s: SEEN %.1f (body %.1f) in %.0f ms (allowed %.1f), offset %.1f (was %.1f), target moved %.1f, our last placement %d ms ago, target speed %.1f, engine speed %.1f, mode %d, last nudge %.2f, indoors %d",
                                it->first, isNpcNetId(it->first) ? " (NPC)" : "", disp, bodyDisp, frameMs, allowed, dist, g.prevDist, sqrtf((target.x - g.prevJTarget.x) * (target.x - g.prevJTarget.x) + (target.z - g.prevJTarget.z) * (target.z - g.prevJTarget.z)), g.lastTpAt ? (int)(now - g.lastTpAt) : -1, speedNow, eng, (int)g.mode, g.lastNudge, c->isIndoors().getBuilding() ? 1 : 0);
                        }
                    }
                }
                { Ogre::Vector3 shown2 = gpos; safeDrawnPos(c, &shown2); g.prevJ = shown2; g.prevBody = gpos; }
                g.hasPrevJ = true; g.lastNudge = 0; g.prevDist = dist; g.prevJTarget = target;
            }
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
            // A ghost that stands still while far from its owner first gets a direct order (unstick);
            // only after STALL_PLACE_MS of that, or when it is absurdly far, is it placed.
            bool placeNow = false;
            if (!settling && dist > SNAP_DISTANCE + speedNow * 0.6f && safeStalled(c))
            {
                if (!g.stallSince) { g.stallSince = now; g.unstickSent = false; }
                DWORD stalledFor = now - g.stallSince;
                if (!g.unstickSent && stalledFor > STALL_UNSTICK_MS) { g.unstickSent = true; g.lastMoveCmd = now; g.lastDest = target; g.hasDest = true; safeMoveTo(c, &target); }
                else if (stalledFor > STALL_PLACE_MS) placeNow = true;
            }
            else { g.stallSince = 0; g.unstickSent = false; }
            // A placement the engine ignores (the spot is not loaded here...) is not repeated: after 3 in a
            // row that left the body where it was, no more for a while (it would only flicker).
            if (now < g.noPlaceUntil) placeNow = false;
            if (!settling && (dist > SNAP_FAR || placeNow) && now >= g.noPlaceUntil)
            {
                if (g.placedFrom.distance(gpos) < 1.f)
                {
                    if (g_cfg.debugKeys)
                        log("placement ignored %08x: dist %.1f, dead %d, unconscious %d, owner flags %d, mode %d, indoors %d, floor %d (owner %d), fails %d",
                            it->first, dist, (int)safeDead(c), (int)safeUnconscious(c), (int)g.last.flags, (int)g.mode,
                            c->isIndoors().getBuilding() ? 1 : 0, c->getFloor(), (int)g.last.floor, g.placeFails + 1);
                    // The body sits in a zone the engine no longer simulates: recreate it where its owner is
                    // (the owner re-announces its spawns every few seconds).
                    if (++g.placeFails >= 3) { g.noPlaceUntil = now + 15000; g.placeFails = 0; recreate.push_back(it->first); }
                }
                else g.placeFails = 0;
                g.placedFrom = gpos;
                g.stallSince = 0; g.unstickSent = false;
                ++g_snaps;
                static DWORD lastSnapLog = 0;
                if (g_cfg.debugKeys && now - lastSnapLog > 2000)
                {
                    lastSnapLog = now;
                    log("snap %08x: dist %.1f ghost (%.1f,%.1f,%.1f) -> target (%.1f,%.1f,%.1f), mode %d",
                        it->first, dist, gpos.x, gpos.y, gpos.z, target.x, target.y, target.z, (int)g.mode);
                }
                { g.lastTpAt = now; safeTeleport(c, &target, g.last.floor); }
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
                { g.lastTpAt = now; safeTeleport(c, &target, g.last.floor); }
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
                    { g.lastTpAt = now; safeTeleport(c, &target, g.last.floor); }
                    g.stuckTarget = target; g.stuckSince = 0; g.follow.reset(); g.settleUntil = now + SETTLE_MS;
                    continue;
                }
            }
            else g.stuckSince = 0;
            static DWORD lastDiag = 0, lastNpcDiag = 0;
            bool npcGhost = isNpcNetId(it->first);
            if (g_cfg.debugKeys && now - (npcGhost ? lastNpcDiag : lastDiag) > 1500 && (!npcGhost || dist > 12.f))   // players, and host NPCs that lag
            {
                (npcGhost ? lastNpcDiag : lastDiag) = now;
                SpeedInfo si;
                if (safeSpeedInfo(c, &si))
                    log("drive %08x%s: target speed %.1f, offset %.2f | engine current %.1f max %.1f orders %d running %d pathOk %d pathFailed %d | owner flags %d delay %.0f ms (send gap %.0f, jitter %.0f), mode %d, indoors %d",
                        it->first, isNpcNetId(it->first) ? " (NPC)" : "", speed, dist, si.current, si.max, si.orders, (int)si.running, (int)si.pathOk, (int)si.pathFailed, (int)g.last.flags, delay, g.intervalMs, clock.jitterMs(), (int)g.mode, c->isIndoors().getBuilding() ? 1 : 0);
            }
            // Blocked while its owner moves (wedged on a corner): stop pushing in a straight line and
            // walk to the owner's exact spot. Only a long, real stall (engine stopped or no path)
            // puts it back on the path, with a cooldown: a teleport restarts the locomotion from
            // zero, so teleporting too eagerly would only feed the freeze.
            bool blocked = g_cfg.tuneUnblock && speedNow > 2.f && g.freezeMs > BLOCKED_MS;
            if (g_cfg.tuneUnblock && !settling && speedNow > 2.f && dist > 2.f && (g.freezeMs > UNBLOCK_HARD_MS || (g.freezeMs > UNBLOCK_TELEPORT_MS && safeStalled(c))))
            {
                ++g_unblocks;
                { g.lastTpAt = now; safeTeleport(c, &target, g.last.floor); }
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
                    // Catching up is gentle: a lag closes over a couple of seconds at most 1.75x the owner's speed
                    // (a sprint at 150 u/s behind a target that hopped looks like a teleport).
                    float catchUp = dist > 3.f ? dist * 1.5f : 0.f;
                    if (catchUp > speed * 0.6f + 12.f) catchUp = speed * 0.6f + 12.f;
                    float want = speed * 1.15f + catchUp;
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
                        else { ++g_trailGlides; g.lastNudge += step; }
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
                    else g.lastNudge += step;
                }
            }
        }
        for (size_t i = 0; i < recreate.size(); ++i)
        {
            ++g_recreated;
            problem("recreate", "a character copy could not be placed and was created again (%ld so far)", g_recreated);
            if (g_cfg.debugKeys) log("ghost %08x recreated (its body could not be placed)", recreate[i]);
            despawnGhost(recreate[i]);
        }
        flushHeals(now);
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
    {
        if (Building* exact = builds_findWorld(t.sid, t.x, t.y, t.z)) return exact;
        // Two worlds furnish the same room a little differently (a chair or a machine a few steps off): the nearest
        // building of the same kind around that spot will do (a ghost uses the table next to it, not the exact one).
        lektor<RootObject*> around; Ogre::Vector3 p(t.x, t.y, t.z);
        int n = safeAroundBuildings(&around, &p, 14.f);
        Building* best = NULL; float bestD = 1e30f;
        for (int i = 0; i < n; ++i)
        {
            Building* b = static_cast<Building*>(around[i]);
            if (!b || !b->data || b->data->stringID != t.sid) continue;
            float d = b->getPosition().distance(p);
            if (d < bestD) { bestD = d; best = b; }
        }
        return best;
    }
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

// Test: our character picks up the first knocked-out ghost of another player (what the Pick up order does).
std::string chars_debugCarryGhost(Character* me)
{
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        if (isNpcNetId(it->first)) continue;
        Character* c = it->second.h.getCharacter();
        if (!c || !(it->second.last.flags & EntityState::UNCONSCIOUS)) continue;
        Ogre::Vector3 at = c->getPosition();
        bool ok = safeOrder(me, LIFT_PERSON_SNATCHING_ALLOWED, c, &at);
        char b[160]; sprintf_s(b, "order Lift person on ghost %08x, %.0f units away -> %s", it->first, me->getPosition().distance(at), ok ? "given" : "failed");
        return b;
    }
    {
        std::string seen = "no knocked-out ghost of another player; ghosts and their flags: ";
        for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
            if (!isNpcNetId(it->first)) { char b[60]; sprintf_s(b, "%08x=%d ", it->first, (int)it->second.last.flags); seen += b; }
        return seen;
    }
}

// Test: what became of the ghost we picked up (engine state, and what its owner was told).
std::string chars_debugCarryState()
{
    char b[200]; std::string out;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        if (isNpcNetId(it->first)) continue;
        Character* c = it->second.h.getCharacter();
        if (!c) continue;
        sprintf_s(b, "ghost %08x: held=%d (engine state %d), carried=%d, at (%.0f,%.0f,%.0f); ", it->first, (int)it->second.held, safeHeldState(c), (int)c->isBeingCarried(), c->getPosition().x, c->getPosition().y, c->getPosition().z);
        out += b;
    }
    return out.empty() ? "no ghost" : out;
}

// Test: our first character is knocked out for `seconds` (another player may then carry it).
std::string chars_debugKnockOutSelf(Character* me, float seconds)
{
    if (!me) return "no character";
    if (!safeKnockOut(me, seconds)) return "failed";
    char b[100]; sprintf_s(b, "knocked out for %.0f s at (%.0f,%.0f,%.0f)", seconds, me->getPosition().x, me->getPosition().y, me->getPosition().z);
    return b;
}

// Test: the player orders our (caged) character to pick the lock.
std::string chars_debugPickLock(Character* me)
{
    Ogre::Vector3 at = me->getPosition();
    bool ok = safeOrder(me, PICK_LOCK, NULL, &at);
    return ok ? "order Pick lock given" : "order failed";
}

// Test: are our characters being held by another player, and where are they now.
std::string chars_debugHeldReport(Character* me)
{
    char b[220];
    sprintf_s(b, "%d character(s) held, %ld position message(s) received; ours is at (%.0f,%.0f,%.0f), knocked out %d", (int)g_held.size(), g_heldMessages,
              me ? me->getPosition().x : 0.f, me ? me->getPosition().y : 0.f, me ? me->getPosition().z : 0.f, me ? (int)safeUnconscious(me) : -1);
    return b;
}

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

void chars_npcGhostsNear(const std::vector<Ogre::Vector3>& centres, float radius, std::vector<uint32_t>& out)
{
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        if (!isNpcNetId(it->first)) continue;
        Character* c = it->second.h.getCharacter();
        if (!c) continue;
        Ogre::Vector3 p = c->getPosition();
        for (size_t i = 0; i < centres.size(); ++i)
            if (centres[i].distance(p) <= radius) { out.push_back(it->first); break; }
    }
}

void chars_traderPositions(std::vector<Ogre::Vector3>& out)
{
    for (std::map<uint32_t, Platoon*>::iterator it = g_traderSquads.begin(); it != g_traderSquads.end(); ++it)
    {
        std::map<uint32_t, Ghost>::iterator g = g_ghosts.find(it->first);
        if (g == g_ghosts.end()) continue;
        if (Character* c = g->second.h.getCharacter()) out.push_back(c->getPosition());
    }
}

void chars_playerGhosts(std::vector<std::pair<uint32_t, Character*> >& out)
{
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        if (isNpcNetId(it->first)) continue;
        Character* c = it->second.h.getCharacter();
        if (c) out.push_back(std::make_pair(it->first, c));
    }
}

// Where that player's characters are, as far as we know: the most recently reported one, with
// or without a body here (too far away to be spawned on our side).
bool chars_playerPosition(uint8_t owner, Ogre::Vector3& out)
{
    DWORD best = 0;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        if (netIdOwner(it->first) != owner || isNpcNetId(it->first) || !it->second.knownAt) continue;
        if (!best || (int)(it->second.knownAt - best) > 0) { best = it->second.knownAt; out = it->second.knownPos; }
    }
    return best != 0;
}

namespace
{
    bool safeRegroupable(Character* c, bool* fighting)
    {
        __try
        {
            *fighting = c->isInCombatMode(true, true);
            return !c->isDead() && !c->medical.isUnconcious();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

// Regroup (F4 "Go to" / "/goto"): our selected characters (all of them if none is selected) are
// moved next to that player's characters, in a ring around the spot. The other players are told
// first (MSG_REGROUP) so the host does not take the jump for a speed hack. Refused while one of
// them is fighting, for knocked-out / dead ones (they stay), and towards a player we are at war with.
// Returns the text to show the player.
std::string chars_regroupTo(uint8_t owner)
{
    if (!ready() || owner == g_session.localId()) return T("Not possible now.");
    if (diplomacy_relation(owner) < 0) return TF("You are at war with %s.", playerName(owner).c_str());
    Ogre::Vector3 target;
    if (!chars_playerPosition(owner, target)) return TF("Position of %s unknown for now (try again in a few seconds).", playerName(owner).c_str());

    std::vector<std::pair<uint32_t, Character*> > mine;
    chars_localCharacters(mine);
    std::set<Character*> selected;
    lektor<RootObject*> sel;
    ou->player->getAllSelectedObjects(sel, CHARACTER);
    for (uint32_t i = 0; i < sel.size(); ++i) { hand h(sel[i]); if (Character* c = h.getCharacter()) selected.insert(c); }
    std::vector<Character*> movers;
    for (size_t i = 0; i < mine.size(); ++i)
    {
        Character* c = mine[i].second;
        if (!c || (!selected.empty() && !selected.count(c))) continue;
        bool fighting = false;
        if (!safeRegroupable(c, &fighting)) continue;
        if (fighting) return T("One of your characters is fighting.");
        movers.push_back(c);
    }
    if (movers.empty()) return T("None of your characters can travel (selected, conscious, alive).");

    ByteWriter w; w.u8(owner);
    g_session.send(MSG_REGROUP, w.data);   // before the jump: same ordered connection
    float radius = 5.f + 0.4f * (float)movers.size();
    int moved = 0;
    for (size_t i = 0; i < movers.size(); ++i)
    {
        float a = 6.2831853f * (float)i / (float)movers.size();
        Ogre::Vector3 p(target.x + radius * cosf(a), target.y, target.z + radius * sinf(a));
        if (safeTeleport(movers[i], &p)) ++moved;
    }
    log("regroup: %d of our characters moved next to %s (%.0f,%.0f,%.0f)", moved, playerName(owner).c_str(), target.x, target.y, target.z);
    chat_notice(TF("You joined %s.", playerName(owner).c_str()));
    return TF("%d character(s) moved next to %s.", moved, playerName(owner).c_str());
}

// Someone regrouped: its characters jump, which is expected (host: no speed-hack warning).
void chars_onRegroup(uint8_t who, uint8_t target)
{
    g_moveGuard.forgetOwner(who);
    std::string to = target == g_session.localId() ? std::string(T("you")) : playerName(target);
    log("%s regrouped next to %s", playerName(who).c_str(), to.c_str());
    chat_notice(TF("%s joined %s.", playerName(who).c_str(), to.c_str()));
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

std::string chars_debugShoot(Character* me)
{
    Character* target = NULL;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end() && !target; ++it)
        if (!isNpcNetId(it->first)) target = it->second.h.getCharacter();
    if (!target) return "no ghost to aim at";
    GunClass* g = safeGunOf(me);
    if (!g)
    {
        int bow = -1, mode = -1, inRanged = -1, wants = -1;
        safeRangedInfo(me, &bow, &mode, &inRanged, &wants);
        char buf[160]; sprintf_s(buf, "no ranged weapon (getRangedWeapon=%d, ranged mode=%d, in ranged combat=%d, wants ranged=%d)", bow, mode, inRanged, wants);
        return buf;
    }
    Ogre::Vector3 aim = target->getPosition() + Ogre::Vector3(0, 15, 0);
    return safeShoot(g, me, target, 0, &aim) ? "shot fired at " + target->getName() : "the engine refused the shot";
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
    chars_clearPlayerNames();
    clearDrawOverrides();   // their animation objects died with the old world
    g_ghosts.clear();
    g_onMap.clear();   // the map was rebuilt with the world
    g_ghostSquads.clear();
    g_traderSquads.clear();
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

// Hand-off with acknowledgement (AUTHORITY_PLAN step C): a claim is repeated until the host answers
// (MSG_AUTH_HANDOFF), so a lost or refused claim cannot leave the NPC simulated twice for good.
struct AwaitGrant { int tries; DWORD last; };
static std::map<uint32_t, AwaitGrant> g_awaitGrant;
static const DWORD GRANT_RETRY_MS = 3000;
static const int GRANT_TRIES = 5;

static void sendClaim(uint32_t id)
{
    ByteWriter w; w.u32(id);
    g_session.sendTo(HOST_ID, MSG_NPC_CLAIM, w.data);
}

void chars_claimNpc(uint32_t id, Character* c)
{
    chars_releaseGhost(id);
    if (c) { Lock l; g_claimed[c] = hand(c); }
    sendClaim(id);
    AwaitGrant a; a.tries = 1; a.last = GetTickCount();
    g_awaitGrant[id] = a;
    log("world NPC %08x claimed (now simulated here)", id);
}

void chars_onHandoff(uint32_t id, uint8_t action, uint16_t epoch)
{
    std::map<uint32_t, AwaitGrant>::iterator it = g_awaitGrant.find(id);
    if (it == g_awaitGrant.end()) return;
    if (action == HANDOFF_REFUSE) { log("host refused our claim of %08x (try %d), will ask again", id, it->second.tries); return; }
    log("host %s the claim of %08x (authority epoch %d, %d try/tries)", action == HANDOFF_GRANT ? "granted" : "has nothing left of", id, (int)epoch, it->second.tries);
    g_awaitGrant.erase(it);
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
    for (std::map<uint32_t, AwaitGrant>::iterator a = g_awaitGrant.begin(); a != g_awaitGrant.end();)
    {
        if (now - a->second.last < GRANT_RETRY_MS) { ++a; continue; }
        if (a->second.tries >= GRANT_TRIES) { log("PROBLEM authority: no grant for the claim of %08x after %d tries, giving up", a->first, a->second.tries); g_awaitGrant.erase(a++); continue; }
        ++a->second.tries; a->second.last = now;
        sendClaim(a->first);
        ++a;
    }
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

// Test: the appearance data as five digests (strings, ints, floats, bools, lists), to see which part differs between an
// owner and its copy: "s12:1a2b3c4d i8:... f30:... b2:... l5:..." (count : hash).
// Test: every key of the appearance data, one log line per kind, for a few characters (owner vs copy comparison).
static void dumpChunks(uint32_t id, const char* role, const char* kind, const std::string& t)
{
    for (size_t pos = 0; pos < t.size() || pos == 0; pos += 360)
    {
        log("look-dump %08x %s %s: %s", id, role, kind, t.substr(pos, 360).c_str());
        if (t.size() <= 360) break;
    }
}
void cs_appearanceDump(Character* c, uint32_t id, const char* role)
{
    AppearanceBlob b = captureAppearance(c->getAppearanceData());
    std::string t;
    char tmp[200];
    for (size_t k = 0; k < b.s.size(); ++k) t += b.s[k].first + "=" + b.s[k].second + " | ";
    dumpChunks(id, role, "strings", t);
    t.clear();
    for (size_t k = 0; k < b.i.size(); ++k) { sprintf_s(tmp, "%s=%d | ", b.i[k].first.c_str(), (int)b.i[k].second); t += tmp; }
    dumpChunks(id, role, "ints", t);
    t.clear();
    for (size_t k = 0; k < b.f.size(); ++k) { sprintf_s(tmp, "%s=%.3f | ", b.f[k].first.c_str(), b.f[k].second); t += tmp; }
    dumpChunks(id, role, "floats", t);
    t.clear();
    for (size_t k = 0; k < b.b.size(); ++k) { sprintf_s(tmp, "%s=%d | ", b.b[k].first.c_str(), (int)b.b[k].second); t += tmp; }
    dumpChunks(id, role, "bools", t);
    t.clear();
    for (size_t k = 0; k < b.lists.size(); ++k)
    {
        t += b.lists[k].first + "=[";
        for (size_t n = 0; n < b.lists[k].second.size(); ++n) { sprintf_s(tmp, "%s(%d,%d,%d) ", b.lists[k].second[n].sid.c_str(), (int)b.lists[k].second[n].v0, (int)b.lists[k].second[n].v1, (int)b.lists[k].second[n].v2); t += tmp; }
        t += "] | ";
    }
    dumpChunks(id, role, "lists", t);
}

std::string cs_appearanceDigest(Character* c)
{
    AppearanceBlob b = captureAppearance(c->getAppearanceData());
    char out[160];
    AppearanceBlob p;
    ByteWriter ws, wi, wf, wb, wl;
    p = AppearanceBlob(); p.s = b.s; p.write(ws);
    p = AppearanceBlob(); p.i = b.i; p.write(wi);
    p = AppearanceBlob(); p.f = b.f; p.write(wf);
    p = AppearanceBlob(); p.b = b.b; p.write(wb);
    p = AppearanceBlob(); p.lists = b.lists; p.write(wl);
    sprintf_s(out, "s%d:%08x i%d:%08x f%d:%08x b%d:%08x l%d:%08x", (int)b.s.size(), hashBytes(ws.data), (int)b.i.size(), hashBytes(wi.data), (int)b.f.size(), hashBytes(wf.data),
              (int)b.b.size(), hashBytes(wb.data), (int)b.lists.size(), hashBytes(wl.data));
    return out;
}

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

bool cs_stashNpc(Character* c, StashedNpc& out)
{
    if (!c || !c->data) return false;
    Faction* f = c->getFaction();
    if (!f || !f->getData()) return false;
    out.tmpl = c->data->stringID;
    out.faction = f->getData()->stringID;
    out.name = c->getName();
    out.pos = c->getPosition();
    out.look = captureAppearance(c->getAppearanceData());
    out.hasGear = c->inventory != NULL;
    if (out.hasGear) out.gear = captureEquipment(c);
    out.at = GetTickCount();
    return !out.tmpl.empty() && !out.faction.empty();
}

namespace
{
    std::map<Faction*, Platoon*> g_restoreSquads;   // one squad per faction for the NPCs we bring back
}

bool cs_restoreNpc(const StashedNpc& s)
{
    Faction* f = ou->factionMgr->getFactionByStringID(s.faction);
    GameData* tmpl = dataOf(s.tmpl);
    if (!f || !tmpl || f->isThePlayer() || f == ou->player->getFaction()) return false;
    ActivePlatoon* squad = NULL;
    std::map<Faction*, Platoon*>::iterator it = g_restoreSquads.find(f);
    if (it != g_restoreSquads.end()) squad = safeActive(it->second);
    if (!squad)
    {
        GameData* squadTemplate = ou->player->currentPlatoon ? ou->player->currentPlatoon->data : NULL;
        Platoon* p = safeNewPlatoon(f, squadTemplate, &s.pos);
        if (!p) return false;
        g_restoreSquads[f] = p;
        squad = safeActive(p);
    }
    if (!squad) return false;
    Character* c = static_cast<Character*>(safeSpawn(f, &s.pos, tmpl, squad));
    if (!c) return false;
    if (!s.name.empty()) c->setName(s.name);
    if (!s.look.s.empty() || !s.look.i.empty() || !s.look.f.empty() || !s.look.b.empty() || !s.look.lists.empty())
        if (GameData* d = c->getAppearanceData()) { applyAppearance(d, s.look); safeRefreshAppearance(c); }
    if (s.hasGear)
    {
        Ghost dressing;   // only its list of given items is used
        applyEquipment(dressing, c, s.gear);
        static int logged = 0;
        if (logged < 5) { ++logged; log("restored NPC '%s': %d worn/held item(s) put back", s.name.c_str(), (int)s.gear.size()); }
    }
    return true;
}

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

// Other players' characters always show their name above their head. Hooking the game's own name
// tag (CharacterNameTag) did not keep it visible, so the names are drawn by us: one MyGUI text per
// other player's character, placed each frame where its head projects on the screen through the
// game camera (Ogre), in the colour of its player (same as the chat). Hidden behind the camera, off
// screen and far away.
namespace
{
    const float NAME_HEAD_HEIGHT = 16.f;     // above the character's position: a human is ~14 world units tall (measured on the game's frames: 2.3 put the label at the feet)
    const int NAME_W = 320, NAME_H = 26;

    struct NameLabel { MyGUI::EditBox* w; std::string text; bool shown; NameLabel() : w(NULL), shown(false) {} };
    long g_nameFrames = 0, g_nameDrawn = 0, g_nameNoCam = 0, g_nameOff = 0, g_nameBehind = 0, g_nameFar = 0; int g_nameLastX = -1, g_nameLastY = -1; float g_nameCam[3] = {0,0,0}, g_nameAt[3] = {0,0,0}, g_nameV[3] = {0,0,0}, g_nameFeet[3] = {0,0,0}, g_nameP[2] = {0,0}, g_nameTag = 0; int g_nameViewW = 0, g_nameViewH = 0; int g_nameFeetPx[2] = {0,0}, g_nameAnchorPx[2] = {0,0}, g_nameBoxPx[2] = {0,0};
    std::map<uint32_t, NameLabel> g_names;
    bool g_namesFailed = false;

    // The game camera, read from the player interface (PlayerInterface::camera -> CameraClass::camera).
    Ogre::Camera* safeGameCamera()
    {
        __try
        {
            if (!ou || !ou->player || !ou->player->camera) return NULL;
            return ou->player->camera->camera;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    bool writeShotInner(const char* file)
    {
        Ogre::RenderSystem* rs = Ogre::Root::getSingleton().getRenderSystem();
        if (!rs) return false;
        Ogre::RenderSystem::RenderTargetIterator it = rs->getRenderTargetIterator();
        while (it.hasMoreElements())
        {
            Ogre::RenderTarget* t = it.getNext();
            if (t && t->isPrimary()) { t->writeContentsToFile(file); return true; }
        }
        return false;
    }
    bool safeWriteShot(const char* file)
    {
        __try { return writeShotInner(file); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeCameraMatrices(Ogre::Camera* cam, Ogre::Matrix4* view, Ogre::Matrix4* proj, Ogre::Vector3* at)
    {
        __try { *view = cam->getViewMatrix(); *proj = cam->getProjectionMatrix(); *at = cam->getDerivedPosition(); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Diagnostic: the camera's position by every route the engine offers (to find the one that is in world coordinates).
    bool safeCamDiag(float* out)
    {
        __try
        {
            CameraClass* cc = ou->player->camera;
            Ogre::Vector3 a = cc->camera->getPosition(), b = cc->camera->getDerivedPosition(), c = cc->getCameraPos();
            out[0] = a.x; out[1] = a.y; out[2] = a.z; out[3] = b.x; out[4] = b.y; out[5] = b.z; out[6] = c.x; out[7] = c.y; out[8] = c.z;
            Ogre::SceneNode* n = cc->getCameraNode();
            if (n) { Ogre::Vector3 d = n->_getDerivedPosition(); out[9] = d.x; out[10] = d.y; out[11] = d.z; } else out[9] = out[10] = out[11] = -1;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // The engine renders in a re-based space (its camera sits near the origin, measured: derived position (7,1617,-126)
    // while the camera is at (-50972,1617,2807) in world coordinates): the offset between both is what turns a world
    // position into the position the view matrix understands.
    bool safeWorldCamPos(Ogre::Vector3* out)
    {
        __try { *out = ou->player->camera->getCameraPos(); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Height of the game's own name tag above the character (ScreenLabel::trackingOffset), when sane.
    float rawTagHeight(Character* c, bool* have)
    {
        *have = false;
        __try
        {
            ScreenLabel* tag = (ScreenLabel*)c->nameTag;
            if (tag) { float y = tag->trackingOffset.y; if (y > 0.05f && y < 200.f) { *have = true; return y; } }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0.f;
    }
    float g_ownTagHeight = 0;        // the game's own tag height on one of our characters (same world units as the positions)
    float safeTagHeight(Character* c)
    {
        bool have;
        float y = rawTagHeight(c, &have);
        if (have) return y;
        if (g_ownTagHeight > 0.05f) return g_ownTagHeight;   // a copy has no tag of its own: use what the game gives our characters
        for (size_t i = 0; i < g_local.size(); ++i)
            if (Character* lc = g_local[i].h.getCharacter())
            {
                float t = rawTagHeight(lc, &have);
                if (have) { g_ownTagHeight = t; log("player names: the game's own name tag sits %.2f units above a character", t); return t; }
            }
        return NAME_HEAD_HEIGHT;
    }

    MyGUI::EditBox* createName()
    {
        try
        {
            MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
            if (!gui) return NULL;
            MyGUI::EditBox* w = gui->createWidget<MyGUI::EditBox>("Kenshi_WordWrapEmpty",
                MyGUI::IntCoord(0, 0, NAME_W, NAME_H), MyGUI::Align::Default, "Main");
            if (!w) return NULL;
            // Set up like the chat history (which draws fine): multi-line + word wrap, a visible base colour.
            w->setEditMultiLine(true);
            w->setEditWordWrap(true);
            w->setEditReadOnly(true);
            w->setTextAlign(MyGUI::Align::Center | MyGUI::Align::Bottom);
            w->setTextShadow(true);
            w->setTextColour(MyGUI::Colour(1.f, 1.f, 1.f));
            w->setAlpha(1.f);
            w->setNeedMouseFocus(false);       // never steals clicks from the game
            w->setNeedKeyFocus(false);
            w->setVisible(false);
            return w;
        }
        catch (...) { return NULL; }
    }
    void destroyName(NameLabel& l)
    {
        try { if (l.w) MyGUI::Gui::getInstance().destroyWidget(l.w); } catch (...) {}
        l.w = NULL;
    }
    void hideName(NameLabel& l)
    {
        if (!l.shown || !l.w) return;
        l.shown = false;
        try { l.w->setVisible(false); } catch (...) {}
    }
}

// Test: the NPCs around our first character, as this game sees them: name, replicated copy or real, trader, has dialogue
// (compare the lines of the two games: the host's real NPCs against the client's copies).
namespace
{
    bool isOurSquad(Character* c) { for (size_t i = 0; i < g_local.size(); ++i) if (g_local[i].h.getCharacter() == c) return true; return false; }
    bool safeNpcProbe(Character* c, int* trader, int* dialogue, int* indoors, int* items)
    {
        __try
        {
            *trader = c->isATrader() ? 1 : 0;
            *dialogue = (c->dialogue && c->dialogue->hasDialogue_Accurate()) ? 1 : 0;
            *indoors = c->isIndoors().getBuilding() ? 1 : 0;
            *items = c->inventory ? (int)c->inventory->getNumItems() : -1;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}
// Step A of AUTHORITY_PLAN.md: one line per entity we know, so the two games' logs can be compared offline
// (tools: tests/harness/authority_diff.ps1). Same PC = same GetTickCount clock in both logs.
namespace
{
    bool authProbe(Character* c, float* x, float* y, float* z, float* hp, float* blood, int* flags)
    {
        __try
        {
            Ogre::Vector3 p = c->getPosition();
            *x = p.x; *y = p.y; *z = p.z;
            *hp = c->medical.getOverallHealthRating();
            *blood = c->medical.blood;
            *flags = (c->isDead() ? 1 : 0) | (c->medical.isUnconcious() ? 2 : 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}
// Test (AUTHORITY_PLAN step C): claim the nearest host NPC copy within 45 units, the way recruiting / carrying does.
std::string chars_debugClaimNpc(Character* me)
{
    if (!me) return "no character";
    Character* best = NULL; uint32_t bid = 0; float bd = 45.f;
    for (std::map<uint32_t, Ghost>::iterator g = g_ghosts.begin(); g != g_ghosts.end(); ++g)
    {
        if (!isNpcNetId(g->first)) continue;
        Character* c = g->second.h.getCharacter();
        if (!c) continue;
        float d = c->getPosition().distance(me->getPosition());
        if (d < bd) { bd = d; best = c; bid = g->first; }
    }
    if (!best) return "no host NPC within 45 units";
    chars_claimWhenNear(bid, best);
    char b[96]; sprintf_s(b, "claiming %08x (%.0f units away)", bid, bd);
    return b;
}

void chars_debugAuthority()
{
    DWORD now = GetTickCount();
    { static DWORD lastBl = 0;
      if (now - lastBl > 10000)
      {
          lastBl = now;
          int bodiless = 0, withBody = 0; std::string sample;
          for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
          {
              if (it->second.h.getCharacter()) { ++withBody; continue; }
              ++bodiless;
              if (bodiless <= 5)
              {
                  float nl = 1e30f;
                  for (size_t i = 0; i < g_local.size(); ++i) if (Character* lc = g_local[i].h.getCharacter()) { float d = lc->getPosition().distance(it->second.knownPos); if (d < nl) nl = d; }
                  char b[80]; sprintf_s(b, "%08x(%.0f u, %d tries) ", it->first, nl < 1e29f ? nl : -1.f, it->second.bodyTries); sample += b;
              }
          }
          log("copies: %d with a body, %d without (%s)", withBody, bodiless, sample.c_str());
      } }
    { static DWORD lastTm = 0; if (now - lastTm > 10000) { lastTm = now; log("task mirroring: %ld too far from the owner's spot (>%.0f for players), %ld owner still moving, %ld wanted, %ld subject not found, %ld order refused", g_tmFar, TASK_RANGE, g_tmMoving, g_tmWanted, g_tmNoSubject, g_tmOrderFail); } }
    for (size_t i = 0; i < g_local.size(); ++i)
    {
        Character* c = g_local[i].h.getCharacter();
        float x, y, z, hp, bl; int fl;
        if (c && authProbe(c, &x, &y, &z, &hp, &bl, &fl))
            {
            { static std::set<uint32_t> dumped; if (dumped.size() < 2 && dumped.insert(g_local[i].id).second) cs_appearanceDump(c, g_local[i].id, "own"); }
            log("auth: t=%u id=%08x role=own owner=%d x=%.1f y=%.1f z=%.1f hp=%.1f blood=%.1f fl=%d tmpl=%s app=%08x look=%s", (unsigned)now, g_local[i].id, (int)auth_owner(g_local[i].id), x, y, z, hp, bl, fl, c->data ? c->data->stringID.c_str() : "-", hashBytes(cs_appearanceMsg(c, g_local[i].id)), cs_appearanceDigest(c).c_str());
            if (ready() && auth_owner(g_local[i].id) != g_session.localId()) log("PROBLEM authority: we simulate %08x but the table says player %d owns it", g_local[i].id, (int)auth_owner(g_local[i].id));
        }
    }
    for (std::map<uint32_t, Ghost>::iterator g = g_ghosts.begin(); g != g_ghosts.end(); ++g)
    {
        Character* c = g->second.h.getCharacter();
        float x, y, z, hp, bl; int fl;
        if (c && authProbe(c, &x, &y, &z, &hp, &bl, &fl))
        {
            { static std::set<uint32_t> dumped; if (dumped.size() < 6 && (g->first == 0x00800008 || g->first == 0x00800011 || g->first == 0x02000001 || g->first == 0x00000001) && dumped.insert(g->first).second) cs_appearanceDump(c, g->first, "copy"); }
            log("auth: t=%u id=%08x role=copy owner=%d x=%.1f y=%.1f z=%.1f hp=%.1f blood=%.1f fl=%d tmpl=%s app=%08x look=%s task=%d", (unsigned)now, g->first, (int)auth_owner(g->first), x, y, z, hp, bl, fl, c->data ? c->data->stringID.c_str() : "-", hashBytes(cs_appearanceMsg(c, g->first)), cs_appearanceDigest(c).c_str(), g->second.mirroredTask);
        }
    }
}

void chars_debugNpcCensus()
{
    std::vector<Ogre::Vector3> mine; cs_localPositions(mine);
    if (mine.empty()) return;
    // around ours AND around the copies of the other players' characters (the host looks where its client stands)
    for (std::map<uint32_t, Ghost>::iterator g = g_ghosts.begin(); g != g_ghosts.end(); ++g)
        if (!isNpcNetId(g->first)) if (Character* gc = g->second.h.getCharacter()) mine.push_back(gc->getPosition());
    std::vector<Character*> all; cs_allActiveCharacters(all);
    if (g_session.isHost()) npcs_realAround(mine, 250.f, all);   // the NPCs the host serves to its clients
    int shown = 0;
    for (size_t i = 0; i < all.size() && shown < 24; ++i)
    {
        Character* c = all[i];
        if (!c) continue;
        float nearest = 1e30f;
        for (size_t k = 0; k < mine.size(); ++k) { float d = c->getPosition().distance(mine[k]); if (d < nearest) nearest = d; }
        if (nearest > 250.f) continue;
        uint32_t id = ghostIdOf(c);
        if (!id && isOurSquad(c)) continue;     // one of ours
        int trader = 0, dialogue = 0, indoors = 0, items = 0;
        if (!safeNpcProbe(c, &trader, &dialogue, &indoors, &items)) continue;
        ++shown;
        log("npc census: '%s' %s trader=%d dialogue=%d indoors=%d items=%d money=%d at %.0f units from a player", c->getName().c_str(), id ? (isNpcNetId(id) ? "host-NPC-copy" : "player-copy") : "real", trader, dialogue, indoors, items, items_moneyOf(c), nearest);
    }
}

// Test: what the trade window would show for the traders around us: the game's own ShopTrader of each one (the shop
// inventory built from the shelves, and the money). Host (real traders) and client (copies) must show the same.
namespace
{
    bool shopViewInner(Character* t, int* items, int* money)
    {
        ShopTrader* st = new ShopTrader(t);
        Inventory* inv = st->getInventory();
        *items = inv ? (int)inv->getNumItems() : -1;
        *money = st->getMoney();
        delete st;
        return true;
    }
    bool safeShopView(Character* t, int* items, int* money)
    {
        __try { return shopViewInner(t, items, money); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}
// Test: who the shop counters around a trader belong to (UseableStuff::shopOwner): the real trader owns them, its copy should too.
namespace
{
    // For each counter: whom does its shopOwner resolve to (a character, a platoon), and is that our trader or its squad?
    struct CounterInfo { int isShop, hasUse, ownerSet, ownerCharIsMe, ownerPlatoonIsMine, ownerCharInMySquad, ownerIsBuilding, ownerIsTown, ownerIsRoot; char ownerName[48]; char sid[40]; char ownerStr[120]; char traderStr[120]; char bfac[48]; char tfac[48]; char townStr[60]; int items; int hasMgr; };
    void counterInner(Building* b, Character* c, CounterInfo* ci)
    {
        ci->isShop = b->getSpecialFunction() == BF_SHOP ? 1 : 0;
        UseableStuff* us = b->getUseableStuff();
        if (!us) return;
        ci->hasUse = 1;
        if (ci->isShop)
        {
            Faction* bf = b->getFaction(); Faction* tf = c->getFaction();
            if (bf) strncpy_s(ci->bfac, bf->getName().c_str(), _TRUNCATE);
            if (tf) strncpy_s(ci->tfac, tf->getName().c_str(), _TRUNCATE);
            strncpy_s(ci->townStr, b->getTownHandle().toString().c_str(), _TRUNCATE);
            Inventory* inv = us->getInventory();
            ci->items = inv ? (int)inv->getAllItems().size() : -1;
            ci->hasMgr = b->buildingsManager ? 1 : 0;
        }
        hand none;
        if (us->shopOwner == none) { if (ci->isShop) strncpy_s(ci->ownerStr, "(null)", _TRUNCATE); return; }
        ci->ownerSet = 1;
        Character* oc = us->shopOwner.getCharacter();
        ActivePlatoon* op = us->shopOwner.getActivePlatoon();
        ActivePlatoon* mine = c->getPlatoon();
        strncpy_s(ci->ownerStr, us->shopOwner.toString().c_str(), _TRUNCATE);
        { hand me(c); strncpy_s(ci->traderStr, me.toString().c_str(), _TRUNCATE); }
        if (us->shopOwner.getBuilding()) ci->ownerIsBuilding = 1;
        if (us->shopOwner.getTown()) ci->ownerIsTown = 1;
        if (us->shopOwner.getRootObject()) ci->ownerIsRoot = 1;
        if (oc == c) ci->ownerCharIsMe = 1;
        if (op && op == mine) ci->ownerPlatoonIsMine = 1;
        if (oc && oc->getPlatoon() == mine) ci->ownerCharInMySquad = 1;
        if (oc) strncpy_s(ci->ownerName, oc->getName().c_str(), _TRUNCATE);
        else if (op) strncpy_s(ci->ownerName, "(a platoon)", _TRUNCATE);
        if (b->data) strncpy_s(ci->sid, b->data->name.c_str(), _TRUNCATE);
    }
    bool safeCounter(Building* b, Character* c, CounterInfo* ci)
    {
        __try { counterInner(b, c, ci); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}
namespace
{
    // What the trader's squad owns (the game links a trader to its shop counters through this).
    struct OwnInfo { int stuff, homeTown, shopsFn, ownedPtrs, counterOwned, counterTotal, ok; char home[80]; };
    void ownInner(Character* c, lektor<RootObject*>* around, OwnInfo* oi)
    {
        Ownerships* o = c->getOwnerships();
        if (!o) return;
        oi->ok = 1;
        oi->stuff = (int)o->stuff.size();
        oi->homeTown = o->_homeTown ? 1 : 0;
        strncpy_s(oi->home, o->_homeBuilding.toString().c_str(), _TRUNCATE);
        lektor<Building*> a; o->getBuildingsWithFunction(a, BF_SHOP); oi->shopsFn = (int)a.size();
        lektor<Building*> b; o->getOwnedBuildingPtrs(b, o->_homeTown); oi->ownedPtrs = (int)b.size();
        for (uint32_t i = 0; i < around->size(); ++i)
        {
            Building* bl = static_cast<Building*>((*around)[i]);
            if (bl->getSpecialFunction() != BF_SHOP) continue;
            ++oi->counterTotal;
            hand hb(bl);
            if (o->isOwned(hb)) ++oi->counterOwned;
        }
    }
    bool safeOwn(Character* c, lektor<RootObject*>* around, OwnInfo* oi)
    {
        __try { ownInner(c, around, oi); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}
static void logCounterOwners(Character* c)
{
    {
        Ogre::Vector3 at0 = c->getPosition();
        lektor<RootObject*> around0;
        OwnInfo oi; memset(&oi, 0, sizeof(oi));
        if (safeShelfQuery(&around0, &at0, 120.f) && safeOwn(c, &around0, &oi))
            log("  squad of '%s' (%s) owns: %d object(s), home town %d, home building [%s], %d shop building(s) by function, %d owned building(s); shop counters around: %d, of which owned by the squad %d",
                c->getName().c_str(), ghostIdOf(c) ? "copy" : "real", oi.stuff, oi.homeTown, oi.home, oi.shopsFn, oi.ownedPtrs, oi.counterTotal, oi.counterOwned);
        else log("  squad of '%s': ownership could not be read", c->getName().c_str());
    }
    Ogre::Vector3 at = c->getPosition();
    lektor<RootObject*> around;
    if (!safeShelfQuery(&around, &at, 120.f)) return;
    int shops = 0, useable = 0, owned = 0, mine = 0, platoon = 0, squad = 0, n = 0, listed = 0;
    for (uint32_t i = 0; i < around.size(); ++i)
    {
        CounterInfo ci; memset(&ci, 0, sizeof(ci));
        if (!safeCounter(static_cast<Building*>(around[i]), c, &ci)) continue;
        ++n; shops += ci.isShop; useable += ci.hasUse; owned += ci.ownerSet; mine += ci.ownerCharIsMe; platoon += ci.ownerPlatoonIsMine; squad += ci.ownerCharInMySquad;
        if (ci.isShop && listed < 2) { ++listed; log("  shop '%s' of '%s' (%s): shopOwner=[%s] trader=[%s] building %d town %d root %d, is the trader %d, its platoon %d, same squad %d; bld faction [%s], trader faction [%s], town [%s], counter items %d, mgr %d", ci.sid, c->getName().c_str(), ghostIdOf(c) ? "copy" : "real", ci.ownerStr, ci.traderStr, ci.ownerIsBuilding, ci.ownerIsTown, ci.ownerIsRoot, ci.ownerCharIsMe, ci.ownerPlatoonIsMine, ci.ownerCharInMySquad, ci.bfac, ci.tfac, ci.townStr, ci.items, ci.hasMgr); }
    }
    log("shop counters around '%s' (%s): %d building(s), %d shop(s), %d with UseableStuff, %d with an owner, %d owned by this trader, %d by its platoon, %d by its squad's members",
        c->getName().c_str(), ghostIdOf(c) ? "copy" : "real", n, shops, useable, owned, mine, platoon, squad);
}

void chars_debugShopView()
{
    std::vector<Ogre::Vector3> mine; cs_localPositions(mine);
    if (mine.empty()) return;
    for (std::map<uint32_t, Ghost>::iterator g = g_ghosts.begin(); g != g_ghosts.end(); ++g)
        if (!isNpcNetId(g->first)) if (Character* gc = g->second.h.getCharacter()) mine.push_back(gc->getPosition());
    std::vector<Character*> all; cs_allActiveCharacters(all);
    if (g_session.isHost()) npcs_realAround(mine, 250.f, all);
    std::set<Character*> seen; int shown = 0;
    for (size_t i = 0; i < all.size() && shown < 8; ++i)
    {
        Character* c = all[i];
        if (!c || seen.count(c)) continue; seen.insert(c);
        uint32_t id = ghostIdOf(c);
        if ((id && !isNpcNetId(id)) || (!id && isOurSquad(c))) continue;
        int tr = 0, dl = 0, ind = 0, it = 0;
        if (!safeNpcProbe(c, &tr, &dl, &ind, &it) || !tr) continue;
        float nearest = 1e30f;
        for (size_t k = 0; k < mine.size(); ++k) { float d = c->getPosition().distance(mine[k]); if (d < nearest) nearest = d; }
        if (nearest > 250.f) continue;
        int items = -2, money = -2;
        bool ok = safeShopView(c, &items, &money);
        ++shown;
        logCounterOwners(c);
        log("shop view: '%s' %s: trade window would show %d item(s), the trader has %d cats (%s)", c->getName().c_str(), id ? "copy" : "real", items, money, ok ? "ok" : "ENGINE ERROR");
    }
}

// Test: our first character is ordered to talk to the nearest NPC that has dialogue (what a right-click > Talk does), then
// what became of the conversation. Run on the host (real NPC) and on the client (copy of the same NPC) to compare.
namespace
{
    Character* g_talkTarget = NULL;
    hand g_talkHand;
    bool safeTalkState(Character* t, int* has, int* running, int* ended)
    {
        __try
        {
            Dialogue* d = t->dialogue;
            if (!d) { *has = 0; return true; }
            *has = 1;
            *running = d->currentConversation != NULL ? 1 : 0;
            *ended = d->_hasEnded ? 1 : 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}
std::string chars_debugTalk(Character* me)
{
    std::vector<Character*> all; cs_allActiveCharacters(all);
    if (g_session.isHost()) { std::vector<Ogre::Vector3> c1(1, me->getPosition()); npcs_realAround(c1, 250.f, all); }
    Character* best = NULL; float bd = 1e30f;
    for (size_t i = 0; i < all.size(); ++i)
    {
        Character* c = all[i];
        if (!c || c == me) continue;
        uint32_t id = ghostIdOf(c);
        if ((id && !isNpcNetId(id)) || (!id && isOurSquad(c))) continue;   // another player's copy / one of ours
        int tr = 0, dl = 0, ind = 0, it = 0;
        if (!safeNpcProbe(c, &tr, &dl, &ind, &it) || !dl) continue;
        float d = c->getPosition().distance(me->getPosition());
        if (d < bd && d < 250.f) { bd = d; best = c; }
    }
    if (!best) return "no NPC with dialogue within 250 units";
    g_talkTarget = best; g_talkHand = hand(best);
    Ogre::Vector3 at = best->getPosition();
    bool ok = safeOrder(me, PLAYER_TALK_TO, best, &at);
    char b[200]; sprintf_s(b, "order Talk to '%s' (%s) %.0f units away -> %s", best->getName().c_str(), ghostIdOf(best) ? "copy" : "real", bd, ok ? "given" : "failed");
    return b;
}
std::string chars_debugTalkState()
{
    Character* t = g_talkHand.getCharacter();
    if (!t || t != g_talkTarget) return "no target";
    int has = 0, running = 0, ended = 0;
    if (!safeTalkState(t, &has, &running, &ended)) return "unreadable";
    float dist = -1.f, speed = -1.f;
    { std::vector<Ogre::Vector3> m; cs_localPositions(m); if (!m.empty()) dist = t->getPosition().distance(m[0]); }
    char b[220]; sprintf_s(b, "'%s' (%s): dialogue object %d, conversation running %d, ended %d, our character %.1f units away", t->getName().c_str(), ghostIdOf(t) ? "copy" : "real", has, running, ended, dist);
    (void)speed;
    return b;
}

// Test (two real games): our character is wounded (one body part set to `flesh`); the other game heals the copy it sees.
namespace
{
    bool safeWound(Character* c, int part, float flesh, float* before)
    {
        __try
        {
            MedicalSystem& m = c->medical;
            if (part >= (int)m.anatomy.size()) return false;
            *before = m.anatomy[part]->flesh;
            m.anatomy[part]->flesh = flesh; m.anatomy[part]->updateDerivedHealths();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeFlesh(Character* c, int part, float* out, float* band = NULL)
    {
        __try { if (part >= (int)c->medical.anatomy.size()) return false; *out = c->medical.anatomy[part]->flesh; if (band) *band = c->medical.anatomy[part]->bandaging; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool safeAddItem(Inventory* inv, Item* it)
    {
        __try { return inv->addItem(it, 1, false, true); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    Item* safeKit(Character* who)
    {
        __try { return who->inventory ? who->inventory->getBestItemWithFunction(ITEM_FIRSTAID) : NULL; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    bool safeFirstAid(Character* patient, Character* who, Item* kit, float skill, float dt)
    {
        __try { return patient->medical.applyFirstAid(skill, kit, dt, who); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}
std::string chars_debugWoundSelf(Character* me)
{
    // a REAL wound (through the damage code, so the engine knows the character needs first aid), not just a number
    if (!me || me->medical.anatomy.size() <= 2) return "failed";
    float before = me->medical.anatomy[2]->flesh;
    Damages dmg(45.f, 0.f, 0.f, 1.0f, 0.0f);
    applyRealDamage(me, me->medical.anatomy[2], dmg);
    char b[110]; sprintf_s(b, "body part 2 of our character: flesh %.1f -> %.1f (a 45 cut)", before, me->medical.anatomy[2]->flesh);
    return b;
}
std::string chars_debugHealGhost(Character* me)
{
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        if (isNpcNetId(it->first)) continue;
        Character* c = it->second.h.getCharacter();
        if (!c) continue;
        // the medic needs a first aid kit of its own: copy one from any character of the world if it has none
        Item* kit = safeKit(me);
        bool gave = false;
        if (!kit)
        {
            std::vector<Character*> everyone; cs_allActiveCharacters(everyone);
            Item* donor = NULL;
            for (size_t k = 0; k < everyone.size() && !donor; ++k) if (everyone[k] && everyone[k] != c && everyone[k] != me) donor = safeKit(everyone[k]);
            if (donor && donor->data)
            {
                bool crashed = false; hand owner(me);
                Item* made = safeCreateItem(donor->data, NULL, NULL, &owner, -1, &crashed);
                if (made && me->inventory && safeAddItem(me->inventory, made)) { kit = safeKit(me); gave = kit != NULL; }
            }
        }
        // what a right-click > First aid does: the order; the engine walks the medic there and treats (the hooks see it)
        Ogre::Vector3 at = c->getPosition();
        bool ok = safeOrder(me, FIRST_AID_ORDER, c, &at);
        char b[220]; sprintf_s(b, "order First aid on the copy %08x (kit %s%s), %.0f units away -> %s", it->first, kit ? "found" : "NONE", gave ? ", given" : "", me->getPosition().distance(at), ok ? "given" : "failed");
        return b;
    }
    return "no player copy";
}
// Control: the same first aid call on our OWN real wounded character (does the engine function work in this setup at all?).
std::string chars_debugHealSelf(Character* me)
{
    Item* kit = safeKit(me);
    if (!kit)
    {
        std::vector<Character*> everyone; cs_allActiveCharacters(everyone);
        for (size_t k = 0; k < everyone.size() && !kit; ++k) if (everyone[k] && everyone[k] != me) kit = safeKit(everyone[k]);
    }
    float f0 = 0, b0 = 0, f1 = 0, b1 = 0;
    safeFlesh(me, 2, &f0, &b0);
    int did = 0;
    for (int i = 0; i < 40; ++i) if (safeFirstAid(me, me, kit, 70.f, 0.5f)) ++did;
    safeFlesh(me, 2, &f1, &b1);
    char b[220]; sprintf_s(b, "control, on our own character: engine did something %d/40 (kit %s), part 2 flesh %.1f -> %.1f, bandaging %.2f -> %.2f", did, kit ? "found" : "NONE", f0, f1, b0, b1);
    return b;
}
std::string chars_debugGhostMedical()
{
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        if (isNpcNetId(it->first)) continue;
        Character* c = it->second.h.getCharacter();
        if (!c) continue;
        std::string out; char b[80];
        MedicalSystem& m = c->medical;
        for (uint32_t i = 0; i < m.anatomy.size() && i < 8; ++i) { sprintf_s(b, "[%u] f%.1f b%.2f s%.2f  ", i, m.anatomy[i]->flesh, m.anatomy[i]->bandaging, m.anatomy[i]->fleshStun); out += b; }
        sprintf_s(b, "blood %.1f", m.blood); out += b;
        return out;
    }
    return "no player copy";
}
// The world clock was moved by `hours` (a client snapping to the host's time, often days ahead when it joins a game
// further on). The engine drains hunger by the time since MedicalSystem::lastPeriodicUpdate: without this, a 10-day jump
// starved our characters to death in the next frame (measured: hunger 3 -> -1, dead within a second). The medical time
// stamps move with the clock, so no time has passed for anyone's body.
namespace
{
    bool safeShiftMedical(Character* c, double hours)
    {
        __try
        {
            MedicalSystem& m = c->medical;
            if (m.lastPeriodicUpdate.time > 0) m.lastPeriodicUpdate.time += hours;
            if (m.lastHungerKO.time > 0) m.lastHungerKO.time += hours;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}
int chars_shiftBodyClocks(double hours)
{
    std::vector<Character*> all;
    cs_allActiveCharacters(all);
    for (size_t i = 0; i < g_local.size(); ++i)   // ours, even if the engine does not list them as active right now
        if (Character* c = g_local[i].h.getCharacter()) if (std::find(all.begin(), all.end(), c) == all.end()) all.push_back(c);
    int n = 0;
    for (size_t i = 0; i < all.size(); ++i) if (all[i] && safeShiftMedical(all[i], hours)) ++n;
    return n;
}

std::string chars_debugVitals()
{
    std::string out; char b[120];
    for (size_t i = 0; i < g_local.size() && i < 6; ++i)
        if (Character* c = g_local[i].h.getCharacter())
        {
            sprintf_s(b, "%s hunger %.1f blood %.1f%s%s; ", chat_clean(c->getName()).c_str(), c->medical.hunger, c->medical.blood,
                      safeDead(c) ? " DEAD" : "", c->medical.isUnconcious() ? " KO" : "");
            out += b;
        }
    return out.empty() ? "no character" : out;
}
std::string chars_debugFleshSelf(Character* me)
{
    float f = -1, bd = -1; safeFlesh(me, 2, &f, &bd);
    char b[100]; sprintf_s(b, "our character's part 2: flesh %.1f, bandaging %.2f", f, bd);
    return b;
}

namespace
{
    bool focusInner(Character* c, float off, float forward)
    {
        Ogre::Camera* cam = safeGameCamera();
        Ogre::Matrix4 view, proj; Ogre::Vector3 camPos;
        Ogre::Vector3 p = c->getPosition();
        if (cam && safeCameraMatrices(cam, &view, &proj, &camPos))
        {
            Ogre::Vector3 right(view[0][0], view[0][1], view[0][2]);
            right.y = 0; right.normalise();
            Ogre::Vector3 fwd(-view[2][0], 0, -view[2][2]);   // where the camera looks, flattened
            if (fwd.squaredLength() > 1e-6f) { fwd.normalise(); p = p - fwd * forward; }
            p = p - right * off;
        }
        ou->player->focusCamera(p);
        return true;
    }
    bool safeFocus(Character* c, float off, float forward)
    {
        __try { return focusInner(c, off, forward); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}
void chars_debugFocus(Character* c, float rightOffset, float forwardOffset) { if (c) safeFocus(c, rightOffset, forwardOffset); }

// Test: the game's own frame (3D view and interface) saved to a PNG next to the game (Ogre's render target).
bool chars_screenshot(const std::string& file)
{
    log("shot %s: name label of the last player copy: feet at (%d,%d), anchor at (%d,%d), box at (%d,%d), tag height %.2f, view %dx%d", file.c_str(),
        g_nameFeetPx[0], g_nameFeetPx[1], g_nameAnchorPx[0], g_nameAnchorPx[1], g_nameBoxPx[0], g_nameBoxPx[1], g_nameTag, g_nameViewW, g_nameViewH);
    return safeWriteShot(file.c_str());
}

// --------------------------------------------------------------------------- names on the world map
// Every other player is named on the game's map at their last reported position, wherever they are: their squad
// marker only exists while their characters have a body here (near our own), a friend at the other end of the world
// had none. Labels are children of the map picture, so they scroll with it; the position comes from the map's own
// world -> map conversion every frame the map is open (zoom).
namespace
{
    struct MapAccess : public MapScreen
    {
        MyGUI::IntPoint toMap(const Ogre::Vector3& p) { return worldToMapCoords(p); }
    };
    bool safeToMap(MapScreen* m, const Ogre::Vector3* p, int* x, int* y)
    {
        __try { MyGUI::IntPoint q = static_cast<MapAccess*>(m)->toMap(*p); *x = q.left; *y = q.top; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // the map picture that exists now (open or not), and the map screen when it is open
    MapScreen* safeOpenMap(MyGUI::ImageBox** image)
    {
        __try
        {
            ManagementScreen* ms = ManagementScreen::getSingleton();
            if (!ms || !ms->mapScreen) return NULL;
            *image = ms->mapScreen->mapImage;
            if (!*image || !ms->getVisible() || !ms->mapScreen->getVisible()) return NULL;
            return ms->mapScreen;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    const int MAP_LABEL_W = 240, MAP_LABEL_H = 44;
    struct MapLabel { MyGUI::EditBox* w; std::string text; MapLabel() : w(NULL) {} };
    std::map<int, MapLabel> g_mapLabels;          // player id (-1: ourselves, test runs only)
    MyGUI::ImageBox* g_mapParent = NULL;          // the map picture they were made in
    bool g_mapLabelsFailed = false;
    DWORD g_mapLogAt = 0;

    void dropMapLabels(bool destroy)
    {
        if (destroy)
            for (std::map<int, MapLabel>::iterator it = g_mapLabels.begin(); it != g_mapLabels.end(); ++it)
                try { if (it->second.w) MyGUI::Gui::getInstance().destroyWidget(it->second.w); } catch (...) {}
        g_mapLabels.clear();
    }
    MyGUI::EditBox* createMapLabel(MyGUI::ImageBox* parent)
    {
        try
        {
            MyGUI::EditBox* w = parent->createWidget<MyGUI::EditBox>("Kenshi_WordWrapEmpty", MyGUI::IntCoord(0, 0, MAP_LABEL_W, MAP_LABEL_H), MyGUI::Align::Default);
            if (!w) return NULL;
            w->setEditMultiLine(true);
            w->setEditWordWrap(false);
            w->setEditReadOnly(true);
            w->setTextAlign(MyGUI::Align::Center | MyGUI::Align::Bottom);
            w->setTextShadow(true);
            w->setTextColour(MyGUI::Colour(1.f, 1.f, 1.f));
            w->setNeedMouseFocus(false);
            w->setNeedKeyFocus(false);
            return w;
        }
        catch (...) { return NULL; }
    }
}

void chars_updateMapNames()
{
    MyGUI::ImageBox* image = NULL;
    MapScreen* map = g_mapLabelsFailed ? NULL : safeOpenMap(&image);
    // Another map picture (the world was reloaded): the old labels died with the old one, forget them untouched.
    if (image != g_mapParent) { dropMapLabels(false); g_mapParent = image; }
    if (!map)
    {
        for (std::map<int, MapLabel>::iterator it = g_mapLabels.begin(); it != g_mapLabels.end(); ++it)
            try { if (it->second.w) it->second.w->setVisible(false); } catch (...) {}
        return;
    }

    // who: every other player we know a position of (not at war, as the squad markers), plus ourselves in test runs
    std::vector<std::pair<int, Ogre::Vector3> > who;
    std::vector<std::string> names;
    Faction* mine = ou && ou->player ? ou->player->getFaction() : NULL;
    std::vector<PlayerInfo> ps = g_session.players();
    for (size_t i = 0; i < ps.size(); ++i)
    {
        if (ps[i].id == g_session.localId()) continue;
        Ogre::Vector3 at;
        if (!chars_playerPosition(ps[i].id, at)) continue;
        Faction* f = factionFor(ps[i].id);
        if (!g_cfg.playersOnMap || (mine && mine->relations && f && mine->relations->getFactionRelation(f) < 0.f)) continue;
        who.push_back(std::make_pair((int)ps[i].id, at));
        names.push_back(std::string(chat_playerColour(ps[i].id)) + chat_clean(ps[i].name));
    }
    if (g_cfg.autotest && !g_local.empty())
        if (Character* me = g_local[0].h.getCharacter()) { who.push_back(std::make_pair(-1, me->getPosition())); names.push_back("#FFFF00(you)"); }

    std::set<int> seen;
    bool logNow = g_cfg.autotest && GetTickCount() - g_mapLogAt > 5000;
    if (logNow) g_mapLogAt = GetTickCount();
    for (size_t i = 0; i < who.size(); ++i)
    {
        int x, y;
        if (!safeToMap(map, &who[i].second, &x, &y)) { g_mapLabelsFailed = true; log("map names: world -> map conversion failed, disabled"); dropMapLabels(true); return; }
        seen.insert(who[i].first);
        MapLabel& l = g_mapLabels[who[i].first];
        if (!l.w)
        {
            l.w = createMapLabel(image); l.text.clear();
            if (!l.w) { g_mapLabelsFailed = true; log("map names: could not create a label, disabled"); g_mapLabels.erase(who[i].first); dropMapLabels(true); return; }
        }
        std::string text = names[i] + "\n+";   // the "+" sits on the spot, the name above it
        try
        {
            if (text != l.text) { l.text = text; l.w->setCaption(text); }
            l.w->setPosition(x - MAP_LABEL_W / 2, y - MAP_LABEL_H + 12);
            l.w->setVisible(true);
        }
        catch (...) {}
        if (logNow)
        {
            MyGUI::IntSize sz(0, 0); try { sz = image->getSize(); } catch (...) {}
            log("map names: player %d at world (%.0f,%.0f) -> map (%d,%d), map picture %dx%d", who[i].first, who[i].second.x, who[i].second.z, x, y, sz.width, sz.height);
        }
    }
    for (std::map<int, MapLabel>::iterator it = g_mapLabels.begin(); it != g_mapLabels.end(); )
        if (!seen.count(it->first)) { try { if (it->second.w) MyGUI::Gui::getInstance().destroyWidget(it->second.w); } catch (...) {} g_mapLabels.erase(it++); }
        else ++it;
}

// Test: open (or close) the management screen on its map tab, the way the map key does.
std::string chars_debugOpenMap(bool open)
{
    try
    {
        ManagementScreen* ms = ManagementScreen::getSingleton();
        if (!ms || !ms->mapScreen || !ms->tabs) return "no management screen";
        if (!open) { ms->setVisible(false, ms->getCurrentTab()); return "closed"; }
        // the map tab: the tab item that holds the map widget
        int tab = -1;
        for (MyGUI::Widget* w = ms->mapScreen->mainWidget; w && tab < 0; w = w->getParent())
            if (MyGUI::TabItem* ti = w->castType<MyGUI::TabItem>(false)) tab = (int)ms->tabs->getItemIndex(ti);
        if (tab < 0) return "map tab not found";
        ms->setVisible(true, tab);
        char b[120]; sprintf_s(b, "opened on tab %d of %d, map visible %d", tab, (int)ms->tabs->getItemCount(), (int)ms->mapScreen->getVisible());
        return b;
    }
    catch (...) { return "exception"; }
}

void chars_clearPlayerNames()
{
    for (std::map<uint32_t, NameLabel>::iterator it = g_names.begin(); it != g_names.end(); ++it) destroyName(it->second);
    g_names.clear();
}

void chars_showPlayerNames()
{
    if (!g_cfg.playerNames || g_namesFailed) { if (!g_names.empty()) chars_clearPlayerNames(); return; }
    Ogre::Camera* cam = safeGameCamera();
    Ogre::Matrix4 view, proj;
    Ogre::Vector3 camPos;
    bool haveCam = cam && safeCameraMatrices(cam, &view, &proj, &camPos);
    Ogre::Vector3 worldCam, rebase(0, 0, 0);
    if (haveCam && safeWorldCamPos(&worldCam)) rebase = worldCam - camPos;   // world = render space + rebase
    MyGUI::IntSize screen(0, 0);
    try { screen = MyGUI::RenderManager::getInstance().getViewSize(); } catch (...) { haveCam = false; }

    std::set<uint32_t> seen;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        if (isNpcNetId(it->first)) continue;
        Character* c = it->second.h.getCharacter();
        // too far to have a body here: still named, at the owner's last reported position
        if (!c && (!it->second.knownAt || !it->second.hasSpawn)) continue;
        seen.insert(it->first);
        NameLabel& l = g_names[it->first];
        if (!l.w)
        {
            l.w = createName(); l.shown = false; l.text.clear();
            if (!l.w)
            {
                g_names.erase(it->first);
                g_namesFailed = true;
                log("player names: could not create the name labels, disabled");
                problem("names", "the name labels above the other players could not be created");
                chars_clearPlayerNames();
                return;
            }
        }
        g_nameViewW = screen.width; g_nameViewH = screen.height;
        ++g_nameFrames;
        if (!haveCam) { ++g_nameNoCam; hideName(l); continue; }

        // Where the model is drawn (render smoothing may draw it off the simulated body).
        Ghost& g = it->second;
        Ogre::Vector3 at = !c ? g.knownPos : (g_cfg.renderSmoothing && g.hasVisual && g.blend > 0.f ? g.lastDrawn : c->getPosition());
        g_nameFeet[0] = at.x; g_nameFeet[1] = at.y; g_nameFeet[2] = at.z; g_nameTag = c ? safeTagHeight(c) : NAME_HEAD_HEIGHT;
        at.y += g_nameTag;
        at -= rebase;                  // world -> render space
        Ogre::Vector3 v = view * at;   // camera space: the camera looks down -Z
        g_nameCam[0] = camPos.x; g_nameCam[1] = camPos.y; g_nameCam[2] = camPos.z; g_nameAt[0] = at.x; g_nameAt[1] = at.y; g_nameAt[2] = at.z; g_nameV[0] = v.x; g_nameV[1] = v.y; g_nameV[2] = v.z;
        if (v.z > -0.5f) { ++g_nameBehind; hideName(l); continue; }
        if (g_cfg.playerNamesRange > 0 && camPos.distance(at) > (float)g_cfg.playerNamesRange) { ++g_nameFar; hideName(l); continue; }
        Ogre::Vector3 p = proj * v;    // normalised device coordinates (-1..1)
        if (p.x < -1.1f || p.x > 1.1f || p.y < -1.1f || p.y > 1.1f) { ++g_nameOff; hideName(l); continue; }
        g_nameP[0] = p.x; g_nameP[1] = p.y;
        int x = (int)((p.x * 0.5f + 0.5f) * screen.width) - NAME_W / 2;
        // Normalised device y runs upwards, the screen's y downwards. (V23 had this mirrored: it looked right only for a
        // character at the vertical centre of the screen, which is where the test frames had it. A player's video showed
        // label_y + character_y constant over the whole screen = a mirror image about the centre.)
        int y = (int)((0.5f - p.y * 0.5f) * screen.height) - NAME_H;
        {   // shot diagnostics: where the feet, the anchor and the label box land on the screen
            Ogre::Vector3 fr(g_nameFeet[0], g_nameFeet[1], g_nameFeet[2]);
            fr -= rebase;
            Ogre::Vector3 fp = proj * (view * fr);
            g_nameFeetPx[0] = (int)((fp.x * 0.5f + 0.5f) * screen.width); g_nameFeetPx[1] = (int)((0.5f - fp.y * 0.5f) * screen.height);
            g_nameAnchorPx[0] = (int)((p.x * 0.5f + 0.5f) * screen.width); g_nameAnchorPx[1] = (int)((0.5f - p.y * 0.5f) * screen.height);
            g_nameBoxPx[0] = x; g_nameBoxPx[1] = y;
        }

        std::string text = std::string(chat_playerColour(netIdOwner(it->first))) + chat_clean(c ? c->getName() : g.spawn.displayName);
        try
        {
            if (text != l.text) { l.text = text; l.w->setCaption(text); }
            l.w->setPosition(x, y);
            if (!l.shown) { l.shown = true; l.w->setVisible(true); }
            ++g_nameDrawn; g_nameLastX = x; g_nameLastY = y;
        }
        catch (...) {}
    }
    // Characters gone (left, died and removed, world reloaded...): drop their labels.
    for (std::map<uint32_t, NameLabel>::iterator it = g_names.begin(); it != g_names.end(); )
        if (!seen.count(it->first)) { destroyName(it->second); g_names.erase(it++); }
        else ++it;
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
        bool want = g.mode == Ghost::MOVE && !g.held && body.distance(g.visualPos) < MAX_VISUAL_OFFSET;
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

// A copy whose body could not be created (the engine refused, the zone was not loaded yet, it was too far when announced)
// is tried again here: the host announces each of its NPCs once, nobody would ever make it otherwise ("invisible NPC").
static void retryBodiless(DWORD now)
{
    static DWORD last = 0;
    if (now - last < 1000) return;
    last = now;
    std::vector<uint32_t> ids;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        Ghost& g = it->second;
        if (!g.hasSpawn || g.h.getCharacter() || now - g.lastBodyTry < 1500) continue;
        ids.push_back(it->first);
    }
    int budget = 6;   // bodies made per second, closest to us first would be better but this stays simple
    for (size_t k = 0; k < ids.size() && budget > 0; ++k)
    {
        std::map<uint32_t, Ghost>::iterator it = g_ghosts.find(ids[k]);
        if (it == g_ghosts.end()) continue;
        Ghost& g = it->second;
        g.lastBodyTry = now; ++g.bodyTries;
        EntitySpawn sp = g.spawn;
        sp.x = g.knownPos.x; sp.y = g.knownPos.y; sp.z = g.knownPos.z;   // where it is now
        spawnGhost(netIdOwner(ids[k]), sp);
        if (g_ghosts[ids[k]].h.getCharacter()) { --budget; if (g_cfg.autotest) log("ghost %08x: body made on try %d", ids[k], g_ghosts[ids[k]].bodyTries); }
    }
}

void chars_tick(DWORD now)
{
    driveGhosts(now);   // every frame
    retryBodiless(now);
    updateHeld(now);
    fixShopOwners(now);
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
        std::vector<EntityState> states;
        StateCodec::readAll(r, states);   // full or compact form (core/StateCodec.h)
        if (states.empty()) break;
        ClockSync& clock = g_clocks[e.sender];
        clock.observe(senderMs, (uint32_t)preciseTick());
        double snapT = clock.toLocal(senderMs);
        for (size_t i = 0; i < states.size(); ++i)
        {
            EntityState& s = states[i];
            if (!s.sanitize()) continue;
            if (!auth_checkSender(s.netId, e.sender)) continue;   // only the entity's owner simulates it (core/Authority.h)
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
        if (r.ok() && target == g_session.localId() && server_isServer())
        {
            // Dedicated server: a hit only counts from one of the sender's own characters, within reach of the victim.
            Character* attacker = d.attackerNetId ? chars_byNetId(d.attackerNetId) : NULL;
            Character* victim = chars_byNetId(d.victimNetId);
            if (d.attackerNetId && netIdOwner(d.attackerNetId) != e.sender && !isNpcNetId(d.attackerNetId)) { server_refused(e.sender, "damage dealt by someone else's character"); break; }
            if (attacker && victim && attacker->getPosition().distance(victim->getPosition()) > MAX_HIT_DISTANCE) { server_refused(e.sender, "damage from too far away"); break; }
        }
        if (r.ok() && target == g_session.localId()) applyIncomingDamage(d, e.sender);
        break;
    }
    case MSG_ESCAPED:
    {
        uint8_t target = r.u8(); uint32_t id = r.u32();
        if (!r.ok() || target != g_session.localId() || netIdOwner(id) != e.sender) break;
        std::map<uint32_t, Ghost>::iterator gi = g_ghosts.find(id);
        Character* c = gi == g_ghosts.end() ? NULL : gi->second.h.getCharacter();
        if (c && gi->second.held) log("ghost %08x picked the lock: let out (%s)", id, safeFreeFromCage(c) ? "done" : "engine refused");
        break;
    }
    case MSG_CARRIED:
    {
        uint8_t target = r.u8();
        CarriedMsg m; m.read(r);
        if (r.ok() && target == g_session.localId() && m.sanitize()) applyIncomingCarried(m, e.sender);
        break;
    }
    case MSG_HEAL:
    {
        uint8_t target = r.u8();
        HealMsg h; h.read(r);
        h.sanitize();
        if (r.ok() && target == g_session.localId()) applyIncomingHeal(h);
        break;
    }
    case MSG_SHOT:
    {
        uint32_t shooter = r.u32();
        TargetRef t; t.read(r);
        uint8_t stat = r.u8();
        float ax = r.f32(), ay = r.f32(), az = r.f32();
        if (!r.ok() || netIdOwner(shooter) != e.sender || !validPos(ax, ay, az)) break;
        Character* ghost = chars_byNetId(shooter);
        RootObject* target = resolveTargetRef(t);
        if (!ghost || !chars_isGhost(ghost) || !target) break;
        Ogre::Vector3 aim(ax, ay, az);
        int fired = replayShot(ghost, target, stat, &aim);
        if (g_cfg.debugKeys)
        {
            static int shown = 0;
            if (shown < 20) { ++shown; log("ghost %08x fires as its owner did: %s", shooter, fired > 0 ? "shot" : fired == 0 ? "no ranged weapon" : "engine refused"); }
        }
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

        bool ghostProjectile = !g_hitAttacker && dmg.pierce > 0 && takeGhostShot(victim);
        bool attackerIsGhost = g_hitAttacker ? chars_isGhost(static_cast<Character*>(g_hitAttacker)) : ghostProjectile;
        if (ghostProjectile)
        {
            // A ghost's shot only shows what its owner did; the owner's machine computed the real
            // hit and sends it to the victim's owner. Whoever the victim is (one of ours, a ghost,
            // a world NPC), this copy deals nothing.
            if (g_cfg.debugKeys) log("hit on %s dropped: a ghost's projectile (its owner sends the real hit)", victim->getName().c_str());
            return HIT_DROP;
        }
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
        if (attackerIsGhost && chars_netIdOf(victim)) return HIT_DROP;
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

// First aid, doctoring, robot repair: when the patient is the copy of somebody else's character, what the care
// changed is measured and sent to the owner (see flushHeals); the copy is overwritten by the owner's state anyway.
struct HealSnap { uint32_t id; int n; float flesh[32], band[32], rig[32]; float blood; MedicalSystem* m; };
static bool healBegin(MedicalSystem* self, HealSnap& s)
{
    if (!self || !ready() || g_applyingSync) return false;
    for (std::map<uint32_t, Ghost>::iterator it = g_ghosts.begin(); it != g_ghosts.end(); ++it)
    {
        Character* c = it->second.h.getCharacter();
        if (!c || &c->medical != self) continue;
        s.id = it->first; s.m = self; s.blood = self->blood;
        s.n = (int)self->anatomy.size(); if (s.n > 32) s.n = 32;
        for (int i = 0; i < s.n; ++i) { s.flesh[i] = self->anatomy[i]->flesh; s.band[i] = self->anatomy[i]->bandaging; s.rig[i] = self->anatomy[i]->juryRigging; }
        return true;
    }
    return false;
}
static void healEnd(const HealSnap& s)
{
    HealAcc& a = g_heal[s.id];
    for (int i = 0; i < s.n && i < (int)s.m->anatomy.size(); ++i)
    {
        float d = s.m->anatomy[i]->flesh - s.flesh[i];
        if (d > 0.001f) { a.d[i] += d; a.any = true; }
        float db = s.m->anatomy[i]->bandaging - s.band[i];
        if (db > 0.001f) { a.b[i] += db; a.any = true; }
        float dr = s.m->anatomy[i]->juryRigging - s.rig[i];
        if (dr > 0.001f) { a.r[i] += dr; a.any = true; }
    }
    float b = s.m->blood - s.blood;
    if (b > 0.001f) { a.blood += b; a.any = true; }
}
bool (*firstAid_orig)(MedicalSystem*, float, Item*, float, Character*) = NULL;
bool firstAid_hook(MedicalSystem* self, float skill, Item* eq, float dt, Character* who)
{
    HealSnap s; bool on = healBegin(self, s);
    bool r = firstAid_orig(self, skill, eq, dt, who);
    if (on) healEnd(s);
    return r;
}
bool (*doctoring_orig)(MedicalSystem*, float, Item*, float, Character*) = NULL;
bool doctoring_hook(MedicalSystem* self, float skill, Item* eq, float dt, Character* who)
{
    HealSnap s; bool on = healBegin(self, s);
    bool r = doctoring_orig(self, skill, eq, dt, who);
    if (on) healEnd(s);
    return r;
}
bool (*rigging_orig)(MedicalSystem*, float, Item*, float) = NULL;
bool rigging_hook(MedicalSystem* self, float skill, Item* eq, float dt)
{
    HealSnap s; bool on = healBegin(self, s);
    bool r = rigging_orig(self, skill, eq, dt);
    if (on) healEnd(s);
    return r;
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
    // One of ours (or, on the host, a world NPC) fired: its ghosts on the other machines fire the
    // same shot. Only the visual: the hit, if any, is computed here and routed as usual.
    if (!g_replayingShot && me && kmp::ready() && !chars_isGhost(me))
    {
        uint32_t id = chars_netIdOf(me);
        if (!id && g_session.isHost()) id = npcs_netIdOf(me);
        if (id)
        {
            ByteWriter w; w.u32(id); makeTargetRef(target).write(w); w.u8((uint8_t)stat);
            w.f32(aim.x); w.f32(aim.y); w.f32(aim.z);
            g_session.send(MSG_SHOT, w.data);
        }
    }
    if (g_cfg.debugKeys && me)
    {
        static int shown = 0;
        if (shown < 20) { ++shown; log("shot by %s (%s)", me->getName().c_str(), chars_isGhost(me) ? "ghost" : "not a ghost"); }
    }
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
    case LIFT_PERSON: case LIFT_PERSON_SNATCHING_ALLOWED: case PUT_DOWN_CHARACTER_IN_BED: case GET_PUT_IN_BED:
    case KNOCKOUT_PRISONER: case CARRY_WOUNDED_SLAVES: case LIFT_OBJECT_BUT_HEAL_FIRST:
        return true;
    default:
        return false;
    }
}

void (*addOrder_orig)(Character*, Building*, TaskType, RootObject*, bool, bool, const Ogre::Vector3&) = NULL;
void addOrder_hook(Character* self, Building* dest, TaskType t, RootObject* subject, bool shift, bool clear, const Ogre::Vector3& loc)
{
    // One of our characters that another player is carrying, caging or has put to bed takes no orders.
    if (ready() && self && !g_held.empty() && !g_applyingSync && !chars_isGhost(self))
    {
        uint32_t mine = chars_netIdOf(self);
        std::map<uint32_t, HeldChar>::iterator hi = mine ? g_held.find(mine) : g_held.end();
        if (hi != g_held.end())
        {
            // In a cage, the lock can be picked (the player's order); nothing else is possible.
            bool lockTask = t == PICK_LOCK || t == PICK_LOCK_ON_SHACKLES || t == UNLOCK_DOOR_PLAYER_ORDER;
            if (lockTask && hi->second.state == CarriedMsg::IN_CAGE && !hi->second.escaping)
            { if (!hi->second.pickUntil) startPick(mine, self, hi->second, GetTickCount()); }
            else showMessage(T("Your character is being held and cannot receive orders."));
            return;
        }
    }
    if (ready() && self && subject && isTakeBodyTask(t) && !chars_isGhost(self))
    {
        hand h(subject);
        Character* target = (h.type == CHARACTER || h.type == ANIMAL_CHARACTER) ? h.getCharacter() : NULL;
        if (uint32_t id = target ? ghostIdOf(target) : 0)
        {
            if (!isNpcNetId(id))
            {
                // Another player's character: only when it is knocked out (no war needed). It stays its owner's;
                // our engine carries / cages / beds the copy and tells the owner where it is (driveGhosts).
                std::map<uint32_t, Ghost>::iterator gi = g_ghosts.find(id);
                bool down = gi != g_ghosts.end() && (gi->second.last.flags & EntityState::UNCONSCIOUS) != 0 &&
                            !(gi->second.last.flags & EntityState::DEAD);
                if (!down || t == KNOCKOUT_PRISONER)
                {
                    showMessage(T("Only a knocked-out character can be carried, caged or put to bed."));
                    return;
                }
            }
            else chars_claimWhenNear(id, target);
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
    if (g_trackErrN > 60 && (g_trackErrSum / g_trackErrN > 40.0 || g_snaps > 25))
        problem("tracking", "the other players' characters follow badly (mean lag %.0f units, %d teleports in the last 30 s): network or load problem", g_trackErrSum / g_trackErrN, g_snaps);
    if (g_jumpFrames) { log("ghost tracking: %ld unexplained jump(s) under 20 units and %ld of 20+ in %ld frame-samples (largest %.1f)", g_microJumps, g_bigJumps, g_jumpFrames, g_jumpMax); g_microJumps = g_bigJumps = g_jumpFrames = 0; g_jumpMax = 0; }
    if (g_nameFrames)
    {
        log("player names: %d label(s); of %ld label-frames: %ld drawn (last at %d,%d), %ld without camera, %ld behind the camera, %ld too far, %ld off screen",
            (int)g_names.size(), g_nameFrames, g_nameDrawn, g_nameLastX, g_nameLastY, g_nameNoCam, g_nameBehind, g_nameFar, g_nameOff);
        float cd[12];
        if (safeCamDiag(cd))
            log("player names: camera local (%.0f,%.0f,%.0f), derived (%.0f,%.0f,%.0f), getCameraPos (%.0f,%.0f,%.0f), node derived (%.0f,%.0f,%.0f)",
                cd[0], cd[1], cd[2], cd[3], cd[4], cd[5], cd[6], cd[7], cd[8], cd[9], cd[10], cd[11]);
        log("player names: camera (%.0f,%.0f,%.0f), label anchor (%.0f,%.0f,%.0f), in camera space (%.1f,%.1f,%.1f)",
            g_nameCam[0], g_nameCam[1], g_nameCam[2], g_nameAt[0], g_nameAt[1], g_nameAt[2], g_nameV[0], g_nameV[1], g_nameV[2]);
        log("player names: feet (%.1f,%.1f,%.1f), tag height %.2f, ndc (%.3f,%.3f), view %dx%d", g_nameFeet[0], g_nameFeet[1], g_nameFeet[2], g_nameTag, g_nameP[0], g_nameP[1], g_nameViewW, g_nameViewH);
        g_nameFrames = g_nameDrawn = g_nameNoCam = g_nameOff = g_nameBehind = g_nameFar = 0;
    }
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
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&MedicalSystem::applyFirstAid), firstAid_hook, &firstAid_orig))
        ErrorLog("KenshiMP: could not hook applyFirstAid (treating a friend will not reach him)");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&MedicalSystem::applyDoctoring), doctoring_hook, &doctoring_orig))
        ErrorLog("KenshiMP: could not hook applyDoctoring");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&MedicalSystem::applyRigging), rigging_hook, &rigging_orig))
        ErrorLog("KenshiMP: could not hook applyRigging");
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
