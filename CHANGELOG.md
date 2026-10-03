# Changelog

Players on different KenshiMP versions cannot play together (the host refuses the connection), so every
Workshop update must be announced here and in the Workshop "Change Notes".
Format: one section per Workshop release, newest first. The protocol number is `PROTOCOL_VERSION`
in `core/Protocol.h`.

## [Unreleased] - first Workshop release (beta)

Summary of what the first public build contains (fill in the final protocol number and date at release):

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
- Runs without RE_Kenshi on the stock Steam game (loader + address table for 1.0.68,
  `Enable KenshiMP.bat`); RE_Kenshi still supported, both kinds of players can play together
- Settings in `%LOCALAPPDATA%\kenshi\KenshiMP\kenshimp.cfg`; the packaged template is `mode=off`

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
