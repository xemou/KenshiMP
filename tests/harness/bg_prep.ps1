# Background test run, part 1: back up everything the run may touch, then set the test state.
$d = Split-Path $MyInvocation.MyCommand.Path
$k = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
$saves = "$env:LOCALAPPDATA\kenshi\save"
$bk = "$d\bg_backup"
if (Get-Process Kenshi_x64 -ErrorAction SilentlyContinue) { "Kenshi is running: abort"; exit 1 }
New-Item -ItemType Directory -Force "$bk\save" | Out-Null
# saves (only the real ones; _currentN are the game's temp folders)
foreach ($s in Get-ChildItem $saves -Directory | Where-Object { $_.Name -notlike '_current*' }) {
  Copy-Item $s.FullName "$bk\save\" -Recurse -Force
}
Copy-Item "$k\kenshi.cfg" "$bk\kenshi.cfg" -Force
Copy-Item "$k\settings.cfg" "$bk\settings.cfg" -Force
Copy-Item "$k\mods\KenshiMP\kenshimp.cfg" "$bk\kenshimp.cfg" -Force
$userCfg = "$env:LOCALAPPDATA\kenshi\KenshiMP\kenshimp.cfg"
if (Test-Path $userCfg) { Copy-Item $userCfg "$bk\user_kenshimp.cfg" -Force }
"backed up: " + ((Get-ChildItem "$bk\save").Name -join ', ')

# windowed 1280x720 with a border
$c = [IO.File]::ReadAllText("$k\kenshi.cfg")
$c = $c -replace 'Border=None', 'Border=Fixed' -replace 'Video Mode=[^\r\n]*', 'Video Mode=1280 x 720 @ 32-bit colour'
[IO.File]::WriteAllText("$k\kenshi.cfg", $c)
# test save = copy of X1
if (Test-Path "$saves\kmptest") { Remove-Item "$saves\kmptest" -Recurse -Force }
Copy-Item "$saves\X1" "$saves\kmptest" -Recurse
# CONTINUE = the test save; the mod's test settings in the active settings file
$s = [IO.File]::ReadAllText("$k\settings.cfg")
$s = if ($s -match 'continue=') { $s -replace 'continue=[^
]*', 'continue=kmptest' } else { $s + "`r`ncontinue=kmptest`r`n" }
[IO.File]::WriteAllText("$k\settings.cfg", $s)
$active = if (Test-Path $userCfg) { $userCfg } else { "$k\mods\KenshiMP\kenshimp.cfg" }
$c = [IO.File]::ReadAllText($active).Replace("debug_keys=0", "debug_keys=1")
$c = $c -replace '(?m)^mode=.*$', 'mode=host'   # host scenarios; client scenarios switch to join afterwards
$c = ($c -split "`r?`n" | Where-Object { $_ -notmatch '^(autotest|autotest_load|load_sharing|ghost_no_collide)=' }) -join "`r`n"
$c += "`r`nautotest=1`r`nautotest_load=new`r`nload_sharing=1`r`n"
[IO.File]::WriteAllText($active, $c)
"test settings in $active"
"ready"



