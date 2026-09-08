# OpenXR (vendored)

Two separate things live here, both from Khronos and both pinned to **1.1.62**:

| What | Where | Source |
| --- | --- | --- |
| API headers | `include/openxr/` | [KhronosGroup/OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK), tag `release-1.1.62` |
| Android loader | `lib/<abi>/libopenxr_loader.so` | `pkg:maven/org.khronos.openxr/openxr_loader_for_android@1.1.62` |

Keep the two on the same release. A loader newer than the headers is fine; the
reverse is not, because the headers may declare entry points the loader cannot
resolve.

License: Apache-2.0 OR MIT. The loader AAR's own licence text is vendored
alongside as `LICENSE`.

## Headers

- `include/openxr/openxr.h`
- `include/openxr/openxr_platform.h`
- `include/openxr/openxr_platform_defines.h`

`openxr_reflection.h` is deliberately not vendored (548 KB of macro tables we do
not use). Result and structure-type names come from the runtime's own
`xrResultToString` / `xrStructureTypeToString`, with a small built-in fallback
table in `XRDiagnostics.cpp` for the case where the instance is not up yet.

### Updating the headers

    git clone --depth 1 --branch release-X.Y.Z \
        https://github.com/KhronosGroup/OpenXR-SDK.git
    cp OpenXR-SDK/include/openxr/openxr.h \
       OpenXR-SDK/include/openxr/openxr_platform.h \
       OpenXR-SDK/include/openxr/openxr_platform_defines.h \
       src/ThirdParty/openxr/include/openxr/

## Android loader

Prebuilt for every ABI Modularity's Android export can target. Quest is
`arm64-v8a`; the others are carried so that choosing a different ABI in Build
Settings does not turn into a missing-loader error at export time.

    lib/arm64-v8a/libopenxr_loader.so      sha256 50d69917…5211da1e
    lib/armeabi-v7a/libopenxr_loader.so    sha256 32d22b52…335461e4
    lib/x86_64/libopenxr_loader.so         sha256 a6106099…668c23b64
    lib/x86/libopenxr_loader.so            sha256 250968b9…cea95cfe

These are **git-lfs** objects (`*.so` is tracked in `.gitattributes`). A clone
without `git lfs pull` gets pointer files, and the Android export will fail with
a message saying so.

`Engine.cpp`'s `stageAndroidOpenXrLoader` copies the right one into the APK's
`lib/<abi>/` whenever a project has OpenXR enabled. It searches, in order:

1. `$MODULARITY_OPENXR_LOADER_DIR/<abi>/` (or that directory directly)
2. `redist/openxr/<abi>/`
3. `src/ThirdParty/openxr/lib/<abi>/`  ← these files

so a developer can override the vendored loader (typically to test against a
vendor loader from the Meta OpenXR Mobile SDK) without editing the engine.

### Updating the loader

    ./tools/fetch-openxr-loader.sh          # refetch the pinned version
    ./tools/fetch-openxr-loader.sh 1.1.64   # bump

The script verifies the AAR against its published SHA1 before extracting.

## Windows loader (PCVR)

Not vendored, and not for the usual reason. Windows has **no system-wide OpenXR
loader**: installing SteamVR or the Meta app gives you a *runtime*, and the
Khronos loader is expected to ship with the application. So a PC VR build needs
`openxr_loader.dll` next to `Modularity.exe`.

    ./tools/fetch-openxr-loader-windows.sh          # into redist/windows-x64/
    ./tools/fetch-openxr-loader-windows.sh 1.1.64   # bump

It comes from the `.nupkg` attached to the **OpenXR-SDK GitHub release**, not
from nuget.org - the `OpenXR.Loader` package published there stopped at
1.0.10.2, and a loader older than these headers is the one combination that does
not work. Currently pinned to **1.1.63** (one patch ahead of the headers, which
is the safe direction). Anything in
`redist/windows-x64/` is copied next to the executables at build time, and
`XRLoader` also probes `redist/windows-x64/` directly so running from the source
tree works without a copy step. The DLL is deliberately not committed: like everything
else in `redist/`, it is a platform redistributable each build host populates
for itself (see `redist/README.md`).

With the loader missing, Project Settings > OpenXR Manager > Runtime Diagnostics
reports `Loader: Not found` and says what to do about it. Nothing else in the
editor is affected.

### The AAR's manifest is NOT merged

Gradle would merge the loader AAR's `AndroidManifest.xml` into the app manifest.
Modularity builds its APK directly with `aapt2` and has no manifest merger, so
everything that manifest declares is written out by hand in
`writeAndroidManifest` instead: the two `org.khronos.openxr.permission.*`
permissions, the runtime-broker `<provider>` query, and the
`OpenXRRuntimeService` / `OpenXRApiLayerService` `<intent>` queries. **If you bump
the loader, diff its `AndroidManifest.xml` against that function** - a new query
entry that does not get copied across shows up as the loader mysteriously finding
no runtime on-device.

## Why the loader is dlopen'd rather than linked

Modularity never links against `libopenxr_loader`. `src/XR/XRLoader.*` opens it
with `dlopen` at runtime, resolves `xrGetInstanceProcAddr`, and builds its own
dispatch table from there. Three reasons this matters:

1. A build with OpenXR compiled in has **no new link-time dependency**, so
   enabling OpenXR support cannot break a desktop or Android build that has no
   OpenXR runtime installed.
2. Which loader an APK uses stays a packaging decision, not a compile-time one -
   which is what makes the override paths above possible at all.
3. When no runtime is present, `XRLoader` reports unavailable and the engine
   stays on its normal non-XR path instead of failing to start.
