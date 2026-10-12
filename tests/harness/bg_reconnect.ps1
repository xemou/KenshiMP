# The host (bot) disappears in the middle of a session and comes back: the client must notice, retry by itself and
# resume (what happens when a host restarts or the connection drops).
. "$PSScriptRoot\common.ps1"
Initialize-ClientTest
try {
  Start-GameAndWait
  Start-Sleep 10; Test-Crash
  $env:KMP_TEST_ZONE = '99998,99999'
  Start-ReplayHost "$d\data\npcs.rec" "$out\reconnect_bot1.txt" 600
  "bot host started (1st time)"
  Wait-Seconds 60
  Get-Process bot -ErrorAction SilentlyContinue | Stop-Process -Force
  "bot host killed"
  Wait-Seconds 30
  Start-ReplayHost "$d\data\npcs.rec" "$out\reconnect_bot2.txt" 600
  "bot host started (2nd time)"
  Wait-Seconds 70
  Copy-Item $log "$out\reconnect_game.txt" -Force
  (Get-Content "$out\reconnect_bot1.txt"), '--- host restarted ---', (Get-Content "$out\reconnect_bot2.txt") | Set-Content "$out\reconnect_bot.txt"
}
finally { Get-Process bot -ErrorAction SilentlyContinue | Stop-Process -Force; & "$d\bg_restore.ps1" | Select-Object -Last 2 }
"done"
