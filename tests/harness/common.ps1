# Shared helpers of the in-game scenarios (dot-source it: . "$PSScriptRoot\common.ps1").
$script:d = $PSScriptRoot
$script:k = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
$script:log = "$k\RE_Kenshi_log.txt"
$script:out = Join-Path $PSScriptRoot 'out'; New-Item -ItemType Directory -Force $script:out | Out-Null
$script:runStart = Get-Date
$script:botExe = Join-Path $PSScriptRoot '..\out\bot.exe'

function Get-BotPassword { ((Get-Content "$env:LOCALAPPDATA\kenshi\KenshiMP\kenshimp.cfg" | Where-Object { $_ -match '^password=' }) -replace '^password=', '').Trim() }

# A crash (log line or the game's crash window): the game is closed, the report kept, the user's state restored.
function Test-Crash {
  $crashed = (Test-Path $script:log) -and (Get-Item $script:log).LastWriteTime -gt $script:runStart -and (Select-String -Path $script:log -Pattern 'Crash detected' -Quiet)
  if (-not $crashed) { $w = & "$script:d\win.ps1" titles 2>$null; if ($w -match 'has crashed|a plant') { $crashed = $true } }
  if (-not $crashed) { return }
  "CRASH detected at {0:HH:mm:ss}" -f (Get-Date)
  New-Item -ItemType Directory -Force "$script:d\crashes" | Out-Null
  Stop-Process -Name Kenshi_x64 -Force -ErrorAction SilentlyContinue; Get-Process bot -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
  Copy-Item $script:log "$script:d\crashes\crash_$(Get-Date -Format HHmmss).txt" -Force
  Get-Content $script:log -Tail 6
  & "$script:d\bg_restore.ps1"
  exit 9
}

# Test settings (backup of the user's), mode=join to 127.0.0.1.
function Initialize-ClientTest {
  & "$script:d\bg_prep.ps1" | Select-Object -Last 1
  $cfg = "$env:LOCALAPPDATA\kenshi\KenshiMP\kenshimp.cfg"
  $c = [IO.File]::ReadAllText($cfg) -replace '(?m)^mode=.*$', 'mode=join' -replace '(?m)^address=.*$', 'address=127.0.0.1'
  [IO.File]::WriteAllText($cfg, $c)
}

# Launches the game muted, presses the launcher's OK, waits for the test world (autotest new game).
function Start-GameAndWait {
  Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -match 'mute.ps1' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
  Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$script:d\mute.ps1" -WindowStyle Hidden
  Start-Process "steam://rungameid/233860"
  $ok = $false
  for ($i = 0; $i -lt 30 -and -not $ok; $i++) { Start-Sleep 2; if ((& "$script:d\win.ps1" list 2>$null) -match "'OK' \[Button\]") { $ok = $true } }
  if (-not $ok) { throw "launcher not found" }
  & "$script:d\win.ps1" press '^OK$'
  $t0 = Get-Date
  while (((Get-Date) - $t0).TotalSeconds -lt 300) {
    Start-Sleep 3; Test-Crash
    if ((Test-Path $script:log) -and (Get-Item $script:log).LastWriteTime -gt $t0 -and (Select-String -Path $script:log -Pattern 'autotest: town override' -Quiet)) { "world loaded after {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds; return }
  }
  throw "world not loaded after 300 s"
}

# Starts a replay-host bot. $mods: the mod list it announces; $password: the session password it asks for.
function Start-ReplayHost([string]$rec, [string]$outFile, [int]$seconds, [string]$mods = '', [string]$password = '') {
  if (-not $mods) { $mods = & "$script:d\modlist.ps1" }
  if (-not $password) { $password = Get-BotPassword }
  $env:KMP_BOT_PASSWORD = $password
  Start-Process $script:botExe -ArgumentList @('--host-replay', $rec, '47000', "$seconds", "`"$mods`"") -WindowStyle Hidden -RedirectStandardOutput $outFile
}

function Wait-Seconds([int]$n) { $end = (Get-Date).AddSeconds($n); while ((Get-Date) -lt $end) { Start-Sleep 3; Test-Crash } }
