# Writing Clean ModuCPP Scripts

## Overview
Clean ModuCPP scripts are not just shorter scripts. They are scripts whose structure makes their runtime behavior obvious.

A good script answers a few questions quickly:

- what does the designer configure?
- what does the runtime calculate?
- which modules does the script depend on?
- where does setup happen?
- where does per-frame behavior happen?

That is the standard this page is aiming for.

## Why This Feature Exists
ModuCPP makes it easy to author scripts quickly. That is useful, but it also creates a risk: if the high-level syntax is treated as a shortcut for everything, scripts can become dense and unclear instead of concise and readable.

This page exists to keep the scripting style aligned with the engine’s real runtime model:

- clear data ownership
- explicit imports
- predictable hooks
- minimal duplication

## When to Use This
Use this page when you are:

- starting a new script
- cleaning up a prototype
- porting from raw native `C++`
- reviewing whether a script is growing too many responsibilities

## How It Works
Clean ModuCPP scripts usually share a few traits:

- imports are explicit and minimal
- public fields are configuration
- private fields are runtime state
- lifecycle methods have one clear job each
- reusable helpers live in shared headers or clearly scoped support code
- editor behavior is separated from runtime behavior

## A Script That Is Too Blurry
This kind of script compiles, but it does not communicate intent well:

```cpp
public float interval = 0.5f;
public float timer = 0.0f;
public bool initialized = false;
public int counter = 0;
```

The problem is not syntax. The problem is that runtime details and configuration are mixed together. A future reader cannot tell what should be edited in the inspector and what only exists to make the logic work.

## A Cleaner Version
```cpp
add ModuCPP;

public class Pulse : ModuNode
{
    public float interval = 0.5f;

    private float timer = 0.0f;
    private int counter = 0;

    void Begin() to timer.Start(interval);

    void TickUpdate()
    {
        if (!timer.Ready()) return;
        counter++;
        ctx.AddConsoleMessage("Pulse");
    }
}
```

This version is cleaner because:

- configuration is easy to identify
- runtime state is private
- timing behavior is expressed through built-in helpers
- the hook responsibilities are clear

## Another Common Cleanup: Over-Importing
It is easy to leave unnecessary imports in a script while prototyping.

Instead of this:

```cpp
add ModuCPP;
add ModuEngine;
add ModuInput;
add ModuCPP.Experimental;
```

use only what the script needs. If the script is only a timer and status logger, `ModuCPP` alone may be enough.

This matters because imports are dependency information, not decoration.

## Real-World Use Case: Small Gameplay Script
For a small gameplay script, cleanliness usually means one behavior, one set of clear fields, and straightforward hooks.

```cpp
add ModuCPP;
add ModuEngine;

public class StatusLight : ModuNode
{
    public string activeText = "On";
    public string inactiveText = "Off";
    public bool active = false;

    void TickUpdate()
    {
        obj.UILabel = active ? activeText : inactiveText;
    }
}
```

Why this reads well:

- the script purpose is obvious
- public data is all authoring-time data
- there is no hidden runtime complexity

## Real-World Use Case: Slightly Larger Script
As scripts grow, cleanliness becomes more about separation of concerns than about line count.

```cpp
add ModuCPP;
add ModuInput;
add ModuEngine;

public class PlayerMover2D : ModuNode
{
    public float walkSpeed = 4.0f;
    public float runSpeed = 7.0f;
    public float acceleration = 18.0f;
    public float drag = 8.0f;

    private bool running = false;

    void TickUpdate()
    {
        Vector2 move = input.WASDNormalized();
        running = input.sprint();

        float speed = running ? runSpeed : walkSpeed;
        TryMoveRigidbody2D(ctx, move * speed, acceleration, drag, dt);
    }
}
```

The script is larger, but it is still clean because:

- field names describe intent
- imports match the feature set
- private state exists only when it helps readability

## Best Practices
- Prefer `ModuNode` for new scripts.
- Keep imports explicit and minimal.
- Use public fields for authoring data and private fields for runtime state.
- Keep `Begin()` for setup and `TickUpdate()` for continuous behavior.
- Use the helper facades when they make the script easier to read.
- Move reusable helper code into shared headers instead of duplicating it across scripts.
- Keep editor-only behavior in inspector or editor-window hooks.
- Preserve runtime behavior when refactoring.

## Related Pages
- [Common Patterns](common-patterns.md)
- [Fields and Inspector](fields-and-inspector.md)
- [Troubleshooting](troubleshooting.md)
