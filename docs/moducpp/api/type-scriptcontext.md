# ScriptContext

## Summary
`ScriptContext` is the engine-side runtime object behind almost every script-facing action.

Even when you write high-level code with `obj`, `audio`, `sprite`, `input`, or timer helpers, those conveniences ultimately sit on top of `ScriptContext`.

The repository scripts use `ctx` constantly for:

- scene lookups
- transform edits
- rigidbody queries and commands
- UI and sprite operations
- audio playback
- settings persistence
- editor utilities
- dirty-state tracking

## Syntax
```cpp
ScriptContext& ctx
```

In high-level hooks, `ctx` is provided automatically.

## Why `ScriptContext` Exists
The scripting layer needs a stable bridge into the engine world:

- which object is this script attached to?
- how do I find other objects?
- how do I move or query rigidbodies?
- how do I write UI or sprite state?
- how do I persist settings or mark data dirty?

`ScriptContext` is that bridge.

## Core Fields

### `object`
The currently attached scene object.

Repository pattern:

```cpp
if (!ctx.object) return;
```

This guard appears frequently because many runtime and editor operations only make sense when a real attached object exists.

### `script`
The current script component instance.

This becomes important when settings or script-to-script coordination operate at the script-component level rather than only the object level.

## Scene Access And Object Resolution

### Members
- `FindObjectByName`
- `FindObjectById`
- `ResolveObjectRef`
- `IsObjectEnabled`
- `SetObjectEnabled`
- `GetSelectedObjectId`

### Why These Matter
Real scripts rarely operate only on themselves. They often need:

- a player object
- a dialogue root
- selected objects in the editor
- targets referenced by name or id

### Repository Uses
- `AnimationWindow.moducpp` finds target objects by id or name
- manual inspectors resolve target objects for buttons such as "Nudge Target"
- shared dialogue helpers search object hierarchies by id

### When To Use Them
Use `ctx` scene lookups when:

- the target is not the current object
- the relationship is runtime-driven
- you need more control than a facade provides

## Transform And Scene Editing

### Members
- `SetPosition`
- `SetPosition2D`
- `SetRotation`
- `SetScale`

### Why They Matter
These are the normal engine-facing ways to move or edit the current object from script code.

### Repository Uses
- `SampleInspector.moducpp` applies offsets and rotation
- `AnimationWindow.moducpp` writes interpolated position, rotation, and scale
- `EditorWindowSample.moducpp` nudges the selected object

### Why A Script Author Chooses These Instead Of Raw Field Mutation
Because these methods express the intended operation clearly and align with engine-side behavior better than arbitrarily patching object fields in ad hoc ways.

## Physics Helpers

### Members
- `HasRigidbody`
- `HasRigidbody2D`
- `EnsureCapsuleCollider`
- `EnsureRigidbody`
- `SetRigidbody2DVelocity`
- `GetRigidbody2DVelocity`
- `SetRigidbodyVelocity`
- `GetRigidbodyVelocity`
- `SetRigidbodyAngularVelocity`
- `TeleportRigidbody`
- `BindStandaloneMovementSettings`
- `DrawStandaloneMovementInspector`
- `TickStandaloneMovement`

### Why These Matter
The repository uses `ctx` heavily when physics is the behavior rather than just an implementation detail.

### 2D Rigidbody Cases
Use:

- `HasRigidbody2D`
- `GetRigidbody2DVelocity`
- `SetRigidbody2DVelocity`

when the script is handling 2D movement directly or through `TryMoveRigidbody2D(...)`.

### 3D Rigidbody Cases
Use:

- `HasRigidbody`
- `SetRigidbodyVelocity`
- `GetRigidbodyVelocity`
- `SetRigidbodyAngularVelocity`
- `TeleportRigidbody`

when the script needs 3D rigidbody control, as shown in `RigidbodyTest.moducpp` and `SampleInspector.moducpp`.

### Setup Enforcement
`StandaloneMovementController.moducpp` demonstrates another important `ctx` use:

```cpp
if (config.settings.enforceCollider) {
    ctx.EnsureCapsuleCollider(config.settings.capsuleTuning.x, config.settings.capsuleTuning.y);
}
if (config.settings.enforceRigidbody) {
    ctx.EnsureRigidbody(true, false);
}
```

This is a good example of `ScriptContext` as both runtime and setup utility surface.

## UI And Sprite Operations

### Members
- `SetUILabel`
- `SetFPSCap`
- `GetSpriteClipCount`
- `GetSpriteClipIndex`
- `GetSpriteClipNameAt`
- `SetSpriteClipIndex`
- `SetSpriteClipName`

### Why These Matter
These are the lower-level engine operations behind helpers such as:

- `obj.UILabel`
- `sprite.SetClip(...)`
- FPS display logic

### Repository Uses
- `FPSDisplay.moducpp` uses `ctx.SetFPSCap(...)`
- `TopDownMovement2D.moducpp` checks `ctx.GetSpriteClipCount()` while validating clip indices

### When To Use Them Directly
Use these methods directly when:

- the helper facade is too limited for the job
- the script needs explicit validation or lower-level clip inspection

## Audio And Animation

### Audio Members
- `HasAudioSource`
- `PlayAudio`
- `StopAudio`
- `PlayAudioOneShot`

### Animation Members
- `HasAnimation`
- `PlayAnimation`
- `StopAnimation`
- `PauseAnimation`
- `ReverseAnimation`
- `SetAnimationTime`
- `GetAnimationTime`
- `IsAnimationPlaying`

### Why These Matter
The repository’s larger scripts use audio and animation as state feedback, not only as manual authoring data.

Repository examples:

- `DialogueSystem.moducpp` plays open, close, skip, and typing sounds
- menu and interaction samples play move/select/trigger sounds

Use `ctx` directly when:

- you need explicit clip playback rather than the current-object `audio` facade
- the script coordinates animation transitions directly

## Settings And Persistence

### Members
- `GetSetting`
- `SetSetting`
- `GetSettingBool`
- `SetSettingBool`
- `GetSettingFloat`
- `SetSettingFloat`
- `GetSettingVec3`
- `SetSettingVec3`
- `AutoSetting`
- `SaveAutoSettings`

### Why These Matter
Scripts need both author-facing persistence and script-to-script protocol storage.

Repository uses include:

- legacy-data migration in `MainMenuController.moducpp`
- interaction requests in `DialogueSystem.moducpp`
- manual inspector persistence in `SampleInspector.moducpp`

### `AutoSetting(...)`
This is the basis of many manual-inspector persistence helpers.

Why a script author chooses it:

- the value should persist like a normal setting
- the script is drawing the widget manually

### `SaveAutoSettings()`
This commits manual-inspector auto-setting changes after widgets report a change.

This is why repository manual inspectors almost always follow:

```cpp
if (changed) {
    ctx.SaveAutoSettings();
}
```

## Dirty-State Tracking

### `MarkDirty()`
This is one of the most important practical `ScriptContext` methods for editor and authoring workflows.

### Why It Exists
Some helper methods already mark dirty internally when they modify scene data. But if a script directly changes serialized object data, it should mark the object dirty so the editor knows something meaningful changed.

### Repository Uses
- `EditorWindowSample.moducpp`
- `AnimationWindow.moducpp`
- migration code in `MainMenuController.moducpp`
- interaction-setting updates in `InteractableObject.moducpp`

### When To Use It
Use `ctx.MarkDirty()` when:

- the script directly mutates scene or script data
- the change should be considered a real authored modification

## Console And Tool Utilities

### `AddConsoleMessage(...)`
Use this when the script should communicate runtime or tool feedback clearly.

Repository uses:

- missing-action warnings
- missing-dialogue-target warnings
- tool-window log messages
- validation messages

### Standalone Movement Utilities
The standalone movement helper surface exposed by `ctx` is documented separately, but it is worth noting here because the repository uses it as a complete subsystem pattern:

- bind settings
- draw built-in inspector
- tick runtime movement

## What `ctx` Looks Like In Real Scripts
The repository shows a useful rule of thumb:

- use facades when the job is simple and local
- use `ctx` when the script needs explicit engine-side control

That is why even high-level scripts still use `ctx` constantly. It is the real runtime surface, and the facades are just the most common shortcuts.

## Common Mistakes
- Forgetting to guard `ctx.object` before object-dependent operations.
- Editing config in a manual inspector without `SaveAutoSettings()`.
- Mutating scene data directly without `MarkDirty()`.
- Treating helper-safe failures, such as missing rigidbodies, like proof that the logic is wrong.

## Related APIs
- [Facades and Helper Types](type-facades-and-helpers.md)
- [Module: ModuEngine](module-moduengine.md)
- [Standalone Movement API](type-standalone-movement.md)
- [Methods and Lifecycle](../manual/methods-and-lifecycle.md)
