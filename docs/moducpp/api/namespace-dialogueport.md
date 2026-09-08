# DialoguePort Namespace

## Summary
`DialoguePort` is the shipped shared helper namespace defined in `Scripts/DialoguePortShared.h`.

It is not minimal core language surface. It is a reusable script-side support layer for the repository’s advanced dialogue, interaction, and menu samples.

It exists because those systems repeatedly need the same things:

- structured dialogue data
- settings serialization helpers
- shared target-resolution logic
- shared editor widgets
- shared UI-effect helpers

## Syntax
```cpp
#include "DialoguePortShared.h"
using namespace DialoguePort;
```

## Why This Namespace Exists
The repository’s larger sample systems are related:

- `DialogueSystem.moducpp`
- `InteractableObject.moducpp`
- `MainMenuController.moducpp`

Without a shared helper namespace, they would duplicate the same support logic over and over:

- localized line data
- line serialization
- interaction request keys
- dialogue-system search
- text-effect helpers

`DialoguePort` packages that reusable layer in one place.

## Enums

### `Language`
Represents which localized sentence field should be used.

Repository use:

- `DialogueSystem.moducpp` exposes `currentLanguage`

Why it exists:

- dialogue content often needs multiple language fields without the runtime script reimplementing the lookup logic

### `TextEffectType`
Represents text-effect flags such as wave, shake, bounce, rotate, and fade.

Why it exists:

- dialogue text may need authored presentation effects, not only raw string output

## Constants

### Interaction Request Keys
- `kInteractionRequestPending`
- `kInteractionOverrideLines`
- `kInteractionEndEnable`
- `kInteractionEndDisable`
- `kInteractionPlayerRef`
- `kInteractionRequestSerial`

### Why They Exist
These keys form the shared settings-based protocol between `InteractableObject.moducpp` and `DialogueSystem.moducpp`.

That is an important repository pattern: one script writes a request into another script’s settings, and the receiving script consumes it next frame.

## Types

### `DialogueLine`
This is the central authored data type for dialogue content.

It includes:

- character name
- localized sentence fields
- per-line typing sound override
- typing speed
- text effect and tuning
- mouth-object refs
- line-level enable and disable lists

### Why It Exists
A real dialogue line is more than plain text. It is authored presentation and state-transition data.

That is why `DialogueLine` deserves a dedicated type instead of being flattened into loose strings and parallel arrays.

### `DialogueScriptTarget`
Represents the result of finding a dialogue system script target in an object hierarchy.

Why it exists:

- interaction scripts need to locate the actual target script and object cleanly

## Serialization And Text Helpers

### Members
- `SerializeDialogueLines(...)`
- `DeserializeDialogueLines(...)`
- `GetSentenceForLanguage(...)`
- `ParseSentenceForDisplay(...)`
- `LanguageLabel(...)`

### `SerializeDialogueLines(...)` / `DeserializeDialogueLines(...)`

#### What They Do
Convert dialogue-line arrays to and from a storable settings representation.

#### Why They Exist
The repository’s interaction flow sometimes overrides dialogue data through settings instead of only through public fields. That requires reliable serialization.

#### Repository Use
`InteractableObject.moducpp` serializes override dialogue lines when pushing an interaction request to the dialogue system.

### `GetSentenceForLanguage(...)`

#### What It Does
Selects the most appropriate sentence field for the requested language.

#### Why A Script Author Chooses It
Because a dialogue runtime should focus on flow and timing, not language-field branching every time it starts a line.

### `ParseSentenceForDisplay(...)`

#### What It Does
Strips authoring tags or markup-like payloads down to the displayable sentence.

#### Why It Exists
Dialogue data may contain extra authoring syntax that should not appear in the final on-screen string.

#### Repository Use
`DialogueSystem.moducpp` uses it before revealing text character by character.

## Search And Target Helpers

### Members
- `IsDialogueSystemScript(...)`
- `FindDialogueSystemScriptOnObject(...)`
- `FindDialogueScriptTarget(...)`

### Why They Exist
Interaction scripts often do not know whether the dialogue system is:

- on the exact referenced object
- on a child
- on a parent

The search helpers solve that problem once so the sample scripts do not have to reimplement hierarchy traversal every time.

### Repository Use
`InteractableObject.moducpp` uses `FindDialogueScriptTarget(...)` before writing interaction request settings.

## UI And Runtime Editor Helpers

### Members
- `DrawLanguageCombo(...)`
- `DrawTextEffectFlagsEditor(...)`
- `DrawDialogueLineToolbar(...)`
- `DrawDialogueLineList(...)`
- `DrawDialogueLineEditor(...)`
- `DrawDialogueRuntimeStatus(...)`

### Why They Exist
Dialogue-heavy inspectors need richer editors than a flat list of primitive fields.

These helpers keep:

- language selection
- effect-flag editing
- line list editing
- runtime status views

consistent across related scripts.

## A Real Repository Pattern: Interaction Requests Into Dialogue
One of the best reasons to understand `DialoguePort` is the way it helps two independent scripts communicate.

The pattern is:

1. `InteractableObject.moducpp` resolves a dialogue-system target.
2. It serializes override dialogue lines and end-state object lists.
3. It writes those values into the dialogue script’s settings.
4. `DialogueSystem.moducpp` detects the request and opens the dialogue with those overrides.

That pattern is only readable because `DialoguePort` centralizes the shared constants, data types, and serialization logic.

## Common Mistakes
- Treating `DialoguePort` as universal core API instead of shipped shared helper code.
- Reimplementing dialogue-line serialization or target search manually.
- Forgetting that it depends on the underlying `ModuCPP.Experimental` helper layer.

## Related APIs
- [Module: ModuCPP.Experimental](module-moducpp-experimental.md)
- [Shipped Example Types](type-shipped-example-types.md)
- [Editor Scripting](../manual/editor-scripting.md)
- [Common Patterns](../manual/common-patterns.md)
