#!/usr/bin/env bash
# Self-test for the package manager / 7.0 package line.
# Links against the already-built build/libcore.a, so run ./build.sh first.
#
# Usage: tools/package-selftest/run.sh [registry-root]
# Defaults to the sibling Modu-Package-Manager checkout.
set -euo pipefail
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
REGISTRY_ROOT="${1:-$(cd "$REPO_ROOT/.." && pwd)/Modu-Package-Manager}"
BUILD_DIR="${TMPDIR:-/tmp}/modularity-package-selftest"
CORE_LIB="$REPO_ROOT/build/libcore.a"

if [[ ! -f "$CORE_LIB" ]]; then
    echo "error: $CORE_LIB not found - build the engine first (./build.sh)" >&2
    exit 1
fi
if [[ ! -f "$REGISTRY_ROOT/PackageManagerInfo.modu" ]]; then
    echo "error: $REGISTRY_ROOT is not a package registry root" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"
rm -rf "${BUILD_DIR:?}/work"
mkdir -p "$BUILD_DIR/work"

g++ -std=c++17 -O0 -g \
    -I "$REPO_ROOT/src" \
    -I "$REPO_ROOT/include" \
    -I "$REPO_ROOT/src/ThirdParty/glm" \
    -I "$REPO_ROOT/src/ThirdParty/glfw/include" \
    -I "$REPO_ROOT/src/ThirdParty/glad" \
    -I "$REPO_ROOT/src/ThirdParty/ModuGUI" \
    package_test.cpp -o "$BUILD_DIR/package_test" \
    -Wl,--start-group \
        "$CORE_LIB" \
        "$REPO_ROOT/build/libglad.a" \
        "$REPO_ROOT/build/libimgui.a" \
        "$REPO_ROOT/build/libimguizmo.a" \
        "$REPO_ROOT/build/jolt/libJolt.a" \
    -Wl,--end-group \
    -lglfw -lGL -ldl -lpthread

"$BUILD_DIR/package_test" "$REGISTRY_ROOT" "$BUILD_DIR/work"
