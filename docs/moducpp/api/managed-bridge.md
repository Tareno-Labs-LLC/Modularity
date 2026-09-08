# Managed Bridge (`ModuCPP.cs`)

## Summary
`Scripts/Managed/ModuCPP.cs` is the managed bridge used by Mono-hosted `C#` scripts. It mirrors part of the native script API through managed types such as `Context`, `ImGui`, and `Inspector`.

This page is included in the ModuCPP docs because the managed bridge is part of the current scripting surface shipped in the repository, even though `.moducpp` authoring and managed `C#` authoring are different workflows.

## Syntax
```csharp
using ModuCPP;

public class SampleInspectorManaged
{
    public void TickUpdate(IntPtr ctx, float dt)
    {
        var context = new Context(ctx);
        context.AddConsoleMessage("Tick");
    }
}
```

## Description
The managed bridge exists so the engine can expose a script-facing API to Mono-hosted `C#` code without making managed scripts call directly into native details by hand.

It gives managed scripts:

- vector and hit data structures
- object and scene lookup
- transform and physics access
- UI, audio, and sprite helpers
- settings helpers
- reflection-based inspector support

In other words, it plays a role for managed scripts similar to the role the high-level modules play for `.moducpp` scripts: it gives authors a practical scripting surface that sits on top of the native runtime.

## Members

### Core Data Types
- `Vec2`
- `Vec3`
- `RaycastHit`
- `ConsoleMessageType`
- `ModuObject`

### Entry and Hosting Types
- `Host.SetNativeApi(...)`
- `Context`
- `ImGui`
- `Inspector`

### `Context` highlights
- object and scene lookup
- transform access
- rigidbody and Rigidbody2D helpers
- animation helpers
- audio helpers
- UI helpers
- sprite helpers
- settings get/set helpers
- `AutoSetting(...)`
- `AutoSettingsFrom(...)`
- `MarkDirty()`

### `ImGui` highlights
- `Text(...)`
- `Separator()`
- `Button(...)`
- `Checkbox(...)`
- `DragFloat(...)`
- `DragFloat3(...)`
- `InputText(...)`
- `BeginCombo(...)`
- `EndCombo()`
- `Selectable(...)`
- `AcceptSceneObjectDrop(...)`

### Inspector Attributes
- `HeadTextAttribute`
- `LabelAttribute`
- `SettingKeyAttribute`
- `DragSpeedAttribute`
- `InspectorIgnoreAttribute`

## Behavior Explanation
The managed bridge is most useful when you think of it in layers.

### Native API Binding
`Host.SetNativeApi(...)` connects the managed side to the native function table. That is the low-level bridge step.

### `Context`
`Context` is the everyday script-side entry point. It exists so a managed script can do the same kinds of work a high-level native script does:

- read and write object state
- query settings
- play audio
- control animation
- drive UI

### `ImGui`
`ImGui` exists so managed scripts can still build manual tool or inspector UI without directly talking to the native ABI.

### `Inspector`
`Inspector` and the related attributes solve the same general problem as high-level ModuCPP public fields and metadata: they reduce manual editor boilerplate.

## Multiple Examples
### Runtime update
```csharp
using System;
using ModuCPP;

public class SpinScript
{
    public void TickUpdate(IntPtr ctx, float dt)
    {
        var context = new Context(ctx);
        context.AddConsoleMessage("Tick");
    }
}
```

### Auto settings
```csharp
using System;
using ModuCPP;

public class SampleInspectorManaged
{
    [Label("Rotation Speed")]
    public Vec3 SpinSpeed = new Vec3(0.0f, 45.0f, 0.0f);

    public void Begin(IntPtr ctx, float deltaTime)
    {
        var context = new Context(ctx);
        context.AutoSettingsFrom(this, save: false);
    }
}
```

### Manual UI
```csharp
ImGui.Text("Managed Tool");
ImGui.Separator();
```

## Remarks
- The current managed bridge ABI version is documented as `7`.
- Managed scripts are a supported part of the repository, but they are a separate authoring path from `.moducpp`.
- This page stays focused on the script-facing shape of the bridge. For runtime setup and embedding details, use the Mono setup docs.

## Related APIs
- [Mono Embedding Setup](../../mono-embedding.md)
- [Scripting Overview](../../Scripting.md)
