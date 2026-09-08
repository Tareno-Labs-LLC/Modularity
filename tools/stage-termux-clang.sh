#!/usr/bin/env bash
set -euo pipefail
usage() {
    cat <<'EOF'
Usage: tools/stage-termux-clang.sh --abi arm64-v8a --bundle <dir> [--cache-dir <dir>]
Downloads the current Termux aarch64 clang/lld packages, verifies package
SHA-256 checksums from the official Packages index, extracts them, and stages
the minimal on-device compiler payload expected by the Android editor APK.
EOF
}
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/.." && pwd)"
abi=""
bundle=""
cache_dir=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --abi)
            abi="${2:-}"
            shift 2 ;;
        --abi=*)
            abi="${1#*=}"
            shift ;;
        --bundle)
            bundle="${2:-}"
            shift 2 ;;
        --bundle=*)
            bundle="${1#*=}"
            shift ;;
        --cache-dir)
            cache_dir="${2:-}"
            shift 2 ;;
        --cache-dir=*)
            cache_dir="${1#*=}"
            shift ;;
        -h|--help)
            usage
            exit 0 ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2 ;;
    esac
done
if [[ "${abi}" != "arm64-v8a" ]]; then
    echo "Termux clang staging currently supports arm64-v8a only." >&2
    exit 2
fi
if [[ -z "${bundle}" ]]; then
    echo "--bundle is required." >&2
    exit 2
fi
for tool in curl awk ar tar sha256sum cp chmod mkdir rm find readelf mv dirname du; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "Required tool not found in PATH: ${tool}" >&2
        exit 1
    fi
done
repo="${MODULARITY_TERMUX_REPO:-https://packages-cf.termux.dev/apt/termux-main}"
fallback_repo="${MODULARITY_TERMUX_FALLBACK_REPO:-https://packages.termux.dev/apt/termux-main}"
cache_dir="${cache_dir:-${repo_root}/build/android-clang/.termux-cache/${abi}}"
packages_file="${cache_dir}/Packages"
download_dir="${cache_dir}/debs"
extract_root="${cache_dir}/extract"
termux_prefix_in_package="data/data/com.termux/files/usr"
mkdir -p "${cache_dir}" "${download_dir}"
fetch_url() {
    local url="$1"
    local out="$2"
    curl -fsSL --retry 4 --retry-delay 2 -o "${out}" "${url}"
}
echo "[Termux clang] Fetching package index..."
if ! fetch_url "${repo}/dists/stable/main/binary-aarch64/Packages" "${packages_file}.tmp"; then
    fetch_url "${fallback_repo}/dists/stable/main/binary-aarch64/Packages" "${packages_file}.tmp"
    repo="${fallback_repo}"
fi
mv "${packages_file}.tmp" "${packages_file}"
package_field() {
    local package="$1"
    local field="$2"
    awk -v pkg="${package}" -v field="${field}" '
        BEGIN { RS=""; FS="\n" }
        $1 == "Package: " pkg {
            for (i = 1; i <= NF; ++i) {
                if ($i ~ "^" field ": ") {
                    sub("^" field ": ", "", $i)
                    print $i
                    exit
                }
            }
        }
    ' "${packages_file}"
}
download_package() {
    local package="$1"
    local filename sha out
    filename="$(package_field "${package}" "Filename")"
    sha="$(package_field "${package}" "SHA256")"
    if [[ -z "${filename}" || -z "${sha}" ]]; then
        echo "Package metadata missing for ${package}." >&2
        exit 1
    fi
    out="${download_dir}/${filename##*/}"
    if [[ -f "${out}" ]] && printf '%s  %s\n' "${sha}" "${out}" | sha256sum -c - >/dev/null 2>&1; then
        echo "[Termux clang] Using cached ${filename##*/}"
        return
    fi
    echo "[Termux clang] Downloading ${filename##*/}"
    rm -f "${out}"
    fetch_url "${repo}/${filename}" "${out}"
    printf '%s  %s\n' "${sha}" "${out}" | sha256sum -c -
}
extract_package() {
    local package="$1"
    local filename deb data_member
    filename="$(package_field "${package}" "Filename")"
    deb="${download_dir}/${filename##*/}"
    data_member="$(ar t "${deb}" | awk '/^data[.]tar[.]/ { print; exit }')"
    if [[ -z "${data_member}" ]]; then
        echo "No data.tar member found in ${deb}." >&2
        exit 1
    fi
    case "${data_member}" in
        *.tar.zst)
            ar p "${deb}" "${data_member}" | tar --zstd -C "${extract_root}" -xf -
            ;;
        *.tar.xz)
            ar p "${deb}" "${data_member}" | tar -C "${extract_root}" -xJf -
            ;;
        *.tar.gz)
            ar p "${deb}" "${data_member}" | tar -C "${extract_root}" -xzf -
            ;;
        *)
            echo "Unsupported package payload: ${data_member}" >&2
            exit 1 ;;
    esac
}

# Keep this intentionally small. These packages provide clang/lld, the clang resource directory, and the dynamic-library closure needed to run them.
packages=(
    clang
    lld
    libllvm
    libcompiler-rt
    libc++
    libffi
    libxml2
    zlib
    zstd
    libicu
    libiconv
)
rm -rf "${extract_root}"
mkdir -p "${extract_root}"
for package in "${packages[@]}"; do
    download_package "${package}"
    extract_package "${package}"
done
src_usr="${extract_root}/${termux_prefix_in_package}"
clang_bin=""
for candidate in "${src_usr}"/bin/clang-[0-9]*; do
    if [[ -x "${candidate}" ]]; then
        clang_bin="${candidate}"
    fi
done
if [[ -z "${clang_bin}" && -x "${src_usr}/bin/clang" ]]; then
    clang_bin="${src_usr}/bin/clang"
fi
lld_bin="${src_usr}/bin/lld"
if [[ -z "${clang_bin}" || ! -x "${lld_bin}" ]]; then
    echo "Extracted Termux clang/lld binaries were not found." >&2
    exit 1
fi
termux_usr="${bundle}/termux/usr"
rm -rf "${termux_usr}" "${bundle}/bin"
mkdir -p "${termux_usr}/bin" "${termux_usr}/lib" "${bundle}/bin"
copy_exec() {
    local src="$1"
    local rel="$2"
    mkdir -p "$(dirname -- "${termux_usr}/${rel}")"
    cp -L "${src}" "${termux_usr}/${rel}"
    chmod 0755 "${termux_usr}/${rel}"
}
copy_data() {
    local src="$1"
    local rel="$2"
    mkdir -p "$(dirname -- "${termux_usr}/${rel}")"
    cp -aL "${src}" "${termux_usr}/${rel}"
}
copy_exec "${clang_bin}" "bin/clang"
copy_exec "${clang_bin}" "bin/clang++"
copy_exec "${lld_bin}" "bin/lld"
copy_exec "${lld_bin}" "bin/ld.lld"
mkdir -p "${termux_usr}/lib/clang"
copied_resource_dirs=0
for resource_dir in "${src_usr}"/lib/clang/*; do
    if [[ -d "${resource_dir}" && ! -L "${resource_dir}" ]]; then
        copy_data "${resource_dir}" "lib/clang/${resource_dir##*/}"
        copied_resource_dirs=$((copied_resource_dirs + 1))
    fi
done
if (( copied_resource_dirs == 0 )); then
    echo "Extracted Termux clang resource directory was not found." >&2
    exit 1
fi
declare -A copied_libs=()
is_system_lib() {
    case "$1" in
        libc.so|libdl.so|liblog.so|libm.so|libandroid.so)
            return 0 ;;
    esac
    return 1
}
read_needed_libs() {
    readelf -d "$1" | awk -F'[][]' '/Shared library:/ { print $2 }'
}
copy_needed_lib() {
    local lib_name="$1"
    if is_system_lib "${lib_name}"; then
        return
    fi
    if [[ -n "${copied_libs[${lib_name}]:-}" ]]; then
        return
    fi
    local src="${src_usr}/lib/${lib_name}"
    if [[ ! -e "${src}" ]]; then
        echo "Missing Termux runtime library needed by clang/lld: ${lib_name}" >&2
        exit 1
    fi
    copied_libs["${lib_name}"]=1
    copy_data "${src}" "lib/${lib_name}"
    local soname
    soname="$(readelf -d "${src}" | awk -F'[][]' '/Library soname:/ { print $2; exit }')"
    if [[ -n "${soname}" && "${soname}" != "${lib_name}" && -z "${copied_libs[${soname}]:-}" ]]; then
        copied_libs["${soname}"]=1
        copy_data "${src}" "lib/${soname}"
    fi
    local needed
    while IFS= read -r needed; do
        [[ -n "${needed}" ]] && copy_needed_lib "${needed}"
    done < <(read_needed_libs "${src}")
}
while IFS= read -r needed; do
[[ -n "${needed}" ]] && copy_needed_lib "${needed}"
done < <(read_needed_libs "${clang_bin}")
while IFS= read -r needed; do
    [[ -n "${needed}" ]] && copy_needed_lib "${needed}"
done < <(read_needed_libs "${lld_bin}")
cat > "${bundle}/bin/clang" <<'EOF'
#!/system/bin/sh
DIR="${0%/*}"
ROOT="${DIR%/*}"
TERMUX_USR="${ROOT}/termux/usr"
export LD_LIBRARY_PATH="${TERMUX_USR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export PATH="${TERMUX_USR}/bin:${ROOT}/bin:${PATH}"
exec "${TERMUX_USR}/bin/clang" "$@"
EOF
cat > "${bundle}/bin/ld.lld" <<'EOF'
#!/system/bin/sh
DIR="${0%/*}"
ROOT="${DIR%/*}"
TERMUX_USR="${ROOT}/termux/usr"
export LD_LIBRARY_PATH="${TERMUX_USR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export PATH="${TERMUX_USR}/bin:${ROOT}/bin:${PATH}"
exec "${TERMUX_USR}/bin/ld.lld" "$@"
EOF
chmod 0755 "${bundle}/bin/clang" "${bundle}/bin/ld.lld"
echo "[Termux clang] Staged $(du -sh "${bundle}/termux" | awk '{print $1}') under ${bundle}/termux"
echo "[Termux clang] Wrapper ready: ${bundle}/bin/clang"
