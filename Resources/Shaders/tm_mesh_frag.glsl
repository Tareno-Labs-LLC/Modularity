#version 330 core

in vec3 vWorldPos;
in vec3 vNormal;
smooth in vec2 vUv;
smooth in vec3 vAffineUvw;

out vec4 FragColor;

uniform sampler2D u_albedoTexture;
uniform vec3 u_cameraPosition;
uniform vec3 u_lightDirection;
uniform bool u_hasTexture;
uniform vec4 u_colorTint;
uniform bool u_affineWarpEnabled;
uniform bool u_flatShadingEnabled;
uniform int u_shadeLevels;

void main() {
    vec2 sampleUv = u_affineWarpEnabled
        ? vAffineUvw.xy / max(vAffineUvw.z, 0.0001)
        : vUv;

    vec4 texel = u_hasTexture ? texture(u_albedoTexture, sampleUv) : vec4(1.0);
    texel *= u_colorTint;
    if (texel.a < 0.05) {
        discard;
    }

    vec3 normal = u_flatShadingEnabled
        ? normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)))
        : normalize(vNormal);
    vec3 lightDir = normalize(-u_lightDirection);
    float diffuse = max(dot(normal, lightDir), 0.0);
    if (u_shadeLevels > 0) {
        diffuse = floor(diffuse * float(u_shadeLevels) + 0.5) / float(u_shadeLevels);
    }
    float ambient = 0.38;
    float fog = smoothstep(36.0, 128.0, distance(vWorldPos, u_cameraPosition));

    vec3 lit = texel.rgb * (ambient + diffuse * 0.62);
    vec3 fogColor = vec3(0.14, 0.17, 0.22);
    lit = mix(lit, fogColor, fog * 0.35);

    FragColor = vec4(lit, texel.a);
}
