#!/usr/bin/env bash
# Self-test for the networking foundation (serializer, session, spawn, RPC).
# Links against the already-built build/libcore.a, so run ./build.sh first.
#
# Usage: tools/network-selftest/run.sh
set -euo pipefail
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/modularity-network-selftest"
NINJA_FILE="$REPO_ROOT/build/build.ninja"

if [[ ! -f "$REPO_ROOT/build/libcore.a" ]]; then
    echo "error: build/libcore.a not found - build the engine first (./build.sh)" >&2
    exit 1
fi
if [[ ! -f "$NINJA_FILE" ]]; then
    echo "error: $NINJA_FILE not found - configure the engine build first" >&2
    exit 1
fi

# Reuse the editor target's own LINK_LIBRARIES rather than hand-maintaining a
# library list here: ModuObj pulls in the scene serializer, which transitively
# drags in most of the engine's third-party dependencies.
LINK_LIBS="$(grep -m1 '^build Modularity:' -A 8 "$NINJA_FILE" \
             | grep -oE 'LINK_LIBRARIES = .*' | head -1 | cut -c18-)"
if [[ -z "$LINK_LIBS" ]]; then
    echo "error: could not read LINK_LIBRARIES from $NINJA_FILE" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"
rm -rf "${BUILD_DIR:?}/work"
mkdir -p "$BUILD_DIR/work"

# Paths in LINK_LIBRARIES are relative to the build directory.
# shellcheck disable=SC2086
(cd "$REPO_ROOT/build" && g++ -std=c++17 -O0 -g \
    -I "$REPO_ROOT/src" \
    -I "$REPO_ROOT/include" \
    -I "$REPO_ROOT/src/ThirdParty/glm" \
    -I "$REPO_ROOT/src/ThirdParty/glfw/include" \
    -I "$REPO_ROOT/src/ThirdParty/glad" \
    -I "$REPO_ROOT/src/ThirdParty/ModuGUI" \
    "$REPO_ROOT/tools/network-selftest/network_test.cpp" \
    -o "$BUILD_DIR/network_test" \
    -Wl,--start-group $LINK_LIBS -Wl,--end-group)

"$BUILD_DIR/network_test" "$BUILD_DIR/work"
