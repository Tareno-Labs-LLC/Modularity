#version 330 core

in vec2 TexCoord;
uniform sampler2D albedoTex;
uniform sampler2D opacityMap;
uniform bool hasAlbedoTex = false;
uniform bool hasOpacityMap = false;
uniform vec4 uvTransform = vec4(1.0, 1.0, 0.0, 0.0);
uniform float materialAlpha = 1.0;
uniform float alphaCutoff = 0.0;

void testShadowAlpha()
{
    vec2 uv = TexCoord * uvTransform.xy + uvTransform.zw;
    float alpha = materialAlpha;
    if (hasOpacityMap) alpha *= texture(opacityMap, uv).r;
    else if (hasAlbedoTex) alpha *= texture(albedoTex, uv).a;
    if (alpha <= 0.001 || (alphaCutoff > 0.0 && alpha < alphaCutoff)) discard;
    // Explicit cutouts remain binary. Otherwise ordered coverage approximates
    // translucent shadows using the existing depth maps and shadow filtering.
    if (alphaCutoff <= 0.0) {
        const int bayer[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
        ivec2 pixel = ivec2(gl_FragCoord.xy) % 4;
        float threshold = (float(bayer[pixel.y * 4 + pixel.x]) + 0.5) / 16.0;
        if (alpha < threshold) discard;
    }
}


void main()
{
    testShadowAlpha();
}
