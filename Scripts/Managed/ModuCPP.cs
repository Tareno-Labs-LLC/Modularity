using System;
using System.Runtime.InteropServices;
using System.Text;

namespace ModuCPP {
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ScriptTickDelegate(IntPtr ctx, float deltaTime);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ScriptInspectorDelegate(IntPtr ctx);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SetNativeApiDelegate(IntPtr apiPtr);

    [StructLayout(LayoutKind.Sequential)]
    public struct Vec3 {
        public float X;
        public float Y;
        public float Z;

        public Vec3(float x, float y, float z) {
            X = x;
            Y = y;
            Z = z;
        }

        public static Vec3 operator +(Vec3 a, Vec3 b) => new Vec3(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
        public static Vec3 operator -(Vec3 a, Vec3 b) => new Vec3(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
        public static Vec3 operator *(Vec3 a, float s) => new Vec3(a.X * s, a.Y * s, a.Z * s);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Vec2 {
        public float X;
        public float Y;

        public Vec2(float x, float y) {
            X = x;
            Y = y;
        }
    }

    public struct RaycastHit {
        public Vec3 Position;
        public Vec3 Normal;
        public float Distance;
        public int ObjectId;
        public Vec3 ObjectVelocity;
        public float StaticFriction;
        public float DynamicFriction;
    }

    public enum ConsoleMessageType {
        Info = 0,
        Warning = 1,
        Error = 2,
        Success = 3
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeApi {
        public uint Version;
        public IntPtr GetObjectId;
        public IntPtr GetPosition;
        public IntPtr SetPosition;
        public IntPtr GetRotation;
        public IntPtr SetRotation;
        public IntPtr GetScale;
        public IntPtr SetScale;
        public IntPtr HasRigidbody;
        public IntPtr EnsureRigidbody;
        public IntPtr SetRigidbodyVelocity;
        public IntPtr GetRigidbodyVelocity;
        public IntPtr AddRigidbodyForce;
        public IntPtr AddRigidbodyImpulse;
        public IntPtr GetSettingFloat;
        public IntPtr GetSettingBool;
        public IntPtr GetSettingString;
        public IntPtr SetSettingFloat;
        public IntPtr SetSettingBool;
        public IntPtr SetSettingString;
        public IntPtr AddConsoleMessage;
        public IntPtr FindObjectIdByName;
        public IntPtr GetObjectName;
        public IntPtr GetSelectedObjectId;
        public IntPtr ImGuiText;
        public IntPtr ImGuiSeparator;
        public IntPtr ImGuiButton;
        public IntPtr ImGuiCheckbox;
        public IntPtr ImGuiDragFloat;
        public IntPtr ImGuiDragFloat3;
        public IntPtr ImGuiInputText;
        public IntPtr ImGuiAcceptSceneObjectDrop;
        // Version 4+ additions appended to preserve ABI layout for older managed assemblies.
        public IntPtr GetSceneObjectCount;
        public IntPtr GetSceneObjectIdAt;
        public IntPtr ImGuiBeginCombo;
        public IntPtr ImGuiEndCombo;
        public IntPtr ImGuiSelectable;
        // Version 5+ additions.
        public IntPtr HasAnimation;
        public IntPtr PlayAnimation;
        public IntPtr StopAnimation;
        public IntPtr PauseAnimation;
        public IntPtr ReverseAnimation;
        public IntPtr SetAnimationTime;
        public IntPtr GetAnimationTime;
        public IntPtr IsAnimationPlaying;
        public IntPtr SetAnimationLoop;
        public IntPtr SetAnimationPlaySpeed;
        public IntPtr SetAnimationPlayOnAwake;
        // Version 6+ additions.
        public IntPtr IsObjectEnabled;
        public IntPtr SetObjectEnabled;
        public IntPtr GetLayer;
        public IntPtr SetLayer;
        public IntPtr HasTag;
        public IntPtr IsInLayer;
        public IntPtr GetTag;
        public IntPtr SetTag;
        public IntPtr SetPosition2D;
        public IntPtr IsSprintDown;
        public IntPtr IsJumpDown;
        public IntPtr GetMoveInputWASD;
        public IntPtr ApplyMouseLook;
        public IntPtr HasRigidbody2D;
        public IntPtr SetRigidbody2DVelocity;
        public IntPtr GetRigidbody2DVelocity;
        public IntPtr AddRigidbodyVelocity;
        public IntPtr SetRigidbodyAngularVelocity;
        public IntPtr GetRigidbodyAngularVelocity;
        public IntPtr AddRigidbodyTorque;
        public IntPtr AddRigidbodyAngularImpulse;
        public IntPtr SetRigidbodyYaw;
        public IntPtr SetRigidbodyRotation;
        public IntPtr TeleportRigidbody;
        public IntPtr RaycastClosestDetailed;
        public IntPtr IsUIButtonPressed;
        public IntPtr IsUIInteractable;
        public IntPtr SetUIInteractable;
        public IntPtr GetUISliderValue;
        public IntPtr SetUISliderValue;
        public IntPtr SetUISliderRange;
        public IntPtr SetUILabel;
        public IntPtr SetUIColor;
        public IntPtr GetUITextScale;
        public IntPtr SetUITextScale;
        public IntPtr SetUISliderStyle;
        public IntPtr SetUIButtonStyle;
        public IntPtr SetUIStylePreset;
        public IntPtr SetFPSCap;
        public IntPtr GetSpriteClipCount;
        public IntPtr GetSpriteClipIndex;
        public IntPtr GetSpriteClipName;
        public IntPtr GetSpriteClipNameAt;
        public IntPtr SetSpriteClipIndex;
        public IntPtr SetSpriteClipName;
        public IntPtr GetSpriteAlpha;
        public IntPtr SetSpriteAlpha;
        public IntPtr FadeSpriteAlpha;
        public IntPtr FadeSpriteToClipIndex;
        public IntPtr FadeSpriteToClipName;
        public IntPtr HasAudioSource;
        public IntPtr PlayAudio;
        public IntPtr StopAudio;
        public IntPtr SetAudioLoop;
        public IntPtr SetAudioVolume;
        public IntPtr SetAudioClip;
        public IntPtr PlayAudioOneShot;
        public IntPtr MarkDirty;
        public IntPtr EnsureCapsuleCollider;
        public IntPtr GetProjectGravityScale;
        public IntPtr SetProjectGravityScale;
    }

    internal unsafe static class Native {
        public static NativeApi Api;
        public static GetObjectIdFn GetObjectId;
        public static GetPositionFn GetPosition;
        public static SetPositionFn SetPosition;
        public static GetRotationFn GetRotation;
        public static SetRotationFn SetRotation;
        public static GetScaleFn GetScale;
        public static SetScaleFn SetScale;
        public static HasRigidbodyFn HasRigidbody;
        public static EnsureRigidbodyFn EnsureRigidbody;
        public static SetRigidbodyVelocityFn SetRigidbodyVelocity;
        public static GetRigidbodyVelocityFn GetRigidbodyVelocity;
        public static AddRigidbodyForceFn AddRigidbodyForce;
        public static AddRigidbodyImpulseFn AddRigidbodyImpulse;
        public static GetSettingFloatFn GetSettingFloat;
        public static GetSettingBoolFn GetSettingBool;
        public static GetSettingStringFn GetSettingString;
        public static SetSettingFloatFn SetSettingFloat;
        public static SetSettingBoolFn SetSettingBool;
        public static SetSettingStringFn SetSettingString;
        public static AddConsoleMessageFn AddConsoleMessage;
        public static FindObjectIdByNameFn FindObjectIdByName;
        public static GetObjectNameFn GetObjectName;
        public static GetSelectedObjectIdFn GetSelectedObjectId;
        public static GetSceneObjectCountFn GetSceneObjectCount;
        public static GetSceneObjectIdAtFn GetSceneObjectIdAt;
        public static ImGuiTextFn ImGuiText;
        public static ImGuiSeparatorFn ImGuiSeparator;
        public static ImGuiButtonFn ImGuiButton;
        public static ImGuiCheckboxFn ImGuiCheckbox;
        public static ImGuiDragFloatFn ImGuiDragFloat;
        public static ImGuiDragFloat3Fn ImGuiDragFloat3;
        public static ImGuiInputTextFn ImGuiInputText;
        public static ImGuiBeginComboFn ImGuiBeginCombo;
        public static ImGuiEndComboFn ImGuiEndCombo;
        public static ImGuiSelectableFn ImGuiSelectable;
        public static ImGuiAcceptSceneObjectDropFn ImGuiAcceptSceneObjectDrop;
        public static HasAnimationFn HasAnimation;
        public static PlayAnimationFn PlayAnimation;
        public static StopAnimationFn StopAnimation;
        public static PauseAnimationFn PauseAnimation;
        public static ReverseAnimationFn ReverseAnimation;
        public static SetAnimationTimeFn SetAnimationTime;
        public static GetAnimationTimeFn GetAnimationTime;
        public static IsAnimationPlayingFn IsAnimationPlaying;
        public static SetAnimationLoopFn SetAnimationLoop;
        public static SetAnimationPlaySpeedFn SetAnimationPlaySpeed;
        public static SetAnimationPlayOnAwakeFn SetAnimationPlayOnAwake;
        public static IsObjectEnabledFn IsObjectEnabled;
        public static SetObjectEnabledFn SetObjectEnabled;
        public static GetLayerFn GetLayer;
        public static SetLayerFn SetLayer;
        public static HasTagFn HasTag;
        public static IsInLayerFn IsInLayer;
        public static GetTagFn GetTag;
        public static SetTagFn SetTag;
        public static SetPosition2DFn SetPosition2D;
        public static IsSprintDownFn IsSprintDown;
        public static IsJumpDownFn IsJumpDown;
        public static GetMoveInputWASDFn GetMoveInputWASD;
        public static ApplyMouseLookFn ApplyMouseLook;
        public static HasRigidbody2DFn HasRigidbody2D;
        public static SetRigidbody2DVelocityFn SetRigidbody2DVelocity;
        public static GetRigidbody2DVelocityFn GetRigidbody2DVelocity;
        public static AddRigidbodyVelocityFn AddRigidbodyVelocity;
        public static SetRigidbodyAngularVelocityFn SetRigidbodyAngularVelocity;
        public static GetRigidbodyAngularVelocityFn GetRigidbodyAngularVelocity;
        public static AddRigidbodyTorqueFn AddRigidbodyTorque;
        public static AddRigidbodyAngularImpulseFn AddRigidbodyAngularImpulse;
        public static SetRigidbodyYawFn SetRigidbodyYaw;
        public static SetRigidbodyRotationFn SetRigidbodyRotation;
        public static TeleportRigidbodyFn TeleportRigidbody;
        public static RaycastClosestDetailedFn RaycastClosestDetailed;
        public static IsUIButtonPressedFn IsUIButtonPressed;
        public static IsUIInteractableFn IsUIInteractable;
        public static SetUIInteractableFn SetUIInteractable;
        public static GetUISliderValueFn GetUISliderValue;
        public static SetUISliderValueFn SetUISliderValue;
        public static SetUISliderRangeFn SetUISliderRange;
        public static SetUILabelFn SetUILabel;
        public static SetUIColorFn SetUIColor;
        public static GetUITextScaleFn GetUITextScale;
        public static SetUITextScaleFn SetUITextScale;
        public static SetUISliderStyleFn SetUISliderStyle;
        public static SetUIButtonStyleFn SetUIButtonStyle;
        public static SetUIStylePresetFn SetUIStylePreset;
        public static SetFPSCapFn SetFPSCap;
        public static GetSpriteClipCountFn GetSpriteClipCount;
        public static GetSpriteClipIndexFn GetSpriteClipIndex;
        public static GetSpriteClipNameFn GetSpriteClipName;
        public static GetSpriteClipNameAtFn GetSpriteClipNameAt;
        public static SetSpriteClipIndexFn SetSpriteClipIndex;
        public static SetSpriteClipNameFn SetSpriteClipName;
        public static GetSpriteAlphaFn GetSpriteAlpha;
        public static SetSpriteAlphaFn SetSpriteAlpha;
        public static FadeSpriteAlphaFn FadeSpriteAlpha;
        public static FadeSpriteToClipIndexFn FadeSpriteToClipIndex;
        public static FadeSpriteToClipNameFn FadeSpriteToClipName;
        public static HasAudioSourceFn HasAudioSource;
        public static PlayAudioFn PlayAudio;
        public static StopAudioFn StopAudio;
        public static SetAudioLoopFn SetAudioLoop;
        public static SetAudioVolumeFn SetAudioVolume;
        public static SetAudioClipFn SetAudioClip;
        public static PlayAudioOneShotFn PlayAudioOneShot;
        public static MarkDirtyFn MarkDirty;
        public static EnsureCapsuleColliderFn EnsureCapsuleCollider;
        public static GetProjectGravityScaleFn GetProjectGravityScale;
        public static SetProjectGravityScaleFn SetProjectGravityScale;

        public static void BindDelegates() {
            GetObjectId = Marshal.GetDelegateForFunctionPointer<GetObjectIdFn>(Api.GetObjectId);
            GetPosition = Marshal.GetDelegateForFunctionPointer<GetPositionFn>(Api.GetPosition);
            SetPosition = Marshal.GetDelegateForFunctionPointer<SetPositionFn>(Api.SetPosition);
            GetRotation = Marshal.GetDelegateForFunctionPointer<GetRotationFn>(Api.GetRotation);
            SetRotation = Marshal.GetDelegateForFunctionPointer<SetRotationFn>(Api.SetRotation);
            GetScale = Marshal.GetDelegateForFunctionPointer<GetScaleFn>(Api.GetScale);
            SetScale = Marshal.GetDelegateForFunctionPointer<SetScaleFn>(Api.SetScale);
            HasRigidbody = Marshal.GetDelegateForFunctionPointer<HasRigidbodyFn>(Api.HasRigidbody);
            EnsureRigidbody = Marshal.GetDelegateForFunctionPointer<EnsureRigidbodyFn>(Api.EnsureRigidbody);
            SetRigidbodyVelocity = Marshal.GetDelegateForFunctionPointer<SetRigidbodyVelocityFn>(Api.SetRigidbodyVelocity);
            GetRigidbodyVelocity = Marshal.GetDelegateForFunctionPointer<GetRigidbodyVelocityFn>(Api.GetRigidbodyVelocity);
            AddRigidbodyForce = Marshal.GetDelegateForFunctionPointer<AddRigidbodyForceFn>(Api.AddRigidbodyForce);
            AddRigidbodyImpulse = Marshal.GetDelegateForFunctionPointer<AddRigidbodyImpulseFn>(Api.AddRigidbodyImpulse);
            GetSettingFloat = Marshal.GetDelegateForFunctionPointer<GetSettingFloatFn>(Api.GetSettingFloat);
            GetSettingBool = Marshal.GetDelegateForFunctionPointer<GetSettingBoolFn>(Api.GetSettingBool);
            GetSettingString = Marshal.GetDelegateForFunctionPointer<GetSettingStringFn>(Api.GetSettingString);
            SetSettingFloat = Marshal.GetDelegateForFunctionPointer<SetSettingFloatFn>(Api.SetSettingFloat);
            SetSettingBool = Marshal.GetDelegateForFunctionPointer<SetSettingBoolFn>(Api.SetSettingBool);
            SetSettingString = Marshal.GetDelegateForFunctionPointer<SetSettingStringFn>(Api.SetSettingString);
            AddConsoleMessage = Marshal.GetDelegateForFunctionPointer<AddConsoleMessageFn>(Api.AddConsoleMessage);
            FindObjectIdByName = Marshal.GetDelegateForFunctionPointer<FindObjectIdByNameFn>(Api.FindObjectIdByName);
            GetObjectName = Marshal.GetDelegateForFunctionPointer<GetObjectNameFn>(Api.GetObjectName);
            GetSelectedObjectId = Marshal.GetDelegateForFunctionPointer<GetSelectedObjectIdFn>(Api.GetSelectedObjectId);
            ImGuiText = Marshal.GetDelegateForFunctionPointer<ImGuiTextFn>(Api.ImGuiText);
            ImGuiSeparator = Marshal.GetDelegateForFunctionPointer<ImGuiSeparatorFn>(Api.ImGuiSeparator);
            ImGuiButton = Marshal.GetDelegateForFunctionPointer<ImGuiButtonFn>(Api.ImGuiButton);
            ImGuiCheckbox = Marshal.GetDelegateForFunctionPointer<ImGuiCheckboxFn>(Api.ImGuiCheckbox);
            ImGuiDragFloat = Marshal.GetDelegateForFunctionPointer<ImGuiDragFloatFn>(Api.ImGuiDragFloat);
            ImGuiDragFloat3 = Marshal.GetDelegateForFunctionPointer<ImGuiDragFloat3Fn>(Api.ImGuiDragFloat3);
            ImGuiInputText = Marshal.GetDelegateForFunctionPointer<ImGuiInputTextFn>(Api.ImGuiInputText);
            ImGuiAcceptSceneObjectDrop = Marshal.GetDelegateForFunctionPointer<ImGuiAcceptSceneObjectDropFn>(Api.ImGuiAcceptSceneObjectDrop);

            // Optional API extensions (v4+). Keep safe fallbacks for older native layouts.
            if (Api.Version >= 4 && Api.GetSceneObjectCount != IntPtr.Zero) {
                GetSceneObjectCount = Marshal.GetDelegateForFunctionPointer<GetSceneObjectCountFn>(Api.GetSceneObjectCount);
            } else {
                GetSceneObjectCount = _ => 0;
            }
            if (Api.Version >= 4 && Api.GetSceneObjectIdAt != IntPtr.Zero) {
                GetSceneObjectIdAt = Marshal.GetDelegateForFunctionPointer<GetSceneObjectIdAtFn>(Api.GetSceneObjectIdAt);
            } else {
                GetSceneObjectIdAt = (_, _) => -1;
            }
            if (Api.Version >= 4 && Api.ImGuiBeginCombo != IntPtr.Zero) {
                ImGuiBeginCombo = Marshal.GetDelegateForFunctionPointer<ImGuiBeginComboFn>(Api.ImGuiBeginCombo);
            } else {
                ImGuiBeginCombo = (_, _) => 0;
            }
            if (Api.Version >= 4 && Api.ImGuiEndCombo != IntPtr.Zero) {
                ImGuiEndCombo = Marshal.GetDelegateForFunctionPointer<ImGuiEndComboFn>(Api.ImGuiEndCombo);
            } else {
                ImGuiEndCombo = () => { };
            }
            if (Api.Version >= 4 && Api.ImGuiSelectable != IntPtr.Zero) {
                ImGuiSelectable = Marshal.GetDelegateForFunctionPointer<ImGuiSelectableFn>(Api.ImGuiSelectable);
            } else {
                ImGuiSelectable = (_, _) => 0;
            }
            if (Api.Version >= 5 && Api.HasAnimation != IntPtr.Zero) {
                HasAnimation = Marshal.GetDelegateForFunctionPointer<HasAnimationFn>(Api.HasAnimation);
            } else {
                HasAnimation = _ => 0;
            }
            if (Api.Version >= 5 && Api.PlayAnimation != IntPtr.Zero) {
                PlayAnimation = Marshal.GetDelegateForFunctionPointer<PlayAnimationFn>(Api.PlayAnimation);
            } else {
                PlayAnimation = (_, _) => 0;
            }
            if (Api.Version >= 5 && Api.StopAnimation != IntPtr.Zero) {
                StopAnimation = Marshal.GetDelegateForFunctionPointer<StopAnimationFn>(Api.StopAnimation);
            } else {
                StopAnimation = (_, _) => 0;
            }
            if (Api.Version >= 5 && Api.PauseAnimation != IntPtr.Zero) {
                PauseAnimation = Marshal.GetDelegateForFunctionPointer<PauseAnimationFn>(Api.PauseAnimation);
            } else {
                PauseAnimation = (_, _) => 0;
            }
            if (Api.Version >= 5 && Api.ReverseAnimation != IntPtr.Zero) {
                ReverseAnimation = Marshal.GetDelegateForFunctionPointer<ReverseAnimationFn>(Api.ReverseAnimation);
            } else {
                ReverseAnimation = (_, _) => 0;
            }
            if (Api.Version >= 5 && Api.SetAnimationTime != IntPtr.Zero) {
                SetAnimationTime = Marshal.GetDelegateForFunctionPointer<SetAnimationTimeFn>(Api.SetAnimationTime);
            } else {
                SetAnimationTime = (_, _) => 0;
            }
            if (Api.Version >= 5 && Api.GetAnimationTime != IntPtr.Zero) {
                GetAnimationTime = Marshal.GetDelegateForFunctionPointer<GetAnimationTimeFn>(Api.GetAnimationTime);
            } else {
                GetAnimationTime = _ => 0f;
            }
            if (Api.Version >= 5 && Api.IsAnimationPlaying != IntPtr.Zero) {
                IsAnimationPlaying = Marshal.GetDelegateForFunctionPointer<IsAnimationPlayingFn>(Api.IsAnimationPlaying);
            } else {
                IsAnimationPlaying = _ => 0;
            }
            if (Api.Version >= 5 && Api.SetAnimationLoop != IntPtr.Zero) {
                SetAnimationLoop = Marshal.GetDelegateForFunctionPointer<SetAnimationLoopFn>(Api.SetAnimationLoop);
            } else {
                SetAnimationLoop = (_, _) => 0;
            }
            if (Api.Version >= 5 && Api.SetAnimationPlaySpeed != IntPtr.Zero) {
                SetAnimationPlaySpeed = Marshal.GetDelegateForFunctionPointer<SetAnimationPlaySpeedFn>(Api.SetAnimationPlaySpeed);
            } else {
                SetAnimationPlaySpeed = (_, _) => 0;
            }
            if (Api.Version >= 5 && Api.SetAnimationPlayOnAwake != IntPtr.Zero) {
                SetAnimationPlayOnAwake = Marshal.GetDelegateForFunctionPointer<SetAnimationPlayOnAwakeFn>(Api.SetAnimationPlayOnAwake);
            } else {
                SetAnimationPlayOnAwake = (_, _) => 0;
            }
            if (Api.Version >= 6 && Api.IsObjectEnabled != IntPtr.Zero) {
                IsObjectEnabled = Marshal.GetDelegateForFunctionPointer<IsObjectEnabledFn>(Api.IsObjectEnabled);
            } else {
                IsObjectEnabled = _ => 0;
            }
            if (Api.Version >= 6 && Api.SetObjectEnabled != IntPtr.Zero) {
                SetObjectEnabled = Marshal.GetDelegateForFunctionPointer<SetObjectEnabledFn>(Api.SetObjectEnabled);
            } else {
                SetObjectEnabled = (_, _) => { };
            }
            if (Api.Version >= 6 && Api.GetLayer != IntPtr.Zero) {
                GetLayer = Marshal.GetDelegateForFunctionPointer<GetLayerFn>(Api.GetLayer);
            } else {
                GetLayer = _ => 0;
            }
            if (Api.Version >= 6 && Api.SetLayer != IntPtr.Zero) {
                SetLayer = Marshal.GetDelegateForFunctionPointer<SetLayerFn>(Api.SetLayer);
            } else {
                SetLayer = (_, _) => { };
            }
            if (Api.Version >= 6 && Api.HasTag != IntPtr.Zero) {
                HasTag = Marshal.GetDelegateForFunctionPointer<HasTagFn>(Api.HasTag);
            } else {
                HasTag = (_, _) => 0;
            }
            if (Api.Version >= 6 && Api.IsInLayer != IntPtr.Zero) {
                IsInLayer = Marshal.GetDelegateForFunctionPointer<IsInLayerFn>(Api.IsInLayer);
            } else {
                IsInLayer = (_, _) => 0;
            }
            if (Api.Version >= 6 && Api.GetTag != IntPtr.Zero) {
                GetTag = Marshal.GetDelegateForFunctionPointer<GetTagFn>(Api.GetTag);
            } else {
                GetTag = (_, _, _) => { };
            }
            if (Api.Version >= 6 && Api.SetTag != IntPtr.Zero) {
                SetTag = Marshal.GetDelegateForFunctionPointer<SetTagFn>(Api.SetTag);
            } else {
                SetTag = (_, _) => { };
            }
            if (Api.Version >= 6 && Api.SetPosition2D != IntPtr.Zero) {
                SetPosition2D = Marshal.GetDelegateForFunctionPointer<SetPosition2DFn>(Api.SetPosition2D);
            } else {
                SetPosition2D = (_, _, _) => { };
            }
            if (Api.Version >= 6 && Api.IsSprintDown != IntPtr.Zero) {
                IsSprintDown = Marshal.GetDelegateForFunctionPointer<IsSprintDownFn>(Api.IsSprintDown);
            } else {
                IsSprintDown = _ => 0;
            }
            if (Api.Version >= 6 && Api.IsJumpDown != IntPtr.Zero) {
                IsJumpDown = Marshal.GetDelegateForFunctionPointer<IsJumpDownFn>(Api.IsJumpDown);
            } else {
                IsJumpDown = _ => 0;
            }
            if (Api.Version >= 6 && Api.GetMoveInputWASD != IntPtr.Zero) {
                GetMoveInputWASD = Marshal.GetDelegateForFunctionPointer<GetMoveInputWASDFn>(Api.GetMoveInputWASD);
            } else {
                GetMoveInputWASD = (_, _, _, _, _, _) => { };
            }
            if (Api.Version >= 6 && Api.ApplyMouseLook != IntPtr.Zero) {
                ApplyMouseLook = Marshal.GetDelegateForFunctionPointer<ApplyMouseLookFn>(Api.ApplyMouseLook);
            } else {
                ApplyMouseLook = (_, _, _, _, _, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.HasRigidbody2D != IntPtr.Zero) {
                HasRigidbody2D = Marshal.GetDelegateForFunctionPointer<HasRigidbody2DFn>(Api.HasRigidbody2D);
            } else {
                HasRigidbody2D = _ => 0;
            }
            if (Api.Version >= 6 && Api.SetRigidbody2DVelocity != IntPtr.Zero) {
                SetRigidbody2DVelocity = Marshal.GetDelegateForFunctionPointer<SetRigidbody2DVelocityFn>(Api.SetRigidbody2DVelocity);
            } else {
                SetRigidbody2DVelocity = (_, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.GetRigidbody2DVelocity != IntPtr.Zero) {
                GetRigidbody2DVelocity = Marshal.GetDelegateForFunctionPointer<GetRigidbody2DVelocityFn>(Api.GetRigidbody2DVelocity);
            } else {
                GetRigidbody2DVelocity = (_, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.AddRigidbodyVelocity != IntPtr.Zero) {
                AddRigidbodyVelocity = Marshal.GetDelegateForFunctionPointer<AddRigidbodyVelocityFn>(Api.AddRigidbodyVelocity);
            } else {
                AddRigidbodyVelocity = (_, _, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.SetRigidbodyAngularVelocity != IntPtr.Zero) {
                SetRigidbodyAngularVelocity = Marshal.GetDelegateForFunctionPointer<SetRigidbodyAngularVelocityFn>(Api.SetRigidbodyAngularVelocity);
            } else {
                SetRigidbodyAngularVelocity = (_, _, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.GetRigidbodyAngularVelocity != IntPtr.Zero) {
                GetRigidbodyAngularVelocity = Marshal.GetDelegateForFunctionPointer<GetRigidbodyAngularVelocityFn>(Api.GetRigidbodyAngularVelocity);
            } else {
                GetRigidbodyAngularVelocity = (_, _, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.AddRigidbodyTorque != IntPtr.Zero) {
                AddRigidbodyTorque = Marshal.GetDelegateForFunctionPointer<AddRigidbodyTorqueFn>(Api.AddRigidbodyTorque);
            } else {
                AddRigidbodyTorque = (_, _, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.AddRigidbodyAngularImpulse != IntPtr.Zero) {
                AddRigidbodyAngularImpulse = Marshal.GetDelegateForFunctionPointer<AddRigidbodyAngularImpulseFn>(Api.AddRigidbodyAngularImpulse);
            } else {
                AddRigidbodyAngularImpulse = (_, _, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.SetRigidbodyYaw != IntPtr.Zero) {
                SetRigidbodyYaw = Marshal.GetDelegateForFunctionPointer<SetRigidbodyYawFn>(Api.SetRigidbodyYaw);
            } else {
                SetRigidbodyYaw = (_, _) => 0;
            }
            if (Api.Version >= 6 && Api.SetRigidbodyRotation != IntPtr.Zero) {
                SetRigidbodyRotation = Marshal.GetDelegateForFunctionPointer<SetRigidbodyRotationFn>(Api.SetRigidbodyRotation);
            } else {
                SetRigidbodyRotation = (_, _, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.TeleportRigidbody != IntPtr.Zero) {
                TeleportRigidbody = Marshal.GetDelegateForFunctionPointer<TeleportRigidbodyFn>(Api.TeleportRigidbody);
            } else {
                TeleportRigidbody = (_, _, _, _, _, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.RaycastClosestDetailed != IntPtr.Zero) {
                RaycastClosestDetailed = Marshal.GetDelegateForFunctionPointer<RaycastClosestDetailedFn>(Api.RaycastClosestDetailed);
            } else {
                RaycastClosestDetailed = (_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.IsUIButtonPressed != IntPtr.Zero) {
                IsUIButtonPressed = Marshal.GetDelegateForFunctionPointer<IsUIButtonPressedFn>(Api.IsUIButtonPressed);
            } else {
                IsUIButtonPressed = _ => 0;
            }
            if (Api.Version >= 6 && Api.IsUIInteractable != IntPtr.Zero) {
                IsUIInteractable = Marshal.GetDelegateForFunctionPointer<IsUIInteractableFn>(Api.IsUIInteractable);
            } else {
                IsUIInteractable = _ => 0;
            }
            if (Api.Version >= 6 && Api.SetUIInteractable != IntPtr.Zero) {
                SetUIInteractable = Marshal.GetDelegateForFunctionPointer<SetUIInteractableFn>(Api.SetUIInteractable);
            } else {
                SetUIInteractable = (_, _) => { };
            }
            if (Api.Version >= 6 && Api.GetUISliderValue != IntPtr.Zero) {
                GetUISliderValue = Marshal.GetDelegateForFunctionPointer<GetUISliderValueFn>(Api.GetUISliderValue);
            } else {
                GetUISliderValue = _ => 0f;
            }
            if (Api.Version >= 6 && Api.SetUISliderValue != IntPtr.Zero) {
                SetUISliderValue = Marshal.GetDelegateForFunctionPointer<SetUISliderValueFn>(Api.SetUISliderValue);
            } else {
                SetUISliderValue = (_, _) => { };
            }
            if (Api.Version >= 6 && Api.SetUISliderRange != IntPtr.Zero) {
                SetUISliderRange = Marshal.GetDelegateForFunctionPointer<SetUISliderRangeFn>(Api.SetUISliderRange);
            } else {
                SetUISliderRange = (_, _, _) => { };
            }
            if (Api.Version >= 6 && Api.SetUILabel != IntPtr.Zero) {
                SetUILabel = Marshal.GetDelegateForFunctionPointer<SetUILabelFn>(Api.SetUILabel);
            } else {
                SetUILabel = (_, _) => { };
            }
            if (Api.Version >= 6 && Api.SetUIColor != IntPtr.Zero) {
                SetUIColor = Marshal.GetDelegateForFunctionPointer<SetUIColorFn>(Api.SetUIColor);
            } else {
                SetUIColor = (_, _, _, _, _) => { };
            }
            if (Api.Version >= 6 && Api.GetUITextScale != IntPtr.Zero) {
                GetUITextScale = Marshal.GetDelegateForFunctionPointer<GetUITextScaleFn>(Api.GetUITextScale);
            } else {
                GetUITextScale = _ => 1f;
            }
            if (Api.Version >= 6 && Api.SetUITextScale != IntPtr.Zero) {
                SetUITextScale = Marshal.GetDelegateForFunctionPointer<SetUITextScaleFn>(Api.SetUITextScale);
            } else {
                SetUITextScale = (_, _) => { };
            }
            if (Api.Version >= 6 && Api.SetUISliderStyle != IntPtr.Zero) {
                SetUISliderStyle = Marshal.GetDelegateForFunctionPointer<SetUISliderStyleFn>(Api.SetUISliderStyle);
            } else {
                SetUISliderStyle = (_, _) => { };
            }
            if (Api.Version >= 6 && Api.SetUIButtonStyle != IntPtr.Zero) {
                SetUIButtonStyle = Marshal.GetDelegateForFunctionPointer<SetUIButtonStyleFn>(Api.SetUIButtonStyle);
            } else {
                SetUIButtonStyle = (_, _) => { };
            }
            if (Api.Version >= 6 && Api.SetUIStylePreset != IntPtr.Zero) {
                SetUIStylePreset = Marshal.GetDelegateForFunctionPointer<SetUIStylePresetFn>(Api.SetUIStylePreset);
            } else {
                SetUIStylePreset = (_, _) => { };
            }
            if (Api.Version >= 6 && Api.SetFPSCap != IntPtr.Zero) {
                SetFPSCap = Marshal.GetDelegateForFunctionPointer<SetFPSCapFn>(Api.SetFPSCap);
            } else {
                SetFPSCap = (_, _, _) => { };
            }
            if (Api.Version >= 6 && Api.GetSpriteClipCount != IntPtr.Zero) {
                GetSpriteClipCount = Marshal.GetDelegateForFunctionPointer<GetSpriteClipCountFn>(Api.GetSpriteClipCount);
            } else {
                GetSpriteClipCount = _ => 0;
            }
            if (Api.Version >= 6 && Api.GetSpriteClipIndex != IntPtr.Zero) {
                GetSpriteClipIndex = Marshal.GetDelegateForFunctionPointer<GetSpriteClipIndexFn>(Api.GetSpriteClipIndex);
            } else {
                GetSpriteClipIndex = _ => -1;
            }
            if (Api.Version >= 6 && Api.GetSpriteClipName != IntPtr.Zero) {
                GetSpriteClipName = Marshal.GetDelegateForFunctionPointer<GetSpriteClipNameFn>(Api.GetSpriteClipName);
            } else {
                GetSpriteClipName = (_, _, _) => { };
            }
            if (Api.Version >= 6 && Api.GetSpriteClipNameAt != IntPtr.Zero) {
                GetSpriteClipNameAt = Marshal.GetDelegateForFunctionPointer<GetSpriteClipNameAtFn>(Api.GetSpriteClipNameAt);
            } else {
                GetSpriteClipNameAt = (_, _, _, _) => { };
            }
            if (Api.Version >= 6 && Api.SetSpriteClipIndex != IntPtr.Zero) {
                SetSpriteClipIndex = Marshal.GetDelegateForFunctionPointer<SetSpriteClipIndexFn>(Api.SetSpriteClipIndex);
            } else {
                SetSpriteClipIndex = (_, _) => 0;
            }
            if (Api.Version >= 6 && Api.SetSpriteClipName != IntPtr.Zero) {
                SetSpriteClipName = Marshal.GetDelegateForFunctionPointer<SetSpriteClipNameFn>(Api.SetSpriteClipName);
            } else {
                SetSpriteClipName = (_, _) => 0;
            }
            if (Api.Version >= 6 && Api.GetSpriteAlpha != IntPtr.Zero) {
                GetSpriteAlpha = Marshal.GetDelegateForFunctionPointer<GetSpriteAlphaFn>(Api.GetSpriteAlpha);
            } else {
                GetSpriteAlpha = _ => 1f;
            }
            if (Api.Version >= 6 && Api.SetSpriteAlpha != IntPtr.Zero) {
                SetSpriteAlpha = Marshal.GetDelegateForFunctionPointer<SetSpriteAlphaFn>(Api.SetSpriteAlpha);
            } else {
                SetSpriteAlpha = (_, _) => { };
            }
            if (Api.Version >= 6 && Api.FadeSpriteAlpha != IntPtr.Zero) {
                FadeSpriteAlpha = Marshal.GetDelegateForFunctionPointer<FadeSpriteAlphaFn>(Api.FadeSpriteAlpha);
            } else {
                FadeSpriteAlpha = (_, _, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.FadeSpriteToClipIndex != IntPtr.Zero) {
                FadeSpriteToClipIndex = Marshal.GetDelegateForFunctionPointer<FadeSpriteToClipIndexFn>(Api.FadeSpriteToClipIndex);
            } else {
                FadeSpriteToClipIndex = (_, _, _, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.FadeSpriteToClipName != IntPtr.Zero) {
                FadeSpriteToClipName = Marshal.GetDelegateForFunctionPointer<FadeSpriteToClipNameFn>(Api.FadeSpriteToClipName);
            } else {
                FadeSpriteToClipName = (_, _, _, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.HasAudioSource != IntPtr.Zero) {
                HasAudioSource = Marshal.GetDelegateForFunctionPointer<HasAudioSourceFn>(Api.HasAudioSource);
            } else {
                HasAudioSource = _ => 0;
            }
            if (Api.Version >= 6 && Api.PlayAudio != IntPtr.Zero) {
                PlayAudio = Marshal.GetDelegateForFunctionPointer<PlayAudioFn>(Api.PlayAudio);
            } else {
                PlayAudio = _ => 0;
            }
            if (Api.Version >= 6 && Api.StopAudio != IntPtr.Zero) {
                StopAudio = Marshal.GetDelegateForFunctionPointer<StopAudioFn>(Api.StopAudio);
            } else {
                StopAudio = _ => 0;
            }
            if (Api.Version >= 6 && Api.SetAudioLoop != IntPtr.Zero) {
                SetAudioLoop = Marshal.GetDelegateForFunctionPointer<SetAudioLoopFn>(Api.SetAudioLoop);
            } else {
                SetAudioLoop = (_, _) => 0;
            }
            if (Api.Version >= 6 && Api.SetAudioVolume != IntPtr.Zero) {
                SetAudioVolume = Marshal.GetDelegateForFunctionPointer<SetAudioVolumeFn>(Api.SetAudioVolume);
            } else {
                SetAudioVolume = (_, _) => 0;
            }
            if (Api.Version >= 6 && Api.SetAudioClip != IntPtr.Zero) {
                SetAudioClip = Marshal.GetDelegateForFunctionPointer<SetAudioClipFn>(Api.SetAudioClip);
            } else {
                SetAudioClip = (_, _) => 0;
            }
            if (Api.Version >= 6 && Api.PlayAudioOneShot != IntPtr.Zero) {
                PlayAudioOneShot = Marshal.GetDelegateForFunctionPointer<PlayAudioOneShotFn>(Api.PlayAudioOneShot);
            } else {
                PlayAudioOneShot = (_, _, _) => 0;
            }
            if (Api.Version >= 6 && Api.MarkDirty != IntPtr.Zero) {
                MarkDirty = Marshal.GetDelegateForFunctionPointer<MarkDirtyFn>(Api.MarkDirty);
            } else {
                MarkDirty = _ => { };
            }
            if (Api.Version >= 6 && Api.EnsureCapsuleCollider != IntPtr.Zero) {
                EnsureCapsuleCollider = Marshal.GetDelegateForFunctionPointer<EnsureCapsuleColliderFn>(Api.EnsureCapsuleCollider);
            } else {
                EnsureCapsuleCollider = (_, _, _) => 0;
            }
            if (Api.Version >= 7 && Api.GetProjectGravityScale != IntPtr.Zero) {
                GetProjectGravityScale = Marshal.GetDelegateForFunctionPointer<GetProjectGravityScaleFn>(Api.GetProjectGravityScale);
            } else {
                GetProjectGravityScale = _ => 1f;
            }
            if (Api.Version >= 7 && Api.SetProjectGravityScale != IntPtr.Zero) {
                SetProjectGravityScale = Marshal.GetDelegateForFunctionPointer<SetProjectGravityScaleFn>(Api.SetProjectGravityScale);
            } else {
                SetProjectGravityScale = (_, _) => { };
            }
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int GetObjectIdFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void GetPositionFn(IntPtr ctx, float* x, float* y, float* z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetPositionFn(IntPtr ctx, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void GetRotationFn(IntPtr ctx, float* x, float* y, float* z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetRotationFn(IntPtr ctx, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void GetScaleFn(IntPtr ctx, float* x, float* y, float* z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetScaleFn(IntPtr ctx, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int HasRigidbodyFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int EnsureRigidbodyFn(IntPtr ctx, int useGravity, int kinematic);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetRigidbodyVelocityFn(IntPtr ctx, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int GetRigidbodyVelocityFn(IntPtr ctx, float* x, float* y, float* z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int AddRigidbodyForceFn(IntPtr ctx, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int AddRigidbodyImpulseFn(IntPtr ctx, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate float GetSettingFloatFn(IntPtr ctx, byte* key, float fallback);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int GetSettingBoolFn(IntPtr ctx, byte* key, int fallback);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void GetSettingStringFn(IntPtr ctx, byte* key, byte* fallback, byte* outBuffer, int outBufferSize);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetSettingFloatFn(IntPtr ctx, byte* key, float value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetSettingBoolFn(IntPtr ctx, byte* key, int value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetSettingStringFn(IntPtr ctx, byte* key, byte* value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void AddConsoleMessageFn(IntPtr ctx, byte* message, int type);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int FindObjectIdByNameFn(IntPtr ctx, byte* name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void GetObjectNameFn(IntPtr ctx, int id, byte* outBuffer, int outBufferSize);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int GetSelectedObjectIdFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int GetSceneObjectCountFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int GetSceneObjectIdAtFn(IntPtr ctx, int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void ImGuiTextFn(byte* text);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void ImGuiSeparatorFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int ImGuiButtonFn(byte* label);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int ImGuiCheckboxFn(byte* label, int* value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int ImGuiDragFloatFn(byte* label, float* value, float speed, float minValue, float maxValue);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int ImGuiDragFloat3Fn(byte* label, float* values, float speed, float minValue, float maxValue);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int ImGuiInputTextFn(byte* label, byte* buffer, int bufferSize);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int ImGuiBeginComboFn(byte* label, byte* previewValue);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void ImGuiEndComboFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int ImGuiSelectableFn(byte* label, int selected);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int ImGuiAcceptSceneObjectDropFn(int* outId);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int HasAnimationFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int PlayAnimationFn(IntPtr ctx, int restart);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int StopAnimationFn(IntPtr ctx, int resetTime);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int PauseAnimationFn(IntPtr ctx, int pause);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int ReverseAnimationFn(IntPtr ctx, int restartIfStopped);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetAnimationTimeFn(IntPtr ctx, float timeSeconds);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate float GetAnimationTimeFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int IsAnimationPlayingFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetAnimationLoopFn(IntPtr ctx, int loop);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetAnimationPlaySpeedFn(IntPtr ctx, float speed);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetAnimationPlayOnAwakeFn(IntPtr ctx, int playOnAwake);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int IsObjectEnabledFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetObjectEnabledFn(IntPtr ctx, int enabled);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int GetLayerFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetLayerFn(IntPtr ctx, int layer);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int HasTagFn(IntPtr ctx, byte* tag);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int IsInLayerFn(IntPtr ctx, int layer);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void GetTagFn(IntPtr ctx, byte* outBuffer, int outBufferSize);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetTagFn(IntPtr ctx, byte* tag);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetPosition2DFn(IntPtr ctx, float x, float y);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int IsSprintDownFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int IsJumpDownFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void GetMoveInputWASDFn(IntPtr ctx, float pitchDeg, float yawDeg, float* x, float* y, float* z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int ApplyMouseLookFn(IntPtr ctx, float* pitchDeg, float* yawDeg,
                                                    float sensitivity, float maxDelta, float deltaTime, int requireMouseButton);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int HasRigidbody2DFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetRigidbody2DVelocityFn(IntPtr ctx, float x, float y);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int GetRigidbody2DVelocityFn(IntPtr ctx, float* x, float* y);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int AddRigidbodyVelocityFn(IntPtr ctx, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetRigidbodyAngularVelocityFn(IntPtr ctx, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int GetRigidbodyAngularVelocityFn(IntPtr ctx, float* x, float* y, float* z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int AddRigidbodyTorqueFn(IntPtr ctx, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int AddRigidbodyAngularImpulseFn(IntPtr ctx, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetRigidbodyYawFn(IntPtr ctx, float yawDegrees);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetRigidbodyRotationFn(IntPtr ctx, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int TeleportRigidbodyFn(IntPtr ctx, float px, float py, float pz, float rx, float ry, float rz);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int RaycastClosestDetailedFn(IntPtr ctx, float ox, float oy, float oz,
                                                            float dx, float dy, float dz, float distance,
                                                            float* hitPosX, float* hitPosY, float* hitPosZ,
                                                            float* hitNormalX, float* hitNormalY, float* hitNormalZ,
                                                            float* hitDistance, int* hitObjectId,
                                                            float* hitObjectVelX, float* hitObjectVelY, float* hitObjectVelZ,
                                                            float* hitStaticFriction, float* hitDynamicFriction);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int IsUIButtonPressedFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int IsUIInteractableFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetUIInteractableFn(IntPtr ctx, int interactable);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate float GetUISliderValueFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetUISliderValueFn(IntPtr ctx, float value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetUISliderRangeFn(IntPtr ctx, float minValue, float maxValue);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetUILabelFn(IntPtr ctx, byte* label);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetUIColorFn(IntPtr ctx, float r, float g, float b, float a);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate float GetUITextScaleFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetUITextScaleFn(IntPtr ctx, float scale);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetUISliderStyleFn(IntPtr ctx, int style);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetUIButtonStyleFn(IntPtr ctx, int style);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetUIStylePresetFn(IntPtr ctx, byte* name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetFPSCapFn(IntPtr ctx, int enabled, float cap);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int GetSpriteClipCountFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int GetSpriteClipIndexFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void GetSpriteClipNameFn(IntPtr ctx, byte* outBuffer, int outBufferSize);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void GetSpriteClipNameAtFn(IntPtr ctx, int index, byte* outBuffer, int outBufferSize);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetSpriteClipIndexFn(IntPtr ctx, int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetSpriteClipNameFn(IntPtr ctx, byte* name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate float GetSpriteAlphaFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetSpriteAlphaFn(IntPtr ctx, float alpha);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int FadeSpriteAlphaFn(IntPtr ctx, float targetAlpha, float duration, float deltaTime);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int FadeSpriteToClipIndexFn(IntPtr ctx, int clipIndex, float fadeOutDuration, float fadeInDuration, float deltaTime);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int FadeSpriteToClipNameFn(IntPtr ctx, byte* clipName, float fadeOutDuration, float fadeInDuration, float deltaTime);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int HasAudioSourceFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int PlayAudioFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int StopAudioFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetAudioLoopFn(IntPtr ctx, int loop);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetAudioVolumeFn(IntPtr ctx, float volume);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int SetAudioClipFn(IntPtr ctx, byte* path);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int PlayAudioOneShotFn(IntPtr ctx, byte* clipPath, float volumeScale);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void MarkDirtyFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate int EnsureCapsuleColliderFn(IntPtr ctx, float height, float radius);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate float GetProjectGravityScaleFn(IntPtr ctx);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public unsafe delegate void SetProjectGravityScaleFn(IntPtr ctx, float scale);
    }

    public struct ModuObject {
        public int Id;
        public string Name;

        public bool IsValid => Id >= 0;

        public ModuObject(int id, string name) {
            Id = id;
            Name = name ?? string.Empty;
        }

        public override string ToString() {
            return IsValid ? $"{Name} ({Id})" : "<None>";
        }
    }

    public static unsafe class Host {
        public static void SetNativeApi(IntPtr apiPtr) {
            Native.Api = Marshal.PtrToStructure<NativeApi>(apiPtr);
            Native.BindDelegates();
        }
    }

    public readonly unsafe struct Context {
        private readonly IntPtr handle;

        public Context(IntPtr ctx) {
            handle = ctx;
        }

        /// @summary Native object id for this script context.
        /// @usage Use when saving object references in settings or logs.
        /// @returns Object id, or -1 if native context is invalid.
        public int ObjectId => Native.GetObjectId(handle);
        public int SelectedObjectId => Native.GetSelectedObjectId(handle);
        public int SceneObjectCount => Native.GetSceneObjectCount(handle);

        public ModuObject FindObjectByName(string name) {
            if (string.IsNullOrEmpty(name)) return new ModuObject(-1, string.Empty);
            byte[] nameBytes = Encoding.UTF8.GetBytes(name + "\0");
            fixed (byte* namePtr = nameBytes) {
                int id = Native.FindObjectIdByName(handle, namePtr);
                return new ModuObject(id, GetObjectName(id));
            }
        }

        public ModuObject FindObjectById(int id) {
            if (id < 0) return new ModuObject(-1, string.Empty);
            return new ModuObject(id, GetObjectName(id));
        }

        public ModuObject GetObjectByIndex(int index) {
            if (index < 0) return new ModuObject(-1, string.Empty);
            int id = Native.GetSceneObjectIdAt(handle, index);
            if (id < 0) return new ModuObject(-1, string.Empty);
            return new ModuObject(id, GetObjectName(id));
        }

        public string GetObjectName(int id) {
            if (id < 0) return string.Empty;
            const int bufferSize = 256;
            byte* buffer = stackalloc byte[bufferSize];
            Native.GetObjectName(handle, id, buffer, bufferSize);
            return FromUtf8(buffer);
        }

        public Vec3 Position {
            get {
                float x = 0f, y = 0f, z = 0f;
                Native.GetPosition(handle, &x, &y, &z);
                return new Vec3(x, y, z);
            }
            set {
                Native.SetPosition(handle, value.X, value.Y, value.Z);
            }
        }

        public Vec3 Rotation {
            get {
                float x = 0f, y = 0f, z = 0f;
                Native.GetRotation(handle, &x, &y, &z);
                return new Vec3(x, y, z);
            }
            set {
                Native.SetRotation(handle, value.X, value.Y, value.Z);
            }
        }

        public Vec3 Scale {
            get {
                float x = 0f, y = 0f, z = 0f;
                Native.GetScale(handle, &x, &y, &z);
                return new Vec3(x, y, z);
            }
            set {
                Native.SetScale(handle, value.X, value.Y, value.Z);
            }
        }

        public bool HasRigidbody => Native.HasRigidbody(handle) != 0;

        public bool EnsureRigidbody(bool useGravity = true, bool kinematic = false) {
            return Native.EnsureRigidbody(handle, useGravity ? 1 : 0, kinematic ? 1 : 0) != 0;
        }

        public bool EnsureCapsuleCollider(float height, float radius) {
            return Native.EnsureCapsuleCollider(handle, height, radius) != 0;
        }

        public Vec3 RigidbodyVelocity {
            get {
                float x = 0f, y = 0f, z = 0f;
                if (Native.GetRigidbodyVelocity(handle, &x, &y, &z) == 0) {
                    return new Vec3(0f, 0f, 0f);
                }
                return new Vec3(x, y, z);
            }
            set {
                Native.SetRigidbodyVelocity(handle, value.X, value.Y, value.Z);
            }
        }

        public void AddRigidbodyForce(Vec3 force) {
            Native.AddRigidbodyForce(handle, force.X, force.Y, force.Z);
        }

        public void AddRigidbodyImpulse(Vec3 impulse) {
            Native.AddRigidbodyImpulse(handle, impulse.X, impulse.Y, impulse.Z);
        }

        public bool HasAnimation => Native.HasAnimation(handle) != 0;

        public bool PlayAnimation(bool restart = true) {
            return Native.PlayAnimation(handle, restart ? 1 : 0) != 0;
        }

        public bool StopAnimation(bool resetTime = true) {
            return Native.StopAnimation(handle, resetTime ? 1 : 0) != 0;
        }

        public bool PauseAnimation(bool pause = true) {
            return Native.PauseAnimation(handle, pause ? 1 : 0) != 0;
        }

        public bool ReverseAnimation(bool restartIfStopped = true) {
            return Native.ReverseAnimation(handle, restartIfStopped ? 1 : 0) != 0;
        }

        public bool SetAnimationTime(float timeSeconds) {
            return Native.SetAnimationTime(handle, timeSeconds) != 0;
        }

        public float GetAnimationTime() {
            return Native.GetAnimationTime(handle);
        }

        public bool IsAnimationPlaying() {
            return Native.IsAnimationPlaying(handle) != 0;
        }

        public bool SetAnimationLoop(bool loop) {
            return Native.SetAnimationLoop(handle, loop ? 1 : 0) != 0;
        }

        public bool SetAnimationPlaySpeed(float speed) {
            return Native.SetAnimationPlaySpeed(handle, speed) != 0;
        }

        public bool SetAnimationPlayOnAwake(bool playOnAwake) {
            return Native.SetAnimationPlayOnAwake(handle, playOnAwake ? 1 : 0) != 0;
        }

        public bool IsObjectEnabled {
            get => Native.IsObjectEnabled(handle) != 0;
            set => Native.SetObjectEnabled(handle, value ? 1 : 0);
        }

        public int Layer {
            get => Native.GetLayer(handle);
            set => Native.SetLayer(handle, value);
        }

        public string Tag {
            get {
                const int bufferSize = 256;
                byte* buffer = stackalloc byte[bufferSize];
                Native.GetTag(handle, buffer, bufferSize);
                return FromUtf8(buffer);
            }
            set {
                byte[] tagBytes = Encoding.UTF8.GetBytes((value ?? string.Empty) + "\0");
                fixed (byte* tagPtr = tagBytes) {
                    Native.SetTag(handle, tagPtr);
                }
            }
        }

        public bool HasTag(string tag) {
            byte[] tagBytes = Encoding.UTF8.GetBytes((tag ?? string.Empty) + "\0");
            fixed (byte* tagPtr = tagBytes) {
                return Native.HasTag(handle, tagPtr) != 0;
            }
        }

        public bool IsInLayer(int layer) {
            return Native.IsInLayer(handle, layer) != 0;
        }

        public void SetPosition2D(Vec2 value) {
            Native.SetPosition2D(handle, value.X, value.Y);
        }

        public bool IsSprintDown() {
            return Native.IsSprintDown(handle) != 0;
        }

        public bool IsJumpDown() {
            return Native.IsJumpDown(handle) != 0;
        }

        public Vec3 GetMoveInputWASD(float pitchDeg, float yawDeg) {
            float x = 0f, y = 0f, z = 0f;
            Native.GetMoveInputWASD(handle, pitchDeg, yawDeg, &x, &y, &z);
            return new Vec3(x, y, z);
        }

        public bool ApplyMouseLook(ref float pitchDeg, ref float yawDeg,
                                   float sensitivity, float maxDelta, float deltaTime,
                                   bool requireMouseButton = false) {
            fixed (float* pitchPtr = &pitchDeg)
            fixed (float* yawPtr = &yawDeg) {
                return Native.ApplyMouseLook(handle, pitchPtr, yawPtr, sensitivity, maxDelta, deltaTime, requireMouseButton ? 1 : 0) != 0;
            }
        }

        public bool HasRigidbody2D => Native.HasRigidbody2D(handle) != 0;

        public Vec2 Rigidbody2DVelocity {
            get {
                float x = 0f, y = 0f;
                if (Native.GetRigidbody2DVelocity(handle, &x, &y) == 0) {
                    return new Vec2(0f, 0f);
                }
                return new Vec2(x, y);
            }
            set {
                Native.SetRigidbody2DVelocity(handle, value.X, value.Y);
            }
        }

        public bool AddRigidbodyVelocity(Vec3 deltaVelocity) {
            return Native.AddRigidbodyVelocity(handle, deltaVelocity.X, deltaVelocity.Y, deltaVelocity.Z) != 0;
        }

        public bool SetRigidbodyAngularVelocity(Vec3 velocity) {
            return Native.SetRigidbodyAngularVelocity(handle, velocity.X, velocity.Y, velocity.Z) != 0;
        }

        public bool TryGetRigidbodyAngularVelocity(out Vec3 velocity) {
            float x = 0f, y = 0f, z = 0f;
            int ok = Native.GetRigidbodyAngularVelocity(handle, &x, &y, &z);
            velocity = new Vec3(x, y, z);
            return ok != 0;
        }

        public bool AddRigidbodyTorque(Vec3 torque) {
            return Native.AddRigidbodyTorque(handle, torque.X, torque.Y, torque.Z) != 0;
        }

        public bool AddRigidbodyAngularImpulse(Vec3 impulse) {
            return Native.AddRigidbodyAngularImpulse(handle, impulse.X, impulse.Y, impulse.Z) != 0;
        }

        public bool SetRigidbodyYaw(float yawDegrees) {
            return Native.SetRigidbodyYaw(handle, yawDegrees) != 0;
        }

        public bool SetRigidbodyRotation(Vec3 rotationDegrees) {
            return Native.SetRigidbodyRotation(handle, rotationDegrees.X, rotationDegrees.Y, rotationDegrees.Z) != 0;
        }

        public bool TeleportRigidbody(Vec3 position, Vec3 rotationDegrees) {
            return Native.TeleportRigidbody(handle, position.X, position.Y, position.Z,
                                            rotationDegrees.X, rotationDegrees.Y, rotationDegrees.Z) != 0;
        }

        public float ProjectGravityScale {
            get => Native.GetProjectGravityScale(handle);
            set => Native.SetProjectGravityScale(handle, value);
        }

        public bool RaycastClosestDetailed(Vec3 origin, Vec3 direction, float distance, out RaycastHit hit) {
            hit = new RaycastHit();
            float hitPosX = 0f, hitPosY = 0f, hitPosZ = 0f;
            float hitNormalX = 0f, hitNormalY = 0f, hitNormalZ = 0f;
            float hitDistance = 0f;
            int hitObjectId = -1;
            float hitObjectVelX = 0f, hitObjectVelY = 0f, hitObjectVelZ = 0f;
            float hitStaticFriction = 0f, hitDynamicFriction = 0f;

            int ok = Native.RaycastClosestDetailed(handle, origin.X, origin.Y, origin.Z,
                                                   direction.X, direction.Y, direction.Z, distance,
                                                   &hitPosX, &hitPosY, &hitPosZ,
                                                   &hitNormalX, &hitNormalY, &hitNormalZ,
                                                   &hitDistance, &hitObjectId,
                                                   &hitObjectVelX, &hitObjectVelY, &hitObjectVelZ,
                                                   &hitStaticFriction, &hitDynamicFriction);
            if (ok == 0) {
                return false;
            }

            hit.Position = new Vec3(hitPosX, hitPosY, hitPosZ);
            hit.Normal = new Vec3(hitNormalX, hitNormalY, hitNormalZ);
            hit.Distance = hitDistance;
            hit.ObjectId = hitObjectId;
            hit.ObjectVelocity = new Vec3(hitObjectVelX, hitObjectVelY, hitObjectVelZ);
            hit.StaticFriction = hitStaticFriction;
            hit.DynamicFriction = hitDynamicFriction;
            return true;
        }

        public bool IsUIButtonPressed() {
            return Native.IsUIButtonPressed(handle) != 0;
        }

        public bool IsUIInteractable {
            get => Native.IsUIInteractable(handle) != 0;
            set => Native.SetUIInteractable(handle, value ? 1 : 0);
        }

        public float UISliderValue {
            get => Native.GetUISliderValue(handle);
            set => Native.SetUISliderValue(handle, value);
        }

        public void SetUISliderRange(float minValue, float maxValue) {
            Native.SetUISliderRange(handle, minValue, maxValue);
        }

        public void SetUILabel(string label) {
            byte[] labelBytes = Encoding.UTF8.GetBytes((label ?? string.Empty) + "\0");
            fixed (byte* labelPtr = labelBytes) {
                Native.SetUILabel(handle, labelPtr);
            }
        }

        public void SetUIColor(float r, float g, float b, float a) {
            Native.SetUIColor(handle, r, g, b, a);
        }

        public float UITextScale {
            get => Native.GetUITextScale(handle);
            set => Native.SetUITextScale(handle, value);
        }

        public void SetUISliderStyle(int style) {
            Native.SetUISliderStyle(handle, style);
        }

        public void SetUIButtonStyle(int style) {
            Native.SetUIButtonStyle(handle, style);
        }

        public void SetUIStylePreset(string name) {
            byte[] nameBytes = Encoding.UTF8.GetBytes((name ?? string.Empty) + "\0");
            fixed (byte* namePtr = nameBytes) {
                Native.SetUIStylePreset(handle, namePtr);
            }
        }

        public void SetFPSCap(bool enabled, float cap = 120f) {
            Native.SetFPSCap(handle, enabled ? 1 : 0, cap);
        }

        public int SpriteClipCount => Native.GetSpriteClipCount(handle);

        public int SpriteClipIndex => Native.GetSpriteClipIndex(handle);

        public string GetSpriteClipName() {
            const int bufferSize = 256;
            byte* buffer = stackalloc byte[bufferSize];
            Native.GetSpriteClipName(handle, buffer, bufferSize);
            return FromUtf8(buffer);
        }

        public string GetSpriteClipNameAt(int index) {
            const int bufferSize = 256;
            byte* buffer = stackalloc byte[bufferSize];
            Native.GetSpriteClipNameAt(handle, index, buffer, bufferSize);
            return FromUtf8(buffer);
        }

        public bool SetSpriteClipIndex(int index) {
            return Native.SetSpriteClipIndex(handle, index) != 0;
        }

        public bool SetSpriteClipName(string name) {
            byte[] nameBytes = Encoding.UTF8.GetBytes((name ?? string.Empty) + "\0");
            fixed (byte* namePtr = nameBytes) {
                return Native.SetSpriteClipName(handle, namePtr) != 0;
            }
        }

        public float SpriteAlpha {
            get => Native.GetSpriteAlpha(handle);
            set => Native.SetSpriteAlpha(handle, value);
        }

        public bool FadeSpriteAlpha(float targetAlpha, float duration, float deltaTime) {
            return Native.FadeSpriteAlpha(handle, targetAlpha, duration, deltaTime) != 0;
        }

        public bool FadeSpriteToClipIndex(int clipIndex, float fadeOutDuration, float fadeInDuration, float deltaTime) {
            return Native.FadeSpriteToClipIndex(handle, clipIndex, fadeOutDuration, fadeInDuration, deltaTime) != 0;
        }

        public bool FadeSpriteToClipName(string clipName, float fadeOutDuration, float fadeInDuration, float deltaTime) {
            byte[] clipBytes = Encoding.UTF8.GetBytes((clipName ?? string.Empty) + "\0");
            fixed (byte* clipPtr = clipBytes) {
                return Native.FadeSpriteToClipName(handle, clipPtr, fadeOutDuration, fadeInDuration, deltaTime) != 0;
            }
        }

        public bool HasAudioSource => Native.HasAudioSource(handle) != 0;

        public bool PlayAudio() {
            return Native.PlayAudio(handle) != 0;
        }

        public bool StopAudio() {
            return Native.StopAudio(handle) != 0;
        }

        public bool SetAudioLoop(bool loop) {
            return Native.SetAudioLoop(handle, loop ? 1 : 0) != 0;
        }

        public bool SetAudioVolume(float volume) {
            return Native.SetAudioVolume(handle, volume) != 0;
        }

        public bool SetAudioClip(string path) {
            byte[] pathBytes = Encoding.UTF8.GetBytes((path ?? string.Empty) + "\0");
            fixed (byte* pathPtr = pathBytes) {
                return Native.SetAudioClip(handle, pathPtr) != 0;
            }
        }

        public bool PlayAudioOneShot(string clipPath = "", float volumeScale = 1.0f) {
            byte[] pathBytes = Encoding.UTF8.GetBytes((clipPath ?? string.Empty) + "\0");
            fixed (byte* pathPtr = pathBytes) {
                return Native.PlayAudioOneShot(handle, pathPtr, volumeScale) != 0;
            }
        }

        public void MarkDirty() {
            Native.MarkDirty(handle);
        }

        public float GetSettingFloat(string key, float fallback = 0f) {
            byte[] keyBytes = Encoding.UTF8.GetBytes((key ?? string.Empty) + "\0");
            fixed (byte* keyPtr = keyBytes) {
                return Native.GetSettingFloat(handle, keyPtr, fallback);
            }
        }

        public bool GetSettingBool(string key, bool fallback = false) {
            byte[] keyBytes = Encoding.UTF8.GetBytes((key ?? string.Empty) + "\0");
            fixed (byte* keyPtr = keyBytes) {
                int value = Native.GetSettingBool(handle, keyPtr, fallback ? 1 : 0);
                return value != 0;
            }
        }

        public string GetSettingString(string key, string fallback = "") {
            const int bufferSize = 256;
            byte[] keyBytes = Encoding.UTF8.GetBytes((key ?? string.Empty) + "\0");
            byte[] fallbackBytes = Encoding.UTF8.GetBytes((fallback ?? string.Empty) + "\0");
            byte* buffer = stackalloc byte[bufferSize];
            fixed (byte* keyPtr = keyBytes)
            fixed (byte* fallbackPtr = fallbackBytes) {
                Native.GetSettingString(handle, keyPtr, fallbackPtr, buffer, bufferSize);
            }
            return FromUtf8(buffer);
        }

        public void SetSettingFloat(string key, float value) {
            byte[] keyBytes = Encoding.UTF8.GetBytes((key ?? string.Empty) + "\0");
            fixed (byte* keyPtr = keyBytes) {
                Native.SetSettingFloat(handle, keyPtr, value);
            }
        }

        public void SetSettingBool(string key, bool value) {
            byte[] keyBytes = Encoding.UTF8.GetBytes((key ?? string.Empty) + "\0");
            fixed (byte* keyPtr = keyBytes) {
                Native.SetSettingBool(handle, keyPtr, value ? 1 : 0);
            }
        }

        public void SetSettingString(string key, string value) {
            byte[] keyBytes = Encoding.UTF8.GetBytes((key ?? string.Empty) + "\0");
            byte[] valueBytes = Encoding.UTF8.GetBytes((value ?? string.Empty) + "\0");
            fixed (byte* keyPtr = keyBytes)
            fixed (byte* valuePtr = valueBytes) {
                Native.SetSettingString(handle, keyPtr, valuePtr);
            }
        }

        public void AddConsoleMessage(string message, ConsoleMessageType type = ConsoleMessageType.Info) {
            byte[] msgBytes = Encoding.UTF8.GetBytes((message ?? string.Empty) + "\0");
            fixed (byte* msgPtr = msgBytes) {
                Native.AddConsoleMessage(handle, msgPtr, (int)type);
            }
        }

        public void AutoSetting(string key, ref bool value, bool save = true) {
            if (!AutoSettings.IsLoaded(handle, key, AutoSettings.ValueType.Bool)) {
                value = GetSettingBool(key, value);
                AutoSettings.MarkLoaded(handle, key, AutoSettings.ValueType.Bool);
            }
            if (save) {
                SetSettingBool(key, value);
            }
        }

        public void AutoSetting(string key, ref float value, bool save = true) {
            if (!AutoSettings.IsLoaded(handle, key, AutoSettings.ValueType.Float)) {
                value = GetSettingFloat(key, value);
                AutoSettings.MarkLoaded(handle, key, AutoSettings.ValueType.Float);
            }
            if (save) {
                SetSettingFloat(key, value);
            }
        }

        public void AutoSetting(string key, ref Vec3 value, bool save = true) {
            if (!AutoSettings.IsLoaded(handle, key, AutoSettings.ValueType.Vec3)) {
                string loaded = GetSettingString(key, "");
                if (!string.IsNullOrEmpty(loaded)) {
                    ParseVec3(loaded, ref value);
                }
                AutoSettings.MarkLoaded(handle, key, AutoSettings.ValueType.Vec3);
            }
            if (save) {
                SetSettingString(key, $"{value.X},{value.Y},{value.Z}");
            }
        }

        public void AutoSetting(string key, ref string value, int bufferSize = 256, bool save = true) {
            if (!AutoSettings.IsLoaded(handle, key, AutoSettings.ValueType.String)) {
                value = GetSettingString(key, value ?? string.Empty);
                AutoSettings.MarkLoaded(handle, key, AutoSettings.ValueType.String);
            }
            if (save) {
                SetSettingString(key, value ?? string.Empty);
            }
        }

        public void AutoSetting(string key, ref ModuObject value, bool save = true) {
            if (!AutoSettings.IsLoaded(handle, key, AutoSettings.ValueType.Object)) {
                string stored = GetSettingString(key, "");
                value = ParseObjectSetting(stored);
                if (!value.IsValid && !string.IsNullOrEmpty(value.Name)) {
                    value = FindObjectByName(value.Name);
                } else if (value.IsValid && string.IsNullOrEmpty(value.Name)) {
                    value.Name = GetObjectName(value.Id);
                }
                AutoSettings.MarkLoaded(handle, key, AutoSettings.ValueType.Object);
            }
            if (save) {
                SetSettingString(key, BuildObjectSetting(value));
            }
        }

        public void AutoSettingsFrom(object instance, bool save = true) {
            if (instance == null) return;
            Inspector.AutoSettingsFrom(this, instance, save);
        }

        private ModuObject ParseObjectSetting(string value) {
            if (string.IsNullOrEmpty(value)) return new ModuObject(-1, string.Empty);
            string[] parts = value.Split('|');
            int id = -1;
            string name = string.Empty;
            if (parts.Length > 0) int.TryParse(parts[0], out id);
            if (parts.Length > 1) name = parts[1];
            return new ModuObject(id, name);
        }

        private string BuildObjectSetting(ModuObject value) {
            if (!value.IsValid && string.IsNullOrEmpty(value.Name)) return string.Empty;
            return $"{value.Id}|{value.Name}";
        }

        private static void ParseVec3(string value, ref Vec3 outVec) {
            if (string.IsNullOrEmpty(value)) return;
            string[] parts = value.Split(',');
            if (parts.Length > 0 && float.TryParse(parts[0], out float x)) outVec.X = x;
            if (parts.Length > 1 && float.TryParse(parts[1], out float y)) outVec.Y = y;
            if (parts.Length > 2 && float.TryParse(parts[2], out float z)) outVec.Z = z;
        }

        internal static string FromUtf8(byte* ptr) {
            if (ptr == null) return string.Empty;
            int length = 0;
            while (ptr[length] != 0) {
                length++;
            }
            if (length == 0) return string.Empty;
            byte[] bytes = new byte[length];
            Marshal.Copy((IntPtr)ptr, bytes, 0, length);
            return Encoding.UTF8.GetString(bytes);
        }
    }

    internal static class AutoSettings {
        internal enum ValueType { Bool, Float, Vec3, String, Object }
        private static readonly System.Collections.Generic.HashSet<string> Loaded =
            new System.Collections.Generic.HashSet<string>();

        internal static bool IsLoaded(IntPtr handle, string key, ValueType type) {
            return Loaded.Contains(MakeKey(handle, key, type));
        }

        internal static void MarkLoaded(IntPtr handle, string key, ValueType type) {
            string id = MakeKey(handle, key, type);
            Loaded.Add(id);
        }

        private static string MakeKey(IntPtr handle, string key, ValueType type) {
            return handle.ToInt64().ToString() + "|" + type.ToString() + "|" + (key ?? string.Empty);
        }
    }

    public static unsafe class ImGui {
        public static void Text(string text) {
            byte[] textBytes = Encoding.UTF8.GetBytes((text ?? string.Empty) + "\0");
            fixed (byte* textPtr = textBytes) {
                Native.ImGuiText(textPtr);
            }
        }

        public static void Separator() {
            Native.ImGuiSeparator();
        }

        public static bool Button(string label) {
            byte[] labelBytes = Encoding.UTF8.GetBytes((label ?? string.Empty) + "\0");
            fixed (byte* labelPtr = labelBytes) {
                return Native.ImGuiButton(labelPtr) != 0;
            }
        }

        public static bool Checkbox(string label, ref bool value) {
            byte[] labelBytes = Encoding.UTF8.GetBytes((label ?? string.Empty) + "\0");
            int v = value ? 1 : 0;
            fixed (byte* labelPtr = labelBytes) {
                int changed = Native.ImGuiCheckbox(labelPtr, &v);
                value = v != 0;
                return changed != 0;
            }
        }

        public static bool DragFloat(string label, ref float value, float speed = 0.1f, float minValue = 0.0f, float maxValue = 0.0f) {
            byte[] labelBytes = Encoding.UTF8.GetBytes((label ?? string.Empty) + "\0");
            fixed (byte* labelPtr = labelBytes)
            fixed (float* valuePtr = &value) {
                return Native.ImGuiDragFloat(labelPtr, valuePtr, speed, minValue, maxValue) != 0;
            }
        }

        public static bool DragFloat3(string label, ref Vec3 value, float speed = 0.1f, float minValue = 0.0f, float maxValue = 0.0f) {
            byte[] labelBytes = Encoding.UTF8.GetBytes((label ?? string.Empty) + "\0");
            fixed (byte* labelPtr = labelBytes)
            fixed (Vec3* valuePtr = &value) {
                return Native.ImGuiDragFloat3(labelPtr, (float*)valuePtr, speed, minValue, maxValue) != 0;
            }
        }

        public static bool InputText(string label, ref string value, int bufferSize = 256) {
            if (bufferSize <= 0) return false;
            byte[] labelBytes = Encoding.UTF8.GetBytes((label ?? string.Empty) + "\0");
            byte[] buffer = new byte[bufferSize];
            string current = value ?? string.Empty;
            int count = Encoding.UTF8.GetBytes(current, 0, current.Length, buffer, 0);
            if (count < bufferSize) {
                buffer[count] = 0;
            } else {
                buffer[bufferSize - 1] = 0;
            }
            fixed (byte* labelPtr = labelBytes)
            fixed (byte* bufferPtr = buffer) {
                int changed = Native.ImGuiInputText(labelPtr, bufferPtr, bufferSize);
                if (changed != 0) {
                    value = Context.FromUtf8(bufferPtr);
                }
                return changed != 0;
            }
        }

        public static bool BeginCombo(string label, string previewValue) {
            byte[] labelBytes = Encoding.UTF8.GetBytes((label ?? string.Empty) + "\0");
            byte[] previewBytes = Encoding.UTF8.GetBytes((previewValue ?? string.Empty) + "\0");
            fixed (byte* labelPtr = labelBytes)
            fixed (byte* previewPtr = previewBytes) {
                return Native.ImGuiBeginCombo(labelPtr, previewPtr) != 0;
            }
        }

        public static void EndCombo() {
            Native.ImGuiEndCombo();
        }

        public static bool Selectable(string label, bool selected = false) {
            byte[] labelBytes = Encoding.UTF8.GetBytes((label ?? string.Empty) + "\0");
            fixed (byte* labelPtr = labelBytes) {
                return Native.ImGuiSelectable(labelPtr, selected ? 1 : 0) != 0;
            }
        }

        public static bool AcceptSceneObjectDrop(out int id) {
            id = -1;
            int result = 0;
            fixed (int* idPtr = &id) {
                result = Native.ImGuiAcceptSceneObjectDrop(idPtr);
            }
            return result != 0;
        }
    }

    [AttributeUsage(AttributeTargets.Class | AttributeTargets.Field)]
    public sealed class HeadTextAttribute : Attribute {
        public string Text { get; }
        public HeadTextAttribute(string text) {
            Text = text;
        }
    }

    [AttributeUsage(AttributeTargets.Field)]
    public sealed class LabelAttribute : Attribute {
        public string Text { get; }
        public LabelAttribute(string text) {
            Text = text;
        }
    }

    [AttributeUsage(AttributeTargets.Field)]
    public sealed class SettingKeyAttribute : Attribute {
        public string Key { get; }
        public SettingKeyAttribute(string key) {
            Key = key;
        }
    }

    [AttributeUsage(AttributeTargets.Field)]
    public sealed class DragSpeedAttribute : Attribute {
        public float Speed { get; }
        public DragSpeedAttribute(float speed) {
            Speed = speed;
        }
    }

    [AttributeUsage(AttributeTargets.Field)]
    public sealed class InspectorIgnoreAttribute : Attribute {
    }

    public static class Inspector {
        private static readonly System.Collections.Generic.HashSet<string> ErrorOnce =
            new System.Collections.Generic.HashSet<string>();
        private static readonly System.Collections.Generic.Dictionary<string, string> ObjectPickerFilter =
            new System.Collections.Generic.Dictionary<string, string>();
        private static bool AttributesEnabled = true;
        private static bool AutoSettingsEnabled = true;
        public static void RenderAuto(IntPtr ctx, object instance) {
            if (instance == null) return;
            try {
                var context = new Context(ctx);
                AutoSettingsFrom(context, instance, true);
                AutoInspect(context, instance);
            } catch (Exception ex) {
                var context = new Context(ctx);
                LogOnce(context, "Inspector", instance, ex);
            }
        }

        internal static void AutoSettingsFrom(Context context, object instance, bool save) {
            if (instance == null) return;
            if (!AutoSettingsEnabled) return;
            try {
                var type = instance.GetType();
                var fields = type.GetFields(System.Reflection.BindingFlags.Instance |
                                            System.Reflection.BindingFlags.Public |
                                            System.Reflection.BindingFlags.NonPublic);
                foreach (var field in fields) {
                    // Avoid attribute reflection here; it can trigger missing System.Native on some Mono builds.
                    // Attribute-driven behavior is handled in AutoInspect only.
                    string key = field.Name;
                    var fieldType = field.FieldType;
                    if (fieldType == typeof(bool)) {
                        bool v = (bool)field.GetValue(instance);
                        context.AutoSetting(key, ref v, save);
                        field.SetValue(instance, v);
                    } else if (fieldType == typeof(float)) {
                        float v = (float)field.GetValue(instance);
                        context.AutoSetting(key, ref v, save);
                        field.SetValue(instance, v);
                    } else if (fieldType == typeof(Vec3)) {
                        Vec3 v = (Vec3)field.GetValue(instance);
                        context.AutoSetting(key, ref v, save);
                        field.SetValue(instance, v);
                    } else if (fieldType == typeof(string)) {
                        string v = (string)field.GetValue(instance);
                        context.AutoSetting(key, ref v, 256, save);
                        field.SetValue(instance, v);
                    } else if (fieldType == typeof(ModuObject)) {
                        ModuObject v = (ModuObject)field.GetValue(instance);
                        context.AutoSetting(key, ref v, save);
                        field.SetValue(instance, v);
                    }
                }
            } catch (Exception ex) {
                if (IsSystemNativeMissing(ex)) {
                    AutoSettingsEnabled = false;
                    return;
                }
                LogOnce(context, "AutoSettings", instance, ex);
            }
        }

        public static void AutoInspect(Context context, object instance) {
            if (instance == null) return;
            try {
                var type = instance.GetType();
                if (AttributesEnabled) {
                    var head = TryGetAttribute<HeadTextAttribute>(type);
                    if (head != null && !string.IsNullOrEmpty(head.Text)) {
                        ImGui.Text(head.Text);
                    }
                }

                var fields = type.GetFields(System.Reflection.BindingFlags.Instance |
                                            System.Reflection.BindingFlags.Public |
                                            System.Reflection.BindingFlags.NonPublic);
                foreach (var field in fields) {
                    if (AttributesEnabled && TryGetAttribute<InspectorIgnoreAttribute>(field) != null) continue;
                    var fieldType = field.FieldType;
                    string label = Humanize(field.Name);
                    if (AttributesEnabled) {
                        var labelAttr = TryGetAttribute<LabelAttribute>(field);
                        if (labelAttr != null && !string.IsNullOrEmpty(labelAttr.Text)) {
                            label = labelAttr.Text;
                        }
                        var headAttr = TryGetAttribute<HeadTextAttribute>(field);
                        if (headAttr != null && !string.IsNullOrEmpty(headAttr.Text)) {
                            ImGui.Text(headAttr.Text);
                        }
                    }

                    float speed = 0.1f;
                    if (AttributesEnabled) {
                        var speedAttr = TryGetAttribute<DragSpeedAttribute>(field);
                        if (speedAttr != null) speed = speedAttr.Speed;
                    }

                    if (fieldType == typeof(bool)) {
                        bool v = (bool)field.GetValue(instance);
                        if (ImGui.Checkbox(label, ref v)) {
                            field.SetValue(instance, v);
                        }
                    } else if (fieldType == typeof(float)) {
                        float v = (float)field.GetValue(instance);
                        if (ImGui.DragFloat(label, ref v, speed)) {
                            field.SetValue(instance, v);
                        }
                    } else if (fieldType == typeof(Vec3)) {
                        Vec3 v = (Vec3)field.GetValue(instance);
                        if (ImGui.DragFloat3(label, ref v, speed)) {
                            field.SetValue(instance, v);
                        }
                    } else if (fieldType == typeof(string)) {
                        string v = (string)field.GetValue(instance);
                        if (ImGui.InputText(label, ref v, 256)) {
                            field.SetValue(instance, v);
                        }
                    } else if (fieldType == typeof(ModuObject)) {
                        ModuObject v = (ModuObject)field.GetValue(instance);
                        if (ObjectField(context, label, ref v)) {
                            field.SetValue(instance, v);
                        }
                    }
                }
            } catch (Exception ex) {
                LogOnce(context, "AutoInspect", instance, ex);
            }
        }

        private static void LogOnce(Context context, string stage, object instance, Exception ex) {
            if (instance == null) return;
            string typeName = instance.GetType().FullName ?? instance.GetType().Name;
            string key = stage + ":" + typeName;
            if (ErrorOnce.Contains(key)) return;
            ErrorOnce.Add(key);
            string msg = stage + " failed for " + typeName + ": " + ex.GetType().Name;
            if (!string.IsNullOrEmpty(ex.Message)) {
                msg += " - " + ex.Message;
            }
            if (ex.InnerException != null) {
                msg += " (Inner: " + ex.InnerException.GetType().Name;
                if (!string.IsNullOrEmpty(ex.InnerException.Message)) {
                    msg += " - " + ex.InnerException.Message;
                }
                msg += ")";
            }
            context.AddConsoleMessage(msg, ConsoleMessageType.Error);
        }

        private static bool IsSystemNativeMissing(Exception ex) {
            if (ex is TypeInitializationException && ex.InnerException is DllNotFoundException) {
                string msg = ex.InnerException.Message ?? string.Empty;
                return msg.Contains("System.Native");
            }
            if (ex is DllNotFoundException) {
                string msg = ex.Message ?? string.Empty;
                return msg.Contains("System.Native");
            }
            return false;
        }

        private static T TryGetAttribute<T>(System.Reflection.MemberInfo member) where T : Attribute {
            if (!AttributesEnabled) return null;
            try {
                return (T)Attribute.GetCustomAttribute(member, typeof(T));
            } catch {
                AttributesEnabled = false;
                return null;
            }
        }

        private static bool ObjectField(Context context, string label, ref ModuObject value) {
            bool changed = false;
            if (value.IsValid) {
                string refreshedName = context.GetObjectName(value.Id);
                if (!string.IsNullOrEmpty(refreshedName)) {
                    value.Name = refreshedName;
                }
            }

            ImGui.Text(label);
            string display = value.IsValid ? value.ToString() : "<None>";
            string stableKey = label ?? string.Empty;
            if (!ObjectPickerFilter.TryGetValue(stableKey, out string filter)) {
                filter = string.Empty;
                ObjectPickerFilter[stableKey] = filter;
            }

            bool comboOpen = ImGui.BeginCombo("##ObjectPicker_" + stableKey, display);

            int droppedId;
            if (ImGui.AcceptSceneObjectDrop(out droppedId)) {
                value = context.FindObjectById(droppedId);
                changed = true;
            }

            if (comboOpen) {
                if (ImGui.InputText("Find##ObjectPicker_" + stableKey, ref filter, 128)) {
                    ObjectPickerFilter[stableKey] = filter;
                }

                if (ImGui.Selectable("<None>", !value.IsValid)) {
                    value = new ModuObject(-1, string.Empty);
                    changed = true;
                }

                int objectCount = context.SceneObjectCount;
                for (int i = 0; i < objectCount; ++i) {
                    ModuObject candidate = context.GetObjectByIndex(i);
                    if (!candidate.IsValid) continue;
                    if (!PassesObjectFilter(candidate, filter)) continue;

                    bool selected = value.IsValid && candidate.Id == value.Id;
                    if (ImGui.Selectable(candidate.ToString(), selected)) {
                        value = candidate;
                        changed = true;
                    }
                }

                ImGui.EndCombo();
            }

            if (ImGui.Button("Use Selected##" + stableKey)) {
                int selected = context.SelectedObjectId;
                if (selected >= 0) {
                    value = context.FindObjectById(selected);
                    changed = true;
                }
            }
            if (ImGui.Button("Clear##" + stableKey)) {
                if (value.IsValid || !string.IsNullOrEmpty(value.Name)) {
                    value = new ModuObject(-1, string.Empty);
                    changed = true;
                }
            }
            if (value.IsValid && string.IsNullOrEmpty(value.Name)) {
                value.Name = context.GetObjectName(value.Id);
            }
            return changed;
        }

        private static bool PassesObjectFilter(ModuObject candidate, string filter) {
            if (string.IsNullOrWhiteSpace(filter)) return true;
            string trimmed = filter.Trim();
            if (candidate.Name != null &&
                candidate.Name.IndexOf(trimmed, StringComparison.OrdinalIgnoreCase) >= 0) {
                return true;
            }
            return candidate.Id.ToString().IndexOf(trimmed, StringComparison.OrdinalIgnoreCase) >= 0;
        }

        private static string Humanize(string name) {
            if (string.IsNullOrEmpty(name)) return string.Empty;
            System.Text.StringBuilder sb = new System.Text.StringBuilder(name.Length + 4);
            sb.Append(char.ToUpper(name[0]));
            for (int i = 1; i < name.Length; ++i) {
                char c = name[i];
                if (char.IsUpper(c) && !char.IsWhiteSpace(name[i - 1])) sb.Append(' ');
                sb.Append(c);
            }
            return sb.ToString();
        }
    }
}
