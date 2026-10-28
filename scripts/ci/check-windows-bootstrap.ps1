param(
    [Parameter(Mandatory = $true)][string]$BuildOutput,
    [Parameter(Mandatory = $true)][string]$DxSdkPackage
)
$ErrorActionPreference = 'Stop'

# This checks native process/DLL startup without uploading licensed game assets.
# The expected result is the engine's missing-assets diagnostic, not a playable game.
$startup = Join-Path $env:RUNNER_TEMP 'kisakcod-windows-bootstrap'
New-Item -ItemType Directory -Path $startup -Force | Out-Null
Copy-Item (Join-Path $BuildOutput '*.exe') $startup
Copy-Item (Join-Path $BuildOutput '*.dll') $startup
Copy-Item 'deps/binklib/binkw32.dll' $startup
Get-ChildItem $DxSdkPackage -Recurse -File -Filter '*.dll' |
    Where-Object { $_.FullName -match '[\\/](x86|win-x86)[\\/]' } |
    Copy-Item -Destination $startup
# Minimal localization configuration prevents a missing-config assertion from
# obscuring the missing game-assets error; this contains no game data.
[IO.File]::WriteAllText((Join-Path $startup 'localization.txt'), "english`n")

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class BootstrapWindows {
    private delegate bool Callback(IntPtr window, IntPtr state);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr state);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent, Callback callback, IntPtr state);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int length);
    public static string Read(uint process) {
        var texts = new List<string>();
        Callback child = (window, state) => {
            var value = new StringBuilder(4096);
            GetWindowText(window, value, value.Capacity);
            if (value.Length > 0) texts.Add(value.ToString());
            return true;
        };
        EnumWindows((window, state) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == process) {
                child(window, IntPtr.Zero);
                EnumChildWindows(window, child, IntPtr.Zero);
            }
            return true;
        }, IntPtr.Zero);
        return String.Join("\n", texts);
    }
}
'@

$process = Start-Process -FilePath (Join-Path $startup 'KisakCOD-mp.exe') `
    -WorkingDirectory $startup -PassThru
$expected = "No languages available|Couldn't load (fileSysCheck|default_mp|default)\.cfg"
$deadline = (Get-Date).AddSeconds(45)
$observed = ''
try {
    while ((Get-Date) -lt $deadline) {
        $observed = [BootstrapWindows]::Read($process.Id)
        if ($observed -match $expected) {
            Write-Output 'PASS: native Windows bootstrap reached the expected missing-game-assets error.'
            Write-Output 'Full game initialization and gameplay are not tested by this check.'
            exit 0
        }
        $process.Refresh()
        if ($process.HasExited) {
            throw "Native executable exited before the expected asset check (exit $($process.ExitCode)): $observed"
        }
        Start-Sleep -Milliseconds 500
    }
    throw "Native executable did not reach the expected asset check within 45 seconds: $observed"
}
finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
