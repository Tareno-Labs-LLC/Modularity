param([string]$BuildDirectory = 'build', [string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    $build = [System.IO.Path]::GetFullPath((Join-Path $repo $BuildDirectory))
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$installation) { throw 'Visual Studio C++ build tools were not found.' }
    $devcmd = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
    $env:PATH = "$(Split-Path $vswhere);$env:PATH"
    $library = Join-Path $build "$($Configuration.ToLower())/imgui.lib"
    if (!(Test-Path -LiteralPath $library)) {
        & cmake --build $build --config $Configuration --target imgui --parallel 2
        if ($LASTEXITCODE) { throw 'Building imgui failed.' }
    }
    $runtime = if ($Configuration -eq 'Debug') { '/MDd' } else { '/MD' }
    $commandFile = Join-Path $build 'run-ui-checks.cmd'
    @"
@echo off
call "$devcmd" >nul
cl /nologo /std:c++17 /EHsc $runtime /I src /I include /I src/ThirdParty/ModuGUI tools/ui_runtime_tests.cpp "$library" /Fe:"$build/ui_runtime_tests.exe" /Fo:"$build/ui_runtime_tests.obj" /link user32.lib imm32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /Zs /I src /I include /I src/ThirdParty/ModuGUI /I src/ThirdParty/glad /I src/ThirdParty/glfw/include /I src/ThirdParty/glm tools/ui_script_api_compile.cpp
if errorlevel 1 exit /b 1
"$build/ui_runtime_tests.exe"
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $commandFile
    & cmd /c $commandFile
    if ($LASTEXITCODE) { throw "UI checks failed ($LASTEXITCODE)." }
} finally { Pop-Location }

