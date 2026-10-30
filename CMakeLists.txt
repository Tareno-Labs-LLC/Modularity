cmake_minimum_required(VERSION 3.16)
project(Modularity LANGUAGES C CXX)

include(CheckCXXCompilerFlag)

if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)

set(MODULARITY_BUILD_CPP_STANDARD "c++23" CACHE STRING "Default C++ standard for Modularity engine targets")
set_property(CACHE MODULARITY_BUILD_CPP_STANDARD PROPERTY STRINGS "c++17" "c++20" "c++23")
string(TOLOWER "${MODULARITY_BUILD_CPP_STANDARD}" MODULARITY_BUILD_CPP_STANDARD_NORMALIZED)

set(MODULARITY_CXX_STANDARD_LABEL "C++23")
set(MODULARITY_CXX_STANDARD_FLAG "")
if(MSVC)
    if(MODULARITY_BUILD_CPP_STANDARD_NORMALIZED STREQUAL "c++17" OR MODULARITY_BUILD_CPP_STANDARD_NORMALIZED STREQUAL "17")
        set(MODULARITY_CXX_STANDARD_LABEL "C++17")
        set(MODULARITY_CXX_STANDARD_FLAG "/std:c++17")
    elseif(MODULARITY_BUILD_CPP_STANDARD_NORMALIZED STREQUAL "c++20" OR MODULARITY_BUILD_CPP_STANDARD_NORMALIZED STREQUAL "20")
        set(MODULARITY_CXX_STANDARD_LABEL "C++20")
        set(MODULARITY_CXX_STANDARD_FLAG "/std:c++20")
    else()
        check_cxx_compiler_flag("/std:c++23" MODULARITY_HAS_CXX23_FLAG)
        if(NOT MODULARITY_HAS_CXX23_FLAG)
            check_cxx_compiler_flag("/std:c++23preview" MODULARITY_HAS_CXX23_PREVIEW_FLAG)
        endif()
        set(MODULARITY_CXX_STANDARD_LABEL "C++23")
        if(MODULARITY_HAS_CXX23_FLAG)
            set(MODULARITY_CXX_STANDARD_FLAG "/std:c++23")
        elseif(MODULARITY_HAS_CXX23_PREVIEW_FLAG)
            set(MODULARITY_CXX_STANDARD_FLAG "/std:c++23preview")
        else()
            message(WARNING "C++23 was requested but this MSVC toolchain does not expose /std:c++23. Falling back to C++20.")
            set(MODULARITY_CXX_STANDARD_LABEL "C++20")
            set(MODULARITY_CXX_STANDARD_FLAG "/std:c++20")
        endif()
    endif()
else()
    if(MODULARITY_BUILD_CPP_STANDARD_NORMALIZED STREQUAL "c++17" OR MODULARITY_BUILD_CPP_STANDARD_NORMALIZED STREQUAL "17")
        set(MODULARITY_CXX_STANDARD_LABEL "C++17")
        set(MODULARITY_CXX_STANDARD_FLAG "-std=c++17")
    elseif(MODULARITY_BUILD_CPP_STANDARD_NORMALIZED STREQUAL "c++20" OR MODULARITY_BUILD_CPP_STANDARD_NORMALIZED STREQUAL "20")
        set(MODULARITY_CXX_STANDARD_LABEL "C++20")
        set(MODULARITY_CXX_STANDARD_FLAG "-std=c++20")
    else()
        check_cxx_compiler_flag("-std=c++23" MODULARITY_HAS_CXX23_FLAG)
        if(NOT MODULARITY_HAS_CXX23_FLAG)
            check_cxx_compiler_flag("-std=c++2b" MODULARITY_HAS_CXX2B_FLAG)
        endif()
        set(MODULARITY_CXX_STANDARD_LABEL "C++23")
        if(MODULARITY_HAS_CXX23_FLAG)
            set(MODULARITY_CXX_STANDARD_FLAG "-std=c++23")
        elseif(MODULARITY_HAS_CXX2B_FLAG)
            set(MODULARITY_CXX_STANDARD_FLAG "-std=c++2b")
        else()
            message(WARNING "C++23 was requested but this compiler does not expose -std=c++23 or -std=c++2b. Falling back to C++20.")
            set(MODULARITY_CXX_STANDARD_LABEL "C++20")
            set(MODULARITY_CXX_STANDARD_FLAG "-std=c++20")
        endif()
    endif()
endif()

message(STATUS "Modularity C++ API default: ${MODULARITY_CXX_STANDARD_LABEL} (${MODULARITY_CXX_STANDARD_FLAG})")

function(modularity_apply_cxx_api_standard target_name)
    target_compile_options(${target_name} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${MODULARITY_CXX_STANDARD_FLAG}>
    )
endfunction()

# ==================== Compiler cache (ccache / sccache) ====================

option(MODULARITY_USE_COMPILER_CACHE "Enable compiler cache if available" ON)

if(MODULARITY_USE_COMPILER_CACHE)
    find_program(CCACHE_PROGRAM ccache)
    find_program(SCCACHE_PROGRAM sccache)

    if(CCACHE_PROGRAM)
        message(STATUS "Using compiler cache: ccache (${CCACHE_PROGRAM})")
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")

    elseif(SCCACHE_PROGRAM)
        message(STATUS "Using compiler cache: sccache (${SCCACHE_PROGRAM})")
        set(CMAKE_C_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")

        # Optional (helps some MSVC setups): ensure environment is used consistently
        # set(ENV{SCCACHE_IDLE_TIMEOUT} "0")

    else()
        message(STATUS "Compiler cache enabled, but neither ccache nor sccache was found.")
    endif()
else()
    message(STATUS "Compiler cache disabled (MODULARITY_USE_COMPILER_CACHE=OFF).")
endif()

# ==================== WINDOWS FIXES (only active on Windows) ====================
if(WIN32)
    set(MODULARITY_WINDOWS_DEFINES
        NOMINMAX                  # Fixes std::min/std::max clash with Windows.h
        WIN32_LEAN_AND_MEAN       # Speeds up Windows.h includes
        _CRT_SECURE_NO_WARNINGS   # Silences strncpy, sscanf, etc. warnings
    )
    if(MSVC)
        list(APPEND MODULARITY_WINDOWS_DEFINES
            _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING
        )
    endif()
endif()

# ==================== Compiler flags ====================
if(MSVC)
    # Debug info, kept to what crash stacks need. Symbols and line tables stay in every
    # .obj, so optimised builds still resolve to function names and source lines in
    # CrashReporter's stack traces (the linker gathers them into a real .pdb, see /DEBUG
    # below). Type records - ~64% of the debug info - go once per target into a shared
    # compiler PDB (/Zi) instead of being repeated in every .obj and again in each static
    # .lib (/Z7), which made the Windows tree several times the size of Linux's (built
    # without -g). /FS lets the parallel compilers (/MP, MultiToolTask) share that PDB.
    # ccache/sccache can only cache /Z7, so a Ninja/Makefile build that uses one keeps it.
    if(CMAKE_CXX_COMPILER_LAUNCHER AND NOT CMAKE_GENERATOR MATCHES "Visual Studio")
        set(_modularity_debug_info_flags /Z7)
    else()
        set(_modularity_debug_info_flags /Zi /FS)
    endif()
    set(MODULARITY_WARNING_FLAGS /W4 /O2 /permissive- /MP ${_modularity_debug_info_flags})
else()
    set(MODULARITY_WARNING_FLAGS -Wall -Wextra -Wpedantic -O2)
endif()

# ==================== CPU / ISA target ====================
# Controls the baseline x86-64 microarchitecture level the engine and its
# bundled dependencies are compiled for. PUBLIC Linux releases MUST default to
# "baseline" (-march=x86-64 -mtune=generic) so the executable and bundled .so
# files run on any 64-bit x86 CPU. Building with a higher level (or "native")
# bakes in AVX/AVX2/AVX-512 instructions that crash on older CPUs with
# "CPU ISA level is lower than required" / SIGILL.
#
# Higher levels are for local developer performance testing only and must not
# be shipped as the default public release. See docs/Build.md.
set(MODULARITY_CPU_TARGET "baseline" CACHE STRING
    "x86-64 CPU/ISA target: baseline | x86-64-v2 | x86-64-v3 | x86-64-v4 | native")
set_property(CACHE MODULARITY_CPU_TARGET PROPERTY STRINGS
    "baseline" "x86-64-v2" "x86-64-v3" "x86-64-v4" "native")

string(TOLOWER "${MODULARITY_CPU_TARGET}" MODULARITY_CPU_TARGET_NORMALIZED)

# Only x86_64 toolchains (GCC/Clang/MinGW; not MSVC) take -march/-mtune. ARM,
# Android, and MSVC are left untouched so their builds are not broken.
set(MODULARITY_X86_ARCH OFF)
if(NOT MSVC AND NOT ANDROID AND
   ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64" OR
    "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "AMD64" OR
    "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86" OR
    "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "i386"))
    set(MODULARITY_X86_ARCH ON)
endif()

# Resolve the requested target to concrete -march flags plus the set of Jolt
# USE_* SIMD switches (Jolt emits its own PUBLIC -mavx2/-mavx512f flags, which
# would otherwise leak into the engine binary and raise its ISA requirement).
set(MODULARITY_ARCH_FLAGS "")
set(MODULARITY_JOLT_USE_SSE4_1 OFF)
set(MODULARITY_JOLT_USE_SSE4_2 OFF)
set(MODULARITY_JOLT_USE_AVX OFF)
set(MODULARITY_JOLT_USE_AVX2 OFF)
set(MODULARITY_JOLT_USE_AVX512 OFF)
set(MODULARITY_JOLT_USE_LZCNT OFF)
set(MODULARITY_JOLT_USE_TZCNT OFF)
set(MODULARITY_JOLT_USE_F16C OFF)
set(MODULARITY_JOLT_USE_FMADD OFF)

if(MODULARITY_CPU_TARGET_NORMALIZED STREQUAL "baseline")
    set(MODULARITY_ARCH_FLAGS -march=x86-64 -mtune=generic)
    # All Jolt SIMD extensions stay OFF -> Jolt emits only -msse2 (baseline).
elseif(MODULARITY_CPU_TARGET_NORMALIZED STREQUAL "x86-64-v2")
    set(MODULARITY_ARCH_FLAGS -march=x86-64-v2 -mtune=generic)
    set(MODULARITY_JOLT_USE_SSE4_1 ON)
    set(MODULARITY_JOLT_USE_SSE4_2 ON)
elseif(MODULARITY_CPU_TARGET_NORMALIZED STREQUAL "x86-64-v3")
    set(MODULARITY_ARCH_FLAGS -march=x86-64-v3 -mtune=generic)
    set(MODULARITY_JOLT_USE_SSE4_1 ON)
    set(MODULARITY_JOLT_USE_SSE4_2 ON)
    set(MODULARITY_JOLT_USE_AVX ON)
    set(MODULARITY_JOLT_USE_AVX2 ON)
    set(MODULARITY_JOLT_USE_LZCNT ON)
    set(MODULARITY_JOLT_USE_TZCNT ON)
    set(MODULARITY_JOLT_USE_F16C ON)
    set(MODULARITY_JOLT_USE_FMADD ON)
elseif(MODULARITY_CPU_TARGET_NORMALIZED STREQUAL "x86-64-v4")
    set(MODULARITY_ARCH_FLAGS -march=x86-64-v4 -mtune=generic)
    set(MODULARITY_JOLT_USE_SSE4_1 ON)
    set(MODULARITY_JOLT_USE_SSE4_2 ON)
    set(MODULARITY_JOLT_USE_AVX ON)
    set(MODULARITY_JOLT_USE_AVX2 ON)
    set(MODULARITY_JOLT_USE_AVX512 ON)
    set(MODULARITY_JOLT_USE_LZCNT ON)
    set(MODULARITY_JOLT_USE_TZCNT ON)
    set(MODULARITY_JOLT_USE_F16C ON)
    set(MODULARITY_JOLT_USE_FMADD ON)
elseif(MODULARITY_CPU_TARGET_NORMALIZED STREQUAL "native")
    # Developer-only: tune for the building machine. NEVER ship this default.
    set(MODULARITY_ARCH_FLAGS -march=native -mtune=native)
    # Enable Jolt's explicit SIMD paths through AVX2. The compiler may still
    # emit AVX-512 from -march=native when the build host supports it.
    set(MODULARITY_JOLT_USE_SSE4_1 ON)
    set(MODULARITY_JOLT_USE_SSE4_2 ON)
    set(MODULARITY_JOLT_USE_AVX ON)
    set(MODULARITY_JOLT_USE_AVX2 ON)
    set(MODULARITY_JOLT_USE_LZCNT ON)
    set(MODULARITY_JOLT_USE_TZCNT ON)
    set(MODULARITY_JOLT_USE_F16C ON)
    set(MODULARITY_JOLT_USE_FMADD ON)
else()
    message(WARNING
        "Unknown MODULARITY_CPU_TARGET='${MODULARITY_CPU_TARGET}'. "
        "Falling back to baseline (-march=x86-64 -mtune=generic).")
    set(MODULARITY_CPU_TARGET_NORMALIZED "baseline")
    set(MODULARITY_ARCH_FLAGS -march=x86-64 -mtune=generic)
endif()

if(MODULARITY_X86_ARCH)
    # Apply the -march/-mtune flags GLOBALLY (CMAKE_C/CXX_FLAGS) rather than
    # per-target. This is deliberate: vendored third-party libs built via
    # add_subdirectory (assimp, glfw, imgui, Jolt, zlib, PhysX) must also be
    # pinned to the target ISA. On distros whose GCC/Clang defaults to a higher
    # level (e.g. CachyOS ships -march=x86-64-v3 as the compiler default), those
    # deps would otherwise emit AVX/AVX2 instructions and the linked binary
    # would crash on baseline CPUs even though our own code is clean. Global
    # flags override the toolchain default for every translation unit.
    string(REPLACE ";" " " MODULARITY_ARCH_FLAGS_STR "${MODULARITY_ARCH_FLAGS}")
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${MODULARITY_ARCH_FLAGS_STR}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${MODULARITY_ARCH_FLAGS_STR}")

    message(STATUS "Modularity CPU target: ${MODULARITY_CPU_TARGET_NORMALIZED} (${MODULARITY_ARCH_FLAGS_STR})")
    if(MODULARITY_CPU_TARGET_NORMALIZED STREQUAL "native")
        message(WARNING
            "MODULARITY_CPU_TARGET=native bakes in host-specific ISA. "
            "Do NOT use native builds for public releases — they crash on other CPUs.")
    endif()
else()
    message(STATUS "Modularity CPU target: ${MODULARITY_CPU_TARGET_NORMALIZED} (no x86 -march flags on this toolchain/arch)")
endif()

# Post-link normalization of the GNU x86 ISA-needed property.
#
# The dynamic loader refuses to start a binary whose .note.gnu.property marks
# "x86 ISA needed: x86-64-v3/-v4" on a CPU that lacks that level
# ("CPU ISA level is lower than required"). That property is contributed by the
# *host* C runtime startup objects (Scrt1.o/crt1.o) as well as our code — and
# on distros that build those CRT objects for a high ISA level (CachyOS, some
# CLEAR/Intel images) the flag is baked into EVERY locally linked binary
# regardless of -march. For a "baseline" public release we therefore strip the
# property note from our own executables so the host toolchain can't silently
# raise the requirement above baseline. We only do this for the baseline target
# (where we've also forced all code to baseline ISA via the global flags above,
# so the stripped binary is genuinely safe to run anywhere). Higher targets keep
# the honest note as a real safety gate.
find_program(MODULARITY_OBJCOPY NAMES objcopy llvm-objcopy)
function(modularity_normalize_isa_note target_name)
    if(MODULARITY_X86_ARCH AND
       MODULARITY_CPU_TARGET_NORMALIZED STREQUAL "baseline" AND
       MODULARITY_OBJCOPY)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${MODULARITY_OBJCOPY} --remove-section .note.gnu.property
                    $<TARGET_FILE:${target_name}>
            COMMENT "Normalizing x86 ISA-needed note to baseline on ${target_name}"
            VERBATIM)
    endif()
endfunction()

# Kept as a thin alias so existing call sites stay valid; the actual arch flags
# are applied globally above, so this only handles the ISA-note normalization.
function(modularity_apply_cpu_target target_name)
    modularity_normalize_isa_note(${target_name})
endfunction()

# ==================== Sanitizers (opt-in) ====================
option(MODULARITY_ENABLE_ASAN "Enable AddressSanitizer (+ UBSan on GCC/Clang)" OFF)

set(MODULARITY_SANITIZER_FLAGS "")
if(MODULARITY_ENABLE_ASAN)
    if(MSVC)
        # MSVC ships AddressSanitizer only (no UBSan). build.ps1 --asan builds this into
        # <repo>/Build-ASAN. It goes on every target, third-party included, rather than
        # only the engine like the GCC/Clang path: the STL stamps objects built with and
        # without /fsanitize=address differently (detect_mismatch "annotate_vector"), so
        # mixing them fails to link - and instrumenting PhysX/Jolt/assimp also catches
        # heap writes that come from inside them. Must run before any add_subdirectory.
        # /bigobj: instrumentation adds enough sections that Engine.cpp exceeds the
        # 2^16 COFF limit (C1128).
        add_compile_options(/fsanitize=address /bigobj)
        # The prebuilt Photon libs are not instrumented, so their STL stamps say
        # annotate_vector/annotate_string=0 and the link fails (LNK2038). Turning the
        # annotations off only drops the container-overflow check (writes between size()
        # and capacity(), still inside the allocation); heap overflows and use-after-free
        # are still caught.
        add_compile_definitions(_DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
        add_link_options(/INCREMENTAL:NO)
        # /RTC1, CMake's Debug default, cannot be combined with /fsanitize=address.
        foreach(_modularity_flags_var CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
            string(REPLACE "/RTC1" "" ${_modularity_flags_var} "${${_modularity_flags_var}}")
        endforeach()
        message(STATUS "AddressSanitizer enabled on all targets (MSVC /fsanitize=address).")
    else()
        set(MODULARITY_SANITIZER_FLAGS
            -fsanitize=address
            -fsanitize=undefined
            # UBSan's vptr check emits typeinfo references for every polymorphic
            # class it sees. Jolt (and other third-party libs) build with
            # -fno-rtti and without sanitizers, so those symbols don't exist and
            # the link fails. Everything else in UBSan still runs.
            -fno-sanitize=vptr
            -fno-omit-frame-pointer
            -g
        )
        message(STATUS "AddressSanitizer + UBSan enabled on engine targets.")
    endif()
endif()

function(modularity_apply_sanitizers target_name)
    if(MODULARITY_ENABLE_ASAN AND MODULARITY_SANITIZER_FLAGS)
        target_compile_options(${target_name} PRIVATE ${MODULARITY_SANITIZER_FLAGS})
        target_link_options(${target_name} PRIVATE ${MODULARITY_SANITIZER_FLAGS})
    endif()
endfunction()

# ==================== Optional PhysX ====================
set(MODULARITY_ENABLE_PHYSX_DEFAULT ON)
set(MODULARITY_BUILD_EDITOR_DEFAULT ON)
set(MODULARITY_ENABLE_VULKAN_DEFAULT ON)
set(MODULARITY_ENABLE_SNDFILE_DEFAULT ON)
set(MODULARITY_ENABLE_OPUSFILE_DEFAULT ON)
set(MODULARITY_USE_MONO_DEFAULT ON)
if(ANDROID)
    set(MODULARITY_BUILD_EDITOR_DEFAULT OFF)
    set(MODULARITY_ENABLE_VULKAN_DEFAULT OFF)
    # PhysX has its own Android build steps that aren't wired up yet
    # (see docs/AndroidRuntime.md, Stage 1+). Default off so configure succeeds.
    set(MODULARITY_ENABLE_PHYSX_DEFAULT OFF)
    # libsndfile / opusfile / FFmpeg / Mono all need separate NDK
    # cross-compiles that aren't wired up yet. miniaudio (single header,
    # auto-picks OpenSL ES) covers basic audio out of the box. C# scripts
    # via Mono are intentionally not coming to Android (ModuCPP is the
    # forward path; see docs/AndroidRuntime.md).
    set(MODULARITY_ENABLE_SNDFILE_DEFAULT OFF)
    set(MODULARITY_ENABLE_OPUSFILE_DEFAULT OFF)
    set(MODULARITY_USE_MONO_DEFAULT OFF)
endif()
option(MODULARITY_ENABLE_PHYSX "Enable PhysX physics integration" ${MODULARITY_ENABLE_PHYSX_DEFAULT})

# Jolt is the cross-platform physics backend (works on Android, where PhysX
# currently does not). It is built alongside PhysX so projects can opt into
# either via ProjectPhysicsSettings::backend at runtime.
option(MODULARITY_ENABLE_JOLT "Enable JoltPhysics physics backend" ON)
# Photon Realtime networking backend.
#
# Auto-detected: if a Photon SDK package is present the backend is built,
# otherwise the engine builds without it and Net::CreatePhotonBackend()
# returns nullptr, so NetworkSession falls back to the offline backend. A missing
# SDK is never a build failure.
#
# Override explicitly with:
#   -DMODULARITY_ENABLE_PHOTON=ON|OFF
#   -DMODULARITY_PHOTON_SDK_ROOT=/path/to/photon.realtime-sdk[-windows]-<version>
# Asking for ON explicitly *is* a hard error when the SDK cannot be found, so a
# deliberate request never silently degrades.
#
# Photon publishes headers and prebuilt libraries as one matched set per platform,
# so the registry carries two packages: photon.realtime-sdk (Linux, GCC .a) and
# photon.realtime-sdk-windows (MSVC x64 .lib). Both may be installed side by side,
# because a candidate only counts once its libraries resolve for *this* toolchain.
# Finding the header is not enough - that alone would happily pick the Linux
# payload on Windows.
set(MODULARITY_PHOTON_SDK_ROOT "" CACHE PATH "Root of the Photon Realtime C++ SDK")

# Before Windows SDK support landed, this flag was an option(), so the first
# configure cached whatever it auto-decided. option() never revisits a cached
# value, and a cached value also makes the DEFINED test below true, so a machine
# that once configured without an SDK stayed pinned OFF - reporting itself
# "disabled by request" - even after the package was installed. Clear that entry
# once; from here the auto decision is a normal variable instead.
if(NOT MODULARITY_PHOTON_OPTION_MIGRATED)
    unset(MODULARITY_ENABLE_PHOTON CACHE)
    set(MODULARITY_PHOTON_OPTION_MIGRATED ON CACHE INTERNAL
        "Legacy cached MODULARITY_ENABLE_PHOTON option entry has been cleared")
endif()

# Remember whether the user asked for Photon explicitly, before we default it.
if(DEFINED MODULARITY_ENABLE_PHOTON)
    set(_modularity_photon_explicit TRUE)
else()
    set(_modularity_photon_explicit FALSE)
endif()

# Resolve the three libraries src/Network/PhotonBackend.cpp links out of an SDK
# root, in link order.
#
# Sets in the caller's scope:
#   <prefix>_RELEASE - release libraries
#   <prefix>_DEBUG   - debug libraries (falls back to the release ones when no
#                      debug set is vendored and none is needed)
#   <prefix>_COMMON  - the release Common-cpp library alone, for the ABI probe
#   <prefix>_REASON  - empty on success, otherwise why this root is unusable
function(modularity_photon_resolve_libs root prefix)
    set(${prefix}_RELEASE "" PARENT_SCOPE)
    set(${prefix}_DEBUG "" PARENT_SCOPE)
    set(${prefix}_COMMON "" PARENT_SCOPE)
    set(${prefix}_REASON "" PARENT_SCOPE)

    # The currently supported Photon package layouts contain desktop binaries.
    # Android is UNIX too, but Linux .a files cannot be linked into an ARM APK,
    # and a host-side try_run cannot validate an Android binary.
    if(ANDROID)
        set(${prefix}_REASON "desktop Photon SDK packages cannot target Android" PARENT_SCOPE)
        return()
    endif()

    if(NOT EXISTS "${root}/LoadBalancing-cpp/inc/Client.h")
        set(${prefix}_REASON "LoadBalancing-cpp/inc/Client.h is missing" PARENT_SCOPE)
        return()
    endif()

    # A Debug configuration has to link the debug libraries: upstream builds those
    # against the debug CRT and the release ones against the release CRT, so mixing
    # them drags two CRTs into one binary. Multi-config generators can build either
    # configuration from a single configure, so there both sets must be present.
    get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    set(_need_debug FALSE)
    if(_multi_config OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_need_debug TRUE)
    endif()

    set(_release_libs "")
    set(_debug_libs "")

    if(MSVC)
        # Upstream names every variant
        #   <Module>-cpp_vc<NN>_<debug|release>_windows_<md|mt>_<arch>.lib
        # and ships 60 of them per module. Three axes have to match how this build
        # is configured, or the link quietly pulls in a second CRT.

        # CRT flavour: md is the dynamic CRT, mt the static one. The CMake default,
        # when CMAKE_MSVC_RUNTIME_LIBRARY is unset, is dynamic - which is what the
        # engine builds with (Jolt is forced to agree through
        # USE_STATIC_MSVC_RUNTIME_LIBRARY).
        set(_crt "md")
        if(CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
            set(_crt "mt")
        endif()

        # Architecture, from the generator platform (build.ps1 passes -A x64).
        if(CMAKE_GENERATOR_PLATFORM)
            set(_arch "${CMAKE_GENERATOR_PLATFORM}")
        elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_arch "x64")
        else()
            set(_arch "Win32")
        endif()
        if(_arch STREQUAL "win32")
            set(_arch "Win32")
        endif()

        # Toolset tags, newest first. MSVC is binary compatible forward from v140,
        # so a newer compiler links older libraries and vc17 - the newest tag
        # upstream publishes - works on the VS 2026 toolset. The reverse does not
        # hold, so a tag built by a newer compiler than this one is never offered.
        set(_tags "")
        foreach(_candidate_tag vc17 vc16 vc15 vc14)
            if(_candidate_tag STREQUAL "vc17")
                set(_tag_floor 1930)
            elseif(_candidate_tag STREQUAL "vc16")
                set(_tag_floor 1920)
            elseif(_candidate_tag STREQUAL "vc15")
                set(_tag_floor 1910)
            else()
                set(_tag_floor 1900)
            endif()
            if(MSVC_VERSION GREATER_EQUAL ${_tag_floor})
                list(APPEND _tags "${_candidate_tag}")
            endif()
        endforeach()

        foreach(_tag IN LISTS _tags)
            set(_release_libs "")
            set(_debug_libs "")
            set(_tag_ok TRUE)
            foreach(_module LoadBalancing Photon Common)
                set(_lib_dir "${root}/${_module}-cpp/lib")
                set(_rel "${_lib_dir}/${_module}-cpp_${_tag}_release_windows_${_crt}_${_arch}.lib")
                set(_dbg "${_lib_dir}/${_module}-cpp_${_tag}_debug_windows_${_crt}_${_arch}.lib")
                if(NOT EXISTS "${_rel}")
                    set(_tag_ok FALSE)
                    break()
                endif()
                if(_need_debug AND NOT EXISTS "${_dbg}")
                    set(_tag_ok FALSE)
                    break()
                endif()
                list(APPEND _release_libs "${_rel}")
                if(EXISTS "${_dbg}")
                    list(APPEND _debug_libs "${_dbg}")
                else()
                    list(APPEND _debug_libs "${_rel}")
                endif()
            endforeach()
            if(_tag_ok)
                # The Photon libraries declare no /DEFAULTLIB for the Win32 APIs
                # they call, so those imports have to be named here: ws2_32 for the
                # sockets (WSAStartup, socket, getaddrinfo, select), winmm for
                # timeGetTime and bcrypt for BCryptGenRandom.
                list(APPEND _release_libs ws2_32 winmm bcrypt)
                list(APPEND _debug_libs ws2_32 winmm bcrypt)
                set(${prefix}_RELEASE "${_release_libs}" PARENT_SCOPE)
                set(${prefix}_DEBUG "${_debug_libs}" PARENT_SCOPE)
                set(${prefix}_COMMON
                    "${root}/Common-cpp/lib/Common-cpp_${_tag}_release_windows_${_crt}_${_arch}.lib"
                    PARENT_SCOPE)
                return()
            endif()
        endforeach()

        set(${prefix}_REASON
            "no ${_crt}/${_arch} libraries for the vc14-vc17 toolsets under <Module>-cpp/lib (this is probably the Linux payload, which ships GCC .a archives)"
            PARENT_SCOPE)
        return()
    endif()

    # GCC/Clang layout: lib<Module><Debug|Release><bits>.a, sitting directly in
    # <Module>-cpp rather than in a lib/ subdirectory.
    set(_bits 64)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_bits 32)
    endif()
    foreach(_module LoadBalancing Photon Common)
        set(_rel "${root}/${_module}-cpp/lib${_module}Release${_bits}.a")
        set(_dbg "${root}/${_module}-cpp/lib${_module}Debug${_bits}.a")
        if(NOT EXISTS "${_rel}")
            set(${prefix}_REASON
                "${_rel} is missing (this is probably the Windows payload, which ships MSVC .lib files)"
                PARENT_SCOPE)
            return()
        endif()
        if(_need_debug AND NOT EXISTS "${_dbg}")
            set(${prefix}_REASON "${_dbg} is missing" PARENT_SCOPE)
            return()
        endif()
        list(APPEND _release_libs "${_rel}")
        if(EXISTS "${_dbg}")
            list(APPEND _debug_libs "${_dbg}")
        else()
            list(APPEND _debug_libs "${_rel}")
        endif()
    endforeach()
    set(${prefix}_RELEASE "${_release_libs}" PARENT_SCOPE)
    set(${prefix}_DEBUG "${_debug_libs}" PARENT_SCOPE)
    set(${prefix}_COMMON "${root}/Common-cpp/libCommonRelease${_bits}.a" PARENT_SCOPE)
endfunction()

function(modularity_find_photon_sdk out_var out_notes)
    set(${out_var} "" PARENT_SCOPE)
    set(${out_notes} "" PARENT_SCOPE)

    set(_roots "")
    # Explicit root wins.
    if(MODULARITY_PHOTON_SDK_ROOT)
        list(APPEND _roots "${MODULARITY_PHOTON_SDK_ROOT}")
    endif()

    # Otherwise look where the package manager puts the package: the sibling
    # registry checkout, then the per-user global package folder. That folder is
    # %APPDATA%/.Modularity on Windows and $HOME/.Modularity elsewhere, matching
    # PackageManager::globalPackagesFolder() - checking only $HOME here used to
    # miss every globally installed package on Windows.
    set(_search_globs
        "${PROJECT_SOURCE_DIR}/../Modu-Package-Manager/Packages/*/Photon/photon.realtime-sdk*"
        "${PROJECT_SOURCE_DIR}/../Modu-Package-Manager/Packages/*/*/photon.realtime-sdk*"
        "$ENV{APPDATA}/.Modularity/Packages/*/*/photon.realtime-sdk*"
        "$ENV{APPDATA}/.Modularity/Packages/*/photon.realtime-sdk*"
        "$ENV{HOME}/.Modularity/Packages/*/*/photon.realtime-sdk*"
        "$ENV{HOME}/.Modularity/Packages/*/photon.realtime-sdk*")
    foreach(_glob IN LISTS _search_globs)
        file(GLOB _candidates "${_glob}")
        list(APPEND _roots ${_candidates})
    endforeach()
    if(_roots)
        list(REMOVE_DUPLICATES _roots)
    endif()

    # A root counts only when its libraries resolve for this toolchain, so an
    # installed wrong-platform package is skipped rather than half-adopted.
    set(_notes "")
    foreach(_candidate IN LISTS _roots)
        if(NOT IS_DIRECTORY "${_candidate}")
            continue()
        endif()
        modularity_photon_resolve_libs("${_candidate}" _photon_candidate)
        if(NOT _photon_candidate_REASON)
            set(${out_var} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
        list(APPEND _notes "${_candidate}: ${_photon_candidate_REASON}")
    endforeach()

    set(${out_notes} "${_notes}" PARENT_SCOPE)
endfunction()

set(_modularity_photon_found "")
set(_modularity_photon_notes "")
# Cross-builds to Windows use MinGW, which links neither the MSVC .lib files nor
# the Linux .a archives, so they still skip Photon.
if(NOT MODULARITY_CROSS_WINDOWS AND NOT ANDROID)
    modularity_find_photon_sdk(_modularity_photon_found _modularity_photon_notes)
endif()

if(_modularity_photon_found)
    modularity_photon_resolve_libs("${_modularity_photon_found}" _modularity_photon)
endif()

# Finding the SDK is not enough: it ships binary-only Common-cpp/Photon-cpp
# libraries, and an SDK built by a much older compiler is not ABI-compatible with
# the current one. Header inline code and the prebuilt out-of-line code then
# disagree, and Photon faults inside its own memory manager.
#
# So actually compile, link and RUN a tiny snippet. If it does not run cleanly the
# backend is disabled with an explanation instead of shipping an editor that
# crashes the moment networking starts. Drop in a newer SDK and this passes on its
# own with no CMake changes.
if(_modularity_photon_found AND NOT DEFINED MODULARITY_PHOTON_SDK_USABLE)
    set(_photon_probe "${CMAKE_BINARY_DIR}/CMakeFiles/photon_abi_probe.cpp")
    file(WRITE "${_photon_probe}"
"#include \"Common-cpp/inc/Common.h\"\n"
"#include <string>\n"
"int main() {\n"
"    std::string s = \"photon-abi-probe\";\n"
"    ExitGames::Common::JString j(s.c_str());\n"
"    return j.length() > 0 ? 0 : 1;\n"
"}\n")
    set(_photon_probe_flags
        "-DINCLUDE_DIRECTORIES=${_modularity_photon_found};${_modularity_photon_found}/Common-cpp/inc")
    set(_photon_probe_libs "${_modularity_photon_COMMON}")
    if(MSVC)
        # The probe links the release Common library, so it must be built in the
        # release configuration and with the engine CRT: a default Debug probe
        # would pull in the debug CRT and collide with the release one in the
        # library.
        # Saved and restored below: this variable steers every try_compile in the
        # project, so the probe must not leave it changed behind it.
        set(_photon_saved_try_config "${CMAKE_TRY_COMPILE_CONFIGURATION}")
        set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
        if(CMAKE_MSVC_RUNTIME_LIBRARY)
            list(APPEND _photon_probe_flags
                 "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}")
        endif()
        # Photon names no /DEFAULTLIB for the Win32 imports it uses, and JString
        # alone does not need them, but the linker still resolves the whole object.
        list(APPEND _photon_probe_libs ws2_32 winmm bcrypt)
    else()
        list(APPEND _photon_probe_libs pthread)
    endif()
    try_run(_photon_run_result _photon_compile_ok
        "${CMAKE_BINARY_DIR}/CMakeFiles/photon_abi_probe"
        "${_photon_probe}"
        CMAKE_FLAGS ${_photon_probe_flags}
        LINK_LIBRARIES ${_photon_probe_libs}
        COMPILE_OUTPUT_VARIABLE _photon_compile_log)
    if(MSVC)
        set(CMAKE_TRY_COMPILE_CONFIGURATION "${_photon_saved_try_config}")
    endif()
    if(_photon_compile_ok AND _photon_run_result EQUAL 0)
        set(MODULARITY_PHOTON_SDK_USABLE ON CACHE INTERNAL "Photon SDK passed the ABI probe")
    else()
        set(MODULARITY_PHOTON_SDK_USABLE OFF CACHE INTERNAL "Photon SDK failed the ABI probe")
    endif()
endif()

if(_modularity_photon_found AND NOT MODULARITY_PHOTON_SDK_USABLE)
    message(WARNING
        "Photon Realtime SDK found at ${_modularity_photon_found}, but it is not usable with this "
        "compiler (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}). The vendored SDK ships "
        "binary-only Common-cpp/Photon-cpp libraries; if they were built by a much older toolchain "
        "they are not ABI-compatible and Photon faults inside its own memory manager. "
        "Networking will run offline-only. Vendor a Photon SDK built for this toolchain to "
        "enable it, then re-run CMake.")
    set(_modularity_photon_unusable TRUE)
    set(_modularity_photon_found "")
endif()

if(_modularity_photon_found)
    set(MODULARITY_ENABLE_PHOTON_DEFAULT ON)
else()
    set(MODULARITY_ENABLE_PHOTON_DEFAULT OFF)
endif()

# Deliberately a normal variable rather than an option() when auto-decided: the
# decision has to be re-derived on every configure so that installing the package
# later actually takes effect. An explicit -DMODULARITY_ENABLE_PHOTON=ON|OFF does
# land in the cache and stays authoritative - that is what the DEFINED test above
# detects. Auto-decided values never reach the cache, so the SDK vanishing again
# simply flips the default back to OFF.
if(NOT _modularity_photon_explicit)
    set(MODULARITY_ENABLE_PHOTON ${MODULARITY_ENABLE_PHOTON_DEFAULT})
endif()

if(MODULARITY_ENABLE_PHOTON AND _modularity_photon_found AND NOT MODULARITY_PHOTON_SDK_ROOT)
    set(MODULARITY_PHOTON_SDK_ROOT "${_modularity_photon_found}" CACHE PATH
        "Root of the Photon Realtime C++ SDK" FORCE)
endif()

if(NOT MODULARITY_ENABLE_PHOTON)
    if(_modularity_photon_explicit)
        message(STATUS "Photon Realtime backend disabled by request; networking runs offline-only.")
    elseif(MODULARITY_CROSS_WINDOWS OR ANDROID)
        message(STATUS "Photon Realtime backend skipped on this platform "
                       "(no SDK payload is vendored for it); networking runs offline-only.")
    elseif(_modularity_photon_unusable)
        message(STATUS "Photon Realtime backend disabled: the installed SDK is not ABI-compatible "
                       "with this compiler (see the warning above). Networking runs offline-only.")
    else()
        # Name the candidates that were rejected and why. A wrong-platform package
        # sitting installed and silently ignored is otherwise hard to diagnose.
        foreach(_note IN LISTS _modularity_photon_notes)
            message(STATUS "Photon Realtime: skipped ${_note}")
        endforeach()
        message(STATUS "Photon Realtime backend disabled: no Photon SDK package with libraries for "
                       "this toolchain was found. Networking runs offline-only; install "
                       "photon.realtime-sdk-windows (Windows) or photon.realtime-sdk (Linux) to "
                       "enable it.")
    endif()
endif()
option(MODULARITY_BUILD_EDITOR "Build the Modularity editor target" ${MODULARITY_BUILD_EDITOR_DEFAULT})
option(MODULARITY_ENABLE_VULKAN "Enable experimental Vulkan backend" ${MODULARITY_ENABLE_VULKAN_DEFAULT})
option(MODULARITY_ENABLE_SNDFILE "Enable libsndfile-based audio import support" ${MODULARITY_ENABLE_SNDFILE_DEFAULT})
option(MODULARITY_ENABLE_OPUSFILE "Enable libopusfile-based Ogg/Opus decoding support" ${MODULARITY_ENABLE_OPUSFILE_DEFAULT})
option(MODULARITY_ENABLE_ASSIMP "Enable Assimp model import/runtime loading" ON)
set(MODULARITY_ASSIMP_IMPORTERS "" CACHE STRING "Semicolon-separated Assimp importers to enable when non-empty")
set(MODULARITY_USE_OPENGL_ES_DEFAULT OFF)
if(ANDROID)
    set(MODULARITY_USE_OPENGL_ES_DEFAULT ON)
endif()
option(MODULARITY_USE_OPENGL_ES "Use OpenGL ES 3.x headers, shaders, and link libraries" ${MODULARITY_USE_OPENGL_ES_DEFAULT})

# ---- OpenXR (VR) ----
# Compiles src/XR/ against the vendored Khronos headers in
# src/ThirdParty/openxr/. There is deliberately no link dependency: the loader is
# dlopen'd at runtime (see src/ThirdParty/openxr/README.md and src/XR/XRLoader.h),
# so a build with this ON still runs normally on a machine with no OpenXR runtime
# and adds nothing to the link line. Notably this does NOT imply Vulkan: the two
# graphics bindings implemented are OpenGL ES (Android / Quest, via
# XR_KHR_opengl_es_enable) and desktop OpenGL (Windows PCVR, via
# XR_KHR_opengl_enable). A Vulkan XR binding is deliberately absent.
#
# On Windows the loader DLL is not part of the build either - see
# tools/fetch-openxr-loader-windows.sh and src/ThirdParty/openxr/README.md.
option(MODULARITY_ENABLE_OPENXR "Compile the OpenXR (VR) backend" ON)
set(MODULARITY_OPENXR_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/src/ThirdParty/openxr/include)
if(MODULARITY_ENABLE_OPENXR AND NOT EXISTS ${MODULARITY_OPENXR_INCLUDE_DIR}/openxr/openxr.h)
    message(WARNING
        "MODULARITY_ENABLE_OPENXR is ON but the vendored OpenXR headers are missing at "
        "${MODULARITY_OPENXR_INCLUDE_DIR}/openxr/. Disabling OpenXR support.")
    set(MODULARITY_ENABLE_OPENXR OFF)
endif()
if(MODULARITY_ENABLE_OPENXR)
    message(STATUS "OpenXR backend enabled (headers only, loader resolved at runtime).")
endif()

# ==================== Discord Social SDK ====================
# Rich Presence is optional and opt-in. A fresh checkout can build and install
# Modularity without downloading the Discord Social SDK. Set
# -DMODULARITY_ENABLE_DISCORD_SOCIAL_SDK=ON to opt in on supported platforms.
# When disabled or unavailable, MODULARITY_HAS_DISCORD_SOCIAL_SDK=0 lets the
# engine compile its no-op implementation without linking or copying Discord.
option(MODULARITY_ENABLE_DISCORD_SOCIAL_SDK "Enable Discord Social SDK Rich Presence" OFF)
set(MODULARITY_DISCORD_SOCIAL_SDK_ROOT
    "${PROJECT_SOURCE_DIR}/src/ThirdParty/DiscordSocialSdk"
    CACHE PATH "Discord Social SDK root containing include/, lib/ and bin/")
set(MODULARITY_HAS_DISCORD_SOCIAL_SDK OFF)
set(MODULARITY_DISCORD_SOCIAL_RUNTIME "")

if(MODULARITY_ENABLE_DISCORD_SOCIAL_SDK AND NOT ANDROID)
    set(_discord_header "${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}/include/discordpp.h")
    set(_discord_arm64 OFF)
    if(CMAKE_GENERATOR_PLATFORM MATCHES "[Aa][Rr][Mm]64" OR
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(_discord_arm64 ON)
    endif()

    if(WIN32 AND MSVC)
        if(_discord_arm64)
            set(_discord_implib "${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}/lib/release/arm64/discord_partner_sdk.lib")
            set(_discord_runtime "${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}/bin/release/arm64/discord_partner_sdk.dll")
        else()
            set(_discord_implib "${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}/lib/release/discord_partner_sdk.lib")
            set(_discord_runtime "${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}/bin/release/discord_partner_sdk.dll")
        endif()
        if(EXISTS "${_discord_header}" AND EXISTS "${_discord_implib}" AND EXISTS "${_discord_runtime}")
            add_library(modularity_discord_social SHARED IMPORTED GLOBAL)
            set_target_properties(modularity_discord_social PROPERTIES
                IMPORTED_IMPLIB "${_discord_implib}"
                IMPORTED_LOCATION "${_discord_runtime}"
                INTERFACE_INCLUDE_DIRECTORIES "${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}/include")
            set(MODULARITY_DISCORD_SOCIAL_RUNTIME "${_discord_runtime}")
            set(MODULARITY_HAS_DISCORD_SOCIAL_SDK ON)
        endif()
    elseif(APPLE)
        set(_discord_runtime "${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}/lib/release/libdiscord_partner_sdk.dylib")
        if(EXISTS "${_discord_header}" AND EXISTS "${_discord_runtime}")
            add_library(modularity_discord_social SHARED IMPORTED GLOBAL)
            set_target_properties(modularity_discord_social PROPERTIES
                IMPORTED_LOCATION "${_discord_runtime}"
                INTERFACE_INCLUDE_DIRECTORIES "${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}/include")
            set(MODULARITY_DISCORD_SOCIAL_RUNTIME "${_discord_runtime}")
            set(MODULARITY_HAS_DISCORD_SOCIAL_SDK ON)
        endif()
    elseif(UNIX)
        set(_discord_runtime "${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}/lib/release/libdiscord_partner_sdk.so")
        if(EXISTS "${_discord_header}" AND EXISTS "${_discord_runtime}")
            add_library(modularity_discord_social SHARED IMPORTED GLOBAL)
            set_target_properties(modularity_discord_social PROPERTIES
                IMPORTED_LOCATION "${_discord_runtime}"
                INTERFACE_INCLUDE_DIRECTORIES "${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}/include")
            set(MODULARITY_DISCORD_SOCIAL_RUNTIME "${_discord_runtime}")
            set(MODULARITY_HAS_DISCORD_SOCIAL_SDK ON)
        endif()
    elseif(WIN32)
        message(STATUS "Discord Social SDK disabled: the supplied Windows package has MSVC import libraries, not MinGW .dll.a files.")
    endif()

    if(NOT MODULARITY_HAS_DISCORD_SOCIAL_SDK AND NOT (WIN32 AND NOT MSVC))
        message(WARNING "Discord Social SDK was requested but no compatible desktop library was found under ${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}; Rich Presence will be disabled.")
    endif()
endif()

if(MODULARITY_HAS_DISCORD_SOCIAL_SDK)
    message(STATUS "Discord Social SDK Rich Presence enabled: ${MODULARITY_DISCORD_SOCIAL_RUNTIME}")
elseif(NOT MODULARITY_ENABLE_DISCORD_SOCIAL_SDK)
    message(STATUS "Discord Social SDK disabled (optional; enable with -DMODULARITY_ENABLE_DISCORD_SOCIAL_SDK=ON).")
endif()
set(MODULARITY_SCRIPT_SDK_COPY_SCRIPT ${CMAKE_SOURCE_DIR}/cmake/CopyScriptSDK.cmake)
set(MODULARITY_RUNTIME_DLL_COPY_SCRIPT ${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDlls.cmake)
set(MODULARITY_FFMPEG_RUNTIME_DLLS)
set(MODULARITY_FFMPEG_RUNTIME_DLL_DIRS)

# ---- Optional bundled redistributable DLLs ----
# Every *.dll dropped into redist/windows-x64 (or redist/windows-x86) is copied
# next to the Windows executables. Populate it on the build host; the Vista (or
# other Windows) builder then picks the arch-matching folder automatically. Use
# it for:
#   * a Mesa llvmpipe opengl32.dll, to clear the engine's GL 3.3 / GLSL 330 floor
#     on machines whose system GL is too old (Windows Vista ships only GL 1.1).
#     opengl32 is not a KnownDLL, so the application-directory copy is loaded
#     ahead of the system one. It must keep the name opengl32.dll.
#   * toolchain runtime DLLs the target may lack (MinGW: libgcc_s_*,
#     libstdc++-6, libwinpthread-1; MSVC: the VC++ redist).
# The directory is selected by target architecture and may be empty/absent.
# See redist/README.md, including the Vista compatibility caveat.
option(MODULARITY_BUNDLE_REDIST_DLLS "Copy DLLs from redist/<arch> next to the Windows executable" ON)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MODULARITY_REDIST_ARCH_DIR "windows-x64")
else()
    set(MODULARITY_REDIST_ARCH_DIR "windows-x86")
endif()
set(MODULARITY_REDIST_DLL_DIR "${CMAKE_SOURCE_DIR}/redist/${MODULARITY_REDIST_ARCH_DIR}"
    CACHE PATH "Directory of redistributable DLLs to bundle next to Windows executables")
set(MODULARITY_TEMPLATE_PROJECTS_DIR ${CMAKE_SOURCE_DIR}/Template-Projects)
if(NOT IS_DIRECTORY ${MODULARITY_TEMPLATE_PROJECTS_DIR})
    set(MODULARITY_TEMPLATE_PROJECTS_DIR ${CMAKE_BINARY_DIR}/generated-empty/Template-Projects)
    file(MAKE_DIRECTORY ${MODULARITY_TEMPLATE_PROJECTS_DIR})
endif()

function(modularity_require_real_asset asset_path)
    set(full_path "${CMAKE_SOURCE_DIR}/${asset_path}")
    if(NOT EXISTS "${full_path}")
        message(FATAL_ERROR "Required asset is missing: ${asset_path}. Run git lfs install && git lfs pull.")
    endif()

    file(READ "${full_path}" asset_header LIMIT 64)
    if(asset_header MATCHES "^version https://git-lfs.github.com/spec/v1")
        message(FATAL_ERROR "Required asset is still a Git LFS pointer: ${asset_path}. Run git lfs install && git lfs pull.")
    endif()
endfunction()

modularity_require_real_asset("Resources/Engine-Root/Modu-Logo.png")
modularity_require_real_asset("Resources/Fonts/TheSunset.ttf")
modularity_require_real_asset("Resources/Fonts/Thesunsethd-Regular (1).ttf")

set(MODULARITY_HAS_VULKAN OFF)
if(MODULARITY_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(MODULARITY_HAS_VULKAN ON)
        message(STATUS "Experimental Vulkan backend enabled.")
    else()
        message(STATUS "Vulkan SDK not found; Vulkan backend disabled.")
    endif()
endif()

file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}" MODULARITY_SOURCE_DIR_CMAKE)

function(modularity_find_windows_imported_runtime_dll out_var import_lib stem)
    if(NOT WIN32)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    if(NOT import_lib)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    get_filename_component(import_dir "${import_lib}" DIRECTORY)
    get_filename_component(import_parent "${import_dir}" DIRECTORY)

    set(search_dirs
        "${import_dir}"
        "${import_parent}"
        "${import_parent}/bin"
        "${import_parent}/bin/x64"
        "${import_parent}/bin/Release"
        "${import_parent}/../bin"
        "${import_parent}/../bin/x64"
        "${import_parent}/../bin/Release"
    )
    list(REMOVE_DUPLICATES search_dirs)

    foreach(search_dir IN LISTS search_dirs)
        if(NOT EXISTS "${search_dir}")
            continue()
        endif()

        file(GLOB dll_candidates
            "${search_dir}/${stem}.dll"
            "${search_dir}/${stem}-*.dll"
            "${search_dir}/lib${stem}.dll"
            "${search_dir}/lib${stem}-*.dll"
        )
        list(SORT dll_candidates COMPARE NATURAL ORDER DESCENDING)
        list(LENGTH dll_candidates dll_count)
        if(dll_count GREATER 0)
            list(GET dll_candidates 0 resolved_dll)
            set(${out_var} "${resolved_dll}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

# ==================== Third-party libraries ====================

set(GLFW_BUILD_DOCS OFF CACHE BOOL "Build the GLFW documentation" FORCE)
set(ASSIMP_BUILD_DOCS OFF CACHE BOOL "Build Assimp documentation" FORCE)

if(ANDROID)
    # The Android NDK toolchain (android.toolchain.cmake) populates ANDROID_NDK
    # and ANDROID_ABI. Surface them so failures here are obvious from the
    # configure log instead of from a cryptic find_library() error later.
    if(NOT DEFINED ANDROID_NDK OR ANDROID_NDK STREQUAL "")
        message(FATAL_ERROR
            "ANDROID is set but ANDROID_NDK is empty. Invoke CMake with "
            "-DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake.")
    endif()
    message(STATUS "Android target: ABI=${ANDROID_ABI} platform=${ANDROID_PLATFORM} NDK=${ANDROID_NDK}")
endif()

# On Android we still build GLFW — but with X11 and Wayland disabled, GLFW
# falls back to its built-in "null" platform: no-op stubs for every window /
# input / monitor call. The engine source can reference glfw* functions
# unchanged; nothing actually happens. Real input/window handling on
# Android is wired through AndroidRuntime + ANativeWindow separately.
if(ANDROID)
    set(GLFW_BUILD_X11 OFF CACHE BOOL "Build GLFW X11 support" FORCE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "Build GLFW Wayland support" FORCE)
endif()
add_subdirectory(src/ThirdParty/glfw EXCLUDE_FROM_ALL)
set(MODULARITY_OPENGL_LIBRARIES)
if(MODULARITY_USE_OPENGL_ES)
    if(ANDROID)
        find_library(MODULARITY_GLES_LIBRARY NAMES GLESv3 GLESv2 REQUIRED)
        find_library(MODULARITY_EGL_LIBRARY EGL REQUIRED)
        find_library(MODULARITY_ANDROID_LIBRARY android REQUIRED)
        find_library(MODULARITY_ANDROID_LOG_LIBRARY log REQUIRED)
        list(APPEND MODULARITY_OPENGL_LIBRARIES
            ${MODULARITY_GLES_LIBRARY}
            ${MODULARITY_EGL_LIBRARY}
            ${MODULARITY_ANDROID_LIBRARY}
            ${MODULARITY_ANDROID_LOG_LIBRARY}
        )
    else()
        find_library(MODULARITY_GLES_LIBRARY NAMES GLESv3 GLESv2)
        find_library(MODULARITY_EGL_LIBRARY EGL)
        if(NOT MODULARITY_GLES_LIBRARY OR NOT MODULARITY_EGL_LIBRARY)
            message(FATAL_ERROR "MODULARITY_USE_OPENGL_ES requires GLES and EGL libraries.")
        endif()
        list(APPEND MODULARITY_OPENGL_LIBRARIES ${MODULARITY_GLES_LIBRARY} ${MODULARITY_EGL_LIBRARY})
    endif()
    message(STATUS "OpenGL ES backend compatibility enabled.")
else()
    find_package(OpenGL REQUIRED)
    list(APPEND MODULARITY_OPENGL_LIBRARIES OpenGL::GL)
endif()
find_package(PkgConfig QUIET)
set(SNDFILE_FOUND FALSE)
if(MODULARITY_ENABLE_SNDFILE AND PkgConfig_FOUND)
    pkg_check_modules(SNDFILE QUIET IMPORTED_TARGET sndfile)
endif()

set(OPUSFILE_FOUND FALSE)
if(MODULARITY_ENABLE_OPUSFILE AND PkgConfig_FOUND)
    pkg_check_modules(OPUSFILE QUIET IMPORTED_TARGET opusfile)
    if(OPUSFILE_FOUND)
        message(STATUS "Ogg/Opus decoding enabled via libopusfile ${OPUSFILE_VERSION}.")
    else()
        message(STATUS "libopusfile not found; Ogg/Opus playback disabled. "
                       "Install one of: opusfile (Arch), libopusfile-dev (Debian/Ubuntu), "
                       "opusfile-devel (Fedora). Pass -DMODULARITY_ENABLE_OPUSFILE=OFF to silence.")
    endif()
endif()

set(MODULARITY_HAS_FFMPEG OFF)

# ---- Windows: vendored FFmpeg ----
# Windows builds link the FFmpeg checked into the tree rather than whatever the
# host environment (PATH / CMAKE_PREFIX_PATH / a vcpkg or MSYS install) happens
# to expose. That keeps cross-builds reproducible and guarantees the runtime
# DLLs we ship match the import libraries we linked against.
set(MODULARITY_FFMPEG_VENDOR_DIR "${PROJECT_SOURCE_DIR}/src/ThirdParty/FFMPEG-8.1.2"
    CACHE PATH "Vendored Windows FFmpeg distribution (include/, lib/, bin/)")
set(MODULARITY_FFMPEG_COMPONENTS avformat avcodec avutil swresample swscale)

# The vendored distribution ships both MSVC (<stem>.lib) and MinGW
# (lib<stem>.dll.a) import libraries; pick the flavour the active toolchain
# links with, falling back to the other so a mismatched setup still configures.
function(modularity_find_vendored_ffmpeg_import_lib out_var stem)
    if(MSVC)
        set(candidates
            "${MODULARITY_FFMPEG_VENDOR_DIR}/lib/${stem}.lib"
            "${MODULARITY_FFMPEG_VENDOR_DIR}/lib/lib${stem}.dll.a")
    else()
        set(candidates
            "${MODULARITY_FFMPEG_VENDOR_DIR}/lib/lib${stem}.dll.a"
            "${MODULARITY_FFMPEG_VENDOR_DIR}/lib/${stem}.lib")
    endif()

    foreach(candidate IN LISTS candidates)
        if(EXISTS "${candidate}")
            set(${out_var} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

if(WIN32 AND EXISTS "${MODULARITY_FFMPEG_VENDOR_DIR}/include/libavformat/avformat.h")
    set(ffmpeg_vendor_libs)
    set(ffmpeg_vendor_missing)
    foreach(ffmpeg_component IN LISTS MODULARITY_FFMPEG_COMPONENTS)
        modularity_find_vendored_ffmpeg_import_lib(ffmpeg_vendor_lib "${ffmpeg_component}")
        if(ffmpeg_vendor_lib)
            list(APPEND ffmpeg_vendor_libs "${ffmpeg_vendor_lib}")
        else()
            list(APPEND ffmpeg_vendor_missing "${ffmpeg_component}")
        endif()
    endforeach()

    if(ffmpeg_vendor_missing)
        message(FATAL_ERROR
            "Vendored FFmpeg at ${MODULARITY_FFMPEG_VENDOR_DIR} has headers but no import "
            "libraries for: ${ffmpeg_vendor_missing}. Restore its lib/ directory, or point "
            "-DMODULARITY_FFMPEG_VENDOR_DIR at a complete distribution.")
    endif()

    add_library(modularity_ffmpeg INTERFACE)
    # SYSTEM: FFmpeg's own inline helpers (libavutil/common.h) trip C4244 in every
    # engine file that includes them - hundreds of warnings that are not ours to fix.
    target_include_directories(modularity_ffmpeg SYSTEM INTERFACE "${MODULARITY_FFMPEG_VENDOR_DIR}/include")
    target_link_libraries(modularity_ffmpeg INTERFACE ${ffmpeg_vendor_libs})

    # The shared build suffixes its DLLs with the ABI major (avcodec-62.dll),
    # so resolve them by glob instead of hardcoding the current versions.
    foreach(ffmpeg_component IN LISTS MODULARITY_FFMPEG_COMPONENTS)
        file(GLOB ffmpeg_vendor_dlls
            "${MODULARITY_FFMPEG_VENDOR_DIR}/bin/${ffmpeg_component}.dll"
            "${MODULARITY_FFMPEG_VENDOR_DIR}/bin/${ffmpeg_component}-*.dll"
            "${MODULARITY_FFMPEG_VENDOR_DIR}/bin/lib${ffmpeg_component}.dll"
            "${MODULARITY_FFMPEG_VENDOR_DIR}/bin/lib${ffmpeg_component}-*.dll")
        list(SORT ffmpeg_vendor_dlls COMPARE NATURAL ORDER DESCENDING)
        if(ffmpeg_vendor_dlls)
            list(GET ffmpeg_vendor_dlls 0 ffmpeg_vendor_dll)
            list(APPEND MODULARITY_FFMPEG_RUNTIME_DLLS "${ffmpeg_vendor_dll}")
        else()
            message(FATAL_ERROR
                "Vendored FFmpeg at ${MODULARITY_FFMPEG_VENDOR_DIR} is missing the runtime "
                "DLL for ${ffmpeg_component} in bin/.")
        endif()
    endforeach()
    # No MODULARITY_FFMPEG_RUNTIME_DLL_DIRS entry here: the vendored build's
    # five linked DLLs depend only on each other, and copying all of bin/ also
    # dragged avfilter (~109 MB) and avdevice next to every dev executable.

    message(STATUS "Using vendored Windows FFmpeg: ${MODULARITY_FFMPEG_VENDOR_DIR}")
    message(STATUS "Windows FFmpeg runtime DLLs: ${MODULARITY_FFMPEG_RUNTIME_DLLS}")
    set(MODULARITY_HAS_FFMPEG ON)
endif()

# pkg_check_modules runs against the host's pkg-config — on Android cross-
# compile that would find x86_64 desktop libs and pollute the link line
# with -lavformat etc. that don't exist in the NDK sysroot. Skip on Android.
if(NOT MODULARITY_HAS_FFMPEG AND PkgConfig_FOUND AND NOT WIN32 AND NOT ANDROID)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
        libswresample
        libswscale
    )
endif()

if(MODULARITY_HAS_FFMPEG)
    # Already satisfied by the vendored Windows distribution above.
elseif(FFMPEG_FOUND)
    add_library(modularity_ffmpeg INTERFACE)
    target_link_libraries(modularity_ffmpeg INTERFACE PkgConfig::FFMPEG)
    set(MODULARITY_HAS_FFMPEG ON)
else()
    find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h)
    find_library(FFMPEG_AVFORMAT_LIBRARY avformat)
    find_library(FFMPEG_AVCODEC_LIBRARY avcodec)
    find_library(FFMPEG_AVUTIL_LIBRARY avutil)
    find_library(FFMPEG_SWRESAMPLE_LIBRARY swresample)
    find_library(FFMPEG_SWSCALE_LIBRARY swscale)

    if(FFMPEG_INCLUDE_DIR AND
       FFMPEG_AVFORMAT_LIBRARY AND
       FFMPEG_AVCODEC_LIBRARY AND
       FFMPEG_AVUTIL_LIBRARY AND
       FFMPEG_SWRESAMPLE_LIBRARY AND
       FFMPEG_SWSCALE_LIBRARY)
        add_library(modularity_ffmpeg INTERFACE)
        target_include_directories(modularity_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIR})
        target_link_libraries(modularity_ffmpeg INTERFACE
            ${FFMPEG_AVFORMAT_LIBRARY}
            ${FFMPEG_AVCODEC_LIBRARY}
            ${FFMPEG_AVUTIL_LIBRARY}
            ${FFMPEG_SWRESAMPLE_LIBRARY}
            ${FFMPEG_SWSCALE_LIBRARY}
        )
        if(WIN32)
            modularity_find_windows_imported_runtime_dll(FFMPEG_AVFORMAT_DLL "${FFMPEG_AVFORMAT_LIBRARY}" "avformat")
            modularity_find_windows_imported_runtime_dll(FFMPEG_AVCODEC_DLL "${FFMPEG_AVCODEC_LIBRARY}" "avcodec")
            modularity_find_windows_imported_runtime_dll(FFMPEG_AVUTIL_DLL "${FFMPEG_AVUTIL_LIBRARY}" "avutil")
            modularity_find_windows_imported_runtime_dll(FFMPEG_SWRESAMPLE_DLL "${FFMPEG_SWRESAMPLE_LIBRARY}" "swresample")
            modularity_find_windows_imported_runtime_dll(FFMPEG_SWSCALE_DLL "${FFMPEG_SWSCALE_LIBRARY}" "swscale")
            foreach(ffmpeg_runtime_dll IN ITEMS
                "${FFMPEG_AVFORMAT_DLL}"
                "${FFMPEG_AVCODEC_DLL}"
                "${FFMPEG_AVUTIL_DLL}"
                "${FFMPEG_SWRESAMPLE_DLL}"
                "${FFMPEG_SWSCALE_DLL}"
            )
                if(ffmpeg_runtime_dll)
                    list(APPEND MODULARITY_FFMPEG_RUNTIME_DLLS "${ffmpeg_runtime_dll}")
                    get_filename_component(ffmpeg_runtime_dir "${ffmpeg_runtime_dll}" DIRECTORY)
                    list(APPEND MODULARITY_FFMPEG_RUNTIME_DLL_DIRS "${ffmpeg_runtime_dir}")
                endif()
            endforeach()
            list(REMOVE_DUPLICATES MODULARITY_FFMPEG_RUNTIME_DLLS)
            list(REMOVE_DUPLICATES MODULARITY_FFMPEG_RUNTIME_DLL_DIRS)
            if(MODULARITY_FFMPEG_RUNTIME_DLLS)
                message(STATUS "Windows FFmpeg runtime DLLs: ${MODULARITY_FFMPEG_RUNTIME_DLLS}")
                message(STATUS "Windows FFmpeg runtime DLL directories: ${MODULARITY_FFMPEG_RUNTIME_DLL_DIRS}")
            else()
                message(WARNING "FFmpeg import libraries were found, but matching Windows runtime DLLs were not detected. The executables will rely on PATH to locate avformat/avcodec/avutil/swresample/swscale.")
            endif()
        endif()
        set(MODULARITY_HAS_FFMPEG ON)
    endif()
endif()

if(NOT MODULARITY_HAS_FFMPEG)
    if(ANDROID)
        # FFmpeg isn't cross-compiled for Android in this tree yet. Video
        # playback is unsupported in the first APK target; resurrect this
        # check once an NDK FFmpeg path lands.
        message(STATUS "FFmpeg unavailable for Android build — video features disabled.")
    else()
        message(FATAL_ERROR "FFmpeg development libraries are required for Modularity video playback integration.")
    endif()
endif()

# ==================== Mono (managed scripting) ====================
option(MODULARITY_USE_MONO "Enable Mono embedding for managed scripts" ${MODULARITY_USE_MONO_DEFAULT})

if(MODULARITY_USE_MONO)
    set(MONO_ROOT "" CACHE PATH "Mono root directory")
    if(NOT MONO_ROOT)
        if(DEFINED ENV{MONO_ROOT} AND NOT "$ENV{MONO_ROOT}" STREQUAL "")
            set(MONO_ROOT "$ENV{MONO_ROOT}" CACHE PATH "Mono root directory" FORCE)
        else()
            set(MONO_ROOT "${PROJECT_SOURCE_DIR}/src/ThirdParty/mono" CACHE PATH "Mono root directory" FORCE)
        endif()
    endif()
    find_path(MONO_INCLUDE_DIR mono/jit/jit.h
        HINTS
            ${MONO_ROOT}/include/mono-2.0
            ${MONO_ROOT}/include
    )
    find_library(MONO_LIBRARY
        NAMES mono-2.0-sgen mono-2.0 monosgen-2.0
        HINTS
            ${MONO_ROOT}/lib
            ${MONO_ROOT}/lib64
    )
    if(NOT MONO_INCLUDE_DIR OR NOT MONO_LIBRARY)
        message(WARNING "Mono not found. Disabling MODULARITY_USE_MONO. Set MONO_ROOT to a Mono runtime with include/mono-2.0 and libmono-2.0-sgen.")
        set(MODULARITY_USE_MONO OFF CACHE BOOL "Enable Mono embedding for managed scripts" FORCE)
    elseif(WIN32)
        find_file(MONO_RUNTIME_DLL
            NAMES mono-2.0-sgen.dll libmono-2.0-sgen.dll monosgen-2.0.dll
            HINTS
                ${MONO_ROOT}/bin
                ${MONO_ROOT}/lib
                ${MONO_ROOT}/lib64
        )
    endif()
endif()

# GLAD
if(MODULARITY_USE_OPENGL_ES)
    add_library(glad INTERFACE)
else()
    add_library(glad STATIC src/ThirdParty/glad/glad.c)
    target_include_directories(glad PUBLIC src/ThirdParty/glad)
endif()

# GLM (header-only)
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE src/ThirdParty/glm)

# zstd (decompress only, single-file amalgamation)
# Blender 3.0+ writes compressed .blend files with Zstandard and Assimp cannot read
# them. This used to shell out to the zstd CLI, which is usually present on Linux and
# essentially never on Windows. See src/ThirdParty/zstd/README.md for how the
# amalgamation is regenerated and the one local edit made to it.
add_library(zstddec STATIC src/ThirdParty/zstd/zstddeclib.c)
target_include_directories(zstddec PUBLIC src/ThirdParty/zstd)
set_target_properties(zstddec PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    # Third-party code, and the amalgamation is noisy at the warning level the engine
    # builds with. Not ours to fix, and drowning real warnings is worse.
    target_compile_options(zstddec PRIVATE /W0)
else()
    target_compile_options(zstddec PRIVATE -w)
endif()

# ModuGuizmo (ImGuizmo-compatible API)
add_library(imguizmo STATIC
    src/ThirdParty/ModuGuizmo/ImGuizmo.cpp
)
target_include_directories(imguizmo PUBLIC src/ThirdParty/ModuGuizmo)
target_link_libraries(imguizmo PUBLIC imgui glm)

# ModuGUI (ImGui fork)
set(MODUGUI_DIR ${PROJECT_SOURCE_DIR}/src/ThirdParty/ModuGUI)

set(IMGUI_SOURCES
    ${MODUGUI_DIR}/imgui.cpp
    ${MODUGUI_DIR}/imgui_demo.cpp
    ${MODUGUI_DIR}/imgui_draw.cpp
    ${MODUGUI_DIR}/imgui_shade.cpp
    ${MODUGUI_DIR}/imgui_tables.cpp
    ${MODUGUI_DIR}/imgui_widgets.cpp
    ${MODUGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
# The GLFW backend has no working Android implementation. On Android the
# eventual ImGui platform layer will be driven by android_native_app_glue
# instead (see docs/AndroidRuntime.md Stage 2).
if(NOT ANDROID)
    list(APPEND IMGUI_SOURCES ${MODUGUI_DIR}/backends/imgui_impl_glfw.cpp)
endif()
if(MODULARITY_HAS_VULKAN)
    list(APPEND IMGUI_SOURCES ${MODUGUI_DIR}/backends/imgui_impl_vulkan.cpp)
endif()
find_package(Freetype)
if(Freetype_FOUND)
    list(APPEND IMGUI_SOURCES ${MODUGUI_DIR}/misc/freetype/imgui_freetype.cpp)
endif()

add_library(imgui STATIC ${IMGUI_SOURCES})
if(WIN32 AND MSVC)
    target_compile_definitions(imgui PRIVATE IMGUI_API=__declspec\(dllexport\))
endif()


target_include_directories(imgui PUBLIC
    ${MODUGUI_DIR}
    ${MODUGUI_DIR}/backends
)
if(Freetype_FOUND)
    target_include_directories(imgui PUBLIC ${MODUGUI_DIR}/misc/freetype)
    target_link_libraries(imgui PUBLIC Freetype::Freetype)
endif()
if(NOT ANDROID)
    target_include_directories(imgui PUBLIC
        ${PROJECT_SOURCE_DIR}/src/ThirdParty/glfw/include
    )
    target_link_libraries(imgui PRIVATE glfw)
endif()
if(MODULARITY_HAS_VULKAN)
    target_link_libraries(imgui PRIVATE Vulkan::Vulkan)
endif()
target_compile_definitions(imgui PUBLIC
    MODULARITY_OPENGL_ES=$<BOOL:${MODULARITY_USE_OPENGL_ES}>
    $<$<BOOL:${MODULARITY_USE_OPENGL_ES}>:IMGUI_IMPL_OPENGL_ES3>
)
if(Freetype_FOUND)
    target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_FREETYPE IMGUI_USE_WCHAR32)
endif()



# ==================== Your code (separated files) ====================
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/*.cpp
)

file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/*.h
    ${PROJECT_SOURCE_DIR}/src/*.hpp
)

set(MODULARITY_EDITOR_TEXTEDITOR_SOURCE
    ${PROJECT_SOURCE_DIR}/src/ThirdParty/ImGuiColorTextEdit/TextEditor.cpp
)

set(MODULARITY_PLAYER_EDITOR_ONLY_SOURCES
    ${MODULARITY_EDITOR_TEXTEDITOR_SOURCE}
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/AnimationWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/AssemblageWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/AssemblageViewport.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/BuildSettingsWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/EnvironmentWindows.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/FileBrowserWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/GameProfilerWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/LightmappingWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/Nebula/NebulaLightmap.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/ModularityDoctorWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/PixelSpriteEditorWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/ProjectManagerWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/RegistryPackagesWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/XRPreviewWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/VREditor.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/VREditorWorkspace.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/TextureTranscodeTool.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/VersionControlWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/VersionControl.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/Panels/SceneHierarchyWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/EditorWindows/ScriptingWindow.cpp
)

list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/ThirdParty/.*")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/ThirdParty/assimp/.*")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/ThirdParty/PhysX/.*")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/main_lsp\\.cpp$")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/main_player.cpp")
# Anchor TU for the Android editor .so target only; not part of the engine libs.
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/AndroidEditorEntry\\.cpp$")
list(FILTER ENGINE_HEADERS EXCLUDE REGEX ".*/ThirdParty/.*")
list(FILTER ENGINE_HEADERS EXCLUDE REGEX ".*/ThirdParty/assimp/.*")
list(FILTER ENGINE_HEADERS EXCLUDE REGEX ".*/ThirdParty/PhysX/.*")

# src/XR/ is the OpenXR backend. It stays in the source list unconditionally:
# the files that need <openxr/openxr.h> guard their whole body with
# `#if MODULARITY_HAS_OPENXR` (same shape as AndroidRuntime.cpp's __ANDROID__
# guard), so with OpenXR off they compile to empty translation units and the
# vendored headers are never reached. XRSettings/XRDiagnostics carry no OpenXR
# dependency at all and must always build, since Project Settings serializes
# them either way.

# Android runtime files compile against the NDK and android_native_app_glue.
# Keep them out of desktop builds; they get added back only when targeting
# Android (see the conditional block below).
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/AndroidRuntime/.*")
list(FILTER ENGINE_HEADERS EXCLUDE REGEX ".*/AndroidRuntime/.*")
if(ANDROID)
    file(GLOB_RECURSE ANDROID_RUNTIME_SOURCES CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/src/AndroidRuntime/*.cpp
    )
    file(GLOB_RECURSE ANDROID_RUNTIME_HEADERS CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/src/AndroidRuntime/*.h
    )
    list(APPEND ENGINE_SOURCES ${ANDROID_RUNTIME_SOURCES})
    list(APPEND ENGINE_HEADERS ${ANDROID_RUNTIME_HEADERS})

    # android_native_app_glue ships with the NDK as a single .c/.h pair.
    # Build it as a small static lib so the player .so can link android_main
    # and the lifecycle pump without dragging in build-system glue.
    set(MODULARITY_NATIVE_APP_GLUE_DIR ${ANDROID_NDK}/sources/android/native_app_glue)
    if(NOT EXISTS ${MODULARITY_NATIVE_APP_GLUE_DIR}/android_native_app_glue.c)
        message(FATAL_ERROR
            "Could not find android_native_app_glue.c at "
            "${MODULARITY_NATIVE_APP_GLUE_DIR}. The NDK install at "
            "${ANDROID_NDK} looks incomplete.")
    endif()
    add_library(android_native_app_glue STATIC
        ${MODULARITY_NATIVE_APP_GLUE_DIR}/android_native_app_glue.c
    )
    target_include_directories(android_native_app_glue PUBLIC
        ${MODULARITY_NATIVE_APP_GLUE_DIR}
    )
endif()

set(ENGINE_PLAYER_SOURCES ${ENGINE_SOURCES})
list(APPEND ENGINE_SOURCES ${MODULARITY_EDITOR_TEXTEDITOR_SOURCE})
list(REMOVE_ITEM ENGINE_PLAYER_SOURCES ${MODULARITY_PLAYER_EDITOR_ONLY_SOURCES})

add_library(core STATIC ${ENGINE_SOURCES} ${ENGINE_HEADERS})
modularity_apply_cxx_api_standard(core)
include(cmake/EmbeddedCompilers.cmake)
if(MODULARITY_ENABLE_EMBEDDED_COMPILERS)
    target_link_libraries(core PUBLIC modularity_embedded_compilers)
    target_compile_definitions(core PRIVATE MODULARITY_HAS_EMBEDDED_COMPILERS=1)
endif()

# Photon Realtime backend. Only src/Network/PhotonBackend.cpp sees these headers
# and libraries; the rest of the engine stays transport-agnostic.
if(MODULARITY_ENABLE_PHOTON)
    if(NOT MODULARITY_PHOTON_SDK_ROOT)
        message(FATAL_ERROR
            "MODULARITY_ENABLE_PHOTON=ON but MODULARITY_PHOTON_SDK_ROOT is not set. Install the "
            "photon.realtime-sdk-windows (Windows) or photon.realtime-sdk (Linux) package, or pass "
            "-DMODULARITY_PHOTON_SDK_ROOT=<path>.")
    endif()
    modularity_photon_resolve_libs("${MODULARITY_PHOTON_SDK_ROOT}" _modularity_photon_link)
    if(_modularity_photon_link_REASON)
        message(FATAL_ERROR
            "MODULARITY_ENABLE_PHOTON=ON but the SDK at ${MODULARITY_PHOTON_SDK_ROOT} carries no "
            "Photon libraries this toolchain can link: ${_modularity_photon_link_REASON}. "
            "Install photon.realtime-sdk-windows on Windows or photon.realtime-sdk on Linux, or "
            "point -DMODULARITY_PHOTON_SDK_ROOT=<path> at the matching SDK.")
    endif()
    # Debug and release libraries are not interchangeable: upstream builds them
    # against different CRTs, so linking the release set into a Debug build pulls
    # two CRTs into one binary. CMAKE_BUILD_TYPE cannot make this call - it is
    # empty under multi-config generators, which is exactly what build.ps1 uses -
    # so the choice is deferred to generate time, per configuration.
    set(MODULARITY_PHOTON_LIBRARIES "")
    foreach(_photon_lib IN LISTS _modularity_photon_link_DEBUG)
        list(APPEND MODULARITY_PHOTON_LIBRARIES "$<$<CONFIG:Debug>:${_photon_lib}>")
    endforeach()
    foreach(_photon_lib IN LISTS _modularity_photon_link_RELEASE)
        list(APPEND MODULARITY_PHOTON_LIBRARIES "$<$<NOT:$<CONFIG:Debug>>:${_photon_lib}>")
    endforeach()
    # Scoped to PhotonBackend.cpp ONLY, never the whole target. The SDK ships
    # Common-cpp/inc/Common.h, which shadows the engine's own src/Common.h (the
    # header that pulls in glm) for every translation unit that sees it. Adding
    # these include paths target-wide broke unrelated files such as
    # Render25D/TMPresentation.h with "'glm' does not name a type".
    #
    # This also enforces the architecture: PhotonBackend.cpp is the only file
    # permitted to see a Photon header.
    set_source_files_properties("${PROJECT_SOURCE_DIR}/src/Network/PhotonBackend.cpp"
        PROPERTIES
            INCLUDE_DIRECTORIES
                "${MODULARITY_PHOTON_SDK_ROOT};${MODULARITY_PHOTON_SDK_ROOT}/Common-cpp/inc;${MODULARITY_PHOTON_SDK_ROOT}/LoadBalancing-cpp/inc;${MODULARITY_PHOTON_SDK_ROOT}/Photon-cpp/inc"
            COMPILE_DEFINITIONS
                "MODULARITY_ENABLE_PHOTON=1")
    target_link_libraries(core PUBLIC ${MODULARITY_PHOTON_LIBRARIES})
    # core_player compiles the same PhotonBackend.cpp (source-file properties are
    # per-directory, not per-target), so it needs the libraries too or the player
    # fails to link with undefined ExitGames symbols.
    if(TARGET core_player)
        target_link_libraries(core_player PUBLIC ${MODULARITY_PHOTON_LIBRARIES})
    endif()
    # Name the library actually chosen: its filename encodes the toolset tag, CRT
    # flavour and architecture on Windows, so a mismatched link is visible here
    # rather than only in the linker output.
    list(GET _modularity_photon_link_RELEASE 0 _photon_reported_lib)
    get_filename_component(_photon_reported_lib "${_photon_reported_lib}" NAME)
    message(STATUS "Photon Realtime backend enabled (${MODULARITY_PHOTON_SDK_ROOT}, linking ${_photon_reported_lib})")
endif()
set(MODULARITY_ASSIMP_INCLUDE_DIRS
    ${PROJECT_SOURCE_DIR}/src/ThirdParty/assimp/include
    ${PROJECT_SOURCE_DIR}/src/ThirdParty/assimp/contrib/unzip
    ${PROJECT_SOURCE_DIR}/src/ThirdParty/assimp/contrib/zlib
    ${CMAKE_BINARY_DIR}/src/ThirdParty/assimp/contrib/zlib
)
if(MODULARITY_ENABLE_ASSIMP)
    set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "Disable Assimp warnings as errors" FORCE)
    set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "Disable Assimp tests" FORCE)
    set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "Disable Assimp samples" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "Disable Assimp tools" FORCE)
    set(ASSIMP_INSTALL OFF CACHE BOOL "Disable Assimp install targets" FORCE)
    if(ANDROID)
        set(ASSIMP_NO_EXPORT ON CACHE BOOL "Disable Assimp exporters for Android runtime builds" FORCE)
    endif()
    if(WIN32)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build package with shared libraries." FORCE)
    endif()
    # Force vendored zlib/minizip in assimp to avoid host minizip/unzip.h dependency on clean builds.
    set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "Build Assimp zlib/minizip from source" FORCE)
    set(ASSIMP_BUILD_MINIZIP ON CACHE BOOL "Build Assimp minizip from source" FORCE)
    # Some distros expose broken minizip pkg-config include dirs (/usr/include instead of /usr/include/minizip).
    # Ensure assimp does not prefer that cached result.
    set(UNZIP_FOUND FALSE CACHE INTERNAL "Force Assimp to use vendored unzip headers" FORCE)
    if(MODULARITY_ASSIMP_IMPORTERS)
        set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "Enable all Assimp importers by default" FORCE)
        foreach(_modularity_importer IN LISTS MODULARITY_ASSIMP_IMPORTERS)
            string(TOUPPER "${_modularity_importer}" _modularity_importer_upper)
            set(ASSIMP_BUILD_${_modularity_importer_upper}_IMPORTER ON CACHE BOOL "" FORCE)
        endforeach()
    endif()
    add_subdirectory(src/ThirdParty/assimp EXCLUDE_FROM_ALL)
    target_include_directories(assimp PRIVATE ${PROJECT_SOURCE_DIR}/src/ThirdParty/assimp/contrib/unzip)
    target_link_libraries(core PUBLIC assimp)
    set(MODULARITY_ARCHIVE_LIBRARIES assimp)
else()
    add_subdirectory(src/ThirdParty/assimp/contrib/zlib ${CMAKE_BINARY_DIR}/modularity-zlib EXCLUDE_FROM_ALL)
    add_library(modularity_minizip STATIC
        ${PROJECT_SOURCE_DIR}/src/ThirdParty/assimp/contrib/unzip/ioapi.c
        ${PROJECT_SOURCE_DIR}/src/ThirdParty/assimp/contrib/unzip/unzip.c
    )
    target_include_directories(modularity_minizip PUBLIC
        ${PROJECT_SOURCE_DIR}/src/ThirdParty/assimp/contrib/unzip
        ${PROJECT_SOURCE_DIR}/src/ThirdParty/assimp/contrib/zlib
        ${CMAKE_BINARY_DIR}/modularity-zlib
    )
    target_link_libraries(modularity_minizip PUBLIC zlibstatic)
    set(MODULARITY_ARCHIVE_LIBRARIES modularity_minizip)
endif()
target_include_directories(core PUBLIC 
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
    ${MODULARITY_ASSIMP_INCLUDE_DIRS}
)
target_link_libraries(core PUBLIC ${MODULARITY_ARCHIVE_LIBRARIES})
target_link_libraries(core PUBLIC glad glm imgui imguizmo zstddec)
if(MODULARITY_HAS_FFMPEG)
    target_link_libraries(core PUBLIC modularity_ffmpeg)
endif()
if(MODULARITY_HAS_VULKAN)
    target_link_libraries(core PUBLIC Vulkan::Vulkan)
endif()
if(MODULARITY_USE_MONO)
    target_include_directories(core PUBLIC ${MONO_INCLUDE_DIR})
    target_link_libraries(core PUBLIC ${MONO_LIBRARY})
endif()
target_compile_definitions(core PUBLIC MODULARITY_USE_MONO=$<BOOL:${MODULARITY_USE_MONO}>)
target_compile_definitions(core PUBLIC MODULARITY_HAS_FFMPEG=$<BOOL:${MODULARITY_HAS_FFMPEG}>)
target_compile_definitions(core PUBLIC MODULARITY_HAS_VULKAN=$<BOOL:${MODULARITY_HAS_VULKAN}>)
target_compile_definitions(core PUBLIC MODULARITY_HAS_SNDFILE=$<BOOL:${SNDFILE_FOUND}>)
target_compile_definitions(core PUBLIC MODULARITY_HAS_OPUSFILE=$<BOOL:${OPUSFILE_FOUND}>)
target_compile_definitions(core PUBLIC MODULARITY_OPENGL_ES=$<BOOL:${MODULARITY_USE_OPENGL_ES}>)
target_compile_definitions(core PUBLIC MODULARITY_HAS_OPENXR=$<BOOL:${MODULARITY_ENABLE_OPENXR}>)
target_compile_definitions(core PUBLIC MODULARITY_HAS_DISCORD_SOCIAL_SDK=$<BOOL:${MODULARITY_HAS_DISCORD_SOCIAL_SDK}>)
if(MODULARITY_HAS_DISCORD_SOCIAL_SDK)
    target_link_libraries(core PUBLIC modularity_discord_social)
endif()
if(MODULARITY_ENABLE_OPENXR)
    # Headers only. The loader is dlopen'd, so nothing is added to the link line
    # here; ${CMAKE_DL_LIBS} is already linked by the executable targets below.
    target_include_directories(core PUBLIC ${MODULARITY_OPENXR_INCLUDE_DIR})
endif()
target_compile_definitions(core PUBLIC MODULARITY_ENABLE_ASSIMP=$<BOOL:${MODULARITY_ENABLE_ASSIMP}>)
target_compile_definitions(core PUBLIC MODULARITY_SOURCE_DIR="${MODULARITY_SOURCE_DIR_CMAKE}")
if(WIN32)
    target_compile_definitions(core PUBLIC ${MODULARITY_WINDOWS_DEFINES} MODULARITY_SCRIPT_HOST)
    # dbghelp backs CrashReporter's structured-exception reporting: StackWalk64,
    # SymFromAddr and MiniDumpWriteDump. It ships with the Windows SDK.
    target_link_libraries(core PUBLIC dbghelp)
endif()
if(SNDFILE_FOUND)
    target_link_libraries(core PUBLIC PkgConfig::SNDFILE)
endif()
if(OPUSFILE_FOUND)
    target_link_libraries(core PUBLIC PkgConfig::OPUSFILE)
endif()
target_compile_options(core PRIVATE ${MODULARITY_WARNING_FLAGS})
modularity_apply_sanitizers(core)

# On Android the player ships as a .so loaded by NativeActivity, not as an
# executable. Build core_player itself as SHARED with output name
# ModularityPlayer so the result is libModularityPlayer.so.
if(ANDROID)
    add_library(core_player SHARED ${ENGINE_PLAYER_SOURCES} ${ENGINE_HEADERS})
    set_target_properties(core_player PROPERTIES OUTPUT_NAME ModularityPlayer)
else()
    add_library(core_player STATIC ${ENGINE_PLAYER_SOURCES} ${ENGINE_HEADERS})
endif()
modularity_apply_cxx_api_standard(core_player)
target_compile_definitions(core_player PUBLIC MODULARITY_PLAYER)
target_compile_definitions(core_player PUBLIC MODULARITY_RUNTIME_ONLY=1)
target_link_libraries(core_player PUBLIC ${MODULARITY_ARCHIVE_LIBRARIES})
# core_player compiles the same src/Network/PhotonBackend.cpp: source-file
# properties (the MODULARITY_ENABLE_PHOTON define and the SDK include paths) are
# per-directory, not per-target, so the player needs the same libraries or it
# fails to link with undefined ExitGames symbols.
if(MODULARITY_ENABLE_PHOTON)
    target_link_libraries(core_player PUBLIC ${MODULARITY_PHOTON_LIBRARIES})
endif()
target_include_directories(core_player PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
    ${MODULARITY_ASSIMP_INCLUDE_DIRS}
)
target_link_libraries(core_player PUBLIC glad glm imgui imguizmo zstddec)
if(MODULARITY_HAS_FFMPEG)
    target_link_libraries(core_player PUBLIC modularity_ffmpeg)
endif()
if(MODULARITY_HAS_VULKAN)
    target_link_libraries(core_player PUBLIC Vulkan::Vulkan)
endif()
if(MODULARITY_USE_MONO)
    target_include_directories(core_player PUBLIC ${MONO_INCLUDE_DIR})
    target_link_libraries(core_player PUBLIC ${MONO_LIBRARY})
endif()
target_compile_definitions(core_player PUBLIC MODULARITY_USE_MONO=$<BOOL:${MODULARITY_USE_MONO}>)
target_compile_definitions(core_player PUBLIC MODULARITY_HAS_FFMPEG=$<BOOL:${MODULARITY_HAS_FFMPEG}>)
target_compile_definitions(core_player PUBLIC MODULARITY_HAS_VULKAN=$<BOOL:${MODULARITY_HAS_VULKAN}>)
target_compile_definitions(core_player PUBLIC MODULARITY_HAS_SNDFILE=$<BOOL:${SNDFILE_FOUND}>)
target_compile_definitions(core_player PUBLIC MODULARITY_HAS_OPUSFILE=$<BOOL:${OPUSFILE_FOUND}>)
target_compile_definitions(core_player PUBLIC MODULARITY_OPENGL_ES=$<BOOL:${MODULARITY_USE_OPENGL_ES}>)
target_compile_definitions(core_player PUBLIC MODULARITY_HAS_OPENXR=$<BOOL:${MODULARITY_ENABLE_OPENXR}>)
target_compile_definitions(core_player PUBLIC MODULARITY_HAS_DISCORD_SOCIAL_SDK=$<BOOL:${MODULARITY_HAS_DISCORD_SOCIAL_SDK}>)
if(MODULARITY_HAS_DISCORD_SOCIAL_SDK)
    target_link_libraries(core_player PUBLIC modularity_discord_social)
endif()
if(MODULARITY_ENABLE_OPENXR)
    target_include_directories(core_player PUBLIC ${MODULARITY_OPENXR_INCLUDE_DIR})
    # On Android the player .so is the final link, so it needs dl itself for
    # XRLoader's dlopen of libopenxr_loader.so (bionic folds dl into libc, but
    # being explicit keeps the desktop shared-player build honest too).
    target_link_libraries(core_player PUBLIC ${CMAKE_DL_LIBS})
endif()
target_compile_definitions(core_player PUBLIC MODULARITY_ENABLE_ASSIMP=$<BOOL:${MODULARITY_ENABLE_ASSIMP}>)
target_compile_definitions(core_player PUBLIC MODULARITY_SOURCE_DIR="${MODULARITY_SOURCE_DIR_CMAKE}")
if(WIN32)
    target_compile_definitions(core_player PUBLIC ${MODULARITY_WINDOWS_DEFINES} MODULARITY_SCRIPT_HOST)
    target_link_libraries(core_player PUBLIC dbghelp)
endif()
if(SNDFILE_FOUND)
    target_link_libraries(core_player PUBLIC PkgConfig::SNDFILE)
endif()
if(OPUSFILE_FOUND)
    target_link_libraries(core_player PUBLIC PkgConfig::OPUSFILE)
endif()
target_compile_options(core_player PRIVATE ${MODULARITY_WARNING_FLAGS})
modularity_apply_sanitizers(core_player)

function(modularity_stage_windows_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    set(runtime_dlls "$<TARGET_RUNTIME_DLLS:${target_name}>")
    if(MODULARITY_USE_MONO AND MONO_RUNTIME_DLL)
        list(APPEND runtime_dlls "${MONO_RUNTIME_DLL}")
    endif()
    if(MODULARITY_FFMPEG_RUNTIME_DLLS)
        list(APPEND runtime_dlls ${MODULARITY_FFMPEG_RUNTIME_DLLS})
    endif()
    if(MSVC AND MODULARITY_ENABLE_ASAN)
        # /fsanitize=address with /MD(d) imports the ASan runtime DLL, which ships next
        # to cl.exe and is on no PATH a double-clicked exe would see.
        get_filename_component(_modularity_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        file(GLOB _modularity_asan_runtime "${_modularity_msvc_bin}/clang_rt.asan*_dynamic-x86_64.dll")
        list(APPEND runtime_dlls ${_modularity_asan_runtime})
    endif()

    set(runtime_dll_dirs ${MODULARITY_FFMPEG_RUNTIME_DLL_DIRS})
    if(MODULARITY_BUNDLE_REDIST_DLLS AND MODULARITY_REDIST_DLL_DIR AND IS_DIRECTORY "${MODULARITY_REDIST_DLL_DIR}")
        message(STATUS "Bundling redist DLLs for ${target_name} from: ${MODULARITY_REDIST_DLL_DIR}")
        list(APPEND runtime_dll_dirs "${MODULARITY_REDIST_DLL_DIR}")
    endif()

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DDST_DIR=$<TARGET_FILE_DIR:${target_name}>"
            "-DRUNTIME_DLLS=$<JOIN:${runtime_dlls},;>"
            "-DRUNTIME_DLL_DIRS=$<JOIN:${runtime_dll_dirs},;>"
            -P "${MODULARITY_RUNTIME_DLL_COPY_SCRIPT}"
        VERBATIM
    )
endfunction()

if(MODULARITY_ENABLE_PHYSX)
    set(PHYSX_ROOT_DIR ${PROJECT_SOURCE_DIR}/src/ThirdParty/PhysX/physx CACHE PATH "PhysX root directory")
    if(WIN32)
    set(TARGET_BUILD_PLATFORM "windows" CACHE STRING "PhysX build platform" FORCE)
    else()
    set(TARGET_BUILD_PLATFORM "linux" CACHE STRING "PhysX build platform" FORCE)
    endif()
    # ---- PhysX hard disables (must be BEFORE add_subdirectory) ----
    set(PX_SUPPORT_GLUT OFF CACHE BOOL "" FORCE)
    set(PX_USE_GLUT OFF CACHE BOOL "" FORCE)
    set(PX_ENABLE_GLUT OFF CACHE BOOL "" FORCE)
    set(PX_BUILDSNIPPETS OFF CACHE BOOL "" FORCE)
    set(PX_BUILDPVDRUNTIME OFF CACHE BOOL "" FORCE)
    set(PX_BUILDPUBLICSAMPLES OFF CACHE BOOL "" FORCE)
    set(PX_BUILDTESTS OFF CACHE BOOL "" FORCE)
    set(PX_BUILDFASTXML OFF CACHE BOOL "" FORCE)
    set(PX_GENERATE_GPU_PROJECTS OFF CACHE BOOL "" FORCE)
    set(PX_GENERATE_GPU_PROJECTS_ONLY OFF CACHE BOOL "" FORCE)

    # PhysX build system expects output locations when using the GameWorks layout.
    set(PX_OUTPUT_LIB_DIR ${CMAKE_BINARY_DIR}/physx CACHE PATH "PhysX output lib directory")
    set(PX_OUTPUT_BIN_DIR ${CMAKE_BINARY_DIR}/physx/bin CACHE PATH "PhysX output bin directory")
    if(WIN32)
        # The PhysX public wrapper is required on Windows because the raw SDK
        # entrypoint references private NVIDIA-only sources when PUBLIC_RELEASE
        # is not enabled. Its public-release path still tries to copy optional
        # freeglut DLLs, so provide local placeholders to satisfy configure.
        set(PHYSX_FREEGLUT_STUB_DIR ${CMAKE_BINARY_DIR}/physx/freeglut-stub/bin/win64)
        file(MAKE_DIRECTORY ${PHYSX_FREEGLUT_STUB_DIR})
        file(WRITE ${PHYSX_FREEGLUT_STUB_DIR}/freeglut.dll "")
        file(WRITE ${PHYSX_FREEGLUT_STUB_DIR}/freeglutd.dll "")
        set(PHYSX_SLN_FREEGLUT_PATH ${CMAKE_BINARY_DIR}/physx/freeglut-stub/bin CACHE PATH "PhysX freeglut copy path" FORCE)
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(MODULARITY_SAVED_CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=free-nonheap-object")
    endif()
    add_subdirectory(${PHYSX_ROOT_DIR}/compiler/public ${CMAKE_BINARY_DIR}/physx)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_CXX_FLAGS "${MODULARITY_SAVED_CMAKE_CXX_FLAGS}")
        unset(MODULARITY_SAVED_CMAKE_CXX_FLAGS)
    endif()
    target_include_directories(core PUBLIC ${PHYSX_ROOT_DIR}/include)
    target_compile_definitions(core PUBLIC MODULARITY_ENABLE_PHYSX PX_PHYSX_STATIC_LIB)
    target_link_libraries(core PUBLIC PhysX PhysXCommon PhysXFoundation PhysXExtensions PhysXCooking)
    target_include_directories(core_player PUBLIC ${PHYSX_ROOT_DIR}/include)
    target_compile_definitions(core_player PUBLIC MODULARITY_ENABLE_PHYSX PX_PHYSX_STATIC_LIB)
    target_link_libraries(core_player PUBLIC PhysX PhysXCommon PhysXFoundation PhysXExtensions PhysXCooking)
endif()

# ==================== JoltPhysics ====================
if(MODULARITY_ENABLE_JOLT)
    set(JOLT_ROOT_DIR ${PROJECT_SOURCE_DIR}/src/ThirdParty/JoltPhysics CACHE PATH "JoltPhysics root directory")

    # Strip Jolt's own demo/test/sample binaries — we only want the static lib.
    set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
    set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
    set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
    # LTO inlines Jolt's hot paths into engine callers and is a real perf
    # win in Release. Turn it off in Debug to keep iteration fast.
    # Jolt is built with slim LTO objects, so every link of Modularity and
    # ModularityPlayer re-runs code generation for all of Jolt - even when only
    # one engine file changed. On a 2-thread Celeron N3060 that is ~3.5 minutes
    # per link. MODULARITY_JOLT_LTO=OFF trades that for plain -O3 Jolt objects.
    option(MODULARITY_JOLT_LTO "Build JoltPhysics with LTO in non-Debug builds (slow links on low-core machines)" ON)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT MODULARITY_JOLT_LTO)
        set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
    else()
        set(INTERPROCEDURAL_OPTIMIZATION ON CACHE BOOL "" FORCE)
    endif()
    # Float exception trapping is MSVC-only in Jolt and noisy elsewhere.
    set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)

    # Jolt emits its instruction-set flags (-mavx2, -mavx512f, ...) as PUBLIC
    # usage requirements, so whatever SIMD level Jolt is built at leaks into
    # the engine executable and raises its x86-64 ISA requirement. Drive Jolt's
    # USE_* switches entirely from MODULARITY_CPU_TARGET so the bundled physics
    # lib never pushes the release above the requested baseline. (See the
    # CPU / ISA target section above.)
    if(NOT ANDROID)
        set(USE_SSE4_1 ${MODULARITY_JOLT_USE_SSE4_1} CACHE BOOL "" FORCE)
        set(USE_SSE4_2 ${MODULARITY_JOLT_USE_SSE4_2} CACHE BOOL "" FORCE)
        set(USE_AVX    ${MODULARITY_JOLT_USE_AVX}    CACHE BOOL "" FORCE)
        set(USE_AVX2   ${MODULARITY_JOLT_USE_AVX2}   CACHE BOOL "" FORCE)
        set(USE_LZCNT  ${MODULARITY_JOLT_USE_LZCNT}  CACHE BOOL "" FORCE)
        set(USE_TZCNT  ${MODULARITY_JOLT_USE_TZCNT}  CACHE BOOL "" FORCE)
        set(USE_F16C   ${MODULARITY_JOLT_USE_F16C}   CACHE BOOL "" FORCE)
        set(USE_FMADD  ${MODULARITY_JOLT_USE_FMADD}  CACHE BOOL "" FORCE)

        # Keep the selected CPU target authoritative. Older builds exposed a
        # separate MODULARITY_JOLT_AVX512 cache option, which could silently turn
        # a later baseline rebuild back into an AVX-512 binary.
        if(DEFINED MODULARITY_JOLT_AVX512)
            if(MODULARITY_JOLT_AVX512)
                message(WARNING
                    "MODULARITY_JOLT_AVX512 is obsolete and has been ignored. "
                    "Use MODULARITY_CPU_TARGET=x86-64-v4 for an AVX-512 build.")
            endif()
            unset(MODULARITY_JOLT_AVX512 CACHE)
        endif()
        set(USE_AVX512 ${MODULARITY_JOLT_USE_AVX512} CACHE BOOL "" FORCE)
    endif()

    # On Android (aarch64) the x86 SIMD options must be off, otherwise Jolt
    # tries to forward -msse* etc. to the NDK clang and fails configure.
    if(ANDROID)
        set(USE_SSE4_1 OFF CACHE BOOL "" FORCE)
        set(USE_SSE4_2 OFF CACHE BOOL "" FORCE)
        set(USE_AVX OFF CACHE BOOL "" FORCE)
        set(USE_AVX2 OFF CACHE BOOL "" FORCE)
        set(USE_AVX512 OFF CACHE BOOL "" FORCE)
        set(USE_LZCNT OFF CACHE BOOL "" FORCE)
        set(USE_TZCNT OFF CACHE BOOL "" FORCE)
        set(USE_F16C OFF CACHE BOOL "" FORCE)
        set(USE_FMADD OFF CACHE BOOL "" FORCE)
        set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
        set(DEBUG_RENDERER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
        set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
        set(PROFILER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
    endif()

    # Force Jolt static. On Android BUILD_SHARED_LIBS is ON by default (via
    # the NDK toolchain / assimp settings), which would produce a separate
    # libJolt.so that we'd have to stage into the APK. A static Jolt gets
    # embedded directly into libModularityPlayer.so — one fewer file to ship.
    set(_modularity_saved_build_shared ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(${JOLT_ROOT_DIR}/Build ${CMAKE_BINARY_DIR}/jolt EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS ${_modularity_saved_build_shared})
    unset(_modularity_saved_build_shared)

    # Jolt installs its own warning flags that don't always agree with the
    # engine's -Werror profile; isolate them to the Jolt target only.
    target_compile_definitions(core PUBLIC MODULARITY_ENABLE_JOLT)
    target_link_libraries(core PUBLIC Jolt::Jolt)
    target_compile_definitions(core_player PUBLIC MODULARITY_ENABLE_JOLT)
    target_link_libraries(core_player PUBLIC Jolt::Jolt)
endif()

# ==================== Executable ====================
if(MODULARITY_BUILD_EDITOR)
    add_executable(Modularity src/main.cpp)
    modularity_apply_cxx_api_standard(Modularity)
    target_compile_options(Modularity PRIVATE ${MODULARITY_WARNING_FLAGS})
    modularity_apply_sanitizers(Modularity)
    modularity_apply_cpu_target(Modularity)
    if(WIN32)
        target_compile_definitions(Modularity PRIVATE ${MODULARITY_WINDOWS_DEFINES})
        set_target_properties(Modularity PROPERTIES
            ENABLE_EXPORTS ON
            WINDOWS_EXPORT_ALL_SYMBOLS ON
            WIN32_EXECUTABLE "$<NOT:$<CONFIG:Debug>>"
        )
        if(MSVC)
            # /DEBUG emits Modularity.pdb from the /Z7 info in the objects, so crash
            # stacks name functions and source lines. It implicitly disables the
            # linker's size optimisations, so ask for them back explicitly - the
            # shipped binary stays identical, the PDB just sits beside it.
            target_link_options(Modularity PRIVATE /DEBUG /OPT:REF /OPT:ICF)
            # Makes CP_ACP UTF-8 so path::string() cannot throw on non-Latin-1
            # filenames. See the comment in the manifest.
            target_sources(Modularity PRIVATE "${PROJECT_SOURCE_DIR}/modularity-utf8.manifest")
        endif()
    endif()
endif()
if(NOT ANDROID)
    add_executable(ModularityPlayer src/main_player.cpp)
    modularity_apply_cxx_api_standard(ModularityPlayer)
    target_compile_options(ModularityPlayer PRIVATE ${MODULARITY_WARNING_FLAGS})
    modularity_apply_sanitizers(ModularityPlayer)
    modularity_apply_cpu_target(ModularityPlayer)
    if(WIN32)
        target_compile_definitions(ModularityPlayer PRIVATE ${MODULARITY_WINDOWS_DEFINES})
        set_target_properties(ModularityPlayer PROPERTIES
            ENABLE_EXPORTS ON
            WINDOWS_EXPORT_ALL_SYMBOLS ON
            WIN32_EXECUTABLE "$<NOT:$<CONFIG:Debug>>"
        )
        if(MSVC)
            target_link_options(ModularityPlayer PRIVATE /DEBUG /OPT:REF /OPT:ICF)
            target_sources(ModularityPlayer PRIVATE "${PROJECT_SOURCE_DIR}/modularity-utf8.manifest")
        endif()
    endif()
endif()

# Link order matters on Linux
if(ANDROID)
    # core_player is the SHARED libModularityPlayer.so. Link the Android
    # platform libs and native_app_glue directly into it. The -u flag forces
    # the linker to keep ANativeActivity_onCreate (defined inside
    # native_app_glue) — without it, --gc-sections drops the symbol and the
    # Android loader cannot find the entry point.
    target_link_libraries(core_player PRIVATE
        android_native_app_glue
        glfw
        ${MODULARITY_OPENGL_LIBRARIES}
        ${CMAKE_DL_LIBS}
    )
    target_compile_options(core_player PRIVATE
        -ffunction-sections
        -fdata-sections
    )
    target_link_options(core_player PRIVATE
        "-u" "ANativeActivity_onCreate"
        "-Wl,--gc-sections"
        "-Wl,--no-undefined"
    )
    # The editor 'core' lib also pulls in AndroidRuntime.cpp (globbed in), so it
    # needs native_app_glue's headers to compile on Android, plus the platform
    # libs so an editor APK (libModularity.so) can be linked straight from core.
    target_link_libraries(core PRIVATE
        android_native_app_glue
        glfw
        ${MODULARITY_OPENGL_LIBRARIES}
        ${CMAKE_DL_LIBS}
    )

    # Editor-on-Android: libModularity.so, the full editor as a NativeActivity .so
    # (android.app.lib_name=Modularity). Built from the editor 'core' lib (which
    # is NOT MODULARITY_RUNTIME_ONLY, so editor panels render) with --whole-archive
    # so android_main + editor symbols survive --gc-sections. Built on demand:
    # `cmake --build <dir> --target Modularity`.
    add_library(ModularityEditorAndroid SHARED ${PROJECT_SOURCE_DIR}/src/AndroidEditorEntry.cpp)
    set_target_properties(ModularityEditorAndroid PROPERTIES OUTPUT_NAME Modularity)
    target_link_libraries(ModularityEditorAndroid PRIVATE
        -Wl,--whole-archive core -Wl,--no-whole-archive
        android_native_app_glue
        glfw
        ${MODULARITY_OPENGL_LIBRARIES}
        ${CMAKE_DL_LIBS}
    )
    target_compile_options(ModularityEditorAndroid PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(ModularityEditorAndroid PRIVATE
        "-u" "ANativeActivity_onCreate"
        "-Wl,--gc-sections"
    )
elseif(NOT WIN32)
    find_package(X11 REQUIRED)
    if(MODULARITY_BUILD_EDITOR)
        target_include_directories(Modularity PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(Modularity PRIVATE
            core
            imgui
            imguizmo
            glad
            glm
            glfw
            ${MODULARITY_OPENGL_LIBRARIES}
            pthread
            dl
            ${X11_LIBRARIES}
            Xrandr
            Xi
            Xinerama
            Xcursor
        )
        # Export symbols so runtime-loaded scripts can resolve ImGui/engine symbols.
        target_link_options(Modularity PRIVATE "-rdynamic")
    endif()

    target_include_directories(ModularityPlayer PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(ModularityPlayer PRIVATE
        core_player
        imgui
        imguizmo
        glad
        glm
        glfw
        ${MODULARITY_OPENGL_LIBRARIES}
        pthread
        dl
        ${X11_LIBRARIES}
        Xrandr
        Xi
        Xinerama
        Xcursor
    )
    target_link_options(ModularityPlayer PRIVATE "-rdynamic")
else()
    if(MODULARITY_BUILD_EDITOR)
        target_link_libraries(Modularity PRIVATE core glfw ${MODULARITY_OPENGL_LIBRARIES})
        if(MSVC)
            target_link_options(Modularity PRIVATE
                "$<$<CONFIG:Debug>:/SUBSYSTEM:CONSOLE>"
                "$<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS>"
                "/WHOLEARCHIVE:$<TARGET_FILE:core>"
                "/WHOLEARCHIVE:$<TARGET_FILE:imgui>"
            )
        endif()
    endif()
    target_link_libraries(ModularityPlayer PRIVATE core_player glfw ${MODULARITY_OPENGL_LIBRARIES})
    if(MSVC)
        target_link_options(ModularityPlayer PRIVATE
            "$<$<CONFIG:Debug>:/SUBSYSTEM:CONSOLE>"
            "$<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS>"
            "/WHOLEARCHIVE:$<TARGET_FILE:core_player>"
            "/WHOLEARCHIVE:$<TARGET_FILE:imgui>"
        )
    endif()
endif()

# ==================== Dev export layout ====================
# build.ps1 configures into build/Dev-Resources and sets this to
# build/Dev-ExportedBuild, so the linker writes the editor and player straight
# into a clean, runnable folder. Debug files (.pdb, and the import .lib/.exp that
# scripts link against) stay in <binary dir>/<config> beside the rest of the
# CMake tree. Empty keeps CMake's per-config folders, which build.sh and the
# editor's own player-cache export build still use.
set(MODULARITY_DEV_EXPORT_DIR "" CACHE PATH
    "Folder the editor and player executables are linked into (empty: per-config build folder)")
if(MODULARITY_DEV_EXPORT_DIR AND NOT ANDROID)
    set(_modularity_dev_export_targets ModularityPlayer)
    if(MODULARITY_BUILD_EDITOR)
        list(APPEND _modularity_dev_export_targets Modularity)
    endif()
    foreach(_modularity_dev_target IN LISTS _modularity_dev_export_targets)
        # A generator expression stops multi-config generators from appending
        # a per-config subfolder, so Release and Debug share the one folder.
        set_target_properties(${_modularity_dev_target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${MODULARITY_DEV_EXPORT_DIR}$<0:>"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
            PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
    endforeach()
    message(STATUS "Dev executables link into: ${MODULARITY_DEV_EXPORT_DIR}")
endif()

# Keep the Social SDK next to development executables. Windows' exported-build
# staging already collects every DLL next to ModularityPlayer; installed Unix
# packages use the lib/ copy below through the existing $ORIGIN/../lib rpath.
if(MODULARITY_HAS_DISCORD_SOCIAL_SDK)
    if(APPLE)
        set(_modularity_discord_build_rpath "@loader_path")
    else()
        set(_modularity_discord_build_rpath "$ORIGIN")
    endif()
    if(MODULARITY_BUILD_EDITOR)
        set_property(TARGET Modularity APPEND PROPERTY BUILD_RPATH "${_modularity_discord_build_rpath}")
        add_custom_command(TARGET Modularity POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:modularity_discord_social>"
                "$<TARGET_FILE_DIR:Modularity>"
            COMMENT "Staging Discord Social SDK for Modularity")
    endif()
    if(NOT ANDROID)
        set_property(TARGET ModularityPlayer APPEND PROPERTY BUILD_RPATH "${_modularity_discord_build_rpath}")
        add_custom_command(TARGET ModularityPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:modularity_discord_social>"
                "$<TARGET_FILE_DIR:ModularityPlayer>"
            COMMENT "Staging Discord Social SDK for ModularityPlayer")
    endif()
endif()

# Copy resources once to avoid parallel target races writing the same directory.
# On Android there is no executable to sit next to — assets will be staged
# into the APK's assets/ directory by the Stage 4 packager instead.
if(NOT ANDROID)
    if(MODULARITY_BUILD_EDITOR)
        add_custom_command(TARGET Modularity POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/Resources
                $<TARGET_FILE_DIR:Modularity>/Resources
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/docs
                $<TARGET_FILE_DIR:Modularity>/docs
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${MODULARITY_TEMPLATE_PROJECTS_DIR}
                $<TARGET_FILE_DIR:Modularity>/Template-Projects
            COMMAND ${CMAKE_COMMAND}
                "-DSRC_ROOT=${CMAKE_SOURCE_DIR}"
                "-DBUILD_ROOT=${CMAKE_BINARY_DIR}"
                "-DDST_ROOT=$<TARGET_FILE_DIR:Modularity>/Resources/ScriptSDK"
                -P "${MODULARITY_SCRIPT_SDK_COPY_SCRIPT}"
        )
    else()
        add_custom_command(TARGET ModularityPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/Resources
                $<TARGET_FILE_DIR:ModularityPlayer>/Resources
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/docs
                $<TARGET_FILE_DIR:ModularityPlayer>/docs
            COMMAND ${CMAKE_COMMAND}
                "-DSRC_ROOT=${CMAKE_SOURCE_DIR}"
                "-DBUILD_ROOT=${CMAKE_BINARY_DIR}"
                "-DDST_ROOT=$<TARGET_FILE_DIR:ModularityPlayer>/Resources/ScriptSDK"
                -P "${MODULARITY_SCRIPT_SDK_COPY_SCRIPT}"
        )
    endif()
endif()

# ==================== Copy PhysX DLLs (Windows) ====================
if(WIN32 AND MODULARITY_ENABLE_PHYSX)
    set(PHYSX_COPY_SCRIPT ${CMAKE_SOURCE_DIR}/cmake/CopyPhysXDLLs.cmake)
    if(MODULARITY_BUILD_EDITOR)
        add_custom_command(TARGET Modularity POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -D SRC_DIR=${PX_OUTPUT_BIN_DIR}
                -D DST_DIR=$<TARGET_FILE_DIR:Modularity>
                -P ${PHYSX_COPY_SCRIPT}
            COMMAND ${CMAKE_COMMAND}
                -D SRC_DIR=${CMAKE_BINARY_DIR}/physx/sdk_source_bin
                -D DST_DIR=$<TARGET_FILE_DIR:Modularity>
                -P ${PHYSX_COPY_SCRIPT}
        )
    endif()
    add_custom_command(TARGET ModularityPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -D SRC_DIR=${PX_OUTPUT_BIN_DIR}
            -D DST_DIR=$<TARGET_FILE_DIR:ModularityPlayer>
            -P ${PHYSX_COPY_SCRIPT}
        COMMAND ${CMAKE_COMMAND}
            -D SRC_DIR=${CMAKE_BINARY_DIR}/physx/sdk_source_bin
            -D DST_DIR=$<TARGET_FILE_DIR:ModularityPlayer>
            -P ${PHYSX_COPY_SCRIPT}
    )
endif()

if(WIN32 AND MODULARITY_BUILD_EDITOR)
    modularity_stage_windows_runtime_dlls(Modularity)
endif()
if(WIN32)
    modularity_stage_windows_runtime_dlls(ModularityPlayer)
endif()

include(GNUInstallDirs)

# Where the editor, player and everything they load at runtime are installed.
# Windows packages are flat (Modularity.exe at the archive root, the layout
# ModuHub and the V6.x zips use); Unix keeps the bin/ + lib/ layout its rpath
# expects.
if(WIN32)
    set(MODULARITY_INSTALL_APPDIR ".")
else()
    set(MODULARITY_INSTALL_APPDIR "${CMAKE_INSTALL_BINDIR}")
endif()

set(MODULARITY_MODUMAKO_COMPILER "" CACHE FILEPATH
    "Path to the mko executable bundled with the Modularity editor")
if(NOT MODULARITY_MODUMAKO_COMPILER)
    if(WIN32)
        set(_modumako_name "mko.exe")
    else()
        set(_modumako_name "mko")
    endif()
    set(_modumako_sibling "${CMAKE_SOURCE_DIR}/../MAKO/bin/${_modumako_name}")
    if(EXISTS "${_modumako_sibling}")
        set(MODULARITY_MODUMAKO_COMPILER "${_modumako_sibling}" CACHE FILEPATH
            "Path to the mko executable bundled with the Modularity editor" FORCE)
    endif()
endif()

if(MODULARITY_BUILD_EDITOR AND EXISTS "${MODULARITY_MODUMAKO_COMPILER}")
    add_custom_command(TARGET Modularity POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Modularity>/Tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MODULARITY_MODUMAKO_COMPILER}"
            "$<TARGET_FILE_DIR:Modularity>/Tools/"
        COMMENT "Staging bundled ModuMAKO compiler"
    )
    install(PROGRAMS "${MODULARITY_MODUMAKO_COMPILER}"
        DESTINATION "${MODULARITY_INSTALL_APPDIR}/Tools"
    )
elseif(MODULARITY_BUILD_EDITOR)
    message(STATUS "ModuMAKO compiler not bundled; set MODULARITY_MODUMAKO_COMPILER to package .mko support")
endif()

# ==================== Nebula bake bindings (optional, Rust) ====================
# NebulaBindings/ is a small C-ABI cdylib wrapping the Nebula baking framework.
# The editor dlopen()s it at runtime rather than linking it, so this whole block
# is optional: without a Rust toolchain the editor still builds and the
# Lightmapping window simply reports the backend as unavailable.
option(MODULARITY_BUILD_NEBULA_BINDINGS
    "Build the Nebula baking bindings for lightmap baking (requires cargo)" ON)

if(MODULARITY_BUILD_NEBULA_BINDINGS AND MODULARITY_BUILD_EDITOR)
    find_program(CARGO_EXECUTABLE cargo)
    set(MODULARITY_NEBULA_MANIFEST "${CMAKE_SOURCE_DIR}/NebulaBindings/Cargo.toml")

    if(NOT EXISTS "${MODULARITY_NEBULA_MANIFEST}")
        message(STATUS "Nebula bindings source not found at ${MODULARITY_NEBULA_MANIFEST}; lightmap baking will be unavailable at runtime")
    elseif(NOT CARGO_EXECUTABLE)
        message(STATUS "Nebula bindings skipped: cargo was not found on PATH. "
                       "Install Rust and re-run CMake to enable lightmap baking, "
                       "or build it manually with: "
                       "cargo build --release --manifest-path NebulaBindings/Cargo.toml")
    else()
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(_nebula_cargo_profile_flag "")
            set(_nebula_cargo_profile_dir "debug")
        else()
            set(_nebula_cargo_profile_flag "--release")
            set(_nebula_cargo_profile_dir "release")
        endif()

        set(_nebula_target_dir "${CMAKE_BINARY_DIR}/nebula-bindings")
        set(_nebula_lib_name "${CMAKE_SHARED_LIBRARY_PREFIX}nebula_modularity${CMAKE_SHARED_LIBRARY_SUFFIX}")
        set(_nebula_lib "${_nebula_target_dir}/${_nebula_cargo_profile_dir}/${_nebula_lib_name}")

        # Always re-run cargo; it is itself incremental and far better at
        # deciding what changed than a file-glob dependency list would be.
        add_custom_target(NebulaBindings ALL
            COMMAND ${CARGO_EXECUTABLE} build ${_nebula_cargo_profile_flag}
                    --manifest-path "${MODULARITY_NEBULA_MANIFEST}"
                    --target-dir "${_nebula_target_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_nebula_lib}" "$<TARGET_FILE_DIR:Modularity>/"
            BYPRODUCTS "${_nebula_lib}"
            COMMENT "Building Nebula bake bindings (cargo ${_nebula_cargo_profile_dir})"
            VERBATIM
        )
        add_dependencies(Modularity NebulaBindings)

        install(FILES "${_nebula_lib}"
            DESTINATION ${MODULARITY_INSTALL_APPDIR}
            OPTIONAL
        )
        message(STATUS "Nebula bindings enabled: ${CARGO_EXECUTABLE} (${_nebula_cargo_profile_dir})")
    endif()
endif()

set(MODULARITY_SCRIPT_SDK_INSTALL_SCRIPT ${CMAKE_BINARY_DIR}/InstallScriptSDK.cmake)
configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/InstallScriptSDK.cmake.in
    ${MODULARITY_SCRIPT_SDK_INSTALL_SCRIPT}
    @ONLY
)

if(MODULARITY_BUILD_EDITOR)
    install(TARGETS Modularity
        RUNTIME DESTINATION ${MODULARITY_INSTALL_APPDIR}
    )
endif()

# Desktop install: ModularityPlayer is an executable; on Android core_player
# is the .so and gets packaged separately by the APK pipeline (Stage 4).
# On Windows it sits beside the editor so it shares the FFmpeg/PhysX/OpenXR DLLs
# and can actually start; alone in "Runtime Manager" it had none of them.
if(WIN32)
    set(MODULARITY_INSTALL_PLAYERDIR "${MODULARITY_INSTALL_APPDIR}")
else()
    set(MODULARITY_INSTALL_PLAYERDIR "Runtime Manager")
endif()
if(NOT ANDROID)
    install(TARGETS ModularityPlayer
        RUNTIME DESTINATION "${MODULARITY_INSTALL_PLAYERDIR}"
    )
    install(SCRIPT ${MODULARITY_SCRIPT_SDK_INSTALL_SCRIPT})
endif()

if(MODULARITY_HAS_DISCORD_SOCIAL_SDK)
    if(WIN32)
        install(FILES "${MODULARITY_DISCORD_SOCIAL_RUNTIME}"
            DESTINATION "${MODULARITY_INSTALL_PLAYERDIR}")
    else()
        install(FILES "${MODULARITY_DISCORD_SOCIAL_RUNTIME}"
            DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
    # Some manually vendored SDK layouts omit the separate license-notice file.
    # It must not make an otherwise valid optional build fail at install time.
    if(EXISTS "${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}/License-Notices.txt")
        install(FILES "${MODULARITY_DISCORD_SOCIAL_SDK_ROOT}/License-Notices.txt"
            DESTINATION "licenses/DiscordSocialSdk")
    else()
        message(WARNING "Discord Social SDK license notices file not found. Review the SDK's redistribution requirements before packaging.")
    endif()
endif()

if(NOT WIN32 AND MODULARITY_ENABLE_PHYSX)
    file(GLOB PHYSX_SO_FILES
        "${CMAKE_BINARY_DIR}/physx/bin/linux.x86_64/release/*.so"
    )

    install(FILES ${PHYSX_SO_FILES}
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

if(WIN32)
    # Only install the FFmpeg DLLs the engine actually links against
    # (avformat, avcodec, avutil, swresample, swscale). The build-time copy
    # script in CopyRuntimeDlls.cmake also pulls everything in the FFmpeg
    # bin/ dir for safety while iterating, but for distribution we ship only
    # the linked set — avoids dragging in avfilter (~109 MB) and avdevice
    # (~6 MB), which the engine doesn't reference.
    if(MODULARITY_FFMPEG_RUNTIME_DLLS)
        install(FILES ${MODULARITY_FFMPEG_RUNTIME_DLLS}
            DESTINATION ${MODULARITY_INSTALL_APPDIR}
        )
    endif()

    if(MODULARITY_USE_MONO AND MONO_RUNTIME_DLL)
        install(FILES "${MONO_RUNTIME_DLL}"
            DESTINATION ${MODULARITY_INSTALL_APPDIR}
        )
    endif()

    # redist/<arch> DLLs (the OpenXR loader, optionally Mesa / toolchain
    # runtimes) were copied next to dev executables but never installed, so the
    # packaged editor reported "No OpenXR loader found" on working VR machines.
    if(MODULARITY_BUNDLE_REDIST_DLLS AND MODULARITY_REDIST_DLL_DIR AND IS_DIRECTORY "${MODULARITY_REDIST_DLL_DIR}")
        file(GLOB _modularity_redist_install_dlls "${MODULARITY_REDIST_DLL_DIR}/*.dll")
        if(_modularity_redist_install_dlls)
            install(FILES ${_modularity_redist_install_dlls}
                DESTINATION ${MODULARITY_INSTALL_APPDIR}
            )
        endif()
    endif()

    if(MODULARITY_ENABLE_PHYSX)
        install(CODE "
            file(GLOB _modularity_physx_dlls
                \"${PX_OUTPUT_BIN_DIR}/*/*/*.dll\"
                \"${PX_OUTPUT_BIN_DIR}/*.dll\"
                \"${CMAKE_BINARY_DIR}/physx/sdk_source_bin/*.dll\"
            )
            foreach(_physx_dll IN LISTS _modularity_physx_dlls)
                file(INSTALL \"\${_physx_dll}\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${MODULARITY_INSTALL_APPDIR}\")
            endforeach()
        ")
    endif()

    # PhysX (and a few other vendored CMake subprojects) install their own
    # static .lib / .pdb / .exp files alongside the runtime DLLs. The runtime
    # player does not need any of them — they exist only for SDK consumers
    # linking against PhysX. Prune them after all install rules have run so
    # the CPack-produced ZIP stays close to the actual runtime payload.
    install(CODE "
        file(GLOB_RECURSE _modularity_runtime_junk
            \"\${CMAKE_INSTALL_PREFIX}/${MODULARITY_INSTALL_APPDIR}/*.lib\"
            \"\${CMAKE_INSTALL_PREFIX}/${MODULARITY_INSTALL_APPDIR}/*.pdb\"
            \"\${CMAKE_INSTALL_PREFIX}/${MODULARITY_INSTALL_APPDIR}/*.exp\"
        )
        foreach(_f IN LISTS _modularity_runtime_junk)
            message(STATUS \"Pruning non-runtime artifact from package: \${_f}\")
            file(REMOVE \"\${_f}\")
        endforeach()

        # PhysX's own install rules add an SDK layout the runtime never reads:
        # include/ (its headers) and bin/win.x86_64.*/release/ (second copies of
        # the DLLs already installed beside the executables above). Our files are
        # installed flat at the root on Windows, so both folders are PhysX-only.
        foreach(_physx_sdk_dir IN ITEMS include bin)
            if(IS_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/\${_physx_sdk_dir}\")
                message(STATUS \"Pruning PhysX SDK folder from package: \${_physx_sdk_dir}/\")
                file(REMOVE_RECURSE \"\${CMAKE_INSTALL_PREFIX}/\${_physx_sdk_dir}\")
            endif()
        endforeach()
    ")
endif()

install(DIRECTORY ${CMAKE_SOURCE_DIR}/Resources
    DESTINATION ${MODULARITY_INSTALL_APPDIR}
)

install(DIRECTORY ${MODULARITY_TEMPLATE_PROJECTS_DIR}
    DESTINATION ${MODULARITY_INSTALL_APPDIR}
)

# ==================== Linux desktop integration ====================
# Linux ELF binaries don't embed icons. Install the logo to share/pixmaps and
# generate .desktop entries that reference it, so file managers and taskbars
# can display the Modularity logo for the executables.
if(NOT WIN32 AND NOT APPLE AND NOT ANDROID)
    install(FILES ${CMAKE_SOURCE_DIR}/Resources/Engine-Root/Modu-Logo.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pixmaps
        RENAME modularity.png
    )

    set(_modu_desktop_dir ${CMAKE_BINARY_DIR}/linux-desktop)
    file(MAKE_DIRECTORY ${_modu_desktop_dir})

    if(MODULARITY_BUILD_EDITOR)
        file(WRITE ${_modu_desktop_dir}/Modularity.desktop
"[Desktop Entry]
Type=Application
Name=Modularity
GenericName=Game Engine
Comment=Modularity Game Engine Editor
Exec=Modularity %F
Icon=modularity
Terminal=false
Categories=Development;IDE;
StartupWMClass=Modularity
")
        install(FILES ${_modu_desktop_dir}/Modularity.desktop
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
        )
    endif()
endif()

# ==================== Packaging (CPack) ====================
# CPack produces a 7z archive of the desktop install. Android packaging is
# handled by the dedicated APK pipeline (Stage 4 in docs/AndroidRuntime.md),
# so skip CPack entirely on Android.
if(NOT ANDROID)

# The release label in the archive name: "Modularity V7.0 - Windows.zip", whose
# top folder carries the same name. build.ps1 --version=V7.1 overrides it, and
# moves the finished archive from the binary dir up next to Dev-ExportedBuild.
set(MODULARITY_RELEASE_VERSION "V7.0" CACHE STRING
    "Release label used in the packaged archive name (e.g. V7.0)")

string(REGEX REPLACE "^[Vv]" "" _modularity_release_number "${MODULARITY_RELEASE_VERSION}")
set(CPACK_PACKAGE_NAME "Modularity")
set(CPACK_PACKAGE_VENDOR "Modularity")
set(CPACK_PACKAGE_VERSION "${_modularity_release_number}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Modularity Game Engine")
set(CPACK_PACKAGE_CONTACT "support@moduengine.xyz")

set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME} ${MODULARITY_RELEASE_VERSION} - ${CMAKE_SYSTEM_NAME}"
)

set(CPACK_GENERATOR "7Z")
# Use all available cores for the LZMA pass — packaging is otherwise
# single-threaded and dominates total CPack wall time on big trees.
set(CPACK_THREADS 0)

set(CPACK_INSTALL_CMAKE_PROJECTS
    "${CMAKE_BINARY_DIR};${CMAKE_PROJECT_NAME};ALL;/"
)

include(CPack)

endif() # NOT ANDROID
