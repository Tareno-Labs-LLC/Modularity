#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Net {

using NetworkId = uint32_t;
constexpr NetworkId kInvalidNetworkId = 0;

using PlayerId = int32_t;
constexpr PlayerId kInvalidPlayerId = 0;

enum class ConnectionState {
    Disconnected = 0,
    Connecting,
    ConnectedToMaster,
    JoiningLobby,
    InLobby,
    JoiningRoom,
    InRoom,
    LeavingRoom,
    Disconnecting,
    Failed,
};

const char* ToString(ConnectionState state);

enum class ErrorCode {
    None = 0,
    InvalidAppId,
    InvalidRegion,
    AuthenticationFailed,
    ConnectionTimeout,
    ConnectionLost,
    RoomNotFound,
    RoomFull,
    RoomAlreadyExists,
    NotInRoom,
    NotAuthorized,
    BackendUnavailable,
    OperationFailed,
    Unknown,
};

const char* ToString(ErrorCode code);

struct NetworkError {
    ErrorCode code = ErrorCode::None;
    int nativeCode = 0;
    std::string message;

    bool ok() const { return code == ErrorCode::None; }
};

enum class SendMode {
    Unreliable = 0,
    Reliable,
};

enum class RpcTarget {
    All = 0,
    Others,
    Host,
    Server,
    Target,
    Local,
    AllBuffered,
    OthersBuffered,
};

const char* ToString(RpcTarget target);

inline bool IsBuffered(RpcTarget target) {
    return target == RpcTarget::AllBuffered || target == RpcTarget::OthersBuffered;
}

enum class LogLevel {
    Info = 0,
    Warning,
    Error,
    Success,
};

const char* ToString(LogLevel level);

struct NetworkPlayer {
    PlayerId id = kInvalidPlayerId;
    std::string nickname;
    bool isLocal = false;
    bool isHost = false;
    std::vector<std::pair<std::string, std::string>> properties;

    const std::string* findProperty(const std::string& key) const;
};

struct RoomInfo {
    std::string name;
    int playerCount = 0;
    int maxPlayers = 0;
    bool isOpen = true;
    bool isVisible = true;
    std::vector<std::pair<std::string, std::string>> properties;
};

struct RoomOptions {
    int maxPlayers = 0;
    bool isOpen = true;
    bool isVisible = true;
    std::vector<std::string> lobbyVisibleProperties;
    std::vector<std::pair<std::string, std::string>> properties;
    bool cleanupCacheOnLeave = true;
};

struct SessionConfig {
    std::string appId;
    std::string appVersion = "1.0";
    std::string nickname = "Player";
    std::string region;
    bool offlineMode = false;
    int maxPlayers = 0;
    int sendRateHz = 20;
    int serializationRateHz = 10;
    int maxOutboundBytesPerSecond = 0;
};

enum class SyncMode {
    Snapshot = 0,
    Interpolate,
    Extrapolate,
};

const char* ToString(SyncMode mode);

struct SyncSettings {
    bool syncPosition = true;
    bool syncRotation = true;
    bool syncScale = false;
    bool syncVelocity = false;

    SyncMode mode = SyncMode::Interpolate;

    int sendRateHz = 0;

    float interpolationDelay = 0.1f;

    float maxExtrapolation = 0.25f;

    float positionThreshold = 0.001f;
    float rotationThreshold = 0.01f;
    float scaleThreshold = 0.001f;
};

struct TransformSnapshot {
    double timestamp = 0.0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[3] = {0.0f, 0.0f, 0.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
    float velocity[3] = {0.0f, 0.0f, 0.0f};
    bool hasPosition = false;
    bool hasRotation = false;
    bool hasScale = false;
    bool hasVelocity = false;
};

struct BandwidthStats {
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    uint32_t packetsSent = 0;
    uint32_t packetsReceived = 0;
    uint32_t sendsSkippedByBudget = 0;
    uint32_t sendsSkippedByThreshold = 0;
};

struct SpawnRequest {
    NetworkId networkId = kInvalidNetworkId;
    std::string assetId;
    std::string instanceId;
    PlayerId owner = kInvalidPlayerId;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[3] = {0.0f, 0.0f, 0.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
    NetworkId parentNetworkId = kInvalidNetworkId;
    bool spawnDisabled = false;
};

// ---------------------------------------------------------------------------
// Avatar pose channel
// ---------------------------------------------------------------------------
//
// Transform sync covers one transform per network object - NetworkObject carries a
// single rootSceneObjectId - so the children of a spawned instance are not synced.
// A VR rig is exactly that shape: one object with a head and two hands under it,
// all moving independently every frame.
//
// Rather than spawn a network object per body part (three ownership records and no
// guarantee they stay grouped), a player streams its tracked parts on a channel of
// its own, keyed by player rather than by object. This is how Photon's own VR rigs
// work, and it keeps the parts atomic: one packet carries the whole pose.
//
// Rotations here are quaternions, not the Euler degrees SceneObject stores. Headset
// orientation cannot survive a Euler round-trip without gimbal artefacts, and the
// engine's Euler composition (QuatFromEulerXYZ, Rx*Ry*Rz) is not glm's default, so
// converting on the wire would bake in a convention mismatch. Scripts convert once
// at the point they apply a pose, through ScriptContext::QuatToEulerDegrees.
//
// The engine deliberately stores only the latest pose and does no smoothing: a
// receiver knows its own frame rate and what it is attached to, so damping belongs
// there. `timestamp` is the sender's session clock, for scripts that want to age
// or extrapolate a pose.

// Slot numbering is open-ended on purpose. Head/hands are what a rig needs today;
// hips, feet and elbows are the obvious next ones, and adding them must not mean an
// ABI change, so the slot crosses every boundary as a plain int.
namespace AvatarSlot {
constexpr int kHead = 0;
constexpr int kLeftHand = 1;
constexpr int kRightHand = 2;
}  // namespace AvatarSlot

// Bounded so a malformed or hostile packet cannot make the receiver allocate: the
// slot count is validated against this before any indexing.
constexpr int kAvatarSlotCount = 8;

struct AvatarPose {
    bool valid = false;
    double timestamp = 0.0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // quaternion, xyzw
};


}  // namespace Net
