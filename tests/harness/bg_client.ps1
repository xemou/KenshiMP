# Background test, client side: the game joins; the bot hosts (replaying a recorded NPC stream)
# and only starts once the game's world is loaded (so the game shows "Connecting to the host..."),
# then tells it "on your own" at 30 s and "shared world" at 60 s (load sharing, client side).
# Same safety as bg_run: muted, no input, crash detection, restore at the end.
param([string]$zone = '30,60', [string]$near = '')
$d = Split-Path $MyInvocation.MyCommand.Path
$k = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
$log = "$k\RE_Kenshi_log.txt"
$runStart = Get-Date
& "$d\bg_prep.ps1" | Select-Object -Last 1
$cfg = "$env:LOCALAPPDATA\kenshi\KenshiMP\kenshimp.cfg"
$c = [IO.File]::ReadAllText($cfg) -replace '(?m)^mode=.*$', 'mode=join' -replace '(?m)^address=.*$', 'address=127.0.0.1'
[IO.File]::WriteAllText($cfg, $c)

function Test-Crash {
  $crashed = (Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $runStart -and (Select-String -Path $log -Pattern 'Crash detected' -Quiet)
  if (-not $crashed) { $w = & "$d\win.ps1" titles 2>$null; if ($w -match 'has crashed|a plant') { $crashed = $true } }
  if (-not $crashed) { return }
  "CRASH detected at {0:HH:mm:ss}" -f (Get-Date)
  New-Item -ItemType Directory -Force "$d\crashes" | Out-Null
  $stamp = Get-Date -Format 'HHmmss'
  Stop-Process -Name Kenshi_x64 -Force -ErrorAction SilentlyContinue; Start-Sleep 2
  if (Test-Path "$k\crashDump1.0.65_x64.zip") { Copy-Item "$k\crashDump1.0.65_x64.zip" "$d\crashes\crash_$stamp.zip" -Force }
  Copy-Item $log "$d\crashes\RE_Kenshi_log_$stamp.txt" -Force
  Get-Content $log -Tail 6
  & "$d\bg_restore.ps1"
  exit 9
}

# launch (muted), press the launcher's OK
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -match 'mute.ps1' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',"$d\mute.ps1" -WindowStyle Hidden
Start-Process "steam://rungameid/233860"
$ok = $false
for ($i = 0; $i -lt 30 -and -not $ok; $i++) { Start-Sleep 2; if ((& "$d\win.ps1" list 2>$null) -match "'OK' \[Button\]") { $ok = $true } }
if (-not $ok) { "launcher not found"; & "$d\bg_restore.ps1"; exit 1 }
& "$d\win.ps1" press '^OK$'
# world loaded = the character editor's CONFIRM was pressed
$t0 = Get-Date; $loaded = $false
while (((Get-Date) - $t0).TotalSeconds -lt 300) {
  Start-Sleep 3; Test-Crash
  if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $t0 -and (Select-String -Path $log -Pattern 'autotest: town override' -Quiet)) { $loaded = $true; break }
}
if (-not $loaded) { "world not loaded after 300 s"; & "$d\bg_restore.ps1"; exit 3 }
"world loaded after {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds
Start-Sleep 15; Test-Crash   # the game tries to connect meanwhile (nobody hosts yet)
function Get-AutotestValue([string]$what) {
  $m = Select-String -Path $log -Pattern ("autotest: " + $what + " (.*)$") | Select-Object -Last 1
  if ($m) { $m.Matches[0].Groups[1].Value } else { '' }
}
$env:KMP_TEST_TOWNDOOR = Get-AutotestValue 'town door'
$env:KMP_TEST_UNIQUE = Get-AutotestValue 'unique'
$env:KMP_TEST_TOWN = Get-AutotestValue 'town override'
$env:KMP_TEST_FACTION = Get-AutotestValue 'faction'
"town targets: door '$($env:KMP_TEST_TOWNDOOR)' unique '$($env:KMP_TEST_UNIQUE)' town '$($env:KMP_TEST_TOWN)'"
$env:KMP_TEST_ZONE = $zone; $env:KMP_TEST_NEAR = $near
$mods = & "$PSScriptRoot\modlist.ps1"
$env:KMP_BOT_PASSWORD = ((Get-Content "$env:LOCALAPPDATA\kenshi\KenshiMP\kenshimp.cfg" | Where-Object { $_ -match '^password=' }) -replace '^password=','').Trim()
Start-Process (Join-Path $PSScriptRoot '..\out\bot.exe') -ArgumentList @('--host-replay', "$PSScriptRoot\data\npcs.rec", '47000', '110', "`"$mods`"") -WindowStyle Hidden -RedirectStandardOutput "$d\bot_host.txt"
"bot host started"
$end = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $end) { Start-Sleep 3; Test-Crash }
& "$d\bg_restore.ps1" | Select-Object -Last 1
"done"

