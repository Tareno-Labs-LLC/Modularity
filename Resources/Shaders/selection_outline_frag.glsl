#version 330 core
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D maskTex;
uniform vec2 texelSize;
// Optional 1/8-resolution occupancy of maskTex (selection_outline_coarse_frag.glsl).
uniform sampler2D coarseMaskTex;
uniform bool useCoarseMask = false;
uniform vec2 maskSize;
uniform vec3 outlineColor = vec3(1.0, 0.65, 0.18);
uniform float outlineRadiusPx = 2.7;
uniform float outlineSoftnessPx = 1.15;
uniform float outlineOpacity = 0.95;

float maskAt(vec2 uv)
{
    return step(0.5, texture(maskTex, uv).r);
}

void main()
{
    float center = maskAt(TexCoord);
    if (center > 0.5) {
        FragColor = vec4(0.0);
        return;
    }

    float searchRadius = max(1.0, outlineRadiusPx + outlineSoftnessPx);
    // Loops run over the search window itself (still capped at 8, as before) rather than a fixed
    // 17x17 grid that skipped most iterations, and distances are compared squared so the square
    // root is taken once for the winner. Same result: sqrt is monotonic. This pass runs for every
    // viewport pixel near the selection, and the old loop shape was most of its GPU cost.
    int r = min(int(ceil(searchRadius)), 8);
    float searchRadiusSq = searchRadius * searchRadius;

    // Every mask pixel the search below could reach lies in the 8x8 blocks covering this window
    // (clamped like the texture's edge clamp), so if none of those blocks is occupied the search
    // can only find nothing. Exact, and it skips the search for most of the viewport.
    if (useCoarseMask) {
        ivec2 maxTexel = ivec2(maskSize) - 1;
        ivec2 p = ivec2(gl_FragCoord.xy);
        ivec2 lo = clamp(p - ivec2(r), ivec2(0), maxTexel) / 8;
        ivec2 hi = clamp(p + ivec2(r), ivec2(0), maxTexel) / 8;
        bool occupied = false;
        for (int by = lo.y; by <= hi.y && !occupied; ++by) {
            for (int bx = lo.x; bx <= hi.x; ++bx) {
                if (texelFetch(coarseMaskTex, ivec2(bx, by), 0).r > 0.5) {
                    occupied = true;
                    break;
                }
            }
        }
        if (!occupied) {
            FragColor = vec4(0.0);
            return;
        }
    }

    float minDistSq = 1e18;
    for (int y = -r; y <= r; ++y) {
        float fy = float(y);
        for (int x = -r; x <= r; ++x) {
            float fx = float(x);
            float distSq = fx * fx + fy * fy;
            if (distSq > searchRadiusSq || distSq >= minDistSq) continue;
            if (maskAt(TexCoord + vec2(fx, fy) * texelSize) > 0.5) {
                minDistSq = distSq;
            }
        }
    }

    if (minDistSq > 1e17) {
        FragColor = vec4(0.0);
        return;
    }
    float minDist = sqrt(minDistSq);

    float inner = max(0.0, outlineRadiusPx - outlineSoftnessPx);
    float outer = outlineRadiusPx + outlineSoftnessPx;
    float alpha = 1.0 - smoothstep(inner, outer, minDist);
    alpha *= outlineOpacity;

    FragColor = vec4(outlineColor, alpha);
}
