#version 330 core
in vec2 TexCoord;
out vec2 FragColor;
uniform sampler2D sceneDepth;
uniform mat4 inverseProjection;
uniform mat4 projection;
uniform vec2 depthTexelSize;
uniform float radius;

vec3 viewPosition(vec2 uv) {
    vec4 p = inverseProjection * vec4(uv * 2.0 - 1.0, texture(sceneDepth, uv).r * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

void main() {
    // Snap to a full-resolution depth texel; never average foreground and background depth.
    vec2 uv = (floor(TexCoord / depthTexelSize) + 0.5) * depthTexelSize;
    float depth = texture(sceneDepth, uv).r;
    if (depth >= 0.999999) { FragColor = vec2(1.0, 0.0); return; }
    vec3 p = viewPosition(uv);
    vec3 l = viewPosition(uv - vec2(depthTexelSize.x, 0.0));
    vec3 r = viewPosition(uv + vec2(depthTexelSize.x, 0.0));
    vec3 d = viewPosition(uv - vec2(0.0, depthTexelSize.y));
    vec3 u = viewPosition(uv + vec2(0.0, depthTexelSize.y));
    // Use the closer neighbor on each axis, so silhouettes do not create false normals.
    vec3 dx = abs(l.z - p.z) < abs(r.z - p.z) ? p - l : r - p;
    vec3 dy = abs(d.z - p.z) < abs(u.z - p.z) ? p - d : u - p;
    vec3 n = normalize(cross(dx, dy) + vec3(0.0, 0.0, 1e-12));
    if (dot(n, -p) < 0.0) n = -n;
    float projectedRadius = radius * abs(projection[1][1]) * 0.5;
    if (abs(projection[3][3]) < 0.5) projectedRadius /= max(-p.z, 0.001);
    float radiusPixels = min(projectedRadius / depthTexelSize.y, 128.0);
    float noise = fract(52.9829189 * fract(dot(floor(gl_FragCoord.xy), vec2(0.06711056, 0.00583715))));
    float occlusion = 0.0;
    // Six horizon directions, three steps each, at quarter pixel count. The noise
    // is spatial only: no changing pattern or temporal ghosting in the headset.
    for (int ray = 0; ray < 6; ++ray) {
        float angle = (float(ray) + noise) * 1.0471975512;
        vec2 direction = vec2(cos(angle), sin(angle));
        float horizon = 0.0;
        for (int stepIndex = 0; stepIndex < 3; ++stepIndex) {
            float fraction = (float(stepIndex) + 0.6 + noise * 0.4) / 3.0;
            vec2 offset = round(direction * max(1.0, radiusPixels * fraction * fraction)) * depthTexelSize;
            vec2 sampleUv = uv + offset;
            if (any(lessThan(sampleUv, vec2(0.0))) || any(greaterThanEqual(sampleUv, vec2(1.0)))) continue;
            if (texture(sceneDepth, sampleUv).r >= 0.999999) continue;
            vec3 delta = viewPosition(sampleUv) - p;
            float distanceSquared = dot(delta, delta);
            float falloff = max(0.0, 1.0 - distanceSquared / (radius * radius));
            // Angular bias removes self-occlusion on flat surfaces without losing contacts.
            float angular = max(0.0, dot(n, delta) * inversesqrt(max(distanceSquared, 1e-8)) - 0.04);
            horizon = max(horizon, angular * falloff);
        }
        occlusion += horizon;
    }
    float visibility = clamp(1.0 - occlusion * (2.0 / 6.0), 0.0, 1.0);
    FragColor = vec2(visibility, min(-p.z, 65000.0));
}
