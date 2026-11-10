param([Parameter(Mandatory)][string]$Out)
# Capture the Vita3K window's screen area to a temporary PNG (viewed once, then deleted).
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace W -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool IsIconic(System.IntPtr h);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
'@
[W.U]::SetProcessDPIAware() | Out-Null
$p = Get-Process Vita3K -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { 'no Vita3K window'; return }
$h = $p.MainWindowHandle
[W.U]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400
$r = New-Object W.U+RECT
[W.U]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
"window '$($p.MainWindowTitle)' at $($r.L),$($r.T) size ${w}x${ht}; minimized $([W.U]::IsIconic($h)); foreground $([W.U]::GetForegroundWindow() -eq $h)"
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $ht))
$g.Dispose(); $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
"saved $Out ($((Get-Item $Out).Length) bytes)"
