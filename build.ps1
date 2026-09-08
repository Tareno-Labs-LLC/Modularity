#Requires -Version 5.1
<#
    Native Windows build driver for Modularity.

    Deliberately written against Windows PowerShell 5.1, which ships with Windows,
    so this needs nothing installed. That rules out '&&', '||', ternaries and '??' -
    if you edit this, keep to 5.1 syntax or the script stops working out of the box.

    The presentation mirrors build.sh (same banner, icons, progress prefix, stage
    hierarchy and issue summary) so the two read as one tool. The Linux script keeps
    its own implementation: almost all of its 1200 lines are Linux-specific
    (package managers, Android NDK, LFS, ISA verification) and share nothing with
    an MSVC build beyond the chrome.

    Unlike the old build.bat this CAN show a real spinner: Start-Process gives us a
    live process handle plus its exit code, which cmd could never do at once.
#>

[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Arguments
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Console
# ---------------------------------------------------------------------------

$script:UseUnicode = $true
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
} catch {
    # A redirected or legacy console can refuse UTF-8; fall back to ASCII glyphs
    # rather than emitting mojibake.
    $script:UseUnicode = $false
}

if ($script:UseUnicode) {
    $script:BarFill = [char]0x2588   # full block
    $script:BarEmpty = [char]0x2591  # light shade
    $script:Sep = [char]0x2502       # box drawing vertical
    # Braille frames advance in smaller visual steps than -\|/, so the line reads
    # as "working" rather than "flickering" at a 125ms tick.
    $script:SpinnerFrames = @(
        [char]0x280B, [char]0x2819, [char]0x2839, [char]0x2838, [char]0x283C,
        [char]0x2834, [char]0x2826, [char]0x2827, [char]0x2807, [char]0x280F
    )
} else {
    $script:BarFill = '#'
    $script:BarEmpty = '-'
    $script:Sep = '|'
    $script:SpinnerFrames = @('-', '\', '|', '/')
}

$script:IconInfo = 'i'
$script:IconWarn = '!'
$script:IconError = 'x'
$script:IconOk = '+'

# Write-Host -ForegroundColor rather than raw ANSI: it works on every console
# 5.1 can run on, including ones with VT sequence processing disabled.
$script:ColInfo = 'Cyan'
$script:ColWarn = 'Yellow'
$script:ColError = 'Red'
$script:ColOk = 'Green'
$script:ColDim = 'DarkGray'
$script:ColBold = 'White'

$script:Warnings = New-Object System.Collections.Generic.List[string]
$script:Errors = New-Object System.Collections.Generic.List[string]
$script:StepTimings = New-Object System.Collections.Generic.List[object]

$script:CurrentStep = 0
$script:TotalSteps = 0
$script:LastStep = 'bootstrap'
$script:StatusLineActive = $false

# The equivalent of build.sh's `[[ -t 1 ]]`. A carriage return only rewinds a
# real console; when output is piped to a file or another process it is just a
# character, so an animated line would append every frame instead of replacing
# it. Redraw only when someone is actually watching.
$script:Interactive = $true
try {
    if ([Console]::IsOutputRedirected) { $script:Interactive = $false }
} catch {
    $script:Interactive = $false
}

function Write-Tagged {
    param([string] $Icon, [string] $Color, [string] $Message)
    Write-Host "[$Icon]" -ForegroundColor $Color -NoNewline
    Write-Host " $Message"
}

function Write-Info { param([string] $Message) Write-Tagged $script:IconInfo $script:ColInfo $Message }
function Write-Ok { param([string] $Message) Write-Tagged $script:IconOk $script:ColOk $Message }
function Write-Err {
    param([string] $Message)
    Write-Tagged $script:IconError $script:ColError $Message
}
function Write-Warn {
    param([string] $Message, [switch] $NoRecord)
    Write-Tagged $script:IconWarn $script:ColWarn $Message
    if (-not $NoRecord) { $script:Warnings.Add($Message) }
}

# ---------------------------------------------------------------------------
# Progress prefix:  [icon] | NN/NN | [####....] PP% |
# ---------------------------------------------------------------------------

function Write-ProgressPrefix {
    param(
        [string] $Icon,
        [string] $Color,
        [switch] $Rewrite   # overwrite the current line instead of starting a new one
    )

    $width = 24
    $filled = 0
    $percent = 0
    if ($script:TotalSteps -gt 0) {
        $filled = [int][Math]::Floor($script:CurrentStep * $width / $script:TotalSteps)
        $percent = [int][Math]::Floor($script:CurrentStep * 100 / $script:TotalSteps)
    }
    $empty = $width - $filled

    if ($Rewrite) { Write-Host "`r" -NoNewline }

    Write-Host "[$Icon]" -ForegroundColor $Color -NoNewline
    Write-Host " $($script:Sep) " -ForegroundColor $script:ColDim -NoNewline
    Write-Host ("{0,2}/{1,-2}" -f $script:CurrentStep, $script:TotalSteps) -ForegroundColor $script:ColBold -NoNewline
    Write-Host " $($script:Sep) [" -ForegroundColor $script:ColDim -NoNewline
    if ($filled -gt 0) {
        Write-Host ([string]$script:BarFill * $filled) -ForegroundColor $script:ColOk -NoNewline
    }
    if ($empty -gt 0) {
        Write-Host ([string]$script:BarEmpty * $empty) -ForegroundColor $script:ColDim -NoNewline
    }
    Write-Host "] " -ForegroundColor $script:ColDim -NoNewline
    Write-Host ("{0,3}%" -f $percent) -ForegroundColor $script:ColBold -NoNewline
    Write-Host " $($script:Sep) " -ForegroundColor $script:ColDim -NoNewline
}

function Get-ConsoleWidth {
    $width = 100
    try { $width = [Console]::WindowWidth } catch { }
    if ($width -lt 40) { $width = 100 }
    return $width
}

function Clear-StatusLine {
    if (-not $script:StatusLineActive) { return }
    $width = (Get-ConsoleWidth) - 1
    Write-Host ("`r" + (' ' * $width) + "`r") -NoNewline
    $script:StatusLineActive = $false
}

function Write-StatusLine {
    <#
        The in-place status line: progress prefix, step label, elapsed seconds and
        whatever the build is currently chewing on. Everything is measured against
        the console width first and the activity text is the part that gets cut,
        because a line that wraps leaves debris the next carriage return cannot
        erase - which is what makes a spinner look broken.
    #>
    param([string] $Icon, [string] $Label, [int] $Seconds, [string] $Activity)

    $width = 24
    $filled = 0
    $percent = 0
    if ($script:TotalSteps -gt 0) {
        $filled = [int][Math]::Floor($script:CurrentStep * $width / $script:TotalSteps)
        $percent = [int][Math]::Floor($script:CurrentStep * 100 / $script:TotalSteps)
    }
    $empty = $width - $filled

    $head = "[$Icon] $($script:Sep) " + ('{0,2}/{1,-2}' -f $script:CurrentStep, $script:TotalSteps) +
            " $($script:Sep) ["
    $tail = "] " + ('{0,3}%' -f $percent) + " $($script:Sep) "
    $stamp = " ($($Seconds)s)"

    $lineWidth = (Get-ConsoleWidth) - 1
    $fixedLength = $head.Length + $width + $tail.Length + $Label.Length + $stamp.Length
    $budget = $lineWidth - $fixedLength

    $ellipsis = '...'
    if ($script:UseUnicode) { $ellipsis = [string][char]0x2026 }

    $activityText = ''
    if ($Activity -and $budget -gt 6) {
        # Collapse whitespace: compiler output is full of runs of spaces and the
        # odd tab, which turn into ragged gaps once it is squeezed onto one line.
        $flat = ($Activity -replace '\s+', ' ').Trim()
        $activityText = '  ' + $flat
        if ($activityText.Length -gt $budget) {
            $activityText = $activityText.Substring(0, [Math]::Max(0, $budget - $ellipsis.Length)) + $ellipsis
        }
    }

    Write-Host "`r" -NoNewline
    Write-Host "[$Icon]" -ForegroundColor $script:ColInfo -NoNewline
    Write-Host " $($script:Sep) " -ForegroundColor $script:ColDim -NoNewline
    Write-Host ('{0,2}/{1,-2}' -f $script:CurrentStep, $script:TotalSteps) -ForegroundColor $script:ColBold -NoNewline
    Write-Host " $($script:Sep) [" -ForegroundColor $script:ColDim -NoNewline
    if ($filled -gt 0) { Write-Host ([string]$script:BarFill * $filled) -ForegroundColor $script:ColOk -NoNewline }
    if ($empty -gt 0) { Write-Host ([string]$script:BarEmpty * $empty) -ForegroundColor $script:ColDim -NoNewline }
    Write-Host "] " -ForegroundColor $script:ColDim -NoNewline
    Write-Host ('{0,3}%' -f $percent) -ForegroundColor $script:ColBold -NoNewline
    Write-Host " $($script:Sep) " -ForegroundColor $script:ColDim -NoNewline
    Write-Host $Label -NoNewline
    Write-Host $stamp -ForegroundColor $script:ColDim -NoNewline
    if ($activityText) { Write-Host $activityText -ForegroundColor $script:ColDim -NoNewline }

    # Blank the rest of the row. A carriage return only moves the cursor back, it
    # does not erase, so a frame shorter than the one before it leaves the old
    # tail sitting there and the line reads as garbled overlapping text. Padding
    # to the full width is what makes each frame replace rather than overwrite.
    # Stop one column short: filling the final cell wraps the cursor onto the
    # next row, and the following carriage return would then rewind the wrong one.
    $painted = $fixedLength + $activityText.Length
    if ($painted -lt $lineWidth) {
        Write-Host (' ' * ($lineWidth - $painted)) -NoNewline
    }

    $script:StatusLineActive = $true
}

# ---------------------------------------------------------------------------
# Build output classification
# ---------------------------------------------------------------------------

$script:ErrorPattern = 'error C\d|error LNK|error MSB|fatal error|unresolved external|CMake Error'
$script:WarnPattern = 'warning C\d|warning LNK|warning MSB|CMake Warning'
$script:InfoPattern = 'Configuring done|Generating done|Build files have been written'

function Read-StepOutput {
    <#
        Errors are surfaced immediately, because that is what someone actually
        needs in front of them. Warnings are only counted and summarised: MSVC
        emits them by the hundred on this codebase, and echoing each one the way
        build.sh does would bury the build it is meant to describe.
    #>
    param([string] $Label, [string[]] $Lines)

    if (-not $Lines) { return }

    foreach ($line in $Lines) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $text = $line.TrimEnd()

        if ($text -match $script:ErrorPattern) {
            $entry = "[$Label] $text"
            $script:Errors.Add($entry)
            Write-Tagged $script:IconError $script:ColError $entry
            continue
        }
        if ($text -match $script:WarnPattern) {
            $script:Warnings.Add("[$Label] $text")
            continue
        }
        if ($text -match $script:InfoPattern) {
            Write-Tagged $script:IconInfo $script:ColInfo "[$Label] $text"
        }
    }
}

# ---------------------------------------------------------------------------
# Step runners
# ---------------------------------------------------------------------------

function Start-Step {
    param([string] $Label)
    $script:CurrentStep++
    $script:LastStep = $Label
    Write-Host ''
    Write-ProgressPrefix -Icon $script:IconInfo -Color $script:ColInfo
    Write-Host $Label
}

function Complete-Step {
    param([string] $Label, [int] $Seconds, [switch] $Failed)

    if ($Failed) {
        Write-ProgressPrefix -Icon $script:IconError -Color $script:ColError
        Write-Host "$Label " -NoNewline
        Write-Host $script:Sep -ForegroundColor $script:ColDim -NoNewline
        Write-Host ' failed ' -ForegroundColor $script:ColError -NoNewline
        Write-Host "($($Seconds)s)" -ForegroundColor $script:ColDim
    } else {
        Write-ProgressPrefix -Icon $script:IconOk -Color $script:ColOk
        Write-Host "$Label " -NoNewline
        Write-Host $script:Sep -ForegroundColor $script:ColDim -NoNewline
        Write-Host " ($($Seconds)s)" -ForegroundColor $script:ColDim
    }
}

function ConvertTo-ProcessArgument {
    # Start-Process joins -ArgumentList with spaces and quotes nothing, so an
    # argument that itself contains a space silently becomes several arguments -
    # which is how "-G Visual Studio 18 2026" reached cmake as "-G Visual".
    param([string] $Value)
    if ($Value -match '[\s"]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }
    return $Value
}

function Invoke-ExternalStep {
    <#
        Runs a real executable with a live spinner. Start-Process hands back a
        process object, so the exit code survives the wait - which is exactly what
        cmd could not do while also animating, and why build.bat had no spinner.
    #>
    param(
        [string] $Label,
        [string] $FilePath,
        [string[]] $StepArguments
    )

    Start-Step $Label
    $sw = [System.Diagnostics.Stopwatch]::StartNew()

    $stdout = [System.IO.Path]::GetTempFileName()
    $stderr = [System.IO.Path]::GetTempFileName()
    $frames = $script:SpinnerFrames
    $frameIndex = 0
    $exitCode = 0
    $activity = ''

    try {
        $quoted = @($StepArguments | ForEach-Object { ConvertTo-ProcessArgument $_ })
        $proc = Start-Process -FilePath $FilePath -ArgumentList $quoted `
            -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr

        # Touching .Handle forces the caller to keep the native process handle open.
        # Without it .NET can release the handle as the process dies and ExitCode
        # comes back $null - which then compares as "not 0" and reports a perfectly
        # good step as failed. This line is load-bearing, not diagnostic.
        $null = $proc.Handle

        if ($script:Interactive) {
            # Follow the child's stdout while it is still being written. FileShare
            # ReadWrite is the part that matters: without it this open fails because
            # the compiler already holds the file, and the status line would have
            # nothing to report but a clock.
            $stream = $null
            $reader = $null
            try {
                $stream = New-Object System.IO.FileStream(
                    $stdout, [System.IO.FileMode]::Open,
                    [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
                $reader = New-Object System.IO.StreamReader($stream)
            } catch { }

            while (-not $proc.HasExited) {
                if ($reader) {
                    while ($null -ne ($line = $reader.ReadLine())) {
                        $trimmed = $line.Trim()
                        if ($trimmed) { $activity = $trimmed }
                    }
                }
                $frame = $frames[$frameIndex % $frames.Length]
                $frameIndex++
                Write-StatusLine -Icon $frame -Label $Label `
                    -Seconds ([int]$sw.Elapsed.TotalSeconds) -Activity $activity
                Start-Sleep -Milliseconds 125
            }

            if ($reader) { $reader.Dispose() }
            if ($stream) { $stream.Dispose() }
        }

        $proc.WaitForExit()
        $exitCode = $proc.ExitCode
    } catch {
        Clear-StatusLine
        Write-Err "Failed to launch $FilePath : $($_.Exception.Message)"
        $exitCode = 1
    }

    Clear-StatusLine
    $sw.Stop()

    $lines = @()
    foreach ($file in @($stdout, $stderr)) {
        if (Test-Path $file) {
            $content = Get-Content -LiteralPath $file -ErrorAction SilentlyContinue
            if ($content) { $lines += $content }
        }
    }

    Read-StepOutput -Label $Label -Lines $lines

    $seconds = [int]$sw.Elapsed.TotalSeconds
    $script:StepTimings.Add([pscustomobject]@{ Name = $Label; Seconds = $seconds; Failed = ($exitCode -ne 0) })

    if ($exitCode -ne 0) {
        Complete-Step -Label $Label -Seconds $seconds -Failed
        $script:Errors.Add("[$Label] Step failed with exit code $exitCode.")

        # The classifier only surfaces lines it recognises. When a step dies for a
        # reason no pattern matches - a missing tool, a crashed generator - that
        # leaves nothing on screen, so show the raw tail and keep the log around.
        $tail = @($lines | Where-Object { $_ -and $_.Trim() } | Select-Object -Last 15)
        if ($tail.Count -gt 0) {
            Write-Host ''
            Write-Host "  Last $($tail.Count) line(s) of output:" -ForegroundColor $script:ColDim
            foreach ($line in $tail) { Write-Host "    $($line.TrimEnd())" -ForegroundColor $script:ColDim }
        }

        $kept = Join-Path ([System.IO.Path]::GetTempPath()) ("modularity-build-" + ($Label -replace '\W', '-') + ".log")
        try {
            Set-Content -LiteralPath $kept -Value $lines -Encoding UTF8
            Write-Host ''
            Write-Err "Full log kept at: $kept"
        } catch { }

        Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
        return $false
    }

    Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
    Complete-Step -Label $Label -Seconds $seconds
    return $true
}

function Invoke-InlineStep {
    # For work done in-process (file copying, directory removal). No spinner:
    # these finish fast enough that animating them would only add flicker.
    param([string] $Label, [scriptblock] $Action)

    Start-Step $Label
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $ok = $true
    try {
        & $Action | Out-Null
    } catch {
        Write-Err $_.Exception.Message
        $script:Errors.Add("[$Label] $($_.Exception.Message)")
        $ok = $false
    }
    $sw.Stop()

    $seconds = [int]$sw.Elapsed.TotalSeconds
    $script:StepTimings.Add([pscustomobject]@{ Name = $Label; Seconds = $seconds; Failed = (-not $ok) })
    if (-not $ok) {
        Complete-Step -Label $Label -Seconds $seconds -Failed
        return $false
    }
    Complete-Step -Label $Label -Seconds $seconds
    return $true
}

# ---------------------------------------------------------------------------
# Root resolution
# ---------------------------------------------------------------------------

function Test-ModularityRoot {
    param([string] $Candidate)
    if ([string]::IsNullOrWhiteSpace($Candidate)) { return $false }
    $cmakeLists = Join-Path $Candidate 'CMakeLists.txt'
    if (-not (Test-Path $cmakeLists)) { return $false }
    if (-not (Test-Path (Join-Path $Candidate 'src'))) { return $false }
    if (-not (Test-Path (Join-Path $Candidate 'Resources'))) { return $false }
    return [bool](Select-String -LiteralPath $cmakeLists -Pattern 'project\(Modularity' -Quiet -SimpleMatch:$false)
}

function Find-ModularityRoot {
    param([string] $Start)
    $current = $Start
    while (-not [string]::IsNullOrWhiteSpace($current)) {
        if (Test-ModularityRoot $current) { return (Resolve-Path $current).Path }
        $nested = Join-Path $current 'Modularity'
        if (Test-ModularityRoot $nested) { return (Resolve-Path $nested).Path }
        $parent = Split-Path $current -Parent
        if ($parent -eq $current) { break }
        $current = $parent
    }
    return $null
}

function Resolve-VisualStudioGenerator {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        $vswhere = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    if (-not (Test-Path $vswhere)) { return $null }

    $version = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationVersion 2>$null
    if (-not $version) { return $null }

    $major = ($version -split '\.')[0]
    switch ($major) {
        '18' { return 'Visual Studio 18 2026' }
        '17' { return 'Visual Studio 17 2022' }
        '16' { return 'Visual Studio 16 2019' }
    }
    return $null
}

# ---------------------------------------------------------------------------
# Argument parsing - matches build.sh's flag spelling, not PowerShell's, so
# muscle memory carries across the two scripts.
# ---------------------------------------------------------------------------

function Show-Usage {
    Write-Host 'Usage: .\build.ps1 [options]'
    Write-Host 'Options:'
    Write-Host '  --clean                 Remove existing build directories first'
    Write-Host '  --build-type=<type>     CMake build type (default: Release)'
    Write-Host '  --jobs=<N>              Parallel compile jobs (default: cores - 2, min 1)'
    Write-Host '  --no-player             Skip the ModularityPlayer runtime build'
    Write-Host '  --zip                   Package as .zip (default; fast to compress)'
    Write-Host '  --7z                    Package as .7z (smaller, but much slower)'
    Write-Host '  --help                  Show this help message'
}

$cleanBuild = $false
$skipPlayer = $false
$buildType = 'Release'
$buildCppStandard = 'c++23'
# ZIP is the default package format. It is stored/deflate and finishes in
# seconds; 7z runs single-threaded LZMA over the whole distribution and can
# rival the compile time on a 4-core machine. Pass --7z for release builds.
$packageFormat = 'ZIP'
$packageExt = 'zip'
$jobs = 0

foreach ($arg in $Arguments) {
    switch -Regex ($arg) {
        '^--clean$'      { $cleanBuild = $true; continue }
        '^--no-player$'  { $skipPlayer = $true; continue }
        '^--zip$'        { $packageFormat = 'ZIP'; $packageExt = 'zip'; continue }
        '^--7z$'         { $packageFormat = '7Z'; $packageExt = '7z'; continue }
        '^--help$|^-h$'  { Show-Usage; exit 0 }
        '^--build-type=(.+)$' { $buildType = $Matches[1]; continue }
        '^--jobs=(\d+)$' { $jobs = [int]$Matches[1]; continue }
        default {
            Write-Warn "Unknown argument: $arg" -NoRecord
            Show-Usage
            exit 1
        }
    }
}

# Match build.sh: default to (logical processors - 2) so the machine stays
# usable during a build and MSVC's per-process memory doesn't swamp RAM.
$coreCount = [Environment]::ProcessorCount
if ($jobs -le 0) {
    $jobs = $coreCount - 2
    $jobsDefaulted = $true
} else {
    $jobsDefaulted = $false
}
if ($jobs -lt 1) { $jobs = 1 }

# ---------------------------------------------------------------------------
# Bootstrap
# ---------------------------------------------------------------------------

$scriptHome = $PSScriptRoot
if (-not $scriptHome) { $scriptHome = (Get-Location).Path }

$root = $null
if ($env:MODULARITY_ROOT -and (Test-ModularityRoot $env:MODULARITY_ROOT)) {
    $root = (Resolve-Path $env:MODULARITY_ROOT).Path
}
if (-not $root) { $root = Find-ModularityRoot $scriptHome }
if (-not $root) { $root = Find-ModularityRoot (Get-Location).Path }
if (-not $root) {
    Write-Err 'Could not find the Modularity source root. Set MODULARITY_ROOT or run from inside/above the Modularity folder.'
    exit 1
}
Set-Location $root

$buildDir = Join-Path $root 'build'
$playerCacheDir = Join-Path $buildDir 'player-cache'

# Resolve the CMake generator explicitly. Without -G, CMake uses the machine's
# default generator, which is "NMake Makefiles" on some setups. That default
# rejects "-A x64" and cannot find cl.exe unless launched from a Developer
# Command Prompt. A Visual Studio generator accepts -A x64 and locates the
# compiler itself, so this works from a plain shell.
$generator = $env:CMAKE_GENERATOR
if (-not $generator) { $generator = Resolve-VisualStudioGenerator }

$generatorArgs = @()
$isVsGenerator = $false
$generatorDisplay = 'CMake default generator'
if ($generator) {
    $generatorDisplay = $generator
    $generatorArgs = @('-G', $generator)
    if ($generator.StartsWith('Visual Studio')) {
        $generatorArgs += @('-A', 'x64')
        $isVsGenerator = $true
    }
}

# MSBuild's /m only parallelizes across projects. Most of the compile time is
# inside one huge target (core), so without this the extra cores idle.
# UseMultiToolTask parallelizes files within a project, and
# EnforceProcessCountAcrossBuilds caps total cl.exe processes at CL_MPCount so
# the two layers don't multiply into jobs*jobs compilers and exhaust RAM.
$vsGlobalsArgs = @()
$buildExtraArgs = @()
if ($isVsGenerator) {
    $vsGlobalsArgs = @("-DCMAKE_VS_GLOBALS=UseMultiToolTask=true;EnforceProcessCountAcrossBuilds=true;CL_MPCount=$jobs")
    $buildExtraArgs = @('--', '/nologo', '/v:m')
}

$monoArgs = @()
if ($env:MONO_ROOT) { $monoArgs = @("-DMONO_ROOT=$env:MONO_ROOT") }

# --- Step accounting. Keep in sync with the hierarchy printed below. --------
$script:TotalSteps = 6
if ($cleanBuild) { $script:TotalSteps += 2 }
if (-not $skipPlayer) { $script:TotalSteps += 4 }

# --- Banner ----------------------------------------------------------------
$overallTimer = [System.Diagnostics.Stopwatch]::StartNew()

Write-Host '================================' -ForegroundColor $script:ColBold
Write-Host '   Modularity - Native Windows Build' -ForegroundColor $script:ColBold
Write-Host '================================' -ForegroundColor $script:ColBold

if ($jobsDefaulted -and $jobs -lt $coreCount) {
    Write-Info "Build type: $buildType  -  C++: $buildCppStandard  -  Jobs: $jobs (of $coreCount cores; reserved 2 to keep desktop responsive - override with --jobs=N)"
} else {
    Write-Info "Build type: $buildType  -  C++: $buildCppStandard  -  Jobs: $jobs"
}
Write-Info "CMake generator: $generatorDisplay"
Write-Info "Package format: $packageFormat"
Write-Info "Modularity root: $root"
if (-not $generator) {
    Write-Warn 'No Visual Studio generator detected; using CMake default.'
    Write-Warn 'If configuration fails, install VS with the "Desktop development with C++" workload.'
}
if (Get-Command ccache -ErrorAction SilentlyContinue) {
    $env:CCACHE_BASEDIR = $root
    $env:CCACHE_NOHASHDIR = '1'
    Write-Info 'ccache detected. Normalizing paths for cross-build cache hits.'
}

# --- Toolchain report ------------------------------------------------------
# Printed every run rather than on demand: when a build breaks on one machine
# and not another, the answer is nearly always a version or a missing tool here,
# and having it already in the scrollback beats asking for it after the fact.
function Get-ToolVersion {
    param([string] $Name, [string[]] $VersionArgs, [string] $Pattern)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) { return $null }
    try {
        $raw = (& $cmd.Source @VersionArgs 2>&1 | Select-Object -First 3) -join ' '
        if ($Pattern -and $raw -match $Pattern) { return $Matches[1] }
        return ($raw -split '\r?\n')[0]
    } catch {
        return 'present (version unknown)'
    }
}

Write-Info 'Toolchain:'
$cmakeVersion = Get-ToolVersion 'cmake' @('--version') 'cmake version (\S+)'
$gitVersion = Get-ToolVersion 'git' @('--version') 'git version (\S+)'
$tools = @(
    @{ Name = 'cmake'; Value = $cmakeVersion },
    @{ Name = 'git'; Value = $gitVersion },
    @{ Name = 'ninja'; Value = (Get-ToolVersion 'ninja' @('--version') $null) },
    @{ Name = 'ccache'; Value = (Get-ToolVersion 'ccache' @('--version') 'ccache version (\S+)') }
)
foreach ($tool in $tools) {
    if ($tool.Value) {
        Write-Host ("      {0,-8} {1}" -f $tool.Name, $tool.Value) -ForegroundColor $script:ColDim
    } else {
        Write-Host ("      {0,-8} not found" -f $tool.Name) -ForegroundColor $script:ColDim
    }
}
if (-not $cmakeVersion) {
    Write-Warn 'cmake is not on PATH. Configure and build will both fail.'
}

# Free space on the build volume. A stalled or truncated link on Windows often
# reports as something unrelated, and this build tree runs to several GB.
try {
    $driveName = (Split-Path -Qualifier $root).TrimEnd(':')
    $drive = Get-PSDrive -Name $driveName -ErrorAction SilentlyContinue
    if ($drive) {
        $freeGb = [Math]::Round($drive.Free / 1GB, 1)
        Write-Host ("      {0,-8} {1} GB free on {2}:" -f 'disk', $freeGb, $driveName) -ForegroundColor $script:ColDim
        if ($freeGb -lt 10) {
            Write-Warn "Only $freeGb GB free on ${driveName}: - a full build plus package needs roughly 10 GB."
        }
    }
} catch { }

if (Test-Path (Join-Path $buildDir 'CMakeCache.txt')) {
    Write-Info 'Existing build directory found - this is an incremental build (pass --clean to start fresh).'
} else {
    Write-Info 'No existing build directory - this is a full build from scratch.'
}

# --- Stage hierarchy -------------------------------------------------------
$stages = New-Object System.Collections.Generic.List[string]
if ($cleanBuild) {
    $stages.Add('Clean editor build directory')
    $stages.Add('Clean player cache directory')
}
$stages.Add('Sync git submodules')
$stages.Add('Configure editor build')
$stages.Add('Build editor + engine targets')
$stages.Add('Collect editor libraries')
$stages.Add('Copy resources')
if (-not $skipPlayer) {
    $stages.Add('Configure player-only cache build')
    $stages.Add('Build ModularityPlayer target')
    $stages.Add('Collect player third-party libraries')
    $stages.Add('Collect player engine libraries')
}
$stages.Add('Package artifacts and resources')

Write-Info 'Build stage hierarchy:'
for ($i = 0; $i -lt $stages.Count; $i++) {
    $branch = '|--'
    if ($i -eq $stages.Count - 1) { $branch = '`--' }
    Write-Host ("  {0} [{1:d2}/{2:d2}] {3}" -f $branch, ($i + 1), $stages.Count, $stages[$i])
}

# ---------------------------------------------------------------------------
# Library collection. Engine static libs and per-target import libs are
# link-time only - not needed at runtime and individually huge (core.lib and
# core_player.lib are ~100 MB each), so they never go into the package.
# ---------------------------------------------------------------------------

$script:ExcludedLibs = @(
    'core.lib', 'core_player.lib', 'Modularity.lib', 'ModularityPlayer.lib',
    'glad.lib', 'imgui.lib', 'imguizmo.lib'
)

$script:BinaryScanCache = @{}

function Get-PackageBinaryCandidates {
    <#
        One recursive scan per target directory, reused across the ThirdParty and
        Engine stages.

        -Filter, not -Include: -Include is applied after the provider has already
        enumerated every entry, which over this build tree (PhysX, assimp, ffmpeg)
        turned a ~40s collect into ~180s. -Filter is evaluated by the filesystem
        itself. The engine DLLs are a subset of the DLL scan, so they cost nothing
        extra rather than a third walk.
    #>
    param([string] $TargetDir)

    if ($script:BinaryScanCache.ContainsKey($TargetDir)) {
        return $script:BinaryScanCache[$TargetDir]
    }

    $libs = @(Get-ChildItem -LiteralPath $TargetDir -Recurse -File -Filter '*.lib' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\Packages\\' })
    $dlls = @(Get-ChildItem -LiteralPath $TargetDir -Recurse -File -Filter '*.dll' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\Packages\\' })

    $result = @{
        Libs = @($libs | Where-Object { $script:ExcludedLibs -notcontains $_.Name })
        Dlls = $dlls
        EngineDlls = @($dlls | Where-Object { $_.Name -like 'core*.dll' })
    }
    $script:BinaryScanCache[$TargetDir] = $result
    return $result
}

function Copy-PackageBinaries {
    param([string] $TargetDir, [switch] $IncludeEngine, [switch] $IncludeThirdParty)

    $found = Get-PackageBinaryCandidates -TargetDir $TargetDir

    if ($IncludeThirdParty) {
        $thirdParty = Join-Path $TargetDir 'Packages\ThirdParty'
        New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null
        foreach ($file in ($found.Libs + $found.Dlls)) {
            Copy-Item -LiteralPath $file.FullName -Destination $thirdParty -Force
        }
    }

    if ($IncludeEngine) {
        $engine = Join-Path $TargetDir 'Packages\Engine'
        New-Item -ItemType Directory -Force -Path $engine | Out-Null
        foreach ($file in $found.EngineDlls) {
            Copy-Item -LiteralPath $file.FullName -Destination $engine -Force
        }
    }
}

# ---------------------------------------------------------------------------
# Steps
# ---------------------------------------------------------------------------

function Invoke-BuildPipeline {
    if ($cleanBuild) {
        if (-not (Invoke-InlineStep 'Cleaning Editor' {
            if (Test-Path $buildDir) { Remove-Item -LiteralPath $buildDir -Recurse -Force }
        })) { return $false }

        if (-not (Invoke-InlineStep 'Cleaning Player' {
            if (Test-Path $playerCacheDir) { Remove-Item -LiteralPath $playerCacheDir -Recurse -Force }
        })) { return $false }
    }

    if (-not (Invoke-ExternalStep 'Syncing Submodules' 'git' @('submodule', 'update', '--init', '--recursive'))) { return $false }

    $configureArgs = @() + $generatorArgs + @('-S', $root, '-B', $buildDir,
        "-DCMAKE_BUILD_TYPE=$buildType", "-DMODULARITY_BUILD_CPP_STANDARD=$buildCppStandard") + $vsGlobalsArgs + $monoArgs
    if (-not (Invoke-ExternalStep 'Configuring Editor' 'cmake' $configureArgs)) { return $false }

    $buildArgs = @('--build', $buildDir, '--config', $buildType, '--parallel', "$jobs") + $buildExtraArgs
    if (-not (Invoke-ExternalStep 'Building Editor' 'cmake' $buildArgs)) { return $false }

    if (-not (Invoke-InlineStep 'Collecting Editor Libs' {
        Copy-PackageBinaries -TargetDir $buildDir -IncludeThirdParty -IncludeEngine
    })) { return $false }

    if (-not (Invoke-InlineStep 'Copying Resources' {
        $dest = Join-Path $buildDir 'Resources'
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        Copy-Item -Path (Join-Path $root 'Resources\*') -Destination $dest -Recurse -Force
        $ini = Join-Path $dest 'imgui.ini'
        if (Test-Path $ini) { Copy-Item -LiteralPath $ini -Destination $buildDir -Force }
    })) { return $false }

    if ($skipPlayer) {
        Write-Info '--no-player: skipping the ModularityPlayer runtime build.'
    } else {
        $playerConfigureArgs = @() + $generatorArgs + @('-S', $root, '-B', $playerCacheDir,
            '-DMODULARITY_BUILD_EDITOR=OFF', "-DCMAKE_BUILD_TYPE=$buildType",
            "-DMODULARITY_BUILD_CPP_STANDARD=$buildCppStandard") + $vsGlobalsArgs + $monoArgs
        if (-not (Invoke-ExternalStep 'Configuring Player' 'cmake' $playerConfigureArgs)) { return $false }

        $playerBuildArgs = @('--build', $playerCacheDir, '--config', $buildType,
            '--target', 'ModularityPlayer', '--parallel', "$jobs") + $buildExtraArgs
        if (-not (Invoke-ExternalStep 'Building Player' 'cmake' $playerBuildArgs)) { return $false }

        if (-not (Invoke-InlineStep 'Collecting Player Libs' {
            Copy-PackageBinaries -TargetDir $playerCacheDir -IncludeThirdParty
        })) { return $false }

        if (-not (Invoke-InlineStep 'Collecting Player Engine Libs' {
            Copy-PackageBinaries -TargetDir $playerCacheDir -IncludeEngine
        })) { return $false }
    }

    # CPack failing is a packaging problem, not a build problem: the editor and
    # player are already built and usable, so this warns rather than failing.
    Start-Step 'Packaging Engine'
    $packTimer = [System.Diagnostics.Stopwatch]::StartNew()
    Push-Location $buildDir
    try {
        $packOutput = & cpack -G $packageFormat -C $buildType 2>&1
        $packExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    $packTimer.Stop()
    Read-StepOutput -Label 'Packaging Engine' -Lines ($packOutput | ForEach-Object { "$_" })
    if ($packExit -ne 0) {
        $script:Warnings.Add("[Packaging Engine] CPack failed with exit $packExit. Distribution archive was not produced.")
    }
    $packSeconds = [int]$packTimer.Elapsed.TotalSeconds
    # Recorded by hand because this is the one step that does not go through
    # Invoke-ExternalStep or Invoke-InlineStep - without this it is silently
    # absent from the timing table despite often being the slowest thing here.
    $script:StepTimings.Add([pscustomobject]@{ Name = 'Packaging Engine'; Seconds = $packSeconds; Failed = ($packExit -ne 0) })
    Complete-Step -Label 'Packaging Engine' -Seconds $packSeconds

    return $true
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

function Write-TimingSummary {
    # Where the time actually went. Cheap to print and it is the first thing you
    # want when a build that used to take two minutes starts taking ten.
    if ($script:StepTimings.Count -eq 0) { return }

    $total = ($script:StepTimings | Measure-Object -Property Seconds -Sum).Sum
    if ($total -le 0) { $total = 1 }

    Write-Host ''
    Write-Host 'Step timings:' -ForegroundColor $script:ColBold
    foreach ($step in $script:StepTimings) {
        $share = [int]([Math]::Floor($step.Seconds * 20 / $total))
        $meter = ([string]$script:BarFill * $share) + ([string]$script:BarEmpty * (20 - $share))
        $color = $script:ColDim
        if ($step.Failed) { $color = $script:ColError }
        Write-Host ('  {0,-32} {1,5}s  ' -f $step.Name, $step.Seconds) -NoNewline
        Write-Host $meter -ForegroundColor $color
    }
}

function Write-IssueSummary {
    $max = 8

    if ($script:Warnings.Count -gt 0) {
        Write-Host ''
        Write-Host "Warnings ($($script:Warnings.Count)):" -ForegroundColor $script:ColWarn
        $shown = [Math]::Min($max, $script:Warnings.Count)
        for ($i = 0; $i -lt $shown; $i++) {
            Write-Host "  [$($script:IconWarn)] $($script:Warnings[$i])"
        }
        if ($script:Warnings.Count -gt $max) {
            Write-Host "  [$($script:IconWarn)] ... and $($script:Warnings.Count - $max) more warning(s)"
        }
    }

    if ($script:Errors.Count -gt 0) {
        Write-Host ''
        Write-Host "Errors ($($script:Errors.Count)):" -ForegroundColor $script:ColError
        $shown = [Math]::Min($max, $script:Errors.Count)
        for ($i = 0; $i -lt $shown; $i++) {
            Write-Host "  [$($script:IconError)] $($script:Errors[$i])"
        }
        if ($script:Errors.Count -gt $max) {
            Write-Host "  [$($script:IconError)] ... and $($script:Errors.Count - $max) more error(s)"
        }
    }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

$succeeded = $false
try {
    $succeeded = Invoke-BuildPipeline
} catch {
    Clear-StatusLine
    Write-Err $_.Exception.Message
    $script:Errors.Add($_.Exception.Message)
    $succeeded = $false
}

$overallTimer.Stop()
$duration = '{0:N2}' -f $overallTimer.Elapsed.TotalSeconds

Write-Host ''
if ($succeeded) {
    Write-Host '================================' -ForegroundColor $script:ColBold
    Write-Host '   Modularity - Native Windows Build Complete' -ForegroundColor $script:ColBold
    Write-Host '================================' -ForegroundColor $script:ColBold
    Write-Ok "Build completed in $($duration)s."
    Write-Info "Editor:        build\$buildType\Modularity.exe"
    if ($skipPlayer) {
        Write-Info 'Player:        skipped (--no-player)'
    } else {
        Write-Info "Player:        build\$buildType\ModularityPlayer.exe"
    }
    Write-Info "Distribution:  build\Modularity-1.0.0-Windows.$packageExt"
    Write-TimingSummary
    Write-IssueSummary
    exit 0
} else {
    Write-Host '================================' -ForegroundColor $script:ColBold
    Write-Host '   Modularity - Native Windows Build Failed' -ForegroundColor $script:ColBold
    Write-Host '================================' -ForegroundColor $script:ColBold
    Write-Err "Build failed after $($duration)s at step: $($script:LastStep)."
    Write-TimingSummary
    Write-IssueSummary
    exit 1
}
