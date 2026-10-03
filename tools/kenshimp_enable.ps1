# Enables KenshiMP on a game WITHOUT RE_Kenshi: adds KenshiMP's loader to the plugins the game's
# engine loads at startup (Plugins_x64.cfg, a copy of the original is kept as
# Plugins_x64.cfg.kenshimp-backup). Works from <Kenshi>\mods\KenshiMP and from the Steam Workshop
# folder (steamapps\workshop\content\233860\<item>): the game then loads KenshiMP straight from
# there, so Workshop updates need nothing else.
# With RE_Kenshi installed this is not needed (RE_Kenshi starts KenshiMP); the loader then does nothing.
param([string]$Game, [switch]$Quiet)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$wsPattern = '\\steamapps\\workshop\\content\\233860\\([^\\]+)$'
if ($Game) { $game = $Game }
elseif ($here -match $wsPattern) { $game = Join-Path ($here -replace $wsPattern, '') "steamapps\common\Kenshi" }
else { $game = Split-Path -Parent (Split-Path -Parent $here) }
$cfg = Join-Path $game "Plugins_x64.cfg"
if (-not (Test-Path $cfg)) {
  Write-Host "Kenshi not found ($cfg)." -ForegroundColor Red
  Write-Host "Put the KenshiMP folder in <Kenshi>\mods\, or run: powershell -File kenshimp_enable.ps1 -Game <Kenshi folder>"
  if (-not $Quiet) { Read-Host "Press Enter to close" }
  exit 1
}
# path of the loader relative to the game folder, without ".dll" (the engine adds it)
$gameFull = (Resolve-Path $game).Path.TrimEnd('\')
$hereFull = (Resolve-Path $here).Path.TrimEnd('\')
if ($hereFull.StartsWith($gameFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
  $rel = $hereFull.Substring($gameFull.Length + 1)
} elseif ($hereFull -match $wsPattern) {
  $rel = "..\..\workshop\content\233860\" + $Matches[1]
} else {
  Write-Host "KenshiMP must be in <Kenshi>\mods\ or in the Steam Workshop folder of Kenshi." -ForegroundColor Red
  if (-not $Quiet) { Read-Host "Press Enter to close" }
  exit 1
}
$line = "Plugin=" + ($rel -replace '\\', '/') + "/KenshiMP_Loader"
$backup = "$cfg.kenshimp-backup"
if (-not (Test-Path $backup)) { Copy-Item $cfg $backup }
$lines = @(Get-Content $cfg | Where-Object { $_ -notmatch 'KenshiMP_Loader\s*$' })   # one KenshiMP line only
$out = New-Object System.Collections.Generic.List[string]
$lastPlugin = -1
for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim().StartsWith("Plugin=")) { $lastPlugin = $i } }
for ($i = 0; $i -lt $lines.Count; $i++) { $out.Add($lines[$i]); if ($i -eq $lastPlugin) { $out.Add($line) } }
if ($lastPlugin -lt 0) { $out.Add($line) }
Set-Content -Path $cfg -Value @($out) -Encoding ASCII
Write-Host "KenshiMP enabled: $line" -ForegroundColor Green
if ((Get-Content $cfg) -match '^\s*Plugin=RE_Kenshi') { Write-Host "(RE_Kenshi is installed too: it starts KenshiMP, the loader stays idle.)" }
Write-Host "Also tick KenshiMP.mod in the Kenshi launcher (MODS tab). Your single-player saves are not affected."
if (-not $Quiet) { Read-Host "Press Enter to close" }
