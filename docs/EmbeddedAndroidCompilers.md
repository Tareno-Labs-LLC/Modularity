# Embedded Android script compilers

The Android editor can embed **Cling/LLVM for C++ and ModuCPP** and **TinyCC for C**.
This is an editor feature. Exported Android games still use NDK-compiled native
libraries and do not carry either compiler.

## Setup

Install the Android SDK/NDK first as described in [AndroidRuntime.md](AndroidRuntime.md).
The compiler setup also needs Python 3.12+, CMake, Ninja, and a native C++ compiler
for LLVM's host table generators. On Windows it discovers Visual Studio's C++
tools without changing machine-wide environment variables.

From the repository root:

```powershell
python tools/setup-embedded-compilers.py --jobs 3
.\build.bat --Android --editor --abi=arm64-v8a --jobs=3
```

The first command downloads pinned official TinyCC, Cling, and Cling's matching
LLVM sources; builds host code generators and target ARM64 libraries; and stages
the NDK sysroot, C/C++ headers, current engine script SDK, compiler resources,
runtime helpers, and license notices. The LLVM source build is substantial;
subsequent runs reuse the build cache. `--fetch` downloads sources only, and
`--tinycc-only` builds the smaller C compiler/probe without starting LLVM.

The default cache is `Build-Android/embedded-compilers` (next to `build/`, not inside it), and the
completed target prefix is `Build-Android/embedded-compilers/arm64-v8a`. Clean builds never touch it.
Its CMake build trees record absolute paths, so if the cache is moved the script notices
(`fresh_if_moved`) and rebuilds those trees from scratch, keeping the downloaded sources.
Set `MODULARITY_EMBEDDED_COMPILERS_ROOT` to use a different completed ARM64 prefix.
The APK exporter discovers the default prefix automatically. Both build drivers
prepare it for `--Android --editor` when it is missing.

For a direct CMake editor build, pass
`-DMODULARITY_ENABLE_EMBEDDED_COMPILERS=ON` and
`-DMODULARITY_EMBEDDED_COMPILERS_ROOT=<prefix>`. The prefix must match the target
architecture; a desktop Cling installation is not an Android dependency.
An explicitly supplied `MODULARITY_ANDROID_CLANG_DIR` retains the external Clang
route for custom builds.

## C and C++ context handling

The existing transpiler and wrapper generator remain responsible for script
hooks and context signatures:

- A `.moducpp` unit becomes C++ and its generated wrapper is compiled by Cling.
- A C++ unit uses Cling with the existing `ScriptContext` wrapper.
- A `.c` unit is compiled as C by TinyCC. Its generated C++ context wrapper is
  compiled by Cling, which links the C object into the same JIT module. The C
  preprocessor sees C conditions, and the wrapper sees C++ conditions.

This preserves the existing `ModuScriptContext` bridge, including hooks with
optional context/delta-time arguments. TinyCC is not asked to parse C++ engine
headers. The wrapper exports still pass the engine's ABI/layout validation;
this integration bumps the native script ABI to **62**.

Embedded artifacts record compilation requests rather than native `.so` bytes.
The editor restores the JIT after a restart using the generated sources and
bundled SDK. Recompiles get separate interpreter instances. Published instances
remain alive for the process lifetime because callbacks, objects, and vtables
may retain their code addresses after reload. Restarting the editor releases
that accumulated compiler/code memory.

The SDK extraction stamp changes with each APK, so installing an updated editor
refreshes headers and compiler resources. The current editor APK retains target
SDK 28; changing that platform policy needs separate Android device validation.

## Verification

`tools/embedded-compilers` builds `tinycc-probe` for ARM64 and can also build a
native host probe. The native probe executes a C function compiled at runtime
and checks its call back into a host function through a context pointer.

After full setup, build the mixed-language device probe:

```powershell
cmake -S tools/embedded-compilers -B build/embedded-compilers/probe-arm64 -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared `
  -DMODULARITY_TINYCC_SOURCE_DIR=<cache>/sources/tinycc-e23dabed154a8495f3ac1ff3cc9d04fa1c207893 `
  -DMODULARITY_ENABLE_EMBEDDED_COMPILERS=ON `
  -DMODULARITY_EMBEDDED_COMPILERS_ROOT=<prefix> -DMODULARITY_BUILD_EMBEDDED_PROBE=ON
cmake --build build/embedded-compilers/probe-arm64 --target embedded-probe --parallel 3
```

Run `embedded-probe <extracted bundle directory> <writable scratch directory>`
on an ARM64 device with the prefix's shared libraries and NDK `libc++_shared.so`
available to the loader. It checks that TinyCC C calls its Cling C++ context
bridge and that an old callback remains valid after recompilation.

`python tools/android-selftest/run.py --scripts` separately verifies native
Windows compiler selection, embedded C/C++ request routing, and real NDK builds
of C API, C++, and ModuCPP scripts. A successful cross-build alone does not verify
JIT execution or Android memory-permission behavior on a device.
