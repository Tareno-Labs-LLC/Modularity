# Standalone Movement API

## Summary
The standalone movement API is the built-in movement-controller helper exposed through `ScriptContext`. It is intended for scripts that want a reusable grounded movement solution without hand-writing the full controller logic from scratch.

It is one of the clearest examples of `ScriptContext` exposing a larger engine-side helper rather than just a single small utility function.

## Syntax
```cpp
ctx.BindStandaloneMovementSettings(settings);
ctx.DrawStandaloneMovementInspector(settings, &showDebug);
ctx.TickStandaloneMovement(state, settings, dt, &debug);
```

## Description
This API exists because grounded player movement is a large enough problem that it benefits from shared runtime support:

- movement tuning
- look tuning
- capsule and gravity tuning
- friction and slope handling
- debug output

The shipped `StandaloneMovementController.moducpp` script demonstrates the intended pattern:

- persisted movement settings
- runtime-only movement state
- optional debug readback

## Members

### Types
- `ScriptContext::StandaloneMovementSettings`
- `ScriptContext::StandaloneMovementState`
- `ScriptContext::StandaloneMovementDebug`

### Methods
- `BindStandaloneMovementSettings(settings)`
- `DrawStandaloneMovementInspector(settings, showDebug)`
- `TickStandaloneMovement(state, settings, deltaTime, debug)`

### `StandaloneMovementSettings` fields
- `moveTuning`
- `lookTuning`
- `capsuleTuning`
- `gravityTuning`
- `locomotionTuning`
- `surfaceTuning`
- `enableMouseLook`
- `requireMouseButton`
- `enforceCollider`
- `enforceRigidbody`

### `StandaloneMovementState` fields
- `pitch`
- `yaw`
- `verticalVelocity`
- `localVelocity`
- `slideVelocity`
- `lastGroundHitPos`
- `hasGroundSample`

### `StandaloneMovementDebug` fields
- `velocity`
- `localVelocity`
- `platformVelocity`
- `surfaceFriction`
- `slopeDegrees`
- `grounded`

## Behavior Explanation
The API is designed around a useful separation of responsibilities.

### Settings
Settings are the authored tuning values. They answer questions such as:

- how fast should the character move?
- how sensitive should look input feel?
- how strong should gravity or grounding behavior be?

### State
State is the runtime memory of the controller. It tracks what the controller is currently doing and what it needs to remember between frames.

### Debug
Debug data exposes the controller’s current interpretation of motion and ground state. This is valuable when tuning movement, especially in a larger project where “it feels wrong” needs more specific visibility.

### Tick
`TickStandaloneMovement(...)` is where the controller actually advances. The other methods exist to make the controller authorable and inspectable.

## Multiple Examples
### Draw the built-in inspector
```cpp
void Script_OnInspector()
{
    auto& config = Config<StandaloneMovementControllerConfig>();
    bindConfig(ctx, config);
    ctx.DrawStandaloneMovementInspector(config.settings, &config.showDebug);
}
```

This is the normal way to expose the controller tuning without manually rebuilding all of its UI.

### Advance runtime state
```cpp
void TickUpdate()
{
    auto& config = Config<StandaloneMovementControllerConfig>();
    auto& state = State<StandaloneMovementControllerState>();
    ctx.TickStandaloneMovement(state.movement, config.settings, dt, &state.debug);
}
```

This is the runtime heart of the controller pattern.

### One-time setup
```cpp
void Begin()
{
    if (config.settings.enforceCollider) {
        ctx.EnsureCapsuleCollider(config.settings.capsuleTuning.x, config.settings.capsuleTuning.y);
    }
    if (config.settings.enforceRigidbody) {
        ctx.EnsureRigidbody(true, false);
    }
}
```

This is where the controller script ensures the object setup matches the expected runtime model.

## Remarks
- The standalone movement API is larger than a typical helper function because it is solving a larger gameplay problem.
- The intended usage pattern is strongly aligned with `Config<T>()` for settings and `State<T>()` for runtime state.
- This API is especially useful when a project wants a reusable baseline controller rather than many one-off movement scripts.

## Related APIs
- [ScriptContext](type-scriptcontext.md)
- [Module: ModuEngine](module-moduengine.md)
- [Methods and Lifecycle](../manual/methods-and-lifecycle.md)
