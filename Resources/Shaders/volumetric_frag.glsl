#version 330 core
// Volumetric lighting: in-scattering accumulated along the view ray.
//
// Runs after the scene geometry, reading the scene depth to know where each ray
// stops, and marching back toward the camera sampling the same shadow maps the
// surface shading used. Output is additive light only - it never darkens, so it
// composites with GL_ONE/GL_ONE over the scene colour, and lands before the
// bloom extract so bright shafts bloom on their own.
//
// The light uniforms deliberately mirror frag.glsl name for name, so the
// renderer can push both from one set of cached uniform names.
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D sceneDepth;

uniform mat4 invViewProj;
uniform vec3 viewPos;
uniform float nearPlane;
uniform float farPlane;

uniform float intensity = 1.0;
uniform int steps = 32;
uniform float scattering = 0.6;
uniform float density = 0.05;
uniform float maxDistance = 60.0;
// Soft ceiling on the result. 0 means unlimited.
uniform float maxIntensity = 0.0;
// Breaks the march's banding into noise the eye reads as grain instead of
// rings. Advanced per frame so the noise is not a fixed screen pattern.
uniform float frameJitter = 0.0;

const int MAX_LIGHTS = 32;
const int MAX_SHADOW_MAPS = 4;
uniform int lightCount = 0;

uniform int lightTypeArr[MAX_LIGHTS];
uniform vec3 lightDirArr[MAX_LIGHTS];
uniform vec3 lightPosArr[MAX_LIGHTS];
uniform vec3 lightColorArr[MAX_LIGHTS];
uniform float lightIntensityArr[MAX_LIGHTS];
uniform float lightRangeArr[MAX_LIGHTS];
uniform float lightInnerCosArr[MAX_LIGHTS];
uniform float lightOuterCosArr[MAX_LIGHTS];
uniform int lightShadowMapArr[MAX_LIGHTS];
uniform int lightShadowKindArr[MAX_LIGHTS]; // 0 off, 1 cube, 2 directional
uniform float lightShadowBiasArr[MAX_LIGHTS];
uniform float lightShadowFarArr[MAX_LIGHTS];
uniform mat4 lightShadowMatrixArr[MAX_LIGHTS];

uniform samplerCube shadowCube0;
uniform samplerCube shadowCube1;
uniform samplerCube shadowCube2;
uniform samplerCube shadowCube3;
uniform sampler2D dirShadow0;
uniform sampler2D dirShadow1;
uniform sampler2D dirShadow2;
uniform sampler2D dirShadow3;

float sampleShadowCube(int mapIndex, vec3 sampleDir)
{
    if (mapIndex == 0) return texture(shadowCube0, sampleDir).r;
    if (mapIndex == 1) return texture(shadowCube1, sampleDir).r;
    if (mapIndex == 2) return texture(shadowCube2, sampleDir).r;
    if (mapIndex == 3) return texture(shadowCube3, sampleDir).r;
    return 1.0;
}

float sampleDirectionalShadow(int mapIndex, vec2 uv)
{
    if (mapIndex == 0) return texture(dirShadow0, uv).r;
    if (mapIndex == 1) return texture(dirShadow1, uv).r;
    if (mapIndex == 2) return texture(dirShadow2, uv).r;
    if (mapIndex == 3) return texture(dirShadow3, uv).r;
    return 1.0;
}

// 1.0 where the sample point is lit, 0.0 where it is in shadow. Always the hard
// test: a soft PCF tap costs 9-20 lookups, and this runs once per light per
// step. The march itself averages neighbouring steps, which softens the result
// anyway.
float visibility(int i, vec3 worldPos)
{
    int kind = lightShadowKindArr[i];
    int mapIndex = lightShadowMapArr[i];
    if (kind <= 0 || mapIndex < 0 || mapIndex >= MAX_SHADOW_MAPS) return 0.0;

    float bias = max(lightShadowBiasArr[i], 0.0001);

    if (kind == 2) {
        vec4 lightSpace = lightShadowMatrixArr[i] * vec4(worldPos, 1.0);
        vec3 projCoords = lightSpace.xyz / max(lightSpace.w, 0.0001);
        projCoords = projCoords * 0.5 + 0.5;
        // Outside the cascade there is no information. Treated as lit, which
        // keeps distant haze uniform instead of stamping the shadow map's
        // footprint into the fog as a hard rectangle.
        if (projCoords.z <= 0.0 || projCoords.z >= 1.0) return 1.0;
        if (projCoords.x <= 0.0 || projCoords.x >= 1.0 ||
            projCoords.y <= 0.0 || projCoords.y >= 1.0) return 1.0;
        float closest = sampleDirectionalShadow(mapIndex, projCoords.xy);
        return (projCoords.z - bias > closest) ? 0.0 : 1.0;
    }

    vec3 lightToPos = worldPos - lightPosArr[i];
    float dist = length(lightToPos);
    if (dist <= 0.0001) return 1.0;
    float shadowFar = max(lightShadowFarArr[i], 0.001);
    float closest = sampleShadowCube(mapIndex, lightToPos) * shadowFar;
    return (dist - bias > closest) ? 0.0 : 1.0;
}

// Henyey-Greenstein. cosTheta is the angle between the direction the photon
// arrives travelling and the direction it scatters off in; see the call site for
// how that reduces to dot(rayDir, toLight). g > 0 is forward scattering, which
// peaks when looking into the light.
float phaseHG(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    // denom^1.5 as denom*sqrt(denom): same value, and sqrt is a native
    // instruction where pow() is exp2(log2(x)*y). Worth naming because this
    // used to run once per light per march step.
    return (1.0 - g2) / (4.0 * 3.14159265359 * (denom * sqrt(denom)));
}

// Interleaved gradient noise - one cheap hash that decorrelates neighbouring
// pixels well enough to turn march banding into fine grain.
float dither(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main()
{
    float depth = texture(sceneDepth, TexCoord).r;

    // Reconstruct the world position this pixel's surface sits at. At depth 1.0
    // nothing was drawn, so the ray runs to the far plane and the sky still
    // gets shafts through it.
    vec4 ndc = vec4(TexCoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = invViewProj * ndc;
    if (abs(world.w) < 1e-6) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 surfacePos = world.xyz / world.w;

    vec3 toSurface = surfacePos - viewPos;
    float rayLength = min(length(toSurface), max(maxDistance, 0.01));
    if (rayLength <= 0.001) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 rayDir = normalize(toSurface);

    int stepCount = clamp(steps, 4, 256);
    float stepLength = rayLength / float(stepCount);

    float offset = fract(dither(gl_FragCoord.xy) + frameJitter);

    vec3 accum = vec3(0.0);
    int count = min(lightCount, MAX_LIGHTS);

    float g = clamp(scattering, -0.95, 0.95);

    // Extinction along the ray. Without it the integral grows linearly with ray
    // length and never saturates, so a pixel looking at open sky (marching the
    // full max distance) drowns out one looking at a wall a few metres away, and
    // the shafts that are the entire point of the effect sit at a few percent of
    // the background. With it the accumulation approaches
    // (1 - exp(-density * length)), so what varies across the screen is the
    // shadowing pattern rather than raw distance.
    //
    // Only the in-scattering is attenuated, not the scene behind it: this pass
    // composites additively and the engine already has a separate fog system for
    // darkening distant surfaces. Doing both here would double up with it.
    float stepTransmittance = exp(-max(density, 0.0) * stepLength);

    // Lights outer, steps inner.
    //
    // The sum is over exactly the same terms either way - addition commutes - but
    // this order hoists every per-light constant out of the march. A directional
    // light's phase in particular is fixed along the whole ray and used to be
    // recomputed, with a pow(), at every single step. It also keeps ONE shadow
    // map resident for a whole ray instead of cycling through all of them at
    // every step, which is the difference that matters on a tile-based mobile
    // GPU where the texture cache is small.
    for (int i = 0; i < MAX_LIGHTS; ++i) {
        if (i >= count) break;
        // No shadow map, nothing to scatter: an unshadowed light would add a
        // flat wash over the whole screen rather than a shaft.
        if (lightShadowKindArr[i] <= 0) continue;
        float lightIntensity = lightIntensityArr[i];
        if (lightIntensity <= 0.0) continue;

        int ltype = lightTypeArr[i];
        bool isDirectional = (ltype == 0);
        bool isSpot = (ltype == 2);
        float range = lightRangeArr[i];
        vec3 lightPos = lightPosArr[i];
        vec3 lightColor = lightColorArr[i];

        vec3 dirToLight = vec3(0.0);
        float dirPhase = 0.0;
        vec3 spotDir = vec3(0.0, -1.0, 0.0);

        if (isDirectional) {
            dirToLight = -normalize(lightDirArr[i]);
            dirPhase = phaseHG(dot(rayDir, dirToLight), g);
        } else {
            // Reject a light whose sphere of influence this ray never enters,
            // before marching a single step. In a room lit by a dozen lamps most
            // rays miss most of them, and the old order paid a full march for
            // every one.
            if (range > 0.0) {
                float along = clamp(dot(lightPos - viewPos, rayDir), 0.0, rayLength);
                vec3 closest = viewPos + rayDir * along;
                if (distance(closest, lightPos) > range) continue;
            }
            if (isSpot) {
                spotDir = normalize(lightDirArr[i]);
            }
        }

        float transmittance = 1.0;
        for (int s = 0; s < stepCount; ++s) {
            // Offsetting the first sample by a per-pixel fraction of a step is
            // what stops the march from drawing concentric bands where every
            // pixel samples at the same distances.
            vec3 samplePos = viewPos + rayDir * ((float(s) + offset) * stepLength);

            vec3 toLight = dirToLight;
            float attenuation = 1.0;
            float phase = dirPhase;

            if (!isDirectional) {
                vec3 delta = lightPos - samplePos;
                float dist = length(delta);
                if (dist > 0.0001 && (range <= 0.0 || dist <= range)) {
                    toLight = delta / dist;
                    if (range > 0.0) {
                        float falloff = clamp(1.0 - (dist / range), 0.0, 1.0);
                        attenuation = falloff * falloff;
                    }
                    if (isSpot) {
                        float cosTheta = dot(-toLight, spotDir);
                        attenuation *= smoothstep(lightOuterCosArr[i],
                                                  lightInnerCosArr[i], cosTheta);
                    }
                    // Phase angle is between the direction the photon arrives
                    // travelling and the direction it leaves in. It arrives along
                    // -toLight, and to reach the eye it must leave along -rayDir,
                    // so cos(theta) = dot(-toLight, -rayDir) = dot(toLight, rayDir).
                    // Getting this backwards puts the forward-scattering peak
                    // behind the camera: beams vanish exactly when you look toward
                    // the light, which is the one time they are supposed to blaze.
                    phase = phaseHG(dot(rayDir, toLight), g);
                } else {
                    attenuation = 0.0;
                }
            }

            if (attenuation > 0.0001) {
                float lit = visibility(i, samplePos);
                if (lit > 0.0) {
                    accum += lightColor * lightIntensity * attenuation * lit * phase *
                             transmittance;
                }
            }

            // Applied after the lights for this step, so the first sample is
            // unattenuated - it sits at the camera, with no medium in front of it.
            transmittance *= stepTransmittance;
            // Nothing beyond this contributes a representable amount, and the
            // loop is the most expensive thing in the frame.
            if (transmittance < 0.002) break;
        }
    }

    vec3 result = accum * max(density, 0.0) * stepLength * max(intensity, 0.0);
    result = max(result, vec3(0.0));

    // Smooth saturation rather than a clamp. maxIntensity * (1 - exp(-x/max))
    // is the identity for small x - so shafts in a dark room are untouched -
    // and asymptotes to maxIntensity instead of clipping, which means a bright
    // shaft rolls off into the ceiling rather than hitting a hard edge that
    // then shows up as a contour in the bloom.
    if (maxIntensity > 0.0) {
        result = maxIntensity * (vec3(1.0) - exp(-result / maxIntensity));
    }

    FragColor = vec4(result, 1.0);
}
