# Android Runtime + APK builds

This is the doc the CMake comments and `src/AndroidRuntime/AndroidRuntime.h` keep
pointing at. It covers how Modularity runs on Android, how touch input works, and
how to actually build an installable `.apk` from the command line.

## TL;DR

```bash
./build.sh --Android                          # bare player APK (boots to no-project state)
./build.sh --Android --project=Game.modu      # your game, packaged into an APK
./build.sh --Android --project=Game.modu -o ~/Game.apk --debug
./build.sh --Android --editor -o ~/Modularity.apk
```

On Windows, install the command-line SDK/NDK once, then use the same exporter:

```powershell
# Requires a JDK (21 or newer). Review Google's SDK license before accepting it.
powershell -ExecutionPolicy Bypass -File tools\Setup-Android.ps1 -AcceptLicenses
.\build.bat --Android --jobs=4
.\build.bat --Android --project=C:\Games\Game\project.modu --output=C:\Games\Game.apk
.\build.bat --Android --editor --output=C:\Games\Modularity.apk
```

The setup script installs tools under `Build-Android/android-sdk`, verifies Google's
command-line tools archive checksum, and installs NDK 30.0.16248370, platform 36,
build-tools 36.0.0, platform-tools, and SDK CMake/Ninja. Android Studio and Gradle
are not required. It does not change machine-wide environment variables.
`-CheckOnly` checks an existing installation without downloading packages.

The exporter detects `Build-Android/android-sdk` (`build/android-sdk` for
`build.sh` trees), the standard Android Studio SDK
directory, and side-by-side NDKs automatically. You can override discovery with:

- `ANDROID_NDK_ROOT` (or `ANDROID_NDK_HOME` / `ANDROID_NDK`) pointing at the NDK
- `ANDROID_SDK_ROOT` (or `ANDROID_HOME`) pointing at the SDK

If no SDK *platform* is installed yet, `build.sh` will try to grab one with
`sdkmanager "platforms;android-34"` (aapt2 needs an `android.jar` to link against).

Default ABI is `arm64-v8a`, which covers basically every Android device from the
last several years. Override with `--abi=<abi>` if you really need something else.

## How a build actually happens

`build.sh --Android` does NOT cross-compile the engine itself in shell the way
`--Windows` does. Instead it:

1. Builds the **native editor** (`Modularity`) like a normal desktop build.
2. Runs it headlessly: `Modularity --build-android [--project=...] [--abi=...] -o <apk>`.

That headless mode (see `Engine::buildAndroidApkHeadless`, `src/Engine.cpp`) runs
with no window and no GL context. It reuses the exact same export pipeline the
editor's Build Settings panel uses, so the CLI and the GUI produce identical APKs.
Under the hood it:

1. Configures + builds `core_player` with the NDK toolchain into
   `libModularityPlayer.so` (CMake is already Android-aware, see `CMakeLists.txt`).
2. Cross-compiles your ModuCPP scripts for the target ABI (see below).
   The engine and scripts use the same NDK shared C++ runtime. Its library and
   notice are packaged with the app, following [Android's runtime guidance](https://developer.android.com/ndk/guides/cpp-support#shared_runtimes).
3. Stages runtime content (scenes, assets) into `assets/content.modbundle`.
4. Writes an `AndroidManifest.xml` that launches `android.app.NativeActivity`
   with `android.app.lib_name=ModularityPlayer`.
5. Packages native libraries with the JDK `jar` tool, aligns the APK with
   `zipalign -P 16`, and signs it with `apksigner`.

Fresh Android builds use Ninja when available, including SDK-bundled Ninja,
so Windows does not select a desktop Visual Studio generator for the NDK.
Android builds default to four compiler workers; set
`CMAKE_BUILD_PARALLEL_LEVEL` to override this, or use `--jobs=N` on Windows.

Player APKs have no Java/Kotlin (`android:hasCode="false"`). The experimental
editor APK generates one tiny Java `NativeActivity` subclass while packaging so
the native editor can launch Android's file explorer and receive selected asset
URIs. The engine, renderer, input loop, and editor still live in native code.

## The runtime (`src/AndroidRuntime/AndroidRuntime.cpp`)

When the APK launches, `android_main` hands control to
`Modularity::AndroidRuntime::Run`. That sets up an EGL ES3 context against the
`ANativeWindow`, pumps the NativeActivity lifecycle + input loop, redirects
stdout/stderr into logcat, installs an `AAssetManager`-backed asset source, then
constructs the normal `Engine` and runs its usual main loop. The Android-specific
bits in `Engine::run()` are all gated behind `#ifdef __ANDROID__`.

To watch what it's doing on device:

```bash
adb logcat -s Modularity:V Modularity.stderr:V Modularity.stdout:V
```

> future me: if the screen is pure black with zero errors in logcat, check
> `ANativeWindow_setBuffersGeometry` in `CreateEGLSurface`. if the native window
> buffer format and the EGL config disagree, some phones just composite nothing
> and tell you absolutely nothing about it. been there.

## Touch input

Touch is wired all the way through, so you don't have to special-case Android in
gameplay code:

- The runtime tracks every active finger (`AndroidRuntime` multi-touch state).
- The primary finger drives ImGui's mouse, so all existing UI (PlayerViewport, UI
  canvas buttons) and any mouse-based scripts respond to taps with no changes.
- Scripts read touches through the platform-agnostic `Touch` facade in
  `include/ModuInputScriptApi.h`:

```cpp
add ModuInput;

if (touch.IsActive()) {
    vec2 p = touch.Primary();   // surface pixels, origin top-left
}
if (touch.Tapped()) { /* a finger just went down */ }
vec2 swipe = touch.Drag();      // primary-finger movement since last frame
int fingers = touch.Count();    // multi-touch
```

On desktop the left mouse button is reported as a single touch, so the same
`touch.*` code runs in the editor and the desktop player. Coordinates are
surface/framebuffer pixels; normalize against the viewport size yourself if you
want 0..1.

## Microphone

`Microphone.Start()` (ModuInput) captures through miniaudio (AAudio/OpenSL) like on
desktop, but `RECORD_AUDIO` is a runtime permission:

- The manifest only declares it when a script under the project mentions
  `Microphone` (`projectUsesMicrophone` in Engine.cpp); the editor APK always does.
  It comes with `android.hardware.microphone` marked `required="false"`, so
  mic-less devices can still install the game.
- The first `Start()` asks. The editor APK asks through its Java bridge
  (`modularityRequestMicrophonePermission`); an exported player has no Java
  code, so it calls `Activity.requestPermissions` over JNI. `AudioSystem` then
  polls `checkSelfPermission` and opens the device once permission is granted.
- The prompt takes focus while it is showing. If the app comes back into focus
  without a grant, or no prompt appears within 3 s (Android stops showing it after
  repeated denials), `Microphone.Status()` reports the denial and points to
  Settings. Polling keeps going, so a grant from Settings works without a restart.

## Scripts on device (the important constraint)

ModuCPP scripts normally compile to a `.so` via the host C++ compiler and get
`dlopen`'d at runtime. Android does not supply an on-device C++ compiler.
Exported games therefore cross-compile scripts and package the libraries in
the APK's `lib/<abi>/` directory. The Android editor has a separate embedded
compiler path for editing and recompiling scripts on the device.

So the Android build does two things:

1. **Cross-compiles** each script ahead of time with the NDK clang++ for the
   target ABI (see `Engine::crossCompileAndroidScripts`, `src/AndroidExport.cpp`,
   which drives `ScriptCompiler`'s cross-compile mode).
2. **Packages** each resulting library into `lib/<abi>/` under a mangled soname,
   and drops a 0-byte placeholder at the script's normal bundle path so the
   engine's "is this script compiled?" check still passes.

At runtime, `ScriptRuntime::getModule` (`src/ScriptRuntime.cpp`) detects the
Android case and `dlopen`s the library **by soname** from `nativeLibraryDir`
instead of by bundle path. The packager and the loader derive that soname from
the same relative path via `moduAndroidScriptSoname` (`src/AndroidScript.h`), so
they always agree.

The script ABI guard (`MODULARITY_NATIVE_SCRIPT_ABI_VERSION` +
`MODULARITY_SCRIPT_LAYOUT_SIGNATURE`) still runs on device. If a cross-compiled
script was built against different struct layouts than the packaged
`libModularityPlayer.so`, it gets rejected at load with a clear "ABI mismatch,
recompile" message rather than corrupting memory. If you see that in logcat,
rebuild the APK so the player and scripts come from the same source tree.

For the editor APK, `build.bat --Android --editor` (or the Bash equivalent)
prepares embedded Cling/LLVM for C++ and ModuCPP and TinyCC for C. C scripts keep
their generated C++ context bridge: TinyCC emits the C object, and Cling links
it with the bridge in the JIT. Setup commands, caching, and the device probes are
documented in [EmbeddedAndroidCompilers.md](EmbeddedAndroidCompilers.md).

The existing external Clang option is still available for custom builds.
With `MODULARITY_ANDROID_CLANG_DIR` explicitly supplied, Bash can stage an on-device
compiler bundle under `build/android-clang/arm64-v8a/`: the NDK sysroot, engine
script headers, GLM, and a trimmed Termux-derived aarch64 clang/lld payload.
The NDK only ships host clang binaries, so `build.sh` fetches the current Termux
aarch64 packages, verifies their SHA-256 checksums from the Termux package
index, and wraps the Termux binaries so they can run from the editor app's
writable files directory.

Useful overrides:

- `MODULARITY_TERMUX_REPO=<url>`: use a specific Termux main package mirror.
- `MODULARITY_ANDROID_CLANG_AUTO_FETCH=0`: skip automatic Termux download.
- `MODULARITY_ANDROID_CLANG_DIR=<dir>`: use a prebuilt bundle instead of the
  default `build/android-clang/arm64-v8a/`.

If no runnable clang is staged, the APK still launches, but script recompiles on
the phone report that the bundled toolchain is missing.

## Importing assets in the Android editor

The File Browser's **Import Assets...** action uses Android's system file
explorer in the editor APK. Selected `content://` documents are copied by the
generated Activity into the app cache, then the native editor imports those
temporary files through the same project-copy path used on desktop.

The Android picker supports selecting one or more files. It does not currently
import an entire external folder in one action.

## Requirements / gotchas

- Needs a working desktop build environment, since the APK build builds the
  native editor first. (This is why it isn't a pure cross-compile.)
- APK packaging and the debug signing key need a JDK. `JAVA_HOME` and the
  Windows Java `latest` installation are checked in addition to PATH.
- The supported Photon SDK packages contain desktop binaries and are excluded
  from Android builds; networking falls back to the offline backend.
- GLES builds use manual exposure. The desktop auto-exposure readback and
  geometry/image-store voxel GI paths are not available on the GLES 3.0 backend.
- Editor APK packaging needs `javac` on PATH and SDK build-tools with `d8` so
  the generated file-picker Activity can be compiled without Gradle.
- arm64-v8a only by default. Other ABIs work via `--abi=` but aren't routinely
  tested.
- minSdk is 26. Player APKs target SDK 34; the editor APK targets SDK 28 so it
  can execute the bundled clang and load freshly-built script `.so` files from
  app storage during on-device development.
- The first `--Android` build is slow (it builds the whole editor + the NDK
  player). After that both are incremental.
