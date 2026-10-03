// Wire protocol shared by host and clients.
//
// Framing on the TCP stream:  [u32 length][u8 type][body...]   (length counts type + body)
//
// Client -> host : [type][body]
// Host -> client : [type][u8 sender][body]     for every relayed message (see isRelayed)
//                  WELCOME / PLAYER_JOIN / PLAYER_LEAVE / PONG have their own layout below.
//
// Authority model: every player owns and simulates his own squad/faction/buildings. Everybody
// else only receives them as "ghosts". A DAMAGE message is routed to the *owner* of the victim,
// who applies it locally.
#pragma once
#include "Buffer.h"
#include <utility>
#include <math.h>

namespace mp {

const uint32_t PROTOCOL_VERSION = 16;

// World NPCs replicated by the host use the upper half of the host's local id space.
const uint32_t NPC_ID_FLAG = 0x800000;
inline bool isNpcNetId(uint32_t id) { return (id >> 24) == 0 && (id & NPC_ID_FLAG) != 0; }
const uint8_t  HOST_ID          = 0;
const uint8_t  BROADCAST        = 0xFF;
const uint32_t MAX_PACKET       = 1u << 20;
const int      MAX_PLAYERS      = 8;

enum MsgType
{
    MSG_HELLO = 1,        // c->h  u32 version, str name, str faction, str mods, u32 passwordHash
    MSG_WELCOME,          // h->c  u8 yourId, u8 count, {u8 id, str name, str faction}*
    MSG_PLAYER_JOIN,      // h->c  u8 id, str name, str faction
    MSG_PLAYER_LEAVE,     // h->c  u8 id
    MSG_PING,             // c->h  u32 token
    MSG_PONG,             // h->c  u32 token
    MSG_REJECT,           // h->c  str reason (connection closed afterwards)

    // --- relayed messages: [type][sender][body] on the way out of the host ---
    MSG_ENTITY_STATE,     // u32 senderClockMs, u16 count, EntityState*   (20 Hz players, LOD for NPCs)
    MSG_ENTITY_SPAWN,     // EntitySpawn
    MSG_ENTITY_DESPAWN,   // u32 netId
    MSG_DAMAGE,           // u8 targetPlayer, DamageMsg     (routed only to targetPlayer)
    MSG_CHAT,             // str text
    MSG_BUILDING_STATE,   // u16 count, BuildingMsg*        (create if unknown, else update progress)
    MSG_BUILDING_REMOVE,  // u32 netId
    MSG_FACTION_RELATION, // u8 otherPlayer, f32 relation   (war/peace between two player factions)
    MSG_WORLD_SYNC,       // f64 gameTime, u32 flags        (host only, periodic)
    MSG_APPEARANCE,       // u32 netId, AppearanceBlob
    MSG_EQUIPMENT,        // u32 netId, u8 count, ItemRef*, then optional limbs: u8 4, (u8 LimbState, ItemRef)*4
    MSG_STATS,            // u32 netId, u16 count, f32*      (skills/attributes, on change)
    MSG_BUILDING_DAMAGE,  // u8 targetPlayer, BuildingDamageMsg (routed only to targetPlayer)
    MSG_FACTION_TABLE,    // u16 count, {str factionSid, f32 relation}*  (sender's standing with world factions)
    MSG_NPC_CLAIM,        // c->h u32 npcNetId : the client recruited this NPC, host hands it over
    MSG_RESYNC,           // (empty) sender (re)loaded its world: everybody resends its full state
    MSG_INVENTORY,        // u8 kind, u32 containerNetId, u16 count, InvItem*  (owner -> others, on change; equipped items excluded), then optional u8 1, u32 squadMoney (NPCs)
    MSG_ITEM_TAKE,        // u8 targetPlayer, u8 kind, u32 containerNetId, InvItem : someone took this from the target's container
    MSG_ITEM_GIVE,        // u8 targetPlayer, u8 kind, u32 containerNetId, InvItem : someone put this into the target's container
    MSG_WEATHER,          // h->c u16 count, per region: str seasonIds, u32 season, u32 seasonEndDay, str weather, f32 strength, effect, wind, wx, wy, wz, time, u32 startMin, endMin
    MSG_GROUND_ITEM,      // u32 netId, InvItem, f32 x, y, z : the owner dropped this item there, then optional u8 1, u16 n, InvItem* (a bag's content)
    MSG_GROUND_REMOVE,    // u32 netId : that ground item is gone (picked up, destroyed)
    MSG_GROUND_TAKE,      // u8 targetPlayer, u32 netId : we picked up our copy of the target's item
    MSG_TRADE,            // u8 targetPlayer, u8 action (0 request, 1 accept, 2 end), u32 senderChar, u32 targetChar
    MSG_ITEM_UNDO,        // u8 targetPlayer, u8 action (UNDO_*), u8 kind (CONTAINER_*), u32 netId, InvItem : the owner refused that transfer
    MSG_ZONE_MODE,        // h->c u8 targetPlayer, u8 shared : 1 = the host simulates the world around you, 0 = you do (far from the host)
    MSG_SHOT,             // u32 shooterNetId, TargetRef target, u8 stat, f32 aimX, aimY, aimZ : that character fired (its ghosts fire too, harmlessly)
    MSG_WORLD_STATES,     // h->c u16 n, {str uniqueNpcSid, u8 state (0 dead, 1 alive, 2 imprisoned), u8 playerInvolved}*, u16 m, {str townSid, str replacementTownSid}*
    MSG_WORLD_STATE_REPORT, // c->h u8 HOST_ID, then the states part of MSG_WORLD_STATES : changes in a client's own world (load sharing)
    MSG_WORLD_BUILDINGS,  // h->c u16 n, WorldBuildingState* : town buildings broken / repaired / destroyed in the host's world
    MSG_WORLD_BUILDING_REPORT // c->h u8 HOST_ID, u8 kind: 0 = hit (WorldBuildingState with no doors, BuildingDamageMsg), 1 = u16 n, WorldBuildingState* (client's own world)
};

inline bool isRelayed(uint8_t t) { return t >= MSG_ENTITY_STATE; }

// netId = (owner << 24) | local index.
inline uint32_t makeNetId(uint8_t owner, uint32_t local) { return ((uint32_t)owner << 24) | (local & 0xFFFFFF); }
inline uint8_t  netIdOwner(uint32_t id) { return (uint8_t)(id >> 24); }

// ---- input validation: never hand NaN/inf or absurd values to the engine ----
const float WORLD_LIMIT = 1.0e6f;      // Kenshi's map spans roughly +-100 000 units
const float MAX_HIT     = 500.f;       // no single blow deals more than this per damage type

inline bool finiteF(float v) { return v == v && v < 3.0e38f && v > -3.0e38f; }
inline bool validCoord(float v) { return finiteF(v) && v < WORLD_LIMIT && v > -WORLD_LIMIT; }
inline float clampF(float v, float lo, float hi) { return !finiteF(v) ? lo : (v < lo ? lo : (v > hi ? hi : v)); }
inline bool validPos(float x, float y, float z) { return validCoord(x) && validCoord(y) && validCoord(z); }
// Unit quaternion or identity.
inline void sanitizeQuat(float& x, float& y, float& z, float& w)
{
    float n = x * x + y * y + z * z + w * w;
    if (!finiteF(n) || n < 1e-6f) { x = y = z = 0; w = 1; return; }
    float inv = 1.0f / sqrtf(n);
    x *= inv; y *= inv; z *= inv; w *= inv;
}

// Reference to something a character interacts with (combat target, task subject).
struct TargetRef
{
    enum Kind { NONE = 0, NET_CHARACTER, NET_BUILDING, WORLD_CHARACTER, WORLD_BUILDING };
    uint8_t kind;
    uint32_t netId;          // NET_* kinds
    std::string sid;         // WORLD_BUILDING: FCS id of the building type
    float x, y, z;           // WORLD_*: where the thing is (matched to the nearest local one)

    TargetRef() : kind(NONE), netId(0), x(0), y(0), z(0) {}
    bool operator==(const TargetRef& o) const
    { return kind == o.kind && netId == o.netId && sid == o.sid && x == o.x && y == o.y && z == o.z; }
    bool operator!=(const TargetRef& o) const { return !(*this == o); }
    void write(ByteWriter& w) const
    {
        w.u8(kind);
        if (kind == NET_CHARACTER || kind == NET_BUILDING) w.u32(netId);
        else if (kind != NONE) { w.str(sid); w.f32(x); w.f32(y); w.f32(z); }
    }
    void read(ByteReader& r)
    {
        kind = r.u8();
        if (kind == NET_CHARACTER || kind == NET_BUILDING) netId = r.u32();
        else if (kind != NONE) { sid = r.str(); x = r.f32(); y = r.f32(); z = r.f32(); }
    }
};

struct EntityState
{
    enum Flags { DEAD = 1, UNCONSCIOUS = 2, RUNNING = 4, IN_COMBAT = 8, RAGDOLL = 16 };

    uint32_t netId;
    float x, y, z;          // world position
    float qx, qy, qz, qw;   // orientation
    float health;           // overall health rating
    uint16_t anim;          // unused (kept for layout stability)
    uint8_t  flags;         // Flags
    uint8_t  moveSpeed;     // MoveSpeed orders (WALK/JOG/RUN...)
    float blood;
    std::vector<float> flesh;       // per body part, anatomy order
    TargetRef combatTarget;
    uint16_t task;                  // current TaskType (0xFFFF = none)
    TargetRef taskSubject;
    float tx, ty, tz;               // task location
    float vx, vy, vz;               // velocity (units/s), for extrapolation
    int8_t floor;                   // building floor (0 = ground / outside)

    EntityState() : netId(0), x(0), y(0), z(0), qx(0), qy(0), qz(0), qw(1), health(1), anim(0), flags(0),
                    moveSpeed(0), blood(0), task(0xFFFF), tx(0), ty(0), tz(0), vx(0), vy(0), vz(0), floor(0) {}
    void write(ByteWriter& w) const
    {
        w.u32(netId); w.f32(x); w.f32(y); w.f32(z);
        w.f32(qx); w.f32(qy); w.f32(qz); w.f32(qw);
        w.f32(health); w.u16(anim); w.u8(flags);
        w.u8(moveSpeed); w.f32(blood);
        w.u8((uint8_t)flesh.size()); for (size_t k = 0; k < flesh.size(); ++k) w.f32(flesh[k]);
        combatTarget.write(w);
        w.u16(task); taskSubject.write(w); w.f32(tx); w.f32(ty); w.f32(tz);
        w.f32(vx); w.f32(vy); w.f32(vz);
        w.u8((uint8_t)floor);
    }
    void read(ByteReader& r)
    {
        netId = r.u32(); x = r.f32(); y = r.f32(); z = r.f32();
        qx = r.f32(); qy = r.f32(); qz = r.f32(); qw = r.f32();
        health = r.f32(); anim = r.u16(); flags = r.u8();
        moveSpeed = r.u8(); blood = r.f32();
        uint8_t n = r.u8(); flesh.clear(); for (int k = 0; k < n && r.ok(); ++k) flesh.push_back(r.f32());
        combatTarget.read(r);
        task = r.u16(); taskSubject.read(r); tx = r.f32(); ty = r.f32(); tz = r.f32();
        vx = r.f32(); vy = r.f32(); vz = r.f32();
        floor = (int8_t)r.u8();
    }
    // False if unusable (bad position); otherwise repairs the rest in place.
    bool sanitize()
    {
        if (!validPos(x, y, z)) return false;
        sanitizeQuat(qx, qy, qz, qw);
        health = clampF(health, -1000.f, 1000.f);
        blood = clampF(blood, 0.f, 10000.f);
        for (size_t k = 0; k < flesh.size(); ++k) flesh[k] = clampF(flesh[k], -1000.f, 1000.f);
        if (!validPos(tx, ty, tz)) { tx = x; ty = y; tz = z; }
        if (combatTarget.kind > TargetRef::WORLD_BUILDING || !validPos(combatTarget.x, combatTarget.y, combatTarget.z)) combatTarget = TargetRef();
        if (taskSubject.kind > TargetRef::WORLD_BUILDING || !validPos(taskSubject.x, taskSubject.y, taskSubject.z)) taskSubject = TargetRef();
        if (moveSpeed > 4) moveSpeed = 4;
        vx = clampF(vx, -100.f, 100.f); vy = clampF(vy, -100.f, 100.f); vz = clampF(vz, -100.f, 100.f);
        if (floor < 0 || floor > 20) floor = 0;
        return true;
    }
};

// Hit on a ghost building (or one of its doors), routed to the building's owner.
struct BuildingDamageMsg
{
    uint32_t buildingNetId;
    uint8_t  door;           // index in the building's door list, 0xFF = the building itself
    uint32_t attackerNetId;  // 0 if unknown
    float cut, blunt, pierce;
    float dismantle;         // addDismantleProgress amount (building itself)
    BuildingDamageMsg() : buildingNetId(0), door(0xFF), attackerNetId(0), cut(0), blunt(0), pierce(0), dismantle(0) {}
    void write(ByteWriter& w) const
    { w.u32(buildingNetId); w.u8(door); w.u32(attackerNetId); w.f32(cut); w.f32(blunt); w.f32(pierce); w.f32(dismantle); }
    void read(ByteReader& r)
    { buildingNetId = r.u32(); door = r.u8(); attackerNetId = r.u32(); cut = r.f32(); blunt = r.f32(); pierce = r.f32(); dismantle = r.f32(); }
    void sanitize()
    {
        cut = clampF(cut, 0, MAX_HIT); blunt = clampF(blunt, 0, MAX_HIT); pierce = clampF(pierce, 0, MAX_HIT);
        dismantle = clampF(dismantle, -MAX_HIT, MAX_HIT);
    }
};

// A building of the world (town, outpost... not a player's): identified by its FCS id and position
// (the same on every machine), state = destroyed flag and broken flag of each door.
struct WorldBuildingState
{
    std::string sid;
    float x, y, z;
    uint8_t destroyed;
    std::vector<uint8_t> brokenDoors;   // 0 / 1 per door, the building's door order

    WorldBuildingState() : x(0), y(0), z(0), destroyed(0) {}
    void write(ByteWriter& w) const
    {
        w.str(sid); w.f32(x); w.f32(y); w.f32(z); w.u8(destroyed);
        w.u8((uint8_t)brokenDoors.size()); for (size_t k = 0; k < brokenDoors.size(); ++k) w.u8(brokenDoors[k]);
    }
    void read(ByteReader& r)
    {
        sid = r.str(); x = r.f32(); y = r.f32(); z = r.f32(); destroyed = r.u8();
        uint8_t n = r.u8(); brokenDoors.clear(); for (int k = 0; k < n && r.ok(); ++k) brokenDoors.push_back(r.u8() ? 1 : 0);
    }
    bool sanitize() { if (destroyed) destroyed = 1; return !sid.empty() && validPos(x, y, z); }
    bool sameState(const WorldBuildingState& o) const { return destroyed == o.destroyed && brokenDoors == o.brokenDoors; }
};

enum EntityKind { KIND_CHARACTER = 0, KIND_ANIMAL = 1, KIND_BUILDING = 2 };

struct EntitySpawn
{
    uint32_t netId;
    uint8_t  kind;
    std::string gameDataName;   // FCS string id of the template (race / building)
    std::string displayName;
    float x, y, z, yaw;
    std::string factionSid;     // world NPCs: FCS id of their real faction ("" = the sender's faction)

    EntitySpawn() : netId(0), kind(0), x(0), y(0), z(0), yaw(0) {}
    void write(ByteWriter& w) const
    {
        w.u32(netId); w.u8(kind); w.str(gameDataName); w.str(displayName);
        w.f32(x); w.f32(y); w.f32(z); w.f32(yaw); w.str(factionSid);
    }
    void read(ByteReader& r)
    {
        netId = r.u32(); kind = r.u8(); gameDataName = r.str(); displayName = r.str();
        x = r.f32(); y = r.f32(); z = r.f32(); yaw = r.f32(); factionSid = r.str();
    }
    bool sanitize() { if (!finiteF(yaw)) yaw = 0; return validPos(x, y, z) && !gameDataName.empty(); }
};

struct BuildingMsg
{
    uint32_t netId;
    std::string gameDataName;
    float x, y, z;
    float qx, qy, qz, qw;
    float buildProgress;    // raw ConstructionState::constructionProgress of the owner
    uint8_t completed;
    uint8_t destroyed;
    enum DoorBits { DOOR_BROKEN = 1, DOOR_LOCKED = 2 };
    std::vector<uint8_t> doors;   // DoorBits per door, parent's door order

    BuildingMsg() : netId(0), x(0), y(0), z(0), qx(0), qy(0), qz(0), qw(1), buildProgress(0), completed(0), destroyed(0) {}
    void write(ByteWriter& w) const
    {
        w.u32(netId); w.str(gameDataName);
        w.f32(x); w.f32(y); w.f32(z);
        w.f32(qx); w.f32(qy); w.f32(qz); w.f32(qw);
        w.f32(buildProgress); w.u8(completed); w.u8(destroyed);
        w.u8((uint8_t)doors.size()); for (size_t k = 0; k < doors.size(); ++k) w.u8(doors[k]);
    }
    void read(ByteReader& r)
    {
        netId = r.u32(); gameDataName = r.str();
        x = r.f32(); y = r.f32(); z = r.f32();
        qx = r.f32(); qy = r.f32(); qz = r.f32(); qw = r.f32();
        buildProgress = r.f32(); completed = r.u8(); destroyed = r.u8();
        uint8_t n = r.u8(); doors.clear(); for (int k = 0; k < n && r.ok(); ++k) doors.push_back(r.u8());
    }
    bool sanitize()
    {
        if (!validPos(x, y, z) || gameDataName.empty()) return false;
        sanitizeQuat(qx, qy, qz, qw);
        buildProgress = clampF(buildProgress, 0.f, 1.0e6f);
        return true;
    }
};

// One equipped item: FCS string ids of the item, its manufacturer/model and its material.
struct ItemRef
{
    std::string item, manufacturer, material;
    void write(ByteWriter& w) const { w.str(item); w.str(manufacturer); w.str(material); }
    void read(ByteReader& r) { item = r.str(); manufacturer = r.str(); material = r.str(); }
    bool operator==(const ItemRef& o) const { return item == o.item && manufacturer == o.manufacturer && material == o.material; }
};

// Container kinds of MSG_INVENTORY / MSG_ITEM_*.
enum { CONTAINER_CHARACTER = 0, CONTAINER_BUILDING = 1, CONTAINER_BACKPACK = 2,   // BACKPACK: id = the wearer's net id
       CONTAINER_GROUND = 3 };                                                     // MSG_ITEM_UNDO only: id = ground item
// MSG_ITEM_UNDO actions: what the requester must do to its own characters.
enum { UNDO_REMOVE = 0,     // it took something the owner did not give up: remove it again
       UNDO_GIVE_BACK = 1 };  // it put something the owner did not accept: give it back
const uint16_t MAX_INV_ITEMS = 1000;

// One stack in an inventory.
struct InvItem
{
    std::string item, manufacturer, material;   // FCS ids
    int32_t quantity;
    float quality;
    InvItem() : quantity(1), quality(0) {}
    void write(ByteWriter& w) const { w.str(item); w.str(manufacturer); w.str(material); w.u32((uint32_t)quantity); w.f32(quality); }
    void read(ByteReader& r) { item = r.str(); manufacturer = r.str(); material = r.str(); quantity = (int32_t)r.u32(); quality = r.f32(); }
    bool sanitize()
    {
        if (item.empty()) return false;
        if (quantity < 1) quantity = 1;
        if (quantity > 100000) quantity = 100000;
        quality = clampF(quality, 0.f, 1000.f);
        return true;
    }
    bool sameKind(const InvItem& o) const { return item == o.item && manufacturer == o.manufacturer && material == o.material; }
};

// Flattened copy of a GameData (used for character appearance).
struct AppearanceBlob
{
    struct Ref { std::string sid; int32_t v0, v1, v2; };
    std::vector<std::pair<std::string, std::string> > s;
    std::vector<std::pair<std::string, int32_t> > i;
    std::vector<std::pair<std::string, float> > f;
    std::vector<std::pair<std::string, uint8_t> > b;
    std::vector<std::pair<std::string, std::vector<Ref> > > lists;

    void write(ByteWriter& w) const
    {
        w.u16((uint16_t)s.size()); for (size_t k = 0; k < s.size(); ++k) { w.str(s[k].first); w.str(s[k].second); }
        w.u16((uint16_t)i.size()); for (size_t k = 0; k < i.size(); ++k) { w.str(i[k].first); w.u32((uint32_t)i[k].second); }
        w.u16((uint16_t)f.size()); for (size_t k = 0; k < f.size(); ++k) { w.str(f[k].first); w.f32(f[k].second); }
        w.u16((uint16_t)b.size()); for (size_t k = 0; k < b.size(); ++k) { w.str(b[k].first); w.u8(b[k].second); }
        w.u16((uint16_t)lists.size());
        for (size_t k = 0; k < lists.size(); ++k)
        {
            w.str(lists[k].first);
            const std::vector<Ref>& refs = lists[k].second;
            w.u16((uint16_t)refs.size());
            for (size_t n = 0; n < refs.size(); ++n)
            { w.str(refs[n].sid); w.u32((uint32_t)refs[n].v0); w.u32((uint32_t)refs[n].v1); w.u32((uint32_t)refs[n].v2); }
        }
    }
    void read(ByteReader& r)
    {
        uint16_t n = r.u16(); for (int k = 0; k < n && r.ok(); ++k) { std::string a = r.str(); s.push_back(std::make_pair(a, r.str())); }
        n = r.u16(); for (int k = 0; k < n && r.ok(); ++k) { std::string a = r.str(); i.push_back(std::make_pair(a, (int32_t)r.u32())); }
        n = r.u16(); for (int k = 0; k < n && r.ok(); ++k) { std::string a = r.str(); f.push_back(std::make_pair(a, r.f32())); }
        n = r.u16(); for (int k = 0; k < n && r.ok(); ++k) { std::string a = r.str(); b.push_back(std::make_pair(a, r.u8())); }
        n = r.u16();
        for (int k = 0; k < n && r.ok(); ++k)
        {
            std::string name = r.str();
            uint16_t m = r.u16();
            std::vector<Ref> refs;
            for (int q = 0; q < m && r.ok(); ++q)
            { Ref x; x.sid = r.str(); x.v0 = (int32_t)r.u32(); x.v1 = (int32_t)r.u32(); x.v2 = (int32_t)r.u32(); refs.push_back(x); }
            lists.push_back(std::make_pair(name, refs));
        }
    }
};

// Cheap content hash so we only resend appearance/equipment when they change.
inline uint32_t hashBytes(const Bytes& b)
{
    uint32_t h = 2166136261u;
    for (size_t k = 0; k < b.size(); ++k) { h ^= b[k]; h *= 16777619u; }
    return h;
}

struct DamageMsg
{
    uint32_t victimNetId;
    uint32_t attackerNetId;
    uint8_t  bodyPart;      // index into the victim's anatomy
    float cut, blunt, pierce;

    DamageMsg() : victimNetId(0), attackerNetId(0), bodyPart(0), cut(0), blunt(0), pierce(0) {}
    void write(ByteWriter& w) const
    {
        w.u32(victimNetId); w.u32(attackerNetId); w.u8(bodyPart);
        w.f32(cut); w.f32(blunt); w.f32(pierce);
    }
    void read(ByteReader& r)
    {
        victimNetId = r.u32(); attackerNetId = r.u32(); bodyPart = r.u8();
        cut = r.f32(); blunt = r.f32(); pierce = r.f32();
    }
    void sanitize() { cut = clampF(cut, 0, MAX_HIT); blunt = clampF(blunt, 0, MAX_HIT); pierce = clampF(pierce, 0, MAX_HIT); }
};

} // namespace mp
