# Runs a scenario WITHOUT RE_Kenshi (stock Steam 1.0.68 executable + our loader): what most Steam players have.
# Temporarily: RE_Kenshi's line in Plugins_x64.cfg is commented out and the loader line added; the files of the
# package are installed in mods\KenshiMP. Everything is put back at the end (finally).
#   bg_nore.ps1 -Script host|client|title|share|trader
param([ValidateSet('host', 'client', 'title', 'share', 'trader')][string]$Script = 'host', [int]$botSeconds = 140)
$d = $PSScriptRoot
$k = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
$cfg = "$k\Plugins_x64.cfg"
if (Get-Process Kenshi_x64 -ErrorAction SilentlyContinue) { "Kenshi is running: abort"; exit 1 }
Copy-Item $cfg "$d\Plugins_x64.cfg.bak" -Force
try {
  # install the loader side of the package next to the installed mod
  $m = "$k\mods\KenshiMP"; $dist = "$PSScriptRoot\..\..\dist\KenshiMP"
  foreach ($f in 'KenshiMP.dll', 'KenshiMP_Loader.dll', 'KenshiLib.dll') { Copy-Item "$dist\$f" "$m\$f" -Force }
  New-Item -ItemType Directory -Force "$m\rva\RE_Kenshi\RVAs" | Out-Null
  Copy-Item "$dist\rva\RE_Kenshi\RVAs\Steam_1.0.65.br" "$m\rva\RE_Kenshi\RVAs\Steam_1.0.65.br" -Force
  $lines = Get-Content $cfg | ForEach-Object { if ($_ -match '^\s*Plugin=RE_Kenshi\s*$') { '#' + $_ } else { $_ } }
  $lines = @($lines) + 'Plugin=mods/KenshiMP/KenshiMP_Loader'
  Set-Content -Path $cfg -Value $lines -Encoding ASCII
  "Plugins_x64.cfg for the test:"; Get-Content $cfg | Where-Object { $_ -match 'Plugin' }
  if (Test-Path "$k\KenshiMP_loader.log") { Remove-Item "$k\KenshiMP_loader.log" -Force }
  switch ($Script) {
    'host'   { & "$d\bg_prep.ps1" | Select-Object -Last 1; try { & "$d\bg_all.ps1" -botSeconds $botSeconds } finally { & "$d\bg_restore.ps1" | Select-Object -Last 1 } }
    'client' { & "$d\bg_client.ps1" }
    'title'  { & "$d\bg_title.ps1" -Tag title_nore }
    'share'  { & "$d\bg_share.ps1" }
    'trader' { & "$d\bg_trader.ps1" }
  }
}
finally {
  Get-Process Kenshi_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep 2
  Copy-Item "$d\Plugins_x64.cfg.bak" $cfg -Force
  "Plugins_x64.cfg restored:"; Get-Content $cfg | Where-Object { $_ -match 'Plugin' }
  "--- loader log:"; if (Test-Path "$k\KenshiMP_loader.log") { Get-Content "$k\KenshiMP_loader.log" } else { "(none)" }
}
