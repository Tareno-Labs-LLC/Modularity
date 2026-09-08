#!/usr/bin/env bash
# Self-test for the Assemblage core (cells, chunking, RLE codec, both formats).
#
# Unlike the ModuOBJ self-test this needs no engine build: the Assemblage core
# depends only on glm and the standard library, so it compiles the one source
# file directly.
#
# Usage: tools/assemblage-selftest/run.sh
set -euo pipefail
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/modularity-assemblage-selftest"

mkdir -p "$BUILD_DIR"
rm -rf "${BUILD_DIR:?}/work"
mkdir -p "$BUILD_DIR/work"

g++ -std=c++17 -O0 -g -Wall -Wextra \
    -I "$REPO_ROOT/src" \
    -I "$REPO_ROOT/src/ThirdParty/glm" \
    "$REPO_ROOT/src/Assemblage.cpp" \
    "$REPO_ROOT/tools/assemblage-selftest/assemblage_test.cpp" \
    -o "$BUILD_DIR/assemblage_test"

"$BUILD_DIR/assemblage_test" "$BUILD_DIR/work"
