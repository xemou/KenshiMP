# Window helpers that need no focus: list Kenshi's windows/controls, press a button by message.
# usage: win.ps1 list          -> top-level windows of Kenshi_x64 and their child controls
#        win.ps1 press <text>  -> BM_CLICK (posted) on the first Kenshi child button whose text matches
param([string]$cmd = 'list', [string]$text = '')
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static List<IntPtr> Tops(uint pid) { var r = new List<IntPtr>(); EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) r.Add(h); return true; }, IntPtr.Zero); return r; }
  public static List<IntPtr> Kids(IntPtr top) { var r = new List<IntPtr>(); EnumChildWindows(top, (h, l) => { r.Add(h); return true; }, IntPtr.Zero); return r; }
  public static string Text(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }
  public static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
}
"@
if ($cmd -eq 'titles') {
  foreach ($p in (Get-Process Kenshi_x64, WerFault -ErrorAction SilentlyContinue)) {
    foreach ($t in [W]::Tops([uint32]$p.Id)) { if ([W]::IsWindowVisible($t)) { [W]::Text($t) } }
  }
  exit 0
}
$procs = Get-Process Kenshi_x64 -ErrorAction SilentlyContinue
if (-not $procs) { "no Kenshi process"; exit 1 }
foreach ($p in $procs) {
  foreach ($t in [W]::Tops([uint32]$p.Id)) {
    if (-not [W]::IsWindowVisible($t)) { continue }
    if ($cmd -eq 'list') { "TOP '{0}' [{1}]" -f [W]::Text($t), [W]::Cls($t) }
    foreach ($c in [W]::Kids($t)) {
      $tx = [W]::Text($c)
      if ($cmd -eq 'list') { "   '{0}' [{1}]" -f $tx, [W]::Cls($c) }
      elseif ($cmd -eq 'press' -and $tx -match $text -and [W]::Cls($c) -match 'Button') {
        [W]::PostMessage($c, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # BM_CLICK
        "pressed '$tx'"; exit 0
      }
    }
  }
}
if ($cmd -eq 'press') { "button '$text' not found"; exit 2 }

