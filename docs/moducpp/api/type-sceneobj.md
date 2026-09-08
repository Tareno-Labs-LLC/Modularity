# SceneObj

## Summary
`SceneObj` is the high-level ModuCPP scene-object reference type used in script-facing field declarations. It is the type you use when a script needs to store references to other scene objects as part of its configurable data.

It is especially common in `SubScript` data, object toggle lists, menus, and interaction systems.

## Syntax
```cpp
public SceneObj[] enable;
public SceneObj[] disable;
public List<SceneObj*> targets;
```

## Description
At the high-level scripting layer, `SceneObj` answers a very practical question: how should a script say “this field refers to another object in the scene”?

The answer is not “store a raw native pointer in every public field”. Public field data needs to be editable, serializable, and restorable. `SceneObj` exists so high-level scripts can describe scene-object relationships in a way that fits those authoring needs.

In practice, this means:

- the field can participate in ModuCPP persistence
- the inspector can treat it as scene-object reference data
- helper code can later resolve it into a runtime object when the script runs

## Members / Parameters
`SceneObj` is primarily a script-facing declaration type. It is not documented as a rich runtime object wrapper with its own standalone methods.

The most important thing to understand is where it is used:

- public high-level fields
- `SubScript` fields
- object lists for enable/disable behavior
- menu and interaction payloads

Forms already used in the repository include:

- `SceneObj[]`
- `List<SceneObj*>`

## Behavior Explanation
`SceneObj` exists on the authoring side of the scripting system. At runtime, scripts usually operate on resolved scene objects through helpers such as:

- `ResolveSceneObjectRef(...)`
- `SetObjectsEnabledState(...)`
- `GetObjectReferencePosition(...)`

That is the important behavioral distinction:

- `SceneObj` is how you declare object relationships in high-level script data
- resolved scene objects are how you act on those relationships at runtime

This is why `SceneObj` is especially useful in persisted data and `SubScript` structures. It gives the authoring layer a clear way to represent scene links without pretending the saved field is already a live runtime pointer.

## When to Use It
Use `SceneObj` when:

- a script field should point to one or more scene objects
- the field should be editable in the inspector
- the field should persist with the script data
- the script will later resolve those objects and act on them

Use lower-level runtime object access when:

- you already have a live object from `ctx.object`, `FindObjectById`, or a resolver helper
- the value is runtime-only rather than persisted script configuration

## Example
### Object toggle lists
```cpp
add ModuCPP;
add ModuCPP.Experimental;

public class AutoEnable : ModuNode
{
    public SceneObj[] enable;
    public SceneObj[] disable;

    void Begin()
    {
        SetObjectsEnabledState(ctx, enable, true);
        SetObjectsEnabledState(ctx, disable, false);
    }
}
```

This is one of the most common uses of `SceneObj`: a small script that coordinates other objects without hard-coding object ids in its logic.

### Nested menu data
```cpp
SubScript MenuAction {
    public SceneObj[] enable;
    public SceneObj[] disable;
};
```

This is a good use case because the script needs structured nested data, and each entry must still be editable and serializable.

## Remarks
- Use `SceneObj` for high-level field declarations, not as a replacement for every runtime object access path.
- When documentation or examples move from field declarations to actual runtime behavior, they will usually switch to helper functions that resolve and operate on scene objects.
- If a field is conceptually “an object reference the designer assigns”, `SceneObj` is usually the right declaration type.

## Related APIs
- [Fields and Inspector](../manual/fields-and-inspector.md)
- [Module: ModuCPP.Experimental](module-moducpp-experimental.md)
- [Script Structure](../manual/script-structure.md)
