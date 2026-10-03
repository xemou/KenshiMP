# Checks dist\KenshiMP before a Workshop upload (run by package.bat; can also be run alone).
# Exit code 0 = ready, 1 = something to fix (listed).
param([string]$Dir = "$PSScriptRoot\..\dist\KenshiMP")
$problems = @()
$Dir = (Resolve-Path $Dir -ErrorAction SilentlyContinue)
if (-not $Dir) { Write-Host "check: package folder not found (run package.bat)"; exit 1 }

# 1. Files players need (see NOTICE.md / RELEASE_CHECKLIST.md)
$required = 'KenshiMP.dll', 'KenshiMP.mod', 'RE_Kenshi.json', 'kenshimp.cfg', 'KenshiMP_Loader.dll', 'KenshiLib.dll',
            'rva\RE_Kenshi\RVAs\Steam_1.0.65.br', 'Enable KenshiMP.bat', 'Disable KenshiMP.bat',
            'kenshimp_enable.ps1', 'kenshimp_disable.ps1', 'README.md', 'GUIDE.md', 'GUIDE_FR.md', 'RISKS.md', 'LICENSE', 'NOTICE.md'
foreach ($f in $required) { if (-not (Test-Path (Join-Path $Dir $f))) { $problems += "missing: $f" } }

# 2. Nothing from the game or RE_Kenshi is redistributed
$forbidden = 'kenshi_x64.exe', 'kenshi_GOG_x64.exe', 'RE_Kenshi.dll', 'PhysXLoader64.dll', 'PhysXCore64.dll', 'PhysXCooking64.dll',
             'cudart64_30_9.dll', 'Plugins_x64.cfg', 'settings.cfg', 'players.cfg'
Get-ChildItem $Dir -Recurse -File | ForEach-Object {
    if ($forbidden -contains $_.Name) { $problems += "must not be shipped: $($_.FullName.Substring($Dir.Path.Length + 1))" }
    if ($_.Extension -eq '.save' -or $_.Extension -eq '.pdb' -or $_.Extension -eq '.log' -or $_.Extension -eq '.rec') { $problems += "leftover file: $($_.Name)" }
}

# 3. Default settings: never ship a test or host/join setup
$cfgPath = Join-Path $Dir 'kenshimp.cfg'
if (Test-Path $cfgPath) {
    $cfg = @{}
    Get-Content $cfgPath | ForEach-Object {
        $l = $_.Trim()
        if ($l -and -not $l.StartsWith('#') -and $l.Contains('=')) { $k, $v = $l.Split('=', 2); $cfg[$k.Trim()] = $v.Trim() }
    }
    $expected = @{ 'mode' = 'off'; 'debug_keys' = '0'; 'load_sharing' = '0'; 'ghost_no_collide' = '0' }
    foreach ($k in $expected.Keys) {
        if ($cfg.ContainsKey($k) -and $cfg[$k] -ne $expected[$k]) { $problems += "kenshimp.cfg: $k=$($cfg[$k]) (must be $($expected[$k]))" }
    }
    foreach ($k in 'autotest', 'password', 'address') {
        if ($cfg.ContainsKey($k) -and $cfg[$k] -and $cfg[$k] -ne '0' -and $cfg[$k] -ne '127.0.0.1') { $problems += "kenshimp.cfg: $k=$($cfg[$k]) (personal / test value)" }
    }
}

# 4. RE_Kenshi.json loads the plugin
$json = Join-Path $Dir 'RE_Kenshi.json'
if ((Test-Path $json) -and -not ((Get-Content $json -Raw) -match '"KenshiMP\.dll"')) { $problems += "RE_Kenshi.json does not list KenshiMP.dll" }

# 5. Version: the DLL is newer than the sources it should come from
$dll = Join-Path $Dir 'KenshiMP.dll'
if (Test-Path $dll) {
    $newest = Get-ChildItem "$PSScriptRoot\..\plugin\*.cpp", "$PSScriptRoot\..\plugin\*.h", "$PSScriptRoot\..\core\*" -File |
              Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($newest -and $newest.LastWriteTime -gt (Get-Item $dll).LastWriteTime) { $problems += "KenshiMP.dll is older than $($newest.Name): rebuild" }
}
$proto = Select-String -Path "$PSScriptRoot\..\core\Protocol.h" -Pattern 'PROTOCOL_VERSION = (\d+)' | Select-Object -First 1
$version = if ($proto) { $proto.Matches[0].Groups[1].Value } else { '?' }

if ($problems.Count -eq 0) {
    Write-Host "check: package OK - KenshiMP v$version ($((Get-ChildItem $Dir -Recurse -File).Count) files)"
    Write-Host "       next: tag the commit (git tag v$version, git push origin v$version), then workshop\UPLOAD.md"
    exit 0
}
Write-Host "check: $($problems.Count) problem(s) in the package:"
$problems | ForEach-Object { Write-Host "  - $_" }
exit 1
