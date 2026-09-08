# Experimental Features

## Overview
`ModuCPP.Experimental` is an opt-in module for advanced helper behavior that is useful today but should not be mistaken for the stable minimal core of ModuCPP.

That distinction matters. A script that depends only on `ModuCPP`, `ModuInput`, and `ModuEngine` is relying on the regular documented module split. A script that imports `ModuCPP.Experimental` is making a stronger claim: it needs extra convenience helpers whose role is clear, but whose surface should still be documented carefully.

## Why This Feature Exists
Some scripting problems show up repeatedly in real projects, but they do not belong in the smallest possible core:

- serializing scene object references as strings
- resolving those references safely at runtime
- editing object reference lists in the inspector
- targeting UI text from scripts that only know an object reference string
- supporting dialogue, menu, and interaction workflows without forcing all of that into `ModuCPP`

The experimental module exists so those helpers can ship and be used, while still making it obvious that they are not the baseline every simple script should assume.

## When to Use It
Import `ModuCPP.Experimental` when a script needs:

- `[ObjectRef]` or `[ObjectList]` heavy workflows
- scene object references stored as strings
- UI text targeting through helper functions
- object list serialization or resolution
- helper widgets such as object-ref and audio-clip editors
- behavior similar to the shipped `DialogueSystem`, `MainMenuController`, or `InteractableObject` scripts

Do not import it automatically “just in case”. If a script does not need it, leaving it out keeps the dependency surface clearer.

## How It Works
Import the module explicitly:

```cpp
add ModuCPP.Experimental;
```

The module then exposes helpers in a few practical groups.

### String and Parsing Helpers
These solve the common problem of turning script settings back into useful runtime values.

- `Trim(...)`
- `ParseInt(...)`
- `ParseFloat(...)`
- `ParseBool(...)`
- escaped-field helpers for serialization

### Object Reference Helpers
These solve the common problem of storing scene object references in a way that survives script settings and can later be resolved again.

- `MakeObjectRef(...)`
- `SerializeObjectRefs(...)`
- `DeserializeObjectRefs(...)`
- `ResolveSceneObjectRef(...)`
- `SetObjectEnabledState(...)`
- `SetObjectsEnabledState(...)`

### UI Text and Object Helpers
These are useful when scripts need to find a UI text target indirectly or manipulate runtime state through object-reference strings.

- `SetUITextLabel(...)`
- `SetUITextEffects(...)`
- `ResolveUITextTarget(...)`
- `GetObjectReferencePosition(...)`

### Editor Widgets
These make object-reference-heavy scripts authorable without writing the full widget logic yourself.

- `DrawObjectRefInput(...)`
- `DrawObjectRefListEditor(...)`
- `DrawAudioClipInput(...)`

## The Problem It Solves
A plain string is a poor way to think about a scene object, but it is often how the data is stored in script settings or nested editor structures.

The experimental helpers close that gap:

- authoring stays convenient
- runtime resolution stays explicit
- complex scripts can still serialize and deserialize their object-driven data

This is why the module is heavily used by the more advanced sample scripts.

## Example: Object Reference Driven Toggle
This is one of the simplest practical uses of the module.

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

This is useful because the script can expose object lists in the inspector while still applying them cleanly at runtime.

## Example: UI Text Targeting
This is another common use case: a script needs to update UI text, but it stores only a reference string.

```cpp
add ModuCPP;
add ModuCPP.Experimental;

public class ToggleMessage : ModuNode
{
    [ObjectRef] public string labelRef;
    public bool active = false;

    void TickUpdate()
    {
        SetUITextLabel(ctx, labelRef, active ? "Active" : "Idle");
    }
}
```

This solves a real runtime problem: the script does not need to know whether the text object is the exact object referenced, a child, or a related target that the helper can resolve.

## Example: Dialogue-Style Script Support
The shipped dialogue and interaction scripts rely on the experimental helpers because they need more than a simple primitive inspector.

Typical uses include:

- serializing arrays of object refs into settings
- resolving dialogue target objects at runtime
- toggling groups of objects at the end of a dialogue line or interaction
- drawing object-ref editors for complex nested data

This is where the module earns its place. It keeps advanced scripts from having to duplicate all of that support code.

## Behavior Notes
- Experimental does not mean “fake” or “unused”. It means the helpers are useful but should be documented as opt-in rather than assumed core.
- The module is script-facing and real in the current repository.
- Some of the helpers exist primarily because they support shipped example systems that are more complex than a minimal gameplay script.

## Best Practices
- Import the module only when the script actually needs it.
- Document that dependency clearly when a script relies on it.
- Prefer stable core modules when the problem can be solved cleanly without experimental helpers.
- Use the experimental helpers especially when they remove duplicated object-reference or UI-targeting logic.

## Related Pages
- [Imports and Modules](imports-and-modules.md)
- [Module: ModuCPP.Experimental](../api/module-moducpp-experimental.md)
- [DialoguePort Namespace](../api/namespace-dialogueport.md)
