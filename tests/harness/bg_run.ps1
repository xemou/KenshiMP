# Background test run, part 2: the launcher is already open. No mouse/keyboard input is sent:
# the launcher's OK is pressed by window message, the mod presses CONTINUE (test save) and runs
# its autotest script, the bot plays the second player.
# A crash is detected at once (log line or the game's crash window): the game is closed, the
# crash report kept, the bot stopped and the user's state restored (exit code 9).
param([int]$botSeconds = 140, [string]$far = '70,100', [switch]$twoBots, [string]$fight = '52,62,72', [int]$shotAt = 0, [string]$nowar = '')
$d = Split-Path $MyInvocation.MyCommand.Path
$k = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
$log = "$k\RE_Kenshi_log.txt"
$runStart = Get-Date

function Test-Crash {
  $crashed = $false
  if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $runStart -and (Select-String -Path $log -Pattern 'Crash detected' -Quiet)) { $crashed = $true }
  if (-not $crashed) {
    $w = & "$d\win.ps1" titles 2>$null
    if ($w -match 'has crashed|a plant') { $crashed = $true }
  }
  if (-not $crashed) { return }
  "CRASH detected at {0:HH:mm:ss}" -f (Get-Date)
  Stop-Process -Name Kenshi_x64 -Force -ErrorAction SilentlyContinue
  Get-Process bot -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep 2
  New-Item -ItemType Directory -Force "$d\crashes" | Out-Null
  $stamp = Get-Date -Format 'HHmmss'
  if (Test-Path "$k\crashDump1.0.65_x64.zip") { Copy-Item "$k\crashDump1.0.65_x64.zip" "$d\crashes\crash_$stamp.zip" -Force }
  Copy-Item $log "$d\crashes\RE_Kenshi_log_$stamp.txt" -Force
  Get-Content $log -Tail 6
  & "$d\bg_restore.ps1"
  exit 9
}

& "$d\win.ps1" press '^OK$'
# wait for the world (the mod logs "world ready")
$t0 = Get-Date; $ready = $false; $gone = $null
while (((Get-Date) - $t0).TotalSeconds -lt 300) {
  Start-Sleep 3
  Test-Crash
  if (-not (Get-Process Kenshi_x64 -ErrorAction SilentlyContinue)) { if (-not $gone) { $gone = Get-Date } elseif (((Get-Date) - $gone).TotalSeconds -gt 30) { "Kenshi exited"; & "$d\bg_restore.ps1"; exit 2 } } else { $gone = $null }
  if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $t0 -and (Select-String -Path $log -Pattern 'world ready' -Quiet)) { $ready = $true; break }
}
if (-not $ready) { "world not ready after 300 s"; Get-Content $log -Tail 5; & "$d\bg_restore.ps1"; exit 3 }
"world ready after {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds
$tw = Get-Date
while (((Get-Date) - $tw).TotalSeconds -lt 40 -and -not (Select-String -Path $log -Pattern 'autotest: town override' -Quiet)) { Start-Sleep 2; Test-Crash }
$pk = Select-String -Path $log -Pattern 'backpack available: (.+?) \(' | Select-Object -First 1
$env:KMP_TEST_BACKPACK = if ($pk) { $pk.Matches[0].Groups[1].Value } else { '' }
$cb = Select-String -Path $log -Pattern 'crossbow available: (.+?) \(' | Select-Object -First 1
$env:KMP_TEST_CROSSBOW = ''   # the host's own crossbow reaches the ghost through the equipment sync
function Get-AutotestValue([string]$what) {
  $m = Select-String -Path $log -Pattern ("autotest: " + $what + " (.*)$") | Select-Object -Last 1
  if ($m) { $m.Matches[0].Groups[1].Value } else { '' }
}
$env:KMP_TEST_TOWNDOOR = Get-AutotestValue 'town door'
$env:KMP_TEST_UNIQUE = Get-AutotestValue 'unique'
$env:KMP_TEST_TOWN = Get-AutotestValue 'town override'
$env:KMP_TEST_FACTION = Get-AutotestValue 'faction'
"town targets: door '$($env:KMP_TEST_TOWNDOOR)' unique '$($env:KMP_TEST_UNIQUE)' town '$($env:KMP_TEST_TOWN)'"
$env:KMP_TEST_TASK = '80,95,27'; $env:KMP_TEST_FIGHT = $fight; $env:KMP_TEST_FLOOR = '96,110'; $env:KMP_TEST_SPOOF = '1'
$env:KMP_TEST_NOWAR = $nowar; $env:KMP_TEST_UNDO = '1'; $env:KMP_TEST_GEAR = '1'; $env:KMP_TEST_MONEY = '1'; $env:KMP_TEST_FAR = $far
$mods = & "$PSScriptRoot\modlist.ps1"
$env:KMP_BOT_PASSWORD = ((Get-Content "$env:LOCALAPPDATA\kenshi\KenshiMP\kenshimp.cfg" | Where-Object { $_ -match '^password=' }) -replace '^password=','').Trim()
Start-Process (Join-Path $PSScriptRoot '..\out\bot.exe') -ArgumentList @('127.0.0.1','47000',"$botSeconds",'6','-',"`"$mods`"") -WindowStyle Hidden -RedirectStandardOutput "$d\bot_bg.txt"
# second bot: same timeline, takes the same item at the same second (race)
foreach ($v in 'KMP_TEST_BACKPACK','KMP_TEST_CROSSBOW','KMP_TEST_TASK','KMP_TEST_FIGHT','KMP_TEST_FLOOR','KMP_TEST_SPOOF','KMP_TEST_UNDO','KMP_TEST_GEAR','KMP_TEST_MONEY','KMP_TEST_FAR','KMP_TEST_TOWNDOOR','KMP_TEST_UNIQUE','KMP_TEST_TOWN','KMP_TEST_FACTION') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
if ($twoBots) { $env:KMP_BOT_NAME = 'Bot2'; Start-Process (Join-Path $PSScriptRoot '..\out\bot.exe') -ArgumentList @('127.0.0.1','47000',"$botSeconds",'-6','-',"`"$mods`"") -WindowStyle Hidden -RedirectStandardOutput "$d\bot2_bg.txt"; Remove-Item Env:KMP_BOT_NAME; "second bot started" }
"bot started (backpack '$($env:KMP_TEST_BACKPACK)')"
$end = (Get-Date).AddSeconds($botSeconds + 10)
$botStart = Get-Date; $shot = $false
while ((Get-Date) -lt $end) {
  Start-Sleep 3; Test-Crash
  if ($shotAt -gt 0 -and -not $shot -and ((Get-Date) - $botStart).TotalSeconds -ge $shotAt) { $shot = $true; & "$d\wshot.ps1" "$d\bg_shot.png" | Out-Null }
}
"done"


