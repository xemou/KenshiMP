# Release checklist - KenshiMP on the Steam Workshop

Legend: [x] done (non-code, verified in the repo) · [ ] to do · (you) = only the author can do it

## 1. Legal / licensing
- [x] `LICENSE` = GPLv3 in repo and package
- [x] `NOTICE.md` lists KenshiLib, RE_Kenshi, Boost, MyGUI, OGRE and what is not redistributed
- [ ] `NOTICE.md` added to the package (add one `copy` line to `package.bat`) and to the "Package content" list
- [ ] Public GitHub repository reachable at the URL written in the descriptions (`github.com/xemou/KenshiMP`): make sure the repo is public and the `origin` is pushed
- [ ] Release commit tagged (e.g. `v<protocol>`) and the tag matches the uploaded DLL
- [ ] Workshop wording says "unofficial, not affiliated with Lo-Fi Games" (already in `NOTICE.md`; add to the Workshop page)
- [ ] No Kenshi / RE_Kenshi / PhysX file in `dist\KenshiMP\` (only the 9 files in NOTICE.md)

## 2. Package (`dist\KenshiMP\`, built by `package.bat`)
- [ ] `KenshiMP.dll` built from the tagged commit (Release, /GL /LTCG)
- [ ] `KenshiMP.mod` present and opens in the Game Editor without "missing dependency" warnings
- [ ] `RE_Kenshi.json` = `{ "Plugins" : [ "KenshiMP.dll" ] }`
- [ ] `kenshimp.cfg` has `mode=off` (never ships a host/join default)
- [ ] `README.md`, `GUIDE.md`, `RISKS.md`, `LICENSE`, `NOTICE.md` are current
- [ ] No leftover test settings (`debug_keys=0`, `load_sharing=0`, `ghost_no_collide=0`)
- [ ] Folder name and `.mod` name are both `KenshiMP`

## 3. Steam page
- [x] Description EN (`workshop/description_en.txt`) and FR (`workshop/description_fr.txt`)
- [x] Preview image `workshop/preview.png` (512×512, < 1 MB)
- [ ] Description reviewed against the final features (player limit is 8 = `MAX_PLAYERS`; remove anything not validated)
- [ ] Add "BETA / back up your saves" line at the top of the description
- [ ] 4-6 extra screenshots (list in `ASSETS.md`)
- [ ] Item title, tags, change notes ready (`CHANGELOG.md`)
- [ ] Steam Workshop legal agreement accepted (you)
- [ ] Link to RE_Kenshi (Nexus) in the Requirements and in the item's links
- [ ] Discussion tab: pinned "How to join / bug reports" post

## 4. Validation before going public (needs the game / a second machine)
Items still marked 🧪 in `RISKS.md` / `ROADMAP.md`:
- [ ] Trader money, client "Connecting..." message, task animations, near-NPC immunity
- [ ] Protocol additions of the latest builds (ground items, trade, backpacks)
- [ ] **A real session between two PCs** over a VPN or a forwarded port (the main remaining unknown)
- [ ] Clean-install test: fresh Kenshi + RE_Kenshi + PhysX, mod subscribed from Steam, host and join
- [ ] Workshop-folder test (local `mods\KenshiMP` removed, plugin loaded from `steamapps\workshop\content\233860\<id>\`)
- [ ] Two saves tested: client saves go to `<name>_MP`, solo save untouched
- [ ] Reload a save while connected, and a client leaving / rejoining
- [ ] Compatibility spot-check with a few popular mods (CheatMenu uses F6; none should use F4)

## 5. Publishing steps (see `UPLOAD.md`)
- [ ] First upload as **Hidden / Friends only** (you)
- [ ] Subscribe from a test account, verify, then switch to **Public** (you)
- [ ] Note the Workshop item ID somewhere (`CHANGELOG.md` header or README)

## 6. After release
- [ ] Watch GitHub issues and Steam comments; ask for `RE_Kenshi_log.txt` with every bug report
- [ ] On each Kenshi / RE_Kenshi update: re-test, bump the protocol if needed, add a changelog entry
- [ ] Update `GUIDE.md` §1 if the supported versions change
