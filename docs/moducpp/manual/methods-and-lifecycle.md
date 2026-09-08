# Methods and Lifecycle

## Overview
ModuCPP scripts are driven by recognized hooks. Those hooks are not just naming conventions. They are the places where the runtime or editor gives your script control.

The shipped scripts show three major lifecycle families:

- runtime hooks such as `Begin()` and `TickUpdate()`
- inspector hooks such as `Script_OnInspector()`
- editor-window hooks such as `RenderEditorWindow()`

Understanding which job belongs in which hook is one of the biggest differences between a script that stays readable and a script that turns into one giant mixed-purpose method.

## Why This Feature Exists
A typical script needs several kinds of logic:

- one-time setup
- per-frame behavior
- per-inspector drawing
- editor-only tool drawing

Those jobs have different timing and different responsibilities. Lifecycle hooks exist so you do not need to invent your own structure for every script.

## What High-Level Hooks Provide Automatically
Inside high-level hook bodies, ModuCPP prepares the common script-facing context for you:

- `ctx` is the current `ScriptContext`
- `obj` is the current object facade
- `dt` is the current frame delta time in update-style hooks
- `time.deltaTime` exposes the same value through a named facade

That is why shipped scripts can stay small and direct. They are written as behavior code, not hook-plumbing code.

## When To Use Which Hook
Use:

- `Begin()` for one-time setup or reset
- `TickUpdate()` for frame-driven runtime behavior
- `Update()` mainly for compatibility with older naming
- `Spec()` and `TestEditor()` when the same behavior should also run in those extra modes
- `Script_OnInspector()` for full manual inspector control
- `RenderEditorWindow()` for standalone editor tools
- `ExitRenderEditorWindow()` for tool-window cleanup

## `Begin()`

### Overview
`Begin()` is the one-time initialization hook.

### What It Is Good For
The repository scripts use `Begin()` for:

- starting timers
- resetting runtime state
- applying initial selection visuals
- auto-opening a system once on startup
- ensuring required components exist

Examples:

- `DialogueSystem.moducpp` resets dialogue runtime state in `Begin()`
- `InteractableObject.moducpp` migrates legacy settings and applies selection state in `Begin()`
- `StandaloneMovementController.moducpp` ensures colliders and rigidbodies in `Begin()`

### Typical Pattern
```cpp
void Begin() to timer.Start(interval);
```

Or, for more realistic setup:

```cpp
void Begin()
{
    if (!ctx.object) return;
    ResetRuntimeState();
    applySelectedState();
}
```

### Why It Exists
Without `Begin()`, scripts often fall into a "first frame special case" pattern inside `TickUpdate()`. That is harder to read and easier to break.

### Common Mistakes
- Do not hide permanent setup inside the first `TickUpdate()` unless there is a real reason.
- If the script depends on `ctx.object`, guard it explicitly as the shipped scripts do.

## `TickUpdate()`

### Overview
`TickUpdate()` is the main runtime hook for gameplay behavior.

### What It Usually Contains
This is where the shipped scripts put:

- input polling
- movement
- UI updates
- timer checks
- state-machine progress
- object enable/disable behavior

Examples:

- `TopDownMovement2D.moducpp` reads `input.WASDNormalized()` and moves every frame
- `FPSDisplay.moducpp` updates the label every frame
- `DialogueSystem.moducpp` advances typing, delays, and submit handling every frame
- `MainMenuController.moducpp` advances menu timing and selection every frame

### A Good Mental Model
Think of `TickUpdate()` as the place where you answer:

- what is true this frame?
- what changed this frame?
- what should advance this frame?

### Typical Pattern
```cpp
void TickUpdate()
{
    if (!ctx.object || dt <= 0.0f) return;

    Vector2 move = input.WASDNormalized();
    TryMoveRigidbody2D(ctx, move * speed, acceleration, drag, dt);
}
```

### Why `dt` Guards Matter
Several shipped scripts explicitly return early when `dt <= 0.0f`. That is a good practical pattern for time-based logic, interpolation, or physics helpers that expect positive delta time.

## `Update()`

### Overview
`Update()` is an alternate runtime update hook.

### When To Use It
In current documentation and shipped high-level examples, `TickUpdate()` is the preferred default. Use `Update()` mainly when:

- you are keeping parity with an older script style
- you are maintaining existing code that already uses that hook

The main advice is consistency. New docs and new gameplay samples should normally choose `TickUpdate()`.

## `Spec()` And `TestEditor()`

### Overview
These are extra execution-mode hooks.

### What They Are For
The repository’s `SampleInspector.moducpp` and `SampleInspector Simplified.moducpp` run the same rotation behavior in:

- `Spec()`
- `TestEditor()`
- `TickUpdate()`

That demonstrates the intended use: a behavior can participate in additional project or editor execution modes without mixing those concerns into inspector drawing.

### When To Use Them
Use them when:

- a runtime-like behavior should also run in spec mode
- an editor test mode should preview or validate behavior
- you want the same small runtime function to be reused across multiple hooks

### Good Pattern
```cpp
void Spec() to ApplyAutoRotate();
void TestEditor() to ApplyAutoRotate();
void TickUpdate() to ApplyAutoRotate();
```

This is a clean example of lifecycle design: the hook bodies stay tiny because the actual behavior lives in one reusable helper.

## `Script_OnInspector()`

### Overview
`Script_OnInspector()` is the manual inspector hook.

### What It Solves
Use it when the declarative `inspector { ... }` layout is not enough and the inspector itself needs logic:

- custom buttons
- explicit ImGui controls
- persistent tool settings
- object actions such as "Launch Now" or "Teleport Offset"

Examples in the repository:

- `SampleInspector.moducpp`
- `SampleInspector Simplified.moducpp`
- `RigidbodyTest.moducpp`
- `StandaloneMovementController.moducpp`

### How It Behaves
This hook is not for runtime gameplay. It is for drawing and handling the per-object editor UI.

That distinction is important. `Script_OnInspector()` may edit gameplay-facing configuration, but it is still editor-facing code.

### Practical Pattern
```cpp
void Script_OnInspector()
{
    auto& config = Config<RigidbodyTestConfig>();
    bindConfig(ctx, config);

    bool changed = false;
    changed |= ImGui::Checkbox("Launch on Begin", &config.autoLaunch);
    changed |= ImGui::DragFloat3("Launch Velocity", &config.launchVelocity.x, 0.25f, -50.0f, 50.0f, "%.2f");
    if (changed) {
        ctx.SaveAutoSettings();
    }
}
```

### Why Binding Happens Here And Elsewhere
With `Config<T>()`, the inspector hook is only one consumer of the config. The runtime hooks still need `bindConfig(...)` too. That is why the shipped tool scripts call the same binding helper in multiple hooks.

## `RenderEditorWindow()`

### Overview
`RenderEditorWindow()` is the standalone editor-window hook.

### What It Is For
Use it for tools that are not naturally just one object inspector:

- custom panels
- workflow tabs
- debugging utilities
- simple animation or scene-editing tools

Examples:

- `EditorWindowSample.moducpp`
- `AnimationWindow.moducpp`

### What Makes It Different From `Script_OnInspector()`
`Script_OnInspector()` is object-centric.

`RenderEditorWindow()` is tool-centric.

That difference shapes how you design the UI:

- inspectors explain one script instance
- editor windows provide a workspace or workflow

### Practical Pattern
```cpp
void RenderEditorWindow()
{
    ImGui::TextUnformatted("EditorWindowSample");
    ImGui::Separator();

    if (ImGui::Button("Log Message")) {
        ctx.AddConsoleMessage("Script tab says: " + note);
    }
}
```

## `ExitRenderEditorWindow()`

### Overview
This hook runs when the editor window is closing or leaving its active lifecycle.

### When To Use It
Use it when the tool needs cleanup or state reset.

The repository examples often leave it empty, which is fine. An empty `ExitRenderEditorWindow()` is still a useful statement: the tool has no special cleanup needs.

## The Real Lifecycle Pattern In Shipped Scripts
The repository scripts show a very practical structure:

1. `Begin()` performs setup, migration, or state reset.
2. `TickUpdate()` advances the live behavior.
3. `Script_OnInspector()` or `inspector { ... }` exposes configuration.
4. Editor-only tools use `RenderEditorWindow()` instead of pretending to be runtime components.

That is a healthy separation of concerns and a good default structure for new scripts.

## One-Line `to` Syntax
ModuCPP supports a short `to` form for tiny methods.

```cpp
void Begin() to timer.Start(interval);
int FacingIndex(FacingDirection direction) to (int)direction;
```

### Why It Exists
Some helpers are clearer as a single expression than as a block.

### When To Use It
Use it when:

- the body is truly one expression or one call
- the method is obvious from its name

Do not use it when:

- the logic contains branching
- the method carries meaningful behavior that deserves room to read

## Common Lifecycle Mistakes
- Putting first-run initialization inside `TickUpdate()` when `Begin()` would be clearer.
- Mixing editor tool code into runtime hooks.
- Forgetting to guard `ctx.object` in hooks that depend on an attached object.
- Forgetting that manual inspector persistence needs `BindSetting(...)` and `ctx.SaveAutoSettings()`.
- Mutating scene data in editor tools without `ctx.MarkDirty()`.

## Best Practices
- Put setup and reset logic in `Begin()`.
- Put frame-driven gameplay in `TickUpdate()`.
- Reuse shared helpers across hooks instead of duplicating behavior.
- Keep editor UI in inspector or editor-window hooks, not runtime hooks.
- Prefer `TickUpdate()` for new gameplay examples.

## Related Pages
- [Hook Reference](../api/hook-reference.md)
- [Fields and Inspector](fields-and-inspector.md)
- [Editor Scripting](editor-scripting.md)
- [ScriptContext](../api/type-scriptcontext.md)
