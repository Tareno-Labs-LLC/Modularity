# Imports and Modules

## Overview
ModuCPP uses explicit imports. A script only gets the module surfaces it asks for.

```cpp
add ModuCPP;
add ModuEngine;
add ModuInput;
add ModuCPP.Experimental;
```

That explicit split is one of the most important things to understand in the current documentation set. Older wording sometimes treated `add ModuCPP;` as if it brought in every helper the scripting layer had ever grown. That is no longer a good mental model.

## Why This Feature Exists
As the scripting surface grew, a single catch-all import stopped being useful. Different kinds of scripts need different things:

- a UI label script usually needs the core layer and maybe one engine helper
- a movement script often needs input and engine helpers
- a dialogue or interaction system may need the experimental object-reference and UI text helpers
- some future scripts may want mesh-building tools without also pulling unrelated high-level helpers into the docs

The module split exists so the docs and the scripts can stay honest about what each script depends on.

## When to Use It
Think about imports before you write the rest of the file.

Use:

- `ModuCPP` for almost every high-level script
- `ModuEngine` when the script touches FPS, sprite/audio facades, movement helpers, or engine-oriented inspector helpers
- `ModuInput` when the script polls keys or uses the `input` facade
- `ModuCPP.Experimental` when the script needs advanced object-reference, UI text, or dialogue/menu helper behavior
- `RMeshBuilder` only when you are intentionally targeting that future module surface

## How It Works
Each `add` line maps to a script API header in the transpiler:

- `add ModuCPP;` -> `ModuCPPScriptApi.h`
- `add ModuEngine;` -> `ModuEngineScriptApi.h`
- `add ModuInput;` -> `ModuInputScriptApi.h`
- `add RMeshBuilder;` -> `RMeshBuilderScriptApi.h`
- `add ModuCPP.Experimental;` -> `ModuCPPExperimentalScriptApi.h`

The practical effect is that imports are not just documentation markers. They control which helper surfaces the generated native code includes.

## The Modules in Practice

### `ModuCPP`
`ModuCPP` is the core module. It gives you the high-level authoring foundation:

- `Vector2`, `Vector3`, and `string`
- `ctx`, `obj`, and `time`
- `Config<T>()` and `State<T>()`
- timer helpers
- rounding helpers such as `IntR(...)`
- math helpers
- `SubScript` serialization helpers

Use it when the script is primarily about:

- authoring structure
- persistent config and runtime state
- common script-facing convenience

Example:

```cpp
add ModuCPP;

public class Pulse : ModuNode
{
    public float interval = 0.5f;
    private float timer = 0.0f;

    void Begin() to timer.Start(interval);

    void TickUpdate()
    {
        if (!timer.Ready()) return;
        ctx.AddConsoleMessage("Pulse");
    }
}
```

### `ModuEngine`
`ModuEngine` is where engine-oriented helpers live. This is the module you reach for when the script needs to talk to familiar gameplay helpers instead of writing everything through raw `ScriptContext`.

It includes:

- `ModuEngine.FPS`
- audio facade helpers
- sprite clip helpers
- 2D movement helpers
- inspector editing helpers such as `EditFloat(...)`
- project gravity helpers

Use it when the script is about:

- movement
- UI feedback
- audio feedback
- sprite animation control
- simple engine-facing configuration UIs

Example:

```cpp
add ModuCPP;
add ModuEngine;

public class FPSDisplay : ModuNode
{
    void TickUpdate()
    {
        obj.UILabel = "FPS: " + IntR(ModuEngine.FPS);
    }
}
```

### `ModuInput`
`ModuInput` exists so common key polling stays short and readable. Without it, many gameplay scripts would keep rewriting the same input translation code.

It includes:

- key constants such as `KEY_W` and `KEY_SPACE`
- `KeyDown(...)` and `KeyPressed(...)`
- the `input` facade for movement-style input

Use it when the script needs:

- WASD movement
- simple action keys
- submit/confirm handling
- sprint or jump state polling

Example:

```cpp
add ModuCPP;
add ModuInput;

public class JumpPrompt : ModuNode
{
    void TickUpdate()
    {
        if (input.jump()) {
            ctx.AddConsoleMessage("Jump pressed");
        }
    }
}
```

### `ModuCPP.Experimental`
This module is opt-in on purpose. It contains useful helpers, but they are not the minimal stable core the rest of the docs should assume by default.

It includes:

- object reference serialization helpers
- string parsing helpers
- advanced UI text helpers
- object-ref inspector widgets
- helpers used by the shipped dialogue, menu, and interaction examples

Use it when the script needs:

- `[ObjectRef]` or `[ObjectList]` heavy workflows
- dialogue text targeting
- object lists stored and resolved by string references
- advanced shipped-sample style editor widgets

Example:

```cpp
add ModuCPP;
add ModuCPP.Experimental;

public class ToggleMessage : ModuNode
{
    [ObjectRef] public string labelRef;
    public bool active = false;

    void TickUpdate()
    {
        SetUITextLabel(ctx, labelRef, active ? "Active" : "Idle");
    }
}
```

### `RMeshBuilder`
`RMeshBuilder` is a real import target, but the current script-facing header does not expose an actual helper surface yet.

That means the correct documentation stance is:

- acknowledge that the import exists
- explain that it is reserved
- do not invent mesh-builder scripting APIs that are not present in the repository

## Choosing Imports by Use Case

### A timer or state-only script
Use `ModuCPP` only.

### A movement script
Use `ModuCPP`, `ModuInput`, and usually `ModuEngine`.

### A menu or dialogue script
Use `ModuCPP`, then add `ModuInput`, `ModuEngine`, or `ModuCPP.Experimental` depending on what the script actually does.

### An editor tool window
Use `ModuCPP`, and add `ModuEngine` if the window needs helper widgets or engine-facing helpers.

## What Actually Happens
When the transpiler sees `add ModuInput;`, it does not merely annotate the file. It emits the corresponding include so the generated native script can use that API surface.

That is why you should think of imports as both:

- documentation for the reader
- a build-time contract for the transpiler

## Best Practices
- Import the smallest set of modules that makes the script readable.
- Do not rely on older assumptions that `add ModuCPP;` implies input, engine, or experimental helpers.
- Treat `ModuCPP.Experimental` as explicit dependency information, not as background noise.
- Keep `RMeshBuilder` documentation conservative until the script API header grows a real surface.

## Related Pages
- [Getting Started](getting-started.md)
- [Input and Engine Scripting](input-and-engine-scripting.md)
- [Module: ModuCPP](../api/module-moducpp.md)
- [Module: ModuEngine](../api/module-moduengine.md)
- [Module: ModuInput](../api/module-moduinput.md)
- [Module: ModuCPP.Experimental](../api/module-moducpp-experimental.md)
