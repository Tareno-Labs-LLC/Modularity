#version 330 core
out vec4 FragColor;

// Shared by the rain streaks (rain_vert.glsl) and splashes (rain_splash_vert.glsl).
// Water is lit, not painted: a drop is close to invisible against a dark wall until a
// light sits behind it, where forward scattering through the drop makes it flare. That
// back-lighting is what makes night rain read in front of street lights and headlights.

in vec2 vUv;
in float vAlpha;
in vec3 vWorld;
flat in int vKind;   // 0 streak, 1 splash ring, 2 splash crown
in float vPhase;

uniform vec3 cameraPos;
uniform vec3 ambientColor;
uniform vec3 skyTint;          // fog / sky colour, the "refraction" of the surroundings
uniform vec3 sunDirection;     // direction the sun's light travels
uniform vec3 sunColor;         // colour * intensity, 0 when there is no sun
uniform float lightningFlash;

#define RAIN_MAX_LIGHTS 8
uniform int rainLightCount;
uniform vec4 rainLightPosRange[RAIN_MAX_LIGHTS];
uniform vec4 rainLightColorType[RAIN_MAX_LIGHTS];
uniform vec4 rainLightDirCone[RAIN_MAX_LIGHTS];
uniform float rainLightOuterCos[RAIN_MAX_LIGHTS];

uniform bool fogEnabled = false;
uniform int fogMode = 0;
uniform vec3 fogColor = vec3(0.65, 0.72, 0.78);
uniform float fogStart = 20.0;
uniform float fogEnd = 120.0;
uniform float fogDensity = 0.015;
uniform float fogHeight = 0.0;
uniform float fogHeightFalloff = 0.0;

// Same curve as frag.glsl's computeFogFactor.
float computeFogFactor(float dist)
{
    if (!fogEnabled) return 0.0;
    float factor = 0.0;
    if (fogMode == 1) {
        factor = 1.0 - exp(-max(fogDensity, 0.0) * dist);
    } else if (fogMode == 2) {
        float dd = max(fogDensity, 0.0) * dist;
        factor = 1.0 - exp(-(dd * dd));
    } else {
        factor = smoothstep(fogStart, max(fogStart + 0.01, fogEnd), dist);
    }
    if (fogHeightFalloff > 0.0001) {
        float heightWeight = 1.0 - exp(-abs(vWorld.y - fogHeight) * fogHeightFalloff);
        factor *= clamp(heightWeight, 0.0, 1.0);
    }
    return clamp(factor, 0.0, 1.0);
}

// Light scattered toward the eye by a drop: a soft omnidirectional part (reflection off
// the surface) plus a sharp forward lobe when the light is behind the drop.
float dropPhase(vec3 toLight, vec3 toEye)
{
    float back = max(dot(toLight, -toEye), 0.0);
    return 0.12 + 1.6 * pow(back, 6.0) + 5.0 * pow(back, 48.0);
}

void main()
{
    float shape;
    if (vKind == 0) {
        float across = 1.0 - vUv.x * vUv.x;
        // Tapered at both ends, brightest just behind the head like a motion-blurred drop.
        float along = smoothstep(0.0, 0.45, vUv.y) * (1.0 - smoothstep(0.8, 1.0, vUv.y));
        shape = across * across * along;
    } else if (vKind == 1) {
        float r = length(vUv);
        float ringRadius = 0.55 + 0.4 * vPhase;
        float ring = 1.0 - smoothstep(0.0, 0.18, abs(r - ringRadius));
        shape = ring * 0.4 * (1.0 - smoothstep(0.9, 1.0, r));
    } else {
        // A few droplets thrown up and outward on ballistic arcs over the crown's life.
        float t = clamp(vPhase / 0.4, 0.0, 1.0);
        vec2 p = vec2(vUv.x, vUv.y * 0.5 + 0.5);
        shape = 0.0;
        for (int k = 0; k < 5; ++k) {
            float fk = float(k);
            float dirX = (fk - 2.0) * 0.38 + 0.05 * sin(fk * 12.9);
            float up = 0.55 + 0.35 * fract(sin(fk * 78.2) * 43758.5);
            vec2 drop = vec2(dirX * (0.25 + 0.75 * t), up * 4.0 * t * (1.0 - t) + 0.05);
            float d = length((p - drop) * vec2(1.0, 0.8));
            shape += 1.0 - smoothstep(0.03, 0.09, d);
        }
        shape = min(shape, 1.0) * 0.9;
    }
    float alpha = vAlpha * shape;
    if (alpha <= 0.002) discard;

    vec3 toEyeVec = cameraPos - vWorld;
    float dist = length(toEyeVec);
    vec3 toEye = toEyeVec / max(dist, 1e-4);

    // Surroundings seen through the drop, and the sky above it.
    vec3 light = ambientColor * 1.4 + skyTint * 0.35;
    if (dot(sunColor, sunColor) > 0.0) {
        light += sunColor * dropPhase(-sunDirection, toEye) * 0.35;
    }
    for (int i = 0; i < RAIN_MAX_LIGHTS; ++i) {
        if (i >= rainLightCount) break;
        vec3 lvec = rainLightPosRange[i].xyz - vWorld;
        float d = length(lvec);
        vec3 l = lvec / max(d, 1e-4);
        float range = rainLightPosRange[i].w;
        float attenuation = 1.0 / (1.0 + d * d * 0.08);
        if (range > 0.0) {
            if (d > range) continue;
            float falloff = clamp(1.0 - d / range, 0.0, 1.0);
            attenuation = falloff * falloff;
        }
        if (rainLightColorType[i].w > 1.5) {
            float cosTheta = dot(-l, rainLightDirCone[i].xyz);
            attenuation *= smoothstep(rainLightOuterCos[i], rainLightDirCone[i].w, cosTheta);
        }
        if (attenuation <= 0.0) continue;
        light += rainLightColorType[i].rgb * attenuation * dropPhase(l, toEye);
    }
    light += vec3(0.75, 0.8, 1.0) * lightningFlash * 0.8;

    float fog = computeFogFactor(dist);
    // Premultiplied: the colour is the light the water adds; the small alpha is the
    // background it scatters away, which is what darkens rain against a bright sky.
    vec3 rgb = light * alpha * (1.0 - fog);
    FragColor = vec4(rgb, alpha * 0.35 * (1.0 - fog));
}
