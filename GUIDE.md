# KenshiMP — Guide

Cooperative/competitive multiplayer mod for Kenshi: every player has their own squad, faction and
towns, and player factions can go to war with each other. The host owns the shared world
(NPCs, clock, pause).

## 1. Requirements (every player)
- Kenshi **Steam or GOG 1.0.65 / 1.0.68** (no modified/cracked copy: RE_Kenshi refuses to run).
- **RE_Kenshi 0.3.5**, installed with its installer.
- **NVIDIA PhysX System Software 9.10.0513** installed (otherwise "PhysX start failure" when
  starting a game), and Kenshi's PhysX DLLs copied next to `Kenshi\RE_Kenshi\Kenshi_x64.exe`
  (PhysXCore64, PhysXCooking64, PhysXDevice64, PhysXLoader64, physxcudart_20, cudart64_30_9,
  NxCharacter). Without them the game crashes when a game starts.
- **Exactly the same mod list**, in the same order (the host refuses otherwise, see
  `strict_mods`), and the **same KenshiMP version**.

## 2. Installation
1. Copy the `dist\KenshiMP` folder into `Kenshi\mods\`.
2. In the Kenshi launcher, Mods tab: tick **KenshiMP**.
3. Settings live in `%LOCALAPPDATA%\kenshi\KenshiMP\kenshimp.cfg` (created on first launch from the
   mod's copy; see §4). The Multiplayer window (F4) is enough in most cases.
4. Recommended: launcher → Video settings → **Borderless** (exclusive fullscreen pauses the game
   on Alt+Tab → your squad freezes for the other players).

## 3. Playing
**Easiest: the Multiplayer window.** A **MULTIPLAYER** button sits under the title screen menu and
under the pause menu (Esc), or press **F4** at any time. The mod speaks the game's language:
1. Fill in **Name** (your name) and **Faction** (the name of your faction as seen by others).
2. **Host**: click **Host**. The window shows this PC's IP addresses (LAN / VPN) to give to your
   friends, then start or load a game.
3. **Client**: enter the **Host address** (the host's IP), the **Port** and the **Password** if any,
   click **Join**, then start a game (a fresh game dedicated to multiplayer is recommended).
4. The window shows the status (connected, ping, player list) or the reason for a refusal:
   missing / extra mods / different order, wrong password, host unreachable…
5. **Leave** ends the session (no automatic reconnect). Your choices are saved in `kenshimp.cfg`:
   next launch, the connection is made again automatically.
**Through Steam (easiest over the Internet, Steam copies of Kenshi):** no IP address, no port to
open, no VPN. The host opens the Multiplayer window, presses **Host**, then **STEAM FRIENDS** and
**INVITE** next to a friend (or the friend uses **Join game** on the host's Steam profile). The
friend accepts the invitation and the game connects through Steam's network (direct when possible,
Steam's relays otherwise). GOG copies keep the IP connection below.

6. **Diplomacy and trading**: one line per player with **WAR / PEACE / ALLY / TRADE**.
   To trade, bring one of your characters next to one of theirs and click **TRADE**; the other
   player gets a message and clicks **ACCEPT** on your line. The game's trade window opens on
   both sides: drag items from one inventory to the other. Esc ends the trade.
   The same WAR / PEACE / ALLY / TRADE buttons also appear in the game's own Factions screen when
   another player's faction is selected.

Everything can also be set by hand in `kenshimp.cfg`:
- **Host**: `mode=host`, start a game (new or loaded). The TCP port (47000 by default) must be
  reachable: port forwarding on your router, or a gaming VPN (ZeroTier, Tailscale, Radmin VPN —
  the easiest). Allow Kenshi through the Windows firewall when asked.
- **Client**: `mode=join`, `address=<host IP>`, start a game. It connects by itself, and
  reconnects by itself if the connection drops.
- **F9**: declare war · **F10**: make peace — with the player whose character is selected,
  otherwise with all players.
- **Enter**: open the chat (Enter to send, Esc to cancel). The latest messages stay displayed above
  (each player's name in their colour) then fade out; opening the chat shows them again.
  Commands: `/players` (list), `/war <player|all>`, `/peace <player|all>`, `/ally <player|all>`
  (the start of the name is enough).
- **Items**: you can loot the body (KO/dead) of another player or an NPC, trade with the host's
  merchants and put in / take from other players' chests. The owner validates (item still there,
  taker within 30 units) and the item only ever exists once.
- You cannot carry or cage another player's character (looting them while KO is allowed; in
  towns, guards see it as theft unless you are at war with them); carrying a world NPC makes it
  yours (the host hands it over to you).
- You cannot build within 3 units of another player's building.
- Speed is locked to x1. Only the host can pause (for everyone).
- **Client saves**: they go to `<name>_MP`; your solo game is untouched. The host saves normally:
  their save holds the shared world.

## 4. `kenshimp.cfg` options
| Option | Default | Purpose |
|---|---|---|
| mode | host | off / host / join |
| address, port | 127.0.0.1, 47000 | host address (client), TCP port |
| name, faction | | displayed name, name of your faction as seen by others |
| relation | 0 | starting relation between players (-100 war … 100 allies) |
| ghost_ai | none | none = other players' characters only do what their owner does |
| sync_appearance / equipment / buildings | 1 | sync of appearance, equipment, towns |
| npc_sync | 1 | the host's world is shared (the client's local NPCs are disabled) |
| pause_sync | 1 | pause controlled by the host |
| strict_mods | 1 | refuse players whose mods differ |
| auto_reconnect | 1 | automatic client reconnect |
| weather_sync | 1 | the host's weather is imposed on everyone |
| lobby_key | F4 | Multiplayer window key (F1…F12; F6 is taken by the CheatMenu mod) |
| language | auto | mod language: auto (the game's), fr or en |
| show_players_on_map | 1 | other players' squads on the world map (except when at war) |
| ghost_no_collide | 0 | experimental: other players' characters are no longer pushed by your characters (fewer catch-up jumps in a crowd); not yet tried in game |
| load_sharing | 0 | (host, experimental) load sharing: a player more than 3000 units away from the host runs the world around them on their own PC and the host stops simulating that region; back within 2000 units, the host takes over again (a single shared world). When you meet up, the NPCs around the client are replaced by the host's |
| assault_hostility | 0 | like Kenshi's factions: a player whose characters hurt yours while you are not at war loses standing with you (-25 per assault, at most one every 3 s); below zero it is war |
| render_smoothing | 1 | other players' model drawn on the smoothed path |
| password | (empty) | game password (identical on host and clients) |
| debug_keys | 0 | testing only: F11 building, F8 clock +3 h, F3 camera on another player, F1 attack another player's first character, F2 take the first item of an open inventory, F5 reload the "kmptest" save; blow-by-blow log |

## 5. Troubleshooting
| Symptom | Cause / fix |
|---|---|
| "PhysX start failure" crash when starting a game | install PhysX 9.10.0513 + copy the PhysX DLLs (§1) |
| Log: "Incorrect address in KenshiLib::GetRealAddress" | DLL built without /GL /LTCG (use `package.bat`) |
| "connection … timed out / refused" | host not running, wrong IP, closed port (router/firewall) → gaming VPN |
| "Your mods must match the host's" | align the mod list (same order) or `strict_mods=0` on the host |
| "KenshiMP version mismatch" | install the same KenshiMP version everywhere |
| A player's squad is frozen | they Alt+Tabbed out of exclusive fullscreen → borderless mode |
| "timed out (no data for 15 s)" | connection lost; the client reconnects by itself |
| Where to read the logs | `Kenshi\RE_Kenshi_log.txt`, lines starting "KenshiMP:" |

## 5b. Smoothness (like a game server)
- Players send their state **20 times/s** (20 Hz tick), timestamped with their own clock.
- Each machine replays the others ~100-150 ms late (adapted to the measured jitter),
  **interpolating** between two states, **extrapolating** with the velocity if a packet is late,
  and **smoothing corrections** (no jumps). Validated offline: on a 40-190 ms connection with
  spikes up to +400 ms, apparent speed stays ≤ 8.1 u/s for 6 u/s real, with no frozen frame
  (the old method: jumps at 108 u/s, 84 % frozen frames).
- In game, the engine makes the ghost **walk/run** towards a point ahead of it on the path (real
  animations), and a small per-frame correction closes the remaining gap. Measured with the bot:
  mean gap ~1-1.6 units, 0-2 teleports per 30 s (versus 248 before).
- NPCs: rate depends on the distance to the nearest player (10 Hz < 80, 5 Hz < 250, 2 Hz beyond)
  and nothing is sent for an idle NPC (refreshed every 2 s).
- Measured bandwidth: ~2 KB/s per player with a small squad; expect ~20 KB/s for a squad of 10
  and ~20-40 KB/s of NPCs per client in a town (the host multiplies by the number of clients).
- **F7** shows ping, inbound/outbound rate, ghost count and skipped states (also in the log every
  30 s, with the ghost tracking quality).

## 6. What is synchronised
Player characters (position, speed, appearance, equipment, stats, health of each limb, blood,
KO/death, combat with animations, visual tasks: building, machines, beds, turrets, sitting…),
damage (computed by the attacker, applied by the victim's owner), player factions and war/peace,
relations with world factions, towns (buildings, construction, destruction, broken/locked doors,
door damage, dismantling), the host's world NPCs and animals around each player, clock, pause,
recruitment of an NPC by a client (it becomes that client's), the floor each character is on,
**inventories** (character bags, **worn backpacks**, players' chests, the host's merchants and
NPCs, items on the ground) with owner-validated transfers, **direct trading** between players,
**hunger**, **severed / crushed limbs / prosthetics**, **weather** of each region, **chat**.
Additions marked 🧪 in `RISKS.md` were coded and tested offline (protocol, build, imports) but
still need to be **validated in game**.

## 7. Known limits (not synchronised today)
- **Backpack lying on the ground**: the backpack is shared, but not its contents while it is on
  the floor (empty it first, or hand it over through a trade).
- **Two players looting the same item in the same second**: the host removes only one, the second
  looter keeps a copy (rare).
- **Merchant money**: synced with the real merchant on the host during the trade — new, to be
  confirmed in game.
- **Production** (farms, mines, research): simulated on the owner's side only.
- **Dialogues** with a ghost NPC: played locally on the client.
- **No host migration**: if the host leaves, clients go back to their local world.
- **Sharp U-turns**: the ghost may lag up to ~1 s while the engine turns it around, then catches
  up by sliding (teleport only beyond 20 units).
- **States** (drunkenness, hunger): not copied onto ghosts.
- **Two characters very close** (< 2 units) get in each other's way physically and tracking is
  less accurate.

## 8. Anticipated future risks and safeguards in place
| Risk | Safeguard |
|---|---|
| Player crash / network drop | detected within 15 s, their ghosts cleaned up, automatic reconnect |
| Slow player / saturated connection | stale positions are skipped; beyond 8 MB queued they are disconnected (no memory leak) |
| Corrupt data / NaN / absurd values / cheating | all positions, rotations, damage (capped), stats are validated; ghost cap per player |
| Different versions or mods | explicit refusal on connect |
| Host reloads another save | detection, engine pointers invalidated, purge and full resync |
| Ghosts left in a save | purged on every load |
| Client connected during the title screen / a load | world messages ignored then resync once the world is ready |
| Client damaging their solo save | saves redirected to `_MP` |
| Client pausing alone (desync) | pause imposed by the host |
| Client far from the host (unsimulated zone) | the host keeps the zones around clients loaded |
| Recruiting a ghost (duplicate) | NPC: ownership transferred to the client; a player's character: blocked |
| F9/F10 pressed in another application | ignored if Kenshi is not focused |
| Kenshi / RE_Kenshi update | KenshiLib adapts per version; the clock is located dynamically; check the logs after an update |
| Antivirus blocking the injected DLL | add an exception for the Kenshi folder |

## 9. Testing without a second PC
You need **one copy of Kenshi per player** (Steam Family Sharing does not allow playing the same
game at the same time, and two instances on one PC are refused by Steam). To test alone, the bot
`tests\out\bot.exe` plays the second player: it copies your character next to you (without a left
arm, to test limbs), mirrors your inventory, writes in the chat and answers your messages, gives
then takes back an item, declares war then makes peace. With `--host-replay` it plays the host and
replays a recorded world (NPCs, inventories, weather, clock).
- `tests\build_and_run.bat`: offline network tests (connection, relay, damage, timeouts, slow
  client, corrupt frames, mods/versions, data validation).
- `tests\build_bot.bat` then `tests\out\bot.exe 127.0.0.1 47000 600 4` while you host: a fake 2nd
  player that sends your squad back offset by 4 m (to test ghosts, combat, damage).
- `bot.exe 127.0.0.1 47000 170 4 stream.rec` records the host's NPC stream;
  `bot.exe --host-replay stream.rec 47000` replays it while hosting, with Kenshi in `mode=join`
  (to test the client side).
