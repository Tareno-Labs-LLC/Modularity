# Scripting (ModuCPP, C++, C, and C#)
Modularity supports four scripting layers:
| Layer | Primary use | Main docs |
| --- | --- | --- |
| `ModuCPP` | High-level gameplay, UI, editor, and runtime scripting | [ModuCPP Overview](moducpp/README.md) |
| Native `C++` | Lower-level engine-adjacent scripts and direct `ScriptContext` work | [ModuCPP API: ScriptContext](moducpp/api/type-scriptcontext.md) |
| Native `C` | Small bridge-style scripts through `ScriptRuntimeCAPI.h` | See `include/ScriptRuntimeCAPI.h` and the C sections in older revisions of this page |
| Managed `C#` | Mono-hosted scripts using `Scripts/Managed/ModuCPP.cs` | [Mono Embedding Setup](mono-embedding.md) |
`ModuCPP` is the recommended scripting layer for new scripts. It is transpiled to native `C++`, so it keeps native runtime behavior while providing higher-level syntax, generated inspectors, and module-based imports.

## ModuCPP Documentation
Start here when you are writing or maintaining `.moducpp` scripts:
- [ModuCPP Overview](moducpp/README.md)
- [Manual Index](moducpp/manual/README.md)
- [API Reference Index](moducpp/api/README.md)

Recommended reading order:
1. [Getting Started](moducpp/manual/getting-started.md)
2. [Script Structure](moducpp/manual/script-structure.md)
3. [Imports and Modules](moducpp/manual/imports-and-modules.md)
4. [Fields and Inspector](moducpp/manual/fields-and-inspector.md)
5. [Methods and Lifecycle](moducpp/manual/methods-and-lifecycle.md)

## Quick Notes
- `add ModuCPP;` gives you the core language helpers. It does not automatically import `ModuEngine`, `ModuInput`, `RMeshBuilder`, or `ModuCPP.Experimental`.
- `ModuNode` is the preferred high-level script base for new gameplay scripts.
- `ModuBehaviour` is still supported for older scripts and native-style ports.
- Public ModuCPP fields are persisted and exposed in the inspector unless you override them with `Script_OnInspector()` or `inspector { ... }`.
- Private ModuCPP fields are runtime-only state.

## Native and Managed Bridges
These areas are still supported, but they are separate from the new module-split ModuCPP manual/reference set:
- Native `C++` scripts use `ScriptContext`, `MODU_SCRIPT(ctx)`, and the helper headers under `include/`.
- Native `C` scripts use `include/ScriptRuntimeCAPI.h` and `Modu_*` exports.
- Managed `C#` scripts use `Scripts/Managed/ModuCPP.cs`, `ModuCPP.Context`, and `ModuCPP.ImGui`.

For current managed runtime layout, ABI versioning, and build notes, use:
- [Mono Embedding Setup](mono-embedding.md)
- [Managed Bridge (`ModuCPP.cs`)](moducpp/api/managed-bridge.md)

## Examples in This Repo
- `Scripts/FPSDisplay.moducpp`
- `Scripts/TopDownMovement2D.moducpp`
- `Scripts/StandaloneMovementController.moducpp`
- `Scripts/MainMenuController.moducpp`
- `Scripts/DialogueSystem.moducpp`
- `Scripts/InteractableObject.moducpp`

## Related Pages
- [Modularity Engine Documentation](Modularity.md)
- [Mono Embedding Setup](mono-embedding.md)
- [ModuCPP Overview](moducpp/README.md)
