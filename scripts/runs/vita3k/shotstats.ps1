param([Parameter(Mandatory)][string]$Path, [int]$Left = 8, [int]$Top = 31, [int]$Width = 960, [int]$Height = 544, [int]$Step = 3)
# Numbers for a capture of the Vita3K window (from shot.ps1): mean brightness (luma, 0..255) and the share of
# pixels that are not black (largest channel above 16), over the game view (the client area: 960x544 at 8,31
# of the default 976x583 window) and over its 3x3 grid. Every $Step-th pixel in each direction is read.
# Prints numbers only; the image is not changed.
Add-Type -AssemblyName System.Drawing
$b = New-Object System.Drawing.Bitmap $Path
try {
    if ($Left + $Width -gt $b.Width) { $Width = $b.Width - $Left }; if ($Top + $Height -gt $b.Height) { $Height = $b.Height - $Top }
    $d = $b.LockBits((New-Object System.Drawing.Rectangle $Left, $Top, $Width, $Height), [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $buf = New-Object byte[] ($d.Stride * $Height); [Runtime.InteropServices.Marshal]::Copy($d.Scan0, $buf, 0, $buf.Length); $stride = $d.Stride; $b.UnlockBits($d)
} finally { $b.Dispose() }
$sum = New-Object double[] 9; $n = New-Object int[] 9; $nb = New-Object int[] 9
for ($y = 0; $y -lt $Height; $y += $Step) {
    $row = $y * $stride; $cy = [int][math]::Floor($y * 3 / $Height) * 3
    for ($x = 0; $x -lt $Width; $x += $Step) {
        $i = $row + $x * 3; $pb = $buf[$i]; $pg = $buf[$i + 1]; $pr = $buf[$i + 2]
        $c = $cy + [int][math]::Floor($x * 3 / $Width)
        $sum[$c] += 0.299 * $pr + 0.587 * $pg + 0.114 * $pb; $n[$c]++
        if ($pr -gt 16 -or $pg -gt 16 -or $pb -gt 16) { $nb[$c]++ }
    }
}
$ts = ($sum | Measure-Object -Sum).Sum; $tn = ($n | Measure-Object -Sum).Sum; $tb = ($nb | Measure-Object -Sum).Sum
"view ${Width}x${Height}: mean brightness {0:F1}, not black {1:P1}; 3x3 grid as brightness/not-black:" -f ($ts / $tn), ($tb / $tn)
for ($r = 0; $r -lt 3; $r++) { '  ' + ((0..2 | ForEach-Object { $k = $r * 3 + $_; '{0,5:F1}/{1,4:P0}' -f ($sum[$k] / $n[$k]), ($nb[$k] / $n[$k]) }) -join '   ') }
