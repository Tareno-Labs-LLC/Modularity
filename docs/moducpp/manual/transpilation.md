# Transpilation Overview

## Overview
ModuCPP is a frontend, not a second runtime. A high-level `.moducpp` file is turned into native `C++`, then compiled and loaded through the same native script path used by lower-level scripts.

That is the right mental model to keep in mind while reading the rest of the docs. ModuCPP makes scripts easier to author, but it does not replace the native engine-side scripting model underneath.

## Why This Feature Exists
The goal of the transpiler is not to invent a separate runtime language. The goal is to remove repetitive native script boilerplate:

- class-like high-level syntax becomes possible
- public/private field behavior becomes predictable
- the inspector can be generated from declared fields
- helper surfaces such as `ctx`, `obj`, and timers can be used naturally

Without that compile-time layer, every script would need more manual glue and more repeated patterns.

## When to Use This
Read this page when you want to understand:

- why public fields persist
- why private fields do not
- how hooks become native exports
- what the module system really controls
- why ModuCPP features can still fail in ways that look like native script behavior

## How It Works
At a high level, the transpiler:

1. reads `add ...;` import lines and maps them to script API headers
2. parses `public class ... : ModuNode` or `ModuBehaviour`
3. collects public and private fields
4. classifies field types and field metadata
5. rewrites persisted fields into generated config storage
6. rewrites private state into generated runtime storage
7. lowers hooks such as `Begin()` and `TickUpdate()` into native script entry points
8. emits the helper setup that makes `ctx`, `obj`, and `dt` available in high-level methods

## The Problem It Solves
Consider a script with:

- a configurable interval
- a runtime timer
- a `Begin()` hook
- a `TickUpdate()` hook

In raw native code, you would need to decide:

- where to store persisted data
- where to store transient state
- how to bind the inspector
- how to export the hook names

In ModuCPP, the transpiler does that repetitive structural work for you.

## Public vs Private
This is the most important lowering rule to understand.

### Public Fields
Public fields are treated as persisted configuration. They become part of generated config storage and are bound through the script settings path.

### Private Fields
Private fields are treated as runtime state. They are not automatically persisted, because they represent behavior in progress rather than authoring-time configuration.

That is why the visibility split is not just style. It changes the generated code.

## Inspector Generation
The inspector path follows the same logic.

- no custom inspector: public fields generate inspector UI automatically
- `inspector { ... }`: the high-level inspector DSL is lowered into native editor code
- `Script_OnInspector()`: your manual inspector replaces the generated one

Again, these are compile-time structural decisions, not runtime magic.

## Example: High-Level Source
```cpp
add ModuCPP;

public class AutoToggle : ModuNode
{
    public float interval = 1.0f;
    private float timer = 0.0f;

    void Begin() to timer.Start(interval);

    void TickUpdate()
    {
        if (!timer.Ready()) return;
        ctx.SetObjectEnabled(!ctx.IsObjectEnabled());
    }
}
```

## What Actually Happens
The generated native shape is conceptually closer to this:

- a generated config type containing `interval`
- a generated state type containing `timer`
- a native inspector binding path for the public field
- a native `Begin(...)` wrapper that sets up the ModuCPP helper context
- a native `TickUpdate(...)` wrapper that advances runtime behavior every frame

The exact generated code is an implementation detail, but the behavior model is stable enough to reason about.

## Why This Matters in Practice
When you hit a problem in ModuCPP, the fix is often easier once you remember that the result is still native script behavior:

- missing module imports behave like missing API surfaces in generated code
- invalid field declarations fail at transpile time
- object setup issues still matter at runtime because the helpers ultimately call into the native engine

The transpiler saves you authoring work, but it does not hide how the engine actually works.

## Best Practices
- Think of ModuCPP as compile-time ergonomics layered on top of native scripting.
- Use the visibility rules intentionally so the generated config/state split matches the real behavior of the script.
- Prefer the declarative inspector path until you need more control.
- Fall back to raw native `C++` when a problem is better solved at the runtime layer than at the authoring-syntax layer.

## Related Pages
- [Script Structure](script-structure.md)
- [Fields and Inspector](fields-and-inspector.md)
- [Methods and Lifecycle](methods-and-lifecycle.md)
- [Module: ModuCPP](../api/module-moducpp.md)
