#!/usr/bin/env bash
# Fetches the Khronos OpenXR loader DLL for Windows into redist/windows-x64/.
#
# WHY THIS EXISTS
# ---------------
# Windows has no system-wide OpenXR loader. Unlike Linux, where the runtime
# package installs libopenxr_loader.so.1, on Windows the Khronos loader is
# shipped *by the application* - installing SteamVR or the Meta app gives you a
# runtime, not a loader. So a Modularity build with no openxr_loader.dll next to
# it reports "No OpenXR loader found" on a machine that plainly has VR working,
# which is exactly the confusion this script removes.
#
# Anything dropped into redist/windows-x64/ is copied next to the built
# executables by CMake, and XRLoader also probes redist/windows-x64/ directly so
# a build run from the source tree works without the copy step.
#
#   ./tools/fetch-openxr-loader-windows.sh          # fetch the pinned version
#   ./tools/fetch-openxr-loader-windows.sh 1.1.64   # bump to a new version
#
# Runs from Git Bash / MSYS on Windows as well as from Linux (for preparing a
# cross-build's redist folder). Needs curl and unzip.
#
# The artifact is the .nupkg attached to the OpenXR-SDK GitHub release
# (Apache-2.0 OR MIT). Deliberately NOT nuget.org: the OpenXR.Loader package
# published there stopped at 1.0.10.2, and a loader *older* than the headers in
# src/ThirdParty/openxr/include/ is the one combination that does not work - the
# headers declare entry points the loader cannot resolve. The GitHub release
# tracks the header releases version for version.

set -euo pipefail

VERSION="${1:-1.1.63}"
RELEASE_URL="https://github.com/KhronosGroup/OpenXR-SDK/releases/download/release-${VERSION}/OpenXR.Loader.${VERSION}.nupkg"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

echo "Fetching OpenXR loader ${VERSION} from the OpenXR-SDK release..."
if ! curl -fsSL --retry 3 -o "${WORK_DIR}/loader.nupkg" "${RELEASE_URL}"; then
    echo "error: could not download ${RELEASE_URL}" >&2
    echo "       check the release exists at" >&2
    echo "       https://github.com/KhronosGroup/OpenXR-SDK/releases" >&2
    echo "       or download openxr_loader.dll by hand into redist/windows-x64/." >&2
    exit 1
fi

# A .nupkg is a zip.
unzip -q -o "${WORK_DIR}/loader.nupkg" -d "${WORK_DIR}/pkg"

# The package also carries _uwp variants and a Win32 (32-bit) build. Only the
# desktop ones are wanted; a UWP loader picked up by a wildcard would load and
# then fail at runtime in a way that looks like a runtime problem, not a
# packaging one.
copy_arch() {
    local package_path="$1"  # path inside the .nupkg
    local dest_subdir="$2"   # redist/<this>
    local src="${WORK_DIR}/pkg/${package_path}"
    if [ ! -f "${src}" ]; then
        echo "warning: ${package_path} is not in this package; skipping ${dest_subdir}." >&2
        return 1
    fi
    mkdir -p "${REPO_ROOT}/redist/${dest_subdir}"
    cp "${src}" "${REPO_ROOT}/redist/${dest_subdir}/openxr_loader.dll"
    printf '  %-14s %s\n' "${dest_subdir}" \
        "$(sha256sum "${REPO_ROOT}/redist/${dest_subdir}/openxr_loader.dll" | cut -d' ' -f1)"
    return 0
}

got_any=0
copy_arch "native/x64/release/bin/openxr_loader.dll" "windows-x64" && got_any=1
# 32-bit is best-effort: Modularity's Windows builds are x64.
copy_arch "native/Win32/release/bin/openxr_loader.dll" "windows-x86" || true

if [ "${got_any}" -eq 0 ]; then
    echo "error: the package contained no x64 openxr_loader.dll at the expected path." >&2
    echo "       the release layout may have changed; look inside the .nupkg and" >&2
    echo "       update copy_arch above." >&2
    exit 1
fi

echo
echo "OpenXR loader ${VERSION} placed in redist/. Rebuild (or re-run CMake) so it is"
echo "copied next to Modularity.exe, then check Project Settings > OpenXR Manager >"
echo "Runtime Diagnostics - the Loader row should stop saying \"Not found\"."
