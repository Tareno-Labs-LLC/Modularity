# ModuNode and ModuBehaviour

## Summary
`ModuNode` and `ModuBehaviour` are the two high-level base names recognized by the current ModuCPP transpiler.

They are less about classical inheritance and more about authoring shape: they tell the transpiler "this file is a high-level ModuCPP script class".

## Syntax
```cpp
public class MyScript : ModuNode
```

```cpp
public class MyScript : ModuBehaviour
```

## Why These Base Names Exist
When the transpiler sees one of these bases, it knows the file should be treated as a high-level script. That allows it to:

- recognize hooks such as `Begin()` and `TickUpdate()`
- collect public and private fields
- generate inspector behavior
- provide the high-level `ctx` and `obj` model

That is the real reason these bases matter.

## `ModuNode`

### What It Represents
`ModuNode` is the preferred current documented base for new gameplay-oriented examples.

### Why A Script Author Chooses It
Choose `ModuNode` when:

- writing a new gameplay script
- following the newer documentation style
- building a normal component with public fields, lifecycle hooks, and optional declarative inspector layout

### Repository-Style Use
The repository’s larger gameplay-style examples use `ModuNode`:

- `DialogueSystem.moducpp`
- `InteractableObject.moducpp`
- `MainMenuController.moducpp`
- `TopDownMovement2D.moducpp`

### Example
```cpp
add ModuCPP;

public class AutoEnable : ModuNode
{
    public bool startEnabled = true;

    void Begin()
    {
        ctx.SetObjectEnabled(startEnabled);
    }
}
```

## `ModuBehaviour`

### What It Represents
`ModuBehaviour` remains a supported high-level base and is common in older-style or tool-oriented scripts.

### Why A Script Author Chooses It
Choose `ModuBehaviour` when:

- maintaining or porting older content
- keeping consistency with existing scripts
- writing tool-like or inspector-heavy examples that already follow the older naming style

### Repository-Style Use
The repository’s manual-inspector and tool samples commonly use `ModuBehaviour`:

- `SampleInspector.moducpp`
- `SampleInspector Simplified.moducpp`
- `RigidbodyTest.moducpp`
- `EditorWindowSample.moducpp`
- `AnimationWindow.moducpp`
- `StandaloneMovementController.moducpp`

### Example
```cpp
add ModuCPP;
add ModuEngine;

public class SampleInspector : ModuBehaviour
{
    public bool autoRotate = false;

    void TickUpdate()
    {
        if (!autoRotate) return;
        ctx.AddConsoleMessage("Rotating");
    }
}
```

## Do They Expose Different Hook Sets?
In practical repository usage, no major hook split is the important point.

Both bases participate in the same general high-level hook model:

- `Begin()`
- `TickUpdate()`
- `Spec()`
- `TestEditor()`
- `Script_OnInspector()`
- `RenderEditorWindow()`
- `ExitRenderEditorWindow()`

The main difference is authoring style, compatibility, and documentation preference.

## A Useful Rule Of Thumb
Use:

- `ModuNode` for new gameplay-facing scripts
- `ModuBehaviour` when preserving continuity with existing scripts or inspector/tool-oriented examples

That rule matches the repository well.

## Common Mistakes
- Thinking these are large API-rich bases in the traditional OOP sense.
- Assuming `ModuBehaviour` is invalid just because newer docs prefer `ModuNode`.
- Treating the base choice as a substitute for good lifecycle design.

## Related APIs
- [Hook Reference](hook-reference.md)
- [Methods and Lifecycle](../manual/methods-and-lifecycle.md)
- [Getting Started](../manual/getting-started.md)
