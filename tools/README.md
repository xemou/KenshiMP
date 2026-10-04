# Tools

## Playing without RE_Kenshi

`plugin/Loader.cpp` builds `KenshiMP_Loader.dll`. Nothing has to be run by the player: Kenshi adds
the `gui` folders of every enabled mod to the resources of its interface library (MyGUI), and the
package's `gui\core\core_settings.xml` (written by `make_gui_xml.ps1`) replaces the game's own file
of that name with the same two settings plus a MyGUI plugin list. MyGUI loads the loader from there
at startup and calls its `dllStartPlugin()`. The list holds the local path
(`mods/KenshiMP/KenshiMP_Loader.dll`) and, once `workshop\item_id.txt` exists, the Workshop path
(`../../workshop/content/233860/<id>/KenshiMP_Loader.dll`); MyGUI skips the one that does not exist.
Untick the mod in the launcher and nothing is loaded. The loader starts KenshiMP once only, whichever
way it is reached (it would also work from Ogre's `Plugins_x64.cfg`).
Limit: another mod shipping its own `gui\core\core_settings.xml` would hide ours (only one of the
two is read); RE_Kenshi is then the fallback.

The loader:
- does nothing when RE_Kenshi is installed (RE_Kenshi starts KenshiMP through `RE_Kenshi.json`);
- recognises the game executable by its MD5. The stock Steam executable (1.0.68,
  `8a03c256f0da1555d9cceb939b41530a`) is supported with the address table shipped with KenshiMP;
  1.0.65 executables use RE_Kenshi's tables; anything else is refused (logged in
  `KenshiMP_loader.log` in the game folder, the game starts normally);
- loads the `KenshiLib.dll` shipped with KenshiMP (the build installed by RE_Kenshi 0.3.5, GPLv3),
  gives it the table, then loads `KenshiMP.dll` and calls `startPlugin()`.

KenshiLib reads its table from `RE_Kenshi/RVAs/<platform>_<version>.br` in the current folder.
The loader sets the version to Steam 1.0.65 and the current folder to `rva\` while KenshiLib
starts, so the package keeps the 1.0.68 table as `rva\RE_Kenshi\RVAs\Steam_1.0.65.br`.
Players on 1.0.65 (RE_Kenshi) and on the stock 1.0.68 build report the same game version and can
play together.

## rva/: address table for the stock Steam build

`rva/Steam_1.0.68.br` was produced offline by `rva/make_table.ps1` (see its header) from RE_Kenshi's
`Steam_1.0.65.br` and the two executables. Result: 7572 of 7630 entries found (5309 paired
functions, 2251 verified offsets, 12 by references); every entry KenshiMP uses is found and was
checked (identical masked code, or the same references for the 3 globals). The 58 missing entries
are written as 0: KenshiMP never calls them.

After a game update, run it again with the new executable and check the entries KenshiMP needs.
