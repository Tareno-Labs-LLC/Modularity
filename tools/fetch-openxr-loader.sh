#!/usr/bin/env bash
# Refreshes the vendored Khronos OpenXR Android loader.
#
# The loaders in src/ThirdParty/openxr/lib/<abi>/ are checked in, so a normal
# clone already has everything an Android XR build needs and this script does not
# need to be run. It exists so that "where did these .so files come from" has an
# answer you can re-run, and so bumping the version is one command.
#
#   ./tools/fetch-openxr-loader.sh            # refetch the pinned version
#   ./tools/fetch-openxr-loader.sh 1.1.64     # bump to a new version
#
# The artifact is the official Khronos prebuilt:
#   pkg:maven/org.khronos.openxr/openxr_loader_for_android
# Apache-2.0 OR MIT. The AAR's own LICENSE is vendored next to the binaries.
#
# NOTE: the headers in src/ThirdParty/openxr/include/ are versioned separately
# (see the README). Keep them on the same release as the loader - a loader newer
# than the headers is fine, the reverse is not.

set -euo pipefail

VERSION="${1:-1.1.62}"
GROUP_PATH="org/khronos/openxr/openxr_loader_for_android"
BASE_URL="https://repo1.maven.org/maven2/${GROUP_PATH}/${VERSION}"
AAR_NAME="openxr_loader_for_android-${VERSION}.aar"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEST_ROOT="${REPO_ROOT}/src/ThirdParty/openxr"

# Every ABI Modularity's Android export can target. Quest is arm64-v8a; the rest
# are carried so that picking another ABI in Build Settings does not turn into a
# missing-loader error at export time.
ABIS=(arm64-v8a armeabi-v7a x86_64 x86)

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

echo "Fetching ${AAR_NAME} from Maven Central..."
if ! curl -fsSL --retry 3 -o "${WORK_DIR}/loader.aar" "${BASE_URL}/${AAR_NAME}"; then
    echo "error: could not download ${BASE_URL}/${AAR_NAME}" >&2
    echo "       check the version exists at https://repo1.maven.org/maven2/${GROUP_PATH}/" >&2
    exit 1
fi

# Verify against the published checksum rather than trusting the transfer.
if curl -fsSL --retry 3 -o "${WORK_DIR}/loader.aar.sha1" "${BASE_URL}/${AAR_NAME}.sha1"; then
    expected="$(tr -d '[:space:]' < "${WORK_DIR}/loader.aar.sha1")"
    actual="$(sha1sum "${WORK_DIR}/loader.aar" | cut -d' ' -f1)"
    if [ "${expected}" != "${actual}" ]; then
        echo "error: SHA1 mismatch for ${AAR_NAME}" >&2
        echo "       expected ${expected}" >&2
        echo "       actual   ${actual}" >&2
        exit 1
    fi
    echo "SHA1 verified: ${actual}"
else
    echo "warning: no published .sha1 to verify against; continuing unverified." >&2
fi

# An AAR is a zip. The prebuilt loaders live under jni/<abi>/.
unzip -q -o "${WORK_DIR}/loader.aar" -d "${WORK_DIR}/aar"

for abi in "${ABIS[@]}"; do
    src="${WORK_DIR}/aar/jni/${abi}/libopenxr_loader.so"
    if [ ! -f "${src}" ]; then
        echo "error: ${AAR_NAME} contains no loader for ${abi}" >&2
        exit 1
    fi
    mkdir -p "${DEST_ROOT}/lib/${abi}"
    cp "${src}" "${DEST_ROOT}/lib/${abi}/libopenxr_loader.so"
    printf '  %-12s %s\n' "${abi}" "$(sha256sum "${DEST_ROOT}/lib/${abi}/libopenxr_loader.so" | cut -d' ' -f1)"
done

if [ -f "${WORK_DIR}/aar/META-INF/LICENSE" ]; then
    cp "${WORK_DIR}/aar/META-INF/LICENSE" "${DEST_ROOT}/LICENSE"
fi

echo
echo "Vendored OpenXR loader ${VERSION} into src/ThirdParty/openxr/lib/."
echo "These are git-lfs tracked (*.so). Remember to update the version noted in"
echo "src/ThirdParty/openxr/README.md if you bumped it."
