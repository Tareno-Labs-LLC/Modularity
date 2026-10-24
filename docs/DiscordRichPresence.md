# Discord Rich Presence

Modularity publishes an editor activity through the engine's Discord application. When Play Mode
starts, a project that has Discord Rich Presence enabled switches to that project's Discord
application and configured activity. Leaving Play Mode restores the editor activity. Standalone
players publish only the project's activity and remain silent when the feature is disabled.

## Build setup

Discord integration is optional and disabled by default. Normal editor/player
builds do not need the Social SDK or its runtime library. The build scripts also
disable a previously cached SDK setting unless explicitly enabled for that build.

To include Rich Presence, run `build.bat --discord` on Windows or
`./build.sh --discord` on Linux. For direct CMake builds, set
`-DMODULARITY_ENABLE_DISCORD_SOCIAL_SDK=ON`. Use `--no-discord` (or omit
`--discord`) to build without it; a clean build is not required.

When enabled, provide a compatible SDK under `src/ThirdParty/DiscordSocialSdk`
or set `MODULARITY_DISCORD_SOCIAL_SDK_ROOT` to its location. Without a usable SDK,
the engine builds with its no-op Discord implementation.

## Project setup

Open **Project Settings > Player Config > Discord Rich Presence**, enable the feature, and enter the
project's Discord Application ID. The image keys must match Rich Presence assets uploaded for that
application in the Discord Developer Portal. The remaining fields provide the default activity used
while the game is running.

Application IDs are public identifiers. Never place a Discord client secret in a project, script,
source file, executable, or shipped configuration. Rich Presence does not require a client secret.

Discord's **User Settings > Activity Privacy > Share my activity** option must be enabled for the
activity to appear on a profile. Discord can accept a Rich Presence update successfully while this
privacy option keeps it hidden.

## Updating the activity from a native script

Native/ModuCPP code can replace the project's defaults at runtime:

```cpp
#include "DiscordPresence.h"

void UpdateNightfallsTownPresence() {
    Modularity::DiscordPresence::Activity activity;
    activity.state = "Exploring Nightfall's Town";
    activity.details = "Chapter 1 - Story Mode";
    activity.largeImageKey = "nightfalls_town";
    activity.largeImageText = "Anébönidé";
    activity.smallImageKey = "playing_as_nightfall";
    activity.smallImageText = "Playing as Nightfall";
    activity.partyId = "ae488379-351d-4a4f-ad32-2b9b01c91657";
    Modularity::DiscordPresence::SetRuntimeActivity(activity);
}
```

Call `Modularity::DiscordPresence::ClearRuntimeActivity()` to return to the project's configured
defaults. Modularity automatically clears a runtime override when Play Mode ends.

The party ID groups players into a Discord party. Use a stable ID only when that is the intended
group; multiplayer sessions should generally provide their own session-specific party ID.
