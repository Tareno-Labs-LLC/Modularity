#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ModuScriptContext ModuScriptContext;

typedef struct ModuVec3 {
    float x;
    float y;
    float z;
} ModuVec3;

enum ModuConsoleMessageType {
    MODU_CONSOLE_INFO = 0,
    MODU_CONSOLE_WARNING = 1,
    MODU_CONSOLE_ERROR = 2,
    MODU_CONSOLE_SUCCESS = 3
};

/// @summary Read the current script object id.
/// @usage Use this to compare against raycast hits or stored ids in settings.
/// @returns Object id, or -1 when the context/object is invalid.
int Modu_GetObjectId(ModuScriptContext* ctx);
int Modu_IsObjectEnabled(ModuScriptContext* ctx);
void Modu_SetObjectEnabled(ModuScriptContext* ctx, int enabled);

ModuVec3 Modu_GetPosition(ModuScriptContext* ctx);
ModuVec3 Modu_GetRotation(ModuScriptContext* ctx);
ModuVec3 Modu_GetScale(ModuScriptContext* ctx);
void Modu_SetPosition(ModuScriptContext* ctx, ModuVec3 value);
void Modu_SetRotation(ModuScriptContext* ctx, ModuVec3 value);
void Modu_SetScale(ModuScriptContext* ctx, ModuVec3 value);

int Modu_SetRigidbodyVelocity(ModuScriptContext* ctx, ModuVec3 velocity);
int Modu_AddRigidbodyForce(ModuScriptContext* ctx, ModuVec3 force);
int Modu_GetRigidbodyVelocity(ModuScriptContext* ctx, ModuVec3* outVelocity);
int Modu_SetRigidbodyRotation(ModuScriptContext* ctx, ModuVec3 rotation);
float Modu_GetProjectGravityScale(ModuScriptContext* ctx);
void Modu_SetProjectGravityScale(ModuScriptContext* ctx, float scale);
int Modu_EnsureCapsuleCollider(ModuScriptContext* ctx, float height, float radius);
int Modu_EnsureRigidbody(ModuScriptContext* ctx, int useGravity, int kinematic);
int Modu_HasAnimation(ModuScriptContext* ctx);
int Modu_PlayAnimation(ModuScriptContext* ctx, int restart);
int Modu_StopAnimation(ModuScriptContext* ctx, int resetTime);
int Modu_PauseAnimation(ModuScriptContext* ctx, int pause);
int Modu_ReverseAnimation(ModuScriptContext* ctx, int restartIfStopped);
int Modu_SetAnimationTime(ModuScriptContext* ctx, float timeSeconds);
float Modu_GetAnimationTime(ModuScriptContext* ctx);
int Modu_IsAnimationPlaying(ModuScriptContext* ctx);
int Modu_SetAnimationLoop(ModuScriptContext* ctx, int loop);
int Modu_SetAnimationPlaySpeed(ModuScriptContext* ctx, float speed);
int Modu_SetAnimationPlayOnAwake(ModuScriptContext* ctx, int playOnAwake);

int Modu_IsSprintDown(ModuScriptContext* ctx);
int Modu_IsJumpDown(ModuScriptContext* ctx);
ModuVec3 Modu_GetMoveInputWASD(ModuScriptContext* ctx, float pitchDeg, float yawDeg);
int Modu_ApplyMouseLook(ModuScriptContext* ctx, float* pitchDeg, float* yawDeg,
                        float sensitivity, float maxDelta, float deltaTime, int requireMouseButton);
int Modu_RaycastClosestDetailed(ModuScriptContext* ctx, ModuVec3 origin, ModuVec3 dir, float distance,
                                ModuVec3* hitPos, ModuVec3* hitNormal, float* hitDistance,
                                int* hitObjectId, ModuVec3* hitObjectVelocity,
                                float* hitStaticFriction, float* hitDynamicFriction);

void Modu_AddConsoleMessage(ModuScriptContext* ctx, const char* message, int type);

float Modu_GetSettingFloat(ModuScriptContext* ctx, const char* key, float fallback);
void Modu_SetSettingFloat(ModuScriptContext* ctx, const char* key, float value);
int Modu_GetSettingBool(ModuScriptContext* ctx, const char* key, int fallback);
void Modu_SetSettingBool(ModuScriptContext* ctx, const char* key, int value);
void Modu_SetSettingString(ModuScriptContext* ctx, const char* key, const char* value);
int Modu_GetSettingString(ModuScriptContext* ctx, const char* key, const char* fallback,
                          char* outBuffer, int outBufferSize);
int Modu_GetSpriteClipCount(ModuScriptContext* ctx);
int Modu_GetSpriteClipIndex(ModuScriptContext* ctx);
int Modu_SetSpriteClipIndex(ModuScriptContext* ctx, int index);
int Modu_SetSpriteClipName(ModuScriptContext* ctx, const char* name);
int Modu_GetSpriteClipName(ModuScriptContext* ctx, char* outBuffer, int outBufferSize);
int Modu_GetSpriteClipNameAt(ModuScriptContext* ctx, int index, char* outBuffer, int outBufferSize);

void Modu_InspectorText(ModuScriptContext* ctx, const char* text);
void Modu_InspectorSeparator(ModuScriptContext* ctx);
int Modu_InspectorDragFloat(ModuScriptContext* ctx, const char* label, float* value,
                            float speed, float minValue, float maxValue, const char* format);
int Modu_InspectorDragFloat2(ModuScriptContext* ctx, const char* label, float* value,
                             float speed, float minValue, float maxValue, const char* format);
int Modu_InspectorDragFloat3(ModuScriptContext* ctx, const char* label, float* value,
                             float speed, float minValue, float maxValue, const char* format);
int Modu_InspectorCheckbox(ModuScriptContext* ctx, const char* label, int* value);
int Modu_InspectorObject(ModuScriptContext* ctx, const char* label, int* objectId);

#ifdef __cplusplus
}
#endif
