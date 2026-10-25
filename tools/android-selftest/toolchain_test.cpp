#include "AndroidToolchain.h"
#include <cstdio>
#include <fstream>

static int checks = 0, failures = 0;
static void check(bool pass, const char* label) {
    ++checks;
    if (!pass) ++failures;
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
}
static void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path).close();
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    namespace fs = std::filesystem;
    const fs::path root = fs::absolute(argv[1]);
    const fs::path sdk = root / "sdk with spaces";
    fs::create_directories(sdk / "platforms");
    fs::create_directories(sdk / "build-tools");
    _putenv_s("ANDROID_SDK_ROOT", sdk.string().c_str());
    _putenv_s("ANDROID_HOME", "");
    _putenv_s("ANDROID_NDK_ROOT", "");
    _putenv_s("ANDROID_NDK_HOME", "");
    _putenv_s("ANDROID_NDK", "");
    check(AndroidToolchain::FindSdkRoot() == sdk, "SDK paths with spaces are discovered without shell parsing");
    const auto low = sdk / "ndk/9.0.0";
    const auto high = sdk / "ndk/30.0.0";
    for (const auto& ndk : {low, high, sdk / "ndk/31.0.0-beta"}) touch(ndk / "build/cmake/android.toolchain.cmake");
    check(AndroidToolchain::FindNdkRoot() == high, "side-by-side NDK selection is numeric and skips previews");
    _putenv_s("ANDROID_NDK_ROOT", low.string().c_str());
    check(AndroidToolchain::FindNdkRoot() == low, "explicit valid NDK overrides automatic selection");
    const auto incomplete = root / "incomplete-ndk";
    touch(incomplete / "source.properties");
    _putenv_s("ANDROID_NDK_ROOT", incomplete.string().c_str());
    check(AndroidToolchain::FindNdkRoot() == high, "incomplete NDK does not shadow an installed toolchain");
    for (const char* version : {"3.9.0", "3.30.0"}) touch(sdk / "cmake" / version / "bin/ninja.exe");
    check(AndroidToolchain::FindNinja(sdk) == sdk / "cmake/3.30.0/bin/ninja.exe", "SDK Ninja discovery uses numeric version ordering");
    const auto runtime = high / "toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so";
    touch(runtime);
    check(AndroidToolchain::FindSharedCxxRuntime(high, "arm64-v8a") == runtime, "shared C++ runtime discovery matches the requested ABI");
    check(AndroidToolchain::FindSharedCxxRuntime(high, "x86").empty(), "a missing ABI runtime is not replaced with another architecture");
    check(AndroidToolchain::ValidAbi("arm64-v8a") && AndroidToolchain::ValidAbi("armeabi-v7a") && AndroidToolchain::ValidAbi("x86_64") && AndroidToolchain::ValidAbi("x86"), "standard Android ABIs are accepted");
    check(!AndroidToolchain::ValidAbi("arm64") && !AndroidToolchain::ValidAbi(""), "unsupported ABI names cannot silently build another architecture");
    std::printf("Android toolchain: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
