# Module: ModuInput

## Summary
`ModuInput` is the script-facing input module.

It provides both direct polling and higher-level intent helpers used throughout the repository:

- key constants
- `KeyDown(...)`
- `KeyPressed(...)`
- `IsRuntimeKeyDown(...)`
- `IsSubmitDown()`
- the `input` facade

This module exists so scripts can express gameplay intent clearly instead of rebuilding low-level input plumbing for every component.

## Syntax
```cpp
add ModuInput;
```

## Why This Module Exists
Most gameplay scripts do not really want to ask "is GLFW key X down?".

They want to ask:

- is the player moving?
- is the player sprinting?
- is jump currently held?
- was confirm pressed?

`ModuInput` gives the scripting layer a vocabulary for those questions.

## Key Constants

### Available Repository-Visible Constants
- `KEY_W`
- `KEY_A`
- `KEY_S`
- `KEY_D`
- `KEY_E`
- `KEY_UP`
- `KEY_DOWN`
- `KEY_LEFT`
- `KEY_RIGHT`
- `KEY_SHIFT_LEFT`
- `KEY_SHIFT_RIGHT`
- `KEY_SPACE`
- `KEY_ENTER`
- `KEY_KP_ENTER`

### Why These Matter
They make direct input code readable when a script genuinely depends on a specific key, such as an interaction shortcut.

## `KeyDown(...)`

### What It Does
Returns whether a specific key is currently held.

### Why It Exists
Some scripts truly want direct key ownership rather than a generalized intent helper.

### When To Use It
Use it for:

- interaction keys such as `E`
- one-off shortcuts
- tool-mode or debug-mode controls

### Example
```cpp
if (KeyDown(KEY_E)) {
    ctx.AddConsoleMessage("Interact");
}
```

## `KeyPressed(...)`

### What It Does
Returns a press event for a specific key instead of a continuous held state.

### Why It Exists
Some logic is specifically key-driven rather than intent-driven, and should only react once when the key is pressed.

### When To Use It
Use it for:

- one-shot shortcuts
- toggle buttons
- debug actions

If your script is already built around a higher-level helper such as `input.jump()` or `IsSubmitDown()`, an explicit previous-frame edge pattern may still read better.

## `IsRuntimeKeyDown(...)`

### What It Does
Checks key state across the runtime/editor input path.

### Why It Exists
Repository scripts such as `MainMenuController.moducpp` use it when they need explicit control over exact key behavior and want a helper that still works cleanly in the runtime/editor context.

### When To Use It
Use it when:

- you need exact key control
- the `input` facade is too opinionated for the task
- you are building custom submit or navigation logic

## `IsSubmitDown()`

### What It Does
Returns the shared submit/confirm held state.

### Why It Exists
Dialogue and menu scripts often care about the concept of "submit" more than any one individual key.

That is why the repository uses it in `DialogueSystem.moducpp`.

### When To Use It
Use it for:

- dialogue advance
- menu confirm
- interaction confirm

### Important Note
`IsSubmitDown()` is still a held-state helper. If the action should happen only once, turn it into an edge:

```cpp
const bool submitDown = IsSubmitDown();
const bool submitPressed = submitDown && !prevSubmitDown;
prevSubmitDown = submitDown;
```

## The `input` Facade

### Overview
The `input` facade is the most common `ModuInput` surface in gameplay scripts.

### `input.WASD()`
Returns raw 2D movement intent.

Use it when:

- you want direct axis values
- you will normalize or scale the vector yourself

### `input.WASDNormalized()`
Returns normalized 2D movement intent.

Why a script author chooses it:

- diagonal movement should not be faster than straight movement
- the vector will directly feed a movement helper

Repository example:

```cpp
Vector2 move = input.WASDNormalized();
TryMoveRigidbody2D(ctx, move * speed, acceleration, drag, dt);
```

This is the normal pattern in `TopDownMovement2D.moducpp`.

### `input.sprint()`
Returns whether sprint is currently held.

Repository example:

```cpp
bool running = input.sprint();
float speed = running ? runSpeed : walkSpeed;
```

### `input.jump()`
Returns whether jump is currently held.

Repository example:

```cpp
bool jumpNow = input.jump();
```

This is often paired with previous-frame tracking when the script needs a one-shot jump event.

## Two Common Repository Patterns

### 1. Movement Intent
```cpp
add ModuCPP;
add ModuInput;
add ModuEngine;

public class PlayerMover : ModuNode
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

### 2. One-Shot Submit Or Action
```cpp
add ModuCPP;
add ModuInput;

public class ConfirmExample : ModuNode
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

## Common Mistakes
- Forgetting `add ModuInput;`.
- Using `input.WASD()` when normalized movement was intended.
- Treating held-state helpers as one-shot events.
- Rebuilding WASD logic manually when the facade already matches the behavior.

## Related APIs
- [Facades and Helper Types](type-facades-and-helpers.md)
- [Module: ModuEngine](module-moduengine.md)
- [Input and Engine Scripting](../manual/input-and-engine-scripting.md)
