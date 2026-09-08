#!/usr/bin/env bash
# Standalone self-test for src/MapCarve.cpp (the Map Maker carve geometry).
# Compiles MapCarve against a shim ModelLoader.h (RawMeshAsset only) so no
# engine/renderer dependencies are needed. Run after touching MapCarve.
set -euo pipefail
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/modularity-carve-selftest"
mkdir -p "$BUILD_DIR"
cp "$REPO_ROOT/src/MapCarve.h" "$REPO_ROOT/src/MapCarve.cpp" ModelLoader.h carve_test.cpp "$BUILD_DIR/"
g++ -std=c++17 -O1 -I "$BUILD_DIR" -I "$REPO_ROOT/src/ThirdParty/glm" \
    "$BUILD_DIR/carve_test.cpp" "$BUILD_DIR/MapCarve.cpp" -o "$BUILD_DIR/carve_test"
"$BUILD_DIR/carve_test"
