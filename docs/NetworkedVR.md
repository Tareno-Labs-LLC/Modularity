# Networked VR (ModuNetwork + the VR rig)

How to get two players into a shared room with tracked heads and hands, in VR or on
a desktop. Covers the `ModuNetwork` script module, the rig scripts that ship in
`Scripts/`, and the setup a scene needs.

## What this is built on

| Piece | Where | What it does |
| --- | --- | --- |
| Networking core | `src/Network/` | Session, rooms, players, RPC, ownership, ModuOBJ spawning, transform sync |
| Photon Realtime backend | `src/Network/PhotonBackend.cpp` | Online transport. Needs the `photon.realtime-sdk-windows` package on Windows |
| `ModuNetwork` module | `include/ModuNetworkScriptApi.h` | The script-facing `Net` facade |
| `NetworkVRAvatar.moducpp` | `Scripts/` | Per-player body: mirrors your local rig, plays back everyone else's |
| `NetworkVRManager.moducpp` | `Scripts/` | Joins a room and spawns one avatar for the local player |

## The one thing that shapes the whole design

Transform sync moves **one transform per network object** - `NetworkObject` carries a
single `rootSceneObjectId`. The children of a spawned object are not synced.

A VR rig is exactly the shape that breaks on: one object with a head and two hands
under it, all moving independently every frame. So the rig splits its state across
two mechanisms:

- **avatar root** - a networked object, so ordinary transform sync streams its world
  position and rotation.
- **head and hands** - streamed on the **avatar pose channel**, keyed by player
  rather than by object, as poses *relative to the rig root*.

Streaming the parts rig-local is what lets the two halves compose. A remote avatar puts
its root wherever transform sync says, then rebuilds the parts on top of it. Sending
world poses instead would fight the root's own interpolation and make hands swim
every time a player moved.

The engine stores only the latest pose per slot and never smooths it - the receiver
knows its own frame rate, so damping belongs there. `NetworkVRAvatar` does it in
`ApplyPart`.

## Scene setup

### 1. Put the App ID on a Network Manager

Add a **Network Manager** component to any object in the scene (Inspector → Add
Component → Network Manager) and paste the App ID into **App ID**. It is stored in
the scene file as `netManagerAppId`, so it travels with the scene, not the engine.

The realtime App ID for this project:

```
985c11e1-7a9c-4b4e-b5fb-b750339a0c8b
```

Leave **Region** empty for automatic best-ping. Turn **Auto Connect** on so the
session comes up without a script asking it to.

With no App ID the session starts offline: `Net.Active` is false, the rig still runs,
and you see exactly one player - yours. That is a useful first test on its own.

### 2. Build the avatar object

> **Two things are called `.moduobj`.** The extension is shared by two formats that
> are not interchangeable:
>
> | Made by | Format | Used for |
> | --- | --- | --- |
> | **Create Packaged Object** (hierarchy right-click) | `MODU_OBJASSET` text | The prefab the engine instantiates: `SpawnIntoScene`, prefab edit mode, and `Net.Spawn` |
> | **Export As ModuOBJ...** | ZIP: manifest + scene + `Dependencies/` | Handing a packaged object to someone else, with its meshes and textures bundled |
>
> Networking resolves assets through `ModuObj::LoadAsset`, which reads the first of
> those, so **the avatar has to be made with Create Packaged Object**. An exported
> zip is skipped by the asset index and `Net.Spawn` will not find it. To turn a
> shared package into a usable prefab: **Import ModuOBJ...** to bring it into the
> scene, then **Create Packaged Object** on the result.


The avatar is an ordinary packaged object (`.moduobj`) - the body other players see.
Its root needs three children named exactly:

```
Avatar             <- NetworkVRAvatar script goes here
├── Head
├── LeftHand
└── RightHand
```

Give each one some visible geometry (a cube is fine). Then select the root and
**Create Packaged Object** (right-click in the hierarchy) to save it into `Assets/`.
Double-clicking the saved `.moduobj`
reopens it for editing, so getting the shapes right is not a one-shot.

The names matter because the avatar finds its parts by name at runtime rather than by
object reference. `ModuObj::Instantiate` remaps ids, parent ids and child lists when
it spawns a packaged object, but it does **not** remap `[ObjectRef]` strings - a ref
baked into a prefab points at whatever happens to hold that id in the scene it landed
in. Names survive that; ids do not.

You can rename the parts as long as you change `headName` / `leftHandName` /
`rightHandName` on the script to match.

### 3. Wire the scripts

- `NetworkVRAvatar` on the **avatar root**, inside the packaged object.
- `NetworkVRManager` on any object in the **scene**, with `rigAssetId` set to the
  packaged object's asset id (shown in the inspector when the `.moduobj` is selected).

On `NetworkVRAvatar`, point the four **Local Rig To Mirror** fields at the objects
your existing rig already drives. For an SCL rig those default to:

| Field | Object |
| --- | --- |
| `localBodyName` | `SCL` (the player body SCLBody sits on) |
| `localHeadName` | `XR Camera 1` |
| `localLeftHandName` | `PhysL` (the physics hand, not the controller target) |
| `localRightHandName` | `PhysR` |

Point these at the **physics hands**, not the controller targets. The physics hands
are what the player actually sees and what the locomotion leaves behind when it
pushes off the world - mirroring the targets would show everyone a pair of hands that
never touch anything.

`NetworkVRManager` only ever spawns **one** avatar - the local player's. `Net.Spawn`
replicates, so every client spawning its own is exactly enough for everyone to see
everyone. Spawning one per remote player as well is the most common way a first
networked scene goes wrong: you end up with N copies of yourself.

### 4. Test with two clients

Build the player (`build.ps1`) and run it alongside the editor in play mode, or run
two player instances. Both join `roomName`, and each sees the other's avatar move.

The console reports the session as it comes up:

```
[VRNet] Connecting (Photon)
[VRNet] ConnectedToMaster (Photon)
[VRNet] InRoom (Photon)
[VRNet] Players in room: 2
[VRAvatar] Mirroring the local rig.
[VRAvatar] Playing back a remote player.
```

## In VR and out of it

`NetworkVRAvatar` never reads a headset. It mirrors whatever local objects you point
it at, so it does not care what moves them - an SCL physics rig, an XR camera rig, or
a flat-mode camera with mouse look all mirror identically, and remote players cannot
tell which was which.

That separation is deliberate. Locomotion already owns the local transforms: SCL
pushes the body around with physics hands, and a networking script writing to those
same transforms would fight it every frame. So the avatar only ever **reads** the
local rig and **writes** the remote ones.

## The `Net` facade

```cpp
add ModuCPP;
add ModuNetwork;

void Begin() {
    Net.SetNickname("Ada");
    Net.JoinOrCreateRoom("lobby", 16);
}

void TickUpdate() {
    if (!Net.InRoom) return;
    if (Net.IsHost) { /* drive the shared world */ }
}
```

With no session running every query reports a neutral value and every command is a
no-op returning false. The two deliberate exceptions are **`Net.IsHost`** and
**`Net.HasAuthority`**, which report `true` with no session: a script alone in a
single-player scene genuinely is the authority, so `if (Net.IsHost) SpawnTheWorld();`
behaves the same offline as it does hosting a room instead of silently doing nothing.

See `include/ModuNetworkScriptApi.h` for the full surface.

### Avatar slots

`AvatarSlot.Head`, `AvatarSlot.LeftHand`, `AvatarSlot.RightHand`, and five more
unnamed slots up to `Net.AvatarSlotCount`. Slots are plain ints so a project can
stream parts the engine has no name for - hips, feet, a tail - without an engine
change.

Rotations on this channel are **quaternions**. A headset orientation does not survive
a Euler round trip, and the engine composes Euler as `Rx*Ry*Rz` which is not glm's
default - the two are 111 degrees apart at (80, 70, 50). Convert with
`Net.QuatToEuler()` at the point you apply a pose to a transform, never by hand.

## Gotchas found writing these scripts

- **`state` is taken.** The transpiler injects the script's persistent state object
  under that name in every method body. Name locals something else.
- **`dt` only exists in lifecycle hooks.** It is a parameter the transpiler adds to
  `TickUpdate` and friends. Helper methods call `DeltaTime()` instead.
- **`obj` is an `ObjectFacade`, not a `SceneObj`.** It has no `.Id()`; use `obj->id`.
  `FindChild(...)` and `ObjectById(...)` do return `SceneObj`, which has `.Id()`.
- **Do not pass a `SceneObj` to a helper method.** The transpiler lowers a `SceneObj`
  parameter to a bare `SceneObject`, which drops `.Transform` entirely. Pass ids.

## Related

- [Scripting Overview](Scripting.md)
- [ModuCPP Overview](moducpp/README.md)
- `Modu-Package-Manager/Packages/7.0/Photon/photon.realtime-sdk-windows-5.0.15.0/README-Modularity-Windows.md`
