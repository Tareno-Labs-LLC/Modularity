# Script Structure

## Overview
A ModuCPP script file is usually small, but it still has a clear structure. That structure is not just style. It reflects what the transpiler expects and how the engine separates script configuration, runtime state, helper code, and lifecycle hooks.

If you understand the shape of a typical file, the rest of the language becomes much easier to read.

## Why This Feature Exists
Without a consistent structure, high-level scripts become hard to reason about:

- it becomes unclear which parts are authoring syntax and which are native passthrough code
- inspector-facing data gets mixed with temporary runtime variables
- reusable helper code ends up duplicated inside unrelated methods
- it becomes difficult for the transpiler to generate clean support code

ModuCPP keeps the file format intentionally simple so common scripts stay readable and the generated native code stays predictable.

## When to Use This
Use the structure on this page whenever you are:

- starting a new script
- converting an older native-style script into high-level ModuCPP
- reviewing a script to see whether it is mixing too many responsibilities
- documenting or teaching ModuCPP to someone else

## How It Works
Most high-level scripts follow this shape:

1. module imports
2. optional `#include` lines for shared helpers
3. optional `public enum` and `SubScript` declarations
4. the `public class`
5. optional helper code outside the class, usually in `namespace { ... }`

Here is the most compact version of that structure:

```cpp
add ModuCPP;
add ModuInput;
add ModuEngine;

public enum FacingDirection { Down, Up, Right, Left }

SubScript FootstepBank {
    public string[] sounds;
};

public class TopDownMovement2D : ModuNode
{
    public float walkSpeed = 4.0f;
    private FacingDirection facing = FacingDirection.Down;

    void TickUpdate()
    {
        Vector2 move = input.WASDNormalized();
    }
}
```

That is enough for the transpiler to understand:

- which modules to import
- which types belong to the high-level script surface
- which fields should persist
- which fields should remain runtime-only
- which methods should become hooks

## The Core Pieces

### Imports
Imports always come first because they define the script-facing API surface available to the rest of the file.

```cpp
add ModuCPP;
add ModuInput;
```

This keeps the file explicit. A reader can tell immediately whether the script depends on input helpers, engine helpers, or experimental helpers.

### `public enum`
Use `public enum` when the script needs a small named set of choices that should remain readable in fields and logic.

```cpp
public enum MenuOrientation { Vertical, Horizontal }
```

Enums are especially useful when a script has a configuration choice that would otherwise become a magic integer.

### `SubScript`
Use `SubScript` for nested data that should still be editable and serializable through the ModuCPP tooling.

```cpp
SubScript MenuAction {
    public SceneObj[] enable;
    public SceneObj[] disable;
};
```

This is the right tool when a script needs repeated structured entries, such as dialogue lines, menu actions, or interaction options.

### The High-Level Class
The class is the actual script entry point. The current transpiler expects one of these forms:

```cpp
public class MyScript : ModuNode
```

or:

```cpp
public class MyScript : ModuBehaviour
```

For new documentation and new scripts, prefer `ModuNode`.

### Helper Code Outside the Class
Sometimes a script needs utility functions or private support structs that do not belong as instance methods. In those cases, a small anonymous namespace is the cleanest approach.

You can see this in several shipped scripts, especially when the script needs:

- runtime caches keyed by object id
- helper drawing functions
- conversion helpers
- custom support structs that are not public script fields

## A Simple Script Layout
This is a good structure for a script with no nested data and no custom inspector:

```cpp
add ModuCPP;
add ModuEngine;

public class AutoEnable : ModuNode
{
    public bool startEnabled = true;

    void Begin()
    {
        ctx.SetObjectEnabled(startEnabled);
    }
}
```

The file is short because the behavior is short. That is a good sign. ModuCPP should let simple scripts stay simple.

## A More Realistic Layout
This example shows a more representative file shape with modules, an enum, a `SubScript`, an inspector block, and a runtime helper outside the class.

```cpp
add ModuCPP;
add ModuInput;
add ModuCPP.Experimental;

public enum InteractableType { Dialogue, ToggleObjects }

SubScript InteractionOption
{
    public string optionName = "New Option";
    public int interactionType = 0;
    public string dialogueSystemRef;
    public string[] itemsToEnable;
    public string[] itemsToDisable;
}

namespace {

bool canRunInteraction(ScriptContext& ctx)
{
    return ctx.object != nullptr;
}

}

public class InteractableObject : ModuNode
{
    public bool canInteract = true;
    public InteractionOption[] options;
    private bool prevInteractDown = false;

    inspector
    {
        Tabs {
            Tab("Basics") {
                AutoFields(canInteract);
            }
            Tab("Options") {
                AutoFields(options);
            }
        }
    }

    void TickUpdate()
    {
        if (!canRunInteraction(ctx)) return;
    }
}
```

This is the kind of file structure you should expect in production-oriented scripts: the top of the file explains the API surface, the class defines instance behavior, and helper code lives nearby but outside the class when it is not instance state.

## Special Syntax You Will See

### One-Line `to` Methods
Use `to` when the body is genuinely a single expression or call and the short form improves readability.

```cpp
void Begin() to timer.Start(interval);
int FacingIndex(FacingDirection direction) to (int)direction;
```

This works well for tiny methods. Once a method needs meaningful branching or explanation, use a normal block body instead.

### `ref` Parameters
Some shipped scripts use `ref` parameters for helper methods that should mutate values passed from the caller.

```cpp
void TryMove(Vector2 targetVelocity, float dt, ref Vector2 actualVelocity)
```

This is useful when a helper should return more than one piece of information without inventing a separate support type.

### `each ... state(...)`
The `each someList.state(true);` shorthand exists for a very specific kind of repetitive script logic: applying enabled-state changes to object-reference lists.

```cpp
each enable.state(true);
each disable.state(false);
```

This is appropriate when the script already stores a list of scene object references and the behavior really is “turn everything in this list on” or “turn everything in this list off”.

## What Actually Happens
The file is not executed as-is. The transpiler reads the structure and turns it into native support code. That is why the layout matters so much:

- public data becomes generated config storage
- private data becomes generated state storage
- high-level hook methods become native exports
- inspector syntax becomes generated editor code

The clearer the file structure is, the more predictable that lowering step becomes.

## Best Practices
- Keep imports at the top and keep them explicit.
- Declare small enums when a field would otherwise become a magic integer.
- Use `SubScript` for structured repeated data, not for everything.
- Keep helper code outside the class only when it genuinely improves readability or reuse.
- Do not push every behavior into anonymous-namespace helpers just because the language allows it.
- Prefer a normal method body over `to` when the behavior deserves explanation.

## Related Pages
- [Getting Started](getting-started.md)
- [Fields and Inspector](fields-and-inspector.md)
- [Methods and Lifecycle](methods-and-lifecycle.md)
- [Transpilation Overview](transpilation.md)
