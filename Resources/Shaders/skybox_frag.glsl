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
// -- Physical atmosphere (uAtmosphereModel 1) --
//
// 0 is the original hand-tuned gradient sky, kept so a scene can opt back to
// it. 1 integrates single Rayleigh + Mie scattering (plus an ozone term folded
// into the Rayleigh density) along the view ray, with the optical depth toward
// the sun from a Chapman-function approximation instead of a nested march, so
// the whole thing is 12 cheap steps per pixel. The CPU half of this lives in
// Skybox.cpp (EvaluatePhysicalSkyDisplay) and must stay in step with it: it is
// what the scene fog colour is matched against.
uniform int uAtmosphereModel = 1;
uniform float uTurbidity = 2.5;
// Angular diameter of the sun disk in degrees.
uniform float uSunSize = 0.8;
uniform float uSunIntensity = 1.0;
// The disk is drawn live on top of the baked cube rather than into it: at a
// 256 cube face it would be a three-texel smear, and leaving it out keeps the
// cube usable as an environment without a second copy of the sun in it.
uniform bool uDrawSunDisk = true;

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
// sunColor/ambientColor are the light reaching the slab; the legacy sky derives
// them from dayAmount (marchClouds below), the physical sky from the atmosphere.
vec4 marchCloudsLit(vec3 dir, vec3 sunDir, vec3 sunColor, vec3 ambientColor) {
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

    // Float clamps: GLSL ES 1.00 has no integer clamp().
#ifdef MODU_ES2
    // The full march is 64 x 8 noise evaluations per pixel; a GLES 2 GPU gets a
    // fraction of that budget, and the cached sky bake hides most of the loss.
    int steps = int(clamp(float(uCloudSteps), 4.0, 12.0));
    int lightSteps = int(clamp(float(uCloudLightSteps), 1.0, 2.0));
#else
    int steps = int(clamp(float(uCloudSteps), 4.0, 64.0));
    int lightSteps = int(clamp(float(uCloudLightSteps), 1.0, 8.0));
#endif
    float stepLen = span / float(steps);

    float cosTheta = dot(dir, sunDir);
    // Two lobes: a strong forward one for the silver lining looking toward the
    // sun, and a weak backward one so the far side is not flat black.
    float phase = mix(cloudPhase(cosTheta, 0.72), cloudPhase(cosTheta, -0.25), 0.28);

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

vec4 marchClouds(vec3 dir, vec3 sunDir, float dayAmount) {
    vec3 sunColor = mix(vec3(0.35, 0.40, 0.52), vec3(1.0, 0.96, 0.88), dayAmount);
    vec3 ambientColor = mix(vec3(0.05, 0.07, 0.12), vec3(0.55, 0.62, 0.74), dayAmount);
    return marchCloudsLit(dir, sunDir, sunColor, ambientColor);
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


// ---------------------------------------------------------------------------
// Physical atmosphere. Units are kilometres; exposures are tuned so the output
// lands in the same display range the legacy sky did.
// ---------------------------------------------------------------------------
const float ATM_R = 6360.0;        // ground radius
const float ATM_H = 100.0;         // atmosphere thickness
const float ATM_HR = 8.0;          // Rayleigh scale height
const float ATM_HM = 1.2;          // Mie scale height
const float ATM_VIEW_ALT = 0.5;    // virtual observer altitude: puts a hazy dip below the horizon
const vec3 ATM_BETA_R = vec3(5.802e-3, 13.558e-3, 33.1e-3);
// Ozone absorption, folded into the Rayleigh density (layer column ~15 km vs
// Rayleigh's 8). It is what keeps the zenith blue rather than grey at dusk.
const vec3 ATM_BETA_OZ = vec3(1.83e-3, 5.30e-3, 0.24e-3);
const float ATM_BETA_M = 3.996e-3;
const float ATM_SUN_E = 20.0;      // solar irradiance scale
const float ATM_EXPOSURE = 1.3;
// Linear saturation lift before the tonemap; single scattering alone renders
// a paler zenith than a camera does.
const float ATM_SATURATION = 1.3;
const float ATM_MIE_G = 0.86;

float atmMieScale() {
    // Turbidity 2.5 is the reference clear-day haze; 1 is nearly pure Rayleigh.
    return clamp((uTurbidity - 1.0) / 1.5, 0.05, 8.0) * 2.0;
}

// Column density (in scale-height-weighted km) from radius r along a ray with
// cosine mu to the zenith, out to space. Schuler's Chapman approximation,
// evaluated with base-2 heights; within ~4% of the exact integral.
float atmChapman(float r, float mu, float H) {
    float H50 = H * 0.693147;
    float c = sqrt(r / H50);
    float h50 = (r - ATM_R) / H50;
    if (mu >= 0.0) {
        return H * c / (c * mu + 1.0) * exp2(-h50);
    }
    float rMin = max(r * sqrt(max(1.0 - mu * mu, 0.0)), ATM_R);
    float x0 = rMin / H50;
    float c0 = sqrt(x0);
    return H * (2.0 * c0 * exp2((ATM_R - rMin) / H50) - c / (1.0 - c * mu) * exp2(-h50));
}

// How much of the sun is above the planet's limb as seen from radius r. A
// hard cut here draws the earth's shadow as a razor line across the dusk sky.
float atmSunVisibility(float r, float mu) {
    if (mu >= 0.0) return 1.0;
    float rMin = r * sqrt(max(1.0 - mu * mu, 0.0));
    return smoothstep(ATM_R - 8.0, ATM_R + 1.0, rMin);
}

vec3 atmTransmittanceToSun(float r, float mu) {
    float mieE = ATM_BETA_M * atmMieScale() / 0.9;
    vec3 tau = (ATM_BETA_R + ATM_BETA_OZ) * atmChapman(r, mu, ATM_HR) +
               vec3(mieE) * atmChapman(r, mu, ATM_HM);
    return exp(-tau) * atmSunVisibility(r, mu);
}

// Linear radiance of the clear sky (or hazy ground below the horizon) toward
// dir. viewT receives the transmittance along the view ray.
vec3 atmosphereRadiance(vec3 dir, vec3 sunDir, out vec3 viewT) {
    float r0 = ATM_R + ATM_VIEW_ALT;
    float mu = dir.y;
    float b = r0 * mu;
    // r0^2 - Rtop^2 and r0^2 - R^2, factored so nothing subtracts two 4e7s.
    float cTop = (ATM_VIEW_ALT - ATM_H) * (2.0 * ATM_R + ATM_VIEW_ALT + ATM_H);
    float cGround = ATM_VIEW_ALT * (2.0 * ATM_R + ATM_VIEW_ALT);
    float tMax = -b + sqrt(max(b * b - cTop, 0.0));
    float discG = b * b - cGround;
    bool hitsGround = mu < 0.0 && discG > 0.0;
    if (hitsGround) tMax = -b - sqrt(discG);

    float mieScale = atmMieScale();
    vec3 betaRE = ATM_BETA_R + ATM_BETA_OZ;
    float betaMS = ATM_BETA_M * mieScale;
    float betaME = betaMS / 0.9;

    float nu = dot(dir, sunDir);
    float phaseR = 3.0 / (16.0 * PI) * (1.0 + nu * nu);
    float g = ATM_MIE_G;
    float g2 = g * g;
    float phaseM = 3.0 / (8.0 * PI) * ((1.0 - g2) * (1.0 + nu * nu)) /
                   ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * nu, 1e-4), 1.5));

    vec3 sumR = vec3(0.0);
    vec3 sumM = vec3(0.0);
    float odR = 0.0;
    float odM = 0.0;
    // Quadratic step distribution: the density that matters is all near the
    // observer, and a horizon ray is over a thousand kilometres long.
    for (int i = 0; i < 12; ++i) {
        float f0 = float(i) / 12.0;
        float f1 = float(i + 1) / 12.0;
        float t0 = tMax * f0 * f0;
        float t1 = tMax * f1 * f1;
        float ds = t1 - t0;
        float t = 0.5 * (t0 + t1);
        float r = sqrt(max(r0 * r0 + t * t + 2.0 * r0 * t * mu, ATM_R * ATM_R));
        float alt = r - ATM_R;
        float rhoR = exp(-alt / ATM_HR);
        float rhoM = exp(-alt / ATM_HM);
        odR += rhoR * ds * 0.5;
        odM += rhoM * ds * 0.5;
        float muS = clamp((r0 * sunDir.y + t * nu) / r, -1.0, 1.0);
        vec3 tau = betaRE * (odR + atmChapman(r, muS, ATM_HR)) +
                   vec3(betaME) * (odM + atmChapman(r, muS, ATM_HM));
        vec3 T = exp(-tau) * atmSunVisibility(r, muS);
        sumR += T * rhoR * ds;
        sumM += T * rhoM * ds;
        odR += rhoR * ds * 0.5;
        odM += rhoM * ds * 0.5;
    }
    viewT = exp(-(betaRE * odR + vec3(betaME) * odM));

    float E = ATM_SUN_E * max(uSunIntensity, 0.0);
    // The small isotropic terms stand in for multiple scattering, which single
    // scattering lacks and which is most of what lights the sky opposite the
    // sun and the blue hour after sunset.
    vec3 L = E * (ATM_BETA_R * sumR * (phaseR + 0.012) + betaMS * sumM * (phaseM + 0.01));

    if (hitsGround) {
        float muG = clamp((r0 * sunDir.y + tMax * nu) / ATM_R, -1.0, 1.0);
        vec3 sunAtGround = atmTransmittanceToSun(ATM_R, muG);
        vec3 skyAmbient = vec3(0.34, 0.45, 0.62) * 0.05 * smoothstep(-0.15, 0.25, sunDir.y);
        vec3 groundAlbedo = vec3(0.056, 0.052, 0.049);
        L += viewT * E * groundAlbedo * (sunAtGround * max(muG, 0.0) / PI + skyAmbient);
    }
    return L;
}

vec3 atmNightRadiance(vec3 dir) {
    // Display targets, linearised: a dark blue night with a slightly lifted
    // horizon (airglow + light pollution) and a darker ground.
    vec3 zenith = pow(vec3(0.020, 0.032, 0.080), vec3(2.2));
    vec3 horizon = pow(vec3(0.055, 0.068, 0.115), vec3(2.2));
    vec3 col = mix(horizon, zenith, smoothstep(-0.02, 0.55, dir.y));
    if (dir.y < 0.0) col = horizon * mix(1.0, 0.4, smoothstep(0.0, -0.2, dir.y));
    return col / ATM_EXPOSURE;
}

// Blue hour. Single scattering leaves the sky grey once the sun is just
// below the horizon; the real blue comes from ozone along the long grazing
// path and from multiple scattering, which this stands in for.
vec3 atmBlueHourRadiance(vec3 dir, vec3 sunDir) {
    float amount = smoothstep(-0.22, -0.03, sunDir.y) * (1.0 - smoothstep(0.0, 0.18, sunDir.y));
    vec3 col = pow(vec3(0.05, 0.11, 0.30), vec3(2.2)) * 0.8;
    float shape = (0.55 + 0.45 * smoothstep(-0.05, 0.6, dir.y)) * (dir.y < 0.0 ? 0.4 : 1.0);
    return col * shape * amount;
}

float atmNightAmount(vec3 sunDir) {
    return smoothstep(0.03, -0.16, sunDir.y);
}

// Exposure, saturation lift, then a tonemap that is part per-channel and part
// luminance-only: per-channel alone bleaches a low sun's glow to white, the
// luminance curve keeps its orange.
vec3 atmToDisplay(vec3 lin) {
    vec3 x = max(lin, vec3(0.0)) * ATM_EXPOSURE;
    const vec3 lumaW = vec3(0.2126, 0.7152, 0.0722);
    float lum0 = dot(x, lumaW);
    x = max(vec3(lum0) + (x - vec3(lum0)) * ATM_SATURATION, vec3(0.0));
    vec3 perChannel = 1.0 - exp(-x);
    float lum = dot(x, lumaW);
    vec3 huePreserving = x * ((1.0 - exp(-lum)) / max(lum, 1e-5));
    vec3 c = mix(perChannel, huePreserving, 0.6);
    float m = max(c.r, max(c.g, c.b));
    if (m > 1.0) c = mix(c / m, vec3(1.0), clamp((m - 1.0) / m, 0.0, 1.0));
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

float cloudNoiseFbm(vec2 p, int octaves) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 6; ++i) {
        if (i >= octaves) break;
        v += a * noise(p);
        // Rotate between octaves so the value-noise grid never lines up.
        p = vec2(1.6 * p.x - 1.2 * p.y, 1.2 * p.x + 1.6 * p.y) + vec2(11.3, 4.7);
        a *= 0.5;
    }
    return v;
}

// Normalised so an isotropic medium is 1.
float hgNorm(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
}

// Flat cloud layer for the physical sky: a domain-warped cumulus sheet
// projected onto a plane overhead, lit by offsetting the density lookup toward
// the sun (thick cores go dark underneath, thin edges catch a forward-scatter
// silver lining), with a faint high cirrus layer behind it.
vec4 flatCloudsPhysical(vec3 dir, vec3 sunDir, vec3 sunColor, vec3 ambientColor) {
    float fade = smoothstep(0.0, max(uCloudHorizonFade, 0.001), dir.y);
    if (fade <= 0.0) return vec4(0.0);

    float cov = clamp(uCloudCoverage, 0.0, 1.0);
    float dark = clamp(uCloudDarkness, 0.0, 1.0);
    vec2 plane = dir.xz / (dir.y + 0.12);
    vec2 wind = vec2(uSkyTime * 0.012, uSkyTime * 0.005) * uCloudWind;
    float cosT = dot(dir, sunDir);
    float phase = mix(hgNorm(cosT, 0.55), hgNorm(cosT, -0.15), 0.35);
    float silverPhase = hgNorm(cosT, 0.88);

    // -- cirrus: high, streaked, thin --
    vec2 cp = plane * 0.45 + wind * 0.35;
    cp = vec2(cp.x * 0.55 + cp.y * 0.25, cp.y * 2.2 - cp.x * 0.4);
    float ci = cloudNoiseFbm(cp + vec2(cloudNoiseFbm(cp * 0.5, 2) * 1.5, 0.0), 4);
    float ciAlpha = smoothstep(0.52, 0.80, ci) * 0.38 * fade * (1.0 - cov * 0.6);
    vec3 ciColor = sunColor * (0.55 + 0.35 * phase) + ambientColor * 0.75;

    // -- cumulus sheet --
    vec2 p = plane * 1.15 + wind;
    vec2 warp = vec2(cloudNoiseFbm(p * 0.6 + vec2(3.1, 7.7), 3),
                     cloudNoiseFbm(p * 0.6 + vec2(9.4, 1.3), 3)) - 0.5;
    vec2 pw = p + warp * 1.35;
    float shape = cloudNoiseFbm(pw, 5);
    float threshold = mix(0.70, 0.30, cov);
    float dens = smoothstep(threshold, threshold + 0.24, shape);

    vec3 col = ciColor;
    float alpha = ciAlpha;
    if (dens > 0.0) {
        vec2 toSun = sunDir.xz;
        float toSunLen = length(toSun);
        toSun = toSunLen > 1e-4 ? toSun / toSunLen : vec2(0.0);
        vec2 lightStep = toSun * (0.10 + 0.12 * (1.0 - clamp(sunDir.y, 0.0, 1.0)));
        float densL = smoothstep(threshold, threshold + 0.24, cloudNoiseFbm(pw + lightStep, 4));
        float lightT = exp(-(densL * 1.7 + dens * 0.5));
        float edge = 1.0 - dens;

        vec3 body = ambientColor * mix(1.10, 0.62, dens);
        body += sunColor * (lightT * (0.45 + 0.55 * phase) + silverPhase * edge * edge * 0.10);
        // Storm decks: less sun through, and the base goes slate grey.
        body = mix(body, vec3(dot(body, vec3(0.2126, 0.7152, 0.0722))) * 0.55, dark * 0.7);

        float a = dens * fade * 0.97;
        col = mix(col * alpha, body, a);
        alpha = a + alpha * (1.0 - a);
        col = alpha > 1e-4 ? col / alpha : body;
    }
    return vec4(col, alpha);
}

vec3 addSunDisk(vec3 col, vec3 dir, vec3 sunDir, float visibility) {
    if (visibility <= 0.001) return col;
    float d = dot(dir, sunDir);
    float radius = radians(clamp(uSunSize, 0.05, 20.0) * 0.5);
    if (d < cos(radius * 1.2)) return col;
    float ang = asin(clamp(length(cross(dir, sunDir)), 0.0, 1.0));
    float x = ang / radius;
    float mask = 1.0 - smoothstep(0.92, 1.08, x);
    if (mask <= 0.0) return col;
    float muL = sqrt(max(1.0 - min(x, 1.0) * min(x, 1.0), 0.0));
    // Limb darkening, steeper in blue: the edge of the real disk is redder and
    // dimmer than its centre.
    vec3 limb = pow(vec3(max(muL, 0.02)), vec3(0.40, 0.50, 0.65));
    vec3 sunT = atmTransmittanceToSun(ATM_R + ATM_VIEW_ALT, sunDir.y);
    // Hue from the transmittance, applied in display space with the brightness
    // held up, so a setting sun reads as an orange disk instead of the tonemap
    // flattening it to white. Above 1 on purpose: an HDR target blooms it.
    vec3 hue = sunT / max(max(sunT.r, max(sunT.g, sunT.b)), 1e-4);
    vec3 disk = pow(hue, vec3(0.55)) * limb * (1.0 + 1.5 * min(max(uSunIntensity, 0.0), 2.0));
    float horizon = smoothstep(-0.01, 0.005, sunDir.y);
    return mix(col, max(col, disk), mask * visibility * horizon);
}

// Full physical sky. rgb is display-space colour; a is how much the clouds and
// overcast hide the sun at this pixel, which the baked cube keeps so the live
// sun disk can be occluded by baked clouds.
vec4 physicalSky(vec3 dir, vec3 sunDir, bool drawDisk) {
    float nightAmount = atmNightAmount(sunDir);
    vec3 viewT;
    vec3 lin = atmosphereRadiance(dir, sunDir, viewT);
    lin += atmNightRadiance(dir) * nightAmount + atmBlueHourRadiance(dir, sunDir);

    float cov = clamp(uCloudCoverage, 0.0, 1.0);
    float dark = clamp(uCloudDarkness, 0.0, 1.0);
    float overcast = smoothstep(0.55, 0.95, cov);

    float cloudA = 0.0;
    if (dir.y > 0.0) {
        vec3 sunAtClouds = atmTransmittanceToSun(ATM_R + 2.0, sunDir.y);
        float E = ATM_SUN_E * max(uSunIntensity, 0.0);
        vec3 sunColor = sunAtClouds * E * 0.11 * mix(1.0, 0.25, dark);
        float lum = dot(lin, vec3(0.2126, 0.7152, 0.0722));
        vec3 ambient = mix(lin, vec3(lum), 0.35) * 1.15 +
                       vec3(0.0025, 0.003, 0.0045) * nightAmount;
        ambient = mix(ambient, vec3(lum) * vec3(0.80, 0.83, 0.88), dark * 0.6);
        vec4 clouds = vec4(0.0);
        if (uCloudMode == 1) {
            clouds = marchCloudsLit(dir, sunDir, sunColor * 1.4, ambient * 1.6);
        } else {
            clouds = flatCloudsPhysical(dir, sunDir, sunColor, ambient);
        }
        cloudA = clamp(clouds.a, 0.0, 1.0);
        lin = mix(lin, clouds.rgb, cloudA);
    }

    // Overcast veil: the sky behind a full deck is a dim, near-neutral grey
    // whatever the clear sky would have been, brightest toward the sun.
    float lumAll = dot(lin, vec3(0.2126, 0.7152, 0.0722));
    vec3 grey = vec3(lumAll) * vec3(0.96, 0.99, 1.04) * mix(0.95, 0.38, dark);
    lin = mix(lin, grey, overcast * 0.82);
    float occlusion = clamp(max(cloudA, overcast * 0.95), 0.0, 1.0);

    vec3 col = atmToDisplay(lin);

    if (nightAmount > 0.001) {
        vec3 moonDir = -sunDir;
        float height = clamp(dir.y, -1.0, 1.0);
        vec2 starUv = vec2(atan(dir.z, dir.x) / (2.0 * PI) + 0.5, asin(height) / PI + 0.5);
        float fineStars = starLayer(starUv, 190.0, 0.953, 2.9);
        float brightStars = starLayer(starUv + vec2(0.37, 0.19), 92.0, 0.974, 2.1);
        float twinkle = 0.82 + 0.18 * sin(uSkyTime * 1.7 + hash(floor(starUv * 145.0)) * 31.4);
        float starField = (fineStars * 0.78 + brightStars * 1.65) * twinkle;
        float starMask = smoothstep(-0.02, 0.25, height);
        vec3 galaxy = vec3(0.30, 0.38, 0.55) * pow(max(1.0 - abs(dot(dir, normalize(vec3(0.18, 0.88, -0.42)))), 0.0), 5.0);
        vec3 stars = (vec3(0.92, 0.94, 1.0) * starField * 1.6 + galaxy * 0.30) * starMask;

        float moonDot = max(dot(dir, moonDir), 0.0);
        vec3 moonHalo = vec3(0.62, 0.70, 0.90) * (pow(moonDot, 24.0) * 0.10 + pow(moonDot, 400.0) * 0.25);
        vec4 moonSprite = sampleBillboard(uMoonTex, dir, moonDir, 0.06);
        float clear = 1.0 - occlusion;
        col += (stars * clear + moonHalo * mix(clear, 0.6, overcast)) * nightAmount;
        col = mix(col, col + moonSprite.rgb * (0.25 + moonSprite.a * 1.1), moonSprite.a * nightAmount * clear);
    }

    if (drawDisk) {
        col = addSunDisk(col, dir, sunDir, 1.0 - occlusion);
    }
    return vec4(col, occlusion);
}

vec3 scrollingSky() {
    vec2 uv = gl_FragCoord.xy / max(uViewportSize, vec2(1.0));
    uv += vec2(uCameraAngles.x * uScrollLookSensitivity,
               uCameraAngles.y * uScrollVerticalInfluence / PI);
    return texture(uScrollTex, uv * uScrollRepeat).rgb;
}

void main() {
    vec3 dir = reconstructSkyDir();

    // Cached-sky path: the CPU bakes the procedural sky into a cubemap a face
    // at a time, so the fullscreen draw is a single fetch instead of the full
    // FBM/star evaluation below.
    if (uUseBakedSky) {
        vec4 baked = texture(uBakedSkyCube, dir);
        vec3 bakedCol = baked.rgb;
        if (uAtmosphereModel == 1 && uDrawSunDisk) {
            bakedCol = addSunDisk(bakedCol, dir, normalize(uLightSunDirection), 1.0 - clamp(baked.a, 0.0, 1.0));
        }
        FragColor = vec4(bakedCol, 1.0);
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
        FragColor = vec4(scrollingSky(), 1.0);
        return;
    }

    if (uAtmosphereModel == 1) {
        // The CPU resolves the sun (light-follow or the timeOfDay arc) for
        // this model, so uLightSunDirection is always the one to use.
        vec4 phys = physicalSky(dir, normalize(uLightSunDirection), uDrawSunDisk);
        // While baking, alpha carries the sun occlusion into the cube; the
        // live draw is opaque.
        FragColor = vec4(phys.rgb, uDrawSunDisk ? 1.0 : phys.a);
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
