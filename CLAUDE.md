# CLAUDE.md
This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is
Modularity is a custom C++14 game engine + editor with its own scripting language (**ModuCPP**). The stack is OpenGL (rendering), GLFW (windowing/input), GLM (math), ImGui/ImGuizmo (editor UI + gizmos), and miniaudio (audio). Integrate with these existing libraries — do not introduce replacement subsystems (renderer, UI framework, math layer, audio backend) unless explicitly asked.

## Build & Run
`build.sh` is the entry point. It locates the repo root, installs system deps (pacman/apt/dnf/zypper), syncs git submodules + git-lfs, configures CMake, and builds. The CMake C++ standard is 14. (but works up to C++ 26 with no issues or changes.)
```bash
./build.sh                       # native (Linux) Release build [faster due to the default packaging being zip]
./build.sh --7z                 # Builds with Packaging using 7z, which is MUCH slower to compress, but typically used in final builds of Modularity
./build.sh --clean               # wipe build dirs first
./build.sh --build-type=Debug
./build.sh --fsanitize           # AddressSanitizer + UBSan (-DMODULARITY_ENABLE_ASAN=ON)
./build.sh --Windows             # cross-compile Windows via MinGW-w64
./build.sh --jobs=N              # parallel jobs (default: nproc-2)
./build.sh --generator=Ninja
./buildandrun.sh                 # build.sh then run the resulting binary
```
Build output lands in `build/`. `compile_commands.json` is symlinked from `build/` for clangd/LSP.
About the mention of any **test suite's** in this repo, the verification is done by either natively testing through building and running the editor/player, or if testing debug in projects, using the project boot function for Modularity via `./Modularity --project <path/to/Project.modu>` (aliases: `--open <path>`, or just pass a `.modu` path positionally; `--project=<path>`/`--open=<path>` also work).

### Targets
- `Modularity` (`src/main.cpp`) — the **editor** executable. Built when `MODULARITY_BUILD_EDITOR=ON`. Run with `--project <path.modu>` / `--open <path>` to auto-open a project, or `--lsp` to run as a language server.
- `ModularityPlayer` (`src/main_player.cpp`) — the standalone **player/runtime** executable.
- `core` (static lib) — full engine including editor.
- `core_player` (static/shared lib) — runtime-only engine, compiled with `MODULARITY_RUNTIME_ONLY=1`. On Android it becomes `libModularityPlayer.so`.

Notable CMake options: `MODULARITY_ENABLE_JOLT` (Jolt physics, ON), `MODULARITY_ENABLE_PHYSX`, `MODULARITY_ENABLE_VULKAN` (experimental), `MODULARITY_ENABLE_ASSIMP`, `MODULARITY_USE_MONO` (C# scripts), `MODULARITY_USE_OPENGL_ES`, `MODULARITY_ENABLE_SNDFILE`/`MODULARITY_ENABLE_OPUSFILE` (audio import). Windows cross-builds disable Mono/PhysX/Vulkan/sndfile/opusfile.

## Editor vs. Runtime Split
This is the most important architectural invariant. The editor is **compile-time removable** from shipped builds:
- Editor-only code is guarded by `#if !MODULARITY_RUNTIME_ONLY` (the player defines `MODULARITY_RUNTIME_ONLY=1`).
- Treat editor code as removable unless proven required; prefer compile-time exclusion for editor/dev systems.
- **Never break standalone player rendering, scripting, or scene loading** when changing editor code, and vice-versa. Keep editor rendering and runtime rendering behavior separate where needed.
- When uncertain whether a subsystem is runtime-required, mark it validation-required rather than removing it.

## Architecture
`Engine` (`src/Engine.h`/`.cpp`) is the central object — owns the window, renderers, scene, scripting, physics, audio, and editor UI, and drives init → run loop → shutdown. `Engine.h`'s include list is a good map of the major subsystems.

Key subsystems (all under `src/`):
- **Rendering** — `Rendering.*`, plus the 2.5D renderer in `Render25D/` (`TMOpenGLRenderer`, `TMSceneBuilder`, `TMRenderer`). `Skybox/`, `Textures/`, `Lighting2D.*`. Prefer existing renderer abstractions and texture/material flow over one-off raw OpenGL; if raw GL is unavoidable, keep it localized. Avoid per-frame heap allocations and reallocating GPU resources every frame.
- **Scene** — `SceneObject.h`, `SceneSerialization.*` (`.modu` projects/scenes). Changing serialization or SceneObject layout has downstream impact — check call sites.
- **Scripting** — see below.
- **Physics** — pluggable via `IPhysicsBackend.h` + `PhysicsBackendFactory.*`; backends are `JoltPhysicsBackend.*` and PhysX. `PhysicsSystem.*` is the engine-facing layer.
- **Editor** — `EditorUI.*` and the panels in `EditorWindows/` (Inspector, FileBrowser, BuildSettings, Scripting, VisualScripting, AIPathfinding, etc.; viewport/menu/play-controls under `EditorWindows/Panels/`). Keep ImGui Begin/End and BeginTable/EndTable balanced; guard editor preview code against null pointers, invalid textures, zero sizes, and unloaded assets.
- **Assets/packaging** — `ModuPak.*` (`.modupak` archives), `PackageManager.*`, `ModelLoader.*` (Assimp), `RuntimeContent.*`, `redist/` (bundled Windows DLLs).
- **Platform** — `Platform/`, `WinView/Window.cpp`, `AndroidRuntime/`, `Vulkan/`.

Third-party libs live in `src/ThirdParty/` as git submodules (ModuGUI = the docking ImGui fork, glfw = Cherno's fork, ImGuizmo, assimp, PhysX, opengl_video_player). Jolt is vendored under `src/ThirdParty/JoltPhysics/`.

## ModuCPP Scripting
ModuCPP (`.moducpp` files, e.g. `Scripts/*.moducpp`) is a high-level gameplay language that is **transpiled to native C++** (`ModuCPPTranspiler.*`), then compiled to a shared library (`ScriptCompiler.*`) and loaded at runtime via `dlopen` (`ScriptRuntime.*`). It keeps native performance while adding higher-level syntax, generated inspectors, and module imports.

Flow: `.moducpp` → transpiler → `.cpp` → `ScriptCompiler` (invokes the system C++ compiler) → `.so`/`.dll` → `ScriptRuntime` loads it.

Script-facing API headers are in `include/` (`ModuCPPScriptApi.h`, `ModuEngineScriptApi.h`, `ModuInputScriptApi.h`, `RMeshBuilderScriptApi.h`, etc.). Imports use `add ModuCPP;` / `add ModuEngine;` which the transpiler lowers to the matching `#include`. `add ModuCPP;` does NOT auto-import ModuEngine/ModuInput/RMeshBuilder/Experimental.

Script classes derive from `ModuNode` (preferred for new scripts) or `ModuBehaviour` (legacy/native ports). Public fields are persisted and shown in the inspector unless overridden via `Script_OnInspector()` / `inspector { ... }`; private fields are runtime-only. Lifecycle methods like `TickUpdate()` are called by the runtime.

There are also native C++ scripts (`ScriptContext`, `MODU_SCRIPT(ctx)`), native C scripts (`include/ScriptRuntimeCAPI.h`), and managed C# via Mono (`Scripts/Managed/ModuCPP.cs`). Docs: `docs/Scripting.md`, `docs/ModuCPP_Language_Reference.md`, `docs/moducpp/`.

### ModuCPP rules
- In ModuCPP script-facing member access, use `.`, never `->`.
- Prefer declarative inspector fields and `AutoFields(...)` where supported.
- Move reusable helpers into shared headers (e.g. under `include/` or `Scripts/*.h`) rather than duplicating them across scripts.
- **If you modify the ModuCPP ABI, bump `MODULARITY_NATIVE_SCRIPT_ABI_VERSION` in `src/ScriptRuntime.h`.** It is validated at load time and emitted into generated wrappers by `ScriptCompiler`.

## Known Traps (read before touching these)
- **`ScriptCompiler::makeCommands` is expensive (~250 ms/script).** It reads script source and runs multiple `std::regex_search` passes to detect entry-point shapes. Never call it from hot paths (per-frame inspector draws, project-load init, scene refreshes) just to get a binary path. To get the output path, derive it directly: `config.outDir / <relative-parent> / <stem><ext>` — the same logic `makeCommands` uses for `binaryPath`. Only call `makeCommands` when you need the full compile/link command lines.
- **`ScriptRuntime::unloadAll` intentionally does NOT call `dlclose`.** Engine-side state (callbacks, vtables on script-defined types, component data) holds pointers into the loaded `.so`s; unmapping creates dangling refs and heap corruption that surfaces hours later. Do not re-add `dlclose` without proper lifetime tracking (or `RTLD_NODELETE` at every `dlopen` site).

## Working Conventions (from AGENTS.md)
- Preserve existing runtime behavior during refactors; fix root causes instead of masking crashes with broad workarounds. For bug fixes, preserve behavior outside the failing case.
- Extend existing classes/helpers/editor panels and reuse current engine systems rather than adding parallel systems or new abstraction layers.
- Read nearby files first and match surrounding style, naming, and structure; prefer repo search over external search for engine questions.
- Don't rename files/classes/methods/fields without a strong reason. Keep public APIs stable unless a change is necessary.
- Add guards/validation where engine or editor code may touch not-yet-initialized resources; use thread-safe patterns for background/async loading. Avoid blocking work in render paths and hot update loops.