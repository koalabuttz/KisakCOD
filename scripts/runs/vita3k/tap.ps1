param([int]$X = -1, [int]$Y = -1, [string]$Key = '', [int]$HoldMs = 700, [int[]]$Drag = @(), [int]$StepMs = 900, [int]$Repeat = 1, [int]$GapMs = 900)
# Input for the Vita3K window.
#   -X -Y            : a mouse click held for $HoldMs at window-relative (X,Y); Vita3K turns it into a front touch.
#   -Drag x1,y1,x2,y2,... : press at the first point, move through the others ($StepMs at each), release at the last.
#   -Key X|Enter|Up|Down|Left|Right [-Repeat n] : hold a key for $HoldMs (X = cross; arrows = d-pad in Vita3K's default binds).
Add-Type -Namespace W -Name I -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, System.UIntPtr e);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, System.UIntPtr e);
[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
'@
[W.I]::SetProcessDPIAware() | Out-Null
$p = Get-Process Vita3K -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { 'no Vita3K window'; return }
[W.I]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
$r = New-Object W.I+RECT; [W.I]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
if ($Key) {
    $ext = 0
    switch ($Key) {
        'Enter' { $vk = 0x0D } 'Up' { $vk = 0x26; $ext = 1 } 'Down' { $vk = 0x28; $ext = 1 } 'Left' { $vk = 0x25; $ext = 1 } 'Right' { $vk = 0x27; $ext = 1 }
        default { $vk = [byte][char]$Key.ToUpper()[0] }
    }
    $sc = [byte][W.I]::MapVirtualKey($vk, 0)
    for ($i = 0; $i -lt $Repeat; $i++) {
        [W.I]::keybd_event([byte]$vk, $sc, $ext, [UIntPtr]::Zero); Start-Sleep -Milliseconds $HoldMs; [W.I]::keybd_event([byte]$vk, $sc, ($ext -bor 2), [UIntPtr]::Zero)
        if ($i -lt $Repeat - 1) { Start-Sleep -Milliseconds $GapMs }
    }
    "held key $Key for $HoldMs ms, $Repeat time(s)"
} elseif ($Drag.Count -ge 4) {
    [W.I]::SetCursorPos($r.L + $Drag[0], $r.T + $Drag[1]) | Out-Null; Start-Sleep -Milliseconds 300
    [W.I]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds $StepMs
    for ($i = 2; $i -lt $Drag.Count; $i += 2) { [W.I]::SetCursorPos($r.L + $Drag[$i], $r.T + $Drag[$i+1]) | Out-Null; [W.I]::mouse_event(1, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds $StepMs }
    [W.I]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    "dragged through $($Drag.Count / 2) points, $StepMs ms each, released at window $($Drag[-2]),$($Drag[-1])"
} else {
    [W.I]::SetCursorPos($r.L + $X, $r.T + $Y) | Out-Null; Start-Sleep -Milliseconds 300
    [W.I]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds $HoldMs; [W.I]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    "clicked at window $X,$Y (screen $($r.L + $X),$($r.T + $Y)) for $HoldMs ms"
}
