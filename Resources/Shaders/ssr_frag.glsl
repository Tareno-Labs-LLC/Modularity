#version 330 core
// Screen-space reflections.
//
// March uniformly in projected screen coordinates, with perspective-correct
// depth. Clip the ray before dividing by w so offscreen endpoints do not spend
// the sample budget outside the viewport or across the camera plane.
//
// Output is premultiplied colour with the blend weight in alpha, composited with
//   glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE)
// which evaluates to mix(scene, reflection, weight) and leaves destination alpha
// alone, so a preview target rendered for a transparent background still means
// what it did.
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D sceneColor;
uniform sampler2D sceneDepth;
uniform sampler2D sceneSurface;   // octahedral normal RG, roughness B, metallic A
uniform samplerCube fallbackCube;
uniform bool hasFallbackCube = false;

uniform mat4 viewProj;
uniform mat4 invViewProj;
uniform vec3 viewPos;
uniform vec2 screenSize;
uniform float nearPlane;
uniform float farPlane;

uniform float intensity = 1.0;
// Maximum sample budget. Short rays use at most one sample per pixel; longer
// rays distribute this budget over the visible segment.
uniform int steps = 48;
uniform int refineSteps = 6;
uniform float maxDistance = 40.0;
// How deep behind a surface a ray may land and still count as touching it. Too
// small and reflections break up on grazing geometry; too large and the ray
// happily "hits" the far side of a thin object.
uniform float thickness = 0.5;
// Reflections stop above this roughness. Screen-space tracing is a mirror
// technique: a rough surface needs many rays, and one ray blurred is just a
// smear, so it is honest to hand rough surfaces back to the cube instead.
uniform float maxRoughness = 0.6;

vec3 octDecode(vec2 f)
{
    vec3 n = vec3(f.xy, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

vec3 worldFromDepth(vec2 uv, float depth)
{
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = invViewProj * ndc;
    return world.xyz / world.w;
}

// Non-linear depth compares badly: the same delta means metres near the far
// plane and millimetres near the camera, so the thickness test would have to be
// depth-dependent. Linearising first lets thickness stay a world-space number.
float linearize(float depth)
{
    float z = depth * 2.0 - 1.0;
    return (2.0 * nearPlane * farPlane) /
           (farPlane + nearPlane - z * (farPlane - nearPlane));
}

// Interleaved gradient noise. Offsetting each pixel's first step by a fraction of
// a step turns whatever banding survives the screen-space march into fine grain,
// which the eye averages instead of reading as rings.
float dither(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Clip the end of a segment against one homogeneous frustum plane. The
// origin must be inside; plane distances are linear before perspective divide.
bool clipRayPlane(float startDistance, float endDistance, inout float endT)
{
    if (startDistance < 0.0) return false;
    if (endDistance < 0.0) {
        endT = min(endT, startDistance / (startDistance - endDistance));
    }
    return true;
}

void main()
{
    float depth = texture(sceneDepth, TexCoord).r;
    if (depth >= 1.0) {          // sky: nothing was drawn here, nothing reflects
        FragColor = vec4(0.0);
        return;
    }

    vec4 surface = texture(sceneSurface, TexCoord);
    float roughness = surface.b;
    float metallic = surface.a;
    if (roughness > maxRoughness) {
        FragColor = vec4(0.0);
        return;
    }

    vec3 normal = octDecode(surface.rg);
    vec3 worldPos = worldFromDepth(TexCoord, depth);
    vec3 toEye = normalize(viewPos - worldPos);
    float ndotv = dot(normal, toEye);
    if (ndotv <= 0.0) {
        FragColor = vec4(0.0);
        return;
    }

    vec3 reflectDir = normalize(reflect(-toEye, normal));

    // Fresnel decides how much of this pixel is reflection at all. F0 uses
    // metalness only, not albedo, because the surface buffer does not carry
    // albedo - so a coloured metal reflects untinted here. Acceptable for a
    // screen-space add on top of shading that already ran; tinting it would need
    // a channel this target does not have.
    float f0 = mix(0.04, 1.0, metallic);
    float fresnel = f0 + (1.0 - f0) * pow(clamp(1.0 - ndotv, 0.0, 1.0), 5.0);

    // Lift off the surface so the first sample does not read this pixel's own
    // depth, which would make every surface reflect itself.
    vec3 startWorld = worldPos + normal * 0.01;
    vec3 endWorld = startWorld + reflectDir * max(maxDistance, 0.01);

    vec4 startClip = viewProj * vec4(startWorld, 1.0);
    vec4 endClip = viewProj * vec4(endWorld, 1.0);

    float endT = 1.0;
    if (!clipRayPlane(startClip.w + startClip.x, endClip.w + endClip.x, endT) ||
        !clipRayPlane(startClip.w - startClip.x, endClip.w - endClip.x, endT) ||
        !clipRayPlane(startClip.w + startClip.y, endClip.w + endClip.y, endT) ||
        !clipRayPlane(startClip.w - startClip.y, endClip.w - endClip.y, endT) ||
        !clipRayPlane(startClip.w + startClip.z, endClip.w + endClip.z, endT) ||
        !clipRayPlane(startClip.w - startClip.z, endClip.w - endClip.z, endT)) {
        FragColor = vec4(0.0);
        return;
    }
    endClip = mix(startClip, endClip, endT);
    if (startClip.w <= 0.0 || endClip.w <= 0.0) {
        FragColor = vec4(0.0);
        return;
    }
    vec3 startNdc = startClip.xyz / startClip.w;
    vec3 endNdc = endClip.xyz / endClip.w;
    vec2 startUv = startNdc.xy * 0.5 + 0.5;
    vec2 endUv = endNdc.xy * 0.5 + 0.5;

    // NDC depth, like UV, is linear along the projected segment. Interpolating
    // homogeneous clip coordinates here instead would bunch samples at one end
    // and skip large spans near the eye (especially for camera-facing rays).
    vec2 deltaPixels = (endUv - startUv) * screenSize;
    float pixelSpan = max(abs(deltaPixels.x), abs(deltaPixels.y));
    int stepCount = int(clamp(ceil(pixelSpan), 1.0, float(clamp(steps, 1, 256))));
    float dt = 1.0 / float(stepCount);
    float jitter = dither(gl_FragCoord.xy);

    bool hit = false;
    vec2 hitUv = vec2(0.0);
    float hitT = 0.0;
    float tPrev = 0.0;
    float previousDelta = linearize(startNdc.z * 0.5 + 0.5) -
                          linearize(texture(sceneDepth, startUv).r);

    for (int i = 1; i <= 256; ++i) {
        if (i > stepCount) break;
        float t = (float(i) - jitter) * dt;
        vec3 ndc = mix(startNdc, endNdc, t);
        vec2 uv = ndc.xy * 0.5 + 0.5;
        float sampledDepth = texture(sceneDepth, uv).r;
        float sceneD = linearize(sampledDepth);
        float rayD = linearize(ndc.z * 0.5 + 0.5);
        float delta = rayD - sceneD;

        // Refine front-to-back crossings even if the coarse step overshot the
        // thickness slab. Rejecting those before refinement cuts holes in the
        // reflection as distance or viewing angle changes.
        if (sampledDepth < 1.0 && previousDelta <= 0.0 && delta > 0.0) {
            float lo = tPrev;
            float hi = t;
            for (int r = 0; r < 16; ++r) {
                if (r >= refineSteps) break;
                float mid = (lo + hi) * 0.5;
                vec3 midNdc = mix(startNdc, endNdc, mid);
                vec2 midUv = midNdc.xy * 0.5 + 0.5;
                float midScene = linearize(texture(sceneDepth, midUv).r);
                float midRay = linearize(midNdc.z * 0.5 + 0.5);
                if (midRay > midScene) hi = mid;
                else lo = mid;
            }
            vec3 hitNdc = mix(startNdc, endNdc, hi);
            vec2 candidateUv = hitNdc.xy * 0.5 + 0.5;
            float candidateDepth = texture(sceneDepth, candidateUv).r;
            float hitDelta = linearize(hitNdc.z * 0.5 + 0.5) - linearize(candidateDepth);
            // A silhouette discontinuity is not necessarily an intersection.
            if (candidateDepth < 1.0 && hitDelta >= 0.0 && hitDelta <= thickness) {
                hit = true;
                hitUv = candidateUv;
                hitT = hi;
                break;
            }
        }
        tPrev = t;
        previousDelta = delta;
    }

    vec3 reflectedColor = vec3(0.0);
    float confidence = 0.0;

    if (hit) {
        reflectedColor = texture(sceneColor, hitUv).rgb;
        // Fade toward the screen border. A ray that leaves the frame has no
        // information behind it, and without this the reflection ends in a hard
        // line along the edge that slides as the camera moves.
        vec2 edge = smoothstep(vec2(0.0), vec2(0.15), hitUv) *
                    (1.0 - smoothstep(vec2(0.85), vec2(1.0), hitUv));
        confidence = edge.x * edge.y;
        // Smooth the finite trace range too, including near/far-plane exits.
        confidence *= 1.0 - smoothstep(0.8, 1.0, hitT);
        // Rays coming back toward the camera are the ones screen space cannot
        // resolve: what they would reflect is behind the viewer.
        confidence *= clamp(1.0 - max(dot(reflectDir, toEye), 0.0), 0.0, 1.0);
    }

    // Only used when the forward pass is not already applying environment
    // reflections - otherwise the sky would be reflected twice.
    if (confidence < 1.0 && hasFallbackCube) {
        vec3 skyColor = texture(fallbackCube, reflectDir).rgb;
        reflectedColor = mix(skyColor, reflectedColor, confidence);
        confidence = 1.0;
    }

    if (confidence <= 0.0) {
        FragColor = vec4(0.0);
        return;
    }

    // Fading out before the cutoff stops reflections popping out of existence
    // at maxRoughness.
    float roughnessLimit = max(maxRoughness, 0.0001);
    float roughnessFade = 1.0 - smoothstep(roughnessLimit * 0.5, roughnessLimit, roughness);
    float weight = clamp(fresnel * confidence * roughnessFade * max(intensity, 0.0), 0.0, 1.0);

    FragColor = vec4(reflectedColor * weight, weight);
}
