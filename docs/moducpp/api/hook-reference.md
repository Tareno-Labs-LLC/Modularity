# Hook Reference

## Summary
Hooks are the named methods through which a high-level ModuCPP script participates in runtime behavior, inspector behavior, and editor-tool behavior.

They are compile-time recognized entry points. That is why their names matter.

## Syntax
```cpp
void Begin()
void TickUpdate()
void Script_OnInspector()
void RenderEditorWindow()
```

## Why Hooks Exist
Scripts need different places for different jobs:

- setup should run once
- gameplay logic should run every frame
- inspector UI should only run while the inspector is being drawn
- editor-window tools should run in editor windows, not in gameplay hooks

The repository scripts follow this separation closely, and that is the best way to read the hook system.

## Runtime Hooks

### `Begin()`
One-time setup hook.

Use it for:

- starting timers
- resetting runtime state
- one-time migration
- setup validation
- ensuring required components

Repository-style examples:

- `void Begin() to timer.Start(interval);`
- resetting state in `DialogueSystem.moducpp`
- ensuring collider/rigidbody setup in `StandaloneMovementController.moducpp`

### `TickUpdate()`
Primary frame-driven gameplay hook.

Use it for:

- input polling
- movement
- timer checks
- UI updates
- state-machine progress

Repository-style examples:

- `TopDownMovement2D.moducpp`
- `DialogueSystem.moducpp`
- `InteractableObject.moducpp`
- `FPSDisplay.moducpp`

### `Update()`
Alternate update-style runtime hook.

Use it mainly for:

- older compatibility-oriented scripts
- codebases that intentionally prefer the older naming

Current docs and shipped high-level examples generally prefer `TickUpdate()`.

### `Spec()`
Extra runtime hook for spec-mode behavior.

Use it when:

- the same behavior should run in spec mode

Repository-style example:

- `SampleInspector.moducpp` reuses the same rotation helper in `Spec()`

### `TestEditor()`
Extra runtime hook for editor test-mode behavior.

Use it when:

- behavior should run in editor test execution without being part of inspector drawing

Repository-style example:

- `SampleInspector.moducpp`

## Inspector Hook

### `Script_OnInspector()`
Manual per-object inspector hook.

Use it when:

- the declarative inspector DSL is not enough
- the inspector needs buttons, custom widgets, or immediate actions
- config is stored through `Config<T>()` plus `BindSetting(...)`

Repository-style examples:

- `SampleInspector.moducpp`
- `RigidbodyTest.moducpp`
- `StandaloneMovementController.moducpp`

## Editor Window Hooks

### `RenderEditorWindow()`
Standalone editor-tool drawing hook.

Use it when:

- the script acts like a tool window instead of a component inspector

Repository-style examples:

- `EditorWindowSample.moducpp`
- `AnimationWindow.moducpp`

### `ExitRenderEditorWindow()`
Editor-window cleanup or close hook.

Use it when:

- the tool needs cleanup or state reset when the window closes

An empty implementation is fine when no cleanup is needed.

## What Hooks Get In High-Level Scripts
In high-level scripts, the recognized hooks automatically receive the common script-facing environment:

- `ctx`
- `obj`
- `dt` in update-style hooks
- `time.deltaTime`

That is why hook bodies can stay focused on behavior.

## One-Line `to` Form
Hooks and helper methods can use a one-line `to` form:

```cpp
void Begin() to timer.Start(interval);
```

Use it when the body is truly tiny and obvious.

## Common Mistakes
- Putting initialization into `TickUpdate()` when it belongs in `Begin()`.
- Mixing editor-tool drawing into runtime hooks.
- Treating held input as one-shot behavior inside `TickUpdate()` without edge detection.
- Forgetting that manual inspectors need explicit persistence helpers.

## Related APIs
- [Methods and Lifecycle](../manual/methods-and-lifecycle.md)
- [Editor Scripting](../manual/editor-scripting.md)
- [ScriptContext](type-scriptcontext.md)
