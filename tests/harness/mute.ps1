# Mutes Kenshi_x64's audio sessions (Windows mixer, this process only), every 2 s until it exits.
param([int]$maxSeconds = 1200)
Add-Type @"
using System; using System.Runtime.InteropServices;
[Guid("BCDE0395-E52F-467C-8E3D-C4579291692E"), ComImport] class MMDeviceEnumeratorCls {}
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator { int NotImpl1(); [PreserveSig] int GetDefaultAudioEndpoint(int flow, int role, out IMMDevice dev); }
[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice { [PreserveSig] int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o); }
[Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionManager2 { int NotImpl1(); int NotImpl2(); [PreserveSig] int GetSessionEnumerator(out IAudioSessionEnumerator e); }
[Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionEnumerator { [PreserveSig] int GetCount(out int n); [PreserveSig] int GetSession(int i, out IAudioSessionControl2 s); }
[Guid("bfb7ff88-7239-4fc9-8fa2-07c950be9c6d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionControl2 {
  int a1(); int a2(); int a3(); int a4(); int a5(); int a6(); int a7(); int a8(); int a9();
  int GetSessionIdentifier(out IntPtr s); int GetSessionInstanceIdentifier(out IntPtr s);
  [PreserveSig] int GetProcessId(out uint pid);
}
[Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface ISimpleAudioVolume { int SetMasterVolume(float v, ref Guid ctx); int GetMasterVolume(out float v); [PreserveSig] int SetMute(bool m, ref Guid ctx); }
public static class Mute {
  public static int Process(uint pid) {
    var en = (IMMDeviceEnumerator)(new MMDeviceEnumeratorCls());
    IMMDevice dev; en.GetDefaultAudioEndpoint(0, 1, out dev);
    Guid iid = typeof(IAudioSessionManager2).GUID; object o;
    dev.Activate(ref iid, 23, IntPtr.Zero, out o);
    var mgr = (IAudioSessionManager2)o; IAudioSessionEnumerator se; mgr.GetSessionEnumerator(out se);
    int n; se.GetCount(out n); int muted = 0; Guid ctx = Guid.Empty;
    for (int i = 0; i < n; i++) {
      IAudioSessionControl2 s; se.GetSession(i, out s);
      uint p; s.GetProcessId(out p);
      if (p == pid) { ((ISimpleAudioVolume)s).SetMute(true, ref ctx); muted++; }
    }
    return muted;
  }
}
"@
# Stops 60 s after the last Kenshi process is gone (the launcher hands over to the game process).
$t0 = Get-Date; $lastSeen = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $maxSeconds -and ((Get-Date) - $lastSeen).TotalSeconds -lt 60) {
  $ps = Get-Process Kenshi_x64 -ErrorAction SilentlyContinue
  if ($ps) { $lastSeen = Get-Date; foreach ($p in $ps) { try { [Mute]::Process([uint32]$p.Id) | Out-Null } catch {} } }
  Start-Sleep 2
}

