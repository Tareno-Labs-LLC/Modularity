# Bundled redistributable DLLs
## *Note Written by Anémunt and Claude Code*

---

Every `*.dll` placed in the arch-matching folder here is copied next to the
Windows executables (`Modularity` and `ModularityPlayer`) as a POST_BUILD step.
Populate these folders on your build host; the builder picks the directory that
matches the target architecture automatically and copies whatever it finds.
| Build architecture | Folder                |
| ------------------ | --------------------- |
| 64-bit (x86_64)    | `redist/windows-x64/` |
| 32-bit (x86)       | `redist/windows-x86/` |

The folder may be empty or absent, the step is skipped when there's nothing to
copy, so the repo builds fine without any binaries committed.

To point at a different directory:
```
-DMODULARITY_REDIST_DLL_DIR=/path/to/dlls
```

To disable bundling entirely:
```
-DMODULARITY_BUNDLE_REDIST_DLLS=OFF
```

## What to put here
### Mesa software OpenGL (`opengl32.dll`)
Modularity's renderer uses the ImGui `opengl3` backend and requires a context of
at least **OpenGL 3.3 / GLSL 330** (the engine's shaders are `#version 330 core`;
see [`include/Graphics/OpenGL.h`](../include/Graphics/OpenGL.h) and
[`src/WinView/Window.cpp`](../src/WinView/Window.cpp)). On machines whose system
OpenGL is older, most notably **Windows Vista, which ships only the GL 1.1
`opengl32.dll`**, the engine cannot create a usable context against the system
driver.

Drop a **Mesa llvmpipe** `opengl32.dll` (a software rasterizer exposing GL 4.x)
here, named **exactly** `opengl32.dll`. Because `opengl32.dll` is **not** on the
Windows KnownDLLs list, the copy in the application directory is loaded ahead of
`C:\Windows\System32\opengl32.dll`, no registry changes or loader shims needed.

> ⚠️ **Do not** drop Vista's own `opengl32.dll` here, that *is* the GL 1.1 DLL.
> we're overriding. Only a Mesa (or other GL 3.3+) software DLL helps.

### OpenXR loader (`openxr_loader.dll`)
Needed for **PC VR**, and for a reason that catches everyone out: Windows has no
system-wide OpenXR loader. Installing SteamVR or the Meta app gives you a
*runtime*; the Khronos loader is expected to travel with the application. Without
it, Modularity reports `No OpenXR loader found` on a machine where VR obviously
works.

Fetch it with:
Fetching OpenXR loader 1.1.63 from the OpenXR-SDK release...
  windows-x64    66946ea6e9cc2649e299ec412521c5eafe56c635d05055097fe4db81e11932df
  windows-x86    d4dd992e067eaabbdfa651c90eb4578e44db6651fb4539aec0964dbc22bcfdba

OpenXR loader 1.1.63 placed in redist/. Rebuild (or re-run CMake) so it is
copied next to Modularity.exe, then check Project Settings > OpenXR Manager >
Runtime Diagnostics - the Loader row should stop saying "Not found".
which drops the official Khronos build here. `XRLoader` also probes
`redist/windows-x64/` directly, so a build run from the source tree finds it even
before the post-build copy. See
[`src/ThirdParty/openxr/README.md`](../src/ThirdParty/openxr/README.md).

### Toolchain runtime DLLs
A fresh Vista box won't be missing Vista's own system DLLs (`kernel32`, `user32`,
`gdi32`, …), those are already present and aren't ours to redistribute. What it
may lack is the **compiler runtime**, so drop those here too:

- **MinGW build:** `libgcc_s_seh-1.dll` (or `libgcc_s_dw2-1.dll`),
  `libstdc++-6.dll`, `libwinpthread-1.dll`
- **MSVC build:** the VC++ redist (`vcruntime140.dll`, `msvcp140.dll`, …).
  Mind the version: VS2015/2017 redists still support Vista; **VS2019+ dropped
  Vista**.

## ⚠️ Vista compatibility caveat

Current `mesa-dist-win` / LLVM builds target **Windows 7+** and import symbols
that don't exist on Vista, so a freshly-downloaded `opengl32.dll` may fail to
load on Vista with a missing-import error. For a Vista target you'll likely need
an **older Mesa build** that still links against the Vista-era runtime, or a Mesa
build produced with a Vista toolchain target. Verify the DLL actually loads on
the target before assuming it works.

Architecture must match the executable: a 64-bit build needs 64-bit DLLs, a
32-bit build needs 32-bit ones. (Note: `build.sh --windows` currently
cross-compiles **x86_64 only**; a 32-bit Vista target would also need an
`i686-w64-mingw32` toolchain path added to the build script. Building natively on
Windows with MinGW or an old MSVC sidesteps that.)
