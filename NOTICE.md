# Third-party notices

KenshiMP is free software, licensed under the **GNU General Public License v3** (see `LICENSE`).
Copyright (C) the KenshiMP authors.

KenshiMP is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by
Lo-Fi Games. *Kenshi* is a trademark / property of Lo-Fi Games.

## What the mod is built on

| Component | Role | License |
|---|---|---|
| [KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) (BFrizzleFoShizzle) | Headers and import library used to call and hook the game's engine; linked into `KenshiMP.dll` | GPLv3 |
| KenshiLib.dll (the build shipped with RE_Kenshi 0.3.5) | **Redistributed unmodified** in the package, used by `KenshiMP_Loader.dll` when RE_Kenshi is not installed. Source: <https://github.com/BFrizzleFoShizzle/KenshiLib> | GPLv3 |
| [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) (BFrizzleFoShizzle) | Optional plugin loader that runs `KenshiMP.dll` (not redistributed). KenshiMP's address table for the stock Steam 1.0.68 executable (`rva/`) is derived from RE_Kenshi's 1.0.65 table by `tools/rva/make_table.ps1` | GPLv3 |
| Iced (0xd4d) | x86 disassembler used offline by `tools/rva/make_table.ps1` (not shipped) | MIT |
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

`KenshiMP.dll`, `KenshiMP_Loader.dll`, `KenshiLib.dll`, `rva/RE_Kenshi/RVAs/Steam_1.0.65.br` (the
stock Steam 1.0.68 table, under the name KenshiLib looks for), `Enable KenshiMP.bat`,
`Disable KenshiMP.bat`, `kenshimp_enable.ps1`, `kenshimp_disable.ps1`, `KenshiMP.mod`,
`RE_Kenshi.json`, `kenshimp.cfg` (default template),
`README.md`, `GUIDE.md`, `RISKS.md`, `LICENSE`, `NOTICE.md`.
