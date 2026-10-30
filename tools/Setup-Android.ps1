#Requires -Version 5.1
<#
Install the command-line Android toolchain into this checkout (no Android Studio
or machine-wide environment changes). Run with -AcceptLicenses after reviewing
https://developer.android.com/studio/terms . SDK archives are verified against
Google's repository metadata before extraction.
#>
[CmdletBinding()]
param(
    [string] $SdkRoot = '',
    [string] $NdkVersion = '30.0.16248370',
    [string] $BuildToolsVersion = '36.0.0',
    [int] $PlatformApi = 36,
    [switch] $AcceptLicenses,
    [switch] $CheckOnly
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
if (-not $SdkRoot -and $CheckOnly) {
    $roots = @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME, (Join-Path (Split-Path $PSScriptRoot -Parent) 'Build-Android\android-sdk'))
    if ($env:LOCALAPPDATA) { $roots += Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
    $SdkRoot = $roots | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'platforms')) -and (Test-Path -LiteralPath (Join-Path $_ 'build-tools')) } | Select-Object -First 1
    if (-not $SdkRoot) { throw 'Android SDK not found. Run tools\Setup-Android.ps1 first, or set ANDROID_SDK_ROOT to an installed SDK.' }
}
if (-not $SdkRoot) { $SdkRoot = Join-Path (Split-Path $PSScriptRoot -Parent) 'Build-Android\android-sdk' }
$SdkRoot = [IO.Path]::GetFullPath($SdkRoot)

$javaRoots = @($env:JAVA_HOME)
if ($env:ProgramFiles) {
    $javaRoots += Join-Path $env:ProgramFiles 'Java\latest'
    foreach ($vendor in @('Java', 'Eclipse Adoptium', 'Microsoft')) {
        $dir = Join-Path $env:ProgramFiles $vendor
        if (Test-Path -LiteralPath $dir) {
            $javaRoots += @(Get-ChildItem -LiteralPath $dir -Directory | Sort-Object Name -Descending | ForEach-Object { $_.FullName })
        }
    }
}
$javac = Get-Command javac.exe -CommandType Application -ErrorAction SilentlyContinue
if ($javac) { $javaRoots += Split-Path (Split-Path $javac.Source -Parent) -Parent }
$jdk = $javaRoots | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'bin\javac.exe')) } | Select-Object -First 1
if (-not $jdk) { throw 'A JDK (21 or newer) is required. Install one or set JAVA_HOME to its directory, then retry.' }
$env:JAVA_HOME = $jdk
$env:PATH = (Join-Path $jdk 'bin') + ';' + $env:PATH

if ($CheckOnly) {
    $ndkRoots = @($env:ANDROID_NDK_ROOT, $env:ANDROID_NDK_HOME, $env:ANDROID_NDK)
    foreach ($root in @($SdkRoot, (Join-Path (Split-Path $PSScriptRoot -Parent) 'Build-Android\android-sdk'))) {
        $sideBySide = Join-Path $root 'ndk'
        if (Test-Path -LiteralPath $sideBySide) {
            $ndkRoots += @(Get-ChildItem -LiteralPath $sideBySide -Directory | Where-Object { $_.Name -match '^\d+(\.\d+){1,3}$' } | Sort-Object { [version]$_.Name } -Descending | ForEach-Object { $_.FullName })
        }
    }
    $ndk = $ndkRoots | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'build\cmake\android.toolchain.cmake')) } | Select-Object -First 1
    if (-not $ndk) { throw 'Android NDK not found. Run tools\Setup-Android.ps1 first, or set ANDROID_NDK_ROOT.' }
    $env:ANDROID_SDK_ROOT = $SdkRoot
    $env:ANDROID_HOME = $SdkRoot
    $env:ANDROID_NDK_ROOT = $ndk
    Write-Host "Android SDK: $SdkRoot"
    Write-Host "Android NDK: $ndk"
    return
}

New-Item -ItemType Directory -Force -Path $SdkRoot | Out-Null
$manager = Join-Path $SdkRoot 'cmdline-tools\latest\bin\sdkmanager.bat'
if (-not (Test-Path -LiteralPath $manager)) {
    Write-Host 'Downloading Android command-line tools from Google...'
    $response = Invoke-WebRequest -UseBasicParsing 'https://dl.google.com/android/repository/repository2-3.xml'
    [xml] $index = $response.Content
    $package = $index.SelectSingleNode("//*[local-name()='remotePackage' and @path='cmdline-tools;latest']")
    $archive = $package.SelectSingleNode("./archives/archive[host-os='windows']/complete")
    if (-not $archive) { throw 'Google repository metadata contains no Windows command-line tools archive.' }
    $url = 'https://dl.google.com/android/repository/' + $archive.url
    $zip = Join-Path $SdkRoot 'commandline-tools.zip'
    Invoke-WebRequest -UseBasicParsing $url -OutFile $zip
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA1).Hash
    if ($hash -ne $archive.checksum.InnerText) { throw 'Android command-line tools checksum mismatch; refusing to extract.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $extract = Join-Path $SdkRoot ('cmdline-staging-' + [Guid]::NewGuid().ToString('N'))
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $extract)
    $toolsRoot = Join-Path $SdkRoot 'cmdline-tools'
    New-Item -ItemType Directory -Force -Path $toolsRoot | Out-Null
    if (Test-Path -LiteralPath (Join-Path $toolsRoot 'latest')) { throw 'Incomplete cmdline-tools/latest already exists; choose a clean SDK root.' }
    Move-Item -LiteralPath (Join-Path $extract 'cmdline-tools') -Destination (Join-Path $toolsRoot 'latest')
}

# Native programs commonly emit progress/warnings on stderr. Their exit status,
# rather than PowerShell's NativeCommandError wrapping, determines success.
$ErrorActionPreference = 'Continue'
if ($AcceptLicenses) {
    1..100 | ForEach-Object { 'y' } | & $manager "--sdk_root=$SdkRoot" --licenses
} else {
    & $manager "--sdk_root=$SdkRoot" --licenses
}
if ($LASTEXITCODE -ne 0) { throw 'Android SDK license setup failed.' }
# A package file keeps semicolons intact through PowerShell 5.1 and the Windows
# batch wrapper, including newer wrappers which forward to the Android CLI.
$packages = Join-Path $SdkRoot 'modularity-packages.txt'
@('platform-tools', "platforms;android-$PlatformApi", "build-tools;$BuildToolsVersion", "ndk;$NdkVersion", 'cmake;4.1.2') | Set-Content -LiteralPath $packages -Encoding ASCII
if ($AcceptLicenses) {
    1..100 | ForEach-Object { 'y' } | & $manager "--sdk_root=$SdkRoot" "--package_file=$packages"
} else {
    & $manager "--sdk_root=$SdkRoot" "--package_file=$packages"
}
if ($LASTEXITCODE -ne 0) { throw 'Android SDK package installation failed.' }
$ErrorActionPreference = 'Stop'
foreach ($required in @("ndk\$NdkVersion\build\cmake\android.toolchain.cmake", "platforms\android-$PlatformApi\android.jar", "build-tools\$BuildToolsVersion\apksigner.bat", 'cmake\4.1.2\bin\ninja.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $SdkRoot $required))) { throw "Android SDK component missing after installation: $required" }
}
$env:ANDROID_SDK_ROOT = $SdkRoot
$env:ANDROID_HOME = $SdkRoot
$env:ANDROID_NDK_ROOT = Join-Path $SdkRoot "ndk\$NdkVersion"
$env:PATH = (Join-Path $SdkRoot 'cmake\4.1.2\bin') + ';' + (Join-Path $SdkRoot 'platform-tools') + ';' + $env:PATH
Write-Host "Android SDK ready: $SdkRoot"
Write-Host "NDK: $env:ANDROID_NDK_ROOT"
Write-Host 'Build an ARM64 APK with .\build.bat --Android --jobs=4'
