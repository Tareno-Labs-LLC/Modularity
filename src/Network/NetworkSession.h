#pragma once

#include "NetworkBackend.h"
#include "NetworkSerializer.h"
#include "../ModuObj.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Net {

struct SceneBridge {
    std::vector<SceneObject>* sceneObjects = nullptr;
    int* nextObjectId = nullptr;
    std::function<const ModuObj::Asset*(const std::string& assetId, std::string& outError)> resolveAsset;

    bool valid() const { return sceneObjects && nextObjectId && resolveAsset; }
};

struct NetworkObject {
    NetworkId networkId = kInvalidNetworkId;
    PlayerId owner = kInvalidPlayerId;
    std::string assetId;
    std::string instanceId;
    int rootSceneObjectId = -1;
    bool spawned = false;
    bool hasAuthority = false;
};

using RpcHandler = std::function<void(PlayerId sender, Reader& payload)>;

class NetworkSession {
public:
    NetworkSession();
    ~NetworkSession();

    bool start(const SessionConfig& config, const SceneBridge& bridge);

    bool startWithBackend(const SessionConfig& config,
                          const SceneBridge& bridge,
                          std::unique_ptr<NetworkBackend> backend);
    void shutdown();
    void tick(float deltaSeconds);

    bool isOffline() const { return offline; }
    const char* backendName() const;

    bool connect();
    void disconnect();
    bool reconnect();
    ConnectionState state() const;
    int pingMs() const;
    const std::string& region() const;
    const NetworkError& lastError() const { return error; }

    bool joinLobby();
    bool refreshRoomList();
    const std::vector<RoomInfo>& roomList() const;
    bool createRoom(const std::string& name, const RoomOptions& options);
    bool joinRoom(const std::string& name);
    bool joinOrCreateRoom(const std::string& name, const RoomOptions& options);
    bool joinRandomRoom();
    bool leaveRoom();
    bool inRoom() const { return state() == ConnectionState::InRoom; }

    const std::vector<NetworkPlayer>& players() const;
    PlayerId localPlayerId() const;
    PlayerId hostPlayerId() const;
    bool isHost() const;
    bool setNickname(const std::string& nickname);
    bool setPlayerProperty(const std::string& key, const std::string& value);
    bool setRoomProperty(const std::string& key, const std::string& value);

    NetworkId spawn(const std::string& assetId,
                    const float position[3] = nullptr,
                    const float rotation[3] = nullptr,
                    const float scale[3] = nullptr,
                    NetworkId parent = kInvalidNetworkId,
                    bool spawnDisabled = false);

    bool despawn(NetworkId networkId);

    bool transferOwnership(NetworkId networkId, PlayerId newOwner);

    void setSyncSettings(NetworkId networkId, const SyncSettings& settings);
    const SyncSettings* syncSettings(NetworkId networkId) const;

    size_t bufferedSnapshotCount(NetworkId networkId) const;

    // Avatar pose channel. Keyed by player rather than by network object, because
    // transform sync moves one transform per object and a rig needs several moving
    // independently - see the block comment in NetworkTypes.h.
    //
    // Setting a slot marks it live; it streams to the room at serializationRateHz
    // for as long as it stays set. Poses are sent unreliably: a dropped one is
    // replaced by the next in ~50ms, and re-sending a stale head position is worse
    // than missing it.
    void setLocalAvatarPose(int slot, const float position[3], const float rotationXyzw[4]);
    void clearLocalAvatarPose(int slot);

    // Latest pose received for a player, or the local one for the local player id.
    // Returns false for an unknown player, an out-of-range slot, or a slot that has
    // never been set - callers should treat that as "not tracked" rather than zero.
    bool avatarPose(PlayerId player, int slot, AvatarPose& out) const;
    void clearAvatarPoses(PlayerId player);

    const BandwidthStats& bandwidth() const { return stats; }
    void resetBandwidthStats() { stats = BandwidthStats{}; }

    double now() const { return clock; }

    const NetworkObject* findObject(NetworkId networkId) const;
    NetworkId findNetworkIdForSceneObject(int sceneObjectId) const;
    std::vector<NetworkId> allObjects() const;
    bool hasAuthority(NetworkId networkId) const;

    void registerRpc(const std::string& name, RpcHandler handler);
    void unregisterRpc(const std::string& name);
    bool callRpc(const std::string& name,
                 const Writer& args,
                 RpcTarget target,
                 SendMode mode = SendMode::Reliable,
                 PlayerId targetPlayer = kInvalidPlayerId);

    std::function<void(ConnectionState)> onStateChanged;
    std::function<void(const NetworkError&)> onError;
    std::function<void(const NetworkPlayer&)> onPlayerJoined;
    std::function<void(PlayerId)> onPlayerLeft;
    std::function<void(NetworkId)> onObjectSpawned;
    std::function<void(NetworkId)> onObjectDespawned;
    std::function<void(NetworkId, PlayerId)> onOwnershipChanged;

    std::function<void(LogLevel, const std::string&)> onLog;

    std::string describeConfig() const;

private:
    void wireCallbacks();
    void handleEvent(PlayerId sender, uint8_t code, const std::vector<uint8_t>& payload);
    void applySpawn(const SpawnRequest& request, bool local);
    void applyDespawn(NetworkId networkId);
    NetworkId allocateNetworkId();
    void fail(ErrorCode code, const std::string& message);
    void log(LogLevel level, const std::string& message) const;

    void sendOwnedTransforms(float deltaSeconds);
    void sendAvatarPoses();
    void handleAvatarPose(PlayerId sender, Reader& reader);
    void applyRemoteTransforms(float deltaSeconds);
    void handleStateSync(PlayerId sender, Reader& reader);
    SceneObject* findSceneObject(int sceneObjectId);

    struct SyncState {
        SyncSettings settings;
        double lastSendTime = -1.0;
        bool hasLastSent = false;
        float lastSentPosition[3] = {0.0f, 0.0f, 0.0f};
        float lastSentRotation[3] = {0.0f, 0.0f, 0.0f};
        float lastSentScale[3] = {1.0f, 1.0f, 1.0f};
        std::vector<TransformSnapshot> snapshots;
    };
    std::unordered_map<NetworkId, SyncState> syncStates;

    // Local slots are the source for outgoing packets; remote ones are the latest
    // received per player. Both are fixed-size so a player costs a known amount and
    // a bad slot index cannot reach past the end.
    std::array<AvatarPose, kAvatarSlotCount> localAvatar{};
    bool localAvatarLive = false;
    double lastAvatarSendTime = -1.0;
    std::unordered_map<PlayerId, std::array<AvatarPose, kAvatarSlotCount>> remoteAvatars;

    BandwidthStats stats;
    double clock = 0.0;
    double budgetWindowStart = 0.0;
    uint64_t budgetWindowBytes = 0;

    // Active calls retain their backend if a callback shuts down/restarts the session.
    std::shared_ptr<NetworkBackend> backend;
    uint64_t backendGeneration = 0;
    SessionConfig config;
    SceneBridge scene;
    NetworkError error;
    bool offline = false;
    bool running = false;

    NetworkId nextLocalNetworkId = 0;

    std::unordered_map<NetworkId, NetworkObject> objects;
    std::unordered_map<std::string, RpcHandler> rpcHandlers;
};

}  // namespace Net
