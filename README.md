<div align="center">
  <a href="https://www.moduengine.xyz/">
    <img src="https://pak.moduengine.xyz/Tareno-Labs-LLC/Modularity/media/branch/main/Resources/Modularity%20is%20a%20modular%20game%20engine%20with%20a%20package-based%20system%2C%20giving%20developers%20full%20control%20over%20features%2C%20tools%2C%20and%20workflows.%20Engine-Root/Modularity%20Full%20Logo.png" alt="Modularity Engine logo" width="650">
  </a>
  <p>
    <a href="https://www.moduengine.xyz/"><img src="https://img.shields.io/badge/ModuEngine-7c6aef?style=for-the-badge" alt="Visit the ModuEngine website"></a>
    <a href="https://www.moduengine.xyz/docs"><img src="https://img.shields.io/badge/Engine_Handbook-6554c0?style=for-the-badge" alt="Read the engine handbook"></a>
    <a href="https://www.moduengine.xyz/docs/getting-started"><img src="https://img.shields.io/badge/Getting_Started-4f8f6f?style=for-the-badge" alt="Open the getting started guide"></a>
    <a href="https://pak.moduengine.xyz/Tareno-Labs-LLC/Modularity/issues"><img src="https://img.shields.io/badge/Report_a_Bug-b56576?style=for-the-badge" alt="View the issue tracker"></a>
  </p>
</div>

<table>
  <tr>
    <td width="44%" valign="middle">
      <img src="https://pak.moduengine.xyz/Tareno-Labs-LLC/Modularity/media/branch/main/Resources/Engine-Root/Modularity%20Logo%20Front%20with%20BG.png" alt="Modularity Engine logo over an editor background" width="100%">
    </td>
    <td width="56%" valign="middle">
      <h2>Welcome!</h2>
      <p>Modularity (also known as <strong>ModuEngine</strong>) is a custom game engine built around a simple idea: your tools should adapt to your project, not the other way around. It combines a native C++ runtime, a built-in editor, a package-based workflow, and the friendly ModuCPP scripting layer so you can move from an idea to a playable build without stitching together a dozen separate tools.</p>
      <p>The project is under active development. There are polished, useful systems here today, but there are also experimental areas and a few rough edges. We would rather be clear about those than pretend otherwise.</p>
      <p>Feedback, bug reports, documentation fixes, and any focused code contributions are all genuinely welcome.</p>
    </td>
  </tr>
</table>

<p align="center">
  <img src="https://www.moduengine.xyz/hero-editor.png" alt="Modularity Engine editor showing project settings, the scene hierarchy, inspector, and asset browser" width="100%">
</p>

<div align="center">
  <br><strong><h1>What can you build with it?<h1></strong>
</div>
<table>
  <tr>
    <td width="50%">
      <img src="https://www.moduengine.xyz/hero-landscape.png" alt="A 2D battle scene running inside the Modularity editor" width="100%">
      <br><strong>2D games and UI-heavy experiences</strong>
    </td>
    <td width="50%">
      <img src="https://www.moduengine.xyz/hero-3d-scene.png" alt="A 3D game scene running inside the Modularity editor" width="100%">
      <br><strong>3D worlds and mixed 2D/3D projects</strong>
    </td>
  </tr>
</table>

Modularity includes:
- **An integrated editor** with a scene hierarchy, inspector, asset browser, project settings, viewport gizmos, build tools, profiling views, and reusable layouts.
- **2D and 3D rendering** through OpenGL, including model and texture loading, skyboxes, custom shaders, render targets, and post-processing such as bloom, color adjustment, motion blur, vignette, chromatic aberration, and ambient occlusion.
- **ModuCPP scripting**, the recommended high-level gameplay layer. ModuCPP scripts are transpiled to native C++, expose public fields in the inspector, and can be compiled from inside the editor.
- **Lower-level scripting options** through native C++, a C runtime bridge, and experimental managed C# support through Mono.
- **Physics for different project shapes** with Jolt as the default 3D backend, optional PhysX support, and a lightweight built-in 2D simulation.
- **Animation tools** for transform keyframes and imported skeletal animation, with optional GPU skinning.
- **Audio tooling** powered by miniaudio, including spatial playback, looping, preview controls, rolloff, and reverb zones.
- **A component-style scene system** for cameras, lights, renderers, rigidbodies, colliders, sprites, UI, scripts, audio, animation, and post-processing.
- **Package and export workflows** for script dependencies, `.modupak` content, editor builds, standalone players, Windows cross-builds, and Android APKs.

<table>
  <tr>
    <td width="50%">
      <img src="https://www.moduengine.xyz/hero-3d-editor.png" alt="The Modularity keyframe animation editor and object inspector" width="100%">
      <br><strong>Animate objects and UI on a keyframe timeline</strong>
    </td>
    <td width="50%">
      <img src="https://www.moduengine.xyz/hero-sprite-editor.png" alt="The Modularity pixel sprite editor editing a character sprite sheet" width="100%">
      <br><strong>Edit pixel art and sprite sheets without leaving the engine</strong>
    </td>
  </tr>
</table>

## How a typical project comes together
1. **Create or open a project** from the launcher. Project content lives mostly under `Assets/`, while generated data stays under `Library/`.
2. **Build a scene** by adding objects in the Hierarchy and editing components in the Inspector. Scenes are stored as readable `.scene` files, which makes them friendlier to version control.
3. **Import your assets**: models, textures, audio, shaders, sprites, and scripts through the asset browser.
4. **Add behavior** with ModuCPP or one of the lower-level scripting surfaces. Scripts can be compiled from the file browser or a script component in the Inspector.
5. **Press Play, Spec, or Test** to run scripts and simulations without mixing runtime changes into normal edit mode.
6. **Build a standalone player or APK** through the editor's build settings or the command-line build workflow.

Here is a small ModuCPP script that updates a UI text object every frame:
```cpp
add ModuCPP;
add ModuEngine;
public class FPSDisplay : ModuNode {
    public string prefix = "FPS: ";
    void TickUpdate() {
        obj.UILabel = prefix + IntR(ModuEngine.FPS);
    }
}
```

Save scripts under your project's `Assets/Scripts/` directory, attach them to a scene object, and compile them in the editor. Public fields such as `prefix` are persisted and exposed in the Inspector automatically.

For a deeper tour, visit the [Engine Handbook](https://www.moduengine.xyz/docs), read the repository's [engine overview](docs/Modularity.md), or jump into the [ModuCPP manual](docs/moducpp/manual/README.md).

## Build and run
### Linux
The build script checks system dependencies, syncs submodules and Git LFS content, configures CMake, builds the editor and standalone player, and creates a package:
```bash
git clone --recurse-submodules https://pak.moduengine.xyz/Tareno-Labs-LLC/Modularity.git
cd Modularity
git lfs install
git lfs pull
./build.sh
./build/Modularity
```

To open an existing project directly:
```bash
./build/Modularity --project /path/to/project.modu
```

Useful development builds include:
```bash
./build.sh --clean
./build.sh --build-type=Debug
./build.sh --build-type=Debug --fsanitize
./build.sh --Windows
./build.sh --Android --project=/path/to/project.modu
```

### Windows
From a developer command prompt with Git, Git LFS, CMake, and the Visual Studio C++ toolchain available:
```bat
git submodule update --init --recursive
git lfs install
git lfs pull
build.bat
```

The usual Release outputs are `build\Release\Modularity.exe` for the editor and `build\Release\ModularityPlayer.exe` for the standalone runtime.
See [docs/Build.md](docs/Build.md) for build flags, Android requirements, packaging, CPU compatibility, and release verification details.

## Project status and known limitations
Modularity is usable, but it is still growing quickly. These are the important expectations to set before you dive in:
| Area | Current status |
| --- | --- |
| Automated tests | There is no automated test suite yet. Changes are currently verified by building and running both the editor and the standalone player. |
| Vulkan | The Vulkan renderer is experimental. OpenGL is the established rendering path. |
| Managed C# | Mono-backed C# scripting is experimental and may be unavailable when Mono is not installed or on builds that disable it. ModuCPP is the recommended scripting surface. |
| Android | Player APK builds are supported, while the editor APK and on-device script compilation remain experimental. `arm64-v8a` is the routinely used ABI, and whole-folder import is not currently available in the Android editor. |
| Windows cross-builds | Linux-to-Windows MinGW builds disable Mono, PhysX, Vulkan, sndfile, and opusfile unless matching Windows-target dependencies are wired in. Native Windows builds have a different feature path. |
| Helper launcher | `buildandrun.sh` still targets the legacy `build/main` executable name. Until it is updated, run `./build.sh` followed by `./build/Modularity`. |
| APIs and file formats | Engine and experimental scripting APIs can still evolve. Keep changes focused and call out compatibility or serialization changes in your pull request. |

If you run into something not listed here, please [open an issue](https://pak.moduengine.xyz/Tareno-Labs-LLC/Modularity/issues). A small reproducible project, crash report, screenshot, or log excerpt can save a lot of guesswork.

## Contributing
You do not need to arrive with a huge engine feature. Fixing a typo, improving a confusing error message, testing a different machine, documenting an edge case, or reducing a reliable crash is valuable work.

### A friendly contribution workflow
1. Check the [open issues](https://pak.moduengine.xyz/Tareno-Labs-LLC/Modularity/issues) or open a discussion issue before starting a large architectural change.
2. Fork the repository and create a focused branch from `main`.
3. Build once before changing anything so you know your local toolchain and dependencies are healthy.
4. Make the smallest coherent change that solves the problem. Follow nearby naming and code style, and reuse the existing renderer, editor, scene, scripting, serialization, asset, and audio systems.
5. Avoid changing files under `src/ThirdParty/` unless the contribution is specifically about that dependency.
6. Verify the editor and player. For risky native-code changes, a Debug build with `--fsanitize` is strongly encouraged on supported platforms.
7. Open a [pull request](https://pak.moduengine.xyz/Tareno-Labs-LLC/Modularity/pulls) explaining what changed, why it changed, how you tested it, and any known tradeoffs or follow-up work.

### Before opening a pull request. Please ensure that:
- [ ] The change is focused and does not include unrelated formatting or generated build output.
- [ ] The editor builds and launches.
- [ ] The standalone `ModularityPlayer` still builds, loads scenes, and runs scripts where relevant.
- [ ] New behavior is documented, including experimental status or known limitations.
- [ ] Scene, material, project, package, or script serialization changes have been checked for compatibility.
- [ ] Engine modifications comply with [the project license](LICENSE.md).

### Reporting a bug
Please include as much of the following as you can:
- Your operating system, compiler/toolchain, GPU, and the commit or build you used.
- Clear reproduction steps and what you expected to happen.
- What actually happened, including the full error message.
- Relevant console output or files from `CrashReports/`.
- A minimal project or scene when the issue depends on project content.
- Screenshots or a short recording for editor and rendering issues.
One reminder: No report has to be perfect. If you can reproduce the problem but are unsure where it lives, no worries! You can open an issue anyway and say what you already tried.

## Repository guide
| Path | What lives there |
| --- | --- |
| `src/` and `include/` | Engine runtime, renderer, editor, platform, scene, physics, audio, and scripting code |
| `Scripts/` | Shipped ModuCPP examples and engine-side script samples |
| `Resources/` | Shaders, textures, fonts, sounds, editor resources, and other runtime assets |
| `docs/` | Build, engine architecture, platform, scripting, and ModuCPP documentation |
| `cmake/` and `CMakeLists.txt` | Build configuration and platform feature switches |
| `tools/` | Build and release support utilities |
| `redist/` | Redistributable runtime files used by packaged builds |

## License
Modularity is distributed under the [Tareno-Labs Community Use License 1.2](LICENSE.md). In short, you may build commercial or closed-source games and applications with the engine, create marketplace content, and modify the engine. If you distribute a modified version of the engine itself, its corresponding source must remain available under the same license, and your modifications must be identified.
The summary above is not legal advice; the full license text is authoritative.

---

<div align="center">
  ❤️ Built with care by Tareno Labs™ and the Modularity community. ❤️<br>
  Questions, experiments, bug reports, and first-time contributions are welcome here.
</div>
