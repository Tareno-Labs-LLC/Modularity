# Module: ModuEngine

## Summary
`ModuEngine` is the engine-helper module.

It exposes the script-facing helpers that turn small decisions into engine-side behavior:

- FPS readback
- 2D movement helpers
- audio and sprite facades
- manual inspector widget helpers
- project gravity helpers
- warning helpers

The repository scripts use this module heavily because it removes repeated gameplay and tooling boilerplate without hiding what the script is doing.

## Syntax
```cpp
add ModuEngine;
```

## Why This Module Exists
Once a script moves beyond pure state and timing, it usually needs engine-facing behavior:

- move through a rigidbody
- play a sound
- switch a sprite clip
- edit a string in a tool inspector
- display the current FPS

Those are recurring problems, and `ModuEngine` exists so scripts can solve them consistently.

## Global And Project Helpers

### `ModuEngine.FPS`
This is the current frame-rate readback.

Why a script author chooses it:

- to show a debug FPS label
- to monitor performance while tuning
- to keep the script simple instead of calculating FPS manually

Repository example:

```cpp
obj.UILabel = "FPS: " + IntR(ModuEngine.FPS);
```

### `GetProjectGravityScale()` / `SetProjectGravityScale(scale)`
These expose project-level gravity tuning to scripts.

Use them when:

- a tool or gameplay script needs to inspect or temporarily adjust the global gravity scale

## Inspector Widget Helpers

### Members
- `EditFloat(...)`
- `EditVec3(...)`
- `EditBool(...)`
- `EditInt(...)`
- `EditString(...)`
- `EditClipSelector(...)`
- `EditDirectionalClipGrid(...)`
- `EditSoundSet(...)`

### Why They Exist
Manual inspectors and editor windows often need the same pattern:

1. read or bind a value
2. draw a widget
3. save the setting if it changed

The widget helpers package that pattern for common field types.

### Repository Uses
- `SampleInspector.moducpp`
- `EditorWindowSample.moducpp`

### Example
```cpp
EditString("Target Name", targetName, 128, "targetName");
```

Why a script author chooses it:

- the script needs a manual inspector
- the value should still behave like a persisted setting

## Rigidbody2D Helpers

### Members
- `hasRigidbody2D(...)`
- `getRigidbody2DVelocity(...)`
- `setRigidbody2DVelocity(...)`
- `TryMoveRigidbody2D(...)`
- `moveRigidbody2D(...)`
- `movePosition2D(...)`
- `moveTowards(...)`

### `TryMoveRigidbody2D(...)`

#### Overview
This is one of the most important gameplay helpers in the module.

#### What It Does
It tries to move the current object toward a target 2D velocity using:

- acceleration
- drag
- frame delta time

#### Why It Exists
Top-down movement and similar 2D behaviors repeat the same logic constantly:

- move toward input velocity
- ease into motion
- slow down when input stops

The helper centralizes that pattern.

#### Repository Use
`TopDownMovement2D.moducpp` is the clearest example:

```cpp
Vector2 move = input.WASDNormalized();
Vector2 targetVelocity = move * speed;
TryMoveRigidbody2D(ctx, targetVelocity, acceleration, drag, dt);
```

#### Why The Output-Velocity Overload Matters
The repository also uses the overload that returns actual resulting velocity:

```cpp
Vector2 actualVelocity = targetVelocity;
TryMoveRigidbody2D(ctx, targetVelocity, acceleration, drag, dt, ref actualVelocity);
```

A script author chooses this form when animation or presentation should follow actual movement instead of raw intent.

#### Important Notes
- The helper returns `false` if the object has no `Rigidbody2D`.
- That safe failure is useful, but it also means setup bugs can look like logic bugs if you do not verify the object.

### `moveTowards(...)`
This is the scalar/vector-style easing primitive behind smooth movement changes.

Use it when:

- you want to move a value toward a target by a maximum delta
- you need the same "approach the target smoothly" behavior outside full rigidbody movement

### `movePosition2D(...)`
This is a position-edit helper for 2D movement-style workflows.

Use it when:

- the job is direct 2D positional adjustment rather than velocity-based rigidbody motion

## Warning Helpers

### Members
- `warnOnce(...)`
- `warnMissingComponentOnce(...)`

### Why They Exist
Scripts that validate setup should tell the user what is wrong without spamming every frame.

Repository example:

```cpp
if (!ctx.SetRigidbodyVelocity(vec3(0.0f))) {
    warnOnce(ctx, state.warnedMissingRb, "RigidbodyTest: zeroing velocity requires a Rigidbody");
}
```

These are especially useful in:

- setup validation
- editor tools
- optional-component scripts

## Audio Helpers

### Low-Level Functions
- `hasAudioSource(...)`
- `playSound(...)`

### `audio` Facade
- `audio.HasSource()`
- `audio.PlayOneShot(...)`
- `audio.Play()`
- `audio.Stop()`

### Why They Exist
The repository’s scripts use audio for lightweight feedback:

- footsteps
- menu movement
- menu select
- trigger sounds
- dialogue typing sounds

That is exactly the kind of behavior the audio facade is designed for.

### Example
```cpp
audio.PlayOneShot(stepSounds[selected]);
```

### When To Use The Facade
Use `audio` when:

- the current object is the intended audio source
- you want the simplest readable script

Use `ctx.PlayAudioOneShot(...)` directly when:

- the script needs more explicit control
- playback is intentionally being routed through a different object or context

## Sprite Helpers

### `sprite` Facade Members
- `HasClips()`
- `ClipCount()`
- `ClipIndex()`
- `SetClip(index)`
- `SetClip(name)`
- `ClipNameAt(index)`

### Why They Exist
Small animation and feedback scripts constantly need to switch sprite clips. The facade keeps that readable.

### Repository Use
`TopDownMovement2D.moducpp` uses sprite clips for:

- idle state
- walk-cycle frames
- directional selection
- graceful fallback when some clips are missing

### Example
```cpp
if (running) {
    sprite.SetClip("Run");
} else {
    sprite.SetClip("Idle");
}
```

## What This Module Looks Like In Real Scripts
The repository tends to use `ModuEngine` in three main ways:

1. gameplay movement and feedback
2. UI or FPS readback
3. manual inspector and editor-window widgets

That is why this module is broader than a single "movement helpers" page.

## Common Mistakes
- Forgetting `add ModuEngine;`.
- Assuming `TryMoveRigidbody2D(...)` implies the presence of a `Rigidbody2D`.
- Using held input for one-shot audio without edge detection.
- Assuming `audio` or `sprite` will work on an object that lacks those components.
- Rebuilding manual widget persistence by hand when `EditString(...)` or related helpers already match the job.

## Related APIs
- [Facades and Helper Types](type-facades-and-helpers.md)
- [ScriptContext](type-scriptcontext.md)
- [Input and Engine Scripting](../manual/input-and-engine-scripting.md)
- [Standalone Movement API](type-standalone-movement.md)
