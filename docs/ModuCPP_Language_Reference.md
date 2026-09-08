# ModuCPP Language Reference
This file summarizes the ModuCPP surface currently implemented by the engine transpiler and script APIs. It is intended as source data for VS Code syntax highlighting, completions, and snippets.

## Source Files
- ModuCPP scripts use `.moducpp`.
- A file is automatically treated as a ModuCPP Script if it contains a high-level class declaration like `public class Name : ModuNode`.
- A Script-facing member access should use `.`, not `->`, where the ModuCPP surface supports it.

## Modularity Package Types
By Default, ModuCPP uses `add 'PackageName';` at the top of scripts.
In Short: The transpiler lowers these to C++ includes.
ModuCPP PackageTypes        | Generated includes
`add ModuCPP;`              | `#include "ModuCPPScriptApi.h"`
`add ModuEngine;`           | `#include "ModuEngineScriptApi.h"`
`add ModuInput;`            | `#include "ModuInputScriptApi.h"`
`add RMeshBuilder;`         | `#include "RMeshBuilderScriptApi.h"`
`add ModuCPP.Experimental;` | `#include "ModuCPPExperimentalScriptApi.h"`

## Declaration Keywords
These are ModuCPP-specific or required by the high-level script form:
```text
add
public
private
class
enum
SubScript
inspector
to
then
ref
each
state
```

ModuCPP also passes through normal C++ syntax after lowering,
For example: VS Code grammar also includes C++ keywords such as `if`, `else`, `for`, `while`, `return`, `static`, `const`, `auto`, `struct`, `namespace`, `using`, `switch`, `case`, `break`, `continue`, `true`, `false`, `ctx`, and `nullptr`.

## Syntax Languages
Every keyword above has an English spelling and, optionally, localized spellings. A syntax language is a dictionary, not a dialect: the lexer maps localized spellings onto the canonical (English) tokens before the parser runs, so all supported languages compile to the same AST and the same generated C++.
```text
Language Dictionary -> Lexer -> Canonical Tokens -> Parser -> AST -> C++ Backend
```
Only language keywords and officially localized built-in API aliases are translated. User classes, fields, methods, namespaces, assets and project identifiers are never renamed, and comments, string literals and preprocessor lines are left untouched.

German is the first supported alternative. These two scripts are the same script:
```moducpp
add ModuCPP;
public class PlayerMovement : ModuNode {
    void TickUpdate() { if (true) return; }
}
```
```moducpp
Nutze ModuCPP;
öffentlich klasse PlayerMovement : ModuNode {
    Nichts Tick() { Wenn (true) Zurück; }
}
```
Core German keyword mappings:
```text
Nutze        -> add
öffentlich   -> public
privat       -> private
klasse       -> class
Nichts       -> void
Wenn         -> if
sonst        -> else
Zurück       -> return
dann         -> then
zu           -> to
für          -> each
Beginnen     -> Begin
Tick         -> TickUpdate
```
Built-in API aliases (matched only after `.`, `::` or `->`): `Starten` -> `Start`, `Bereit` -> `Ready`, `Zustand` -> `state`, `Länge` -> `Length`, `IstLeer` -> `IsEmpty`.

The source language is detected per file, so localized scripts compile in any project without configuration. The project's authoring language and import translation behavior live in Project Settings > Language Manager. Full dictionary, architecture and instructions for registering another language: `docs/moducpp/manual/syntax-languages.md`.

## Default Script Layout
High-level script classes must derive from `ModuNode`, `ModuBehaviour` is deprecated as of V6.8
Supported declaration form:
```moducpp
public class MyScript : ModuNode {}
```

## Primitive And Alias Types
The transpiler maps these script-facing aliases:
ModuCPP type   | C++ type
`string`       | `std::string`
`vec2`         | `glm::vec2`
`Vector2`      | `glm::vec2`
`vec3`         | `glm::vec3`
`Vector3`      | `glm::vec3`
`SceneObj`     | `SceneObject`
`SceneObject`  | `SceneObject`
`SceneObj*`    | `SceneObject*`
`SceneObject*` | `SceneObject*`
`List<T>`      | `std::vector<T>`
`T[]`          | `std::vector<T>`
`T[N]`         | `std::array<T, N>`
`T[R][C]`      | `std::array<std::array<T, C>, R>`

Inspector-backed primitive fields support:
```text
float
int
bool
vec3
Vector3
string
SceneObject[]
SceneObj[]
List<SceneObject*>
DialoguePort::DialogueLine[]
```

## Inspector Attributes
Attributes are written before a field declaration.
Attribute            | Arguments            | Notes
`[Header(title)]`    | one title expression | Emits inspector heading and separator.
`[Slider(min, max)]` | two expressions      | Works on `float`, `int`, and `vec3`.
`[ObjectRef]`        | none                 | Treats a `string` field as an object reference picker.
`[ObjectList]`       | none                 | Treats a string/object array as an object reference list.
`[DialogueLines]`    | none                 | Treats a field as dialogue line data.
`[ClipGridPair]`     | none                 | Requires an `int[4]` field followed by an `int[4][4]` field.
`[Separator]`        | none                 | Emits an inspector separator before the field.
`[SoundSet(label)]`  | one label expression | Requires a string array.
`@range(min, max)`   | two expressions      | Adds range metadata for auto inspector numeric fields.
`@step(value)`       | one expression       | Adds drag step metadata for auto inspector numeric fields.

Example:
```moducpp
[Header("Movement")]
[Slider(0.0f, 20.0f)] public float speed = 4.0f;
[ObjectRef] public string targetRef;
private vec3 velocity = vec3(0.0f);
```

## Lifecycle Methods
These method names are recognized by the transpiler/runtime. `ScriptContext& ctx` is auto-injected when omitted. `float dt` is auto-injected for runtime tick-style hooks where supported.
```text
Begin
TickUpdate
Update
Spec
TestEditor
RenderEditorWindow
ExitRenderEditorWindow
Script_OnInspector (though, most of the time, this isn't needed.)
```

Common signatures:
```moducpp
void Begin()
void TickUpdate()
void TickUpdate(float dt)
void TickUpdate(ScriptContext& ctx, float dt)
void RenderEditorWindow(ScriptContext& ctx)
void ExitRenderEditorWindow(ScriptContext& ctx)
void Script_OnInspector()
void Script_OnInspector(ScriptContext& ctx)
```
`MODU_obj` (aka: SceneOBJ) is also accepted as a parameter alias and lowers to `ScriptContext& ctx`.

## Expression-Bodied Methods
Methods may use `to` for expression-bodied syntax:
```moducpp
bool IsReady() to timer.Ready;
int ClampFrame(int frame) to Math.Clamp(frame, 0, 3);
```
For non-`void` methods, the transpiler emits `return <expression>;`. For `void`, it emits `<expression>;`.

## Inspector DSL
A class can define an `inspector { ... }` block instead of writing `Script_OnInspector`. Statements end with `;`. Containers use braces.

### Inspector Statements
```text
Config(Type, varName)
AutoSave(configVar)
Save(configVar)
Header(title)
Run(expression)
Separator()
Toggle(value)
Toggle(label, value)
Slider(value, min, max)
Slider(label, value, min, max)
Number(value)
Number(label, value)
String(value)
String(label, value)
ObjectRef(value)
ObjectRef(label, value)
AudioClip(value)
AudioClip(label, value)
ObjectList(value)
ObjectList(label, value)
Enum(value)
Enum(label, value)
DialogueLines(lines)
DialogueLines(label, lines)
AutoFields(fieldA, fieldB, ...)
InteractionOptions(options)
InteractionOptions(label, options)
MenuActions(menuItemRefs, actions)
MenuActions(label, menuItemRefs, actions)
RuntimeDialogueStatus()
RuntimeInteractableStatus()
RuntimeMenuStatus()
TextEffectFlags(effect)
TextEffectFlags(label, effect)
ClipGrid(idleClips, walkClips)
SoundSet(label, sounds)
```

### Inspector Containers
```text
Tabs           { ... }
Tab(title)     { ... }
Section(title) { ... }
Group          { ... }
Group(title)   { ... }
Foldout(title) { ... }
```

Example:
```moducpp
inspector {
    Section("Movement") {
        AutoFields(speed, acceleration);
        ObjectRef("Target", targetRef);
    }
}
```

## Surface Syntax Rewrites
The transpiler rewrites these convenience forms:
ModuCPP form                 | Lowered form
`Math.Max(a, b)`             | `Math::Max(a, b)`
`Math.Min(a, b)`             | `Math::Min(a, b)`
`Math.Clamp(v, min, max)`    | `Math::Clamp(v, min, max)`
`Math.Abs(v)`                | `Math::Abs(v)`
`SomeEnum.Value`             | `SomeEnum::Value` (when both names start uppercase)
`ConsoleMessageType.Warning` | `ConsoleMessageType::Warning`
`Vector2`                    | `glm::vec2`
`Vector3`                    | `glm::vec3`
`string`                     | `std::string`
`value.Length()`             | `glm::length(value)`
`a.Dot(b)`                   | `glm::dot(a, b)`
`value.IsEmpty()`            | `value.empty()`
`timer.Start(interval)`      | `::ModuCPP::StartTimer(timer, interval)`
`timer.Ready`                | `::ModuCPP::TimerReady(timer)`
`timer.Ready(interval)`      | `::ModuCPP::TimerReady(timer, interval)`
`time.deltaTime`             | `::ModuCPP::time.deltaTime`
`ModuEngine.FPS`             | `::ModuCPP::ModuEngine.FPS`

### Angle Bracket Escapes
If you can't (or would rather not) type `<` and `>`, write `[/L]` and `[/R]` instead.
The transpiler swaps them back before anything else runs, so they work anywhere a
real angle bracket would: generic types, comparisons, shifts, even raw `#include`s in
a `ctx` block.
Escape  | Lowered form
`[/L]`  | `<`
`[/R]`  | `>`
```moducpp
public List[/L]SceneObj*[/R] targets;   // -> List<SceneObj*>
if (count [/L] max) { ... }              // -> if (count < max)
```
The slash is what makes this safe: `[/L]` can never be a valid array subscript or
`[Attribute]`, so it never collides with normal code. The swap is skipped inside
string and character literals and comments, so `"[/L]"` stays literal text.

## Object List Syntax
For persisted object-list fields, this convenience statement is supported:
```moducpp
each selectedStateEnable.state(true);
each selectedStateDisable.state(false);
```
It resolves every object reference in the list and sets enabled state.

## IEnum Helpers
Lightweight coroutine-style helpers are macro-backed:
```text
IEnum
IEnum_Start(fn)
IEnum_Stop(fn)
IEnum_Ensure(fn)
```

Runtime methods:
```text
ctx.StartIEnum(fn)
ctx.StopIEnum(fn)
ctx.EnsureIEnum(fn)
ctx.IsIEnumRunning(fn)
ctx.StopAllIEnums()
```

## ModuCPP Core API
Available from `add ModuCPP;`.
### Aliases
```text
ModuCPP::vec2
ModuCPP::vec3
ModuCPP::Vector2
ModuCPP::Vector3
ModuCPP::string
```
### Math
```text
Math.Max(a, b)
Math.Min(a, b)
Math.Clamp(value, minValue, maxValue)
Math.Abs(value)
```
### Conversion Helpers
```text
IntRD(value)
IntR(value)
IntRU(value)
```
### Script State Helpers
```text
ctx()
ctxPtr()
Config<T>()
State<T>()
BindSetting(key, value)
BindArray(ctx, keyPrefix, values)
BindArray(keyPrefix, values)
BindArray2D(ctx, keyPrefix, values)
BindArray2D(keyPrefix, values)
SerializeSubScript(value)
DeserializeSubScript<T>(encoded)
SerializeSubScriptArray(values)
DeserializeSubScriptArray<T>(encoded)
EditSubScript(label, value)
EditSubScriptArray(label, values)
StartTimer(timerValue, interval)
TimerReady(timerValue)
TimerReady(timerValue, interval)
```

### Generated Prelude
Lifecycle functions get this prelude through `MODU_SCRIPT(ctx)`:
```text
MODU_SCRIPT(ctx)
obj
```
`obj` is an `ObjectFacade` for `ctx.object`. It supports bool checks, pointer-style access, and `obj.UILabel = "text";`.

## ModuEngine API
Available from `add ModuEngine;`.
```text
GetProjectGravityScale()
SetProjectGravityScale(scale)
EditFloat(label, value, speed, minValue, maxValue, format, keyOverride)
EditVec3(label, value, speed, minValue, maxValue, format, keyOverride)
EditBool(label, value, keyOverride)
EditInt(label, value, step, stepFast, keyOverride)
EditString(label, value, capacity, keyOverride, flags, multiline, multilineHeight)
hasRigidbody2D(ctx)
hasRigidbody2D()
getRigidbody2DVelocity(ctx)
getRigidbody2DVelocity()
setRigidbody2DVelocity(ctx, velocity)
setRigidbody2DVelocity(velocity)
moveTowards(current, target, maxDelta)
TryMoveRigidbody2D(ctx, targetVelocity, acceleration, drag, dt, outVelocity)
TryMoveRigidbody2D(targetVelocity, acceleration, drag, dt, outVelocity)
moveRigidbody2D(ctx, targetVelocity, acceleration, drag, dt)
moveRigidbody2D(targetVelocity, acceleration, drag, dt)
movePosition2D(ctx, delta)
movePosition2D(delta)
warnOnce(ctx, alreadyWarned, message, type)
warnOnce(alreadyWarned, message, type)
warnMissingComponentOnce(ctx, alreadyWarned, scriptName, componentName)
warnMissingComponentOnce(alreadyWarned, scriptName, componentName)
hasAudioSource(ctx)
hasAudioSource()
playSound(ctx, clipPath, volumeScale)
playSound(clipPath, volumeScale)
EditClipSelector(label, clipIndex)
EditDirectionalClipGrid(idleClips, walkClips)
EditSoundSet(heading, sounds, itemPrefix)
```

### Facades
```text
audio.HasSource()
audio.PlayOneShot(clipPath, volumeScale)
audio.Play()
audio.Stop()
time.deltaTime
ModuEngine.FPS
sprite.HasClips()
sprite.ClipCount()
sprite.ClipIndex()
sprite.SetClip(clipIndex)
sprite.SetClip(clipName)
sprite.ClipNameAt(clipIndex)
```

## ModuInput API
Available from `add ModuInput;`.
### Key Constants
```text
KEY_W
KEY_A
KEY_S
KEY_D
KEY_E
KEY_UP
KEY_DOWN
KEY_LEFT
KEY_RIGHT
KEY_SHIFT_LEFT
KEY_SHIFT_RIGHT
KEY_SPACE
KEY_ENTER
KEY_KP_ENTER
```

### Input Helpers
```text
KeyDown(ctx, key)
KeyDown(key)
KeyPressed(ctx, key)
KeyPressed(key)
IsRuntimeKeyDown(glfwKey, imguiKey)
IsSubmitDown()
input.WASD()
input.WASDNormalized()
input.sprint()
input.jump()
```
### Touch
Multi-touch + pointer input. On Android these are real fingers; on desktop the
left mouse button is reported as a single touch, so the same code runs in both.
Coordinates are surface pixels with the origin at the top-left.
```text
touch.Count()          // fingers down right now
touch.IsActive()       // anything touching?
touch.Position(index)  // i-th finger position (vec2)
touch.Primary()        // primary finger / mouse (vec2)
touch.Began()          // true the frame the primary touch goes down
touch.Ended()          // true the frame it lifts
touch.Tapped()         // alias for Began()
touch.Drag()           // primary-finger movement since last frame (vec2)
```

## ModuCPP.Experimental API
Available from `add ModuCPP.Experimental;`.
```text
Trim(value)
ParseInt(value, fallback)
ParseFloat(value, fallback)
ParseBool(value, fallback)
GetScriptSetting(script, key, fallback)
SetScriptSetting(script, key, value)
EscapeField(value, delimiter)
UnescapeField(value)
SplitEscaped(value, delimiter)
JoinEscaped(values, delimiter)
DeserializeObjectRefs(encoded)
SerializeObjectRefs(refs)
MakeObjectRef(objectId)
IsAllDigits(value)
ResolveSceneObjectRef(ctx, objectRef)
SetObjectEnabledState(ctx, object, enabled)
SetObjectsEnabledState(ctx, refs, enabled)
GetObjectReferencePosition(ctx, object)
GetCurrentObjectName(ctx, fallback)
TryPlayAnimationClipNamed(ctx, clipName)
ResolveUITextTarget(ctx, objectRef)
SetUITextLabel(ctx, objectRef, label)
SetUITextEffects(ctx, objectRef, effectFlags, animationSpeed, effectIntensity)
SetRigidbody2DSimulated(ctx, objectRef, simulated)
DrawStdStringInput(label, value, capacity, flags, multiline, multilineHeight)
DrawObjectRefInput(ctx, label, objectRef)
IsAudioClipPath(path)
DrawAudioClipInput(label, clipPath, capacity)
DrawObjectRefListEditor(ctx, label, refs)
```

## RMeshBuilder API
`add RMeshBuilder;` is currently reserved. `RMeshBuilderScriptApi.h` includes `ModuCPPScriptApi.h` but does not expose additional script-facing helpers yet.

## ScriptContext API
Scripts generally access this as `ctx`.

### Object Lookup And Object Metadata
```text
ctx.FindObjectByName(name)
ctx.FindObjectById(id)
ctx.ResolveObjectRef(ref)
ctx.IsObjectEnabled()
ctx.SetObjectEnabled(enabled)
ctx.GetLayer()
ctx.SetLayer(layer)
ctx.GetTag()
ctx.SetTag(tag)
ctx.HasTag(tag)
ctx.IsInLayer(layer)
ctx.GetSelectedObjectId()
ctx.MarkDirty()
```

### Transform And Input
```text
ctx.SetPosition(pos)
ctx.SetPosition2D(pos)
ctx.SetRotation(rot)
ctx.SetScale(scale)
ctx.GetPlanarYawPitchVectors(pitchDeg, yawDeg, outForward, outRight)
ctx.GetMoveInputWASD(pitchDeg, yawDeg)
ctx.ApplyMouseLook(pitchDeg, yawDeg, sensitivity, maxDelta, deltaTime, requireMouseButton)
ctx.IsSprintDown()
ctx.IsJumpDown()
ctx.IsKeyDown(glfwKey, imguiKey)
ctx.IsKeyPressed(glfwKey, imguiKey)
ctx.ResolveGround(capsuleHalf, probeExtra, groundSnap, verticalVelocity, ...)
ctx.ApplyVelocity(velocity, deltaTime)
```

### Standalone Movement
```text
ctx.BindStandaloneMovementSettings(settings)
ctx.DrawStandaloneMovementInspector(settings, showDebug)
ctx.TickStandaloneMovement(state, settings, deltaTime, debug)
ScriptContext::StandaloneMovementSettings
ScriptContext::StandaloneMovementState
ScriptContext::StandaloneMovementDebug
```

### UI And Sprite
```text
ctx.IsUIButtonPressed()
ctx.IsUIInteractable()
ctx.SetUIInteractable(interactable)
ctx.GetUISliderValue()
ctx.SetUISliderValue(value)
ctx.SetUISliderRange(minValue, maxValue)
ctx.SetUILabel(label)
ctx.SetUIColor(color)
ctx.SetUISliderStyle(style)
ctx.SetUIButtonStyle(style)
ctx.SetUIStylePreset(name)
ctx.RegisterUIStylePreset(name, style, replace)
ctx.GetUITextScale()
ctx.SetUITextScale(scale)
ctx.SetFPSCap(enabled, cap)
ctx.GetSpriteClipCount()
ctx.GetSpriteClipIndex()
ctx.GetSpriteClipName()
ctx.GetSpriteClipNameAt(index)
ctx.SetSpriteClipIndex(index)
ctx.SetSpriteClipName(name)
ctx.GetSpriteAlpha()
ctx.SetSpriteAlpha(alpha)
ctx.FadeSpriteAlpha(targetAlpha, duration, deltaTime)
ctx.FadeSpriteToClipIndex(clipIndex, fadeOutDuration, fadeInDuration, deltaTime)
ctx.FadeSpriteToClipName(clipName, fadeOutDuration, fadeInDuration, deltaTime)
```

### Physics
```text
ctx.HasRigidbody()
ctx.HasRigidbody2D()
ctx.EnsureCapsuleCollider(height, radius)
ctx.EnsureRigidbody(useGravity, kinematic)
ctx.SetRigidbody2DVelocity(velocity)
ctx.GetRigidbody2DVelocity(outVelocity)
ctx.SetRigidbodyVelocity(velocity)
ctx.GetRigidbodyVelocity(outVelocity)
ctx.AddRigidbodyVelocity(deltaVelocity)
ctx.SetRigidbodyAngularVelocity(velocity)
ctx.GetRigidbodyAngularVelocity(outVelocity)
ctx.AddRigidbodyForce(force)
ctx.AddRigidbodyImpulse(impulse)
ctx.AddRigidbodyTorque(torque)
ctx.AddRigidbodyAngularImpulse(impulse)
ctx.SetRigidbodyYaw(yawDegrees)
ctx.GetProjectGravityScale()
ctx.SetProjectGravityScale(scale)
ctx.RaycastClosest(origin, dir, distance, hitPos, hitNormal, hitDistance)
ctx.RaycastClosestDetailed(origin, dir, distance, hitPos, hitNormal, hitDistance, hitObjectId, hitObjectVelocity, hitStaticFriction, hitDynamicFriction)
ctx.SetRigidbodyRotation(rotDeg)
ctx.TeleportRigidbody(pos, rotDeg)
```

### Audio
```text
ctx.HasAudioSource()
ctx.PlayAudio()
ctx.StopAudio()
ctx.SetAudioLoop(loop)
ctx.SetAudioVolume(volume)
ctx.SetAudioClip(path)
ctx.PlayAudioOneShot(clipPath, volumeScale)
```

### Animation
```text
ctx.HasAnimation()
ctx.PlayAnimation(restart)
ctx.StopAnimation(resetTime)
ctx.PauseAnimation(pause)
ctx.ReverseAnimation(restartIfStopped)
ctx.SetAnimationTime(timeSeconds)
ctx.GetAnimationTime()
ctx.IsAnimationPlaying()
ctx.SetAnimationLoop(loop)
ctx.SetAnimationPlaySpeed(speed)
ctx.SetAnimationPlayOnAwake(playOnAwake)
```

### Settings, Files, HTTP, Console
```text
ctx.GetSetting(key, fallback)
ctx.SetSetting(key, value)
ctx.GetSettingBool(key, fallback)
ctx.SetSettingBool(key, value)
ctx.GetSettingFloat(key, fallback)
ctx.SetSettingFloat(key, value)
ctx.GetSettingVec3(key, fallback)
ctx.SetSettingVec3(key, value)
ctx.HttpPost(url, contentType, body, headers)
ctx.StartHttpPost(url, contentType, body, headers, stream)
ctx.PollHttpPost(requestId, outChunk, outDone, outSuccess)
ctx.CancelHttpPost(requestId)
ctx.ReadFileText(path)
ctx.WriteFileText(path, content)
ctx.DeleteFile(path)
ctx.ListFiles(path, recursive, maxEntries)
ctx.SearchFiles(root, query, maxResults)
ctx.GetProgramRootPath()
ctx.GetEngineDocsRootPath()
ctx.SaveProject()
ctx.AddConsoleMessage(message, type)
ctx.AutoSetting(key, value)
ctx.SaveAutoSettings()
```

## Common Enums
```text
ConsoleMessageType.Info
ConsoleMessageType.Warning
ConsoleMessageType.Error
ConsoleMessageType.Success

UISliderStyle.ImGui
UISliderStyle.Fill
UISliderStyle.Circle

UIButtonStyle.ImGui
UIButtonStyle.Outline

UIElementType.None
UIElementType.Canvas
UIElementType.Image
UIElementType.Slider
UIElementType.Button
UIElementType.Text
UIElementType.Sprite2D
```
The transpiler also accepts the C++ scoped enum form, for example `ConsoleMessageType::Warning`.
## The C Backend (Fast Compile Path)
The transpiler has two backends. The classic one lowers your script to C++ against
the full script api. The C backend lowers it to plain C against
`ScriptRuntimeCAPI.h` instead, and it exists for exactly one reason: the C++ path
pulls ~95k preprocessed lines into every script, the C path pulls ~100, so a
script that qualifies compiles in milliseconds instead of seconds. same script,
same behavior, just a faster trip through the compiler.

You do not opt in and you cannot get hurt by it. the transpiler tries C first and
the moment it meets anything it cannot prove it can lower, it silently falls back
to the C++ backend. to see which path a script took, look at the generated file
next to the compiled binaries: `.moducpp.gen.c` means fast path,
`.moducpp.gen.cpp` means classic. `MODUCPP_DISABLE_C_BACKEND=1` turns the whole
thing off if you ever need to rule it out while debugging.

What currently fits through the C path:
```text
public float / bool fields        (with [Header], [Separator] and [Slider], read-only in method bodies)
private float / int / bool / vec3 fields   (per-object state, literal initializers)
Begin / TickUpdate / Update       (plus pure helper methods without ctx)
Math.*                            (Sin, Cos, Clamp, Lerp, the usual suspects)
vec3 construction and .x/.y/.z    (but NOT vec3 arithmetic like a + b)
obj.Transform.position/rotation/scale   (reads, writes and compound writes)
AddLog(message, Type.X)
a ctx.* subset that maps 1:1 onto Modu_* C calls
```
Anything else, `each` loops, object refs, strings beyond literals, vec3 math,
custom inspectors, falls back. `Scripts/Spinny.moducpp` is the living demo of a
script that qualifies.
