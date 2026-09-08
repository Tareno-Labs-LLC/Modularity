# Troubleshooting

## Overview
Most ModuCPP problems fall into a few practical buckets:

- the script structure is not being recognized
- the wrong module is missing
- the inspector is editing the wrong kind of field
- a helper is correct but the scene or component setup is incomplete
- a manual inspector or editor tool changed data without persisting it correctly

This page uses the repository’s real script patterns to explain how those failures usually look.

## A Good Debugging Order
When a script is not behaving correctly, check in this order:

1. script structure and hook placement
2. module imports
3. field visibility and inspector mode
4. scene references and required components
5. persistence and dirty-state behavior
6. runtime logic in the active hook

That order usually narrows the problem quickly.

## Problem: "No ModuCPP class found"

### What It Usually Means
The transpiler did not find a recognized high-level class declaration.

It expects a form such as:

```cpp
public class MyScript : ModuNode
```

or:

```cpp
public class MyScript : ModuBehaviour
```

### What To Check
- Is the file meant to be a high-level ModuCPP script at all?
- Does it declare a class using `ModuNode` or `ModuBehaviour`?
- Are imports at the top of the file instead of interleaved oddly through the body?

## Problem: A Helper Or Facade Is Missing

### What It Usually Means
The helper belongs to a module that was never imported.

### Common Cases
- `input`, `KeyDown(...)`, and `IsSubmitDown()` need `add ModuInput;`
- `TryMoveRigidbody2D(...)`, `audio`, `sprite`, `ModuEngine.FPS`, and `EditString(...)` need `add ModuEngine;`
- `ResolveSceneObjectRef(...)`, `SetObjectsEnabledState(...)`, `SetUITextLabel(...)`, and parsing helpers such as `ParseBool(...)` need `add ModuCPP.Experimental;`

### Why This Happens
Repository scripts are intentionally explicit about their imports because these helpers do not come from `add ModuCPP;` by default.

## Problem: `AutoFields(...)` Cannot See A Value

### What It Usually Means
`AutoFields(...)` only works with persisted public fields declared on the current high-level class.

It does not work for:

- private runtime state
- values stored only inside `Config<T>()`
- local variables or helper structs

### Real Fixes
- Make the value a public field if it is true authored configuration.
- Keep it private and show it through `Run(...)` if it is runtime-only.
- Use `Script_OnInspector()` if the inspector is built around `Config<T>()` instead of public fields.

## Problem: Manual Inspector Changes Do Not Persist

### What It Usually Means
The script changed ImGui-driven values but did not save them through the auto-settings path.

### What To Check
- Did the manual inspector call `ctx.SaveAutoSettings()` when values changed?
- If the script uses `Config<T>()`, did it also call `BindSetting(...)` or a binding helper in the active hooks?

### Why This Shows Up In Real Scripts
`SampleInspector Simplified.moducpp` and `RigidbodyTest.moducpp` both demonstrate that `Config<T>()` is only half of the persistence story. The fields still need binding, and manual edits still need saving.

## Problem: Object Reference Does Not Resolve

### What It Usually Means
A stored object-reference string could not be resolved as:

- an explicit object ref
- a numeric id
- an exact object name

### What To Check
- Is the referenced object actually present?
- Was the object renamed after the setting was stored?
- Should the field be marked `[ObjectRef]` or `[ObjectList]`?
- Is the script using `ResolveSceneObjectRef(...)` when it should?

### Related Pattern
Scripts such as `DialogueSystem.moducpp`, `InteractableObject.moducpp`, and `MainMenuController.moducpp` are built around correct object-reference resolution. If those references are stale, the helpers cannot do their job.

## Problem: UI Label Changes Do Not Appear

### What It Usually Means
The script is writing to the wrong object or the wrong UI path.

### What To Check
- If the code uses `obj.UILabel`, is the current script object actually the UI text object you want?
- If the target text is elsewhere, should the script use `[ObjectRef]` plus `SetUITextLabel(...)` instead?
- Does the object reference resolve to a real UI text target?

### Common Real-World Cause
`obj.UILabel` is correct for a self-updating text object such as `FPSDisplay.moducpp`. It is not the right tool for a dialogue system that writes into separate referenced text widgets.

## Problem: 2D Movement Does Nothing

### What It Usually Means
`TryMoveRigidbody2D(...)` is being called correctly, but the object setup is incomplete.

### What To Check
- Does the object have a `Rigidbody2D`?
- Is `dt` greater than zero?
- Is the movement script attached to the intended object?

### Why This Matters
The helper fails safely instead of crashing. That is good behavior, but it also means a setup problem can look like a logic problem until you inspect the object.

## Problem: Rigidbody Helpers Fail In Tool Scripts

### What It Usually Means
The script expects a `Rigidbody`, but the selected object does not have one.

### Real Repository Example
`RigidbodyTest.moducpp` explicitly handles this with `ctx.HasRigidbody()` and `warnOnce(...)`.

### What To Check
- Does the object have a 3D rigidbody?
- Are you calling `SetRigidbodyVelocity(...)`, `TeleportRigidbody(...)`, or angular velocity helpers on an object that only has transform data?

## Problem: Audio Or Sprite Calls Do Nothing

### What It Usually Means
The current object does not have the required audio or sprite setup.

### What To Check
- Does the current object actually have an audio source when using `audio.PlayOneShot(...)`, `audio.Play()`, or `audio.Stop()`?
- Does it have sprite clips when using `sprite.SetClip(...)`?
- Is the requested clip index valid?

### Practical Tip
Repository scripts often guard or validate clip access explicitly when clips are authored by index.

## Problem: Direct Object Changes Seem To Work But Do Not Save

### What It Usually Means
The script mutated object data directly in an inspector or editor window but did not mark the scene object dirty.

### What To Check
- After direct edits such as position, rotation, scale, label, or enabled-state changes, did the script call `ctx.MarkDirty()`?
- If a helper already marks dirty internally, are you duplicating the behavior or bypassing it?

### Real Examples
`EditorWindowSample.moducpp` and `AnimationWindow.moducpp` both illustrate the pattern: after scene-editing operations, the tool marks the object dirty so the editor knows the data changed.

## Problem: A Tool Script Uses `Config<T>()`, But Runtime Behavior Reads Defaults

### What It Usually Means
The config was edited in the inspector, but the runtime hook did not bind settings before reading it.

### What To Check
- Is `bindConfig(ctx, config)` called in `TickUpdate()`, `Begin()`, `Spec()`, or `TestEditor()` as needed?
- Is the binding only happening inside `Script_OnInspector()`?

### Why This Happens
`Config<T>()` gives access to structured storage, but the actual field values still need to be synchronized through the binding path in each relevant hook.

## Problem: Toggle Helpers Or UI Text Helpers Are Missing

### What It Usually Means
The script forgot `add ModuCPP.Experimental;`.

### Common Missing Items
- `ResolveSceneObjectRef(...)`
- `SetObjectEnabledState(...)`
- `SetObjectsEnabledState(...)`
- `SetUITextLabel(...)`
- `SetUITextEffects(...)`
- `TryPlayAnimationClipNamed(...)`
- `ParseInt(...)`, `ParseFloat(...)`, `ParseBool(...)`

## Problem: Experimental Or Shared Helpers Are Being Treated As Core

### What It Usually Means
The script or docs are assuming a helper is universal when it is actually opt-in or shared-sample-specific.

### Examples
- `ModuCPP.Experimental` helpers are opt-in.
- `DialoguePortShared.h` provides shared sample-system helpers, not minimal core language features.

This matters when copying code between scripts. Bring the actual dependencies with you.

## Best Practices
- Diagnose missing helpers as import problems first.
- Diagnose "nothing happens" problems as scene or component setup problems next.
- Treat persistence bugs and dirty-state bugs as separate from gameplay logic bugs.
- Keep object-reference data clearly marked with `[ObjectRef]` and `[ObjectList]`.
- Reuse the same binding helper in every hook that reads `Config<T>()`.

## Related Pages
- [Methods and Lifecycle](methods-and-lifecycle.md)
- [Fields and Inspector](fields-and-inspector.md)
- [Input and Engine Scripting](input-and-engine-scripting.md)
- [Module: ModuCPP.Experimental](../api/module-moducpp-experimental.md)
