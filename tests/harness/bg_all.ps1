# Muted background launch + test run (no mouse/keyboard input).
param([int]$botSeconds = 140, [string]$far = '70,100', [switch]$twoBots, [string]$fight = '52,62,72', [int]$shotAt = 0, [string]$nowar = '')
$d = Split-Path $MyInvocation.MyCommand.Path
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -match 'mute.ps1' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',"$d\mute.ps1" -WindowStyle Hidden
Start-Process "steam://rungameid/233860"
$ok = $false
for ($i = 0; $i -lt 30 -and -not $ok; $i++) {
  Start-Sleep 2
  $r = & "$d\win.ps1" list 2>$null
  if ($r -match "'OK' \[Button\]") { $ok = $true }
}
if (-not $ok) { "launcher not found"; exit 1 }
& "$d\bg_run.ps1" -botSeconds $botSeconds -far $far -twoBots:$twoBots -fight $fight -shotAt $shotAt -nowar $nowar

