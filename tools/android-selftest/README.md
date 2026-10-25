# Android build checks (Windows)

Install the SDK/NDK with `tools/Setup-Android.ps1`, then run:

```powershell
python tools/android-selftest/run.py --scripts
powershell -ExecutionPolicy Bypass -File tools/android-selftest/build_driver_test.ps1
.\build.bat --Android --jobs=4
```

The C++ tests cover SDK/NDK/Ninja/runtime discovery, native MSVC command
preservation, and real NDK compilation of C++, ModuCPP and C API scripts into
ARM64 ELF libraries. They include paths with spaces and nested script folders,
and check that temporary `.staging` paths cannot change packaged script names.
The driver tests mock external build commands, exercise the real Android argument
and step wiring, and test cleanup only inside a uniquely named fixture directory.

Build outputs and fixtures stay under `build/`. Verify APK signatures with SDK
`apksigner verify --verbose` and alignment with `zipalign -c -P 16 -v 4`.
Both the player and scripts use the packaged `libc++_shared.so` from the selected
NDK. A successful build/signature check does not replace running the APK on a
real Android device.
