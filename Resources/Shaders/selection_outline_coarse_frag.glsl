#version 330 core
out vec4 FragColor;

// Coarse occupancy of the selection mask: one texel per 8x8 block of mask pixels, set when any
// pixel in the block passes the same >= 0.5 test the outline shader uses. The outline pass reads
// it to skip its per-pixel neighbourhood search wherever no mask pixel can be within reach, which
// is most of the viewport. Rendered at 1/8 resolution, so the 64 fetches per texel here cost about
// one fetch per viewport pixel.

uniform sampler2D maskTex;
uniform vec2 maskSize;

const int kBlockSize = 8;

void main()
{
    ivec2 size = ivec2(maskSize);
    ivec2 base = ivec2(gl_FragCoord.xy) * kBlockSize;
    float occupied = 0.0;
    for (int y = 0; y < kBlockSize; ++y) {
        int py = base.y + y;
        if (py >= size.y) break;
        for (int x = 0; x < kBlockSize; ++x) {
            int px = base.x + x;
            if (px >= size.x) break;
            if (texelFetch(maskTex, ivec2(px, py), 0).r >= 0.5) {
                occupied = 1.0;
                break;
            }
        }
        if (occupied > 0.5) break;
    }
    FragColor = vec4(occupied, 0.0, 0.0, 1.0);
}
