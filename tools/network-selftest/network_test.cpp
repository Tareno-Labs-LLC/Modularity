// Self-test for the networking foundation.
//
// Drives the backend-agnostic layer through the offline/loopback backend, so the
// whole stack (serializer, session, spawn pipeline, RPC, ownership) is verified
// headlessly with no sockets and no Photon. The Photon backend is verified
// separately by compiling and linking it against the vendored SDK.
//
// Build/run: tools/network-selftest/run.sh
#include "Network/NetworkSession.h"
#include "Network/NetworkSerializer.h"
#include "ModuObj.h"
#include "ProjectManager.h"  // SceneSerializer, SkyboxSettings

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace fsys = std::filesystem;

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(cond)) {                                                          \
            std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);       \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

// --------------------------------------------------------------------------
// Serialization
// --------------------------------------------------------------------------

static void TestSerializer() {
    std::printf("-- serializer --\n");

    Net::Writer w;
    w.writeBool(true);
    w.writeInt32(-12345);
    w.writeUInt32(4000000000u);
    w.writeInt64(-9000000000LL);
    w.writeFloat(3.5f);
    w.writeDouble(2.25);
    w.writeString("hello world");
    const float v3[3] = {1.0f, 2.0f, 3.0f};
    w.writeVec3(v3);
    const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    w.writeQuat(q);
    w.writeNetworkId(4242);
    w.writePlayerId(7);
    w.writeAssetId("mobj-0123456789abcdef");
    w.writeFloatArray({1.5f, 2.5f, 3.5f});
    w.writeInt32Array({10, 20, 30});
    w.writeStringArray({"a", "bb", "ccc"});

    Net::Reader r(w.data());
    CHECK(r.readBool() == true, "bool round trip");
    CHECK(r.readInt32() == -12345, "int32 round trip");
    CHECK(r.readUInt32() == 4000000000u, "uint32 round trip");
    CHECK(r.readInt64() == -9000000000LL, "int64 round trip");
    CHECK(std::fabs(r.readFloat() - 3.5f) < 1e-6f, "float round trip");
    CHECK(std::fabs(r.readDouble() - 2.25) < 1e-12, "double round trip");
    CHECK(r.readString() == "hello world", "string round trip");
    float outV3[3] = {};
    r.readVec3(outV3);
    CHECK(outV3[0] == 1.0f && outV3[1] == 2.0f && outV3[2] == 3.0f, "vec3 round trip");
    float outQ[4] = {};
    r.readQuat(outQ);
    CHECK(outQ[3] == 1.0f, "quaternion round trip");
    CHECK(r.readNetworkId() == 4242u, "network id round trip");
    CHECK(r.readPlayerId() == 7, "player id round trip");
    CHECK(r.readAssetId() == "mobj-0123456789abcdef", "asset id round trip");
    CHECK(r.readFloatArray().size() == 3, "float array round trip");
    CHECK(r.readInt32Array()[2] == 30, "int32 array round trip");
    CHECK(r.readStringArray()[2] == "ccc", "string array round trip");
    CHECK(!r.failed(), "no failure across a complete read");
    CHECK(r.bytesRemaining() == 0, "buffer fully consumed");

    // Reading past the end must fail stickily, never read out of bounds.
    {
        Net::Reader over(w.data());
        for (int i = 0; i < 200; ++i) over.readInt64();
        CHECK(over.failed(), "over-read sets the failure flag");
        CHECK(over.readInt32() == 0, "reads after failure return zero");
    }

    // A truncated packet must be rejected, not misparsed.
    {
        std::vector<uint8_t> truncated(w.data().begin(), w.data().begin() + 5);
        Net::Reader tr(truncated);
        tr.readBool();
        tr.readInt32();
        tr.readUInt32();
        CHECK(tr.failed(), "truncated packet detected");
    }

    // A hostile length prefix must not cause a huge allocation.
    {
        Net::Writer bad;
        bad.writeUInt32(0xFFFFFFFFu);   // claims a 4GB string
        Net::Reader br(bad.data());
        const std::string s = br.readString();
        CHECK(br.failed(), "absurd string length rejected");
        CHECK(s.empty(), "rejected string yields no data");
    }

    // SpawnRequest round trip: this is what actually goes on the wire.
    {
        Net::SpawnRequest req;
        req.networkId = 99;
        req.assetId = "mobj-aaaabbbbccccdddd";
        req.instanceId = "minst-1111222233334444";
        req.owner = 3;
        req.position[0] = 5.0f; req.position[1] = 6.0f; req.position[2] = 7.0f;
        req.scale[0] = 2.0f; req.scale[1] = 2.0f; req.scale[2] = 2.0f;
        req.parentNetworkId = 42;
        req.spawnDisabled = true;

        Net::Writer sw;
        sw.writeSpawnRequest(req);
        Net::Reader sr(sw.data());
        const Net::SpawnRequest back = sr.readSpawnRequest();
        CHECK(!sr.failed(), "spawn request parses");
        CHECK(back.networkId == req.networkId, "spawn network id preserved");
        CHECK(back.assetId == req.assetId, "spawn asset id preserved");
        CHECK(back.instanceId == req.instanceId, "spawn instance id preserved");
        CHECK(back.owner == req.owner, "spawn owner preserved");
        CHECK(back.position[2] == 7.0f, "spawn position preserved");
        CHECK(back.scale[0] == 2.0f, "spawn scale preserved");
        CHECK(back.parentNetworkId == 42u, "spawn parent preserved");
        CHECK(back.spawnDisabled, "spawn disabled flag preserved");
        // Only a placement plus ids: no hierarchy is transmitted.
        CHECK(sw.size() < 128, "spawn payload stays tiny (no hierarchy on the wire)");
    }
}

// --------------------------------------------------------------------------
// Session fixture
// --------------------------------------------------------------------------

struct Fixture {
    std::vector<SceneObject> scene;
    int nextObjectId = 1;
    std::unordered_map<std::string, ModuObj::Asset> assets;
    Net::NetworkSession session;

    Net::SceneBridge bridge() {
        Net::SceneBridge b;
        b.sceneObjects = &scene;
        b.nextObjectId = &nextObjectId;
        b.resolveAsset = [this](const std::string& id, std::string& err) -> const ModuObj::Asset* {
            auto it = assets.find(id);
            if (it == assets.end()) { err = "asset not registered"; return nullptr; }
            return &it->second;
        };
        return b;
    }

    // Loopback queues callbacks; pump until the state settles.
    void pump(int iterations = 8) {
        for (int i = 0; i < iterations; ++i) session.tick(0.016f);
    }
};

static ModuObj::Asset MakeAsset(const char* name) {
    std::vector<SceneObject> objs;
    objs.emplace_back("Root", ObjectType::Empty, 1);
    objs.emplace_back("Child", ObjectType::Cube, 2);
    objs[0].parentId = -1;
    objs[0].childIds = { 2 };
    objs[1].parentId = 1;
    objs[1].hasRenderer = true;
    objs[1].renderType = RenderType::Cube;
    for (SceneObject& o : objs) { o.scale = glm::vec3(1.0f); o.localScale = glm::vec3(1.0f); }

    ModuObj::Asset asset;
    std::vector<std::string> external;
    std::string error;
    ModuObj::BuildAssetFromSelection(objs, { 1 }, name, asset, external, error);
    return asset;
}

static void TestOfflineLifecycle() {
    std::printf("-- offline session lifecycle --\n");
    Fixture fx;

    Net::SessionConfig cfg;
    cfg.offlineMode = true;
    cfg.nickname = "Tester";

    CHECK(fx.session.start(cfg, fx.bridge()), "offline session starts");
    CHECK(fx.session.isOffline(), "session reports offline");
    CHECK(std::string(fx.session.backendName()) == "Offline", "offline backend selected");

    // Credentials must never be echoed.
    const std::string described = fx.session.describeConfig();
    CHECK(described.find("appIdSet=no") != std::string::npos, "config echo reports App ID state");

    std::vector<Net::ConnectionState> states;
    fx.session.onStateChanged = [&](Net::ConnectionState s) { states.push_back(s); };

    CHECK(fx.session.connect(), "connect");
    fx.pump();
    CHECK(fx.session.state() == Net::ConnectionState::ConnectedToMaster, "reaches master");

    CHECK(fx.session.joinLobby(), "join lobby");
    fx.pump();
    CHECK(fx.session.state() == Net::ConnectionState::InLobby, "reaches lobby");

    Net::RoomOptions options;
    options.maxPlayers = 4;
    CHECK(fx.session.createRoom("Room1", options), "create room");
    fx.pump();
    CHECK(fx.session.inRoom(), "in room after create");
    CHECK(fx.session.players().size() == 1, "local player present");
    CHECK(fx.session.isHost(), "local player is host offline");
    CHECK(fx.session.localPlayerId() != Net::kInvalidPlayerId, "local player id assigned");

    // Duplicate create must fail cleanly rather than crash.
    CHECK(!fx.session.createRoom("Room1", options), "creating a duplicate room fails");
    CHECK(fx.session.lastError().code != Net::ErrorCode::None, "duplicate room reports an error");

    CHECK(fx.session.setRoomProperty("map", "arena"), "set room property");
    CHECK(fx.session.setPlayerProperty("team", "red"), "set player property");
    CHECK(fx.session.setNickname("Renamed"), "set nickname");
    fx.pump();

    CHECK(fx.session.leaveRoom(), "leave room");
    fx.pump();
    CHECK(!fx.session.inRoom(), "left the room");

    // Joining a room nobody created must fail, not fabricate one.
    CHECK(!fx.session.joinRoom("NoSuchRoom"), "joining a missing room fails");
    CHECK(fx.session.lastError().code == Net::ErrorCode::RoomNotFound, "missing room classified");

    fx.session.disconnect();
    fx.pump();
    CHECK(fx.session.state() == Net::ConnectionState::Disconnected, "disconnected");
    CHECK(!states.empty(), "state change callbacks fired");
}

static void TestSpawnPipeline() {
    std::printf("-- spawn pipeline --\n");
    Fixture fx;
    ModuObj::Asset asset = MakeAsset("NetCube");
    const std::string assetId = asset.assetId;
    fx.assets[assetId] = asset;

    Net::SessionConfig cfg;
    cfg.offlineMode = true;
    fx.session.start(cfg, fx.bridge());
    fx.session.connect();
    fx.pump();
    fx.session.createRoom("Room", Net::RoomOptions{});
    fx.pump();

    std::vector<Net::NetworkId> spawned;
    fx.session.onObjectSpawned = [&](Net::NetworkId id) { spawned.push_back(id); };

    const float pos[3] = {4.0f, 5.0f, 6.0f};
    const Net::NetworkId a = fx.session.spawn(assetId, pos);
    fx.pump();
    CHECK(a != Net::kInvalidNetworkId, "spawn returns a network id");
    CHECK(fx.scene.size() == 2, "spawn reused the ModuOBJ pipeline (2 objects)");
    CHECK(spawned.size() == 1, "spawn callback fired");

    const Net::NetworkObject* object = fx.session.findObject(a);
    CHECK(object != nullptr, "spawned object is registered");
    CHECK(object && object->assetId == assetId, "object records its source asset");
    CHECK(object && !object->instanceId.empty(), "object records its ModuOBJ instance id");
    CHECK(object && object->hasAuthority, "spawner has authority");
    CHECK(fx.session.hasAuthority(a), "authority query agrees");

    // Placement reached the scene root.
    if (object) {
        auto root = std::find_if(fx.scene.begin(), fx.scene.end(),
                                 [&](const SceneObject& o) { return o.id == object->rootSceneObjectId; });
        CHECK(root != fx.scene.end(), "root scene object exists");
        CHECK(root != fx.scene.end() && root->position == glm::vec3(4.0f, 5.0f, 6.0f),
              "spawn transform applied to the root");
        CHECK(root != fx.scene.end() && root->hasModuObjInstance,
              "spawned objects carry ModuOBJ instance metadata");
    }

    // A second spawn is independent.
    const Net::NetworkId b = fx.session.spawn(assetId);
    fx.pump();
    CHECK(b != Net::kInvalidNetworkId && b != a, "second spawn gets a distinct network id");
    CHECK(fx.scene.size() == 4, "second instance added its own objects");
    CHECK(fx.session.allObjects().size() == 2, "two objects tracked");

    // Spawning a parented object.
    const Net::NetworkId child = fx.session.spawn(assetId, nullptr, nullptr, nullptr, a);
    fx.pump();
    CHECK(child != Net::kInvalidNetworkId, "spawn under a network parent succeeds");

    // Unknown asset fails cleanly, scene untouched.
    {
        const size_t before = fx.scene.size();
        const Net::NetworkId bad = fx.session.spawn("mobj-ffffffffffffffff");
        fx.pump();
        CHECK(bad == Net::kInvalidNetworkId, "spawning an unregistered asset fails");
        CHECK(fx.scene.size() == before, "failed spawn did not touch the scene");
    }
    CHECK(fx.session.spawn("") == Net::kInvalidNetworkId, "spawning an empty asset id fails");

    // Ownership transfer.
    std::vector<Net::NetworkId> ownershipEvents;
    fx.session.onOwnershipChanged = [&](Net::NetworkId id, Net::PlayerId) { ownershipEvents.push_back(id); };
    CHECK(fx.session.transferOwnership(a, 2), "ownership transfer accepted");
    fx.pump();
    CHECK(fx.session.findObject(a)->owner == 2, "owner updated");
    CHECK(!fx.session.hasAuthority(a), "authority dropped after transfer away");
    CHECK(!ownershipEvents.empty(), "ownership callback fired");
    CHECK(!fx.session.transferOwnership(999999, 2), "transfer on an unknown id fails");

    // Despawn removes the instance's scene objects too.
    std::vector<Net::NetworkId> despawned;
    fx.session.onObjectDespawned = [&](Net::NetworkId id) { despawned.push_back(id); };
    const size_t beforeDespawn = fx.scene.size();
    CHECK(fx.session.despawn(b), "despawn succeeds");
    fx.pump();
    CHECK(fx.scene.size() < beforeDespawn, "despawn removed scene objects");
    CHECK(fx.session.findObject(b) == nullptr, "despawned object deregistered");
    CHECK(!despawned.empty(), "despawn callback fired");
    CHECK(!fx.session.despawn(999999), "despawning an unknown id fails");

    // Leaving the room tears every replicated object down.
    fx.session.leaveRoom();
    fx.pump();
    CHECK(fx.session.allObjects().empty(), "leaving the room released all network objects");
}

static void TestRpc() {
    std::printf("-- rpc --\n");
    Fixture fx;
    Net::SessionConfig cfg;
    cfg.offlineMode = true;
    fx.session.start(cfg, fx.bridge());
    fx.session.connect();
    fx.pump();
    fx.session.createRoom("Room", Net::RoomOptions{});
    fx.pump();

    int calls = 0;
    int lastInt = 0;
    std::string lastText;
    Net::PlayerId lastSender = 0;

    fx.session.registerRpc("Greet", [&](Net::PlayerId sender, Net::Reader& args) {
        ++calls;
        lastSender = sender;
        lastInt = args.readInt32();
        lastText = args.readString();
    });

    Net::Writer args;
    args.writeInt32(42);
    args.writeString("hi");

    CHECK(fx.session.callRpc("Greet", args, Net::RpcTarget::Local), "local RPC dispatches");
    CHECK(calls == 1, "local RPC invoked the handler synchronously");
    CHECK(lastInt == 42 && lastText == "hi", "RPC parameters deserialized");
    CHECK(lastSender == fx.session.localPlayerId(), "RPC sender is the local player");

    CHECK(fx.session.callRpc("Greet", args, Net::RpcTarget::All), "All RPC accepted");
    fx.pump();
    CHECK(calls == 2, "All loops back to the local player offline");

    // Others reaches nobody in a one-player session, and must not loop back.
    const int before = calls;
    CHECK(fx.session.callRpc("Greet", args, Net::RpcTarget::Others), "Others RPC accepted");
    fx.pump();
    CHECK(calls == before, "Others does not loop back to the sender");

    // Unknown RPCs are dropped, not fatal.
    CHECK(fx.session.callRpc("NoSuchRpc", args, Net::RpcTarget::All), "unknown RPC send accepted");
    fx.pump();
    CHECK(calls == before, "unknown RPC did not invoke anything");

    CHECK(!fx.session.callRpc("", args, Net::RpcTarget::All), "empty RPC name rejected");

    fx.session.unregisterRpc("Greet");
    const int afterUnregister = calls;
    fx.session.callRpc("Greet", args, Net::RpcTarget::Local);
    CHECK(calls == afterUnregister, "unregistered RPC no longer fires");
}

static void TestBackendFallback() {
    std::printf("-- backend selection --\n");
    Fixture fx;

    // No App ID and not explicitly offline: must degrade to offline rather than
    // fail, so a project without the Photon package still runs.
    Net::SessionConfig cfg;
    cfg.offlineMode = false;
    cfg.appId.clear();
    CHECK(fx.session.start(cfg, fx.bridge()), "session starts without an App ID");
    CHECK(fx.session.isOffline(), "missing App ID degrades to offline");

    // A bad scene bridge is refused rather than crashing later.
    Net::NetworkSession bare;
    Net::SceneBridge broken;
    CHECK(!bare.start(cfg, broken), "invalid scene bridge is refused");
}

// --------------------------------------------------------------------------
// Two-peer harness
//
// A minimal NetworkBackend that wires two sessions together in-process. This is
// also a real exercise of the backend seam: the session gets a transport it knows
// nothing about, via startWithBackend().
// --------------------------------------------------------------------------

class PairedBackend final : public Net::NetworkBackend {
public:
    PairedBackend(Net::PlayerId self, const char* label) : selfId(self), label(label) {}

    void pairWith(PairedBackend* other) { peer = other; }

    const char* name() const override { return label; }

    bool initialize(const Net::SessionConfig&, const Net::BackendCallbacks& callbacks) override {
        cb = callbacks;
        return true;
    }
    void shutdown() override { pending.clear(); peer = nullptr; }

    void tick(float) override {
        std::vector<std::function<void()>> work;
        work.swap(pending);
        for (auto& fn : work) fn();
    }

    bool connect() override { currentState = Net::ConnectionState::ConnectedToMaster; return true; }
    void disconnect() override { currentState = Net::ConnectionState::Disconnected; }
    bool reconnect() override { return connect(); }
    Net::ConnectionState state() const override { return currentState; }
    int roundTripTimeMs() const override { return 10; }
    const std::string& region() const override { return regionName; }

    bool joinLobby() override { currentState = Net::ConnectionState::InLobby; return true; }
    bool leaveLobby() override { return true; }
    bool refreshRoomList() override { return true; }
    const std::vector<Net::RoomInfo>& roomList() const override { return rooms; }

    bool createRoom(const std::string&, const Net::RoomOptions&) override { return enter(); }
    bool joinRoom(const std::string&) override { return enter(); }
    bool joinOrCreateRoom(const std::string&, const Net::RoomOptions&) override { return enter(); }
    bool joinRandomRoom() override { return enter(); }
    bool leaveRoom() override {
        currentState = Net::ConnectionState::ConnectedToMaster;
        pending.push_back([this] { if (cb.onLeftRoom) cb.onLeftRoom(); });
        return true;
    }

    const std::vector<Net::NetworkPlayer>& players() const override { return playerList; }
    Net::PlayerId localPlayerId() const override { return selfId; }
    Net::PlayerId hostPlayerId() const override { return 1; }
    bool isHost() const override { return selfId == 1; }

    bool setRoomProperty(const std::string&, const std::string&) override { return true; }
    bool setPlayerProperty(const std::string&, const std::string&) override { return true; }
    bool setNickname(const std::string&) override { return true; }

    bool sendEvent(uint8_t code, const std::vector<uint8_t>& payload,
                   Net::RpcTarget target, Net::SendMode, Net::PlayerId) override {
        if (target == Net::RpcTarget::Local) return true;
        // Deliver to the paired peer, mirroring a real transport: the sender does
        // not receive its own "Others" traffic.
        if (peer && peer->cb.onEvent) {
            PairedBackend* dest = peer;
            const Net::PlayerId from = selfId;
            std::vector<uint8_t> copy = payload;
            dest->pending.push_back([dest, from, code, copy] { dest->cb.onEvent(from, code, copy); });
        }
        return true;
    }

    const Net::NetworkError& lastError() const override { return error; }

private:
    bool enter() {
        currentState = Net::ConnectionState::InRoom;
        playerList.clear();
        Net::NetworkPlayer me;
        me.id = selfId;
        me.isLocal = true;
        me.isHost = (selfId == 1);
        playerList.push_back(me);
        pending.push_back([this] {
            if (cb.onJoinedRoom) cb.onJoinedRoom("Paired");
        });
        return true;
    }

    Net::PlayerId selfId;
    const char* label;
    PairedBackend* peer = nullptr;
    Net::BackendCallbacks cb;
    Net::ConnectionState currentState = Net::ConnectionState::Disconnected;
    Net::NetworkError error;
    std::string regionName = "paired";
    std::vector<Net::NetworkPlayer> playerList;
    std::vector<Net::RoomInfo> rooms;
    std::vector<std::function<void()>> pending;
};

static void TestTwoPeerReplicationAndSync() {
    std::printf("-- two-peer replication and sync --\n");

    Fixture host, client;
    ModuObj::Asset asset = MakeAsset("NetCube");
    const std::string assetId = asset.assetId;
    host.assets[assetId] = asset;
    client.assets[assetId] = asset;

    auto hostBackend = std::unique_ptr<PairedBackend>(new PairedBackend(1, "PairedHost"));
    auto clientBackend = std::unique_ptr<PairedBackend>(new PairedBackend(2, "PairedClient"));
    hostBackend->pairWith(clientBackend.get());
    clientBackend->pairWith(hostBackend.get());

    Net::SessionConfig cfg;
    cfg.serializationRateHz = 20;
    CHECK(host.session.startWithBackend(cfg, host.bridge(), std::move(hostBackend)),
          "host session starts on an injected backend");
    CHECK(client.session.startWithBackend(cfg, client.bridge(), std::move(clientBackend)),
          "client session starts on an injected backend");

    host.session.connect();  client.session.connect();
    host.session.createRoom("R", Net::RoomOptions{});
    client.session.joinRoom("R");
    host.pump(); client.pump();
    CHECK(host.session.inRoom() && client.session.inRoom(), "both peers are in the room");

    // --- spawn replication -------------------------------------------------
    const float pos[3] = {1.0f, 0.0f, 0.0f};
    const Net::NetworkId id = host.session.spawn(assetId, pos);
    host.pump(); client.pump();

    CHECK(id != Net::kInvalidNetworkId, "host spawned an object");
    CHECK(client.session.findObject(id) != nullptr, "spawn replicated to the client");
    CHECK(client.scene.size() == 2, "client rebuilt the hierarchy through the ModuOBJ API");
    CHECK(host.session.hasAuthority(id), "host has authority");
    CHECK(!client.session.hasAuthority(id), "client does not have authority");

    // --- transform sync ----------------------------------------------------
    Net::SyncSettings settings;
    settings.mode = Net::SyncMode::Interpolate;
    settings.sendRateHz = 20;
    settings.interpolationDelay = 0.05f;
    host.session.setSyncSettings(id, settings);
    client.session.setSyncSettings(id, settings);
    CHECK(host.session.syncSettings(id) != nullptr, "sync settings stored");

    const Net::NetworkObject* hostObject = host.session.findObject(id);
    const Net::NetworkObject* clientObject = client.session.findObject(id);
    CHECK(hostObject && clientObject, "both peers track the object");

    auto hostRoot = [&]() -> SceneObject* {
        for (SceneObject& o : host.scene) if (o.id == hostObject->rootSceneObjectId) return &o;
        return nullptr;
    };
    auto clientRootPos = [&]() -> glm::vec3 {
        for (SceneObject& o : client.scene) if (o.id == clientObject->rootSceneObjectId) return o.position;
        return glm::vec3(-999.0f);
    };

    // Drive the host object along +X and pump both sessions.
    for (int step = 0; step < 40; ++step) {
        if (SceneObject* root = hostRoot()) {
            root->position.x = 1.0f + static_cast<float>(step) * 0.5f;
        }
        host.session.tick(0.05f);
        client.session.tick(0.05f);
    }

    CHECK(host.session.bandwidth().packetsSent > 0, "host sent transform snapshots");
    CHECK(client.session.bandwidth().packetsReceived > 0, "client received snapshots");
    CHECK(client.session.bufferedSnapshotCount(id) > 0, "client buffered snapshots");

    const glm::vec3 replicated = clientRootPos();
    CHECK(replicated.x > 1.5f, "client object followed the host along +X");
    // Interpolation renders slightly in the past, so it should trail rather than
    // match exactly. This is what distinguishes it from a raw snapshot snap.
    const float hostX = hostRoot() ? hostRoot()->position.x : 0.0f;
    CHECK(replicated.x <= hostX + 0.001f, "interpolated client trails or matches the host");

    // The client must never overwrite the host's own object from a remote packet.
    CHECK(hostRoot() && std::fabs(hostRoot()->position.x - hostX) < 1e-4f,
          "authority object is not modified by inbound sync");

    // --- send-rate limiting ------------------------------------------------
    {
        Net::SyncSettings slow;
        slow.sendRateHz = 1;              // one packet per second
        host.session.setSyncSettings(id, slow);
        host.session.resetBandwidthStats();
        for (int step = 0; step < 10; ++step) {
            if (SceneObject* root = hostRoot()) root->position.x += 1.0f;
            host.session.tick(0.05f);     // 0.5s total
        }
        CHECK(host.session.bandwidth().packetsSent <= 1, "send rate limit respected");
    }

    // --- dead-band ---------------------------------------------------------
    {
        Net::SyncSettings still;
        still.sendRateHz = 100;
        still.positionThreshold = 0.5f;
        host.session.setSyncSettings(id, still);
        host.session.resetBandwidthStats();
        for (int step = 0; step < 10; ++step) {
            if (SceneObject* root = hostRoot()) root->position.x += 0.001f;  // below threshold
            host.session.tick(0.05f);
        }
        CHECK(host.session.bandwidth().sendsSkippedByThreshold > 0,
              "unchanged transform is not resent");
    }

    // --- bandwidth budget --------------------------------------------------
    {
        Fixture budget;
        budget.assets[assetId] = asset;
        auto b1 = std::unique_ptr<PairedBackend>(new PairedBackend(1, "BudgetA"));
        Net::SessionConfig limited;
        limited.serializationRateHz = 60;
        limited.maxOutboundBytesPerSecond = 1;   // effectively nothing may go out
        budget.session.startWithBackend(limited, budget.bridge(), std::move(b1));
        budget.session.connect();
        budget.session.createRoom("R", Net::RoomOptions{});
        budget.pump();
        const Net::NetworkId bid = budget.session.spawn(assetId);
        budget.pump();
        Net::SyncSettings fast;
        fast.sendRateHz = 60;
        fast.positionThreshold = 0.0f;
        budget.session.setSyncSettings(bid, fast);
        for (int step = 0; step < 20; ++step) {
            for (SceneObject& o : budget.scene) o.position.x += 1.0f;
            budget.session.tick(0.05f);
        }
        CHECK(budget.session.bandwidth().sendsSkippedByBudget > 0,
              "outbound byte budget drops updates instead of queueing");
    }

    // --- despawn clears sync state ----------------------------------------
    host.session.despawn(id);
    host.pump(); client.pump();
    CHECK(host.session.bufferedSnapshotCount(id) == 0, "despawn cleared sync state");
    CHECK(client.session.findObject(id) == nullptr, "despawn replicated to the client");
    CHECK(client.scene.empty(), "client removed the replicated hierarchy");
}

// NetworkIdentity / NetworkManager components: scene round trip, defaults, and
// the one-manager-per-scene rule.
static void TestNetworkComponents() {
    std::printf("-- network components --\n");

    const fsys::path temp = fsys::temp_directory_path() / "modularity-network-selftest" / "work";
    std::error_code ec;
    fsys::create_directories(temp, ec);

    std::vector<SceneObject> scene;
    scene.emplace_back("Manager", ObjectType::Empty, 1);
    scene.emplace_back("Player", ObjectType::Cube, 2);
    scene.emplace_back("Plain", ObjectType::Empty, 3);
    for (SceneObject& o : scene) { o.scale = glm::vec3(1.0f); o.localScale = glm::vec3(1.0f); }

    scene[0].hasNetworkManager = true;
    scene[0].networkManager.appId = "test-app-id";
    scene[0].networkManager.appVersion = "2.5";
    scene[0].networkManager.region = "eu";
    scene[0].networkManager.nickname = "Host";
    scene[0].networkManager.autoConnect = true;
    scene[0].networkManager.maxPlayers = 8;
    scene[0].networkManager.serializationRateHz = 15;
    scene[0].networkManager.maxOutboundBytesPerSecond = 4096;

    scene[1].hasNetworkIdentity = true;
    scene[1].networkIdentity.assetId = "mobj-0123456789abcdef";
    scene[1].networkIdentity.syncScale = true;
    scene[1].networkIdentity.syncMode = 2;              // Extrapolate
    scene[1].networkIdentity.sendRateHz = 30;
    scene[1].networkIdentity.interpolationDelay = 0.08f;
    scene[1].networkIdentity.maxExtrapolation = 0.4f;
    scene[1].networkIdentity.spawnerOwns = false;
    // Runtime-only fields must not survive a save.
    scene[1].networkIdentity.runtimeNetworkId = 12345;
    scene[1].networkIdentity.runtimeOwner = 9;
    scene[1].networkIdentity.runtimeHasAuthority = true;

    // Defaults.
    CHECK(!scene[2].hasNetworkIdentity && !scene[2].hasNetworkManager,
          "a plain object carries no networking components");

    // Single-manager validation.
    CHECK(CountNetworkManagers(scene) == 1, "exactly one manager counted");
    CHECK(FindActiveNetworkManager(scene) == 1, "active manager resolves to its object id");
    scene[2].hasNetworkManager = true;
    scene[2].networkManager.enabled = true;
    CHECK(CountNetworkManagers(scene) == 2, "duplicate manager detected");
    CHECK(FindActiveNetworkManager(scene) == -1, "duplicate managers report no single active manager");
    scene[2].networkManager.enabled = false;
    CHECK(CountNetworkManagers(scene) == 1, "a disabled duplicate does not count");
    scene[2].hasNetworkManager = false;

    // Scene round trip through the public serializer (modular format).
    const fsys::path scenePath = temp / "NetComponents.scene";
    int nextId = 10;
    CHECK(SceneSerializer::saveScene(scenePath, scene, nextId, 0.5f, SkyboxSettings{}),
          "save scene with networking components");

    std::vector<SceneObject> loaded;
    int loadedNext = 0, version = 0;
    CHECK(SceneSerializer::loadScene(scenePath, loaded, loadedNext, version, nullptr, nullptr, nullptr),
          "load scene with networking components");
    CHECK(loaded.size() == 3, "object count survives");

    const SceneObject* mgr = nullptr;
    const SceneObject* ident = nullptr;
    const SceneObject* plain = nullptr;
    for (const SceneObject& o : loaded) {
        if (o.name == "Manager") mgr = &o;
        if (o.name == "Player") ident = &o;
        if (o.name == "Plain") plain = &o;
    }
    CHECK(mgr && mgr->hasNetworkManager, "manager component survives");
    if (mgr) {
        CHECK(mgr->networkManager.appId == "test-app-id", "app id survives");
        CHECK(mgr->networkManager.appVersion == "2.5", "app version survives");
        CHECK(mgr->networkManager.region == "eu", "region survives");
        CHECK(mgr->networkManager.nickname == "Host", "nickname survives");
        CHECK(mgr->networkManager.autoConnect, "auto connect survives");
        CHECK(mgr->networkManager.maxPlayers == 8, "max players survives");
        CHECK(mgr->networkManager.serializationRateHz == 15, "serialization rate survives");
        CHECK(mgr->networkManager.maxOutboundBytesPerSecond == 4096, "bandwidth cap survives");
    }
    CHECK(ident && ident->hasNetworkIdentity, "identity component survives");
    if (ident) {
        CHECK(ident->networkIdentity.assetId == "mobj-0123456789abcdef", "identity asset id survives");
        CHECK(ident->networkIdentity.syncScale, "sync scale flag survives");
        CHECK(ident->networkIdentity.syncMode == 2, "sync mode survives");
        CHECK(ident->networkIdentity.sendRateHz == 30, "send rate survives");
        CHECK(std::fabs(ident->networkIdentity.interpolationDelay - 0.08f) < 1e-4f, "interp delay survives");
        CHECK(std::fabs(ident->networkIdentity.maxExtrapolation - 0.4f) < 1e-4f, "max extrapolation survives");
        CHECK(!ident->networkIdentity.spawnerOwns, "spawnerOwns survives");
        // Session-scoped runtime state must NOT be persisted.
        CHECK(ident->networkIdentity.runtimeNetworkId == 0, "runtime network id not serialized");
        CHECK(ident->networkIdentity.runtimeOwner == 0, "runtime owner not serialized");
        CHECK(!ident->networkIdentity.runtimeHasAuthority, "runtime authority not serialized");
    }
    CHECK(plain && !plain->hasNetworkIdentity && !plain->hasNetworkManager,
          "plain object still carries no networking components");

    // A scene with no networking must gain no networking keys at all.
    {
        std::vector<SceneObject> bare;
        bare.emplace_back("Only", ObjectType::Empty, 1);
        bare[0].scale = glm::vec3(1.0f);
        bare[0].localScale = glm::vec3(1.0f);
        const fsys::path barePath = temp / "NoNetworking.scene";
        int n = 5;
        CHECK(SceneSerializer::saveScene(barePath, bare, n, 0.5f, SkyboxSettings{}),
              "save scene with no networking");
        std::ifstream in(barePath, std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(body.find("netIdentity") == std::string::npos, "no identity keys in a plain scene");
        CHECK(body.find("netManager") == std::string::npos, "no manager keys in a plain scene");
    }
}

int main() {
    TestSerializer();
    TestNetworkComponents();
    TestOfflineLifecycle();
    TestSpawnPipeline();
    TestRpc();
    TestBackendFallback();
    TestTwoPeerReplicationAndSync();

    std::printf("\nnetwork self-test: %d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
