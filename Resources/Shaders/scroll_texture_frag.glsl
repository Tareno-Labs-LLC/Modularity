#version 330 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;

uniform sampler2D texture1;
uniform sampler2D overlayTex;
uniform float mixAmount = 0.2;
uniform bool hasOverlay = false;
uniform bool unlit = false;
uniform vec4 uvTransform = vec4(1.0, 1.0, 0.0, 0.0);

uniform float uTime = 0.0;
uniform float uScrollSpeed = 0.5;          // UV units per second
uniform vec2 uScrollDir = vec2(1.0, 0.3);  // drift axis, normalized below
uniform vec3 materialColor = vec3(1.0);
uniform float materialAlpha = 1.0;
uniform float ambientStrength = 0.2;
uniform float specularStrength = 0.5;
uniform float shininess = 32.0;
uniform vec3 viewPos;

void main()
{
    float speed = max(uScrollSpeed, 0.0);
    // pure direction; a zero vector just parks the scroll instead of NaN-ing
    vec2 baseDir = (length(uScrollDir) > 0.0001) ? normalize(uScrollDir) : vec2(0.0);
    vec2 transformedUv = TexCoord * uvTransform.xy + uvTransform.zw;
    vec2 baseUV = transformedUv + baseDir * (uTime * speed);
    vec4 baseSample = texture(texture1, baseUV);

    vec3 color = baseSample.rgb;
    if (hasOverlay) {
        // detail layer drifts along the same axis but slower for a parallax feel
        vec2 overlayUV = transformedUv + baseDir * (uTime * speed * 0.65);
        vec3 overlayColor = texture(overlayTex, overlayUV).rgb;
        color = mix(color, overlayColor, clamp(mixAmount, 0.0, 1.0));
    }
    color *= materialColor;
    float alpha = baseSample.a * materialAlpha;
    if (alpha <= 0.001) {
        discard;
    }

    if (unlit) {
        FragColor = vec4(color, alpha);
        return;
    }

    vec3 N = normalize(Normal);
    vec3 L = normalize(vec3(0.35, 0.9, 0.2));
    vec3 V = normalize(viewPos - FragPos);
    vec3 H = normalize(L + V);

    float diffuse = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), max(shininess, 1.0)) * clamp(specularStrength, 0.0, 2.0);
    vec3 lit = color * (clamp(ambientStrength, 0.0, 1.0) + diffuse) + vec3(spec);

    FragColor = vec4(lit, alpha);
}
