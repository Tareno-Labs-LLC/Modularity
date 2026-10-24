#include "DiscordPresence.h"

#include "ProjectManager.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>

#ifndef MODULARITY_HAS_DISCORD_SOCIAL_SDK
#define MODULARITY_HAS_DISCORD_SOCIAL_SDK 0
#endif

#if MODULARITY_HAS_DISCORD_SOCIAL_SDK
#define DISCORDPP_IMPLEMENTATION
#include <discordpp.h>
#endif

namespace Modularity::DiscordPresence {
namespace {

struct PendingOverride {
    std::mutex mutex;
    std::optional<Activity> value;
    unsigned long long revision = 0;
};

PendingOverride& RuntimeOverride() {
    static PendingOverride overrideState;
    return overrideState;
}

std::optional<std::uint64_t> ParseApplicationId(const std::string& text) {
    if (text.empty()) return std::nullopt;
    try {
        size_t used = 0;
        const unsigned long long value = std::stoull(text, &used, 10);
        if (used != text.size() || value == 0 ||
            value > std::numeric_limits<std::uint64_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(value);
    } catch (...) {
        return std::nullopt;
    }
}

Activity ProjectActivity(const ProjectDiscordPresenceSettings& settings) {
    Activity activity;
    activity.state = settings.state;
    activity.details = settings.details;
    activity.largeImageKey = settings.largeImageKey;
    activity.largeImageText = settings.largeImageText;
    activity.smallImageKey = settings.smallImageKey;
    activity.smallImageText = settings.smallImageText;
    activity.partyId = settings.partyId;
    return activity;
}

std::string ActivitySignature(std::uint64_t applicationId,
                              const std::string& name,
                              const Activity& activity,
                              unsigned long long overrideRevision) {
    std::ostringstream out;
    out << applicationId << '\n' << name << '\n' << activity.state << '\n'
        << activity.details << '\n' << activity.largeImageKey << '\n'
        << activity.largeImageText << '\n' << activity.smallImageKey << '\n'
        << activity.smallImageText << '\n' << activity.partyId << '\n'
        << overrideRevision;
    return out.str();
}

#if MODULARITY_HAS_DISCORD_SOCIAL_SDK
class Service {
public:
    void tick(const Project& project, bool gameRunning, bool standalonePlayer) {
        discordpp::RunCallbacks();

        std::optional<Activity> runtimeActivity;
        unsigned long long overrideRevision = 0;
        {
            PendingOverride& pending = RuntimeOverride();
            std::lock_guard<std::mutex> lock(pending.mutex);
            if (!gameRunning && lastGameRunning_) {
                pending.value.reset();
                ++pending.revision;
            }
            runtimeActivity = pending.value;
            overrideRevision = pending.revision;
        }
        lastGameRunning_ = gameRunning;

        const ProjectDiscordPresenceSettings& game = project.discordPresenceSettings;
        if (gameRunning && game.enabled) {
            const std::optional<std::uint64_t> applicationId =
                ParseApplicationId(game.applicationId);
            if (!applicationId) {
                if (lastInvalidApplicationId_ != game.applicationId) {
                    std::cerr << "[Discord] Rich Presence is enabled, but application id '"
                              << game.applicationId << "' is not a valid Discord application id.\n";
                    lastInvalidApplicationId_ = game.applicationId;
                }
                if (standalonePlayer) {
                    clear();
                    return;
                }
            } else {
                lastInvalidApplicationId_.clear();
                Activity activity = runtimeActivity.value_or(ProjectActivity(game));
                const std::string activityName = project.playerSettings.productName.empty()
                    ? project.name
                    : project.playerSettings.productName;
                publish(*applicationId, activityName, activity, overrideRevision);
                return;
            }
        }

        if (standalonePlayer) {
            clear();
            return;
        }

        Activity editor;
        editor.state = project.isLoaded && !project.name.empty()
            ? "Editor - " + project.name
            : "Editor";
        editor.details = "Currently active";
        editor.largeImageKey = "modu_logo";
        editor.largeImageText = "In Editor Mode";
        publish(kEditorApplicationId, "Modularity", editor, 0);
    }

    void shutdown() {
        clear();
        client_.reset();
    }

private:
    void ensureClient() {
        if (client_) return;
        client_.emplace();
        client_->AddLogCallback(
            [](std::string message, discordpp::LoggingSeverity) {
                std::cerr << "[Discord SDK] " << message << '\n';
            },
            discordpp::LoggingSeverity::Warning);
    }

    static void SetOptionalText(const std::string& value,
                                size_t minimum,
                                size_t maximum,
                                const std::function<void(std::optional<std::string>)>& setter) {
        if (value.size() >= minimum && value.size() <= maximum) setter(value);
    }

    void publish(std::uint64_t applicationId,
                 const std::string& name,
                 const Activity& source,
                 unsigned long long overrideRevision) {
        const std::string signature =
            ActivitySignature(applicationId, name, source, overrideRevision);
        if (signature == lastSignature_) return;

        ensureClient();
        if (applicationId_ != applicationId) {
            if (applicationId_ != 0) client_->ClearRichPresence();
            client_->SetApplicationId(applicationId);
            applicationId_ = applicationId;
        }

        discordpp::Activity activity;
        if (!name.empty()) activity.SetName(name);
        activity.SetType(discordpp::ActivityTypes::Playing);
        const bool hasState = source.state.size() >= 2 && source.state.size() <= 128;
        const bool hasDetails = source.details.size() >= 2 && source.details.size() <= 128;
        activity.SetStatusDisplayType(hasDetails
            ? discordpp::StatusDisplayTypes::Details
            : (hasState ? discordpp::StatusDisplayTypes::State
                        : discordpp::StatusDisplayTypes::Name));
        SetOptionalText(source.state, 2, 128,
                        [&](std::optional<std::string> value) { activity.SetState(std::move(value)); });
        SetOptionalText(source.details, 2, 128,
                        [&](std::optional<std::string> value) { activity.SetDetails(std::move(value)); });

        discordpp::ActivityAssets assets;
        bool hasAssets = false;
        const bool hasLargeImage =
            source.largeImageKey.size() >= 1 && source.largeImageKey.size() <= 300;
        const bool hasSmallImage =
            source.smallImageKey.size() >= 1 && source.smallImageKey.size() <= 300;
        if (hasLargeImage) {
            assets.SetLargeImage(source.largeImageKey);
            hasAssets = true;
        }
        if (hasLargeImage && source.largeImageText.size() >= 2 &&
            source.largeImageText.size() <= 128) {
            assets.SetLargeText(source.largeImageText);
            hasAssets = true;
        }
        if (hasSmallImage) {
            assets.SetSmallImage(source.smallImageKey);
            hasAssets = true;
        }
        if (hasSmallImage && source.smallImageText.size() >= 2 &&
            source.smallImageText.size() <= 128) {
            assets.SetSmallText(source.smallImageText);
            hasAssets = true;
        }
        if (hasAssets) activity.SetAssets(std::move(assets));

        if (source.partyId.size() >= 2 && source.partyId.size() <= 128) {
            discordpp::ActivityParty party;
            party.SetId(source.partyId);
            party.SetCurrentSize(1);
            party.SetMaxSize(0);
            activity.SetParty(std::move(party));
        }

        client_->UpdateRichPresence(
            std::move(activity),
            [](discordpp::ClientResult result) {
                if (!result.Successful()) {
                    std::cerr << "[Discord] Rich Presence update failed: "
                              << result.ToString() << '\n';
                }
            });
        lastSignature_ = signature;
    }

    void clear() {
        if (client_ && applicationId_ != 0) client_->ClearRichPresence();
        applicationId_ = 0;
        lastSignature_.clear();
    }

    std::optional<discordpp::Client> client_;
    std::uint64_t applicationId_ = 0;
    std::string lastSignature_;
    std::string lastInvalidApplicationId_;
    bool lastGameRunning_ = false;
};

Service& GetService() {
    static Service service;
    return service;
}
#endif

} // namespace

void Tick(const Project& project, bool gameRunning, bool standalonePlayer) {
#if MODULARITY_HAS_DISCORD_SOCIAL_SDK
    GetService().tick(project, gameRunning, standalonePlayer);
#else
    (void)project;
    (void)gameRunning;
    (void)standalonePlayer;
#endif
}

void Shutdown() {
#if MODULARITY_HAS_DISCORD_SOCIAL_SDK
    GetService().shutdown();
#endif
}

void SetRuntimeActivity(const Activity& activity) {
    PendingOverride& pending = RuntimeOverride();
    std::lock_guard<std::mutex> lock(pending.mutex);
    pending.value = activity;
    ++pending.revision;
}

void ClearRuntimeActivity() {
    PendingOverride& pending = RuntimeOverride();
    std::lock_guard<std::mutex> lock(pending.mutex);
    pending.value.reset();
    ++pending.revision;
}

bool IsSdkAvailable() {
    return MODULARITY_HAS_DISCORD_SOCIAL_SDK != 0;
}

} // namespace Modularity::DiscordPresence
