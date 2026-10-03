# Removes KenshiMP's loader from Plugins_x64.cfg (the rest of the file is left as it is).
param([string]$Game, [switch]$Quiet)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$wsPattern = '\\steamapps\\workshop\\content\\233860\\([^\\]+)$'
if ($Game) { $game = $Game }
elseif ($here -match $wsPattern) { $game = Join-Path ($here -replace $wsPattern, '') "steamapps\common\Kenshi" }
else { $game = Split-Path -Parent (Split-Path -Parent $here) }
$cfg = Join-Path $game "Plugins_x64.cfg"
if (-not (Test-Path $cfg)) { Write-Host "Kenshi not found ($cfg)." -ForegroundColor Red; if (-not $Quiet) { Read-Host "Press Enter to close" }; exit 1 }
$lines = @(Get-Content $cfg)
$keep = @($lines | Where-Object { $_ -notmatch 'KenshiMP_Loader\s*$' })
if ($keep.Count -eq $lines.Count) { Write-Host "KenshiMP's loader was not enabled." -ForegroundColor Green }
else { Set-Content -Path $cfg -Value $keep -Encoding ASCII; Write-Host "KenshiMP's loader removed." -ForegroundColor Green }
if (-not $Quiet) { Read-Host "Press Enter to close" }
