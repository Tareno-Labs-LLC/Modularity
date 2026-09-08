# Module: ModuCPP.Experimental

## Summary
`ModuCPP.Experimental` is the shared helper module for advanced script-facing workflows that are visible in the repository’s larger sample systems.

It covers:

- parsing and conversion helpers
- script-setting helpers
- object-reference serialization and resolution
- object and UI state helpers
- a set of editor widgets for reference-heavy authoring

This module is especially important for the repository’s more complex scripts:

- `DialogueSystem.moducpp`
- `InteractableObject.moducpp`
- `MainMenuController.moducpp`

## Syntax
```cpp
add ModuCPP.Experimental;
```

## Why This Module Exists
Some problems show up often in real scripts, but not in every tiny script:

- storing object references as data
- resolving those references safely at runtime
- serializing structured arrays into script settings
- writing UI labels to referenced text objects
- toggling groups of referenced objects

Those workflows are too common to leave to copy-paste boilerplate, but also too specialized to treat as minimal core language features. That is the role of the experimental module.

## Parsing And Conversion Helpers

### Members
- `Trim(...)`
- `ParseInt(...)`
- `ParseFloat(...)`
- `ParseBool(...)`
- `EscapeField(...)`
- `UnescapeField(...)`
- `SplitEscaped(...)`
- `JoinEscaped(...)`

### Why They Exist
The larger repository scripts read and write structured values through settings strings.

Examples:

- legacy data migration in `MainMenuController.moducpp`
- interaction request handling in `DialogueSystem.moducpp`
- sub-data migration in `InteractableObject.moducpp`

### When To Use Them
Use these helpers when:

- a value must be read from `ctx.GetSetting(...)`
- structured data is stored as an encoded string
- user-authored or older saved data needs safe parsing with fallback behavior

### Example
```cpp
const int interactionSerial = ParseInt(ctx.GetSetting(kInteractionRequestSerial, "0"), 0);
const bool interactionPending = ParseBool(ctx.GetSetting(kInteractionRequestPending, "0"), false);
```

This is a good example of why these helpers matter. They keep settings-driven logic tolerant and readable.

## Script Setting Helpers

### Members
- `GetScriptSetting(...)`
- `SetScriptSetting(...)`

### Why They Exist
Sometimes a script needs to read or modify another script component’s settings rather than its own field data.

That is exactly what `InteractableObject.moducpp` does when sending a request into a target `DialogueSystem` script.

### When To Use Them
Use them when:

- one script is coordinating another script instance
- the target is a `ScriptComponent*` rather than the current `ctx`
- a shared protocol is implemented through script settings

## Object Reference Helpers

### Members
- `DeserializeObjectRefs(...)`
- `SerializeObjectRefs(...)`
- `MakeObjectRef(...)`
- `ResolveSceneObjectRef(...)`
- `ResolveUITextTarget(...)`
- `IsAllDigits(...)`

### Why They Exist
Object references are one of the main recurring data shapes in the repository.

Scripts store them as:

- single string refs
- string arrays
- serialized object-ref lists in settings blobs

These helpers exist so those references can move cleanly between inspector data, serialized settings, and live runtime objects.

### `ResolveSceneObjectRef(...)`

#### What It Does
It tries to resolve a script-facing object reference string into a live scene object.

#### Why A Script Author Chooses It
Because object refs are often stored as strings for authoring reasons, but runtime logic needs actual objects.

#### Repository Use
This helper appears everywhere in the larger samples:

- dialogue target resolution
- player distance checks
- menu item lookups
- mouth object lookups

### `ResolveUITextTarget(...)`

#### What It Does
It resolves a reference into the most useful UI text object target rather than only the exact object.

#### Why It Exists
Many real UI scripts want to say "write to the text associated with this object hierarchy", not manually traverse parents and children every time.

That is why higher-level helpers such as `SetUITextLabel(...)` and `SetUITextEffects(...)` are built on top of it.

## Object And UI State Helpers

### Members
- `SetObjectEnabledState(...)`
- `SetObjectsEnabledState(...)`
- `GetObjectReferencePosition(...)`
- `GetCurrentObjectName(...)`
- `TryPlayAnimationClipNamed(...)`
- `SetUITextLabel(...)`
- `SetUITextEffects(...)`
- `SetRigidbody2DSimulated(...)`

### `SetObjectEnabledState(...)` And `SetObjectsEnabledState(...)`

#### What They Do
They enable or disable one resolved object or a group of references, and mark dirty when a real change occurs.

#### Why They Exist
The repository’s gameplay scripts repeatedly coordinate scene object groups:

- enable these objects
- disable those objects
- flip selection visuals
- apply dialogue consequences

That is common enough to deserve dedicated helpers.

#### Repository Uses
- selection visuals in `InteractableObject.moducpp`
- dialogue line object toggles in `DialogueSystem.moducpp`
- menu actions in `MainMenuController.moducpp`

### `GetObjectReferencePosition(...)`

#### What It Does
Returns a useful world-space-like position for a referenced object, including UI-aware handling.

#### Why It Exists
Scripts such as `InteractableObject.moducpp` need distance checks between authored references, and some of those references may be UI objects rather than plain world objects.

This helper keeps that logic centralized.

### `SetUITextLabel(...)`

#### What It Does
Writes a label to a referenced UI text target and marks dirty if the value actually changed.

#### Why A Script Author Chooses It
Because the target text object is not always `ctx.object`.

#### Repository Use
`DialogueSystem.moducpp` uses this pattern extensively for character name and dialogue text widgets.

### `SetUITextEffects(...)`

#### What It Does
Writes text effect flags and tuning values to a referenced UI text target.

#### Why It Exists
The dialogue sample needs visual text effects as part of authored dialogue behavior, not just raw label changes.

### `TryPlayAnimationClipNamed(...)`

#### What It Does
Attempts to play an animation clip on the current object by name.

#### Repository Use
`DialogueSystem.moducpp` uses it for named dialogue open and close animation transitions.

### `SetRigidbody2DSimulated(...)`

#### What It Does
Finds a referenced object and toggles its 2D rigidbody simulation state.

#### Repository Use
The dialogue sample uses this helper to temporarily disable player movement while dialogue is active.

## Editor Widgets

### Members
- `DrawStdStringInput(...)`
- `DrawObjectRefInput(...)`
- `DrawAudioClipInput(...)`
- `DrawObjectRefListEditor(...)`
- `IsAudioClipPath(...)`

### Why They Exist
Advanced reference-heavy inspectors need more than primitive text boxes.

These helpers exist so custom editors can reuse consistent object-ref and clip-path editing behavior rather than rebuilding it in every sample system.

## What This Module Looks Like In Real Scripts
The repository’s larger scripts show a common pattern:

1. authored refs and settings are stored as strings or structured arrays
2. experimental helpers serialize, parse, or resolve them
3. runtime helpers act on the resulting objects or UI targets

That is the real teaching model for this module.

## Common Mistakes
- Forgetting the import and assuming these helpers come from `ModuCPP`.
- Using free-text strings for object refs without `[ObjectRef]` or `[ObjectList]`.
- Reimplementing object-group toggling or UI-text resolution by hand.
- Treating shared sample helpers such as `DialoguePort` as if they were part of the core module surface.

## Related APIs
- [SceneObj](type-sceneobj.md)
- [DialoguePort Namespace](namespace-dialogueport.md)
- [Common Patterns](../manual/common-patterns.md)
- [Experimental Features](../manual/experimental-features.md)
