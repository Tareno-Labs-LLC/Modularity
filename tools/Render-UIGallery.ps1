param([string]$BuildDirectory = 'build', [string]$Output = 'build/ui-gallery.png', [string]$Defines = '', [switch]$Edit)
# Renders every extended UI control with the editor theme to a PNG (see tools/ui_gallery.cpp).
# -Edit draws them non-interactive, the way the Scene view and a stopped Game view show them.
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
    $libs = @("$build/release/imgui.lib", "$build/release/glad.lib", "$build/src/ThirdParty/glfw/src/release/glfw3.lib")
    foreach ($lib in $libs) { if (!(Test-Path -LiteralPath $lib)) { throw "Missing $lib - build the editor once first." } }
    $defineArgs = ($Defines -split ',' | Where-Object { $_ } | ForEach-Object { "/D$_" }) -join ' '
    $ppm = [System.IO.Path]::ChangeExtension((Join-Path $repo $Output), '.ppm')
    $mode = if ($Edit) { '--edit' } else { '' }
    $commandFile = Join-Path $build 'render-ui-gallery.cmd'
    @"
@echo off
call "$devcmd" >nul
cl /nologo /std:c++17 /EHsc /MD /O1 $defineArgs /I src /I include /I src/ThirdParty/ModuGUI /I src/ThirdParty/glad /I src/ThirdParty/glfw/include /I src/ThirdParty/glm tools/ui_gallery.cpp $($libs -join ' ') /Fe:"$build/ui_gallery.exe" /Fo:"$build/ui_gallery.obj" /link opengl32.lib user32.lib imm32.lib gdi32.lib shell32.lib
if errorlevel 1 exit /b 1
"$build/ui_gallery.exe" "$ppm" $mode
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $commandFile
    & cmd /c $commandFile
    if ($LASTEXITCODE) { throw "UI gallery failed ($LASTEXITCODE)." }
    & python -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" $ppm (Join-Path $repo $Output)
    if ($LASTEXITCODE) { throw 'PPM to PNG conversion failed (needs Python + Pillow).' }
    Write-Host "Wrote $Output"
} finally { Pop-Location }
