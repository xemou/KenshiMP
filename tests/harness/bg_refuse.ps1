# Refusals and recovery, seen from the client: a host with another mod list, a host with another password, then a
# correct host (the client keeps retrying by itself and must get in). The offline network test covers the same
# refusals on the wire; this checks what the game does with them.
. "$PSScriptRoot\common.ps1"
Initialize-ClientTest
try {
  Start-GameAndWait
  Start-Sleep 5; Test-Crash
  $env:KMP_TEST_ZONE = '99998,99999'
  $mods = & "$d\modlist.ps1"
  Start-ReplayHost "$d\data\npcs.rec" "$out\refuse_bot1.txt" 600 ($mods + ';Some Other Mod')
  "bot host with another mod list"
  Wait-Seconds 35
  Get-Process bot -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
  Start-ReplayHost "$d\data\npcs.rec" "$out\refuse_bot2.txt" 600 $mods 'not-the-password'
  "bot host with another password"
  Wait-Seconds 35
  Get-Process bot -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
  Start-ReplayHost "$d\data\npcs.rec" "$out\refuse_bot3.txt" 600
  "bot host with the right settings"
  Wait-Seconds 45
  Copy-Item $log "$out\refuse_game.txt" -Force
  (Get-Content "$out\refuse_bot3.txt") | Set-Content "$out\refuse_bot.txt"
}
finally { Get-Process bot -ErrorAction SilentlyContinue | Stop-Process -Force; & "$d\bg_restore.ps1" | Select-Object -Last 2 }
"done"
