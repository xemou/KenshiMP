# Client scenario with a REAL recorded host world (bar.rec: the bar of the test town, its barman, guards,
# residents and shop shelves, recorded from the game as host): the bot hosts and replays it, with a host player
# character standing in the bar, a chat line, a pause of the host's game, and the shelves' real content.
# The game joins in game, goes to the host's character (GO TO), and the checks look at what it sees there.
. "$PSScriptRoot\common.ps1"
Initialize-ClientTest
try {
  Start-GameAndWait
  Start-Sleep 10; Test-Crash
  $env:KMP_TEST_ZONE = '99998,99999'; $env:KMP_TEST_HOSTCHAR = '-51611;1634;1950'; $env:KMP_TEST_CHAT = '1'; $env:KMP_TEST_PAUSE = '60,85'
  Start-ReplayHost "$d\data\bar.rec" "$out\bar_bot.txt" 260
  "bot host started"
  Wait-Seconds 210
  Copy-Item $log "$out\bar_game.txt" -Force
}
finally { Get-Process bot -ErrorAction SilentlyContinue | Stop-Process -Force; & "$d\bg_restore.ps1" | Select-Object -Last 2 }
"done"
