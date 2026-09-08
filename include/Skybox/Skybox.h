#ifndef SKYBOX_H
#define SKYBOX_H

#include <string>
#include "ThirdParty/glm/glm.hpp"

class Shader;
class Texture;

enum class SkyboxMode {
    Procedural = 0,
    Scrolling = 1
};

// Built-in weather. Drives the cloud layer, the scene's ambient light, a rain
// overlay and a looping ambience track from one control, so a scene can go
// overcast without hand-animating five separate settings.
//
// Off by default, and when off it writes to none of them - a scene that has
// never touched weather keeps whatever cloud coverage and ambience it was
// authored with.
enum class WeatherMode {
    Clear = 0,
    Overcast = 1,
    DarkCloudy = 2,
    Rain = 3,
    Thunderstorm = 4
};

inline const char* ToString(WeatherMode mode) {
    switch (mode) {
        case WeatherMode::Overcast:     return "Overcast";
        case WeatherMode::DarkCloudy:   return "DarkCloudy";
        case WeatherMode::Rain:         return "Rain";
        case WeatherMode::Thunderstorm: return "Thunderstorm";
        case WeatherMode::Clear:
        default:                        return "Clear";
    }
}

struct WeatherSettings {
    bool enabled = false;
    WeatherMode mode = WeatherMode::Clear;
    // Scales how far the mode pushes everything, so "light rain" and "downpour"
    // are the same mode at different strengths rather than two modes.
    float intensity = 1.0f;
    float windSpeed = 1.0f;

    // -- Rain --
    float rainOpacity = 0.55f;
    float rainSpeed = 1.6f;
    // Screen-space streaks rather than particles: rain has to cover the whole
    // view at all times, which is the one case where a full-screen pass is
    // cheaper than any particle count that would look convincing - and it
    // costs the same on a Quest as on a desktop.
    float rainScale = 1.0f;

    // -- Lightning --
    // Average seconds between strikes; the actual gap is randomised around it
    // so a storm does not tick like a metronome.
    float lightningInterval = 11.0f;
    float lightningIntensity = 2.5f;

    // -- Ambience --
    // One looping bed per condition, picked by mode. Thunder is fired as a
    // one-shot on each strike instead, so it lines up with the flash.
    std::string clearSoundPath;
    std::string overcastSoundPath;
    std::string rainSoundPath;
    std::string heavyRainSoundPath;
    std::string thunderSoundPath;
    float ambienceVolume = 0.7f;
    // Above this intensity a Rain/Thunderstorm scene uses the heavy loop.
    float heavyRainThreshold = 0.65f;
};

// Mutable per-frame weather state. Lives outside WeatherSettings because it is
// runtime, not authored - nothing here is serialized.
struct WeatherRuntime {
    float timeToNextStrike = -1.0f;  // negative = not scheduled yet
    float flash = 0.0f;              // current lightning brightness, decaying
    uint32_t rngState = 0x9E3779B9u;
};

// What the weather resolves to this frame.
struct WeatherDrive {
    bool active = false;
    float cloudCoverage = 0.45f;
    float cloudDarkness = 0.0f;
    float rainAmount = 0.0f;       // 0..1
    float flash = 0.0f;            // additive scene brightness from lightning
    bool thunderTriggered = false; // true only on the frame a strike fires
    // Owned, not a pointer into WeatherSettings: this struct is copied and
    // stored by both the renderer and the skybox, and reassigning the settings
    // would leave those copies pointing at freed strings.
    std::string ambiencePath;
};

// Resolves settings + elapsed time into this frame's weather. Split out and
// kept free of engine types so the mode table, the intensity blend and the
// strike scheduling can be tested without a renderer or an audio device.
WeatherDrive EvaluateWeather(const WeatherSettings& weather, WeatherRuntime& runtime,
                             float deltaSeconds);

struct SkyboxSettings {
    SkyboxMode mode = SkyboxMode::Procedural;
    // Render the procedural sky into a cached cubemap (refreshed a face at a
    // time on a short interval) instead of evaluating it per screen pixel.
    bool cachedSky = true;
    std::string sunTexturePath = "Resources/Engine-Root/Skybox/Sun Skybox.png";
    std::string moonTexturePath = "Resources/Engine-Root/Skybox/Moon Skybox.png";
    std::string scrollingTexturePath;
    float scrollingRepeatX = 2.0f;
    float scrollingRepeatY = 1.0f;
    float scrollingLookSensitivity = 1.0f;
    float scrollingVerticalInfluence = 0.18f;
    bool environmentReflections = false;
    float environmentReflectionIntensity = 0.5f;
    float reflectionDistanceFadeStart = 4.0f;
    float reflectionDistanceFadeEnd = 24.0f;
    bool fogEnabled = false;
    int fogMode = 0; // 0 linear, 1 exponential, 2 exponential squared
    float fogStart = 20.0f;
    float fogEnd = 120.0f;
    float fogDensity = 0.015f;
    float fogHeight = 0.0f;
    float fogHeightFalloff = 0.0f;
    glm::vec3 fogColor = glm::vec3(0.65f, 0.72f, 0.78f);
    // Place the sun where the scene's directional light points from, instead of
    // deriving it from timeOfDay. Off by default: existing scenes drive the sky
    // with timeOfDay and would otherwise jump the moment they are opened.
    bool sunFollowsDirectionalLight = false;


    // -- Volumetric clouds --
    //
    // cloudMode 0 keeps the flat FBM band the procedural sky has always drawn.
    // 1 ray-marches a cloud slab with a light march toward the sun, which is
    // far more expensive per pixel - affordable because cachedSky bakes the sky
    // into a cubemap a face at a time. With caching off it is paid per screen
    // pixel per frame, so treat mode 1 and cachedSky as a pair.
    //
    // Default stays 0 so existing scenes open looking exactly as they did.
    int cloudMode = 0;
    float cloudCoverage = 0.45f;
    float cloudDensity = 1.0f;
    int cloudSteps = 24;
    int cloudLightSteps = 4;
    float cloudBottom = 900.0f;
    float cloudTop = 2400.0f;
    float cloudWind = 1.0f;
    float cloudDarkness = 0.0f;
    // How far clouds fade as the view nears the horizon. Hides both the finite
    // edge of the slab and the point where the noise starts repeating.
    float cloudHorizonFade = 0.16f;
    // Cube face resolution for the baked sky. The volumetric cloud march runs
    // per baked pixel, so this is the single biggest cost lever: 1024 is 16x
    // the work of 256 for a sky that is mostly smooth gradients either way.
    int skyResolution = 256;
    // Seconds between full re-bakes. Every face is rebuilt together - baking
    // one face per frame left the six holding six different moments of wind,
    // which showed as seams along the cube edges.
    float skyRefreshInterval = 0.1f;

    WeatherSettings weather;

    // -- Volumetric lighting --
    //
    // Light scattering through the air between the camera and the surface it
    // sees: god rays through a window, a spot cone made visible, haze picking up
    // the sun. Sits with fog rather than with post FX because it is a property
    // of the scene's atmosphere, not of a camera.
    //
    // Only shadow-casting lights take part, and that is not a shortcut: the
    // effect *is* the shadow map sampled along a ray, so a light with no shadow
    // map has nothing to scatter around. Off by default; it is the most
    // expensive thing in the frame when it is on.
    bool volumetricEnabled = false;
    float volumetricIntensity = 1.0f;
    // Ray-march steps per pixel. Cost is linear in this and in the number of
    // participating lights, so eight shadowed lights at 64 steps is 512 shadow
    // lookups for every pixel on screen.
    int volumetricSteps = 32;
    // Henyey-Greenstein asymmetry. Positive scatters forward, which is what
    // makes a beam blaze when you look toward the light and fade when you look
    // away; 0 is uniform scattering in all directions.
    float volumetricScattering = 0.6f;
    // How much medium there is per world unit. This is the brightness knob that
    // stays physically sane - intensity above just scales the result.
    float volumetricDensity = 0.05f;
    // Rays stop here even if the surface is further away, which bounds the cost
    // of a pixel looking at the horizon.
    float volumetricMaxDistance = 60.0f;
    // Marches at half width and height and upsamples. Roughly a quarter of the
    // cost, and the effect is low-frequency enough that it rarely shows.
    bool volumetricHalfResolution = true;
    // Depth-aware separable blur over the volumetric buffer before it is
    // composited. The march dithers each pixel's start offset to turn banding
    // into grain; this is what removes the grain afterwards, and it is what
    // makes a low step count usable - which is the whole game on mobile.
    // Ceiling on in-scattered light, applied as a smooth roll-off rather than
    // a clip. The pass is purely additive - it never darkens anything - so a
    // shaft crossing an already-bright surface simply piles on top of it and
    // blows out, and the bloom threshold sits below that, so the blowout then
    // blooms as well. This lets a scene keep strong shafts in the dark parts
    // without the lit parts going to white. 0 disables the limit.
    float volumetricMaxIntensity = 2.0f;
    bool volumetricDenoise = true;
    // Taps each side, per axis. 0 is off; above ~4 it starts smearing thin
    // shafts rather than cleaning them.
    int volumetricDenoiseRadius = 3;

    // -- Screen-space reflections --
    //
    // Sits with the cubemap reflections above rather than in post FX: it is the
    // same question ("what does this surface see") answered from the depth
    // buffer instead of from a probe, and it falls back to that same cube where
    // the screen has no answer.
    //
    // Needs the surface prepass, which only runs while this is on - so the cost
    // of being switched off is zero, not merely small.
    bool ssrEnabled = false;
    float ssrIntensity = 1.0f;
    int ssrSteps = 48;
    // Binary-search iterations refining the crossing once the coarse march has
    // stepped through a surface. Cheap and worth far more than extra march
    // steps: this is what makes a contact sharp instead of a staircase.
    int ssrRefineSteps = 6;
    float ssrMaxDistance = 40.0f;
    // World-space depth behind a surface that still counts as touching it. Too
    // small breaks reflections up on grazing geometry; too large lets rays hit
    // the far side of thin objects.
    float ssrThickness = 0.5f;
    // Above this roughness the surface is handed back to the cube. Screen-space
    // tracing is a mirror technique - one ray blurred is a smear, not a rough
    // reflection.
    float ssrMaxRoughness = 0.6f;
    bool ssrHalfResolution = true;

    // -- Voxel cone traced global illumination --
    //
    // The scene's opaque geometry is rasterized into a 3D radiance volume once,
    // then every lit fragment gathers bounced light from it by tracing a handful
    // of cones through that volume's mip chain.
    //
    // Requires a GL 4.3 context (image load/store into a 3D texture, and a
    // framebuffer with no attachments) and a seventeenth fragment texture unit,
    // because the standard shader already uses all sixteen the 3.3 spec
    // guarantees. The renderer refuses to enable it otherwise and says why.
    //
    // The Nebula lightmap baker stays useful alongside this rather than being
    // replaced: bakes are assigned as ordinary textures and nothing samples them
    // as a separate lighting term, so the two do not double-count. Turn this
    // down on a lightmapped scene rather than off, and it fills in the bounce
    // for everything the bake could not see.
    bool giEnabled = false;
    float giIntensity = 1.0f;
    // Voxels per axis. The volume is RGBA16F so a voxel is 8 bytes; with the mip
    // chain that is roughly 21 MB at 128 and 150 MB at 256.
    int giResolution = 128;
    // Cap on the auto-fitted volume size. The volume is centred on the scene and
    // sized to it, up to this; past it the scene simply outgrows the volume and
    // GI covers the middle.
    float giMaxExtent = 60.0f;
    int giConeSteps = 24;
    // Bounces resolved per bake. Each pass rasterizes the scene again, gathering
    // from the previous pass's volume, so light that reached a wall on bounce 1
    // becomes a light source for bounce 2.
    //
    // Iterated inside the bake rather than converged across frames, because the
    // bake is already event-driven - a scene that has not changed would never
    // get a second frame to converge in. Cost is linear in this and it is paid
    // only when the volume is rebuilt.
    //
    // Above 1 this allocates a second volume to ping-pong through, so the memory
    // in giResolution's note doubles.
    int giBounces = 2;
};

class Skybox {
private:
    unsigned int VAO, VBO;
    Shader* skyboxShader;
    Texture* sunTexture = nullptr;
    Texture* moonTexture = nullptr;
    Texture* scrollingTexture = nullptr;
    std::string vertPath = "Resources/Shaders/skybox_vert.glsl";
    std::string fragPath = "Resources/Shaders/skybox_frag.glsl";
    float timeOfDay = 0.5f; // 0.0 = night, 0.25 = sunrise, 0.5 = day, 0.75 = sunset, 1.0 = midnight
    float animationTime = 0.0f;
    SkyboxSettings settings;
    WeatherDrive weatherDrive;

    // Direction TOWARD the sun (the reverse of the light's forward), supplied by
    // the renderer each frame from the scene's main directional light.
    glm::vec3 sunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
    bool sunDirectionValid = false;
    // Set while the sun is being moved. The baked cube is refreshed one face per
    // draw, so during a move it would hold a mix of old and new sun positions and
    // show a hard seam at the face boundary; these let draw() sidestep the cache
    // until the sun settles and then refresh every face at once.
    float sunMotionSec = -1.0f;
    bool bakedSunStale = false;

    // Baked-sky cache: the procedural sky is rendered into this cubemap a face
    // at a time so fullscreen sky draws collapse to one cube fetch per pixel.
    unsigned int bakedCube = 0;
    unsigned int bakedFbo = 0;
    int bakedDirtyMask = 0;          // bit per cube face
    bool bakedHasFullCapture = false;
    bool bakedAllocFailed = false;
    float bakedLastRefreshSec = -1.0f;
    // The cube is only valid for the resolution and viewpoint it was baked at.
    // Volumetric clouds march from the camera, so moving far enough invalidates
    // it the same way a moved sun does.
    int bakedResolution = 0;
    glm::vec3 bakedCameraPos = glm::vec3(0.0f);
    bool bakedCameraValid = false;
    glm::vec3 currentCameraPos = glm::vec3(0.0f);
    uint64_t cameraFrameSerial = ~0ull;

    void setupMesh();
    bool reloadShader();
    void reloadTextures();
    void releaseBakedSky();
    bool ensureBakedSkyTargets();
    void bakeDirtyFaces(int maxFaces);
    bool cachedSkyUsable() const;
    void applySkyUniforms(const glm::mat4& viewMat, const glm::mat4& projMat,
                          float viewportW, float viewportH,
                          const glm::vec2& cameraAngles, bool sampleBaked);
    void drawSkyTriangle();

public:
    Skybox();
    ~Skybox();
    
    void draw(const float* view, const float* projection);
    void setTimeOfDay(float time); // 0.0 to 1.0
    float getTimeOfDay() const { return timeOfDay; }
    void setShaderPaths(const std::string& vert, const std::string& frag);
    std::string getVertPath() const { return vertPath; }
    std::string getFragPath() const { return fragPath; }
    void setSettings(const SkyboxSettings& newSettings);
    // This frame's resolved weather. The skybox only consumes it; the schedule
    // and the audio live with the engine.
    void setWeatherDrive(const WeatherDrive& drive) { weatherDrive = drive; }
    // Where the clouds are marched from. Without this they are an infinitely
    // distant dome you can never reach.
    //
    // Only the FIRST call of each frame is honoured. The baked cube is a single
    // shared resource but the skybox is drawn once per viewport, each with its
    // own camera - letting every viewport re-point it made the editor and game
    // viewports fight over it, re-baking all six faces twice per frame forever,
    // with each one then sampling a cube baked for the other's camera.
    void setCameraPosition(const glm::vec3& pos, uint64_t frameSerial) {
        if (cameraFrameSerial == frameSerial) return;
        cameraFrameSerial = frameSerial;
        currentCameraPos = pos;
    }
    // `direction` points toward the light source, i.e. -forward of the light.
    // Pass valid=false when the scene has no usable directional light; the sky
    // then falls back to the timeOfDay arc.
    void setDirectionalLight(const glm::vec3& direction, bool valid);
    // Where the sun actually ended up, for anything that needs to agree with the
    // sky (lens flare, for one). Matches the shader's own choice either way.
    glm::vec3 resolvedSunDirection() const;
    const SkyboxSettings& getSettings() const { return settings; }
};

#endif
