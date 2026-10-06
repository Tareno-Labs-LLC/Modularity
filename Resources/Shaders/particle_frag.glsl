#version 330 core
out vec4 FragColor;

in vec2 TexCoord;
in vec4 Color;
in vec3 WorldPos;

uniform sampler2D particleTexture;
// The material's emission colour times its strength; glows past 1 feed bloom like a mesh's.
uniform vec3 emission = vec3(0.0);
// 0 alpha, 1 additive, 2 premultiplied, 3 multiply. The blend function is set to match on
// the CPU; this decides what the fragment has to hand it.
uniform int blendMode = 0;

uniform vec3 viewPos;
uniform bool fogEnabled = false;
uniform int fogMode = 0;
uniform vec3 fogColor = vec3(0.65, 0.72, 0.78);
uniform float fogStart = 20.0;
uniform float fogEnd = 120.0;
uniform float fogDensity = 0.015;
uniform float fogHeight = 0.0;
uniform float fogHeightFalloff = 0.0;

// Same curve as frag.glsl's computeFogFactor, so a particle fades into the same haze as the
// geometry beside it instead of glowing through it.
float computeFogFactor()
{
    if (!fogEnabled) {
        return 0.0;
    }
    float dist = length(viewPos - WorldPos);
    float factor = 0.0;
    if (fogMode == 1) {
        factor = 1.0 - exp(-max(fogDensity, 0.0) * dist);
    } else if (fogMode == 2) {
        float densityDistance = max(fogDensity, 0.0) * dist;
        factor = 1.0 - exp(-(densityDistance * densityDistance));
    } else {
        factor = smoothstep(fogStart, max(fogStart + 0.01, fogEnd), dist);
    }
    if (fogHeightFalloff > 0.0001) {
        float heightWeight = 1.0 - exp(-abs(WorldPos.y - fogHeight) * fogHeightFalloff);
        factor *= clamp(heightWeight, 0.0, 1.0);
    }
    return clamp(factor, 0.0, 1.0);
}

void main()
{
    vec4 tex = texture(particleTexture, TexCoord);
    float fog = computeFogFactor();

    if (blendMode == 3) {
        // Multiply: white is "no change", so transparent texels and fog both fade to white.
        vec3 tinted = tex.rgb * Color.rgb;
        vec3 multiplied = mix(vec3(1.0), tinted, tex.a * Color.a);
        FragColor = vec4(mix(multiplied, vec3(1.0), fog), 1.0);
        return;
    }

    if (blendMode == 2) {
        // Premultiplied texture: its colour is already weighted by its own alpha.
        float alpha = tex.a * Color.a;
        vec3 rgb = (tex.rgb * Color.rgb + emission * tex.a) * Color.a;
        if (alpha <= 0.002 && dot(rgb, vec3(1.0)) <= 0.002) {
            discard;
        }
        FragColor = vec4(mix(rgb, fogColor * alpha, fog), alpha);
        return;
    }

    vec4 color = tex * Color;
    color.rgb += emission * tex.a;
    if (color.a <= 0.002) {
        discard;
    }
    if (blendMode == 1) {
        // Additive light fades out into fog rather than taking on the fog's colour.
        FragColor = vec4(color.rgb * (1.0 - fog), color.a);
        return;
    }
    FragColor = vec4(mix(color.rgb, fogColor, fog), color.a);
}
