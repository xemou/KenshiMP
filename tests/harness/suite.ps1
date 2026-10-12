# Runs every in-game scenario one after the other (the game is launched muted, in a window, with no mouse or
# keyboard input; the user's settings and saves are backed up and restored each time) and verifies each one.
# The result is written to out\REPORT.md. Needs: Kenshi (Steam) closed, the package built and installed
# (package.bat + tools\install_local.ps1), tests\out\bot.exe built (tests\build_bot.bat). About 45 minutes.
#   suite.ps1                      everything
#   suite.ps1 -Only host,title     some scenarios
param([string[]]$Only = @(), [switch]$SkipNoRE)
$d = $PSScriptRoot
$out = Join-Path $d 'out'; New-Item -ItemType Directory -Force $out | Out-Null
$k = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
$gameLog = "$k\RE_Kenshi_log.txt"
$report = Join-Path $out 'REPORT.md'
"# In-game verification report`n`nGenerated $(Get-Date -Format 'yyyy-MM-dd HH:mm') - KenshiMP protocol $((Select-String -Path "$d\..\..\core\Protocol.h" -Pattern 'PROTOCOL_VERSION = (\d+)').Matches[0].Groups[1].Value)`n`n| Scenario | Id | Result | Check |`n|---|---|---|---|" | Set-Content $report -Encoding UTF8

function Stop-All { Get-Process Kenshi_x64, bot -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 3 }
function Save-Logs([string]$name, [string]$botFile) {
  Copy-Item $gameLog (Join-Path $out "${name}_game.txt") -Force -ErrorAction SilentlyContinue
  if ($botFile -and (Test-Path $botFile)) { Copy-Item $botFile (Join-Path $out "${name}_bot.txt") -Force }
}

# name, verify scenario, runner
$scenarios = @(
  @{ name = 'host';   verify = 'host';   run = { & "$d\bg_prep.ps1" | Select-Object -Last 1; try { & "$d\bg_all.ps1" -botSeconds 140 } finally { & "$d\bg_restore.ps1" | Select-Object -Last 1 } }; bot = "$d\bot_bg.txt" },
  @{ name = 'client'; verify = 'client'; run = { & "$d\bg_client.ps1" }; bot = "$d\bot_host.txt" },
  @{ name = 'title';  verify = 'title';  run = { & "$d\bg_title.ps1" -Tag title }; bot = "$out\title_bot.txt" },
  @{ name = 'share';  verify = 'share';  run = { & "$d\bg_share.ps1" }; bot = "$out\share_auto_bot.txt" },
  @{ name = 'trader'; verify = 'trader'; run = { & "$d\bg_trader.ps1" }; bot = "$out\trader_bot.txt" }
)
if (-not $SkipNoRE) {
  $scenarios += @{ name = 'nore_host';  verify = 'host';  run = { & "$d\bg_nore.ps1" -Script host }; bot = "$d\bot_bg.txt" }
  $scenarios += @{ name = 'nore_title'; verify = 'title'; run = { & "$d\bg_nore.ps1" -Script title }; bot = "$out\title_nore_bot.txt" }
}

$totalFail = 0
foreach ($s in $scenarios) {
  if ($Only.Count -and $Only -notcontains $s.name) { continue }
  Stop-All
  "=== $($s.name) ($(Get-Date -Format HH:mm:ss))"
  try { & $s.run | Out-Null } catch { "scenario $($s.name) aborted: $_" }
  Save-Logs $s.name $s.bot
  Stop-All
  & "$d\verify.ps1" -Scenario $s.verify -Game (Join-Path $out "$($s.name)_game.txt") -Bot (Join-Path $out "$($s.name)_bot.txt") -Report $report
  $totalFail += $LASTEXITCODE
}
"`nTotal FAIL: $totalFail  (report: $report)"
exit $totalFail
