#include "NetworkSession.h"

#include <algorithm>
#include <cmath>

namespace Net {
namespace {

constexpr int kPlayerShift = 20;
constexpr NetworkId kPerPlayerCapacity = 1u << kPlayerShift;

NetworkId FirstIdForPlayer(PlayerId player) {
    if (player <= 0) return 1;
    return static_cast<NetworkId>(player) * kPerPlayerCapacity + 1;
}

}  // namespace

NetworkSession::NetworkSession() = default;

NetworkSession::~NetworkSession() { shutdown(); }

bool NetworkSession::start(const SessionConfig& cfg, const SceneBridge& bridge) {
    const bool wantOffline =
        cfg.offlineMode || !IsPhotonBackendAvailable() || cfg.appId.empty();
    std::unique_ptr<NetworkBackend> chosen =
        wantOffline ? CreateLoopbackBackend() : CreatePhotonBackend();
    if (!chosen) chosen = CreateLoopbackBackend();

    if (!startWithBackend(cfg, bridge, std::move(chosen))) return false;
    offline = wantOffline;
    return true;
}

bool NetworkSession::startWithBackend(const SessionConfig& cfg,
                                      const SceneBridge& bridge,
                                      std::unique_ptr<NetworkBackend> injected) {
    shutdown();

    config = cfg;
    scene = bridge;
    error = NetworkError{};

    if (!scene.valid()) {
        fail(ErrorCode::OperationFailed, "Networking started without a valid scene bridge.");
        return false;
    }
    if (!injected) {
        fail(ErrorCode::BackendUnavailable, "No networking backend supplied.");
        return false;
    }
    backend = std::move(injected);
    offline = (std::string(backend->name()) == "Offline");

    BackendCallbacks callbacks;
    callbacks.onStateChanged = [this](ConnectionState s) {
        LogLevel level = LogLevel::Info;
        if (s == ConnectionState::InRoom || s == ConnectionState::ConnectedToMaster) {
            level = LogLevel::Success;
        } else if (s == ConnectionState::Failed) {
            level = LogLevel::Error;
        } else if (s == ConnectionState::Disconnected) {
            level = LogLevel::Warning;
        }
        log(level, std::string("[Network] State: ") + ToString(s));
        if (onStateChanged) onStateChanged(s);
    };
    callbacks.onError = [this](const NetworkError& e) {
        error = e;
        std::string text = std::string("[Network] ") + ToString(e.code);
        if (!e.message.empty()) text += ": " + e.message;
        if (e.nativeCode != 0) text += " (backend code " + std::to_string(e.nativeCode) + ")";
        log(LogLevel::Error, text);
        if (onError) onError(e);
    };
    callbacks.onPlayerJoined = [this](const NetworkPlayer& p) {
        std::string who = p.nickname.empty() ? ("player " + std::to_string(p.id)) : p.nickname;
        log(LogLevel::Success, "[Network] " + who + " joined (id " + std::to_string(p.id) + ")" +
                               (p.isLocal ? " [local]" : "") + (p.isHost ? " [host]" : ""));
        if (onPlayerJoined) onPlayerJoined(p);
    };
    callbacks.onPlayerLeft = [this](PlayerId id) {
        if (isHost()) {
            for (auto& entry : objects) {
                if (entry.second.owner != id) continue;
                entry.second.owner = hostPlayerId();
                entry.second.hasAuthority = true;
                if (onOwnershipChanged) onOwnershipChanged(entry.first, entry.second.owner);
            }
        }
        log(LogLevel::Warning, "[Network] Player " + std::to_string(id) + " left the room.");
        if (onPlayerLeft) onPlayerLeft(id);
    };
    callbacks.onJoinedRoom = [this](const std::string& roomName) {
        nextLocalNetworkId = FirstIdForPlayer(backend->localPlayerId());
        log(LogLevel::Success,
            "[Network] Joined room \"" + roomName + "\" as player " +
            std::to_string(backend->localPlayerId()) +
            (backend->isHost() ? " (host)" : "") +
            " via " + backend->name() +
            (region().empty() ? "" : " [" + region() + "]"));
    };
    callbacks.onLeftRoom = [this]() {
        log(LogLevel::Info, "[Network] Left the room; releasing " +
                            std::to_string(objects.size()) + " networked object(s).");
        std::vector<NetworkId> ids;
        ids.reserve(objects.size());
        for (const auto& entry : objects) ids.push_back(entry.first);
        for (NetworkId id : ids) applyDespawn(id);
        objects.clear();
    };
    callbacks.onHostChanged = [this](PlayerId newHost) {
        log(LogLevel::Info, "[Network] Host is now player " + std::to_string(newHost) +
                            (newHost == localPlayerId() ? " (this client)" : ""));
    };
    callbacks.onEvent = [this](PlayerId sender, uint8_t code, const std::vector<uint8_t>& payload) {
        handleEvent(sender, code, payload);
    };

    if (!backend->initialize(config, callbacks)) {
        fail(ErrorCode::OperationFailed, "Backend failed to initialize.");
        backend.reset();
        return false;
    }

    nextLocalNetworkId = FirstIdForPlayer(backend->localPlayerId());
    running = true;
    log(LogLevel::Info, "[Network] Session started: " + describeConfig());
    return true;
}

void NetworkSession::shutdown() {
    if (backend) {
        backend->shutdown();
        backend.reset();
    }
    objects.clear();
    syncStates.clear();
    rpcHandlers.clear();
    stats = BandwidthStats{};
    clock = 0.0;
    budgetWindowStart = 0.0;
    budgetWindowBytes = 0;
    running = false;
    offline = false;
    nextLocalNetworkId = 0;
}

void NetworkSession::tick(float deltaSeconds) {
    if (!backend) return;

    clock += deltaSeconds;

    if (clock - budgetWindowStart >= 1.0) {
        budgetWindowStart = clock;
        budgetWindowBytes = 0;
    }

    backend->tick(deltaSeconds);

    if (inRoom()) {
        sendOwnedTransforms(deltaSeconds);
    }
    applyRemoteTransforms(deltaSeconds);
}

void NetworkSession::setSyncSettings(NetworkId networkId, const SyncSettings& settings) {
    syncStates[networkId].settings = settings;
}

const SyncSettings* NetworkSession::syncSettings(NetworkId networkId) const {
    auto it = syncStates.find(networkId);
    return it == syncStates.end() ? nullptr : &it->second.settings;
}

size_t NetworkSession::bufferedSnapshotCount(NetworkId networkId) const {
    auto it = syncStates.find(networkId);
    return it == syncStates.end() ? 0 : it->second.snapshots.size();
}

SceneObject* NetworkSession::findSceneObject(int sceneObjectId) {
    if (!scene.sceneObjects || sceneObjectId < 0) return nullptr;
    for (SceneObject& obj : *scene.sceneObjects) {
        if (obj.id == sceneObjectId) return &obj;
    }
    return nullptr;
}

void NetworkSession::sendOwnedTransforms(float) {
    if (!scene.valid()) return;

    for (auto& entry : objects) {
        NetworkObject& object = entry.second;
        if (!object.hasAuthority || object.rootSceneObjectId < 0) continue;

        SyncState& sync = syncStates[entry.first];
        const SyncSettings& settings = sync.settings;
        if (!settings.syncPosition && !settings.syncRotation &&
            !settings.syncScale && !settings.syncVelocity) {
            continue;
        }

        const int rate = settings.sendRateHz > 0 ? settings.sendRateHz
                                                 : std::max(1, config.serializationRateHz);
        const double interval = 1.0 / static_cast<double>(rate);
        if (sync.lastSendTime >= 0.0 && (clock - sync.lastSendTime) < interval) {
            continue;
        }

        const SceneObject* obj = findSceneObject(object.rootSceneObjectId);
        if (!obj) continue;

        const float position[3] = {obj->position.x, obj->position.y, obj->position.z};
        const float rotation[3] = {obj->rotation.x, obj->rotation.y, obj->rotation.z};
        const float scaleValues[3] = {obj->scale.x, obj->scale.y, obj->scale.z};

        if (sync.hasLastSent) {
            auto within = [](const float* a, const float* b, float threshold) {
                if (threshold <= 0.0f) return false;
                for (int i = 0; i < 3; ++i) {
                    if (std::fabs(a[i] - b[i]) > threshold) return false;
                }
                return true;
            };
            const bool positionStill =
                !settings.syncPosition || within(position, sync.lastSentPosition, settings.positionThreshold);
            const bool rotationStill =
                !settings.syncRotation || within(rotation, sync.lastSentRotation, settings.rotationThreshold);
            const bool scaleStill =
                !settings.syncScale || within(scaleValues, sync.lastSentScale, settings.scaleThreshold);
            if (positionStill && rotationStill && scaleStill) {
                ++stats.sendsSkippedByThreshold;
                sync.lastSendTime = clock;
                continue;
            }
        }

        Writer writer;
        writer.writeNetworkId(entry.first);
        writer.writeDouble(clock);
        uint8_t mask = 0;
        if (settings.syncPosition) mask |= 0x1;
        if (settings.syncRotation) mask |= 0x2;
        if (settings.syncScale) mask |= 0x4;
        if (settings.syncVelocity) mask |= 0x8;
        writer.writeUInt8(mask);
        if (settings.syncPosition) writer.writeVec3(position);
        if (settings.syncRotation) writer.writeVec3(rotation);
        if (settings.syncScale) writer.writeVec3(scaleValues);
        if (settings.syncVelocity) {
            float velocity[3] = {0.0f, 0.0f, 0.0f};
            if (sync.hasLastSent && sync.lastSendTime >= 0.0) {
                const double dt = clock - sync.lastSendTime;
                if (dt > 1e-6) {
                    for (int i = 0; i < 3; ++i) {
                        velocity[i] = static_cast<float>((position[i] - sync.lastSentPosition[i]) / dt);
                    }
                }
            }
            writer.writeVec3(velocity);
        }

        const size_t packetBytes = writer.size();
        if (config.maxOutboundBytesPerSecond > 0 &&
            budgetWindowBytes + packetBytes >
                static_cast<uint64_t>(config.maxOutboundBytesPerSecond)) {
            ++stats.sendsSkippedByBudget;
            continue;
        }

        if (backend->sendEvent(EventCode::kStateSync, writer.data(),
                               RpcTarget::Others, SendMode::Unreliable)) {
            budgetWindowBytes += packetBytes;
            stats.bytesSent += packetBytes;
            ++stats.packetsSent;
        }

        sync.lastSendTime = clock;
        sync.hasLastSent = true;
        std::copy(position, position + 3, sync.lastSentPosition);
        std::copy(rotation, rotation + 3, sync.lastSentRotation);
        std::copy(scaleValues, scaleValues + 3, sync.lastSentScale);
    }
}

void NetworkSession::handleStateSync(PlayerId, Reader& reader) {
    const NetworkId networkId = reader.readNetworkId();
    const double timestamp = reader.readDouble();
    const uint8_t mask = reader.readUInt8();

    TransformSnapshot snapshot;
    snapshot.timestamp = timestamp;
    if (mask & 0x1) { reader.readVec3(snapshot.position); snapshot.hasPosition = true; }
    if (mask & 0x2) { reader.readVec3(snapshot.rotation); snapshot.hasRotation = true; }
    if (mask & 0x4) { reader.readVec3(snapshot.scale);    snapshot.hasScale = true; }
    if (mask & 0x8) { reader.readVec3(snapshot.velocity); snapshot.hasVelocity = true; }
    if (reader.failed()) {
        fail(ErrorCode::OperationFailed, "Malformed state-sync packet ignored.");
        return;
    }

    auto objectIt = objects.find(networkId);
    if (objectIt == objects.end()) return;
    if (objectIt->second.hasAuthority) return;

    SyncState& sync = syncStates[networkId];

    if (!sync.snapshots.empty() && timestamp <= sync.snapshots.back().timestamp) {
        return;
    }
    sync.snapshots.push_back(snapshot);

    constexpr size_t kMaxSnapshots = 32;
    if (sync.snapshots.size() > kMaxSnapshots) {
        sync.snapshots.erase(sync.snapshots.begin(),
                             sync.snapshots.begin() +
                                 static_cast<std::ptrdiff_t>(sync.snapshots.size() - kMaxSnapshots));
    }
}

void NetworkSession::applyRemoteTransforms(float) {
    if (!scene.valid()) return;

    for (auto& entry : objects) {
        NetworkObject& object = entry.second;
        if (object.hasAuthority || object.rootSceneObjectId < 0) continue;

        auto syncIt = syncStates.find(entry.first);
        if (syncIt == syncStates.end() || syncIt->second.snapshots.empty()) continue;
        SyncState& sync = syncIt->second;
        const SyncSettings& settings = sync.settings;

        SceneObject* obj = findSceneObject(object.rootSceneObjectId);
        if (!obj) continue;

        const TransformSnapshot& newest = sync.snapshots.back();

        auto assign = [&](const TransformSnapshot& s) {
            if (settings.syncPosition && s.hasPosition) {
                obj->position = glm::vec3(s.position[0], s.position[1], s.position[2]);
            }
            if (settings.syncRotation && s.hasRotation) {
                obj->rotation = glm::vec3(s.rotation[0], s.rotation[1], s.rotation[2]);
            }
            if (settings.syncScale && s.hasScale) {
                obj->scale = glm::vec3(s.scale[0], s.scale[1], s.scale[2]);
            }
        };

        if (settings.mode == SyncMode::Snapshot) {
            assign(newest);
            if (sync.snapshots.size() > 1) {
                sync.snapshots.erase(sync.snapshots.begin(),
                                     sync.snapshots.end() - 1);
            }
            continue;
        }

        const double renderTime = newest.timestamp - settings.interpolationDelay;

        const TransformSnapshot* older = nullptr;
        const TransformSnapshot* newer = nullptr;
        for (size_t i = 0; i + 1 < sync.snapshots.size(); ++i) {
            if (sync.snapshots[i].timestamp <= renderTime &&
                sync.snapshots[i + 1].timestamp >= renderTime) {
                older = &sync.snapshots[i];
                newer = &sync.snapshots[i + 1];
                break;
            }
        }

        if (older && newer) {
            const double span = newer->timestamp - older->timestamp;
            const float t = (span > 1e-9)
                ? static_cast<float>((renderTime - older->timestamp) / span)
                : 1.0f;
            TransformSnapshot blended = *newer;
            auto lerp3 = [t](const float* a, const float* b, float* out) {
                for (int i = 0; i < 3; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
            };
            if (older->hasPosition && newer->hasPosition) lerp3(older->position, newer->position, blended.position);
            if (older->hasRotation && newer->hasRotation) lerp3(older->rotation, newer->rotation, blended.rotation);
            if (older->hasScale && newer->hasScale)       lerp3(older->scale, newer->scale, blended.scale);
            assign(blended);
        } else if (settings.mode == SyncMode::Extrapolate && newest.hasVelocity) {
            const double ahead = std::min(static_cast<double>(settings.maxExtrapolation),
                                          std::max(0.0, renderTime - newest.timestamp));
            TransformSnapshot projected = newest;
            for (int i = 0; i < 3; ++i) {
                projected.position[i] = newest.position[i] +
                                        newest.velocity[i] * static_cast<float>(ahead);
            }
            assign(projected);
        } else {
            assign(newest);
        }

        size_t keepFrom = 0;
        for (size_t i = 0; i + 1 < sync.snapshots.size(); ++i) {
            if (sync.snapshots[i + 1].timestamp < renderTime) keepFrom = i + 1;
        }
        if (keepFrom > 0) {
            sync.snapshots.erase(sync.snapshots.begin(),
                                 sync.snapshots.begin() + static_cast<std::ptrdiff_t>(keepFrom));
        }
    }
}

const char* NetworkSession::backendName() const {
    return backend ? backend->name() : "None";
}

bool NetworkSession::connect() { return backend && backend->connect(); }
void NetworkSession::disconnect() { if (backend) backend->disconnect(); }
bool NetworkSession::reconnect() { return backend && backend->reconnect(); }

ConnectionState NetworkSession::state() const {
    return backend ? backend->state() : ConnectionState::Disconnected;
}

int NetworkSession::pingMs() const { return backend ? backend->roundTripTimeMs() : 0; }

const std::string& NetworkSession::region() const {
    static const std::string kNone;
    return backend ? backend->region() : kNone;
}

bool NetworkSession::joinLobby() { return backend && backend->joinLobby(); }
bool NetworkSession::refreshRoomList() { return backend && backend->refreshRoomList(); }

const std::vector<RoomInfo>& NetworkSession::roomList() const {
    static const std::vector<RoomInfo> kEmpty;
    return backend ? backend->roomList() : kEmpty;
}

bool NetworkSession::createRoom(const std::string& name, const RoomOptions& options) {
    return backend && backend->createRoom(name, options);
}
bool NetworkSession::joinRoom(const std::string& name) {
    return backend && backend->joinRoom(name);
}
bool NetworkSession::joinOrCreateRoom(const std::string& name, const RoomOptions& options) {
    return backend && backend->joinOrCreateRoom(name, options);
}
bool NetworkSession::joinRandomRoom() { return backend && backend->joinRandomRoom(); }
bool NetworkSession::leaveRoom() { return backend && backend->leaveRoom(); }

const std::vector<NetworkPlayer>& NetworkSession::players() const {
    static const std::vector<NetworkPlayer> kEmpty;
    return backend ? backend->players() : kEmpty;
}
PlayerId NetworkSession::localPlayerId() const {
    return backend ? backend->localPlayerId() : kInvalidPlayerId;
}
PlayerId NetworkSession::hostPlayerId() const {
    return backend ? backend->hostPlayerId() : kInvalidPlayerId;
}
bool NetworkSession::isHost() const { return backend && backend->isHost(); }
bool NetworkSession::setNickname(const std::string& n) { return backend && backend->setNickname(n); }
bool NetworkSession::setPlayerProperty(const std::string& k, const std::string& v) {
    return backend && backend->setPlayerProperty(k, v);
}
bool NetworkSession::setRoomProperty(const std::string& k, const std::string& v) {
    return backend && backend->setRoomProperty(k, v);
}

NetworkId NetworkSession::allocateNetworkId() { return nextLocalNetworkId++; }

NetworkId NetworkSession::spawn(const std::string& assetId,
                                const float position[3],
                                const float rotation[3],
                                const float scale[3],
                                NetworkId parent,
                                bool spawnDisabled) {
    if (!backend) {
        fail(ErrorCode::BackendUnavailable, "No networking backend.");
        return kInvalidNetworkId;
    }
    if (assetId.empty()) {
        fail(ErrorCode::OperationFailed, "Spawn requires a ModuOBJ asset id.");
        return kInvalidNetworkId;
    }

    SpawnRequest request;
    request.networkId = allocateNetworkId();
    request.assetId = assetId;
    request.instanceId = "minst-" + ModuObj::GenerateAssetId().substr(5);
    request.owner = localPlayerId();
    request.parentNetworkId = parent;
    request.spawnDisabled = spawnDisabled;
    if (position) std::copy(position, position + 3, request.position);
    if (rotation) std::copy(rotation, rotation + 3, request.rotation);
    if (scale) std::copy(scale, scale + 3, request.scale);

    const size_t before = objects.size();
    applySpawn(request, true);
    if (objects.size() == before) {
        return kInvalidNetworkId;
    }

    if (inRoom()) {
        Writer writer;
        writer.writeSpawnRequest(request);
        backend->sendEvent(EventCode::kSpawn, writer.data(),
                           RpcTarget::OthersBuffered, SendMode::Reliable);
    }
    return request.networkId;
}

void NetworkSession::applySpawn(const SpawnRequest& request, bool local) {
    if (!scene.valid()) {
        fail(ErrorCode::OperationFailed, "Spawn without a valid scene bridge.");
        return;
    }
    if (objects.count(request.networkId)) {
        return;
    }

    std::string resolveError;
    const ModuObj::Asset* asset = scene.resolveAsset(request.assetId, resolveError);
    if (!asset) {
        fail(ErrorCode::OperationFailed,
             "Cannot spawn: ModuOBJ asset " + request.assetId + " unavailable. " + resolveError);
        return;
    }

    int parentSceneId = -1;
    if (request.parentNetworkId != kInvalidNetworkId) {
        auto parentIt = objects.find(request.parentNetworkId);
        if (parentIt == objects.end()) {
            fail(ErrorCode::OperationFailed,
                 "Cannot spawn: parent network object not present.");
            return;
        }
        parentSceneId = parentIt->second.rootSceneObjectId;
    }

    const glm::vec3 pos(request.position[0], request.position[1], request.position[2]);
    const glm::vec3 rot(request.rotation[0], request.rotation[1], request.rotation[2]);
    const glm::vec3 scl(request.scale[0], request.scale[1], request.scale[2]);

    ModuObj::InstanceResult result = ModuObj::SpawnIntoScene(
        *asset, *scene.sceneObjects, *scene.nextObjectId,
        parentSceneId, &pos, &rot, &scl, request.spawnDisabled);
    if (!result.success) {
        fail(ErrorCode::OperationFailed, "ModuOBJ spawn failed: " + result.error);
        return;
    }

    NetworkObject object;
    object.networkId = request.networkId;
    object.owner = request.owner;
    object.assetId = request.assetId;
    object.instanceId = result.instanceId;
    object.rootSceneObjectId = result.rootObjectIds.empty() ? -1 : result.rootObjectIds.front();
    object.spawned = true;
    object.hasAuthority = (request.owner == localPlayerId());
    objects.emplace(request.networkId, object);

    if (!local && request.networkId >= nextLocalNetworkId &&
        request.networkId < FirstIdForPlayer(localPlayerId()) + kPerPlayerCapacity) {
        nextLocalNetworkId = request.networkId + 1;
    }

    log(LogLevel::Info,
        "[Network] Spawned network object " + std::to_string(request.networkId) +
        " from " + request.assetId + (local ? " (local)" : " (remote)") +
        ", owner " + std::to_string(request.owner));
    if (onObjectSpawned) onObjectSpawned(request.networkId);
}

bool NetworkSession::despawn(NetworkId networkId) {
    auto it = objects.find(networkId);
    if (it == objects.end()) {
        fail(ErrorCode::OperationFailed, "Despawn: unknown network id.");
        return false;
    }
    if (!it->second.hasAuthority && !isHost()) {
        fail(ErrorCode::NotAuthorized, "Despawn: local peer has no authority over this object.");
        return false;
    }

    if (inRoom() && backend) {
        Writer writer;
        writer.writeNetworkId(networkId);
        backend->sendEvent(EventCode::kDespawn, writer.data(),
                           RpcTarget::OthersBuffered, SendMode::Reliable);
    }
    applyDespawn(networkId);
    return true;
}

void NetworkSession::applyDespawn(NetworkId networkId) {
    auto it = objects.find(networkId);
    if (it == objects.end()) return;

    if (scene.valid() && !it->second.instanceId.empty()) {
        ModuObj::DestroyInstance(it->second.instanceId, *scene.sceneObjects);
    }
    objects.erase(it);
    syncStates.erase(networkId);
    log(LogLevel::Info, "[Network] Despawned network object " + std::to_string(networkId) + ".");
    if (onObjectDespawned) onObjectDespawned(networkId);
}

bool NetworkSession::transferOwnership(NetworkId networkId, PlayerId newOwner) {
    auto it = objects.find(networkId);
    if (it == objects.end()) {
        fail(ErrorCode::OperationFailed, "Ownership transfer: unknown network id.");
        return false;
    }
    if (!it->second.hasAuthority && !isHost()) {
        fail(ErrorCode::NotAuthorized, "Ownership transfer requires ownership or host.");
        return false;
    }

    it->second.owner = newOwner;
    it->second.hasAuthority = (newOwner == localPlayerId());

    if (inRoom() && backend) {
        Writer writer;
        writer.writeNetworkId(networkId);
        writer.writePlayerId(newOwner);
        backend->sendEvent(EventCode::kOwnershipTransfer, writer.data(),
                           RpcTarget::OthersBuffered, SendMode::Reliable);
    }
    log(LogLevel::Info, "[Network] Object " + std::to_string(networkId) +
                        " ownership transferred to player " + std::to_string(newOwner) + ".");
    if (onOwnershipChanged) onOwnershipChanged(networkId, newOwner);
    return true;
}

const NetworkObject* NetworkSession::findObject(NetworkId networkId) const {
    auto it = objects.find(networkId);
    return it == objects.end() ? nullptr : &it->second;
}

NetworkId NetworkSession::findNetworkIdForSceneObject(int sceneObjectId) const {
    for (const auto& entry : objects) {
        if (entry.second.rootSceneObjectId == sceneObjectId) return entry.first;
    }
    return kInvalidNetworkId;
}

std::vector<NetworkId> NetworkSession::allObjects() const {
    std::vector<NetworkId> ids;
    ids.reserve(objects.size());
    for (const auto& entry : objects) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool NetworkSession::hasAuthority(NetworkId networkId) const {
    const NetworkObject* object = findObject(networkId);
    return object && object->hasAuthority;
}

void NetworkSession::registerRpc(const std::string& name, RpcHandler handler) {
    if (name.empty() || !handler) return;
    rpcHandlers[name] = std::move(handler);
}

void NetworkSession::unregisterRpc(const std::string& name) { rpcHandlers.erase(name); }

bool NetworkSession::callRpc(const std::string& name,
                             const Writer& args,
                             RpcTarget target,
                             SendMode mode,
                             PlayerId targetPlayer) {
    if (name.empty()) {
        fail(ErrorCode::OperationFailed, "RPC requires a name.");
        return false;
    }
    if (!backend) {
        fail(ErrorCode::BackendUnavailable, "No networking backend.");
        return false;
    }

    Writer packet;
    packet.writeString(name);
    packet.writeBytes(args.data().data(), args.data().size());

    if (target == RpcTarget::Local) {
        Reader reader(packet.data());
        reader.readString();
        auto it = rpcHandlers.find(name);
        if (it != rpcHandlers.end()) it->second(localPlayerId(), reader);
        return true;
    }
    return backend->sendEvent(EventCode::kRpc, packet.data(), target, mode, targetPlayer);
}

void NetworkSession::handleEvent(PlayerId sender, uint8_t code, const std::vector<uint8_t>& payload) {
    stats.bytesReceived += payload.size();
    ++stats.packetsReceived;

    Reader reader(payload);

    switch (code) {
        case EventCode::kSpawn: {
            const SpawnRequest request = reader.readSpawnRequest();
            if (reader.failed()) {
                fail(ErrorCode::OperationFailed, "Malformed spawn packet ignored.");
                return;
            }
            applySpawn(request, false);
            return;
        }
        case EventCode::kDespawn: {
            const NetworkId id = reader.readNetworkId();
            if (reader.failed()) {
                fail(ErrorCode::OperationFailed, "Malformed despawn packet ignored.");
                return;
            }
            applyDespawn(id);
            return;
        }
        case EventCode::kOwnershipTransfer: {
            const NetworkId id = reader.readNetworkId();
            const PlayerId owner = reader.readPlayerId();
            if (reader.failed()) {
                fail(ErrorCode::OperationFailed, "Malformed ownership packet ignored.");
                return;
            }
            auto it = objects.find(id);
            if (it == objects.end()) return;
            it->second.owner = owner;
            it->second.hasAuthority = (owner == localPlayerId());
            if (onOwnershipChanged) onOwnershipChanged(id, owner);
            return;
        }
        case EventCode::kStateSync: {
            handleStateSync(sender, reader);
            return;
        }
        case EventCode::kRpc: {
            const std::string name = reader.readString();
            if (reader.failed()) {
                fail(ErrorCode::OperationFailed, "Malformed RPC packet ignored.");
                return;
            }
            auto it = rpcHandlers.find(name);
            if (it == rpcHandlers.end()) {
                return;
            }
            it->second(sender, reader);
            return;
        }
        default:
            return;
    }
}

void NetworkSession::fail(ErrorCode code, const std::string& message) {
    error.code = code;
    error.nativeCode = 0;
    error.message = message;
    log(LogLevel::Error, std::string("[Network] ") + ToString(code) + ": " + message);
    if (onError) onError(error);
}

void NetworkSession::log(LogLevel level, const std::string& message) const {
    if (onLog) onLog(level, message);
}

std::string NetworkSession::describeConfig() const {
    std::string out = "backend=";
    out += backendName();
    out += " appIdSet=";
    out += config.appId.empty() ? "no" : "yes";
    out += " appVersion=" + config.appVersion;
    out += " region=" + (config.region.empty() ? std::string("auto") : config.region);
    out += " offline=";
    out += offline ? "yes" : "no";
    return out;
}

}  // namespace Net
