# Getting Started with ModuCPP

## Overview
ModuCPP is the recommended scripting layer for new Modularity gameplay scripts. You write a `.moducpp` file with a small set of high-level rules, the build pipeline transpiles that file to native `C++`, and the result runs through the same native script runtime as hand-written `C++` scripts.

That design matters. ModuCPP is not a separate virtual machine, and it is not a simplified toy language sitting next to the engine. It exists so you can write common gameplay and editor logic with less boilerplate while still keeping the native runtime model, native data structures, and the existing script loading pipeline.

If you already understand the engine but do not want to write raw `ScriptContext` code for every script, ModuCPP is the layer you should start with.

## Why This Feature Exists
Raw native scripting gives you full control, but simple scripts quickly become repetitive:

- you need to wire hook signatures yourself
- you need to bind inspector state manually
- you need to decide what should persist and what should stay runtime-only
- common tasks such as timers, FPS labels, object lists, and simple input polling need helper code every time

ModuCPP exists to solve those problems without changing the runtime model. The transpiler takes care of the repetitive structure so the script can focus on behavior.

## When to Use It
Use ModuCPP when you are writing:

- gameplay behaviors attached to scene objects
- UI behaviors such as labels, menus, and interaction prompts
- small to medium systems that benefit from inspector-driven configuration
- editor-facing tools that still fit into the normal script workflow

Use raw native `C++` instead when:

- you are extending the engine-side runtime itself
- you need a scripting feature the high-level syntax does not expose cleanly
- you are porting a native script that already uses low-level runtime APIs directly

## How It Works
The basic workflow is straightforward:

1. Create a script file under your project script folder, usually `Assets/Scripts/`.
2. Import the modules you need with `add ...;`.
3. Declare a high-level script class such as `public class MyScript : ModuNode`.
4. Add public fields for data you want exposed in the inspector and saved with the script.
5. Add private fields for runtime-only state.
6. Implement lifecycle hooks such as `Begin()` and `TickUpdate()`.
7. Compile the script through the editor or build flow.

At runtime, the script still receives the native `ScriptContext`, but high-level hook bodies already have `ctx`, `obj`, and `dt` prepared for you.

## A Small First Script
The simplest useful script is usually one that reacts every frame and updates something visible.

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

This example is small, but it demonstrates the core idea:

- `add ModuCPP;` gives you the core language helpers
- `add ModuEngine;` gives you the FPS facade
- `obj.UILabel` writes to the current object's UI label through the script-facing object facade
- `IntR(...)` turns the frame FPS into a readable whole number

The script does not need to set up a custom hook signature, calculate FPS manually, or expose any inspector logic because the task is simple.

## A Better Starter Script
Most real scripts want at least a small amount of configuration. Public fields are where ModuCPP becomes more practical than a one-off native script.

```cpp
add ModuCPP;
add ModuEngine;

public class FPSDisplay : ModuNode
{
    public bool clampTo120 = false;
    public string prefix = "FPS: ";

    void Begin()
    {
        if (clampTo120) {
            ctx.SetFPSCap(true, 120.0f);
        }
    }

    void TickUpdate()
    {
        obj.UILabel = prefix + IntR(ModuEngine.FPS);
    }
}
```

This version solves a more realistic problem:

- designers can change the label prefix in the inspector
- the script can optionally clamp the runtime to 120 FPS
- setup happens once in `Begin()`
- display logic happens every frame in `TickUpdate()`

In practice, this is the shape many real ModuCPP scripts follow: a few public fields for configuration, a small amount of setup, and a clear per-frame behavior.

## Another Common First Script: Repeating Behavior
Many first gameplay scripts need to do something over time. ModuCPP’s timer shorthand exists for exactly that case.

```cpp
add ModuCPP;

public class PulseLogger : ModuNode
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

This is the kind of script that would be more awkward in a raw one-frame-only mental model. `timer.Start(...)` stores the interval, and `timer.Ready()` advances using the current frame delta time until one interval has elapsed.

## What Actually Happens
Behind the scenes, the transpiler turns high-level constructs into native glue:

- the class becomes generated native support code
- public fields are placed into persisted config storage
- private fields are placed into runtime state storage
- hooks such as `Begin()` and `TickUpdate()` become native exports used by the script runtime

The important consequence is that ModuCPP should be thought of as authoring syntax, not as a separate runtime environment.

## Practical Use Cases
Typical first-week ModuCPP scripts include:

- an FPS or status label
- a UI button or slider driver
- a pickup or interactable prompt
- a timed enable/disable script
- a simple mover that reads `input.WASDNormalized()`

Those are good early projects because they teach the recurring patterns:

- explicit module imports
- public configuration vs private state
- setup in `Begin()`
- ongoing behavior in `TickUpdate()`

## Best Practices
- Start with `ModuNode` unless you are deliberately maintaining an older `ModuBehaviour` script.
- Import only the modules the script actually uses.
- Put persistent configuration in public fields and transient behavior in private fields.
- Prefer one clear behavior per script when you are learning the workflow.
- Use the shipped scripts in `Scripts/` as reference once the syntax on this page feels familiar.

## Related Pages
- [Script Structure](script-structure.md)
- [Imports and Modules](imports-and-modules.md)
- [Methods and Lifecycle](methods-and-lifecycle.md)
- [Fields and Inspector](fields-and-inspector.md)
