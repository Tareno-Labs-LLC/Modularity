# verify-cpu-isa.sh: Release gate for x86-64 CPU/ISA compatibility.
# Public Linux releases of Modularity must run on any baseline x86-64 CPU.
# A binary or bundled shared library that was compiled with -march=native,
# -march=x86-64-v3/-v4, or AVX/AVX2/AVX-512 flags gets a GNU_PROPERTY note of
# "x86 ISA needed: ... x86-64-v3" (or -v4). The dynamic loader then refuses to
# start it on older CPUs with: './Modularity: CPU ISA level is lower than required'
# This script scans the main executable plus every bundled ELF (.so) and fails
# if any of them *require* an ISA level above the allowed maximum.
# Usage:
#   tools/verify-cpu-isa.sh [--max-level=LEVEL] [PATH ...]
#   --max-level=LEVEL   Highest ISA level allowed in "x86 ISA needed".
#                       One of: baseline (default), x86-64-v2, x86-64-v3,
#                       x86-64-v4. Public releases MUST use baseline.
#   PATH ...            Files and/or directories to scan. Directories are
#                       searched recursively for the Modularity/
#                       ModularityPlayer executables and *.so files.
#                       Defaults to ./build if nothing is given.
# Exit status: 0 = all clear, 1 = a binary exceeds the allowed level, 2 = usage / environment error.

set -u
max_level="baseline"
declare -a scan_paths=()
for arg in "$@"; do
    case "${arg}" in
        --max-level=*)
            max_level="${arg#*=}"
            ;;
        -h|--help)
            sed -n '2,30p' "$0"
            exit 0
            ;;
        --*)
            echo "verify-cpu-isa: unknown option: ${arg}" >&2
            exit 2
            ;;
        *)
            scan_paths+=("${arg}")
            ;;
    esac
done

level_rank() { # Map an ISA level name to a numeric rank so we can compare them.
    case "$1" in
        baseline|x86-64-baseline) echo 0 ;;
        x86-64-v2)                echo 2 ;;
        x86-64-v3)                echo 3 ;;
        x86-64-v4)                echo 4 ;;
        *)                        echo -1 ;;
    esac
}
max_rank="$(level_rank "${max_level}")"
if [[ "${max_rank}" -lt 0 ]]; then
    echo "verify-cpu-isa: invalid --max-level='${max_level}' (use baseline|x86-64-v2|x86-64-v3|x86-64-v4)" >&2
    exit 2
fi
if ! command -v readelf >/dev/null 2>&1; then
    echo "verify-cpu-isa: 'readelf' not found (install binutils) — cannot verify ISA levels." >&2
    exit 2
fi
if [[ "${#scan_paths[@]}" -eq 0 ]]; then
    scan_paths=("build")
fi
# Collect the concrete list of ELF files to check.
declare -a files=()
for path in "${scan_paths[@]}"; do
    if [[ -f "${path}" ]]; then
        files+=("${path}")
    elif [[ -d "${path}" ]]; then
        while IFS= read -r -d '' f; do
            files+=("${f}")
        done < <(find "${path}" -type f \
            \( -name '*.so' -o -name '*.so.*' \
               -o -name 'Modularity' -o -name 'ModularityPlayer' \) -print0)
    else
        echo "verify-cpu-isa: warning: '${path}' is not a file or directory; skipping." >&2
    fi
done
if [[ "${#files[@]}" -eq 0 ]]; then
    echo "verify-cpu-isa: no executables or .so files found to check." >&2
    exit 2
fi
echo "verify-cpu-isa: max allowed ISA level = ${max_level}"
echo
fail=0
checked=0
for file in "${files[@]}"; do
    # Only inspect ELF binaries; skip text/scripts that share a name pattern.
    if ! head -c4 "${file}" 2>/dev/null | grep -q $'\x7fELF'; then
        continue
    fi
    checked=$((checked + 1))
    notes="$(readelf -n "${file}" 2>/dev/null)"
    needed_line="$(printf '%s\n' "${notes}" | grep -i 'x86 ISA needed' | head -n1)"
    if [[ -z "${needed_line}" ]]; then
        printf '  [ ok ] %s\n         x86 ISA needed: (none)\n' "${file}"
        continue
    fi

    # Find the highest level mentioned in the "needed" line.
    file_rank=0
    file_level="baseline"
    for lvl in x86-64-v4 x86-64-v3 x86-64-v2; do
        if printf '%s' "${needed_line}" | grep -q "${lvl}"; then
            file_rank="$(level_rank "${lvl}")"
            file_level="${lvl}"
            break
        fi
    done
    trimmed="$(printf '%s' "${needed_line}" | sed 's/^[[:space:]]*//')"
    if [[ "${file_rank}" -gt "${max_rank}" ]]; then
        printf '  [FAIL] %s\n         %s  (requires %s > allowed %s)\n' \
            "${file}" "${trimmed}" "${file_level}" "${max_level}"
        fail=1
    else
        printf '  [ ok ] %s\n         %s\n' "${file}" "${trimmed}"
    fi
done
echo
if [[ "${checked}" -eq 0 ]]; then
    echo "verify-cpu-isa: no ELF binaries among the scanned files." >&2
    exit 2
fi
if [[ "${fail}" -ne 0 ]]; then
    echo "verify-cpu-isa: FAILED — one or more binaries exceed the '${max_level}' ISA level."
    echo "                Rebuild with -DMODULARITY_CPU_TARGET=baseline (clean build dir) and re-run."
    exit 1
fi
echo "verify-cpu-isa: OK — all ${checked} binaries are within the '${max_level}' ISA level."
exit 0