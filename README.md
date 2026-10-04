# KenshiMP — co-op multiplayer plugin for Kenshi

Each player owns his squad, faction, buildings and towns (own simulation). Other players' entities
show up as ghosts. Damage is sent to the *owner* of the victim, who applies it. Host also runs the
shared world. Fast-forward is locked to x1.

## Layout
- `core/` – engine-independent networking (TCP star topology, host relays). VS2010-compatible C++.
  - `Protocol.h` message types & structs, `Session.h/.cpp` host/client, `Buffer.h` serialisation.
- `tests/` – `build_and_run.bat` builds & runs the loopback test (host + 2 clients) with any modern MSVC.
- `plugin/` – RE_Kenshi plugin (`KenshiMP.dll`), must be built with the **VS2010 (v100)** toolset.
- `kenshimp.cfg` – mode=host|join, address, port, name, faction.

## Status
- [x] Network core, tested (handshake, relay, targeted damage, disconnect, ping)
- [x] Plugin skeleton: config, main-loop hook, x1 speed lock, position broadcast of selected character
- [x] Build plugin with a portable VC++ 2010 x64 toolchain (`package.bat` -> `dist\KenshiMP\`)
- [x] Ghost squads: every player character is announced (EntitySpawn, template stringID) and
      streamed at 10 Hz; remote ones are spawned in a per-player local faction and walk/teleport
- [x] Damage routing: hits on a ghost (HealthPartStatus::applyDamage hook) are sent to the owner
- [x] Faction war/peace between players: F9 = war, F10 = peace, `relation=` in cfg for the default
- [x] Ghost AI suppression (AI::periodicUpdate skipped for ghosts, `ghost_ai=none|full`)
- [x] Appearance sync (appearance GameData copied key by key, then AppearanceBase::reload)
- [x] Equipment sync (equipped weapons + armour recreated from item/manufacturer/material ids)
- [x] Buildings/towns (createBuilding hook on player faction, construction progress, removal)
- [x] Risky engine calls wrapped in SEH (logged instead of crashing)
- [x] Tested in game (Steam 1.0.68 + RE_Kenshi 0.3.5) against `tests/bot.exe` (fake 2nd player):
      ghost spawn + appearance + gear, movement, war (F9), melee on a ghost routed to its owner,
      incoming damage applied, building replication both ways. No crash.
- [x] Complete ghosts (protocol v3): per-body-part health + blood, KO/death, stats/skills,
      walk/jog/run, real combat (same target, engine animations), whitelisted tasks
      (build, operate machines, turrets, beds, sit...)
- [x] Damage authority: attacker's machine computes, victim's owner applies; ghost blows on
      replicated characters play with zero damage (Character::hitByMeleeAttack), ghost
      projectiles filtered via GunClass::shoot; verified in game (no double damage)
- [x] Buildings: destroyed flag, door states (broken/locked), door hits and dismantling of
      ghost buildings routed to the owner
- [x] Stale ghosts from previous sessions purged from loaded saves (factions kenshimp_player_N)
- [x] World clock: host is authoritative; clock found at runtime from getTimeStamp_inGameHours
      (day int + sky hour float), verified in game
- [x] Alt-tab: only exclusive fullscreen pauses; borderless/windowed keeps running
- [x] World NPC sync (protocol v4): host replicates NPCs/animals within 600 units of each client
      (real faction, full ghost pipeline, delta-compressed states); clients' own population is
      switched off (squad spawns, town residents blocked; leftovers removed with GameWorld::destroy)
      — host side verified in game with the bot; client side partially verified (replay)
- [x] Robustness (protocol v5, offline-tested in tests/net_test.cpp): 15 s timeouts, backlog caps,
      version + mod-list handshake, input sanitisation, ghost caps, auto-reconnect, disconnect
      cleanup, world-reload detection (pointer caches dropped, purge, MSG_RESYNC), no world ops
      before a world is loaded
- [x] Gameplay safety: host-owned pause, client saves redirected to "<name>_MP", faction-standing
      table synced to mirror factions, NPC recruitment = ownership transfer (player chars blocked),
      host keeps zones loaded around remote players, hotkeys only when the game has focus
- [ ] Items/containers/trading, weather, production, carrying bodies, chat UI (see GUIDE.md §7)

See [GUIDE.md](GUIDE.md) for install, config, troubleshooting, known limits and anticipated risks, [RISKS.md](RISKS.md) for the multiplayer risk register and [ROADMAP.md](ROADMAP.md) for what is left to do.

## Known setup pitfalls (fixed on this machine)
- Plugin must be built with /GL + /LTCG (else KenshiLib::GetRealAddress asserts, game crashes).
- RE_Kenshi's downgraded exe lives in `Kenshi\RE_Kenshi\`: copy the PhysX DLLs
  (PhysXCore64, PhysXCooking64, PhysXDevice64, PhysXLoader64, physxcudart_20, cudart64_30_9,
  NxCharacter) next to it, and install NVIDIA PhysX System Software 9.10.0513, otherwise
  "PhysX start failure" when starting a game.

## Testing without a second PC
`tests\build_bot.bat`, then run `tests\out\bot.exe 127.0.0.1 47000 600 4` while hosting: the bot
joins as player 1 and mirrors your squad/buildings back 4 m away, logs every DAMAGE it receives
and sends a 5 blunt hit to your characters after 60 s.

## Building the plugin
`package.bat` builds with the VC++ 2010 x64 compilers extracted (msiexec /a, no install) from the
Windows SDK 7.1 ISO into `deps\vc2010`, KenshiLib headers from `deps\KenshiLib\Include`, libs from
`deps\KenshiLib_Examples_deps\KenshiLib\Libraries` (KenshiLib.lib v0.5.1) and Boost 1.60.

## Installing
1. Copy `dist\KenshiMP\` into `[Kenshi]\mods\` (or subscribe on the Steam Workshop).
2. Enable KenshiMP in the launcher's Mods tab. That is all: nothing to run, no game file changed.
   The game itself starts KenshiMP through the mod's `gui\core\core_settings.xml` (see
   [tools/README.md](tools/README.md)). With RE_Kenshi (`deps\re_release\RE_Kenshi_installer.exe`,
   needed on GOG) it is the same.

## Documentation
- [GUIDE.md](GUIDE.md) - install, usage, options, troubleshooting, limits
- [RISKS.md](RISKS.md) - multiplayer risk register
- [ROADMAP.md](ROADMAP.md) - roadmap

## License

GPLv3 (see [LICENSE](LICENSE)), like KenshiLib which the plugin depends on.
