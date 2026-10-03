# Publishing KenshiMP on the Steam Workshop

Everything here is done by the mod's author, with their own Steam account (the upload and the
Workshop legal agreement cannot be automated).

## Before the first upload
1. Build the package: `package.bat` → `dist\KenshiMP\` (DLL, `KenshiMP.mod`, `RE_Kenshi.json`,
   default `kenshimp.cfg` with `mode=off`, `LICENSE`, `README.md`, `GUIDE.md`, `RISKS.md`).
2. Copy `dist\KenshiMP` to `Kenshi\mods\KenshiMP` (the game editor uploads from the `mods` folder;
   the folder name must stay `KenshiMP`, same as `KenshiMP.mod`).
3. Check the source code on GitHub matches the uploaded DLL (GPL v3: the published source must be
   the one the binary was built from). Tag the commit, e.g. `v13` (= protocol version).

## Upload
1. Start Kenshi **from Steam** and choose **Game Editor** (the FCS) in the launcher.
2. Open the **KenshiMP** mod (as the active mod).
3. Click **Steam Workshop** (top left), then fill in:
   - Title: `KenshiMP — Co-op & PvP multiplayer`
   - Description: paste `workshop/description_en.txt` (Steam BBCode). The French text is in
     `workshop/description_fr.txt` (add it below the English one, or as a translation if the
     Workshop page offers it).
   - Preview image: `workshop/preview.png` (512×512, under 1 MB).
   - Visibility: **Hidden** or **Friends only** for the first upload, until it is tested.
4. Upload, then open the item's page in Steam: accept the **Steam Workshop legal agreement** if
   asked (otherwise the item stays hidden). Add the requirement links (RE_Kenshi on Nexus) and the
   GitHub link in the page's links if you like.

## Test the Workshop version (before making it public)
1. Move your local `Kenshi\mods\KenshiMP` folder out of `mods` (otherwise two copies with the same
   name are loaded), subscribe to the item, let Steam download it.
2. Enable KenshiMP in the launcher, start the game: `RE_Kenshi_log.txt` must show the KenshiMP
   lines (plugin loaded from the Workshop folder), and
   `%LOCALAPPDATA%\kenshi\KenshiMP\kenshimp.cfg` must exist.
3. Open the Multiplayer window (F4) and host: everything should work as with the local copy.
4. Then switch the visibility to **Public**.

## Updates
1. Rebuild (`package.bat`), copy `dist\KenshiMP` to `Kenshi\mods\KenshiMP`.
2. Game Editor → open KenshiMP → **Steam Workshop** → update the existing item, with a short change
   note (and the protocol version: players on different versions cannot play together, Steam
   updates everybody automatically).
3. Push the matching source to GitHub and tag it.
