#!/usr/bin/env bash
# Standalone self-test for the Renderer::resize validation policy
# (src/RendererResizePolicy.h). No GL context, no engine link, no build required.
# Run after touching Renderer::resize or its guards.
set -euo pipefail
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/modularity-resize-selftest"
mkdir -p "$BUILD_DIR"

g++ -std=c++14 -O1 -Wall -Wextra \
    -I "$REPO_ROOT/src" \
    resize_test.cpp -o "$BUILD_DIR/resize_test"

"$BUILD_DIR/resize_test"

# Also run under the sanitizers, matching ./build.sh --fsanitize.
g++ -std=c++14 -O1 -g -fsanitize=address,undefined \
    -I "$REPO_ROOT/src" \
    resize_test.cpp -o "$BUILD_DIR/resize_test_asan"

"$BUILD_DIR/resize_test_asan"
