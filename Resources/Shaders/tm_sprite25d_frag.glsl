#version 330 core

in vec2 vUv;
in vec4 vColor;
in vec3 vWorldPosition;

out vec4 FragColor;

uniform sampler2D u_spriteTexture;
uniform bool u_hasTexture;

const int MAX_POINT_LIGHTS = 16;
uniform int u_pointLightCount;
uniform vec3 u_pointLightPosition[MAX_POINT_LIGHTS];
uniform vec3 u_pointLightColor[MAX_POINT_LIGHTS];
uniform float u_pointLightIntensity[MAX_POINT_LIGHTS];
uniform float u_pointLightRange[MAX_POINT_LIGHTS];

void main() {
    vec4 texel = u_hasTexture ? texture(u_spriteTexture, vUv) : vec4(1.0);
    vec4 color = texel * vColor;
    if (color.a < 0.05) {
        discard;
    }

    // Sprite25D billboards have no useful surface normal for lamps beside them, so
    // local lights use radial attenuation instead of Lambert shading. This preserves
    // the authored sprite while allowing nearby 3D point lights to add colored light.
    vec3 localLight = vec3(0.0);
    int lightCount = min(u_pointLightCount, MAX_POINT_LIGHTS);
    for (int i = 0; i < lightCount; ++i) {
        float range = max(u_pointLightRange[i], 0.001);
        float distanceToLight = length(u_pointLightPosition[i] - vWorldPosition);
        if (distanceToLight >= range || u_pointLightIntensity[i] <= 0.0) {
            continue;
        }
        float falloff = clamp(1.0 - distanceToLight / range, 0.0, 1.0);
        falloff *= falloff;
        localLight += u_pointLightColor[i] * u_pointLightIntensity[i] * falloff * 0.22;
    }
    color.rgb *= vec3(1.0) + min(localLight, vec3(1.65));
    FragColor = color;
}
