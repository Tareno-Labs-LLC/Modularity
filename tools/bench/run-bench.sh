#!/usr/bin/env bash
# Repeatable frame-time benchmark for a Modularity project.
#
#   tools/bench/run-bench.sh <label> <project.modu> [mode] [seconds] [warmup-seconds]
#
# mode:  edit        editor, edit mode (Hierarchy + Game Viewport + Inspector as saved in the layout)
#        editor-play editor, play mode inside the Game Viewport (MODULARITY_DEBUG_AUTOPLAY)
#        play        editor binary, --play (the player render path, no editor panels)
#
# Environment:
#   MODU_BENCH_BIN    path to the Modularity binary (default: build/Modularity)
#   MODU_BENCH_CPUS   taskset CPU list (default "2,3": two cores, the N3060's core count)
#   MODU_BENCH_VSYNC  1 keeps the driver's vsync; default 0 forces it off (Mesa vblank_mode=0)
#   MODU_BENCH_OUT    output directory (default: /tmp/modularity-bench)
#
# Uses the engine's own MODULARITY_PROFILE_DUMP windows ([ProfileStats] lines) and averages the
# windows after the warmup, so script compilation and asset streaming at startup are excluded.
# CPU utilisation and RSS are read from /proc for the same windows. Nothing here changes the
# project: point it at a copy if you do not want Library/ caches touched.
set -u
label="${1:?label}"; project="${2:?project.modu}"; mode="${3:-edit}"; seconds="${4:-60}"; warmup="${5:-40}"
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
bin="${MODU_BENCH_BIN:-${script_dir}/../../build/Modularity}"
cpus="${MODU_BENCH_CPUS:-2,3}"
out="${MODU_BENCH_OUT:-/tmp/modularity-bench}"
mkdir -p "${out}"
log="${out}/${label}.log"
args=(--project "${project}")
export MODULARITY_PROFILE_DUMP=5 MODULARITY_PROFILE_DUMP_TOP="${MODULARITY_PROFILE_DUMP_TOP:-28}"
[[ "${MODU_BENCH_VSYNC:-0}" == "1" ]] || export vblank_mode=0
case "${mode}" in
    edit) ;;
    editor-play) export MODULARITY_DEBUG_AUTOPLAY=600 ;;
    play) args+=(--play) ;;
    *) echo "unknown mode ${mode}" >&2; exit 2 ;;
esac
cd "$(dirname -- "${bin}")" || exit 1
taskset -c "${cpus}" "${bin}" "${args[@]}" >"${log}" 2>&1 &
pid=$!
sleep "${warmup}"
if ! kill -0 "${pid}" 2>/dev/null; then echo "engine exited during warmup; see ${log}" >&2; exit 1; fi
hz=$(getconf CLK_TCK)
read_ticks() { awk '{print $14+$15}' "/proc/${pid}/stat" 2>/dev/null; }
t0=$(read_ticks); w0=$(date +%s.%N); lines0=$(wc -l <"${log}")
sleep "${seconds}"
t1=$(read_ticks); w1=$(date +%s.%N)
rss_kb=$(awk '/VmRSS/{print $2}' "/proc/${pid}/status" 2>/dev/null)
kill -INT "${pid}" 2>/dev/null; sleep 2; kill -KILL "${pid}" 2>/dev/null; wait "${pid}" 2>/dev/null
tail -n "+$((lines0 + 1))" "${log}" > "${out}/${label}.window.log"
python3 - "${label}" "${mode}" "${out}/${label}.window.log" "${t0:-0}" "${t1:-0}" "${w0}" "${w1}" "${hz}" "${rss_kb:-0}" <<'PY'
import re, sys, collections
label, mode, path, t0, t1, w0, w1, hz, rss = sys.argv[1:]
stats = collections.defaultdict(list); scopes = collections.defaultdict(list); windows = 0
for line in open(path, errors="replace"):
    if line.startswith("[ProfileStats]"):
        windows += 1
        for k, v in re.findall(r"(\w+)=([\d.]+)", line): stats[k].append(float(v))
    else:
        m = re.match(r"\[ProfileDump\]\s{3}(?!worst>)(.+?)\s+([\d.]+)ms/frame", line)
        if m: scopes[m.group(1).strip()].append(float(m.group(2)))
if not windows: sys.exit("no [ProfileStats] windows captured")
frames = sum(stats["frames"])
w = lambda k: sum(a * f for a, f in zip(stats[k], stats["frames"])) / frames   # frame-weighted
mx = lambda k: max(stats[k])
cpu_pct = 100.0 * (float(t1) - float(t0)) / float(hz) / (float(w1) - float(w0))
print(f"== {label} [{mode}] windows={windows} frames={int(frames)}")
print(f"   fps_avg={1000.0 / w('frame_avg'):.1f}  frame avg/p50/p95/p99(max of windows) = "
      f"{w('frame_avg'):.3f}/{w('frame_p50'):.3f}/{mx('frame_p95'):.3f}/{mx('frame_p99'):.3f} ms")
print(f"   work  avg/p50/p95/p99 = {w('work_avg'):.3f}/{w('work_p50'):.3f}/{mx('work_p95'):.3f}/{mx('work_p99'):.3f} ms"
      f"   main-thread cpu = {w('cpu_avg'):.3f} ms/frame   wait = {w('wait_avg'):.3f} ms")
gpu = w('gpu_avg') if sum(stats['gpu_samples']) else float('nan')
print(f"   gpu = {gpu:.3f} ms   draws={w('draws'):.1f} texbinds={w('texbinds'):.1f} statebinds={w('statebinds'):.1f}"
      f" allocs/frame={w('allocs'):.1f}   process cpu={cpu_pct:.0f}% of one core   rss={int(rss) // 1024} MB")
for name, vals in sorted(scopes.items(), key=lambda kv: -sum(kv[1]) / windows)[:28]:
    print(f"      {name:<38s} {sum(vals) / windows:8.3f} ms/frame")
PY
