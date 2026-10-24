#pragma once

// Hover text for the build switches, shared by the Build Settings window and the Build tab of
// Project Settings so the two can never describe the same switch differently.
//
// Several switches are saved with the profile but not yet read by the exporter
// (Engine::startExportBuild). Their hints say so rather than promising behaviour the export does
// not have; update the text when one of them gets wired up.
namespace BuildSettingsHints {

inline constexpr const char* kConfiguration =
    "Release is optimised and is what you ship.\n"
    "Debug keeps symbols and assertions for debugging the player, and runs much slower.";

inline constexpr const char* kRuntimeOnly =
    "Ship the standalone player without editor code.\n"
    "Exports already build only the player, so this is saved with the profile but the exporter "
    "does not act on it yet.";

inline constexpr const char* kIncludeEditor =
    "Configures the export build with the editor target enabled.\n"
    "The export still builds and ships only the player (it builds the ModularityPlayer target), "
    "so today this only makes the configure step slower.";

inline constexpr const char* kLeanRuntimeExport =
    "Copies only what the player needs: shaders, textures, fonts, the skybox and the logo instead "
    "of all of Resources, and shared libraries without their static (.a / .lib) counterparts.\n"
    "Smaller build; turn it off if something loads an engine resource by path at runtime.";

inline constexpr const char* kShipScriptSdk =
    "Copies Resources/ScriptSDK (the headers scripts compile against) into the build, so scripts "
    "can be rebuilt next to the shipped game.\n"
    "Always on for a Development Build.";

inline constexpr const char* kCompressAudio =
    "WAV, AIFF and FLAC clips ship as Ogg Vorbis. MP3 and OGG are left as they are.\n"
    "Scenes and scripts keep using the original file names.";

inline constexpr const char* kVorbisQuality =
    "libvorbis quality, 0-10. 5 is about 160 kbps for stereo and is transparent for most game audio; "
    "lower values trade quality for size.";

inline constexpr const char* kCompressionMethod =
    "Compression for packaged content.\n"
    "Saved with the profile; the exporter does not apply it yet.";

inline constexpr const char* kExportArchive =
    "Also packs the finished build folder into a single .tar.gz next to it, ready to upload.";

inline constexpr const char* kDevelopmentBuild =
    "Marks the build as a development build: ships the ScriptSDK, and makes Android APKs debuggable.";

inline constexpr const char* kServerBuild =
    "Headless server build with no window or rendering.\n"
    "Saved with the profile; the exporter does not act on it yet.";

inline constexpr const char* kScriptDebugging =
    "Build scripts with debug information so a debugger can step through them in the player.\n"
    "Saved with the profile; the exporter does not act on it yet.";

inline constexpr const char* kScriptsOnly =
    "Rebuild only the compiled scripts and reuse the previous player and content.\n"
    "Saved with the profile; the exporter does not act on it yet.";

inline constexpr const char* kAutoconnectProfiler =
    "The player connects to the editor's profiler as soon as it starts.\n"
    "Saved with the profile; the exporter does not act on it yet.";

inline constexpr const char* kDeepProfiling =
    "Profile every script call, not just the engine's own scopes. Much higher overhead.\n"
    "Saved with the profile; the exporter does not act on it yet.";

inline constexpr const char* kAutomaticModules =
    "Decide each optional engine module from the project: its physics backend, its renderer, the "
    "model and audio files in Assets, the C# scripts the build scenes run, and the target platform.\n"
    "Turn off to force modules on or off by hand.";

inline constexpr const char* kGles2Shaders =
    "Also ship GLSL ES 1.00 versions of the material, scrolling texture, procedural clouds and "
    "skybox shaders (as <name>.es2.glsl), for GLES 2.0-only GPUs such as the original Anbernic "
    "RG35XX.\n"
    "The ES 2 versions are unshadowed, have no voxel GI or parallax, and light at most 4 lights. "
    "The export log lists any shader that could not be converted. The player does not have a "
    "GLES 2.0 renderer yet, so today these files are prepared but not used.";

inline constexpr const char* kPreloadShaders =
    "Compile every shader the build scenes use while the player boots, instead of the first time "
    "something draws with it, so there is no hitch mid-game. Costs a little startup time.\n"
    "Always on when GLES 2.0 Shaders is enabled.";

inline constexpr const char* kSplash =
    "Show an image while the player starts. The image is copied into the build.";

}  // namespace BuildSettingsHints
