# Module: RMeshBuilder

## Summary
`RMeshBuilder` is a recognized ModuCPP import target, but the current repository does not expose an actual script-facing helper surface for it yet.

That makes it a real documented module name with a placeholder-level API.

## Syntax
```cpp
add RMeshBuilder;
```

## Description
The important thing to document here is what exists today, not what might exist later.

The module name is recognized by the transpiler and appears in the editor-facing scripting identifiers. However, the current `include/RMeshBuilderScriptApi.h` file does not expose script-facing functions, types, or helper facades beyond a placeholder comment.

In practice, that means:

- the import is valid
- the module is reserved for mesh-builder script helpers
- there is no current public script API to teach or depend on

## Members
No current script-facing members are exposed in the repository’s `RMeshBuilderScriptApi.h`.

## Behavior Explanation
This module behaves differently from the other documented modules because the meaningful behavior right now is absence of an exposed script surface.

That is still useful to document. It tells the reader:

- the module name is not a typo
- the docs are not missing a hidden API section
- if a script imports it today, that import is effectively a reservation for future functionality rather than an active helper dependency

## Example
### Placeholder import
```cpp
add ModuCPP;
add RMeshBuilder;

public class MeshBuilderPlaceholder : ModuNode
{
    void Begin()
    {
        ctx.AddConsoleMessage("RMeshBuilder currently has no script-facing helpers.");
    }
}
```

### When not to use it
If a script needs:

- object references
- timers
- input polling
- movement helpers
- editor widgets

then the needed modules are almost certainly `ModuCPP`, `ModuEngine`, `ModuInput`, or `ModuCPP.Experimental`, not `RMeshBuilder`.

## Remarks
- Treat the module as reserved API surface.
- Do not invent mesh-building helpers in project docs until the header actually exposes them.
- If the mesh-builder module grows later, this page should expand from “placeholder import” into a normal module reference page.

## Related APIs
- [Imports and Modules](../manual/imports-and-modules.md)
- [ModuCPP Overview](../README.md)
