# Autonomous test, game = CLIENT: the bot host turns its first NPC into a shopkeeper (protocol 20 squad
# trailer) standing at a shop building next to the client's start; the client logs what its trader ghost
# is and whether the shop shelves get stock.
$h = $PSScriptRoot
$out = Join-Path $PSScriptRoot 'out'; New-Item -ItemType Directory -Force $out | Out-Null
$k = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
$log = "$k\RE_Kenshi_log.txt"
$runStart = Get-Date
if (Get-Process Kenshi_x64 -ErrorAction SilentlyContinue) { "Kenshi is running: abort"; exit 1 }
& "$h\bg_prep.ps1" | Select-Object -Last 1
$cfg = "$env:LOCALAPPDATA\kenshi\KenshiMP\kenshimp.cfg"
$c = [IO.File]::ReadAllText($cfg) -replace '(?m)^mode=.*$', 'mode=join' -replace '(?m)^address=.*$', 'address=127.0.0.1'
$c += "`r`nautotest_goto_shop=1`r`n"; [IO.File]::WriteAllText($cfg, $c)
$pw = ((Get-Content $cfg | Where-Object { $_ -match '^password=' }) -replace '^password=','').Trim()

function Test-Crash {
  $crashed = (Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $runStart -and (Select-String -Path $log -Pattern 'Crash detected' -Quiet)
  if (-not $crashed) { $w = & "$h\win.ps1" titles 2>$null; if ($w -match 'has crashed') { $crashed = $true } }
  if (-not $crashed) { return }
  "CRASH detected at {0:HH:mm:ss}" -f (Get-Date)
  New-Item -ItemType Directory -Force "$out\crashes" | Out-Null
  Stop-Process -Name Kenshi_x64 -Force -ErrorAction SilentlyContinue; Get-Process bot -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
  Copy-Item $log "$out\crashes\trader_crash_log.txt" -Force
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

  $shop = Select-String -Path $log -Pattern 'autotest: shop (.*)$' | Select-Object -Last 1
  $shopv = if ($shop) { $shop.Matches[0].Groups[1].Value } else { '' }
  "shop building: '$shopv'"
  $env:KMP_BOT_PASSWORD = $pw; $env:KMP_TEST_SHARE = '1'; $env:KMP_TEST_LOAD_MS = '5000'; $env:KMP_TEST_HOSTPATH = '400@0'
  $env:KMP_TEST_TRADER_SQUAD = '5007324-Genesis.mod'; $env:KMP_TEST_BOXITEM = '67-CBT Leisure Objects and Furniture.mod'
  if ($shopv -match '^[^;]*;(.*)$') { $env:KMP_TEST_TRADER_AT = $Matches[1] }
  $mods = & "$PSScriptRoot\modlist.ps1"
  Start-Process (Join-Path $PSScriptRoot '..\out\bot.exe') -ArgumentList @('--host-replay', "$PSScriptRoot\data\npcs.rec", '47000', '230', "`"$mods`"") -WindowStyle Hidden -RedirectStandardOutput "$out\trader_bot.txt"
  "bot host started"
  $end = (Get-Date).AddSeconds(240)
  while ((Get-Date) -lt $end) { Start-Sleep 3; Test-Crash }
  Copy-Item $log "$out\trader_full_log.txt" -Force
}
finally {
  & "$h\bg_restore.ps1" | Select-Object -Last 2
}
"done"





