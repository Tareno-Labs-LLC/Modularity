# Inspector Reference

## Summary
This page documents the declarative ModuCPP inspector system as it is actually used in the repository.

The key idea is simple: the inspector DSL is for scripts that still want public fields to be the source of authored data, but need more structure, status, or focused editors than a flat generated inspector can provide.

## Syntax
```cpp
inspector {
    Tabs {
        Tab("General") {
            AutoFields(speed, targetRef);
        }
    }
}
```

## Why This System Exists
Complex scripts need two things at once:

- stable, persisted authored fields
- an inspector that teaches the user how the script is meant to be configured

The repository’s larger scripts demonstrate this well:

- `DialogueSystem.moducpp`
- `InteractableObject.moducpp`
- `MainMenuController.moducpp`

Those scripts do not need fully manual inspectors. They need structured, teaching-oriented inspectors.

## Field Attributes

| Attribute | What it is for | Repository-style use |
| --- | --- | --- |
| `[Header("Title")]` | visually group fields in automatic inspectors | sectioning gameplay values |
| `[Slider(min, max)]` | edit a tuned numeric range as a slider | speed, timing, distance |
| `[ObjectRef]` | treat a `string` as one object reference | player, heart, text target refs |
| `[ObjectList]` | treat a `string[]` as multiple object refs | enable/disable lists, menu items |
| `[DialogueLines]` | use dialogue-line editing for dialogue arrays | dialogue authoring |
| `[ClipGridPair]` | edit related directional clip arrays together | top-down movement clip grids |
| `[Separator]` | insert visual separation | clip and sound grouping |
| `[SoundSet("Label")]` | use a sound-set editor for string arrays | footstep or effect sounds |
| `@range(min, max)` | numeric metadata for range-based editing | tuned fields |
| `@step(value)` | drag-speed metadata | fine-grained editing |

### Why Attributes Matter
Attributes are not cosmetic. They tell the inspector what the data really means.

For example:

- `[ObjectRef]` says "this string is not arbitrary text"
- `[ObjectList]` says "this array is scene coordination data"
- `[Slider(...)]` says "this number is tuned within a known range"

That is why real scripts become easier to configure when the metadata is honest.

## Layout Containers

### `Tabs { ... }`
Use tabs when the script has several distinct categories of configuration.

This is the dominant repository pattern for larger scripts.

### `Tab("Name") { ... }`
Use tabs to split responsibilities clearly, such as:

- `Bindings`
- `Timing`
- `Audio`
- `Flags`
- `Runtime`

### `Section("Name")`, `Group("Name")`, `Foldout("Name")`
Use these for lighter structure when a full tabbed layout is unnecessary.

## Core Inspector Statements

### `AutoFields(fieldA, fieldB, ...)`

#### What It Does
Places normal generated editors for the listed persisted public fields at the current point in the layout.

#### Why A Script Author Chooses It
Because the fields are already correct, but the default order or grouping is no longer good enough.

#### Repository Pattern
```cpp
Tab("Audio") {
    AutoFields(characterSoundClip, triggerSoundClip, enterSoundClip, exitSoundClip, skipSoundClip);
}
```

#### Important Note
`AutoFields(...)` only works for persisted public fields on the current high-level class.

### `Run(expression)`

#### What It Does
Runs arbitrary inspector-time code inside the declarative inspector.

#### Why It Exists
Not everything shown in an inspector is a persisted field. Sometimes you need:

- runtime status text
- helper methods that draw custom sections
- validation or migration calls

#### Repository Patterns
Runtime status:

```cpp
Run(ImGui::TextDisabled("Running: %s", running ? "Yes" : "No"));
```

Custom helper call:

```cpp
Run(DrawRuntimeStatus());
```

Migration helper:

```cpp
Run(MigrateLegacySettingsIfNeeded());
```

This is an important teaching point: `Run(...)` is not only for text labels. It is the escape hatch for inspector-time logic while still staying inside the declarative layout system.

### `Header("Title")`
Use this for a prominent section heading inside the declarative inspector.

### `Separator()`
Use this for visual separation when a section should read as two smaller groups.

### `Slider(...)`, `Enum(...)`, `ObjectRef(...)`, `ObjectList(...)`, `AudioClip(...)`
Use these when one field needs a specific inline editor rather than going through `AutoFields(...)`.

This is useful when:

- the label should be customized
- only one field from a larger group needs special placement

### Specialized Statements
The repository and shipped helpers also support more specialized editors such as:

- `DialogueLines(...)`
- `InteractionOptions(...)`
- `TextEffectFlags(...)`
- `MenuActions(...)`
- `RuntimeDialogueStatus()`
- `RuntimeInteractableStatus()`
- `RuntimeMenuStatus()`
- `ClipGrid(...)`
- `SoundSet(...)`

These exist because some authored data is too rich for plain primitive widgets.

## A Practical Mental Model
Use the declarative inspector as a layered tool:

1. `Tabs` and containers define the information architecture
2. `AutoFields(...)` places the core authored values
3. specialized statements improve editors for richer data shapes
4. `Run(...)` adds runtime visibility or custom logic

That mental model matches the repository scripts closely.

## Common Repository Patterns

### Configuration Plus Runtime Tab
```cpp
inspector {
    Tabs {
        Tab("Config") {
            AutoFields(interval, targetRef);
        }
        Tab("Runtime") {
            Run(ImGui::TextDisabled("Elapsed: %.2f", timer));
        }
    }
}
```

Why a script author chooses this:

- configuration and runtime status both matter
- private runtime state should be visible but not editable

### Mixed Auto Fields And Custom Status Helpers
```cpp
Tab("Runtime") {
    Run(DrawRuntimeStatus());
}
```

Why a script author chooses this:

- the runtime view is more complex than one or two `ImGui::TextDisabled(...)` lines

### Focused Special Editors
```cpp
Tab("Targets") {
    ObjectRef("Dialogue Root", dialogueSystemRef);
    ObjectList("Objects To Enable", itemsToEnable);
}
```

Why a script author chooses this:

- the field is a reference-like value and should read that way in the UI

## Common Mistakes
- Expecting `AutoFields(...)` to work with private fields.
- Using a fully manual inspector when tabs plus `Run(...)` would have been simpler.
- Forgetting that `Run(...)` can perform logic, not just draw text.
- Treating object-reference strings like plain text instead of using `[ObjectRef]` or `[ObjectList]`.

## Related APIs
- [Fields and Inspector](../manual/fields-and-inspector.md)
- [Editor Scripting](../manual/editor-scripting.md)
- [Hook Reference](hook-reference.md)
- [Module: ModuCPP.Experimental](module-moducpp-experimental.md)
