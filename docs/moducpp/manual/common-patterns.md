# Common Patterns

## Overview
This page documents the patterns that actually recur in the shipped and example scripts under `Scripts/`.

That matters. A useful scripting manual should not only list helpers in isolation. It should show how those helpers are combined in real scripts such as:

- `TopDownMovement2D.moducpp`
- `FPSDisplay.moducpp`
- `DialogueSystem.moducpp`
- `InteractableObject.moducpp`
- `MainMenuController.moducpp`
- `SampleInspector.moducpp`
- `SampleInspector Simplified.moducpp`
- `RigidbodyTest.moducpp`
- `StandaloneMovementController.moducpp`

Most day-to-day ModuCPP work is not about inventing a brand-new architecture for every script. It is about choosing a small, readable pattern that fits the job:

- a timer that repeats
- an input vector that drives movement
- an input edge that fires only once
- a UI label that mirrors state
- a list of referenced objects that are enabled or disabled together
- a declarative inspector that mixes editable fields with runtime status
- a manual inspector or editor window backed by `Config<T>()` and `State<T>()`

## Why These Patterns Exist
The repository scripts make the same larger lesson clear: most scripts are built from a few tiny helpers used together.

For example:

- `timer.Start(...)` and `timer.Ready()` are not interesting alone, but together they create reliable periodic behavior.
- `input.WASDNormalized()` and `TryMoveRigidbody2D(...)` are not just two unrelated calls; together they form the normal top-down movement pattern.
- `obj.UILabel`, `IntR(...)`, and `ModuEngine.FPS` form a common "live readout" pattern.
- `[ObjectList]`, `ResolveSceneObjectRef(...)`, and `SetObjectsEnabledState(...)` form a common "scene coordination" pattern.

This page is about those combinations.

## When To Use This Page
Use this page when:

- you know the behavior you want, but you do not know the cleanest ModuCPP structure yet
- a helper name makes sense individually, but you do not yet know what usually goes with it
- you want examples that feel like real scripts instead of isolated API fragments

## Pattern: Repeating Timer

### Overview
This is the standard pattern for "do something every N seconds".

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

### What It Does
The timer stores runtime progress privately, advances automatically from frame delta time, and becomes ready whenever the interval completes.

### Why It Exists
Without the helper, every small timed script would need to hand-roll:

- an elapsed accumulator
- interval comparison
- reset or wraparound logic

That is boilerplate. The timer helper exists so scripts can express periodic intent directly.

### When To Use It
Use this pattern for:

- repeated UI refresh
- delayed toggles
- periodic audio cues
- polling or heartbeat-style behavior
- typing, cooldown, or pacing loops when a full state machine would be excessive

### What Happens Over Time
In practice the flow is:

1. `Begin()` starts the cycle once.
2. `TickUpdate()` advances the internal elapsed time every frame.
3. `timer.Ready()` returns `true` only on the frames where the interval completes.
4. The timer immediately continues into the next cycle.

That is why this pattern is good for repeated actions rather than one-shot delays.

### Multiple Uses
Status pulse:

```cpp
add ModuCPP;
add ModuEngine;

public class StatusPulse : ModuNode
{
    public float interval = 1.0f;
    private float timer = 0.0f;
    private bool visible = false;

    void Begin() to timer.Start(interval);

    void TickUpdate()
    {
        if (!timer.Ready()) return;
        visible = !visible;
        obj.UILabel = visible ? "ONLINE" : "";
    }
}
```

Timed object group change:

```cpp
add ModuCPP;
add ModuCPP.Experimental;

public class DelayedToggle : ModuNode
{
    [ObjectList] public string[] enable;
    [ObjectList] public string[] disable;
    public float delay = 2.0f;

    private float timer = 0.0f;

    void Begin() to timer.Start(delay);

    void TickUpdate()
    {
        if (!timer.Ready()) return;
        SetObjectsEnabledState(ctx, enable, true);
        SetObjectsEnabledState(ctx, disable, false);
    }
}
```

### Common Mistakes
- Do not make the timer public unless a designer truly needs to author the runtime progress value.
- Start the timer in `Begin()` unless you intentionally want a lazy first-use pattern.
- Remember that the timer helper is repeating. For a one-time event, gate it with additional state.

### Related Helpers
- `Begin()`
- `TickUpdate()`
- `time.deltaTime`
- `Math::Max`

## Pattern: Live UI Label Or Status Readout

### Overview
This is the pattern for turning runtime state into a readable UI label.

The smallest real example in the repository is `FPSDisplay.moducpp`:

```cpp
add ModuCPP;
add ModuEngine;

public class FPSDisplay : ModuBehaviour
{
    public bool clampTo120 = false;

    void TickUpdate()
    {
        ctx.SetFPSCap(clampTo120, 120.0f);
        obj.UILabel = "FPS: " + IntR(ModuEngine.FPS);
    }
}
```

### What It Does
The script updates a text-like UI property every frame from current runtime state.

### Why It Exists
Many scripts do not need a complex UI binding system. They just need to write a short label:

- FPS
- current mode
- currently selected item
- whether an interaction is available

`obj.UILabel` keeps that case simple.

### When To Use It
Use it for:

- FPS counters
- short debug readouts
- selection labels
- temporary prompts

Use `SetUITextLabel(...)` instead when the label belongs to another referenced object rather than the current one.

### Multiple Uses
Simple FPS counter:

```cpp
obj.UILabel = "FPS: " + IntR(ModuEngine.FPS);
```

Selection prompt:

```cpp
add ModuCPP;

public class InteractionPrompt : ModuNode
{
    public string idle = "Walk closer";
    public string ready = "Press Enter";
    public bool inRange = false;

    void TickUpdate()
    {
        obj.UILabel = inRange ? ready : idle;
    }
}
```

Referenced text target:

```cpp
add ModuCPP;
add ModuCPP.Experimental;

public class SelectionStatus : ModuNode
{
    [ObjectRef] public string statusTextRef;
    public string[] labels;
    private int currentIndex = 0;

    void TickUpdate()
    {
        if (currentIndex < 0 || currentIndex >= (int)labels.size()) return;
        SetUITextLabel(ctx, statusTextRef, labels[currentIndex]);
    }
}
```

### Common Mistakes
- `obj.UILabel` only acts on the current script object. If the text you want to update lives somewhere else, use an object reference and `SetUITextLabel(...)`.
- UI display often benefits from `IntR(...)`, `IntRD(...)`, or `IntRU(...)` so the reader sees stable numbers instead of noisy floats.

### Related Helpers
- `obj.UILabel`
- `SetUITextLabel(...)`
- `SetUITextEffects(...)`
- `IntR(...)`
- `ModuEngine.FPS`

## Pattern: Input Vector Into Movement Helper

### Overview
This is the normal 2D movement pattern used by `TopDownMovement2D.moducpp`.

```cpp
add ModuCPP;
add ModuInput;
add ModuEngine;

public class Player2D : ModuNode
{
    public float speed = 4.0f;
    public float acceleration = 18.0f;
    public float drag = 8.0f;

    void TickUpdate()
    {
        Vector2 move = input.WASDNormalized();
        TryMoveRigidbody2D(ctx, move * speed, acceleration, drag, dt);
    }
}
```

### What It Does
It reads player intent as a direction vector, turns that into a target velocity, and lets the engine helper handle smoothing and drag.

### Why It Exists
Most top-down movement scripts want the same behavior:

- diagonal movement should not be faster than straight movement
- acceleration should feel smooth
- idle motion should slow down cleanly

That is exactly why `input.WASDNormalized()` and `TryMoveRigidbody2D(...)` are used together so often.

### Why Normalized Input Matters
`input.WASD()` gives raw axis intent. `input.WASDNormalized()` rescales diagonal input back to length 1.

That means:

- pressing `W` gives full-speed forward movement
- pressing `W` + `D` still gives full-speed movement, not extra-fast diagonal movement

For character movement, menu cursors, and most author-controlled motion, normalized input is usually the correct default.

### A More Complete Version
The repository’s `TopDownMovement2D.moducpp` also captures actual post-move velocity and uses it to drive animation:

```cpp
add ModuCPP;
add ModuInput;
add ModuEngine;

public class MovingSprite : ModuNode
{
    public float walkSpeed = 4.0f;
    public float acceleration = 18.0f;
    public float drag = 8.0f;
    public float movementThreshold = 0.15f;
    public int idleClip = 0;
    public int walkClip = 1;

    void TickUpdate()
    {
        Vector2 move = input.WASDNormalized();
        Vector2 targetVelocity = move * walkSpeed;
        Vector2 actualVelocity = targetVelocity;

        TryMoveRigidbody2D(ctx, targetVelocity, acceleration, drag, dt, ref actualVelocity);

        const bool moving = actualVelocity.Dot(actualVelocity) >
                            movementThreshold * movementThreshold;
        sprite.SetClip(moving ? walkClip : idleClip);
    }
}
```

This is a better pattern when presentation should follow actual motion instead of raw key state.

### Another Use Case: Menu Cursor Motion
The same normalized input idea also works outside character movement. A 2D cursor or menu focus indicator can use the same pattern if it is driven by a 2D body or position helper.

### Common Mistakes
- If the object has no `Rigidbody2D`, `TryMoveRigidbody2D(...)` fails safely and nothing moves.
- Do not use raw key checks for every axis unless you truly need a custom control scheme. `input.WASDNormalized()` already solves the common case.
- If you drive animation from motion, prefer actual resulting velocity when available instead of target velocity alone.

### Related Helpers
- `input.WASD()`
- `input.WASDNormalized()`
- `input.sprint()`
- `TryMoveRigidbody2D(...)`
- `sprite.SetClip(...)`
- `audio.PlayOneShot(...)`

## Pattern: Input Edge For One-Shot Actions

### Overview
Many actions should happen once when a button becomes pressed, not every frame while it stays held.

This shows up repeatedly in the repository:

- `InteractableObject.moducpp` tracks `prevInteractDown`
- `DialogueSystem.moducpp` tracks `prevSubmitDown`

### What It Does
The script stores the previous frame’s button state and compares it with the current frame’s state.

### Why It Exists
Continuous input is correct for movement, but wrong for:

- confirming a menu choice
- skipping dialogue once
- playing a one-shot sound
- triggering an interaction one time

### The Pattern
```cpp
add ModuCPP;
add ModuInput;
add ModuEngine;

public class JumpSound : ModuNode
{
    public string jumpClip;
    private bool prevJump = false;

    void TickUpdate()
    {
        bool jumpNow = input.jump();
        bool jumpPressed = jumpNow && !prevJump;
        prevJump = jumpNow;

        if (jumpPressed) {
            audio.PlayOneShot(jumpClip);
        }
    }
}
```

### When To Use It
Use this pattern when:

- a sound should fire once
- a dialogue advance should happen once
- a menu confirm should happen once
- an interaction should not retrigger every frame

### A More General Submit Pattern
```cpp
add ModuCPP;
add ModuInput;

public class MenuConfirm : ModuNode
{
    private bool prevSubmitDown = false;

    void TickUpdate()
    {
        const bool submitDown = IsSubmitDown();
        const bool submitPressed = submitDown && !prevSubmitDown;
        prevSubmitDown = submitDown;

        if (submitPressed) {
            ctx.AddConsoleMessage("Confirmed");
        }
    }
}
```

### Common Mistakes
- `KeyDown(...)` and `input.jump()` are level checks, not edge checks.
- `KeyPressed(...)` exists for direct key polling, but when you use higher-level helpers such as `input.jump()` or `IsSubmitDown()`, the explicit previous-frame pattern is the normal approach.

### Related Helpers
- `input.jump()`
- `IsSubmitDown()`
- `KeyPressed(...)`
- `audio.PlayOneShot(...)`

## Pattern: Inspector Object Lists That Drive Scene State

### Overview
This is the normal pattern for authoring scene-object groups in the inspector and toggling them at runtime.

It appears in:

- `AutoEnableAndDisableListsOfObjectsAfterAmountOfTime.moducpp`
- `InteractableObject.moducpp`
- `DialogueSystem.moducpp`
- `MainMenuController.moducpp`

### What It Does
The script stores scene references as authorable fields, then resolves and acts on them through helpers.

### Why It Exists
Hard-coding object ids or names in the middle of logic is brittle. The inspector list approach is better because:

- the relationships stay visible to the designer
- the logic stays short
- the same helper can enable or disable many targets

### Simple Version
```cpp
add ModuCPP;
add ModuCPP.Experimental;

public class ToggleObjects : ModuNode
{
    [ObjectList] public string[] enable;
    [ObjectList] public string[] disable;

    void Begin()
    {
        SetObjectsEnabledState(ctx, enable, true);
        SetObjectsEnabledState(ctx, disable, false);
    }
}
```

### Why This Pattern Is Better Than Manual Lookups
A naive script might do repeated name lookups by hand inside the logic. The inspector list pattern is better because the list is editable and the runtime call is a single readable statement.

### Another Use Case: Selection State
`InteractableObject.moducpp` uses the same idea for selection visuals:

```cpp
SetObjectsEnabledState(ctx, selectedStateEnable, isSelected);
SetObjectsEnabledState(ctx, selectedStateDisable, !isSelected);
```

That is a good example of a tiny helper doing real work. The helper is small, but it is central to how real scene coordination scripts stay readable.

### Common Mistakes
- Use `[ObjectList]` on `string[]` fields when those strings are meant to be scene object references.
- If only one object should be referenced, prefer `[ObjectRef]` and a single string instead of a one-element list.
- If you mutate scene objects directly without going through helper functions, remember to call `ctx.MarkDirty()` when appropriate.

### Related Helpers
- `[ObjectRef]`
- `[ObjectList]`
- `ResolveSceneObjectRef(...)`
- `SetObjectEnabledState(...)`
- `SetObjectsEnabledState(...)`
- `GetObjectReferencePosition(...)`

## Pattern: Declarative Inspector With Runtime Status

### Overview
The repository’s larger scripts do not stop at auto-generated inspectors. They often use `inspector { ... }` to separate configuration from runtime visibility.

`DialogueSystem.moducpp`, `InteractableObject.moducpp`, and `MainMenuController.moducpp` all follow this pattern.

```cpp
inspector {
    Tabs {
        Tab("Config") {
            AutoFields(interval, labelRef);
        }
        Tab("Runtime") {
            Run(ImGui::TextDisabled("Elapsed: %.2f", timer));
        }
    }
}
```

### What It Does
It keeps authored settings in one place and live debugging information in another.

### Why It Exists
Once a script grows past a few fields, a flat inspector becomes hard to scan. The declarative inspector solves that without forcing you into a fully manual editor implementation.

### When To Use It
Use it when:

- the script has several groups of settings
- runtime state is useful to inspect while debugging
- the default field order is no longer enough
- you want custom layout, but not the full cost of `Script_OnInspector()`

### `Run(...)` Is More Than A Status Line
The repository also uses `Run(...)` for helper calls such as legacy-data migration and runtime drawing functions.

That is an important teaching point:

- `AutoFields(...)` is for persisted fields
- `Run(...)` is for arbitrary inspector-time logic

### Common Mistakes
- `AutoFields(...)` only works for persisted public fields on the current class.
- Runtime-only private values should be shown through `Run(...)`, not pushed into `AutoFields(...)`.

### Related Helpers
- `Tabs`
- `Tab(...)`
- `AutoFields(...)`
- `Run(...)`
- `Header(...)`
- `Slider(...)`

## Pattern: Manual Inspector Or Tool Window Backed By `Config<T>()` And `State<T>()`

### Overview
The repository also shows a second family of authoring pattern in `SampleInspector Simplified.moducpp`, `RigidbodyTest.moducpp`, and `StandaloneMovementController.moducpp`.

Instead of public fields, these scripts keep persisted tool configuration in `Config<T>()` and runtime-only tool state in `State<T>()`.

### Why It Exists
This is useful when:

- the script is inspector-driven or tool-driven first
- configuration should still persist
- you want structured config without exposing everything as top-level public fields
- runtime helper state does not belong in saved script data

### Pattern
```cpp
add ModuCPP;
add ModuEngine;

public class RigidbodyTest : ModuBehaviour
{
    void Script_OnInspector()
    {
        auto& config = Config<RigidbodyTestConfig>();
        bindConfig(ctx, config);
        auto& state = State<RigidbodyTestState>();

        bool changed = false;
        changed |= ImGui::Checkbox("Launch on Begin", &config.autoLaunch);
        changed |= ImGui::DragFloat3("Launch Velocity", &config.launchVelocity.x, 0.25f, -50.0f, 50.0f, "%.2f");
        if (changed) {
            ctx.SaveAutoSettings();
        }
    }
}
```

### Why This Pattern Is Useful
It cleanly separates:

- persisted authoring config
- transient runtime state like warning flags or debug caches

That makes tool scripts easier to extend than a pile of public fields and ad hoc globals.

### Common Mistakes
- `Config<T>()` does not persist by itself. You still need `BindSetting(...)` or another settings binding path.
- Call the binding function every hook where the config is used, not only in the inspector.
- Use `State<T>()` for temporary flags such as "already warned once".

### Related Helpers
- `Config<T>()`
- `State<T>()`
- `BindSetting(...)`
- `ctx.SaveAutoSettings()`
- `warnOnce(...)`

## Pattern: Direct Scene Edits In Editor Tools

### Overview
The editor-side scripts show one more important pattern: if a tool directly mutates object fields, it should usually call `ctx.MarkDirty()` after the change.

This appears in `EditorWindowSample.moducpp` and `AnimationWindow.moducpp`.

### Why It Exists
Some helpers such as `SetObjectEnabledState(...)` already mark dirty internally. But when an editor tool directly changes fields like:

- `target->position += offset`
- `object->ui.label = label`

the script is responsible for marking the object as changed.

### Example
```cpp
if (ImGui::Button("Nudge +Y"))
{
    Vector3 pos = obj->position;
    pos.y += 0.25f;
    ctx.SetPosition(pos);
    ctx.MarkDirty();
}
```

### When To Use It
Use this pattern in:

- custom inspectors
- editor windows
- animation tools
- scripts that patch serialized scene data directly

### Related Helpers
- `ctx.SetPosition(...)`
- `ctx.SetRotation(...)`
- `ctx.SetScale(...)`
- `ctx.MarkDirty()`

## Best Practices
- Start from a repository-shaped pattern rather than rebuilding the structure from scratch every time.
- Keep authored configuration public or in `Config<T>()`; keep runtime progress private or in `State<T>()`.
- Prefer helper combinations that express intent directly.
- When a small helper appears repeatedly in real scripts, treat it as part of the pattern, not as an unimportant detail.

## Related Pages
- [Methods and Lifecycle](methods-and-lifecycle.md)
- [Input and Engine Scripting](input-and-engine-scripting.md)
- [Fields and Inspector](fields-and-inspector.md)
- [Editor Scripting](editor-scripting.md)
- [Facades and Helper Types](../api/type-facades-and-helpers.md)
