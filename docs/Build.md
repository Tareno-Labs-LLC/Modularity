# Building & Testing Modularity
In Simple: `build.sh` is the entry point for all builds. It locates the repo root, installs system dependencies, syncs submodules + git-lfs, configures CMake, builds, and packages.
See `CLAUDE.md` for the full flag list. (it's human readable too, no worries!)
```bash
./build.sh                       # Native (Linux) Release build [faster due to the default packaging being zip]
./build.sh --7z                  # Builds with Packaging using 7z, much slower to compress, but much smaller as well
./build.sh --clean               # Wipe build dirs first
./build.sh --build-type=Debug    # Builds in Debug Mode
./build.sh --fsanitize           # AddressSanitizer + UBSan (-DMODULARITY_ENABLE_ASAN=ON)
./build.sh --low                 # Low-end profile: PhysX, Vulkan, OpenXR and Jolt LTO off (auto on <=2-thread CPUs)
./build.sh --full                # Every module, even on a <=2-thread CPU; undoes a previous --low
./build.sh --lto / --no-lto      # Force JoltPhysics LTO on/off (auto: off on --low and <=2-thread CPUs)
./build.sh --Windows             # Cross-compile Windows via MinGW-w64
./build.sh --Android             # Build an installable Android .apk (arm64-v8a)
./build.sh --jobs=N              # Parallel jobs (default: nproc-2)
./build.sh --generator=Ninja     # Builds using Ninja
./buildandrun.sh                 # Build.sh then run the resulting binary
```

## Windows (`build.ps1`) output layout
`build.ps1` (or the `build.bat` shim) leaves exactly three things in `build\`:

| Path | What it is |
| --- | --- |
| `Dev-ExportedBuild\` | The runnable editor and player. The linker writes `Modularity.exe` and `ModularityPlayer.exe` straight here; POST_BUILD steps add the DLLs, `Resources\` and the Script SDK. Run the editor from here. |
| `Dev-Resources\` | Everything you only open to debug: the CMake tree, `.pdb` files and the import `.lib` scripts link against (`Dev-Resources\Release\`), the `player-cache\` tree game exports reuse, and `github-publish` (kept by `--clean`). |
| `Modularity V7.0 - Windows.zip` | The release archive, built by CPack from the install rules (not a copy of `Dev-ExportedBuild`): no PDBs, import libs, PhysX SDK headers or unused FFmpeg DLLs. `Modularity.exe` sits at the top of the archive's folder. |

```powershell
.\build.ps1                  # Release build + "Modularity V7.0 - Windows.zip"
.\build.ps1 --version=V7.1   # change the release label
.\build.ps1 --7z             # .7z instead of .zip (smaller, slower)
.\build.ps1 --clean          # wipe Dev-Resources (keeping github-publish) and Dev-ExportedBuild; Build-Android is untouched
```

Android lives outside `build\` entirely, in `<repo>\Build-Android\`:

| Path | What it is |
| --- | --- |
| `android-sdk\` | SDK + NDK from `tools\Setup-Android.ps1` (~2.6 GB). |
| `embedded-compilers\` | Cling/LLVM/TinyCC sources and build trees for the Android editor (`tools\setup-embedded-compilers.py`); the finished bundle is `arm64-v8a\`. |
| `android-clang\` | Optional Termux clang bundle (`build.sh`). |
| `APK-Output\` | APKs, their build trees, and `signing\debug.keystore` that installed APKs are signed with. |

The Cling/LLVM build trees record absolute paths, so moving `embedded-compilers`
makes the setup script rebuild them from scratch; the downloaded sources are kept.

The first run on a checkout that still has the old flat `build\` tree moves the
Android folders into `Build-Android\`, deletes what CMake/CPack generated there, and
lists anything else it found instead of deleting it. Because the CMake tree
moves, that first build compiles everything from scratch. `build.sh` (Linux)
keeps the flat `build/` layout; the engine looks in `build/Dev-Resources/` first
and falls back to `build/` (`src/DevBuildDir.h`).

### Debug info (why `Dev-Resources` is the size it is)
MSVC builds compile with `/Zi /FS`: every `.obj` keeps its symbols and line tables, so
crash stacks from optimised builds still show function names and source lines, but type
records are written once per target into a shared compiler PDB. `/Z7` (the old setting)
repeated them in every object and again in each static `.lib`; they were ~64% of the debug
info, and the reason the Windows tree dwarfed Linux's (which builds without `-g`). A Ninja or
Makefile build using ccache/sccache keeps `/Z7`, the only format those can cache.

## AddressSanitizer builds (`--asan`)
```powershell
.\build.ps1 --asan                     # also --ASANDEBUG or --fsanitize (build.sh's spelling)
.\build.ps1 --asan --build-type=Debug  # unoptimised, if you need to step through it
```
This builds an instrumented editor and player into `<repo>\Build-ASAN\`, laid out like
`build\` (`Dev-Resources\`, `Dev-ExportedBuild\`) but never packaged. Expect them to run
2-3x slower and use far more memory. MSVC's ASan has no UBSan, and it is applied to every
target, third-party included: the STL marks instrumented objects differently, so mixing them
fails to link (LNK2038 `annotate_vector`), and it means overflows from inside
PhysX/Jolt/assimp are caught too. `HeapAlloc` users are covered as well.

The editor and player are GUI programs, so ASan reports are written beside the exe as
`Build-ASAN\Dev-ExportedBuild\asan-report.<pid>` (`src/AsanReportPath.h`). Set
`ASAN_OPTIONS=log_path=...` to send them elsewhere; MSVC's runtime ignores
`__asan_default_options`. The ASan runtime DLL is copied next to the exes automatically.

## Android (.apk)

```bash
./build.sh --Android                       # bare player APK
./build.sh --Android --project=Game.modu   # package a game into the APK
./build.sh --Android --project=Game.modu -o ~/Game.apk --debug
./build.sh --Android --abi=arm64-v8a       # default ABI; override if you must
```

Needs the Android NDK + SDK with `ANDROID_NDK_ROOT` and `ANDROID_SDK_ROOT` (or
`ANDROID_NDK_HOME` / `ANDROID_HOME`) set. If no SDK platform is installed it'll
pull one with `sdkmanager "platforms;android-34"`. The full story (runtime, touch,
how scripts get cross-compiled and loaded on device) is in
[AndroidRuntime.md](AndroidRuntime.md).

## CPU / ISA target (x86_64 compatibility)
**Public Linux releases target baseline x86-64 so they run on any 64-bit x86 CPU.**
This is the default and you normally do not need to touch it.
Higher microarchitecture levels bake in newer instruction sets (SSE4, AVX, AVX2, AVX-512).
A binary built for a level the running CPU doesn't support
is rejected by the dynamic loader at startup with: `./Modularity: CPU ISA level is lower than required`
(or crashes later with `SIGILL`).

To keep releases portable, the build pins the CPU target with `MODULARITY_CPU_TARGET`:
MODULARITY_CPU_TARGET   | Compiler flags                    | Intended for
`baseline` *(default)*  | `-march=x86-64 -mtune=generic`    | **Public releases**, runs on any x86-64
`x86-64-v2`             | `-march=x86-64-v2 -mtune=generic` | ~2009+ CPUs (SSE4.2, POPCNT)
`x86-64-v3`             | `-march=x86-64-v3 -mtune=generic` | ~2015+ CPUs (AVX2, BMI, FMA)
`x86-64-v4`             | `-march=x86-64-v4 -mtune=generic` | AVX-512 CPUs only
`native`                | `-march=native -mtune=native`     | **Dev only**, tuned to *your* machine

From the build script:
```bash
./build.sh                         # baseline
./build.sh --cpu-target=x86-64-v3  # opt into a higher level
./build.sh --native                # developer perf testing: DO NOT SHIP
```
Or directly to CMake: `-DMODULARITY_CPU_TARGET=baseline`.

### Why this matters even on a "normal" `-march=x86-64`
The flag is applied **globally** (`CMAKE_C/CXX_FLAGS`), not just to engine targets,
so the vendored third-party libraries (assimp, GLFW, Dear ImGui, Jolt, zlib, PhysX)
are pinned to the same level. This is required on distributions whose compiler **defaults** to a higher ISA,

for example: **CachyOS ships GCC configured for `-march=x86-64-v3`**,
so without the explicit baseline flag every dependency would silently emit AVX2
and the release would crash on older CPUs.

Jolt is a special case: it advertises its SIMD flags (`-mavx2`, `-mavx512f`, …)
as **PUBLIC** usage requirements, which leak into the engine executable.
Its `USE_SSE4_*/USE_AVX*` switches are therefore driven entirely from
`MODULARITY_CPU_TARGET` in the top-level `CMakeLists.txt`
so the bundled physics lib can never push a release above the requested baseline.

### The CRT ISA note
On some distros (again, CachyOS and similar performance-tuned images) the C
runtime startup objects (`Scrt1.o` / `crt1.o`)
are themselves built for a high ISA level. They get linked into **every** executable and stamp a
`.note.gnu.property` "x86 ISA needed: x86-64-v3/-v4" onto the binary
regardless of `-march`. That note is what the loader checks.

For the **baseline** target the build strips this note from the produced
executables (`objcopy --remove-section .note.gnu.property`, wired as a CMake `POST_BUILD` step).
Because the baseline target also forces *all* code to
baseline ISA, the stripped binary is genuinely safe to run anywhere. Higher
targets keep the honest note as a real safety gate.
> Note: miniaudio (in `AudioSystem.cpp`) contains AVX2 code paths that are
> dispatched at runtime behind CPUID checks (`ma_has_avx2()` etc.). Those
> instructions never execute on a CPU that doesn't advertise support, hence, they
> are safe on baseline builds.

## Release verification
`tools/verify-cpu-isa.sh` is the release gate. It runs `readelf -n` over the
main executables and every bundled `.so`, prints each binary's x86 ISA notes, and **fails** (exit 1)

if any binary *requires* an ISA level above the allowed maximum.
`build.sh` runs it automatically after packaging on native Linux builds
(it is skipped for `--native`, which is dev-only).
To run it by hand:
```bash
tools/verify-cpu-isa.sh --max-level=baseline build/         # scan a dir
tools/verify-cpu-isa.sh --max-level=baseline ./Modularity   # scan a file
readelf -n ./Modularity | grep -i -A4 "x86"                 # manual spot-check
```
A passing baseline release shows `x86 ISA needed: (none)` (note stripped) or
`x86-64-baseline`, and never `x86-64-v3` / `x86-64-v4`.
If the verifier flags a **bundled third-party `.so`** (e.g. a system FFmpeg
copied from a CachyOS host) as requiring v3/v4, that library genuinely uses
those instructions and cannot be made portable by stripping a note. Rebuild it
generically or build the public release on a generic-toolchain host/container.
