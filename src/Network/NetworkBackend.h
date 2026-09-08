#pragma once

#include "NetworkTypes.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Net {

struct BackendCallbacks {
    std::function<void(ConnectionState)> onStateChanged;
    std::function<void(const NetworkError&)> onError;

    std::function<void(const std::vector<RoomInfo>&)> onRoomListUpdated;
    std::function<void(const std::string& roomName)> onJoinedRoom;
    std::function<void()> onLeftRoom;

    std::function<void(const NetworkPlayer&)> onPlayerJoined;
    std::function<void(PlayerId)> onPlayerLeft;
    std::function<void(PlayerId newHost)> onHostChanged;

    std::function<void(const std::string& key, const std::string& value)> onRoomPropertyChanged;
    std::function<void(PlayerId, const std::string& key, const std::string& value)> onPlayerPropertyChanged;

    std::function<void(PlayerId sender, uint8_t code, const std::vector<uint8_t>&)> onEvent;
};

namespace EventCode {
constexpr uint8_t kSpawn = 1;
constexpr uint8_t kDespawn = 2;
constexpr uint8_t kRpc = 3;
constexpr uint8_t kStateSync = 4;
constexpr uint8_t kOwnershipTransfer = 5;
constexpr uint8_t kUserBase = 64;
}  // namespace EventCode

class NetworkBackend {
public:
    virtual ~NetworkBackend() = default;

    virtual const char* name() const = 0;

    virtual bool initialize(const SessionConfig& config, const BackendCallbacks& callbacks) = 0;
    virtual void shutdown() = 0;

    virtual void tick(float deltaSeconds) = 0;

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool reconnect() = 0;

    virtual ConnectionState state() const = 0;
    virtual int roundTripTimeMs() const = 0;
    virtual const std::string& region() const = 0;

    virtual bool joinLobby() = 0;
    virtual bool leaveLobby() = 0;
    virtual bool refreshRoomList() = 0;
    virtual const std::vector<RoomInfo>& roomList() const = 0;

    virtual bool createRoom(const std::string& name, const RoomOptions& options) = 0;
    virtual bool joinRoom(const std::string& name) = 0;
    virtual bool joinOrCreateRoom(const std::string& name, const RoomOptions& options) = 0;
    virtual bool joinRandomRoom() = 0;
    virtual bool leaveRoom() = 0;

    virtual const std::vector<NetworkPlayer>& players() const = 0;
    virtual PlayerId localPlayerId() const = 0;
    virtual PlayerId hostPlayerId() const = 0;
    virtual bool isHost() const = 0;

    virtual bool setRoomProperty(const std::string& key, const std::string& value) = 0;
    virtual bool setPlayerProperty(const std::string& key, const std::string& value) = 0;
    virtual bool setNickname(const std::string& nickname) = 0;

    virtual bool sendEvent(uint8_t code,
                           const std::vector<uint8_t>& payload,
                           RpcTarget target,
                           SendMode mode,
                           PlayerId targetPlayer = kInvalidPlayerId) = 0;

    virtual const NetworkError& lastError() const = 0;
};

std::unique_ptr<NetworkBackend> CreateLoopbackBackend();

std::unique_ptr<NetworkBackend> CreatePhotonBackend();

bool IsPhotonBackendAvailable();

}  // namespace Net
