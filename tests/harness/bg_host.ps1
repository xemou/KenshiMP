# Autonomous test, game = HOST (autotest game, test save), bot = client. Collects the host's logs
# (trader diagnostics, NPC census). Same safety as bg_share: muted, no input, crash detection, restore.
param([int]$botSeconds = 100)
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
  Copy-Item $log "$out\crashes\host_crash_log.txt" -Force
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
  $mods = & "$PSScriptRoot\modlist.ps1"
  Start-Process (Join-Path $PSScriptRoot '..\out\bot.exe') -ArgumentList @('127.0.0.1','47000',"$botSeconds",'6','-',"`"$mods`"") -WindowStyle Hidden -RedirectStandardOutput "$out\host_bot.txt"
  "bot started"
  $end = (Get-Date).AddSeconds($botSeconds + 10)
  while ((Get-Date) -lt $end) { Start-Sleep 3; Test-Crash }
  Copy-Item $log "$out\host_full_log.txt" -Force
}
finally {
  & "$h\bg_restore.ps1" | Select-Object -Last 2
}
"done"

