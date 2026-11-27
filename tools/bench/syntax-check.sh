#!/usr/bin/env bash
# Compile-check one engine source file without touching any build directory.
#   tools/bench/syntax-check.sh src/Engine.cpp [more.cpp ...]
# Replays the file's exact command from compile_commands.json with -fsyntax-only, so it is safe
# to run while a build (or another check) is in progress. MODU_COMPILE_DB overrides the database.
set -u
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
root="$(cd -- "${script_dir}/../.." && pwd -P)"
db="${MODU_COMPILE_DB:-${root}/build/compile_commands.json}"
status=0
for f in "$@"; do
    abs="$(cd -- "$(dirname -- "$f")" && pwd -P)/$(basename -- "$f")"
    python3 - "$db" "$abs" <<'PY' || status=1
import json, shlex, subprocess, sys
db, target = sys.argv[1], sys.argv[2]
entries = [e for e in json.load(open(db)) if e["file"] == target]
if not entries:
    sys.exit(f"no compile command for {target} in {db}")
# Prefer the editor (core) variant over the runtime-only one when a file is in both.
entries.sort(key=lambda e: "MODULARITY_RUNTIME_ONLY" in e["command"])
ok = True
for e in entries:
    args = shlex.split(e["command"])
    out = []
    skip = False
    for a in args:
        if skip: skip = False; continue
        if a == "-o": skip = True; continue
        if a in ("-c",) or a.startswith("-MF") or a in ("-MD", "-MMD", "-MT", "-MQ"):
            if a in ("-MT", "-MQ", "-MF"): skip = True
            continue
        out.append(a)
    if out and out[0].endswith("ccache"): out = out[1:]
    variant = "player" if "MODULARITY_RUNTIME_ONLY" in e["command"] else "editor"
    r = subprocess.run(out + ["-fsyntax-only", "-w"], cwd=e["directory"])
    print(f"[syntax-check] {target} ({variant}): {'ok' if r.returncode == 0 else 'FAILED'}")
    ok = ok and r.returncode == 0
sys.exit(0 if ok else 1)
PY
done
exit $status
