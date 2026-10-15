# KenshiMP dedicated server as a service (DEDICATED_SERVER_PLAN.md, phase 7).
#
# Starts Kenshi with the server settings (kenshimp.cfg: mode=server, see tools\server\server.cfg.example), presses the
# game launcher's OK button by itself, and starts the game again when it stops or crashes. The server's own log, accounts,
# bans and player records are in its data folder (server_data_dir, default %LOCALAPPDATA%\kenshi\KenshiMP\server).
#
#   powershell -ExecutionPolicy Bypass -File run_server.ps1 [-Kenshi "<Kenshi folder>"] [-MaxRestartsPerHour 6]
#
# Stop it with Ctrl+C here (the game keeps running: type 'stop' in the server console or put 'stop' in
# server_commands.txt to save and quit cleanly). On Linux, run the same Kenshi under Wine with a loop like this one.
param(
  [string]$Kenshi = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
  [int]$MaxRestartsPerHour = 6,
  [int]$MemoryLogMinutes = 10,
  [string]$DataDir = (Join-Path $env:LOCALAPPDATA 'kenshi\KenshiMP\server')   # the server's server_data_dir
)
$ErrorActionPreference = 'Continue'
$exe = Join-Path $Kenshi 'RE_Kenshi\Kenshi_x64.exe'
if (-not (Test-Path $exe)) { $exe = Join-Path $Kenshi 'kenshi_x64.exe' }
$logDir = $DataDir
$stopFlag = Join-Path $DataDir 'server_stopped.flag'   # written by the server's 'stop' command: no restart then
Remove-Item $stopFlag -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $logDir | Out-Null
$runLog = Join-Path $logDir 'run_server.log'
function Note([string]$t) { $l = "{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $t; $l; Add-Content $runLog $l }

Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public class KW {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  static string T(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }
  static string C(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
  // Presses the launcher's OK (BM_CLICK, no focus needed). True if a button was found.
  public static bool PressOk(uint pid) {
    bool done = false;
    EnumWindows((t, l) => { uint p; GetWindowThreadProcessId(t, out p);
      if (p == pid && IsWindowVisible(t)) EnumChildWindows(t, (c, l2) => { if (!done && T(c) == "OK" && C(c).Contains("Button")) { PostMessage(c, 0x00F5, IntPtr.Zero, IntPtr.Zero); done = true; } return true; }, IntPtr.Zero);
      return true; }, IntPtr.Zero);
    return done;
  }
  // A visible window of that process titled like a crash report.
  public static bool Crashed(uint pid) {
    bool found = false;
    EnumWindows((t, l) => { uint p; GetWindowThreadProcessId(t, out p); string s = T(t);
      if ((p == pid || s.Contains("Kenshi")) && IsWindowVisible(t) && (s.Contains("has crashed") || s.Contains("a plant"))) found = true; return true; }, IntPtr.Zero);
    return found;
  }
}
"@

$restarts = New-Object System.Collections.Generic.List[datetime]
Note "server runner started ($exe)"
while ($true) {
  $restarts.RemoveAll({ param($t) $t -lt (Get-Date).AddHours(-1) }) | Out-Null
  if ($restarts.Count -ge $MaxRestartsPerHour) { Note "$($restarts.Count) restarts in the last hour: waiting 15 min before trying again"; Start-Sleep 900; continue }
  $restarts.Add((Get-Date))
  $p = Start-Process $exe -ArgumentList '--norestart' -WorkingDirectory $Kenshi -PassThru
  Note "game started (pid $($p.Id))"
  for ($i = 0; $i -lt 60; $i++) { Start-Sleep 2; if ($p.HasExited) { break }; if ([KW]::PressOk([uint32]$p.Id)) { Start-Sleep 4; if (-not [KW]::PressOk([uint32]$p.Id)) { break } } }
  # RE_Kenshi may start the real game as another process: follow the newest Kenshi_x64
  Start-Sleep 10
  $game = Get-Process Kenshi_x64 -ErrorAction SilentlyContinue | Sort-Object StartTime -Descending | Select-Object -First 1
  if (-not $game) { Note "no game process after the launcher: retrying in 30 s"; Start-Sleep 30; continue }
  $startMem = 0; $lastMem = Get-Date
  while (-not $game.HasExited) {
    Start-Sleep 5
    if ([KW]::Crashed([uint32]$game.Id)) { Note "crash window seen: killing the game"; Stop-Process -Id $game.Id -Force; break }
    if (((Get-Date) - $lastMem).TotalMinutes -ge $MemoryLogMinutes) {
      $lastMem = Get-Date
      try { $game.Refresh(); $mb = [math]::Round($game.WorkingSet64 / 1MB); if (-not $startMem) { $startMem = $mb }
            Note ("memory {0} MB ({1:+0;-0}% since the first measure)" -f $mb, (100.0 * ($mb - $startMem) / [math]::Max(1, $startMem))) } catch {}
    }
  }
  if (Test-Path $stopFlag) { Remove-Item $stopFlag -ErrorAction SilentlyContinue; Note "server stopped by an admin: not restarted"; break }
  Note "game stopped (exit code $(try { $game.ExitCode } catch { '?' })): restarting in 10 s"
  Start-Sleep 10
}
