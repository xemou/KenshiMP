# KenshiMP — Multiplayer risk register

Legend: ✅ handled (and how it is verified) · 🧪 coded and tested offline, **to validate in game** · 🟡 partial / mitigated · ❌ not handled (planned fix)
"tested offline" = `tests\net_test.cpp` · "seen in game" = observed with the bot.

## 1. Network and connection
| # | Problem | Status | Safeguard |
|---|---|---|---|
| 1 | Host behind a router without port forwarding / CGNAT (4G, shared fibre) | 🟡 | Doc: forward the TCP port or use a gaming VPN (Radmin, ZeroTier, Tailscale). ❌ Public relay server: to do if needed. |
| 2 | Windows firewall blocks Kenshi | 🟡 | Doc (allow on first prompt); explicit error message on the client side. |
| 3 | Wrong IP / host not started | ✅ | Clear message; automatic reconnect every 10 s. |
| 4 | High latency, jitter, spikes | ✅ | Timestamps + interpolation + extrapolation + smoothing (simulation: ≤ 8.1 u/s for 6 real with +400 ms spikes, 0 frozen frames). |
| 5 | TCP head-of-line blocking on packet loss | 🟡 | Buffered rendering of 60-300 ms absorbs short stalls. ❌ Move positions to UDP (big task) if real Internet play requires it. |
| 6 | Slow connection / low host upload | ✅ | Stale states skipped beyond 512 KB queued; clean disconnect at 8 MB (tested offline). NPC LOD, no sending for idle NPCs. |
| 7 | A player's drop / crash | ✅ | Detected within 15 s (tested offline), ghosts cleaned up, auto reconnect. |
| 8 | Returning player gets a different number → factions/relations mixed up | ✅ | Same name = same slot during the session (tested offline). |
| 9 | Stranger joining the game | ✅ | `password=` option (explicit refusal, tested offline). |
| 10 | Different mod / protocol versions | ✅ | Refusal with a message (tested offline). |
| 11 | Different mods or Kenshi version | ✅ | Mod list + game version compared; refusal or warning (`strict_mods`, tested offline and seen in game). |
| 12 | More than 8 players | ✅ | "server full" refusal. |
| 13 | The host leaves | 🟡 | Clients are told and return to their local world. ❌ Host migration: not planned. |
| 14 | PC sleep / hibernation (clock jumps) | ✅ | Timeout then reconnect; clock sync with a sliding window. |
| 15 | Two games on the same port / same PC | ✅ | "port already used" message. |

## 2. Cheating, abuse, corrupt data
| # | Problem | Status | Safeguard |
|---|---|---|---|
| 16 | Corrupt / malicious packets | ✅ | Bounded decoders, fuzzing of 20,000 packets without a crash (tested offline). |
| 17 | NaN / off-map positions / absurd values | ✅ | All positions, rotations, stats, damage are validated (tested offline). |
| 18 | Impersonating another player's entities | ✅ | Every ID carries its owner; the host sets the sender. |
| 19 | Inflated damage | ✅ | Cap per hit (500 per type). |
| 20 | "Remote" melee damage (modified client) | ✅ | Hit refused if the attacker is more than 30 units from the victim. |
| 21 | Demolishing a friend's town | ✅ | Building damage / dismantling only between hostile players (relation < 0). |
| 22 | Stealing a host NPC with a fake recruitment | ✅ | The host checks that the client is within 60 units of the NPC. |
| 23 | Flooding a player with characters | ✅ | 400 ghosts max per player. |
| 24 | Recruiting another player's character | ✅ | Blocked. |
| 25 | Speedhack / teleporting your own characters | 🧪 | The host watches announced positions (sender's clock, so no false alarm from the network): speed > 80 u/s over 0.5 s or jump > 50 units → warning in the log and on screen (once a minute per player). Fast runner (45 u/s) not flagged (tested offline). No blocking: cooperative model. |
| 26 | Declaring war by mistake (key pressed in another app) | ✅ | F9/F10 ignored if Kenshi is not focused. |

## 3. World consistency
| # | Problem | Status | Safeguard |
|---|---|---|---|
| 27 | Everyone has their own save, worlds diverge | 🟡 | Host's world shared (NPCs, clock, weather, world states, town versions, broken town doors and destroyed town buildings - 🧪 `town_sync`; 🧪 town containers: a client's copy is replaced by the host's content when opened, transfers go through the host, protocol 18); advice: clients on a fresh/dedicated game. A town changed in a client's own save but not in the host's world stays changed on that client (towns are never reverted, as in the base game). A client saving while connected keeps the host's world states in its save. |
| 28 | Client damages their solo save | ✅ | Client saves redirected to `<name>_MP`. |
| 29 | Ghosts stored in a save | ✅ | Purged on every load. |
| 30 | Host reloads another save mid-session | ✅ | Detection, caches invalidated, purge, resync. Seen in game: loading a save with a player connected, no crash, ghost recreated. |
| 31 | Connecting during the title screen / a load | ✅ | World messages ignored then resync request. |
| 32 | Different time of day | ✅ | Host's clock imposed (seen in game). |
| 33 | Different weather | ✅ | The host broadcasts the weather of every active region; clients apply it and no longer roll their own (`weather_sync`). Seen in game: 2 regions sent by the host, "weather synced" on the client. |
| 34 | Pause by a single player | ✅ | Only the host pauses, for everyone. |
| 35 | Fast-forward | ✅ | Locked to x1. |
| 36 | Different NPCs for everyone | ✅ | Host NPCs replicated, client's local population disabled (seen in game on the host side). |
| 37 | Client far from the host (unsimulated zone) | ✅ | The host keeps clients' zones loaded. 🧪 Option `load_sharing=1`: beyond 3000 units the client simulates its surroundings itself and the host stops simulating that region (less CPU for the host); within 2000, back to the host's shared world. |
| 38 | Ghost created in an unloaded zone (create / destroy loop) | ✅ | Creation deferred while it is more than 2,500 units from our characters. |
| 39 | Items: inventories, chests, ground, loot | ✅ | Inventories, chests, NPCs and looting: seen in game. Ground items: an item dropped by another player appears on your side, picking it up removes it on its owner's side (seen in game). Worn backpacks: contents mirrored, items taken from them removed on the owner's side (seen in game). 🟡 Contents of a backpack on the ground not shared. |
| 40 | Trading between players / with a ghost merchant | 🧪 | Between players: TRADE / ACCEPT button, the game's trade window on both sides, validated transfers (seen in game). Host NPC inventories mirrored (seen in game). Merchant money (coded, tested offline): the host sends each NPC squad's money; when the trade window opens, the merchant's copy receives that amount; what it earns or pays during the trade is applied to the real merchant on the host. |
| 39b | Transfer refused (too far, someone was faster) | 🧪 | The owner sends back a cancellation: the taken item is removed, the given item is returned, the container copy goes back to its last known state (ground items included, the refused item is put back). Message to the player. |
| 41 | Carrying a body, prisoners, cages, slaves between players | ✅ | Carrying/caging another player's character: blocked. Looting is allowed (items validated by their owner). On a host NPC: it becomes yours. |
| 42 | One player's turrets shooting another | 🟡 | Turrets manned by a ghost: their shots go through the same filter as ghost characters (4 s window, see #54). |
| 43 | Production, farms, research | 🟡 | Simulated on the owner's side only (others see the buildings). |
| 44 | Overlapping constructions from two players | 🧪 | Plan refused and removed if within 3 units of another player's building (message). |
| 45 | Building interiors / floors | 🧪 | Floor sent with the position; teleport onto the right floor. |
| 49 | Name of a reused player slot | ✅ | Mirror faction renamed. |
| 50 | Dialogues with a ghost NPC | 🟡 | Played locally; recruitment = ownership transfer. |
| 51 | Bounties / crimes | 🧪 | Crimes seen by the host's NPCs are added to the real character's bounty (`MSG_BOUNTY_CRIME`); each player's bounty table is copied onto its characters in the other worlds (`MSG_BOUNTIES`). Faction relation drops caused by a crime stay in the world where it was seen. |
| 52 | Free-for-all war only | ✅ | F9/F10 target the player whose character is selected (engine selection list); chat `/war`, `/peace`, `/ally`. Seen in game: "You declared war on Bot". |

## 4. Combat and rendering
| # | Problem | Status | Safeguard |
|---|---|---|---|
| 53 | Double damage (the hit counts on both sides) | ✅ | The attacker computes, the victim's owner applies; ghost hits neutralised. Re-verified in game on 30/09 (blow-by-blow log: ghost hit on our character = 0 damage, our hit forwarded to its owner). |
| 54 | Ghost projectiles | 🧪 | Every shot by a ghost is counted against its target during its flight time (distance / shot speed, 0.8 to 4 s); an arrow that hits consumes one shot, one from a real archer arriving at the same time is no longer attributed to the ghost. A hit is judged only once. |
| 55 | Ghost that freezes / teleports | ✅ | Main cause found and fixed: outside the camera view, Kenshi simulates characters in "off-screen" mode (coarse updates); player ghosts are exempted. Plus: teleport only if the ghost is really lost (stuck or more than 150 units away), settling delay after a teleport, lead proportional to speed, speed matched to the owner's, precise clock, task/combat released. Measured in game on a recorded path replayed identically: extreme path (zigzag sprints at 70 u/s) 1980 → 5 teleports in 6.5 min, ghost frozen 62 % → 0 %; normal path frozen 32 % → 0.2 %. |
| 56 | Lag on sharp U-turns | ✅ | Rendering layer verified in game: the model is drawn on the smoothed path (applied position kept by the engine). |
| 57 | Two very close bodies get in each other's way | 🧪 | Experimental option `ghost_no_collide=1` (off by default): ghosts are no longer pushed by your characters. To try in game. |
| 58 | Gap between the displayed model and the body (click, selection) | 🟡 | Rendering layer limited to 6 units and only while moving. |
| 59 | Very different FPS between players | ✅ | Everything is based on real time, not frames. |

## 5. Performance and operation
| # | Problem | Status | Safeguard |
|---|---|---|---|
| 60 | Too many NPCs to send | ✅ | Interest radius 600, LOD 10/5/2 Hz, nothing for idle ones. |
| 61 | Memory leaks over long sessions | ✅ | Tables cleaned up. Measured in game: 10 min two-player session, +170 MB (Kenshi's world streaming), stable handles, no disconnect. |
| 62 | Flooded log | ✅ | Messages aggregated and limited; detailed diagnostics only with `debug_keys=1`. |
| 63 | Exclusive fullscreen (Alt+Tab pauses the game) | ✅ | Doc: borderless mode. |
| 64 | Kenshi / RE_Kenshi update | 🟡 | KenshiLib adapts; clock located dynamically; check imports (script) and logs after an update. |
| 65 | Other mods hooking the same functions | 🟡 | KenshiLib handles multiple hooks; incompatibilities possible, to test case by case. |
| 66 | Antivirus blocking the DLL | 🟡 | Doc: exception for the Kenshi folder. |
| 67 | Chat | ✅ | Enter opens the box (decided at key press: the Enter that confirms a game window, e.g. save, does not open it), Enter sends, Esc cancels; Kenshi's keys inactive while typing. Seen in game. |
| 69 | Complicated connection (IP, port, config file) | ✅ | Multiplayer window (title screen button / F4): Host/Join/Leave, local IPs shown, readable refusal reasons (missing / extra / reordered mods), config saved (seen in game: host, client, refusal, input). |
| 70 | Key conflict with other mods (CheatMenu uses F6) | ✅ | Configurable `lobby_key` (F4 by default). |
| 71 | The mod feels "modded" (English texts, separate windows) | ✅ | Texts in the game's language (seen in game: window in French), coloured chat history (seen in game), name/faction pre-filled (seen in game), MULTIPLAYER button under the title menu and the pause menu (🧪 pause menu). |
| 72 | You can't see where the other players are | ✅ | Their squads are added to the game's world map (same markers as squads), except at war (fog). Option `show_players_on_map`. Verified in game: shown, hidden on war declaration, shown again at peace. |
| 68 | Distributing the same version to everyone | 🟡 | Checked on connect; distribute the zipped `dist\KenshiMP` folder. |
| 46 | Severed limbs, robotic limbs, states (drunkenness, hunger) | ✅ | Seen in game: left arm severed on the owner → severed on their ghost. Prosthetics: seen in game. Hunger: sent with the stats and applied to the ghost (seen in the log). Kenshi has no drunkenness. |
| 47 | A client's captives removed by the disabling of local NPCs | 🧪 | Cleanup skips carried characters, slaves, and NPCs taken by the client. |
| 48 | Relations with world factions | ✅ | Table synced to the mirror faction. |
| 73 | Joining a running game far from the other players | 🧪 | Protocol 18: GO TO (multiplayer window) / `/goto` teleports the selected characters next to another player's last known position (refused in combat or at war; announced first so the host's speed check does not warn). Reminder to a client loaded more than 1500 units from the host. |
| 74 | Clock / weather wrong for a few seconds after joining | 🧪 | The host sends both at once when someone joins or reloads (was: up to 5 s). |
| 75 | Player number changing when the host restarts | 🧪 | `players.cfg` on the host keeps name -> number between sessions (tested offline: returning player gets its number back after a host restart). |
| 76 | Host save damaged by a multiplayer bug | 🧪 | `backup_saves=5`: each hosted game is copied (background thread) to `KenshiMP\backups\<save>_<date>` when it is ready; last 5 copies per save kept (copy and pruning tested offline). |

## Suggested priorities for what comes next
1. Validate the 🧪 additions in game: ground items, merchant money, speed alert (the bot can drop
   an item on the ground at 45 s and answer the pickup).
2. A real two-PC game over the Internet (VPN or port forwarding).
3. Backpack contents (#39). UDP or relay depending on Internet feedback (#1, #5).
4. Host migration (#13). World towns (#27): to see in game.
