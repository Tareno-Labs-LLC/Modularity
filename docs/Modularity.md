# Modularity Engine Documentation
New here? Welcome! This is the big-picture tour of Modularity: how the engine fits together, how projects and scenes work, and how you actually use the editor to make stuff. You don't need prior game-engine experience, we'll explain things as we go. (and yes, this doc is kept in sync with the real codebase, so it shouldn't lie to you.)

A couple of mental shortcuts to get you going:
- If you've touched **Unity** before, a lot will feel familiar. You build a scene out of objects, each object carries components (renderer, light, collider, script, etc.), and scripts bring it all to life.
- If you're coming from something like **Scratch**, it's the same "snap behaviors onto things and press play" idea, just with a lot more power under the hood and real code (ModuCPP is basically just simplified C# if you're familiar with that term).

**Where to start:** skim section 1 for the lay of the land, build the engine with section 2, then jump to whatever you actually want to do (make a scene, write a script, add physics). You don't have to read this top to bottom.

## 1) Overview
Modularity is a native C++ game engine with the editor built right in. At its heart are a few pieces working together:
- A **scene graph** made of `SceneObject` instances. Each one is a thing in your world (a cube, a light, a UI button) with component-style flags and data attached.
- An **OpenGL renderer** with post-processing and UI rendering.
- A **scripting system** with three flavors: high-level `ModuCPP` authoring (the friendly one), raw native C++/C shared-library scripts (the powerful one), and managed C# via Mono (experimental).
- **Pluggable 3D physics**: Jolt is the default backend, with PhysX available as an option, plus a lightweight built-in 2D simulation.
- **Audio** via miniaudio, with spatial (3D-positioned) playback and reverb zones.

Don't worry if some of those words are new, each gets its own section below.

## 2) Build and Run
### Primary build scripts
The build scripts do the heavy lifting so you don't have to memorize CMake. They install system dependencies, sync submodules, configure, build, and package for you.
- `build.sh` (Linux/macOS) and `build.bat` (Windows) build the full editor + player.
  - Initializes git submodules.
  - Builds the editor into `build/`.
  - Builds a player-only target into `build/player-cache/`.
  - Copies built libraries into `Packages/Engine` and `Packages/ThirdParty` inside each build folder.
- `build.sh --Windows` cross-builds a Windows target from Linux using MinGW-w64.
  - Needs `x86_64-w64-mingw32-g++` or `x86_64-w64-mingw32-clang++` plus `windres`.
  - Uses the vendored GLFW MinGW toolchain file.
  - Disables Mono, PhysX, Vulkan, and libsndfile for the cross build unless you wire in Windows-target dependencies yourself.
- Android is supported too, but it's heavily work-in-progress (the runtime code lives under `src/AndroidRuntime/` if you want to see where it stands).

For the full flag list (clean builds, debug, sanitizers, packaging, CPU/ISA targets, build profiles), see `docs/Build.md` and `CLAUDE.md`. (both are human-readable, no worries!)

### CMake options
If you'd rather drive CMake directly, these are the switches worth knowing (defined in `CMakeLists.txt`):
- `MODULARITY_BUILD_EDITOR` (on by default): build the editor target. Turn it off for runtime-only/shipping builds.
- `MODULARITY_ENABLE_JOLT` (on by default): the Jolt physics backend, used for 3D physics out of the box.
- `MODULARITY_ENABLE_PHYSX`: the optional PhysX backend. If a project asks for PhysX but it wasn't compiled in, the engine quietly falls back to Jolt.
- `MODULARITY_ENABLE_ASSIMP` (on by default): import models like FBX/GLTF via Assimp.
- `MODULARITY_USE_MONO`: enable Mono embedding for managed C# scripts.
- `MODULARITY_ENABLE_VULKAN`: the experimental Vulkan backend (used when the SDK is available).
- `MODULARITY_ENABLE_SNDFILE` / `MODULARITY_ENABLE_OPUSFILE`: extra audio import formats (WAV/FLAC/etc. and Ogg/Opus).
- `MONO_ROOT`: point at a specific Mono runtime if you're not using the bundled one.

(Some defaults flip depending on platform, for example PhysX, Vulkan, Mono, and sndfile are turned off for Windows cross-builds and Android. The build scripts handle that for you.)

### Entry points
- Editor: `src/main.cpp`
- Player: `src/main_player.cpp`

Both set the working directory to the executable's location before engine init.

## 3) Project Layout
When you create a new project, Modularity sets up a consistent folder structure for you (see `Project::create`). You mostly live in `Assets/`; the rest is the engine's bookkeeping.

```
YourProject/
  Assets/
    Scenes/
    Scripts/
      Runtime/
      Editor/
    Models/
    Shaders/
    Materials/
  Library/
    CompiledScripts/
    InstalledPackages/
    ScriptTemp/
    Temp/
  ProjectUserSettings/
    ProjectLayout/
    ScriptSettings/
  Scripts/
    Managed/            # optional, created when managed C# scripting is set up
  project.modu
  scripts.modu
  packages.modu
```

The files that matter most:
- `project.modu`: project name + last opened scene.
- `scripts.modu`: native script build configuration (the legacy `Scripts.modu` name is still detected).
- `packages.modu`: script dependency manifest.
- Scenes live in `Assets/Scenes` with the `.scene` extension.

## 4) Scene Format and Serialization
A scene is just a text file handled by `SceneSerializer`, which is on purpose:
- The header carries keys like `version`, `nextId`, `timeOfDay`, and `objectCount`.
- Each object is written as an `[Object]` block of key/value pairs.
- Transforms are stored as **local** position/rotation/scale relative to the parent.

Because it's plain text, scenes diff cleanly in git and you can hand-edit them in a pinch.

## 5) Scene Objects and Components
Every object in your world is a `SceneObject`: a transform (where it is) plus a set of component flags and data (what it can do). If you know Unity's GameObject + Component model, this is that. The engine checks flags like `hasRenderer`, `hasLight`, `hasUI`, and so on to decide which systems should touch each object.

### Object types (high level)
Some of the built-in types you can drop into a scene:
- 3D primitives: `Cube`, `Sphere`, `Capsule`, `Plane`, `Torus`
- Imported meshes: `OBJMesh`, `Model` (Assimp-based)
- Lights: `DirectionalLight`, `PointLight`, `SpotLight`, `AreaLight`
- Cameras: `Camera`
- PostFX nodes: `PostFXNode`
- Sprites: `Sprite` (3D), `Sprite2D` (screen space)
- UI: `Canvas`, `UIImage`, `UISlider`, `UIButton`, `UIText`
- Misc: `Mirror`, `Empty`

### Core components
- **Transform**: position/rotation/scale, plus the local transform and parent/child relationships.
- **Renderer**: mesh type + material + textures + shader paths.
- **Light**: light type, color, intensity, range, and light-specific parameters.
- **Camera**: FOV, near/far planes, 2D settings, post-FX toggle.
- **PostFX**: global effects settings (bloom, color adjust, motion blur, vignette, chromatic aberration, AO).
- **Scripts**: one or more script components, authored as `ModuCPP`/native `C++`, `C`, or managed `C#`.

### Physics components
3D (handled by the active backend, Jolt or PhysX):
- `Rigidbody` (mass, damping, gravity, kinematic, lock rotation)
- `Collider` (box, mesh, convex mesh, capsule)

2D (built-in):
- `Rigidbody2D` (velocity, gravity)
- `Collider2D` (box, polygon, edge)
- `ParallaxLayer2D`
- `CameraFollow2D`

### Audio components
- `AudioSource` (clip path, volume, spatial, rolloff)
- `ReverbZone` (shape, blend distance, preset/custom parameters)

### Animation components
- `Animation` (keyframe clip, interpolation)
- `SkeletalAnimation` (GPU skinning, clip index, bone data)

### UI components
The UI component is your Canvas + UI elements:
- Anchoring, pixel positioning, size, color, text, and interaction state.
- UI elements can optionally render in 3D to a texture target.

## 6) Rendering System
The renderer is OpenGL-based and gives you:
- GPU meshes for primitives, imported models, and raw mesh assets.
- Texture and shader caching (so the same asset isn't re-uploaded every frame).
- Skybox rendering.
- Render targets for the game view and previews.
- An optional post-processing pipeline.

The default shaders live in `Renderer`:
- `Resources/Shaders/vert.glsl`
- `Resources/Shaders/frag.glsl`
- `Resources/Shaders/skinned_vert.glsl`
- Post-FX shaders under `Resources/Shaders/`

### Post-processing features
- Bloom
- Exposure/contrast/saturation/color filter
- Motion blur (history buffer)
- Vignette
- Chromatic aberration
- Ambient occlusion (screen-space)

Post-FX is enabled per camera and configured in `PostFXSettings`.

## 7) Materials and Textures
Each renderable object has:
- `MaterialProperties` (color, ambient/specular strength, shininess, texture mix)
- An optional `materialPath` for external material assets
- Albedo/overlay/normal map textures
- Per-object vertex/fragment shader overrides

## 8) Models and Meshes
Where meshes come from:
- **OBJ** via `OBJLoader` (includes triangle data, which is also used for picking and physics).
- **Assimp** via `ModelLoader` for formats like FBX/GLTF, with node hierarchies and animation data.

Raw meshes:
- Editable mesh assets are stored as `.rmesh` and can be loaded/saved through `ModelLoader`.

## 9) Scripting
Scripting is how your objects actually *do* things. You've got three options, from friendliest to most low-level.

### Native scripting (C++ and C)
Native scripts are compiled to shared libraries and hot-loaded at runtime. A per-project `scripts.modu` controls build settings and include paths.

The important ideas:
- `ModuCPP` is the recommended way to write gameplay scripts. `.moducpp` files (and `.cpp` files that declare `public class ... : ModuNode` or `ModuBehaviour`) are transpiled into native C++ before they compile, so you get high-level syntax with native speed.
- The high-level scripting docs are split into manual pages and API reference pages under `docs/moducpp/`.
- Imports are explicit. `add ModuCPP;` brings in the core layer only; `ModuEngine`, `ModuInput`, `RMeshBuilder`, and `ModuCPP.Experimental` are separate, opt-in modules.
- C++ hooks like `Begin`, `TickUpdate`, and `Update` are called for you at the right time.
- C scripts use the C API bridge (`Modu_*` hook names in `.c` files).
- Auto-generated wrappers export `Script_Begin`, `Script_TickUpdate`, and friends when hook names are detected.
- The helper header `#include "ModuCPP"` also works in raw native C++ and hands you `Config<T>()`, `State<T>()`, persistent inspector helpers, input helpers, 2D movement helpers, and audio/sprite facades.

For the full story, see:
- `docs/Scripting.md`
- `docs/moducpp/README.md`

### Managed C# scripting (experimental)
Modularity can also host managed C# scripts through Mono, via the `ModuCPP` managed bridge. This path is experimental and still evolving, so treat it as a preview.
- Managed project files live under `Scripts/Managed` (separate from the `Assets/Scripts` folder native scripts use).
- Scripts can reach object/transform, physics, animation, UI, sprite, audio, settings, and inspector helpers through `ModuCPP.Context`.

For setup and caveats, see `docs/Scripting.md`.

### Shipped scripting examples
The repo ships real example systems that are handy to learn from (and worth showing off in public docs):
- Dialogue runtime with localization, typewriter timing, audio, and text effects
- Interactable objects with proximity checks, keypress interaction, selection-state toggles, and dialogue handoff
- Main menu controller with configurable cursor movement, sounds, and menu actions
- Top-down 2D movement with optional Rigidbody2D driving, directional sprite clips, sprinting, and footsteps
- A standalone 3D movement controller with configurable locomotion/grounding tuning
- Timed object enable/disable behavior, scripted editor windows, and animation tooling

## 10) Physics
### 3D (Jolt by default, PhysX optional)
3D physics goes through a pluggable backend (`IPhysicsBackend`), so the engine isn't tied to one physics library. Jolt is the default and ships on by default; PhysX is available as an option (`MODULARITY_ENABLE_PHYSX`), and if you ask for a backend that wasn't compiled in, the engine falls back to the other one instead of dying. You pick the backend per project. Either way the system:
- Creates rigid bodies/colliders on play start.
- Steps the simulation during play/spec modes.
- Supports raycasts and force/impulse APIs.

### 2D
There's also a simple built-in 2D simulation for UI and 2D gameplay:
- Rigidbody2D movement and damping
- 2D colliders
- Camera follow and parallax layers

## 11) Audio
Audio runs through `AudioSystem` (miniaudio):
- Spatialized sources with rolloff settings.
- Play/stop/loop controls.
- Preview playback right from the editor.
- Reverb zones that blend based on where the listener is.

## 12) Animation
Two ways to animate:
- **Keyframe animation** on the transform (position/rotation/scale).
- **Skeletal animation** for imported skinned meshes, with optional GPU skinning.

## 13) Editor and Workflow
### Core editor areas
The editor is built on Dear ImGui + ImGuizmo. A typical session moves through:
- **Launcher**: create or open a project.
- **Hierarchy**: the scene's object list and parent/child relationships.
- **Inspector**: edit components and run script inspectors.
- **File Browser**: browse assets and run context actions (compile scripts, import models, etc.).
- **Viewport**: edit the scene directly, with gizmos for moving/rotating/scaling.

### Play, Spec, Test
The engine keeps "editing" and "running" cleanly separated:
- Scripts only run in Play/Spec/Test, so they never cause side effects while you're just editing.
- 3D physics simulates when playing or in spec mode.
  - The 2D simulation can run in play/spec/test.

### Script compilation
Inside the editor, you can compile scripts from:
- The file browser context menu
- The script component menu in the inspector

## 14) Packages (Script Dependencies)
The package manager handles script build dependencies, kind of like a tiny package registry baked into the editor:
- Registry-based, with built-in and optional external packages.
- Installed packages contribute include directories, defines, and link libraries.
- The per-project manifest lives in `packages.modu`.

## 15) Engine Loop (High Level)
From `Engine::run`, the core flow each frame is:
1. Poll events and check for project/scene loads.
2. Update viewport focus and camera controls.
3. Update scripts (Play/Spec/Test only).
4. Update the player controller + 2D physics + camera follow.
5. Update animations and world transforms.
6. Step the active 3D physics backend if it's enabled and running.
7. Render the editor and game views.

This ordering is what keeps your edit-time changes isolated from the running simulation.

## 16) Extending the Engine
Common places to hook in your own stuff:
- Add new components in `SceneObject.h` + serialization in `ProjectManager.cpp`.
- Add new rendering features in `Rendering.*`.
- Add new script APIs in `ScriptRuntime.*`, and managed bindings in `ManagedBindings.*`.
- Add custom editor windows via script exports.

## 17) References in This Repo
- `docs/Scripting.md`: full scripting guide and API details.
- `docs/ModuCPP_Language_Reference.md`: the ModuCPP language reference.
- `docs/Build.md`: build flags, CPU/ISA targets, and release packaging.
- `src/Engine.*`: main loop and editor control.
- `src/SceneObject.h`: component definitions.
- `src/Rendering.*`: renderer and post-processing.
- `src/PhysicsSystem.*`: the engine-facing physics layer (backends live in `JoltPhysicsBackend.*`, PhysX, and `PhysicsBackendFactory.*`).
- `src/AudioSystem.*`: audio playback and reverb.
- `src/ProjectManager.*`: project + scene serialization.
