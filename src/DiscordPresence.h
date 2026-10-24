#pragma once

#include <string>

class Project;

#if defined(_WIN32)
    #if defined(MODULARITY_SCRIPT_HOST)
        #define MODULARITY_DISCORD_PRESENCE_API __declspec(dllexport)
    #elif defined(MODULARITY_SCRIPT_IMPORTS)
        #define MODULARITY_DISCORD_PRESENCE_API __declspec(dllimport)
    #else
        #define MODULARITY_DISCORD_PRESENCE_API
    #endif
#else
    #define MODULARITY_DISCORD_PRESENCE_API __attribute__((visibility("default")))
#endif

namespace Modularity::DiscordPresence {

// Modularity's public Discord application id. An application id identifies the
// activity assets to use; it is not a credential and cannot be kept secret by a
// desktop application.
constexpr unsigned long long kEditorApplicationId = 1558652908516413470ULL;

struct Activity {
    std::string state;
    std::string details;
    std::string largeImageKey;
    std::string largeImageText;
    std::string smallImageKey;
    std::string smallImageText;
    std::string partyId;
};

// Called from the engine loop. In the editor, gameRunning means play mode. In
// ModularityPlayer it remains true for the lifetime of the loaded game.
MODULARITY_DISCORD_PRESENCE_API void Tick(const Project& project,
                                          bool gameRunning,
                                          bool standalonePlayer);
MODULARITY_DISCORD_PRESENCE_API void Shutdown();

// Native/ModuCPP scripts can include this header and replace the project's
// default activity while play mode is active. ClearRuntimeActivity restores the
// defaults authored in Project Settings > Player Config.
MODULARITY_DISCORD_PRESENCE_API void SetRuntimeActivity(const Activity& activity);
MODULARITY_DISCORD_PRESENCE_API void ClearRuntimeActivity();

MODULARITY_DISCORD_PRESENCE_API bool IsSdkAvailable();

} // namespace Modularity::DiscordPresence

#undef MODULARITY_DISCORD_PRESENCE_API
