# Facades and Helper Types

## Summary
ModuCPP exposes a small set of facades and shorthand helpers so common script code reads like script code rather than engine glue.

These helpers are important because the repository scripts rely on them constantly:

- `obj` for current-object work
- `time.deltaTime` and `dt` for frame-driven behavior
- `input` for gameplay intent
- `audio` and `sprite` for lightweight feedback
- `ModuEngine.FPS` for display and tuning
- timer, math, and integer-formatting helpers for recurring utility logic

The point is not that the helpers are short. The point is that they match how real scripts are written.

## Why These Helpers Exist
Without these facades, small scripts would keep repeating lower-level calls for jobs that are conceptually simple:

- update my own label
- read movement intent
- play a one-shot clip
- choose a sprite clip
- round a float for UI

The helpers sit on top of `ScriptContext`. They do not replace it. They make the most common script-facing operations easier to express.

## Aliases

### `vec2` / `Vector2`
These are the standard 2D vector aliases.

They appear throughout the repository in movement code such as:

- `input.WASDNormalized()`
- `TryMoveRigidbody2D(...)`
- animation motion checks

### `vec3` / `Vector3`
These are the standard 3D vector aliases.

They appear in:

- transform offsets
- rigidbody velocity helpers
- standalone movement settings

### `string`
`string` is the script-facing alias for `std::string`.

This matters because many high-level scripts use string-based object refs, audio clip paths, labels, and serialized settings values.

## `obj`

### Overview
`obj` is the current object facade.

### What It Does
It gives the current script a lightweight way to access its attached object without spelling out `ctx.object` constantly.

### Why It Exists
Most scripts are primarily acting on the object they are attached to:

- reading transform or component state
- updating their own UI label
- checking whether they are attached at all

That is why `obj` is a facade rather than a rare convenience.

### Common Uses
Self UI label:

```cpp
obj.UILabel = "FPS: " + IntR(ModuEngine.FPS);
```

Direct scene-object checks:

```cpp
if (!obj) return;
```

Inspector display:

```cpp
if (obj) {
    ImGui::TextDisabled("Attached to: %s (id=%d)", obj->name.c_str(), obj->id);
}
```

### Important Notes
- `obj` is only for the current attached object.
- If another object should be changed, resolve it explicitly.
- `obj.UILabel` is appropriate for the current UI text object. If the target text lives elsewhere, use `SetUITextLabel(...)`.

## `time`

### Overview
`time` is the frame-timing facade.

### Main Member
- `time.deltaTime`

### Why It Exists
Time-based logic reads more clearly when the value is named semantically instead of passed around as a raw floating-point variable.

### Repository Uses
The repository uses `time.deltaTime` in manual/tool-style scripts such as:

- `SampleInspector.moducpp`
- `SampleInspector Simplified.moducpp`
- `StandaloneMovementController.moducpp`

while high-level gameplay scripts often use the shorter injected `dt`.

### Example
```cpp
float dt = time.deltaTime;
ctx.SetRotation(obj->rotation + spinSpeed * dt);
```

## `input`

### Overview
`input` is the movement-oriented facade from `ModuInput`.

### Members
- `WASD()`
- `WASDNormalized()`
- `sprint()`
- `jump()`

### Why It Exists
The repository’s gameplay scripts typically care about intent, not key plumbing:

- which direction should the character move?
- is the player sprinting?
- is jump currently held?

The facade answers those questions directly.

### `input.WASD()`
Returns raw movement intent.

Use it when:

- you want the directional axis values
- you will normalize or scale the vector yourself

### `input.WASDNormalized()`
Returns normalized movement intent.

Use it when:

- diagonal speed should match straight-line speed
- the vector will be used directly for movement or cursor velocity

This is the normal repository pattern for 2D movement.

### `input.sprint()`
Returns whether sprint input is currently held.

Typical use:

```cpp
bool running = input.sprint();
float speed = running ? runSpeed : walkSpeed;
```

### `input.jump()`
Returns whether jump input is currently held.

Typical use:

```cpp
bool jumpNow = input.jump();
```

When the script needs a one-shot jump event, pair it with previous-frame tracking.

## `audio`

### Overview
`audio` is the current-object audio facade from `ModuEngine`.

### Members
- `HasSource()`
- `PlayOneShot(path, volumeScale)`
- `Play()`
- `Stop()`

### Why It Exists
Many scripts need small audio responses:

- footsteps
- confirm sounds
- typing sounds
- trigger sounds

The facade keeps those responses short and readable.

### Common Uses
```cpp
audio.PlayOneShot(stepSounds[selected]);
```

```cpp
if (audio.HasSource()) {
    audio.Play();
}
```

### Important Notes
- The facade is about the current object’s audio setup.
- If playback should come from another object or needs more explicit context handling, use `ctx.PlayAudioOneShot(...)`.

## `sprite`

### Overview
`sprite` is the current-object sprite clip facade from `ModuEngine`.

### Members
- `HasClips()`
- `ClipCount()`
- `ClipIndex()`
- `SetClip(index)`
- `SetClip(name)`
- `ClipNameAt(index)`

### Why It Exists
Movement and feedback scripts often need to switch clips, but should not need to write clip-management boilerplate each time.

### Repository Uses
`TopDownMovement2D.moducpp` relies heavily on clip selection for:

- idle clip selection
- walking clip selection
- defensive fallback when some clips are missing

### Example
```cpp
if (running) {
    sprite.SetClip("Run");
} else {
    sprite.SetClip("Idle");
}
```

### Important Notes
- Name-based clips are easier to read.
- Index-based clips are common when the inspector stores fixed clip grids.
- If clips are authored by index, validating indices with `ctx.GetSpriteClipCount()` is a good defensive pattern.

## `ModuEngine`

### Overview
`ModuEngine` is the engine facade value exposed by `ModuEngine`.

### Most Used Member
- `FPS`

### Why It Exists
Scripts often need frame-rate readback for:

- simple UI
- performance labels
- tuning and debugging

### Example
```cpp
obj.UILabel = "FPS: " + IntR(ModuEngine.FPS);
```

## Numeric Helpers

### `IntRD(float)`
Rounds down.

### `IntR(float)`
Rounds to nearest.

### `IntRU(float)`
Rounds up.

### Why They Exist
The repository’s UI scripts demonstrate the need clearly: many values are calculated as floats, but should be displayed as stable whole numbers.

### Common Uses
```cpp
obj.UILabel = "FPS: " + IntR(ModuEngine.FPS);
```

```cpp
ctx.AddConsoleMessage("Elapsed: " + IntRD(timer));
```

## Math Helpers

### Members
- `Math::Max`
- `Math::Min`
- `Math::Clamp`
- `Math::Abs`

### Why They Exist
These helpers keep simple numeric logic readable in the high-level scripting layer.

Repository examples include:

- clamping selected option indices
- guarding minimum speeds
- comparing directional magnitudes
- forcing non-negative timing or distance

### Example
```cpp
const int optionIndex = Math.Clamp(selectedOptionIndex, 0, (int)options.size() - 1);
```

## Timer Helpers

### Script-Facing Forms
- `StartTimer(float&, interval)`
- `TimerReady(float&)`
- `TimerReady(float&, interval)`
- high-level shorthand: `timer.Start(interval)` and `timer.Ready()`

### Why They Exist
Periodic behavior is extremely common and should not require each script to maintain its own timing boilerplate.

### Repository Pattern
```cpp
void Begin() to timer.Start(interval);

void TickUpdate()
{
    if (!timer.Ready()) return;
    ctx.AddConsoleMessage("Pulse");
}
```

### Important Notes
- The timer helper is ideal for repeating behavior.
- Store the timer value as runtime state, usually in a private field.

## How These Helpers Fit Together
The repository scripts repeatedly combine these helpers in a predictable way:

- `input` decides intent
- `TryMoveRigidbody2D(...)` or other engine helpers apply behavior
- `sprite` and `audio` present the state
- `obj.UILabel` or `SetUITextLabel(...)` expose feedback
- math and timer helpers keep the surrounding logic small

That combination is the real value of the facade layer.

## Common Mistakes
- Using `obj.UILabel` for a UI text object that is not the current object.
- Treating `input.jump()` or `IsSubmitDown()` as one-shot events without edge detection.
- Assuming `audio` or `sprite` work when the current object lacks those components.
- Using raw `input.WASD()` when normalized intent was the correct movement input.

## Related APIs
- [Module: ModuCPP](module-moducpp.md)
- [Module: ModuInput](module-moduinput.md)
- [Module: ModuEngine](module-moduengine.md)
- [Common Patterns](../manual/common-patterns.md)
