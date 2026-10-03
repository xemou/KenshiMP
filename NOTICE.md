# Third-party notices

KenshiMP is free software, licensed under the **GNU General Public License v3** (see `LICENSE`).
Copyright (C) the KenshiMP authors.

KenshiMP is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by
Lo-Fi Games. *Kenshi* is a trademark / property of Lo-Fi Games.

## What the mod is built on

| Component | Role | License |
|---|---|---|
| [KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) (BFrizzleFoShizzle) | Headers and import library used to call and hook the game's engine; linked into `KenshiMP.dll` | GPLv3 |
| [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) (BFrizzleFoShizzle) | Plugin loader that runs `KenshiMP.dll` (**not redistributed**: every player installs it themselves) | GPLv3 |
| Boost 1.60 (headers / static libs used by KenshiLib's API) | Compile-time dependency | Boost Software License 1.0 |
| MyGUI | In-game UI toolkit (the game's own copy is used at run time) | MIT |
| OGRE | Rendering engine API used through KenshiLib (the game's own copy is used at run time) | MIT |

Because KenshiLib is GPLv3 and is linked into the plugin, the whole of KenshiMP is distributed under
GPLv3 and **its complete corresponding source code is published** at
<https://github.com/xemou/KenshiMP>. The published source must match the released DLL
(tag the commit used for each release).

## What is NOT included in the package

- Kenshi itself, its executables, assets or game data.
- RE_Kenshi, its downgraded executable, or the PhysX DLLs.
- NVIDIA PhysX System Software (a separate NVIDIA installer; users download it themselves).
- The Microsoft Visual C++ 2010 compilers used to build the plugin (build-time only, extracted from the
  Windows SDK 7.1 for local use; never shipped).

## Package content (what Steam players receive)

`KenshiMP.dll`, `KenshiMP.mod`, `RE_Kenshi.json`, `kenshimp.cfg` (default template),
`README.md`, `GUIDE.md`, `RISKS.md`, `LICENSE`, `NOTICE.md`.
