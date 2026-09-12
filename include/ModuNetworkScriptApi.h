#pragma once

// ModuNetwork - the script-facing networking API.
//
//     add ModuCPP;
//     add ModuNetwork;
//
//     void Begin() {
//         Net.SetNickname("Ada");
//         Net.JoinOrCreateRoom("lobby", 16);
//     }
//
//     void TickUpdate() {
//         if (!Net.InRoom) return;
//         if (Net.IsHost) { /* drive the shared world */ }
//     }
//
// A script using this never sees NetworkSession, Writer, Reader or a Net:: type
// (those live engine-side). Everything routes through ScriptContext, whose
// networking methods are flat - ints, floats, strings and glm vectors - so the
// script ABI does not move when the networking internals do.
//
// With no session running, every query reports a neutral value: disconnected, no
// room, no players, ping zero. Commands are no-ops that return false. The two
// deliberate exceptions are IsHost and HasAuthority, which report *true* when there
// is no session, because a script alone in a single-player scene genuinely is the
// authority - that way `if (Net.IsHost) SpawnTheWorld();` behaves the same offline
// as it does hosting a room, instead of silently doing nothing.

#include "ModuCPPScriptApi.h"

namespace ModuCPP {

// Mirrors Net::ConnectionState. Kept as a separate enum so the script surface does
// not depend on the engine header, and so the numbering is something scripts can
// rely on across engine versions.
enum class NetState {
    Disconnected = 0,
    Connecting = 1,
    ConnectedToMaster = 2,
    JoiningLobby = 3,
    InLobby = 4,
    JoiningRoom = 5,
    InRoom = 6,
    LeavingRoom = 7,
    Disconnecting = 8,
    Failed = 9,
};

inline const char* ToString(NetState state) {
    switch (state) {
        case NetState::Disconnected:      return "Disconnected";
        case NetState::Connecting:        return "Connecting";
        case NetState::ConnectedToMaster: return "ConnectedToMaster";
        case NetState::JoiningLobby:      return "JoiningLobby";
        case NetState::InLobby:           return "InLobby";
        case NetState::JoiningRoom:       return "JoiningRoom";
        case NetState::InRoom:            return "InRoom";
        case NetState::LeavingRoom:       return "LeavingRoom";
        case NetState::Disconnecting:     return "Disconnecting";
        case NetState::Failed:            return "Failed";
    }
    return "Unknown";
}

// Which tracked part an avatar pose describes. Plain ints rather than an enum class
// so a project can stream parts the engine has no name for (hips, feet, a tail)
// without touching this header: any slot below Net.AvatarSlotCount works.
namespace AvatarSlot {
constexpr int Head = 0;
constexpr int LeftHand = 1;
constexpr int RightHand = 2;
}  // namespace AvatarSlot

// A network object handle. 0 is "none" - every call that can fail returns it, so
// `if (id)` is the success test.
using NetworkId = unsigned int;
constexpr NetworkId InvalidNetworkId = 0;

// A player handle. 0 is "none", matching Net::kInvalidPlayerId.
using PlayerId = int;
constexpr PlayerId InvalidPlayerId = 0;

namespace detail {

// Property proxies, same shape and for the same reason as the ones in
// ModuInputXRScriptApi.h: the API reads as fields (`Net.InRoom`, `Net.Ping`) but
// each one has to run code to ask the live session, so they cannot be plain
// members. Each is an empty value with a conversion operator that performs the
// read.
//
// The same consequence applies here: because these convert rather than *are* the
// value, chained member access does not compile. Assign first
// (`string r = Net.Region;`) or call `.Value()`.

struct NetBoolProperty {
    enum class Source { Active, Offline, InRoom, IsHost };
    Source source = Source::Active;

    bool Value() const {
        ScriptContext* c = ctxPtr();
        if (!c) {
            // No context at all (editor preview, tooling). Match the no-session
            // answers, including IsHost reporting true.
            return source == Source::Offline || source == Source::IsHost;
        }
        switch (source) {
            case Source::Active:  return c->IsNetworkActive();
            case Source::Offline: return c->IsNetworkOffline();
            case Source::InRoom:  return c->IsInNetworkRoom();
            case Source::IsHost:  return c->IsNetworkHost();
        }
        return false;
    }
    operator bool() const { return Value(); }
};

struct NetIntProperty {
    enum class Source { Ping, LocalPlayer, HostPlayer, PlayerCount, ObjectCount };
    Source source = Source::Ping;

    int Value() const {
        ScriptContext* c = ctxPtr();
        if (!c) return 0;
        switch (source) {
            case Source::Ping:        return c->GetNetworkPing();
            case Source::LocalPlayer: return c->GetLocalNetworkPlayerId();
            case Source::HostPlayer:  return c->GetNetworkHostPlayerId();
            case Source::PlayerCount: return c->GetNetworkPlayerCount();
            case Source::ObjectCount: return c->GetNetworkObjectCount();
        }
        return 0;
    }
    operator int() const { return Value(); }
};

struct NetStateProperty {
    NetState Value() const {
        ScriptContext* c = ctxPtr();
        if (!c) return NetState::Disconnected;
        return static_cast<NetState>(c->GetNetworkState());
    }
    operator NetState() const { return Value(); }
};

struct NetStringProperty {
    enum class Source { Region, Backend };
    Source source = Source::Region;

    std::string Value() const {
        ScriptContext* c = ctxPtr();
        if (!c) return std::string();
        return source == Source::Region ? c->GetNetworkRegion() : c->GetNetworkBackendName();
    }
    operator std::string() const { return Value(); }
};

}  // namespace detail

// The avatar pose channel: the per-player stream a rig uses for parts that move
// independently of the object they belong to.
//
// Transform sync moves one transform per network object, so the children of a
// spawned instance are not synced. A VR rig is exactly that shape - one object with
// a head and two hands under it, all moving every frame - so the rig streams its
// parts here instead, keyed by player. One packet carries the whole pose, which is
// what keeps a hand from arriving a frame apart from the head it belongs to.
//
// Rotations are quaternions. A headset orientation does not survive a Euler round
// trip, and the engine's Euler composition is not glm's, so converting on the way
// in or out would bake in a convention mismatch. Use Net.QuatToEuler() at the point
// you apply one to a transform.
//
// The engine keeps only the latest pose and never smooths it: the receiver knows
// its own frame rate and what it is attached to, so damping belongs there. A rig
// that snaps is a rig that forgot to interpolate.
struct AvatarFacade {
    // Publish one of the local player's tracked parts. Streams to the room at the
    // session's serialization rate until Clear() - calling it every frame is the
    // intended use; the engine rate-limits, not the caller.
    void SetLocal(int slot, const Vector3& position, const Quaternion& rotation) const {
        if (ScriptContext* c = ctxPtr()) c->SetLocalAvatarPose(slot, position, rotation);
    }

    // Stop streaming a slot. Remote copies keep the last pose they saw rather than
    // snapping to the origin, so a controller put down leaves its hand where it was.
    void ClearLocal(int slot) const {
        if (ScriptContext* c = ctxPtr()) c->ClearLocalAvatarPose(slot);
    }

    // Latest pose for a player, or the local player's own published pose. False
    // means "not tracked" - never treat a false return as a pose at the origin.
    bool TryGet(PlayerId player, int slot, Vector3& position, Quaternion& rotation) const {
        ScriptContext* c = ctxPtr();
        if (!c) return false;
        return c->GetAvatarPose(player, slot, position, rotation);
    }

    bool IsTracked(PlayerId player, int slot) const {
        Vector3 position(0.0f);
        Quaternion rotation(1.0f, 0.0f, 0.0f, 0.0f);
        return TryGet(player, slot, position, rotation);
    }
};

// The networking root. `Net.InRoom`, `Net.Spawn(...)`, `Net.Avatar.SetLocal(...)`.
struct NetworkFacade {
    // --- Session ---
    detail::NetBoolProperty Active{detail::NetBoolProperty::Source::Active};
    detail::NetBoolProperty Offline{detail::NetBoolProperty::Source::Offline};
    detail::NetStateProperty State{};
    detail::NetIntProperty Ping{detail::NetIntProperty::Source::Ping};
    detail::NetStringProperty Region{detail::NetStringProperty::Source::Region};
    detail::NetStringProperty Backend{detail::NetStringProperty::Source::Backend};

    bool Connect() const {
        ScriptContext* c = ctxPtr();
        return c && c->NetworkConnect();
    }
    void Disconnect() const {
        if (ScriptContext* c = ctxPtr()) c->NetworkDisconnect();
    }

    // --- Rooms ---
    detail::NetBoolProperty InRoom{detail::NetBoolProperty::Source::InRoom};

    // maxPlayers 0 means "the room's own default". Joins the room if it exists and
    // creates it otherwise, which is the shape almost every small session wants.
    bool JoinOrCreateRoom(const std::string& name, int maxPlayers = 0) const {
        ScriptContext* c = ctxPtr();
        return c && c->NetworkJoinOrCreateRoom(name, maxPlayers);
    }
    bool JoinRandomRoom() const {
        ScriptContext* c = ctxPtr();
        return c && c->NetworkJoinRandomRoom();
    }
    bool LeaveRoom() const {
        ScriptContext* c = ctxPtr();
        return c && c->NetworkLeaveRoom();
    }

    // --- Players ---
    detail::NetBoolProperty IsHost{detail::NetBoolProperty::Source::IsHost};
    detail::NetIntProperty LocalPlayer{detail::NetIntProperty::Source::LocalPlayer};
    detail::NetIntProperty HostPlayer{detail::NetIntProperty::Source::HostPlayer};
    detail::NetIntProperty PlayerCount{detail::NetIntProperty::Source::PlayerCount};

    PlayerId PlayerAt(int index) const {
        ScriptContext* c = ctxPtr();
        return c ? c->GetNetworkPlayerIdAt(index) : InvalidPlayerId;
    }
    std::string NicknameOf(PlayerId player) const {
        ScriptContext* c = ctxPtr();
        return c ? c->GetNetworkPlayerNickname(player) : std::string();
    }
    bool SetNickname(const std::string& nickname) const {
        ScriptContext* c = ctxPtr();
        return c && c->SetNetworkNickname(nickname);
    }
    bool IsLocalPlayer(PlayerId player) const {
        return player != InvalidPlayerId && player == LocalPlayer.Value();
    }

    // --- Networked objects ---
    //
    // Spawn takes a ModuOBJ asset id: the same packaged object you drag into a
    // scene, instantiated on every client with one owner. Returns 0 on failure.
    detail::NetIntProperty ObjectCount{detail::NetIntProperty::Source::ObjectCount};

    NetworkId Spawn(const std::string& assetId,
                    const Vector3& position = Vector3(0.0f),
                    const Vector3& rotationDegrees = Vector3(0.0f),
                    const Vector3& scale = Vector3(1.0f)) const {
        ScriptContext* c = ctxPtr();
        return c ? c->NetworkSpawn(assetId, position, rotationDegrees, scale) : InvalidNetworkId;
    }
    bool Despawn(NetworkId id) const {
        ScriptContext* c = ctxPtr();
        return c && c->NetworkDespawn(id);
    }
    bool HasAuthority(NetworkId id) const {
        ScriptContext* c = ctxPtr();
        return c ? c->NetworkHasAuthority(id) : true;
    }
    bool TransferOwnership(NetworkId id, PlayerId newOwner) const {
        ScriptContext* c = ctxPtr();
        return c && c->NetworkTransferOwnership(id, newOwner);
    }
    PlayerId OwnerOf(NetworkId id) const {
        ScriptContext* c = ctxPtr();
        return c ? c->GetNetworkObjectOwner(id) : InvalidPlayerId;
    }
    // The scene object id of a networked object's root, or -1. This is the bridge
    // between a NetworkId and everything else a script can do to an object.
    int SceneObjectOf(NetworkId id) const {
        ScriptContext* c = ctxPtr();
        return c ? c->GetNetworkObjectSceneId(id) : -1;
    }
    NetworkId NetworkIdOf(int sceneObjectId) const {
        ScriptContext* c = ctxPtr();
        return c ? c->FindNetworkIdForObject(sceneObjectId) : InvalidNetworkId;
    }
    NetworkId ObjectAt(int index) const {
        ScriptContext* c = ctxPtr();
        return c ? c->GetNetworkObjectIdAt(index) : InvalidNetworkId;
    }

    // --- Avatar pose channel ---
    AvatarFacade Avatar{};
    static constexpr int AvatarSlotCount = 8;

    // --- Rotation conversion ---
    //
    // In the engine's convention (Rx*Ry*Rz, degrees), which is NOT glm's default
    // quat(vec3) composition - they agree only while a single axis is non-zero, and
    // are 111 degrees apart at (80,70,50). Always convert through these when moving
    // between a tracked pose and a transform's Euler rotation.
    Vector3 QuatToEuler(const Quaternion& rotation) const {
        ScriptContext* c = ctxPtr();
        return c ? c->QuatToEulerDegrees(rotation) : Vector3(0.0f);
    }
    Quaternion EulerToQuat(const Vector3& degrees) const {
        ScriptContext* c = ctxPtr();
        return c ? c->EulerDegreesToQuat(degrees) : Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    }
};

inline const NetworkFacade Net{};

}  // namespace ModuCPP
