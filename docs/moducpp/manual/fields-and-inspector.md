# Fields and Inspector

## Overview
In ModuCPP, field design is part of script design.

The repository scripts use two main authoring styles:

1. public fields plus automatic or declarative inspector generation
2. `Config<T>()` and `State<T>()` plus a manual inspector or editor window

If you understand when to use each style, the rest of the inspector system becomes much easier to reason about.

## Why This Matters
Most scripts contain two different categories of data:

- authored configuration that a designer or tool user should edit
- runtime state that only exists while the script is executing

Good ModuCPP scripts keep those categories separate.

That separation is visible all through the repository:

- `DialogueSystem.moducpp` keeps authored dialogue fields public and typing state private
- `InteractableObject.moducpp` keeps selection configuration public and previous-input booleans private
- `SampleInspector Simplified.moducpp` stores persisted tool configuration in `Config<T>()` and runtime-only details elsewhere

## Public Fields: The Normal Authoring Path

### What Public Fields Are For
Public fields are the standard persisted script configuration path.

Use them when:

- the value belongs in the inspector
- the value should persist with the script instance
- the field shape already matches the authored data you want

Common public field shapes used in the repository include:

- `bool`, `int`, `float`
- `string`
- `Vector2`, `Vector3`, `vec3`
- enums
- `string[]`
- fixed-size arrays such as `int[4]` and `string[6]`
- `SubScript[]`
- `DialoguePort.DialogueLine[]`

### Why Public Fields Are Usually The Best Default
They are the clearest option when the script is a normal gameplay component:

- the field declaration shows the editable configuration immediately
- the inspector can be generated automatically
- the docs and the scene data stay aligned

### Example
```cpp
add ModuCPP;
add ModuInput;
add ModuEngine;

public class TopDownMovement2D : ModuNode
{
    [Slider(0.0f, 50.0f)]  public float walkSpeed = 4.0f;
    [Slider(0.0f, 80.0f)]  public float runSpeed = 7.0f;
    [Slider(0.0f, 200.0f)] public float acceleration = 18.0f;
    [Slider(0.0f, 200.0f)] public float drag = 8.0f;
}
```

This is a good fit for public fields because the script is primarily an authored component, not a custom editor tool.

## Private Fields: Runtime-Only State

### What Private Fields Are For
Private fields are the default place for transient runtime values:

- timers
- cached booleans such as previous input state
- current indices
- animation phase
- temporary status flags

### Why They Exist
Runtime-only data should not pollute the inspector or persisted scene settings.

The repository scripts use this consistently:

- `prevSubmitDown` in `DialogueSystem.moducpp`
- `prevInteractDown` in `InteractableObject.moducpp`
- `animationTime` and `lastWalkFrame` in `TopDownMovement2D.moducpp`

### Better Than A Naive "Everything Public" Approach
This is noisy and misleading:

```cpp
public float interval = 1.0f;
public float timer = 0.0f;
public bool initialized = false;
public int currentIndex = 0;
```

This is better:

```cpp
public float interval = 1.0f;

private float timer = 0.0f;
private bool initialized = false;
private int currentIndex = 0;
```

The second version teaches the reader what is configuration and what is just runtime bookkeeping.

## `Config<T>()` And `State<T>()`: Structured Tool-Oriented Authoring

### Overview
The repository also demonstrates a second style used by tool-like or manual-inspector scripts:

- `SampleInspector Simplified.moducpp`
- `RigidbodyTest.moducpp`
- `StandaloneMovementController.moducpp`

These scripts often avoid large top-level public field lists and instead use:

- `Config<T>()` for persisted structured configuration
- `State<T>()` for runtime-only structured state

### Why This Pattern Exists
It is useful when:

- the inspector is mostly manual anyway
- the config is logically grouped as a struct
- you want persisted settings without exposing each value as a public field
- the script is closer to a tool or subsystem wrapper than a tiny behavior component

### Important Behavior
`Config<T>()` gives you per-script-instance storage, but you still need to bind the individual fields through `BindSetting(...)` or another persistence path.

That is why the repository tool scripts repeatedly call helpers like:

```cpp
void bindConfig(ScriptContext& ctx, SampleInspectorSimpleConfig& config) {
    BindSetting(ctx, "autoRotate", config.autoRotate);
    BindSetting(ctx, "spinSpeed", config.spinSpeed);
    BindSetting(ctx, "offset", config.offset);
    BindSetting(ctx, "targetName", config.targetName);
}
```

### Example
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
        changed |= ImGui::DragFloat3("Spin Speed (deg/s)", &config.spinSpeed.x, 1.0f, -360.0f, 360.0f, "%.2f");
        if (changed) {
            ctx.SaveAutoSettings();
        }
    }
}
```

### When To Prefer This Style
Prefer `Config<T>()` and `State<T>()` when the script is:

- a manual inspector tool
- a subsystem wrapper such as standalone movement
- a script where grouped config reads more clearly than many top-level public fields

For normal gameplay components, public fields are usually still the simpler choice.

## Field Attributes

### Overview
Field attributes are authoring metadata. They tell the inspector how a field should behave and what kind of data it represents.

Common attributes already used in the repository include:

- `[Header("Title")]`
- `[Slider(min, max)]`
- `[ObjectRef]`
- `[ObjectList]`
- `[ClipGridPair]`
- `[Separator]`
- `[SoundSet("Label")]`

### Why They Exist
These attributes solve specific authoring problems:

- a number with a sensible range should feel like a tuned value, not raw text entry
- a string that really means "scene object reference" should be edited like one
- grouped clip fields should be edited together

### `[ObjectRef]`
Use this when a `string` field is really one scene object reference.

Repository examples:

- `heartRef`
- `playerRef`
- `dialogueTextRef`

This matters because the inspector can then present object-reference editing instead of a plain free-text string.

### `[ObjectList]`
Use this when a `string[]` field stores a group of scene object references.

Repository examples:

- `itemsToEnable`
- `itemsToDisable`
- `menuItemRefs`
- selection state lists

This is one of the most heavily reused field patterns in the repository, so it deserves to be treated as a first-class authoring tool, not a niche attribute.

### `[Slider(min, max)]`
Use this when the value is tuned, not just stored.

Repository examples:

- movement speeds
- animation timing
- interaction distance

Sliders teach the intended value range much better than a bare numeric field.

### `[ClipGridPair]`, `[Separator]`, And `[SoundSet("...")]`
These appear in `TopDownMovement2D.moducpp` and exist for more specialized authoring:

- directional clip sets
- visual grouping
- sound collections edited as a set

## Inspector Modes

### 1. Automatic Inspector
This is the simplest path. Public fields become the inspector.

Use it when:

- the field list is already clear
- layout does not need extra structure
- the script is small

### 2. Declarative `inspector { ... }`
This is the most common "real script" upgrade path.

Use it when:

- the script has several categories of configuration
- runtime status belongs next to the settings
- you want layout control without hand-writing all the widgets

Repository examples:

- `DialogueSystem.moducpp`
- `InteractableObject.moducpp`
- `MainMenuController.moducpp`

### 3. Manual `Script_OnInspector()`
Use this when the inspector itself needs behavior:

- buttons that perform actions immediately
- custom control logic
- explicit `ImGui` layout
- manual use of `Config<T>()`, `State<T>()`, and `ctx.SaveAutoSettings()`

Repository examples:

- `SampleInspector.moducpp`
- `RigidbodyTest.moducpp`
- `StandaloneMovementController.moducpp`

## `AutoFields(...)` And `Run(...)`

### `AutoFields(...)`
`AutoFields(...)` says: use the normal generated editors for these persisted public fields, but place them here.

This is the workhorse of declarative inspectors.

Example:

```cpp
Tab("Timing") {
    AutoFields(openAnimationDelay, closeAnimationDelay, spacingFactor, sizeMultiplier);
}
```

### `Run(...)`
`Run(...)` is for inspector-time logic that is not just a field editor.

The repository uses it for:

- runtime status text
- helper methods that draw larger custom sections
- migration or validation work that should happen during inspector use

This is a useful mental split:

- use `AutoFields(...)` for authored data
- use `Run(...)` for behavior, status, or one-off custom drawing

## Manual Inspector Persistence

### Why `ctx.SaveAutoSettings()` Matters
In a manual inspector, changing an ImGui widget does not automatically persist the value unless you use the persistence helpers correctly.

The repository’s manual inspectors normally follow this pattern:

```cpp
bool changed = false;
changed |= ImGui::Checkbox("Auto Rotate", &config.autoRotate);
changed |= EditString("Target Name", config.targetName, 128, "targetName");
if (changed) {
    ctx.SaveAutoSettings();
}
```

### Why `BindSetting(...)` Still Matters
If the script uses `Config<T>()`, it must still bind those fields so they are loaded and saved consistently.

This is why scripts such as `SampleInspector Simplified.moducpp` and `RigidbodyTest.moducpp` call their `bindConfig(...)` helper in every relevant hook.

## Nested Data And `SubScript`

### Overview
The repository uses `SubScript` to keep nested authored data editable and structured.

Examples:

- `MenuAction` in `MainMenuController.moducpp`
- `InteractionOption` in `InteractableObject.moducpp`

### Why It Exists
Some data is not just one value. It is a little authored record:

- an action with enable and disable lists
- an interaction option with dialogue overrides and object consequences

`SubScript` lets you model that directly instead of flattening it into unrelated parallel arrays.

## Common Mistakes
- Making runtime-only state public.
- Expecting `AutoFields(...)` to work on private fields or config structs outside the current class.
- Forgetting `[ObjectRef]` or `[ObjectList]` on reference-like string fields.
- Using `Config<T>()` without binding settings.
- Forgetting `ctx.SaveAutoSettings()` after manual inspector changes.

## Best Practices
- Default to public fields for normal authored component configuration.
- Keep runtime-only progress private.
- Use `inspector { ... }` when the main need is layout and runtime visibility.
- Use `Config<T>()` and `State<T>()` for manual inspectors and tool-like scripts.
- Use field metadata to communicate authoring intent, not just appearance.

## Related Pages
- [Common Patterns](common-patterns.md)
- [Inspector Reference](../api/inspector-reference.md)
- [Module: ModuCPP](../api/module-moducpp.md)
- [Module: ModuCPP.Experimental](../api/module-moducpp-experimental.md)
