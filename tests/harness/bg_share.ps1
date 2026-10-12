# Autonomous test: the game (client, autotest new game) joins the bot host; the bot "walks" its host
# position so the client goes own world -> shared world -> own world -> shared world.
# Helpers (prep / restore / win / mute) come from the earlier harness; this script only drives.
$h = $PSScriptRoot
$out = Join-Path $PSScriptRoot 'out'; New-Item -ItemType Directory -Force $out | Out-Null
$k = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
$log = "$k\RE_Kenshi_log.txt"
$runStart = Get-Date
if (Get-Process Kenshi_x64 -ErrorAction SilentlyContinue) { "Kenshi is running: abort"; exit 1 }
& "$h\bg_prep.ps1" | Select-Object -Last 1
$cfg = "$env:LOCALAPPDATA\kenshi\KenshiMP\kenshimp.cfg"
$c = [IO.File]::ReadAllText($cfg) -replace '(?m)^mode=.*$', 'mode=join' -replace '(?m)^address=.*$', 'address=127.0.0.1'
[IO.File]::WriteAllText($cfg, $c)
$pw = ((Get-Content $cfg | Where-Object { $_ -match '^password=' }) -replace '^password=','').Trim()

function Test-Crash {
  $crashed = (Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $runStart -and (Select-String -Path $log -Pattern 'Crash detected' -Quiet)
  if (-not $crashed) { $w = & "$h\win.ps1" titles 2>$null; if ($w -match 'has crashed') { $crashed = $true } }
  if (-not $crashed) { return }
  "CRASH detected at {0:HH:mm:ss}" -f (Get-Date)
  New-Item -ItemType Directory -Force "$out\crashes" | Out-Null
  Stop-Process -Name Kenshi_x64 -Force -ErrorAction SilentlyContinue; Start-Sleep 2
  Copy-Item $log "$out\crashes\RE_Kenshi_log_crash.txt" -Force
  Get-Content $log -Tail 6
  & "$h\bg_restore.ps1"
  exit 9
}

try {
  Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -match 'mute.ps1' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
  Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',"$h\mute.ps1" -WindowStyle Hidden
  Start-Process "steam://rungameid/233860"
  $ok = $false
  for ($i = 0; $i -lt 30 -and -not $ok; $i++) { Start-Sleep 2; if ((& "$h\win.ps1" list 2>$null) -match "'OK' \[Button\]") { $ok = $true } }
  if (-not $ok) { "launcher not found"; throw "launcher" }
  & "$h\win.ps1" press '^OK$'
  $t0 = Get-Date; $loaded = $false
  while (((Get-Date) - $t0).TotalSeconds -lt 300) {
    Start-Sleep 3; Test-Crash
    if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $t0 -and (Select-String -Path $log -Pattern 'autotest: town override' -Quiet)) { $loaded = $true; break }
  }
  if (-not $loaded) { "world not loaded after 300 s"; throw "noload" }
  "world loaded after {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds
  Start-Sleep 10; Test-Crash

  $env:KMP_BOT_PASSWORD = $pw; $env:KMP_TEST_SHARE = '1'; $env:KMP_TEST_LOAD_MS = '12000'
  $env:KMP_TEST_HOSTPATH = '3500@0,400@45,3500@130,400@180'
  $mods = & "$PSScriptRoot\modlist.ps1"
  Start-Process (Join-Path $PSScriptRoot '..\out\bot.exe') -ArgumentList @('--host-replay', "$PSScriptRoot\data\npcs.rec", '47000', '260', "`"$mods`"") -WindowStyle Hidden -RedirectStandardOutput "$out\share_auto_bot.txt"
  "bot host started"
  $end = (Get-Date).AddSeconds(270)
  while ((Get-Date) -lt $end) { Start-Sleep 3; Test-Crash }
  Select-String -Path $log -Pattern 'KenshiMP' | Where-Object { $_.Line -match 'load sharing|world sync|ghost|session|connected|zone watch|never received|disconnected|autotest' -and $_.Line -notmatch 'Ping ' } | ForEach-Object { $_.Line } | Set-Content "$out\share_auto_game.txt"
  Copy-Item $log "$out\share_auto_full_log.txt" -Force
  foreach ($f in 'kenshi.log', 'save.log', 'KenshiMP_loader.log', 'Havok.log') { try { if (Test-Path "$k\$f") { Copy-Item "$k\$f" "$out\share_auto_$f" -Force -ErrorAction Stop } } catch { "could not copy $f" } }
  Select-String -Path $log -Pattern 'census|building census' | ForEach-Object { $_.Line } | Set-Content "$out\share_auto_census.txt"
}
finally {
  & "$h\bg_restore.ps1" | Select-Object -Last 2
}
"done"

