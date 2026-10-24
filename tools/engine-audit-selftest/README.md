# Engine audit regression checks

Windows prerequisites: Python, Visual Studio C++ tools, and the repository's
existing Release build/dependency libraries.

From the repository root:

```powershell
python tools/engine-audit-selftest/run.py
python tools/engine-audit-selftest/run_hierarchy.py
python tools/engine-audit-selftest/run_network.py
python tools/engine-audit-selftest/run_engine.py
python tools/engine-audit-selftest/run_graphics.py
python tools/engine-audit-selftest/check_compile.py
```

The mesh suite compiles the production parser/validator without creating a GL
context. Rendering constructor/destructor stubs abort if unexpectedly reached.
The networking suite compiles the changed session implementation with the existing
networking tests, then links against the configured engine dependencies. Compiler
checks use the editor/player defines from their generated Visual Studio projects.

`python tools/engine-audit-selftest/run.py --before` compiles the original validator
from `HEAD` and intentionally fails the integer-overflow regression. It never runs
the unsafe render loop. Test files and generated binaries stay under
`build/engine-audit-check`.

The Engine suite runs a native script DLL, a loopback-only HTTP fixture, and local
child-process cancellation fixtures. The graphics test briefly creates GL windows
and checks real mesh destruction on normal exit and exception unwinding.

`build_direct.py` rebuilds every configured editor/player source into separate
archives and `build/release/*-audit.exe`. It preserves Release definitions/includes
and uses existing dependency libraries while bypassing MSBuild file tracking.
Default invocation recompiles all engine sources. Explicit `--resume` is for a
failed invocation with unchanged headers/configuration; it is not a header tracker.

The focused checks do not replace normal executables. Explicit
`build_direct.py --install` backs up the previous binaries/libraries, relinks the
normal editor/player filenames, and stages Resources/docs/the matching script SDK.
`run_engine.py --full-build` tests the fully rebuilt optimized archive instead of
linking individually compiled changed units.
See [the audit report](../../docs/EngineBugAudit-2026-10-23.md) for scope and limits.
