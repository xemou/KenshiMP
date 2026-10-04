# Release checklist - KenshiMP on the Steam Workshop

Legend: [x] done (non-code, verified in the repo) · [ ] to do · (you) = only the author can do it

## 1. Legal / licensing
- [x] `LICENSE` = GPLv3 in repo and package
- [x] `NOTICE.md` lists KenshiLib (DLL redistributed for players without RE_Kenshi), RE_Kenshi, Boost, MyGUI, OGRE, Iced and what is not redistributed
- [x] `NOTICE.md` added to the package and to the "Package content" list
- [ ] Public GitHub repository reachable at the URL written in the descriptions (`github.com/xemou/KenshiMP`): make sure the repo is public and the `origin` is pushed
- [ ] Release commit tagged `v18` (`git tag v18`, `git push origin v18`) and the tag matches the uploaded DLL
- [ ] Workshop wording says "unofficial, not affiliated with Lo-Fi Games" (already in `NOTICE.md`; add to the Workshop page)
- [ ] No Kenshi / RE_Kenshi / PhysX file in `dist\KenshiMP\` (only the files listed in NOTICE.md; `KenshiLib.dll` is allowed, GPLv3)

## 2. Package (`dist\KenshiMP\`, built by `package.bat`)
- [x] `package.bat` ends with `tools\check_package.ps1` (files, default settings, nothing from the game): it must say "package OK"
- [ ] `KenshiMP.dll` built from the tagged commit (Release, /GL /LTCG)
- [ ] `KenshiMP.mod` present and opens in the Game Editor without "missing dependency" warnings
- [ ] `RE_Kenshi.json` = `{ "Plugins" : [ "KenshiMP.dll" ] }`
- [ ] `KenshiMP_Loader.dll`, `KenshiLib.dll`, `rva\RE_Kenshi\RVAs\Steam_1.0.65.br` and `gui\core\core_settings.xml` present; after the first upload, `workshop\item_id.txt` holds the item number and the XML lists the Workshop path (check_package says so)
- [ ] `kenshimp.cfg` has `mode=off` (never ships a host/join default)
- [ ] `README.md`, `GUIDE.md`, `RISKS.md`, `LICENSE`, `NOTICE.md` are current
- [ ] No leftover test settings (`debug_keys=0`, `load_sharing=0`, `ghost_no_collide=0`)
- [ ] Folder name and `.mod` name are both `KenshiMP`

## 3. Steam page
- [x] Description EN (`workshop/description_en.txt`) and FR (`workshop/description_fr.txt`)
- [x] Preview image `workshop/preview.png` (512×512, < 1 MB, real capture, no "Requires RE_Kenshi" anymore)
- [x] Description reviewed against the v18 features (8 players, GO TO, town chests, version in the window title)
- [x] "BETA / back up your saves" line at the top of the description
- [x] 4 extra screenshots in `workshop/screenshots/` (in game, English UI, cursor removed); preview remade from a real capture (two players)
- [x] Change notes ready (`workshop/changenotes.txt`, `CHANGELOG.md` v18); [ ] title and tags (you)
- [ ] Steam Workshop legal agreement accepted (you)
- [ ] Link to RE_Kenshi (Nexus) in the Requirements (optional on Steam, needed on GOG)
- [ ] Discussion tab: pinned "How to join / bug reports" post

## 4. Validation before going public (needs the game / a second machine)
Items still marked 🧪 in `RISKS.md` / `ROADMAP.md`:
- [ ] Trader money, client "Connecting..." message, task animations, near-NPC immunity
- [ ] Protocol additions of the latest builds (ground items, trade, backpacks)
- [ ] **A real session between two PCs** (Steam invitation, or a VPN): follow `workshop/TEST_AMI.md` (the main remaining unknown)
- [ ] Clean-install test without RE_Kenshi: fresh Steam Kenshi, mod subscribed and ticked (nothing else), MULTIPLAYER button on the title screen, host and join
- [ ] Clean-install test with RE_Kenshi + PhysX, host and join (and a mixed session: one player with, one without)
- [ ] Workshop-folder test (local `mods\KenshiMP` removed, plugin loaded from `steamapps\workshop\content\233860\<id>\`)
- [ ] Two saves tested: client saves go to `<name>_MP`, solo save untouched
- [ ] Reload a save while connected, and a client leaving / rejoining
- [ ] Compatibility spot-check with a few popular mods (CheatMenu uses F6; none should use F4)

## 5. Publishing steps (see `UPLOAD.md`)
- [ ] First upload as **Hidden / Friends only** (you)
- [ ] Subscribe from a test account, verify, then switch to **Public** (you)
- [ ] Note the Workshop item ID somewhere (`CHANGELOG.md` header or README)

## 6. After release
- [ ] Watch GitHub issues and Steam comments; ask for `RE_Kenshi_log.txt` and `KenshiMP_loader.log` with every bug report
- [ ] On each Kenshi / RE_Kenshi update: re-test, bump the protocol if needed, add a changelog entry; a new Steam executable needs a new address table (`tools/rva/make_table.ps1`) and its MD5 in `plugin/Loader.cpp`
- [ ] Update `GUIDE.md` §1 if the supported versions change
