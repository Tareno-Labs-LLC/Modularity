#!/usr/bin/env bash
# Type-checks src/XR/ in the Quest configuration without an Android NDK.
#
# Why this exists: the OpenXR backend's most important code is only reachable
# when XR_USE_GRAPHICS_API_OPENGL_ES and XR_USE_PLATFORM_ANDROID are both defined
# - the GL ES swapchains, the EGL graphics binding, and the Android loader init.
# A normal desktop build compiles none of it, so on a machine with no NDK that
# code can rot silently. This forces both macros and runs the host compiler in
# -fsyntax-only mode over the XR translation units.
#
#   ./tools/xr-android-syntax-check.sh
#
# Requires a configured build/ (for compile_commands.json) and system EGL headers.
#
# What it does NOT cover: anything that includes real NDK headers, i.e.
# src/AndroidRuntime/AndroidRuntime.cpp and the __ANDROID__ branch of
# src/XRRenderPath.cpp. Those need a real NDK build. This checks the OpenXR-facing
# code, which is the part with no other coverage at all.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build}"
CC_JSON="${BUILD_DIR}/compile_commands.json"

if [ ! -f "${CC_JSON}" ]; then
    echo "error: ${CC_JSON} not found. Configure a build first (./build.sh)." >&2
    exit 1
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

# Reuse the real build's include and define flags, so this check cannot drift
# from how the engine is actually compiled. MODULARITY_OPENGL_ES is flipped on
# because the XR graphics path is GL ES only.
python3 - "${CC_JSON}" "${WORK_DIR}/flags.txt" <<'PY'
import json, shlex, sys
cc_path, out_path = sys.argv[1], sys.argv[2]
with open(cc_path) as handle:
    entries = json.load(handle)
for entry in entries:
    if entry['file'].endswith('src/ModuCPPTranspiler.cpp'):
        args = shlex.split(entry['command'])
        flags = [a for a in args if a.startswith('-I') or a.startswith('-D')]
        flags = ['-DMODULARITY_OPENGL_ES=1' if f.startswith('-DMODULARITY_OPENGL_ES=') else f
                 for f in flags]
        with open(out_path, 'w') as handle:
            handle.write('\n'.join(flags))
        break
else:
    sys.exit('could not find a compile command to borrow flags from')
PY

# openxr_platform.h's Android section references a few JNI handle types (it passes
# the VM and activity themselves as void*). A real NDK would supply jni.h; these
# opaque typedefs are enough for a syntax check and never enter a real build.
mkdir -p "${WORK_DIR}/jnistub"
cat > "${WORK_DIR}/jnistub/jni.h" <<'EOF'
#pragma once
typedef void* jobject;
typedef void* jstring;
typedef void* jclass;
typedef struct _JavaVM JavaVM;
typedef struct _JNIEnv JNIEnv;
EOF

SOURCES=(
    src/XR/XRLoader.cpp
    src/XR/XRRuntime.cpp
    src/XR/XRSession.cpp
    src/XR/XRSwapchainGL.cpp
    src/XR/XRInput.cpp
    src/XR/XRInputBindings.cpp
    src/XR/XRSystem.cpp
    src/XR/XRSettings.cpp
    src/XR/XRFeatures.cpp
    src/XR/XRDiagnostics.cpp
    # The scene-component layer. Pulls in Engine.h, so it is the slowest entry
    # here by a wide margin - but it is also where the tracked-pose and grab code
    # lives, which is worth the wait.
    src/XR/XRComponents.cpp
)

mapfile -t FLAGS < "${WORK_DIR}/flags.txt"
CXX="${CXX:-c++}"
failures=0

echo "Type-checking src/XR/ as Quest (OpenGL ES + Android)..."
for source in "${SOURCES[@]}"; do
    if "${CXX}" -fsyntax-only -std=c++23 -w \
        -DXR_USE_PLATFORM_ANDROID \
        -I"${WORK_DIR}/jnistub" -include jni.h \
        "${FLAGS[@]}" "${REPO_ROOT}/${source}"; then
        printf '  ok    %s\n' "${source}"
    else
        printf '  FAIL  %s\n' "${source}"
        failures=$((failures + 1))
    fi
done

echo
if [ "${failures}" -ne 0 ]; then
    echo "${failures} file(s) failed the Quest-configuration syntax check." >&2
    exit 1
fi
echo "All ${#SOURCES[@]} XR translation units type-check in the Quest configuration."
