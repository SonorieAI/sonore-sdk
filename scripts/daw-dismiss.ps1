# SPDX-License-Identifier: Apache-2.0
# Sonore SDK -- answers the dialogs a fresh REAPER configuration puts in front
# of its own startup script, so scripts/daw-render.mjs can drive it unattended.
#
# An isolated -cfgfile profile has never chosen an audio device and holds no
# licence, so REAPER asks "select your audio device driver now?" (modal, and
# the startup script waits behind it) and, unlicensed, shows the evaluation
# notice. Both are plain Win32 dialogs owned by reaper.exe with the bare title
# "REAPER"; this posts BM_CLICK to the button that means "carry on" -- No to
# the device question, Still Evaluating / Continue / OK to a notice -- for as
# long as it is asked to watch. It never touches a window that is not
# REAPER's, and never one whose title is more than the word.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File daw-dismiss.ps1 <seconds>
param([int] $seconds = 60)

$sig = @'
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public class SonoreDawDismiss {
  public delegate bool CB(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(CB cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, CB cb, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  static string Text(IntPtr h) { var sb = new StringBuilder(512); GetWindowText(h, sb, 512); return sb.ToString(); }
  static string Cls(IntPtr h) { var sb = new StringBuilder(64); GetClassName(h, sb, 64); return sb.ToString(); }
  /// Dialogs titled exactly "REAPER" belonging to any of the given pids.
  public static List<IntPtr> Dialogs(HashSet<uint> pids) {
    var r = new List<IntPtr>();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (pids.Contains(p) && IsWindowVisible(h) && Text(h) == "REAPER") r.Add(h); return true; }, IntPtr.Zero);
    return r;
  }
  /// Click the button that lets REAPER carry on. Returns what was clicked, or "".
  public static string Answer(IntPtr dlg) {
    string clicked = "";
    string[] prefer = { "&No", "Still Evaluating", "Continue", "&Continue", "&OK", "OK", "Close", "&Close" };
    var buttons = new List<KeyValuePair<IntPtr, string>>();
    EnumChildWindows(dlg, (h, l) => { if (Cls(h) == "Button" && IsWindowEnabled(h)) buttons.Add(new KeyValuePair<IntPtr, string>(h, Text(h))); return true; }, IntPtr.Zero);
    foreach (var want in prefer)
      foreach (var b in buttons)
        if (clicked == "" && b.Value.StartsWith(want)) { PostMessage(b.Key, 0x00F5, IntPtr.Zero, IntPtr.Zero); clicked = b.Value; }
    return clicked;
  }
}
'@
Add-Type -TypeDefinition $sig

$deadline = (Get-Date).AddSeconds($seconds)
while ((Get-Date) -lt $deadline) {
  $pids = New-Object 'System.Collections.Generic.HashSet[uint32]'
  Get-Process reaper -ErrorAction SilentlyContinue | ForEach-Object { [void] $pids.Add([uint32] $_.Id) }
  if ($pids.Count -gt 0) {
    foreach ($dlg in [SonoreDawDismiss]::Dialogs($pids)) {
      $what = [SonoreDawDismiss]::Answer($dlg)
      if ($what -ne "") { Write-Output ("dismissed: " + $what) }
    }
  }
  Start-Sleep -Milliseconds 500
}
