# Builds KenshiLib's address table for another Kenshi build (offline, the game is never run).
#
# KenshiLib (the library KenshiMP is built on) only knows the 1.0.65 executables that RE_Kenshi
# installs. This maps every entry of RE_Kenshi's 1.0.65 table onto another executable:
#  1. every function of both executables (from their .pdata exception tables) gets a signature:
#     its instructions with call/jump targets, RIP-relative displacements and 32-bit immediates
#     masked out; functions with the same unique signature are paired, then the functions between
#     two paired neighbours are paired in order when the counts agree;
#  2. addresses that paired functions reference the same way (globals, small functions) are paired
#     by majority vote;
#  3. remaining code entries: same offset from the nearest paired function, accepted only if the
#     24 masked bytes there are identical.
# Entries that cannot be found are written as 0 and listed (none of them is used by KenshiMP for
# Steam 1.0.68; check with the list printed at the end before using a table for another build).
#
# Needs Iced (MIT, x86 disassembler): download https://www.nuget.org/api/v2/package/Iced/1.21.0,
# unzip it, pass -Iced <folder>\lib\net45\Iced.dll.
param(
  [Parameter(Mandatory = $true)][string]$OldExe,     # 1.0.65 executable (RE_Kenshi\Kenshi_x64.exe)
  [Parameter(Mandatory = $true)][string]$OldTable,   # its table (RE_Kenshi\RVAs\Steam_1.0.65.br)
  [Parameter(Mandatory = $true)][string]$NewExe,     # the build to support (stock Kenshi_x64.exe)
  [Parameter(Mandatory = $true)][string]$OutTable,
  [Parameter(Mandatory = $true)][string]$Iced
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Add-Type -Path $Iced
Add-Type -Path "$here\FuncDiff.cs" -ReferencedAssemblies $Iced, 'System.Core'
$old = New-Object PeImage $OldExe
$new = New-Object PeImage $NewExe
"functions: old {0}, new {1}" -f $old.Funcs.Count, $new.Funcs.Count
$u = 0; $p = 0
$map = [FuncDiff]::Match($old, $new, [ref]$u, [ref]$p)
"pass 1: {0} unique signatures + {1} by position" -f $u, $p
$ends = New-Object 'System.Collections.Generic.Dictionary[uint32,uint32]'
foreach ($f in $old.Funcs) { $ends[$f[0]] = $f[1] }
$endsB = New-Object 'System.Collections.Generic.Dictionary[uint32,uint32]'
foreach ($f in $new.Funcs) { $endsB[$f[0]] = $f[1] }
$x = [FuncDiff]::Xrefs($old, $new, $map, $ends, $endsB)
"pass 2: {0} referenced addresses paired" -f $x.Count
$anchors = New-Object 'System.Collections.Generic.List[uint32]'
foreach ($key in $map.Keys) { $anchors.Add($key) }
$anchors.Sort()

$tbl = [IO.File]::ReadAllBytes($OldTable)
$n = $tbl.Length / 4
$out = New-Object byte[] ($tbl.Length)
$how = @{ func = 0; xref = 0; delta = 0; none = 0 }
$missing = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt $n; $i++) {
  $r = [BitConverter]::ToUInt32($tbl, $i * 4)
  $v = [uint32]0
  if ($map.ContainsKey($r)) { $v = $map[$r]; $how.func++ }
  elseif ($x.ContainsKey($r)) { $v = $x[$r]; $how.xref++ }
  elseif (-not $old.IsCode($r)) { $how.none++; $missing.Add(("{0} {1:X} data" -f $i, $r)) }
  else {
    $lo = 0; $hi = $anchors.Count - 1; $best = -1
    while ($lo -le $hi) { $mid = [int](($lo + $hi) / 2); if ($anchors[$mid] -le $r) { $best = $mid; $lo = $mid + 1 } else { $hi = $mid - 1 } }
    $ok = $false
    foreach ($cand in @($best, ($best + 1))) {
      if ($cand -lt 0 -or $cand -ge $anchors.Count) { continue }
      $a = $anchors[$cand]
      if ([Math]::Abs([int64]$r - [int64]$a) -gt 0x4000) { continue }
      $c = [uint32]([int64]$map[$a] + ([int64]$r - [int64]$a))
      $s1 = [FuncDiff]::Head($old, $r, 24); $s2 = [FuncDiff]::Head($new, $c, 24)
      if ($s1 -and $s1 -eq $s2) { $v = $c; $ok = $true; break }
    }
    if ($ok) { $how.delta++ } else { $how.none++; $missing.Add(("{0} {1:X}" -f $i, $r)) }
  }
  [Array]::Copy([BitConverter]::GetBytes([uint32]$v), 0, $out, $i * 4, 4)
}
[IO.File]::WriteAllBytes($OutTable, $out)
"table: {0} entries -> by function {1}, by reference {2}, by verified offset {3}, NOT FOUND {4}" -f $n, $how.func, $how.xref, $how.delta, $how.none
"not found (index, old address):"
$missing
