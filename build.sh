#!/usr/bin/env bash
set -euo pipefail

start_time="$(date +%s)"

script_home="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

is_modularity_root() {
    local candidate="$1"
    [[ -f "${candidate}/CMakeLists.txt" ]] &&
    [[ -d "${candidate}/src" ]] &&
    [[ -d "${candidate}/Resources" ]] &&
    grep -q 'project(Modularity' "${candidate}/CMakeLists.txt"
}

find_modularity_root_from() {
    local start="$1"
    local current
    current="$(cd -- "${start}" 2>/dev/null && pwd)" || return 1

    while [[ -n "${current}" ]]; do
        if is_modularity_root "${current}"; then
            printf "%s\n" "${current}"
            return 0
        fi
        if is_modularity_root "${current}/Modularity"; then
            printf "%s\n" "${current}/Modularity"
            return 0
        fi

        local parent
        parent="$(dirname -- "${current}")"
        if [[ "${parent}" == "${current}" ]]; then
            break
        fi
        current="${parent}"
    done

    return 1
}

find_modularity_root() {
    if [[ -n "${MODULARITY_ROOT:-}" ]] && is_modularity_root "${MODULARITY_ROOT}"; then
        cd -- "${MODULARITY_ROOT}" && pwd
        return 0
    fi

    find_modularity_root_from "${script_home}" ||
    find_modularity_root_from "${PWD}"
}

script_dir="$(find_modularity_root)" || {
    printf "Could not find the Modularity source root. Set MODULARITY_ROOT or run from inside/above the Modularity folder.\n" >&2
    exit 1
}

build_platform="native"
build_platform_label="Linux"
build_dir="${script_dir}/build"
player_cache_dir="${build_dir}/player-cache"
windows_toolchain_file=""
cmake_platform_args=()
last_step="bootstrap"
current_step=0
total_steps=0
build_started=0
status_line_active=0
status_current_label=""
status_current_spinner="-"
status_current_elapsed=0

declare -a build_warning_messages=()
declare -a build_error_messages=()

if [[ -t 1 ]]; then
    C_RESET=$'\033[0m'
    C_BOLD=$'\033[1m'
    C_DIM=$'\033[2m'
    C_GREEN=$'\033[32m'
    C_YELLOW=$'\033[33m'
    C_RED=$'\033[31m'
    C_CYAN=$'\033[36m'
    ICON_INFO="ℹ"
    ICON_WARN="⚠"
    ICON_ERROR="✖"
    ICON_OK="✔"
else
    C_RESET=''
    C_BOLD=''
    C_DIM=''
    C_GREEN=''
    C_YELLOW=''
    C_RED=''
    C_CYAN=''
    ICON_INFO="[i]"
    ICON_WARN="[!]"
    ICON_ERROR="[x]"
    ICON_OK="[ok]"
fi

log_info() { printf "%s%s%s %s\n" "${C_CYAN}" "${ICON_INFO}" "${C_RESET}" "$*"; }
log_warn() { printf "%s%s%s %s\n" "${C_YELLOW}" "${ICON_WARN}" "${C_RESET}" "$*"; record_warning "$*"; }
log_error() { printf "%s%s%s %s\n" "${C_RED}" "${ICON_ERROR}" "${C_RESET}" "$*"; record_error "$*"; }
log_ok() { printf "%s%s%s %s\n" "${C_GREEN}" "${ICON_OK}" "${C_RESET}" "$*"; }

record_warning() {
    build_warning_messages+=("$1")
}

record_error() {
    build_error_messages+=("$1")
}

repeat_char() {
    local char="$1"
    local count="$2"
    if (( count <= 0 )); then
        return
    fi
    local result=""
    local i
    for ((i = 0; i < count; i++)); do
        result+="${char}"
    done
    printf "%s" "${result}"
}

if [[ -t 1 ]]; then
    BAR_FILL_CHAR="█"
    BAR_EMPTY_CHAR="░"
    SEP="│"
else
    BAR_FILL_CHAR="#"
    BAR_EMPTY_CHAR="-"
    SEP="|"
fi

progress_prefix() {
    local icon="${1:-${ICON_INFO}}"
    local icon_color="${2:-${C_CYAN}}"
    local width=24
    local filled=0
    local empty="${width}"
    local percent=0
    if (( total_steps > 0 )); then
        filled=$((current_step * width / total_steps))
        empty=$((width - filled))
        percent=$((current_step * 100 / total_steps))
    fi
    local bar_fill bar_empty
    bar_fill="$(repeat_char "${BAR_FILL_CHAR}" "${filled}")"
    bar_empty="$(repeat_char "${BAR_EMPTY_CHAR}" "${empty}")"

    printf "%s[%s]%s %s%s%s %s%2d/%2d%s %s%s%s [%s%s%s%s%s] %s%3d%%%s %s%s%s" \
        "${icon_color}" "${icon}" "${C_RESET}" \
        "${C_DIM}" "${SEP}" "${C_RESET}" \
        "${C_BOLD}" "${current_step}" "${total_steps}" "${C_RESET}" \
        "${C_DIM}" "${SEP}" "${C_RESET}" \
        "${C_GREEN}" "${bar_fill}" "${C_DIM}" "${bar_empty}" "${C_RESET}" \
        "${C_BOLD}" "${percent}" "${C_RESET}" \
        "${C_DIM}" "${SEP}" "${C_RESET}"
}

clear_status_line() {
    if [[ -t 1 && "${status_line_active}" -eq 1 ]]; then
        printf "\r\033[2K"
        status_line_active=0
    fi
}

render_status_line() {
    local label="$1"
    local spinner="$2"
    local elapsed="$3"

    if [[ ! -t 1 ]]; then
        return
    fi

    status_current_label="${label}"
    status_current_spinner="${spinner}"
    status_current_elapsed="${elapsed}"

    printf "\r\033[2K%s %s %s%s%s %s(%ss)%s" \
        "$(progress_prefix "${spinner}" "${C_CYAN}")" \
        "${label}" \
        "${C_DIM}" "${SEP}" "${C_RESET}" \
        "${C_DIM}" "${elapsed}" "${C_RESET}"

    status_line_active=1
}

emit_scrolling_event() {
    local level="$1"
    local message="$2"
    local had_status="${status_line_active}"

    clear_status_line
    case "${level}" in
        info)
            printf "%s%s%s %s\n" "${C_CYAN}" "${ICON_INFO}" "${C_RESET}" "${message}"
            ;;
        warn)
            printf "%s%s%s %s\n" "${C_YELLOW}" "${ICON_WARN}" "${C_RESET}" "${message}"
            ;;
        error)
            printf "%s%s%s %s\n" "${C_RED}" "${ICON_ERROR}" "${C_RESET}" "${message}"
            ;;
        ok)
            printf "%s%s%s %s\n" "${C_GREEN}" "${ICON_OK}" "${C_RESET}" "${message}"
            ;;
        *)
            printf "%s\n" "${message}"
            ;;
    esac

    if [[ -t 1 && "${had_status}" -eq 1 ]]; then
        render_status_line "${status_current_label}" "${status_current_spinner}" "${status_current_elapsed}"
    fi
}

emit_step_completion() {
    local message="$1"
    local had_status="${status_line_active}"

    clear_status_line
    printf "%s\n" "${message}"

    if [[ -t 1 && "${had_status}" -eq 1 ]]; then
        render_status_line "${status_current_label}" "${status_current_spinner}" "${status_current_elapsed}"
    fi
}

is_interesting_info_line() {
    local lower="$1"
    [[ "${lower}" == *"built target"* ]] ||
    [[ "${lower}" == *"configuring done"* ]] ||
    [[ "${lower}" == *"generating done"* ]] ||
    [[ "${lower}" == *"build files have been written"* ]] ||
    [[ "${lower}" == *"installing:"* ]] ||
    [[ "${lower}" == *"up-to-date:"* ]] ||
    [[ "${lower}" == *"linking cxx executable"* ]] ||
    [[ "${lower}" == *"linking cxx static library"* ]] ||
    [[ "${lower}" == *"copying"* ]]
}

process_build_output_line() {
    local step="$1"
    local line="$2"

    line="${line%$'\r'}"
    if [[ -z "${line//[[:space:]]/}" ]]; then
        return
    fi

    local lower="${line,,}"
    local issue_text="[${step}] ${line}"

    if [[ "${lower}" == *"0 errors generated"* ]]; then
        return
    fi

    if [[ "${lower}" == *"warning:"* ]] || [[ "${lower}" == *" cmake warning"* ]] || [[ "${lower}" == *"warning "* ]]; then
        record_warning "${issue_text}"
        emit_scrolling_event "warn" "${issue_text}"
        return
    fi

    if [[ "${lower}" == *"error:"* ]] || [[ "${lower}" == *"fatal"* ]] || [[ "${lower}" == *"undefined reference"* ]] || [[ "${lower}" == *"collect2: error"* ]] || [[ "${lower}" == *"ld: cannot"* ]]; then
        record_error "${issue_text}"
        emit_scrolling_event "error" "${issue_text}"
        return
    fi

    if is_interesting_info_line "${lower}"; then
        emit_scrolling_event "info" "[${step}] ${line}"
    fi
}

print_issue_summary() {
    local max_items=8
    local i

    if [[ "${#build_warning_messages[@]}" -gt 0 ]]; then
        printf "\n%sWarnings (%d):%s\n" "${C_YELLOW}" "${#build_warning_messages[@]}" "${C_RESET}"
        for ((i = 0; i < ${#build_warning_messages[@]} && i < max_items; i++)); do
            printf "  %s %s\n" "${ICON_WARN}" "${build_warning_messages[$i]}"
        done
        if [[ "${#build_warning_messages[@]}" -gt "${max_items}" ]]; then
            printf "  %s ... and %d more warning(s)\n" "${ICON_WARN}" "$(( ${#build_warning_messages[@]} - max_items ))"
        fi
    fi

    if [[ "${#build_error_messages[@]}" -gt 0 ]]; then
        printf "\n%sErrors (%d):%s\n" "${C_RED}" "${#build_error_messages[@]}" "${C_RESET}"
        for ((i = 0; i < ${#build_error_messages[@]} && i < max_items; i++)); do
            printf "  %s %s\n" "${ICON_ERROR}" "${build_error_messages[$i]}"
        done
        if [[ "${#build_error_messages[@]}" -gt "${max_items}" ]]; then
            printf "  %s ... and %d more error(s)\n" "${ICON_ERROR}" "$(( ${#build_error_messages[@]} - max_items ))"
        fi
    fi
}

advance_step() {
    local label="$1"
    current_step=$((current_step + 1))
    last_step="${label}"
}

run_step() {
    local label="$1"
    shift

    advance_step "${label}"
    clear_status_line
    printf "\n%s %s\n" "$(progress_prefix "${ICON_INFO}" "${C_CYAN}")" "${label}"

    "$@"
}

run_long_step() {
    local label="$1"
    shift

    advance_step "${label}"

    if [[ ! -t 1 ]]; then
        printf "\n%s %s\n" "$(progress_prefix "${ICON_INFO}" "${C_CYAN}")" "${label}"
        "$@"
        return
    fi

    local log_file
    log_file="$(mktemp "/tmp/modularity-build-step-${current_step}.XXXX.log")"
    local start_step=${SECONDS}
    local spinner_frames='-\|/'
    local spinner_index=0

    set +e
    "$@" >"${log_file}" 2>&1 &
    local cmd_pid=$!
    render_status_line "${label}" "-" 0

    while kill -0 "${cmd_pid}" >/dev/null 2>&1; do
        local elapsed=$((SECONDS - start_step))
        local frame="${spinner_frames:spinner_index:1}"
        render_status_line "${label}" "${frame}" "${elapsed}"
        spinner_index=$(((spinner_index + 1) % 4))
        sleep 0.2
    done

    wait "${cmd_pid}"
    local exit_code=$?
    set -e

    while IFS= read -r line; do
        process_build_output_line "${label}" "${line}"
    done < "${log_file}"

    local elapsed=$((SECONDS - start_step))

    if [[ "${exit_code}" -ne 0 ]]; then
        clear_status_line
        record_error "[${label}] Step failed with exit code ${exit_code}. Log: ${log_file}"
        emit_step_completion "$(progress_prefix "${ICON_ERROR}" "${C_RED}") ${label} ${C_DIM}${SEP}${C_RESET} ${C_RED}failed${C_RESET} ${C_DIM}(${elapsed}s)${C_RESET}"
        log_error "Detailed log kept at: ${log_file}"
        return "${exit_code}"
    fi

    clear_status_line
    emit_step_completion "$(progress_prefix "${ICON_OK}" "${C_GREEN}") ${label} ${C_DIM}${SEP}${C_RESET} ${C_DIM}(${elapsed}s)${C_RESET}"
    rm -f "${log_file}"
}

finish() {
    local exit_code="$1"
    set +e

    if [[ "${build_started}" -eq 0 ]]; then
        return
    fi
    clear_status_line

    local end_time
    end_time="$(date +%s)"
    local duration=$((end_time - start_time))
    echo

    if [[ "${exit_code}" -eq 0 ]]; then
        printf "%s%s================================%s\n" "${C_RESET}" "${C_BOLD}" "${C_RESET}"
        printf "%s   Modularity - Native Linux Build Complete%s\n" "${C_BOLD}" "${C_RESET}"
        printf "%s================================%s\n" "${C_BOLD}" "${C_RESET}"
        log_ok "Build completed in ${duration}s."
        log_info "Artifacts: ${build_dir}"
        print_issue_summary
    else
        printf "%s%s================================%s\n" "${C_RESET}" "${C_BOLD}" "${C_RESET}"
        printf "%s   Modularity - Native Linux Build Failed%s\n" "${C_BOLD}" "${C_RESET}"
        printf "%s================================%s\n" "${C_BOLD}" "${C_RESET}"
        log_error "Build failed after ${duration}s at step: ${last_step} (exit code ${exit_code})."
        print_issue_summary
    fi
}
trap 'finish $?' EXIT

usage() {
    cat <<'EOF'
Usage: ./build.sh [options]
Options:
  --clean                 Remove existing build directories first
  --Windows               Cross-build a Windows target with MinGW-w64
  --Android               Build an installable Android .apk (arm64-v8a)
  --abi=<abi>             Android ABI to target (default: arm64-v8a)
  --project=<path.modu>   Bundle a project into the Android APK (else a bare player)
  --output=<path.apk>     Where to write the Android APK (-o also works)
  --debug                 Android: build a debuggable APK (Debug config)
  --editor                Android: build the Modularity EDITOR APK (experimental)
  --build-type=<type>     CMake build type (default: Release)
  --cpu-target=<level>    x86-64 ISA target: baseline (default, ships to everyone),
                          x86-64-v2, x86-64-v3, x86-64-v4, or native
  --native                Shorthand for --cpu-target=native (DEV ONLY; do not ship)
  --generator=<name>      Force CMake generator (e.g. Ninja, "Unix Makefiles")
  --jobs=<N>              Parallel compile jobs (default: nproc - 2, min 1)
  --skip-deps             Skip automatic dependency checks/install
  --zip                   Package as .zip (default; fast to compress)
  --7z                    Package as .7z (smaller, but much slower to compress)
  --fsanitize             Build with AddressSanitizer + UBSan (-DMODULARITY_ENABLE_ASAN=ON)
  --help                  Show this help message
EOF
}

clean_build=0
build_type="Release"
build_cpp_standard="c++23"
# Default to baseline x86-64 so public Linux releases run on any 64-bit x86 CPU.
# Higher levels (or --native) are developer-only and must not be shipped.
cpu_target="baseline"
skip_deps=0
package_format="ZIP"
enable_asan=0
ncpus="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)"
jobs=$(( ncpus - 2 ))
if (( jobs < 1 )); then
    jobs=1
fi
jobs_overridden=0
preferred_generator=""
cmake_generator=""
# Android APK build options (used when --Android is passed).
android_abi="arm64-v8a"
android_project=""
android_output=""
android_debug=0
android_editor=0

for arg in "$@"; do
    case "$arg" in
        --clean)
            clean_build=1
            ;;
        --Windows|--windows|--platform=Windows|--platform=windows)
            build_platform="windows"
            ;;
        --Android|--android|--platform=Android|--platform=android)
            build_platform="android"
            ;;
        --abi=*)
            android_abi="${arg#*=}"
            ;;
        --project=*)
            android_project="${arg#*=}"
            ;;
        --output=*|-o=*)
            android_output="${arg#*=}"
            ;;
        --debug)
            android_debug=1
            ;;
        --editor)
            android_editor=1
            ;;
        --platform=native|--platform=Linux|--platform=linux)
            build_platform="native"
            ;;
        --build-type=*)
            build_type="${arg#*=}"
            ;;
        --cpu-target=*)
            cpu_target="${arg#*=}"
            ;;
        --native)
            # Developer convenience: tune for the building machine. NEVER use
            # this for a public release — the binary will require the host CPU's
            # ISA and crash elsewhere with "CPU ISA level is lower than required".
            cpu_target="native"
            ;;
        --generator=*)
            preferred_generator="${arg#*=}"
            ;;
        --jobs=*)
            jobs_value="${arg#*=}"
            if ! [[ "${jobs_value}" =~ ^[0-9]+$ ]] || (( jobs_value < 1 )); then
                log_error "--jobs requires a positive integer (got: ${jobs_value})"
                exit 1
            fi
            jobs="${jobs_value}"
            jobs_overridden=1
            ;;
        --skip-deps)
            skip_deps=1
            ;;
        --zip|--ZIP)
            package_format="ZIP"
            ;;
        --7z|--7Z)
            package_format="7Z"
            ;;
        --fsanitize|--asan|--sanitize=address)
            enable_asan=1
            ;;
        --help)
            usage
            exit 0
            ;;
        *)
            log_warn "Unknown argument: ${arg}"
            usage
            exit 1
        ;;
    esac
done

if [[ "${build_platform}" == "android" ]]; then
    # The Android APK is produced by the native editor (it cross-builds the
    # player .so via the NDK and packages the APK itself). So this is a normal
    # native editor build plus an --build-android post-step; no cross toolchain
    # here. Keep the default build_dir so the editor build stays incremental.
    build_platform_label="Android APK build"
fi

if [[ "${build_platform}" == "windows" ]]; then
    build_platform_label="Windows cross-build"
    build_dir="${script_dir}/build/windows"
    player_cache_dir="${build_dir}/player-cache"
    cmake_platform_args=(-DMODULARITY_USE_MONO=OFF -DMODULARITY_ENABLE_PHYSX=OFF -DMODULARITY_ENABLE_VULKAN=OFF -DMODULARITY_ENABLE_SNDFILE=OFF -DMODULARITY_ENABLE_OPUSFILE=OFF)

    if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1 && \
       command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 && \
       command -v x86_64-w64-mingw32-windres >/dev/null 2>&1; then
        windows_toolchain_file="${script_dir}/src/ThirdParty/glfw/CMake/x86_64-w64-mingw32.cmake"
    elif command -v x86_64-w64-mingw32-clang++ >/dev/null 2>&1 && \
         command -v x86_64-w64-mingw32-clang >/dev/null 2>&1 && \
         command -v x86_64-w64-mingw32-windres >/dev/null 2>&1; then
        windows_toolchain_file="${script_dir}/src/ThirdParty/glfw/CMake/x86_64-w64-mingw32-clang.cmake"
    else
        log_error "Windows cross-build requested, but no x86_64-w64-mingw32 toolchain was found."
        log_error "Install MinGW-w64 (gcc or clang variant) before using --Windows."
        exit 1
    fi

    cmake_platform_args+=(-DCMAKE_TOOLCHAIN_FILE="${windows_toolchain_file}")
fi

if [[ "${enable_asan}" -eq 1 ]]; then
    if [[ "${build_platform}" == "windows" ]]; then
        log_warn "--fsanitize ignored on Windows cross-build (ASan not supported with MinGW here)."
        enable_asan=0
    else
        cmake_platform_args+=(-DMODULARITY_ENABLE_ASAN=ON)
        build_dir="${build_dir}/asan"
        player_cache_dir="${build_dir}/player-cache"
    fi
fi

if [[ "${enable_asan}" -eq 0 ]]; then
    cmake_platform_args+=(-DMODULARITY_ENABLE_ASAN=OFF)
fi

pkg_manager=""
pkg_prefix=()
pkg_index_updated=0

detect_package_manager() {
    if command -v apt-get >/dev/null 2>&1; then
        pkg_manager="apt"
    elif command -v dnf >/dev/null 2>&1; then
        pkg_manager="dnf"
    elif command -v pacman >/dev/null 2>&1; then
        pkg_manager="pacman"
    elif command -v zypper >/dev/null 2>&1; then
        pkg_manager="zypper"
    else
        pkg_manager=""
    fi

    if [[ "$(id -u)" -eq 0 ]]; then
        pkg_prefix=()
    elif command -v sudo >/dev/null 2>&1; then
        pkg_prefix=(sudo)
    elif command -v doas >/dev/null 2>&1; then
        pkg_prefix=(doas)

    else
        pkg_prefix=()
    fi
}

detect_cmake_generator() {
    if [[ -n "${preferred_generator}" ]]; then
        cmake_generator="${preferred_generator}"
        return
    fi

    if [[ -n "${CMAKE_GENERATOR:-}" ]]; then
        cmake_generator="${CMAKE_GENERATOR}"
        return
    fi

    if [[ "${clean_build}" -eq 0 && -f "${build_dir}/CMakeCache.txt" ]]; then
        cmake_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "${build_dir}/CMakeCache.txt" | head -n1)"
        if [[ -n "${cmake_generator}" ]]; then
            return
        fi
    fi

    if command -v ninja >/dev/null 2>&1; then
        cmake_generator="Ninja"
        return
    fi

    cmake_generator=""
}

admin_cmd() {
    if [[ "${#pkg_prefix[@]}" -gt 0 ]]; then
        "${pkg_prefix[@]}" "$@"
    else
        "$@"
    fi
}

update_pkg_index_once() {
    if [[ "${pkg_index_updated}" -eq 1 ]]; then
        return
    fi
    case "${pkg_manager}" in
        apt)
            admin_cmd apt-get update
            ;;
        pacman)
            admin_cmd pacman -Sy --noconfirm
            ;;
        dnf|zypper)
            ;;
    esac
    pkg_index_updated=1
}

install_packages() {
    local -a packages=("$@")
    if [[ "${#packages[@]}" -eq 0 ]]; then
        return
    fi

    update_pkg_index_once
    case "${pkg_manager}" in
        apt)
            admin_cmd apt-get install -y "${packages[@]}"
            ;;
        dnf)
            admin_cmd dnf install -y "${packages[@]}"
            ;;
        pacman)
            local -a unsatisfied=()
            local dep
            while IFS= read -r dep; do
                [[ -n "${dep}" ]] && unsatisfied+=("${dep}")
            done < <(pacman -T "${packages[@]}" 2>/dev/null)
            if [[ "${#unsatisfied[@]}" -eq 0 ]]; then
                return 0
            fi
            admin_cmd pacman -S --noconfirm --needed "${unsatisfied[@]}"
            ;;
        zypper)
            admin_cmd zypper --non-interactive install --no-recommends "${packages[@]}"
            ;;
        *)
            return 1
            ;;
    esac
}

install_optional_first_hit() {
    local -a candidates=("$@")
    local candidate
    for candidate in "${candidates[@]}"; do
        if install_packages "${candidate}" >/dev/null 2>&1; then
            log_ok "Installed optional package: ${candidate}"
            return 0
        fi
    done
    return 1
}

ensure_linux_dependencies() {
    detect_package_manager

    if [[ -z "${pkg_manager}" ]]; then
        log_warn "No supported package manager detected (apt/dnf/pacman/zypper). Skipping auto-install."
        return
    fi

    if [[ "$(id -u)" -ne 0 && "${#pkg_prefix[@]}" -eq 0 ]]; then
        log_warn "Auto-install requires root or sudo. Skipping dependency installation."
        return
    fi

    local need_install=0
    local -a missing=()
    local cmd
    for cmd in git git-lfs cmake pkg-config c++; do
        if ! command -v "${cmd}" >/dev/null 2>&1; then
            need_install=1
            missing+=("${cmd}")
        fi
    done

    if command -v pkg-config >/dev/null 2>&1; then
        local module
        for module in x11 xrandr xi xinerama xcursor gl; do
            if ! pkg-config --exists "${module}"; then
                need_install=1
                missing+=("${module}-dev")
            fi
        done
        if ! pkg-config --exists vulkan; then
            need_install=1
            missing+=("vulkan-dev")
        fi
        for module in sndfile opusfile; do
            if ! pkg-config --exists "${module}"; then
                need_install=1
                missing+=("${module}")
            fi
        done
    fi

    if ! command -v glslc >/dev/null 2>&1; then
        need_install=1
        missing+=("glslc")
    fi
    if ! command -v vulkaninfo >/dev/null 2>&1; then
        need_install=1
        missing+=("vulkan-tools")
    fi

    local -a required_pkgs=()
    local -a core_pkgs=()
    local -a x11_pkgs=()
    local -a graphics_pkgs=()
    local -a vulkan_pkgs=()
    local -a optional_pkgs=()
    local -a glslc_candidates=()
    case "${pkg_manager}" in
        apt)
            core_pkgs=(build-essential cmake pkg-config git git-lfs zlib1g-dev)
            x11_pkgs=(libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev)
            graphics_pkgs=(libgl1-mesa-dev libegl1-mesa-dev libwayland-dev)
            vulkan_pkgs=(libvulkan-dev vulkan-tools glslang-tools)
            audio_pkgs=(libsndfile1-dev libopusfile-dev)
            optional_pkgs=(ccache)
            glslc_candidates=(glslc shaderc)
            ;;
        dnf)
            core_pkgs=(gcc gcc-c++ make cmake pkgconf-pkg-config git git-lfs zlib-devel)
            x11_pkgs=(libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel)
            graphics_pkgs=(mesa-libGL-devel mesa-libEGL-devel wayland-devel)
            vulkan_pkgs=(vulkan-loader-devel vulkan-tools glslang)
            audio_pkgs=(libsndfile-devel opusfile-devel)
            optional_pkgs=(ccache)
            glslc_candidates=(shaderc shaderc-devel)
            ;;
        pacman)
            core_pkgs=(base-devel cmake pkgconf git git-lfs zlib)
            x11_pkgs=(libx11 libxrandr libxinerama libxcursor libxi)
            graphics_pkgs=(mesa wayland)
            vulkan_pkgs=(vulkan-headers vulkan-tools glslang)
            audio_pkgs=(libsndfile opusfile)
            optional_pkgs=(ccache)
            glslc_candidates=(shaderc)
            ;;
        zypper)
            core_pkgs=(gcc gcc-c++ make cmake pkg-config git git-lfs zlib-devel)
            x11_pkgs=(libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel)
            graphics_pkgs=(Mesa-libGL-devel Mesa-libEGL-devel wayland-devel)
            vulkan_pkgs=(vulkan-devel vulkan-tools glslang)
            audio_pkgs=(libsndfile-devel libopusfile-devel)
            optional_pkgs=(ccache)
            glslc_candidates=(shaderc shaderc-devel)
            ;;
    esac
    required_pkgs=(
        "${core_pkgs[@]}"
        "${x11_pkgs[@]}"
        "${graphics_pkgs[@]}"
        "${vulkan_pkgs[@]}"
        "${audio_pkgs[@]}"
        "${optional_pkgs[@]}"
    )

    log_info "Dependency hierarchy (${pkg_manager}):"
    echo "  +-- Core toolchain"
    for pkg in "${core_pkgs[@]}"; do echo "  |   +-- ${pkg}"; done
    echo "  +-- X11/Windowing"
    for pkg in "${x11_pkgs[@]}"; do echo "  |   +-- ${pkg}"; done
    echo "  +-- OpenGL/EGL/Wayland"
    for pkg in "${graphics_pkgs[@]}"; do echo "  |   +-- ${pkg}"; done
    echo "  +-- Vulkan stack"
    for pkg in "${vulkan_pkgs[@]}"; do echo "  |   +-- ${pkg}"; done
    echo "  +-- Audio import (sndfile, opusfile)"
    for pkg in "${audio_pkgs[@]}"; do echo "  |   +-- ${pkg}"; done
    echo "  +-- Optional acceleration"
    for pkg in "${optional_pkgs[@]}"; do echo "      +-- ${pkg}"; done

    if [[ "${need_install}" -eq 0 ]]; then
        log_ok "Build dependencies already present."
        return
    fi

    log_info "Missing prerequisites detected: ${missing[*]}"
    log_info "Installing packages using ${pkg_manager}..."

    install_packages "${required_pkgs[@]}"

    if ! command -v glslc >/dev/null 2>&1; then
        install_optional_first_hit "${glslc_candidates[@]}" || log_warn "glslc package candidate not found; Vulkan runtime shader compile may fail."
    fi

    if ! command -v glslc >/dev/null 2>&1; then
        log_warn "glslc is still missing."
    fi
    if ! command -v vulkaninfo >/dev/null 2>&1; then
        log_warn "vulkaninfo is still missing."
    fi
}

# This repo used to ship committed hooks in .githooks/ wired up through
# core.hooksPath. Those are gone: git-lfs owns its hooks in .git/hooks again
# (sync_lfs_assets reinstalls them every build), and sync_submodule_branches
# below replaces the branch-sync hooks. Old clones still point core.hooksPath
# at the deleted folder, which makes git skip .git/hooks entirely, so pushes
# would silently skip LFS uploads. Unset it anywhere it points into .githooks.
retire_repo_githooks() {
    local hooks_path
    hooks_path="$(git -C "${script_dir}" config --local core.hooksPath 2>/dev/null || true)"
    if [[ "${hooks_path}" == *.githooks* ]]; then
        git -C "${script_dir}" config --local --unset core.hooksPath || true
    fi
    git -C "${script_dir}" submodule foreach --quiet '
        hooks_path="$(git config --local core.hooksPath 2>/dev/null || true)"
        case "${hooks_path}" in
            *.githooks*) git config --local --unset core.hooksPath || true ;;
        esac
    ' >/dev/null 2>&1 || true

    # The old hook installer also set these; they are what lets a superproject
    # push carry ModuGUI commits along, so keep setting them here.
    git -C "${script_dir}" config --local push.recurseSubmodules on-demand
    git -C "${script_dir}" config --local submodule.recurse true
}

# Keeps branched submodules (ModuGUI on docking) on their branch instead of a
# detached HEAD, carrying over any commits made while detached. This used to be
# a post-checkout/post-commit hook; running it here covers the same flow
# without anyone having to install hooks.
sync_submodule_branches() {
    local sync_script="${script_dir}/tools/git/sync_submodule_branch.sh"
    if [[ -x "${sync_script}" ]]; then
        "${sync_script}" || log_warn "Submodule branch sync failed; submodules may be on a detached HEAD."
    fi
}

sync_submodules() {
    retire_repo_githooks
    git -C "${script_dir}" submodule sync --recursive
    git -C "${script_dir}" submodule update --init --recursive
    sync_submodule_branches
    sync_lfs_assets
    verify_lfs_assets
}

sync_lfs_assets() {
    if ! command -v git-lfs >/dev/null 2>&1; then
        log_warn "git-lfs is not installed; cannot refresh large binary assets."
        return
    fi

    git -C "${script_dir}" lfs install --local >/dev/null
    git -C "${script_dir}" lfs pull
}

is_lfs_pointer_file() {
    local path="$1"
    [[ -f "${path}" ]] && head -n 1 "${path}" 2>/dev/null | grep -qx 'version https://git-lfs.github.com/spec/v1'
}

verify_lfs_assets() {
    local -a required_assets=(
        "Resources/Engine-Root/Modu-Logo.png"
        "Resources/Fonts/TheSunset.ttf"
        "Resources/Fonts/Thesunsethd-Regular (1).ttf"
    )
    local asset
    local -a missing=()
    local -a pointers=()

    for asset in "${required_assets[@]}"; do
        local full_path="${script_dir}/${asset}"
        if [[ ! -f "${full_path}" ]]; then
            missing+=("${asset}")
        elif is_lfs_pointer_file "${full_path}"; then
            pointers+=("${asset}")
        fi
    done

    if [[ "${#missing[@]}" -gt 0 || "${#pointers[@]}" -gt 0 ]]; then
        if [[ "${#missing[@]}" -gt 0 ]]; then
            log_error "Missing required LFS assets: ${missing[*]}"
        fi
        if [[ "${#pointers[@]}" -gt 0 ]]; then
            log_error "Git LFS assets are still pointer files: ${pointers[*]}"
        fi
        log_error "Install git-lfs, then run: git lfs install && git lfs pull"
        return 1
    fi
}

configure_ccache() {
    if command -v ccache >/dev/null 2>&1; then
        export CCACHE_BASEDIR="${script_dir}"
        export CCACHE_NOHASHDIR=1
        log_info "ccache detected. Normalizing paths for cross-build cache hits."
    fi
}

clean_editor_build() {
    if [[ -d "${build_dir}" ]]; then
        rm -rf "${build_dir}"
        log_ok "Removed ${build_dir}"
    fi
}

clean_player_cache() {
    if [[ -d "${player_cache_dir}" ]]; then
        rm -rf "${player_cache_dir}"
        log_ok "Removed ${player_cache_dir}"
    fi
}

configure_editor_build() {
    local -a generator_args=()
    if [[ ! -f "${build_dir}/CMakeCache.txt" && -n "${cmake_generator}" ]]; then
        generator_args=(-G "${cmake_generator}")
    fi
    cmake "${generator_args[@]}" -S "${script_dir}" -B "${build_dir}" \
        "${cmake_platform_args[@]}" \
        -DMONO_ROOT=/usr \
        -DMODULARITY_BUILD_CPP_STANDARD="${build_cpp_standard}" \
        -DMODULARITY_CPU_TARGET="${cpu_target}" \
        -DCMAKE_BUILD_TYPE="${build_type}"
}

build_editor_targets() {
    cmake --build "${build_dir}" --parallel "${jobs}"
}

install_editor_targets() {
    cmake --install "${build_dir}" --prefix "${build_dir}/install"
}

copy_third_party_libraries() {
    local target_dir="$1"

    mkdir -p "${target_dir}/Packages/ThirdParty"
    if [[ "${build_platform}" == "windows" ]]; then
        find "${target_dir}" -type f \( -name "*.a" -o -name "*.so" -o -name "*.dylib" -o -name "*.lib" -o -name "*.dll" -o -name "*.dll.a" \) \
            -not -path "${target_dir}/Packages/*" -exec cp -f {} "${target_dir}/Packages/ThirdParty/" \;
    else
        find "${target_dir}" -type f \( -name "*.a" -o -name "*.so" -o -name "*.dylib" -o -name "*.lib" \) \
            -not -path "${target_dir}/Packages/*" -exec cp -f {} "${target_dir}/Packages/ThirdParty/" \;
    fi
}

copy_engine_libraries() {
    local target_dir="$1"

    mkdir -p "${target_dir}/Packages/Engine"
    find "${target_dir}" -type f \( -name "libcore*" -o -name "core*.lib" -o -name "core*.dll" \) \
        -not -path "${target_dir}/Packages/*" -exec cp -f {} "${target_dir}/Packages/Engine/" \;
}

configure_player_build() {
    local -a generator_args=()
    if [[ ! -f "${player_cache_dir}/CMakeCache.txt" && -n "${cmake_generator}" ]]; then
        generator_args=(-G "${cmake_generator}")
    fi
    cmake "${generator_args[@]}" -S "${script_dir}" -B "${player_cache_dir}" \
        "${cmake_platform_args[@]}" \
        -DMONO_ROOT=/usr \
        -DMODULARITY_BUILD_CPP_STANDARD="${build_cpp_standard}" \
        -DMODULARITY_CPU_TARGET="${cpu_target}" \
        -DCMAKE_BUILD_TYPE="${build_type}" \
        -DMODULARITY_BUILD_EDITOR=OFF
}

build_player_target() {
    cmake --build "${player_cache_dir}" --target ModularityPlayer --parallel "${jobs}"
}

install_desktop_integration() {
    local xdg_data_home="${XDG_DATA_HOME:-${HOME}/.local/share}"
    local pixmaps_dir="${xdg_data_home}/pixmaps"
    local applications_dir="${xdg_data_home}/applications"
    local icon_src="${script_dir}/Resources/Engine-Root/Modu-Logo.png"
    local icon_dst="${pixmaps_dir}/modularity.png"

    if [[ ! -f "${icon_src}" ]]; then
        log_warn "Logo not found at ${icon_src}; skipping desktop integration."
        return
    fi

    mkdir -p "${pixmaps_dir}" "${applications_dir}"
    cp -f "${icon_src}" "${icon_dst}"
    log_ok "Installed icon: ${icon_dst}"

    local editor_bin="${build_dir}/install/bin/Modularity"
    if [[ -x "${editor_bin}" ]]; then
        local editor_desktop="${applications_dir}/Modularity.desktop"
        cat > "${editor_desktop}" <<EOF
[Desktop Entry]
Type=Application
Name=Modularity
GenericName=Game Engine
Comment=Modularity Game Engine Editor
Exec=${editor_bin} %F
Icon=${icon_dst}
Terminal=false
Categories=Development;IDE;
StartupWMClass=Modularity
EOF
        log_ok "Installed desktop entry: ${editor_desktop}"
    fi

    rm -f "${applications_dir}/ModularityPlayer.desktop"

    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "${applications_dir}" >/dev/null 2>&1 || true
    fi
}

finalize_packaging() {
    rm -rf "${build_dir}/Template-Projects"
    cp -r "${script_dir}/Resources" "${build_dir}/"
    if [[ -d "${script_dir}/Template-Projects" ]]; then
        cp -r "${script_dir}/Template-Projects" "${build_dir}/"
    else
        mkdir -p "${build_dir}/Template-Projects"
    fi
    if [[ -f "${build_dir}/Resources/imgui.ini" ]]; then
        cp "${build_dir}/Resources/imgui.ini" "${build_dir}/"
    fi
    ln -sfn "${build_dir}/compile_commands.json" "${script_dir}/compile_commands.json"
    (cd "${build_dir}" && cpack -G "${package_format}")
}

verify_release_isa() {
    # Gate the packaged binaries against the requested CPU target so a stray
    # AVX/AVX-512 dependency can never silently ship a release that crashes on
    # older CPUs with "CPU ISA level is lower than required".
    local script="${script_dir}/tools/verify-cpu-isa.sh"
    if [[ ! -x "${script}" ]]; then
        log_warn "verify-cpu-isa.sh not found or not executable; skipping ISA verification."
        return 0
    fi
    if [[ "${cpu_target}" == "native" ]]; then
        log_warn "CPU target is 'native' — skipping ISA verification (dev build, do not ship)."
        return 0
    fi

    # Scan the concrete release artifacts only (executables + the staged
    # Packages/ trees), not the whole build tree — that avoids tripping over
    # unrelated scratch/cross-compile output dirs.
    local -a scan=()
    [[ -f "${build_dir}/Modularity" ]] && scan+=("${build_dir}/Modularity")
    [[ -f "${build_dir}/ModularityPlayer" ]] && scan+=("${build_dir}/ModularityPlayer")
    [[ -d "${build_dir}/Packages" ]] && scan+=("${build_dir}/Packages")
    [[ -d "${player_cache_dir}/Packages" ]] && scan+=("${player_cache_dir}/Packages")
    if [[ -f "${player_cache_dir}/ModularityPlayer" ]]; then
        scan+=("${player_cache_dir}/ModularityPlayer")
    fi

    if [[ "${#scan[@]}" -eq 0 ]]; then
        log_warn "No packaged binaries found to verify; skipping ISA check."
        return 0
    fi
    "${script}" --max-level="${cpu_target}" "${scan[@]}"
}

# Stage the toolchain bundle the EDITOR APK ships so the device editor can compile
# scripts on-device. The deterministic parts (target sysroot + engine headers) come
# straight from the NDK and this repo; the device-runnable aarch64-Android clang/lld
# binaries are fetched from Termux packages when absent. On success this exports
# MODULARITY_ANDROID_CLANG_DIR, which the editor reads to pack the bundle into
# assets/. Best-effort: never fails the APK build - the editor just won't be able
# to compile until a clang is present.
stage_android_clang_toolchain() {
    local ndk="$1"
    local abi="$2"
    local bundle="${script_dir}/build/android-clang/${abi}"

    # 1) A complete user-supplied bundle takes priority and is used verbatim.
    if [[ -n "${MODULARITY_ANDROID_CLANG_DIR:-}" && -x "${MODULARITY_ANDROID_CLANG_DIR}/bin/clang" ]]; then
        log_ok "Using user-supplied Android clang bundle: ${MODULARITY_ANDROID_CLANG_DIR}"
        export MODULARITY_ANDROID_CLANG_DIR
        return 0
    fi
    if [[ -n "${MODULARITY_ANDROID_CLANG_DIR:-}" ]]; then
        log_warn "MODULARITY_ANDROID_CLANG_DIR is set but '${MODULARITY_ANDROID_CLANG_DIR}/bin/clang' is missing; falling back to ${bundle}."
    fi

    local host_prebuilt
    host_prebuilt="$(ls -d "${ndk}"/toolchains/llvm/prebuilt/*/ 2>/dev/null | head -1)"
    if [[ -z "${host_prebuilt}" ]]; then
        log_warn "NDK llvm prebuilt dir not found; cannot stage the on-device clang sysroot."
        return 1
    fi
    local ndk_sysroot="${host_prebuilt%/}/sysroot"

    mkdir -p "${bundle}/bin" "${bundle}/sysroot/usr/lib" "${bundle}/headers"

    # 2) Target sysroot: bionic headers + aarch64 libs/crt (deterministic, from NDK).
    if [[ ! -d "${bundle}/sysroot/usr/include" ]]; then
        log_info "Staging NDK target sysroot (headers + aarch64 libs) for on-device clang..."
        cp -a "${ndk_sysroot}/usr/include" "${bundle}/sysroot/usr/include" 2>/dev/null || true
        cp -a "${ndk_sysroot}/usr/lib/aarch64-linux-android" "${bundle}/sysroot/usr/lib/aarch64-linux-android" 2>/dev/null || true
    fi

    # 3) Engine script-API headers + glm so generated wrappers can #include them.
    rm -rf "${bundle}/headers/include"
    cp -a "${script_dir}/include" "${bundle}/headers/include" 2>/dev/null || true
    if [[ -d "${script_dir}/src/ThirdParty/glm/glm" ]]; then
        mkdir -p "${bundle}/headers/glm-root"
        rm -rf "${bundle}/headers/glm-root/glm"
        cp -a "${script_dir}/src/ThirdParty/glm/glm" "${bundle}/headers/glm-root/glm" 2>/dev/null || true
    fi

    # 4) The device clang/lld themselves. The NDK only has a host clang, so the
    #    default path stages the current Termux aarch64 packages into this bundle.
    #    Set MODULARITY_ANDROID_CLANG_AUTO_FETCH=0 to keep the old manual behavior,
    #    or set MODULARITY_ANDROID_CLANG_DIR above to use a prebuilt bundle.
    if [[ ! -x "${bundle}/bin/clang" && "${MODULARITY_ANDROID_CLANG_AUTO_FETCH:-1}" != "0" ]]; then
        local fetcher="${script_dir}/tools/stage-termux-clang.sh"
        if [[ -x "${fetcher}" ]]; then
            log_info "Fetching/staging Termux aarch64 clang for on-device compilation..."
            if ! "${fetcher}" --abi "${abi}" --bundle "${bundle}" \
                 --cache-dir "${script_dir}/build/android-clang/.termux-cache/${abi}"; then
                log_warn "Automatic Termux clang staging failed; continuing without on-device compile."
            fi
        else
            log_warn "Termux clang staging helper missing: ${fetcher}"
        fi
    fi

    if [[ ! -x "${bundle}/bin/clang" ]]; then
        log_warn "No device-runnable aarch64-Android clang at ${bundle}/bin/clang."
        log_warn "Set MODULARITY_ANDROID_CLANG_DIR to a complete bundle, or leave"
        log_warn "MODULARITY_ANDROID_CLANG_AUTO_FETCH enabled so build.sh can stage Termux clang."
        log_warn "The editor APK will build WITHOUT on-device compile."
        return 1
    fi

    export MODULARITY_ANDROID_CLANG_DIR="${bundle}"
    log_ok "On-device clang bundle ready: ${bundle}"
    return 0
}

run_android_build() {
    # Resolve the NDK + SDK the same way the editor's exporter does.
    local ndk="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}}"
    if [[ -z "${ndk}" || ! -f "${ndk}/source.properties" ]]; then
        log_error "Android NDK not found. Set ANDROID_NDK_ROOT / ANDROID_NDK_HOME / ANDROID_NDK to a valid NDK."
        exit 1
    fi
    local sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
    if [[ -z "${sdk}" ]]; then
        log_error "Android SDK not found. Set ANDROID_SDK_ROOT or ANDROID_HOME."
        exit 1
    fi
    log_ok "Android NDK: ${ndk}"
    log_ok "Android SDK: ${sdk}"
    log_info "Android ABI: ${android_abi}"
    if [[ -n "${android_project}" ]]; then
        log_info "Android project: ${android_project}"
    else
        log_info "Android project: (none — building a bare player APK)"
    fi

    # aapt2 link needs a platform android.jar. The system SDK (e.g. /opt/android-sdk)
    # is usually root-owned and not writable, so install into a local writable SDK
    # root that the editor also searches (sourceRoot/build/android-sdk).
    local local_sdk="${script_dir}/build/android-sdk"
    if ! ls "${sdk}"/platforms/android-*/android.jar >/dev/null 2>&1 && \
       ! ls "${local_sdk}"/platforms/android-*/android.jar >/dev/null 2>&1; then
        log_warn "No Android platform installed under ${sdk} or ${local_sdk}."
        local sdkmanager="${sdk}/cmdline-tools/latest/bin/sdkmanager"
        if [[ ! -x "${sdkmanager}" ]]; then
            sdkmanager="$(command -v sdkmanager 2>/dev/null || true)"
        fi
        if [[ -z "${sdkmanager}" ]]; then
            log_error "android.jar missing and sdkmanager not found. Install a platform: sdkmanager \"platforms;android-34\""
            exit 1
        fi
        mkdir -p "${local_sdk}"
        log_info "Installing platforms;android-34 into ${local_sdk} via sdkmanager..."
        # sdkmanager often exits non-zero even on success (it warns about writing
        # a package properties file), so don't trust its exit code: run it, then
        # check whether android.jar actually landed.
        yes 2>/dev/null | "${sdkmanager}" --sdk_root="${local_sdk}" --licenses >/dev/null 2>&1 || true
        yes 2>/dev/null | "${sdkmanager}" --sdk_root="${local_sdk}" "platforms;android-34" || true
        if ! ls "${local_sdk}"/platforms/android-*/android.jar >/dev/null 2>&1; then
            log_error "sdkmanager did not produce an android.jar under ${local_sdk}/platforms."
            exit 1
        fi
        log_ok "Installed Android platform into ${local_sdk}."
    fi

    # Build the native editor: it drives the NDK player build, script
    # cross-compile, and APK packaging through --build-android.
    sync_submodules
    configure_editor_build
    build_editor_targets

    local editor_bin="${build_dir}/Modularity"
    if [[ ! -x "${editor_bin}" ]]; then
        editor_bin="$(find "${build_dir}" -maxdepth 2 -type f -name Modularity -perm -u+x 2>/dev/null | head -1)"
    fi
    if [[ -z "${editor_bin}" || ! -x "${editor_bin}" ]]; then
        log_error "Could not find the built Modularity editor binary to drive the Android build."
        exit 1
    fi

    # Editor APK ships an on-device clang so scripts can be compiled on the phone.
    # Best-effort: stage the bundle and let the editor pack it (warns if no clang).
    if [[ "${android_editor}" -eq 1 ]]; then
        stage_android_clang_toolchain "${ndk}" "${android_abi}" || \
            log_warn "Continuing editor APK build without an on-device compiler."
    fi

    local -a build_args=(--build-android "--abi=${android_abi}")
    [[ -n "${android_project}" ]] && build_args+=("--project=${android_project}")
    [[ -n "${android_output}" ]] && build_args+=(-o "${android_output}")
    [[ "${android_debug}" -eq 1 ]] && build_args+=(--debug)
    [[ "${android_editor}" -eq 1 ]] && build_args+=(--editor)

    log_info "Running: ${editor_bin} ${build_args[*]}"
    "${editor_bin}" "${build_args[@]}"
}

show_stage_hierarchy() {
    local -a stages=()

    if [[ "${skip_deps}" -eq 0 && "${build_platform}" != "windows" && "$(uname -s)" == "Linux" ]]; then
        stages+=("Check/install system dependencies")
    fi
    if [[ "${clean_build}" -eq 1 ]]; then
        stages+=("Clean editor build directory")
        stages+=("Clean player cache directory")
    fi
    stages+=(
        "Sync git submodules"
        "Configure editor build"
        "Build editor + engine targets"
        "Install editor artifacts"
        "Collect editor third-party libraries"
        "Collect editor engine libraries"
        "Configure player-only cache build"
        "Build ModularityPlayer target"
        "Collect player third-party libraries"
        "Collect player engine libraries"
        "Package artifacts and resources"
    )
    if [[ "${build_platform}" != "windows" && "$(uname -s)" == "Linux" ]]; then
        stages+=("Verify CPU/ISA compatibility of packaged binaries")
        stages+=("Install desktop icon and .desktop entries")
    fi

    log_info "Build stage hierarchy:"
    local i
    local stage_count="${#stages[@]}"
    for ((i = 0; i < stage_count; i++)); do
        local branch="|--"
        if [[ $((i + 1)) -eq "${stage_count}" ]]; then
            branch='`--'
        fi
        printf "  %s [%02d/%02d] %s\n" "${branch}" "$((i + 1))" "${stage_count}" "${stages[$i]}"
    done
}

base_steps=11
total_steps="${base_steps}"
if [[ "${clean_build}" -eq 1 ]]; then
    total_steps=$((total_steps + 2))
fi
if [[ "${skip_deps}" -eq 0 && "${build_platform}" != "windows" && "$(uname -s)" == "Linux" ]]; then
    total_steps=$((total_steps + 1))
fi
if [[ "${build_platform}" != "windows" && "$(uname -s)" == "Linux" ]]; then
    # +1 for desktop integration, +1 for the CPU/ISA verification step.
    total_steps=$((total_steps + 2))
fi

build_started=1
detect_cmake_generator
printf "%s================================%s\n" "${C_BOLD}" "${C_RESET}"
printf "%s   Modularity - %s%s\n" "${C_BOLD}" "${build_platform_label}" "${C_RESET}"
printf "%s================================%s\n" "${C_BOLD}" "${C_RESET}"
if [[ "${jobs_overridden}" -eq 0 && "${jobs}" -lt "${ncpus}" ]]; then
    log_info "Build type: ${build_type} | C++: ${build_cpp_standard} | Jobs: ${jobs} (of ${ncpus} cores; reserved 2 to keep desktop responsive — override with --jobs=N)"
else
    log_info "Build type: ${build_type} | C++: ${build_cpp_standard} | Jobs: ${jobs}"
fi
if [[ -n "${cmake_generator}" ]]; then
    log_info "CMake generator: ${cmake_generator}"
fi
log_info "Package format: ${package_format}"
if [[ "${cpu_target}" == "native" ]]; then
    log_warn "CPU target: native (DEV ONLY — host-specific ISA; do not ship this build)"
else
    log_info "CPU target: ${cpu_target} (x86-64 ISA level for shipped binaries)"
fi
if [[ "${enable_asan}" -eq 1 ]]; then
    log_info "Sanitizers: AddressSanitizer + UBSan enabled (consider --build-type=Debug for richer reports)"
fi
if [[ "${build_platform}" == "windows" && -n "${windows_toolchain_file}" ]]; then
    log_info "Windows toolchain: ${windows_toolchain_file}"
fi
# Android takes a dedicated path: install native deps, build the editor, then
# let it produce the signed APK. It does not run the desktop player-cache /
# packaging / ISA-verification stages.
if [[ "${build_platform}" == "android" ]]; then
    if [[ "${skip_deps}" -eq 0 && "$(uname -s)" == "Linux" ]]; then
        run_step "Installing Dependencies" ensure_linux_dependencies
    fi
    configure_ccache
    run_android_build
    exit $?
fi

show_stage_hierarchy

if [[ "${skip_deps}" -eq 0 && "${build_platform}" != "windows" && "$(uname -s)" == "Linux" ]]; then
    run_step "Installing Dependencies" ensure_linux_dependencies
elif [[ "${skip_deps}" -eq 0 ]]; then
    log_warn "Auto dependency install is only implemented for native Linux builds."
fi

configure_ccache

if [[ "${clean_build}" -eq 1 ]]; then
    run_long_step "Cleaning Editor" clean_editor_build
    run_long_step "Cleaning Player" clean_player_cache
fi

run_long_step "Syncing Submodules" sync_submodules
run_long_step "Configuring Editor" configure_editor_build
run_long_step "Building Editor" build_editor_targets
run_long_step "Installing Editor" install_editor_targets
run_long_step "Collecting Editor Libs" copy_third_party_libraries "${build_dir}"
run_long_step "Collecting Engine Libs" copy_engine_libraries "${build_dir}"
run_long_step "Configuring Player" configure_player_build
run_long_step "Building Player" build_player_target
run_long_step "Collecting Player Libs" copy_third_party_libraries "${player_cache_dir}"
run_long_step "Collecting Player Engine Libs" copy_engine_libraries "${player_cache_dir}"
run_long_step "Packaging Engine" finalize_packaging
if [[ "${build_platform}" != "windows" && "$(uname -s)" == "Linux" ]]; then
    run_step "Verifying CPU/ISA" verify_release_isa
    run_long_step "Installing Desktop Icon" install_desktop_integration
fi
