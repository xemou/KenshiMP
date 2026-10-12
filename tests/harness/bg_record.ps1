# Records a REAL host world around the bar of the test town: the game is the HOST (autotest new game); the bot
# joins as a client, stands in the bar (KMP_TEST_FARPOS) and asks for the shop shelves (KMP_TEST_OPENBOX).
# Everything the host sends about its NPCs (spawn, looks, gear, stats, inventories, states) and about the shelves is
# written to bar.rec, which the bot can then replay as a host for a real player (host-replay).
param([int]$botSeconds = 230)
$h = $PSScriptRoot
$out = Join-Path $PSScriptRoot 'out'; New-Item -ItemType Directory -Force $out | Out-Null
$k = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
$log = "$k\RE_Kenshi_log.txt"
$runStart = Get-Date
if (Get-Process Kenshi_x64 -ErrorAction SilentlyContinue) { "Kenshi is running: abort"; exit 1 }
& "$h\bg_prep.ps1" | Select-Object -Last 1
$cfg = "$env:LOCALAPPDATA\kenshi\KenshiMP\kenshimp.cfg"
$c = [IO.File]::ReadAllText($cfg) -replace '(?m)^mode=.*$', 'mode=host'
[IO.File]::WriteAllText($cfg, $c)
$pw = ((Get-Content $cfg | Where-Object { $_ -match '^password=' }) -replace '^password=','').Trim()

function Test-Crash {
  $crashed = (Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $runStart -and (Select-String -Path $log -Pattern 'Crash detected' -Quiet)
  if (-not $crashed) { $w = & "$h\win.ps1" titles 2>$null; if ($w -match 'has crashed') { $crashed = $true } }
  if (-not $crashed) { return }
  "CRASH detected at {0:HH:mm:ss}" -f (Get-Date)
  New-Item -ItemType Directory -Force "$out\crashes" | Out-Null
  Stop-Process -Name Kenshi_x64 -Force -ErrorAction SilentlyContinue; Get-Process bot -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
  Copy-Item $log "$out\crashes\record_crash_log.txt" -Force
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
  $t0 = Get-Date; $ready = $false
  while (((Get-Date) - $t0).TotalSeconds -lt 300) {
    Start-Sleep 3; Test-Crash
    if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $t0 -and (Select-String -Path $log -Pattern 'world ready' -Quiet)) { $ready = $true; break }
  }
  if (-not $ready) { "world not ready after 300 s"; throw "noready" }
  "world ready after {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds
  Start-Sleep 10; Test-Crash
  $env:KMP_BOT_PASSWORD = $pw
  $env:KMP_TEST_FAR = "6,$($botSeconds - 20)"
  $env:KMP_TEST_FARPOS = '-51611,1634,1950'
  $env:KMP_TEST_OPENBOX = '3469-otto.mod;-51611.29;1634.08;1971.00|3469-otto.mod;-51582;1634;1930'
  $mods = & "$PSScriptRoot\modlist.ps1"
  Start-Process (Join-Path $PSScriptRoot '..\out\bot.exe') -ArgumentList @('127.0.0.1','47000',"$botSeconds",'6',"$out\bar.rec","`"$mods`"") -WindowStyle Hidden -RedirectStandardOutput "$out\record_bot.txt"
  "bot started (recording to bar.rec)"
  $end = (Get-Date).AddSeconds($botSeconds + 10)
  while ((Get-Date) -lt $end) { Start-Sleep 3; Test-Crash }
  Copy-Item $log "$out\record_full_log.txt" -Force
}
finally {
  & "$h\bg_restore.ps1" | Select-Object -Last 2
}
"done"

