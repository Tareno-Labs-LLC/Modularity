# Module: ModuCPP

## Summary
`ModuCPP` is the core high-level scripting module.

It provides the authoring foundation used by almost every script in the repository:

- type aliases
- automatic access to `ctx`, `obj`, and frame timing
- `Config<T>()` and `State<T>()`
- `BindSetting(...)`, `BindArray(...)`, and `BindArray2D(...)`
- timer helpers
- math and integer-formatting helpers
- `SubScript` serialization helpers

If `ModuInput` and `ModuEngine` are "intent" and "response" layers, `ModuCPP` is the structure layer they sit on top of.

## Syntax
```cpp
add ModuCPP;
```

## Why This Module Exists
The core module solves several very common script-authoring problems:

- how to write readable high-level types
- how to separate persisted config from runtime state
- how to keep small timed logic from becoming boilerplate
- how to store nested authored data

The repository scripts use all of these patterns extensively, so this module is not optional background theory. It is the basis of how real ModuCPP scripts are structured.

## Types And Aliases

### `vec2`, `vec3`, `Vector2`, `Vector3`, `string`
These are the standard high-level aliases used throughout the docs and repository examples.

They exist so a script can read like gameplay logic instead of C++-heavy scaffolding.

## Automatic Access Helpers

### `ctx`
The current `ScriptContext` is injected into high-level hooks automatically.

### `obj`
The current object facade is also injected, making current-object code shorter and easier to read.

### `time.deltaTime`
Frame delta time is available through the time facade, and update-style hooks also typically expose the shorter `dt`.

These helpers are part of why high-level ModuCPP scripts stay small.

## `Config<T>()` And `State<T>()`

### Overview
These two helpers are among the most important authoring tools in the module.

They solve the core script-structure question:

- what data should persist as authored configuration?
- what data should only exist while the script is running?

### `Config<T>()`
Use this for persisted structured configuration.

Repository uses:

- `SampleInspector Simplified.moducpp`
- `RigidbodyTest.moducpp`
- `StandaloneMovementController.moducpp`

Why a script author chooses it:

- the config is logically a struct
- the script is manual-inspector-heavy
- top-level public fields would be noisier than grouped config

### `State<T>()`
Use this for runtime-only structured state.

Repository uses:

- warning flags in tool scripts
- standalone movement runtime state and debug data

Why a script author chooses it:

- the state does not belong in saved scene data
- the runtime behavior needs grouped transient memory

### Important Behavior
`Config<T>()` and `State<T>()` provide storage. They do not automatically bind individual config fields to saved settings values by themselves.

That is why repository scripts still use binding helpers such as:

```cpp
void bindConfig(ScriptContext& ctx, SampleInspectorSimpleConfig& config) {
    BindSetting(ctx, "autoRotate", config.autoRotate);
    BindSetting(ctx, "spinSpeed", config.spinSpeed);
    BindSetting(ctx, "offset", config.offset);
    BindSetting(ctx, "targetName", config.targetName);
}
```

### Example: Manual Inspector Config
```cpp
add ModuCPP;
add ModuEngine;

public class SampleInspectorSimplified : ModuBehaviour
{
    void Script_OnInspector()
    {
        auto& config = Config<SampleInspectorSimpleConfig>();
        bindConfig(ctx, config);

        bool changed = false;
        changed |= ImGui::Checkbox("Auto Rotate", &config.autoRotate);
        if (changed) {
            ctx.SaveAutoSettings();
        }
    }
}
```

### Example: Runtime State
```cpp
auto& state = State<RigidbodyTestState>();
if (!ctx.SetRigidbodyVelocity(vec3(0.0f))) {
    warnOnce(ctx, state.warnedMissingRb, "RigidbodyTest: zeroing velocity requires a Rigidbody");
}
```

## `BindSetting(...)`

### Overview
`BindSetting(...)` connects a config value to the script settings path.

### Why It Exists
When a script stores config in `Config<T>()` instead of public fields, individual fields still need to participate in persistence.

That is what `BindSetting(...)` is for.

### When To Use It
Use it when:

- configuration lives in a custom struct
- a manual inspector or editor window edits the value
- runtime hooks need the same saved config values

### Important Note
Bind config in every hook that reads it, not only in the inspector. This is one of the main patterns visible in the repository.

## `BindArray(...)` And `BindArray2D(...)`

### Overview
These are array-shaped versions of the same persistence idea.

### Why They Exist
Some authored data is naturally stored as fixed-size clip grids or structured arrays, such as the directional clip data patterns visible in movement examples.

Use them when a custom config struct contains fixed-size arrays that should be bound systematically rather than field by field.

## Timer Helpers

### Members
- `SetFrameDeltaTime(...)`
- `StartTimer(...)`
- `TimerReady(...)`
- shorthand `timer.Start(...)` and `timer.Ready()`

### Why They Exist
Periodic behavior is common and should not require each script to rebuild the timing logic.

### What Problem They Solve
Without these helpers, a simple repeating task would need:

- elapsed accumulation
- interval comparison
- wraparound or reset logic

### Repository Pattern
```cpp
public float interval = 1.0f;
private float timer = 0.0f;

void Begin() to timer.Start(interval);

void TickUpdate()
{
    if (!timer.Ready()) return;
    ctx.AddConsoleMessage("Pulse");
}
```

### Important Notes
- Store timers as runtime state.
- The helper is ideal for repeating behavior.
- Pair it with `Begin()` for clean lifecycle design.

## Math And Formatting Helpers

### `Math::Max`, `Math::Min`, `Math::Clamp`, `Math::Abs`
These are used repeatedly in repository scripts for:

- clamping indices
- enforcing minimum speeds or timings
- selecting directional motion
- keeping distances or delays non-negative

### `IntRD(...)`, `IntR(...)`, `IntRU(...)`
These exist for clean integer-style presentation in UI and debug text.

Repository example:

```cpp
obj.UILabel = "FPS: " + IntR(ModuEngine.FPS);
```

## `SubScript` Serialization Helpers

### Members
- `SerializeSubScript(...)`
- `DeserializeSubScript(...)`
- `SerializeSubScriptArray(...)`
- `DeserializeSubScriptArray(...)`
- `EditSubScript(...)`
- `EditSubScriptArray(...)`

### Why They Exist
Many real scripts need nested authored data, not just flat fields.

Repository examples:

- `MenuAction[]` in `MainMenuController.moducpp`
- `InteractionOption[]` in `InteractableObject.moducpp`

Those scripts need nested arrays that still serialize cleanly and stay editable in the inspector. That is the job of the sub-script helpers.

### Example
```cpp
SubScript MenuAction {
    public SceneObj[] enable;
    public SceneObj[] disable;
};
```

### Why A Script Author Would Choose This
Because it is better than maintaining several parallel arrays that must stay in sync manually.

## What This Module Looks Like In Real Scripts
The repository tends to use `ModuCPP` in one of two ways:

1. public fields plus private runtime fields in a normal gameplay component
2. `Config<T>()` plus `State<T>()` in a manual inspector or subsystem wrapper

Both approaches are part of the core module’s intended usage.

## Common Mistakes
- Using `Config<T>()` without binding the fields.
- Treating runtime-only state as persisted config.
- Reimplementing timer logic manually when the timer helpers already fit the job.
- Flattening nested authored data instead of using `SubScript`.

## Related APIs
- [Facades and Helper Types](type-facades-and-helpers.md)
- [Module: ModuEngine](module-moduengine.md)
- [Module: ModuInput](module-moduinput.md)
- [Fields and Inspector](../manual/fields-and-inspector.md)
