# NOTE: This ModuCPP folder is made for AI Agents in mind!
## see `https://moduengine.xyz/docs` for a more format correct version.

# ModuCPP Documentation
## Overview
ModuCPP is the scripting layer used by Modularity for gameplay, tool, and inspector-facing logic. You write scripts in a C#-like syntax, but the scripting system does not run on a separate managed gameplay VM. Instead, the ModuCPP transpiler lowers supported script syntax into generated `C++`, and the result is compiled into the same native runtime as the rest of the engine.

That design shapes the way the language feels:
- scripts read like high-level engine code rather than raw `C++`
- the runtime stays native and engine-facing
- the supported syntax is intentional rather than open-ended
- imports are explicit, so each module exposes a clear area of responsibility

If you are approaching ModuCPP from Unity, Unreal Blueprints, or plain `C++`, it helps to think of it as a focused gameplay language layered on top of the existing engine. It is not trying to be a general-purpose programming environment. It exists to make game and editor scripts easier to author, inspect, and ship inside this codebase.

## Why ModuCPP Exists
Modularity already has a native runtime, native scene objects, native editor systems, and native gameplay helpers. Writing every small gameplay behavior directly in low-level engine code is possible, but it pushes everyday scripting work into a slower and more repetitive workflow.
ModuCPP exists to solve that gap.

Instead of forcing every gameplay script to manage native boilerplate, the language gives you:
- a small set of recognized base classes such as `ModuNode`
- declarative public fields that map cleanly into inspector and serialization workflows
- well-known lifecycle hooks such as `Begin()` and `Update()`
- module imports that expose only the helper surface you actually need
- a scripting style that is easier to teach, review, and iterate on than handwritten engine glue

That is why the documentation is split into a manual and an API reference. Most scripting problems start with a workflow question such as “Where should this logic live?” or “Which module am I supposed to import?” Once that part is clear, the API pages help you look up exact members and behavior.

## When To Use This Documentation
Use this documentation set in two passes.
Read the manual first when you are still learning how ModuCPP scripts are organized, when to use public fields, how modules are separated, and how runtime hooks behave over time. The manual pages are written as teaching material. They explain what a feature is for before they explain syntax.

Use the API reference when you already know what area of the scripting surface you need and you want details about exact types, helper facades, lifecycle hooks, or module-specific members.
That workflow mirrors professional engine documentation for a reason. A direct type reference is only useful once the higher-level model is already clear.

## How ModuCPP Works
At a high level, the scripting workflow looks like this:
1. You write a `.moducpp` script and import the modules it needs.
2. The transpiler recognizes supported declarations, hooks, fields, attributes, and helper access.
3. The script is lowered into generated `C++` code that matches the engine's native runtime model.
4. The generated code is compiled and used by the runtime/editor like any other native script logic.
This means there are two important constraints to keep in mind.

First, every documented feature must correspond to something the transpiler and runtime actually support. The docs in this section intentionally avoid inventing “future” language features that are not implemented.

Second, script behavior is grounded in the engine's existing systems. A public field is not magical metadata by itself. It matters because the runtime, inspector generation, and serialization pipeline know how to treat it.

## Modules At A Glance
The module system is one of the most important changes in the current ModuCPP surface.

Older documentation often read as if `add ModuCPP;` brought the whole scripting world into scope. That is no longer the right mental model. `ModuCPP` is the core module. Other engine areas are imported separately so scripts can stay explicit about what they rely on.

| Import | What it is for | Typical use |
| --- | --- | --- |
| `add ModuCPP;` | Core script base names, object/state facades, timers, math/value helpers, script context | Nearly every gameplay script |
| `add ModuEngine;` | Engine-facing helpers such as object utilities, editor widgets, movement/audio helpers, project/runtime helpers | Gameplay scripts that touch more engine systems |
| `add ModuInput;` | Input constants, direct key polling, directional input helpers | Character control, UI navigation, debug controls |
| `add RMeshBuilder;` | Reserved mesh-builder import | Future-facing or placeholder usage only |
| `add ModuCPP.Experimental;` | Experimental helpers such as object-reference serialization and extra UI/editor helpers | Tools, prototypes, unstable feature work |

The dedicated module pages go deeper on this split. The main rule is simple: import the smallest stable surface that solves the problem you have right now.

## Suggested Reading Path
If you are new to the language, read in this order:

1. [Getting Started](manual/getting-started.md)
2. [Script Structure](manual/script-structure.md)
3. [Imports and Modules](manual/imports-and-modules.md)
4. [Fields and Inspector](manual/fields-and-inspector.md)
5. [Methods and Lifecycle](manual/methods-and-lifecycle.md)
6. [Input and Engine Scripting](manual/input-and-engine-scripting.md)
7. [Common Patterns](manual/common-patterns.md)

After that, move into the API pages for the modules and types you use most often.

## Documentation Structure
### Manual
The manual pages teach how to use ModuCPP in practice.

- [Manual Index](manual/README.md)
- [Getting Started](manual/getting-started.md)
- [Script Structure](manual/script-structure.md)
- [Imports and Modules](manual/imports-and-modules.md)
- [Fields and Inspector](manual/fields-and-inspector.md)
- [Methods and Lifecycle](manual/methods-and-lifecycle.md)
- [Input and Engine Scripting](manual/input-and-engine-scripting.md)
- [Editor Scripting](manual/editor-scripting.md)
- [Experimental Features](manual/experimental-features.md)
- [Common Patterns](manual/common-patterns.md)
- [Transpilation Overview](manual/transpilation.md)
- [Writing Clean Scripts](manual/writing-clean-scripts.md)
- [Troubleshooting](manual/troubleshooting.md)

### API Reference
The API pages describe the script-facing types, modules, hooks, and helper surfaces in detail.

- [API Reference Index](api/README.md)
- [ModuNode and ModuBehaviour](api/type-modunode-and-modubehaviour.md)
- [SceneObj](api/type-sceneobj.md)
- [ScriptContext](api/type-scriptcontext.md)
- [Facades and Helper Types](api/type-facades-and-helpers.md)
- [Managed Bridge (`ModuCPP.cs`)](api/managed-bridge.md)
- [Inspector Reference](api/inspector-reference.md)
- [Hook Reference](api/hook-reference.md)
- [Module: ModuCPP](api/module-moducpp.md)
- [Module: ModuEngine](api/module-moduengine.md)
- [Module: ModuInput](api/module-moduinput.md)
- [Module: RMeshBuilder](api/module-rmeshbuilder.md)
- [Module: ModuCPP.Experimental](api/module-moducpp-experimental.md)
- [Standalone Movement API](api/type-standalone-movement.md)
- [DialoguePort Namespace](api/namespace-dialogueport.md)
- [Shipped Example Types](api/type-shipped-example-types.md)

## Scope
This documentation set covers the current script-facing language and helper surface that can be traced in the repository:

- the modular import system
- supported script class shapes and script markers
- public/private field behavior and inspector-facing attributes
- lifecycle and editor hooks recognized by the runtime/transpiler
- script helper facades exposed by the native headers in `include/`
- example-script data patterns that are important enough to document as real usage

It does not treat unfinished ideas as shipped language features. If a surface exists but is partial, unstable, or clearly intended for experimentation, the docs call that out explicitly.

## Notes
- Prefer the manual pages when learning a new area. They explain intent, tradeoffs, and typical workflows.
- Prefer the API pages when you already know the area and need exact names, members, or module requirements.
- Treat `ModuCPP.Experimental` as opt-in and unstable unless a page says otherwise.
- Treat `RMeshBuilder` as reserved until it exposes real script-facing helpers in the repository.

## Related Pages
- [Scripting Overview](../Scripting.md)
- [Modularity Engine Documentation](../Modularity.md)
- [Manual Index](manual/README.md)
- [API Reference Index](api/README.md)
