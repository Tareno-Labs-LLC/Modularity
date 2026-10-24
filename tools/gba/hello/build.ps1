# build.ps1 - builds the hello GBA ROM with the Arm GNU Toolchain.
#
#   .\build.ps1          build build\hello.gba
#   .\build.ps1 -Run     build, then open it in mGBA
#
# Toolchain: $env:MODULARITY_GBA_TOOLCHAIN, else the newest arm-gnu-toolchain-*-arm-none-eabi
# under %LOCALAPPDATA%\Modularity\Toolchains. mGBA is looked up in the same folder.
#
# Besides the ROM it writes build\hello.lst (every function's ARM/Thumb code next to the source
# line it came from) and build\main.s, and it fails the build if any software floating-point
# routine was linked in - the "no decimals reach the GBA" rule, checked rather than trusted.
param([switch]$Run)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$runtime = (Resolve-Path (Join-Path $here '..\runtime')).Path
$out = Join-Path $here 'build'
$toolRoot = Join-Path $env:LOCALAPPDATA 'Modularity\Toolchains'

$tc = $env:MODULARITY_GBA_TOOLCHAIN
if (-not $tc) {
    $found = Get-ChildItem -Path $toolRoot -Directory -Filter 'arm-gnu-toolchain-*arm-none-eabi' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($found) { $tc = $found.FullName }
}
if (-not $tc -or -not (Test-Path (Join-Path $tc 'bin\arm-none-eabi-g++.exe'))) {
    throw "Arm GNU Toolchain not found. Set MODULARITY_GBA_TOOLCHAIN or unpack it under $toolRoot."
}
$bin = Join-Path $tc 'bin'
$gxx = Join-Path $bin 'arm-none-eabi-g++.exe'
$objcopy = Join-Path $bin 'arm-none-eabi-objcopy.exe'
$objdump = Join-Path $bin 'arm-none-eabi-objdump.exe'
$nm = Join-Path $bin 'arm-none-eabi-nm.exe'
$size = Join-Path $bin 'arm-none-eabi-size.exe'

function Invoke-Tool([string]$exe, [string[]]$toolArgs) {
    & $exe @toolArgs
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed (exit $LASTEXITCODE)" }
}

New-Item -ItemType Directory -Force -Path $out | Out-Null

$arch = @('-mcpu=arm7tdmi', '-mthumb')
$cxx = $arch + @('-O2', '-std=c++20', '-ffreestanding', '-fno-exceptions', '-fno-rtti',
    '-fno-threadsafe-statics', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
    '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra')

$crt0 = Join-Path $runtime 'gba_crt0.s'
$rtSrc = Join-Path $runtime 'modu_gba.cpp'
$mainSrc = Join-Path $here 'main.cpp'
$crt0Obj = Join-Path $out 'gba_crt0.o'
$rtObj = Join-Path $out 'modu_gba.o'
$mainObj = Join-Path $out 'main.o'
$elf = Join-Path $out 'hello.elf'
$rom = Join-Path $out 'hello.gba'

Write-Host '[gba] compiling'
Invoke-Tool $gxx ($arch + @('-x', 'assembler-with-cpp', '-c', $crt0, '-o', $crt0Obj))
Invoke-Tool $gxx ($cxx + @('-g', '-c', $rtSrc, '-o', $rtObj))
Invoke-Tool $gxx ($cxx + @('-g', '-c', $mainSrc, '-o', $mainObj))
Invoke-Tool $gxx ($cxx + @('-S', $mainSrc, '-o', (Join-Path $out 'main.s')))

Write-Host '[gba] linking'
Invoke-Tool $gxx ($arch + @('-nostdlib', '-nostartfiles', '-T', (Join-Path $runtime 'gba.ld'),
    '-Wl,--gc-sections', '-Wl,--no-warn-rwx-segments', "-Wl,-Map,$(Join-Path $out 'hello.map')",
    '-o', $elf, $crt0Obj, $rtObj, $mainObj, '-lgcc'))
Invoke-Tool $objcopy @('-O', 'binary', $elf, $rom)

# Header checksum: 0xBD = -(sum of bytes 0xA0..0xBC) - 0x19.
$bytes = [System.IO.File]::ReadAllBytes($rom)
$sum = 0
for ($i = 0xA0; $i -le 0xBC; $i++) { $sum += $bytes[$i] }
$bytes[0xBD] = [byte]((0 - $sum - 0x19) -band 0xFF)
[System.IO.File]::WriteAllBytes($rom, $bytes)

# No decimals at runtime: refuse a ROM that links any soft-float helper.
$symbols = & $nm $elf
$softFloat = $symbols | Where-Object { $_ -match '__aeabi_([fd](add|sub|rsub|mul|div|neg|cmp|2)|u?[il]2[fd])|__(add|sub|mul|div)[sd]f3|__float|__fix(uns)?[sd]f' }
if ($softFloat) {
    $softFloat | ForEach-Object { Write-Host "  $_" }
    throw 'Software floating point was linked into the ROM (listed above). Use fx, Px(), PxPerSecond().'
}

& $objdump @('-d', '-S', '-C', '--no-show-raw-insn', $elf) | Set-Content -Encoding ascii (Join-Path $out 'hello.lst')

Write-Host '[gba] sections (bytes):'
& $size @('-A', $elf) | Where-Object { $_ -match '^\.(crt0|text|rodata|iwram|bss|ewram|init_array|ARM)' } |
    ForEach-Object { Write-Host "  $_" }
Write-Host ("[gba] built {0} ({1} bytes), no soft-float routines linked" -f $rom, $bytes.Length)

if ($Run) {
    $mgba = Get-ChildItem -Path $toolRoot -Recurse -Filter 'mGBA.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $mgba) { throw "mGBA.exe not found under $toolRoot" }
    Start-Process -FilePath $mgba.FullName -ArgumentList "`"$rom`""
}
