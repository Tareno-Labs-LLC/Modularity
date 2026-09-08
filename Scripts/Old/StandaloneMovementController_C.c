#include "ScriptRuntimeCAPI.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ControllerSettings {
    ModuVec3 moveTuning;
    ModuVec3 lookTuning;
    ModuVec3 capsuleTuning;
    ModuVec3 gravityTuning;
    ModuVec3 locomotionTuning;
    ModuVec3 surfaceTuning;
    int enableMouseLook;
    int requireMouseButton;
    int enforceCollider;
    int enforceRigidbody;
} ControllerSettings;

typedef struct ControllerState {
    int objectId;
    int initialized;
    int warnedMissingRigidbody;
    int hasGroundSample;
    float pitch;
    float yaw;
    float verticalVelocity;
    float localVelX;
    float localVelZ;
    float slideVelX;
    float slideVelZ;
    ModuVec3 lastGroundHitPos;
} ControllerState;

static ControllerSettings g_settings = {
    {4.5f, 7.5f, 6.5f},
    {0.12f, 200.0f, 0.0f},
    {1.8f, 0.4f, 0.2f},
    {-9.81f, 0.4f, 30.0f},
    {24.0f, 8.0f, 16.0f},
    {0.2f, 40.0f, 1.0f},
    1,
    0,
    1,
    1
};

static int g_settingsLoaded = 0;

static ControllerState* g_states = NULL;
static int g_stateCount = 0;
static int g_stateCapacity = 0;

static float clampf32(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

static float length2f(float x, float y) {
    return sqrtf(x * x + y * y);
}

static void moveTowards2(float* x, float* y, float targetX, float targetY, float maxDelta) {
    if (!x || !y) return;
    if (maxDelta <= 0.0f) return;
    float dx = targetX - *x;
    float dy = targetY - *y;
    float len = length2f(dx, dy);
    if (len <= maxDelta || len <= 1e-5f) {
        *x = targetX;
        *y = targetY;
        return;
    }
    float scale = maxDelta / len;
    *x += dx * scale;
    *y += dy * scale;
}

static ModuVec3 sanitizePlanar(ModuVec3 value) {
    ModuVec3 out = {value.x, 0.0f, value.z};
    if (!isfinite(out.x) || !isfinite(out.z)) {
        out.x = 0.0f;
        out.z = 0.0f;
    }
    return out;
}

static int ensureStateCapacity(int minCapacity) {
    if (g_stateCapacity >= minCapacity) return 1;

    int nextCapacity = (g_stateCapacity > 0) ? g_stateCapacity * 2 : 16;
    while (nextCapacity < minCapacity) {
        nextCapacity *= 2;
    }

    ControllerState* nextStates =
        (ControllerState*)realloc(g_states, (size_t)nextCapacity * sizeof(ControllerState));
    if (!nextStates) return 0;

    g_states = nextStates;
    g_stateCapacity = nextCapacity;
    return 1;
}

static ControllerState* getState(int objectId) {
    for (int i = 0; i < g_stateCount; ++i) {
        if (g_states[i].objectId == objectId) {
            return &g_states[i];
        }
    }

    if (!ensureStateCapacity(g_stateCount + 1)) return NULL;
    ControllerState* state = &g_states[g_stateCount++];
    memset(state, 0, sizeof(*state));
    state->objectId = objectId;
    return state;
}

static void skipSeparators(const char** cursor) {
    while (**cursor == ' ' || **cursor == '\t' || **cursor == ',') {
        ++(*cursor);
    }
}

static int parseVec3(const char* text, ModuVec3* out) {
    if (!text || !out) return 0;

    const char* cursor = text;
    char* end = NULL;
    float values[3] = {0.0f, 0.0f, 0.0f};

    for (int i = 0; i < 3; ++i) {
        skipSeparators(&cursor);
        values[i] = strtof(cursor, &end);
        if (end == cursor) return 0;
        cursor = end;
    }

    out->x = values[0];
    out->y = values[1];
    out->z = values[2];
    return 1;
}

static void formatVec3(ModuVec3 value, char* outBuffer, int outBufferSize) {
    if (!outBuffer || outBufferSize <= 0) return;
    snprintf(outBuffer, (size_t)outBufferSize, "%.6g,%.6g,%.6g", value.x, value.y, value.z);
}

static ModuVec3 readVec3Setting(ModuScriptContext* ctx, const char* key, ModuVec3 fallback) {
    char fallbackBuffer[96];
    char valueBuffer[96];
    ModuVec3 out = fallback;

    formatVec3(fallback, fallbackBuffer, (int)sizeof(fallbackBuffer));
    if (!Modu_GetSettingString(ctx, key, fallbackBuffer, valueBuffer, (int)sizeof(valueBuffer))) {
        return out;
    }

    if (!parseVec3(valueBuffer, &out)) {
        return fallback;
    }
    return out;
}

static void writeVec3Setting(ModuScriptContext* ctx, const char* key, ModuVec3 value) {
    char buffer[96];
    formatVec3(value, buffer, (int)sizeof(buffer));
    Modu_SetSettingString(ctx, key, buffer);
}

static void bindSettings(ModuScriptContext* ctx, ControllerSettings* settings) {
    settings->moveTuning = readVec3Setting(ctx, "moveTuning", settings->moveTuning);
    settings->lookTuning = readVec3Setting(ctx, "lookTuning", settings->lookTuning);
    settings->capsuleTuning = readVec3Setting(ctx, "capsuleTuning", settings->capsuleTuning);
    settings->gravityTuning = readVec3Setting(ctx, "gravityTuning", settings->gravityTuning);
    settings->locomotionTuning = readVec3Setting(ctx, "locomotionTuning", settings->locomotionTuning);
    settings->surfaceTuning = readVec3Setting(ctx, "surfaceTuning", settings->surfaceTuning);

    settings->enableMouseLook =
        Modu_GetSettingBool(ctx, "enableMouseLook", settings->enableMouseLook ? 1 : 0);
    settings->requireMouseButton =
        Modu_GetSettingBool(ctx, "requireMouseButton", settings->requireMouseButton ? 1 : 0);
    settings->enforceCollider =
        Modu_GetSettingBool(ctx, "enforceCollider", settings->enforceCollider ? 1 : 0);
    settings->enforceRigidbody =
        Modu_GetSettingBool(ctx, "enforceRigidbody", settings->enforceRigidbody ? 1 : 0);
}

static void saveSettings(ModuScriptContext* ctx, const ControllerSettings* settings) {
    writeVec3Setting(ctx, "moveTuning", settings->moveTuning);
    writeVec3Setting(ctx, "lookTuning", settings->lookTuning);
    writeVec3Setting(ctx, "capsuleTuning", settings->capsuleTuning);
    writeVec3Setting(ctx, "gravityTuning", settings->gravityTuning);
    writeVec3Setting(ctx, "locomotionTuning", settings->locomotionTuning);
    writeVec3Setting(ctx, "surfaceTuning", settings->surfaceTuning);
    Modu_SetSettingBool(ctx, "enableMouseLook", settings->enableMouseLook ? 1 : 0);
    Modu_SetSettingBool(ctx, "requireMouseButton", settings->requireMouseButton ? 1 : 0);
    Modu_SetSettingBool(ctx, "enforceCollider", settings->enforceCollider ? 1 : 0);
    Modu_SetSettingBool(ctx, "enforceRigidbody", settings->enforceRigidbody ? 1 : 0);
}

void Modu_OnInspector(ModuScriptContext* ctx) {
    if (!ctx) return;

    bindSettings(ctx, &g_settings);
    g_settingsLoaded = 1;

    int changed = 0;
    Modu_InspectorText(ctx, "Standalone Movement Controller");
    Modu_InspectorSeparator(ctx);
    changed |= Modu_InspectorDragFloat3(ctx, "Walk/Run/Jump", &g_settings.moveTuning.x, 0.05f,
                                        0.0f, 25.0f, "%.2f");
    changed |= Modu_InspectorDragFloat2(ctx, "Look Sens/Clamp", &g_settings.lookTuning.x, 0.01f,
                                        0.0f, 500.0f, "%.2f");
    changed |= Modu_InspectorDragFloat3(ctx, "Height/Radius/Snap", &g_settings.capsuleTuning.x, 0.02f,
                                        0.0f, 5.0f, "%.2f");
    changed |= Modu_InspectorDragFloat3(ctx, "Gravity/Probe/MaxFall", &g_settings.gravityTuning.x, 0.05f,
                                        -50.0f, 50.0f, "%.2f");
    changed |= Modu_InspectorDragFloat3(ctx, "Ground/Air/Brake", &g_settings.locomotionTuning.x, 0.05f,
                                        0.0f, 200.0f, "%.2f");
    changed |= Modu_InspectorDragFloat3(ctx, "Grip/Slide/Carry", &g_settings.surfaceTuning.x, 0.02f,
                                        0.0f, 200.0f, "%.2f");
    changed |= Modu_InspectorCheckbox(ctx, "Enable Mouse Look", &g_settings.enableMouseLook);
    changed |= Modu_InspectorCheckbox(ctx, "Hold RMB to Look", &g_settings.requireMouseButton);
    changed |= Modu_InspectorCheckbox(ctx, "Force Collider", &g_settings.enforceCollider);
    changed |= Modu_InspectorCheckbox(ctx, "Force Rigidbody", &g_settings.enforceRigidbody);

    if (changed) {
        saveSettings(ctx, &g_settings);
    }
}

void Modu_Begin(ModuScriptContext* ctx) {
    if (!ctx) return;

    int objectId = Modu_GetObjectId(ctx);
    if (objectId < 0) return;

    if (!g_settingsLoaded) {
        bindSettings(ctx, &g_settings);
        g_settingsLoaded = 1;
    }
    ControllerState* state = getState(objectId);
    if (!state) return;

    ModuVec3 rot = Modu_GetRotation(ctx);
    state->pitch = rot.x;
    state->yaw = rot.y;
    state->verticalVelocity = 0.0f;
    state->localVelX = 0.0f;
    state->localVelZ = 0.0f;
    state->slideVelX = 0.0f;
    state->slideVelZ = 0.0f;
    state->hasGroundSample = 0;
    state->initialized = 1;

    if (g_settings.enforceCollider) {
        Modu_EnsureCapsuleCollider(ctx, g_settings.capsuleTuning.x, g_settings.capsuleTuning.y);
    }
    if (g_settings.enforceRigidbody) {
        Modu_EnsureRigidbody(ctx, 1, 0);
    }
}

void Modu_TickUpdate(ModuScriptContext* ctx, float dt) {
    if (!ctx || dt <= 0.0f) return;

    int objectId = Modu_GetObjectId(ctx);
    if (objectId < 0) return;

    if (!g_settingsLoaded) {
        bindSettings(ctx, &g_settings);
        g_settingsLoaded = 1;
    }
    ControllerState* state = getState(objectId);
    if (!state) return;

    if (!state->initialized) {
        ModuVec3 rot = Modu_GetRotation(ctx);
        state->pitch = rot.x;
        state->yaw = rot.y;
        state->verticalVelocity = 0.0f;
        state->localVelX = 0.0f;
        state->localVelZ = 0.0f;
        state->slideVelX = 0.0f;
        state->slideVelZ = 0.0f;
        state->hasGroundSample = 0;
        state->initialized = 1;
    }

    if (g_settings.enableMouseLook) {
        float maxMouseDelta = fmaxf(5.0f, g_settings.lookTuning.y);
        Modu_ApplyMouseLook(ctx, &state->pitch, &state->yaw, g_settings.lookTuning.x,
                            maxMouseDelta, dt, g_settings.requireMouseButton);
    }

    ModuVec3 move = Modu_GetMoveInputWASD(ctx, state->pitch, state->yaw);
    float targetSpeed = Modu_IsSprintDown(ctx) ? g_settings.moveTuning.y : g_settings.moveTuning.x;
    float targetLocalX = 0.0f;
    float targetLocalZ = 0.0f;

    float yawRad = state->yaw * (3.14159265358979323846f / 180.0f);
    ModuVec3 planarForward = {-sinf(yawRad), 0.0f, -cosf(yawRad)};
    ModuVec3 planarRight = {cosf(yawRad), 0.0f, -sinf(yawRad)};

    targetLocalX = move.x * planarRight.x + move.z * planarRight.z;
    targetLocalZ = move.x * planarForward.x + move.z * planarForward.z;
    {
        float inLen = length2f(targetLocalX, targetLocalZ);
        if (inLen > 1.0f && inLen > 1e-5f) {
            targetLocalX /= inLen;
            targetLocalZ /= inLen;
        }
    }
    targetLocalX *= targetSpeed;
    targetLocalZ *= targetSpeed;

    float capsuleHalf = fmaxf(0.1f, g_settings.capsuleTuning.x * 0.5f);
    float groundSnap = g_settings.capsuleTuning.z;
    float gravity = g_settings.gravityTuning.x;
    float probeExtra = g_settings.gravityTuning.y;
    float maxFall = fmaxf(1.0f, g_settings.gravityTuning.z);

    ModuVec3 physVel = {0.0f, 0.0f, 0.0f};
    int havePhysVel = Modu_GetRigidbodyVelocity(ctx, &physVel);
    if (havePhysVel) {
        state->verticalVelocity = physVel.y;
    }

    ModuVec3 pos = Modu_GetPosition(ctx);
    ModuVec3 hitPos = {0.0f, 0.0f, 0.0f};
    ModuVec3 hitNormal = {0.0f, 1.0f, 0.0f};
    float hitDist = 0.0f;
    int hitActorId = -1;
    ModuVec3 hitActorVelocity = {0.0f, 0.0f, 0.0f};
    float hitStaticFriction = 0.9f;
    float hitDynamicFriction = 0.9f;

    ModuVec3 rayStart = {pos.x, pos.y + 0.1f, pos.z};
    ModuVec3 rayDir = {0.0f, -1.0f, 0.0f};
    float probeDist = capsuleHalf + probeExtra;
    int hitGround = Modu_RaycastClosestDetailed(ctx, rayStart, rayDir, probeDist,
                                                &hitPos, &hitNormal, &hitDist,
                                                &hitActorId, &hitActorVelocity,
                                                &hitStaticFriction, &hitDynamicFriction);

    int grounded = hitGround && (hitNormal.y > 0.25f) &&
                   (hitDist <= capsuleHalf + groundSnap) &&
                   (state->verticalVelocity <= 0.35f);
    if (!hitGround) {
        grounded = (pos.y <= capsuleHalf + 0.12f) && (state->verticalVelocity <= 0.35f);
    }

    (void)hitActorId;
    (void)hitStaticFriction;

    float dynamicFriction = clampf32(hitDynamicFriction, 0.0f, 2.0f);
    float minSurfaceControl = clampf32(g_settings.surfaceTuning.x, 0.0f, 1.0f);
    float grip = grounded ? clampf32(dynamicFriction, minSurfaceControl, 1.0f) : 1.0f;
    float groundAccel = fmaxf(0.0f, g_settings.locomotionTuning.x);
    float airAccel = fmaxf(0.0f, g_settings.locomotionTuning.y);
    float braking = fmaxf(0.0f, g_settings.locomotionTuning.z);
    float slideGravity = fmaxf(0.0f, g_settings.surfaceTuning.y);
    float platformCarry = clampf32(g_settings.surfaceTuning.z, 0.0f, 3.0f);

    {
        float accelRate = grounded ? (groundAccel * grip) : airAccel;
        if (accelRate > 0.0f) {
            moveTowards2(&state->localVelX, &state->localVelZ, targetLocalX, targetLocalZ, accelRate * dt);
        } else {
            state->localVelX = targetLocalX;
            state->localVelZ = targetLocalZ;
        }
    }

    if ((targetLocalX * targetLocalX + targetLocalZ * targetLocalZ) < 1e-4f && braking > 0.0f) {
        float brakeScale = grounded ? (0.5f + 0.5f * grip) : 0.15f;
        float damp = fmaxf(0.0f, 1.0f - braking * brakeScale * dt);
        state->localVelX *= damp;
        state->localVelZ *= damp;
    }

    {
        float localSpeed = length2f(state->localVelX, state->localVelZ);
        if (localSpeed > targetSpeed && targetSpeed > 0.0f) {
            float inv = targetSpeed / localSpeed;
            state->localVelX *= inv;
            state->localVelZ *= inv;
        }
    }

    ModuVec3 platformVelocity = sanitizePlanar(hitActorVelocity);
    if (grounded && hitGround) {
        if (state->hasGroundSample && dt > 1e-5f) {
            ModuVec3 pointVelocity = {
                (hitPos.x - state->lastGroundHitPos.x) / dt,
                0.0f,
                (hitPos.z - state->lastGroundHitPos.z) / dt
            };
            pointVelocity = sanitizePlanar(pointVelocity);
            if ((pointVelocity.x * pointVelocity.x + pointVelocity.z * pointVelocity.z) < (120.0f * 120.0f)) {
                float pvLen2 = platformVelocity.x * platformVelocity.x + platformVelocity.z * platformVelocity.z;
                if (pvLen2 < 1e-4f) {
                    platformVelocity = pointVelocity;
                } else {
                    platformVelocity.x = platformVelocity.x + (pointVelocity.x - platformVelocity.x) * 0.35f;
                    platformVelocity.z = platformVelocity.z + (pointVelocity.z - platformVelocity.z) * 0.35f;
                }
            }
        }
        state->lastGroundHitPos = hitPos;
        state->hasGroundSample = 1;
    } else {
        state->hasGroundSample = 0;
    }

    if (grounded && hitGround) {
        ModuVec3 n = hitNormal;
        float nLen = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
        if (nLen > 1e-5f) {
            n.x /= nLen;
            n.y /= nLen;
            n.z /= nLen;
        } else {
            n.x = 0.0f;
            n.y = 1.0f;
            n.z = 0.0f;
        }

        if (isfinite(n.x) && isfinite(n.y) && isfinite(n.z)) {
            ModuVec3 gravityDir = {0.0f, -1.0f, 0.0f};
            float dotGN = gravityDir.x * n.x + gravityDir.y * n.y + gravityDir.z * n.z;
            ModuVec3 downSlope = {
                gravityDir.x - n.x * dotGN,
                gravityDir.y - n.y * dotGN,
                gravityDir.z - n.z * dotGN
            };
            float downLen = sqrtf(downSlope.x * downSlope.x + downSlope.y * downSlope.y + downSlope.z * downSlope.z);
            if (downLen > 1e-4f) {
                downSlope.x /= downLen;
                downSlope.y /= downLen;
                downSlope.z /= downLen;
                float slopeFactor = clampf32((1.0f - n.y) * 3.0f, 0.0f, 1.5f);
                float slip = clampf32(1.0f - dynamicFriction * 0.85f, 0.0f, 1.0f);
                float slideAccel = slideGravity * slopeFactor * (0.35f + slip);
                state->slideVelX += downSlope.x * slideAccel * dt;
                state->slideVelZ += downSlope.z * slideAccel * dt;
            }
        }
        {
            float slideDamp = clampf32((dynamicFriction + 0.15f) * 6.0f, 0.5f, 12.0f);
            float damp = fmaxf(0.0f, 1.0f - slideDamp * dt);
            state->slideVelX *= damp;
            state->slideVelZ *= damp;
        }
    } else {
        float damp = fmaxf(0.0f, 1.0f - 4.0f * dt);
        state->slideVelX *= damp;
        state->slideVelZ *= damp;
    }

    if (!isfinite(state->slideVelX)) state->slideVelX = 0.0f;
    if (!isfinite(state->slideVelZ)) state->slideVelZ = 0.0f;

    if (grounded) {
        state->verticalVelocity = 0.0f;
        if (!havePhysVel) {
            if (hitGround) {
                float minY = hitPos.y + capsuleHalf;
                if (pos.y < minY) {
                    pos.y = minY;
                    Modu_SetPosition(ctx, pos);
                }
            } else if (pos.y < capsuleHalf) {
                pos.y = capsuleHalf;
                Modu_SetPosition(ctx, pos);
            }
        }
        if (Modu_IsJumpDown(ctx)) {
            state->verticalVelocity = g_settings.moveTuning.z;
            state->hasGroundSample = 0;
        }
    } else {
        state->verticalVelocity += gravity * dt;
    }

    state->verticalVelocity = clampf32(state->verticalVelocity, -maxFall, maxFall);

    if (grounded) {
        platformVelocity.x *= platformCarry;
        platformVelocity.z *= platformCarry;
    } else {
        platformVelocity.x = 0.0f;
        platformVelocity.z = 0.0f;
    }

    ModuVec3 velocity = {
        planarRight.x * state->localVelX + planarForward.x * state->localVelZ +
            platformVelocity.x + state->slideVelX,
        state->verticalVelocity,
        planarRight.z * state->localVelX + planarForward.z * state->localVelZ +
            platformVelocity.z + state->slideVelZ
    };

    ModuVec3 rotation = {state->pitch, state->yaw, 0.0f};
    if (!Modu_SetRigidbodyRotation(ctx, rotation)) {
        Modu_SetRotation(ctx, rotation);
    }

    if (!Modu_SetRigidbodyVelocity(ctx, velocity)) {
        if (g_settings.enforceRigidbody && !state->warnedMissingRigidbody) {
            Modu_AddConsoleMessage(ctx,
                                   "StandaloneMovementController_C: add Rigidbody for physics movement.",
                                   MODU_CONSOLE_WARNING);
            state->warnedMissingRigidbody = 1;
        }
        pos = Modu_GetPosition(ctx);
        pos.x += velocity.x * dt;
        pos.y += velocity.y * dt;
        pos.z += velocity.z * dt;
        if (pos.y < capsuleHalf) {
            pos.y = capsuleHalf;
            state->verticalVelocity = 0.0f;
        }
        Modu_SetPosition(ctx, pos);
    }
}
