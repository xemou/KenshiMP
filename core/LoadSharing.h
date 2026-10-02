// Load sharing: who simulates the world around a client (see plugin/Npcs.cpp).
// Close to the host's characters, the host does (one shared world); far away, the client does
// (its own CPU, and the host no longer keeps that region loaded). Two distances with a wide gap
// between them, so a player walking along the limit does not make the world flip back and forth.
#pragma once

namespace mp {

const float SHARE_ENTER = 2000.f;   // closer than this: shared world
const float SHARE_LEAVE = 3000.f;   // further than this: the client's own world

enum { SHARE_UNKNOWN = -1, SHARE_APART = 0, SHARE_SHARED = 1 };

// prev: the client's current mode (SHARE_UNKNOWN the first time), distance: from the client's
// nearest character to the host's nearest character.
inline int shareMode(int prev, float distance)
{
    if (prev == SHARE_SHARED) return distance < SHARE_LEAVE ? SHARE_SHARED : SHARE_APART;
    return distance < SHARE_ENTER ? SHARE_SHARED : SHARE_APART;
}

} // namespace mp
