
#include "NetworkBackend.h"

#if MODULARITY_ENABLE_PHOTON

#include "LoadBalancing-cpp/inc/Client.h"
#include "LoadBalancing-cpp/inc/Listener.h"

#include <algorithm>
#include <deque>
#include <mutex>

namespace Net {
namespace {

using ExitGames::Common::JString;
using ExitGames::Common::JVector;

std::string ToStd(const JString& value) {
    return std::string(value.UTF8Representation().cstr());
}

JString ToJ(const std::string& value) {
    std::wstring wide;
    wide.reserve(value.size());
    for (size_t i = 0; i < value.size();) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        uint32_t cp = 0;
        size_t extra = 0;
        if (c < 0x80)            { cp = c;          extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { ++i; continue; }
        if (i + extra >= value.size() + 0 && extra > 0 && i + extra > value.size() - 1) {
            break;
        }
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(value[i + k]);
            if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        wide.push_back(static_cast<wchar_t>(cp));
        i += extra + 1;
    }
    return JString(wide.c_str());
}

ErrorCode ClassifyPhotonError(int code) {
    namespace App = ExitGames::LoadBalancing::ErrorCode::ApplicationLayer;
    namespace Core = ExitGames::LoadBalancing::ErrorCode::Core;
    switch (code) {
        case App::GAME_ID_ALREADY_EXISTS:        return ErrorCode::RoomAlreadyExists;
        case App::GAME_FULL:                     return ErrorCode::RoomFull;
        case App::GAME_CLOSED:                   return ErrorCode::RoomNotFound;
        case App::GAME_DOES_NOT_EXIST:           return ErrorCode::RoomNotFound;
        case App::NO_MATCH_FOUND:                return ErrorCode::RoomNotFound;
        case App::INVALID_AUTHENTICATION:        return ErrorCode::InvalidAppId;
        case App::CUSTOM_AUTHENTICATION_FAILED:  return ErrorCode::AuthenticationFailed;
        case App::AUTHENTICATION_TICKET_EXPIRED: return ErrorCode::AuthenticationFailed;
        case App::MAX_CCU_REACHED:               return ErrorCode::NotAuthorized;
        case App::SERVER_FULL:                   return ErrorCode::NotAuthorized;
        case App::USER_BLOCKED:                  return ErrorCode::NotAuthorized;
        case App::INVALID_REGION:                return ErrorCode::InvalidRegion;
        case Core::OPERATION_DENIED:             return ErrorCode::NotAuthorized;
        case Core::OPERATION_INVALID:            return ErrorCode::OperationFailed;
        case Core::INTERNAL_SERVER_ERROR:        return ErrorCode::OperationFailed;
        default:                                 return ErrorCode::Unknown;
    }
}

class PhotonBackend final : public NetworkBackend,
                            private ExitGames::LoadBalancing::Listener {
public:
    const char* name() const override { return "Photon"; }

    bool initialize(const SessionConfig& config, const BackendCallbacks& callbacks) override {
        cfg = config;
        cb = callbacks;
        error = NetworkError{};

        if (cfg.appId.empty()) {
            setError(Net::ErrorCode::InvalidAppId, 0, "Photon App ID is not configured.");
            return false;
        }

        client.reset(new ExitGames::LoadBalancing::Client(
            *this, ToJ(cfg.appId), ToJ(cfg.appVersion)));
        client->setAutoJoinLobby(false);
        return true;
    }

    void shutdown() override {
        if (client) {
            if (client->getIsInGameRoom()) client->opLeaveRoom();
            client->disconnect();
            for (int i = 0; i < 10 && client->getState() != 0; ++i) client->service();
            client.reset();
        }
        drainQueue();
        playerList.clear();
        rooms.clear();
    }

    void tick(float) override {
        if (client) client->service();
        drainQueue();
    }

    bool connect() override {
        if (!client) return false;
        setState(ConnectionState::Connecting);
        ExitGames::LoadBalancing::ConnectOptions options;
        if (!cfg.nickname.empty()) options.setUsername(ToJ(cfg.nickname));
        if (!client->connect(options)) {
            setError(Net::ErrorCode::OperationFailed, 0, "Photon connect() was rejected.");
            setState(ConnectionState::Failed);
            return false;
        }
        return true;
    }

    void disconnect() override {
        if (!client) return;
        setState(ConnectionState::Disconnecting);
        client->disconnect();
    }

    bool reconnect() override {
        if (!client) return false;
        setState(ConnectionState::Connecting);
        return client->reconnectAndRejoin() || client->connect();
    }

    ConnectionState state() const override { return currentState; }
    int roundTripTimeMs() const override { return client ? client->getRoundTripTime() : 0; }
    const std::string& region() const override { return currentRegion; }

    bool joinLobby() override {
        if (!client) return false;
        setState(ConnectionState::JoiningLobby);
        return client->opJoinLobby();
    }

    bool leaveLobby() override { return client && client->opLeaveLobby(); }

    bool refreshRoomList() override {
        queue([this] { if (cb.onRoomListUpdated) cb.onRoomListUpdated(rooms); });
        return true;
    }

    const std::vector<RoomInfo>& roomList() const override { return rooms; }

    bool createRoom(const std::string& name, const RoomOptions& options) override {
        if (!client) return false;
        setState(ConnectionState::JoiningRoom);
        return client->opCreateRoom(ToJ(name), makeRoomOptions(options));
    }

    bool joinRoom(const std::string& name) override {
        if (!client) return false;
        setState(ConnectionState::JoiningRoom);
        return client->opJoinRoom(ToJ(name));
    }

    bool joinOrCreateRoom(const std::string& name, const RoomOptions& options) override {
        if (!client) return false;
        setState(ConnectionState::JoiningRoom);
        return client->opJoinOrCreateRoom(ToJ(name), makeRoomOptions(options));
    }

    bool joinRandomRoom() override {
        if (!client) return false;
        setState(ConnectionState::JoiningRoom);
        return client->opJoinRandomRoom();
    }

    bool leaveRoom() override {
        if (!client) return false;
        setState(ConnectionState::LeavingRoom);
        return client->opLeaveRoom();
    }

    const std::vector<NetworkPlayer>& players() const override { return playerList; }
    PlayerId localPlayerId() const override {
        return client ? static_cast<PlayerId>(client->getLocalPlayer().getNumber()) : kInvalidPlayerId;
    }
    PlayerId hostPlayerId() const override {
        if (!client || !client->getIsInGameRoom()) return kInvalidPlayerId;
        return static_cast<PlayerId>(client->getCurrentlyJoinedRoom().getMasterClientID());
    }
    bool isHost() const override {
        return client && client->getIsInGameRoom() && localPlayerId() == hostPlayerId();
    }

    bool setRoomProperty(const std::string& key, const std::string& value) override {
        if (!client || !client->getIsInGameRoom()) return false;
        ExitGames::Common::Hashtable props;
        props.put(ToJ(key), ToJ(value));
        client->getCurrentlyJoinedRoom().addCustomProperties(props);
        return true;
    }

    bool setPlayerProperty(const std::string& key, const std::string& value) override {
        if (!client) return false;
        ExitGames::Common::Hashtable props;
        props.put(ToJ(key), ToJ(value));
        client->getLocalPlayer().addCustomProperties(props);
        return true;
    }

    bool setNickname(const std::string& nickname) override {
        if (!client) return false;
        client->getLocalPlayer().setName(ToJ(nickname));
        cfg.nickname = nickname;
        return true;
    }

    bool sendEvent(uint8_t code,
                   const std::vector<uint8_t>& payload,
                   RpcTarget target,
                   SendMode mode,
                   PlayerId targetPlayer) override {
        if (target == RpcTarget::Local) {
            const PlayerId self = localPlayerId();
            std::vector<uint8_t> copy = payload;
            queue([this, self, code, copy] { if (cb.onEvent) cb.onEvent(self, code, copy); });
            return true;
        }
        if (!client || !client->getIsInGameRoom()) {
            setError(Net::ErrorCode::NotInRoom, 0, "Cannot send: not in a room.");
            return false;
        }

        std::vector<nByte> bytes(payload.begin(), payload.end());

        ExitGames::LoadBalancing::RaiseEventOptions options;
        switch (target) {
            case RpcTarget::All:
            case RpcTarget::AllBuffered:
                options.setReceiverGroup(ExitGames::Lite::ReceiverGroup::ALL);
                break;
            case RpcTarget::Host:
            case RpcTarget::Server:
                options.setReceiverGroup(ExitGames::Lite::ReceiverGroup::MASTER_CLIENT);
                break;
            case RpcTarget::Target: {
                int targets[1] = { static_cast<int>(targetPlayer) };
                options.setTargetPlayers(targets, 1);
                break;
            }
            case RpcTarget::Others:
            case RpcTarget::OthersBuffered:
            default:
                options.setReceiverGroup(ExitGames::Lite::ReceiverGroup::OTHERS);
                break;
        }
        if (IsBuffered(target)) {
            options.setEventCaching(ExitGames::Lite::EventCache::ADD_TO_ROOM_CACHE);
        }

        const bool reliable = (mode == SendMode::Reliable);
        if (bytes.empty()) bytes.push_back(0);
        return client->opRaiseEvent(reliable, bytes.data(), static_cast<int>(bytes.size()),
                                    static_cast<nByte>(code), options);
    }

    const NetworkError& lastError() const override { return error; }

private:

    void debugReturn(int, const JString&) override {}

    void connectionErrorReturn(int errorCode) override {
        setError(Net::ErrorCode::ConnectionLost, errorCode, "Photon connection error.");
        setState(ConnectionState::Failed);
    }

    void clientErrorReturn(int errorCode) override {
        setError(ClassifyPhotonError(errorCode), errorCode, "Photon client error.");
    }

    void warningReturn(int) override {}

    void serverErrorReturn(int errorCode) override {
        setError(ClassifyPhotonError(errorCode), errorCode, "Photon server error.");
    }

    void joinRoomEventAction(int playerNr, const JVector<int>&,
                             const ExitGames::LoadBalancing::Player& player) override {
        NetworkPlayer entry;
        entry.id = static_cast<PlayerId>(playerNr);
        entry.nickname = ToStd(player.getName());
        entry.isLocal = (playerNr == localPlayerId());
        entry.isHost = (playerNr == hostPlayerId());
        upsertPlayer(entry);
        queue([this, entry] { if (cb.onPlayerJoined) cb.onPlayerJoined(entry); });
    }

    void leaveRoomEventAction(int playerNr, bool) override {
        playerList.erase(std::remove_if(playerList.begin(), playerList.end(),
                                        [&](const NetworkPlayer& p) { return p.id == playerNr; }),
                         playerList.end());
        const PlayerId host = hostPlayerId();
        queue([this, playerNr, host] {
            if (cb.onPlayerLeft) cb.onPlayerLeft(static_cast<PlayerId>(playerNr));
            if (cb.onHostChanged) cb.onHostChanged(host);
        });
    }

    void customEventAction(int playerNr, nByte eventCode,
                           const ExitGames::Common::Object& eventContent) override {
        if (eventContent.getType() != ExitGames::Common::TypeCode::BYTEARRAY) return;
        const auto& array =
            static_cast<const ExitGames::Common::ValueObject<nByte*>&>(eventContent);
        const nByte* data = array.getDataCopy();
        const int size = *array.getSizes();
        std::vector<uint8_t> payload(data, data + size);
        ExitGames::Common::MemoryManagement::deallocateArray(data);

        const PlayerId sender = static_cast<PlayerId>(playerNr);
        const uint8_t code = static_cast<uint8_t>(eventCode);
        queue([this, sender, code, payload] {
            if (cb.onEvent) cb.onEvent(sender, code, payload);
        });
    }

    void connectReturn(int errorCode, const JString& errorString,
                       const JString& regionValue, const JString&) override {
        if (errorCode) {
            setError(ClassifyPhotonError(errorCode), errorCode,
                     "Photon connect failed: " + ToStd(errorString));
            setState(ConnectionState::Failed);
            return;
        }
        currentRegion = ToStd(regionValue);
        setState(ConnectionState::ConnectedToMaster);
    }

    void disconnectReturn(void) override { setState(ConnectionState::Disconnected); }

    void leaveRoomReturn(int errorCode, const JString& errorString) override {
        if (errorCode) {
            setError(ClassifyPhotonError(errorCode), errorCode,
                     "Photon leave room failed: " + ToStd(errorString));
        }
        playerList.clear();
        setState(ConnectionState::ConnectedToMaster);
        queue([this] { if (cb.onLeftRoom) cb.onLeftRoom(); });
    }

    void joinLobbyReturn(void) override { setState(ConnectionState::InLobby); }
    void leaveLobbyReturn(void) override { setState(ConnectionState::ConnectedToMaster); }

    void createRoomReturn(int, const ExitGames::Common::Hashtable&,
                          const ExitGames::Common::Hashtable&,
                          int errorCode, const JString& errorString) override {
        finishJoin(errorCode, errorString);
    }

    void joinOrCreateRoomReturn(int, const ExitGames::Common::Hashtable&,
                                const ExitGames::Common::Hashtable&,
                                int errorCode, const JString& errorString) override {
        finishJoin(errorCode, errorString);
    }

    void joinRoomReturn(int, const ExitGames::Common::Hashtable&,
                        const ExitGames::Common::Hashtable&,
                        int errorCode, const JString& errorString) override {
        finishJoin(errorCode, errorString);
    }

    void joinRandomRoomReturn(int, const ExitGames::Common::Hashtable&,
                              const ExitGames::Common::Hashtable&,
                              int errorCode, const JString& errorString) override {
        finishJoin(errorCode, errorString);
    }

    void onRoomListUpdate(void) override { rebuildRoomList(); }
    void onLobbyStatsUpdate(const JVector<ExitGames::LoadBalancing::LobbyStatsResponse>&) override {}

    void onMasterClientChanged(int id, int) override {
        const PlayerId host = static_cast<PlayerId>(id);
        for (NetworkPlayer& p : playerList) p.isHost = (p.id == host);
        queue([this, host] { if (cb.onHostChanged) cb.onHostChanged(host); });
    }

    void onRoomPropertiesChange(const ExitGames::Common::Hashtable& changes) override {
        forEachStringPair(changes, [this](const std::string& key, const std::string& value) {
            queue([this, key, value] {
                if (cb.onRoomPropertyChanged) cb.onRoomPropertyChanged(key, value);
            });
        });
    }

    void onPlayerPropertiesChange(int playerNr, const ExitGames::Common::Hashtable& changes) override {
        const PlayerId id = static_cast<PlayerId>(playerNr);
        forEachStringPair(changes, [this, id](const std::string& key, const std::string& value) {
            queue([this, id, key, value] {
                if (cb.onPlayerPropertyChanged) cb.onPlayerPropertyChanged(id, key, value);
            });
        });
    }

    void finishJoin(int errorCode, const JString& errorString) {
        if (errorCode) {
            setError(ClassifyPhotonError(errorCode), errorCode,
                     "Photon room operation failed: " + ToStd(errorString));
            setState(ConnectionState::InLobby);
            return;
        }
        rebuildPlayerList();
        setState(ConnectionState::InRoom);
        std::string roomName;
        if (client && client->getIsInGameRoom()) {
            roomName = ToStd(client->getCurrentlyJoinedRoom().getName());
        }
        queue([this, roomName] { if (cb.onJoinedRoom) cb.onJoinedRoom(roomName); });
    }

    void rebuildPlayerList() {
        playerList.clear();
        if (!client || !client->getIsInGameRoom()) return;
        const auto& players = client->getCurrentlyJoinedRoom().getPlayers();
        const PlayerId host = hostPlayerId();
        const PlayerId self = localPlayerId();
        for (unsigned int i = 0; i < players.getSize(); ++i) {
            NetworkPlayer entry;
            entry.id = static_cast<PlayerId>(players[i]->getNumber());
            entry.nickname = ToStd(players[i]->getName());
            entry.isLocal = (entry.id == self);
            entry.isHost = (entry.id == host);
            playerList.push_back(entry);
        }
    }

    void rebuildRoomList() {
        rooms.clear();
        if (!client) return;
        const auto& list = client->getRoomList();
        for (unsigned int i = 0; i < list.getSize(); ++i) {
            RoomInfo info;
            info.name = ToStd(list[i]->getName());
            info.playerCount = list[i]->getPlayerCount();
            info.maxPlayers = list[i]->getMaxPlayers();
            info.isOpen = list[i]->getIsOpen();
            rooms.push_back(info);
        }
        queue([this] { if (cb.onRoomListUpdated) cb.onRoomListUpdated(rooms); });
    }

    void upsertPlayer(const NetworkPlayer& entry) {
        for (NetworkPlayer& p : playerList) {
            if (p.id == entry.id) { p = entry; return; }
        }
        playerList.push_back(entry);
    }

    ExitGames::LoadBalancing::RoomOptions makeRoomOptions(const RoomOptions& options) const {
        ExitGames::LoadBalancing::RoomOptions photonOptions;
        photonOptions.setMaxPlayers(static_cast<nByte>(std::max(0, options.maxPlayers)));
        photonOptions.setIsOpen(options.isOpen);
        photonOptions.setIsVisible(options.isVisible);
        if (!options.properties.empty()) {
            ExitGames::Common::Hashtable props;
            for (const auto& entry : options.properties) {
                props.put(ToJ(entry.first), ToJ(entry.second));
            }
            photonOptions.setCustomRoomProperties(props);
        }
        if (!options.lobbyVisibleProperties.empty()) {
            JVector<JString> keys;
            for (const std::string& key : options.lobbyVisibleProperties) keys.addElement(ToJ(key));
            photonOptions.setPropsListedInLobby(keys);
        }
        return photonOptions;
    }

    template <typename Fn>
    static void forEachStringPair(const ExitGames::Common::Hashtable& table, Fn&& fn) {
        const JVector<ExitGames::Common::Object>& keys = table.getKeys();
        for (unsigned int i = 0; i < keys.getSize(); ++i) {
            if (keys[i].getType() != ExitGames::Common::TypeCode::STRING) continue;
            const JString key =
                ExitGames::Common::ValueObject<JString>(keys[i]).getDataCopy();
            const ExitGames::Common::Object* value = table.getValue(key);
            if (!value || value->getType() != ExitGames::Common::TypeCode::STRING) continue;
            const JString text =
                ExitGames::Common::ValueObject<JString>(*value).getDataCopy();
            fn(ToStd(key), ToStd(text));
        }
    }

    void setState(ConnectionState next) {
        if (currentState == next) return;
        currentState = next;
        queue([this, next] { if (cb.onStateChanged) cb.onStateChanged(next); });
    }

    void setError(Net::ErrorCode code, int nativeCode, const std::string& message) {
        error.code = code;
        error.nativeCode = nativeCode;
        error.message = message;
        NetworkError copy = error;
        queue([this, copy] { if (cb.onError) cb.onError(copy); });
    }

    void queue(std::function<void()> fn) { pending.push_back(std::move(fn)); }

    void drainQueue() {
        std::deque<std::function<void()>> work;
        work.swap(pending);
        for (auto& fn : work) fn();
    }

    std::unique_ptr<ExitGames::LoadBalancing::Client> client;
    SessionConfig cfg;
    BackendCallbacks cb;
    NetworkError error;

    ConnectionState currentState = ConnectionState::Disconnected;
    std::string currentRegion;
    std::vector<NetworkPlayer> playerList;
    std::vector<RoomInfo> rooms;
    std::deque<std::function<void()>> pending;
};

}  // namespace

std::unique_ptr<NetworkBackend> CreatePhotonBackend() {
    return std::unique_ptr<NetworkBackend>(new PhotonBackend());
}

bool IsPhotonBackendAvailable() { return true; }

}  // namespace Net

#else

namespace Net {

std::unique_ptr<NetworkBackend> CreatePhotonBackend() { return nullptr; }

bool IsPhotonBackendAvailable() { return false; }

}  // namespace Net

#endif  // MODULARITY_ENABLE_PHOTON
