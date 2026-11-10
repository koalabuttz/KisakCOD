param([Parameter(Mandatory)][string]$Description, [switch]$Check)
# One PS Vita client run in Vita3K from one description file: build (WSL), install, the starts, the results.
#
#   pwsh -File vita-run.ps1 <run.json>          run it
#   pwsh -File vita-run.ps1 <run.json> -Check   read the description and say what would be run
#
# Input: a JSON description (README-vita-run.md). Output: runs\<id>\results.json in the repository's
# kisak-run-result/1 format, sealed with scripts/runs/run_report.py's checksum, and results-compact.json,
# its one-line short form. Log lines, counts and figures only; no game file contents, no image is kept.
#
# What it writes: runs\<id>\ and local-builds\ under the Vita3K work folder; on the emulated card only
# cmdline-mp.txt, the marker files a start asks for (removed again), and what installing and running the app
# writes; in WSL the build tree (through wsl\03-build.sh). It never pushes, and it leaves the program cache
# alone unless the description says "cold_cache": true (then the cache is moved aside, not deleted).
$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot; $v = Split-Path $tools
$card = "$env:APPDATA\Vita3K\Vita3K\ux0\data\KisakCOD"
$appDir = "$env:APPDATA\Vita3K\Vita3K\ux0\app\KSKD00002"
$exe = "$v\app\Vita3K.exe"
$endMarker = 'gxm: display shut down'
# What the results call this machine: the first line of machine.txt beside this script (not in the
# repository: it names one machine), or the computer's name.
$machine = if (Test-Path "$tools\machine.txt") { (Get-Content "$tools\machine.txt" -TotalCount 1).Trim() } else { "$env:COMPUTERNAME, Vita3K, local WSL build" }
function WslPath([string]$p) { '/mnt/' + $p.Substring(0, 1).ToLower() + $p.Substring(2).Replace('\', '/') }
function Utc { (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }
function Has($o, [string]$n) { $null -ne $o -and $o.PSObject.Properties.Name -contains $n -and $null -ne $o.$n }
function Say([string]$m) { Write-Host "[$((Get-Date).ToString('HH:mm:ss'))] $m" }

# ---------------------------------------------------------------- the description
$d = Get-Content -Raw $Description | ConvertFrom-Json
$id = if (Has $d 'id') { [string]$d.id } else { [IO.Path]::GetFileNameWithoutExtension($Description) }
if ($id -notmatch '^[A-Za-z0-9_.+-]+$') { throw "id '$id': letters, digits, _ . + - only" }
$opt = if (Has $d 'options') { $d.options } else { [pscustomobject]@{} }
$noAssertExit = (Has $opt 'no_assert_exit') -and $opt.no_assert_exit
$noSafetyNet = (Has $opt 'no_safety_net') -and $opt.no_safety_net
$coldCache = (Has $d 'cold_cache') -and $d.cold_cache
$useInstalled = (Has $d 'use_installed') -and $d.use_installed
$safe = '^[A-Za-z0-9_./+-]+$'
if (-not $useInstalled) {
    if (-not (Has $d 'commit') -or [string]$d.commit -notmatch $safe) { throw 'commit: a commit or branch name is needed (or "use_installed": true)' }
    if ((Has $d 'merge') -and [string]$d.merge -notmatch $safe) { throw 'merge: not a commit name' }
}
$cmake = @(); if (Has $d 'cmake') { $cmake = @($d.cmake | ForEach-Object { [string]$_ }) }
foreach ($c in $cmake) { if ($c -notmatch '^-D[A-Za-z0-9_]+=[A-Za-z0-9_./:+-]*$') { throw "cmake option '$c': only -DNAME=value" } }
$buildName = if (Has $d 'name') { [string]$d.name } elseif (Has $d 'merge') { "$($d.commit)+$($d.merge)" } elseif ($cmake.Count) { "$($d.commit)-opts" } else { '' }
if ($buildName -and $buildName -notmatch '^[A-Za-z0-9_.+-]+$') { throw "name '$buildName': letters, digits, _ . + - only" }
$starts = @($d.starts)
if (-not $starts.Count) { throw 'starts: at least one start is needed' }
foreach ($s in $starts) {
    if (-not (Has $s 'tag') -or [string]$s.tag -notmatch '^[A-Za-z0-9_.+-]+$') { throw 'every start needs a tag (letters, digits, _ . + -)' }
    if ((Has $s 'markers')) { foreach ($m in @($s.markers)) { if ([string]$m -notmatch '^[A-Za-z0-9_.-]+$' -or [string]$m -like '*.gxp') { throw "marker '$m': a plain file name, not a .gxp" } } }
}
$out = "$v\runs\$id"
if ($Check) {
    "run $id -> $out"
    if ($useInstalled) { 'build: the installed one' } else { "build: $($d.commit)$(if (Has $d 'merge') { " merged with $($d.merge)" })$(if ($cmake.Count) { " with $($cmake -join ' ')" })$(if ($buildName) { " as $buildName" })" }
    foreach ($s in $starts) { "start $($s.tag): $($s.cmdline)" }
    return
}
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item $Description "$out\description.json" -Force
$started = Utc
$build = [ordered]@{}; $status = 'ok'; $rows = New-Object System.Collections.Generic.List[object]
$elf = ''; $hasSwitches = $false

# ---------------------------------------------------------------- build and install
function Fail-Run([string]$why) { $script:status = "failed: $why"; Say $script:status }
if ($useInstalled) {
    $build['build'] = 'the installed build, not built or installed by this run'
    if (Test-Path "$v\installed-build.txt") {
        $n = (Get-Content "$v\installed-build.txt" -TotalCount 1).Trim()
        if ($n -and (Test-Path "$v\local-builds\$n\KisakCOD-mp.elf")) { $elf = "$v\local-builds\$n\KisakCOD-mp.elf"; $build['name'] = $n }
    }
} else {
    if (Get-Process Vita3K -ErrorAction SilentlyContinue) { throw 'Vita3K is running; close it first' }
    $envs = @("DEST_ROOT='$(WslPath "$v\local-builds")'")
    if (Has $d 'merge') { $envs += "MERGE=$($d.merge)"; $envs += 'MERGE_OPTS=' }
    if ($cmake.Count) { $envs += "BUILD_SUFFIX=-$buildName"; $envs += "EXTRA_CMAKE='$($cmake -join ' ')'" }
    if ($buildName) { $envs += "DEST_NAME=$buildName" }
    Say "building $($d.commit)$(if (Has $d 'merge') { " + $($d.merge)" })"
    $t0 = Get-Date
    $log = & wsl.exe -d Ubuntu -- bash -c "sed 's/\r$//' '$(WslPath "$tools\wsl\03-build.sh")' | $($envs -join ' ') bash -s -- $($d.commit)" 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $log | Set-Content "$out\build.log" -Encoding utf8
    $build['seconds'] = [string][math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    $build['asked'] = "$($d.commit)$(if (Has $d 'merge') { " merged with $($d.merge) (plain git merge --no-edit, local, not pushed)" })"
    if ($cmake.Count) { $build['cmake'] = $cmake -join ' ' }
    $m = $log | Select-String -Pattern '^commit: ([0-9a-f]{40})' | Select-Object -First 1
    if ($code -ne 0 -or -not $m) {
        $build['output'] = (($log | Select-Object -Last 12) -join ' / ')
        Fail-Run $(if ($log -match 'CONFLICT') { 'the merge has conflicts' } else { 'the build failed' })
    } else {
        $sha = $m.Matches[0].Groups[1].Value
        $folder = "$v\local-builds\$(if ($buildName) { $buildName } else { $sha })"
        $build['commit'] = $sha
        $build['name'] = Split-Path $folder -Leaf
        if (Test-Path "$folder\COMMIT.txt") { $p = (Get-Content "$folder\COMMIT.txt" | Select-String '^parents (.+)$'); if ($p -and $p.Matches[0].Groups[1].Value.Trim().Contains(' ')) { $build['merge_commit'] = "$sha (local) of $($p.Matches[0].Groups[1].Value.Trim())" } }
        if (Test-Path "$folder\BUILD.txt") { $build['compiler'] = (Get-Content "$folder\BUILD.txt" | Select-String 'clang' | Select-Object -First 1).Line }
        $checks = @($log | Select-String -Pattern '^(check: |CHECK FAILED)')
        $bad = @($checks | Where-Object { $_.Line -like 'CHECK FAILED*' })
        $build['binary_checks'] = if ($bad.Count) { ($bad | ForEach-Object { $_.Line }) -join '; ' } else { "$($checks.Count) passed" }
        $elf = "$folder\KisakCOD-mp.elf"
        $build['elf_sha256'] = (Get-FileHash $elf).Hash.ToLower()
        # install: a harmless command line for the short boot the install does
        [IO.File]::WriteAllText("$card\cmdline-mp.txt", '+set vita_quitAfter 15')
        if ($coldCache) {
            $aside = "$card\gxp-before-$id"; New-Item -ItemType Directory -Force $aside | Out-Null
            $n = 0; Get-ChildItem "$card\gxp" -Filter *.gxp -File -ErrorAction SilentlyContinue | ForEach-Object { Move-Item $_.FullName $aside -Force; $n++ }
            $build['cold_cache'] = "$n programs moved aside to gxp-before-$id"
        }
        Say 'installing'
        $p = Start-Process $exe -ArgumentList '--frontend', 'sdl', "`"$folder\KisakCOD-mp.vpk`"" -WorkingDirectory "$v\app" -RedirectStandardOutput "$out\install.out" -RedirectStandardError "$out\install.err" -PassThru
        Start-Sleep 8; if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null; if (-not $p.WaitForExit(20000)) { Stop-Process -Id $p.Id -Force } }
        if ($coldCache) { Get-ChildItem "$card\gxp" -Filter *.gxp -File -ErrorAction SilentlyContinue | Remove-Item -Force }
        $build['name'] | Set-Content "$v\installed-build.txt" -Encoding ascii
    }
}
if (Test-Path "$appDir\eboot.bin") { $build['eboot_sha256'] = (Get-FileHash "$appDir\eboot.bin").Hash.ToLower() }
if ($elf -and (Test-Path $elf)) {
    # does the build know the switches for unattended runs? (their names are in the ELF's strings)
    $bytes = [IO.File]::ReadAllBytes($elf); $text = [Text.Encoding]::ASCII.GetString($bytes)
    $hasSwitches = $text.Contains('vita_quitAfterMenu') -and $text.Contains('vita_atMap')
    $bytes = $null; $text = $null
    $build['unattended_switches'] = if ($hasSwitches) { 'in this build' } else { 'not in this build: fixed-length fallback' }
} else { $build['unattended_switches'] = 'unknown (no ELF for the installed build): fixed-length fallback' }

# ---------------------------------------------------------------- one start
function Read-Echo($state) {
    # read what Vita3K wrote since the last call; keep the app's output lines ("*** TTY:") with their wall time
    $fs = [IO.File]::Open($state.path, 'Open', 'Read', 'ReadWrite,Delete')
    try {
        $fs.Position = $state.pos; $buf = New-Object byte[] ($fs.Length - $state.pos)
        $n = $fs.Read($buf, 0, $buf.Length); $state.pos += $n
    } finally { $fs.Dispose() }
    if ($n -le 0) { return }
    $chunk = $state.rest + [Text.Encoding]::UTF8.GetString($buf, 0, $n)
    $parts = $chunk -split "`r?`n"; $state.rest = $parts[-1]
    for ($i = 0; $i -lt $parts.Count - 1; $i++) {
        $l = $parts[$i]
        if ($l -match '^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\] \|(.)\| \[([^\]]*)\]: (.*)$') {
            $wall = [int]$Matches[1] * 3600 + [int]$Matches[2] * 60 + [int]$Matches[3] + [int]$Matches[4] / 1000
            if ($Matches[6] -eq 'write_file' -and $Matches[7].StartsWith('*** TTY: ')) {
                $state.cur = [pscustomobject]@{ W = $wall; T = $Matches[7].Substring(9) }; $state.lines.Add($state.cur)
            } else {
                $state.cur = $null
                if ($Matches[6] -like '*invalid_access*') { $state.invalid++; if ($Matches[7] -match '\(pc (0x[0-9a-fA-F]+)\)') { $state.pcs[$Matches[1].ToLower()] = 1 + [int]$state.pcs[$Matches[1].ToLower()] } }
                elseif ($Matches[6] -like '*exception_handler*' -and $Matches[7] -like 'Exception *') { $state.exceptions++ }
                elseif ($Matches[7] -match '^The app exited with code (-?\d+)') { $state.appExit = [int]$Matches[1] }
                elseif ($Matches[7] -match 'Undefined instruction at address (0x[0-9a-fA-F]+)') { $state.pcs[$Matches[1].ToLower()] = 1 + [int]$state.pcs[$Matches[1].ToLower()] }
            }
        } elseif ($state.cur -and $l.Trim() -and $l -notmatch '^(PC:|r\d+ ?:|Thumb:)') { $state.cur.T = ($state.cur.T + ' / ' + $l.Trim()).TrimStart(' ', '/') }
    }
}
function Run-Start($s) {
    $tag = [string]$s.tag
    $limit = 300; $stopPattern = ''; $stopAfter = 0
    if (Has $s 'stop') { if (Has $s.stop 'seconds') { $limit = [int]$s.stop.seconds }; if (Has $s.stop 'pattern') { $stopPattern = [string]$s.stop.pattern; if (Has $s.stop 'after') { $stopAfter = [int]$s.stop.after } } }
    $cmd = [string]$s.cmdline; $notes = New-Object System.Collections.Generic.List[string]
    $quitAfter = $limit
    foreach ($pair in @(@('quit_after_menu', 'vita_quitAfterMenu', 45), @('quit_after_map', 'vita_quitAfterMap', 90))) {
        if (Has $s $pair[0]) {
            if ($hasSwitches) { $cmd += " +set $($pair[1]) $([int]$s.($pair[0]))" }
            else { $quitAfter = [math]::Min($quitAfter, [int]$s.($pair[0]) + $pair[2]); $notes.Add("$($pair[0]): no such switch in this build, so a fixed vita_quitAfter $quitAfter") }
        }
    }
    if (Has $s 'stats_every') { if ($hasSwitches) { $cmd += " +set vita_statsEvery $([int]$s.stats_every)" } else { $notes.Add('stats_every: no such switch in this build, figures come once a minute') } }
    foreach ($pair in @(@('at_menu', 'vita_atMenu'), @('at_map', 'vita_atMap'))) {
        if (Has $s $pair[0]) {
            if ([string]$s.($pair[0]) -match '"') { throw "$($pair[0]): no double quotes inside" }
            if ($hasSwitches) { $cmd += " +set $($pair[1]) `"$($s.($pair[0]))`"" } else { $notes.Add("$($pair[0]): no such switch in this build, its commands were not sent (use input steps)") }
        }
    }
    if (-not $noAssertExit -and $cmd -notmatch 'vita_assertExit') { $cmd += ' +set vita_assertExit 1' }
    if (-not $noSafetyNet -and $cmd -notmatch 'vita_quitAfter ') { $cmd += " +set vita_quitAfter $quitAfter" }
    $made = @()
    if (Has $s 'markers') { foreach ($m in @($s.markers)) { $mp = "$card\gxp\$m"; if (Test-Path $mp) { $notes.Add("marker $m was already there and is left there") } else { New-Item -ItemType File $mp | Out-Null; $made += $mp } } }
    [IO.File]::WriteAllText("$card\cmdline-mp.txt", $cmd)
    # Vita3K does not cut a file the game rewrites, so an old, longer log would leave its tail behind the new one
    foreach ($dir in $card, "$card\main") { foreach ($n in 'console-mp.log', 'console_mp.log', 'console.log') { Remove-Item "$dir\$n" -ErrorAction SilentlyContinue } }
    $steps = @(); if (Has $s 'input') { $steps = @($s.input) }
    $views = New-Object System.Collections.Generic.List[string]
    Say "start $tag ($limit s at most): $cmd"
    $t0 = Get-Date
    $p = Start-Process $exe -ArgumentList '--frontend', 'sdl', '-r', 'KSKD00002' -WorkingDirectory "$v\app" -RedirectStandardOutput "$out\$tag.out" -RedirectStandardError "$out\$tag.err" -PassThru
    $null = $p.Handle
    $st = [pscustomobject]@{ path = "$out\$tag.out"; pos = [long]0; rest = ''; lines = (New-Object System.Collections.Generic.List[object]); cur = $null; invalid = 0; exceptions = 0; pcs = @{}; appExit = $null }
    $stepIx = 0; $stepSeen = $null; $stopSeen = $null; $ended = ''; $seen = 0; $exitSeen = $null
    while (-not $p.HasExited) {
        Start-Sleep -Milliseconds 500
        $el = ((Get-Date) - $t0).TotalSeconds
        if (Test-Path $st.path) { Read-Echo $st }
        $new = @(); if ($st.lines.Count -gt $seen) { $new = $st.lines.GetRange($seen, $st.lines.Count - $seen); $seen = $st.lines.Count }
        if ($stopPattern -and $null -eq $stopSeen) { foreach ($l in $new) { if ($l.T -cmatch $stopPattern) { $stopSeen = $el; break } } }
        if ($stepIx -lt $steps.Count) {
            $step = $steps[$stepIx]; $due = $false
            if (Has $step 'at') { $due = $el -ge [double]$step.at }
            elseif (Has $step 'after') {
                if ($null -eq $stepSeen) { foreach ($l in $st.lines) { if ($l.T -cmatch [string]$step.after) { $stepSeen = $el; break } } }
                if ($null -ne $stepSeen) { $due = ($el - $stepSeen) -ge $(if (Has $step 'plus') { [double]$step.plus } else { 0 }) }
            } else { $due = $true }
            if ($due) {
                $hold = if (Has $step 'hold_ms') { [int]$step.hold_ms } else { 900 }
                if (Has $step 'key') { & "$tools\tap.ps1" -Key ([string]$step.key) -HoldMs $hold -Repeat $(if (Has $step 'repeat') { [int]$step.repeat } else { 1 }) | Out-Null }
                elseif (Has $step 'click') { & "$tools\tap.ps1" -X ([int]$step.click[0]) -Y ([int]$step.click[1]) -HoldMs $hold | Out-Null }
                elseif (Has $step 'view') {
                    $png = "$out\_view.png"; & "$tools\shot.ps1" -Out $png | Out-Null
                    if (Test-Path $png) { $views.Add("$($step.view) at $([int]$el) s: " + ((& "$tools\shotstats.ps1" -Path $png | Select-Object -First 1))); Remove-Item $png -Force }
                }
                $stepIx++; $stepSeen = $null
            }
        }
        # Vita3K says when the app has left; after an exit code other than 0 Vita3K itself can stay for ever
        if ($null -ne $st.appExit -and $null -eq $exitSeen) { $exitSeen = $el }
        if ($null -ne $exitSeen -and ($el - $exitSeen) -ge 8) { $ended = 'app exit'; break }
        if ($null -ne $stopSeen -and ($el - $stopSeen) -ge $stopAfter) { $ended = 'closed by the script after its stop pattern'; break }
        if ($el -ge $limit + 45) { $ended = 'hard limit'; break }
    }
    $seconds = [math]::Round($(if ($null -ne $exitSeen) { $exitSeen } else { ((Get-Date) - $t0).TotalSeconds }), 1)
    if ($p.HasExited) { $p.WaitForExit(); $ended = 'by itself'; $exit = $p.ExitCode }
    else {
        $p.CloseMainWindow() | Out-Null
        $closed = $p.WaitForExit(20000)
        if (-not $closed) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(5000) | Out-Null }
        $how = if ($closed) { 'closed' } else { 'killed' }
        if ($ended -eq 'app exit') { $ended = "by itself; Vita3K stayed after the app had left and was $how by the script"; $exit = $null }
        elseif ($ended -eq 'hard limit') { $ended = "$how by the script at the hard limit"; $exit = $null }
        else { $exit = $(if ($closed) { $p.ExitCode } else { $null }); if (-not $closed) { $ended = "$ended, then killed (the window did not close)" } }
    }
    Start-Sleep -Milliseconds 800; if (Test-Path $st.path) { Read-Echo $st }
    if ($null -ne $st.appExit) { $exit = $st.appExit }   # the app's own exit code, as Vita3K reports it
    foreach ($mp in $made) { Remove-Item $mp -Force -ErrorAction SilentlyContinue }

    # the log: the file, and Vita3K's echo for what the file lost
    $file = @(); if (Test-Path "$card\console-mp.log") { Copy-Item "$card\console-mp.log" "$out\$tag-console-mp.log" -Force; $file = @(Get-Content "$out\$tag-console-mp.log") }
    $lines = New-Object System.Collections.Generic.List[string]; $source = 'the log file'; $echoed = 0
    $whole = $file.Count -and ($file[-1] -like "*$endMarker")
    if ($whole) { $lines.AddRange([string[]]$file) }
    else {
        # keep the file's complete lines, then add the echoed lines after the last one that both have
        $keep = $file.Count; $anchor = -1; $off = 0.0
        for ($i = $file.Count - 1; $i -ge [math]::Max(0, $file.Count - 40) -and $anchor -lt 0; $i--) {
            if ($file[$i] -match '^\[\s*([\d.]+)\] (.+)$') {
                $text = $Matches[2]; $time = [double]$Matches[1]
                for ($j = $st.lines.Count - 1; $j -ge 0; $j--) { if ($st.lines[$j].T -eq $text -or $st.lines[$j].T -eq "^3$text") { $anchor = $j; $off = $st.lines[$j].W - $time; $keep = $i + 1; break } }
            }
        }
        if ($keep -gt 0) { $lines.AddRange([string[]]$file[0..($keep - 1)]) }
        $from = $anchor + 1
        if ($anchor -lt 0) { $from = 0; if ($st.lines.Count) { $off = $st.lines[0].W } }
        for ($j = $from; $j -lt $st.lines.Count; $j++) { $lines.Add(('[{0,9:F3}] {1}' -f ($st.lines[$j].W - $off), $st.lines[$j].T)) }
        $echoed = [math]::Max(0, $st.lines.Count - $from)
        $source = if ($st.lines.Count -gt $from) { "the log file to its last whole line, then $($st.lines.Count - $from) line(s) from Vita3K's echo (the file's tail was lost; lines the game writes only to the file are missing there)" } else { 'the log file, which does not end with the shutdown line' }
    }
    $got = [ordered]@{}
    if (Has $s 'lines') { foreach ($pat in @($s.lines)) { $got[[string]$pat] = [string[]]@($lines | Where-Object { $_ -cmatch [string]$pat }) } }
    $counts = [ordered]@{
        asserts = @($lines | Where-Object { $_ -cmatch 'ASSERT FAILED' }).Count; errors = @($lines | Where-Object { $_ -cmatch 'ERROR' }).Count
        warnings = @($lines | Where-Object { $_ -cmatch 'WARNING' }).Count; out_of_gpu_memory = @($lines | Where-Object { $_ -cmatch 'out of GPU memory' }).Count
        no_memory = @($lines | Where-Object { $_ -cmatch 'no memory for |std::bad_alloc|Failed to Z_Malloc' }).Count
        vita3k_invalid_access = $st.invalid; vita3k_exceptions = $st.exceptions
    }
    if (Has $s 'counts') { foreach ($pat in @($s.counts)) { $counts[[string]$pat] = @($lines | Where-Object { $_ -cmatch [string]$pat }).Count } }
    $framelog = @($lines | Where-Object { $_ -cmatch 'frame log(?: \S+)?: .* fps' } | ForEach-Object { [ordered]@{ label = $(if ($_ -cmatch 'frame log (\S+): ') { $Matches[1] } else { '' }); line = $_ } })
    $why = ''
    if ($counts.asserts) { $why = "$($counts.asserts) failed assertion(s)" }
    elseif ($ended -like 'by itself*' -and $exit -ne 0) { $why = "exit code $exit" }
    elseif ($ended -like '*hard limit*') { $why = $ended }   # a stop the description asked for is not a failure, killed or not
    $row = [ordered]@{ name = $tag; variant = $tag; status = $(if ($why) { "failed: $why" } else { 'ok' }); exit = $exit; seconds = $seconds
        values = [ordered]@{ ended = $ended; log = $(if ($whole) { 'whole' } elseif ($echoed) { 'tail from the echo' } else { 'the file as written, without the shutdown line' }) }
        lines = $got; counts = $counts; framelog = $framelog }
    $outputs = [ordered]@{ 'command line as run' = [string[]]@($cmd); 'where the lines come from' = [string[]]@($source) }
    if ($views.Count) { $outputs['views (numbers only, images deleted)'] = [string[]]$views }
    if ($st.pcs.Count -and $elf -and (Test-Path $elf)) {
        $top = @($st.pcs.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 5)
        $res = @(& wsl.exe -d Ubuntu -- bash -c "~/vitasdk-pinned/vitasdk/bin/arm-vita-eabi-addr2line -f -C -e '$(WslPath $elf)' $(@($top | ForEach-Object { $_.Key }) -join ' ')" 2>&1 | ForEach-Object { "$_" })
        $a = @(); for ($i = 0; $i -lt $top.Count; $i++) { $a += "$($top[$i].Key) x$($top[$i].Value): $($res[2 * $i]) at $(($res[2 * $i + 1] -replace '^.*/\.\./\.\./', ''))" }
        $outputs['faulting addresses in Vita3K (addr2line with the kept ELF)'] = [string[]]$a
    } elseif ($st.pcs.Count) { $outputs['faulting addresses in Vita3K (no ELF at hand)'] = [string[]]@($st.pcs.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 5 | ForEach-Object { "$($_.Key) x$($_.Value)" }) }
    $row['outputs'] = $outputs
    if ($notes.Count) { $row['note'] = $notes -join '; ' }
    if ($why) { $row['tail'] = [string[]]@($lines | Select-Object -Last 30) }
    Say "start ${tag}: $($row.status); ended $ended; exit code $(if ($null -eq $exit) { 'unknown' } else { $exit }); $seconds s"
    return $row
}

# ---------------------------------------------------------------- the starts
if ($status -eq 'ok') {
    if (Get-Process Vita3K -ErrorAction SilentlyContinue) { throw 'Vita3K is running; close it first' }
    foreach ($s in $starts) { $rows.Add((Run-Start $s)) }
    $bad = @($rows | Where-Object { $_.status -ne 'ok' }).Count
    if ($bad) { $status = "$bad of $($rows.Count) starts failed" }
}

# ---------------------------------------------------------------- the results
$gxp = @(Get-ChildItem "$card\gxp" -Force -ErrorAction SilentlyContinue)
$state = [ordered]@{
    installed_eboot_sha256 = $(if (Test-Path "$appDir\eboot.bin") { (Get-FileHash "$appDir\eboot.bin").Hash.ToLower() } else { '' })
    installed_build = $(if (Test-Path "$v\installed-build.txt") { (Get-Content "$v\installed-build.txt" -TotalCount 1).Trim() } else { 'unknown' })
    cmdline_mp_txt = $(if (Test-Path "$card\cmdline-mp.txt") { [IO.File]::ReadAllText("$card\cmdline-mp.txt") } else { '' })
    programs_in_gxp = @($gxp | Where-Object { $_.Extension -eq '.gxp' }).Count
    marker_files = (@($gxp | Where-Object { $_.Extension -ne '.gxp' } | ForEach-Object { $_.Name }) -join ', ')
}
$body = [ordered]@{ format = 'kisak-run-result/1'; id = $id; title = $(if (Has $d 'title') { [string]$d.title } else { $id })
    machine = $machine; started = $started; finished = (Utc); status = $status
    build = $build; rows = [object[]]$rows.ToArray(); state = $state }
if (Has $d 'asker') { $body['asker'] = [string]$d.asker }
$body | ConvertTo-Json -Depth 12 | Set-Content "$out\results-body.json" -Encoding utf8
$seal = @(& python -I "$tools\vita_seal.py" "$out\results-body.json" "$out\results.json" "$out\results-compact.json" 2>&1 | ForEach-Object { "$_" })
Remove-Item "$out\results-body.json" -ErrorAction SilentlyContinue
"results: $out\results.json"
$seal
"run_report.py --check: $((& python -I "$tools\run_report.py" "$out\results.json" --check 2>&1 | ForEach-Object { "$_" }) -join ' ')"
"status: $status"
if (Test-Path "$out\results-compact.json") { 'compact line:'; Get-Content "$out\results-compact.json" -Raw }
