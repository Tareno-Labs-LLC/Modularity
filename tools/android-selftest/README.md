# Android build checks (Windows)

Install the SDK/NDK with `tools/Setup-Android.ps1`, then run:

```powershell
python tools/android-selftest/run.py --scripts
python tools/android-selftest/run.py --runtime
powershell -ExecutionPolicy Bypass -File tools/android-selftest/build_driver_test.ps1
.\build.bat --Android --jobs=4
```

The C++ tests cover SDK/NDK/Ninja/runtime discovery, native MSVC command
preservation, and real NDK compilation of C++, ModuCPP and C API scripts into
ARM64 ELF libraries. They include paths with spaces and nested script folders,
and check that temporary `.staging` paths cannot change packaged script names.
The driver tests mock external build commands, exercise the real Android argument
and step wiring, and test cleanup only inside a uniquely named fixture directory.
Runtime checks exercise same-size APK bundle updates, unchanged cache reuse, and
the real Android filesystem asset fallback with Windows-authored paths. Only the
Android asset-manager and logging calls are stubbed for these Windows checks.

Build outputs and fixtures stay under `build/`. Verify APK signatures with SDK
`apksigner verify --verbose` and alignment with `zipalign -c -P 16 -v 4`.
Both the player and scripts use the packaged `libc++_shared.so` from the selected
NDK. A successful build/signature check does not replace running the APK on a
real Android device.

For Tenna's exported project/scene regression, run
`python tools/android-selftest/project_check.py <Tenna.apk>` after building the
desktop editor. Add `--before` to reproduce the old Unix CRLF-reader behavior:
the Windows-authored project becomes 3D, its scene name retains CR, and true
boolean settings become false. The corrected reader opens Main with 61 objects.

`gles_probe.cpp` compiles and links the production backend-prepared shaders on
the attached Android GPU using an EGL pbuffer. Build it with NDK clang++ and
`-std=c++23 -DMODULARITY_OPENGL_ES=1 -ffunction-sections -fdata-sections
-Wl,--gc-sections -static-libstdc++ -lEGL -lGLESv3`, with include roots `src`,
`include`, and `src/ThirdParty/glm`. Push it and the shader pair under
`/data/local/tmp/`, then pass the vertex and fragment paths to the executable.
This catches driver-specific shader failures that APK packaging checks cannot.

Check an exported game with `python tools/android-selftest/verify_apk.py Game.apk
--readelf <NDK>/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe`.
This checks each script's explicit player/runtime dependencies, 16 KB ELF load
alignment, and its bundle placeholder. Add repeated `--asset Assets/...` arguments
to check files loaded only by scripts, such as numbered frames and voice clips.
