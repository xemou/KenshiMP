# Background test run, last part: put back the user's settings and saves exactly as backed up.
$d = Split-Path $MyInvocation.MyCommand.Path
$k = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
$saves = "$env:LOCALAPPDATA\kenshi\save"
$bk = "$d\bg_backup"
Get-Process bot -ErrorAction SilentlyContinue | Stop-Process -Force
Stop-Process -Name Kenshi_x64 -Force -ErrorAction SilentlyContinue; Start-Sleep 3
Copy-Item "$bk\kenshi.cfg" "$k\kenshi.cfg" -Force
Copy-Item "$bk\settings.cfg" "$k\settings.cfg" -Force
Copy-Item "$bk\kenshimp.cfg" "$k\mods\KenshiMP\kenshimp.cfg" -Force
# the user's own settings file: what it was, or (first run) a copy of their mod settings
$userCfg = "$env:LOCALAPPDATA\kenshi\KenshiMP\kenshimp.cfg"
if (Test-Path "$bk\user_kenshimp.cfg") { Copy-Item "$bk\user_kenshimp.cfg" $userCfg -Force }
else { Copy-Item "$bk\kenshimp.cfg" $userCfg -Force }
if (Test-Path "$saves\kmptest") { Remove-Item "$saves\kmptest" -Recurse -Force }
foreach ($s in Get-ChildItem "$bk\save" -Directory) {
  $dst = Join-Path $saves $s.Name
  $a = (Get-ChildItem $s.FullName -Recurse -File | Get-FileHash | ForEach-Object Hash) -join ''
  $b = if (Test-Path $dst) { (Get-ChildItem $dst -Recurse -File | Get-FileHash | ForEach-Object Hash) -join '' } else { '' }
  if ($a -ne $b) {
    if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
    Copy-Item $s.FullName $saves -Recurse
    "restored save $($s.Name)"
  }
}
"restore done"

