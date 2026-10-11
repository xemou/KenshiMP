# Changelog

Players on different KenshiMP versions cannot play together (the host refuses the connection), so every
Workshop update must be announced here and in the Workshop "Change Notes".
Format: one section per Workshop release, newest first. The protocol number is `PROTOCOL_VERSION`
in `core/Protocol.h`.

## [v19] - unreleased, protocol 19

- NPC inventories and trader money for clients: if the host's description of an NPC never reached a
  client (ghost not ready in time, message lost), the client now asks again (`MSG_INVENTORY_REQUEST`,
  up to 4 times, for NPCs within 60 units of its characters) and the host answers with the inventory,
  the squad money and the backpack. Code + network test only, to be validated in game (reviews: "NPCs
  have no items or money for anyone other than the host").

- Looks of ghosts (reviews: "characters look different every time the game reloads"): the appearance
  received from the owner now REPLACES the ghost's own random looks (it used to be merged, so any key
  the source character did not have kept a random value on the ghost).
- Empty towns (reviews: "the town was empty, my friend could not see the cells"): the host checks that
  the zone around each client in the shared world is really loaded. If it is not, and no host NPC is
  shown to that client, after 25 s the client simulates its own surroundings instead (as when far from
  the host) until the zone loads or it moves 2500 units away. Log lines start with `zone watch:`.
  Code + build only, to be validated in game.
- Coming back to the host's world after being far away (own world -> shared world): the client used to
  delete all its local NPCs at once while the host was still loading the zone, leaving the town empty
  for a while. Now the local people stay until the host's NPCs have arrived around the client (45 s at
  most), and the host loads the zones around a returning client immediately instead of within 3 s.
  Log line: `load sharing: keeping the local people until the host's arrive`.
- Back in our own world (far from the host): the local NPCs removed while the host's world was shown
  are brought back (template, faction, position, name, looks and worn/held gear; those within 1500
  units, in loaded zones, at most 10 minutes old, 12 per 100 ms), on top of what the zones create.
  Their inventory and money are not restored. Validated in game with the test bot (autotest census:
  own world 0 -> 51 local NPCs within 1500 units after the return; 4 of 55 failed to spawn).
  Log lines: `load sharing: N remembered NPC(s)`, `N NPC(s) brought back`.
- Test versions (autotest / debug_keys) log a census every 10 s (NPCs, host ghosts, zone, buildings).

## [v18] - first Workshop release (beta), protocol 18

Date: the day of the upload (write it here, and the Workshop item ID below, once published).
Workshop item ID: 3813505705 (https://steamcommunity.com/sharedfiles/filedetails/?id=3813505705)

Summary of what the first public build contains:

- Per-player squads, factions, towns; player-vs-player war / peace / alliance
- Live ghosts of other players: movement (interpolated, no teleport in normal conditions), appearance,
  equipment, per-limb health, KO / death, combat animations, tasks, prosthetics, hunger
- Damage authority model (attacker computes, victim's owner applies)
- Shared host world: NPCs and animals around each player, clock, host-controlled pause, weather,
  world states of unique NPCs and the town changes they cause, town doors broken and town buildings
  destroyed by any player (`town_sync`), bounties for crimes seen in the host's world (protocol 17),
  town containers with the host's content for everybody (protocol 18)
- Joining a running game (protocol 18): GO TO in the multiplayer window / `/goto` moves your squad next
  to another player's characters (reminder when a client arrives far from the host); the host sends its
  clock and weather at once to a player who joins or reloads; player numbers kept between host restarts
  (`players.cfg`)
- Buildings and towns replicated both ways
- Items: bodies, chests, worn backpacks, ground items, traders' stock and money, direct trade between players
- Multiplayer window (title screen / pause menu / F4), chat, players on the world map, English + French
- Other players' names always shown above their characters, in their chat colour (`player_names`)
- Robust networking: timeouts, auto-reconnect, version + mod list check, input validation
- Steam first: join through a Steam invitation from the title screen, then NEW GAME or CONTINUE in
  the Multiplayer window; address / port / password only under ADVANCED SETTINGS
- Runs without RE_Kenshi on the stock Steam game (loader + address table for 1.0.68): subscribe and
  tick the mod, nothing to run (the game starts the loader as an interface plugin, no game file
  changed); RE_Kenshi still supported, both kinds of players can play together
- Settings in `%LOCALAPPDATA%\kenshi\KenshiMP\kenshimp.cfg`; the packaged template is `mode=off`
- The version (= protocol number) is shown in the multiplayer window title and in the log

Known limits: see `GUIDE.md` section 7 and `RISKS.md` (items marked as not yet validated in game).

<!--
Template for next releases:

## [vN] - YYYY-MM-DD (protocol N)
### Added
### Changed
### Fixed
### Compatibility
- Saves: compatible / not compatible
- Config: new options / renamed options
-->
