# Input and Engine Scripting

## Overview
Most real gameplay scripts in the repository combine `ModuCPP` with `ModuInput`, `ModuEngine`, or both.

That is not accidental. The core module gives you structure, timers, config/state helpers, and basic facades. But once a script needs to react to the player or drive engine-facing feedback, it usually reaches for:

- `ModuInput` to describe intent
- `ModuEngine` to turn that intent into movement, UI, audio, sprite, or tool behavior

This page explains those helpers the way real scripts use them.

## Why These Modules Exist
Without these modules, small scripts repeatedly have to rebuild the same logic:

- key polling for WASD or confirm
- diagonal normalization
- acceleration and drag
- one-shot audio feedback
- sprite clip switching
- FPS readout formatting

The repository scripts show the intended design: use small helpers to solve recurring gameplay problems cleanly.

## Module Requirements
Keep imports explicit in examples.

Common combinations:

```cpp
add ModuCPP;
add ModuInput;
```

```cpp
add ModuCPP;
add ModuEngine;
```

```cpp
add ModuCPP;
add ModuInput;
add ModuEngine;
```

## `ModuInput`: Express Player Intent

### Overview
`ModuInput` is the script-facing input layer.

It gives you both low-level polling and high-level intent helpers:

- `KeyDown(...)`
- `KeyPressed(...)`
- `IsRuntimeKeyDown(...)`
- `IsSubmitDown()`
- `input.WASD()`
- `input.WASDNormalized()`
- `input.sprint()`
- `input.jump()`

### Why The `input` Facade Exists
Most scripts do not care which exact key caused "move right". They care that the player is asking to move right.

That is why the `input` facade exists. It packages the normal gameplay meaning of the keys into a more readable surface.

## `input.WASD()` vs `input.WASDNormalized()`

### What `input.WASD()` Does
It returns a raw direction vector from the current key state.

That is useful when:

- you want to inspect the raw axis combination
- you intend to apply your own scaling or normalization
- the script is not treating the vector as direct velocity

### What `input.WASDNormalized()` Does
It returns the same direction intent, but normalized so diagonal movement is not faster than straight movement.

That is the normal choice for:

- top-down movement
- menu cursors
- any script where "full input strength" should mean the same speed in every direction

### Why Normalization Matters
Without normalization, pressing `W` + `D` gives a vector longer than pressing only `W`. That makes diagonal movement faster.

For most gameplay movement, that is not the intended feel.

### Example: Character Movement
```cpp
add ModuCPP;
add ModuInput;
add ModuEngine;

public class TopDownMover : ModuNode
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

### Example: Menu Cursor Motion
The same helper is also appropriate when a menu cursor should move in a clean four-way or eight-way pattern without diagonal speed boosts.

## `input.sprint()` And `input.jump()`

### What They Do
These are small intent helpers for two very common gameplay actions:

- sprint
- jump

### Why They Exist
They save a movement script from constantly repeating specific key checks and make the code read like gameplay logic instead of input plumbing.

### Example: Sprinting
```cpp
bool running = input.sprint();
float speed = running ? runSpeed : walkSpeed;
```

This exact style appears in `TopDownMovement2D.moducpp`.

### Example: Jump Audio Edge
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

## `KeyDown(...)`, `KeyPressed(...)`, And `IsSubmitDown()`

### When To Use `KeyDown(...)`
Use `KeyDown(...)` for continuous held-state logic on a specific key.

Example:

```cpp
if (KeyDown(KEY_E)) {
    ctx.AddConsoleMessage("Interact");
}
```

### When To Use `KeyPressed(...)`
Use `KeyPressed(...)` when you want a press event for a specific key rather than a continuous held state.

That is usually the direct-key version of the edge-detection pattern.

### When To Use `IsSubmitDown()`
Use `IsSubmitDown()` when the script concept is not "Enter specifically", but "confirm/submit".

That makes dialogue and menu logic easier to read and easier to retarget if the shared submit definition changes.

### Why Repository Scripts Still Track Previous State
`DialogueSystem.moducpp` still turns submit state into an explicit one-frame edge:

```cpp
const bool submitDown = IsSubmitDown();
const bool submitPressed = submitDown && !prevSubmitDown;
prevSubmitDown = submitDown;
```

That is the correct pattern when a held confirm key should not advance every frame.

## `TryMoveRigidbody2D(...)`: The Core 2D Movement Helper

### Overview
This is one of the most practical helpers in `ModuEngine`.

### What It Does
It takes:

- a target velocity
- acceleration
- drag
- frame delta time

and tries to move the current object through its `Rigidbody2D`.

### Why It Exists
Most 2D movement scripts want the same thing:

- smooth movement toward target velocity
- damping when no input is present
- safe failure when the object is not set up correctly

That is exactly what the helper provides.

### Simple Use
```cpp
TryMoveRigidbody2D(ctx, move * speed, acceleration, drag, dt);
```

### Why The `outVelocity` Form Exists
The repository’s `TopDownMovement2D.moducpp` uses the overload that returns the resulting velocity through an output reference:

```cpp
Vector2 actualVelocity = targetVelocity;
TryMoveRigidbody2D(ctx, targetVelocity, acceleration, drag, dt, ref actualVelocity);
```

This is useful when animation or audio should respond to actual motion rather than only to input intent.

### Example: Animation Driven By Real Motion
```cpp
Vector2 move = input.WASDNormalized();
Vector2 targetVelocity = move * walkSpeed;
Vector2 actualVelocity = targetVelocity;

TryMoveRigidbody2D(ctx, targetVelocity, acceleration, drag, dt, ref actualVelocity);

if (actualVelocity.Dot(actualVelocity) > 0.01f) {
    sprite.SetClip("Walk");
} else {
    sprite.SetClip("Idle");
}
```

### Important Notes
- If the current object has no `Rigidbody2D`, the helper returns `false` and does nothing.
- If `acceleration <= 0`, the helper behaves like direct target-velocity assignment.
- Drag is mainly relevant when there is little or no target motion.

## Audio Helpers And The `audio` Facade

### Overview
The repository uses one-shot audio heavily for feedback:

- footsteps in `TopDownMovement2D.moducpp`
- dialogue typing sounds in `DialogueSystem.moducpp`
- interaction and menu sounds in the larger samples

### Why `audio.PlayOneShot(...)` Exists
Most gameplay scripts do not want to manage a full audio system. They just want to say:

- play confirm sound
- play footstep sound
- play typing tick

The `audio` facade makes that case short and readable.

### Example: Footstep Playback
```cpp
audio.PlayOneShot(stepSounds[selected]);
```

### Example: Confirm Sound On Input Edge
```cpp
if (submitPressed) {
    audio.PlayOneShot(confirmClip);
}
```

### When To Use `ctx.PlayAudioOneShot(...)` Instead
Use the facade for the current object’s normal audio behavior.

Use `ctx.PlayAudioOneShot(...)` directly when:

- you need lower-level control
- the script is deliberately routing playback through a different object or context

## Sprite Helpers And The `sprite` Facade

### Overview
`sprite.SetClip(...)` appears throughout the movement example for visual state changes.

### Why It Exists
Animation-like sprite switching is a very common 2D scripting task. The facade keeps it small:

- inspect clip availability
- choose a clip
- set the clip by index or name

### Example: Movement State
```cpp
if (running) {
    sprite.SetClip("Run");
} else {
    sprite.SetClip("Idle");
}
```

### Example: Defensive Clip Selection
The repository’s `TopDownMovement2D.moducpp` also checks clip ranges with `ctx.GetSpriteClipCount()` before selecting clips. That is a good pattern when clips are authored as indices and missing entries are possible.

## UI And FPS Helpers

### `ModuEngine.FPS`
This is the current frame-rate readback and is best used for:

- debug labels
- performance monitoring
- quick sanity checks while profiling or tuning

### `IntR(...)`
Use `IntR(...)` when a float should read like a stable whole-number UI value.

The repository’s FPS display is a perfect example:

```cpp
obj.UILabel = "FPS: " + IntR(ModuEngine.FPS);
```

### `ctx.SetFPSCap(...)`
`FPSDisplay.moducpp` also demonstrates a practical engine-setting pattern:

```cpp
ctx.SetFPSCap(clampTo120, 120.0f);
```

This is a reminder that many scripts combine display and behavior adjustment in the same update loop.

## Inspector Helpers From `ModuEngine`

### Why They Matter
`ModuEngine` is not only runtime movement and feedback. It also provides manual inspector widgets such as:

- `EditString(...)`
- `EditFloat(...)`
- `EditVec3(...)`
- `EditBool(...)`

These are used by:

- `SampleInspector.moducpp`
- `EditorWindowSample.moducpp`

### What Problem They Solve
They reduce repeated "load auto setting -> draw widget -> save if changed" boilerplate in manual editors.

### Example
```cpp
EditString("Target Name", targetName, 128, "targetName");
```

This is especially useful when the script wants a manual inspector but still wants normal persisted settings behavior.

## Warning Helpers

### Overview
`RigidbodyTest.moducpp` uses `warnOnce(...)` to avoid spamming the console every frame or every button press.

### Why It Exists
Repeated missing-component messages are noisy. `warnOnce(...)` lets a script surface the problem without turning the console into spam.

### Example
```cpp
if (!ctx.SetRigidbodyVelocity(vec3(0.0f))) {
    warnOnce(ctx, state.warnedMissingRb, "RigidbodyTest: zeroing velocity requires a Rigidbody");
}
```

## Common Mistakes
- Forgetting `add ModuInput;` when using `input`, `KeyDown(...)`, or `IsSubmitDown()`.
- Forgetting `add ModuEngine;` when using `TryMoveRigidbody2D(...)`, `audio`, `sprite`, `EditString(...)`, or `ModuEngine.FPS`.
- Using `input.WASD()` when normalized movement would be more correct.
- Driving one-shot actions from a held state without edge detection.
- Assuming `TryMoveRigidbody2D(...)` will work without a `Rigidbody2D`.

## Best Practices
- Use `ModuInput` for intent and `ModuEngine` for response.
- Prefer `input.WASDNormalized()` for movement unless raw diagonal magnitude is intentional.
- Treat `TryMoveRigidbody2D(...)` as the default helper for simple 2D rigidbody motion.
- Use explicit edge detection for menu confirm, interaction, and one-shot audio.
- Keep imports explicit so missing helpers are easy to diagnose.

## Related Pages
- [Common Patterns](common-patterns.md)
- [Module: ModuInput](../api/module-moduinput.md)
- [Module: ModuEngine](../api/module-moduengine.md)
- [Facades and Helper Types](../api/type-facades-and-helpers.md)
