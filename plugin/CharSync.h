// Character capture helpers shared by Characters.cpp (player squads) and Npcs.cpp (world NPCs).
#pragma once
#include "../core/Protocol.h"
#include <ogre/OgreVector3.h>
#include <vector>

class Character;
class Faction;

namespace kmp {

// Full state of a character (transform, health, combat, task...).
void cs_captureState(Character* c, uint32_t id, mp::EntityState& s);
// Bodies of MSG_APPEARANCE / MSG_EQUIPMENT / MSG_STATS for this character.
mp::Bytes cs_appearanceMsg(Character* c, uint32_t id);
mp::Bytes cs_equipmentMsg(Character* c, uint32_t id);
mp::Bytes cs_statsMsg(Character* c, uint32_t id);
// Current positions of the ghosts owned by a remote player (its squad, as seen here).
void cs_ghostPositions(uint8_t owner, std::vector<Ogre::Vector3>& out);
// Positions of our own player characters.
void cs_localPositions(std::vector<Ogre::Vector3>& out);
// SEH-guarded Faction::destroyObject.
bool cs_destroy(Character* c);
// A world NPC described well enough to be created again (client: the local people removed when the
// host's world takes over come back when we are on our own again, see Npcs.cpp).
struct StashedNpc
{
    std::string tmpl, faction, name;     // character GameData, faction GameData, display name
    Ogre::Vector3 pos;
    mp::AppearanceBlob look;
    std::vector<mp::ItemRef> gear;       // what it wore and held (hasGear: it was read, an empty list then means naked)
    bool hasGear;
    uint32_t at;                         // GetTickCount() when it was removed
    StashedNpc() : pos(Ogre::Vector3::ZERO), hasGear(false), at(0) {}
};
bool cs_stashNpc(Character* c, StashedNpc& out);       // describe it (before cs_destroy)
bool cs_restoreNpc(const StashedNpc& s);               // create it again as a local world NPC
// Loaded characters of a faction within `radius` of `pos` (walks its active squads; the engine's
// own getCharactersInArea returns nothing for most factions).
void cs_charactersNear(Faction* f, const Ogre::Vector3& pos, float radius, std::vector<Character*>& out);
// Every character the engine is currently updating (GameWorld::charUpdateListMain).
void cs_allActiveCharacters(std::vector<Character*>& out);

// Npcs.cpp
uint32_t npcs_netIdOf(Character* c);   // host: replicated NPC id, 0 otherwise
Character* npcs_byNetId(uint32_t id);  // host: the real NPC

} // namespace kmp
