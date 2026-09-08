# NOTE: This ModuCPP folder is made for AI Agents in mind!
## see `https://moduengine.xyz/docs` for a more format correct version.

# ModuCPP: The Complete Guide
A single, beginner-friendly walkthrough of the ModuCPP scripting language used by Modularity. Read it top to bottom and you should know enough to write, ship, and review real gameplay and editor scripts.
If you want short reference tables instead, see [ModuCPP_Language_Reference.md](../ModuCPP_Language_Reference.md). If you want bite-sized topic pages, see the [manual](manual/README.md) and [API](api/README.md) folders. This document is the long-form tour.

---

## Table of contents

1. [What ModuCPP is](#1-what-moducpp-is)
2. [The mental model: how a script becomes engine code](#2-the-mental-model-how-a-script-becomes-engine-code)
3. [Your first script](#3-your-first-script)
4. [Anatomy of a ModuCPP script](#4-anatomy-of-a-moducpp-script)
5. [Imports: bringing in modules](#5-imports-bringing-in-modules)
6. [Variables: public fields vs private fields](#6-variables-public-fields-vs-private-fields)
7. [Types you can use](#7-types-you-can-use)
8. [Lifecycle hooks](#8-lifecycle-hooks)
9. [Collision hooks (`OnCollideEnter` / `OnCollideHold` / `OnCollideExit`)](#9-collision-hooks)
10. [The built-ins available inside a hook](#10-the-built-ins-available-inside-a-hook)
11. [Raw scene-object access via `obj->`](#11-raw-scene-object-access-via-obj-)
12. [Timers and the `to` / `each` shorthand](#12-timers-and-the-to--each-shorthand)
13. [The inspector: three ways to expose data](#13-the-inspector-three-ways-to-expose-data)
14. [The `inspector { … }` DSL reference](#14-the-inspector----dsl-reference)
15. [The typed UI proxy: `obj.UI.*`](#15-the-typed-ui-proxy-objui)
16. [Input](#16-input)
17. [Engine helpers (`ModuEngine`)](#17-engine-helpers-moduengine)
18. [Editor-window scripts](#18-editor-window-scripts)
19. [The editor UI surface: `ModuGUI` / ImGui](#19-the-editor-ui-surface-modugui--imgui)
20. [Coroutines: `IEnum`](#20-coroutines-ienum)
21. [Sub-scripts: structured repeated data](#21-sub-scripts-structured-repeated-data)
22. [Other recognized syntax: lambdas, `$` strings, `Calc`, `mark`, std:: passthrough](#22-other-recognized-syntax)
23. [Common patterns, with full examples](#23-common-patterns-with-full-examples)
24. [Good practice checklist](#24-good-practice-checklist)
25. [Bad practice: examples of what not to do](#25-bad-practice-examples-of-what-not-to-do)
26. [Experimental, reserved, and shared helpers](#26-experimental-reserved-and-shared-helpers)
27. [Cheat sheet](#27-cheat-sheet)
28. [Troubleshooting & FAQ](#28-troubleshooting--faq)

---

## 1. What ModuCPP is

ModuCPP is the scripting language we use to write gameplay, UI, and editor logic in the Modularity engine. Scripts live in `.moducpp` files and look a bit like C#: you write a class, declare some fields, and implement a few well-known methods like `Begin()` and `TickUpdate()`.

Where it differs from a typical scripting language: **there is no separate gameplay VM**. ModuCPP is transpiled to C++, that C++ is compiled with the rest of the engine, and the result runs through the same native script runtime as everything else. ModuCPP is *authoring syntax* on top of the engine, not a sandbox sitting next to it.

That gives you a useful pair of properties:

- The friendly, declarative front end (no boilerplate, no manual hook signatures, no manual inspector wiring).
- The native back end (one runtime, one set of data structures, no marshalling cost between "script" and "engine"). Anything you can do in C++ in this codebase (call `std::`, use ImGui directly, read a raw `SceneObject*`) you can do in ModuCPP too.

If your background is **Unity**, think "MonoBehaviour, but compiled into the engine directly." If your background is **Unreal**, think "a leaner equivalent of UObject scripts." If your background is **plain C++**, think of it as a focused front-end that hides the most repetitive parts of writing engine glue.

ModuCPP is intentionally small at the front but wide at the back. The high-level keywords (`add`, `public class`, lifecycle hooks, attributes, `inspector { … }`) are a deliberately constrained set. Once you're past those, the underlying C++ surface (`ctx`, `obj`, every header in `include/`, the entire `std::` library, all of ImGui under `ModuGUI::`) is fully available.

---

## 2. The mental model: how a script becomes engine code

It helps to know the pipeline before you write your first line:

```
   you write           the transpiler           the C++ compiler
  ─────────────       ────────────────         ──────────────────
   MyScript.moducpp ─▶ generated C++       ─▶  native binary
                       (config storage,
                        runtime state,
                        hook entry points,
                        inspector code)
```

What this means in practice:

- **A `public` field is not just metadata.** The transpiler routes it into persisted config storage. The runtime, the inspector, and serialization all know how to find it because it lives in a known shape.
- **A `private` field becomes runtime-only state**, not saved to disk and not shown in the inspector.
- **A hook like `void Begin()` is a recognized name**, not just any method. The transpiler emits a real native entry point and wires it to the runtime.
- **Imports map to includes.** `add ModuEngine;` lowers to `#include "ModuEngineScriptApi.h"`. The set of helpers you can see is exactly the set the headers expose.
- **Anything not recognized as ModuCPP syntax falls through as plain C++.** A free function, a `namespace { … }`, an `std::vector<int>`, a hand-rolled `ImGui::Begin()` call: all valid.

You write the high-level shape. The transpiler handles the boring half. Everything else is just C++.

---

## 3. Your first script

A script that prints the frame rate to its own object's UI label, every frame:

```cpp
add ModuCPP;
add ModuEngine;

public class FPSDisplay : ModuNode
{
    void TickUpdate()
    {
        obj.UILabel = "FPS: " + IntR(ModuEngine.FPS);
    }
}
```

Five things to notice:

1. The two `add …;` lines at the top are the imports. `ModuCPP` is the core; `ModuEngine` is what gives us `ModuEngine.FPS`.
2. `public class FPSDisplay : ModuNode` is the standard declaration form. `ModuNode` is the recommended base.
3. `TickUpdate` is a recognized hook: the runtime calls it every frame.
4. `obj` is the current scene object, made available for free inside the hook. Assigning to `obj.UILabel` sets the UI label on this object.
5. `IntR(x)` rounds to the nearest integer.

Save this as `Assets/Scripts/FPSDisplay.moducpp`, attach it to an object with a UI text component, and you have a working frame-rate readout.

A slightly more realistic version adds configuration through public fields:

```cpp
add ModuCPP;
add ModuEngine;

public class FPSDisplay : ModuNode
{
    public bool clampTo120 = false;
    public string prefix = "FPS: ";

    void Begin()
    {
        if (clampTo120) {
            ctx.SetFPSCap(true, 120.0f);
        }
    }

    void TickUpdate()
    {
        obj.UILabel = prefix + IntR(ModuEngine.FPS);
    }
}
```

This shape (*imports → public fields for configuration → `Begin` for setup → `TickUpdate` for per-frame work*) is the backbone of most real ModuCPP scripts.

---

## 4. Anatomy of a ModuCPP script

A `.moducpp` file is recognized when it contains a class that derives from one of the two script base types:

```cpp
public class MyScript : ModuNode      // preferred for new code
public class MyScript : ModuBehaviour // also supported
```

Both bases share the same hook set. For new gameplay scripts, default to `ModuNode`. `ModuBehaviour` exists because older scripts use it; there is no reason to pick it for greenfield work.

The full set of declaration keywords the transpiler recognizes:

| Keyword | Meaning |
| --- | --- |
| `add` | Package import at the top of the file. |
| `mark` | Forward-declares a namespace so other scripts can `add` it. Uncommon. |
| `public` | Field is persisted and exposed to the inspector. |
| `private` | Field is runtime-only state. |
| `class` | Standard class declaration. |
| `enum` | Standard enum declaration. Becomes `enum class` in the lowered output. |
| `SubScript` | A nested, serializable data block (see [§21](#21-sub-scripts-structured-repeated-data)). |
| `inspector` | Opens an `inspector { … }` block, the declarative inspector DSL. |
| `to` | Expression-bodied method/hook shorthand (`void Begin() to timer.Start(0.5f);`). |
| `Calc` | Declares a static helper method via `Calc Name(args) to expr;`. Uncommon. |
| `ref` | In/out parameter (mirrors C# `ref`). |
| `each` | Object-list iteration shorthand (`each list.state(true);`). |
| `in`, `then` | Used in the long form of `each`: `each ref item in list then item.State(true);`. |

After the transpile step, ordinary C++ keywords pass through unchanged: `if`, `else`, `for`, `while`, `return`, `static`, `const`, `auto`, `struct`, `namespace`, `using`, `switch`, `case`, `break`, `continue`, `true`, `false`, `nullptr`. You'll use them whenever the high-level surface doesn't have a shorthand.

Member access on the high-level surface uses `.` (not `->`) for the parts ModuCPP wraps (`obj.UILabel`, `obj.UI.Text`, `timer.Start(…)`, `Math.Clamp(…)`). When you reach into the raw `SceneObject` through `obj->name` or `obj->position`, that's normal C++ pointer access, see [§11](#11-raw-scene-object-access-via-obj-).

Helper functions and small types can live outside the class:

```cpp
add ModuCPP;

namespace {
    float Smooth(float v) { return v * v * (3.0f - 2.0f * v); }
}

public class MyScript : ModuNode { /* … */ }
```

---

## 5. Imports: bringing in modules

Every script begins with one or more `add …;` lines. Each maps to a single C++ include, so the rule of thumb is **import the smallest surface that solves your problem**:

| Import | What it gives you | When to use |
| --- | --- | --- |
| `add ModuCPP;` | Core authoring surface: `ctx`, `obj`, `time`, type aliases, `Math.*`, `IntR*`, `FloatR`, timer helpers, `Config<T>()`/`State<T>()`, `BindSetting`, `SubScript` serializers, `AddLog`, `Color`, the `ModuGUI`/ImGui surface, `obj.UI` typed proxy. | Almost every script. |
| `add ModuEngine;` | Engine helpers: `ModuEngine.FPS`, `audio`/`sprite` facades, `TryMoveRigidbody2D`, `moveTowards`, inspector widgets (`EditFloat`, `EditVec3`, …), project gravity, `warnOnce`. | Gameplay scripts that touch movement, audio, sprites, or extra editor widgets. |
| `add ModuInput;` | Input: `KEY_*` constants, `KeyDown`, `KeyPressed`, `IsSubmitDown`, the `input` facade (`WASD`, `WASDNormalized`, `sprint`, `jump`). | Anything that reads input. |
| `add ModuCPP.Experimental;` | Opt-in: object-ref serialization, UI text targeting, parsing helpers, advanced editor widgets. | Tools, dialogue/menu/interaction scripts, prototype work. Treat as unstable. |
| `add RMeshBuilder;` | Reserved. The import is recognized but the header currently exposes no script-facing helpers. | Don't import it yet. |

A common mistake: adding `add ModuCPP;` and then expecting `input.WASD()` or `ModuEngine.FPS` to be in scope. They are not. Each module is independent, and importing what you actually use is a feature, not a chore: it makes a script's dependencies obvious at the top of the file.

Two niche import forms also work:

- `using PackageName;`: synonym for `add`, accepted for readability.
- `mark NamespaceName;`: forward-declares an empty namespace so another script can `add` it later. Rare; only used when scripts coordinate across files.

---

## 6. Variables: public fields vs private fields

This is the single most important distinction in ModuCPP. Get it right and most of the rest follows.

**Public fields** are persisted and exposed to the inspector. They are configuration data: what a designer would change without rebuilding.

**Private fields** are runtime-only state. They are not saved, not shown in the inspector, and reset back to their initializer between sessions.

```cpp
public class PatrolGuard : ModuNode
{
    // Configuration. Designers can edit these. They survive a reload.
    public float speed       = 3.0f;
    public float pauseTime   = 1.5f;
    public Vector3 patrolEnd = Vector3(10, 0, 0);

    // Runtime state. Lives only during play.
    private float timer    = 0.0f;
    private bool  movingTo = true;
    private Vector3 origin = Vector3(0, 0, 0);
}
```

A useful test when you can't decide: *"If a designer reloads the scene, should this value reset?"* If yes, it's private. If no, it's public.

Default values are normal:

```cpp
public float speed = 4.0f;
public string label = "Start";
public bool enabled = true;
```

Field types that work as public fields today, drawn from real scripts in the repository:

`bool`, `int`, `float`, `string`, `Vector2`, `Vector3`, `vec2`, `vec3`, enums, `string[]`, `int[4]`, `int[4][4]`, `string[6]`, `SceneObj[]`, `List<SceneObj*>`, `SubScript[]`, `DialoguePort::DialogueLine[]`.

There is a second authoring family (`Config<T>()` plus `State<T>()` with manual settings binding) used mostly by tool/subsystem scripts. It's covered briefly in [§18](#18-editor-window-scripts). For ordinary gameplay scripts, prefer plain public/private fields.

---

## 7. Types you can use

Most of the types you'll write are familiar. The transpiler rewrites the script-side spelling into the matching C++ type:

| Script-side | Generated C++ |
| --- | --- |
| `string` | `std::string` |
| `vec2`, `Vector2` | `glm::vec2` |
| `vec3`, `Vector3` | `glm::vec3` |
| `SceneObj`, `SceneObject` | `SceneObject` |
| `SceneObj*`, `SceneObject*` | `SceneObject*` |
| `Col` | `SceneObject*` (used in collision hook parameters) |
| `List<T>` | `std::vector<T>` |
| `T[]` | `std::vector<T>` |
| `T[N]` | `std::array<T, N>` |
| `T[R][C]` | `std::array<std::array<T, C>, R>` |

A few common method calls are rewritten for convenience so the script side reads naturally:

- `v.Length()` → `glm::length(v)`
- `a.Dot(b)` → `glm::dot(a, b)`
- `list.IsEmpty()` → `list.empty()`

Enum members can be accessed with `.`; the transpiler rewrites them to `::` when both names start with a capital. So `ConsoleMessageType.Warning` becomes `ConsoleMessageType::Warning` in the lowered code. Write it the friendly way.

There is also a small set of numeric helpers in the core module:

- `IntRD(x)`: round toward negative infinity (floor-like).
- `IntR(x)`: round to nearest.
- `IntRU(x)`: round toward positive infinity (ceiling-like).
- `FloatR(value, decimals = 2)`: format a float as a string with N decimals.
- `Math.Max`, `Math.Min`, `Math.Clamp`, `Math.Abs`.
- `Color(r, g, b, a = 1.0f)` / `Color(rgb, a = 1.0f)`: build a `vec4` colour.

`IntR` returns a small wrapper type that implicitly stringifies, which is why `"FPS: " + IntR(ModuEngine.FPS)` works as natural string concatenation. The same applies to `FloatR`.

---

## 8. Lifecycle hooks

A "hook" is a method with a recognized name. The transpiler turns it into a native entry point and the runtime calls it at the right time. You do not register, declare a signature, or wire it manually: naming it correctly is enough.

The runtime-side hooks:

| Hook | When it fires | Typical use |
| --- | --- | --- |
| `void Begin()` | Once, before the first update. | Setup: start timers, cache references, initialize state, ensure required components. |
| `void TickUpdate()` | Every frame. (Variants: `TickUpdate(float dt)`, `TickUpdate(ScriptContext& ctx, float dt)`.) | The main per-frame body: input, movement, UI updates, timers. |
| `void Update()` | Every frame, older-style. | Supported for back-compat. Prefer `TickUpdate` for new scripts. |
| `void Spec()` | When the engine runs the script in "spec mode". | Runtime-spec behavior. Most scripts don't need this. |
| `void TestEditor()` | When the engine runs the script in editor test mode. | Editor-side tests. Most scripts don't need this. |

The editor-side hooks:

| Hook | When it fires | Typical use |
| --- | --- | --- |
| `void Script_OnInspector()` | When the inspector is drawn for this script. | Fully manual inspector drawing. Skip unless the declarative form isn't enough. |
| `void RenderEditorWindow()` | When this script is a standalone editor-window tool. | Drawing the contents of an editor window. |
| `void ExitRenderEditorWindow()` | When the same editor window closes. | Cleanup for an editor window. |

The collision hooks (`OnCollideEnter` / `OnCollideHold` / `OnCollideExit`) get their own section, see [§9](#9-collision-hooks).

Each of the inspector/editor hooks also accepts an explicit `(ScriptContext& ctx)` overload if you want it.

You don't have to declare `ScriptContext& ctx` to use `ctx`. The transpiler auto-injects it into hook bodies through a small prelude. Same for `obj` (the current object) and `dt` (the frame delta in tick-style hooks). You can still take them as parameters if you prefer to be explicit.

**What is *not* a hook:** `OnEnable`, `OnDisable`, `OnDestroy`, `OnTriggerEnter`, `Awake`, `Start` (use `Begin`), and the Unity-style update variants like `FixedUpdate` or `LateUpdate`. The recognized hook list above is exhaustive. If you write a method with one of those names, it just becomes a regular function that nothing calls.

A complete minimal lifecycle example:

```cpp
add ModuCPP;

public class Heartbeat : ModuNode
{
    public float interval = 1.0f;
    private float timer   = 0.0f;
    private int   beats   = 0;

    void Begin()
    {
        timer.Start(interval);
        AddLog("Heartbeat ready", Type.Info);
    }

    void TickUpdate()
    {
        if (!timer.Ready()) return;
        beats = beats + 1;
        AddLog("Beat " + IntR(beats));
    }
}
```

---

## 9. Collision hooks

When two physics objects collide, the runtime calls these on any script attached to either side:

| Hook | When it fires |
| --- | --- |
| `void OnCollideEnter(Col other)` | The frame a new collision begins. |
| `void OnCollideHold(Col other)` | Every ~2 seconds while still in contact (configurable per script, see below). |
| `void OnCollideExit(Col other)` | The frame the collision ends. |

`Col` is an alias for `SceneObject*`: it's just a pointer to the other object. The name is a shorthand for "collider" but there's no per-hit struct with normals, depth, or impact velocity. If you need those, raycast or query the physics state through `ctx` separately.

The `OnCollideHold` interval is **2.0 seconds by default**. To change it on a per-script basis, add a trailing float to the parameter list:

```cpp
add ModuCPP;

public class GateLock : ModuNode
{
    [ObjectList] public string[] toEnable;

    void OnCollideEnter(Col other)
    {
        AddLog("Touched by " + other->name, Type.Info);
    }

    // Fires every 3.5 seconds while contact is held.
    void OnCollideHold(Col other 3.5f)
    {
        each toEnable.state(true);
    }

    void OnCollideExit(Col other)
    {
        each toEnable.state(false);
    }
}
```

That trailing `3.5f` looks unusual but it's the documented per-script firing interval, not a default argument.

Collision hooks do not auto-inject `dt`. They only auto-inject `ctx` (so `ctx.AddConsoleMessage(…)` and the other engine calls still work inside them). The collision target object is `other`.

For collision to fire at all, both objects need physics components, typically a `Rigidbody2D` and a `Collider2D` (or their 3D counterparts), on at least one side. If a hook never runs, the object is missing physics, not the script is broken.

---

## 10. The built-ins available inside a hook

Inside any lifecycle hook, the following names are already in scope. You don't import them, declare them, or pass them around.

### `ctx`: the script context

`ctx` is your bridge to the engine. It is the most-used identifier in any non-trivial script. A short list of the categories of things it can do:

- **Current object**: `ctx.object`, `ctx.MarkDirty()`, `ctx.IsObjectEnabled(…)`, `ctx.SetObjectEnabled(…)`, `ctx.FindObjectByName(…)`, `ctx.FindObjectById(…)`, `ctx.ResolveObjectRef(…)`.
- **Iteration**: `ctx.GetSceneObjectCount()`, `ctx.GetSceneObjectIdAt(i)`, `ctx.GetSelectedObjectId()`.
- **Tag/Layer**: `ctx.GetTag()`, `ctx.SetTag(s)`, `ctx.HasTag(s)`, `ctx.GetLayer()`, `ctx.SetLayer(n)`, `ctx.IsInLayer(n)`.
- **Transform**: `ctx.SetPosition`, `ctx.SetPosition2D`, `ctx.SetRotation`, `ctx.SetScale`.
- **Input** (whether or not `ModuInput` is imported): `ctx.GetMoveInputWASD`, `ctx.IsSprintDown`, `ctx.IsJumpDown`, `ctx.IsKeyDown`, `ctx.IsKeyPressed`, `ctx.ApplyMouseLook`.
- **Physics 3D**: `ctx.HasRigidbody`, `ctx.EnsureRigidbody(useGravity=true, kinematic=false)`, `ctx.EnsureCapsuleCollider(height, radius)`, `ctx.SetRigidbodyVelocity`, `ctx.GetRigidbodyVelocity`, `ctx.AddRigidbodyVelocity`, `ctx.AddRigidbodyForce`, `ctx.AddRigidbodyImpulse`, `ctx.AddRigidbodyTorque`, `ctx.AddRigidbodyAngularImpulse`, `ctx.SetRigidbodyAngularVelocity`, `ctx.GetRigidbodyAngularVelocity`, `ctx.SetRigidbodyYaw`, `ctx.SetRigidbodyRotation`, `ctx.TeleportRigidbody(pos, rotDeg)`, `ctx.RaycastClosest`, `ctx.RaycastClosestDetailed`.
- **Physics 2D**: `ctx.HasRigidbody2D`, `ctx.SetRigidbody2DVelocity`, `ctx.GetRigidbody2DVelocity`.
- **Other objects** (operate on a target by id, not the current one): `ctx.SetObjectRigidbodyVelocity`, `ctx.GetObjectRigidbodyVelocity`, `ctx.AddObjectRigidbodyImpulse`, `ctx.TeleportObjectRigidbody`.
- **Gravity**: `ctx.GetProjectGravityScale`, `ctx.SetProjectGravityScale(s)`.
- **UI**: `ctx.SetUILabel`, `ctx.SetUIColor`, `ctx.SetUIInteractable`, `ctx.IsUIButtonPressed`, `ctx.IsUIHovered`, `ctx.IsUIActive`, `ctx.GetUISliderValue`, `ctx.SetUISliderValue`, `ctx.SetUISliderRange`, `ctx.SetUISliderStyle`, `ctx.SetUIButtonStyle`, `ctx.SetUIStylePreset`, `ctx.RegisterUIStylePreset`, `ctx.SetUITextScale`, `ctx.GetUITextScale`, `ctx.SetFPSCap`.
- **Sprite/animation**: `ctx.GetSpriteClipIndex`, `ctx.SetSpriteClipIndex`, `ctx.SetSpriteClipName`, `ctx.GetSpriteClipName`, `ctx.GetSpriteClipCount`, `ctx.GetSpriteClipNameAt`, `ctx.SetSpriteAlpha`, `ctx.GetSpriteAlpha`, `ctx.FadeSpriteAlpha(target, duration, dt)`, `ctx.FadeSpriteToClipIndex(idx, fadeOut, fadeIn, dt)`, `ctx.FadeSpriteToClipName`, `ctx.PlayAnimation`, `ctx.StopAnimation`, `ctx.PauseAnimation`, `ctx.ReverseAnimation`, `ctx.SetAnimationLoop`, `ctx.IsAnimationPlaying`, `ctx.SetAnimationPlaySpeed`, `ctx.SetAnimationPlayOnAwake`, `ctx.SetAnimationTime`, `ctx.GetAnimationTime`.
- **Audio**: `ctx.HasAudioSource`, `ctx.PlayAudio`, `ctx.StopAudio`, `ctx.PlayAudioOneShot`, `ctx.SetAudioVolume`, `ctx.SetAudioLoop`, `ctx.SetAudioClip`.
- **Settings & files**: `ctx.GetSetting`, `ctx.SetSetting`, `ctx.AutoSetting`, `ctx.SaveAutoSettings`, `ctx.ReadFileText`, `ctx.WriteFileText`, `ctx.DeleteFile`, `ctx.ListFiles`, `ctx.SearchFiles`, `ctx.SaveProject`, `ctx.GetProgramRootPath`, `ctx.GetEngineDocsRootPath`.
- **HTTP**: `ctx.HttpPost`, `ctx.StartHttpPost`, `ctx.PollHttpPost`, `ctx.CancelHttpPost`.
- **Console**: `ctx.AddConsoleMessage("…", ConsoleMessageType::Info)`. There is also a friendlier free-function form (`AddLog(message, Type.Info)`) covered below.

The full member list lives in [api/type-scriptcontext.md](api/type-scriptcontext.md). The names above cover what 90% of scripts ever need.

### `obj`: the current scene object

`obj` is a small facade around `ctx.object`. You can:

- Bool-check it: `if (!obj) return;`
- Pointer-style read fields: `obj->name`, `obj->id`, `obj->position`, `obj->rotation`. See [§11](#11-raw-scene-object-access-via-obj-) for the full surface.
- Use the typed UI proxy: `obj.UI.Text`, `obj.UI.Position`, `obj.UI.OnClick(…)`. See [§15](#15-the-typed-ui-proxy-objui).
- Assign UI labels directly with the legacy shorthand: `obj.UILabel = "Hello";`

If a hook acts on the object the script is attached to, prefer `obj` over `ctx.object`.

### `dt`: frame delta time

A `float` representing the seconds since the last frame, available inside update-style hooks. Use it for any time-scaled work:

```cpp
void TickUpdate()
{
    pos = pos + velocity * dt;
}
```

You can also reach it via `time.deltaTime` from the `ModuCPP` core, or via the free function `DeltaTime()`.

### `AddLog` and `Type`: the console shorthand

The friendly form of `ctx.AddConsoleMessage`:

```cpp
AddLog("started");                       // Info by default
AddLog("low on health", Type.Warning);
AddLog("save failed", Type.Error);
AddLog("level loaded", Type.Success);
```

`Type.Info`, `Type.Warning`, `Type.Error`, `Type.Success` are inline constants that map to `ConsoleMessageType` values. The `Type.X` syntax is rewritten to `Type::X` automatically.

### `input`: the input facade (requires `add ModuInput;`)

Convenient high-level input:

- `input.WASD()`: raw `Vector2` from the WASD keys.
- `input.WASDNormalized()`: unit-length when any key is held; zero when none. **Use this** for movement so diagonals are not faster.
- `input.sprint()`: true while sprint (Shift) is held.
- `input.jump()` (true while jump is held. This is *not* an edge) see [§16](#16-input) for a one-shot jump.

### `audio` and `sprite`: component facades (require `add ModuEngine;`)

Tiny wrappers around the corresponding components on the current object:

- `audio.HasSource()`, `audio.PlayOneShot(path, volumeScale)`, `audio.Play()`, `audio.Stop()`.
- `sprite.HasClips()`, `sprite.ClipCount()`, `sprite.ClipIndex()`, `sprite.SetClip(index)`, `sprite.SetClip(name)`, `sprite.ClipNameAt(index)`.

Both fail silently when the component is missing. Guard with the matching `Has*` check if the object might not have one.

### `ModuEngine`: the engine facade

A namespaced value with engine-scope info such as `ModuEngine.FPS`.

### `timer`: the timer shorthand

Not really a separate object: it's a rewrite around a `float` field you declared. See [§12](#12-timers-and-the-to--each-shorthand).

---

## 11. Raw scene-object access via `obj->`

`obj` implicitly converts to a `SceneObject*`, so you can dereference straight through with `obj->fieldName`. Same goes for `ctx.object->fieldName`, `myRef->fieldName` where `myRef` is a `SceneObj*`, or `other->fieldName` inside a collision hook.

This is your escape hatch when the high-level facades don't cover a case. Use it carefully (the high-level surface exists for a reason) but know it's there.

### Identity / hierarchy

```cpp
obj->name        // std::string
obj->id          // int
obj->tag         // std::string ("Untagged" by default)
obj->layer       // int
obj->parentId    // int, -1 if root
obj->childIds    // std::vector<int>
obj->enabled     // bool; toggle for "active in hierarchy"
```

### Transform

```cpp
obj->position    // vec3 world-space
obj->rotation    // vec3 (Euler, degrees)
obj->scale       // vec3
obj->localPosition / obj->localRotation / obj->localScale
```

**Important:** if the object has a physics rigidbody, direct writes to `obj->position` bypass the physics sync and can put the visual and the simulation out of step. Prefer `ctx.SetPosition(vec3)` or `ctx.TeleportRigidbody(pos, rotDeg)` for physics-driven objects.

### Component presence flags

Each major component family has a `bool has…` flag plus a matching struct. Read the flag first, then read the struct fields if it's true:

```
hasRigidbody          hasRigidbody2D
hasCollider           hasCollider2D
hasAudioSource        hasVideoPlayer
hasAnimation          hasSkeletalAnimation
hasLight              hasLight2D
hasCamera             hasCameraFollow2D
hasUI                 hasRenderer
hasParticleSystem2D   hasParallaxLayer2D
hasReverbZone         hasShadowCaster2D
hasPlayerController   hasAIAgent
hasRig25DRoot         hasRig25DNode
hasPostFX             hasReflectionCast
hasGroundBakedType    hasObsticleObject
```

A typical guard:

```cpp
if (!obj->hasRigidbody2D) {
    warnMissingComponentOnce(ctx, warnedNoRb, "Mover2D", "Rigidbody2D");
    return;
}
```

### Rendering and material

```cpp
obj->meshPath               // std::string
obj->materialPath           // std::string
obj->albedoTexturePath
obj->normalMapPath
obj->shaderPackPath
obj->useOverlay             // bool
```

### Free helpers that take a `SceneObject*`

These come from `add ModuCPP;` and let you write less code than `ctx.…` calls when the target is something other than `ctx.object`:

- `CurrentObject()`: `SceneObj` for `ctx.object`.
- `ResolveObject(ref)` / `SmartResolveRef(ref)`: turn an object name or `"Object.ID-N"` string into a `SceneObject*`.
- `HasUI(obj)`: UI-component check.
- `SetActive(obj, active)`: toggles `obj->enabled` and dirties context.
- `PlaySound(objectRef, volumeScale = 1.0f)`: plays the named object's audio source.
- `PlaySoundClip(clipPath, volumeScale = 1.0f)`: plays a clip directly.

A few more free shorthands that operate on the current object (no `ctx.` prefix needed): `SetPosition`, `SetRotation`, `SetScale`, `MarkDirty`, `DeltaTime`, `SetFPSCap`, `GetSetting`/`SetSetting` and friends, `SaveAutoSettings`.

---

## 12. Timers and the `to` / `each` shorthand

ModuCPP has a tiny built-in timer pattern that covers most "do something every N seconds" needs.

Declare a private `float` for the accumulator, start it in `Begin`, then early-return in `TickUpdate` while it isn't ready:

```cpp
add ModuCPP;

public class PulseLogger : ModuNode
{
    public float interval = 0.5f;
    private float timer   = 0.0f;

    void Begin() to timer.Start(interval);

    void TickUpdate()
    {
        if (!timer.Ready()) return;
        AddLog("Pulse");
    }
}
```

The shorthand is a pair of rewrites on **any `float` field** (the name `timer` is just a convention):

- `<field>.Start(interval)` → `::ModuCPP::StartTimer(<field>, interval)`
- `<field>.Ready()`         → `::ModuCPP::TimerReady(<field>)`
- `<field>.Ready(interval)` → `::ModuCPP::TimerReady(<field>, interval)`

`TimerReady` advances by the current frame delta on its own. After it returns true, it rolls into the next interval automatically, so the example above pulses once every half-second indefinitely. For a one-shot, set a private `bool fired` and gate on it.

### The `to` keyword

`to` is an expression-bodied method shorthand. Any method whose entire body is a single expression can use it:

```cpp
void Begin() to timer.Start(interval);          // void → expression as-is
bool IsReady() to timer.Ready;                  // non-void → emits `return …;`
int  ClampFrame(int f) to Math.Clamp(f, 0, 3);  // non-void → emits `return …;`
```

It's a readability tool. Don't reach for it in branching or multi-statement code: write a normal body instead.

### The `each` shorthand for object lists

For persisted object-list fields, you can enable or disable every referenced object in one line:

```cpp
[ObjectList] public string[] toEnable;
[ObjectList] public string[] toDisable;

void Begin()
{
    each toEnable.state(true);
    each toDisable.state(false);
}
```

Equivalent to resolving each ref against the scene and calling `SetObjectEnabled`. Use it for menus and event-triggered batches.

There is a longer form that lets you name the loop variable, although the short form covers most needs:

```cpp
each ref item in toEnable then item.State(true);
```

### The `Calc` keyword

`Calc Name(args) to expr;` declares a static helper method. Rare (most helpers go in `namespace { … }` instead) but available:

```cpp
Calc int DoubleIt(int x) to x * 2;
```

---

## 13. The inspector: three ways to expose data

Public fields are the contract between a script and a designer. ModuCPP gives you three escalating ways to draw them, and the right pick depends on how much control you need.

### A) Automatic: declare and you're done

If you don't need control over labels, ordering, or sections, just declare public fields and stop.

```cpp
public class Mover : ModuNode
{
    public float speed = 3.0f;
    public bool  loop  = true;
}
```

You get a default inspector with both fields, in the order they were declared.

### B) Attributes: tweak each field where it lives

When you want a slider, a header, a tooltip, or a different widget for an array, annotate the field. Attributes live on the line above the field:

```cpp
public class Patroller : ModuNode
{
    [Header("Speed")]
    [Slider(0.0f, 10.0f)]
    public float speed = 3.0f;

    [Header("Targets")]
    [ObjectList]
    public string[] waypoints;

    [Separator]
    [Header("Audio")]
    [SoundSet("Footsteps")]
    public string[] footstepSounds;
}
```

Recognized attributes:

| Attribute | Effect |
| --- | --- |
| `[Header("Title")]` | Insert a heading and visual separator before the field. |
| `[Slider(min, max)]` | Slider widget. Works on `float`, `int`, `vec3`. |
| `[ObjectRef]` | A `string` field becomes a single object-reference picker. |
| `[ObjectList]` | A `string[]` becomes an object-reference list. |
| `[DialogueLines]` | Mark an array as dialogue data. |
| `[ClipGridPair]` | For an `int[4]` followed by `int[4][4]`: animation grid. |
| `[Separator]` | Visual separator before the field. |
| `[SoundSet("Label")]` | Sound-set editor on a `string[]`. |
| `@range(min, max)` | Range metadata for numeric auto inspectors. |
| `@step(value)` | Drag-step metadata for numeric auto inspectors. |

`[ObjectRef]` fields also enable a truthiness shortcut: `if (myRef)` becomes `if (!myRef.empty())` under the hood, so you can write the natural form.

### C) The declarative `inspector { … }` block

When you want tabs, sections, runtime status panels, or a fully custom layout, open an `inspector { … }` block on the class. This is the canonical "rich inspector" form and it covers almost all custom UIs without dropping into manual drawing.

```cpp
public class Patroller : ModuNode
{
    public float speed = 3.0f;
    public string[] waypoints;
    private int currentWaypoint = 0;

    inspector
    {
        Tabs
        {
            Tab("Config")
            {
                Header("Movement");
                AutoFields(speed);
                Header("Path");
                ObjectList("Waypoints", waypoints);
            }
            Tab("Runtime")
            {
                Run(ModuGUI::TextDisabled("Current waypoint: %d", currentWaypoint));
            }
        }
    }
}
```

`AutoFields(...)` is a convenience: it draws the named *persisted public fields on this class* using their normal widgets. It does not see private fields, `Config<T>()` storage, or values from other classes; use the explicit `Slider`/`String`/`Number`/`ObjectRef`/etc. statements for those.

`Run(...)` is the escape hatch. Whatever you put inside is run when the inspector draws, so it can show ModuGUI/ImGui text, run migrations, or call any other helper.

### D) Fully manual: `Script_OnInspector()`

If even the DSL is not enough (which is rare), implement `void Script_OnInspector()` and draw the entire panel yourself with the engine's editor helpers. This is the path used by tool/subsystem scripts that build their own widgets from scratch. For ordinary gameplay scripts, you should not need this.

---

## 14. The `inspector { … }` DSL reference

The DSL is statement-oriented. Statements end with a semicolon; containers use braces.

### Containers (open with `{ … }`)

- `Tabs { … }`: a tab bar.
- `Tab(title) { … }`: one tab inside `Tabs`.
- `Section(title) { … }`: a labelled section.
- `Group { … }`, `Group(title) { … }`: a logical group.
- `Foldout(title) { … }`: collapsible section.

### Field statements (each ends with `;`)

- Storage: `Config(Type, varName);`, `AutoSave(configVar);`, `Save(configVar);`.
- Layout: `Header(title);`, `Separator();`.
- Logic escape hatch: `Run(expression);` runs the expression when the inspector draws.

### Widget statements

Each accepts both an unlabelled form and a labelled form:

- `Toggle(value);` / `Toggle(label, value);`
- `Slider(value, min, max);` / `Slider(label, value, min, max);`
- `Number(value);` / `Number(label, value);`
- `String(value);` / `String(label, value);`
- `Enum(value);` / `Enum(label, value);`
- `ObjectRef(value);` / `ObjectRef(label, value);`
- `ObjectList(value);` / `ObjectList(label, value);`
- `AudioClip(value);` / `AudioClip(label, value);`
- `DialogueLines(lines);` / `DialogueLines(label, lines);`
- `TextEffectFlags(effect);` / `TextEffectFlags(label, effect);`
- `InteractionOptions(options);` / `InteractionOptions(label, options);`
- `MenuActions(refs, actions);` / `MenuActions(label, refs, actions);`
- `ClipGrid(idleClips, walkClips);`
- `SoundSet(label, sounds);`
- `AutoFields(field1, field2, …);`: auto-draw persisted public fields by name.

### Runtime-status helpers

These are no-arg widgets that print the current runtime state of a system. They are intended for the "Runtime" tab of a multi-tab inspector:

- `RuntimeDialogueStatus();`
- `RuntimeInteractableStatus();`
- `RuntimeMenuStatus();`

---

## 15. The typed UI proxy: `obj.UI.*`

For objects that have a UI component, `obj.UI` is a typed proxy that lets you read and write UI fields with natural assignments and register input callbacks with lambdas. It comes with `add ModuCPP;`.

### Existence check

```cpp
if (!obj.UI.Exists) return;
if (obj.UI.IsText)   { /* … */ }
if (obj.UI.IsButton) { /* … */ }
if (obj.UI.IsSlider) { /* … */ }
if (obj.UI.IsImage)  { /* … */ }
```

### Generic properties (read and write)

These work on any UI element:

| Property | Type |
| --- | --- |
| `obj.UI.Text` | `string` |
| `obj.UI.FontSize` | `float` |
| `obj.UI.Position` | `vec2` |
| `obj.UI.Size` | `vec2` |
| `obj.UI.Rotation` | `float` (degrees) |
| `obj.UI.Scale` | `vec2` |
| `obj.UI.Color` | `vec4` |
| `obj.UI.TextColor` | `vec4` |
| `obj.UI.BackgroundColor` | `vec4` |
| `obj.UI.Tint` | `vec4` |
| `obj.UI.Alpha` | `float` (clamped 0…1) |
| `obj.UI.Visible` | `bool` (proxy for `enabled`) |
| `obj.UI.Enabled` | `bool` (same as `Visible`) |
| `obj.UI.Interactable` | `bool` |
| `obj.UI.Layer` | `int` |
| `obj.UI.SortingOrder` (alias `obj.UI.Order`) | `int` |
| `obj.UI.Pivot` | `vec2` (clamped 0…1) |
| `obj.UI.Anchor` | `int` (UIAnchor cast) |

Plus two convenience methods:

- `obj.UI.SetFront()`: sortingOrder = 1000.
- `obj.UI.SetBack()`: sortingOrder = -1000.

### Typed sub-proxies

For widgets that have widget-specific fields:

```cpp
// Buttons
obj.UI.Button.Text;
obj.UI.Button.Enabled;
obj.UI.Button.Interactable;

// Sliders
obj.UI.Slider.Label;
obj.UI.Slider.Value;
obj.UI.Slider.Min;
obj.UI.Slider.Max;
obj.UI.Slider.FillColor;
obj.UI.Slider.BackgroundColor;
obj.UI.Slider.TextColor;
obj.UI.Slider.TextSize;

// Images
obj.UI.Image.Texture;   // string path
obj.UI.Image.Tint;      // vec4
```

`obj.UI.Toggle` and `obj.UI.Input` exist as stubs. They warn once and return defaults: the engine doesn't have those UI types yet. Treat them as reserved.

### Event callbacks (lambdas)

Use the arrow lambda syntax (covered in [§22](#22-other-recognized-syntax)):

```cpp
void Begin()
{
    obj.UI.OnClick(() => {
        AddLog("button clicked");
    });

    obj.UI.OnHover(() => { AddLog("hover in"); });
    obj.UI.OnUnhover(() => { AddLog("hover out"); });

    obj.UI.Slider.OnValueChanged((float v) => {
        AddLog("slider: " + FloatR(v));
    });
}
```

Available events: `OnClick`, `OnUnclick`, `OnPress`, `OnRelease`, `OnHover`, `OnUnhover` (plus slider/toggle/input-specific ones where the widget supports it).

### UI proxies on `[ObjectRef]` fields

If you have an `[ObjectRef] public string someUiTarget;`, you can drive that other object's UI through the same proxy form:

```cpp
[ObjectRef] public string statusLabel;

void TickUpdate()
{
    statusLabel.UI.Text = "FPS: " + IntR(ModuEngine.FPS);
}
```

The transpiler rewrites these into the appropriate engine helpers automatically.

---

## 16. Input

Input lives in `ModuInput`. There are two ergonomic styles.

### High-level facade (`input.*`): for movement and standard actions

```cpp
add ModuCPP;
add ModuInput;

public class Walker : ModuNode
{
    public float speed = 4.0f;

    void TickUpdate()
    {
        Vector2 move = input.WASDNormalized();
        ctx.SetPosition2D(Vector2(obj->position.x, obj->position.y) + move * speed * dt);
    }
}
```

| Call | Returns | Notes |
| --- | --- | --- |
| `input.WASD()` | `Vector2` | Raw component-summed direction. Diagonals are longer than 1. |
| `input.WASDNormalized()` | `Vector2` | Unit length when held, zero when idle. Use this for movement. |
| `input.sprint()` | `bool` | True while Shift is held. |
| `input.jump()` | `bool` | True while Space is held. **Held state, not an edge.** |

### Key-level polling: for one-shots and bindings

For "press once" behaviour you need either `KeyPressed` (edge-triggered) or a manual previous-frame check:

```cpp
add ModuCPP;
add ModuInput;

public class JumpOnce : ModuNode
{
    public float jumpImpulse = 5.0f;
    private bool prevJump    = false;

    void TickUpdate()
    {
        bool nowJump = input.jump();
        bool pressed = nowJump && !prevJump;
        if (pressed) {
            ctx.AddRigidbodyImpulse(Vector3(0, jumpImpulse, 0));
        }
        prevJump = nowJump;
    }
}
```

Recognized key constants: `KEY_W`, `KEY_A`, `KEY_S`, `KEY_D`, `KEY_E`, `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`, `KEY_SHIFT_LEFT`, `KEY_SHIFT_RIGHT`, `KEY_SPACE`, `KEY_ENTER`, `KEY_KP_ENTER`.

Free functions: `KeyDown(key)`, `KeyPressed(key)`, `IsRuntimeKeyDown(glfwKey, imguiKey)`, `IsSubmitDown()`.

---

## 17. Engine helpers (`ModuEngine`)

`add ModuEngine;` covers the bulk of "I want a thing the engine already does." Highlights:

### Movement (free functions on `ctx`)

- `TryMoveRigidbody2D(ctx, targetVelocity, acceleration, drag, dt[, outVelocity])`: accelerates and drags toward a target velocity. Fails silently if there is no `Rigidbody2D` on the object.
- `moveRigidbody2D(ctx, targetVelocity, acceleration, drag, dt)`: same shape, no `out` velocity.
- `movePosition2D(ctx, delta)`: direct 2D position offset.
- `moveTowards(current, target, maxDelta)`: clamp a step toward a target.
- `EnsureRigidbody(useGravity = true, kinematic = false)`: create one if missing.
- `EnsureCapsuleCollider(height, radius)`: same for a capsule collider.

### Audio facade

- `audio.HasSource()`, `audio.PlayOneShot(path, volumeScale)`, `audio.Play()`, `audio.Stop()`.

### Sprite facade

- `sprite.HasClips()`, `sprite.ClipCount()`, `sprite.ClipIndex()`, `sprite.SetClip(…)`, `sprite.ClipNameAt(index)`.

### Inspector / editor widgets

For use inside `Script_OnInspector` and editor windows:

- `EditFloat(label, value, speed, min, max, format, keyOverride)`
- `EditInt(label, value, step, stepFast, keyOverride)`
- `EditBool(label, value, keyOverride)`
- `EditVec3(label, value, …)`
- `EditString(label, value, capacity, keyOverride, flags, multiline, multilineHeight)`
- `EditClipSelector(label, clipIndex)`
- `EditDirectionalClipGrid(idleClips, walkClips)`
- `EditSoundSet(heading, sounds, itemPrefix)`

### Engine values

- `ModuEngine.FPS`: current frame rate.
- `GetProjectGravityScale()`, `SetProjectGravityScale(scale)`.

### Warnings (avoid log spam)

- `warnOnce(ctx, alreadyWarned, message, type)`: log a one-time warning gated by a `bool`.
- `warnMissingComponentOnce(ctx, alreadyWarned, scriptName, componentName)`: the same pattern, specialized for missing components.

---

## 18. Editor-window scripts

For scripts that act as standalone editor tools rather than scene components, two extra hooks come into play:

- `void RenderEditorWindow()`: draw the window each frame the editor opens it.
- `void ExitRenderEditorWindow()`: cleanup when the window closes.

Tool scripts often also use the `Config<T>()` + `State<T>()` storage pattern instead of public/private fields, because the script isn't attached to a scene object:

```cpp
add ModuCPP;
add ModuEngine;

public class MyTool : ModuNode
{
    inspector
    {
        Config(MyToolConfig, config);
        AutoSave(config);
        // … widgets that read/write fields on `config` …
    }

    void RenderEditorWindow()
    {
        auto& config = Config<MyToolConfig>();
        // draw the tool window using ModuGUI / EditFloat / etc.
    }
}
```

A few rules for `Config<T>()` if you go down this path:

- It only allocates storage. To actually persist values, call `BindSetting` / `BindArray` / `BindArray2D` from the `ModuCPP` module.
- Re-bind in every hook that *reads* the config, not just the inspector hook, or you will see stale data at runtime.
- After mutating settings manually, call `ctx.SaveAutoSettings()` to flush.

For ordinary gameplay scripts, you do not need any of this. Use public/private fields.

---

## 19. The editor UI surface: `ModuGUI` / ImGui

Editor-window scripts and manual inspectors draw their UI with the engine's bundled ImGui. ModuCPP exposes the entire ImGui API under two interchangeable names:

- `ModuGUI::Whatever(…)`: preferred.
- `ImGui::Whatever(…)`: same function, accepted everywhere.

They are literally aliases. Use either prefix; pick one and be consistent within a file.

There is one Modularity-specific helper on top of ImGui:

- `ModuGUI::SubsectionFoldout(label, flags = 0)`: a styled `TreeNodeEx` used by `inspector Foldout { … }` and `EditDirectionalClipGrid`.

Everything else (`Begin`, `End`, `Text`, `TextWrapped`, `Button`, `Checkbox`, `InputText`, `SliderFloat`, `Combo`, `BeginChild`/`EndChild`, `BeginTabBar`, `BeginTable`, `BeginPopup`, drag-drop, style stacks, ID stacks) is the ImGui surface you already know.

The supporting ImGui types and macros are also in scope:

| Name | Use |
| --- | --- |
| `ImVec2(x, y)` | 2-float position/size constructor. |
| `ImVec4(r, g, b, a)` | 4-float colour constructor. |
| `IM_ARRAYSIZE(arr)` | Array length macro. |
| `ImGuiInputTextFlags_*` | Input text flags (`EnterReturnsTrue`, `Password`, `ReadOnly`, …). |
| `ImGuiTreeNodeFlags_*`, `ImGuiTableFlags_*`, `ImGuiWindowFlags_*`, `ImGuiChildFlags_*`, `ImGuiSelectableFlags_*` | Widget flag enums. |
| `ImGuiCol_*` | Colour slot enum for `PushStyleColor`. |
| `ImGuiStyleVar_*` | Style variable enum for `PushStyleVar`. |
| `ImGuiKey_*` | Key codes used by `ctx.IsKeyDown(int glfwKey, ImGuiKey imguiKey)`. |

A short editor-window example showing common ModuGUI patterns:

```cpp
add ModuCPP;
add ModuEngine;

public class TinyTool : ModuNode
{
    void RenderEditorWindow()
    {
        ModuGUI::TextUnformatted("Quick controls");
        ModuGUI::Separator();

        static float speed = 1.0f;
        ModuGUI::SliderFloat("Speed", &speed, 0.0f, 10.0f);

        static bool showAdvanced = false;
        ModuGUI::Checkbox("Show advanced", &showAdvanced);

        if (showAdvanced) {
            if (ModuGUI::BeginChild("##adv", ImVec2(0, 120), true)) {
                ModuGUI::TextDisabled("Advanced options…");
            }
            ModuGUI::EndChild();
        }

        if (ModuGUI::Button("Reset", ImVec2(80, 0))) {
            speed = 1.0f;
        }
    }
}
```

For the full ImGui API, treat the upstream ImGui reference as authoritative: every function is callable through `ModuGUI::`.

---

## 20. Coroutines: `IEnum`

For "do something across many frames" work, ModuCPP exposes a small coroutine-style helper layer built on free functions plus a few macros:

```cpp
add ModuCPP;

// A coroutine is just a free function taking (ctx, dt).
IEnum void FadeOut(ScriptContext& ctx, float dt)
{
    static float alpha = 1.0f;
    alpha -= dt * 0.5f;
    ctx.SetSpriteAlpha(alpha);
    if (alpha <= 0.0f) {
        IEnum_Stop(FadeOut);   // request that the runtime drop it
    }
}

public class FadeOnClick : ModuNode
{
    void Begin()
    {
        IEnum_Start(FadeOut);
    }
}
```

The relevant pieces:

- `IEnum`: empty marker macro on the function declaration; documents intent.
- `IEnum_Start(fn)` → `ctx.StartIEnum(fn)`: begin running.
- `IEnum_Stop(fn)`  → `ctx.StopIEnum(fn)`: stop running.
- `IEnum_Ensure(fn)` → `ctx.EnsureIEnum(fn)`: start if not already running.
- `ctx.IsIEnumRunning(fn)`, `ctx.StopAllIEnums()`: query and bulk-stop.

The function signature is fixed: `void fn(ScriptContext&, float dt)`. The runtime calls it every frame until it's stopped.

---

## 21. Sub-scripts: structured repeated data

When you need a list of structured items (a menu of options, an interaction with multiple branches, a wave of enemies with per-wave settings), use `SubScript`. It declares a named, serializable record type that can be used as a field type:

```cpp
add ModuCPP;

public class MainMenuController : ModuNode
{
    SubScript MenuAction
    {
        public SceneObj[] enable;
        public SceneObj[] disable;
    };

    public MenuAction[] actions;

    void Begin()
    {
        // actions is a normal array; each element has `enable` and `disable` lists.
    }
}
```

This is much better than flattening into parallel arrays. The inspector understands `SubScript[]` and gives you a per-entry editor, and the values serialize as one structured block.

Helpers (in the `ModuCPP` module):

- `SerializeSubScript(value)`, `DeserializeSubScript<T>(encoded)`
- `SerializeSubScriptArray(values)`, `DeserializeSubScriptArray<T>(encoded)`
- `EditSubScript(label, value)`, `EditSubScriptArray(label, values)`: for manual inspector drawing.

---

## 22. Other recognized syntax

A handful of small syntactic conveniences that don't fit cleanly into the sections above.

### Arrow lambdas: `(args) => { body }`

The transpiler rewrites this into `[&](args) { body }`, so lambdas have natural capture-by-reference semantics:

```cpp
obj.UI.OnClick(() => { AddLog("click"); });
obj.UI.Slider.OnValueChanged((float v) => { AddLog(FloatR(v)); });
```

### `$"…"` strings: newline normalization only

`$` in front of a string literal lets the string contain raw newlines, which are normalized to `\n` for the compiler:

```cpp
public string greeting = $"Hello,
this is a multi-line greeting.";
```

**Important:** `$` strings do *not* do `{var}` interpolation, despite looking like C#. If you want a value spliced in, concatenate it:

```cpp
AddLog("Hello, " + name + "!");
```

### `mark Foo;`: namespace forward-declaration

Declares an empty `namespace Foo {}` so that another script can `add Foo;` and see it. Rare; only needed when scripts coordinate across files.

### `Calc Name(args) to expr;`: static helper

A one-line static-method declaration. Most helpers go in `namespace { … }` instead, but `Calc` is available if you want the shorter form.

### Standard library passthrough

ModuCPP doesn't restrict `std::`. The core script include transitively brings in:

`<algorithm>`, `<array>`, `<cmath>`, `<cctype>`, `<cstdio>`, `<cstdlib>`, `<cfloat>`, `<limits>`, `<regex>`, `<string>`, `<vector>`, `<unordered_map>`, `<unordered_set>`, `<typeinfo>`, `<functional>`.

So the following are available without an extra `#include` line:

- Strings: `std::string`, `std::to_string`, `std::stof`, `std::stoi`
- Containers: `std::vector`, `std::array`, `std::unordered_map`, `std::unordered_set`
- Algorithms: `std::max`, `std::min`, `std::clamp`, `std::abs`, `std::floor`, `std::ceil`, `std::lround`, `std::sort`, `std::transform`, `std::find_if`
- Function objects: `std::function`, `std::move`, `std::pair`, `std::tuple`
- Formatting: `std::snprintf`, `std::printf`
- Regex: `std::regex`, `std::regex_replace`, `std::regex_search`, `std::regex_match`, `std::smatch`

For anything else, add an `#include <header>` line at the top of the script: it passes straight through the transpiler.

### `MODU_SCRIPT(ctx)`: manual prelude

Lifecycle methods get this prelude injected automatically. If you write a free helper function and want `obj` available inside it without taking it as a parameter, call `MODU_SCRIPT(ctx)` at the top of the function body: it sets up the same `obj` facade. Advanced; rarely needed.

---

## 23. Common patterns, with full examples

These are the recurring shapes you will see in real scripts.

### Repeating timer

```cpp
add ModuCPP;

public class Pulser : ModuNode
{
    public float interval = 0.5f;
    private float timer   = 0.0f;

    void Begin() to timer.Start(interval);

    void TickUpdate()
    {
        if (!timer.Ready()) return;
        AddLog("tick");
    }
}
```

### Live status label

```cpp
add ModuCPP;
add ModuEngine;

public class StatusLabel : ModuNode
{
    void TickUpdate()
    {
        obj.UI.Text = "FPS: " + IntR(ModuEngine.FPS);
    }
}
```

### WASD-driven 2D mover

```cpp
add ModuCPP;
add ModuEngine;
add ModuInput;

public class Mover2D : ModuNode
{
    public float speed = 4.0f;
    public float accel = 20.0f;
    public float drag  = 12.0f;

    void TickUpdate()
    {
        Vector2 move = input.WASDNormalized();
        TryMoveRigidbody2D(ctx, move * speed, accel, drag, dt);
    }
}
```

### One-shot input action (edge detection)

```cpp
add ModuCPP;
add ModuInput;

public class ShootOnce : ModuNode
{
    private bool prevFire = false;

    void TickUpdate()
    {
        bool nowFire = KeyDown(KEY_SPACE);
        bool pressed = nowFire && !prevFire;
        if (pressed) {
            AddLog("Bang");
        }
        prevFire = nowFire;
    }
}
```

### Collision-driven gate

```cpp
add ModuCPP;

public class TriggerGate : ModuNode
{
    [ObjectList] public string[] toEnable;
    [ObjectList] public string[] toDisable;

    void OnCollideEnter(Col other)
    {
        AddLog("Triggered by " + other->name, Type.Info);
        each toEnable.state(true);
        each toDisable.state(false);
    }

    void OnCollideExit(Col other)
    {
        each toEnable.state(false);
        each toDisable.state(true);
    }
}
```

### Clickable button via `obj.UI`

```cpp
add ModuCPP;

public class Clicker : ModuNode
{
    private int count = 0;

    void Begin()
    {
        obj.UI.Button.Text = "Click me (0)";
        obj.UI.OnClick(() => {
            count = count + 1;
            obj.UI.Button.Text = "Click me (" + IntR(count) + ")";
        });
    }
}
```

### Object-list driven scene state

```cpp
add ModuCPP;

public class OpenGate : ModuNode
{
    [ObjectList] public string[] toEnable;
    [ObjectList] public string[] toDisable;

    void Begin()
    {
        each toEnable.state(true);
        each toDisable.state(false);
    }
}
```

### Declarative inspector with a runtime tab

```cpp
add ModuCPP;
add ModuEngine;

public class Patroller : ModuNode
{
    public float speed   = 3.0f;
    public Vector3 start = Vector3(0, 0, 0);
    public Vector3 end   = Vector3(10, 0, 0);

    private bool movingToEnd = true;

    inspector
    {
        Tabs
        {
            Tab("Config")
            {
                Header("Movement");
                AutoFields(speed);
                Header("Path");
                AutoFields(start, end);
            }
            Tab("Runtime")
            {
                Run(ModuGUI::TextDisabled("Moving to end: %s", movingToEnd ? "yes" : "no"));
            }
        }
    }

    void TickUpdate() { /* … movement … */ }
}
```

---

## 24. Good practice checklist

- **Prefer `ModuNode`** for new gameplay scripts. Use `ModuBehaviour` only when you are maintaining an older script that already uses it.
- **Import only what you use.** Each `add …;` declares a real dependency. A long, speculative import list hides what a script actually needs.
- **`public` is for configuration, `private` is for state.** If a designer should never see it, it is not public. If you want it to survive a reload, it is not private.
- **Set up once in `Begin`, run every frame in `TickUpdate`.** First-frame initialization in `TickUpdate` is a smell.
- **Multiply time-based work by `dt`.** Anything else is frame-rate dependent.
- **Use `input.WASDNormalized()` for movement.** `WASD()` makes diagonals 1.41× faster.
- **Detect button presses with an edge**, not by reading the held-state on every frame.
- **Guard against missing components** with the matching `Has*` check (`audio.HasSource()`, `ctx.HasRigidbody2D()`, `obj->hasRigidbody2D`). These calls fail silently.
- **Move physics-driven objects through `ctx.SetPosition` or `ctx.TeleportRigidbody`**, not by writing `obj->position` directly. Direct writes skip physics sync.
- **Use `obj.UI.*`** for UI-attached objects instead of raw `obj->ui.*` reads: it's a thinner, typed surface that auto-dirties.
- **Use `each list.state(true|false);`** for object-list batches instead of a hand-written loop.
- **Use the `inspector { … }` DSL** for tabs/sections/runtime status. Drop to `Script_OnInspector()` only when you genuinely need it.
- **Call `ctx.MarkDirty()` after editor-time scene mutations** so the change is saved.
- **Use `warnOnce` / `warnMissingComponentOnce`** when a script can't do its job without a component. Once per missing thing is enough; spamming logs hides real issues.
- **`SubScript` over parallel arrays** any time the data has structure. The inspector and serialization will thank you.
- **Pick one of `ModuGUI::` or `ImGui::` per file**: both work, but mixing them on adjacent lines is noisy.

---

## 25. Bad practice: examples of what not to do

Side-by-side examples of common mistakes, with the right shape next to them.

### 25.1 Making runtime state public

```cpp
// BAD: pollutes the inspector and gets serialized to disk.
public bool isJumping = false;
public float jumpTimer = 0.0f;

// GOOD: these are pure runtime state.
private bool isJumping = false;
private float jumpTimer = 0.0f;
```

### 25.2 Initializing in `TickUpdate`

```cpp
// BAD: runs every frame, allocates and resets repeatedly.
private bool started = false;
void TickUpdate()
{
    if (!started) {
        ctx.SetFPSCap(true, 120.0f);
        started = true;
    }
    // …
}

// GOOD: one-time work belongs in Begin.
void Begin()
{
    ctx.SetFPSCap(true, 120.0f);
}
```

### 25.3 Using `input.WASD()` for movement

```cpp
// BAD: diagonal movement is sqrt(2) faster.
Vector2 move = input.WASD();
TryMoveRigidbody2D(ctx, move * speed, accel, drag, dt);

// GOOD: same intent, consistent speed in all directions.
Vector2 move = input.WASDNormalized();
TryMoveRigidbody2D(ctx, move * speed, accel, drag, dt);
```

### 25.4 Treating a held-state helper as a press

```cpp
// BAD: fires every frame the key is held.
void TickUpdate()
{
    if (input.jump()) {
        ctx.AddRigidbodyImpulse(Vector3(0, 5, 0));
    }
}

// GOOD: detect the press edge with a previous-frame state.
private bool prevJump = false;
void TickUpdate()
{
    bool nowJump = input.jump();
    if (nowJump && !prevJump) {
        ctx.AddRigidbodyImpulse(Vector3(0, 5, 0));
    }
    prevJump = nowJump;
}
```

### 25.5 Time-based code that ignores `dt`

```cpp
// BAD: speed depends on frame rate.
obj->position = obj->position + Vector3(speed, 0, 0);

// GOOD: speed is meters per second, regardless of FPS.
ctx.SetPosition(obj->position + Vector3(speed * dt, 0, 0));
```

### 25.6 Writing `obj->position` directly on a physics object

```cpp
// BAD: bypasses the physics sync; visuals and simulation drift apart.
obj->position = startPos;

// GOOD: let the rigidbody know about the teleport.
ctx.TeleportRigidbody(startPos, obj->rotation);
```

### 25.7 Mutating scene data in the editor without `MarkDirty`

```cpp
// BAD: the change won't be saved.
void RenderEditorWindow()
{
    target->position = target->position + Vector3(1, 0, 0);
}

// GOOD: let the editor know the scene changed.
void RenderEditorWindow()
{
    target->position = target->position + Vector3(1, 0, 0);
    ctx.MarkDirty();
}
```

### 25.8 Expecting `AutoFields` to see things it can't

```cpp
// BAD: `currentWaypoint` is private; AutoFields will silently skip it.
private int currentWaypoint = 0;
inspector { AutoFields(speed, currentWaypoint); }

// GOOD: show runtime state with Run(...).
inspector
{
    AutoFields(speed);
    Run(ModuGUI::TextDisabled("Current waypoint: %d", currentWaypoint));
}
```

### 25.9 Forgetting `[ObjectRef]` / `[ObjectList]`

```cpp
// BAD: looks like a free-form string in the inspector.
public string target;
public string[] waypoints;

// GOOD: designer sees an object picker / list.
[ObjectRef]  public string target;
[ObjectList] public string[] waypoints;
```

### 25.10 Flattening structured data into parallel arrays

```cpp
// BAD: three arrays that must stay in sync by index.
public string[]   optionName;
public SceneObj[] optionEnable;
public SceneObj[] optionDisable;

// GOOD: one structured array.
SubScript Option
{
    public string    name;
    public SceneObj[] enable;
    public SceneObj[] disable;
};
public Option[] options;
```

### 25.11 Assuming components exist

```cpp
// BAD: silent failure on objects without a Rigidbody2D.
void TickUpdate()
{
    TryMoveRigidbody2D(ctx, target, accel, drag, dt);
}

// GOOD: warn once and skip cleanly.
private bool warnedNoRb = false;
void TickUpdate()
{
    if (!ctx.HasRigidbody2D()) {
        warnMissingComponentOnce(ctx, warnedNoRb, "Mover2D", "Rigidbody2D");
        return;
    }
    TryMoveRigidbody2D(ctx, target, accel, drag, dt);
}
```

### 25.12 Importing speculatively

```cpp
// BAD: misleading. Nothing in this script uses Engine, Input, or Experimental.
add ModuCPP;
add ModuEngine;
add ModuInput;
add ModuCPP.Experimental;

// GOOD: exactly the dependencies the script uses.
add ModuCPP;
```

### 25.13 Reaching for `to` when the body is not a single expression

```cpp
// BAD: readable as a normal body, not as a shorthand.
void Begin() to ctx.AddConsoleMessage(complicated ? "a" : "b");

// GOOD: use the shorthand only for genuinely one-expression methods.
void Begin()
{
    if (complicated) AddLog("a");
    else             AddLog("b");
}
```

### 25.14 Expecting `$"…"` to interpolate

```cpp
// BAD: this prints the literal text "Hello, {name}!"
AddLog($"Hello, {name}!");

// GOOD: concatenate values explicitly.
AddLog("Hello, " + name + "!");
```

### 25.15 Treating `Col` as a hit record

```cpp
// BAD: Col has no .normal, .point, .impulse; it's just a SceneObject*.
void OnCollideEnter(Col other)
{
    Vector3 n = other.normal;   // does not compile
}

// GOOD: Col is the other object. Use it for identity / fields,
// and raycast separately if you need a normal or hit point.
void OnCollideEnter(Col other)
{
    AddLog("Hit " + other->name + " on layer " + IntR(other->layer));
}
```

---

## 26. Experimental, reserved, and shared helpers

### `add ModuCPP.Experimental;`

Real, shipped, and used by dialogue/menu/interaction sample scripts, but explicitly **opt-in** and not part of the stable core. Treat its surface as a moving target.

Highlights:

- **Object references**: `MakeObjectRef`, `SerializeObjectRefs`, `DeserializeObjectRefs`, `ResolveSceneObjectRef`, `ResolveUITextTarget`.
- **Object/UI state**: `SetObjectEnabledState`, `SetObjectsEnabledState`, `SetUITextLabel`, `SetUITextEffects`, `SetRigidbody2DSimulated`, `TryPlayAnimationClipNamed`, `GetCurrentObjectName`, `GetObjectReferencePosition`.
- **Script settings**: `GetScriptSetting`, `SetScriptSetting`.
- **String / parse helpers**: `Trim`, `ParseInt`, `ParseFloat`, `ParseBool`, `EscapeField`, `UnescapeField`, `SplitEscaped`, `JoinEscaped`, `IsAllDigits`.
- **Editor widgets**: `DrawStdStringInput`, `DrawObjectRefInput`, `DrawAudioClipInput`, `DrawObjectRefListEditor`.

Import this only when you genuinely need a helper from it, and isolate the dependency to the script that uses it.

### `add RMeshBuilder;`

Reserved. The import is recognized and the header exists, but it currently exposes no script-facing helpers. Do not write code against it yet: wait until the documented surface lands.

### `DialoguePort` namespace

`Scripts/DialoguePortShared.h` is a shipped *shared sample helper*, not core language. It backs the dialogue / interaction / menu sample scripts (`DialogueSystem`, `InteractableObject`, `MainMenuController`) and depends on `ModuCPP.Experimental`. Useful when reproducing those patterns; do not assume new scripts need it.

### Shipped example types

The pattern docs reference enums and sub-scripts like `FacingDirection`, `MenuOrientation`, `MenuAction`, `InteractableType`, `InteractionOption`, and `MouthState`. These are *examples from shipped scripts*, not built-in primitives. Copy them as a starting point when relevant; don't expect them to exist by default.

### `obj.UI.Toggle` and `obj.UI.Input`

Reserved sub-proxies. They exist in the API surface so you can write code targeting them, but the engine doesn't have those UI element types yet. Calls currently warn-once and return defaults. Don't rely on them.

### The managed bridge (`Scripts/Managed/ModuCPP.cs`)

This is a parallel authoring path for Mono/C# tools. It exposes `Context`, `ImGui`, and `Inspector` to managed code only; current ABI version is **7**. You will not call into it from a `.moducpp` script, and `.moducpp` scripts do not call out to it. It exists; you don't need it to write gameplay.

---

## 27. Cheat sheet

### Skeleton

```cpp
add ModuCPP;          // core
add ModuEngine;       // engine helpers (FPS, movement, audio facade, …)
add ModuInput;        // input

public class MyScript : ModuNode
{
    public float speed = 4.0f;     // configuration (persisted, inspector-visible)
    private float timer = 0.0f;    // runtime state

    void Begin() to timer.Start(0.5f);

    void TickUpdate()
    {
        if (!timer.Ready()) return;
        // per-tick work
    }
}
```

### Hooks

Runtime: `Begin` · `TickUpdate(float dt)` · `Update` · `Spec` · `TestEditor`

Editor: `Script_OnInspector` · `RenderEditorWindow` · `ExitRenderEditorWindow`

Collision: `OnCollideEnter(Col other)` · `OnCollideHold(Col other [interval])` · `OnCollideExit(Col other)`

### Built-ins in any hook

`ctx` · `obj` · `dt` · `AddLog` · `Type.{Info,Warning,Error,Success}` · (with `ModuInput`) `input` · (with `ModuEngine`) `audio`, `sprite`, `ModuEngine.FPS`, `time.deltaTime`

### Type aliases

`string` `vec2/Vector2` `vec3/Vector3` `SceneObj/SceneObject` `Col` `List<T>` `T[]` `T[N]` `T[R][C]`

### Numeric helpers

`IntRD`, `IntR`, `IntRU`, `FloatR`, `Math.Max`, `Math.Min`, `Math.Clamp`, `Math.Abs`, `Color(r,g,b,a)`

### Field attributes

`[Header]` `[Slider(min,max)]` `[ObjectRef]` `[ObjectList]` `[DialogueLines]` `[ClipGridPair]` `[Separator]` `[SoundSet("Label")]` `@range(min,max)` `@step(v)`

### Inspector DSL keywords

`Tabs` · `Tab(title)` · `Section(title)` · `Group` · `Foldout(title)` · `Header(title)` · `Separator()` · `Config(T, name)` · `AutoSave(c)` · `Save(c)` · `AutoFields(...)` · `Run(expr)` · widget statements (`Toggle`, `Slider`, `Number`, `String`, `Enum`, `ObjectRef`, `ObjectList`, `AudioClip`, `DialogueLines`, `TextEffectFlags`, `InteractionOptions`, `MenuActions`, `ClipGrid`, `SoundSet`) · runtime status (`RuntimeDialogueStatus`, `RuntimeInteractableStatus`, `RuntimeMenuStatus`)

### `obj.UI` (typed UI proxy)

Props: `Text` `FontSize` `Position` `Size` `Rotation` `Scale` `Color` `TextColor` `BackgroundColor` `Tint` `Alpha` `Visible` `Enabled` `Interactable` `Layer` `SortingOrder` `Pivot` `Anchor`

Sub-proxies: `Button.{Text,Enabled,Interactable}` `Slider.{Value,Min,Max,…}` `Image.{Texture,Tint}`

Events: `OnClick` `OnUnclick` `OnPress` `OnRelease` `OnHover` `OnUnhover` `Slider.OnValueChanged`

### Input

`input.WASD()` · `input.WASDNormalized()` · `input.sprint()` · `input.jump()` · `KeyDown(KEY_X)` · `KeyPressed(KEY_X)` · `IsSubmitDown()`

### Editor UI

`ModuGUI::*` (alias of `ImGui::*`) · `ImVec2/ImVec4` · `IM_ARRAYSIZE` · `ModuGUI::SubsectionFoldout(label)` (Modularity-specific)

### Coroutines

`IEnum void Fn(ScriptContext& ctx, float dt) { … }` · `IEnum_Start(Fn)` · `IEnum_Stop(Fn)` · `IEnum_Ensure(Fn)`

### Timer shorthand

```cpp
private float timer = 0.0f;
void Begin() to timer.Start(0.5f);
void TickUpdate()
{
    if (!timer.Ready()) return;
    // …
}
```

### Object-list shorthand

```cpp
[ObjectList] public string[] toEnable;
each toEnable.state(true);
```

### Lambda

```cpp
obj.UI.OnClick(() => { AddLog("click"); });
```

---

## 28. Troubleshooting & FAQ

**My script's field doesn't show up in the inspector.**
Is it `public`? Private fields are runtime-only. Is its type supported (see [§7](#7-types-you-can-use))? Are you in a `Tab` or `Section` and forgot to add it via `AutoFields(field)` or an explicit widget? `AutoFields` only sees persisted public fields on the same class.

**`TryMoveRigidbody2D` does nothing.**
The current object probably has no `Rigidbody2D`. The helper fails silently. Gate with `if (!ctx.HasRigidbody2D()) { warnMissingComponentOnce(ctx, warnedNoRb, "MyScript", "Rigidbody2D"); return; }`.

**`OnCollideEnter` never fires.**
Both objects need physics components (a rigidbody plus a collider) and at least one of them needs to be moving. If the script is on a static prop with no collider, the collision system has nothing to detect.

**`OnCollideHold` fires way too often / not often enough.**
The default interval is 2 seconds. Override it per-script with `void OnCollideHold(Col other 0.5f) { … }` or whatever interval you want.

**My one-shot key action fires every frame.**
`input.jump()` and `KeyDown(...)` report *held* state, not a press. Detect the press edge with a previous-frame `bool` or use `KeyPressed(...)`.

**My speed/timing is different at 60 FPS vs 120 FPS.**
You forgot to multiply by `dt`. Time-based math should always be `value * dt` for per-second behaviour.

**`add ModuCPP;` doesn't seem to include `input`.**
Correct: it doesn't. Imports are independent. Add `add ModuInput;` (and `add ModuEngine;` for `ModuEngine.FPS`, audio, movement, etc.).

**`AutoFields` doesn't show a value from `Config<T>()` or a private field.**
That's by design. `AutoFields` is for persisted public fields on the current class. For everything else, draw it explicitly with the matching widget statement or with `Run(...)`.

**My editor change vanishes when I reopen the scene.**
After mutating scene data in an editor-time hook, call `ctx.MarkDirty()` so the editor knows to save.

**I get stale data when I read a `Config<T>()` value at runtime.**
You need to re-bind the settings on every hook that reads the config, not only when the inspector draws. Then call `ctx.SaveAutoSettings()` after manual changes.

**My logs are flooding with "missing component" messages.**
Use `warnOnce(ctx, warned, "...", type)` or `warnMissingComponentOnce(ctx, warned, "MyScript", "Rigidbody2D")` with a private `bool` gate. One warning is enough.

**My `$"Hello, {name}"` string prints `{name}` literally.**
`$` strings only enable embedded newlines; they don't interpolate. Use `"Hello, " + name`.

**`obj->position` works but my physics object jumps weirdly.**
Direct writes to `obj->position` bypass the physics sync. Use `ctx.SetPosition(…)` for non-physics objects and `ctx.TeleportRigidbody(pos, rotDeg)` for rigidbody-driven ones.

**Can I use `ImGui::Button` instead of `ModuGUI::Button`?**
Yes. `ModuGUI` is a `using namespace ImGui;` alias: the only Modularity-specific addition is `ModuGUI::SubsectionFoldout`. Pick one prefix per file and stick to it.

**Can I include extra std headers?**
Yes. `#include <header>` at the top of the file passes straight through. The common ones (`<string>`, `<vector>`, `<algorithm>`, `<cmath>`, `<regex>`, `<unordered_map>`, `<functional>`) are already in scope, so you usually don't need to.

**Where is the canonical reference for keyword X / member Y?**
The terse reference is [ModuCPP_Language_Reference.md](../ModuCPP_Language_Reference.md). The topic-by-topic deep dives are under [manual/](manual/README.md) and the per-type pages are under [api/](api/README.md). This document is the single-file tour; the reference docs are the source of truth.
