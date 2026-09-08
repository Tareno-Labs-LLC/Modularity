# Shipped Example Types

## Summary
This page documents the script-defined enums and `SubScript` data blocks that ship with the example ModuCPP scripts in this repository.

These types are not universal built-in engine primitives. They matter anyway, because they show how real gameplay scripts are expected to model state, inspector data, and configuration in practice. A good scripting manual should not stop at the smallest built-in core surface. It should also explain the patterns the shipped scripts actually use when they grow beyond trivial examples.

## Syntax
```cpp
public enum InteractableType { Dialogue, ToggleObjects }

SubScript InteractionOption {
    public string optionName;
    public InteractableType interactionType;
    public SceneObj[] itemsToEnable;
    public SceneObj[] itemsToDisable;
};
```

```cpp
public enum FacingDirection { Down, Up, Right, Left }
```

## Description
Large scripts need a vocabulary of their own.

Once a script does more than flip a single object on or off, raw booleans and loosely related arrays stop being enough. You need names for modes, stable containers for repeated data, and readable ways to express gameplay intent. That is the role these types serve in the shipped examples.

There are two important ideas on this page:

`enum` types solve the problem of meaning. Instead of asking readers to remember that `0` means dialogue and `1` means toggle mode, the script says `InteractableType.Dialogue` or `InteractableType.ToggleObjects`.

`SubScript` types solve the problem of structured repeated data. Instead of maintaining several parallel arrays that must always stay in sync, the script groups related fields into one nested inspector-editable block.

Those are not just style preferences. They change how easy the script is to configure, debug, and extend.

## Members
### `FacingDirection`
Declared in `TopDownMovement2D.moducpp`.

Values:

- `Down`
- `Up`
- `Right`
- `Left`

This enum represents the last meaningful direction a top-down character is facing. It exists because movement input and facing state are related, but they are not the same thing.

For example, when a player stops moving, the current movement vector becomes zero. That does not mean the character has no facing direction. Animation, interaction prompts, and directional attacks often still need to know whether the player was last facing up, down, left, or right.

Use `FacingDirection` when:

- animation should keep an idle pose consistent with the last movement direction
- interactions should happen “in front of” the character
- directional sprites or indicators must remain stable when input stops

### `MenuOrientation`
Declared in `MainMenuController.moducpp`.

Values:

- `Vertical`
- `Horizontal`

This enum describes how a menu is navigated. It exists because menu layout is design data, not an implementation accident.

When a menu is authored as vertical, navigation logic should read up/down input. When it is authored as horizontal, navigation logic should read left/right input. Encoding that choice as a named enum makes the script easier to configure and easier to understand during review.

Use `MenuOrientation` when:

- the same menu controller should work for multiple layouts
- the inspector should make orientation obvious to a designer
- navigation rules should branch by intent rather than by arbitrary integer flags

### `MenuAction`
Declared as a `SubScript` in `MainMenuController.moducpp`.

Fields:

- `enable`
- `disable`

`MenuAction` groups the scene objects that should be enabled or disabled when a menu item is activated. It exists because a menu action is larger than a single boolean. One selection may need to show one group of objects while hiding another.

Without a nested data block, a script often drifts toward several parallel arrays such as `enableTargets[]`, `disableTargets[]`, and `buttonNames[]`. That structure is fragile because every array must always be edited in lockstep. `MenuAction` avoids that by keeping one action's data together.

Use `MenuAction` when:

- each menu entry carries its own object-toggle payload
- the inspector should expose grouped action data clearly
- the script must stay readable as menu content grows

### `InteractableType`
Declared in `InteractableObject.moducpp`.

Values:

- `Dialogue`
- `ToggleObjects`

`InteractableType` describes what kind of interaction a configured option performs. It solves a very common gameplay problem: one interaction system often needs to support more than one authored outcome.

For example, an interactable object might sometimes start a dialogue sequence and sometimes toggle scene objects on or off. Both are “interactions”, but they are not handled the same way. The enum makes that difference explicit.

Use `InteractableType` when:

- one interactable script must support several distinct outcomes
- the inspector should communicate interaction intent clearly
- the runtime should branch by named behavior instead of hard-coded numeric assumptions

### `InteractionOption`
Declared as a `SubScript` in `InteractableObject.moducpp`.

Fields:

- `optionName`
- `interactionType`
- `dialogueSystemRef`
- `dialogueLines`
- `dialogueItemsToEnableOnEnd`
- `dialogueItemsToDisableOnEnd`
- `itemsToEnable`
- `itemsToDisable`

`InteractionOption` is the strongest example on this page of why `SubScript` is useful. An interactable object may expose several possible actions, and each action needs a substantial payload of related data. Some options configure dialogue. Others configure object toggles. All of them still belong to one list of authored interaction choices.

This type keeps each choice self-contained. That makes the inspector easier to reason about and the runtime logic easier to branch on cleanly.

Use `InteractionOption` when:

- a single object exposes multiple interaction choices
- each choice needs several related fields
- you want one array of coherent data records rather than multiple loosely coupled arrays

### `MouthState`
Declared in `DialogueSystem.moducpp`.

Values:

- `TalkingOpen`
- `TalkingClosed`
- `NotTalking`

`MouthState` models dialogue presentation state for a speaking portrait or object. It exists because dialogue display often changes over time instead of switching instantly between only “playing” and “stopped”.

Use `MouthState` when:

- a dialogue system swaps visual mouth states while text is being presented
- the script needs clear names for temporary presentation states
- animation/presentation logic should be separated from raw string display rules

## Behavior Explanation
The most important takeaway from these example types is not that a handful of enums exist. The important takeaway is how they shape runtime behavior and authoring behavior together.

### Enums make branching readable
When a script branches on `InteractableType` or `MenuOrientation`, the branch reads like gameplay intent. A reader can see why the branch exists without first tracing magic numbers or comments.

That matters even more over time. As scripts grow, readable branching logic is one of the easiest ways to keep features maintainable.

### `SubScript` keeps repeated data coherent
Repeated game data almost always starts simple and then grows. One menu action needs two object lists. One dialogue option needs lines, targets, and follow-up objects to toggle. Once that happens, parallel arrays become a liability.

`SubScript` avoids that problem by storing the related fields together as one reusable nested shape. The result is easier to serialize, easier to expose to the inspector, and much less error-prone to maintain.

### Example-defined types are still worth documenting
Even though these types are not universal engine built-ins, they represent real supported scripting patterns. They show how the current language surface is meant to be used in larger scripts.

That is why they belong in the documentation. They teach script authors how to design their own enums and nested data structures instead of flattening everything into primitive fields.

## Multiple Examples
### Example 1: Remembering facing direction for idle animation
```cpp
add ModuCPP;
add ModuInput;

public enum FacingDirection { Down, Up, Right, Left }

public class TopDownFacing : ModuNode
{
    private FacingDirection facing = FacingDirection.Down;

    void Update()
    {
        if (input.Up()) facing = FacingDirection.Up;
        else if (input.Down()) facing = FacingDirection.Down;
        else if (input.Left()) facing = FacingDirection.Left;
        else if (input.Right()) facing = FacingDirection.Right;

        if (!input.Up() && !input.Down() && !input.Left() && !input.Right())
        {
            ctx.SetTagText("Facing", facing.ToString());
        }
    }
}
```

This pattern separates movement intent from persistent facing state. The character can stop moving while still keeping a stable idle direction.

### Example 2: Using `InteractableType` to branch between dialogue and object toggles
```cpp
add ModuCPP;

public enum InteractableType { Dialogue, ToggleObjects }

SubScript InteractionOption {
    public string optionName;
    public InteractableType interactionType;
    public SceneObj[] itemsToEnable;
    public SceneObj[] itemsToDisable;
};

public class SimpleInteractable : ModuNode
{
    public InteractionOption[] options;

    void TriggerOption(int index)
    {
        if (index < 0 || index >= options.Count())
        {
            return;
        }

        InteractionOption selected = options[index];

        if (selected.interactionType == InteractableType.ToggleObjects)
        {
            for (SceneObj item : selected.itemsToEnable)
            {
                item.SetEnabled(true);
            }

            for (SceneObj item : selected.itemsToDisable)
            {
                item.SetEnabled(false);
            }
        }
    }
}
```

This pattern makes the interaction branch self-explanatory. The script can later grow new interaction modes without collapsing into unreadable boolean combinations.

### Example 3: Grouping menu side effects with `MenuAction`
```cpp
add ModuCPP;

SubScript MenuAction {
    public SceneObj[] enable;
    public SceneObj[] disable;
};

public class MenuStateSwitcher : ModuNode
{
    public MenuAction playAction;
    public MenuAction optionsAction;

    void ApplyAction(MenuAction action)
    {
        for (SceneObj item : action.enable)
        {
            item.SetEnabled(true);
        }

        for (SceneObj item : action.disable)
        {
            item.SetEnabled(false);
        }
    }
}
```

This keeps each authored action coherent. The alternative would usually be a harder-to-maintain collection of unrelated arrays.

### Example 4: Using `MouthState` to drive simple dialogue visuals
```cpp
add ModuCPP;

public enum MouthState { TalkingOpen, TalkingClosed, NotTalking }

public class DialoguePortrait : ModuNode
{
    private MouthState mouth = MouthState.NotTalking;
    private float switchTimer = 0.0f;

    void Update()
    {
        if (ctx.GetBool("DialogueActive"))
        {
            switchTimer += ctx.GetDeltaTime();

            if (switchTimer > 0.12f)
            {
                mouth = mouth == MouthState.TalkingOpen ? MouthState.TalkingClosed : MouthState.TalkingOpen;
                switchTimer = 0.0f;
            }
        }
        else
        {
            mouth = MouthState.NotTalking;
        }
    }
}
```

This pattern gives the presentation layer clear states instead of leaving the behavior implicit in several booleans.

## Remarks
- These types are real script-facing examples from shipped scripts, but they are not guaranteed global engine primitives.
- They are best read as reference patterns for authoring your own game-specific enums and nested data blocks.
- `InteractableType` is particularly useful as a model for branching by named gameplay intent instead of numeric modes.
- `SubScript` examples such as `InteractionOption` and `MenuAction` show how to expose repeated structured data to the inspector without relying on parallel arrays.
- When your own script starts accumulating several related fields for one repeated concept, that is usually a sign to introduce a `SubScript`.

## Related APIs
- [SceneObj](type-sceneobj.md)
- [DialoguePort Namespace](namespace-dialogueport.md)
- [Fields and Inspector](../manual/fields-and-inspector.md)
- [Common Patterns](../manual/common-patterns.md)
