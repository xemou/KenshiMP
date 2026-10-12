# KenshiMP — co-op multiplayer for Kenshi

Source code of the KenshiMP mod (Steam Workshop). Each player owns their squad; other players' characters
appear as copies, and the host shares the world (NPCs, time, weather, towns, shops).

## Layout
- `core/` – engine-independent networking (host relays between clients, Steam P2P or TCP), message
  definitions (`Protocol.h`), sessions (`Session.h/.cpp`), movement interpolation (`Interp.h`).
  Plain C++ compatible with VS2010.
- `plugin/` – the Kenshi plugin (`KenshiMP.dll`) and its loader, built with the **VS2010 (v100)** toolset
  against KenshiLib.
- `tools/` – packaging and installation helpers.
- `kenshimp.cfg` – default settings (mode, address, port, name, password).

## Building
`package.bat` builds the plugin with the VC++ 2010 x64 compilers (extracted from the Windows SDK 7.1 ISO into
`deps\vc2010`), the KenshiLib headers from `deps\KenshiLib\Include`, KenshiLib.lib 0.5.1 and Boost 1.60, then
assembles `dist\KenshiMP\`. The plugin must be built with /GL + /LTCG.

## Installing
Copy `dist\KenshiMP\` into `[Kenshi]\mods\` (or subscribe on the Steam Workshop), enable KenshiMP in the
launcher. Nothing else to run.

## License
GPLv3 (see [LICENSE](LICENSE)), like KenshiLib which the plugin depends on. See [NOTICE.md](NOTICE.md).
