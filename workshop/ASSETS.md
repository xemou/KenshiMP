# Visual assets (need the game running)

`preview.png` (512×512) already exists. The Workshop page also accepts extra screenshots / videos
(added from the item page on Steam, not from the Game Editor).

| File to produce | Content |
|---|---|
| screen_lobby | Multiplayer window: host / join, player list, ping, WAR / PEACE / ALLY / TRADE buttons |
| screen_ghosts | Two squads side by side in a town, other player's name tag visible |
| screen_map | World map showing another player's squad |
| screen_chat | Chat history with player colours and a join / war message |
| screen_trade | Game's trade window open between two players |
| screen_combat | Combat between two player factions |
| demo.gif / video (optional) | ~15 s: click Host, other player joins, ghost appears |

Tips: borderless 1920×1200, clean save, no cheat-menu window visible, English UI for the main shots
(language follows `settings.cfg`). Keep the files out of `dist/` (they are uploaded to the Steam page by hand,
not shipped in the mod folder). Suggested location: `workshop/screenshots/`.
