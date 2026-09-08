#version 330 core

in vec2 vUv;

out vec4 FragColor;

uniform mat4 u_inverseViewProjection;
uniform mat4 u_viewProjection;
uniform sampler2D u_floorTexture;
uniform vec3 u_cameraPosition;
uniform vec2 u_boundsMinXZ;
uniform vec2 u_boundsMaxXZ;
uniform vec2 u_uvScale;
uniform int u_uvMode;         // 0 = stretch, 1 = tile repeat (mode7)
uniform vec2 u_tileWorldSize; // world units per tile cell in tile-repeat mode
uniform bool u_extendToHorizon;
uniform float u_floorHeight;
uniform float u_maxDistance;
uniform float u_perspectiveStrength;
uniform float u_horizonOffset;
uniform bool u_hasTexture;

// glue-style fallback tile: teal checker cells with bright borders
vec3 sampleProceduralFloor(vec2 uv) {
    vec2 cellUv = fract(uv);
    float edge = max(abs(cellUv.x - 0.5), abs(cellUv.y - 0.5));
    float borderLine = smoothstep(0.36, 0.48, edge);

    vec2 checkerCell = floor(uv);
    float checker = mod(checkerCell.x + checkerCell.y, 2.0);
    vec3 base = mix(vec3(0.30, 0.78, 0.62), vec3(0.26, 0.66, 0.74), checker);

    float innerGlow = 1.0 - smoothstep(0.0, 0.42, edge);
    base += vec3(0.10, 0.12, 0.12) * innerGlow * 0.5;
    return mix(base, vec3(0.92, 0.97, 0.95), borderLine * 0.85);
}

void main() {
    vec2 ndc = vec2(vUv.x * 2.0 - 1.0, vUv.y * 2.0 - 1.0 + u_horizonOffset);

    vec4 nearPoint = u_inverseViewProjection * vec4(ndc, -1.0, 1.0);
    vec4 farPoint = u_inverseViewProjection * vec4(ndc, 1.0, 1.0);
    nearPoint /= max(nearPoint.w, 0.0001);
    farPoint /= max(farPoint.w, 0.0001);

    vec3 rayDir = normalize(farPoint.xyz - nearPoint.xyz);
    rayDir.y *= u_perspectiveStrength;
    rayDir = normalize(rayDir);

    if (abs(rayDir.y) < 0.0001) {
        discard;
    }

    float t = (u_floorHeight - u_cameraPosition.y) / rayDir.y;
    if (t <= 0.0) {
        discard;
    }

    vec3 worldPos = u_cameraPosition + rayDir * t;
    if (!u_extendToHorizon &&
        (worldPos.x < u_boundsMinXZ.x || worldPos.x > u_boundsMaxXZ.x ||
         worldPos.z < u_boundsMinXZ.y || worldPos.z > u_boundsMaxXZ.y)) {
        discard;
    }

    float distanceToCamera = distance(worldPos.xz, u_cameraPosition.xz);
    if (distanceToCamera > u_maxDistance) {
        discard;
    }

    vec2 uv = (u_uvMode == 1)
        ? worldPos.xz / max(u_tileWorldSize, vec2(0.0001))
        : worldPos.xz * u_uvScale;
    vec4 shaded = u_hasTexture
        ? texture(u_floorTexture, uv)
        : vec4(sampleProceduralFloor(uv), 1.0);

    float fade = 1.0 - smoothstep(u_maxDistance * 0.72, u_maxDistance, distanceToCamera);
    shaded.rgb *= mix(0.55, 1.0, fade);

    vec4 clipPos = u_viewProjection * vec4(worldPos, 1.0);
    float ndcDepth = clipPos.z / max(clipPos.w, 0.0001);
    gl_FragDepth = ndcDepth * 0.5 + 0.5;

    FragColor = vec4(shaded.rgb, 1.0);
}
