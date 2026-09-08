#version 330 core
out vec4 FragColor;
in vec3 fragPos;

uniform float timeOfDay;
uniform float uSkyTime;
uniform sampler2D uSunTex;
uniform sampler2D uMoonTex;
uniform sampler2D uScrollTex;
uniform int uSkyMode;
uniform vec2 uScrollRepeat;
uniform float uScrollLookSensitivity;
uniform float uScrollVerticalInfluence;
uniform bool uHasScrollTexture;
uniform vec2 uViewportSize;
uniform vec2 uCameraAngles;
uniform mat4 projection;
uniform mat4 view;
uniform mat4 uInvProjection;
uniform mat3 uInvViewRot;
uniform bool uUseBakedSky;
// When set, the sun sits where the scene's directional light points from rather
// than on the timeOfDay arc. uLightSunDirection points TOWARD the light source.
// -- Volumetric clouds --
//
// 0 keeps the original flat FBM band; 1 ray-marches a cloud slab with a light
// march toward the sun for self-shadowing. The marched version is far more
// expensive per pixel, which is affordable here only because the procedural sky
// is normally baked into a cubemap a face at a time - turn cachedSky off and it
// is paid per screen pixel per frame instead.
uniform int uCloudMode = 0;
uniform float uCloudCoverage = 0.45;
uniform float uCloudDensity = 1.0;
uniform int uCloudSteps = 24;
uniform int uCloudLightSteps = 4;
uniform float uCloudBottom = 900.0;
uniform float uCloudTop = 2400.0;
uniform float uCloudWind = 1.0;
// Weather drives this: 0 leaves clouds lit normally, 1 crushes them toward
// storm grey. Kept separate from coverage so an overcast sky and a black
// thunderhead are two different knobs rather than one.
uniform float uCloudDarkness = 0.0;
// Where the camera actually is. The march starts here rather than at the world
// origin, which is what lets you climb into and above the cloud layer instead
// of it being an infinitely distant painted dome.
uniform vec3 uCameraPos = vec3(0.0);
// Clouds fade out as the view approaches the horizon. The slab is finite and
// the noise tiles, so at grazing angles you can see it end; fading there reads
// as distance haze instead of an edge.
uniform float uCloudHorizonFade = 0.16;
uniform bool uUseLightSunDirection;
uniform vec3 uLightSunDirection;
uniform samplerCube uBakedSkyCube;

const float PI = 3.14159265359;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(hash(i + vec2(0.0, 0.0)), hash(i + vec2(1.0, 0.0)), u.x),
        mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x),
        u.y
    );
}

float noisePeriodic(vec2 p, vec2 period) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    vec2 i00 = mod(i + vec2(0.0, 0.0), period);
    vec2 i10 = mod(i + vec2(1.0, 0.0), period);
    vec2 i01 = mod(i + vec2(0.0, 1.0), period);
    vec2 i11 = mod(i + vec2(1.0, 1.0), period);
    return mix(
        mix(hash(i00), hash(i10), u.x),
        mix(hash(i01), hash(i11), u.x),
        u.y
    );
}

float fbm(vec2 p) {
    float value = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; ++i) {
        value += noise(p) * amp;
        p = p * 2.03 + vec2(17.2, 9.3);
        amp *= 0.5;
    }
    return value;
}

float fbmPeriodic(vec2 p, vec2 period) {
    float value = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; ++i) {
        value += noisePeriodic(p, period) * amp;
        p = p * 2.0 + vec2(17.2, 9.3);
        period *= 2.0;
        amp *= 0.5;
    }
    return value;
}

// 3D value noise. The band clouds above get away with 2D because they are a
// flat sheet; a marched cloud needs density to vary with altitude too, or every
// sample along a ray through it returns the same value and the slab reads as a
// solid wall.
float hash3(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// NOT named noise3: GLSL reserves noise1..noise4 as built-ins, and
// redeclaring one is "overloaded functions must have the same return
// type" - which fails the entire shader, not just this function.
float cloudValueNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i + vec3(0.0, 0.0, 0.0)), hash3(i + vec3(1.0, 0.0, 0.0)), f.x),
                   mix(hash3(i + vec3(0.0, 1.0, 0.0)), hash3(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0.0, 0.0, 1.0)), hash3(i + vec3(1.0, 0.0, 1.0)), f.x),
                   mix(hash3(i + vec3(0.0, 1.0, 1.0)), hash3(i + vec3(1.0, 1.0, 1.0)), f.x), f.y),
               f.z);
}

// Octave count is a parameter because the light march does not need the
// detail the view march does - it is only asking "how much stuff is between
// this point and the sun", and that answer is dominated by the low
// frequencies. Two octaves there instead of four is most of a 2x saving on the
// inner loop, which is the hottest code in the whole sky.
float fbm3(vec3 p, int octaves) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; ++i) {
        if (i >= octaves) break;
        v += a * cloudValueNoise(p);
        p *= 2.03;
        a *= 0.5;
    }
    return v;
}

// Density at a point inside the slab. heightFrac is 0 at the base and 1 at the
// top; the gradient rounds the bottom and flattens the top, which is what makes
// the silhouette read as cloud rather than as noise in a box.
float cloudDensityAt(vec3 p, float heightFrac, int octaves) {
    vec3 wind = vec3(uSkyTime * 0.010, 0.0, uSkyTime * 0.004) * uCloudWind * 100.0;
    float base = fbm3((p + wind) * 0.0011, octaves);
    float shape = base;
    if (octaves > 2) {
        float detail = fbm3((p + wind * 1.7) * 0.0043, octaves);
        shape = base * 0.78 + detail * 0.22;
    }

    float gradient = smoothstep(0.0, 0.22, heightFrac) *
                     (1.0 - smoothstep(0.5, 1.0, heightFrac));
    shape *= gradient;

    // Coverage as a threshold rather than a multiply: multiplying thins every
    // cloud uniformly into haze, where a threshold removes the weak ones and
    // leaves the strong ones with their edges intact.
    float cov = clamp(uCloudCoverage, 0.0, 1.0);
    float d = smoothstep(0.62 - cov * 0.42, 0.86 - cov * 0.42, shape);
    return d * max(uCloudDensity, 0.0);
}

// Henyey-Greenstein, same as the volumetric lighting pass uses.
float cloudPhase(float cosTheta, float g) {
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g2) / (4.0 * PI * (denom * sqrt(denom)));
}

// Returns rgb = in-scattered light, a = how much of the sky behind is hidden.
vec4 marchClouds(vec3 dir, vec3 sunDir, float dayAmount) {
    float bottom = max(uCloudBottom, 1.0);
    float top = max(uCloudTop, bottom + 1.0);
    vec3 origin = uCameraPos;

    // Slab intersection from wherever the camera actually is, which covers all
    // three cases without special-casing them: below looking up, inside the
    // layer, and above it looking back down.
    float tEnter;
    float tExit;
    if (abs(dir.y) < 0.001) {
        // Parallel to the slab - it is only ever hit from inside.
        if (origin.y < bottom || origin.y > top) return vec4(0.0);
        tEnter = 0.0;
        tExit = (top - bottom) * 12.0;
    } else {
        float t0 = (bottom - origin.y) / dir.y;
        float t1 = (top - origin.y) / dir.y;
        tEnter = min(t0, t1);
        tExit = max(t0, t1);
        if (tExit <= 0.0) return vec4(0.0);   // slab entirely behind the camera
        tEnter = max(tEnter, 0.0);
    }

    float span = tExit - tEnter;
    if (span <= 0.0) return vec4(0.0);

    // Rays near the horizon cross an enormous slice of the slab. Capping the
    // marched length keeps the step size sane there instead of spending the
    // whole budget on one pixel and aliasing badly.
    span = min(span, (top - bottom) * 12.0);

    int steps = clamp(uCloudSteps, 4, 64);
    int lightSteps = clamp(uCloudLightSteps, 1, 8);
    float stepLen = span / float(steps);

    float cosTheta = dot(dir, sunDir);
    // Two lobes: a strong forward one for the silver lining looking toward the
    // sun, and a weak backward one so the far side is not flat black.
    float phase = mix(cloudPhase(cosTheta, 0.72), cloudPhase(cosTheta, -0.25), 0.28);

    vec3 sunColor = mix(vec3(0.35, 0.40, 0.52), vec3(1.0, 0.96, 0.88), dayAmount);
    vec3 ambientColor = mix(vec3(0.05, 0.07, 0.12), vec3(0.55, 0.62, 0.74), dayAmount);
    // Storm clouds: less sun through, and the ambient goes flat grey.
    float dark = clamp(uCloudDarkness, 0.0, 1.0);
    sunColor *= mix(1.0, 0.18, dark);
    ambientColor = mix(ambientColor, vec3(0.16, 0.17, 0.19), dark);

    float transmittance = 1.0;
    vec3 scatter = vec3(0.0);

    for (int i = 0; i < 64; ++i) {
        if (i >= steps) break;
        float t = tEnter + (float(i) + 0.5) * stepLen;
        vec3 p = origin + dir * t;
        float heightFrac = clamp((p.y - bottom) / (top - bottom), 0.0, 1.0);
        float d = cloudDensityAt(p, heightFrac, 4);
        if (d <= 0.001) continue;

        // Light march: how much sun reaches this sample. Without it the cloud
        // has no interior shading at all and reads as a flat cut-out.
        float lightDensity = 0.0;
        float lightStep = (top - bottom) / float(lightSteps) * 0.6;
        for (int j = 0; j < 8; ++j) {
            if (j >= lightSteps) break;
            vec3 lp = p + sunDir * (float(j) + 0.5) * lightStep;
            float lh = clamp((lp.y - bottom) / (top - bottom), 0.0, 1.0);
            lightDensity += cloudDensityAt(lp, lh, 2) * lightStep;
        }
        float lightTransmittance = exp(-lightDensity * 0.0018);

        float extinction = d * stepLen * 0.0022;
        vec3 stepScatter = (sunColor * lightTransmittance * phase * 2.2 + ambientColor * 0.35) * d;
        // Energy-conserving integration: the analytic integral of scattering
        // over the step rather than a plain rectangle, which stops the result
        // depending on step count.
        scatter += transmittance * stepScatter * (1.0 - exp(-extinction));
        transmittance *= exp(-extinction);
        if (transmittance < 0.01) break;
    }

    // Fade toward the horizon so the far edge of a finite slab, and the point
    // where the noise starts visibly repeating, dissolve into haze rather than
    // ending. Uses |dir.y| so it works looking down from above the layer too.
    float horizonFade = smoothstep(0.0, max(uCloudHorizonFade, 0.001), abs(dir.y));
    return vec4(scatter, (1.0 - transmittance) * horizonFade);
}

float starLayer(vec2 uv, float density, float threshold, float sharpness) {
    vec2 grid = uv * density;
    vec2 cell = floor(grid);
    cell.x = mod(cell.x, density);
    vec2 local = fract(grid) - 0.5;
    float seed = hash(cell);
    vec2 starOffset = vec2(hash(cell + 13.17), hash(cell + 71.43)) - 0.5;
    float distToStar = length(local - starOffset * 0.72);
    float starCore = smoothstep(0.045, 0.0, distToStar);
    float visible = step(threshold, seed);
    float brightness = pow(seed, sharpness);
    return starCore * visible * brightness;
}

vec3 reconstructSkyDir() {
    vec2 viewport = max(uViewportSize, vec2(1.0));
    vec2 ndc = (gl_FragCoord.xy / viewport) * 2.0 - 1.0;
    vec4 viewRay = uInvProjection * vec4(ndc, 1.0, 1.0);
    float safeW = abs(viewRay.w) < 0.0001 ? 0.0001 : viewRay.w;
    vec3 viewDir = normalize(viewRay.xyz / safeW);
    return normalize(uInvViewRot * viewDir);
}

vec4 sampleBillboard(sampler2D tex, vec3 dir, vec3 centerDir, float angularRadius) {
    vec3 basisUp = abs(centerDir.y) > 0.96 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(basisUp, centerDir));
    vec3 up = normalize(cross(centerDir, right));

    float forward = dot(dir, centerDir);
    if (forward <= 0.0) {
        return vec4(0.0);
    }

    vec2 plane = vec2(dot(dir, right), dot(dir, up)) / max(forward, 0.001);
    vec2 uv = plane / tan(angularRadius) * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return vec4(0.0);
    }

    return texture(tex, uv);
}

vec3 proceduralSky(vec3 dir, vec3 sunDir, float dayAmount) {
    float height = clamp(dir.y, -1.0, 1.0);
    float horizonMix = smoothstep(-0.18, 0.72, height);

    vec3 dayHorizon = vec3(0.84, 0.92, 1.02);
    vec3 dayUpper = vec3(0.18, 0.48, 0.88);
    vec3 twilightHorizon = vec3(1.02, 0.62, 0.34);
    vec3 twilightUpper = vec3(0.15, 0.22, 0.45);
    vec3 nightHorizon = vec3(0.03, 0.05, 0.10);
    vec3 nightUpper = vec3(0.005, 0.015, 0.045);

    float twilight = 1.0 - smoothstep(0.08, 0.38, abs(sunDir.y));
    vec3 horizonCol = mix(nightHorizon, twilightHorizon, twilight);
    horizonCol = mix(horizonCol, dayHorizon, dayAmount);
    vec3 upperCol = mix(nightUpper, twilightUpper, twilight);
    upperCol = mix(upperCol, dayUpper, dayAmount);

    vec3 skyColor = mix(horizonCol, upperCol, horizonMix);

    float sunDot = max(dot(dir, sunDir), 0.0);
    float mieGlow = pow(sunDot, 9.0);
    float aureole = pow(sunDot, 40.0);
    vec3 scatterColor = mix(vec3(1.05, 0.62, 0.30), vec3(1.0, 0.97, 0.92), dayAmount);
    skyColor += scatterColor * (mieGlow * 0.22 + aureole * 0.55) * mix(0.35, 1.0, dayAmount);

    float haze = exp(-max(height, -0.1) * 9.0);
    skyColor += mix(vec3(0.06, 0.08, 0.12), vec3(1.0, 0.93, 0.86), dayAmount) * haze * mix(0.05, 0.18, dayAmount);

    if (uCloudMode == 1) {
        vec4 clouds = marchClouds(dir, sunDir, dayAmount);
        skyColor = mix(skyColor, clouds.rgb, clamp(clouds.a, 0.0, 1.0));
    }

    // cloudBand is exactly 0 below the horizon band and at the zenith cap, so
    // those pixels skip the two 5-octave FBM evaluations entirely.
    float cloudBand = (uCloudMode == 0)
        ? smoothstep(-0.12, 0.16, height) * (1.0 - smoothstep(0.68, 0.92, height))
        : 0.0;
    if (cloudBand > 0.0) {
        float cloudYaw = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
        vec2 windA = vec2(uSkyTime * 0.012, uSkyTime * 0.004);
        vec2 windB = vec2(-uSkyTime * 0.006, uSkyTime * 0.009);
        vec2 cloudUv = vec2(cloudYaw * 6.0, height * 3.4 + 1.25);
        float broadClouds = fbmPeriodic(cloudUv + windA, vec2(6.0, 1024.0));
        float detailClouds = fbmPeriodic(cloudUv * 2.0 + windB, vec2(12.0, 2048.0));
        float cloudShape = broadClouds * 0.74 + detailClouds * 0.26;
        float cloudMask = smoothstep(0.50, 0.72, cloudShape) * cloudBand;
        float cloudSilver = pow(max(dot(dir, sunDir), 0.0), 5.0) * dayAmount;
        vec3 cloudShadow = mix(vec3(0.08, 0.10, 0.15), vec3(0.58, 0.65, 0.72), dayAmount);
        vec3 cloudBright = mix(vec3(0.20, 0.23, 0.30), vec3(1.0, 0.98, 0.93), dayAmount);
        vec3 cloudLit = mix(cloudShadow, cloudBright, 0.55 + cloudSilver * 0.45);
        float cloudAlpha = cloudMask * mix(0.08, 0.62, dayAmount);
        skyColor = mix(skyColor, cloudLit, cloudAlpha);
        skyColor += vec3(1.0, 0.92, 0.76) * cloudSilver * cloudMask * 0.12;
    }

    float nightAmount = 1.0 - dayAmount;
    if (nightAmount > 0.001) {
        vec2 starUv = vec2(atan(dir.z, dir.x) / (2.0 * PI) + 0.5, asin(height) / PI + 0.5);
        float fineStars = starLayer(starUv, 190.0, 0.953, 2.9);
        float brightStars = starLayer(starUv + vec2(0.37, 0.19), 92.0, 0.974, 2.1);
        float twinkle = 0.82 + 0.18 * sin(uSkyTime * 1.7 + hash(floor(starUv * 145.0)) * 31.4);
        float starField = (fineStars * 0.78 + brightStars * 1.65) * twinkle;
        float starMask = smoothstep(-0.04, 0.28, height) * (1.0 - smoothstep(0.88, 1.0, height) * 0.25);
        vec3 galaxy = vec3(0.30, 0.38, 0.55) * pow(max(1.0 - abs(dot(dir, normalize(vec3(0.18, 0.88, -0.42)))), 0.0), 5.0);
        skyColor += (vec3(0.92, 0.94, 1.0) * starField * 1.8 + galaxy * 0.45) * nightAmount * starMask;
    }

    return skyColor;
}

vec3 scrollingSky(vec3 dir) {
    float yaw = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float vertical = asin(clamp(dir.y, -0.96, 0.96)) / PI + 0.5;
    float horizonLift = mix(0.0, uCameraAngles.y * 0.18, uScrollVerticalInfluence);
    float wind = uSkyTime * 0.018;
    vec2 uv = vec2((yaw + uCameraAngles.x * uScrollLookSensitivity * 0.35 + wind) * uScrollRepeat.x,
                   (vertical + horizonLift + sin(yaw * 6.28318 + uSkyTime * 0.22) * 0.012) * uScrollRepeat.y);
    vec3 cloudColor = texture(uScrollTex, uv).rgb;

    vec2 highUv = vec2(uv.x + uSkyTime * 0.006, (0.82 + horizonLift) * uScrollRepeat.y);
    vec3 zenithColor = texture(uScrollTex, highUv).rgb;
    float zenithFade = smoothstep(0.70, 0.92, dir.y);
    float nadirFade = smoothstep(-0.72, -0.92, dir.y);
    vec3 bottomColor = texture(uScrollTex, vec2(uv.x, (0.08 + horizonLift) * uScrollRepeat.y)).rgb;
    return mix(mix(cloudColor, zenithColor, zenithFade), bottomColor, nadirFade);
}

void main() {
    vec3 dir = reconstructSkyDir();

    // Cached-sky path: the CPU bakes the procedural sky into a cubemap a face
    // at a time, so the fullscreen draw is a single fetch instead of the full
    // FBM/star evaluation below.
    if (uUseBakedSky) {
        FragColor = vec4(texture(uBakedSkyCube, dir).rgb, 1.0);
        return;
    }

    float t = fract(timeOfDay);
    float angle = t * 2.0 * PI;
    vec3 sunDir = uUseLightSunDirection
        ? normalize(uLightSunDirection)
        : normalize(vec3(cos(angle), sin(angle) * 0.55, sin(angle)));
    vec3 moonDir = -sunDir;
    float dayAmount = smoothstep(-0.12, 0.16, sunDir.y);
    float nightAmount = 1.0 - dayAmount;

    if (uSkyMode == 1 && uHasScrollTexture) {
        FragColor = vec4(scrollingSky(dir), 1.0);
        return;
    }

    vec3 col = proceduralSky(dir, sunDir, dayAmount);

    float sunDot = max(dot(dir, sunDir), 0.0);
    float moonDot = max(dot(dir, moonDir), 0.0);

    // The sprite contributions are multiplied by dayAmount/nightAmount below,
    // so skip the billboard projection+fetch when the factor is exactly 0.
    vec4 sunSprite = (dayAmount > 0.0) ? sampleBillboard(uSunTex, dir, sunDir, 0.11) : vec4(0.0);
    vec4 moonSprite = (nightAmount > 0.0) ? sampleBillboard(uMoonTex, dir, moonDir, 0.075) : vec4(0.0);

    vec3 sunHalo = vec3(1.0, 0.97, 0.9) * (pow(sunDot, 18.0) * 0.45 + pow(sunDot, 220.0) * 4.0);
    vec3 moonHalo = vec3(0.75, 0.82, 1.0) * (pow(moonDot, 20.0) * 0.16);

    col += sunHalo * dayAmount;
    col = mix(col, moonHalo + col, nightAmount * 0.55);
    col = mix(col, col + sunSprite.rgb * (0.35 + sunSprite.a * 1.65), sunSprite.a * dayAmount);
    col = mix(col, col + moonSprite.rgb * (0.25 + moonSprite.a * 1.15), moonSprite.a * nightAmount);

    col = 1.0 - exp(-col * 1.15);
    FragColor = vec4(col, 1.0);
}
