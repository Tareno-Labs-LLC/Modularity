#include "NetworkBackend.h"

#include <algorithm>
#include <deque>

namespace Net {
namespace {

class LoopbackBackend final : public NetworkBackend {
public:
    const char* name() const override { return "Offline"; }

    bool initialize(const SessionConfig& config, const BackendCallbacks& callbacks) override {
        cfg = config;
        cb = callbacks;

        localPlayer = NetworkPlayer{};
        localPlayer.id = 1;
        localPlayer.nickname = config.nickname.empty() ? "Player" : config.nickname;
        localPlayer.isLocal = true;
        localPlayer.isHost = true;

        currentRegion = config.region.empty() ? "offline" : config.region;
        error = NetworkError{};
        return true;
    }

    void shutdown() override {
        playerList.clear();
        rooms.clear();
        pending.clear();
        bufferedEvents.clear();
        currentState = ConnectionState::Disconnected;
    }

    void tick(float) override {
        std::deque<std::function<void()>> work;
        work.swap(pending);
        for (auto& fn : work) fn();
    }

    bool connect() override {
        setState(ConnectionState::Connecting);
        queue([this] { setState(ConnectionState::ConnectedToMaster); });
        return true;
    }

    void disconnect() override {
        if (inRoom) leaveRoom();
        queue([this] { setState(ConnectionState::Disconnected); });
    }

    bool reconnect() override {
        disconnect();
        return connect();
    }

    ConnectionState state() const override { return currentState; }
    int roundTripTimeMs() const override { return 0; }
    const std::string& region() const override { return currentRegion; }

    bool joinLobby() override {
        setState(ConnectionState::JoiningLobby);
        queue([this] {
            setState(ConnectionState::InLobby);
            if (cb.onRoomListUpdated) cb.onRoomListUpdated(rooms);
        });
        return true;
    }

    bool leaveLobby() override {
        queue([this] { setState(ConnectionState::ConnectedToMaster); });
        return true;
    }

    bool refreshRoomList() override {
        queue([this] { if (cb.onRoomListUpdated) cb.onRoomListUpdated(rooms); });
        return true;
    }

    const std::vector<RoomInfo>& roomList() const override { return rooms; }

    bool createRoom(const std::string& name, const RoomOptions& options) override {
        if (inRoom) {
            fail(ErrorCode::OperationFailed, "Already in a room.");
            return false;
        }
        if (std::any_of(rooms.begin(), rooms.end(),
                        [&](const RoomInfo& r) { return r.name == name; })) {
            fail(ErrorCode::RoomAlreadyExists, "Room already exists: " + name);
            return false;
        }
        RoomInfo info;
        info.name = name;
        info.maxPlayers = options.maxPlayers;
        info.isOpen = options.isOpen;
        info.isVisible = options.isVisible;
        info.properties = options.properties;
        info.playerCount = 1;
        rooms.push_back(info);
        enterRoom(name);
        return true;
    }

    bool joinRoom(const std::string& name) override {
        auto it = std::find_if(rooms.begin(), rooms.end(),
                               [&](const RoomInfo& r) { return r.name == name; });
        if (it == rooms.end()) {
            fail(ErrorCode::RoomNotFound, "Room not found: " + name);
            return false;
        }
        if (it->maxPlayers > 0 && it->playerCount >= it->maxPlayers) {
            fail(ErrorCode::RoomFull, "Room is full: " + name);
            return false;
        }
        enterRoom(name);
        return true;
    }

    bool joinOrCreateRoom(const std::string& name, const RoomOptions& options) override {
        auto it = std::find_if(rooms.begin(), rooms.end(),
                               [&](const RoomInfo& r) { return r.name == name; });
        return (it != rooms.end()) ? joinRoom(name) : createRoom(name, options);
    }

    bool joinRandomRoom() override {
        if (rooms.empty()) {
            fail(ErrorCode::RoomNotFound, "No rooms available.");
            return false;
        }
        return joinRoom(rooms.front().name);
    }

    bool leaveRoom() override {
        if (!inRoom) {
            fail(ErrorCode::NotInRoom, "Not in a room.");
            return false;
        }
        setState(ConnectionState::LeavingRoom);
        queue([this] {
            inRoom = false;
            currentRoom.clear();
            playerList.clear();
            bufferedEvents.clear();
            setState(ConnectionState::ConnectedToMaster);
            if (cb.onLeftRoom) cb.onLeftRoom();
        });
        return true;
    }

    const std::vector<NetworkPlayer>& players() const override { return playerList; }
    PlayerId localPlayerId() const override { return localPlayer.id; }
    PlayerId hostPlayerId() const override { return inRoom ? localPlayer.id : kInvalidPlayerId; }
    bool isHost() const override { return inRoom; }

    bool setRoomProperty(const std::string& key, const std::string& value) override {
        if (!inRoom) { fail(ErrorCode::NotInRoom, "Not in a room."); return false; }
        auto it = std::find_if(rooms.begin(), rooms.end(),
                               [&](const RoomInfo& r) { return r.name == currentRoom; });
        if (it != rooms.end()) setProperty(it->properties, key, value);
        queue([this, key, value] {
            if (cb.onRoomPropertyChanged) cb.onRoomPropertyChanged(key, value);
        });
        return true;
    }

    bool setPlayerProperty(const std::string& key, const std::string& value) override {
        setProperty(localPlayer.properties, key, value);
        for (NetworkPlayer& p : playerList) {
            if (p.id == localPlayer.id) setProperty(p.properties, key, value);
        }
        const PlayerId id = localPlayer.id;
        queue([this, id, key, value] {
            if (cb.onPlayerPropertyChanged) cb.onPlayerPropertyChanged(id, key, value);
        });
        return true;
    }

    bool setNickname(const std::string& nickname) override {
        localPlayer.nickname = nickname;
        for (NetworkPlayer& p : playerList) {
            if (p.id == localPlayer.id) p.nickname = nickname;
        }
        return true;
    }

    bool sendEvent(uint8_t code,
                   const std::vector<uint8_t>& payload,
                   RpcTarget target,
                   SendMode,
                   PlayerId targetPlayer) override {
        if (!inRoom && target != RpcTarget::Local) {
            fail(ErrorCode::NotInRoom, "Cannot send: not in a room.");
            return false;
        }
        if (IsBuffered(target)) {
            bufferedEvents.push_back({code, payload});
        }
        const bool deliverLocally =
            target == RpcTarget::All || target == RpcTarget::AllBuffered ||
            target == RpcTarget::Local || target == RpcTarget::Host ||
            target == RpcTarget::Server ||
            (target == RpcTarget::Target && targetPlayer == localPlayer.id);
        if (!deliverLocally) return true;

        const PlayerId sender = localPlayer.id;
        queue([this, sender, code, payload] {
            if (cb.onEvent) cb.onEvent(sender, code, payload);
        });
        return true;
    }

    const NetworkError& lastError() const override { return error; }

private:
    struct BufferedEvent {
        uint8_t code;
        std::vector<uint8_t> payload;
    };

    void queue(std::function<void()> fn) { pending.push_back(std::move(fn)); }

    void setState(ConnectionState next) {
        if (currentState == next) return;
        currentState = next;
        if (cb.onStateChanged) cb.onStateChanged(next);
    }

    void enterRoom(const std::string& name) {
        setState(ConnectionState::JoiningRoom);
        queue([this, name] {
            inRoom = true;
            currentRoom = name;
            playerList.clear();
            playerList.push_back(localPlayer);
            error = NetworkError{};
            setState(ConnectionState::InRoom);
            if (cb.onJoinedRoom) cb.onJoinedRoom(name);
            if (cb.onPlayerJoined) cb.onPlayerJoined(localPlayer);
            if (cb.onHostChanged) cb.onHostChanged(localPlayer.id);
        });
    }

    void fail(ErrorCode code, const std::string& message) {
        error.code = code;
        error.nativeCode = 0;
        error.message = message;
        if (cb.onError) cb.onError(error);
    }

    static void setProperty(std::vector<std::pair<std::string, std::string>>& props,
                            const std::string& key, const std::string& value) {
        for (auto& entry : props) {
            if (entry.first == key) { entry.second = value; return; }
        }
        props.emplace_back(key, value);
    }

    SessionConfig cfg;
    BackendCallbacks cb;
    NetworkError error;

    ConnectionState currentState = ConnectionState::Disconnected;
    std::string currentRegion;
    std::string currentRoom;
    bool inRoom = false;

    NetworkPlayer localPlayer;
    std::vector<NetworkPlayer> playerList;
    std::vector<RoomInfo> rooms;
    std::vector<BufferedEvent> bufferedEvents;
    std::deque<std::function<void()>> pending;
};

}  // namespace

std::unique_ptr<NetworkBackend> CreateLoopbackBackend() {
    return std::unique_ptr<NetworkBackend>(new LoopbackBackend());
}

const std::string* NetworkPlayer::findProperty(const std::string& key) const {
    for (const auto& entry : properties) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

const char* ToString(ConnectionState state) {
    switch (state) {
        case ConnectionState::Disconnected:      return "Disconnected";
        case ConnectionState::Connecting:        return "Connecting";
        case ConnectionState::ConnectedToMaster: return "ConnectedToMaster";
        case ConnectionState::JoiningLobby:      return "JoiningLobby";
        case ConnectionState::InLobby:           return "InLobby";
        case ConnectionState::JoiningRoom:       return "JoiningRoom";
        case ConnectionState::InRoom:            return "InRoom";
        case ConnectionState::LeavingRoom:       return "LeavingRoom";
        case ConnectionState::Disconnecting:     return "Disconnecting";
        case ConnectionState::Failed:            return "Failed";
    }
    return "Unknown";
}

const char* ToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::None:                 return "None";
        case ErrorCode::InvalidAppId:         return "InvalidAppId";
        case ErrorCode::InvalidRegion:        return "InvalidRegion";
        case ErrorCode::AuthenticationFailed: return "AuthenticationFailed";
        case ErrorCode::ConnectionTimeout:    return "ConnectionTimeout";
        case ErrorCode::ConnectionLost:       return "ConnectionLost";
        case ErrorCode::RoomNotFound:         return "RoomNotFound";
        case ErrorCode::RoomFull:             return "RoomFull";
        case ErrorCode::RoomAlreadyExists:    return "RoomAlreadyExists";
        case ErrorCode::NotInRoom:            return "NotInRoom";
        case ErrorCode::NotAuthorized:        return "NotAuthorized";
        case ErrorCode::BackendUnavailable:   return "BackendUnavailable";
        case ErrorCode::OperationFailed:      return "OperationFailed";
        case ErrorCode::Unknown:              return "Unknown";
    }
    return "Unknown";
}

const char* ToString(LogLevel level) {
    switch (level) {
        case LogLevel::Info:    return "Info";
        case LogLevel::Warning: return "Warning";
        case LogLevel::Error:   return "Error";
        case LogLevel::Success: return "Success";
    }
    return "Info";
}

const char* ToString(SyncMode mode) {
    switch (mode) {
        case SyncMode::Snapshot:    return "Snapshot";
        case SyncMode::Interpolate: return "Interpolate";
        case SyncMode::Extrapolate: return "Extrapolate";
    }
    return "Unknown";
}

const char* ToString(RpcTarget target) {
    switch (target) {
        case RpcTarget::All:            return "All";
        case RpcTarget::Others:         return "Others";
        case RpcTarget::Host:           return "Host";
        case RpcTarget::Server:         return "Server";
        case RpcTarget::Target:         return "Target";
        case RpcTarget::Local:          return "Local";
        case RpcTarget::AllBuffered:    return "AllBuffered";
        case RpcTarget::OthersBuffered: return "OthersBuffered";
    }
    return "Unknown";
}

}  // namespace Net
