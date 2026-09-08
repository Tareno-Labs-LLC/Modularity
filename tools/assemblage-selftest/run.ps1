# Self-test for the Assemblage core (cells, chunking, RLE codec, both formats).
#
# Windows/MSVC counterpart to run.sh. The other selftests in tools/ are bash +
# g++ + build/libcore.a only, which never runs on a Visual Studio generator
# build; the Assemblage core depends on nothing but glm and the standard
# library, so this compiles the one source file directly with cl.exe.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\assemblage-selftest\run.ps1
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir '..\..')).Path
$buildDir = Join-Path $env:TEMP 'modularity-assemblage-selftest'
$workDir = Join-Path $buildDir 'work'

if (Test-Path $workDir) { Remove-Item -Recurse -Force $workDir }
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

# cl.exe is only on PATH inside a Developer Command Prompt. Locate the VS install
# with vswhere and import its environment, so this runs from a plain shell.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "cl.exe is not on PATH and vswhere.exe was not found. Run this from a Developer PowerShell for VS, or use tools/assemblage-selftest/run.sh under a POSIX toolchain."
    }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ([string]::IsNullOrWhiteSpace($vsPath)) {
        throw "vswhere found no Visual Studio install with the C++ toolset."
    }
    $devShell = Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path $devShell)) {
        throw "Could not find Microsoft.VisualStudio.DevShell.dll under $vsPath."
    }
    Import-Module $devShell
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -no_logo' | Out-Null
}

$exe = Join-Path $buildDir 'assemblage_test.exe'

Push-Location $buildDir
try {
    # /EHsc for standard exception semantics, /utf-8 so the source is read as
    # UTF-8 rather than the system ANSI codepage.
    & cl.exe /nologo /std:c++17 /EHsc /utf-8 /W3 /Zi /Od `
        "/I$repoRoot\src" `
        "/I$repoRoot\src\ThirdParty\glm" `
        "$repoRoot\src\Assemblage.cpp" `
        "$repoRoot\tools\assemblage-selftest\assemblage_test.cpp" `
        /Fe:$exe
    if ($LASTEXITCODE -ne 0) { throw "compile failed with exit code $LASTEXITCODE" }
}
finally {
    Pop-Location
}

& $exe $workDir
exit $LASTEXITCODE
