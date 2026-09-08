#version 330 core
// Screen-space reflections.
//
// The march walks the ray in SCREEN space, one step per pixel it crosses, not in
// fixed world-space increments. That matters twice over:
//
//   Banding. A fixed world step is many pixels wide near the camera and far
//   below a pixel in the distance, so the same ray both skips over thin geometry
//   (the gaps) and re-tests the same texel repeatedly (the stripes). Stepping by
//   pixels samples the depth buffer exactly as finely as it is stored.
//
//   Cost. Step count becomes proportional to how far the reflection actually
//   travels on screen rather than to a world distance guessed in advance, so a
//   short reflection is cheap and `steps` is a ceiling instead of a fixed price.
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
// Ceiling on the march, not a fixed count. A ray crossing 40 pixels takes 40
// steps whatever this says.
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

    // A ray heading behind the camera crosses w = 0, where the projection is
    // undefined. Clip the segment to just in front of the eye instead of letting
    // the interpolation pass through the singularity and wrap to nonsense.
    const float kMinW = 0.001;
    if (endClip.w < kMinW) {
        float t = (kMinW - startClip.w) / (endClip.w - startClip.w);
        endClip = mix(startClip, endClip, clamp(t, 0.0, 1.0));
    }
    if (startClip.w < kMinW) {
        FragColor = vec4(0.0);
        return;
    }

    vec2 startUv = (startClip.xy / startClip.w) * 0.5 + 0.5;
    vec2 endUv = (endClip.xy / endClip.w) * 0.5 + 0.5;

    // One step per pixel the ray crosses, capped by `steps`. This single line is
    // what makes the cost track the reflection's real screen extent.
    vec2 deltaPixels = (endUv - startUv) * screenSize;
    float pixelSpan = max(abs(deltaPixels.x), abs(deltaPixels.y));
    int stepCount = int(clamp(pixelSpan, 1.0, float(clamp(steps, 1, 256))));
    float dt = 1.0 / float(stepCount);
    float jitter = dither(gl_FragCoord.xy) * dt;

    bool hit = false;
    vec2 hitUv = vec2(0.0);
    float tPrev = 0.0;

    for (int i = 1; i <= 256; ++i) {
        if (i > stepCount) break;
        float t = min(float(i) * dt + jitter, 1.0);

        // Linear interpolation in clip space is linear along the world-space
        // ray, so this stays perspective-correct without tracking view depth
        // separately.
        vec4 clip = mix(startClip, endClip, t);
        if (clip.w < kMinW) break;
        vec3 ndc = clip.xyz / clip.w;
        vec2 uv = ndc.xy * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;

        float sceneD = linearize(texture(sceneDepth, uv).r);
        float rayD = linearize(ndc.z * 0.5 + 0.5);
        float delta = rayD - sceneD;

        if (delta > 0.0 && delta < thickness) {
            // Binary search between the last miss and this hit, in the same
            // parameter. The march is deliberately coarse; this is what turns a
            // crossing back into a sharp contact.
            float lo = tPrev;
            float hi = t;
            for (int r = 0; r < 16; ++r) {
                if (r >= refineSteps) break;
                float mid = (lo + hi) * 0.5;
                vec4 midClip = mix(startClip, endClip, mid);
                if (midClip.w < kMinW) break;
                vec3 midNdc = midClip.xyz / midClip.w;
                vec2 midUv = midNdc.xy * 0.5 + 0.5;
                float midScene = linearize(texture(sceneDepth, midUv).r);
                float midRay = linearize(midNdc.z * 0.5 + 0.5);
                if (midRay - midScene > 0.0) {
                    hi = mid;
                    uv = midUv;
                } else {
                    lo = mid;
                }
            }
            hit = true;
            hitUv = uv;
            break;
        }

        tPrev = t;
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
    float roughnessFade = 1.0 - smoothstep(maxRoughness * 0.5, maxRoughness, roughness);
    float weight = clamp(fresnel * confidence * roughnessFade * max(intensity, 0.0), 0.0, 1.0);

    FragColor = vec4(reflectedColor * weight, weight);
}
