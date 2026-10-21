# SPDX-License-Identifier: Apache-2.0
# Sonore SDK -- the eyes of scripts/reaper-host-test.mjs while REAPER runs.
#
# Every top-level window the isolated REAPER opens is written to a log, so a
# plugin that throws up an error box, or a REAPER that stops to ask
# something, is in the test report instead of in a window nobody saw. Two
# questions, and only two, are ANSWERED, because the run cannot go on
# without an answer and the answer is never in doubt:
#
#   - "Save [project] before closing?" at the end of a session whose project
#     was already written with Main_SaveProjectEx, or was opened only to be
#     read and rendered -> No;
#   - "You have not yet selected an audio device" (a fresh profile, should
#     the Dummy Audio setting ever stop working) -> No.
#
# Answered with WM_COMMAND IDNO, not by clicking a button found by its
# caption: the captions are the system's language ("&No" here, "&Nein" or
# "&Non" elsewhere), the button id is not. A popup menu left open (a page
# asking the host for a context menu) is closed with Escape after a moment,
# or it would hold REAPER's message loop and the script behind it.
#
# A plugin's floating editor is logged too, with what is inside it -- class
# and size of every child window -- which is how the test sees the size an
# editor came up at in each format.
#
# ONLY windows of the REAPER started with the -cfgfile given here are looked
# at: the user may have their own REAPER open, and its "Save changes?" is
# theirs to answer.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File reaper-host-dialogs.ps1 <seconds> <cfgfile> <log>
param([int] $seconds = 120, [string] $cfg = "", [string] $logFile = "")
if ($cfg -eq "" -or $logFile -eq "") {
  Write-Output "usage: reaper-host-dialogs.ps1 <seconds> <cfgfile> <log>"
  exit 1
}

$sig = @'
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public class SonoreReaperDialogs {
  public delegate bool CB(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(CB cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, CB cb, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  /// Every visible descendant of a window: class, size in device pixels, and
  /// depth below it -- where a floating FX window keeps the plugin's view.
  public static string Children(IntPtr top) {
    var sb = new StringBuilder();
    EnumChildWindows(top, (h, l) => {
      if (!IsWindowVisible(h)) return true;
      RECT r; GetWindowRect(h, out r);
      int depth = 0; for (var p = GetParent(h); p != IntPtr.Zero && p != top && depth < 16; p = GetParent(p)) depth++;
      sb.Append(depth + ":" + Cls(h) + ":" + (r.Right - r.Left) + "x" + (r.Bottom - r.Top) + " ");
      return true; }, IntPtr.Zero);
    return sb.ToString();
  }
  public static string Text(IntPtr h) { var sb = new StringBuilder(1024); GetWindowText(h, sb, 1024); return sb.ToString(); }
  public static string Cls(IntPtr h) { var sb = new StringBuilder(128); GetClassName(h, sb, 128); return sb.ToString(); }
  public static List<IntPtr> Windows(HashSet<uint> pids) {
    var r = new List<IntPtr>();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (pids.Contains(p) && IsWindowVisible(h)) r.Add(h); return true; }, IntPtr.Zero);
    return r;
  }
  /// The texts inside a dialog, and the ids of its buttons.
  public static string Contents(IntPtr dlg, List<int> buttons) {
    var sb = new StringBuilder();
    EnumChildWindows(dlg, (h, l) => {
      var c = Cls(h); var t = Text(h);
      if (c == "Button") buttons.Add(GetDlgCtrlID(h));
      if (t.Length > 0 && (c == "Static" || c == "Button")) sb.Append(t.Replace("\r", " ").Replace("\n", " ") + " | ");
      return true; }, IntPtr.Zero);
    return sb.ToString();
  }
}
'@
Add-Type -TypeDefinition $sig

function Log([string] $line) { Add-Content -Path $logFile -Value $line -Encoding utf8 }

# Ready. Add-Type runs the C# compiler, which took seconds on a busy machine,
# and REAPER was already cycling the first plugin's editor by then: that
# editor was never seen, and its size never compared. The log file appearing
# is the signal the test waits for before it starts REAPER.
New-Item -ItemType File -Path $logFile -Force | Out-Null

$seen = @{}
$menuSince = @{}
$lookedAt = @{}
$deadline = (Get-Date).AddSeconds($seconds)
while ((Get-Date) -lt $deadline) {
  $pids = New-Object 'System.Collections.Generic.HashSet[uint32]'
  Get-CimInstance Win32_Process -Filter "Name='reaper.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine.Contains($cfg) } |
    ForEach-Object { [void] $pids.Add([uint32] $_.ProcessId) }
  if ($pids.Count -gt 0) {
    foreach ($h in [SonoreReaperDialogs]::Windows($pids)) {
      $cls = [SonoreReaperDialogs]::Cls($h)
      # Gone since the list was taken (a menu closing, a tooltip): a window
      # that no longer exists has no class to read and nothing to answer.
      if ($cls -eq "" -and -not [SonoreReaperDialogs]::IsWindow($h)) { continue }
      $title = [SonoreReaperDialogs]::Text($h)
      $buttons = New-Object 'System.Collections.Generic.List[int]'
      $text = ""
      if ($cls -eq "#32770") { $text = [SonoreReaperDialogs]::Contents($h, $buttons) }
      $key = "$h|$title"
      if (-not $seen.ContainsKey($key)) {
        $seen[$key] = 1
        Log ("window`t$cls`t$title`t$text")
      }
      # A plugin's floating editor: what is inside it, each time that changes
      # -- the editor's size as the host shows it, in every format. Looked
      # at twice a second at most, so the watcher stays quick to reach a
      # popup menu that is holding REAPER's message loop.
      if ($title -match "^(CLAP|VST3|LV2)i?: " -and
          (-not $lookedAt.ContainsKey($h) -or ((Get-Date) - $lookedAt[$h]).TotalSeconds -ge 0.5)) {
        $lookedAt[$h] = Get-Date
        $inside = [SonoreReaperDialogs]::Children($h)
        $ekey = "editor|$h|$inside"
        if ($inside -ne "" -and -not $seen.ContainsKey($ekey)) {
          $seen[$ekey] = 1
          Log ("editor`t$title`t$inside")
        }
      }
      if ($cls -eq "#32770" -and $buttons.Contains(7) -and
          ($text -match "audio device" -or $text -match "before closing")) {
        [void] [SonoreReaperDialogs]::PostMessage($h, 0x0111, [IntPtr] 7, [IntPtr]::Zero) # WM_COMMAND IDNO
        Log ("answered`tNo`t$title`t$text")
      }
      if ($cls -eq "#32768") {
        if (-not $menuSince.ContainsKey($h)) { $menuSince[$h] = Get-Date }
        elseif (((Get-Date) - $menuSince[$h]).TotalSeconds -gt 1.5) {
          [void] [SonoreReaperDialogs]::PostMessage($h, 0x0100, [IntPtr] 0x1B, [IntPtr]::Zero) # WM_KEYDOWN Escape
          [void] [SonoreReaperDialogs]::PostMessage($h, 0x0101, [IntPtr] 0x1B, [IntPtr]::Zero)
          Log ("dismissed`tpopup menu")
          $menuSince.Remove($h)
        }
      }
    }
  }
  Start-Sleep -Milliseconds 250
}
