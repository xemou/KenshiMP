# Mod compatibility

KenshiMP does not need other mods, and works with most of them as long as **every player has exactly the
same mod list, in the same order** (the host refuses a player whose list differs and tells them what is
missing, extra or in another order; `strict_mods=0` turns the refusal into a warning).

This page says what is known. "Tested" means: played in the game with the test bot as the other player,
with that mod active, without a crash or a visible problem in the logs. It does not mean every feature of
that mod was used.

## Why most mods work

KenshiMP never sends the game's own data. It sends **names** (the `stringID` of a character, item, faction,
race or building) and values, and each game looks the name up in its own data. So a mod that only adds data
(items, weapons, armour, races, factions, shops, buildings, retextures) works when both sides have it:

| What is shared | How | A mod that adds... | Works if both have it |
|---|---|---|---|
| Characters | template name + faction name + position | new characters, squads, factions | yes |
| Looks | the character's appearance data, key by key (the ghost's own random looks are replaced) | races, faces, hair, body parts | yes (not tested with a race mod yet) |
| Gear and inventories | item name + manufacturer + material + quantity | weapons, armour, clothes, resources | yes |
| Buildings, towns | building name + position, town name | new buildings, new towns, town replacers | yes |
| Prosthetics | item name | new limbs | yes |

If a name is unknown on one side (a mod missing there), the log says so (`unknown item 'x' (missing mod?)`,
`cannot spawn ghost ... template ...`) and that object is skipped instead of crashing.

## Tested

Active in the test game (`data\mods.cfg`) while joining, sharing the world, leaving it and coming back:

| Mod | Result |
|---|---|
| Genesis | works |
| shops have more items + | works |
| WeaponRetexMetalness | works |
| Slopeless | works |
| RecruitPrisoners | loaded without problem (its recruiting feature was not exercised) |
| 256 Squad Limit | works |
| Buildable Resources (Cheat), Build Stone Mines Everywhere | work |
| Dark UI | works |
| CheatMenu | works; it uses F6, KenshiMP uses F4 (`lobby_key`) |

## Not tested yet

| Mod / kind | Expectation | Notes |
|---|---|---|
| Race mods (for example VRoid Race Face+) | should work when both players have it | looks are copied key by key; please report what you see |
| AnarchyKenshi | unknown | it is a DLL plugin; not active in the tests |
| Other DLL plugins (RE_Kenshi plugins) | unknown | KenshiLib lets several plugins hook the same function, but two mods changing the same behaviour can still fight |
| Mods that change game speed or the clock | will conflict | KenshiMP locks the speed at x1 (everybody simulates at the same speed) |
| Mods that change AI, combat or squad behaviour | probably fine | ghosts are driven by the network, not by the AI (`ghost_ai=none`) |
| Mods that change the world map or move towns | works if the host and the clients have the same ones | towns are shared by name; clients far from the host simulate their own surroundings (`load_sharing`) |
| Huge mods (thousands of items or characters) | works, slower to join | the mod list is compared, then the game's data is used locally |

## What does not work, whatever the mods

- **Different mod lists or order**: refused (or warned with `strict_mods=0`, which can desynchronise).
- **Fast-forward**: locked to x1.
- **Host leaving**: no host migration; clients go back to their own world.

## Reporting a problem with a mod

If a mod does not work with KenshiMP, please open an issue on github.com/xemou/KenshiMP with:
1. the mod name (and Workshop link),
2. what happens (crash, missing character, wrong looks, items that vanish...),
3. the **/report** folder zipped (type `/report` in the chat, or BUG REPORT in the multiplayer window): it contains
   the logs, the mod list and a session summary,
4. whether it also happens with only KenshiMP and that mod.

The author reads them and tries to make the mod work.
