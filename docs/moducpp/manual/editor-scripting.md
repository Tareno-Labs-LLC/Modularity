# Editor Scripting

## Overview
ModuCPP can script editor workflows as well as runtime behavior.

The repository shows three practical editor-scripting styles:

1. automatic or declarative per-object inspectors for gameplay scripts
2. full manual inspectors for scripts that need buttons, custom widgets, or tool actions
3. standalone editor windows for focused workflows such as object utilities or simple animation tools

This page explains how those styles differ and why that separation matters.

## Why Editor Scripting Exists
Not every useful tool belongs in engine source. Many project-specific needs are closer to the script that uses them:

- a better inspector for a complex gameplay component
- a runtime status tab while tuning behavior
- a small rigidbody test panel
- a script-backed animation or scene-editing window

Editor scripting exists so those workflows can live with the script instead of forcing every project to modify the engine for every convenience feature.

## The Three Main Editor Paths

### 1. Automatic Inspector
This is the default generated editor from public fields.

Use it when:

- the script is simple
- the field list already teaches the user how to configure the script

### 2. Declarative `inspector { ... }`
This is the most common next step once a script becomes more serious.

Use it when:

- the script needs tabs or grouping
- runtime status belongs in the inspector
- the fields are still the main source of authored data

Repository examples:

- `DialogueSystem.moducpp`
- `InteractableObject.moducpp`
- `MainMenuController.moducpp`

### 3. Manual `Script_OnInspector()`
Use this when the inspector itself needs custom behavior.

Repository examples:

- `SampleInspector.moducpp`
- `SampleInspector Simplified.moducpp`
- `RigidbodyTest.moducpp`
- `StandaloneMovementController.moducpp`

## Declarative Inspectors: When Layout Is The Main Problem

### Why This Style Exists
Many real scripts do not need a fully custom editor. They just need the inspector to explain itself better.

That is exactly what the declarative inspector DSL is for.

### Example
```cpp
add ModuCPP;
add ModuCPP.Experimental;

public class DialogueSystem : ModuNode
{
    [ObjectRef] public string characterNameTextRef;
    [ObjectRef] public string dialogueTextRef;
    public string characterSoundClip;
    public bool autoOpenOnBegin = false;

    private bool running = false;
    private int index = 0;

    inspector
    {
        Tabs {
            Tab("Bindings") {
                AutoFields(characterNameTextRef, dialogueTextRef);
            }
            Tab("Audio") {
                AutoFields(characterSoundClip);
            }
            Tab("Flags") {
                AutoFields(autoOpenOnBegin);
            }
            Tab("Runtime") {
                Run(ImGui::TextDisabled("Running: %s", running ? "Yes" : "No"));
                Run(ImGui::TextDisabled("Line: %d", index + 1));
            }
        }
    }
}
```

### Why It Works
This pattern gives you the best of both worlds:

- persisted public fields remain the source of truth
- the inspector becomes easier to scan
- runtime debugging information can be shown without exposing private fields as editable settings

## Manual Inspectors: When The Inspector Needs Behavior

### Overview
Manual inspectors are for cases where the editor UI is not just a layout problem.

Use them when the inspector needs:

- explicit buttons that perform actions immediately
- manual ImGui widgets
- direct scene operations
- grouped config stored in `Config<T>()`

### Repository Pattern: `Config<T>()` Plus Manual Widgets
`SampleInspector Simplified.moducpp` and `RigidbodyTest.moducpp` show a repeatable pattern:

1. fetch `Config<T>()`
2. bind it with `BindSetting(...)`
3. draw widgets
4. call `ctx.SaveAutoSettings()` when something changed

### Example
```cpp
void Script_OnInspector()
{
    auto& config = Config<RigidbodyTestConfig>();
    bindConfig(ctx, config);
    auto& state = State<RigidbodyTestState>();

    bool changed = false;
    changed |= ImGui::Checkbox("Launch on Begin", &config.autoLaunch);
    changed |= ImGui::DragFloat3("Launch Velocity", &config.launchVelocity.x, 0.25f, -50.0f, 50.0f, "%.2f");
    if (changed) {
        ctx.SaveAutoSettings();
    }

    if (ImGui::Button("Launch Now")) {
        Launch(ctx, config, state);
    }
}
```

### Why This Pattern Exists
It is better than forcing every manual inspector to use top-level public fields for everything:

- config stays grouped logically
- runtime warning flags or debug state can live in `State<T>()`
- the inspector can behave more like a purpose-built tool

## Editor Windows: When The Script Is A Tool First

### Overview
`RenderEditorWindow()` is the right choice when the script should behave like a tool window rather than a component inspector.

Repository examples:

- `EditorWindowSample.moducpp`
- `AnimationWindow.moducpp`

### Why This Is Different From An Inspector
A per-object inspector explains one script instance.

An editor window provides a workflow:

- a selected target
- controls that affect more than one object
- transport controls
- utility buttons

### Example
```cpp
add ModuCPP;
add ModuEngine;

public class EditorWindowSample : ModuBehaviour
{
    private bool toggle = false;
    private float sliderValue = 0.5f;
    private string note = "Hello from script!";

    void RenderEditorWindow()
    {
        ImGui::TextUnformatted("EditorWindowSample");
        ImGui::Separator();

        ImGui::Checkbox("Toggle", &toggle);
        ImGui::SliderFloat("Value", &sliderValue, 0.0f, 1.0f, "%.2f");
        EditString("Note", note, 128, "note");

        if (ImGui::Button("Log Message")) {
            ctx.AddConsoleMessage("Script tab says: " + note);
        }
    }

    void ExitRenderEditorWindow() {}
}
```

### What `AnimationWindow.moducpp` Teaches
The animation sample shows that editor windows can be much richer than a few controls. They can maintain:

- current selection
- playback time
- keyframe data
- transport state

That is an important teaching point: editor windows are not second-class inspectors. They are a real tool surface.

## Persistence In Editor Tools

### Why This Needs Care
Editor scripts often mix two very different kinds of change:

- changing the script’s own persisted configuration
- changing scene objects directly

Those two changes use different mechanisms.

### Persisting Tool Configuration
For config values edited through ImGui widgets:

- bind settings with `BindSetting(...)` or `AutoSetting(...)`
- call `ctx.SaveAutoSettings()` when values change

### Persisting Scene Data
For direct edits to scene objects:

- use helpers such as `ctx.SetPosition(...)`, `ctx.SetRotation(...)`, or `ctx.SetScale(...)`
- call `ctx.MarkDirty()` when the script directly changes serialized scene state

This pattern appears in both `EditorWindowSample.moducpp` and `AnimationWindow.moducpp`.

## Safe Runtime Separation

### Why This Matters
Editor code and runtime code are different responsibilities. A clean script keeps them separated.

The repository examples model that well:

- gameplay behavior stays in `TickUpdate()`, `Begin()`, `Spec()`, or `TestEditor()`
- editor UI stays in `inspector { ... }`, `Script_OnInspector()`, or `RenderEditorWindow()`

That separation is good for readability, portability, and build safety.

## Common Mistakes
- Using a manual inspector when declarative layout would have been enough.
- Forgetting `ctx.SaveAutoSettings()` in a manual inspector.
- Editing scene objects directly in a tool without `ctx.MarkDirty()`.
- Trying to put workflow-tool UI into runtime hooks.
- Binding `Config<T>()` only inside the inspector and not in runtime hooks that also read it.

## Best Practices
- Prefer automatic or declarative inspectors until you genuinely need custom behavior.
- Use manual inspectors for custom actions and grouped tool config.
- Use editor windows when the workflow is tool-centric rather than object-centric.
- Separate tool persistence from scene-dirty tracking.
- Keep runtime behavior and editor behavior in their proper hooks.

## Related Pages
- [Methods and Lifecycle](methods-and-lifecycle.md)
- [Fields and Inspector](fields-and-inspector.md)
- [Inspector Reference](../api/inspector-reference.md)
- [Hook Reference](../api/hook-reference.md)
