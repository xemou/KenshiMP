# KenshiMP — "Native feel": point by point

Status as of 2026-10-01. ✅ done and seen in game · 🧪 done, not yet seen in game · 💡 proposed solution (not done yet)

## 1. What the other player sees of you

| Point | Status | Solution |
|---|---|---|
| Ghost that freezes then teleports | ✅ | Cause: Kenshi's "off-screen" mode (coarse updates of characters outside the camera). Player ghosts are exempted from it. Teleport only if the ghost is really lost. Extreme path replayed: 1980 → 5 teleports, frozen 62 % → 0 %. |
| Lag at the start of a sprint (25-50 units) | ✅ | Nudge along the owner's real path (never through a wall) when the engine accelerates too slowly. Measured: mean gap 10.5 → 6 units, 4 teleports over the whole extreme path. |
| Task animations | 🧪 | Wider list (medic, robot repair, machines to fill/empty/unjam, mines). A task is only copied if the owner is standing still (otherwise the ghost stayed planted). The ghost is released when the task or combat ends. |
| Player name on their characters | ✅ | The ghost is called "Name [Nickname]"; that name is always drawn above it in the player's colour (`player_names`, to check in game). |
| Robotic prosthetics | ✅ | Seen in game: the ghost's right arm shows up as a prosthetic (blue bar in health), applied only once. |
| States: hunger, encumbrance | ✅ | The owner's hunger is sent with the stats and applied to the ghost (seen in the log in game). Encumbrance follows from the already mirrored inventory. Kenshi has no drunkenness. |
| Backpack contents | ✅ | The worn backpack is one more container (`CONTAINER_BACKPACK`): its contents are mirrored on the ghost and an item taken from it is removed on the owner's side (seen in game, "Refitted Oil Drum Backpack"). Put back when the ghost is re-dressed. |
| Host NPCs outside the camera | 🧪 | NPCs within 150 units of your characters are exempted from off-screen mode (smooth fights and chases), the others stay cheap. |

## 2. UI integrated into the game

| Point | Status | Solution |
|---|---|---|
| MULTIPLAYER button under the pause menu | ✅ | Seen in game, same style as the game's buttons. |
| Other players on the map | ✅ | Squads added to the game's map, removed during a war and restored at peace (verified in game: shown → hidden at war → shown again at peace). Option `show_players_on_map`. |
| Diplomacy | ✅ | In the Multiplayer window: one line per player, current state, WAR / PEACE / ALLY / TRADE buttons (seen in game). 💡 Later: the same buttons in the game's Factions screen (hooking `FactionsScreen`: modifies a game screen, more fragile; mostly cosmetic gain). |
| Player list | ✅ | Multiplayer window (F4, pause menu, title screen): players, ping, diplomacy. |
| Joins / leaves / wars in the chat | ✅ | Grey lines in the history (seen in game: "Bot joined the game", "Bot declared war on you!"). |
| Esc closes the window | ✅ | Esc closes the Multiplayer window without opening the game's pause menu behind it (seen in game). |
| Language | ✅ | The game's (seen in game in French). |

## 3. Trading and items

| Point | Status | Solution |
|---|---|---|
| Item dropped on the ground by another player | ✅ | Appears on your side; picking it up removes the original on the owner's side (seen in game, full chain). |
| Merchant money | 🧪 | Merchant's amount sent by the host, applied when the trade opens, gains/payments sent back to the real merchant. |
| Direct trade between players | ✅ | TRADE button on the player's line (Multiplayer window, F4) when one of your characters is next to one of theirs; the other player gets a message and clicks ACCEPT. The game's trade window opens on both sides (seen in game); every moved item goes through the validated transfers. Closing the window (Esc) ends the trade on both sides. |

## 4. Connection

| Point | Status | Solution |
|---|---|---|
| Message while connecting | 🧪 | "Connecting to host..." while loading if the connection is not established yet, then "Connected" (message + chat). |
| Reconnect after a drop | ✅ | Same slot, ghost recreated (seen in game). Fixed: the relation (war/peace/alliance) is remembered by name and restored on return (before: reset to the default). |
| Real game over the Internet | 💡 | Session to plan between two PCs (VPN such as Radmin/Tailscale, or forwarding TCP port 47000). The only real remaining network unknown. |

## 5. Shared world

| Point | Status | Solution |
|---|---|---|
| World towns (destruction, captures) | 🧪 | Kenshi changes a town through "world states" (unique NPCs dead / jailed, a hidden table saved with the game) and the town's "override town" versions, chosen when its area loads. The host sends its world states and its changed towns (`MSG_WORLD_STATES`); clients write the states into their own table and switch the towns whose area is not loaded (the new version shows when the area loads, as in the base game). A client simulating its own surroundings (load sharing) reports its uniques' deaths to the host. Town buildings (🧪): a door broken by a client in the host's world is broken by the host (the hit is forwarded, no local damage), broken/repaired doors and destroyed buildings go from the host to everybody (`MSG_WORLD_BUILDINGS`, applied when the building is loaded); a client in its own world reports them to the host, which adopts them. NPCs killed by a client were already the host's (hits forwarded). Door locks are not synced (towns lock every door each night). Engine table found by disassembly, reached through a byte pattern (feature off and logged if not found). |
| Bounties and crimes | 🧪 | Kenshi keeps bounties per character. A crime a client commits in the host's world is seen by the host's NPCs and lands on the host's copy of that character: the host sends the increase to the owner, which adds it to its real character (the game's own bounty, guards, prisons). Every player sends the bounty table of its characters when it changes, so the guards of the other worlds know who is wanted. Between players: `assault_hostility` (attacking a player's characters lowers relations, war below zero). |
| Host migration | 💡 | If the host leaves, the client with the lowest number becomes host: it keeps its game, the others reconnect to it (address known to all). NPCs become theirs. Simple on the network side; the reference world changes (the new host's). |

## Proposed order for what comes next
1. Still to see in game: merchant money, connection message (client side), task animations, nearby NPCs.
2. Diplomacy in the game's Factions screen (cosmetic).
3. Real Internet game (needs a second copy of the game).
