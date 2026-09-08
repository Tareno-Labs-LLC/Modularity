#pragma once

// support header for ModuCPP scripts that the transpiler lowered to C instead
// of C++. this is the ENTIRE include surface of a generated .moducpp.gen.c
// file, and that is the whole point: the C++ script api drags in ~95k
// preprocessed lines (ImGui and friends), this one stays around a hundred, so
// scripts compile in milliseconds instead of seconds.
//
// future me: do NOT include engine headers from here. no ScriptRuntime.h, no
// imgui.h, nothing. the moment you do, the fast path stops being fast and the
// C backend loses its reason to exist. everything a lowered script needs goes
// through the flat Modu_* calls in ScriptRuntimeCAPI.h or the little static
// helpers below.
//
// also note: script TUs may be compiled by g++/clang++ (the script driver is
// shared with the C++ path and NDK cross builds), so everything in here has to
// be valid C AND valid C++. plain functions, no designated initializers, no
// compound literals.

#include "ScriptRuntimeCAPI.h"

#include <math.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

// how many objects can hold per-object state for one script before slots run
// out and the extras start sharing the last slot. bump it if you somehow put
// the same C-backed script on more objects than this.
#ifndef MODUC_MAX_STATE_SLOTS
#define MODUC_MAX_STATE_SLOTS 512
#endif

static inline ModuVec3 ModuC_Vec3(float x, float y, float z) {
    ModuVec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static inline ModuVec3 ModuC_Vec3Splat(float value) {
    return ModuC_Vec3(value, value, value);
}

static inline float ModuC_Vec3Length(ModuVec3 v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

// Math.* lowers to these. names line up with the ModuCPP surface so the
// transpiler mapping stays a dumb one-liner per entry.
static inline float ModuC_Math_Sin(float v)   { return sinf(v); }
static inline float ModuC_Math_Cos(float v)   { return cosf(v); }
static inline float ModuC_Math_Tan(float v)   { return tanf(v); }
static inline float ModuC_Math_Sqrt(float v)  { return sqrtf(v); }
static inline float ModuC_Math_Abs(float v)   { return fabsf(v); }
static inline float ModuC_Math_Floor(float v) { return floorf(v); }
static inline float ModuC_Math_Ceil(float v)  { return ceilf(v); }
static inline float ModuC_Math_Exp(float v)   { return expf(v); }
static inline float ModuC_Math_Pow(float base, float exponent) { return powf(base, exponent); }
static inline float ModuC_Math_Min(float a, float b) { return a < b ? a : b; }
static inline float ModuC_Math_Max(float a, float b) { return a > b ? a : b; }
static inline float ModuC_Math_Clamp(float v, float minValue, float maxValue) {
    return v < minValue ? minValue : (v > maxValue ? maxValue : v);
}
static inline float ModuC_Math_Clamp01(float v) { return ModuC_Math_Clamp(v, 0.0f, 1.0f); }
static inline float ModuC_Math_Lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline float ModuC_Math_Length(ModuVec3 v) { return ModuC_Vec3Length(v); }
