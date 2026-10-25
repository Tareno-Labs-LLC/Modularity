#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $repo 'build.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw $parseErrors }
foreach ($name in @('Invoke-BuildPipeline', 'Get-PackageBinaryCandidates')) {
    $function = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name }, $true)
    Invoke-Expression $function.Extent.Text
}
$parser = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.ForEachStatementAst] -and $n.Variable.VariablePath.UserPath -eq 'arg' }, $true)
$script:calls = New-Object 'System.Collections.Generic.List[object]'
$script:stopAtSync = $false
function Invoke-ExternalStep($Label, $FilePath, $StepArguments) {
    $script:calls.Add(@{Label=$Label; File=$FilePath; Args=@($StepArguments)})
    return -not ($script:stopAtSync -and $Label -eq 'Syncing Submodules')
}
function Invoke-InlineStep($Label, $Action) { & $Action; return $true }
function Write-Info($Value) {}
$root = $repo
$buildDir = Join-Path $repo 'build'
$playerCacheDir = Join-Path $buildDir 'player-cache'
$generatorArgs = @(); $vsGlobalsArgs = @(); $monoArgs = @(); $buildExtraArgs = @()
$buildType = 'Release'; $buildCppStandard = 'c++23'; $discordSdk = 'OFF'; $jobs = 3
$cleanBuild = $false; $skipPlayer = $true
foreach ($case in @(
    @{Flags=@('--Android'); Abi='arm64-v8a'; Debug=$false; Editor=$false},
    @{Flags=@('--Android','--abi=x86_64','--debug','--editor','--project=C:\Game With Spaces\project.modu','--output=C:\Game With Spaces\game.apk'); Abi='x86_64'; Debug=$true; Editor=$true})) {
    $androidBuild=$false; $androidAbi='arm64-v8a'; $androidOutput=''; $androidProject=''; $androidDebug=$false; $androidEditor=$false
    $Arguments = $case.Flags
    Invoke-Expression $parser.Extent.Text
    $script:calls.Clear()
    if (-not (Invoke-BuildPipeline)) { throw 'Android pipeline failed' }
    $build = @($script:calls | Where-Object { $_.Label -eq 'Building Editor' })[0]
    if ($build.Args -notcontains 'Modularity') { throw 'Android must build the native editor target' }
    $apk = @($script:calls | Where-Object { $_.Label -eq 'Building Android APK' })[0]
    if ($apk.Args -notcontains '--build-android' -or $apk.Args -notcontains "--abi=$($case.Abi)") { throw 'Missing Android exporter arguments' }
    if (($apk.Args -contains '--debug') -ne $case.Debug -or ($apk.Args -contains '--editor') -ne $case.Editor) { throw 'Android mode flags lost' }
    if ($case.Editor -and $apk.Args -notcontains '--project=C:\Game With Spaces\project.modu') { throw 'Project path with spaces was split' }
    if ($script:calls.Count -ne 4) { throw 'Unexpected player build or packaging command in Android pipeline' }
    Write-Output "PASS: Windows Android pipeline [$($case.Flags -join ' ')]"
}

$scratch = Join-Path $repo ('build\android-selftest\driver-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path (Join-Path $scratch 'android-sdk\nested'), (Join-Path $scratch 'native') | Out-Null
Set-Content -LiteralPath (Join-Path $scratch 'android-sdk\nested\compiler.dll') 'SDK marker'
Set-Content -LiteralPath (Join-Path $scratch 'native\runtime.dll') 'runtime marker'
$script:BinaryScanCache=@{}; $script:ExcludedLibs=@('core.lib','core_player.lib')
$binaries=Get-PackageBinaryCandidates $scratch
if ($binaries.Dlls.Count -ne 1 -or $binaries.Dlls[0].Name -ne 'runtime.dll') { throw 'Android toolchain DLLs leaked into desktop packaging' }
Write-Output 'PASS: Android SDK binaries excluded from desktop package candidates'

# Exercise the real clean callback only on this uniquely named fixture directory.
$cleanBuild=$true; $buildDir=$scratch; $playerCacheDir=Join-Path $scratch 'player-cache'; $script:stopAtSync=$true
$resolved=[IO.Path]::GetFullPath($scratch)
$prefix=[IO.Path]::GetFullPath((Join-Path $repo 'build\android-selftest')).TrimEnd('\')+'\'
if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture must stay inside android-selftest' }
$null=Invoke-BuildPipeline
if (-not (Test-Path -LiteralPath (Join-Path $scratch 'android-sdk\nested\compiler.dll'))) { throw 'Clean removed the SDK cache' }
if (Test-Path -LiteralPath (Join-Path $scratch 'native\runtime.dll')) { throw 'Clean left engine build outputs' }
Write-Output 'PASS: clean removes engine outputs while preserving the Android SDK cache'
