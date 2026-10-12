# The mod list the bot must announce: the game's launcher list (data\mods.cfg) minus the mods that are not
# actually installed (a mod missing on disk is not loaded, and the host refuses a different list).
param([string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi")
$ws = "C:\Program Files (x86)\Steam\steamapps\workshop\content\233860"
$names = Get-Content "$Game\data\mods.cfg" | Where-Object { $_.Trim() -ne '' } | ForEach-Object { $_.Trim() }
$ok = foreach ($n in $names) {
  $base = $n -replace '\.mod$', ''
  $found = (Test-Path "$Game\mods\$base\$n") -or (Test-Path "$Game\data\$n") -or ((Test-Path $ws) -and (Get-ChildItem $ws -Recurse -Filter $n -ErrorAction SilentlyContinue | Select-Object -First 1))
  if ($found) { $base }
}
'game 1.0.65;base;Newwworld;Dialogue;rebirth;' + ($ok -join ';')

