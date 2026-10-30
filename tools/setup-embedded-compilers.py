"""Build pinned Cling/LLVM and TinyCC for the Android editor (no Termux needed)."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request

REPO = Path(__file__).resolve().parents[1]
PINS = {
    "tinycc": ("TinyCC/tinycc", "e23dabed154a8495f3ac1ff3cc9d04fa1c207893"),
    "cling": ("root-project/cling", "b0b2e7f1f18494d6b52498c0a2bbdce765713059"),
    "llvm-project": ("root-project/llvm-project", "6a80d8e5c4e904256a322dc037a837a0232c68bc"),
}


def fresh_if_moved(build_dir: Path) -> None:
    """CMake refuses a build tree whose CMakeCache.txt was written at another path, and
    these trees record absolute paths everywhere, so a moved cache cannot be resumed.
    Start that tree over; the downloaded sources and archives are kept."""
    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.is_file():
        return
    for line in cache_file.read_text(errors="replace").splitlines():
        if line.startswith("CMAKE_CACHEFILE_DIR:"):
            recorded = Path(line.split("=", 1)[1].strip())
            if os.path.normcase(str(recorded.resolve())) != os.path.normcase(str(build_dir.resolve())):
                print(f"{build_dir.name} was configured at {recorded}; it moved, so it is rebuilt from scratch.", flush=True)
                shutil.rmtree(build_dir)
            return


def run(*args: object, env: dict | None = None) -> None:
    command = [str(arg) for arg in args]
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True, env=env)


def source(cache: Path, name: str) -> Path:
    project, revision = PINS[name]
    dest = cache / "sources" / f"{name}-{revision}"
    if (dest / ".modularity-source.json").is_file():
        return dest
    archive = cache / f"{name}.tar.gz"
    if not archive.exists():
        url = f"https://codeload.github.com/{project}/tar.gz/{revision}"
        print(f"Downloading {project} at {revision}", flush=True)
        temporary = archive.with_suffix(".part")
        with urllib.request.urlopen(url, timeout=60) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        temporary.replace(archive)
    digest = hashlib.file_digest(archive.open("rb"), "sha256").hexdigest()
    # Commit-addressed HTTPS archives; the receipt records the archive digest as
    # well as its source revision. Downloads never modify tracked third parties.
    with tarfile.open(archive) as bundle:
        members = bundle.getmembers()
        prefix = f"{name}-{revision}/"
        if any(member.name != prefix[:-1] and not member.name.startswith(prefix) for member in members):
            raise RuntimeError(f"Unexpected archive root: {archive}")
        if name == "llvm-project":
            members = [member for member in members if len(member.name.split("/")) < 2 or
                       member.name.split("/")[1] in ("llvm", "clang", "cmake", "third-party") or
                       len(member.name.split("/")) == 2]
            members = [member for member in members if not set(member.name.split("/")[2:]) & {"test", "unittests"}]
        bundle.extractall(cache / "sources", members=members, filter="data")
    (dest / ".modularity-source.json").write_text(json.dumps({
        "project": project, "revision": revision, "archiveSha256": digest}, indent=2))
    return dest


def ndk_root(value: str | None) -> Path:
    for root in [value, os.environ.get("ANDROID_NDK_ROOT"), os.environ.get("ANDROID_NDK_HOME")]:
        if root and (Path(root) / "build/cmake/android.toolchain.cmake").is_file():
            return Path(root).resolve()
    sdk = Path(os.environ.get("ANDROID_SDK_ROOT", REPO / "Build-Android/android-sdk"))
    candidates = list((sdk / "ndk").glob("*"))
    candidates = [p for p in candidates if p.name.replace(".", "").isdigit()]
    if not candidates:
        raise RuntimeError("NDK not found. Run tools/Setup-Android.ps1 -AcceptLicenses, or pass --ndk.")
    return max(candidates, key=lambda p: tuple(map(int, p.name.split("."))))


def native_environment(cache: Path) -> dict:
    env = os.environ.copy()
    if os.name != "nt" or shutil.which("cl"):
        return env
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                  "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                  "-property", "installationPath"], text=True).strip()
    vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
    if not vcvars.is_file():
        raise RuntimeError("A native host C++ compiler is needed to build LLVM's table generators.")
    batch = cache / "host-environment.cmd"
    batch.write_text(f'@set "PATH={vswhere.parent};%PATH%"\n@call "{vcvars}" >nul\n@if errorlevel 1 exit /b 1\n@set\n')
    output = subprocess.check_output(["cmd", "/d", "/c", str(batch)], text=True)
    for line in output.splitlines():
        key, separator, value = line.partition("=")
        if separator and key:
            # Environment names on Windows are case insensitive; avoid PATH/Path duplicates.
            for existing in list(env):
                if existing.lower() == key.lower():
                    del env[existing]
            env[key] = value
    env["_CL_"] = "/MP2"
    return env


def copy_tree(source_root: Path, destination: Path, headers_only: bool = False) -> None:
    if not source_root.is_dir():
        return
    for path in source_root.rglob("*"):
        if path.is_file() and (not headers_only or path.suffix in (".h", ".hpp", ".inc", ".def", ".inl", ".modulemap")):
            target = destination / path.relative_to(source_root)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)


def stage(cache: Path, prefix: Path, ndk: Path, llvm: Path, cling: Path, tinycc: Path) -> None:
    (prefix / "ready.json").unlink(missing_ok=True)
    target = cache / "cling-arm64"
    host = next((ndk / "toolchains/llvm/prebuilt").iterdir())
    sysroot = host / "sysroot"
    # Include all target API levels: Clang selects the requested one itself.
    copy_tree(sysroot / "usr/include", prefix / "bundle/sysroot/usr/include")
    copy_tree(sysroot / "usr/lib/aarch64-linux-android", prefix / "bundle/sysroot/usr/lib/aarch64-linux-android")
    copy_tree(tinycc / "include", prefix / "bundle/tinycc/include")
    shutil.copyfile(cache / "tinycc-arm64/libtcc1.a", prefix / "bundle/tinycc/libtcc1.a")
    copy_tree(cling / "include", prefix / "include", True)
    copy_tree(target / "tools/cling/include", prefix / "include", True)
    copy_tree(llvm / "llvm/include", prefix / "include", True)
    copy_tree(target / "include", prefix / "include", True)
    copy_tree(llvm / "clang/include", prefix / "include", True)
    copy_tree(target / "tools/clang/include", prefix / "include", True)
    copy_tree(cling / "include", prefix / "bundle/cling/include", True)
    copy_tree(target / "tools/cling/include", prefix / "bundle/cling/include", True)
    copy_tree(target / "lib/clang", prefix / "bundle/cling/lib/clang")
    if not list((prefix / "bundle/cling/lib/clang").glob("*/include/stddef.h")):
        raise RuntimeError("Clang resource headers were not built.")
    libs = prefix / "lib"
    libs.mkdir(parents=True, exist_ok=True)
    runtime = []
    for path in (target / "lib").glob("*.so*"):
        if path.is_file() and not path.is_symlink():
            shutil.copyfile(path, libs / path.name)
            runtime.append(path.name)
    shutil.copyfile(cache / "tinycc-arm64/libmodularity_tinycc.a", libs / "libmodularity_tinycc.a")
    shutil.copyfile(tinycc / "libtcc.h", prefix / "include/libtcc.h")
    (prefix / "runtime-libraries.txt").write_text("\n".join(sorted(runtime)) + "\n")
    # Package current script headers on every staging run, including editor-only
    # Engine declarations used by the generated C-context bridge.
    sdk = prefix / "bundle/headers"
    run("cmake", f"-DSRC_ROOT={REPO.as_posix()}", f"-DDST_ROOT={sdk.as_posix()}", "-P", REPO / "cmake/CopyScriptSDK.cmake")
    copy_tree(REPO / "src/ThirdParty/openxr/include", sdk / "src/ThirdParty/openxr/include", True)
    licenses = prefix / "bundle/licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    for path, name in [(tinycc / "COPYING", "TinyCC-COPYING"), (cling / "LICENSE.TXT", "Cling-LICENSE.txt"),
                       (llvm / "llvm/LICENSE.TXT", "LLVM-LICENSE.txt"), (host / "NOTICE", "NDK-NOTICE.txt")]:
        shutil.copyfile(path, licenses / name)
    (prefix / "sources.json").write_text(json.dumps(PINS, indent=2))
    (prefix / "ready.json").write_text(json.dumps({"abi": "arm64-v8a", "sources": PINS}, indent=2))
    print(f"Embedded compiler bundle ready: {prefix}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true", help="Download pinned sources only")
    parser.add_argument("--tinycc-only", action="store_true", help="Build TinyCC and its ARM64 probe first")
    parser.add_argument("--ndk")
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--cache", type=Path, default=REPO / "Build-Android/embedded-compilers")
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 32:
        parser.error("--jobs must be between 1 and 32")
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    tinycc = source(cache, "tinycc")
    if args.fetch:
        source(cache, "cling")
        source(cache, "llvm-project")
        return
    ndk = ndk_root(args.ndk)
    android = [f"-DCMAKE_TOOLCHAIN_FILE={(ndk / 'build/cmake/android.toolchain.cmake').as_posix()}",
               "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-26", "-DANDROID_STL=c++_shared"]
    for tree in ("tinycc-arm64", "llvm-host", "cling-arm64"):
        fresh_if_moved(cache / tree)
    run("cmake", "-S", REPO / "tools/embedded-compilers", "-B", cache / "tinycc-arm64", "-G", "Ninja",
        *android, "-DCMAKE_BUILD_TYPE=Release", f"-DMODULARITY_TINYCC_SOURCE_DIR={tinycc.as_posix()}")
    run("cmake", "--build", cache / "tinycc-arm64", "--parallel", args.jobs)
    if args.tinycc_only:
        return
    cling = source(cache, "cling")
    llvm = source(cache, "llvm-project")
    native_env = native_environment(cache)
    common = ["-DLLVM_ENABLE_PROJECTS=clang", "-DLLVM_INCLUDE_TESTS=OFF", "-DCLANG_INCLUDE_TESTS=OFF",
              "-DLLVM_INCLUDE_BENCHMARKS=OFF", "-DLLVM_INCLUDE_EXAMPLES=OFF", "-DLLVM_ENABLE_ZLIB=OFF",
              "-DLLVM_ENABLE_ZSTD=OFF", "-DLLVM_ENABLE_LIBXML2=OFF", "-DLLVM_ENABLE_TERMINFO=OFF",
              "-DCMAKE_BUILD_TYPE=Release", "-DLLVM_ENABLE_ASSERTIONS=OFF"]
    native = cache / "llvm-host"
    run("cmake", "-S", llvm / "llvm", "-B", native, "-G", "Ninja", *common,
        "-DLLVM_TARGETS_TO_BUILD=", "-DLLVM_ENABLE_BINDINGS=OFF", env=native_env)
    run("cmake", "--build", native, "--target", "llvm-tblgen", "clang-tblgen", "llvm-config", "--parallel", args.jobs, env=native_env)
    exe = ".exe" if os.name == "nt" else ""
    target = cache / "cling-arm64"
    run("cmake", "-S", llvm / "llvm", "-B", target, "-G", "Ninja", *common, *android,
        "-DLLVM_TARGETS_TO_BUILD=AArch64", "-DLLVM_HOST_TRIPLE=aarch64-linux-android",
        f"-DLLVM_TABLEGEN={(native / ('bin/llvm-tblgen' + exe)).as_posix()}",
        f"-DCLANG_TABLEGEN={(native / ('bin/clang-tblgen' + exe)).as_posix()}",
        f"-DLLVM_NATIVE_TOOL_DIR={(native / 'bin').as_posix()}",
        "-DLLVM_EXTERNAL_PROJECTS=cling", f"-DLLVM_EXTERNAL_CLING_SOURCE_DIR={cling.as_posix()}",
        "-DLLVM_ENABLE_PIC=ON", "-DLLVM_ENABLE_EH=ON", "-DLLVM_ENABLE_RTTI=ON",
        "-DLLVM_BUILD_LLVM_DYLIB=ON", "-DLLVM_LINK_LLVM_DYLIB=ON",
        "-DCLANG_BUILD_CLANG_DYLIB=ON", "-DCLANG_LINK_CLANG_DYLIB=ON",
        "-DCLING_INCLUDE_TESTS=OFF", "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384")
    run("cmake", "--build", target, "--target", "libcling", "clang-headers", "--parallel", args.jobs)
    stage(cache, cache / "arm64-v8a", ndk, llvm, cling, tinycc)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, tarfile.TarError) as error:
        print(f"Embedded compiler setup failed: {error}", file=sys.stderr)
        sys.exit(1)
