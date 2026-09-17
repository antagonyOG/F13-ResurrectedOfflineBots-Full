// Compile the frozen implementation through a narrow adapter translation unit.
// Do not edit Features.cpp: its authoritative hash is verified by the package.
#include <cstdint>
extern "C" __declspec(noinline) bool SafeProcessEventCall(
    uintptr_t vptr,
    void* obj,
    void* func,
    void* params);
extern "C" __declspec(noinline) bool FrozenBridgeSafeProcessEventCall(
    uintptr_t vptr,
    void* obj,
    void* func,
    void* params);
extern "C" void FrozenBridgeRegisterAdditionalJason(
    void* pawn,
    void* controller);
extern "C" bool FrozenBridgeDriveAdditionalJason(
    void* controller,
    float deltaSeconds);

// Keep the frozen source byte-for-byte authoritative while interposing its
// ProcessEvent calls inside this translation unit.  The adapter forwards every
// call except an ordinary Jason K2_TeleportTo attempted while he is already
// within walking distance of the nearest counselor.
#define SafeProcessEventCall FrozenBridgeSafeProcessEventCall
#include "../Features/Features.cpp"
#undef SafeProcessEventCall
#include "FrozenJasonBridge.hpp"
#include "../../../vendor/minhook/include/MinHook.h"
#include <cstring>

struct AdditionalCombatJasonState
{
    UWorld* World = nullptr;
    AActor* Jason = nullptr;
    UObject* Controller = nullptr;
    AActor* Target = nullptr;
    ULONGLONG NextMoveAt = 0;
    ULONGLONG NextCombatAt = 0;
    ULONGLONG AttackReleaseAt = 0;
    bool AttackPressed = false;
    bool FinalKillStopped = false;
};

static std::vector<AdditionalCombatJasonState>
    g_AdditionalCombatJasons;

static int32_t g_TrapTeleportExemptionDepth = 0;
static UFunction* g_MinimumMorphTeleportFunction = nullptr;
static ULONGLONG g_NextMinimumMorphLogAt = 0;
static ULONGLONG g_NextMorphCooldownRefusalLogAt = 0;

extern "C" __declspec(noinline) bool FrozenBridgeSafeProcessEventCall(
    uintptr_t vptr,
    void* obj,
    void* func,
    void* params)
{
    AActor* jason = g_JasonAIState.Jason;
    const bool mayBeJasonTeleport =
        g_JasonAIState.Active &&
        jason &&
        obj == jason &&
        func &&
        params;
    bool forwardedStuckRecoveryTeleport = false;

    if (mayBeJasonTeleport &&
        Memory::IsReadable(jason, sizeof(UObject)) &&
        jason->Class &&
        Memory::IsReadable(jason->Class, sizeof(UObject)))
    {
        if (!g_MinimumMorphTeleportFunction ||
            !Memory::IsReadable(
                g_MinimumMorphTeleportFunction,
                sizeof(UObject)))
        {
            g_MinimumMorphTeleportFunction =
                FindFunctionInHierarchyByName(
                    jason->Class,
                    "K2_TeleportTo");
        }

        if (func == g_MinimumMorphTeleportFunction)
        {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG morphReadyAt = JasonAIMorphReadyAt();
            if (morphReadyAt != 0 && now < morphReadyAt)
            {
                // The frozen counselor-ring stuck fallback bypasses the
                // shared Morph timer entirely.  Refuse every ordinary Jason
                // teleport until the same twenty-second cooldown used by
                // startup, distance, and trap-response Morphs has elapsed.
                reinterpret_cast<uint8_t*>(params)[24] = 0;
                if (now >= g_NextMorphCooldownRefusalLogAt)
                {
                    g_NextMorphCooldownRefusalLogAt = now + 2000;
                    Logger::Success(
                        "18L-AI counselor bridge: ordinary/stuck Morph refused by shared cooldown | remainingMs=" +
                        std::to_string(morphReadyAt - now));
                }
                return true;
            }

            // Scripted phone/car setup and owned-trap response are exempt
            // only from the proximity rule.  They still share the same
            // cooldown above.
            const bool ordinaryDistanceGate =
                !g_JasonAIState.StartupTrapSetupActive &&
                g_TrapTeleportExemptionDepth == 0;
            if (!ordinaryDistanceGate)
                return SafeProcessEventCall(vptr, obj, func, params);

            const bool stuckRecovery =
                g_JasonAIState.ConsecutiveStuckChecks >= 2;
            AActor* counselor =
                FindNearestJasonAICounselorTarget(jason);
            FVector jasonLocation{};
            FVector counselorLocation{};
            if (counselor &&
                GetJasonAIActorLocation(jason, jasonLocation) &&
                GetJasonAIActorLocation(counselor, counselorLocation))
            {
                const float dx = counselorLocation.X - jasonLocation.X;
                const float dy = counselorLocation.Y - jasonLocation.Y;
                const float dz = counselorLocation.Z - jasonLocation.Z;
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                // The general opening/hunt guard remains the requested 10m.
                // A stuck-recovery counselor-ring jump is more disruptive:
                // walking, door retry, and ordinary target rotation should
                // solve local obstruction, so require 30m for that path.
                const float minimumMorphDistanceCm =
                    stuckRecovery ? 3000.0f : 1000.0f;
                if (std::isfinite(distanceSquared) &&
                    distanceSquared <
                        minimumMorphDistanceCm * minimumMorphDistanceCm)
                {
                    // K2_TeleportTo's bool return value is the final byte in
                    // the frozen helper's verified 28-byte parameter block.
                    reinterpret_cast<uint8_t*>(params)[24] = 0;
                    if (now >= g_NextMinimumMorphLogAt)
                    {
                        g_NextMinimumMorphLogAt = now + 2000;
                        Logger::Success(
                            std::string(
                                "18L-AI counselor bridge: ") +
                            (stuckRecovery
                                ? "stuck-recovery Morph refused inside 30m local-action radius"
                                : "ordinary Morph refused inside 10m walk radius") +
                            " | distanceCm=" +
                            std::to_string(std::sqrt(distanceSquared)));
                    }
                    return true;
                }

                forwardedStuckRecoveryTeleport = stuckRecovery;
            }
        }
    }

    const bool callOK = SafeProcessEventCall(vptr, obj, func, params);
    if (forwardedStuckRecoveryTeleport &&
        callOK &&
        reinterpret_cast<uint8_t*>(params)[24] != 0)
    {
        // Frozen stuck recovery did not participate in the shared Morph
        // timer. Count its successful counselor-ring jump so it cannot chain
        // another Morph before the normal twenty-second recharge.
        MarkJasonAIMorphTeleportUsed("StuckRecovery");
    }
    return callOK;
}

namespace
{
    UObject* ReadReflectedObjectProperty(
        UObject* owner,
        const char* propertyName);
    bool IsValidatedLiveCounselorPawn(AActor* actor);
    bool SetLocalSpectatorViewTarget(UObject* target);

    UObject* g_JasonSpectatorPreviousTarget = nullptr;
    UObject* g_JasonSpectatorPreviousPlayerState = nullptr;
    bool g_JasonSpectatorHotkeyHeld = false;
    bool g_JasonSpectatorForced = false;
    bool g_InsertJasonOnNextSpectatorCycle = true;
    ULONGLONG g_LastSpectatorCycleEventAt = 0;
    UObject* g_LastObservedSpectatorPlayerState = nullptr;
    AActor* g_CachedJasonSpectatorPawn = nullptr;
    UObject* g_CachedJasonSpectatorPlayerState = nullptr;
    constexpr int32_t MaxSpectatorCycleCounselors = 8;
    UObject* g_SpectatorCycleSeenCounselors[MaxSpectatorCycleCounselors]{};
    int32_t g_SpectatorCycleSeenCount = 0;
    ULONGLONG g_NextSpectatorCyclePollAt = 0;
    ULONGLONG g_NextJasonSpectatorRepairAt = 0;
    ULONGLONG g_NextJasonSpectatorPlayerStateRepairAt = 0;
    bool g_JasonSpectatorOrbitInitialized = false;
    FRotator g_JasonSpectatorOrbitRotation{};
    FVector g_JasonSpectatorSmoothedFocus{};
    ULONGLONG g_JasonDeathVisualNormalizeUntil = 0;
    ULONGLONG g_NextJasonDeathVisualNormalizeAt = 0;

    AActor* ReadLivePossessedCounselor(APlayerController* controller)
    {
        if (!controller ||
            !Memory::IsReadable(controller, sizeof(UObject)))
        {
            return nullptr;
        }

        const char* pawnProperties[] = { "Pawn", "AcknowledgedPawn" };
        for (const char* propertyName : pawnProperties)
        {
            AActor* pawn = reinterpret_cast<AActor*>(
                ReadReflectedObjectProperty(
                    reinterpret_cast<UObject*>(controller),
                    propertyName));
            if (pawn && pawn != g_JasonAIState.Jason &&
                IsValidatedLiveCounselorPawn(pawn))
            {
                return pawn;
            }
        }
        return nullptr;
    }

    bool HasLocalPossessedPawn(APlayerController* controller)
    {
        if (!controller ||
            !Memory::IsReadable(controller, sizeof(UObject)))
        {
            return false;
        }
        __try
        {
            return *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(controller) + 0x370) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    UObject* ReadJasonSpectatorPlayerState()
    {
        AActor* jason = g_JasonAIState.Jason;
        if (jason == g_CachedJasonSpectatorPawn &&
            g_CachedJasonSpectatorPlayerState &&
            Memory::IsReadable(
                g_CachedJasonSpectatorPlayerState,
                sizeof(UObject)))
        {
            return g_CachedJasonSpectatorPlayerState;
        }

        g_CachedJasonSpectatorPawn = jason;
        g_CachedJasonSpectatorPlayerState = nullptr;
        UObject* jasonPlayerState = nullptr;
        // The verified native AController::PlayerState field is the cheap
        // path. Reflection is needed only once if a future packed build moves
        // that field.
        if (jason &&
            Memory::IsReadable(g_JasonAIState.Jason, 0x390))
        {
            UObject** directPlayerState = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(jason) + 0x388);
            if (Memory::IsReadable(directPlayerState, sizeof(UObject*)) &&
                *directPlayerState &&
                Memory::IsReadable(*directPlayerState, sizeof(UObject)))
            {
                jasonPlayerState = *directPlayerState;
            }
        }
        if (!jasonPlayerState && jason)
        {
            jasonPlayerState = ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(jason),
                "PlayerState");
        }
        g_CachedJasonSpectatorPlayerState = jasonPlayerState;
        return jasonPlayerState;
    }

    bool ReadLocalSpectatorLookRotation(FRotator& outRotation)
    {
        APlayerController* controller =
            Engine::GetLocalPlayerController();
        if (!controller || !controller->Class ||
            !Memory::IsReadable(controller, sizeof(UObject)))
        {
            return false;
        }

        static UClass* cachedControllerClass = nullptr;
        static UFunction* getControlRotation = nullptr;
        if (cachedControllerClass != controller->Class)
        {
            cachedControllerClass = controller->Class;
            getControlRotation = FindFunctionInHierarchyByName(
                controller->Class,
                "GetControlRotation");
        }
        if (!getControlRotation)
            return false;

        struct Params { FRotator ReturnValue; } params{};
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(controller),
                controller,
                getControlRotation,
                &params) ||
            !std::isfinite(params.ReturnValue.Pitch) ||
            !std::isfinite(params.ReturnValue.Yaw) ||
            !std::isfinite(params.ReturnValue.Roll))
        {
            return false;
        }
        outRotation = params.ReturnValue;
        return true;
    }

    void SeedLocalSpectatorLookRotation(const FRotator& rotation)
    {
        APlayerController* controller =
            Engine::GetLocalPlayerController();
        if (!controller || !controller->Class ||
            !Memory::IsReadable(controller, sizeof(UObject)))
        {
            return;
        }

        UFunction* setControlRotation = FindFunctionInHierarchyByName(
            controller->Class,
            "SetControlRotation");
        if (!setControlRotation)
            return;
        struct Params { FRotator NewRotation; } params{ rotation };
        SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(controller),
            controller,
            setControlRotation,
            &params);
    }

    bool BuildJasonSpectatorCameraPose(
        FVector& outLocation,
        FRotator& outRotation)
    {
        AActor* jason = g_JasonAIState.Jason;
        if (!jason)
            return false;

        // BlueprintUpdateCamera is called every rendered frame. VirtualQuery
        // validation here reduced the live spectator view to 14 FPS. The
        // active Jason and root component are already lifecycle-owned by this
        // bridge, so use one guarded read of the component transform instead.
        struct RawQuaternion
        {
            float X;
            float Y;
            float Z;
            float W;
        };
        RawQuaternion rotation{};
        __try
        {
            UObject* root = *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(jason) +
                Offsets::Actor_RootComponent);
            if (!root)
                return false;
            const uintptr_t transform =
                reinterpret_cast<uintptr_t>(root) +
                Offsets::Scene_ComponentToWorld;
            rotation = *reinterpret_cast<RawQuaternion*>(transform);
            outLocation = *reinterpret_cast<FVector*>(
                transform + Offsets::FTransform_Translation);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        const float yaw = std::atan2(
            2.0f * (rotation.W * rotation.Z + rotation.X * rotation.Y),
            1.0f - 2.0f *
                (rotation.Y * rotation.Y + rotation.Z * rotation.Z)) *
            57.29577951308232f;
        if (!std::isfinite(yaw) ||
            !std::isfinite(outLocation.X) ||
            !std::isfinite(outLocation.Y) ||
            !std::isfinite(outLocation.Z) ||
            std::fabs(outLocation.X) > 1000000.0f ||
            std::fabs(outLocation.Y) > 1000000.0f ||
            std::fabs(outLocation.Z) > 1000000.0f)
        {
            // A spectator camera RPC moves the stock spectator pawn, so an
            // invalid/stale Jason transform is not merely visual: it can push
            // that pawn far outside the navigation quadtree and make Unreal
            // allocate until it exhausts memory. This is most likely during
            // the frame where the final counselor dies and post-match teardown
            // begins. Never publish an implausible world-space camera pose.
            return false;
        }

        FVector focus = outLocation;
        focus.Z += 92.0f;
        if (!g_JasonSpectatorOrbitInitialized)
        {
            g_JasonSpectatorOrbitRotation.Pitch = -13.72f;
            g_JasonSpectatorOrbitRotation.Yaw = yaw;
            g_JasonSpectatorOrbitRotation.Roll = 0.0f;
            g_JasonSpectatorSmoothedFocus = focus;
            g_JasonSpectatorOrbitInitialized = true;
            SeedLocalSpectatorLookRotation(
                g_JasonSpectatorOrbitRotation);
        }
        else
        {
            FRotator liveLook{};
            if (ReadLocalSpectatorLookRotation(liveLook))
                g_JasonSpectatorOrbitRotation = liveLook;

            const float dx = focus.X - g_JasonSpectatorSmoothedFocus.X;
            const float dy = focus.Y - g_JasonSpectatorSmoothedFocus.Y;
            const float dz = focus.Z - g_JasonSpectatorSmoothedFocus.Z;
            const float movementSquared = dx * dx + dy * dy + dz * dz;
            if (!std::isfinite(movementSquared) ||
                movementSquared > 1440000.0f)
            {
                // Morph/teleport: snap the orbit center to Jason instead of
                // sweeping the camera across the map.
                g_JasonSpectatorSmoothedFocus = focus;
            }
            else
            {
                // Smooth ordinary locomotion and animation-root jitter while
                // leaving mouse/right-stick look response immediate.
                // Keep the same approximate tracking response at the 60 Hz
                // camera cadence while reducing per-frame root-motion shake.
                constexpr float followAlpha = 0.14f;
                g_JasonSpectatorSmoothedFocus.X += dx * followAlpha;
                g_JasonSpectatorSmoothedFocus.Y += dy * followAlpha;
                g_JasonSpectatorSmoothedFocus.Z += dz * followAlpha;
            }
        }

        while (g_JasonSpectatorOrbitRotation.Pitch > 180.0f)
            g_JasonSpectatorOrbitRotation.Pitch -= 360.0f;
        while (g_JasonSpectatorOrbitRotation.Pitch < -180.0f)
            g_JasonSpectatorOrbitRotation.Pitch += 360.0f;
        // Orbit only above Jason. Positive pitch put the camera beneath the
        // landscape when the player pulled down on the right stick.
        if (g_JasonSpectatorOrbitRotation.Pitch < -55.0f)
            g_JasonSpectatorOrbitRotation.Pitch = -55.0f;
        else if (g_JasonSpectatorOrbitRotation.Pitch > -2.0f)
            g_JasonSpectatorOrbitRotation.Pitch = -2.0f;
        g_JasonSpectatorOrbitRotation.Roll = 0.0f;

        const float yawRadians =
            g_JasonSpectatorOrbitRotation.Yaw * 0.01745329251994329577f;
        const float pitchRadians =
            g_JasonSpectatorOrbitRotation.Pitch * 0.01745329251994329577f;
        constexpr float chaseDistance = 340.0f;
        const float horizontal = std::cos(pitchRadians) * chaseDistance;
        outLocation.X = g_JasonSpectatorSmoothedFocus.X -
            std::cos(yawRadians) * horizontal;
        outLocation.Y = g_JasonSpectatorSmoothedFocus.Y -
            std::sin(yawRadians) * horizontal;
        outLocation.Z = g_JasonSpectatorSmoothedFocus.Z -
            std::sin(pitchRadians) * chaseDistance;
        outRotation = g_JasonSpectatorOrbitRotation;
        return std::isfinite(outLocation.X) &&
            std::isfinite(outLocation.Y) &&
            std::isfinite(outLocation.Z) &&
            std::isfinite(outRotation.Pitch);
    }

    void ArmJasonDeathVisualNormalization(ULONGLONG now)
    {
        // A terrified local counselor can carry the extreme fear grading into
        // the native Jason-death camera. Suppress only that visual contribution
        // during the bounded cinematic; gameplay fear is untouched otherwise.
        g_JasonDeathVisualNormalizeUntil = now + 20000;
        g_NextJasonDeathVisualNormalizeAt = now;
    }

    void MaintainJasonDeathVisualNormalization(ULONGLONG now)
    {
        if (now >= g_JasonDeathVisualNormalizeUntil ||
            now < g_NextJasonDeathVisualNormalizeAt)
        {
            return;
        }
        g_NextJasonDeathVisualNormalizeAt = now + 250;
        USCFearComponent* fear = Engine::GetFearComponent();
        if (fear && Memory::IsReadable(fear, sizeof(USCFearComponent)))
            fear->FearAmount.Value = 0.0f;
    }

    bool WriteLocalSpectatingPlayer(UObject* playerState)
    {
        APlayerController* controller =
            Engine::GetLocalPlayerController();
        if (!controller || !controller->Class ||
            !Memory::IsReadable(controller, sizeof(UObject)))
        {
            return false;
        }

        static UClass* cachedControllerClass = nullptr;
        static int32_t spectatingPlayerOffset = -1;
        if (cachedControllerClass != controller->Class)
        {
            cachedControllerClass = controller->Class;
            spectatingPlayerOffset = -1;
            UPropertyLite* property = FindPropertyInHierarchyByName(
                controller->Class,
                "SpectatingPlayer");
            if (property && property->Offset_Internal > 0 &&
                property->Offset_Internal < 0x10000)
            {
                spectatingPlayerOffset = property->Offset_Internal;
            }
        }
        if (spectatingPlayerOffset < 0)
            return false;

        UObject** field = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(controller) +
            static_cast<uintptr_t>(spectatingPlayerOffset));
        if (!Memory::IsReadable(field, sizeof(UObject*)))
            return false;
        __try
        {
            *field = playerState;
            return *field == playerState;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    UObject* ReadLocalSpectatingPlayer()
    {
        APlayerController* controller =
            Engine::GetLocalPlayerController();
        if (!controller || !controller->Class ||
            !Memory::IsReadable(controller, sizeof(UObject)))
        {
            return nullptr;
        }

        static UClass* cachedControllerClass = nullptr;
        static int32_t spectatingPlayerOffset = -1;
        if (cachedControllerClass != controller->Class)
        {
            cachedControllerClass = controller->Class;
            spectatingPlayerOffset = -1;
            UPropertyLite* property = FindPropertyInHierarchyByName(
                controller->Class,
                "SpectatingPlayer");
            if (property && property->Offset_Internal > 0 &&
                property->Offset_Internal < 0x10000)
            {
                spectatingPlayerOffset = property->Offset_Internal;
            }
        }
        if (spectatingPlayerOffset < 0)
            return nullptr;

        UObject** field = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(controller) +
            static_cast<uintptr_t>(spectatingPlayerOffset));
        if (!Memory::IsReadable(field, sizeof(UObject*)))
            return nullptr;

        UObject* playerState = nullptr;
        __try
        {
            playerState = *field;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
        return playerState &&
            Memory::IsReadable(playerState, sizeof(UObject))
                ? playerState
                : nullptr;
    }

    void ReleaseJasonSpectatorOverrideForCinematic(const char* reason)
    {
        if (!g_JasonSpectatorForced)
            return;

        WriteLocalSpectatingPlayer(g_JasonSpectatorPreviousPlayerState);
        if (g_JasonSpectatorPreviousTarget &&
            Memory::IsReadable(
                g_JasonSpectatorPreviousTarget,
                sizeof(UObject)))
        {
            SetLocalSpectatorViewTarget(g_JasonSpectatorPreviousTarget);
        }

        g_JasonSpectatorForced = false;
        g_JasonSpectatorPreviousTarget = nullptr;
        g_JasonSpectatorPreviousPlayerState = nullptr;
        g_NextJasonSpectatorRepairAt = 0;
        g_NextJasonSpectatorPlayerStateRepairAt = 0;
        g_JasonSpectatorOrbitInitialized = false;
        g_InsertJasonOnNextSpectatorCycle = true;
        Logger::Success(
            std::string("18L-BL Jason spectator released for native camera | reason=") +
            (reason ? reason : "unknown"));
    }

    bool ClearJasonSpectatorOverrideForLivePawn(
        APlayerController* controller)
    {
        AActor* livePawn = ReadLivePossessedCounselor(controller);
        if (!livePawn)
            return false;

        if (g_JasonSpectatorForced)
        {
            SetLocalSpectatorViewTarget(
                reinterpret_cast<UObject*>(livePawn));
            Logger::Success(
                "18L-BL Jason spectator override released for newly possessed live counselor");
        }
        g_JasonSpectatorForced = false;
        g_JasonSpectatorPreviousTarget = nullptr;
        g_JasonSpectatorPreviousPlayerState = nullptr;
        g_NextJasonSpectatorRepairAt = 0;
        g_NextJasonSpectatorPlayerStateRepairAt = 0;
        g_JasonSpectatorOrbitInitialized = false;
        g_InsertJasonOnNextSpectatorCycle = true;
        return true;
    }

    UObject* ReadLocalSpectatorViewTarget()
    {
        APlayerController* controller =
            Engine::GetLocalPlayerController();
        if (!controller ||
            !Memory::IsReadable(controller, sizeof(UObject)) ||
            !controller->Class)
        {
            return nullptr;
        }

        UFunction* getViewTarget = FindFunctionInHierarchyByName(
            controller->Class,
            "GetViewTarget");
        if (!getViewTarget)
            return nullptr;

        struct Params
        {
            UObject* ReturnValue;
        } params{};
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(controller),
                controller,
                getViewTarget,
                &params))
        {
            return nullptr;
        }
        return params.ReturnValue &&
            Memory::IsReadable(params.ReturnValue, sizeof(UObject))
                ? params.ReturnValue
                : nullptr;
    }

    bool ApplyJasonSpectatorCameraPose()
    {
        APlayerController* controller =
            Engine::GetLocalPlayerController();
        AActor* jason = g_JasonAIState.Jason;
        if (!controller || !controller->Class || !jason || !jason->Class ||
            !Memory::IsReadable(controller, sizeof(UObject)) ||
            !Memory::IsReadable(jason, sizeof(UObject)))
        {
            return false;
        }

        FVector location{};
        FRotator rotation{};
        if (!BuildJasonSpectatorCameraPose(location, rotation))
            return false;

        static UClass* cachedControllerClass = nullptr;
        static UFunction* setSpectatorCamera = nullptr;
        if (cachedControllerClass != controller->Class)
        {
            cachedControllerClass = controller->Class;
            setSpectatorCamera = FindFunctionInHierarchyByName(
                controller->Class,
                "ClientSetSpectatorCamera");
        }
        if (!setSpectatorCamera)
            return false;

        struct SpectatorCameraParams
        {
            FVector CameraLocation;
            FRotator CameraRotation;
        } params{};
        params.CameraLocation = location;
        params.CameraRotation = rotation;
        return SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(controller),
            controller,
            setSpectatorCamera,
            &params);
    }

    bool SetLocalSpectatorViewTarget(UObject* target)
    {
        APlayerController* controller =
            Engine::GetLocalPlayerController();
        if (!controller || !target ||
            !Memory::IsReadable(controller, sizeof(UObject)) ||
            !controller->Class ||
            !Memory::IsReadable(target, sizeof(UObject)))
        {
            return false;
        }

        UFunction* setViewTarget = FindFunctionInHierarchyByName(
            controller->Class,
            "SetViewTargetWithBlend");
        UFunction* clientSetViewTarget = FindFunctionInHierarchyByName(
            controller->Class,
            "ClientSetViewTarget");
        if (!setViewTarget && !clientSetViewTarget)
            return false;

        struct Params
        {
            UObject* NewViewTarget;
            float BlendTime;
            uint8_t BlendFunction;
            uint8_t Padding[3];
            float BlendExp;
            bool LockOutgoing;
            uint8_t TailPadding[3];
        } params{};
        params.NewViewTarget = target;
        params.BlendTime = 0.0f;
        params.BlendFunction = 0;
        params.BlendExp = 0.0f;
        params.LockOutgoing = false;
        bool applied = false;
        if (clientSetViewTarget)
        {
            applied = SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(controller),
                controller,
                clientSetViewTarget,
                &params) || applied;
        }
        if (setViewTarget)
        {
            applied = SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(controller),
                controller,
                setViewTarget,
                &params) || applied;
        }
        return applied;
    }

    bool ShowJasonSpectatorView(bool rememberCurrent)
    {
        UObject* jason = reinterpret_cast<UObject*>(
            g_JasonAIState.Jason);
        if (!g_JasonAIState.Active || !jason ||
            !Memory::IsReadable(jason, sizeof(UObject)))
        {
            return false;
        }

        UObject* current = ReadLocalSpectatorViewTarget();
        if (current == jason && g_JasonSpectatorForced)
            return true;
        APlayerController* controller =
            Engine::GetLocalPlayerController();
        if (rememberCurrent && current)
            g_JasonSpectatorPreviousTarget = current;
        if (rememberCurrent && controller)
        {
            g_JasonSpectatorPreviousPlayerState =
                ReadReflectedObjectProperty(
                    reinterpret_cast<UObject*>(controller),
                    "SpectatingPlayer");
        }

        UObject* jasonPlayerState = ReadJasonSpectatorPlayerState();
        const bool nativeTargeted = jasonPlayerState &&
            WriteLocalSpectatingPlayer(jasonPlayerState);

        // Keep the view target on the stock SCSpectatorPawn. Targeting an AI
        // Jason actor directly falls back to the pawn capsule origin, which is
        // at or below the landscape. ClientSetSpectatorCamera positions the
        // stock pawn at a proper third-person chase pose instead.
        g_JasonSpectatorForced = true;
        g_JasonSpectatorOrbitInitialized = false;
        const bool viewed = ApplyJasonSpectatorCameraPose();
        if (!viewed)
        {
            g_JasonSpectatorForced = false;
            return false;
        }

        g_NextJasonSpectatorRepairAt = GetTickCount64() + 33;
        g_NextJasonSpectatorPlayerStateRepairAt =
            GetTickCount64() + 2000;
        Logger::Success(
            std::string(
                "18L-BL Jason spectator POV inserted into counselor cycle") +
            (nativeTargeted ? " | playerState=true" : " | playerState=false"));
        return true;
    }

    void PollJasonSpectatorHotkeyOnGameThread()
    {
        const bool keyDown =
            (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (!keyDown)
        {
            g_JasonSpectatorHotkeyHeld = false;
            return;
        }
        if (g_JasonSpectatorHotkeyHeld)
            return;
        g_JasonSpectatorHotkeyHeld = true;

        APlayerController* controller =
            Engine::GetLocalPlayerController();
        if (g_JasonSpectatorForced &&
            ClearJasonSpectatorOverrideForLivePawn(controller))
        {
            return;
        }

        UObject* jason = reinterpret_cast<UObject*>(
            g_JasonAIState.Jason);
        UObject* current = ReadLocalSpectatorViewTarget();
        if (g_JasonSpectatorForced || current == jason)
        {
            UObject* restore = g_JasonSpectatorPreviousTarget;
            if (!restore ||
                !Memory::IsReadable(restore, sizeof(UObject)) ||
                restore == jason)
            {
                restore = nullptr;
                for (int32_t i = 0; i < g_JasonAITargetCount; ++i)
                {
                    AActor* candidate = g_JasonAITargets[i];
                    if (candidate && candidate != g_JasonAIState.Jason &&
                        Memory::IsReadable(candidate, sizeof(UObject)))
                    {
                        restore = reinterpret_cast<UObject*>(candidate);
                        break;
                    }
                }
            }
            if (restore && SetLocalSpectatorViewTarget(restore))
            {
                WriteLocalSpectatingPlayer(
                    g_JasonSpectatorPreviousPlayerState);
                g_JasonSpectatorForced = false;
                g_NextJasonSpectatorRepairAt = 0;
                g_NextJasonSpectatorPlayerStateRepairAt = 0;
                g_JasonSpectatorOrbitInitialized = false;
                Logger::Success(
                    "18L-BL F4 spectator POV returned to counselor");
            }
        }
        else if (ShowJasonSpectatorView(true))
        {
            Logger::Success(
                "18L-BL F4 direct Jason spectator fallback accepted");
        }
    }

    void ResetObservedSpectatorCounselorCycle()
    {
        std::memset(
            g_SpectatorCycleSeenCounselors,
            0,
            sizeof(g_SpectatorCycleSeenCounselors));
        g_SpectatorCycleSeenCount = 0;
    }

    bool MarkObservedSpectatorCounselor(UObject* playerState)
    {
        if (!playerState)
            return false;
        for (int32_t i = 0; i < g_SpectatorCycleSeenCount; ++i)
        {
            if (g_SpectatorCycleSeenCounselors[i] == playerState)
                return true;
        }
        if (g_SpectatorCycleSeenCount < MaxSpectatorCycleCounselors)
        {
            g_SpectatorCycleSeenCounselors[g_SpectatorCycleSeenCount++] =
                playerState;
        }
        return false;
    }

    void ObserveStockSpectatorCycleOnGameThread(ULONGLONG now)
    {
        if (!g_JasonAIState.Active || !g_JasonAIState.Jason ||
            now < g_NextSpectatorCyclePollAt)
        {
            return;
        }
        // The shipping controller can execute ServerViewNext/PrevPlayer
        // natively, bypassing the ProcessEvent hook. Observe only the cached
        // SpectatingPlayer pointer at 20 Hz; this adds no actor scan and lets
        // the stock Previous/Next inputs remain the sole source of cycling.
        g_NextSpectatorCyclePollAt = now + 50;

        APlayerController* controller =
            Engine::GetLocalPlayerController();
        if (!controller || !Memory::IsReadable(controller, sizeof(UObject)))
            return;

        if (HasLocalPossessedPawn(controller))
        {
            g_LastObservedSpectatorPlayerState = nullptr;
            ResetObservedSpectatorCounselorCycle();
            return;
        }

        UObject* currentPlayerState = ReadLocalSpectatingPlayer();
        UObject* jasonPlayerState = ReadJasonSpectatorPlayerState();
        if (!currentPlayerState || !jasonPlayerState)
            return;

        if (g_JasonSpectatorForced)
        {
            if (currentPlayerState != jasonPlayerState)
            {
                // Stock Previous/Next has already selected and framed a
                // counselor. Stop repairing Jason's camera immediately and
                // leave that native counselor view untouched.
                g_JasonSpectatorForced = false;
                g_JasonSpectatorPreviousTarget = nullptr;
                g_JasonSpectatorPreviousPlayerState = nullptr;
                g_NextJasonSpectatorRepairAt = 0;
                g_NextJasonSpectatorPlayerStateRepairAt = 0;
                g_JasonSpectatorOrbitInitialized = false;
                g_InsertJasonOnNextSpectatorCycle = true;
                ResetObservedSpectatorCounselorCycle();
                MarkObservedSpectatorCounselor(currentPlayerState);
                Logger::Success(
                    "18L-BL stock spectator cycle advanced from Jason to counselor");
            }
            g_LastObservedSpectatorPlayerState = currentPlayerState;
            return;
        }

        if (currentPlayerState == jasonPlayerState)
        {
            if (ShowJasonSpectatorView(false))
            {
                g_InsertJasonOnNextSpectatorCycle = false;
                ResetObservedSpectatorCounselorCycle();
                Logger::Success(
                    "18L-BL stock spectator cycle selected Jason natively");
            }
            g_LastObservedSpectatorPlayerState = jasonPlayerState;
            return;
        }

        if (!g_LastObservedSpectatorPlayerState)
        {
            // Establish the counselor selected when death spectating begins;
            // do not steal the initial stock view before the player cycles.
            g_LastObservedSpectatorPlayerState = currentPlayerState;
            ResetObservedSpectatorCounselorCycle();
            MarkObservedSpectatorCounselor(currentPlayerState);
            return;
        }

        if (currentPlayerState != g_LastObservedSpectatorPlayerState)
        {
            g_LastObservedSpectatorPlayerState = currentPlayerState;
            // A repeated counselor marks the stock cycle wrapping around.
            // Insert Jason at that boundary so every living counselor appears
            // exactly once before Jason, rather than alternating Jason after
            // every counselor.
            const bool wrapped =
                MarkObservedSpectatorCounselor(currentPlayerState);
            if (wrapped && g_InsertJasonOnNextSpectatorCycle &&
                ShowJasonSpectatorView(true))
            {
                g_InsertJasonOnNextSpectatorCycle = false;
                g_LastObservedSpectatorPlayerState = jasonPlayerState;
                ResetObservedSpectatorCounselorCycle();
                Logger::Success(
                    "18L-BL Jason inserted at completed counselor spectator-cycle boundary");
            }
        }
    }

    void MaintainJasonSpectatorViewOnGameThread(ULONGLONG now)
    {
        if (!g_JasonSpectatorForced ||
            now < g_NextJasonSpectatorRepairAt)
        {
            return;
        }
        // Follow Jason at 60 Hz through the stock spectator-camera RPC. This
        // avoids touching BlueprintUpdateCamera's unstable out-parameter
        // memory and removes the visible 30 Hz judder on high-refresh displays.
        // This lane is active only while the dead player is viewing Jason.
        g_NextJasonSpectatorRepairAt = now + 16;

        APlayerController* controller =
            Engine::GetLocalPlayerController();
        if (!controller)
            return;

        // Pawn is a stable native AController field in this build. During
        // death spectating it is null; after Tommy/live possession it becomes
        // non-null. Keep this hot path free of reflected property scans.
        bool hasPossessedPawn = false;
        __try
        {
            hasPossessedPawn =
                *reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(controller) + 0x370) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        if (hasPossessedPawn)
        {
            ClearJasonSpectatorOverrideForLivePawn(controller);
            return;
        }

        UObject* jason = reinterpret_cast<UObject*>(
            g_JasonAIState.Jason);
        if (!jason)
        {
            g_JasonSpectatorForced = false;
            return;
        }

        if (!ApplyJasonSpectatorCameraPose())
            return;

        if (now < g_NextJasonSpectatorPlayerStateRepairAt)
            return;
        g_NextJasonSpectatorPlayerStateRepairAt = now + 2000;

        UObject* jasonPlayerState = ReadJasonSpectatorPlayerState();
        if (jasonPlayerState)
        {
            APlayerController* controller =
                Engine::GetLocalPlayerController();
            UObject* currentPlayerState = controller
                ? ReadReflectedObjectProperty(
                    reinterpret_cast<UObject*>(controller),
                    "SpectatingPlayer")
                : nullptr;
            if (currentPlayerState != jasonPlayerState)
                WriteLocalSpectatingPlayer(jasonPlayerState);
        }
    }

    using UpdateSoundBlipsFn =
        void(__fastcall*)(UObject* killer, float deltaSeconds);

    using SetSoundBlipVisibilityFn =
        void(__fastcall*)(UObject* killer, bool visible);

    using TrapTriggeredFn =
        void(__fastcall*)(AActor* trap, AActor* victim);

    using GiveStartingItemFn =
        void(__fastcall*)(AActor* pawn, UClass* requestedClass);

    using AttemptInteractFn =
        void(__fastcall*)(UObject* manager,
            UObject* interactionComponent,
            uint8_t interactionMethod,
            bool autoLock);

    using ContextKillCanInteractFn =
        int32_t(__fastcall*)(UObject* contextKillComponent,
            AActor* interactor,
            const FVector* viewLocation,
            const FVector* viewDirection);

    using PamelaSweaterCanInteractFn =
        int32_t(__fastcall*)(AActor* sweater,
            AActor* interactor,
            const FVector* viewLocation,
            const FVector* viewDirection);

    using NativeInteractionUpdateFn = void(__fastcall*)(UObject* interaction);

    static UpdateSoundBlipsFn g_OriginalUpdateSoundBlips = nullptr;
    static TrapTriggeredFn g_OriginalTrapTriggered = nullptr;
    static GiveStartingItemFn g_OriginalGiveStartingItem = nullptr;
    static ContextKillCanInteractFn g_OriginalContextKillCanInteract = nullptr;
    static PamelaSweaterCanInteractFn g_OriginalPamelaSweaterCanInteract = nullptr;
    static PamelaSweaterCanInteractFn g_BasePamelaPickupCanInteract = nullptr;
    static NativeInteractionUpdateFn g_OriginalNativeInteractionUpdate = nullptr;
    static LPVOID g_UpdateSoundBlipsTarget = nullptr;
    static LPVOID g_TrapTriggeredTarget = nullptr;
    static LPVOID g_GiveStartingItemTarget = nullptr;
    static LPVOID g_ContextKillCanInteractTarget = nullptr;
    static LPVOID g_PamelaSweaterCanInteractTarget = nullptr;
    static LPVOID g_NativeInteractionUpdateTarget = nullptr;
    static bool g_UpdateSoundBlipsHookInstalled = false;
    static bool g_TrapTriggeredHookInstalled = false;
    static bool g_GiveStartingItemHookInstalled = false;
    static bool g_ContextKillCanInteractHookInstalled = false;
    static bool g_PamelaSweaterCanInteractHookInstalled = false;
    static bool g_NativeInteractionUpdateHookInstalled = false;
    static bool g_NativeInteractionNullGuardLogged = false;
    static PVOID g_NativeInteractionExceptionGuard = nullptr;
    static uintptr_t g_NativeInteractionGameBase = 0;
    static volatile LONG g_NativeInteractionExceptionGuardHits = 0;
    static AActor* g_LastUniversalFinalEligibilityFinisher = nullptr;
    static ULONGLONG g_LastUniversalFinalEligibilityAcceptedAt = 0;
    static bool g_HunterAxeLoadoutRouteEnabled = false;
    static UClass* g_HunterSpawnAxeClass = nullptr;
    static AActor* g_HunterSpawnAxeItem = nullptr;
    static bool g_CounselorRouteTickWrapperInstalled = false;
    static AActor* g_LastExtendedDoorTarget = nullptr;
    static ULONGLONG g_LastNonAggroReadyAt = 0;
    static ULONGLONG g_NextTrapPriorityAttemptAt = 0;
    static ULONGLONG g_NextTrapPriorityLogAt = 0;
    static ULONGLONG g_TrapPriorityUntil = 0;
    static ULONGLONG g_TrapPriorityBaselineMorphAt = 0;
    static ULONGLONG g_NextKnifePickupScanAt = 0;
    static ULONGLONG g_NextKnifeRegistryRefreshAt = 0;
    static int32_t g_KnifeRegistryRefreshLevel = 0;
    static int32_t g_KnifeRegistryRefreshActor = 0;
    static AActor* g_KnifePickupRegistry[128]{};
    static int32_t g_KnifePickupRegistryCount = 0;
    static AActor* g_HidingSpotRegistry[256]{};
    static int32_t g_HidingSpotRegistryCount = 0;
    static bool g_WorldInteractionRegistryComplete = false;
    static ULONGLONG g_KnifePickupStartedAt = 0;
    static ULONGLONG g_CombatBusyUntil = 0;
    static std::atomic<ULONGLONG> g_QueuedTrapMorphBaseline{ 0 };
    static std::atomic<AActor*> g_QueuedTrapVictim{ nullptr };
    static std::atomic<AActor*> g_QueuedTriggeredTrap{ nullptr };
    static AActor* g_TrapPriorityVictim = nullptr;
    static bool g_TrapPriorityMorphCompleted = false;
    static int32_t g_LastShortenedPlacementObjective = -1;
    static AActor* g_PendingKnifePickup = nullptr;
    static UObject* g_PendingKnifeComponent = nullptr;
    static int32_t g_KnifeCountBeforePickup = -1;
    static AActor* g_LastHeldCounselor = nullptr;
    static ULONGLONG g_HeldCounselorObservedAt = 0;
    static ULONGLONG g_GrabKillInputCommittedAt = 0;
    // A released grab briefly leaves the counselor pawn/controller and its
    // inventory in a native transition.  Do not issue item, blackboard, path,
    // or attack work against that pawn until the paired animation has settled.
    static AActor* g_PostGrabTransitionCounselor = nullptr;
    static ULONGLONG g_PostGrabTransitionUntil = 0;
    static AActor* g_GrabKillSlotsLoggedFor = nullptr;
    static ULONGLONG g_NextAdapterGrabKillAttemptAt = 0;
    static ULONGLONG g_NextGrabKillStateLogAt = 0;
    static int32_t g_NextAdapterGrabKillSlot = 0;
    static bool g_StaleGrabReleaseAttempted = false;
    static AActor* g_LocalCounselorTarget = nullptr;
    static UObject* g_LocalPlayerController = nullptr;
    static ULONGLONG g_NextLocalCounselorRefreshAt = 0;
    static int32_t g_CounselorEscapedPropertyOffset = -2;
    static uint8_t g_CounselorEscapedByteOffset = 0;
    static uint8_t g_CounselorEscapedFieldMask = 0;
    static int32_t g_PlayerStateEscapedPropertyOffset = -2;
    static uint8_t g_PlayerStateEscapedByteOffset = 0;
    static uint8_t g_PlayerStateEscapedFieldMask = 0;
    static ULONGLONG g_HumanPursuitStartedAt = 0;
    static AActor* g_VehicleInterceptCar = nullptr;
    static UObject* g_VehicleInterceptSeat = nullptr;
    static FVector g_VehicleInterceptHeading{};
    static ULONGLONG g_VehicleHeadingStableSince = 0;
    static ULONGLONG g_VehicleInterceptDeadline = 0;
    static ULONGLONG g_VehicleInterceptRetryAfter = 0;
    static AActor* g_IgnoredVehicleInterceptCar = nullptr;
    static ULONGLONG g_IgnoredVehicleInterceptUntil = 0;
    static ULONGLONG g_NextVehicleInterceptActionAt = 0;
    static ULONGLONG g_NextVehicleSeatRediscoveryAt = 0;
    static ULONGLONG g_NextVehicleForwardSpeedSampleAt = 0;
    static float g_CachedVehicleForwardSpeed = 0.0f;
    static ULONGLONG g_NextVehicleInterceptLogAt = 0;
    static ULONGLONG g_NextVehicleMoveLogAt = 0;
    static FVector g_VehicleLastCenterlineMovePoint{};
    static ULONGLONG g_VehicleLastCenterlineMoveAt = 0;
    static bool g_VehicleHaveLastCenterlineMovePoint = false;
    static AActor* g_VehicleMotionSampleCar = nullptr;
    static FVector g_VehicleMotionSampleLocation{};
    static FVector g_VehicleMotionSampleDirection{};
    static float g_VehicleMotionSampleSpeed = 0.0f;
    static ULONGLONG g_VehicleMotionSampleAt = 0;
    static bool g_VehicleHaveMotionSample = false;
    static bool g_VehicleInterceptMorphUsed = false;
    static ULONGLONG g_VehicleHoodInputSentAt = 0;
    static ULONGLONG g_VehicleMovingHoodAttemptAt = 0;
    static int32_t g_VehicleMovingHoodAttempts = 0;
    static bool g_VehicleSlamObserved = false;
    static bool g_VehicleExtractionCommitted = false;
    static ULONGLONG g_VehicleExtractionCommittedAt = 0;
    static ULONGLONG g_VehicleExtractionInputAt = 0;
    static int32_t g_VehicleExtractionAttempts = 0;
    static bool g_VehicleDriverDetourReached = false;
    static int32_t g_VehicleDriverDetourAttempts = 0;
    static ULONGLONG g_VehicleDriverApproachStartedAt = 0;
    static ULONGLONG g_VehicleDriverLastProgressAt = 0;
    static ULONGLONG g_VehicleDriverReadySince = 0;
    static float g_VehicleDriverBestDistance = FLT_MAX;
    static FVector g_VehicleCachedDriverDoorPoint{};
    static FVector g_VehicleCachedDriverDetourPoint{};
    static FVector g_VehicleLastDriverMovePoint{};
    static AActor* g_VehicleCachedDriverDoorCar = nullptr;
    static UObject* g_VehicleCachedDriverDoorSeat = nullptr;
    static bool g_VehicleHaveCachedDriverDoorPoint = false;
    static bool g_VehicleHaveCachedDriverDetourPoint = false;
    static bool g_VehicleHaveLastDriverMovePoint = false;
    static ULONGLONG g_VehicleLastDriverMoveAt = 0;
    static UObject* g_LocalAimReticleWeapon = nullptr;
    static bool g_LocalAimReticleWasAiming = false;
    static bool g_LocalAimReticleEndSent = false;
    static ULONGLONG g_NextLocalAimReticleCheckAt = 0;
    static UWorld* g_VehicleRoadPointWorld = nullptr;
    static AActor* g_VehicleRoadPoints[256]{};
    static int32_t g_VehicleRoadPointCount = 0;
    static int32_t g_VehicleRoadNeighborsOffset = -2;
    static bool g_JasonPursuitBoostActive = false;
    static void* g_JasonPursuitMovement = nullptr;
    static float g_JasonBaseWalkSpeed = 0.0f;
    static float g_JasonBaseSprintSpeed = 0.0f;
    static float g_JasonBaseRunSpeed = 0.0f;
    static float g_JasonBaseSlowRunSpeed = 0.0f;
    static float g_JasonPursuitAppliedMultiplier = 0.0f;
    static ULONGLONG g_JasonPursuitAppliedCooldownMs = 0;
    static ULONGLONG g_NextJasonPursuitParameterRefreshAt = 0;
    static int32_t g_PoliceArrivedPropertyOffset = -2;
    static uint8_t g_PoliceArrivedByteOffset = 0;
    static uint8_t g_PoliceArrivedFieldMask = 0;
    static AActor* g_HidingSpotTarget = nullptr;
    static AActor* g_HidingSpotCounselor = nullptr;
    static UObject* g_HidingSpotInteractable = nullptr;
    static AActor* g_IgnoredHidingSpot = nullptr;
    static AActor* g_IgnoredHidingCounselor = nullptr;
    static ULONGLONG g_HidingSpotStartedAt = 0;
    static ULONGLONG g_HidingSpotAttemptPendingUntil = 0;
    static ULONGLONG g_HidingSpotIgnoreUntil = 0;
    static ULONGLONG g_NextHidingSpotMoveAt = 0;
    static int32_t g_HidingSpotAttempts = 0;
    static bool g_HidingSpotRepositionAttempted = false;
    static bool g_HidingSpotRepositionSucceeded = false;
    static ULONGLONG g_NextHidingSpotScanAt = 0;
    static ULONGLONG g_NextHidingSpotInteractAt = 0;
    static ULONGLONG g_NextHidingSpotLogAt = 0;
    static ULONGLONG g_NextWalkieCleanupAt = 0;
    static int32_t g_WalkieCleanupLevel = 0;
    static int32_t g_WalkieCleanupActor = 0;
    static int32_t g_WalkiesRemoved = 0;
    static int32_t g_TapesRemoved = 0;
    static int32_t g_InvalidPropellersRemoved = 0;
    static int32_t g_SurplusKeysRemoved = 0;
    static int32_t g_SurplusFusesRemoved = 0;
    static int32_t g_UsefulPickupsSpawned = 0;
    static bool g_LootCleanupPassActive = false;
    static bool g_LootCleanupComplete = false;
    static bool g_LootCensusSawBoat = false;
    static int32_t g_LootCensusCarCount = 0;
    static int32_t g_LootNonLooseKeyCount = 0;
    static int32_t g_LootNonLooseFuseCount = 0;
    static int32_t g_LootKeysKeptThisPass = 0;
    static int32_t g_LootFusesKeptThisPass = 0;
    static uint32_t g_LootReplacementRandomState = 0;
    static UClass* g_LootReplacementClasses[3]{};
    static ULONGLONG g_NextCounselorFleeRefreshAt = 0;
    static bool g_CounselorBlackboardNameResolutionComplete = false;
    static int32_t g_CounselorBlackboardNameResolveCursor = 0;
    static int32_t g_SCWeaponNameIndex = -1;
    static int32_t g_ShouldFleeKillerNameIndex = -1;
    static int32_t g_ShouldFightBackNameIndex = -1;
    static int32_t g_ShouldArmedFightBackNameIndex = -1;
    static int32_t g_ShouldMeleeFightBackNameIndex = -1;
    static int32_t g_SeekWeaponWhileFleeingNameIndex = -1;
    static int32_t g_ShouldHideNameIndex = -1;
    static int32_t g_ShouldOrientTowardKillerNameIndex = -1;
    static int32_t g_JasonCharacterNameIndex = -1;
    static ULONGLONG g_NextCounselorTargetPruneAt = 0;
    struct PendingDeadCounselorRetire
    {
        AActor* Pawn = nullptr;
        ULONGLONG ReadyAt = 0;
    };
    static PendingDeadCounselorRetire
        g_PendingDeadCounselorRetires[JasonAITargetCapacity]{};
    static bool g_FinalCounselorDeathObserved = false;
    static ULONGLONG g_NextCounselorRosterRefreshAt = 0;
    static int32_t g_CounselorRosterRefreshLevel = 0;
    static UObject* g_CounselorControllers[JasonAITargetCapacity]{};
    static int32_t g_CounselorControllerCount = 0;
    static bool g_CounselorConvergenceActive = false;
    static ULONGLONG g_NextCounselorConvergenceAt = 0;
    static ULONGLONG g_NextConvergenceCombatSweepAt = 0;
    static int32_t g_SweaterPreconvergenceCursor = 0;
    static int32_t g_CounselorConvergenceCursor = 0;
    static UClass* g_CounselorConvergenceMeleeClass = nullptr;
    static UClass* g_CounselorConvergenceMacheteClass = nullptr;
    static AActor* g_ConvergenceArmedCounselors[JasonAITargetCapacity]{};
    static int32_t g_ConvergenceArmedCounselorCount = 0;
    static AActor* g_ConvergenceTravelCounselors[JasonAITargetCapacity]{};
    static int32_t g_ConvergenceTravelCounselorCount = 0;
    static FVector g_ConvergenceTravelLastLocations[JasonAITargetCapacity]{};
    static ULONGLONG g_ConvergenceTravelLastProgressAt[JasonAITargetCapacity]{};
    static ULONGLONG g_ConvergenceTravelRecoveryUntil[JasonAITargetCapacity]{};
    static bool g_ConvergenceTravelHaveLocation[JasonAITargetCapacity]{};
    static FVector g_ConvergenceTravelLastGoals[JasonAITargetCapacity]{};
    static ULONGLONG g_ConvergenceTravelLastRouteAt[JasonAITargetCapacity]{};
    static bool g_ConvergenceTravelHaveGoal[JasonAITargetCapacity]{};
    enum class KillTeamRoute : uint8_t
    {
        None,
        HumanTommyFemaleHelper,
        HumanSweaterAITommy
    };
    static KillTeamRoute g_KillTeamRoute = KillTeamRoute::None;
    static AActor* g_KillTeamHelper = nullptr;
    static AActor* g_KillTeamShack = nullptr;
    static AActor* g_KillTeamSweater = nullptr;
    static AActor* g_KillTeamAxe = nullptr;
    static AActor* g_KillTeamMask = nullptr;
    static ULONGLONG g_NextKillTeamTickAt = 0;
    static ULONGLONG g_NextKillTeamDiscoveryAt = 0;
    static ULONGLONG g_NextKillTeamInteractAt = 0;
    static ULONGLONG g_NextHelperKnifeGrantAt = 0;
    static ULONGLONG g_NextSweaterUseAt = 0;
    static ULONGLONG g_NextFinalKillInteractAt = 0;
    static ULONGLONG g_NextKillTeamMoveAt = 0;
    static ULONGLONG g_NextKillTeamFollowAt = 0;
    static AActor* g_KillTeamMoveTarget = nullptr;
    static AActor* g_TommyJasonObjectiveOwner = nullptr;
    static ULONGLONG g_NextTommyJasonObjectiveRepairAt = 0;
    static ULONGLONG g_OrphanJasonStunStartedAt = 0;
    static bool g_KillTeamHelperArmed = false;
    static bool g_KillTeamHelperProtected = false;
    static bool g_KillTeamMaskAcquired = false;
    static bool g_KillTeamSweaterUseDispatched = false;
    static bool g_KillTeamAxeDiscoveryAttempted = false;
    static ULONGLONG g_KillTeamAxePursuitStartedAt = 0;
    static ULONGLONG g_KillTeamAxeLastProgressAt = 0;
    static float g_KillTeamAxeBestDistance = FLT_MAX;
    static ULONGLONG g_KillTeamHelperNativeBusyUntil = 0;
    static int32_t g_KillTeamHelperStableObservations = 0;
    static ULONGLONG g_KillTeamFinalContextUntil = 0;
    static bool g_KillTeamFinalInteractionDispatched = false;
    static bool g_KillTeamFinalInteractionPending = false;
    static int32_t g_KillTeamFinalInteractionAttempts = 0;
    static ULONGLONG g_KillTeamFinalInteractionStartedAt = 0;
    static ULONGLONG g_KillTeamFinalSequenceStartedAt = 0;
    static bool g_KillTeamFinalInteractionCommitted = false;
    static ULONGLONG g_FinalKillCompletionDeadline = 0;
    static std::atomic<bool> g_NativeJasonDeathEventObserved{false};
    static ULONGLONG g_NativeJasonDeathCinematicObservedAt = 0;
    static ULONGLONG g_NativeJasonDeathMatchEndAt = 0;
    static ULONGLONG g_KillTeamFinalRecoveryCooldownUntil = 0;
    static int32_t g_KillTeamFinalMoveFailures = 0;
    static bool g_KillTeamFinalRepositionAttempted = false;
    static UObject* g_KillTeamLastCancelledInteraction = nullptr;
    static ULONGLONG g_NextKillTeamInteractionCancelAt = 0;
    static AActor* g_KillTeamPendingFinalContext = nullptr;
    static UObject* g_KillTeamPendingFinalComponent = nullptr;
    static AActor* g_KillTeamPendingFinalFinisher = nullptr;
    static AActor* g_LastRejectedFinalContext = nullptr;
    static AActor* g_LastAcceptedFinalContext = nullptr;
    static UObject* g_LastAcceptedFinalComponent = nullptr;
    static UObject* g_LastAcceptedFinalKillComponent = nullptr;
    // Set only when the stock ContextKill predicate accepts a counselor for
    // this exact JasonDeath actor.  A JasonDeath context can disappear when a
    // kneel expires without any execution; do not mistake that for a kill.
    static AActor* g_LastUniversalFinalEligibilityContext = nullptr;
    static AActor* g_PermanentHumanSweaterCarrier = nullptr;
    static UObject* g_PermanentHumanSweaterAbility = nullptr;
    // Captured once when the sweater is acquired. The stock HUD keeps the
    // counselor's innate ability in ActiveAbility and advertises Pamela's
    // power separately through SweaterAbility. Restoring the sweater into
    // ActiveAbility after use suppresses the Y prompt.
    static UObject* g_PermanentHumanInnateActiveAbility = nullptr;
    static bool g_PermanentHumanSweaterLatched = false;
    static bool g_PermanentHumanSweaterRestoreLogged = false;
    static bool g_PermanentHumanUnlimitedSweaterArmed = false;
    static ULONGLONG g_PermanentHumanSweaterRearmAt = 0;
    static int32_t g_PermanentHumanSweaterRearmAttempts = 0;
    static bool g_PermanentHumanSweaterRearmSucceeded = false;
    // Treat stock's ten-second ownership cleanup as one activation edge.
    // Republishing the complete ability during that interval can replay the
    // voice/montage and reset the requested two-slash kill sequence.
    static ULONGLONG g_PermanentHumanSweaterActivationUntil = 0;
    static ULONGLONG g_PermanentHumanSweaterAttackUnlockAt = 0;
    static int32_t g_PermanentHumanSweaterUseCount = 0;
    static AActor* g_MaskSweaterPowerLastOwner = nullptr;
    static AActor* g_MaskSweaterPickupOwnerPending = nullptr;
    static ULONGLONG g_MaskSweaterPickupPendingUntil = 0;
    static UObject* g_MaskPickupEventObject = nullptr;
    static ULONGLONG g_NextMaskSweaterPowerCheckAt = 0;
    static std::atomic<bool> g_MaskPickupDiagnosticRequested{ false };
    static ULONGLONG g_PamelaTranceKillWindowUntil = 0;
    static float g_PamelaTranceJasonHealthBaseline = 0.0f;
    static bool g_PamelaTranceJasonHealthBaselineValid = false;
    static std::atomic<bool> g_PamelaTranceDamageEventObserved{ false };
    static bool g_PamelaTranceKneelRequested = false;
    static bool g_PamelaTranceFinalSlashAccepted = false;
    // A melee hit from the local counselor is the closest equivalent to the
    // user's requested "slash to finish" input. Remember that pawn across the
    // short EnterKillStance transition so the automatic native A interaction
    // is not accidentally assigned to a nearby AI counselor instead.
    static AActor* g_PamelaTrancePreferredFinisher = nullptr;
    static ULONGLONG g_PamelaTrancePreferredFinisherUntil = 0;
    static ULONGLONG g_RepeatJasonKillStanceAt = 0;
    static ULONGLONG g_RepeatJasonKillStanceDeadline = 0;
    static int32_t g_RepeatJasonKillStanceAttempts = 0;
    static bool g_PamelaExclusiveKillLaneActive = false;
    static ULONGLONG g_NextPamelaExclusiveStopAt = 0;
    static bool g_UnlimitedSweaterWorldRuleLogged = false;
    static ULONGLONG g_NextFinalContextDiagnosticAt = 0;
    static ULONGLONG g_NextAITommyProtectionAt = 0;
    static AActor* g_ProtectedAITommy = nullptr;
    static ULONGLONG g_NextTommyObjectivePublishAt = 0;
    static AActor* g_TommyObjectiveRadio = nullptr;
    static int32_t g_CounselorFleeRefreshCursor = 0;
    static bool g_TommyObjectivePublished = false;
    static ULONGLONG g_FuseDiagnosticAt = 0;
    static bool g_FuseDiagnosticLogged = false;
    static int32_t g_MatchStateOffset = -1;
    static bool g_PostMatchAIRetired = false;

    bool IsValidatedLiveCounselorPawn(AActor* actor);
    void RetireDeadAICounselorFromSpectating(AActor* actor);
    bool IsHunterCounselor(AActor* counselor);
    bool HasPamelaSweater(AActor* counselor);
    bool IsJasonBridgeCombatBusy(ULONGLONG now);
    bool ReadReflectedBoolByte(UObject* object, const char* propertyName);
    bool ReadCachedReflectedBool(
        UObject* object,
        const char* propertyName,
        int32_t& cachedOffset,
        uint8_t& cachedByteOffset,
        uint8_t& cachedFieldMask);
    AActor* ReadActorField(UObject* owner, uintptr_t offset);
    UObject* GetJasonInteractionManager(AActor* jason);
    UObject* GetLockedJasonInteractable(AActor* jason);
    bool ReleaseStaleJasonHidingInteraction(
        UObject* manager,
        AActor* jason,
        ULONGLONG now,
        const char* reason);
    bool IsFinalContextLockMatch(
        UObject* locked,
        AActor* context,
        UObject* component);
    bool ClassifyRepairableCar(
        AActor* actor,
        uint8_t& outKind,
        int32_t& outSeatCount);
    bool IssueAIMoveToLocationOnGameThread(
        UObject* controller,
        const FVector& destination,
        float acceptanceRadius,
        const char* label,
        bool exactCenterline = false);

    bool ObjectClassDerivesFromExact(UObject* object, const char* className)
    {
        if (!object ||
            !className ||
            !Memory::IsReadable(object, sizeof(UObject)) ||
            !object->Class)
        {
            return false;
        }

        for (UStruct* current = reinterpret_cast<UStruct*>(object->Class);
            current;
            current = SafeReadSuperStruct(current))
        {
            if (!Memory::IsReadable(current, sizeof(UStruct)))
                break;
            if (JasonAISafeName(reinterpret_cast<UObject*>(current)) == className)
                return true;
        }
        return false;
    }

    UObject* ReadReflectedObjectProperty(UObject* owner, const char* propertyName)
    {
        if (!owner ||
            !propertyName ||
            !Memory::IsReadable(owner, sizeof(UObject)) ||
            !owner->Class)
        {
            return nullptr;
        }

        UPropertyLite* property =
            FindPropertyInHierarchyByName(owner->Class, propertyName);
        if (!property ||
            property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000)
        {
            return nullptr;
        }

        UObject** field = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(owner) + property->Offset_Internal);
        if (!Memory::IsReadable(field, sizeof(UObject*)) ||
            !*field ||
            !Memory::IsReadable(*field, sizeof(UObject)))
        {
            return nullptr;
        }
        return *field;
    }

    int32_t FindFNameIndexExact(const char* wanted)
    {
        if (!wanted || !GNames ||
            !Memory::IsReadable(GNames, sizeof(TNameEntryArray)) ||
            GNames->NumElements <= 0 ||
            GNames->NumElements > 4000000)
        {
            return -1;
        }

        for (int32_t index = 0; index < GNames->NumElements; ++index)
        {
            const FNameEntry* entry = GNames->GetById(index);
            if (entry &&
                Memory::IsReadable(entry, 0x20) &&
                std::strcmp(entry->AnsiName, wanted) == 0)
            {
                return index;
            }
        }
        return -1;
    }

    void ResolveCounselorBlackboardNameIndices()
    {
        if (g_CounselorBlackboardNameResolutionComplete ||
            (g_SCWeaponNameIndex >= 0 &&
            g_ShouldFleeKillerNameIndex >= 0 &&
            g_ShouldFightBackNameIndex >= 0 &&
            g_ShouldArmedFightBackNameIndex >= 0 &&
            g_ShouldMeleeFightBackNameIndex >= 0 &&
            g_SeekWeaponWhileFleeingNameIndex >= 0 &&
            g_ShouldHideNameIndex >= 0 &&
            g_ShouldOrientTowardKillerNameIndex >= 0 &&
            g_JasonCharacterNameIndex >= 0))
        {
            g_CounselorBlackboardNameResolutionComplete = true;
            return;
        }

        if (!GNames ||
            !Memory::IsReadable(GNames, sizeof(TNameEntryArray)) ||
            GNames->NumElements <= 0 ||
            GNames->NumElements > 4000000)
        {
            return;
        }

        // Never scan the complete multi-million-entry name table on a game
        // frame. Advance a very small bounded chunk and stop permanently after
        // the keys or current table end. The caller schedules incomplete
        // discovery frequently so this finishes without a visible burst.
        constexpr int32_t NamesPerMaintenancePass = 512;
        const int32_t end = (std::min)(
            GNames->NumElements,
            g_CounselorBlackboardNameResolveCursor +
                NamesPerMaintenancePass);
        for (int32_t index = g_CounselorBlackboardNameResolveCursor;
             index < end;
             ++index)
        {
            const FNameEntry* entry = GNames->GetById(index);
            if (!entry || !Memory::IsReadable(entry, 0x20))
                continue;
            const char* name = entry->AnsiName;
            if (g_SCWeaponNameIndex < 0 &&
                std::strcmp(name, "SCWeapon") == 0)
                g_SCWeaponNameIndex = index;
            else if (g_ShouldFleeKillerNameIndex < 0 &&
                std::strcmp(name, "ShouldFleeKiller") == 0)
                g_ShouldFleeKillerNameIndex = index;
            else if (g_ShouldFightBackNameIndex < 0 &&
                std::strcmp(name, "ShouldFightBack") == 0)
                g_ShouldFightBackNameIndex = index;
            else if (g_ShouldArmedFightBackNameIndex < 0 &&
                std::strcmp(name, "ShouldArmedFightBack") == 0)
                g_ShouldArmedFightBackNameIndex = index;
            else if (g_ShouldMeleeFightBackNameIndex < 0 &&
                std::strcmp(name, "ShouldMeleeFightBack") == 0)
                g_ShouldMeleeFightBackNameIndex = index;
            else if (g_SeekWeaponWhileFleeingNameIndex < 0 &&
                std::strcmp(name, "SeekWeaponWhileFleeing") == 0)
                g_SeekWeaponWhileFleeingNameIndex = index;
            else if (g_ShouldHideNameIndex < 0 &&
                std::strcmp(name, "ShouldHide") == 0)
                g_ShouldHideNameIndex = index;
            else if (g_ShouldOrientTowardKillerNameIndex < 0 &&
                std::strcmp(name, "ShouldOrientTowardKiller") == 0)
                g_ShouldOrientTowardKillerNameIndex = index;
            else if (g_JasonCharacterNameIndex < 0 &&
                std::strcmp(name, "JasonCharacter") == 0)
                g_JasonCharacterNameIndex = index;

            if (g_SCWeaponNameIndex >= 0 &&
                g_ShouldFleeKillerNameIndex >= 0 &&
                g_ShouldFightBackNameIndex >= 0 &&
                g_ShouldArmedFightBackNameIndex >= 0 &&
                g_ShouldMeleeFightBackNameIndex >= 0 &&
                g_SeekWeaponWhileFleeingNameIndex >= 0 &&
                g_ShouldHideNameIndex >= 0 &&
                g_ShouldOrientTowardKillerNameIndex >= 0 &&
                g_JasonCharacterNameIndex >= 0)
            {
                g_CounselorBlackboardNameResolveCursor = index + 1;
                g_CounselorBlackboardNameResolutionComplete = true;
                break;
            }
        }
        g_CounselorBlackboardNameResolveCursor = end;
        if (end >= GNames->NumElements)
            g_CounselorBlackboardNameResolutionComplete = true;
    }

    bool SetBlackboardBool(UObject* blackboard, int32_t keyIndex, bool value)
    {
        if (!blackboard ||
            keyIndex < 0 ||
            !Memory::IsReadable(blackboard, sizeof(UObject)) ||
            !blackboard->Class)
        {
            return false;
        }

        // All counselor blackboards share a class. Cache both successful and
        // failed reflection lookups so the one-second kill-team path never
        // re-walks the UFunction hierarchy.
        static UClass* cachedClass = nullptr;
        static UFunction* cachedFunction = nullptr;
        if (cachedClass != blackboard->Class)
        {
            cachedClass = blackboard->Class;
            cachedFunction = FindFunctionInHierarchyByName(
                blackboard->Class,
                "SetValueAsBool");
        }
        UFunction* function = cachedFunction;
        if (!function)
            return false;

        struct Params
        {
            FName KeyName;
            bool BoolValue;
            uint8_t Padding[3];
        };
        Params params{};
        params.KeyName = FName(keyIndex, 0);
        params.BoolValue = value;
        return SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(blackboard),
            blackboard,
            function,
            &params);
    }

    bool SetBlackboardObject(
        UObject* blackboard,
        int32_t keyIndex,
        UObject* value)
    {
        if (!blackboard ||
            keyIndex < 0 ||
            !value ||
            !Memory::IsReadable(blackboard, sizeof(UObject)) ||
            !blackboard->Class ||
            !Memory::IsReadable(value, sizeof(UObject)))
        {
            return false;
        }

        static UClass* cachedClass = nullptr;
        static UFunction* cachedFunction = nullptr;
        if (cachedClass != blackboard->Class)
        {
            cachedClass = blackboard->Class;
            cachedFunction = FindFunctionInHierarchyByName(
                blackboard->Class,
                "SetValueAsObject");
        }
        UFunction* function = cachedFunction;
        if (!function)
            return false;

        struct Params
        {
            FName KeyName;
            UObject* ObjectValue;
        };
        Params params{};
        params.KeyName = FName(keyIndex, 0);
        params.ObjectValue = value;
        return SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(blackboard),
            blackboard,
            function,
            &params);
    }

    UObject* GetBlackboardObject(UObject* blackboard, int32_t keyIndex)
    {
        if (!blackboard ||
            keyIndex < 0 ||
            !Memory::IsReadable(blackboard, sizeof(UObject)) ||
            !blackboard->Class)
        {
            return nullptr;
        }

        static UClass* cachedClass = nullptr;
        static UFunction* cachedFunction = nullptr;
        if (cachedClass != blackboard->Class)
        {
            cachedClass = blackboard->Class;
            cachedFunction = FindFunctionInHierarchyByName(
                blackboard->Class,
                "GetValueAsObject");
        }
        UFunction* function = cachedFunction;
        if (!function)
            return nullptr;

        struct Params
        {
            FName KeyName;
            UObject* ReturnValue;
        };
        Params params{};
        params.KeyName = FName(keyIndex, 0);
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(blackboard),
                blackboard,
                function,
                &params))
        {
            return nullptr;
        }
        return params.ReturnValue &&
            Memory::IsReadable(params.ReturnValue, sizeof(UObject))
                ? params.ReturnValue
                : nullptr;
    }

    enum class CounselorRouteMatchPhase : uint8_t
    {
        Unknown,
        InProgress,
        NotInProgress
    };

    CounselorRouteMatchPhase ReadCounselorRouteMatchPhase()
    {
        UWorld* world = Engine::GetWorld();
        if (!world ||
            world != g_JasonAIState.World ||
            !Memory::IsReadable(world, sizeof(UWorld)))
        {
            return CounselorRouteMatchPhase::NotInProgress;
        }

        AActor* gameState = ReadActorField(
            reinterpret_cast<UObject*>(world),
            0xF8);
        if (!gameState ||
            !Memory::IsReadable(gameState, sizeof(UObject)) ||
            !gameState->Class ||
            !Memory::IsReadable(gameState->Class, sizeof(UClass)))
        {
            return CounselorRouteMatchPhase::Unknown;
        }

        if (g_MatchStateOffset < 0)
        {
            UPropertyLite* property =
                FindPropertyInHierarchyByName(
                    gameState->Class,
                    "MatchState");
            if (!property ||
                property->Offset_Internal <= 0 ||
                property->Offset_Internal >= 0x10000 ||
                property->ElementSize != sizeof(FName))
            {
                return CounselorRouteMatchPhase::Unknown;
            }
            g_MatchStateOffset = property->Offset_Internal;
        }

        const FName* matchState = reinterpret_cast<const FName*>(
            reinterpret_cast<uintptr_t>(gameState) + g_MatchStateOffset);
        if (!Memory::IsReadable(matchState, sizeof(FName)) ||
            !GNames ||
            !GNames->IsValidIndex(matchState->ComparisonIndex))
        {
            return CounselorRouteMatchPhase::Unknown;
        }

        const FNameEntry* entry =
            GNames->GetById(matchState->ComparisonIndex);
        if (!entry || !Memory::IsReadable(entry, 0x20))
            return CounselorRouteMatchPhase::Unknown;

        return std::strcmp(entry->AnsiName, "InProgress") == 0
            ? CounselorRouteMatchPhase::InProgress
            : CounselorRouteMatchPhase::NotInProgress;
    }

    void ResetVehicleInterceptionState(ULONGLONG retryAfter = 0)
    {
        g_VehicleInterceptCar = nullptr;
        g_VehicleInterceptSeat = nullptr;
        g_VehicleInterceptHeading = FVector{};
        g_VehicleHeadingStableSince = 0;
        g_VehicleInterceptDeadline = 0;
        g_VehicleInterceptRetryAfter = retryAfter;
        // A nonzero retry deadline is also the next allowed discovery pass.
        // Do not erase it and accidentally turn idle vehicle inspection into
        // presentation-frame work.
        g_NextVehicleInterceptActionAt = retryAfter;
        g_NextVehicleSeatRediscoveryAt = 0;
        g_NextVehicleForwardSpeedSampleAt = 0;
        g_CachedVehicleForwardSpeed = 0.0f;
        g_NextVehicleInterceptLogAt = 0;
        g_NextVehicleMoveLogAt = 0;
        g_VehicleLastCenterlineMovePoint = FVector{};
        g_VehicleLastCenterlineMoveAt = 0;
        g_VehicleHaveLastCenterlineMovePoint = false;
        g_VehicleMotionSampleCar = nullptr;
        g_VehicleMotionSampleLocation = FVector{};
        g_VehicleMotionSampleDirection = FVector{};
        g_VehicleMotionSampleSpeed = 0.0f;
        g_VehicleMotionSampleAt = 0;
        g_VehicleHaveMotionSample = false;
        g_VehicleInterceptMorphUsed = false;
        g_VehicleHoodInputSentAt = 0;
        g_VehicleMovingHoodAttemptAt = 0;
        g_VehicleMovingHoodAttempts = 0;
        g_VehicleSlamObserved = false;
        g_VehicleExtractionCommitted = false;
        g_VehicleExtractionCommittedAt = 0;
        g_VehicleExtractionInputAt = 0;
        g_VehicleExtractionAttempts = 0;
        g_VehicleDriverDetourReached = false;
        g_VehicleDriverDetourAttempts = 0;
        g_VehicleDriverApproachStartedAt = 0;
        g_VehicleDriverLastProgressAt = 0;
        g_VehicleDriverReadySince = 0;
        g_VehicleDriverBestDistance = FLT_MAX;
        g_VehicleCachedDriverDoorPoint = FVector{};
        g_VehicleCachedDriverDetourPoint = FVector{};
        g_VehicleLastDriverMovePoint = FVector{};
        g_VehicleCachedDriverDoorCar = nullptr;
        g_VehicleCachedDriverDoorSeat = nullptr;
        g_VehicleHaveCachedDriverDoorPoint = false;
        g_VehicleHaveCachedDriverDetourPoint = false;
        g_VehicleHaveLastDriverMovePoint = false;
        g_VehicleLastDriverMoveAt = 0;
    }

    bool HasPoliceArrived()
    {
        UObject* gameState = reinterpret_cast<UObject*>(
            g_JasonAIState.StartupTrapGameState);
        return gameState && ReadCachedReflectedBool(
            gameState,
            "bHasPoliceArrived",
            g_PoliceArrivedPropertyOffset,
            g_PoliceArrivedByteOffset,
            g_PoliceArrivedFieldMask);
    }

    void SetJasonHighPriorityPursuitBoost(
        bool enabled,
        const char* reason,
        float speedMultiplier = 2.0f,
        ULONGLONG morphCooldownMs = 10000ULL)
    {
        // These are plain movement fields, but rewriting all four every AI
        // controller frame needlessly dirties the movement component for the
        // complete car chase.  Latch a stable pursuit mode and refresh only
        // occasionally in case native gameplay restores one of the values.
        const ULONGLONG now = GetTickCount64();

        if (!enabled && !g_JasonPursuitBoostActive)
            return;

        AActor* jason = g_JasonAIState.Jason;
        void* movement = nullptr;
        if (jason && Memory::IsReadable(jason, 0x3D8))
            movement = reinterpret_cast<ACharacter*>(jason)->CharacterMovement;

        auto validSpeed = [](float value)
        {
            return std::isfinite(value) && value > 25.0f && value < 5000.0f;
        };

        if (enabled && movement &&
            Memory::IsReadable(movement, sizeof(UCharacterMovementComponent)))
        {
            auto* move = reinterpret_cast<UCharacterMovementComponent*>(movement);
            if (movement != g_JasonPursuitMovement ||
                !validSpeed(g_JasonBaseWalkSpeed))
            {
                if (!validSpeed(move->MaxWalkSpeed) ||
                    !validSpeed(move->MaxSprintSpeed) ||
                    !validSpeed(move->MaxRunSpeed) ||
                    !validSpeed(move->MaxSlowRunSpeed))
                {
                    return;
                }
                g_JasonPursuitMovement = movement;
                g_JasonBaseWalkSpeed = move->MaxWalkSpeed;
                g_JasonBaseSprintSpeed = move->MaxSprintSpeed;
                g_JasonBaseRunSpeed = move->MaxRunSpeed;
                g_JasonBaseSlowRunSpeed = move->MaxSlowRunSpeed;
            }
            const float effectiveMultiplier =
                std::isfinite(speedMultiplier) && speedMultiplier > 0.0f
                    ? speedMultiplier
                    : 2.0f;
            const ULONGLONG effectiveCooldown =
                morphCooldownMs >= 2000ULL ? morphCooldownMs : 2000ULL;
            const bool sameParameters =
                g_JasonPursuitBoostActive &&
                std::fabs(
                    g_JasonPursuitAppliedMultiplier - effectiveMultiplier) <
                    0.001f &&
                g_JasonPursuitAppliedCooldownMs == effectiveCooldown;
            if (sameParameters &&
                now < g_NextJasonPursuitParameterRefreshAt)
            {
                return;
            }
            move->MaxWalkSpeed = g_JasonBaseWalkSpeed * effectiveMultiplier;
            move->MaxSprintSpeed = g_JasonBaseSprintSpeed * effectiveMultiplier;
            move->MaxRunSpeed = g_JasonBaseRunSpeed * effectiveMultiplier;
            move->MaxSlowRunSpeed = g_JasonBaseSlowRunSpeed * effectiveMultiplier;
            JasonAIMorphCooldownMs = effectiveCooldown;
            g_JasonPursuitAppliedMultiplier = effectiveMultiplier;
            g_JasonPursuitAppliedCooldownMs = effectiveCooldown;
            // The four movement fields remain stable for the complete chase.
            // Rewriting them every two seconds matched the live hitch exactly;
            // retain only a slow safety repair, deliberately out of phase with
            // navigation and overlay maintenance.
            g_NextJasonPursuitParameterRefreshAt = now + 30013;
            if (!g_JasonPursuitBoostActive)
            {
                g_JasonPursuitBoostActive = true;
                Logger::Success(std::string(
                    "18L-BJ Jason high-priority pursuit boost engaged | speed=") +
                    std::to_string(effectiveMultiplier) +
                    "x | morphCooldownMs=" +
                    std::to_string(effectiveCooldown) +
                    " | reason=" +
                    (reason ? reason : "priority"));
            }
            return;
        }

        if (g_JasonPursuitMovement &&
            Memory::IsReadable(
                g_JasonPursuitMovement,
                sizeof(UCharacterMovementComponent)))
        {
            auto* move = reinterpret_cast<UCharacterMovementComponent*>(
                g_JasonPursuitMovement);
            if (validSpeed(g_JasonBaseWalkSpeed))
                move->MaxWalkSpeed = g_JasonBaseWalkSpeed;
            if (validSpeed(g_JasonBaseSprintSpeed))
                move->MaxSprintSpeed = g_JasonBaseSprintSpeed;
            if (validSpeed(g_JasonBaseRunSpeed))
                move->MaxRunSpeed = g_JasonBaseRunSpeed;
            if (validSpeed(g_JasonBaseSlowRunSpeed))
                move->MaxSlowRunSpeed = g_JasonBaseSlowRunSpeed;
        }
        JasonAIMorphCooldownMs = 20000ULL;
        g_JasonPursuitAppliedMultiplier = 0.0f;
        g_JasonPursuitAppliedCooldownMs = 0;
        g_NextJasonPursuitParameterRefreshAt = 0;
        if (g_JasonPursuitBoostActive)
        {
            g_JasonPursuitBoostActive = false;
            Logger::Debug(
                "18L-BJ Jason high-priority pursuit boost retired; stock movement and 20s Morph restored");
        }
    }

    __declspec(noinline) AActor* SafeGetGrabbedCounselor(
        AActor* jason,
        uintptr_t functionAddress)
    {
        if (!jason ||
            !functionAddress ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(functionAddress), 1))
        {
            return nullptr;
        }

        __try
        {
            using Function = AActor* (__fastcall*)(AActor*);
            return reinterpret_cast<Function>(functionAddress)(jason);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    __declspec(noinline) bool SafeJasonBoolCall(
        AActor* jason,
        uintptr_t functionAddress)
    {
        if (!jason ||
            !functionAddress ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(functionAddress), 1))
        {
            return false;
        }

        __try
        {
            using Function = bool(__fastcall*)(AActor*);
            return reinterpret_cast<Function>(functionAddress)(jason);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __declspec(noinline) bool SafeIsGrabKillAvailable(
        AActor* jason,
        uintptr_t functionAddress,
        int32_t nativeSlot)
    {
        if (!jason ||
            !functionAddress ||
            nativeSlot < 0 ||
            nativeSlot > 3 ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(functionAddress), 1))
        {
            return false;
        }

        __try
        {
            using Function = bool(__fastcall*)(AActor*, int32_t);
            return reinterpret_cast<Function>(functionAddress)(
                jason,
                nativeSlot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __declspec(noinline) bool SafeJasonGrabKillInput(
        AActor* jason,
        uintptr_t functionAddress,
        int32_t nativeSlot)
    {
        if (!jason ||
            !functionAddress ||
            nativeSlot < 0 ||
            nativeSlot > 3 ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(functionAddress), 1))
        {
            return false;
        }

        __try
        {
            using Function = void(__fastcall*)(AActor*, int32_t);
            reinterpret_cast<Function>(functionAddress)(jason, nativeSlot);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ResetStuckSamplingAfterNativeInteraction(ULONGLONG now)
    {
        g_JasonAIState.ConsecutiveStuckChecks = 0;
        g_JasonAIState.LastAcceptedMoveAt = 0;
        g_JasonAIState.HaveLastLocation = false;
        g_JasonAIState.PathLocked = false;
        g_JasonAIState.PathLockUntil = 0;
        g_JasonAIState.NextStuckCheckAt = now + 1375;
        g_JasonAIState.NoPathTarget = nullptr;
        g_JasonAIState.ConsecutiveNoPathTargets = 0;
    }

    void LogRuntimeGrabKillSlots(AActor* jason)
    {
        if (!jason || g_GrabKillSlotsLoggedFor == jason)
            return;

        g_GrabKillSlotsLoggedFor = jason;
        UObject** grabKills = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(jason) + 0x11C0);
        if (!Memory::IsReadable(grabKills, sizeof(UObject*) * 4))
        {
            Logger::Debug(
                "18L-AI grab kill: runtime slot array unreadable");
            return;
        }

        for (int32_t slot = 0; slot < 4; ++slot)
        {
            UObject* grabKill = grabKills[slot];
            Logger::Debug(
                "18L-AI grab kill slot[" + std::to_string(slot) + "]=" +
                (grabKill && Memory::IsReadable(grabKill, sizeof(UObject))
                    ? JasonAISafeName(grabKill)
                    : std::string("null")));
        }
    }

    bool IsCounselorInPostGrabTransition(
        AActor* counselor,
        ULONGLONG now)
    {
        if (g_PostGrabTransitionCounselor &&
            now >= g_PostGrabTransitionUntil)
        {
            g_PostGrabTransitionCounselor = nullptr;
            g_PostGrabTransitionUntil = 0;
        }
        return counselor &&
            counselor == g_PostGrabTransitionCounselor &&
            now < g_PostGrabTransitionUntil;
    }

    void QuarantineReleasedGrabCounselor(
        AActor* counselor,
        ULONGLONG now)
    {
        if (!counselor)
            return;

        constexpr ULONGLONG kNativeGrabSettleMs = 2500;
        g_PostGrabTransitionCounselor = counselor;
        g_PostGrabTransitionUntil = now + kNativeGrabSettleMs;

        const int32_t targetIndex = FindJasonAITargetIndex(counselor);
        if (targetIndex >= 0 && targetIndex < JasonAITargetCapacity)
        {
            g_JasonAITargetBlockedUntil[targetIndex] =
                (std::max)(
                    g_JasonAITargetBlockedUntil[targetIndex],
                    g_PostGrabTransitionUntil);
        }
        if (g_JasonAIState.Target == counselor)
            g_JasonAIState.Target = nullptr;

        // Defer the convergence lane past the native transition as well. This
        // specifically prevents GiveStartingItem/ProcessEvent from running on
        // a pawn that UE4 is still releasing or destroying.
        g_NextCounselorConvergenceAt =
            (std::max)(g_NextCounselorConvergenceAt,
                g_PostGrabTransitionUntil);
        g_JasonAIState.NextCombatAttemptAt =
            (std::max)(g_JasonAIState.NextCombatAttemptAt,
                g_PostGrabTransitionUntil);
        Logger::Debug(
            "18L-AK post-grab transition quarantine armed | counselor=" +
            JasonAISafeName(reinterpret_cast<UObject*>(counselor)) +
            " | durationMs=" + std::to_string(kNativeGrabSettleMs));
    }

    bool IsNativeCounselorDeathConfirmed(AActor* counselor)
    {
        if (!counselor)
            return false;

        const uint8_t* dead = reinterpret_cast<const uint8_t*>(
            reinterpret_cast<uintptr_t>(counselor) + 0x1031);
        const float* health = reinterpret_cast<const float*>(
            reinterpret_cast<uintptr_t>(counselor) +
            Offsets::ASCCharacter_Health);
        return (Memory::IsReadable(dead, 1) && *dead != 0) ||
            (Memory::IsReadable(health, sizeof(float)) &&
             std::isfinite(*health) && *health <= 0.0f);
    }

    bool DriveHeldCounselorGrabKill(ULONGLONG now)
    {
        AActor* jason = g_JasonAIState.Jason;
        HMODULE module = GetModuleHandle(nullptr);
        if (!jason || !module)
            return false;

        constexpr uintptr_t RVA_GetGrabbedCounselor = 0x004055E0;
        constexpr uintptr_t RVA_CanGrabKill = 0x003FDB40;
        constexpr uintptr_t RVA_IsGrabKillAvailable = 0x0040D070;
        constexpr uintptr_t RVA_IsGrabKilling = 0x0040D1A0;
        constexpr uintptr_t RVA_GrabKillInput = 0x0041B040;

        const uintptr_t base = reinterpret_cast<uintptr_t>(module);
        AActor* heldCounselor = SafeGetGrabbedCounselor(
            jason,
            base + RVA_GetGrabbedCounselor);
        if (!heldCounselor)
        {
            // The mature combat driver treats a successful native function
            // call as a possible grab and schedules a follow-up input. The
            // call itself does not guarantee that Jason actually acquired a
            // counselor. Clear that speculative timer once it matures unless
            // GetGrabbedCounselor confirms the paired native state; otherwise
            // Jason can repeatedly send grab-kill input while merely following
            // a target and appear frozen.
            if (g_JasonAIState.GrabKillReadyAt != 0 &&
                now >= g_JasonAIState.GrabKillReadyAt)
            {
                g_JasonAIState.GrabKillReadyAt = 0;
            }
            if (g_LastHeldCounselor)
            {
                AActor* releasedCounselor = g_LastHeldCounselor;
                if (g_LastHeldCounselor == g_KillTeamHelper ||
                    g_LastHeldCounselor == g_ProtectedAITommy)
                {
                    g_KillTeamHelperNativeBusyUntil = now + 3500;
                    g_KillTeamHelperStableObservations = 0;
                    // Recheck the pocket knife once the native grab/escape
                    // transition is quiet instead of polling inventory on a
                    // short global cadence.
                    g_NextAITommyProtectionAt = now + 3550;
                }
                Logger::Debug(
                    "18L-AI grab kill: counselor released; chase state reset");
                if (!IsNativeCounselorDeathConfirmed(releasedCounselor))
                    QuarantineReleasedGrabCounselor(releasedCounselor, now);
                ResetStuckSamplingAfterNativeInteraction(now);
            }
            g_LastHeldCounselor = nullptr;
            g_HeldCounselorObservedAt = 0;
            g_GrabKillInputCommittedAt = 0;
            g_NextAdapterGrabKillAttemptAt = 0;
            g_StaleGrabReleaseAttempted = false;
            return false;
        }

        if (g_VehicleInterceptCar)
        {
            // A successful driver extraction transfers ownership to the
            // native grab/kill state. Retire the car route immediately so its
            // road projection, MoveTo refresh and 4x pursuit writes cannot run
            // alongside the paired grab animation or reacquire the same car.
            AActor* extractedFromCar = g_VehicleInterceptCar;
            g_IgnoredVehicleInterceptCar = extractedFromCar;
            g_IgnoredVehicleInterceptUntil = now + 5000;
            ResetVehicleInterceptionState(now + 5000);
            SetJasonHighPriorityPursuitBoost(false, "driver-extracted");
            Logger::Success(
                "18L-BK vehicle intercept retired immediately after driver extraction");
        }

        ResetStuckSamplingAfterNativeInteraction(now);
        g_JasonAIState.GrabKillReadyAt = 0;

        if (heldCounselor == g_KillTeamHelper ||
            heldCounselor == g_ProtectedAITommy)
        {
            g_KillTeamHelperNativeBusyUntil = now + 3500;
            g_KillTeamHelperStableObservations = 0;
        }

        if (g_LastHeldCounselor != heldCounselor)
        {
            g_LastHeldCounselor = heldCounselor;
            g_HeldCounselorObservedAt = now;
            g_GrabKillInputCommittedAt = 0;
            g_NextAdapterGrabKillAttemptAt = now + 100;
            g_NextGrabKillStateLogAt = 0;
            g_StaleGrabReleaseAttempted = false;
            Logger::Success(
                "18L-AI grab kill: held counselor detected=" +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(heldCounselor)));
            LogRuntimeGrabKillSlots(jason);
        }

        if (IsNativeCounselorDeathConfirmed(heldCounselor))
        {
            // Stock still reports a completed kill victim as held through
            // the death montage. Never call EndStun or retry a grab kill on
            // that dead pawn; the game owns the match-end transition.
            if (now >= g_NextGrabKillStateLogAt)
            {
                g_NextGrabKillStateLogAt = now + 3000;
                Logger::Debug(
                    "18L-BN grab kill: dead victim remains in stock hold; waiting for native release/end match");
            }
            return true;
        }

        if (SafeJasonBoolCall(jason, base + RVA_IsGrabKilling))
        {
            g_NextAdapterGrabKillAttemptAt = now + 200;
            return true;
        }

        if (now < g_NextAdapterGrabKillAttemptAt)
            return true;

        g_NextAdapterGrabKillAttemptAt = now + 175;
        if (!SafeJasonBoolCall(jason, base + RVA_CanGrabKill))
        {
            // A pocket-knife escape normally clears GetGrabbedCounselor on
            // its own.  If the native kill predicate stays false for several
            // seconds, however, the stock interaction can strand Jason in a
            // permanent hold after a failed final-kill attempt.  Release only
            // this stale non-final grab through the native interaction manager
            // and return to ordinary AI; never interfere with a live
            // JasonDeath context or an active grab-kill montage. Once a real
            // native kill input has been dispatched, give its montage five
            // seconds to take ownership before stale recovery may intervene.
            if (g_HeldCounselorObservedAt != 0 &&
                now >= g_HeldCounselorObservedAt + 2500 &&
                (g_GrabKillInputCommittedAt == 0 ||
                    now >= g_GrabKillInputCommittedAt + 5000) &&
                now >= g_KillTeamFinalContextUntil &&
                !g_KillTeamFinalInteractionCommitted &&
                !g_StaleGrabReleaseAttempted)
            {
                UObject* manager = GetJasonInteractionManager(jason);
                if (manager && ReleaseStaleJasonHidingInteraction(
                    manager,
                    jason,
                    now,
                    "stale-grab-kill"))
                {
                    g_StaleGrabReleaseAttempted = true;
                    Logger::Error(
                        "18L-AI grab kill: stale CanGrabKill=false hold released after bounded timeout");
                    QuarantineReleasedGrabCounselor(heldCounselor, now);
                    g_NextAdapterGrabKillAttemptAt = now + 1000;
                    return true;
                }
            }
            if (now >= g_NextGrabKillStateLogAt)
            {
                g_NextGrabKillStateLogAt = now + 1000;
                Logger::Debug(
                    "18L-AI grab kill: native CanGrabKill=false; preserving pocket-knife escape/state transition");
            }
            return true;
        }

        int32_t selectedSlot = -1;
        uint8_t availableMask = 0;
        for (int32_t offset = 0; offset < 4; ++offset)
        {
            const int32_t slot =
                (g_NextAdapterGrabKillSlot + offset) & 3;
            if (SafeIsGrabKillAvailable(
                jason,
                base + RVA_IsGrabKillAvailable,
                slot))
            {
                availableMask |= static_cast<uint8_t>(1u << slot);
                if (selectedSlot < 0)
                    selectedSlot = slot;
            }
        }

        if (selectedSlot < 0)
        {
            if (g_HeldCounselorObservedAt != 0 &&
                now >= g_HeldCounselorObservedAt + 2500 &&
                (g_GrabKillInputCommittedAt == 0 ||
                    now >= g_GrabKillInputCommittedAt + 5000) &&
                now >= g_KillTeamFinalContextUntil &&
                !g_KillTeamFinalInteractionCommitted &&
                !g_StaleGrabReleaseAttempted)
            {
                UObject* manager = GetJasonInteractionManager(jason);
                if (manager && ReleaseStaleJasonHidingInteraction(
                    manager,
                    jason,
                    now,
                    "stale-grab-kill-no-slot"))
                {
                    g_StaleGrabReleaseAttempted = true;
                    Logger::Error(
                        "18L-AI grab kill: no-slot hold released after bounded timeout");
                    QuarantineReleasedGrabCounselor(heldCounselor, now);
                    g_NextAdapterGrabKillAttemptAt = now + 1000;
                    return true;
                }
            }
            if (now >= g_NextGrabKillStateLogAt)
            {
                g_NextGrabKillStateLogAt = now + 1000;
                Logger::Debug(
                    "18L-AI grab kill: no usable runtime slot | availableMask=" +
                    std::to_string(availableMask));
            }
            return true;
        }

        const bool inputDispatched = SafeJasonGrabKillInput(
            jason,
            base + RVA_GrabKillInput,
            selectedSlot);
        if (inputDispatched)
            g_GrabKillInputCommittedAt = now;
        g_NextAdapterGrabKillSlot = (selectedSlot + 1) & 3;
        g_NextAdapterGrabKillAttemptAt = now + 350;
        g_JasonAIState.NextCombatAttemptAt = now + 700;
        Logger::Success(
            "18L-AI grab kill: selected native-available slot=" +
            std::to_string(selectedSlot) +
            " | availableMask=" + std::to_string(availableMask) +
            " | dispatch=" + (inputDispatched ? "true" : "false"));
        return true;
    }

    void ApplyHumanPursuitBalance(ULONGLONG now)
    {
        if (!g_LocalCounselorTarget ||
            g_JasonAIState.Target != g_LocalCounselorTarget ||
            !IsValidatedLiveCounselorPawn(g_LocalCounselorTarget))
        {
            g_HumanPursuitStartedAt = 0;
            return;
        }

        if (g_HumanPursuitStartedAt == 0)
        {
            g_HumanPursuitStartedAt = now;
            return;
        }

        // Preserve close combat and explicit trapped-counselor priority.  If
        // Jason has pursued the human for fifteen seconds without engaging,
        // give a live bot a ten-second pursuit window before reconsidering the
        // player.  This is a target-selection pause, never a teleport.
        if (now - g_HumanPursuitStartedAt < 15000 ||
            g_TrapPriorityVictim ||
            IsJasonBridgeCombatBusy(now) ||
            !HasAlternativeUsableJasonAITarget(g_LocalCounselorTarget))
        {
            return;
        }

        const int32_t humanIndex =
            FindJasonAITargetIndex(g_LocalCounselorTarget);
        if (humanIndex >= 0)
        {
            g_JasonAITargetBlockedUntil[humanIndex] = now + 10000;
            g_JasonAIState.Target = nullptr;
            Logger::Success(
                "18L-AI counselor bridge: rotating pursuit from human to live bot for 10s");
        }
        g_HumanPursuitStartedAt = 0;
    }

    void ExtendTraversalGraceForActiveDoor(ULONGLONG now)
    {
        if (!g_JasonAIState.DoorBreakActive ||
            !g_JasonAIState.DoorBreakTarget)
        {
            return;
        }

        // Frozen code granted post-break traversal grace only to doors whose
        // asset name contained "Interior". Exterior cabin doors exhibit the
        // same threshold pause. Preserve MoveTo and suppress counselor-ring
        // recovery long enough for either kind to clear its doorway.
        const ULONGLONG graceUntil = now + 8000;
        if (g_JasonAIState.DoorTraversalGraceUntil < graceUntil)
            g_JasonAIState.DoorTraversalGraceUntil = graceUntil;
        g_JasonAIState.DoorRetryTarget =
            g_JasonAIState.DoorBreakTarget;
        if (g_JasonAIState.DoorRetryAfter < now + 2500)
            g_JasonAIState.DoorRetryAfter = now + 2500;

        if (g_LastExtendedDoorTarget != g_JasonAIState.DoorBreakTarget)
        {
            g_LastExtendedDoorTarget = g_JasonAIState.DoorBreakTarget;
            Logger::Success(
                "18L-AI counselor bridge: all-door traversal grace armed | door=" +
                JasonAISafeName(reinterpret_cast<UObject*>(
                    g_JasonAIState.DoorBreakTarget)));
        }
    }

    bool ReadCachedReflectedBool(
        UObject* object,
        const char* propertyName,
        int32_t& cachedOffset,
        uint8_t& cachedByteOffset,
        uint8_t& cachedFieldMask)
    {
        if (!object || !object->Class || !propertyName ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }

        if (cachedOffset == -2)
        {
            cachedOffset = -1;
            UPropertyLite* property = FindPropertyInHierarchyByName(
                object->Class,
                propertyName);
            if (property &&
                property->Offset_Internal > 0 &&
                property->Offset_Internal < 0x10000)
            {
                cachedOffset = property->Offset_Internal;
                cachedByteOffset = 0;
                cachedFieldMask = 0xFF;
                const std::string propertyType = JasonAISafeName(
                    reinterpret_cast<UObject*>(property->ClassPrivate));
                if (propertyType == "BoolProperty")
                {
                    uint8_t* boolLayout =
                        reinterpret_cast<uint8_t*>(property) + 0x70;
                    if (Memory::IsReadable(boolLayout, 4))
                    {
                        cachedByteOffset = boolLayout[1];
                        cachedFieldMask = boolLayout[3]
                            ? boolLayout[3]
                            : boolLayout[2];
                    }
                }
            }
        }

        if (cachedOffset < 0 || cachedFieldMask == 0)
            return false;
        uint8_t* value = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(object) +
            static_cast<uintptr_t>(cachedOffset) +
            cachedByteOffset);
        return Memory::IsReadable(value, 1) &&
            (*value & cachedFieldMask) != 0;
    }

    bool HasCounselorEscaped(AActor* actor)
    {
        if (!actor || !Memory::IsReadable(actor, 0x14EE))
            return false;

        // Resolve the reflected field once and thereafter perform only a
        // masked byte read. Calling the Blueprint HasEscaped getter from the
        // AI Tick path caused a game-thread slowdown even at a 250-ms cadence.
        UObject* actorObject = reinterpret_cast<UObject*>(actor);
        if (ReadCachedReflectedBool(
                actorObject,
                "bHasEscaped",
                g_CounselorEscapedPropertyOffset,
                g_CounselorEscapedByteOffset,
                g_CounselorEscapedFieldMask))
        {
            return true;
        }

        // Both the pawn and its PlayerState carry an escape flag. Check both
        // because the pawn flag can lag by a frame during vehicle exits.
        if (g_CounselorEscapedPropertyOffset < 0)
        {
            uint8_t* pawnEscaped = reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(actor) + 0x14ED);
            if (Memory::IsReadable(pawnEscaped, 1) && *pawnEscaped != 0)
                return true;
        }

        UObject** playerState = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(actor) + 0x388);
        if (!Memory::IsReadable(playerState, sizeof(UObject*)) ||
            !*playerState ||
            !Memory::IsReadable(*playerState, 0x6E8))
        {
            return false;
        }

        if (ReadCachedReflectedBool(
                *playerState,
                "bEscaped",
                g_PlayerStateEscapedPropertyOffset,
                g_PlayerStateEscapedByteOffset,
                g_PlayerStateEscapedFieldMask))
        {
            return true;
        }

        if (g_PlayerStateEscapedPropertyOffset >= 0)
            return false;
        uint8_t* stateEscaped = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(*playerState) + 0x6E7);
        return Memory::IsReadable(stateEscaped, 1) && *stateEscaped != 0;
    }

    void RemoveCounselorTarget(AActor* target)
    {
        if (!target)
            return;

        const int32_t index = FindJasonAITargetIndex(target);
        if (index >= 0)
        {
            for (int32_t i = index; i + 1 < g_JasonAITargetCount; ++i)
            {
                g_JasonAITargets[i] = g_JasonAITargets[i + 1];
                g_JasonAITargetBlockedUntil[i] =
                    g_JasonAITargetBlockedUntil[i + 1];
                g_JasonAITargetBlockStrikes[i] =
                    g_JasonAITargetBlockStrikes[i + 1];
            }
            if (g_JasonAITargetCount > 0)
                --g_JasonAITargetCount;
            g_JasonAITargets[g_JasonAITargetCount] = nullptr;
            g_JasonAITargetBlockedUntil[g_JasonAITargetCount] = 0;
            g_JasonAITargetBlockStrikes[g_JasonAITargetCount] = 0;
        }

        if (g_JasonAIState.Target == target)
            g_JasonAIState.Target = nullptr;
        if (g_TrapPriorityVictim == target)
        {
            g_TrapPriorityVictim = nullptr;
            g_TrapPriorityUntil = 0;
            g_TrapPriorityMorphCompleted = false;
        }
    }

    void RefreshLocalCounselorTarget(ULONGLONG now)
    {
        if (now < g_NextLocalCounselorRefreshAt)
            return;
        g_NextLocalCounselorRefreshAt = now + 250;

        constexpr uintptr_t Offset_ControllerPawn = 0x370;
        if (!g_LocalPlayerController ||
            !Memory::IsReadable(g_LocalPlayerController, sizeof(UObject)))
        {
            return;
        }

        AActor** pawnField = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(g_LocalPlayerController) +
            Offset_ControllerPawn);
        if (!Memory::IsReadable(pawnField, sizeof(AActor*)))
            return;

        AActor* possessed = *pawnField;
        AActor* previous = g_LocalCounselorTarget;
        const bool previousEscaped = previous && HasCounselorEscaped(previous);
        if (previous &&
            (previousEscaped || possessed != previous ||
             !IsValidatedLiveCounselorPawn(previous)))
        {
            RemoveCounselorTarget(previous);
            g_LocalCounselorTarget = nullptr;
            g_HumanPursuitStartedAt = 0;
            Logger::Success(
                std::string("18L-AQ local counselor retired from Jason targets | reason=") +
                (previousEscaped ? "escaped" : "possession-changed"));
        }

        if (possessed &&
            possessed != g_LocalCounselorTarget &&
            IsValidatedLiveCounselorPawn(possessed))
        {
            g_LocalCounselorTarget = possessed;
            RegisterJasonAITarget(possessed);
            g_NextKillTeamTickAt = 0;
            Logger::Success(
                "18L-AQ local counselor possession refreshed | pawn=" +
                JasonAISafeName(reinterpret_cast<UObject*>(possessed)));
        }
    }

    bool IsValidatedLiveCounselorPawn(AActor* actor)
    {
        if (!actor ||
            !Memory::IsReadable(actor, sizeof(UObject)) ||
            !JasonAIObjectDerivesFromNameContaining(
                reinterpret_cast<UObject*>(actor),
                "SCCounselorCharacter"))
        {
            return false;
        }

        // Require a live pawn/controller pair in both directions.  The old
        // name-only scan admitted counselor spawn actors, preview actors, and
        // AI-controller objects into the fixed eight-entry target registry.
        constexpr uintptr_t Offset_PawnController = 0x3A0;
        constexpr uintptr_t Offset_ControllerPawn = 0x370;
        constexpr uintptr_t Offset_CounselorDead = 0x1031;

        float* health = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(actor) +
            Offsets::ASCCharacter_Health);
        if (!Memory::IsReadable(health, sizeof(float)) ||
            !std::isfinite(*health) || *health <= 0.0f)
        {
            return false;
        }

        UObject** controllerField = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(actor) + Offset_PawnController);
        uint8_t* deadField = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(actor) + Offset_CounselorDead);
        if (!Memory::IsReadable(controllerField, sizeof(UObject*)) ||
            !*controllerField ||
            !Memory::IsReadable(*controllerField, sizeof(UObject)) ||
            !Memory::IsReadable(deadField, 1) ||
            *deadField != 0 ||
            HasCounselorEscaped(actor))
        {
            return false;
        }

        AActor** possessedPawnField = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(*controllerField) +
            Offset_ControllerPawn);
        return Memory::IsReadable(possessedPawnField, sizeof(AActor*)) &&
            *possessedPawnField == actor;
    }

    void RetireDeadAICounselorFromSpectating(AActor* actor)
    {
        if (!actor ||
            !Memory::IsReadable(actor, 0x1032) ||
            !JasonAIObjectDerivesFromNameContaining(
                reinterpret_cast<UObject*>(actor),
                "SCCounselorCharacter"))
        {
            return;
        }

        float* health = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(actor) +
            Offsets::ASCCharacter_Health);
        uint8_t* dead = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(actor) + 0x1031);
        const bool healthDead =
            Memory::IsReadable(health, sizeof(float)) &&
            std::isfinite(*health) && *health <= 0.0f;
        const bool stateDead = Memory::IsReadable(dead, 1) && *dead != 0;
        if (!healthDead && !stateDead)
            return;

        UObject** controllerField = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(actor) + 0x3A0);
        if (!Memory::IsReadable(controllerField, sizeof(UObject*)) ||
            !*controllerField ||
            *controllerField == g_LocalPlayerController ||
            !Memory::IsReadable(*controllerField, sizeof(UObject)) ||
            !(*controllerField)->Class)
        {
            return;
        }

        UObject* controller = *controllerField;
        UFunction* unpossess = FindFunctionInHierarchyByName(
            controller->Class,
            "UnPossess");
        const bool retired = unpossess && SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(controller),
            controller,
            unpossess,
            nullptr);
        if (retired)
        {
            Logger::Success(
                "18L-BG dead AI counselor unpossessed and removed from spectator targets | pawn=" +
                JasonAISafeName(reinterpret_cast<UObject*>(actor)));
        }
    }

    void TrackCounselorController(AActor* counselor)
    {
        if (!counselor ||
            !Memory::IsReadable(counselor, 0x3A8))
        {
            return;
        }

        UObject** controllerField = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(counselor) + 0x3A0);
        if (!Memory::IsReadable(controllerField, sizeof(UObject*)) ||
            !*controllerField ||
            !Memory::IsReadable(*controllerField, sizeof(UObject)))
        {
            return;
        }

        for (int32_t i = 0; i < g_CounselorControllerCount; ++i)
        {
            if (g_CounselorControllers[i] == *controllerField)
                return;
        }
        if (g_CounselorControllerCount < JasonAITargetCapacity)
            g_CounselorControllers[g_CounselorControllerCount++] =
                *controllerField;
    }

    void RegisterRespawnedCounselorsFromKnownControllers()
    {
        constexpr uintptr_t Offset_ControllerPawn = 0x370;
        for (int32_t i = 0; i < g_CounselorControllerCount; ++i)
        {
            UObject* controller = g_CounselorControllers[i];
            if (!controller ||
                !Memory::IsReadable(controller, sizeof(UObject)))
            {
                continue;
            }

            AActor** pawnField = reinterpret_cast<AActor**>(
                reinterpret_cast<uintptr_t>(controller) +
                Offset_ControllerPawn);
            if (!Memory::IsReadable(pawnField, sizeof(AActor*)) ||
                !IsValidatedLiveCounselorPawn(*pawnField))
            {
                continue;
            }

            // Returned counselors, including Tommy, use the same ordinary
            // target/flee behavior.  There is no protected Tommy role in the
            // proximity-only final-kill design.
            RegisterJasonAITarget(*pawnField);
        }
    }

    void PruneAndRefreshCounselorTargets(UWorld* world, ULONGLONG now)
    {
        // Pruning the fixed eight-entry registry is cheap and keeps dead,
        // escaped and unpossessed pawns out of combat immediately. The costly
        // streamed-level actor discovery is independently limited to once per
        // minute so it cannot cause a hitch every ten seconds.
        if (now >= g_NextCounselorTargetPruneAt)
        {
            g_NextCounselorTargetPruneAt = now + 1000;
            for (int32_t i = 0;
                i < g_JasonAITargetCount && i < JasonAITargetCapacity;
                ++i)
            {
                TrackCounselorController(g_JasonAITargets[i]);
            }

            int32_t writeIndex = 0;
            for (int32_t readIndex = 0;
                readIndex < g_JasonAITargetCount &&
                    readIndex < JasonAITargetCapacity;
                ++readIndex)
            {
                AActor* target = g_JasonAITargets[readIndex];
                if (!IsValidatedLiveCounselorPawn(target))
                {
                    if (IsNativeCounselorDeathConfirmed(target))
                    {
                        for (PendingDeadCounselorRetire& pending :
                             g_PendingDeadCounselorRetires)
                        {
                            if (pending.Pawn == target)
                                break;
                            if (!pending.Pawn)
                            {
                                pending.Pawn = target;
                                pending.ReadyAt = now + 15000;
                                break;
                            }
                        }
                    }
                    if (g_JasonAIState.Target == target)
                        g_JasonAIState.Target = nullptr;
                    if (g_LocalCounselorTarget == target)
                        g_LocalCounselorTarget = nullptr;
                    if (g_TrapPriorityVictim == target)
                    {
                        g_TrapPriorityVictim = nullptr;
                        g_TrapPriorityUntil = 0;
                        g_TrapPriorityMorphCompleted = false;
                    }
                    continue;
                }

                if (writeIndex != readIndex)
                {
                    g_JasonAITargets[writeIndex] = target;
                    g_JasonAITargetBlockedUntil[writeIndex] =
                        g_JasonAITargetBlockedUntil[readIndex];
                    g_JasonAITargetBlockStrikes[writeIndex] =
                        g_JasonAITargetBlockStrikes[readIndex];
                }
                ++writeIndex;
            }

            for (int32_t i = writeIndex;
                i < JasonAITargetCapacity;
                ++i)
            {
                g_JasonAITargets[i] = nullptr;
                g_JasonAITargetBlockedUntil[i] = 0;
                g_JasonAITargetBlockStrikes[i] = 0;
            }
            g_JasonAITargetCount = writeIndex;
            RegisterRespawnedCounselorsFromKnownControllers();

            const bool matchHasLiveCounselor =
                g_JasonAITargetCount > 0 ||
                IsValidatedLiveCounselorPawn(g_LocalCounselorTarget);
            bool hasPendingDeath = false;
            for (const PendingDeadCounselorRetire& pending :
                 g_PendingDeadCounselorRetires)
                hasPendingDeath = hasPendingDeath || pending.Pawn != nullptr;
            if (!matchHasLiveCounselor && hasPendingDeath)
            {
                // The native kill/game-mode path must retain the final
                // victim's controller and camera until it has shown the
                // death and Jason-return ending. Stop our spectator RPCs;
                // stock will take over the end-of-match view.
                for (PendingDeadCounselorRetire& pending :
                     g_PendingDeadCounselorRetires)
                    pending = PendingDeadCounselorRetire{};
                if (!g_FinalCounselorDeathObserved)
                {
                    g_FinalCounselorDeathObserved = true;
                    g_JasonSpectatorForced = false;
                    g_JasonSpectatorPreviousTarget = nullptr;
                    g_JasonSpectatorPreviousPlayerState = nullptr;
                    g_NextJasonSpectatorRepairAt = 0;
                    g_NextJasonSpectatorPlayerStateRepairAt = 0;
                    g_JasonSpectatorOrbitInitialized = false;
                    g_InsertJasonOnNextSpectatorCycle = false;
                    Logger::Success(
                        "18L-BN final counselor death: native ending owns controller and spectator camera");
                }
            }
            else
            {
                for (PendingDeadCounselorRetire& pending :
                     g_PendingDeadCounselorRetires)
                {
                    if (!pending.Pawn || now < pending.ReadyAt ||
                        pending.Pawn == g_LastHeldCounselor)
                        continue;
                    RetireDeadAICounselorFromSpectating(pending.Pawn);
                    pending = PendingDeadCounselorRetire{};
                }
            }
        }

        if (now < g_NextCounselorRosterRefreshAt)
            return;
        g_NextCounselorRosterRefreshAt = now + 60000;

        // The initial roster and its controller set are authoritative. New
        // Tommy possession is detected through those controllers, so a full
        // streamed-level fallback scan is unnecessary during normal matches.
        if (g_CounselorControllerCount > 0)
            return;

        if (!world ||
            !Memory::IsReadable(world, sizeof(UWorld)))
        {
            return;
        }

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return;
        }

        if (g_CounselorRosterRefreshLevel >= levels->Count)
            g_CounselorRosterRefreshLevel = 0;
        ULevel* level = levels->Data[g_CounselorRosterRefreshLevel++];
        if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
            return;

        TArray<AActor*>& actors = level->Actors;
        if (!actors.Data ||
            actors.Count <= 0 ||
            actors.Count > 100000 ||
            !Memory::IsReadable(
                actors.Data,
                sizeof(AActor*) * static_cast<size_t>(actors.Count)))
        {
            return;
        }

        const int32_t countBefore = g_JasonAITargetCount;
        for (int32_t i = 0;
            i < actors.Count &&
                g_JasonAITargetCount < JasonAITargetCapacity;
            ++i)
        {
            if (IsValidatedLiveCounselorPawn(actors.Data[i]))
                RegisterJasonAITarget(actors.Data[i]);
        }

        if (g_JasonAITargetCount != countBefore)
        {
            Logger::Success(
                "18L-AI counselor bridge: live roster refreshed | targets=" +
                std::to_string(g_JasonAITargetCount));
        }
    }

    void RegisterLiveCounselorTargets(UWorld* world)
    {
        if (!world || !Memory::IsReadable(world, sizeof(UWorld)))
            return;

        constexpr uintptr_t Offset_Levels = 0x110;
        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + Offset_Levels);

        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return;
        }

        for (int32_t levelIndex = 0;
            levelIndex < levels->Count;
            ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;

            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data ||
                actors.Count <= 0 ||
                actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t actorIndex = 0;
                actorIndex < actors.Count;
                ++actorIndex)
            {
                AActor* actor = actors.Data[actorIndex];
                if (IsValidatedLiveCounselorPawn(actor))
                {
                    RegisterJasonAITarget(actor);
                }
            }
        }

        Logger::Success(
            "18L-AI counselor bridge: validated live counselor target roster=" +
            std::to_string(g_JasonAITargetCount));
        for (int32_t i = 0; i < g_JasonAITargetCount; ++i)
        {
            Logger::Debug(
                "18L-AI counselor target[" + std::to_string(i) + "]=" +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(g_JasonAITargets[i])));
        }
    }

    bool IsActorInWorld(UWorld* world, AActor* wanted)
    {
        if (!world ||
            !wanted ||
            !Memory::IsReadable(world, sizeof(UWorld)) ||
            !Memory::IsReadable(wanted, sizeof(UObject)))
        {
            return false;
        }

        constexpr uintptr_t Offset_Levels = 0x110;
        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + Offset_Levels);

        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return false;
        }

        for (int32_t levelIndex = 0; levelIndex < levels->Count; ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;

            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data ||
                actors.Count <= 0 ||
                actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t actorIndex = 0;
                actorIndex < actors.Count;
                ++actorIndex)
            {
                if (actors.Data[actorIndex] == wanted)
                    return true;
            }
        }

        return false;
    }

    AActor* ReadActorField(UObject* owner, uintptr_t offset)
    {
        if (!owner || !Memory::IsReadable(owner, sizeof(UObject)))
            return nullptr;

        AActor** field = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(owner) + offset);

        if (!Memory::IsReadable(field, sizeof(AActor*)))
            return nullptr;

        AActor* actor = *field;
        return actor && Memory::IsReadable(actor, sizeof(UObject))
            ? actor
            : nullptr;
    }

    AActor* GetActorOwnerSafe(AActor* actor)
    {
        if (!actor || !actor->Class ||
            !Memory::IsReadable(actor, sizeof(UObject)))
        {
            return nullptr;
        }

        UFunction* getOwner = FindFunctionInHierarchyByName(
            actor->Class,
            "GetOwner");
        if (!getOwner)
            return nullptr;

        struct Params { AActor* ReturnValue; };
        Params params{};
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(actor),
                actor,
                getOwner,
                &params))
        {
            return nullptr;
        }
        return params.ReturnValue;
    }

    AActor* GetAttachParentActorSafe(AActor* actor)
    {
        if (!actor || !actor->Class ||
            !Memory::IsReadable(actor, sizeof(UObject)))
        {
            return nullptr;
        }

        UFunction* getAttachParentActor = FindFunctionInHierarchyByName(
            actor->Class,
            "GetAttachParentActor");
        if (!getAttachParentActor)
            return nullptr;

        struct Params { AActor* ReturnValue; };
        Params params{};
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(actor),
                actor,
                getAttachParentActor,
                &params))
        {
            return nullptr;
        }
        return params.ReturnValue;
    }

    bool DestroyWorldActorSafe(AActor* actor)
    {
        if (!actor || !actor->Class ||
            !Memory::IsReadable(actor, sizeof(UObject)))
        {
            return false;
        }

        UFunction* destroy = FindFunctionInHierarchyByName(
            actor->Class,
            "K2_DestroyActor");
        return destroy && SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(actor),
            actor,
            destroy,
            nullptr);
    }

    bool SpawnUsefulOfflinePickup(const FVector& location)
    {
        UWorld* world = g_JasonAIState.World;
        UClass* itemClass = nullptr;

        // Tiny per-world xorshift generator: enough to vary the three useful
        // pickup classes without CRT rand state, allocation, or a recurring
        // scan. A missing class simply falls through to the next choice.
        uint32_t randomState = g_LootReplacementRandomState;
        if (randomState == 0)
        {
            randomState = static_cast<uint32_t>(GetTickCount64()) ^
                static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(world) >> 4);
            if (randomState == 0)
                randomState = 0x9E3779B9u;
        }
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;
        g_LootReplacementRandomState = randomState;
        const int32_t randomStart = static_cast<int32_t>(randomState % 3u);
        for (int32_t attempt = 0; attempt < 3; ++attempt)
        {
            const int32_t index = (randomStart + attempt) % 3;
            if (g_LootReplacementClasses[index] &&
                Memory::IsReadable(
                    g_LootReplacementClasses[index],
                    sizeof(UClass)))
            {
                itemClass = g_LootReplacementClasses[index];
                break;
            }
        }
        if (!world || !itemClass)
            return false;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        using SpawnParamsCtorFn = void* (__fastcall*)(void*);
        using SpawnActorFn = AActor* (__fastcall*)(
            UWorld*, UClass*, const FVector*, const FRotator*, const void*);
        SpawnParamsCtorFn construct = reinterpret_cast<SpawnParamsCtorFn>(
            reinterpret_cast<uintptr_t>(module) + 0x017F1160);
        SpawnActorFn spawn = reinterpret_cast<SpawnActorFn>(
            reinterpret_cast<uintptr_t>(module) + 0x01519D10);
        if (!Memory::IsReadable(reinterpret_cast<void*>(construct), 1) ||
            !Memory::IsReadable(reinterpret_cast<void*>(spawn), 1))
        {
            return false;
        }

        alignas(16) uint8_t parameters[0x30]{};
        construct(parameters);
        parameters[0x28] = 2; // AlwaysSpawn; distribution points are known-safe.
        FRotator rotation{};
        AActor* spawned = spawn(
            world,
            itemClass,
            &location,
            &rotation,
            parameters);
        if (spawned)
        {
            ++g_UsefulPickupsSpawned;
            return true;
        }
        return false;
    }

    void CleanupCounselorRouteWalkieTalkies(ULONGLONG now)
    {
        UWorld* world = g_JasonAIState.World;
        if (!world ||
            !Memory::IsReadable(world, sizeof(UWorld)) ||
            g_LootCleanupComplete ||
            now < g_NextWalkieCleanupAt)
        {
            return;
        }

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return;
        }

        if (g_WalkieCleanupLevel >= levels->Count)
        {
            g_WalkieCleanupLevel = 0;
            g_WalkieCleanupActor = 0;
            if (!g_LootCleanupPassActive)
            {
                g_LootCleanupPassActive = true;
                g_LootKeysKeptThisPass = 0;
                g_LootFusesKeptThisPass = 0;
                g_NextWalkieCleanupAt = now + 250;
                Logger::Success(
                    "18L-AI offline inventory census complete | cars=" +
                    std::to_string(g_LootCensusCarCount) +
                    " | boat=" + std::to_string(g_LootCensusSawBoat ? 1 : 0) +
                    " | reservedKeys=" +
                    std::to_string(g_LootNonLooseKeyCount) +
                    " | reservedFuses=" +
                    std::to_string(g_LootNonLooseFuseCount) +
                    " | replacements=" +
                    std::to_string(g_LootReplacementClasses[0] ? 1 : 0) + "," +
                    std::to_string(g_LootReplacementClasses[1] ? 1 : 0) + "," +
                    std::to_string(g_LootReplacementClasses[2] ? 1 : 0));
            }
            else
            {
                g_LootCleanupPassActive = false;
                g_LootCleanupComplete = true;
                g_NextWalkieCleanupAt = ~0ULL;
                Logger::Success(
                    "18L-AI offline inventory cleanup complete; recurring world scans retired");
            }
            return;
        }
        // Spread startup census/cleanup over small fixed actor slices. The old
        // code ancestry-walked and classified a complete streamed level on one
        // frame, causing the early-match jerk the player observed.
        g_NextWalkieCleanupAt = now + 25;

        ULevel* level = levels->Data[g_WalkieCleanupLevel];
        if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
        {
            ++g_WalkieCleanupLevel;
            g_WalkieCleanupActor = 0;
            return;
        }

        TArray<AActor*>& actors = level->Actors;
        if (!actors.Data ||
            actors.Count <= 0 ||
            actors.Count > 100000 ||
            !Memory::IsReadable(
                actors.Data,
                sizeof(AActor*) * static_cast<size_t>(actors.Count)))
        {
            ++g_WalkieCleanupLevel;
            g_WalkieCleanupActor = 0;
            return;
        }

        if (g_WalkieCleanupActor >= actors.Count)
        {
            ++g_WalkieCleanupLevel;
            g_WalkieCleanupActor = 0;
            return;
        }
        const int32_t actorStart = g_WalkieCleanupActor;
        const int32_t actorEnd = (std::min)(
            actors.Count,
            actorStart + 16);

        if (!g_LootCleanupPassActive)
        {
            for (int32_t i = actorStart; i < actorEnd; ++i)
            {
                AActor* actor = actors.Data[i];
                if (!actor || !actor->Class ||
                    !Memory::IsReadable(actor, sizeof(UObject)))
                {
                    continue;
                }

                if (ObjectClassDerivesFromExact(
                        reinterpret_cast<UObject*>(actor),
                        "SCDriveableBoat"))
                {
                    g_LootCensusSawBoat = true;
                }

                uint8_t carKind = 0;
                int32_t seatCount = 0;
                if (ClassifyRepairableCar(actor, carKind, seatCount))
                    ++g_LootCensusCarCount;

                const bool isKey = ObjectClassDerivesFromExact(
                    reinterpret_cast<UObject*>(actor),
                    "CarKeys_C");
                const bool isFuse = ObjectClassDerivesFromExact(
                    reinterpret_cast<UObject*>(actor),
                    "PhoneBoxFuse_C");
                if (isKey || isFuse)
                {
                    AActor* owner = GetActorOwnerSafe(actor);
                    bool nonLoose = owner || GetAttachParentActorSafe(actor);
                    if (!nonLoose)
                    {
                        UObject* rootComponent = ReadReflectedObjectProperty(
                            reinterpret_cast<UObject*>(actor),
                            "RootComponent");
                        nonLoose = rootComponent && ReadReflectedObjectProperty(
                            rootComponent,
                            "AttachParent");
                    }
                    if (nonLoose)
                    {
                        if (isKey)
                            ++g_LootNonLooseKeyCount;
                        else
                            ++g_LootNonLooseFuseCount;
                    }
                }

                // Reuse the existing 16-actor startup census to remember a
                // guaranteed melee class. Sweater convergence can then equip
                // bots without ever adding a synchronous world scan.
                if (!g_CounselorConvergenceMeleeClass &&
                    ObjectClassDerivesFromExact(
                        reinterpret_cast<UObject*>(actor),
                        "CounselorTwoHandedAxe_C"))
                {
                    g_CounselorConvergenceMeleeClass = actor->Class;
                }
                if (!g_CounselorConvergenceMacheteClass &&
                    ObjectClassDerivesFromExact(
                        reinterpret_cast<UObject*>(actor),
                        "CounselorMachete_C"))
                {
                    g_CounselorConvergenceMacheteClass = actor->Class;
                }

                const char* replacementNames[] =
                {
                    "BP_PocketKnife_C",
                    "BP_FirstAid_C",
                    "BP_Firecracker_C"
                };
                for (int32_t replacementIndex = 0;
                    replacementIndex < 3;
                    ++replacementIndex)
                {
                    if (!g_LootReplacementClasses[replacementIndex] &&
                        ObjectClassDerivesFromExact(
                            reinterpret_cast<UObject*>(actor),
                            replacementNames[replacementIndex]))
                    {
                        g_LootReplacementClasses[replacementIndex] =
                            actor->Class;
                    }
                }
            }
            g_WalkieCleanupActor = actorEnd;
            if (g_WalkieCleanupActor >= actors.Count)
            {
                ++g_WalkieCleanupLevel;
                g_WalkieCleanupActor = 0;
            }
            return;
        }

        enum class CleanupKind : uint8_t
        {
            Walkie,
            Tape,
            InvalidPropeller,
            SurplusKey,
            SurplusFuse
        };
        struct CleanupTarget
        {
            AActor* Actor;
            CleanupKind Kind;
        };

        // Collect first: K2_DestroyActor mutates the level actor array.
        CleanupTarget targets[128]{};
        int32_t targetCount = 0;
        for (int32_t i = actorStart; i < actorEnd && targetCount < 128; ++i)
        {
            AActor* actor = actors.Data[i];
            if (!actor || !actor->Class ||
                !Memory::IsReadable(actor, sizeof(UObject)))
            {
                continue;
            }

            CleanupKind kind{};
            bool remove = false;
            const std::string itemClassName = JasonAISafeName(
                reinterpret_cast<UObject*>(actor->Class));
            if (ObjectClassDerivesFromExact(
                    reinterpret_cast<UObject*>(actor),
                    "WalkieTalkie_C"))
            {
                kind = CleanupKind::Walkie;
                remove = true;
            }
            else if (ObjectClassDerivesFromExact(
                         reinterpret_cast<UObject*>(actor),
                         "PamelaTape_C") ||
                     ObjectClassDerivesFromExact(
                         reinterpret_cast<UObject*>(actor),
                         "TommyTape_C") ||
                     itemClassName.find("Pamela_Tape") != std::string::npos ||
                     itemClassName.find("Tommy_Tape") != std::string::npos ||
                     itemClassName.find("Tape_C") != std::string::npos)
            {
                kind = CleanupKind::Tape;
                remove = true;
            }
            else if (!g_LootCensusSawBoat &&
                     ObjectClassDerivesFromExact(
                         reinterpret_cast<UObject*>(actor),
                         "BoatPropeller_C"))
            {
                kind = CleanupKind::InvalidPropeller;
                remove = true;
            }
            else if (ObjectClassDerivesFromExact(
                          reinterpret_cast<UObject*>(actor),
                          "CarKeys_C"))
            {
                kind = CleanupKind::SurplusKey;
                remove = true;
            }
            else if (ObjectClassDerivesFromExact(
                         reinterpret_cast<UObject*>(actor),
                         "PhoneBoxFuse_C"))
            {
                kind = CleanupKind::SurplusFuse;
                remove = true;
            }

            if (!remove)
                continue;

            // Resolve ownership only for the handful of candidate pickups.
            // Calling reflected GetOwner for every world actor caused the
            // severe repeating render stall in the previous build.
            AActor* itemOwner = GetActorOwnerSafe(actor);
            const bool carriedByCounselor =
                itemOwner && IsValidatedLiveCounselorPawn(itemOwner);
            if (carriedByCounselor)
                continue;

            // A drawer/cabinet retains a native reference to its hidden loot.
            // Destroying that actor during startup leaves a dangling payload
            // and can stall the game thread when the furniture opens. Replace
            // only genuinely loose pickups; owned or attached items remain on
            // the stock container lifecycle.
            if (itemOwner || GetAttachParentActorSafe(actor))
                continue;
            UObject* rootComponent = ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(actor),
                "RootComponent");
            if (rootComponent && ReadReflectedObjectProperty(
                    rootComponent,
                    "AttachParent"))
            {
                continue;
            }

            if (kind == CleanupKind::SurplusKey &&
                g_LootKeysKeptThisPass <
                    (std::max)(0,
                        g_LootCensusCarCount - g_LootNonLooseKeyCount))
            {
                ++g_LootKeysKeptThisPass;
                continue;
            }
            if (kind == CleanupKind::SurplusFuse &&
                g_LootFusesKeptThisPass <
                    (std::max)(0, 1 - g_LootNonLooseFuseCount))
            {
                ++g_LootFusesKeptThisPass;
                continue;
            }

            targets[targetCount++] = { actor, kind };
        }

        int32_t removedThisLevel = 0;
        for (int32_t i = 0; i < targetCount; ++i)
        {
            FVector location{};
            const bool haveLocation = GetJasonAIActorLocation(
                targets[i].Actor,
                location);
            if (!DestroyWorldActorSafe(targets[i].Actor))
                continue;

            ++removedThisLevel;
            switch (targets[i].Kind)
            {
            case CleanupKind::Walkie: ++g_WalkiesRemoved; break;
            case CleanupKind::Tape: ++g_TapesRemoved; break;
            case CleanupKind::InvalidPropeller:
                ++g_InvalidPropellersRemoved;
                break;
            case CleanupKind::SurplusKey: ++g_SurplusKeysRemoved; break;
            case CleanupKind::SurplusFuse: ++g_SurplusFusesRemoved; break;
            }
            if (haveLocation)
                SpawnUsefulOfflinePickup(location);
        }

        if (removedThisLevel > 0)
        {
            Logger::Success(
                "18L-AI offline inventory cleanup | thisLevel=" +
                std::to_string(removedThisLevel) +
                " | walkies=" + std::to_string(g_WalkiesRemoved) +
                " | tapes=" + std::to_string(g_TapesRemoved) +
                " | invalidPropellers=" +
                std::to_string(g_InvalidPropellersRemoved) +
                " | surplusKeys=" + std::to_string(g_SurplusKeysRemoved) +
                " | surplusFuses=" + std::to_string(g_SurplusFusesRemoved) +
                " | usefulReplacements=" +
                std::to_string(g_UsefulPickupsSpawned));
        }
        g_WalkieCleanupActor = actorEnd;
        if (g_WalkieCleanupActor >= actors.Count)
        {
            ++g_WalkieCleanupLevel;
            g_WalkieCleanupActor = 0;
        }
    }

    bool CallReflectedBoolGetter(UObject* object, const char* functionName)
    {
        if (!object || !functionName || !object->Class ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }

        UFunction* function = FindFunctionInHierarchyByName(
            object->Class,
            functionName);
        if (!function)
            return false;

        struct Params { bool ReturnValue; };
        Params params{};
        return SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(object),
            object,
            function,
            &params) && params.ReturnValue;
    }

    bool ReadReflectedBoolByte(UObject* object, const char* propertyName)
    {
        if (!object || !propertyName || !object->Class ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }
        UPropertyLite* property = FindPropertyInHierarchyByName(
            object->Class,
            propertyName);
        if (!property ||
            property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000)
        {
            return false;
        }
        uintptr_t valueOffset = static_cast<uintptr_t>(property->Offset_Internal);
        uint8_t fieldMask = 0xFF;
        const std::string propertyType = JasonAISafeName(
            reinterpret_cast<UObject*>(property->ClassPrivate));
        if (propertyType == "BoolProperty")
        {
            // UE4 UBoolProperty appends FieldSize, ByteOffset, ByteMask and
            // FieldMask at +0x70. Honor its byte offset/mask instead of
            // treating unrelated packed flags in the same byte as true.
            uint8_t* boolLayout = reinterpret_cast<uint8_t*>(property) + 0x70;
            if (Memory::IsReadable(boolLayout, 4))
            {
                valueOffset += boolLayout[1];
                fieldMask = boolLayout[3];
                if (fieldMask == 0)
                    fieldMask = boolLayout[2];
            }
        }

        uint8_t* value = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(object) + valueOffset);
        return Memory::IsReadable(value, 1) &&
            fieldMask != 0 &&
            (*value & fieldMask) != 0;
    }

    bool WriteReflectedBoolByte(
        UObject* object,
        const char* propertyName,
        bool enabled)
    {
        if (!object || !propertyName || !object->Class ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }

        UPropertyLite* property = FindPropertyInHierarchyByName(
            object->Class,
            propertyName);
        if (!property ||
            property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000)
        {
            return false;
        }

        uintptr_t valueOffset = static_cast<uintptr_t>(property->Offset_Internal);
        uint8_t fieldMask = 0xFF;
        const std::string propertyType = JasonAISafeName(
            reinterpret_cast<UObject*>(property->ClassPrivate));
        if (propertyType == "BoolProperty")
        {
            uint8_t* boolLayout = reinterpret_cast<uint8_t*>(property) + 0x70;
            if (Memory::IsReadable(boolLayout, 4))
            {
                valueOffset += boolLayout[1];
                fieldMask = boolLayout[3];
                if (fieldMask == 0)
                    fieldMask = boolLayout[2];
            }
        }

        uint8_t* value = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(object) + valueOffset);
        if (!Memory::IsReadable(value, 1) || fieldMask == 0)
            return false;

        if (enabled)
            *value |= fieldMask;
        else
            *value &= static_cast<uint8_t>(~fieldMask);
        return true;
    }

    bool WriteReflectedFloatValue(
        UObject* object,
        const char* propertyName,
        float value)
    {
        if (!object || !propertyName || !object->Class ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }
        UPropertyLite* property = FindPropertyInHierarchyByName(
            object->Class,
            propertyName);
        if (!property || property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000 ||
            property->ElementSize != sizeof(float) ||
            JasonAISafeName(
                reinterpret_cast<UObject*>(property->ClassPrivate)) !=
                "FloatProperty")
        {
            return false;
        }
        float* field = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(object) +
            property->Offset_Internal);
        if (!Memory::IsReadable(field, sizeof(float)))
            return false;
        *field = value;
        return true;
    }

    bool WriteReflectedObjectValue(
        UObject* object,
        const char* propertyName,
        UObject* value)
    {
        if (!object || !propertyName || !object->Class ||
            !Memory::IsReadable(object, sizeof(UObject)) ||
            (value && !Memory::IsReadable(value, sizeof(UObject))))
        {
            return false;
        }
        UPropertyLite* property = FindPropertyInHierarchyByName(
            object->Class,
            propertyName);
        if (!property || property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000 ||
            property->ElementSize != sizeof(UObject*) ||
            JasonAISafeName(
                reinterpret_cast<UObject*>(property->ClassPrivate)) !=
                "ObjectProperty")
        {
            return false;
        }
        UObject** field = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(object) +
            property->Offset_Internal);
        if (!Memory::IsReadable(field, sizeof(UObject*)))
            return false;
        *field = value;
        return true;
    }

    bool RestoreNativePamelaSweaterContract(
        AActor* counselor,
        UObject* ability,
        UObject* controller)
    {
        if (!counselor || !ability ||
            !Memory::IsReadable(counselor, 0x18D0) ||
            !Memory::IsReadable(ability, 0xA8))
        {
            return false;
        }

        // Shipping UAbility::CanUse (RVA 0x3B9AB0) and Activate
        // (RVA 0x3B6570) both reject the ability while byte +0x98 is set.
        // Stock activation sets it, but sweater removal does not clear it.
        // This function runs only after the completed ten-second lifecycle,
        // so clear that proven one-shot gate before republishing the ability.
        *reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(ability) + 0x98) = 0;

        // Shipping ASCCounselorCharacter::GivePamelasSweater
        // (RVA 0x3E16A0) establishes this exact two-way relationship:
        //
        //   counselor + 0x18C8 = sweater ability
        //   ability   + 0x00A0 = owning counselor
        //
        // ASCCounselorCharacter::RemovePamelasSweater (RVA 0x3EE7C0)
        // clears both values. Re-publishing only the counselor slot leaves a
        // detached, consumed ability and the HUD correctly withholds Y.
        *reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(counselor) + 0x18C8) = ability;
        *reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(ability) + 0xA0) =
                reinterpret_cast<UObject*>(counselor);

        if (!controller ||
            !Memory::IsReadable(controller, sizeof(UObject)))
        {
            return false;
        }

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        constexpr uintptr_t RVA_GetLocalSCPlayerController = 0x00409EA0;
        constexpr uintptr_t RVA_SetSweaterAbilityAvailable = 0x004675D0;
        uint8_t* getLocalTarget = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) +
            RVA_GetLocalSCPlayerController);
        uint8_t* setAvailableTarget = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) +
            RVA_SetSweaterAbilityAvailable);
        const uint8_t expectedGetLocal[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8
        };
        const uint8_t expectedSetAvailable[] = {
            0x48, 0x8B, 0x89, 0xD0, 0x04, 0x00, 0x00,
            0x48, 0x85, 0xC9, 0x0F, 0x85
        };
        if (!Memory::IsReadable(
                getLocalTarget,
                sizeof(expectedGetLocal)) ||
            std::memcmp(
                getLocalTarget,
                expectedGetLocal,
                sizeof(expectedGetLocal)) != 0 ||
            !Memory::IsReadable(
                setAvailableTarget,
                sizeof(expectedSetAvailable)) ||
            std::memcmp(
                setAvailableTarget,
                expectedSetAvailable,
                sizeof(expectedSetAvailable)) != 0)
        {
            return false;
        }

        using GetLocalSCPlayerControllerFn =
            UObject*(__fastcall*)(UObject* controller);
        using SetSweaterAbilityAvailableFn =
            void(__fastcall*)(UObject* localController, bool available);
        UObject* localController = nullptr;
        __try
        {
            localController =
                reinterpret_cast<GetLocalSCPlayerControllerFn>(
                    getLocalTarget)(controller);
            if (!localController ||
                !Memory::IsReadable(localController, sizeof(UObject)))
            {
                return false;
            }
            reinterpret_cast<SetSweaterAbilityAvailableFn>(
                setAvailableTarget)(localController, true);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool IsPamelaSweaterAbilityObject(UObject* object)
    {
        if (!object ||
            !Memory::IsReadable(object, sizeof(UObject)) ||
            !object->Class ||
            !Memory::IsReadable(object->Class, sizeof(UStruct)))
        {
            return false;
        }
        const std::string objectName = JasonAISafeName(object);
        const std::string className = JasonAISafeName(
            reinterpret_cast<UObject*>(object->Class));
        return objectName.find("PamelasSweater") != std::string::npos ||
            className.find("PamelasSweater") != std::string::npos;
    }

    UObject* FindPamelaSweaterAbilityOnOwner(UObject* owner)
    {
        if (!owner || !owner->Class ||
            !Memory::IsReadable(owner, sizeof(UObject)))
        {
            return nullptr;
        }
        const char* propertyNames[] =
        {
            "SweaterAbility",
            "ActiveAbility",
            "AvailableAbility",
            "ActivatedAbility",
            "SelectedAbility",
            "UsedAbility",
            "CounselorActiveAbility"
        };
        for (const char* propertyName : propertyNames)
        {
            UObject* candidate = ReadReflectedObjectProperty(
                owner,
                propertyName);
            if (IsPamelaSweaterAbilityObject(candidate))
                return candidate;
        }
        return nullptr;
    }

    bool ArmUnlimitedSweaterRuleForWorld()
    {
        UWorld* world = g_JasonAIState.World;
        if (!world)
            return false;
        UObject* owners[] =
        {
            reinterpret_cast<UObject*>(ReadActorField(
                reinterpret_cast<UObject*>(world),
                0xF0)),
            reinterpret_cast<UObject*>(ReadActorField(
                reinterpret_cast<UObject*>(world),
                0xF8))
        };
        bool armed = false;
        std::string armedOwner;
        for (UObject* owner : owners)
        {
            if (owner && owner->Class &&
                Memory::IsReadable(owner, sizeof(UObject)) &&
                WriteReflectedBoolByte(
                    owner,
                    "bUnlimitedSweaterStun",
                    true))
            {
                armed = true;
                if (armedOwner.empty())
                {
                    armedOwner = JasonAISafeName(
                        reinterpret_cast<UObject*>(owner->Class));
                }
            }
        }
        if (armed && !g_UnlimitedSweaterWorldRuleLogged)
        {
            g_UnlimitedSweaterWorldRuleLogged = true;
            Logger::Success(
                "18L-BB unlimited Pamela stun rule armed before sweater acquisition | owner=" +
                armedOwner);
        }
        return armed;
    }

    bool IsHunterCounselor(AActor* counselor)
    {
        if (!counselor || !counselor->Class ||
            !Memory::IsReadable(counselor, sizeof(UObject)))
        {
            return false;
        }
        return ObjectClassDerivesFromExact(
                   reinterpret_cast<UObject*>(counselor),
                   "Hunter_Counselor_C") ||
            CallReflectedBoolGetter(
                reinterpret_cast<UObject*>(counselor),
                "GetIsHunter") ||
            ReadReflectedBoolByte(
                reinterpret_cast<UObject*>(counselor),
                "bIsHunter");
    }

    bool HasPamelaSweater(AActor* counselor)
    {
        if (!counselor ||
            !Memory::IsReadable(counselor, 0x1C6B) ||
            !ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(counselor),
                "SCCounselorCharacter"))
        {
            return false;
        }

        // Verified Shipping HasPamelasSweater (RVA 0x497900) returns this
        // native byte directly. Avoid four reflected lookups on every active
        // helper cadence.
        return *reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(counselor) + 0x1C6A) != 0;
    }

    bool RearmPermanentHumanSweater(
        AActor* counselor,
        bool refreshVisual,
        bool* abilityAvailableOut = nullptr)
    {
        if (abilityAvailableOut)
            *abilityAvailableOut = false;
        if (!counselor || !counselor->Class ||
            !Memory::IsReadable(counselor, 0x1C6B))
        {
            return false;
        }

        UObject* counselorObject = reinterpret_cast<UObject*>(counselor);
        *reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(counselor) + 0x1C6A) = 1;
        WriteReflectedBoolByte(
            counselorObject,
            "bHasPamelasSweater",
            true);
        WriteReflectedBoolByte(
            counselorObject,
            "bIsWearingSweater",
            true);
        WriteReflectedBoolByte(
            counselorObject,
            "bWearingSweater",
            true);

        // The visible outfit is server-owned. Rewriting the replicated bytes
        // and manually invoking OnRep was not sufficient after activation:
        // the trace claimed restoration while the player still lost the
        // sweater. Use the stock server setter that owns both state and mesh.
        if (refreshVisual)
        {
            if (UFunction* setWearing = FindFunctionInHierarchyByName(
                    counselorObject->Class,
                    "SERVER_SetWearingSweater"))
            {
                struct SetWearingSweaterParams { bool Wearing; };
                SetWearingSweaterParams params{true};
                SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(counselorObject),
                    counselorObject,
                    setWearing,
                    &params);
            }
        }

        // Resurrected exposes the stock custom-rule switch that permits the
        // Pamela stun more than once. Its owner differs between builds, so
        // resolve it only at acquisition/re-arm time across the few known
        // participants rather than adding a recurring object scan.
        UObject* controller = nullptr;
        UObject** controllerField = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(counselor) + 0x3A0);
        if (Memory::IsReadable(controllerField, sizeof(UObject*)) &&
            *controllerField &&
            Memory::IsReadable(*controllerField, sizeof(UObject)))
        {
            controller = *controllerField;
        }
        UWorld* world = g_JasonAIState.World;
        UObject* gameMode = world
            ? reinterpret_cast<UObject*>(ReadActorField(
                reinterpret_cast<UObject*>(world),
                0xF0))
            : nullptr;
        UObject* gameState = world
            ? reinterpret_cast<UObject*>(ReadActorField(
                reinterpret_cast<UObject*>(world),
                0xF8))
            : nullptr;
        if (!IsPamelaSweaterAbilityObject(
                g_PermanentHumanSweaterAbility))
        {
            g_PermanentHumanSweaterAbility =
                FindPamelaSweaterAbilityOnOwner(counselorObject);
            if (!g_PermanentHumanSweaterAbility && controller)
            {
                g_PermanentHumanSweaterAbility =
                    FindPamelaSweaterAbilityOnOwner(controller);
            }
        }
        UObject* unlimitedOwners[] =
        {
            counselorObject,
            controller,
            gameMode,
            gameState,
            g_PermanentHumanSweaterAbility
        };
        bool unlimitedArmed = false;
        for (UObject* owner : unlimitedOwners)
        {
            if (owner && owner->Class &&
                Memory::IsReadable(owner, sizeof(UObject)) &&
                WriteReflectedBoolByte(
                    owner,
                    "bUnlimitedSweaterStun",
                    true))
            {
                unlimitedArmed = true;
            }
        }

        // Native removal does not destroy or mutate the data-only sweater
        // ability. It detaches the object from both sides and sends a direct
        // client/HUD availability event. Reverse those exact operations after
        // the stock ten-second lifecycle; broad guessed property resets can
        // fight the real lifecycle and are intentionally avoided here.
        bool abilityStateReset = false;
        bool availabilityArmed = false;
        if (refreshVisual &&
            IsPamelaSweaterAbilityObject(
                g_PermanentHumanSweaterAbility))
        {
            UObject* ability = g_PermanentHumanSweaterAbility;
            availabilityArmed = RestoreNativePamelaSweaterContract(
                counselor,
                ability,
                controller);
            if (abilityAvailableOut)
                *abilityAvailableOut = availabilityArmed;
            abilityStateReset =
                *reinterpret_cast<uint8_t*>(
                    reinterpret_cast<uintptr_t>(ability) + 0x98) == 0 &&
                *reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(counselor) + 0x18C8) ==
                    ability &&
                *reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(ability) + 0xA0) ==
                    counselorObject;

            UObject* abilityOwners[] =
            {
                counselorObject,
                controller
            };
            for (UObject* abilityOwner : abilityOwners)
            {
                if (!abilityOwner || !abilityOwner->Class ||
                    !Memory::IsReadable(abilityOwner, sizeof(UObject)))
                {
                    continue;
                }
                if (g_PermanentHumanInnateActiveAbility &&
                    Memory::IsReadable(
                        g_PermanentHumanInnateActiveAbility,
                        sizeof(UObject)) &&
                    !IsPamelaSweaterAbilityObject(
                        g_PermanentHumanInnateActiveAbility))
                {
                    WriteReflectedObjectValue(
                        abilityOwner,
                        "ActiveAbility",
                        g_PermanentHumanInnateActiveAbility);
                }
                WriteReflectedObjectValue(
                    abilityOwner,
                    "UsedAbility",
                    nullptr);
                WriteReflectedObjectValue(
                    abilityOwner,
                    "ActivatedAbility",
                    nullptr);
            }
        }

        if (refreshVisual)
        {
            if (UFunction* wearingRepFunction =
                    FindFunctionInHierarchyByName(
                        counselorObject->Class,
                        "OnRep_WearingSweater"))
            {
                SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(counselorObject),
                    counselorObject,
                    wearingRepFunction,
                    nullptr);
            }
        }

        if (!g_PermanentHumanUnlimitedSweaterArmed)
        {
            g_PermanentHumanUnlimitedSweaterArmed = unlimitedArmed;
            Logger::Success(
                std::string("18L-BA permanent Pamela stun route armed | unlimitedRule=") +
                (unlimitedArmed ? "true" : "false") +
                " | abilityAvailable=" +
                (availabilityArmed ? "true" : "false") +
                " | abilityInstance=" +
                (g_PermanentHumanSweaterAbility
                    ? JasonAISafeName(g_PermanentHumanSweaterAbility)
                    : "<none>") +
                " | stateReset=" +
                (abilityStateReset ? "true" : "false"));
        }
        return unlimitedArmed || availabilityArmed || abilityStateReset;
    }

    void ReleasePermanentHumanSweaterAttackLockIfNeeded(
        AActor* counselor,
        ULONGLONG now)
    {
        if (g_PermanentHumanSweaterAttackUnlockAt == 0 ||
            now < g_PermanentHumanSweaterAttackUnlockAt ||
            !counselor || !counselor->Class)
        {
            return;
        }

        // The authored Pamela-use montage owns counselor combat input for
        // most of the ten-second voice lifecycle. End only the local
        // counselor's current montage after the opening line; Jason's stun and
        // Pamela's audio are independently owned and remain active.
        g_PermanentHumanSweaterAttackUnlockAt = 0;
        UFunction* stopMontage = FindFunctionInHierarchyByName(
            counselor->Class,
            "StopAnimMontage");
        struct StopAnimMontageParams
        {
            UObject* AnimMontage;
        };
        StopAnimMontageParams params{ nullptr };
        const bool released = stopMontage && SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(counselor),
            counselor,
            stopMontage,
            &params);
        Logger::Success(
            std::string("18L-BG Pamela counselor attack lock released after opening line | montageStopped=") +
            (released ? "true" : "false"));
    }

    void MaintainPermanentHumanSweater(AActor* counselor)
    {
        if (!counselor ||
            !Memory::IsReadable(counselor, 0x1C6B) ||
            !ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(counselor),
                "SCCounselorCharacter"))
        {
            return;
        }

        const ULONGLONG now = GetTickCount64();

        // Stock keeps the sweater wearer inside the Pamela-use montage for
        // most of the spoken line. The kill rule only needs the activation
        // edge; release the local montage shortly after that edge so the
        // player can reposition and deliver the first slash while Pamela's
        // audio and Jason's native trance continue independently.
        ReleasePermanentHumanSweaterAttackLockIfNeeded(counselor, now);

        // Pamela's stock ability lasts ten seconds. Its cleanup can run after
        // the consumed ownership byte is first restored and clear the HUD
        // ability slot again. Rearm after that lifecycle ends, with one
        // bounded replication-order retry and no additional world scan.
        if (g_PermanentHumanSweaterLatched &&
            g_PermanentHumanSweaterCarrier == counselor &&
            g_PermanentHumanSweaterRearmAt != 0 &&
            now >= g_PermanentHumanSweaterRearmAt)
        {
            if (g_PamelaTranceKillWindowUntil != 0 &&
                now <= g_PamelaTranceKillWindowUntil)
            {
                // Do not expose Y during the current trance/kill attempt.
                // Repeated Pamela lines were restarting the movement lock.
                g_PermanentHumanSweaterRearmAt =
                    g_PamelaTranceKillWindowUntil + 1;
            }
            else
            {
                RearmPermanentHumanSweater(counselor, true);
                ++g_PermanentHumanSweaterRearmAttempts;
                if (g_PermanentHumanSweaterRearmAttempts < 2)
                {
                    g_PermanentHumanSweaterRearmAt = now + 2000;
                }
                else
                {
                    g_PermanentHumanSweaterRearmAt = 0;
                    Logger::Success(
                        "18L-BC Pamela sweater HUD ability rearmed after stock ten-second activation lifecycle");
                }
            }
        }

        uint8_t* sweaterFlag = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(counselor) + 0x1C6A);
        if (*sweaterFlag != 0)
        {
            if (!g_PermanentHumanSweaterLatched ||
                g_PermanentHumanSweaterCarrier != counselor)
            {
                g_PermanentHumanSweaterCarrier = counselor;
                g_PermanentHumanSweaterLatched = true;
                UObject* activeAbility = ReadReflectedObjectProperty(
                    reinterpret_cast<UObject*>(counselor),
                    "ActiveAbility");
                g_PermanentHumanInnateActiveAbility =
                    activeAbility &&
                    Memory::IsReadable(activeAbility, sizeof(UObject)) &&
                    !IsPamelaSweaterAbilityObject(activeAbility)
                        ? activeAbility
                        : nullptr;
                g_PermanentHumanSweaterRestoreLogged = false;
                g_PermanentHumanUnlimitedSweaterArmed = false;
                g_PermanentHumanSweaterRearmAt = 0;
                g_PermanentHumanSweaterRearmAttempts = 0;
                g_PermanentHumanSweaterRearmSucceeded = false;
                g_PermanentHumanSweaterActivationUntil = 0;
                g_PermanentHumanSweaterAttackUnlockAt = 0;
                g_PermanentHumanSweaterUseCount = 0;
                g_PamelaTranceKillWindowUntil = 0;
                g_PamelaTranceJasonHealthBaseline = 0.0f;
                g_PamelaTranceJasonHealthBaselineValid = false;
                g_PamelaTranceDamageEventObserved.store(
                    false,
                    std::memory_order_release);
                g_PamelaTranceKneelRequested = false;
                g_PamelaTranceFinalSlashAccepted = false;
                g_RepeatJasonKillStanceAt = 0;
                g_RepeatJasonKillStanceDeadline = 0;
                g_RepeatJasonKillStanceAttempts = 0;
                RearmPermanentHumanSweater(counselor, false);
                Logger::Success(
                    "18L-AX human Pamela sweater ownership latched for the kill sequence");
            }
            return;
        }

        if (g_PermanentHumanSweaterLatched &&
            g_PermanentHumanSweaterCarrier == counselor)
        {
            ++g_PermanentHumanSweaterUseCount;
            // The ownership byte falling to zero is the reliable native
            // activation edge.  Arm the previously dormant early-unlock path
            // once per use and keep reinforcement Jasons quiet for the stock
            // Pamela trance so they cannot interrupt the solo counselor before
            // the opening slash. Keep the native trance available for thirty
            // seconds; requiring an immediate back-to-back hit made otherwise
            // valid player attempts fail as the voice line ended.
            g_PermanentHumanSweaterActivationUntil = now + 30000;
            g_PermanentHumanSweaterAttackUnlockAt = now + 700;
            // This is the authoritative Pamela activation edge. Arm the
            // already-implemented slash observer here so the next real melee
            // hit can own the kneel/final sequence. The previous packed build
            // never set this window, leaving both the simplified slash path
            // and its all-Jason exclusion guard permanently dormant.
            g_PamelaTranceKillWindowUntil = now + 30000;
            g_PamelaTranceJasonHealthBaselineValid = false;
            g_PamelaTranceDamageEventObserved.store(
                false,
                std::memory_order_release);
            g_PamelaTranceKneelRequested = false;
            g_PamelaTranceFinalSlashAccepted = false;
            g_PamelaTrancePreferredFinisher = nullptr;
            g_PamelaTrancePreferredFinisherUntil = 0;
            if (g_PermanentHumanSweaterUseCount >= 2)
            {
                // Final v11's proven retry route lets the stock Pamela stun
                // publish JasonDeath_C first, then recreates only a consumed
                // repeat opportunity. Never pre-freeze Jason or synthesize a
                // competing 60-second trance state before that context exists.
                g_RepeatJasonKillStanceAt = now + 1500;
                g_RepeatJasonKillStanceDeadline = now + 8000;
                g_RepeatJasonKillStanceAttempts = 0;
            }
            // The stock sweater activation consumes this native ownership
            // byte. Restore only the already-earned human sweater, on the
            // existing kill-team cadence, so the item remains available
            // throughout the paired kill sequence without any world scan.
            // Preserve the earned sweater while the stock Pamela line plays,
            // but do not republish Y until its ten-second ability lifecycle
            // finishes. Immediate republishing let the same line restart and
            // kept the local counselor in the ability movement lock.
            RearmPermanentHumanSweater(counselor, false);
            g_PermanentHumanSweaterRearmAt = now + 11000;
            g_PermanentHumanSweaterRearmAttempts = 0;
            if (!g_PermanentHumanSweaterRestoreLogged)
            {
                g_PermanentHumanSweaterRestoreLogged = true;
                Logger::Success(
                    "18L-AY restored permanent human Pamela sweater ownership and visible wearing state after activation");
            }
        }
    }

    bool IsFemaleCounselor(AActor* counselor)
    {
        if (!counselor || !counselor->Class ||
            !Memory::IsReadable(counselor, sizeof(UObject)))
        {
            return false;
        }
        if (ReadReflectedBoolByte(
                reinterpret_cast<UObject*>(counselor),
                "bIsFemale"))
        {
            return true;
        }

        const std::string className = JasonAISafeName(
            reinterpret_cast<UObject*>(counselor->Class));
        const char* femaleArchetypes[] =
        {
            "Athlete_Counselor_C",
            "Bookworm_Counselor_C",
            "Flirt_Counselor_C",
            "Hero_Counselor_C",
            "Rocker_Counselor_C",
            "Head_Counselor_C",
            "Biker_Counselor_C",
            "Catty_Counselor_C",
            "Tina_Counselor_C"
        };
        for (const char* archetype : femaleArchetypes)
        {
            if (className == archetype)
                return true;
        }
        return false;
    }

    UObject* GetPawnControllerSafe(AActor* pawn)
    {
        if (!pawn || !Memory::IsReadable(pawn, 0x3A8))
            return nullptr;
        UObject** controller = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(pawn) + 0x3A0);
        return Memory::IsReadable(controller, sizeof(UObject*)) &&
            *controller && Memory::IsReadable(*controller, sizeof(UObject))
            ? *controller
            : nullptr;
    }

    UObject* GetCounselorBlackboardCached(UObject* controller)
    {
        if (!controller ||
            !Memory::IsReadable(controller, sizeof(UObject)) ||
            !controller->Class)
        {
            return nullptr;
        }

        static UClass* cachedClass = nullptr;
        static int32_t cachedOffset = -1;
        if (cachedClass != controller->Class)
        {
            cachedClass = controller->Class;
            cachedOffset = -1;
            UPropertyLite* property = FindPropertyInHierarchyByName(
                controller->Class,
                "Blackboard");
            if (!property)
            {
                property = FindPropertyInHierarchyByName(
                    controller->Class,
                    "BlackboardComponent");
            }
            if (property &&
                property->Offset_Internal > 0 &&
                property->Offset_Internal < 0x10000)
            {
                cachedOffset = property->Offset_Internal;
            }
        }
        if (cachedOffset <= 0)
            return nullptr;

        UObject** field = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(controller) + cachedOffset);
        return Memory::IsReadable(field, sizeof(UObject*)) &&
            *field && Memory::IsReadable(*field, sizeof(UObject))
                ? *field
                : nullptr;
    }

    UObject* GetCounselorBrainComponentCached(UObject* controller)
    {
        if (!controller ||
            !Memory::IsReadable(controller, sizeof(UObject)) ||
            !controller->Class)
        {
            return nullptr;
        }

        static UClass* cachedClass = nullptr;
        static int32_t cachedOffset = -1;
        if (cachedClass != controller->Class)
        {
            cachedClass = controller->Class;
            cachedOffset = -1;
            UPropertyLite* property = FindPropertyInHierarchyByName(
                controller->Class,
                "BrainComponent");
            if (property &&
                property->Offset_Internal > 0 &&
                property->Offset_Internal < 0x10000)
            {
                cachedOffset = property->Offset_Internal;
            }
        }
        if (cachedOffset <= 0)
            return nullptr;

        UObject** field = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(controller) + cachedOffset);
        return Memory::IsReadable(field, sizeof(UObject*)) &&
            *field && Memory::IsReadable(*field, sizeof(UObject))
                ? *field
                : nullptr;
    }

    AActor* FindNearestWorldActorByClass(
        const char* className,
        const FVector* preferredOrigin,
        float maximumDistance)
    {
        UWorld* world = g_JasonAIState.World;
        if (!world || !className || !Memory::IsReadable(world, sizeof(UWorld)))
            return nullptr;

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data || levels->Count <= 0 || levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return nullptr;
        }

        AActor* best = nullptr;
        float bestDistanceSquared = maximumDistance > 0.0f
            ? maximumDistance * maximumDistance
            : FLT_MAX;
        for (int32_t levelIndex = 0; levelIndex < levels->Count; ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;
            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data || actors.Count <= 0 || actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t actorIndex = 0; actorIndex < actors.Count; ++actorIndex)
            {
                AActor* actor = actors.Data[actorIndex];
                if (!ObjectClassDerivesFromExact(
                        reinterpret_cast<UObject*>(actor),
                        className))
                {
                    continue;
                }

                if (!preferredOrigin)
                    return actor;
                FVector location{};
                if (!GetJasonAIActorLocation(actor, location))
                    continue;
                const float dx = location.X - preferredOrigin->X;
                const float dy = location.Y - preferredOrigin->Y;
                const float distanceSquared = dx * dx + dy * dy;
                if (std::isfinite(distanceSquared) &&
                    distanceSquared < bestDistanceSquared)
                {
                    bestDistanceSquared = distanceSquared;
                    best = actor;
                }
            }
        }
        return best;
    }

    AActor* FindNearestWorldDoorActor(
        const FVector& preferredOrigin,
        float maximumDistance)
    {
        UWorld* world = g_JasonAIState.World;
        if (!world || !Memory::IsReadable(world, sizeof(UWorld)))
            return nullptr;

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data || levels->Count <= 0 || levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return nullptr;
        }

        AActor* best = nullptr;
        float bestDistanceSquared = maximumDistance * maximumDistance;
        for (int32_t levelIndex = 0; levelIndex < levels->Count; ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;
            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data || actors.Count <= 0 || actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t actorIndex = 0; actorIndex < actors.Count; ++actorIndex)
            {
                AActor* actor = actors.Data[actorIndex];
                if (!actor || !actor->Class ||
                    !Memory::IsReadable(actor, sizeof(UObject)))
                {
                    continue;
                }

                const std::string actorName = JasonAISafeName(
                    reinterpret_cast<UObject*>(actor));
                const std::string className = JasonAISafeName(
                    reinterpret_cast<UObject*>(actor->Class));
                if (actorName.find("Door") == std::string::npos &&
                    className.find("Door") == std::string::npos)
                {
                    continue;
                }

                FVector location{};
                if (!GetJasonAIActorLocation(actor, location))
                    continue;
                const float dx = location.X - preferredOrigin.X;
                const float dy = location.Y - preferredOrigin.Y;
                const float distanceSquared = dx * dx + dy * dy;
                if (std::isfinite(distanceSquared) &&
                    distanceSquared < bestDistanceSquared)
                {
                    bestDistanceSquared = distanceSquared;
                    best = actor;
                }
            }
        }
        return best;
    }

    void EnsureTommyRadioObjectivePublished(ULONGLONG now)
    {
        if (g_TommyObjectivePublished ||
            now < g_NextTommyObjectivePublishAt)
        {
            return;
        }
        g_NextTommyObjectivePublishAt = now + 5000;

        UWorld* world = g_JasonAIState.World;
        AActor* gameState = world
            ? ReadActorField(reinterpret_cast<UObject*>(world), 0xF8)
            : nullptr;
        if (!gameState || !gameState->Class ||
            !Memory::IsReadable(gameState, sizeof(UObject)))
        {
            return;
        }

        UFunction* getCBRadio = FindFunctionInHierarchyByName(
            gameState->Class,
            "GetCBRadio");
        struct GetRadioParams { AActor* ReturnValue; };
        GetRadioParams getParams{};
        if (getCBRadio &&
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(gameState),
                gameState,
                getCBRadio,
                &getParams) &&
            getParams.ReturnValue &&
            Memory::IsReadable(getParams.ReturnValue, sizeof(UObject)))
        {
            g_TommyObjectiveRadio = getParams.ReturnValue;
            g_TommyObjectivePublished = true;
            Logger::Success(
                "18L-AP Tommy objective ready: GameState already publishes " +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(g_TommyObjectiveRadio)));
            return;
        }

        if (g_TommyObjectiveRadio &&
            !Memory::IsReadable(g_TommyObjectiveRadio, sizeof(UObject)))
        {
            g_TommyObjectiveRadio = nullptr;
        }
        if (!g_TommyObjectiveRadio)
        {
            g_TommyObjectiveRadio = FindNearestWorldActorByClass(
                "HunterCBRadio_C",
                nullptr,
                0.0f);
            if (!g_TommyObjectiveRadio)
            {
                g_TommyObjectiveRadio = FindNearestWorldActorByClass(
                    "SCCBRadio",
                    nullptr,
                    0.0f);
            }
        }
        if (!g_TommyObjectiveRadio)
            return;

        const char* propertyNames[] =
        {
            "CBRadio",
            "HunterCBRadio",
            "CBRadioActor"
        };
        UPropertyLite* radioProperty = nullptr;
        const char* resolvedPropertyName = nullptr;
        for (const char* propertyName : propertyNames)
        {
            UPropertyLite* candidate = FindPropertyInHierarchyByName(
                gameState->Class,
                propertyName);
            if (!candidate ||
                candidate->ElementSize != sizeof(AActor*) ||
                candidate->Offset_Internal <= 0 ||
                candidate->Offset_Internal >= 0x10000)
            {
                continue;
            }
            const std::string propertyType = JasonAISafeName(
                reinterpret_cast<UObject*>(candidate->ClassPrivate));
            if (propertyType.find("ObjectProperty") == std::string::npos)
                continue;
            radioProperty = candidate;
            resolvedPropertyName = propertyName;
            break;
        }
        if (!radioProperty)
        {
            Logger::Error(
                "18L-AP Tommy objective: CBRadio GameState property unavailable; preserving stock UI");
            g_TommyObjectivePublished = true;
            return;
        }

        AActor** radioField = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(gameState) +
            radioProperty->Offset_Internal);
        if (!Memory::IsReadable(radioField, sizeof(AActor*)))
            return;
        *radioField = g_TommyObjectiveRadio;

        getParams = {};
        const bool getterConfirmed = getCBRadio &&
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(gameState),
                gameState,
                getCBRadio,
                &getParams) &&
            getParams.ReturnValue == g_TommyObjectiveRadio;
        if (getterConfirmed)
        {
            g_TommyObjectivePublished = true;
            Logger::Success(
                std::string("18L-AP Tommy objective published through GameState.") +
                resolvedPropertyName + " | radio=" +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(g_TommyObjectiveRadio)));
        }
    }

    int32_t CountPawnItem(AActor* pawn, UClass* itemClass)
    {
        if (!pawn || !itemClass ||
            !Memory::IsReadable(pawn, sizeof(UObject)))
        {
            return 0;
        }
        uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(pawn);
        if (!vtable ||
            !Memory::IsReadable(vtable, 0xE98 + sizeof(uintptr_t)))
        {
            return 0;
        }
        using CountItemFn = int32_t(__fastcall*)(AActor*, UClass*);
        CountItemFn countItem = reinterpret_cast<CountItemFn>(
            vtable[0xE98 / sizeof(uintptr_t)]);
        if (!countItem ||
            !Memory::IsReadable(reinterpret_cast<void*>(countItem), 1))
        {
            return 0;
        }
        __try
        {
            return countItem(pawn, itemClass);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    UObject* GetCounselorInteractionManager(AActor* counselor)
    {
        if (!counselor ||
            !Memory::IsReadable(counselor, sizeof(UObject)) ||
            !counselor->Class)
            return nullptr;

        // Shipping counselors use the native manager field at +0xE18. Prefer
        // the verified direct pointer so proximity-final checks do not walk
        // reflected property metadata when several counselor classes are
        // standing beside Jason. Reflection remains a compatibility fallback.
        UObject** direct = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(counselor) + 0xE18);
        if (Memory::IsReadable(direct, sizeof(UObject*)) &&
            *direct && Memory::IsReadable(*direct, sizeof(UObject)))
        {
            return *direct;
        }

        static UClass* cachedClass = nullptr;
        static int32_t cachedOffset = -1;
        if (cachedClass != counselor->Class)
        {
            cachedClass = counselor->Class;
            cachedOffset = -1;
            UPropertyLite* property = FindPropertyInHierarchyByName(
                counselor->Class,
                "InteractableManagerComponent");
            if (property &&
                property->Offset_Internal > 0 &&
                property->Offset_Internal < 0x10000)
            {
                cachedOffset = property->Offset_Internal;
            }
        }
        if (cachedOffset > 0)
        {
            UObject** field = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(counselor) + cachedOffset);
            if (Memory::IsReadable(field, sizeof(UObject*)) &&
                *field && Memory::IsReadable(*field, sizeof(UObject)))
            {
                return *field;
            }
        }

        return nullptr;
    }

    bool TryGetLatchedJasonDeathContext(
        AActor*& outContext,
        UObject*& outInteractComponent,
        ULONGLONG now)
    {
        if (now >= g_KillTeamFinalContextUntil ||
            !g_LastAcceptedFinalContext ||
            !g_LastAcceptedFinalComponent ||
            !Memory::IsReadable(
                g_LastAcceptedFinalContext,
                sizeof(UObject)) ||
            !Memory::IsReadable(
                g_LastAcceptedFinalComponent,
                sizeof(UObject)) ||
            !ObjectClassDerivesFromExact(
                g_LastAcceptedFinalContext,
                "JasonDeath_C") ||
            !ObjectClassDerivesFromExact(
                g_LastAcceptedFinalComponent,
                "SCInteractComponent"))
        {
            return false;
        }

        outContext = g_LastAcceptedFinalContext;
        outInteractComponent = g_LastAcceptedFinalComponent;
        return true;
    }

    bool TryGetValidatedJasonDeathContext(
        AActor* jason,
        AActor*& outContext,
        UObject*& outInteractComponent,
        ULONGLONG now)
    {
        outContext = nullptr;
        outInteractComponent = nullptr;
        AActor* candidate = jason
            ? ReadActorField(reinterpret_cast<UObject*>(jason), 0x12D8)
            : nullptr;
        if (!candidate)
        {
            // +0x12D8 can clear briefly while the stock paired interaction
            // transitions between kneel and kill. Keep the last fully
            // validated context authoritative during the bounded final-action
            // window instead of handing Tommy back to flee/loot behavior.
            if (TryGetLatchedJasonDeathContext(
                    outContext,
                    outInteractComponent,
                    now))
            {
                return true;
            }
            if (g_LastAcceptedFinalContext)
            {
                g_LastAcceptedFinalContext = nullptr;
                g_LastAcceptedFinalComponent = nullptr;
                g_LastAcceptedFinalKillComponent = nullptr;
                g_KillTeamFinalInteractionDispatched = false;
                g_KillTeamFinalInteractionPending = false;
                g_KillTeamFinalInteractionAttempts = 0;
                g_KillTeamFinalInteractionStartedAt = 0;
                g_KillTeamPendingFinalContext = nullptr;
                g_KillTeamPendingFinalComponent = nullptr;
            }
            return false;
        }

        UObject** componentField = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(candidate) + 0x370);
        UObject* component =
            Memory::IsReadable(componentField, sizeof(UObject*))
                ? *componentField
                : nullptr;

        // +0x12D8 is not exclusively a Jason-death flag, so validate both
        // sides of the stock relationship. JasonDeath_C owns an ordinary
        // SCInteractComponent named Interactable; its separate
        // ContextKillComponent drives the paired animation but is not the
        // component accepted by counselor AttemptInteract. Treating every
        // non-null occupant as the final scene previously suspended Jason's
        // chase, door-breaking and combat for three seconds out of every two.
        const bool valid =
            ObjectClassDerivesFromExact(candidate, "JasonDeath_C") &&
            component &&
            Memory::IsReadable(component, sizeof(UObject)) &&
            ObjectClassDerivesFromExact(component, "SCInteractComponent");
        if (!valid)
        {
            if (candidate != g_LastRejectedFinalContext ||
                now >= g_NextFinalContextDiagnosticAt)
            {
                g_LastRejectedFinalContext = candidate;
                g_NextFinalContextDiagnosticAt = now + 30000;
                Logger::Debug(
                    "18L-AU ignored non-death Jason +0x12D8 object | object=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(candidate)) +
                    " | objectClass=" +
                    (candidate && candidate->Class
                        ? JasonAISafeName(
                            reinterpret_cast<UObject*>(candidate->Class))
                        : "NULL") +
                    " | component=" +
                    JasonAISafeName(component) +
                    " | componentClass=" +
                    (component && component->Class
                        ? JasonAISafeName(
                            reinterpret_cast<UObject*>(component->Class))
                        : "NULL"));
            }
            if (TryGetLatchedJasonDeathContext(
                    outContext,
                    outInteractComponent,
                    now))
            {
                return true;
            }
            if (g_LastAcceptedFinalContext)
            {
                g_LastAcceptedFinalContext = nullptr;
                g_LastAcceptedFinalComponent = nullptr;
                g_LastAcceptedFinalKillComponent = nullptr;
                g_KillTeamFinalInteractionDispatched = false;
                g_KillTeamFinalInteractionPending = false;
                g_KillTeamFinalInteractionAttempts = 0;
                g_KillTeamFinalInteractionStartedAt = 0;
                g_KillTeamPendingFinalContext = nullptr;
                g_KillTeamPendingFinalComponent = nullptr;
            }
            return false;
        }

        outContext = candidate;
        outInteractComponent = component;
        g_LastAcceptedFinalComponent = component;

        // JasonDeath_C exposes two different interaction components. Counselors
        // press A through Interactable, while the native Hunter/weapon predicate
        // executes on ContextKillComponent. Cache the latter so the eligibility
        // hook is scoped to this exact death actor instead of the wrong sibling.
        UObject* contextKillComponent = ReadReflectedObjectProperty(
            candidate,
            "ContextKillComponent");
        if (contextKillComponent &&
            ObjectClassDerivesFromExact(
                contextKillComponent,
                "SCContextKillComponent"))
        {
            g_LastAcceptedFinalKillComponent = contextKillComponent;
        }
        else
        {
            g_LastAcceptedFinalKillComponent = nullptr;
        }
        if (candidate != g_LastAcceptedFinalContext)
        {
            g_LastAcceptedFinalContext = candidate;
            g_LastUniversalFinalEligibilityContext = nullptr;
            g_LastUniversalFinalEligibilityFinisher = nullptr;
            g_LastUniversalFinalEligibilityAcceptedAt = 0;
            g_KillTeamFinalInteractionCommitted = false;
            // A MoveTo accepted just before the native kneel can remain active
            // even though normal Jason decisions are suspended. Cancel it at
            // the transition and retire its path bookkeeping so the kneeling
            // actor cannot glide toward the previous counselor target.
            StopJasonAIMovementForKnifeOnGameThread();
            g_JasonAIState.PathLocked = false;
            g_JasonAIState.PathLockUntil = 0;
            g_JasonAIState.LastAcceptedMoveAt = 0;
            g_JasonAIState.HaveLastLocation = false;
            g_KillTeamFinalInteractionDispatched = false;
            g_KillTeamFinalInteractionPending = false;
            g_KillTeamFinalInteractionAttempts = 0;
            g_KillTeamFinalInteractionStartedAt = 0;
            g_KillTeamFinalSequenceStartedAt = now;
            g_KillTeamFinalMoveFailures = 0;
            g_KillTeamFinalRepositionAttempted = false;
            g_KillTeamPendingFinalContext = nullptr;
            g_KillTeamPendingFinalComponent = nullptr;
            g_KillTeamPendingFinalFinisher = nullptr;
            Logger::Success(
                "18L-AU validated native Jason death context | object=" +
                JasonAISafeName(reinterpret_cast<UObject*>(candidate)) +
                " | component=" + JasonAISafeName(component) +
                " | componentClass=" +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(component->Class)));
        }
        return true;
    }

    void CancelKillTeamHelperNonFinalInteraction(
        AActor* helper,
        ULONGLONG now)
    {
        if (!helper || now < g_NextKillTeamInteractionCancelAt)
            return;

        UObject* manager = GetCounselorInteractionManager(helper);
        UObject** locked = manager
            ? reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(manager) + 0x230)
            : nullptr;
        if (!locked || !Memory::IsReadable(locked, sizeof(UObject*)) ||
            !*locked)
        {
            g_KillTeamLastCancelledInteraction = nullptr;
            return;
        }

        AActor* deathContext = nullptr;
        UObject* deathComponent = nullptr;
        if (TryGetValidatedJasonDeathContext(
                g_JasonAIState.Jason,
                deathContext,
                deathComponent,
                now) &&
            IsFinalContextLockMatch(
                *locked,
                deathContext,
                deathComponent))
        {
            return;
        }

        UObject* currentLock = *locked;
        if (currentLock == g_KillTeamLastCancelledInteraction)
            return;

        // Returned Tommy never owns ordinary loot/hide/repair objectives.
        // Ask the native interaction manager to cancel its current attempt;
        // never clear the lock field directly, and never touch the validated
        // JasonDeath interaction or a Jason grab/pocket-knife transition.
        static UClass* cachedManagerClass = nullptr;
        static UFunction* cancelFunction = nullptr;
        if (manager->Class != cachedManagerClass)
        {
            cachedManagerClass = manager->Class;
            cancelFunction = FindFunctionInHierarchyByName(
                manager->Class,
                "CLIENT_CancelInteractAttempt");
            if (!cancelFunction)
            {
                cancelFunction = FindFunctionInHierarchyByName(
                    manager->Class,
                    "CLIENT_UnlockInteraction");
            }
        }

        const bool cancelled = cancelFunction &&
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(manager),
                manager,
                cancelFunction,
                nullptr);
        // ProcessEvent is allowed to release the manager lock immediately, so
        // retain the validated pre-call pointer for bookkeeping/logging rather
        // than dereferencing the field again after cancellation.
        g_KillTeamLastCancelledInteraction = currentLock;
        g_NextKillTeamInteractionCancelAt = now + 2000;
        Logger::Debug(
            std::string("18L-AY AI Tommy cancelled non-final interaction | call=") +
            (cancelled ? "true" : "false") +
            " | object=" + JasonAISafeName(currentLock));
    }

    bool IsFinalContextLockMatch(
        UObject* locked,
        AActor* context,
        UObject* component)
    {
        if (!locked || !context || !component)
            return false;
        if (locked == component)
            return true;
        AActor** ownerField = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(locked) + 0xE0);
        return Memory::IsReadable(ownerField, sizeof(AActor*)) &&
            *ownerField == context;
    }

    bool IsKillTeamHelperNativeBusy(AActor* helper, ULONGLONG now)
    {
        if (!helper)
            return true;

        if (g_LastHeldCounselor == helper)
        {
            g_KillTeamHelperNativeBusyUntil = now + 3500;
            g_KillTeamHelperStableObservations = 0;
            return true;
        }

        UObject* manager = GetCounselorInteractionManager(helper);
        UObject** locked = manager
            ? reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(manager) + 0x230)
            : nullptr;
        if (locked && Memory::IsReadable(locked, sizeof(UObject*)) && *locked)
        {
            g_KillTeamHelperNativeBusyUntil = now + 2500;
            g_KillTeamHelperStableObservations = 0;
            AActor* deathContext = nullptr;
            UObject* deathComponent = nullptr;
            if (TryGetValidatedJasonDeathContext(
                    g_JasonAIState.Jason,
                    deathContext,
                    deathComponent,
                    now) &&
                IsFinalContextLockMatch(
                    *locked,
                    deathContext,
                    deathComponent) &&
                deathContext == g_KillTeamPendingFinalContext &&
                deathComponent == g_KillTeamPendingFinalComponent)
            {
                g_KillTeamFinalContextUntil = now + 15000;
                if (!g_KillTeamFinalInteractionDispatched &&
                    (g_KillTeamFinalInteractionPending ||
                        g_KillTeamFinalInteractionAttempts > 0))
                {
                    g_KillTeamFinalInteractionDispatched = true;
                    g_KillTeamFinalInteractionPending = false;
                    Logger::Success(
                        "18L-AW AI Tommy final interaction confirmed by native counselor lock");
                }
            }
            if (g_KillTeamRoute == KillTeamRoute::HumanSweaterAITommy &&
                *locked == g_KillTeamLastCancelledInteraction)
            {
                // The cancel request owns this ordinary interaction now. Let
                // the Jason-only MoveTo abort its path immediately instead of
                // waiting several seconds for a loot/hide lock to decay.
                g_KillTeamHelperNativeBusyUntil = 0;
                g_KillTeamHelperStableObservations = 2;
                return false;
            }
            return true;
        }

        if (now < g_KillTeamHelperNativeBusyUntil)
        {
            g_KillTeamHelperStableObservations = 0;
            return true;
        }

        // Require two consecutive quiet control observations after a native
        // grab/pocket-knife/context interaction before custom movement,
        // pickup, equip or inventory work resumes.
        if (g_KillTeamHelperStableObservations < 2)
        {
            ++g_KillTeamHelperStableObservations;
            return true;
        }
        return false;
    }

    bool AttemptCounselorPickup(AActor* counselor, AActor* item)
    {
        UObject* manager = GetCounselorInteractionManager(counselor);
        UObject* interactComponent = item
            ? ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(item),
                "InteractComponent")
            : nullptr;
        HMODULE module = GetModuleHandle(nullptr);
        if (!manager || !interactComponent || !module ||
            !Memory::IsReadable(manager, sizeof(UObject)))
        {
            return false;
        }

        UObject** locked = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(manager) + 0x230);
        if (Memory::IsReadable(locked, sizeof(UObject*)) && *locked)
            return false;

        uintptr_t managerVTable = *reinterpret_cast<uintptr_t*>(manager);
        uintptr_t* attemptSlot = reinterpret_cast<uintptr_t*>(
            managerVTable + 0x430);
        const uintptr_t expected = reinterpret_cast<uintptr_t>(module) +
            0x0025AC50;
        if (!managerVTable ||
            !Memory::IsReadable(attemptSlot, sizeof(uintptr_t)) ||
            *attemptSlot != expected)
        {
            return false;
        }

        reinterpret_cast<AttemptInteractFn>(*attemptSlot)(
            manager,
            interactComponent,
            1,
            true);
        return true;
    }

    bool TeleportCounselorNearObjective(
        AActor* counselor,
        UObject* controller,
        const FVector& objectiveLocation)
    {
        if (!counselor || !controller || !counselor->Class)
            return false;

        UFunction* stopMovement = controller->Class
            ? FindFunctionInHierarchyByName(
                controller->Class,
                "StopMovement")
            : nullptr;
        if (stopMovement)
        {
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(controller),
                controller,
                stopMovement,
                nullptr);
        }

        UFunction* teleportFunction =
            FindFunctionInHierarchyByName(
                counselor->Class,
                "K2_TeleportTo");
        if (!teleportFunction)
            return false;

        struct Rotation3 { float Pitch; float Yaw; float Roll; };
        struct TeleportParams
        {
            FVector DestLocation;
            Rotation3 DestRotation;
            bool ReturnValue;
        };
        static_assert(sizeof(TeleportParams) == 28,
            "Counselor TeleportParams must be 28 bytes");

        // The fixed shack axe can be inside a room the counselor navmesh does
        // not enter. Try four interaction-range offsets; K2_TeleportTo keeps
        // normal capsule collision checks and rejects an unsafe point.
        const FVector offsets[] =
        {
            FVector{ 110.0f, 0.0f, 20.0f },
            FVector{ -110.0f, 0.0f, 20.0f },
            FVector{ 0.0f, 110.0f, 20.0f },
            FVector{ 0.0f, -110.0f, 20.0f }
        };
        for (const FVector& offset : offsets)
        {
            TeleportParams params{};
            params.DestLocation.X = objectiveLocation.X + offset.X;
            params.DestLocation.Y = objectiveLocation.Y + offset.Y;
            params.DestLocation.Z = objectiveLocation.Z + offset.Z;
            if (SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(counselor),
                    counselor,
                    teleportFunction,
                    &params) &&
                params.ReturnValue)
            {
                return true;
            }
        }
        return false;
    }

    bool DriveCounselorToPickup(
        AActor* counselor,
        AActor* item,
        ULONGLONG now,
        const char* label)
    {
        UObject* controller = GetPawnControllerSafe(counselor);
        FVector counselorLocation{};
        FVector itemLocation{};
        if (!controller || !item ||
            !GetJasonAIActorLocation(counselor, counselorLocation) ||
            !GetJasonAIActorLocation(item, itemLocation))
        {
            return false;
        }

        const float dx = itemLocation.X - counselorLocation.X;
        const float dy = itemLocation.Y - counselorLocation.Y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(distance))
            return false;

        const bool shackAxeObjective =
            item == g_KillTeamAxe &&
            std::strcmp(label, "ShackAxe") == 0;
        if (shackAxeObjective)
        {
            if (g_KillTeamAxePursuitStartedAt == 0)
            {
                g_KillTeamAxePursuitStartedAt = now;
                g_KillTeamAxeLastProgressAt = now;
                g_KillTeamAxeBestDistance = distance;
            }
            else if (distance + 75.0f < g_KillTeamAxeBestDistance)
            {
                g_KillTeamAxeBestDistance = distance;
                g_KillTeamAxeLastProgressAt = now;
            }

            // Let stock pathing take Tommy to the shack first. If its looting
            // tree repeatedly steals the route or the interior has no usable
            // navmesh, perform one collision-checked relocation into pickup
            // range instead of scanning or issuing MoveTo forever.
            if (distance > 165.0f &&
                now >= g_KillTeamAxeLastProgressAt + 15000 &&
                TeleportCounselorNearObjective(
                    counselor,
                    controller,
                    itemLocation))
            {
                g_KillTeamAxeLastProgressAt = now;
                g_KillTeamAxeBestDistance = 0.0f;
                g_NextKillTeamInteractAt = now + 2000;
                const bool pickupDispatched =
                    AttemptCounselorPickup(counselor, item);
                Logger::Success(
                    std::string("18L-AS AI Tommy shack-axe stuck fallback completed | pickup=") +
                    (pickupDispatched ? "true" : "false"));
                return true;
            }
        }

        if (distance > 165.0f)
        {
            if (g_KillTeamMoveTarget != item)
            {
                g_KillTeamMoveTarget = item;
                g_NextKillTeamMoveAt = 0;
            }
            // Native MoveTo keeps its path active. Rebuilding the same long
            // path every second made Tommy stutter and slowed his trip to the
            // shack. Retry only as a recovery measure.
            if (now >= g_NextKillTeamMoveAt)
            {
                IssueAIMoveToLocationOnGameThread(
                    controller,
                    itemLocation,
                    90.0f,
                    label);
                g_NextKillTeamMoveAt = now + 10000;
            }
            return true;
        }

        g_KillTeamMoveTarget = nullptr;
        g_NextKillTeamMoveAt = 0;

        if (now >= g_NextKillTeamInteractAt &&
            AttemptCounselorPickup(counselor, item))
        {
            g_NextKillTeamInteractAt = now + 2000;
            Logger::Success(
                std::string("18L-AN kill-team native pickup dispatched | objective=") +
                label + " | helper=" +
                JasonAISafeName(reinterpret_cast<UObject*>(counselor)));
        }
        return true;
    }

    UObject* GetCounselorCurrentWeapon(AActor* counselor)
    {
        HMODULE module = GetModuleHandle(nullptr);
        if (!counselor || !module ||
            !Memory::IsReadable(counselor, sizeof(UObject)))
            return nullptr;
        using GetCurrentWeaponFn = UObject*(__fastcall*)(AActor*);
        GetCurrentWeaponFn getCurrentWeapon =
            reinterpret_cast<GetCurrentWeaponFn>(
                reinterpret_cast<uintptr_t>(module) + 0x002ED9F0);
        if (!Memory::IsReadable(
                reinterpret_cast<void*>(getCurrentWeapon), 1))
        {
            return nullptr;
        }
        __try
        {
            UObject* weapon = getCurrentWeapon(counselor);
            return weapon && Memory::IsReadable(weapon, sizeof(UObject))
                ? weapon
                : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void WriteLargePositiveNumericProperty(
        UObject* object,
        const char* propertyName)
    {
        if (!object || !object->Class || !propertyName)
            return;
        UPropertyLite* property = FindPropertyInHierarchyByName(
            object->Class,
            propertyName);
        if (!property || property->ElementSize != 4 ||
            property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000)
        {
            return;
        }
        void* value = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(object) + property->Offset_Internal);
        if (!Memory::IsReadable(value, 4))
            return;
        const std::string typeName = JasonAISafeName(
            reinterpret_cast<UObject*>(property->ClassPrivate));
        if (typeName == "FloatProperty")
            *reinterpret_cast<float*>(value) = 1000000.0f;
        else if (typeName == "IntProperty")
            *reinterpret_cast<int32_t*>(value) = 1000000;
    }

    bool IsAIControlledCounselor(AActor* counselor)
    {
        UObject* controller = GetPawnControllerSafe(counselor);
        return counselor &&
            counselor != g_LocalCounselorTarget &&
            counselor != g_JasonAIState.Jason &&
            controller &&
            controller != g_LocalPlayerController &&
            JasonAIObjectDerivesFromNameContaining(
                controller,
                "AIController");
    }

    void MaintainAIHelperVitalsAndKnife(AActor* helper, ULONGLONG now)
    {
        if (!IsAIControlledCounselor(helper))
            return;
        // These are the verified ASCCharacter fields used by the mature ESP
        // and health systems. The former reflected-property path did not
        // resolve these inherited native fields on returned Tommy.
        float* health = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(helper) +
            Offsets::ASCCharacter_Health);
        float* healthMaxValue = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(helper) +
            Offsets::ASCCharacter_Health + sizeof(float));
        float* healthMinValue = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(helper) +
            Offsets::ASCCharacter_Health + sizeof(float) * 2);
        float* maxHealth = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(helper) +
            Offsets::ASCCharacter_MaxHealth);
        if (Memory::IsReadable(health, sizeof(float)) &&
            Memory::IsReadable(healthMaxValue, sizeof(float)) &&
            Memory::IsReadable(healthMinValue, sizeof(float)) &&
            Memory::IsReadable(maxHealth, sizeof(float)))
        {
            *maxHealth = 1000000.0f;
            *healthMaxValue = 1000000.0f;
            *healthMinValue = 0.0f;
            *health = 1000000.0f;
        }

        UClass* knifeClass = g_LootReplacementClasses[0];
        if (knifeClass &&
            now >= g_NextHelperKnifeGrantAt &&
            CountPawnItem(helper, knifeClass) <= 0)
        {
            HMODULE module = GetModuleHandle(nullptr);
            using GiveStartingItemFn = void(__fastcall*)(AActor*, UClass*);
            GiveStartingItemFn giveStartingItem = module
                ? reinterpret_cast<GiveStartingItemFn>(
                    reinterpret_cast<uintptr_t>(module) + 0x002EF980)
                : nullptr;
            if (giveStartingItem &&
                Memory::IsReadable(
                    reinterpret_cast<void*>(giveStartingItem),
                    1))
            {
                giveStartingItem(helper, knifeClass);
                Logger::Success(
                    "18L-AN kill-team helper pocket knife replenished through stock inventory path");
            }
            g_NextHelperKnifeGrantAt = now + 2000;
        }
    }

    void MaintainKillTeamHelperProtection(AActor* helper, ULONGLONG now)
    {
        if (!IsAIControlledCounselor(helper))
            return;

        // Returning AI Tommy already has a dedicated, phase-separated
        // protection lane. Do not duplicate the same health/inventory work
        // here every active kill-team cadence. Female helpers still use this
        // path after acquiring the sweater.
        if (helper != g_ProtectedAITommy)
            MaintainAIHelperVitalsAndKnife(helper, now);

        UObject* weapon = GetCounselorCurrentWeapon(helper);
        if (weapon &&
            ObjectClassDerivesFromExact(weapon, "SCWeapon"))
        {
            // SCWeapon.CurrentDurability is a direct float at +0x640. The
            // stock axe starts at 30 and wear subtracts from this instance
            // value; replenishing it keeps the helper's equipped weapon from
            // breaking without mutating class defaults or damage balance.
            float* currentDurability = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(weapon) + 0x640);
            if (Memory::IsReadable(currentDurability, sizeof(float)))
                *currentDurability = 1000000.0f;
        }

        if (!g_KillTeamHelperProtected)
        {
            g_KillTeamHelperProtected = true;
            Logger::Success(
                "18L-AN kill-team helper protection armed: AI-only health, replenishing pocket knife, and weapon durability");
        }
    }

    void MaintainReturningAITommyProtection(ULONGLONG now)
    {
        if (now < g_NextAITommyProtectionAt)
            return;
        // Health is set to a very large value, so this lane does not need to
        // rewrite it every 1.3 seconds. Grab release explicitly schedules an
        // earlier knife check above; the quiet steady-state cadence prevents
        // the rhythmic hitch reported when Tommy enters the match.
        g_NextAITommyProtectionAt = now + 5153;

        AActor* candidate = g_ProtectedAITommy;
        if (!candidate ||
            !Memory::IsReadable(candidate, sizeof(UObject)) ||
            !ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(candidate),
                "Hunter_Counselor_C"))
        {
            g_ProtectedAITommy = nullptr;
            return;
        }

        // A grab/death transition can briefly unpossess the returned pawn.
        // Preserve its identity through that transition and resume protection
        // as soon as its AI controller is attached again.
        if (!IsValidatedLiveCounselorPawn(candidate) ||
            !IsAIControlledCounselor(candidate))
        {
            return;
        }

        if (candidate == g_LastHeldCounselor ||
            now < g_KillTeamHelperNativeBusyUntil)
        {
            return;
        }

        MaintainAIHelperVitalsAndKnife(candidate, now);
    }

    bool ArmKillTeamCounselorCombat(AActor* helper)
    {
        if (!helper ||
            IsCounselorInPostGrabTransition(helper, GetTickCount64()) ||
            g_ShouldFleeKillerNameIndex < 0 ||
            g_ShouldFightBackNameIndex < 0 ||
            g_ShouldArmedFightBackNameIndex < 0 ||
            g_ShouldMeleeFightBackNameIndex < 0 ||
            g_SeekWeaponWhileFleeingNameIndex < 0 ||
            g_JasonCharacterNameIndex < 0 ||
            !g_JasonAIState.Jason)
        {
            return false;
        }

        UObject* controller = GetPawnControllerSafe(helper);
        if (!controller)
            return false;
        UObject* blackboard = GetCounselorBlackboardCached(controller);
        if (!blackboard)
            return false;

        // The native fight service treats this blackboard object—not merely
        // the pawn inventory—as the authoritative armed state. Publishing the
        // equipped melee weapon prevents its next evaluation from immediately
        // selecting the stock flee branch again.
        UObject* equippedWeapon = GetCounselorCurrentWeapon(helper);
        if (equippedWeapon && g_SCWeaponNameIndex >= 0)
        {
            SetBlackboardObject(
                blackboard,
                g_SCWeaponNameIndex,
                equippedWeapon);
        }

        // Use the mature counselor behavior tree for melee decisions. These
        // keys direct the armed helper into its existing fight branch. Publish
        // Jason himself as the authoritative object goal first, so the stock
        // tree pursues him from anywhere instead of alternating with loot,
        // hiding, and local-proximity discovery goals.
        bool applied = SetBlackboardObject(
            blackboard,
            g_JasonCharacterNameIndex,
            reinterpret_cast<UObject*>(g_JasonAIState.Jason));
        SetBlackboardBool(
            blackboard,
            g_ShouldFightBackNameIndex,
            true);
        SetBlackboardBool(
            blackboard,
            g_ShouldArmedFightBackNameIndex,
            true);
        SetBlackboardBool(
            blackboard,
            g_ShouldMeleeFightBackNameIndex,
            true);
        SetBlackboardBool(
            blackboard,
            g_SeekWeaponWhileFleeingNameIndex,
            false);
        SetBlackboardBool(
            blackboard,
            g_ShouldFleeKillerNameIndex,
            false);
        if (g_ShouldHideNameIndex >= 0)
            SetBlackboardBool(blackboard, g_ShouldHideNameIndex, false);
        if (g_ShouldOrientTowardKillerNameIndex >= 0)
        {
            SetBlackboardBool(
                blackboard,
                g_ShouldOrientTowardKillerNameIndex,
                true);
        }
        return applied;
    }

    void SuppressConvergenceCounselorFear(AActor* counselor)
    {
        if (!counselor ||
            !Memory::IsReadable(
                counselor,
                OFFSET_FEARMANAGER + sizeof(void*)))
        {
            return;
        }

        void** fearManager = reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(counselor) + OFFSET_FEARMANAGER);
        if (!Memory::IsReadable(fearManager, sizeof(void*)) || !*fearManager)
            return;

        float* fearAmount = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(*fearManager) + 0x130);
        if (Memory::IsReadable(fearAmount, sizeof(float)) &&
            std::isfinite(*fearAmount) && *fearAmount != 0.0f)
        {
            *fearAmount = 0.0f;
        }
    }

    bool SetCounselorBrainTickEnabled(AActor* counselor, bool enabled)
    {
        UObject* controller = GetPawnControllerSafe(counselor);
        UObject* brain = GetCounselorBrainComponentCached(controller);
        if (!brain || !brain->Class)
            return false;

        static UClass* cachedClass = nullptr;
        static UFunction* cachedFunction = nullptr;
        if (cachedClass != brain->Class)
        {
            cachedClass = brain->Class;
            cachedFunction = FindFunctionInHierarchyByName(
                brain->Class,
                "SetComponentTickEnabled");
        }
        if (!cachedFunction)
            return false;

        struct Params
        {
            bool bEnabled;
        };
        Params params{};
        params.bEnabled = enabled;
        return SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(brain),
            brain,
            cachedFunction,
            &params);
    }

    int32_t FindConvergenceTravelCounselor(AActor* counselor)
    {
        for (int32_t i = 0; i < g_ConvergenceTravelCounselorCount; ++i)
        {
            if (g_ConvergenceTravelCounselors[i] == counselor)
                return i;
        }
        return -1;
    }

    void RememberConvergenceTravelCounselor(AActor* counselor)
    {
        if (!counselor ||
            FindConvergenceTravelCounselor(counselor) >= 0 ||
            g_ConvergenceTravelCounselorCount >=
                JasonAITargetCapacity)
        {
            return;
        }
        const int32_t index = g_ConvergenceTravelCounselorCount++;
        g_ConvergenceTravelCounselors[index] = counselor;
        g_ConvergenceTravelLastLocations[index] = FVector{};
        g_ConvergenceTravelLastProgressAt[index] = 0;
        g_ConvergenceTravelRecoveryUntil[index] = 0;
        g_ConvergenceTravelHaveLocation[index] = false;
        g_ConvergenceTravelLastGoals[index] = FVector{};
        g_ConvergenceTravelLastRouteAt[index] = 0;
        g_ConvergenceTravelHaveGoal[index] = false;
    }

    void ForgetConvergenceTravelCounselor(AActor* counselor)
    {
        const int32_t index = FindConvergenceTravelCounselor(counselor);
        if (index < 0)
            return;
        --g_ConvergenceTravelCounselorCount;
        g_ConvergenceTravelCounselors[index] =
            g_ConvergenceTravelCounselors[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelLastLocations[index] =
            g_ConvergenceTravelLastLocations[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelLastProgressAt[index] =
            g_ConvergenceTravelLastProgressAt[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelRecoveryUntil[index] =
            g_ConvergenceTravelRecoveryUntil[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelHaveLocation[index] =
            g_ConvergenceTravelHaveLocation[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelLastGoals[index] =
            g_ConvergenceTravelLastGoals[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelLastRouteAt[index] =
            g_ConvergenceTravelLastRouteAt[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelHaveGoal[index] =
            g_ConvergenceTravelHaveGoal[g_ConvergenceTravelCounselorCount];
        g_ConvergenceTravelCounselors[
            g_ConvergenceTravelCounselorCount] = nullptr;
        g_ConvergenceTravelLastLocations[
            g_ConvergenceTravelCounselorCount] = FVector{};
        g_ConvergenceTravelLastProgressAt[
            g_ConvergenceTravelCounselorCount] = 0;
        g_ConvergenceTravelRecoveryUntil[
            g_ConvergenceTravelCounselorCount] = 0;
        g_ConvergenceTravelHaveLocation[
            g_ConvergenceTravelCounselorCount] = false;
        g_ConvergenceTravelLastGoals[
            g_ConvergenceTravelCounselorCount] = FVector{};
        g_ConvergenceTravelLastRouteAt[
            g_ConvergenceTravelCounselorCount] = 0;
        g_ConvergenceTravelHaveGoal[
            g_ConvergenceTravelCounselorCount] = false;
    }

    void ResumeAllConvergenceCounselorBrains()
    {
        for (int32_t i = 0; i < g_ConvergenceTravelCounselorCount; ++i)
        {
            AActor* counselor = g_ConvergenceTravelCounselors[i];
            if (counselor && Memory::IsReadable(counselor, sizeof(UObject)))
                SetCounselorBrainTickEnabled(counselor, true);
        }
        std::memset(
            g_ConvergenceTravelCounselors,
            0,
            sizeof(g_ConvergenceTravelCounselors));
        std::memset(
            g_ConvergenceTravelLastLocations,
            0,
            sizeof(g_ConvergenceTravelLastLocations));
        std::memset(
            g_ConvergenceTravelLastProgressAt,
            0,
            sizeof(g_ConvergenceTravelLastProgressAt));
        std::memset(
            g_ConvergenceTravelRecoveryUntil,
            0,
            sizeof(g_ConvergenceTravelRecoveryUntil));
        std::memset(
            g_ConvergenceTravelHaveLocation,
            0,
            sizeof(g_ConvergenceTravelHaveLocation));
        std::memset(
            g_ConvergenceTravelLastGoals,
            0,
            sizeof(g_ConvergenceTravelLastGoals));
        std::memset(
            g_ConvergenceTravelLastRouteAt,
            0,
            sizeof(g_ConvergenceTravelLastRouteAt));
        std::memset(
            g_ConvergenceTravelHaveGoal,
            0,
            sizeof(g_ConvergenceTravelHaveGoal));
        g_ConvergenceTravelCounselorCount = 0;
    }

    bool IsConvergenceCounselorArmed(AActor* counselor)
    {
        for (int32_t i = 0;
            i < g_ConvergenceArmedCounselorCount;
            ++i)
        {
            if (g_ConvergenceArmedCounselors[i] == counselor)
                return true;
        }
        return false;
    }

    void RememberConvergenceCounselor(AActor* counselor)
    {
        if (!counselor || IsConvergenceCounselorArmed(counselor) ||
            g_ConvergenceArmedCounselorCount >=
                JasonAITargetCapacity)
        {
            return;
        }
        g_ConvergenceArmedCounselors[
            g_ConvergenceArmedCounselorCount++] = counselor;
    }

    bool IsStrongConvergenceCounselor(AActor* counselor)
    {
        if (!counselor)
            return false;
        // These are the shipped high-strength archetypes (Strength 6+), plus
        // the hero/guest variants used by Resurrected. This is evaluated only
        // when a weapon is granted, never in the movement or combat cadence.
        const char* strongTypes[] = {
            "Jock", "Tough", "Biker", "Tommy", "Hero",
            "Rob", "Julius", "Boxer", "Mark"
        };
        for (const char* type : strongTypes)
        {
            if (JasonAIObjectDerivesFromNameContaining(
                    reinterpret_cast<UObject*>(counselor),
                    type))
            {
                return true;
            }
        }
        return false;
    }

    bool GiveConvergenceMeleeWeapon(AActor* counselor)
    {
        const ULONGLONG now = GetTickCount64();
        if (!counselor ||
            IsCounselorInPostGrabTransition(counselor, now))
            return false;
        const bool strong = IsStrongConvergenceCounselor(counselor);
        UClass* weaponClass = strong
            ? g_CounselorConvergenceMeleeClass
            : g_CounselorConvergenceMacheteClass;
        // A map can omit one pickup type. Preserve convergence with the other
        // cached melee class instead of leaving that counselor unarmed.
        if (!weaponClass)
            weaponClass = g_CounselorConvergenceMeleeClass
                ? g_CounselorConvergenceMeleeClass
                : g_CounselorConvergenceMacheteClass;
        if (!weaponClass ||
            !Memory::IsReadable(
                weaponClass,
                sizeof(UClass)))
        {
            return false;
        }

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;
        constexpr uintptr_t RVA_GiveStartingItem = 0x002EF980;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) + RVA_GiveStartingItem);
        const uint8_t expected[] = {
            0x48, 0x85, 0xD2, 0x0F, 0x84, 0xDF, 0x00, 0x00,
            0x00, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48
        };
        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            return false;
        }

        reinterpret_cast<GiveStartingItemFn>(target)(
            counselor,
            weaponClass);
        Logger::Debug(
            std::string("18L-BK convergence weapon assigned | counselor=") +
            JasonAISafeName(reinterpret_cast<UObject*>(counselor)) +
            " | tier=" + (strong ? "strong" : "standard") +
            " | weapon=" +
            JasonAISafeName(reinterpret_cast<UObject*>(weaponClass)));
        return true;
    }

    void DriveCounselorSweaterConvergence(ULONGLONG now)
    {
        if (now < g_NextCounselorConvergenceAt)
            return;

        if (!g_CounselorConvergenceActive)
        {
            bool sweaterAcquired = g_PermanentHumanSweaterLatched;
            if (!sweaterAcquired)
            {
                // Before pickup, the old 1.5-second check validated the
                // entire roster on one game frame (31-63 ms in the live
                // startup trace). Inspect one pawn every 211 ms instead;
                // all seven are still covered within about 1.5 seconds.
                const int32_t count = (std::min)(
                    g_JasonAITargetCount, JasonAITargetCapacity);
                if (count > 0)
                {
                    const int32_t index =
                        g_SweaterPreconvergenceCursor % count;
                    g_SweaterPreconvergenceCursor = (index + 1) % count;
                    AActor* counselor = g_JasonAITargets[index];
                    sweaterAcquired =
                        IsValidatedLiveCounselorPawn(counselor) &&
                        HasPamelaSweater(counselor);
                }
            }
            if (!sweaterAcquired)
            {
                g_NextCounselorConvergenceAt = now + 211;
                return;
            }

            g_CounselorConvergenceActive = true;
            g_NextCounselorConvergenceAt = now;
            Logger::Success(
                "18L-BF sweater acquired: surviving counselor melee convergence armed");
        }

        // Keep every armed counselor in the native melee branch throughout
        // both the trance and JasonDeath kneel. The final interaction is
        // attack-enabled, so a real bot swing must remain eligible to acquire
        // it; stopping convergence when the first slash created JasonDeath_C
        // left the whole roster staring at a stationary Jason. Retire the
        // convergence controller only after the stock interaction commits.
        if (g_KillTeamFinalInteractionCommitted)
        {
            // JasonDeath owns the native final interaction. Restore every
            // counselor brain before handing control to that stock sequence.
            ResumeAllConvergenceCounselorBrains();
            g_NextCounselorConvergenceAt = now + 2003;
            return;
        }
        if (!g_CounselorBlackboardNameResolutionComplete)
        {
            g_NextCounselorConvergenceAt = now + 2003;
            return;
        }

        const int32_t count =
            (std::min)(g_JasonAITargetCount, JasonAITargetCapacity);
        if (count <= 0)
        {
            g_NextCounselorConvergenceAt = now + 4513;
            return;
        }

        // Prime the entire live AI roster when convergence first becomes
        // active.  The old cursor-only path armed one counselor per slice,
        // which allowed the remaining stock brains to keep looting or fleeing
        // until their turn arrived.  A single bounded pass gives every bot a
        // Jason target and one persistent native move request; subsequent
        // slices continue servicing one bot at a time, so this does not create
        // a per-frame ProcessEvent/path-command burst.
        FVector jasonLocation{};
        const bool haveJasonLocation =
            GetJasonAIActorLocation(g_JasonAIState.Jason, jasonLocation);

        // Do not refresh the whole roster here. MoveToLocation performs a
        // synchronous navigation rebuild, and the former 750 ms all-counselor
        // sweep stacked those rebuilds on one frame. The rotating service below
        // already republishes combat state and maintains each accepted path.
        int32_t rosterPrimed = 0;
        int32_t travelQueued = 0;
        for (int32_t i = 0; i < count; ++i)
        {
            AActor* counselor = g_JasonAITargets[i];
            if (!counselor || counselor == g_LocalCounselorTarget ||
                IsCounselorInPostGrabTransition(counselor, now) ||
                !IsValidatedLiveCounselorPawn(counselor) ||
                !IsAIControlledCounselor(counselor) ||
                IsConvergenceCounselorArmed(counselor))
            {
                continue;
            }

            GiveConvergenceMeleeWeapon(counselor);
            RememberConvergenceCounselor(counselor);
            SuppressConvergenceCounselorFear(counselor);

            UObject* controller = GetPawnControllerSafe(counselor);
            UObject* blackboard = controller
                ? GetCounselorBlackboardCached(controller)
                : nullptr;
            UObject* weapon = GetCounselorCurrentWeapon(counselor);
            if (!weapon && blackboard && g_SCWeaponNameIndex >= 0)
            {
                weapon = GetBlackboardObject(blackboard, g_SCWeaponNameIndex);
            }
            if (!weapon)
            {
                continue;
            }

            ArmKillTeamCounselorCombat(counselor);
            ++rosterPrimed;

            if (!controller || !haveJasonLocation)
            {
                continue;
            }

            FVector counselorLocation{};
            if (!GetJasonAIActorLocation(counselor, counselorLocation))
            {
                continue;
            }
            const float dx = counselorLocation.X - jasonLocation.X;
            const float dy = counselorLocation.Y - jasonLocation.Y;
            const float distanceSquared = dx * dx + dy * dy;
            if (!std::isfinite(distanceSquared) ||
                distanceSquared <= 300.0f * 300.0f)
            {
                continue;
            }

            if (SetCounselorBrainTickEnabled(counselor, false))
            {
                RememberConvergenceTravelCounselor(counselor);
                // Queue the bot for the rotating route service instead of
                // issuing every initial path synchronously on this frame.
                ++travelQueued;
                Logger::Success(
                    "18L-BH counselor exclusive Jason travel engaged | pawn=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(counselor)));
            }
        }
        if (rosterPrimed > 0)
        {
            Logger::Success(
                "18L-BF counselor convergence roster primed | armed=" +
                std::to_string(rosterPrimed) +
                " | staggeredPaths=" + std::to_string(travelQueued));
        }

        AActor* selected = nullptr;
        bool firstArm = false;
        for (int32_t attempt = 0; attempt < count; ++attempt)
        {
            const int32_t index =
                (g_CounselorConvergenceCursor + attempt) % count;
            AActor* counselor = g_JasonAITargets[index];
            if (!counselor || counselor == g_LocalCounselorTarget ||
                IsCounselorInPostGrabTransition(counselor, now) ||
                !IsValidatedLiveCounselorPawn(counselor) ||
                !IsAIControlledCounselor(counselor))
            {
                continue;
            }
            if (!IsConvergenceCounselorArmed(counselor))
            {
                selected = counselor;
                firstArm = true;
                g_CounselorConvergenceCursor = (index + 1) % count;
                break;
            }
            if (!selected)
            {
                selected = counselor;
                g_CounselorConvergenceCursor = (index + 1) % count;
            }
        }

        if (!selected)
        {
            g_NextCounselorConvergenceAt = now + 4513;
            return;
        }

        if (firstArm)
        {
            GiveConvergenceMeleeWeapon(selected);
            RememberConvergenceCounselor(selected);
        }

        // Service exactly one counselor per slice. With the 333 ms cadence
        // below, a full six-bot roster is refreshed in about 2.00 s. Their
        // behavior brains remain suspended during travel, so the stock
        // fear/loot service cannot reclaim them between these staggered paths.
        SuppressConvergenceCounselorFear(selected);

        UObject* controller = GetPawnControllerSafe(selected);
        UObject* blackboard = controller
            ? GetCounselorBlackboardCached(controller)
            : nullptr;
        UObject* weapon = GetCounselorCurrentWeapon(selected);
        if (!weapon && blackboard && g_SCWeaponNameIndex >= 0)
            weapon = GetBlackboardObject(blackboard, g_SCWeaponNameIndex);
        if (!weapon && !firstArm)
        {
            GiveConvergenceMeleeWeapon(selected);
            weapon = GetCounselorCurrentWeapon(selected);
            if (!weapon && blackboard && g_SCWeaponNameIndex >= 0)
                weapon = GetBlackboardObject(
                    blackboard,
                    g_SCWeaponNameIndex);
        }

        if (weapon)
        {
            // Publish fight state while the brain is still suspended so its
            // first resumed behavior-tree frame sees only the Jason branch.
            ArmKillTeamCounselorCombat(selected);

            FVector counselorLocation{};
            FVector jasonLocation{};
            const bool haveLocations =
                GetJasonAIActorLocation(selected, counselorLocation) &&
                GetJasonAIActorLocation(
                    g_JasonAIState.Jason,
                    jasonLocation);
            const float dx = haveLocations
                ? counselorLocation.X - jasonLocation.X
                : 0.0f;
            const float dy = haveLocations
                ? counselorLocation.Y - jasonLocation.Y
                : 0.0f;
            const float distanceSquared = dx * dx + dy * dy;
            const bool wasTraveling =
                FindConvergenceTravelCounselor(selected) >= 0;
            // Hysteresis avoids toggling the brain at the edge of melee range:
            // begin exclusive travel beyond 3 m, but do not hand control back
            // until the bot is within 2.2 m of Jason. The earlier 3.25 m
            // release allowed the stock tree to flee before melee was viable.
            const bool shouldTravel = haveLocations &&
                std::isfinite(distanceSquared) &&
                distanceSquared >
                    (wasTraveling
                        ? 220.0f * 220.0f
                        : 300.0f * 300.0f);

            if (shouldTravel)
            {
                if (!wasTraveling)
                {
                    if (SetCounselorBrainTickEnabled(selected, false))
                    {
                        RememberConvergenceTravelCounselor(selected);
                        Logger::Success(
                            "18L-BH counselor exclusive Jason travel engaged | pawn=" +
                            JasonAISafeName(
                                reinterpret_cast<UObject*>(selected)));
                    }
                    else if (firstArm)
                    {
                        Logger::Error(
                            "18L-BH counselor brain suspension unavailable; direct travel remains best-effort | pawn=" +
                            JasonAISafeName(
                                reinterpret_cast<UObject*>(selected)));
                    }
                }
                bool recoveryPulse = false;
                const int32_t travelIndex =
                    FindConvergenceTravelCounselor(selected);
                if (travelIndex >= 0)
                {
                    if (g_ConvergenceTravelRecoveryUntil[travelIndex] != 0)
                    {
                        if (now <
                            g_ConvergenceTravelRecoveryUntil[travelIndex])
                        {
                            recoveryPulse = true;
                        }
                        else
                        {
                            SetCounselorBrainTickEnabled(selected, false);
                            g_ConvergenceTravelRecoveryUntil[travelIndex] = 0;
                            g_ConvergenceTravelHaveLocation[travelIndex] = false;
                            g_ConvergenceTravelLastProgressAt[travelIndex] = now;
                        }
                    }

                    if (!recoveryPulse)
                    {
                        const FVector& previous =
                            g_ConvergenceTravelLastLocations[travelIndex];
                        const float moveX = counselorLocation.X - previous.X;
                        const float moveY = counselorLocation.Y - previous.Y;
                        const bool progressed =
                            !g_ConvergenceTravelHaveLocation[travelIndex] ||
                            moveX * moveX + moveY * moveY > 80.0f * 80.0f;
                        if (progressed)
                        {
                            g_ConvergenceTravelLastLocations[travelIndex] =
                                counselorLocation;
                            g_ConvergenceTravelLastProgressAt[travelIndex] = now;
                            g_ConvergenceTravelHaveLocation[travelIndex] = true;
                        }
                        else if (g_ConvergenceTravelLastProgressAt[travelIndex] != 0 &&
                            now >=
                                g_ConvergenceTravelLastProgressAt[travelIndex] +
                                    3500)
                        {
                            // A closed door or smart-link traversal can require
                            // the counselor behavior tree. Give only the stuck
                            // pawn a short native-navigation pulse, then reclaim
                            // exclusive travel; no roster scan or group burst.
                            SetCounselorBrainTickEnabled(selected, true);
                            g_ConvergenceTravelRecoveryUntil[travelIndex] =
                                now + 1001;
                            recoveryPulse = true;
                            Logger::Debug(
                                "18L-BI counselor travel obstruction recovery pulse | pawn=" +
                                JasonAISafeName(
                                    reinterpret_cast<UObject*>(selected)));
                        }
                    }
                }

                if (!recoveryPulse)
                {
                    const int32_t travelIndex =
                        FindConvergenceTravelCounselor(selected);
                    bool refreshRoute = travelIndex < 0;
                    if (travelIndex >= 0)
                    {
                        const FVector& previousGoal =
                            g_ConvergenceTravelLastGoals[travelIndex];
                        const float goalX = jasonLocation.X - previousGoal.X;
                        const float goalY = jasonLocation.Y - previousGoal.Y;
                        const bool targetMoved =
                            g_ConvergenceTravelHaveGoal[travelIndex] &&
                            goalX * goalX + goalY * goalY > 400.0f * 400.0f;
                        refreshRoute =
                            !g_ConvergenceTravelHaveGoal[travelIndex] ||
                            targetMoved ||
                            now >=
                                g_ConvergenceTravelLastRouteAt[travelIndex] +
                                    6001;
                    }

                    if (refreshRoute &&
                        IssueAIMoveToLocationOnGameThread(
                            controller,
                            jasonLocation,
                            100.0f,
                            "SweaterConvergenceJason") &&
                        travelIndex >= 0)
                    {
                        g_ConvergenceTravelLastGoals[travelIndex] =
                            jasonLocation;
                        g_ConvergenceTravelLastRouteAt[travelIndex] = now;
                        g_ConvergenceTravelHaveGoal[travelIndex] = true;
                    }
                }
            }
            else if (wasTraveling)
            {
                // The direct path has delivered the bot. Resume only here so
                // the mature stock melee animation/attack loop can take over.
                SetCounselorBrainTickEnabled(selected, true);
                ForgetConvergenceTravelCounselor(selected);
                Logger::Success(
                    "18L-BH counselor reached Jason; native melee brain resumed | pawn=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(selected)));
            }
            if (firstArm)
            {
                Logger::Success(
                    "18L-BF counselor armed and sent to Jason | pawn=" +
                    JasonAISafeName(
                        reinterpret_cast<UObject*>(selected)));
            }
        }
        else if (blackboard)
        {
            // If the cached axe was not present yet, use the stock weapon-seek
            // branch. A later slow pass promotes the bot to melee combat.
            SetBlackboardObject(
                blackboard,
                g_JasonCharacterNameIndex,
                reinterpret_cast<UObject*>(g_JasonAIState.Jason));
            SetBlackboardBool(
                blackboard,
                g_SeekWeaponWhileFleeingNameIndex,
                true);
            SetBlackboardBool(
                blackboard,
                g_ShouldHideNameIndex,
                false);
        }

        // Always rotate one bot at a time. This caps synchronous navigation at
        // three requests per second for the entire roster and avoids the
        // rhythmic frame-time spike observed in the live trace.
        g_NextCounselorConvergenceAt = now + 333;
    }

    void ArmKillTeamAxeTravel(AActor* helper)
    {
        if (!helper ||
            g_ShouldFleeKillerNameIndex < 0 ||
            g_ShouldFightBackNameIndex < 0 ||
            g_ShouldArmedFightBackNameIndex < 0 ||
            g_ShouldMeleeFightBackNameIndex < 0 ||
            g_SeekWeaponWhileFleeingNameIndex < 0)
        {
            return;
        }

        UObject* controller = GetPawnControllerSafe(helper);
        UObject* blackboard = controller
            ? GetCounselorBlackboardCached(controller)
            : nullptr;
        if (!blackboard)
            return;

        // The kill-team route owns Tommy while he travels to the fixed axe.
        // Disable the stock fight/flee/weapon-looting branches that otherwise
        // replace our native MoveTo every behavior-tree tick.
        SetBlackboardBool(
            blackboard,
            g_ShouldFightBackNameIndex,
            false);
        SetBlackboardBool(
            blackboard,
            g_ShouldArmedFightBackNameIndex,
            false);
        SetBlackboardBool(
            blackboard,
            g_ShouldMeleeFightBackNameIndex,
            false);
        SetBlackboardBool(
            blackboard,
            g_SeekWeaponWhileFleeingNameIndex,
            false);
        SetBlackboardBool(
            blackboard,
            g_ShouldFleeKillerNameIndex,
            false);
    }

    void HoldKillTeamCounselorForFinalAction(AActor* helper)
    {
        if (!helper ||
            g_ShouldFleeKillerNameIndex < 0 ||
            g_ShouldFightBackNameIndex < 0 ||
            g_ShouldArmedFightBackNameIndex < 0 ||
            g_ShouldMeleeFightBackNameIndex < 0 ||
            g_SeekWeaponWhileFleeingNameIndex < 0)
        {
            return;
        }
        UObject* controller = GetPawnControllerSafe(helper);
        UObject* blackboard = controller
            ? GetCounselorBlackboardCached(controller)
            : nullptr;
        if (!blackboard)
            return;

        // JasonDeath is attack-enabled in counselor mode. Preserve the native
        // melee branch even for the selected finisher so a real swing can
        // acquire the context when the synthetic action request is ignored.
        // Only flee, hide, and loot remain suppressed.
        SetBlackboardObject(
            blackboard,
            g_JasonCharacterNameIndex,
            reinterpret_cast<UObject*>(g_JasonAIState.Jason));
        SetBlackboardBool(blackboard, g_ShouldFightBackNameIndex, true);
        SetBlackboardBool(blackboard, g_ShouldArmedFightBackNameIndex, true);
        SetBlackboardBool(blackboard, g_ShouldMeleeFightBackNameIndex, true);
        SetBlackboardBool(blackboard, g_SeekWeaponWhileFleeingNameIndex, false);
        SetBlackboardBool(blackboard, g_ShouldFleeKillerNameIndex, false);
        if (g_ShouldHideNameIndex >= 0)
            SetBlackboardBool(blackboard, g_ShouldHideNameIndex, false);
        if (g_ShouldOrientTowardKillerNameIndex >= 0)
        {
            SetBlackboardBool(
                blackboard,
                g_ShouldOrientTowardKillerNameIndex,
                true);
        }
    }

    bool DispatchSweaterAbility(AActor* counselor)
    {
        if (!counselor || !counselor->Class)
            return false;

        UObject* candidates[] =
        {
            reinterpret_cast<UObject*>(counselor),
            ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(counselor),
                "SweaterAbility"),
            ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(counselor),
                "ActiveAbility")
        };
        for (UObject* candidate : candidates)
        {
            if (!candidate || !candidate->Class ||
                !Memory::IsReadable(candidate, sizeof(UObject)))
            {
                continue;
            }
            UFunction* useAbility = FindFunctionInHierarchyByName(
                candidate->Class,
                "SERVER_UseAbility");
            if (!useAbility)
                continue;
            if (SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(candidate),
                    candidate,
                    useAbility,
                    nullptr))
            {
                return true;
            }
        }
        return false;
    }

    bool IsJasonNativelyStunned(AActor* jason)
    {
        if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
            return false;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        using IsStunnedFn = bool(__fastcall*)(AActor*);
        IsStunnedFn isStunned = reinterpret_cast<IsStunnedFn>(
            reinterpret_cast<uintptr_t>(module) + 0x002F00E0);
        return Memory::IsReadable(reinterpret_cast<void*>(isStunned), 1) &&
            isStunned(jason);
    }

    void RestoreRepeatJasonDeathOpportunityIfNeeded(ULONGLONG now)
    {
        if (g_RepeatJasonKillStanceAt == 0 ||
            now < g_RepeatJasonKillStanceAt)
        {
            return;
        }

        AActor* jason = g_JasonAIState.Jason;
        AActor* existingContext = nullptr;
        UObject* existingComponent = nullptr;
        if (TryGetValidatedJasonDeathContext(
                jason,
                existingContext,
                existingComponent,
                now))
        {
            // Stock recreated the kneel itself; never duplicate it. Publish
            // the slash-to-finish gates immediately so a second strike that
            // follows the visible kneel cannot lose a race with this cadence.
            if (existingComponent)
            {
                WriteReflectedBoolByte(
                    existingComponent,
                    "bCanAttackToInteract",
                    true);
                WriteReflectedBoolByte(
                    existingComponent,
                    "bAlwaysAllowInteractionFromAI",
                    true);
            }
            g_RepeatJasonKillStanceAt = 0;
            g_RepeatJasonKillStanceDeadline = 0;
            g_RepeatJasonKillStanceAttempts = 0;
            g_KillTeamFinalContextUntil = now + 60000;
            return;
        }

        if (!jason || !jason->Class ||
            now > g_RepeatJasonKillStanceDeadline ||
            g_RepeatJasonKillStanceAttempts >= 4)
        {
            const bool attemptedWithoutContext =
                g_RepeatJasonKillStanceAttempts >= 4;
            g_RepeatJasonKillStanceAt = 0;
            g_RepeatJasonKillStanceDeadline = 0;
            if (g_PamelaTranceKneelRequested &&
                g_PamelaTranceKillWindowUntil > now + 2500)
            {
                // A hit was accepted, but stock never created JasonDeath_C.
                // Give it one last brief latent-frame grace, then release and
                // restore the sweater instead of trapping the player for the
                // remaining artificial minute.
                g_PamelaTranceKillWindowUntil = now + 2500;
            }
            if (attemptedWithoutContext)
            {
                Logger::Error(
                    "18L-BE bounded Jason kneel retries produced no death context; exclusive kill window remains available for another strike");
            }
            return;
        }

        // Do not bypass the mask prerequisite or invent a death opportunity.
        // bMaskOn is a reflected SCKillerCharacter field; resolve it only in
        // this rare repeat-use window.
        if (!FindPropertyInHierarchyByName(jason->Class, "bMaskOn") ||
            ReadReflectedBoolByte(
                reinterpret_cast<UObject*>(jason),
                "bMaskOn"))
        {
            g_RepeatJasonKillStanceAt = 0;
            g_RepeatJasonKillStanceDeadline = 0;
            return;
        }

        // The September 11 route called EnterKillStance only while the
        // authored stun was actually active. Repeating it against an upright
        // Jason reports a dispatched RPC but never creates JasonDeath_C.
        if (!IsJasonNativelyStunned(jason))
        {
            g_RepeatJasonKillStanceAt = now + 750;
            return;
        }

        // The known-good loose-files build retried this authored transition
        // when the first call did not publish JasonDeath_C. Keep that proven
        // behavior bounded to four calls inside the exclusive kill lane: no
        // general AI, car, grab, or combat work can compete with it.
        UFunction* enterKillStance = FindFunctionInHierarchyByName(
            jason->Class,
            "SERVER_EnterKillStance");
        if (!enterKillStance)
        {
            enterKillStance = FindFunctionInHierarchyByName(
                jason->Class,
                "EnterKillStance");
        }
        const bool dispatched = enterKillStance && SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(jason),
            jason,
            enterKillStance,
            nullptr);
        ++g_RepeatJasonKillStanceAttempts;
        g_RepeatJasonKillStanceAt = now + 750;
        Logger::Success(
            std::string("18L-BE Jason kneel retry dispatched through known-good stock stance route | attempt=") +
            std::to_string(g_RepeatJasonKillStanceAttempts) +
            " | dispatched=" + (dispatched ? "true" : "false"));
    }

    void RecoverOrphanJasonStunIfNeeded(
        AActor* human,
        AActor* jason,
        ULONGLONG now)
    {
        // A real sweater/mask transition publishes JasonDeath_C at +0x12D8
        // and is handled above at the responsive final-action cadence. An
        // ordinary stun that remains active without that context is allowed
        // twelve seconds to finish naturally, then released once so an
        // external instant-stun modifier cannot strand Jason indefinitely.
        if (!IsJasonNativelyStunned(jason))
        {
            g_OrphanJasonStunStartedAt = 0;
            return;
        }
        if (g_OrphanJasonStunStartedAt == 0)
        {
            g_OrphanJasonStunStartedAt = now;
            return;
        }

        const bool pamelaOpportunityActive =
            (g_PamelaTranceKillWindowUntil != 0 &&
                now <= g_PamelaTranceKillWindowUntil) ||
            (g_RepeatJasonKillStanceDeadline != 0 &&
                now <= g_RepeatJasonKillStanceDeadline);
        if (pamelaOpportunityActive)
        {
            // Do not let generic anti-freeze cleanup cancel the deliberately
            // widened Pamela opportunity. The same cleanup becomes eligible
            // immediately after this bounded window expires.
            return;
        }
        if (now - g_OrphanJasonStunStartedAt < 12000)
            return;

        UFunction* endStun = jason && jason->Class
            ? FindFunctionInHierarchyByName(jason->Class, "EndStun")
            : nullptr;
        const bool released = endStun && SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(jason),
            jason,
            endStun,
            nullptr);

        g_OrphanJasonStunStartedAt = 0;
        g_KillTeamFinalContextUntil = 0;
        g_KillTeamFinalRecoveryCooldownUntil = now + 5000;
        g_KillTeamFinalInteractionDispatched = false;
        g_KillTeamFinalInteractionPending = false;
        g_KillTeamFinalInteractionAttempts = 0;
        g_KillTeamFinalInteractionStartedAt = 0;
        g_KillTeamFinalSequenceStartedAt = 0;
        g_KillTeamSweaterUseDispatched = false;
        g_NextSweaterUseAt = now + 1000;
        if (g_PermanentHumanSweaterLatched &&
            human == g_PermanentHumanSweaterCarrier)
        {
            RearmPermanentHumanSweater(human, true);
        }
        g_JasonAIState.Target = nullptr;
        ResetStuckSamplingAfterNativeInteraction(now);
        Logger::Error(
            std::string("18L-BB orphan Jason stun released without JasonDeath context | released=") +
            (released ? "true" : "false"));
    }

    void AbortFailedJasonKillSequence(
        AActor* jason,
        AActor* killObject,
        ULONGLONG now)
    {
        // Once the native paired kill has consumed its JasonDeath context,
        // never run recovery cleanup: EndStun would revive the dead Jason.
        if (g_KillTeamFinalInteractionCommitted)
            return;

        // A real Jason death event/flag can outlive the brief counselor
        // interaction lock. Never destroy its context or restart combat.
        if (g_NativeJasonDeathCinematicObservedAt != 0)
            return;

        const bool retryJasonDeath =
            g_PermanentHumanSweaterLatched &&
            jason &&
            IsJasonNativelyStunned(jason);
        bool destroyedContext = false;
        if (killObject && killObject->Class &&
            Memory::IsReadable(killObject, sizeof(UObject)))
        {
            UFunction* destroyActor = FindFunctionInHierarchyByName(
                killObject->Class,
                "K2_DestroyActor");
            destroyedContext = destroyActor &&
                SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(killObject),
                    killObject,
                    destroyActor,
                    nullptr);
        }

        if (jason && Memory::IsReadable(jason, 0x12E0))
        {
            AActor** finalContextField = reinterpret_cast<AActor**>(
                reinterpret_cast<uintptr_t>(jason) + 0x12D8);
            if (*finalContextField == killObject)
                *finalContextField = nullptr;

            // Final v11 kept the real native stun alive after a failed final
            // input so the stock server route could recreate JasonDeath_C.
            // Only a non-sweater failure is released back to ordinary AI.
            UFunction* endStun = jason->Class
                ? FindFunctionInHierarchyByName(jason->Class, "EndStun")
                : nullptr;
            if (endStun && !retryJasonDeath)
            {
                SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(jason),
                    jason,
                    endStun,
                    nullptr);
            }
        }

        g_KillTeamFinalContextUntil = 0;
        g_KillTeamFinalRecoveryCooldownUntil = now + 10000;
        g_KillTeamFinalInteractionDispatched = false;
        g_KillTeamFinalInteractionPending = false;
        g_KillTeamFinalInteractionAttempts = 0;
        g_KillTeamFinalInteractionStartedAt = 0;
        g_KillTeamFinalSequenceStartedAt = 0;
        g_NativeJasonDeathEventObserved.store(
            false, std::memory_order_release);
        g_NativeJasonDeathMatchEndAt = 0;
        g_KillTeamFinalMoveFailures = 0;
        g_KillTeamFinalRepositionAttempted = false;
        g_KillTeamPendingFinalContext = nullptr;
        g_KillTeamPendingFinalComponent = nullptr;
        g_KillTeamPendingFinalFinisher = nullptr;
        g_LastAcceptedFinalContext = nullptr;
        g_LastAcceptedFinalComponent = nullptr;
        g_LastAcceptedFinalKillComponent = nullptr;
        g_LastUniversalFinalEligibilityContext = nullptr;
        g_LastUniversalFinalEligibilityFinisher = nullptr;
        g_LastUniversalFinalEligibilityAcceptedAt = 0;
        g_PamelaTranceKillWindowUntil = 0;
        g_PamelaTranceJasonHealthBaselineValid = false;
        g_PamelaTranceDamageEventObserved.store(
            false,
            std::memory_order_release);
        g_PamelaTranceKneelRequested = false;
        g_PamelaTranceFinalSlashAccepted = false;
        g_PamelaTrancePreferredFinisher = nullptr;
        g_PamelaTrancePreferredFinisherUntil = 0;
        g_RepeatJasonKillStanceAt = 0;
        g_RepeatJasonKillStanceDeadline = 0;
        g_RepeatJasonKillStanceAttempts = 0;
        g_PermanentHumanSweaterActivationUntil = 0;
        g_PermanentHumanSweaterAttackUnlockAt = 0;
        g_KillTeamSweaterUseDispatched = false;
        g_NextSweaterUseAt = now + 1000;
        if (retryJasonDeath)
        {
            g_RepeatJasonKillStanceAt = now + 750;
            g_RepeatJasonKillStanceDeadline = now + 12000;
            g_RepeatJasonKillStanceAttempts = 0;
            // Keep the killer-route tick in the final-interaction lane while
            // the native kneel is being re-armed.  Without this short hold,
            // the next controller tick can immediately hand Jason back to
            // vehicle/hiding/grab AI during the gap between destroying the
            // failed JasonDeath_C and recreating the stock stance.
            g_KillTeamFinalContextUntil = now + 12000;
            g_KillTeamFinalRecoveryCooldownUntil = now + 500;
            Logger::Success(
                "18L-BE failed Jason kill retained native stun; retrying JasonDeath stance instead of restoring chase AI");
        }
        if (g_PermanentHumanSweaterLatched &&
            g_PermanentHumanSweaterCarrier &&
            Memory::IsReadable(
                g_PermanentHumanSweaterCarrier,
                sizeof(UObject)))
        {
            // A failed Tommy final action must return the stock kill setup to
            // a usable state. Re-arm the already-earned human sweater here,
            // once per failed sequence, instead of polling or auto-activating.
            // Restore the native two-way ability contract immediately after
            // EndStun, then retain the bounded delayed passes because stock
            // cleanup can clear the HUD availability again a few frames
            // later.  Waiting only for the delayed path left the player
            // visibly wearing the sweater without a usable Y action.
            const bool immediateRearm = RearmPermanentHumanSweater(
                g_PermanentHumanSweaterCarrier,
                true);
            g_PermanentHumanSweaterRearmAt = now + 2000;
            g_PermanentHumanSweaterRearmAttempts = 0;
            g_PermanentHumanSweaterRearmSucceeded = immediateRearm;
            Logger::Success(
                std::string("18L-BA failed Jason kill released; clean human sweater rearm scheduled | immediate=") +
                (immediateRearm ? "true" : "false"));
        }
        g_JasonAIState.Target = nullptr;
        ResetStuckSamplingAfterNativeInteraction(now);
        Logger::Error(
            std::string("18L-AY failed Jason-kill interaction released; normal AI restored | contextDestroyed=") +
            (destroyedContext ? "true" : "false") +
            " | retryStance=" + (retryJasonDeath ? "true" : "false"));
    }

    bool DriveCounselorFinalKill(
        AActor* finisher,
        bool allowAIMovement,
        ULONGLONG now)
    {
        AActor* jason = g_JasonAIState.Jason;
        UObject* manager = GetCounselorInteractionManager(finisher);
        if (!jason || !manager)
            return false;

        // EnterKillStance creates the stock JasonDeath context actor here
        // after the sweater/mask prerequisites are satisfied.
        AActor* killObject = nullptr;
        UObject* component = nullptr;
        if (!TryGetValidatedJasonDeathContext(
                jason,
                killObject,
                component,
                now))
            return false;

        // JasonDeath_C publishes two cooperating components. The counselor's
        // interaction manager must press the ordinary Interactable component;
        // ContextKillComponent is consulted internally for the Hunter/weapon
        // predicate and owns the paired animation. Dispatching AttemptInteract
        // directly to ContextKillComponent can pass its predicate but never
        // acquire the counselor-manager lock, producing a false kill timeout.
        UObject* finalActionComponent = component;

        // A forced F4 camera must relinquish the stock spectator pawn before
        // JasonDeath_C begins its paired sequence. Otherwise the spectator
        // camera and cinematic camera alternate ownership, producing a dark,
        // disorienting view even though the kill itself succeeds.
        ArmJasonDeathVisualNormalization(now);
        ReleaseJasonSpectatorOverrideForCinematic("JasonDeath-context");

        // The stock death-context owns both participants from this point.
        // Keep custom Jason chase and helper behavior out until it completes.
        g_KillTeamFinalContextUntil = now + 15000;

        // The death context is transient and already validated against
        // JasonDeath_C. Let a regular melee hit feed the same interaction path
        // for both the local counselor and AI finishers. The stock context
        // predicate still enforces its weapon, range and live-state rules.
        WriteReflectedBoolByte(
            finalActionComponent,
            "bCanAttackToInteract",
            true);

        if (IsAIControlledCounselor(finisher))
        {
            // JasonDeath_C is authored for a locally-controlled Tommy input.
            // In counselor mode any nearby living bot may be the finisher;
            // relax only the validated death component's two native gates so
            // its melee/action path can acquire the same stock interaction
            // lock.  The context is transient and owns the cinematic once
            // locked, so this cannot affect ordinary world interactables.
            WriteReflectedBoolByte(
                finalActionComponent,
                "bAlwaysAllowInteractionFromAI",
                true);
        }

        UObject** locked = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(manager) + 0x230);
        if (Memory::IsReadable(locked, sizeof(UObject*)) && *locked)
        {
            const bool exactFinalLock = IsFinalContextLockMatch(
                *locked,
                killObject,
                finalActionComponent);
            if (exactFinalLock &&
                killObject == g_KillTeamPendingFinalContext &&
                finalActionComponent == g_KillTeamPendingFinalComponent &&
                !g_KillTeamFinalInteractionDispatched &&
                (g_KillTeamFinalInteractionPending ||
                    g_KillTeamFinalInteractionAttempts > 0))
            {
                Logger::Success(
                    "18L-AW counselor final interaction confirmed by native lock | finisher=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(finisher)));
                g_KillTeamFinalInteractionDispatched = true;
                g_KillTeamFinalInteractionPending = false;
            }
            if (!exactFinalLock &&
                g_KillTeamFinalInteractionAttempts > 0 &&
                g_KillTeamFinalInteractionStartedAt != 0 &&
                now - g_KillTeamFinalInteractionStartedAt >= 1250)
            {
                // A stale loot/weapon/attack interaction can occupy the
                // counselor manager just after the final-action input. The old
                // path waited 22 seconds with that unrelated lock, destroyed
                // JasonDeath_C, then handed a visually kneeling Jason back to
                // grab AI. Cancel only the already-validated non-final lock;
                // the next bounded attempt can then acquire JasonDeath_C.
                CancelKillTeamHelperNonFinalInteraction(finisher, now);
                if (Memory::IsReadable(locked, sizeof(UObject*)) && !*locked)
                {
                    g_KillTeamFinalInteractionPending = false;
                    g_NextFinalKillInteractAt = now + 100;
                }
            }
            if (!exactFinalLock &&
                g_KillTeamFinalSequenceStartedAt != 0 &&
                now - g_KillTeamFinalSequenceStartedAt >= 30000)
            {
                AbortFailedJasonKillSequence(jason, killObject, now);
            }
            g_KillTeamHelperNativeBusyUntil = now + 2500;
            return true;
        }
        if (g_KillTeamFinalInteractionDispatched)
            return true;

        if (g_KillTeamFinalSequenceStartedAt != 0 &&
            now - g_KillTeamFinalSequenceStartedAt >= 30000)
        {
            AbortFailedJasonKillSequence(jason, killObject, now);
            return true;
        }

        // September 11 used short bounded input bursts. The packed route
        // removed that guard and the live trace sent 52 inputs to one manager
        // without ever acquiring a lock. Allow the interaction/animation to
        // settle, then try a newly selected finisher instead of spamming it.
        if (g_KillTeamFinalInteractionAttempts >= 4)
        {
            if (g_KillTeamFinalInteractionStartedAt != 0 &&
                now - g_KillTeamFinalInteractionStartedAt < 4000)
            {
                return true;
            }
            g_KillTeamFinalInteractionAttempts = 0;
            g_KillTeamFinalInteractionStartedAt = 0;
            g_KillTeamFinalInteractionPending = false;
            g_KillTeamPendingFinalFinisher = nullptr;
        }

        // AttemptInteract is a void native input dispatch. Confirm it through
        // the counselor interaction-manager lock and keep retrying for the
        // entire validated kneel window. The old four-attempt/eight-second
        // pause routinely missed the animation frame in which the context
        // becomes lockable.

        FVector* interactionLocation = reinterpret_cast<FVector*>(
            reinterpret_cast<uintptr_t>(finalActionComponent) +
            Offsets::Scene_ComponentToWorld +
            Offsets::FTransform_Translation);
        FVector finisherLocation{};
        if (!Memory::IsReadable(interactionLocation, sizeof(FVector)) ||
            !GetJasonAIActorLocation(finisher, finisherLocation))
        {
            return true;
        }
        FVector jasonLocation{};
        if (!GetJasonAIActorLocation(jason, jasonLocation))
            return true;
        // Dispatch from Jason's visible body. The validated death component's
        // transform can be animation-offset, which previously discarded a
        // detected second slash before AttemptInteract was ever called.
        const float dx = jasonLocation.X - finisherLocation.X;
        const float dy = jasonLocation.Y - finisherLocation.Y;
        const float distanceSquared = dx * dx + dy * dy;
        if (!std::isfinite(distanceSquared))
            return true;
        (void)allowAIMovement;
        // The authored interaction requires near-melee overlap. A 500 cm
        // synthetic press was eligible in the hook but never lockable in the
        // stock manager. A real slash lands well inside this bound.
        constexpr float dispatchDistanceCm = 200.0f;
        if (distanceSquared > dispatchDistanceCm * dispatchDistanceCm)
        {
            // Convergence already owns a persistent path to Jason's visible
            // body. A second MoveTo toward the animation-offset interaction
            // component made the two routes fight each other and prevented
            // either from reaching dispatch range. Wait for convergence.
            return true;
        }

        if (now < g_NextFinalKillInteractAt)
            return true;

        // StopMovement on the locally possessed counselor cancels the
        // player's movement on every retry and prevents the follow-up slash.
        // Only AI finishers need their path paused for the native action.
        UObject* controller = finisher != g_LocalCounselorTarget &&
            IsAIControlledCounselor(finisher)
            ? GetPawnControllerSafe(finisher)
            : nullptr;
        UFunction* stopMovement = controller && controller->Class
            ? FindFunctionInHierarchyByName(
                controller->Class,
                "StopMovement")
            : nullptr;
        if (stopMovement)
        {
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(controller),
                controller,
                stopMovement,
                nullptr);
        }

        HMODULE module = GetModuleHandle(nullptr);
        uintptr_t managerVTable = *reinterpret_cast<uintptr_t*>(manager);
        uintptr_t* attemptSlot = reinterpret_cast<uintptr_t*>(
            managerVTable + 0x430);
        const uintptr_t expected = module
            ? reinterpret_cast<uintptr_t>(module) + 0x0025AC50
            : 0;
        if (!managerVTable ||
            !Memory::IsReadable(attemptSlot, sizeof(uintptr_t)) ||
            *attemptSlot != expected)
        {
            return false;
        }
        // The native third argument is an interaction-method bitmask, not a
        // zero-based enum. Disassembly of +0x25AC50 confirms it ANDs this byte
        // with SCInteractComponent::CanInteractWith's return value. Physical
        // controller A uses bit 0 (value 1); value 0 can report eligibility
        // through the hook but can never enter the accepted branch.
        constexpr uint8_t PrimaryActionMethod = 1;
        reinterpret_cast<AttemptInteractFn>(*attemptSlot)(
            manager,
            finalActionComponent,
            PrimaryActionMethod,
            true);
        if (g_KillTeamFinalInteractionStartedAt == 0)
            g_KillTeamFinalInteractionStartedAt = now;
        ++g_KillTeamFinalInteractionAttempts;
        g_KillTeamFinalInteractionPending = true;
        if (g_KillTeamPendingFinalContext != killObject ||
            !g_KillTeamPendingFinalFinisher)
        {
            g_KillTeamPendingFinalContext = killObject;
            g_KillTeamPendingFinalComponent = finalActionComponent;
            g_KillTeamPendingFinalFinisher = finisher;
        }
        g_KillTeamHelperNativeBusyUntil = now + 500;
        g_NextFinalKillInteractAt = now + 350;

        const bool lockedImmediately =
            Memory::IsReadable(locked, sizeof(UObject*)) && *locked;
        if (lockedImmediately && IsFinalContextLockMatch(
                *locked,
                killObject,
                finalActionComponent))
        {
            g_KillTeamFinalInteractionDispatched = true;
            g_KillTeamFinalInteractionPending = false;
            g_KillTeamHelperNativeBusyUntil = now + 3000;
            Logger::Success(
                "18L-AW counselor final interaction accepted immediately by native lock | finisher=" +
                JasonAISafeName(reinterpret_cast<UObject*>(finisher)));
        }
        else
        {
            Logger::Debug(
                "18L-AW counselor pressed native final interaction; awaiting lock | attempt=" +
                std::to_string(g_KillTeamFinalInteractionAttempts) +
                " | method=1" +
                " | finisher=" +
                JasonAISafeName(reinterpret_cast<UObject*>(finisher)));
        }
        return true;
    }

    void ResetKillTeamRoute(KillTeamRoute route, AActor* helper)
    {
        if (g_KillTeamRoute == route && g_KillTeamHelper == helper)
            return;
        g_KillTeamRoute = route;
        g_KillTeamHelper = helper;
        g_KillTeamSweater = nullptr;
        g_KillTeamAxe = nullptr;
        g_KillTeamMask = nullptr;
        g_KillTeamHelperArmed = false;
        g_KillTeamHelperProtected = false;
        g_KillTeamMaskAcquired = false;
        g_KillTeamSweaterUseDispatched = false;
        g_KillTeamAxeDiscoveryAttempted = false;
        g_KillTeamAxePursuitStartedAt = 0;
        g_KillTeamAxeLastProgressAt = 0;
        g_KillTeamAxeBestDistance = FLT_MAX;
        g_KillTeamHelperNativeBusyUntil = 0;
        g_KillTeamHelperStableObservations = 0;
        g_KillTeamFinalContextUntil = 0;
        g_KillTeamFinalInteractionDispatched = false;
        g_KillTeamFinalInteractionPending = false;
        g_KillTeamFinalInteractionAttempts = 0;
        g_KillTeamFinalInteractionStartedAt = 0;
        g_KillTeamFinalSequenceStartedAt = 0;
        g_NativeJasonDeathEventObserved.store(
            false, std::memory_order_release);
        g_NativeJasonDeathCinematicObservedAt = 0;
        g_NativeJasonDeathMatchEndAt = 0;
        g_KillTeamFinalRecoveryCooldownUntil = 0;
        g_KillTeamFinalMoveFailures = 0;
        g_KillTeamFinalRepositionAttempted = false;
        g_KillTeamLastCancelledInteraction = nullptr;
        g_NextKillTeamInteractionCancelAt = 0;
        g_KillTeamPendingFinalContext = nullptr;
        g_KillTeamPendingFinalComponent = nullptr;
        g_LastRejectedFinalContext = nullptr;
        g_LastAcceptedFinalContext = nullptr;
        g_LastAcceptedFinalComponent = nullptr;
        g_LastAcceptedFinalKillComponent = nullptr;
        g_LastUniversalFinalEligibilityContext = nullptr;
        // Do not discard an earned human sweater merely because Tommy has
        // not spawned yet or a helper pawn was temporarily unavailable. The
        // permanent sweater state is cleared by the world/session reset paths.
        g_NextFinalContextDiagnosticAt = 0;
        g_NextKillTeamDiscoveryAt = 0;
        g_NextKillTeamInteractAt = 0;
        g_NextHelperKnifeGrantAt = 0;
        g_NextSweaterUseAt = 0;
        g_NextFinalKillInteractAt = 0;
        g_NextKillTeamMoveAt = 0;
        g_NextKillTeamFollowAt = 0;
        g_KillTeamMoveTarget = nullptr;
        g_TommyJasonObjectiveOwner = nullptr;
        g_NextTommyJasonObjectiveRepairAt = 0;
        g_OrphanJasonStunStartedAt = 0;
        if (route != KillTeamRoute::None && helper)
        {
            Logger::Success(
                std::string("18L-AN Jason kill-team route armed | mode=") +
                (route == KillTeamRoute::HumanTommyFemaleHelper
                    ? "human-Tommy/female-sweater-bot"
                    : "human-counselor/AI-Tommy") +
                " | helper=" +
                JasonAISafeName(reinterpret_cast<UObject*>(helper)));
        }
    }

    void DriveLegacyCounselorKillTeamAI(ULONGLONG now)
    {
        if (now < g_NextKillTeamTickAt)
            return;

        // Until a Tommy/sweater route exists this is a lightweight target-list
        // check. Once active, the
        // counselor blackboard owns pursuit; this lane is only a sparse repair
        // path until a validated final-kill context needs short interaction
        // retries.
        // World-object discovery is separately limited to one pass per sixty
        // seconds below; it must never run at this control cadence.
        AActor* human = g_LocalCounselorTarget;
        if (!IsValidatedLiveCounselorPawn(human))
        {
            ResetKillTeamRoute(KillTeamRoute::None, nullptr);
            g_NextKillTeamTickAt = now + 60000;
            return;
        }

        const bool humanIsTommy = IsHunterCounselor(human);
        // Latch and restore the human's earned sweater independently of
        // Tommy's spawn lifecycle. This is a direct byte/property check, not
        // a world scan, and prevents the first Pamela use from being consumed
        // while the helper route is still absent.
        if (!humanIsTommy)
            MaintainPermanentHumanSweater(human);

        g_NextKillTeamTickAt = now + 4173;
        AActor* helper = nullptr;
        KillTeamRoute wantedRoute = KillTeamRoute::None;
        for (int32_t i = 0; i < g_JasonAITargetCount; ++i)
        {
            AActor* candidate = g_JasonAITargets[i];
            if (!candidate || candidate == human ||
                !IsValidatedLiveCounselorPawn(candidate))
            {
                continue;
            }
            if (humanIsTommy &&
                !IsHunterCounselor(candidate) &&
                IsFemaleCounselor(candidate))
            {
                helper = candidate;
                wantedRoute = KillTeamRoute::HumanTommyFemaleHelper;
                break;
            }
            if (!humanIsTommy && IsHunterCounselor(candidate))
            {
                helper = candidate;
                wantedRoute = KillTeamRoute::HumanSweaterAITommy;
                break;
            }
        }

        const bool routeChanged =
            g_KillTeamRoute != wantedRoute || g_KillTeamHelper != helper;
        ResetKillTeamRoute(wantedRoute, helper);

        AActor* previousFinalContext = g_LastAcceptedFinalContext;
        AActor* deathContext = nullptr;
        UObject* deathComponent = nullptr;
        const bool finalContextActive =
            now >= g_KillTeamFinalRecoveryCooldownUntil &&
            TryGetValidatedJasonDeathContext(
                g_JasonAIState.Jason,
                deathContext,
                deathComponent,
                now);
        if (finalContextActive)
        {
            g_OrphanJasonStunStartedAt = 0;
            g_KillTeamFinalContextUntil = now + 15000;
            g_NextKillTeamTickAt = now + 350;

            // The stock interaction manager is present on every counselor,
            // not only Tommy. Prefer the local player when they are already
            // beside the kneeling Jason (including the sweater wearer), then
            // use the configured helper or nearest live counselor as an AI
            // fallback. Local movement remains entirely player controlled.
            AActor* finisher = nullptr;
            bool allowAIMovement = false;
            FVector* interactionLocation = reinterpret_cast<FVector*>(
                reinterpret_cast<uintptr_t>(deathComponent) +
                Offsets::Scene_ComponentToWorld +
                Offsets::FTransform_Translation);
            FVector humanLocation{};
            if (GetCounselorInteractionManager(human) &&
                Memory::IsReadable(interactionLocation, sizeof(FVector)) &&
                GetJasonAIActorLocation(human, humanLocation))
            {
                const float dx = interactionLocation->X - humanLocation.X;
                const float dy = interactionLocation->Y - humanLocation.Y;
                if (dx * dx + dy * dy <= 350.0f * 350.0f)
                    finisher = human;
            }
            if (!finisher && helper &&
                GetCounselorInteractionManager(helper))
            {
                finisher = helper;
                allowAIMovement = true;
            }
            if (!finisher)
            {
                float bestDistanceSquared = FLT_MAX;
                for (int32_t i = 0; i < g_JasonAITargetCount; ++i)
                {
                    AActor* candidate = g_JasonAITargets[i];
                    FVector candidateLocation{};
                    if (!candidate || candidate == human ||
                        !IsValidatedLiveCounselorPawn(candidate) ||
                        !GetCounselorInteractionManager(candidate) ||
                        !Memory::IsReadable(
                            interactionLocation,
                            sizeof(FVector)) ||
                        !GetJasonAIActorLocation(
                            candidate,
                            candidateLocation))
                    {
                        continue;
                    }
                    const float dx =
                        interactionLocation->X - candidateLocation.X;
                    const float dy =
                        interactionLocation->Y - candidateLocation.Y;
                    const float distanceSquared = dx * dx + dy * dy;
                    if (std::isfinite(distanceSquared) &&
                        distanceSquared < bestDistanceSquared)
                    {
                        bestDistanceSquared = distanceSquared;
                        finisher = candidate;
                        allowAIMovement = true;
                    }
                }
            }
            if (!finisher && GetCounselorInteractionManager(human))
                finisher = human;

            if (finisher && deathContext != previousFinalContext &&
                allowAIMovement)
            {
                HoldKillTeamCounselorForFinalAction(finisher);
            }
            if (finisher && DriveCounselorFinalKill(
                    finisher,
                    allowAIMovement,
                    now))
                return;
            // Never hand a validated final context back to ordinary combat or
            // loot routing even if its interaction manager is transiently
            // unavailable on this retry.
            return;
        }
        else if (!humanIsTommy &&
                 g_PermanentHumanSweaterLatched)
        {
            RecoverOrphanJasonStunIfNeeded(
                human,
                g_JasonAIState.Jason,
                now);
        }

        if (!helper)
        {
            // Tommy may not have spawned yet. The sweater maintenance and
            // any validated human final action above still remain active.
            return;
        }

        // Make Jason Tommy's first and authoritative objective as soon as the
        // returned pawn is discovered. Thereafter, inspect the object key only
        // on a sparse repair cadence and rewrite the fight branch solely when
        // another stock goal displaced Jason. This replaces the old 1.1-second
        // block of seven reflected writes that caused the rhythmic hitch.
        if (wantedRoute == KillTeamRoute::HumanSweaterAITommy)
        {
            bool objectiveNeedsRepair =
                routeChanged || g_TommyJasonObjectiveOwner != helper;
            if (!objectiveNeedsRepair &&
                now >= g_NextTommyJasonObjectiveRepairAt)
            {
                UObject* controller = GetPawnControllerSafe(helper);
                UObject* blackboard = controller
                    ? GetCounselorBlackboardCached(controller)
                    : nullptr;
                objectiveNeedsRepair = !blackboard ||
                    GetBlackboardObject(
                        blackboard,
                        g_JasonCharacterNameIndex) !=
                        reinterpret_cast<UObject*>(g_JasonAIState.Jason);
                g_NextTommyJasonObjectiveRepairAt = now + 12151;
            }
            if (objectiveNeedsRepair)
            {
                const bool objectivePublished =
                    ArmKillTeamCounselorCombat(helper);
                CancelKillTeamHelperNonFinalInteraction(helper, now);
                g_TommyJasonObjectiveOwner = helper;
                g_NextTommyJasonObjectiveRepairAt = now + 12151;

                FVector jasonLocation{};
                if (GetJasonAIActorLocation(
                        g_JasonAIState.Jason,
                        jasonLocation))
                {
                    IssueAIMoveToLocationOnGameThread(
                        GetPawnControllerSafe(helper),
                        jasonLocation,
                        175.0f,
                        "AITommyFirstObjectiveJason");
                    g_NextKillTeamFollowAt = now + 6151;
                }
                Logger::Success(
                    std::string("18L-BB AI Tommy authoritative first objective set to Jason | blackboard=") +
                    (objectivePublished ? "true" : "false"));
            }
        }

        if (IsKillTeamHelperNativeBusy(helper, now))
            return;

        const bool discoveryDue = now >= g_NextKillTeamDiscoveryAt;
        if (discoveryDue)
        {
            g_NextKillTeamDiscoveryAt = now + 60000;
            if (!g_KillTeamShack)
            {
                g_KillTeamShack = FindNearestWorldActorByClass(
                    "Jason_Shack_C",
                    nullptr,
                    0.0f);
            }
        }
        FVector shackLocation{};
        const FVector* shackOrigin =
            g_KillTeamShack &&
            GetJasonAIActorLocation(g_KillTeamShack, shackLocation)
                ? &shackLocation
                : nullptr;

        if (wantedRoute == KillTeamRoute::HumanTommyFemaleHelper)
        {
            if (!HasPamelaSweater(helper))
            {
                if (g_KillTeamSweater &&
                    !Memory::IsReadable(g_KillTeamSweater, sizeof(UObject)))
                {
                    g_KillTeamSweater = nullptr;
                }
                if (discoveryDue)
                {
                    if (!g_KillTeamSweater)
                    {
                        g_KillTeamSweater = FindNearestWorldActorByClass(
                            "PamelasSweater_C",
                            shackOrigin,
                            1800.0f);
                    }
                }
                if (g_KillTeamSweater)
                    DriveCounselorToPickup(
                        helper,
                        g_KillTeamSweater,
                        now,
                        "PamelaSweater");
                return;
            }

            MaintainKillTeamHelperProtection(helper, now);
            if (!g_KillTeamMaskAcquired)
            {
                if (g_KillTeamMask &&
                    !Memory::IsReadable(g_KillTeamMask, sizeof(UObject)))
                {
                    g_KillTeamMask = nullptr;
                }
                if (discoveryDue)
                {
                    if (!g_KillTeamMaskAcquired && !g_KillTeamMask)
                    {
                        g_KillTeamMask = FindNearestWorldActorByClass(
                            "SCKillerMask",
                            nullptr,
                            0.0f);
                    }
                }
                if (g_KillTeamMask)
                {
                    DriveCounselorToPickup(
                        helper,
                        g_KillTeamMask,
                        now,
                        "JasonMask");
                    if (GetActorOwnerSafe(g_KillTeamMask) == helper ||
                        g_KillTeamMaskAcquired)
                    {
                        g_KillTeamMaskAcquired = true;
                        Logger::Success(
                            "18L-AN kill-team female helper acquired Jason mask");
                    }
                    return;
                }
            }

            FVector helperLocation{};
            FVector humanLocation{};
            if (GetJasonAIActorLocation(helper, helperLocation) &&
                GetJasonAIActorLocation(human, humanLocation))
            {
                const float dx = humanLocation.X - helperLocation.X;
                const float dy = humanLocation.Y - helperLocation.Y;
                if (dx * dx + dy * dy > 300.0f * 300.0f &&
                    now >= g_NextKillTeamFollowAt)
                {
                    IssueAIMoveToLocationOnGameThread(
                        GetPawnControllerSafe(helper),
                        humanLocation,
                        180.0f,
                        "FollowHumanTommy");
                    g_NextKillTeamFollowAt = now + 2500;
                }
            }

            FVector jasonLocation{};
            if (g_KillTeamMaskAcquired &&
                now >= g_NextSweaterUseAt &&
                GetJasonAIActorLocation(helper, helperLocation) &&
                GetJasonAIActorLocation(
                    g_JasonAIState.Jason,
                    jasonLocation))
            {
                const float dx = jasonLocation.X - helperLocation.X;
                const float dy = jasonLocation.Y - helperLocation.Y;
                if (dx * dx + dy * dy <= 500.0f * 500.0f &&
                    DispatchSweaterAbility(helper))
                {
                    g_KillTeamSweaterUseDispatched = true;
                    Logger::Success(
                        "18L-AN kill-team female helper dispatched native Pamela sweater ability");
                }
                g_NextSweaterUseAt = now + 3000;
            }
            return;
        }

        // AI Tommy is a dedicated Jason-kill helper from the moment his
        // return pawn is possessed.  Assert the complete melee branch before
        // checking his weapon so the stock tree cannot divert him into
        // cabins, hiding spots, searchable furniture, or generic looting.
        MaintainKillTeamHelperProtection(helper, now);

        UObject* equippedWeapon = GetCounselorCurrentWeapon(helper);
        bool tommyHasAxe = equippedWeapon &&
            ObjectClassDerivesFromExact(
                equippedWeapon,
                "CounselorTwoHandedAxe_C");
        if (tommyHasAxe)
        {
            g_HunterSpawnAxeItem = reinterpret_cast<AActor*>(equippedWeapon);
            g_KillTeamAxe = g_HunterSpawnAxeItem;
        }
        if (!tommyHasAxe)
        {
            // Tommy's return loadout is already replaced with an axe by the
            // GiveStartingItem hook. Never search for, path to, teleport to,
            // or repeatedly attempt to pick up the cabin axe. If native grab
            // handling removes his equipped instance, restore the cached axe
            // class directly through the same stock inventory function.
            if (now >= g_NextKillTeamInteractAt &&
                g_OriginalGiveStartingItem &&
                g_HunterSpawnAxeClass)
            {
                g_OriginalGiveStartingItem(helper, g_HunterSpawnAxeClass);
                g_NextKillTeamInteractAt = now + 10000;
                equippedWeapon = GetCounselorCurrentWeapon(helper);
                tommyHasAxe = equippedWeapon &&
                    ObjectClassDerivesFromExact(
                        equippedWeapon,
                        "CounselorTwoHandedAxe_C");
                if (tommyHasAxe)
                {
                    g_HunterSpawnAxeItem =
                        reinterpret_cast<AActor*>(equippedWeapon);
                    Logger::Success(
                        "18L-AU AI Tommy permanent starting axe restored without cabin search");
                }
            }
            // Even if the native grant does not equip synchronously, retain
            // combat-only routing.  The bounded ten-second recovery can try
            // again without handing Tommy back to the loot/hide branches.
        }

        g_KillTeamHelperArmed = true;

        FVector helperLocation{};
        FVector jasonLocation{};
        if (GetJasonAIActorLocation(helper, helperLocation) &&
            GetJasonAIActorLocation(
                g_JasonAIState.Jason,
                jasonLocation))
        {
            const float dx = jasonLocation.X - helperLocation.X;
            const float dy = jasonLocation.Y - helperLocation.Y;
            const float distanceSquared = dx * dx + dy * dy;
            if (std::isfinite(distanceSquared) &&
                distanceSquared > 425.0f * 425.0f &&
                now >= g_NextKillTeamFollowAt)
            {
                IssueAIMoveToLocationOnGameThread(
                    GetPawnControllerSafe(helper),
                    jasonLocation,
                    175.0f,
                    "AITommyCombatPursueJason");
                // One accepted MoveTo remains active in the path-following
                // component. Rebuilding it every 1.2 seconds caused the exact
                // periodic video hitch seen after Tommy spawned. Refresh only
                // as a bounded correction if he is still far from Jason.
                g_NextKillTeamFollowAt = now + 6151;
            }
        }

    }

    bool RequestCounselorRouteMatchCompletionAfterJasonDeath()
    {
        UWorld* world = g_JasonAIState.World;
        if (!world ||
            !Memory::IsReadable(world, sizeof(UWorld)))
        {
            return false;
        }

        UObject* gameMode = reinterpret_cast<UObject*>(ReadActorField(
            reinterpret_cast<UObject*>(world),
            0xF0));
        UObject* gameState = reinterpret_cast<UObject*>(ReadActorField(
            reinterpret_cast<UObject*>(world),
            0xF8));

        // A dead Jason must not enter the stock walking-home shot.  Arm the
        // existing native rule on whichever lifecycle owner exposes it, then
        // let GameMode drive its normal results/menu transition.
        const bool skippedGameModeOutro = WriteReflectedBoolByte(
            gameMode,
            "bSkipLevelOutro",
            true);
        const bool skippedGameStateOutro = WriteReflectedBoolByte(
            gameState,
            "bSkipLevelOutro",
            true);

        // The skip must be armed BEFORE EndMatch changes the match state.
        // Calling it after that transition still returned success, but the
        // stock state machine had already queued ClientPlayOutro.
        bool worldOutroSkipped = false;
        bool worldOutroFlagged = false;
        {
            UObject* worldSettings = reinterpret_cast<UObject*>(
                FindNearestWorldActorByClass(
                    "SCWorldSettings", nullptr, 0.0f));
            if (worldSettings &&
                Memory::IsReadable(worldSettings, sizeof(UObject)) &&
                worldSettings->Class &&
                Memory::IsReadable(worldSettings->Class, sizeof(UObject)))
            {
                Logger::Debug(
                    "18L-BR final outro world settings=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(
                        worldSettings->Class)));
                UFunction* skipOutro = FindFunctionInHierarchyByName(
                    worldSettings->Class, "SkipLevelIntroOutro");
                if (skipOutro)
                {
                    alignas(16) uint8_t params[0x40]{};
                    worldOutroSkipped = SafeProcessEventCall(
                        reinterpret_cast<uintptr_t>(worldSettings),
                        worldSettings, skipOutro, params);
                }
                else
                {
                    UPropertyLite* skipFlag = FindPropertyInHierarchyByName(
                        worldSettings->Class, "SkipLevelIntroOutro");
                    if (skipFlag && skipFlag->ClassPrivate &&
                        JasonAISafeName(reinterpret_cast<UObject*>(
                            skipFlag->ClassPrivate)) == "BoolProperty")
                    {
                        worldOutroFlagged = WriteReflectedBoolByte(
                            worldSettings, "SkipLevelIntroOutro", true);
                    }
                }
            }
        }

        bool completionDispatched = false;
        if (gameMode && gameMode->Class &&
            Memory::IsReadable(gameMode, sizeof(UObject)) &&
            Memory::IsReadable(gameMode->Class, sizeof(UObject)))
        {
            const char* completionFunctions[] = { "EndMatch", "FinishMatch" };
            for (const char* functionName : completionFunctions)
            {
                UFunction* completion = FindFunctionInHierarchyByName(
                    gameMode->Class, functionName);
                if (!completion)
                    continue;

                alignas(16) uint8_t params[0x40]{};
                completionDispatched = SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(gameMode), gameMode,
                    completion, params);
                if (completionDispatched)
                    break;
            }
        }

        // OnShowEndMatchMenus lives on the menu/game-instance side in this
        // packed build, not necessarily on the controller/HUD as in Sandbox.
        // Include the owning GameInstance among the narrow results targets.
        bool resultsShown = false;
        UObject* resultTargets[5]{};
        resultTargets[0] = Engine::GetLocalPlayerController();
        resultTargets[1] = gameMode;
        resultTargets[2] = gameState;
        resultTargets[4] = reinterpret_cast<UObject*>(
            world->OwningGameInstance);
        if (resultTargets[0] &&
            Memory::IsReadable(resultTargets[0], sizeof(UObject)) &&
            resultTargets[0]->Class)
        {
            UFunction* getHUD = FindFunctionInHierarchyByName(
                resultTargets[0]->Class, "GetHUD");
            if (getHUD)
            {
                struct HUDParams { UObject* ReturnValue; } params{};
                if (SafeProcessEventCall(
                        reinterpret_cast<uintptr_t>(resultTargets[0]),
                        resultTargets[0], getHUD, &params))
                {
                    resultTargets[3] = params.ReturnValue;
                }
            }
        }
        if (completionDispatched)
        {
            for (int32_t targetIndex = 0; targetIndex < 5;
                 ++targetIndex)
            {
                UObject* target = resultTargets[targetIndex];
                if (!target ||
                    !Memory::IsReadable(target, sizeof(UObject)) ||
                    !target->Class ||
                    !Memory::IsReadable(target->Class, sizeof(UObject)))
                {
                    continue;
                }
                UFunction* showMenus = FindFunctionInHierarchyByName(
                    target->Class, "OnShowEndMatchMenus");
                Logger::Debug(
                    "18L-BR final results target=" +
                    std::to_string(targetIndex) + " | class=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(
                        target->Class)) + " | event=" +
                    (showMenus ? "true" : "false"));
                if (!showMenus)
                    continue;
                alignas(16) uint8_t params[0x40]{};
                resultsShown = SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(target), target,
                    showMenus, params);
                if (resultsShown)
                    break;
            }
        }

        Logger::Success(
            std::string("18L-BD final Jason death requested native match completion | dispatched=") +
            (completionDispatched ? "true" : "false") +
            " | skipGameModeOutro=" +
            (skippedGameModeOutro ? "true" : "false") +
            " | skipGameStateOutro=" +
            (skippedGameStateOutro ? "true" : "false") +
            " | worldOutroSkipped=" +
            (worldOutroSkipped ? "true" : "false") +
            " | worldOutroFlagged=" +
            (worldOutroFlagged ? "true" : "false") +
            " | resultsShown=" + (resultsShown ? "true" : "false"));
        return completionDispatched;
    }

    bool TryCompleteSimplifiedPamelaTranceKill(ULONGLONG now)
    {
        if (g_KillTeamFinalInteractionCommitted ||
            g_PamelaTranceKillWindowUntil == 0)
        {
            return false;
        }
        if (now > g_PamelaTranceKillWindowUntil)
        {
            AActor* jason = g_JasonAIState.Jason;
            AActor* expiredContext = jason
                ? ReadActorField(
                    reinterpret_cast<UObject*>(jason),
                    0x12D8)
                : nullptr;
            AActor* validatedContext = nullptr;
            UObject* validatedComponent = nullptr;
            if (TryGetValidatedJasonDeathContext(
                    jason,
                    validatedContext,
                    validatedComponent,
                    now))
            {
                g_PamelaTranceKillWindowUntil = now + 30000;
                return false;
            }
            g_PamelaTranceKillWindowUntil = 0;
            g_PamelaTranceJasonHealthBaselineValid = false;
            g_PamelaTranceDamageEventObserved.store(
                false,
                std::memory_order_release);
            g_PamelaTranceKneelRequested = false;
            g_PamelaTranceFinalSlashAccepted = false;
            g_RepeatJasonKillStanceAt = 0;
            g_RepeatJasonKillStanceDeadline = 0;
            g_RepeatJasonKillStanceAttempts = 0;
            // Tear down the expired native death context immediately. Merely
            // arming orphan-stun recovery let the context branch renew its
            // movement lock every tick, leaving Jason upright and frozen.
            AbortFailedJasonKillSequence(jason, expiredContext, now);
            return false;
        }

        AActor* jason = g_JasonAIState.Jason;
        if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
            return false;

        bool damageObserved =
            g_PamelaTranceDamageEventObserved.exchange(
                false,
                std::memory_order_acq_rel);
        float currentHealth = g_PamelaTranceJasonHealthBaseline;
        float* health = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(jason) +
            Offsets::ASCCharacter_Health);
        if (Memory::IsReadable(health, sizeof(float)) &&
            std::isfinite(*health))
        {
            currentHealth = *health;
            if (!g_PamelaTranceJasonHealthBaselineValid)
            {
                g_PamelaTranceJasonHealthBaseline = currentHealth;
                g_PamelaTranceJasonHealthBaselineValid = true;
            }
            else
            {
                // Keep the ProcessEvent damage observation authoritative when
                // another mod clamps Jason's numeric health. The health delta
                // is an additional signal, not a replacement for the slash.
                damageObserved = damageObserved ||
                    currentHealth + 0.05f <
                        g_PamelaTranceJasonHealthBaseline;
            }
        }

        AActor* directDeathContext = nullptr;
        UObject* directDeathComponent = nullptr;
        const bool validatedDeathContext =
            TryGetValidatedJasonDeathContext(
                jason,
                directDeathContext,
                directDeathComponent,
                now);
        if (validatedDeathContext &&
            g_PamelaTranceKillWindowUntil < now + 3000)
        {
            // A real context won the race with the no-context timeout. Let
            // its paired cinematic/interaction own the full completion lane.
            g_PamelaTranceKillWindowUntil = now + 30000;
        }
        if (!g_PamelaTranceKneelRequested)
        {
            if (!damageObserved)
                return false;

            // One real slash during Pamela's trance is the complete kill
            // command, but the authored JasonDeath context must still own the
            // death state and cinematic before GameMode ends the match.
            g_PamelaTranceKneelRequested = true;
            g_PamelaTranceFinalSlashAccepted = true;
            g_PamelaTranceKillWindowUntil = now + 60000;
            g_KillTeamFinalContextUntil = now + 60000;
            g_PamelaTranceJasonHealthBaselineValid = false;
            g_OrphanJasonStunStartedAt = 0;
            g_JasonAIState.Target = nullptr;
            ArmJasonDeathVisualNormalization(now);
            ReleaseJasonSpectatorOverrideForCinematic("Pamela-one-slash-native-death");
            StopJasonAIMovementForKnifeOnGameThread();

            bool attackFinishPrearmed = false;
            UObject* finalActionComponent = directDeathComponent;
            if (validatedDeathContext && finalActionComponent)
            {
                attackFinishPrearmed = WriteReflectedBoolByte(
                    finalActionComponent,
                    "bCanAttackToInteract",
                    true);
                WriteReflectedBoolByte(
                    finalActionComponent,
                    "bAlwaysAllowInteractionFromAI",
                    true);

                // The public Interactable acquires the counselor-manager lock;
                // its sibling ContextKillComponent evaluates the authored
                // weapon/Hunter predicate and owns the paired animation. Make
                // both halves attack-finishable for this one validated death
                // actor so any real slash during Pamela's trance can complete
                // the normal cinematic path.
                if (g_LastAcceptedFinalKillComponent &&
                    Memory::IsReadable(
                        g_LastAcceptedFinalKillComponent,
                        sizeof(UObject)))
                {
                    WriteReflectedBoolByte(
                        g_LastAcceptedFinalKillComponent,
                        "bCanAttackToInteract",
                        true);
                    WriteReflectedBoolByte(
                        g_LastAcceptedFinalKillComponent,
                        "bAlwaysAllowInteractionFromAI",
                        true);
                }
            }

        const bool nativeStunnedAtHit = IsJasonNativelyStunned(jason);
        bool stanceRefreshed = validatedDeathContext;
        if (!validatedDeathContext && nativeStunnedAtHit)
        {
            UFunction* enterKillStance = FindFunctionInHierarchyByName(
                jason->Class,
                "SERVER_EnterKillStance");
            if (!enterKillStance)
            {
                enterKillStance = FindFunctionInHierarchyByName(
                    jason->Class,
                    "EnterKillStance");
            }
            stanceRefreshed = enterKillStance && SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(jason),
                jason,
                enterKillStance,
                nullptr);
        }
            if (!validatedDeathContext || !stanceRefreshed)
            {
                g_RepeatJasonKillStanceAt = now + 250;
                g_RepeatJasonKillStanceDeadline = now + 6000;
                g_RepeatJasonKillStanceAttempts = 0;
            }
            Logger::Success(
                std::string("18L-BG Pamela one-slash native Jason death armed | contextReady=") +
                (validatedDeathContext ? "true" : "false") +
                " | finalComponent=" +
                (finalActionComponent
                    ? JasonAISafeName(finalActionComponent)
                    : "NULL") +
                " | attackFinishPrearmed=" +
                (attackFinishPrearmed ? "true" : "false") +
                " | nativeStunnedAtHit=" +
                (nativeStunnedAtHit ? "true" : "false") +
                " | stanceRefreshed=" +
                (stanceRefreshed ? "true" : "false"));
            return true;
        }

        if (!validatedDeathContext)
            return false;

        // The native context now owns the death. Keep it stationary and allow
        // the already-accepted slash to dispatch its paired final interaction.
        g_KillTeamFinalContextUntil = now + 60000;
        g_JasonAIState.Target = nullptr;
        StopJasonAIMovementForKnifeOnGameThread();
        return false;
    }

    bool IsPamelaExclusiveKillLaneActive(ULONGLONG now)
    {
        return g_KillTeamFinalInteractionCommitted ||
            (g_PamelaTranceKillWindowUntil != 0 &&
                now <= g_PamelaTranceKillWindowUntil) ||
            (g_RepeatJasonKillStanceDeadline != 0 &&
                now <= g_RepeatJasonKillStanceDeadline) ||
            now < g_KillTeamFinalContextUntil;
    }

    bool CounselorHasJasonMaskInInventory(AActor* counselor)
    {
        if (!IsValidatedLiveCounselorPawn(counselor))
            return false;

        UObject* inventory = ReadReflectedObjectProperty(
            reinterpret_cast<UObject*>(counselor),
            "SmallItemInventory");
        if (!inventory ||
            !Memory::IsReadable(inventory, sizeof(UObject)) ||
            !inventory->Class ||
            !Memory::IsReadable(inventory->Class, sizeof(UStruct)))
        {
            return false;
        }

        UPropertyLite* itemList = FindPropertyInHierarchyByName(
            inventory->Class,
            "ItemList");
        if (!itemList || itemList->Offset_Internal <= 0 ||
            itemList->Offset_Internal >= 0x10000 ||
            itemList->ElementSize != sizeof(TArray<UObject*>))
        {
            return false;
        }

        auto* items = reinterpret_cast<TArray<UObject*>*>(
            reinterpret_cast<uintptr_t>(inventory) +
            itemList->Offset_Internal);
        if (!Memory::IsReadable(items, sizeof(*items)) ||
            !items->Data || items->Count <= 0 || items->Count > 16 ||
            items->Max < items->Count || items->Max > 32 ||
            !Memory::IsReadable(
                items->Data,
                sizeof(UObject*) * static_cast<size_t>(items->Count)))
        {
            return false;
        }

        for (int32_t i = 0; i < items->Count; ++i)
        {
            UObject* item = items->Data[i];
            if (!item || !Memory::IsReadable(item, sizeof(UObject)) ||
                !item->Class ||
                !Memory::IsReadable(item->Class, sizeof(UObject)))
            {
                continue;
            }
            // The live F6 inventory snapshot identified the stock pickup as
            // 2009_MaskPickup_C, not SCKillerMask. Check the held item itself,
            // never just Jason's mask-off flag or proximity to a world actor.
            const std::string name = JasonAISafeName(item);
            const std::string className = JasonAISafeName(
                reinterpret_cast<UObject*>(item->Class));
            if (name.find("MaskPickup") != std::string::npos ||
                className.find("MaskPickup") != std::string::npos ||
                name.find("Mask_Pickup") != std::string::npos ||
                className.find("Mask_Pickup") != std::string::npos ||
                name.find("KillerMask") != std::string::npos ||
                className.find("KillerMask") != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    void LogMaskPickupDiagnostic(AActor* counselor)
    {
        if (!IsValidatedLiveCounselorPawn(counselor))
        {
            Logger::Error("18L-BM F6 mask diagnostic: no living local counselor");
            return;
        }

        UObject* pawn = reinterpret_cast<UObject*>(counselor);
        UPropertyLite* property = FindPropertyInHierarchyByName(
            pawn->Class,
            "SmallItemInventory");
        Logger::Debug(
            "18L-BM F6 mask diagnostic | pawn=" +
            JasonAISafeName(pawn) +
            " | JasonMaskOn=" +
            (g_JasonAIState.Jason && ReadReflectedBoolByte(
                reinterpret_cast<UObject*>(g_JasonAIState.Jason),
                "bMaskOn") ? "true" : "false") +
            " | inventoryOffset=" +
            std::to_string(property ? property->Offset_Internal : -1) +
            " | elementSize=" +
            std::to_string(property ? property->ElementSize : -1));
        if (!property || property->ElementSize != sizeof(UObject*))
            return;

        UObject* inventory = ReadReflectedObjectProperty(
            pawn,
            "SmallItemInventory");
        if (!inventory || !GNames ||
            !GNames->IsValidIndex(inventory->NameIndex) ||
            !inventory->Class ||
            !Memory::IsReadable(inventory->Class, sizeof(UStruct)))
        {
            Logger::Debug("18L-BM F6 mask diagnostic: inventory object absent");
            return;
        }
        Logger::Debug(
            "18L-BM F6 inventory object | name=" +
            JasonAISafeName(inventory) +
            " | class=" +
            JasonAISafeName(reinterpret_cast<UObject*>(inventory->Class)));

        // One bounded reflection snapshot, only on the F6 request. No world
        // traversal or per-second counselor inventory scanning occurs.
        int fieldsSeen = 0;
        int fieldsLogged = 0;
        for (UStruct* type = reinterpret_cast<UStruct*>(inventory->Class);
             type && fieldsSeen < 192 && fieldsLogged < 24;
             type = SafeReadSuperStruct(type))
        {
            if (!Memory::IsReadable(type, sizeof(UStruct)))
                break;
            for (UField* field = type->Children;
                 field && fieldsSeen++ < 192 && fieldsLogged < 24;)
            {
                if (!Memory::IsReadable(field, sizeof(UPropertyLite)))
                    break;
                UField* next = field->Next;
                const std::string fieldName = field->GetName();
                const std::string fieldClass = JasonAISafeName(
                    reinterpret_cast<UObject*>(field->ClassPrivate));
                if (fieldClass.find("Property") == std::string::npos ||
                    (fieldName.find("Item") == std::string::npos &&
                     fieldName.find("Slot") == std::string::npos &&
                     fieldName.find("Mask") == std::string::npos &&
                     fieldName.find("Inventory") == std::string::npos))
                {
                    field = next;
                    continue;
                }
                auto* itemProperty = reinterpret_cast<UPropertyLite*>(field);
                const int32_t offset = itemProperty->Offset_Internal;
                const int32_t size = itemProperty->ElementSize;
                std::string value;
                if (offset > 0 && offset < 0x10000 &&
                    size == sizeof(UObject*))
                {
                    UObject** slot = reinterpret_cast<UObject**>(
                        reinterpret_cast<uintptr_t>(inventory) + offset);
                    if (Memory::IsReadable(slot, sizeof(*slot)) && *slot &&
                        Memory::IsReadable(*slot, sizeof(UObject)))
                    {
                        value = JasonAISafeName(*slot);
                        if ((*slot)->Class && Memory::IsReadable(
                            (*slot)->Class, sizeof(UObject)))
                        {
                            value += "/" + JasonAISafeName(
                                reinterpret_cast<UObject*>((*slot)->Class));
                        }
                    }
                }
                else if (offset > 0 && offset < 0x10000 &&
                    size == sizeof(TArray<UObject*>))
                {
                    auto* items = reinterpret_cast<TArray<UObject*>*>(
                        reinterpret_cast<uintptr_t>(inventory) + offset);
                    if (Memory::IsReadable(items, sizeof(*items)) &&
                        items->Count >= 0 && items->Count <= 16 &&
                        items->Max >= items->Count && items->Max <= 32)
                    {
                        value = "count=" + std::to_string(items->Count);
                        if (items->Data && Memory::IsReadable(
                            items->Data,
                            sizeof(UObject*) *
                                static_cast<size_t>(items->Count)))
                        {
                            for (int32_t i = 0;
                                 i < items->Count && i < 6; ++i)
                            {
                                value += "," + JasonAISafeName(items->Data[i]);
                            }
                        }
                    }
                }
                Logger::Debug(
                    "18L-BM F6 inventory field | name=" + fieldName +
                    " | type=" + fieldClass +
                    " | offset=" + std::to_string(offset) +
                    " | size=" + std::to_string(size) +
                    " | value=" + value);
                ++fieldsLogged;
                field = next;
            }
        }
    }

    void MaintainMaskGrantedSweaterPower(ULONGLONG now)
    {
        if (now < g_NextMaskSweaterPowerCheckAt)
            return;

        // The real pickup is an item in SCInventoryComponent::ItemList.
        // Check at most once per second after Jason loses his mask; never
        // traverse the world or interpret SmallItemInventory as an array.
        g_NextMaskSweaterPowerCheckAt = now + 1001;
        AActor* owner = g_MaskSweaterPickupOwnerPending;
        auto anyKnownJasonUnmasked = [&]()
        {
            if (g_JasonAIState.Jason &&
                !ReadReflectedBoolByte(
                    reinterpret_cast<UObject*>(g_JasonAIState.Jason),
                    "bMaskOn"))
                return true;
            for (const AdditionalCombatJasonState& additional :
                 g_AdditionalCombatJasons)
            {
                if (additional.World == g_JasonAIState.World &&
                    additional.Jason &&
                    Memory::IsReadable(additional.Jason, sizeof(UObject)) &&
                    !ReadReflectedBoolByte(
                        reinterpret_cast<UObject*>(additional.Jason),
                        "bMaskOn"))
                    return true;
            }
            return false;
        };
        // The pickup callback wakes this lane immediately. Until a mask is
        // actually off or a previous carrier needs maintenance, the opening
        // trap phase has no reason to inspect any counselor inventory.
        if (!owner && !g_MaskSweaterPowerLastOwner &&
            !anyKnownJasonUnmasked())
            return;
        if (owner && !CounselorHasJasonMaskInInventory(owner))
        {
            // Pickup events can arrive before the stock inventory insertion.
            // A failed/aborted pickup is forgotten after a bounded retry.
            if (now >= g_MaskSweaterPickupPendingUntil)
            {
                g_MaskSweaterPickupOwnerPending = nullptr;
                g_MaskSweaterPickupPendingUntil = 0;
                Logger::Debug(
                    "18L-BM mask pickup grant cancelled: held mask not present in inventory");
            }
            return;
        }
        if (!owner)
        {
            // A Jason variant can leave bMaskOn stale after dropping a mask;
            // an F1-spawned Jason can also lose his while the primary keeps
            // his. Check the held item first, independent of either flag.
            if (CounselorHasJasonMaskInInventory(
                    g_MaskSweaterPowerLastOwner))
            {
                owner = g_MaskSweaterPowerLastOwner;
            }
            else if (CounselorHasJasonMaskInInventory(
                         g_LocalCounselorTarget))
            {
                owner = g_LocalCounselorTarget;
            }
            else
            {
                if (!anyKnownJasonUnmasked())
                    return;

                // Only an unmasked killer or a pickup callback justifies the
                // at-most-seven-counselor inventory fallback scan.
                for (int32_t i = 0; i < g_JasonAITargetCount; ++i)
                {
                    AActor* candidate = g_JasonAITargets[i];
                    if (candidate != g_LocalCounselorTarget &&
                        CounselorHasJasonMaskInInventory(candidate))
                    {
                        owner = candidate;
                        break;
                    }
                }
            }
        }
        if (!owner)
            return;

        if (owner == g_MaskSweaterPowerLastOwner)
        {
            // The existing sweater lane owns every later use and its native
            // eleven-second rearm. Do not call GivePamelasSweater on a consumed
            // activation byte and interrupt the trance/death sequence.
            MaintainPermanentHumanSweater(owner);
            return;
        }
        // Never create two simultaneous Pamela powers. A live sweater carrier
        // keeps the earned ability; the mask becomes the recovery token only
        // after that carrier dies/escapes or when no sweater was acquired.
        if (g_PermanentHumanSweaterLatched &&
            g_PermanentHumanSweaterCarrier &&
            g_PermanentHumanSweaterCarrier != owner &&
            IsValidatedLiveCounselorPawn(g_PermanentHumanSweaterCarrier))
        {
            return;
        }

        // GivePamelasSweater is a two-argument native method in Shipping, not
        // a reflected no-argument UFunction. The former call always failed.
        // Reuse the real sweater ability object and the already-proven native
        // ownership/HUD restoration used after a normal sweater activation.
        // A world lookup is needed at most once on acquisition, never on the
        // regular mask-inventory or car-chase cadence.
        UObject* ability = g_PermanentHumanSweaterAbility;
        if (!IsPamelaSweaterAbilityObject(ability))
        {
            if (g_VehicleInterceptCar)
                return;
            AActor* sweater = FindNearestWorldActorByClass(
                "PamelasSweater_C", nullptr, 0.0f);
            UObject** abilityField = sweater
                ? reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(sweater) + 0x598)
                : nullptr;
            if (abilityField &&
                Memory::IsReadable(abilityField, sizeof(UObject*)))
            {
                ability = *abilityField;
            }
        }
        if (!IsPamelaSweaterAbilityObject(ability))
        {
            g_NextMaskSweaterPowerCheckAt = now + 5000;
            Logger::Error(
                "18L-BM held mask verified, but no live Pamela ability is available for transfer");
            return;
        }
        g_PermanentHumanSweaterAbility = ability;
        bool abilityAvailable = false;
        RearmPermanentHumanSweater(owner, true, &abilityAvailable);
        MaintainPermanentHumanSweater(owner);
        if (HasPamelaSweater(owner) && abilityAvailable)
        {
            g_MaskSweaterPowerLastOwner = owner;
            g_MaskSweaterPickupOwnerPending = nullptr;
            g_MaskSweaterPickupPendingUntil = 0;
            Logger::Success(
                "18L-BJ Jason mask pickup granted the single Pamela sweater power | owner=" +
                JasonAISafeName(reinterpret_cast<UObject*>(owner)));
        }
        else
        {
            Logger::Error(
                "18L-BM held mask transfer did not publish native Pamela Y availability; retrying");
        }
    }

    bool DispatchEndAimingIfPresent(UObject* object)
    {
        if (!object ||
            !Memory::IsReadable(object, sizeof(UObject)) ||
            !object->Class)
        {
            return false;
        }

        UFunction* endAiming = FindFunctionInHierarchyByName(
            object->Class,
            "OnEndAiming");
        return endAiming && SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(object),
            object,
            endAiming,
            nullptr);
    }

    void MaintainLocalShotgunAimReticleState(ULONGLONG now)
    {
        if (now < g_NextLocalAimReticleCheckAt)
            return;
        g_NextLocalAimReticleCheckAt = now + 211;

        AActor* counselor = g_LocalCounselorTarget;
        UObject* weapon = counselor
            ? GetCounselorCurrentWeapon(counselor)
            : nullptr;
        const bool shotgun = weapon &&
            JasonAIObjectDerivesFromNameContaining(weapon, "Shotgun");
        if (!counselor || !shotgun)
        {
            g_LocalAimReticleWeapon = nullptr;
            g_LocalAimReticleWasAiming = false;
            g_LocalAimReticleEndSent = false;
            return;
        }

        static UClass* cachedWeaponClass = nullptr;
        static int32_t weaponAimOffset = -2;
        static uint8_t weaponAimByteOffset = 0;
        static uint8_t weaponAimMask = 0;
        if (weapon->Class != cachedWeaponClass)
        {
            cachedWeaponClass = weapon->Class;
            weaponAimOffset = -2;
            weaponAimByteOffset = 0;
            weaponAimMask = 0;
        }

        static UClass* cachedCounselorClass = nullptr;
        static int32_t counselorAimOffset = -2;
        static uint8_t counselorAimByteOffset = 0;
        static uint8_t counselorAimMask = 0;
        if (counselor->Class != cachedCounselorClass)
        {
            cachedCounselorClass = counselor->Class;
            counselorAimOffset = -2;
            counselorAimByteOffset = 0;
            counselorAimMask = 0;
        }

        const bool weaponAiming = ReadCachedReflectedBool(
            weapon,
            "bIsAiming",
            weaponAimOffset,
            weaponAimByteOffset,
            weaponAimMask);
        const bool counselorAiming = ReadCachedReflectedBool(
            reinterpret_cast<UObject*>(counselor),
            "bIsAiming",
            counselorAimOffset,
            counselorAimByteOffset,
            counselorAimMask);
        const bool aiming = weaponAiming || counselorAiming;

        if (weapon != g_LocalAimReticleWeapon)
        {
            g_LocalAimReticleWeapon = weapon;
            g_LocalAimReticleWasAiming = aiming;
            g_LocalAimReticleEndSent = false;
        }

        if (aiming)
        {
            g_LocalAimReticleWasAiming = true;
            g_LocalAimReticleEndSent = false;
            return;
        }

        if (g_LocalAimReticleEndSent)
            return;

        // The stock shotgun can release bIsAiming without delivering its
        // Blueprint end event, leaving the HUD reticle latched indefinitely.
        // Replaying only that idempotent end event is cheaper and safer than
        // mutating the HUD widget or weapon state directly.
        const bool weaponEnded = DispatchEndAimingIfPresent(weapon);
        const bool counselorEnded = DispatchEndAimingIfPresent(
            reinterpret_cast<UObject*>(counselor));
        g_LocalAimReticleEndSent = true;
        if (weaponEnded || counselorEnded)
        {
            Logger::Success(
                std::string("18L-BL shotgun aim cleanup: stock OnEndAiming replayed | afterAim=") +
                (g_LocalAimReticleWasAiming ? "true" : "false"));
        }
        g_LocalAimReticleWasAiming = false;
    }

    void DriveCounselorKillTeamAI(ULONGLONG now)
    {
        if (g_MaskPickupDiagnosticRequested.exchange(
                false,
                std::memory_order_acq_rel))
        {
            AActor* local = g_LocalCounselorTarget;
            LogMaskPickupDiagnostic(local);
            if (CounselorHasJasonMaskInInventory(local))
            {
                // F6 confirms the exact inventory item found in the live
                // snapshot. It never grants power merely because Jason's
                // visible mask has fallen off.
                g_MaskSweaterPickupOwnerPending = local;
                g_MaskSweaterPickupPendingUntil = now + 5000;
                g_NextMaskSweaterPowerCheckAt = 0;
                Logger::Success(
                    "18L-BM F6 held-mask inventory verified; native Pamela grant queued");
            }
            else
            {
                Logger::Error(
                    "18L-BM F6 mask pickup rejected: no recognized Jason mask held in inventory");
            }
        }

        // Preserve Final v11's independent convergence cadence. It may guide
        // counselors toward Jason, but it does not own or synthesize Jason's
        // native Pamela/death state.
        // A moving-car interception owns Jason's position and is the most
        // latency-sensitive path. Counselor convergence performs synchronous
        // navigation work on a staggered cadence; pause it during the chase so
        // its route rebuild cannot produce the observed rhythmic driving hitch.
        if (!g_VehicleInterceptCar &&
            !g_PamelaTranceKneelRequested &&
            now >= g_KillTeamFinalContextUntil)
            DriveCounselorSweaterConvergence(now);
        if (!g_VehicleInterceptCar || g_MaskSweaterPickupOwnerPending)
            MaintainMaskGrantedSweaterPower(now);

        if (now < g_NextKillTeamTickAt)
            return;

        // The final death cinematic can run without leaving the brief
        // counselor-manager interaction lock visible to this one-second
        // observer. Jason's own native death event/state, after a validated
        // JasonDeath context and accepted final slash, is authoritative.
        // Give the paired scene time to play, then ask GameMode to finish;
        // never let the 30-second retry timeout revive Jason meanwhile.
        AActor* cinematicJason = g_JasonAIState.Jason;
        if (g_NativeJasonDeathCinematicObservedAt == 0 &&
            g_KillTeamFinalSequenceStartedAt != 0 &&
            (g_PamelaTranceFinalSlashAccepted ||
             g_KillTeamFinalInteractionAttempts > 0) &&
            cinematicJason &&
            Memory::IsReadable(cinematicJason, sizeof(UObject)))
        {
            const bool deathEvent =
                g_NativeJasonDeathEventObserved.exchange(
                    false, std::memory_order_acq_rel);
            static UClass* cachedDeathClass = nullptr;
            static int32_t deadOffset = -2;
            static int32_t dyingOffset = -2;
            static int32_t playedDeathAnimationOffset = -2;
            static uint8_t deadByteOffset = 0;
            static uint8_t dyingByteOffset = 0;
            static uint8_t playedDeathAnimationByteOffset = 0;
            static uint8_t deadMask = 0;
            static uint8_t dyingMask = 0;
            static uint8_t playedDeathAnimationMask = 0;
            if (cachedDeathClass != cinematicJason->Class)
            {
                cachedDeathClass = cinematicJason->Class;
                deadOffset = -2;
                dyingOffset = -2;
                playedDeathAnimationOffset = -2;
            }
            const bool nativeDead = ReadCachedReflectedBool(
                reinterpret_cast<UObject*>(cinematicJason), "bIsDead",
                deadOffset, deadByteOffset, deadMask);
            const bool nativeDying = ReadCachedReflectedBool(
                reinterpret_cast<UObject*>(cinematicJason), "bIsDying",
                dyingOffset, dyingByteOffset, dyingMask);
            const bool deathAnimationPlayed = ReadCachedReflectedBool(
                reinterpret_cast<UObject*>(cinematicJason),
                "bPlayedDeathAnimation", playedDeathAnimationOffset,
                playedDeathAnimationByteOffset,
                playedDeathAnimationMask);
            if (deathEvent || nativeDead || nativeDying ||
                deathAnimationPlayed)
            {
                g_NativeJasonDeathCinematicObservedAt = now;
                // Eight seconds interrupted the live final-death movie.
                // Keep Jason inert for a conservative cinematic interval;
                // the stock results event below then skips his cabin outro.
                g_NativeJasonDeathMatchEndAt = now + 25000;
                g_KillTeamFinalContextUntil = now + 60000;
                g_JasonAIState.Target = nullptr;
                Logger::Success(
                    std::string("18L-BO native Jason death cinematic confirmed; match ending armed | event=") +
                    (deathEvent ? "true" : "false") +
                    " | dead=" + (nativeDead ? "true" : "false") +
                    " | dying=" + (nativeDying ? "true" : "false") +
                    " | deathAnimation=" +
                    (deathAnimationPlayed ? "true" : "false"));
            }
        }
        if (g_NativeJasonDeathCinematicObservedAt != 0)
        {
            g_NextKillTeamTickAt = now + 250;
            if (!g_KillTeamFinalInteractionCommitted &&
                now >= g_NativeJasonDeathMatchEndAt)
            {
                g_KillTeamFinalInteractionCommitted = true;
                g_KillTeamFinalInteractionDispatched = true;
                g_KillTeamFinalInteractionPending = false;
                g_FinalKillCompletionDeadline = now + 5000;
                const bool dispatched =
                    RequestCounselorRouteMatchCompletionAfterJasonDeath();
                Logger::Success(
                    std::string("18L-BO death cinematic requested native match ending | dispatched=") +
                    (dispatched ? "true" : "false"));
            }
            else if (g_KillTeamFinalInteractionCommitted &&
                g_FinalKillCompletionDeadline != 0 &&
                now >= g_FinalKillCompletionDeadline)
            {
                // A confirmed death must not become a live Jason again just
                // because GameMode's first completion request was delayed.
                g_FinalKillCompletionDeadline = now + 5000;
                RequestCounselorRouteMatchCompletionAfterJasonDeath();
            }
            return;
        }

        AActor* human = g_LocalCounselorTarget;
        if (!IsValidatedLiveCounselorPawn(human))
        {
            g_NextKillTeamTickAt = now + 60000;
            return;
        }

        // A consumed JasonDeath context is only a pending completion signal.
        // Keep this controller wrapper alive until GameMode actually leaves
        // InProgress.  If that never happens, restore the retryable sweater /
        // kneel state instead of leaving Jason permanently frozen.
        if (g_KillTeamFinalInteractionCommitted)
        {
            g_NextKillTeamTickAt = now + 250;
            if (g_FinalKillCompletionDeadline != 0 &&
                now >= g_FinalKillCompletionDeadline)
            {
                g_KillTeamFinalInteractionCommitted = false;
                g_FinalKillCompletionDeadline = 0;
                AbortFailedJasonKillSequence(
                    g_JasonAIState.Jason,
                    nullptr,
                    now);
                Logger::Error(
                    "18L-BD native match completion timed out; restored Jason kill retry instead of freezing AI");
            }
            return;
        }

        // Proximity-only design: no Tommy discovery, helper route, loot/hide
        // cancellation, recurring blackboard writes, MoveTo rebuild, or bot
        // teleport. Poll only the human's verified native sweater byte and
        // Jason's direct final-context pointer. Heavy restoration happens
        // once, only on the real owned->consumed transition.
        MaintainPermanentHumanSweater(human);
        if (g_PermanentHumanSweaterCarrier &&
            g_PermanentHumanSweaterCarrier != human &&
            IsValidatedLiveCounselorPawn(
                g_PermanentHumanSweaterCarrier))
        {
            MaintainPermanentHumanSweater(
                g_PermanentHumanSweaterCarrier);
        }
        RestoreRepeatJasonDeathOpportunityIfNeeded(now);
        TryCompleteSimplifiedPamelaTranceKill(now);
        g_NextKillTeamTickAt = now + 997;
        if (g_PermanentHumanSweaterAttackUnlockAt != 0 &&
            now < g_PermanentHumanSweaterAttackUnlockAt)
        {
            // Service the opening-line montage release at its actual deadline
            // rather than waiting nearly another full second for the normal
            // sweater-maintenance pass.
            g_NextKillTeamTickAt = g_PermanentHumanSweaterAttackUnlockAt;
        }
        else if (g_PermanentHumanSweaterLatched &&
            g_PamelaTranceKillWindowUntil == 0)
        {
            // Detect the consumed sweater edge promptly while idle. This is
            // a single local-pawn check, not a world or counselor scan.
            g_NextKillTeamTickAt = now + 250;
        }

        // AttemptInteract is void, and a successful paired kill naturally
        // clears Jason's +0x12D8 death context near the end of its animation.
        // That disappearance after an accepted final input is the success
        // signal. Retire custom AI and leave the stock match-ending sequence
        // alone; treating it as a timeout used to call EndStun and revive him.
        AActor* directDeathContext = g_JasonAIState.Jason
            ? ReadActorField(
                reinterpret_cast<UObject*>(g_JasonAIState.Jason),
                0x12D8)
            : nullptr;
        const bool finalContextDisappearedAfterInput =
            !g_KillTeamFinalInteractionCommitted &&
            g_KillTeamFinalInteractionDispatched &&
            g_KillTeamFinalInteractionAttempts > 0 &&
            g_KillTeamFinalInteractionStartedAt != 0 &&
            now - g_KillTeamFinalInteractionStartedAt >= 8000 &&
            !directDeathContext;
        // Eligibility alone is not a kill. A stock predicate can pass while
        // AttemptInteract never acquires the final lock; the previous 20-second
        // fallback falsely ended those attempts as "You survived". Require a
        // confirmed native lock and consumed death context. Otherwise the
        // bounded failure path restores the sweater/death opportunity.
        if (finalContextDisappearedAfterInput)
        {
            g_KillTeamFinalInteractionCommitted = true;
            g_KillTeamFinalInteractionDispatched = true;
            g_KillTeamFinalInteractionPending = false;
            g_KillTeamFinalContextUntil = now + 60000;
            g_FinalKillCompletionDeadline = now + 60000;
            g_OrphanJasonStunStartedAt = 0;
            g_JasonAIState.Target = nullptr;
            ResetVehicleInterceptionState();
            SetJasonHighPriorityPursuitBoost(false, "Jason-final-death");
            const bool completionDispatched =
                RequestCounselorRouteMatchCompletionAfterJasonDeath();
            Logger::Success(
                std::string("18L-BD native final kill committed; awaiting verified GameMode completion | signal=") +
                "context-consumed" +
                " | dispatched=" +
                (completionDispatched ? "true" : "false"));
            return;
        }
        AActor* deathContext = nullptr;
        UObject* deathComponent = nullptr;
        if (now < g_KillTeamFinalRecoveryCooldownUntil ||
            !TryGetValidatedJasonDeathContext(
                g_JasonAIState.Jason,
                deathContext,
                deathComponent,
                now))
        {
            if (g_PermanentHumanSweaterLatched)
            {
                RecoverOrphanJasonStunIfNeeded(
                    human,
                    g_JasonAIState.Jason,
                    now);
            }
            return;
        }

        g_OrphanJasonStunStartedAt = 0;
        g_KillTeamFinalContextUntil = now + 30000;
        g_NextKillTeamTickAt = now + 250;
        // Keep the native kneel stationary. This is a cheap controller stop on
        // the short-lived final-context cadence, not a world scan or MoveTo.
        StopJasonAIMovementForKnifeOnGameThread();

        FVector* interactionLocation = reinterpret_cast<FVector*>(
            reinterpret_cast<uintptr_t>(deathComponent) +
            Offsets::Scene_ComponentToWorld +
            Offsets::FTransform_Translation);
        if (!Memory::IsReadable(interactionLocation, sizeof(FVector)))
            return;

        FVector jasonFinalLocation{};
        if (!GetJasonAIActorLocation(
                g_JasonAIState.Jason,
                jasonFinalLocation))
        {
            return;
        }
        auto distanceSquaredToFinal =
            [jasonFinalLocation](AActor* counselor) -> float
        {
            FVector location{};
            if (!IsValidatedLiveCounselorPawn(counselor) ||
                !GetJasonAIActorLocation(counselor, location))
            {
                return FLT_MAX;
            }
            // The authored death component can be animation-offset. Choose
            // the finisher from Jason's visible body, matching the melee hit.
            const float dx = jasonFinalLocation.X - location.X;
            const float dy = jasonFinalLocation.Y - location.Y;
            const float distanceSquared = dx * dx + dy * dy;
            return std::isfinite(distanceSquared)
                ? distanceSquared
                : FLT_MAX;
        };

        // Keep selection inside the same near-melee range used for dispatch.
        // The packed route chose a finisher nine meters away, then repeatedly
        // pressed the final action without a lockable stock overlap.
        constexpr float HumanFinalProximityCm = 200.0f;
        constexpr float HumanFinalProximitySquared =
            HumanFinalProximityCm * HumanFinalProximityCm;
        // A bot that just landed a native melee strike should already be in
        // range of Jason's visible body; no competing final-kill navigation
        // request is needed.
        constexpr float AIFinalApproachCm = 200.0f;
        constexpr float AIFinalApproachSquared =
            AIFinalApproachCm * AIFinalApproachCm;
        AActor* finisher = nullptr;
        float bestDistanceSquared = FLT_MAX;

        // Pin the first counselor that began this exact death context. The
        // stock interaction lock belongs to that pawn's manager; changing
        // finishers between synthetic A retries strands Jason in the kneel.
        if (g_KillTeamPendingFinalContext == deathContext &&
            g_KillTeamPendingFinalFinisher &&
            IsValidatedLiveCounselorPawn(
                g_KillTeamPendingFinalFinisher) &&
            GetCounselorInteractionManager(
                g_KillTeamPendingFinalFinisher))
        {
            bestDistanceSquared = distanceSquaredToFinal(
                g_KillTeamPendingFinalFinisher);
            if (bestDistanceSquared <= HumanFinalProximitySquared)
                finisher = g_KillTeamPendingFinalFinisher;
        }

        // The stock ContextKill predicate may already have approved a nearby
        // counselor for this exact JasonDeath actor. Preserve that decision:
        // this pawn has satisfied the authored weapon, view, range, and live
        // checks and is therefore the safest target for the synthetic primary
        // action. The previous nearest-pawn search discarded this evidence.
        if (!finisher &&
            g_LastUniversalFinalEligibilityContext == deathContext &&
            g_LastUniversalFinalEligibilityFinisher &&
            now <= g_LastUniversalFinalEligibilityAcceptedAt + 20000)
        {
            AActor* eligible = g_LastUniversalFinalEligibilityFinisher;
            bestDistanceSquared = distanceSquaredToFinal(eligible);
            if (bestDistanceSquared <= HumanFinalProximitySquared &&
                GetCounselorInteractionManager(eligible))
            {
                finisher = eligible;
            }
        }

        // Prefer the human counselor who actually supplied the observed melee
        // damage. Previous builds searched AI first, so a nearby bot received
        // the synthetic A input while the player's later physical A press was
        // the only action that could acquire the local interaction lock.
        if (!finisher &&
            g_PamelaTrancePreferredFinisher &&
            now <= g_PamelaTrancePreferredFinisherUntil &&
            g_PamelaTrancePreferredFinisher == human)
        {
            bestDistanceSquared = distanceSquaredToFinal(human);
            if (bestDistanceSquared <= HumanFinalProximitySquared &&
                GetCounselorInteractionManager(human))
            {
                finisher = human;
            }
        }
        else if (now > g_PamelaTrancePreferredFinisherUntil)
        {
            g_PamelaTrancePreferredFinisher = nullptr;
            g_PamelaTrancePreferredFinisherUntil = 0;
        }

        // Even if the exact damage callback arrived before the local-target
        // cache refreshed, prefer a nearby human over an AI actor. A direct
        // manager call on the locally possessed counselor follows the same
        // route that the confirmed physical A press used in the last test.
        if (!finisher)
        {
            bestDistanceSquared = distanceSquaredToFinal(human);
            if (bestDistanceSquared <= HumanFinalProximitySquared &&
                GetCounselorInteractionManager(human))
            {
                finisher = human;
            }
        }

        for (int32_t i = 0; !finisher && i < g_JasonAITargetCount; ++i)
        {
            AActor* candidate = g_JasonAITargets[i];
            if (!candidate || candidate == human ||
                !IsAIControlledCounselor(candidate) ||
                !GetCounselorInteractionManager(candidate))
            {
                continue;
            }
            const float candidateDistanceSquared =
                distanceSquaredToFinal(candidate);
            if (candidateDistanceSquared <= AIFinalApproachSquared &&
                candidateDistanceSquared < bestDistanceSquared)
            {
                finisher = candidate;
                bestDistanceSquared = candidateDistanceSquared;
            }
        }
        if (!finisher)
        {
            // Keep the stock kneel available long enough for someone to step
            // beside Jason, but never move or teleport a counselor into it.
            if (g_KillTeamFinalSequenceStartedAt != 0 &&
                now - g_KillTeamFinalSequenceStartedAt >= 30000)
            {
                AbortFailedJasonKillSequence(
                    g_JasonAIState.Jason,
                    deathContext,
                    now);
            }
            return;
        }

        if (deathContext != g_KillTeamPendingFinalContext &&
            IsAIControlledCounselor(finisher))
        {
            HoldKillTeamCounselorForFinalAction(finisher);
        }
        DriveCounselorFinalKill(
            finisher,
            IsAIControlledCounselor(finisher),
            now);
    }

    void RefreshUnarmedCounselorFleeState(ULONGLONG now)
    {
        if (now < g_NextCounselorFleeRefreshAt)
            return;
        // Finish name-key discovery in tiny slices. Once resolved, service at
        // most one counselor per staggered pass instead of bursting up to 32
        // reflected blackboard writes on the same ten-second frame.
        g_NextCounselorFleeRefreshAt = now +
            (g_CounselorBlackboardNameResolutionComplete ? 1250 : 250);

        if (!g_CounselorBlackboardNameResolutionComplete)
        {
            const ULONGLONG startedAt = GetTickCount64();
            const int32_t startIndex =
                g_CounselorBlackboardNameResolveCursor;
            ResolveCounselorBlackboardNameIndices();
            if (g_CounselorBlackboardNameResolutionComplete ||
                (g_CounselorBlackboardNameResolveCursor % 8192) == 0)
            {
                Logger::Debug(
                    "18L-AI counselor flee key scan throttled | range=" +
                    std::to_string(startIndex) + "-" +
                    std::to_string(g_CounselorBlackboardNameResolveCursor) +
                    " | complete=" +
                    std::string(
                        g_CounselorBlackboardNameResolutionComplete
                            ? "true"
                            : "false") +
                    " | durationMs=" +
                    std::to_string(GetTickCount64() - startedAt));
            }
        }

        if (g_SCWeaponNameIndex < 0 ||
            g_ShouldFleeKillerNameIndex < 0 ||
            g_ShouldFightBackNameIndex < 0 ||
            g_ShouldArmedFightBackNameIndex < 0 ||
            g_SeekWeaponWhileFleeingNameIndex < 0)
        {
            return;
        }

        // Sweater convergence deliberately owns the fight/flee keys after its
        // one-time transition; the ordinary unarmed proximity lane must not
        // overwrite those assignments.
        if (g_CounselorConvergenceActive)
            return;

        AActor* jason = g_JasonAIState.Jason;
        FVector jasonLocation{};
        if (!jason || !GetJasonAIActorLocation(jason, jasonLocation))
            return;

        const int32_t boundedTargetCount =
            (std::min)(g_JasonAITargetCount, JasonAITargetCapacity);
        for (int32_t attempt = 0;
             attempt < boundedTargetCount;
             ++attempt)
        {
            const int32_t i =
                (g_CounselorFleeRefreshCursor + attempt) %
                boundedTargetCount;
            AActor* counselor = g_JasonAITargets[i];
            if (!counselor ||
                counselor == g_LocalCounselorTarget ||
                !IsValidatedLiveCounselorPawn(counselor))
            {
                continue;
            }

            FVector counselorLocation{};
            if (!GetJasonAIActorLocation(counselor, counselorLocation))
                continue;
            const float dx = counselorLocation.X - jasonLocation.X;
            const float dy = counselorLocation.Y - jasonLocation.Y;
            if (!std::isfinite(dx) ||
                !std::isfinite(dy) ||
                dx * dx + dy * dy > 2500.0f * 2500.0f)
            {
                continue;
            }

            UObject** controllerField = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(counselor) + 0x3A0);
            if (!Memory::IsReadable(controllerField, sizeof(UObject*)) ||
                !*controllerField ||
                !Memory::IsReadable(*controllerField, sizeof(UObject)))
            {
                continue;
            }

            UObject* controller = *controllerField;
            UObject* blackboard = GetCounselorBlackboardCached(controller);
            if (!blackboard)
                continue;

            // The behavior tree's SCWeapon object is authoritative for whether
            // this bot can fight.  When it is empty and Jason is within 25 m,
            // force the existing abort-aware flee branch and let its native
            // FindFleeLocation/escape tasks choose the route.
            if (!GetBlackboardObject(blackboard, g_SCWeaponNameIndex))
            {
                SetBlackboardBool(
                    blackboard,
                    g_ShouldFightBackNameIndex,
                    false);
                SetBlackboardBool(
                    blackboard,
                    g_ShouldArmedFightBackNameIndex,
                    false);
                SetBlackboardBool(
                    blackboard,
                    g_SeekWeaponWhileFleeingNameIndex,
                    true);
                SetBlackboardBool(
                    blackboard,
                    g_ShouldFleeKillerNameIndex,
                    true);
            }

            g_CounselorFleeRefreshCursor =
                (i + 1) % boundedTargetCount;
            break;
        }
    }

    bool ClassifyRepairableCar(
        AActor* actor,
        uint8_t& outKind,
        int32_t& outSeatCount)
    {
        outKind = 0;
        outSeatCount = 0;

        if (!actor ||
            !Memory::IsReadable(actor, sizeof(UObject)) ||
            !JasonAIObjectDerivesFromNameContaining(
                reinterpret_cast<UObject*>(actor),
                "SCDriveableVehicle"))
        {
            return false;
        }

        // Resurrected native vehicle-cache classification (RVA 0x3B87C0):
        // VehicleType 1 is a car; Seats.Num > 2 distinguishes the 4-seater.
        uint8_t* vehicleType = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(actor) + 0x3D8);
        TArray<UObject*>* seats = reinterpret_cast<TArray<UObject*>*>(
            reinterpret_cast<uintptr_t>(actor) + 0x4E8);

        if (!Memory::IsReadable(vehicleType, 1) ||
            !Memory::IsReadable(seats, sizeof(TArray<UObject*>)) ||
            *vehicleType != 1 ||
            seats->Count <= 0 ||
            seats->Count > 16)
        {
            return false;
        }

        outSeatCount = seats->Count;
        outKind = seats->Count > 2 ? 3 : 2;
        return true;
    }

    void ConsiderRepairableCar(
        AActor* actor,
        const char* source,
        AActor*& car2,
        AActor*& car4,
        std::string& car2Source,
        std::string& car4Source)
    {
        uint8_t kind = 0;
        int32_t seatCount = 0;
        if (!ClassifyRepairableCar(actor, kind, seatCount))
            return;

        if (kind == 2 && !car2)
        {
            car2 = actor;
            car2Source = source ? source : "unknown";
        }
        else if (kind == 3 && !car4)
        {
            car4 = actor;
            car4Source = source ? source : "unknown";
        }
        else
        {
            return;
        }

        Logger::Success(
            "18L-AG counselor bridge car objective: actor=" +
            JasonAISafeName(reinterpret_cast<UObject*>(actor)) +
            " | type=1 | seats=" + std::to_string(seatCount) +
            " | kind=" + JasonAIStartupTrapKindName(kind) +
            " | source=" + (source ? source : "unknown"));
    }

    void ScanVehicleArray(
        TArray<AActor*>* vehicles,
        const char* source,
        AActor*& car2,
        AActor*& car4,
        std::string& car2Source,
        std::string& car4Source)
    {
        if (!vehicles ||
            !Memory::IsReadable(vehicles, sizeof(TArray<AActor*>)) ||
            !vehicles->Data ||
            vehicles->Count <= 0 ||
            vehicles->Count > 128 ||
            !Memory::IsReadable(
                vehicles->Data,
                sizeof(AActor*) * static_cast<size_t>(vehicles->Count)))
        {
            return;
        }

        for (int32_t i = 0;
            i < vehicles->Count && (!car2 || !car4);
            ++i)
        {
            ConsiderRepairableCar(
                vehicles->Data[i],
                source,
                car2,
                car4,
                car2Source,
                car4Source);
        }
    }

    bool ReadLiveVehicleSeat(
        UObject* seat,
        AActor* expectedCar,
        AActor*& outOccupant)
    {
        outOccupant = nullptr;
        if (!seat ||
            !expectedCar ||
            !Memory::IsReadable(seat, 0x550) ||
            !JasonAIObjectDerivesFromNameContaining(
                seat,
                "SCVehicleSeatComponent"))
        {
            return false;
        }

        AActor** parentVehicle = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(seat) + 0x548);
        AActor** occupant = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(seat) + 0x500);
        if (!Memory::IsReadable(parentVehicle, sizeof(AActor*)) ||
            *parentVehicle != expectedCar ||
            !Memory::IsReadable(occupant, sizeof(AActor*)) ||
            !*occupant ||
            !Memory::IsReadable(*occupant, 0x15A0) ||
            !JasonAIObjectDerivesFromNameContaining(
                reinterpret_cast<UObject*>(*occupant),
                "SCCounselorCharacter"))
        {
            return false;
        }

        uint8_t* dead = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(*occupant) + 0x1031);
        UObject** currentSeat = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(*occupant) + 0x1588);
        uint8_t* exiting = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(*occupant) + 0x159C);
        if (!Memory::IsReadable(dead, 1) ||
            *dead != 0 ||
            HasCounselorEscaped(*occupant) ||
            !Memory::IsReadable(currentSeat, sizeof(UObject*)) ||
            *currentSeat != seat ||
            (Memory::IsReadable(exiting, 1) && *exiting != 0))
        {
            return false;
        }

        outOccupant = *occupant;
        return true;
    }

    bool ReadCachedVehicleSeatFast(
        UObject* seat,
        AActor* expectedCar,
        AActor*& outOccupant)
    {
        outOccupant = nullptr;
        if (!seat || !expectedCar || !Memory::IsReadable(seat, 0x550))
            return false;

        AActor** parentVehicle = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(seat) + 0x548);
        AActor** occupant = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(seat) + 0x500);
        if (!Memory::IsReadable(parentVehicle, sizeof(AActor*)) ||
            *parentVehicle != expectedCar ||
            !Memory::IsReadable(occupant, sizeof(AActor*)) ||
            !*occupant ||
            !Memory::IsReadable(*occupant, 0x15A0))
        {
            return false;
        }

        uint8_t* dead = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(*occupant) + 0x1031);
        float* health = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(*occupant) +
            Offsets::ASCCharacter_Health);
        UObject** currentSeat = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(*occupant) + 0x1588);
        uint8_t* exiting = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(*occupant) + 0x159C);
        if (!Memory::IsReadable(dead, 1) || *dead != 0 ||
            !Memory::IsReadable(health, sizeof(float)) ||
            !std::isfinite(*health) || *health <= 0.0f ||
            !Memory::IsReadable(currentSeat, sizeof(UObject*)) ||
            *currentSeat != seat ||
            (Memory::IsReadable(exiting, 1) && *exiting != 0))
        {
            return false;
        }

        outOccupant = *occupant;
        return true;
    }

    bool FindLiveSeatInCar(
        AActor* car,
        AActor* preferredOccupant,
        UObject*& outSeat,
        AActor*& outOccupant)
    {
        outSeat = nullptr;
        outOccupant = nullptr;
        uint8_t kind = 0;
        int32_t seatCount = 0;
        if (!ClassifyRepairableCar(car, kind, seatCount))
            return false;

        TArray<UObject*>* seats = reinterpret_cast<TArray<UObject*>*>(
            reinterpret_cast<uintptr_t>(car) + 0x4E8);
        if (!Memory::IsReadable(seats, sizeof(TArray<UObject*>)) ||
            !seats->Data ||
            seats->Count <= 0 ||
            seats->Count > 16 ||
            !Memory::IsReadable(
                seats->Data,
                sizeof(UObject*) * static_cast<size_t>(seats->Count)))
        {
            return false;
        }

        UObject* firstSeat = nullptr;
        AActor* firstOccupant = nullptr;
        UObject* preferredSeat = nullptr;
        AActor* preferredLiveOccupant = nullptr;
        for (int32_t i = 0; i < seats->Count; ++i)
        {
            AActor* occupant = nullptr;
            if (!ReadLiveVehicleSeat(seats->Data[i], car, occupant))
                continue;

            if (!firstSeat)
            {
                firstSeat = seats->Data[i];
                firstOccupant = occupant;
            }

            uint8_t* driver = reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(seats->Data[i]) + 0x511);
            if (Memory::IsReadable(driver, 1) && *driver != 0)
            {
                // Always stop the vehicle by removing its actual driver,
                // even when Jason was previously chasing a passenger.
                outSeat = seats->Data[i];
                outOccupant = occupant;
                return true;
            }

            if (preferredOccupant && occupant == preferredOccupant)
            {
                preferredSeat = seats->Data[i];
                preferredLiveOccupant = occupant;
            }
        }

        outSeat = preferredSeat ? preferredSeat : firstSeat;
        outOccupant = preferredSeat
            ? preferredLiveOccupant
            : firstOccupant;
        return outSeat != nullptr;
    }

    bool IsStartedOccupiedCar(
        AActor* car,
        AActor* preferredOccupant,
        UObject*& outSeat,
        AActor*& outOccupant,
        bool requireStarted)
    {
        if (!FindLiveSeatInCar(
                car,
                preferredOccupant,
                outSeat,
                outOccupant))
        {
            return false;
        }

        uint8_t* started = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(car) + 0x47A);
        return !requireStarted ||
            (Memory::IsReadable(started, 1) && *started != 0);
    }

    AActor* FindOccupiedEscapeCar(
        UObject*& outSeat,
        AActor*& outOccupant)
    {
        outSeat = nullptr;
        outOccupant = nullptr;
        AActor* preferred = g_JasonAIState.Target;
        const ULONGLONG now = GetTickCount64();
        if (g_IgnoredVehicleInterceptCar &&
            now >= g_IgnoredVehicleInterceptUntil)
        {
            g_IgnoredVehicleInterceptCar = nullptr;
            g_IgnoredVehicleInterceptUntil = 0;
        }
        const auto isTemporarilyIgnoredCar =
            [now](AActor* car) -> bool
        {
            return car &&
                car == g_IgnoredVehicleInterceptCar &&
                now < g_IgnoredVehicleInterceptUntil;
        };

        // The current chase target's real vehicle seat is the strongest
        // signal. Vehicle possession can temporarily break the ordinary
        // controller/pawn reciprocity used by the on-foot target registry,
        // so validate the seat/occupant relationship directly here.
        if (preferred && Memory::IsReadable(preferred, 0x1590))
        {
            UObject** currentSeat = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(preferred) + 0x1588);
            if (Memory::IsReadable(currentSeat, sizeof(UObject*)) &&
                *currentSeat &&
                Memory::IsReadable(*currentSeat, 0x550))
            {
                AActor** parentCar = reinterpret_cast<AActor**>(
                    reinterpret_cast<uintptr_t>(*currentSeat) + 0x548);
                if (Memory::IsReadable(parentCar, sizeof(AActor*)) &&
                    *parentCar &&
                    !isTemporarilyIgnoredCar(*parentCar) &&
                    IsStartedOccupiedCar(
                        *parentCar,
                        preferred,
                        outSeat,
                        outOccupant,
                        true))
                {
                    return *parentCar;
            }
        }
    }

        if (g_VehicleInterceptCar &&
            !isTemporarilyIgnoredCar(g_VehicleInterceptCar) &&
            IsStartedOccupiedCar(
                g_VehicleInterceptCar,
                preferred,
                outSeat,
                outOccupant,
                false))
        {
            return g_VehicleInterceptCar;
        }

        AActor* jason = g_JasonAIState.Jason;
        FVector jasonLocation{};
        if (!jason || !GetJasonAIActorLocation(jason, jasonLocation))
            return nullptr;

        AActor* bestCar = nullptr;
        UObject* bestSeat = nullptr;
        AActor* bestOccupant = nullptr;
        float bestDistanceSquared = FLT_MAX;
        bool haveKnownCarRegistry = false;

        // The opening objective resolver already validated and cached both
        // real cars. Keep the normal interception hot path bounded to those
        // two actors instead of rescanning every actor in every level three
        // times per second.
        for (int32_t i = 0;
            i < g_JasonAIState.StartupTrapObjectiveCount;
            ++i)
        {
            if (g_JasonAIState.StartupTrapObjectiveKinds[i] != 2 &&
                g_JasonAIState.StartupTrapObjectiveKinds[i] != 3)
            {
                continue;
            }

            AActor* car = g_JasonAIState.StartupTrapObjectives[i];
            if (isTemporarilyIgnoredCar(car))
                continue;
            uint8_t knownKind = 0;
            int32_t knownSeats = 0;
            if (ClassifyRepairableCar(car, knownKind, knownSeats))
                haveKnownCarRegistry = true;
            UObject* seat = nullptr;
            AActor* occupant = nullptr;
            if (!IsStartedOccupiedCar(
                    car,
                    preferred,
                    seat,
                    occupant,
                    true))
            {
                continue;
            }

            FVector carLocation{};
            if (!GetJasonAIActorLocation(car, carLocation))
                continue;
            const float dx = carLocation.X - jasonLocation.X;
            const float dy = carLocation.Y - jasonLocation.Y;
            const float dz = carLocation.Z - jasonLocation.Z;
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(distanceSquared) &&
                distanceSquared < bestDistanceSquared)
            {
                bestDistanceSquared = distanceSquared;
                bestCar = car;
                bestSeat = seat;
                bestOccupant = occupant;
            }
        }

        if (bestCar)
        {
            outSeat = bestSeat;
            outOccupant = bestOccupant;
            return bestCar;
        }

        // We skipped the previously stalled car above so a second occupied
        // car could win. If there is no alternative, immediately reacquire
        // the original car instead of allowing ordinary knife/combat logic to
        // steal the stop-car priority for the full ignore window.
        if (g_IgnoredVehicleInterceptCar &&
            now < g_IgnoredVehicleInterceptUntil)
        {
            UObject* ignoredSeat = nullptr;
            AActor* ignoredOccupant = nullptr;
            AActor* ignoredCar = g_IgnoredVehicleInterceptCar;
            if (IsStartedOccupiedCar(
                    ignoredCar,
                    preferred,
                    ignoredSeat,
                    ignoredOccupant,
                    true))
            {
                g_IgnoredVehicleInterceptCar = nullptr;
                g_IgnoredVehicleInterceptUntil = 0;
                outSeat = ignoredSeat;
                outOccupant = ignoredOccupant;
                return ignoredCar;
            }
        }

        // A valid opening-objective car registry is authoritative. If neither
        // known car is currently started and occupied, do not fall through to
        // a complete world scan on every AI action interval.
        if (haveKnownCarRegistry)
            return nullptr;

        // Fallback only for unusual maps whose car did not participate in
        // opening objective resolution.
        UWorld* world = g_JasonAIState.World;
        if (!world || !Memory::IsReadable(world, sizeof(UWorld)))
            return nullptr;

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return nullptr;
        }

        for (int32_t levelIndex = 0;
            levelIndex < levels->Count;
            ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;

            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data ||
                actors.Count <= 0 ||
                actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t i = 0; i < actors.Count; ++i)
            {
                AActor* car = actors.Data[i];
                if (isTemporarilyIgnoredCar(car))
                    continue;
                UObject* seat = nullptr;
                AActor* occupant = nullptr;
                if (!IsStartedOccupiedCar(
                        car,
                        preferred,
                        seat,
                        occupant,
                        true))
                {
                    continue;
                }

                FVector carLocation{};
                if (!GetJasonAIActorLocation(car, carLocation))
                    continue;

                const float dx = carLocation.X - jasonLocation.X;
                const float dy = carLocation.Y - jasonLocation.Y;
                const float dz = carLocation.Z - jasonLocation.Z;
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                if (std::isfinite(distanceSquared) &&
                    distanceSquared < bestDistanceSquared)
                {
                    bestDistanceSquared = distanceSquared;
                    bestCar = car;
                    bestSeat = seat;
                    bestOccupant = occupant;
                }
            }
        }

        outSeat = bestSeat;
        outOccupant = bestOccupant;
        return bestCar;
    }

    __declspec(noinline) float SafeVehicleForwardSpeed(AActor* car)
    {
        if (!car || !Memory::IsReadable(car, 0x3D8))
            return 0.0f;

        UObject** movement = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(car) + 0x3D0);
        HMODULE module = GetModuleHandle(nullptr);
        if (!module ||
            !Memory::IsReadable(movement, sizeof(UObject*)) ||
            !*movement ||
            !Memory::IsReadable(*movement, sizeof(UObject)))
        {
            return 0.0f;
        }

        __try
        {
            using Function = float(__fastcall*)(UObject*);
            return reinterpret_cast<Function>(
                reinterpret_cast<uintptr_t>(module) + 0x01FAAB80)(
                    *movement);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0.0f;
        }
    }

    bool GetVehicleTravelDirection(
        AActor* car,
        float forwardSpeed,
        FVector& outDirection,
        float& outHorizontalSpeed)
    {
        outDirection = FVector{};
        outHorizontalSpeed = 0.0f;
        if (!car || !car->Class || !Memory::IsReadable(car, sizeof(UObject)))
            return false;

        FVector velocityDirection{};
        bool haveVelocityDirection = false;
        UFunction* velocityFunction =
            FindFunctionInHierarchyByName(car->Class, "GetVelocity");
        if (velocityFunction)
        {
            struct VelocityParams { FVector ReturnValue; };
            VelocityParams params{};
            if (SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(car),
                    car,
                    velocityFunction,
                    &params))
            {
                const float horizontalSquared =
                    params.ReturnValue.X * params.ReturnValue.X +
                    params.ReturnValue.Y * params.ReturnValue.Y;
                if (std::isfinite(horizontalSquared) && horizontalSquared > 1.0f)
                {
                    outHorizontalSpeed = std::sqrt(horizontalSquared);
                    velocityDirection.X =
                        params.ReturnValue.X / outHorizontalSpeed;
                    velocityDirection.Y =
                        params.ReturnValue.Y / outHorizontalSpeed;
                    haveVelocityDirection = true;
                }
            }
        }

        // A vehicle's instantaneous velocity contains lateral slip while it
        // turns. Extrapolating that vector several seconds ahead is what put
        // Jason on the road shoulder. Use the chassis' longitudinal axis for
        // the intercept line, and use velocity only to determine travel sign
        // and lead distance.
        FVector chassisForward{};
        if (GetJasonAIActorForwardVectorOnGameThread(car, chassisForward))
        {
            const float forwardLength = std::sqrt(
                chassisForward.X * chassisForward.X +
                chassisForward.Y * chassisForward.Y);
            if (std::isfinite(forwardLength) && forwardLength > 0.1f)
            {
                outDirection.X = chassisForward.X / forwardLength;
                outDirection.Y = chassisForward.Y / forwardLength;
                const float velocityDot = haveVelocityDirection
                    ? (outDirection.X * velocityDirection.X +
                       outDirection.Y * velocityDirection.Y)
                    : 1.0f;
                if ((haveVelocityDirection && velocityDot < 0.0f) ||
                    (!haveVelocityDirection && forwardSpeed < 0.0f))
                {
                    outDirection.X = -outDirection.X;
                    outDirection.Y = -outDirection.Y;
                }
                if (outHorizontalSpeed <= 1.0f)
                    outHorizontalSpeed = std::fabs(forwardSpeed);
                return outHorizontalSpeed > 1.0f;
            }
        }

        if (!haveVelocityDirection)
            return false;
        outDirection = velocityDirection;
        return true;
    }

    bool GetSceneComponentLocation(UObject* component, FVector& outLocation)
    {
        outLocation = FVector{};
        if (!component ||
            !Memory::IsReadable(component, sizeof(UObject)) ||
            !component->Class)
        {
            return false;
        }

        UFunction* getLocation = FindFunctionInHierarchyByName(
            component->Class,
            "K2_GetComponentLocation");
        if (!getLocation)
            return false;

        struct Params { FVector ReturnValue; };
        Params params{};
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(component),
                component,
                getLocation,
                &params) ||
            !std::isfinite(params.ReturnValue.X) ||
            !std::isfinite(params.ReturnValue.Y) ||
            !std::isfinite(params.ReturnValue.Z))
        {
            return false;
        }

        outLocation = params.ReturnValue;
        return true;
    }

    void ResetVehicleRoadPointRegistry()
    {
        g_VehicleRoadPointWorld = nullptr;
        std::memset(g_VehicleRoadPoints, 0, sizeof(g_VehicleRoadPoints));
        g_VehicleRoadPointCount = 0;
        g_VehicleRoadNeighborsOffset = -2;
    }

    bool BuildVehicleRoadPointRegistry()
    {
        UWorld* world = g_JasonAIState.World;
        if (!world || !Memory::IsReadable(world, sizeof(UWorld)))
            return false;
        if (g_VehicleRoadPointWorld == world && g_VehicleRoadPointCount > 0)
            return true;

        ResetVehicleRoadPointRegistry();
        g_VehicleRoadPointWorld = world;
        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data || levels->Count <= 0 || levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return false;
        }

        for (int32_t levelIndex = 0;
            levelIndex < levels->Count &&
                g_VehicleRoadPointCount <
                    static_cast<int32_t>(std::size(g_VehicleRoadPoints));
            ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;
            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data || actors.Count <= 0 || actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t actorIndex = 0;
                actorIndex < actors.Count &&
                    g_VehicleRoadPointCount <
                        static_cast<int32_t>(std::size(g_VehicleRoadPoints));
                ++actorIndex)
            {
                AActor* actor = actors.Data[actorIndex];
                if (ObjectClassDerivesFromExact(
                        reinterpret_cast<UObject*>(actor),
                        "SCRoadPoint"))
                {
                    g_VehicleRoadPoints[g_VehicleRoadPointCount++] = actor;
                }
            }
        }

        if (g_VehicleRoadPointCount <= 0)
            return false;

        UPropertyLite* neighborsProperty = FindPropertyInHierarchyByName(
            g_VehicleRoadPoints[0]->Class,
            "Neighbors");
        if (neighborsProperty &&
            neighborsProperty->Offset_Internal > 0 &&
            neighborsProperty->Offset_Internal < 0x10000)
        {
            g_VehicleRoadNeighborsOffset =
                neighborsProperty->Offset_Internal;
        }

        Logger::Success(
            "18L-AI vehicle intercept: cached native road graph | points=" +
            std::to_string(g_VehicleRoadPointCount) +
            " | neighborsOffset=" +
            std::to_string(g_VehicleRoadNeighborsOffset));
        return g_VehicleRoadNeighborsOffset > 0;
    }

    bool ReadVehicleRoadNeighbors(
        AActor* roadPoint,
        AActor**& outNeighbors,
        int32_t& outCount)
    {
        outNeighbors = nullptr;
        outCount = 0;
        if (!roadPoint || g_VehicleRoadNeighborsOffset <= 0)
            return false;

        TArray<AActor*>* neighbors = reinterpret_cast<TArray<AActor*>*>(
            reinterpret_cast<uintptr_t>(roadPoint) +
            g_VehicleRoadNeighborsOffset);
        if (!Memory::IsReadable(neighbors, sizeof(TArray<AActor*>)) ||
            !neighbors->Data || neighbors->Count <= 0 || neighbors->Count > 8 ||
            !Memory::IsReadable(
                neighbors->Data,
                sizeof(AActor*) * static_cast<size_t>(neighbors->Count)))
        {
            return false;
        }

        outNeighbors = neighbors->Data;
        outCount = neighbors->Count;
        return true;
    }

    bool ResolveVehicleRoadCenterIntercept(
        const FVector& carLocation,
        const FVector& travelDirection,
        float requestedLead,
        FVector& outLocation,
        FVector& outRouteDirection)
    {
        outLocation = FVector{};
        outRouteDirection = FVector{};
        if (!BuildVehicleRoadPointRegistry())
            return false;

        // Anchor the prediction to the road EDGE under the car, rather than
        // merely choosing the nearest road node.  At junctions the nearest
        // node can belong to a crossing/parallel branch and was the source of
        // otherwise valid Morphs landing on the wrong road or its shoulder.
        AActor* nearest = nullptr;
        AActor* firstForward = nullptr;
        FVector nearestLocation{};
        FVector firstForwardLocation{};
        float bestEdgeScore = FLT_MAX;
        for (int32_t i = 0; i < g_VehicleRoadPointCount; ++i)
        {
            AActor* point = g_VehicleRoadPoints[i];
            FVector pointLocation{};
            if (!point || !GetJasonAIActorLocation(point, pointLocation))
                continue;

            AActor** neighbors = nullptr;
            int32_t neighborCount = 0;
            if (!ReadVehicleRoadNeighbors(point, neighbors, neighborCount))
                continue;

            for (int32_t neighborIndex = 0;
                neighborIndex < neighborCount;
                ++neighborIndex)
            {
                AActor* neighbor = neighbors[neighborIndex];
                FVector neighborLocation{};
                if (!neighbor ||
                    !ObjectClassDerivesFromExact(
                        reinterpret_cast<UObject*>(neighbor),
                        "SCRoadPoint") ||
                    !GetJasonAIActorLocation(neighbor, neighborLocation))
                {
                    continue;
                }

                float segmentX = neighborLocation.X - pointLocation.X;
                float segmentY = neighborLocation.Y - pointLocation.Y;
                const float segmentLengthSquared =
                    segmentX * segmentX + segmentY * segmentY;
                if (!std::isfinite(segmentLengthSquared) ||
                    segmentLengthSquared < 1.0f)
                {
                    continue;
                }

                const float segmentLength = std::sqrt(segmentLengthSquared);
                const float unitX = segmentX / segmentLength;
                const float unitY = segmentY / segmentLength;
                const float signedAlignment =
                    unitX * travelDirection.X +
                    unitY * travelDirection.Y;
                const float alignment = std::fabs(signedAlignment);
                if (!std::isfinite(alignment) || alignment < 0.45f)
                    continue;

                const float projection = (std::max)(
                    0.0f,
                    (std::min)(
                        1.0f,
                        ((carLocation.X - pointLocation.X) * segmentX +
                         (carLocation.Y - pointLocation.Y) * segmentY) /
                            segmentLengthSquared));
                const float projectedX =
                    pointLocation.X + segmentX * projection;
                const float projectedY =
                    pointLocation.Y + segmentY * projection;
                const float lateralX = carLocation.X - projectedX;
                const float lateralY = carLocation.Y - projectedY;
                const float lateralDistanceSquared =
                    lateralX * lateralX + lateralY * lateralY;
                if (!std::isfinite(lateralDistanceSquared) ||
                    lateralDistanceSquared > 700.0f * 700.0f)
                {
                    continue;
                }

                // Prefer the edge physically under the car.  Alignment is a
                // small tie-breaker at junctions, not permission to select a
                // farther parallel road.
                const float edgeScore = lateralDistanceSquared +
                    (1.0f - alignment) * 40000.0f;
                if (edgeScore >= bestEdgeScore)
                    continue;

                bestEdgeScore = edgeScore;
                if (signedAlignment >= 0.0f)
                {
                    nearest = point;
                    nearestLocation = pointLocation;
                    firstForward = neighbor;
                    firstForwardLocation = neighborLocation;
                }
                else
                {
                    nearest = neighbor;
                    nearestLocation = neighborLocation;
                    firstForward = point;
                    firstForwardLocation = pointLocation;
                }
            }
        }
        if (!nearest || !firstForward)
            return false;

        float segmentX = firstForwardLocation.X - nearestLocation.X;
        float segmentY = firstForwardLocation.Y - nearestLocation.Y;
        float segmentZ = firstForwardLocation.Z - nearestLocation.Z;
        float segmentLength = std::sqrt(segmentX * segmentX + segmentY * segmentY);
        if (!std::isfinite(segmentLength) || segmentLength < 1.0f)
            return false;

        // Begin at the car's projection onto its current road-point edge, not
        // at the nearest node. This keeps the requested lead measured from the
        // car while retaining the exact authored road centerline.
        const float projectionNumerator =
            (carLocation.X - nearestLocation.X) * segmentX +
            (carLocation.Y - nearestLocation.Y) * segmentY;
        const float unclampedProjection = projectionNumerator /
            (segmentLength * segmentLength);
        const float projection = (std::max)(
            0.0f,
            (std::min)(1.0f, unclampedProjection));
        FVector currentLocation{};
        currentLocation.X = nearestLocation.X + segmentX * projection;
        currentLocation.Y = nearestLocation.Y + segmentY * projection;
        currentLocation.Z = nearestLocation.Z + segmentZ * projection;
        // The car may be turning, skidding, or occupying one side of the road
        // when this sample is taken. Carrying its current lateral displacement
        // many metres down the graph can put Jason on the shoulder. Morph to
        // the road-route point itself and use the live car only to select the
        // current edge and travel direction.
        AActor* previous = nearest;
        AActor* current = firstForward;
        FVector currentNodeLocation = firstForwardLocation;
        float remainingLead = requestedLead;

        const float distanceToCurrent = segmentLength * (1.0f - projection);
        if (remainingLead <= distanceToCurrent)
        {
            const float alpha = remainingLead / distanceToCurrent;
            outLocation.X = currentLocation.X +
                (currentNodeLocation.X - currentLocation.X) * alpha;
            outLocation.Y = currentLocation.Y +
                (currentNodeLocation.Y - currentLocation.Y) * alpha;
            outLocation.Z = currentLocation.Z +
                (currentNodeLocation.Z - currentLocation.Z) * alpha;
            outRouteDirection.X = segmentX / segmentLength;
            outRouteDirection.Y = segmentY / segmentLength;
            return true;
        }
        remainingLead -= distanceToCurrent;
        float traversedLead = distanceToCurrent;
        outRouteDirection.X = segmentX / segmentLength;
        outRouteDirection.Y = segmentY / segmentLength;

        // Maps use a small linked SCRoadPoint graph. Walk only a bounded
        // number of nodes, choosing the smooth continuation at each junction.
        for (int32_t step = 0; step < 64; ++step)
        {
            AActor** neighbors = nullptr;
            int32_t neighborCount = 0;
            if (!ReadVehicleRoadNeighbors(current, neighbors, neighborCount))
                break;

            AActor* next = nullptr;
            FVector nextLocation{};
            float bestScore = -FLT_MAX;
            for (int32_t i = 0; i < neighborCount; ++i)
            {
                AActor* neighbor = neighbors[i];
                if (!neighbor || neighbor == previous)
                    continue;
                FVector location{};
                if (!ObjectClassDerivesFromExact(
                        reinterpret_cast<UObject*>(neighbor),
                        "SCRoadPoint") ||
                    !GetJasonAIActorLocation(neighbor, location))
                {
                    continue;
                }
                float dx = location.X - currentNodeLocation.X;
                float dy = location.Y - currentNodeLocation.Y;
                const float length = std::sqrt(dx * dx + dy * dy);
                if (!std::isfinite(length) || length < 1.0f)
                    continue;
                dx /= length;
                dy /= length;
                const float score =
                    dx * outRouteDirection.X + dy * outRouteDirection.Y;
                if (score > bestScore)
                {
                    bestScore = score;
                    next = neighbor;
                    nextLocation = location;
                }
            }
            if (!next)
                break;

            segmentX = nextLocation.X - currentNodeLocation.X;
            segmentY = nextLocation.Y - currentNodeLocation.Y;
            segmentZ = nextLocation.Z - currentNodeLocation.Z;
            segmentLength = std::sqrt(
                segmentX * segmentX + segmentY * segmentY);
            if (!std::isfinite(segmentLength) || segmentLength < 1.0f)
                break;
            outRouteDirection.X = segmentX / segmentLength;
            outRouteDirection.Y = segmentY / segmentLength;

            if (remainingLead <= segmentLength)
            {
                const float alpha = remainingLead / segmentLength;
                outLocation.X = currentNodeLocation.X + segmentX * alpha;
                outLocation.Y = currentNodeLocation.Y + segmentY * alpha;
                outLocation.Z = currentNodeLocation.Z + segmentZ * alpha;
                return true;
            }

            remainingLead -= segmentLength;
            traversedLead += segmentLength;
            previous = current;
            current = next;
            currentNodeLocation = nextLocation;
        }

        // Near an authored road endpoint there may be less route remaining
        // than the ideal lead. The farthest connected node is still a valid
        // centerline interception point and is safer than a tangent fallback.
        if (traversedLead >= 2500.0f)
        {
            outLocation = currentNodeLocation;
            return true;
        }
        return false;
    }

    bool IssueAIMoveToLocationOnGameThread(
        UObject* controller,
        const FVector& destination,
        float acceptanceRadius,
        const char* label,
        bool exactCenterline)
    {
        if (!controller ||
            !controller->Class ||
            !Memory::IsReadable(controller, sizeof(UObject)))
        {
            return false;
        }

        UFunction* moveToLocation = FindFunctionInHierarchyByName(
            controller->Class,
            "MoveToLocation");
        if (!moveToLocation)
            return false;

        // UE4.18 AAIController::MoveToLocation reflected parameter layout.
        // Keeping this as the public reflected call avoids constructing a
        // goal-actor request whose target remains inside an occupied car.
        struct MoveToLocationParams
        {
            FVector Dest;
            float AcceptanceRadius;
            bool bStopOnOverlap;
            bool bUsePathfinding;
            bool bProjectDestinationToNavigation;
            bool bCanStrafe;
            uint8_t Padding0[4];
            UClass* FilterClass;
            bool bAllowPartialPath;
            uint8_t ReturnValue;
            uint8_t Padding1[6];
        };
        static_assert(
            sizeof(MoveToLocationParams) == 40,
            "MoveToLocationParams must match UE4.18 alignment");

        MoveToLocationParams params{};
        params.Dest = destination;
        params.AcceptanceRadius = acceptanceRadius;
        params.bStopOnOverlap = false;
        params.bUsePathfinding = !exactCenterline;
        params.bProjectDestinationToNavigation = !exactCenterline;
        // Centerline interception must rotate Jason into the approach vector;
        // preserving an old focus while strafing is what made his capsule
        // slide alongside the hood even with a correct X/Y destination.
        params.bCanStrafe = !exactCenterline;
        params.FilterClass = nullptr;
        params.bAllowPartialPath = !exactCenterline;

        const bool callOK = SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(controller),
            controller,
            moveToLocation,
            &params);
        const bool vehicleMove = label &&
            (std::strcmp(label, "OccupiedCarCenterline") == 0 ||
             std::strcmp(label, "StoppedCarFront") == 0 ||
             std::strcmp(label, "DriverSideDetour") == 0 ||
             std::strcmp(label, "DriverDoorSide") == 0);
        const ULONGLONG now = GetTickCount64();
        if (!vehicleMove || now >= g_NextVehicleMoveLogAt)
        {
            if (vehicleMove)
                g_NextVehicleMoveLogAt = now + 3103;
            Logger::Debug(
                std::string("18L-AI MoveToLocation: objective=") +
                (label ? label : "Unknown") +
                " | call=" + (callOK ? "true" : "false") +
                " | result=" + std::to_string(params.ReturnValue) +
                " | exactCenterline=" +
                (exactCenterline ? "true" : "false") +
                " | x=" + std::to_string(destination.X) +
                " | y=" + std::to_string(destination.Y));
        }
        return callOK && params.ReturnValue != 0;
    }

    bool ResolveDriverDoorNavPoint(
        AActor* car,
        UObject* driverSeat,
        FVector& outDoorPoint)
    {
        outDoorPoint = FVector{};
        if (!car || !driverSeat)
            return false;

        FVector carLocation{};
        FVector seatLocation{};
        if (!GetJasonAIActorLocation(car, carLocation) ||
            !GetSceneComponentLocation(driverSeat, seatLocation))
        {
            return false;
        }

        float sideX = seatLocation.X - carLocation.X;
        float sideY = seatLocation.Y - carLocation.Y;
        float sideLength = std::sqrt(sideX * sideX + sideY * sideY);
        if (!std::isfinite(sideLength) || sideLength < 25.0f)
        {
            FVector forward{};
            if (!GetJasonAIActorForwardVectorOnGameThread(car, forward))
                return false;
            // UE vehicles use the left side for the driver.  This fallback is
            // used only when the seat component has no useful lateral offset.
            sideX = forward.Y;
            sideY = -forward.X;
            sideLength = std::sqrt(sideX * sideX + sideY * sideY);
        }
        if (!std::isfinite(sideLength) || sideLength < 0.1f)
            return false;

        sideX /= sideLength;
        sideY /= sideLength;

        // Start outside the door rather than at the seated pawn.  The three
        // radii accommodate both the two-seat and four-seat car meshes.
        const float radii[] = { 210.0f, 180.0f, 245.0f };
        FVector extent{};
        extent.X = 120.0f;
        extent.Y = 120.0f;
        extent.Z = 180.0f;
        for (float radius : radii)
        {
            FVector candidate = carLocation;
            candidate.X += sideX * radius;
            candidate.Y += sideY * radius;
            candidate.Z = seatLocation.Z;
            FVector projected{};
            if (ProjectJasonAINavPointOnGameThread(
                    candidate,
                    extent,
                    projected) &&
                std::isfinite(projected.X) &&
                std::isfinite(projected.Y) &&
                std::isfinite(projected.Z) &&
                std::fabs(projected.Z - carLocation.Z) <= 180.0f)
            {
                outDoorPoint = projected;
                return true;
            }
        }
        return false;
    }

    bool ResolveDriverSideDetourPoint(
        AActor* car,
        const FVector& driverDoorPoint,
        const FVector& jasonLocation,
        bool useOppositeEnd,
        FVector& outDetourPoint)
    {
        outDetourPoint = FVector{};
        FVector carLocation{};
        FVector forward{};
        if (!car ||
            !GetJasonAIActorLocation(car, carLocation) ||
            !GetJasonAIActorForwardVectorOnGameThread(car, forward))
        {
            return false;
        }

        float sideX = driverDoorPoint.X - carLocation.X;
        float sideY = driverDoorPoint.Y - carLocation.Y;
        const float sideLength = std::sqrt(sideX * sideX + sideY * sideY);
        const float forwardLength = std::sqrt(
            forward.X * forward.X + forward.Y * forward.Y);
        if (!std::isfinite(sideLength) || sideLength < 1.0f ||
            !std::isfinite(forwardLength) || forwardLength < 0.1f)
        {
            return false;
        }
        sideX /= sideLength;
        sideY /= sideLength;
        forward.X /= forwardLength;
        forward.Y /= forwardLength;

        const float jasonLongitudinal =
            (jasonLocation.X - carLocation.X) * forward.X +
            (jasonLocation.Y - carLocation.Y) * forward.Y;
        float endSign = jasonLongitudinal >= 0.0f ? 1.0f : -1.0f;
        if (useOppositeEnd)
            endSign = -endSign;

        // The direct seat path can cut through the hood collision. First send
        // Jason around the nearer front/rear corner and onto the driver's side,
        // then let the normal door-point path close the interaction distance.
        FVector candidate = carLocation;
        candidate.X += sideX * 360.0f + forward.X * endSign * 360.0f;
        candidate.Y += sideY * 360.0f + forward.Y * endSign * 360.0f;
        candidate.Z = driverDoorPoint.Z;

        FVector extent{};
        extent.X = 160.0f;
        extent.Y = 160.0f;
        extent.Z = 180.0f;
        FVector projected{};
        if (!ProjectJasonAINavPointOnGameThread(candidate, extent, projected) ||
            !std::isfinite(projected.X) ||
            !std::isfinite(projected.Y) ||
            !std::isfinite(projected.Z) ||
            std::fabs(projected.Z - carLocation.Z) > 180.0f)
        {
            return false;
        }

        outDetourPoint = projected;
        return true;
    }

    bool ResolveOccupiedCarFrontHoodPoint(
        AActor* car,
        UObject* hoodComponent,
        const FVector& travelDirection,
        FVector& outHoodPoint,
        bool projectToNavigation = true,
        float signedForwardSpeed = 1.0f)
    {
        outHoodPoint = FVector{};
        if (!car || !Memory::IsReadable(car, 0x3D8))
            return false;

        FVector carLocation{};
        if (!GetJasonAIActorLocation(car, carLocation))
            return false;

        // The hood is the vehicle's physical front, even when the driver is
        // reversing. Always prefer the chassis forward axis so Jason runs to
        // the center of the hood rather than toward a side/rear tangent.
        FVector forward{};
        if (!projectToNavigation)
        {
            // During a live chase, the sampled motion vector already supplies
            // the road centerline. Avoid reflected GetActorForwardVector and
            // component-location ProcessEvents on the 419 ms lane; those two
            // synchronous calls caused the short rhythmic driving jerk. A
            // reversing car travels opposite its physical hood direction.
            forward = travelDirection;
            if (signedForwardSpeed < -1.0f)
            {
                forward.X = -forward.X;
                forward.Y = -forward.Y;
            }
        }
        else if (!GetJasonAIActorForwardVectorOnGameThread(car, forward))
        {
            forward = travelDirection;
        }
        float forwardX = forward.X;
        float forwardY = forward.Y;
        const float forwardLength =
            std::sqrt(forwardX * forwardX + forwardY * forwardY);
        if (!std::isfinite(forwardLength) || forwardLength < 1.0f)
        {
            if (!GetJasonAIActorForwardVectorOnGameThread(car, forward))
                return false;
            forwardX = forward.X;
            forwardY = forward.Y;
        }
        const float fallbackLength = std::sqrt(forwardX * forwardX + forwardY * forwardY);
        if (!std::isfinite(fallbackLength) || fallbackLength < 1.0f)
            return false;

        forwardX /= fallbackLength;
        forwardY /= fallbackLength;

        // Prefer the shipped hood interaction component's longitudinal
        // placement. Flatten it onto the chassis axis so an asset-specific
        // lateral component offset can never steer Jason beside the car.
        float hoodLongitudinal = 240.0f;
        FVector componentLocation{};
        const bool haveComponentLocation = projectToNavigation &&
            GetSceneComponentLocation(hoodComponent, componentLocation);
        if (haveComponentLocation)
        {
            const float componentLongitudinal =
                (componentLocation.X - carLocation.X) * forwardX +
                (componentLocation.Y - carLocation.Y) * forwardY;
            if (std::isfinite(componentLongitudinal) &&
                componentLongitudinal >= 80.0f &&
                componentLongitudinal <= 500.0f)
            {
                hoodLongitudinal = componentLongitudinal;
            }
        }

        FVector candidate = carLocation;
        candidate.X += forwardX * hoodLongitudinal;
        candidate.Y += forwardY * hoodLongitudinal;
        candidate.Z = haveComponentLocation &&
                std::isfinite(componentLocation.Z)
            ? componentLocation.Z
            : carLocation.Z;

        // A moving car changes position continuously. Projecting its hood onto
        // the navmesh on every 2503 ms pursuit pass synchronously rebuilt a
        // navigation query and matched the measured two-to-three-second frame
        // hitch. The chassis-derived point is already centered on the hood;
        // defer pathfinding until a materially different MoveTo is actually
        // issued. Stopped-car extraction keeps the projection for precision.
        if (!projectToNavigation)
        {
            candidate.Z = carLocation.Z;
            outHoodPoint = candidate;
            return true;
        }

        FVector projected{};
        FVector extent{};
        extent.X = 240.0f;
        extent.Y = 220.0f;
        extent.Z = 220.0f;
        if (!ProjectJasonAINavPointOnGameThread(
                candidate,
                extent,
                projected) ||
            !std::isfinite(projected.X) ||
            !std::isfinite(projected.Y) ||
            !std::isfinite(projected.Z) ||
            std::fabs(projected.Z - carLocation.Z) > 220.0f)
        {
            // The car can be driving over a road spline that has no walkable
            // nav sample directly beneath its moving hood.  Returning false
            // here preserved an old shoulder path and made Jason run beside
            // the car.  The chassis-derived X/Y is already the exact hood
            // centerline; use the vehicle height and issue the direct,
            // non-pathfinding intercept instead of falling back sideways.
            candidate.Z = carLocation.Z;
            outHoodPoint = candidate;
            return true;
        }

        // Projection proves that this portion of the road has compatible
        // height/navigation, but its X/Y can slide toward the shoulder. Keep
        // the car's true forward centerline and borrow only the safe height.
        candidate.Z = projected.Z;
        outHoodPoint = candidate;
        return true;
    }

    bool TeleportJasonAheadOfVehicle(
        AActor* car,
        const FVector& travelDirection,
        float horizontalSpeed,
        ULONGLONG now)
    {
        AActor* jason = g_JasonAIState.Jason;
        if (!jason ||
            !car ||
            JasonAIMorphRemainingMs(now) != 0 ||
            !Memory::IsReadable(jason, sizeof(UObject)))
        {
            return false;
        }

        FVector jasonLocation{};
        FVector carLocation{};
        if (!GetJasonAIActorLocation(jason, jasonLocation) ||
            !GetJasonAIActorLocation(car, carLocation))
        {
            return false;
        }

        const float currentDX = carLocation.X - jasonLocation.X;
        const float currentDY = carLocation.Y - jasonLocation.Y;
        const float currentDistanceSquared =
            currentDX * currentDX + currentDY * currentDY;
        if (!std::isfinite(currentDistanceSquared) ||
            currentDistanceSquared < 600.0f * 600.0f)
        {
            return false;
        }

        // Once Jason is already established in front of the moving car, keep
        // the confrontation physical.  A fresh Morph here is the visible
        // "teleport away just before contact" regression.
        const float carToJasonX = jasonLocation.X - carLocation.X;
        const float carToJasonY = jasonLocation.Y - carLocation.Y;
        const float aheadDistance =
            carToJasonX * travelDirection.X +
            carToJasonY * travelDirection.Y;
        const float lateralX = carToJasonX -
            travelDirection.X * aheadDistance;
        const float lateralY = carToJasonY -
            travelDirection.Y * aheadDistance;
        const float lateralDistanceSquared =
            lateralX * lateralX + lateralY * lateralY;
        if (aheadDistance > 250.0f &&
            std::isfinite(lateralDistanceSquared) &&
            lateralDistanceSquared <= 1200.0f * 1200.0f)
        {
            return false;
        }

        // Intercept well down-road so a fast car cannot pass Jason during the
        // Morph recovery.  The old 2.5-second lead was consistently late on
        // long escape roads.  Keep the candidate on the road centerline by
        // using a tight projection extent; a wide 500 cm search could slide
        // the teleport onto a roadside nav island.
        const float lead = (std::max)(
            7000.0f,
            (std::min)(12000.0f, horizontalSpeed * 7.5f));
        FVector extent{};
        extent.X = 180.0f;
        extent.Y = 180.0f;
        extent.Z = 180.0f;
        FVector projected{};
        FVector routeDirection = travelDirection;
        float selectedLead = 0.0f;
        const bool usedNativeRoadGraph = ResolveVehicleRoadCenterIntercept(
            carLocation,
            travelDirection,
            lead,
            projected,
            routeDirection);
        if (usedNativeRoadGraph)
        {
            selectedLead = lead;
            // The authored road point provides exact centerline X/Y. Use the
            // nav query only to validate the destination and refine ground Z.
            FVector attempt{};
            if (ProjectJasonAINavPointOnGameThread(
                    projected,
                    extent,
                    attempt) &&
                std::isfinite(attempt.Z) &&
                std::fabs(attempt.Z - projected.Z) <= 220.0f)
            {
                projected.Z = attempt.Z;
            }
        }
        // A long straight-line tangent is not a valid fallback on a curved
        // road: it is exactly how Jason reached shoulders and neighboring
        // roads.  If the authored SCRoadPoint edge cannot be resolved, keep
        // the 4x chase active and retry from the next fresh car sample rather
        // than performing a knowingly unsafe Morph.
        if (selectedLead <= 0.0f)
            return false;

        UFunction* teleportFunction =
            FindFunctionInHierarchyByName(jason->Class, "K2_TeleportTo");
        if (!teleportFunction)
            return false;

        struct Rotation3 { float Pitch; float Yaw; float Roll; };
        struct TeleportParams
        {
            FVector DestLocation;
            Rotation3 DestRotation;
            bool ReturnValue;
        };
        static_assert(sizeof(TeleportParams) == 28,
            "Vehicle TeleportParams must be 28 bytes");

        constexpr float RadiansToDegrees = 57.29577951308232f;
        TeleportParams params{};
        params.DestLocation = projected;
        params.DestRotation.Yaw =
            std::atan2(-routeDirection.Y, -routeDirection.X) *
            RadiansToDegrees;
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(jason),
                jason,
                teleportFunction,
                &params) ||
            !params.ReturnValue)
        {
            return false;
        }

        // K2_TeleportTo can collision-adjust the requested position. Record
        // its actual landing so a roadside result can be distinguished from a
        // road-point prediction error in the next physical test.
        FVector landed{};
        const bool haveLanding = GetJasonAIActorLocation(jason, landed);
        const float landingDX = haveLanding ? landed.X - projected.X : 0.0f;
        const float landingDY = haveLanding ? landed.Y - projected.Y : 0.0f;
        const float landingShiftCm = haveLanding
            ? std::sqrt(landingDX * landingDX + landingDY * landingDY)
            : -1.0f;

        MarkJasonAIMorphTeleportUsed("CarIntercept");
        StopJasonAIMovementForKnifeOnGameThread();
        FaceJasonAIAtStartupTrapObjectiveOnGameThread(car);
        Logger::Success(
            "18L-AI vehicle intercept: Morph completed ahead of occupied car | car=" +
            JasonAISafeName(reinterpret_cast<UObject*>(car)) +
            " | leadCm=" + std::to_string(selectedLead) +
            " | centerline=true" +
            " | routeSource=" +
            (usedNativeRoadGraph ? "SCRoadPoint-neighbors" : "chassis-fallback") +
            " | destinationX=" + std::to_string(projected.X) +
            " | destinationY=" + std::to_string(projected.Y) +
            " | landingX=" + (haveLanding ? std::to_string(landed.X) : "unknown") +
            " | landingY=" + (haveLanding ? std::to_string(landed.Y) : "unknown") +
            " | landingShiftCm=" + std::to_string(landingShiftCm) +
            " | speedCmPerSec=" + std::to_string(horizontalSpeed));
        return true;
    }

    bool CancelJasonInteractionLockOnly(UObject* manager)
    {
        if (!manager || !manager->Class)
            return false;

        static UClass* cachedManagerClass = nullptr;
        static UFunction* cancelFunction = nullptr;
        if (manager->Class != cachedManagerClass)
        {
            cachedManagerClass = manager->Class;
            cancelFunction = FindFunctionInHierarchyByName(
                manager->Class,
                "CLIENT_CancelInteractAttempt");
            if (!cancelFunction)
            {
                cancelFunction = FindFunctionInHierarchyByName(
                    manager->Class,
                    "CLIENT_UnlockInteraction");
            }
        }

        return cancelFunction && SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(manager),
            manager,
            cancelFunction,
            nullptr);
    }

    bool AttemptJasonVehicleComponent(UObject* component)
    {
        AActor* jason = g_JasonAIState.Jason;
        UObject* manager = GetJasonInteractionManager(jason);
        HMODULE module = GetModuleHandle(nullptr);
        UObject* locked = GetLockedJasonInteractable(jason);
        if (!component ||
            !manager ||
            !module ||
            !Memory::IsReadable(component, sizeof(UObject)) ||
            !ObjectClassDerivesFromExact(component, "SCInteractComponent"))
        {
            return false;
        }

        // AttemptInteract is a void dispatch.  Seeing the requested component
        // become the native lock is the strongest acknowledgement available;
        // do not reject that accepted state on the following maintenance pass.
        if (locked)
            return locked == component;

        uintptr_t managerVTable = *reinterpret_cast<uintptr_t*>(manager);
        uintptr_t* attemptSlot = reinterpret_cast<uintptr_t*>(
            managerVTable + 0x430);
        const uintptr_t expected =
            reinterpret_cast<uintptr_t>(module) + 0x0025AC50;
        if (!managerVTable ||
            !Memory::IsReadable(attemptSlot, sizeof(uintptr_t)) ||
            *attemptSlot != expected)
        {
            return false;
        }

        reinterpret_cast<AttemptInteractFn>(*attemptSlot)(
            manager,
            component,
            1,
            false);
        return true;
    }

    bool AttemptJasonVehicleSeatComponent(UObject* seat)
    {
        AActor* jason = g_JasonAIState.Jason;
        UObject* manager = GetJasonInteractionManager(jason);
        HMODULE module = GetModuleHandle(nullptr);
        if (!seat || !manager || !module ||
            !Memory::IsReadable(seat, sizeof(UObject)) ||
            !ObjectClassDerivesFromExact(seat, "SCVehicleSeatComponent") ||
            GetLockedJasonInteractable(jason))
        {
            return false;
        }

        const uintptr_t managerVTable =
            *reinterpret_cast<uintptr_t*>(manager);
        uintptr_t* attemptSlot = reinterpret_cast<uintptr_t*>(
            managerVTable + 0x430);
        const uintptr_t expected =
            reinterpret_cast<uintptr_t>(module) + 0x0025AC50;
        if (!managerVTable ||
            !Memory::IsReadable(attemptSlot, sizeof(uintptr_t)) ||
            *attemptSlot != expected)
        {
            return false;
        }

        // September 11 extracted the driver through the seat wrapper. Keep
        // that proven target, but call it only after the newer stop, occupant,
        // door-position and latent-animation checks in the caller pass.
        __try
        {
            reinterpret_cast<AttemptInteractFn>(*attemptSlot)(
                manager, seat, 1, false);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    UObject* ResolveJasonCarExtractionInteractComponent(
        AActor* car,
        UObject* hoodComponent)
    {
        if (!car || !Memory::IsReadable(car, sizeof(UObject)))
            return nullptr;

        // The seat is an SCVehicleSeatComponent wrapper, not the
        // SCInteractComponent expected by AttemptInteract. Both supported car
        // blueprints expose their real Jason components by reflection. The
        // known hood pointer identifies the stop-car component; the other
        // verified component owns the driver-extraction interaction.
        UObject* candidates[] = {
            ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(car),
                "JasonCarInteractComponent"),
            ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(car),
                "JasonInteractComponent")
        };
        for (UObject* candidate : candidates)
        {
            if (candidate &&
                candidate != hoodComponent &&
                Memory::IsReadable(candidate, sizeof(UObject)) &&
                ObjectClassDerivesFromExact(candidate, "SCInteractComponent"))
            {
                return candidate;
            }
        }
        return nullptr;
    }

    bool AttemptJasonHidingSpotComponent(UObject* component)
    {
        AActor* jason = g_JasonAIState.Jason;
        UObject* manager = GetJasonInteractionManager(jason);
        HMODULE module = GetModuleHandle(nullptr);
        if (!component || !manager || !module ||
            GetLockedJasonInteractable(jason) ||
            !Memory::IsReadable(component, sizeof(UObject)))
        {
            return false;
        }

        uintptr_t managerVTable = *reinterpret_cast<uintptr_t*>(manager);
        uintptr_t* attemptSlot = reinterpret_cast<uintptr_t*>(
            managerVTable + 0x430);
        const uintptr_t expected =
            reinterpret_cast<uintptr_t>(module) + 0x0025AC50;
        if (!managerVTable ||
            !Memory::IsReadable(attemptSlot, sizeof(uintptr_t)) ||
            *attemptSlot != expected)
        {
            return false;
        }

        // Dispatch is asynchronous. Do not require the interaction manager's
        // lock to change inside this native call; the caller owns a short,
        // bounded confirmation window and verifies the exact component/spot.
        reinterpret_cast<AttemptInteractFn>(*attemptSlot)(
            manager,
            component,
            1,
            true);
        return true;
    }

    bool ResolveHidingSpotInteraction(
        UObject* component,
        AActor*& outSpot,
        AActor*& outCounselor,
        UObject*& outKillerInteractable)
    {
        outSpot = nullptr;
        outCounselor = nullptr;
        outKillerInteractable = nullptr;
        if (!component || !Memory::IsReadable(component, sizeof(UObject)))
            return false;

        AActor** ownerField = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(component) + 0xE0);
        if (!Memory::IsReadable(ownerField, sizeof(AActor*)) ||
            !*ownerField ||
            !ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(*ownerField),
                "SCHidingSpot"))
        {
            return false;
        }

        AActor* spot = *ownerField;
        UFunction* getHidingCounselor = FindFunctionInHierarchyByName(
            spot->Class,
            "GetHidingCounselor");
        if (!getHidingCounselor)
            return false;

        struct Params { AActor* ReturnValue; };
        Params params{};
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(spot),
                spot,
                getHidingCounselor,
                &params) ||
            !IsValidatedLiveCounselorPawn(params.ReturnValue))
        {
            return false;
        }

        // A manager candidate can be a side-specific search component (beds
        // are the important example). Preserve that exact candidate instead
        // of replacing it with the spot's generic KillerInteractable field.
        UObject* killerInteractable = component;
        if (!killerInteractable)
        {
            killerInteractable = ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(spot),
                "KillerInteractable");
        }
        if (!killerInteractable)
        {
            killerInteractable = ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(spot),
                "KillerInteractComponent");
        }
        if (!killerInteractable)
            killerInteractable = component;

        outSpot = spot;
        outCounselor = params.ReturnValue;
        outKillerInteractable = killerInteractable;
        return true;
    }

    bool FindNearbyOccupiedHidingSpot(
        AActor*& outSpot,
        AActor*& outCounselor,
        UObject*& outKillerInteractable)
    {
        outSpot = nullptr;
        outCounselor = nullptr;
        outKillerInteractable = nullptr;
        AActor* jason = g_JasonAIState.Jason;
        FVector jasonLocation{};
        if (!jason ||
            !GetJasonAIActorLocation(jason, jasonLocation))
        {
            return false;
        }

        float bestDistanceSquared = 600.0f * 600.0f;
        for (int32_t actorIndex = 0;
            actorIndex < g_HidingSpotRegistryCount;
            ++actorIndex)
        {
            AActor* spot = g_HidingSpotRegistry[actorIndex];
            if (!spot ||
                !Memory::IsReadable(spot, sizeof(UObject)) ||
                !ObjectClassDerivesFromExact(
                    reinterpret_cast<UObject*>(spot),
                    "SCHidingSpot"))
            {
                continue;
            }

            FVector spotLocation{};
            if (!GetJasonAIActorLocation(spot, spotLocation))
                continue;
            const float dx = spotLocation.X - jasonLocation.X;
            const float dy = spotLocation.Y - jasonLocation.Y;
            const float distanceSquared = dx * dx + dy * dy;
            if (!std::isfinite(distanceSquared) ||
                distanceSquared >= bestDistanceSquared)
            {
                continue;
            }

            UObject* component = ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(spot),
                "KillerInteractable");
            if (!component)
            {
                component = ReadReflectedObjectProperty(
                    reinterpret_cast<UObject*>(spot),
                    "KillerInteractComponent");
            }
            AActor* resolvedSpot = nullptr;
            AActor* counselor = nullptr;
            UObject* interactable = nullptr;
            if (ResolveHidingSpotInteraction(
                    component,
                    resolvedSpot,
                    counselor,
                    interactable))
            {
                bestDistanceSquared = distanceSquared;
                outSpot = resolvedSpot;
                outCounselor = counselor;
                outKillerInteractable = interactable;
            }
        }
        return outSpot && outCounselor && outKillerInteractable;
    }

    bool FaceJasonAIAtLocationOnGameThread(const FVector& targetLocation)
    {
        AActor* jason = g_JasonAIState.Jason;
        UObject* controller = g_JasonAIState.Controller;
        FVector jasonLocation{};
        if (!jason || !jason->Class ||
            !GetJasonAIActorLocation(jason, jasonLocation))
        {
            return false;
        }

        const float dx = targetLocation.X - jasonLocation.X;
        const float dy = targetLocation.Y - jasonLocation.Y;
        if (!std::isfinite(dx) || !std::isfinite(dy) ||
            dx * dx + dy * dy < 1.0f)
        {
            return false;
        }

        struct Rotation3 { float Pitch; float Yaw; float Roll; };
        constexpr float RadiansToDegrees = 57.29577951308232f;
        Rotation3 rotation{};
        rotation.Yaw = std::atan2(dy, dx) * RadiansToDegrees;

        bool actorOK = false;
        UFunction* setActorRotation = FindFunctionInHierarchyByName(
            jason->Class,
            "K2_SetActorRotation");
        if (setActorRotation)
        {
            struct Params
            {
                Rotation3 NewRotation;
                bool bTeleportPhysics;
                bool ReturnValue;
                uint8_t Padding[2];
            } params{};
            params.NewRotation = rotation;
            params.bTeleportPhysics = false;
            actorOK = SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(jason),
                jason,
                setActorRotation,
                &params) && params.ReturnValue;
        }

        bool controlOK = false;
        if (controller && controller->Class &&
            Memory::IsReadable(controller, sizeof(UObject)))
        {
            UFunction* setControlRotation = FindFunctionInHierarchyByName(
                controller->Class,
                "SetControlRotation");
            if (setControlRotation)
            {
                struct Params { Rotation3 NewRotation; } params{};
                params.NewRotation = rotation;
                controlOK = SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(controller),
                    controller,
                    setControlRotation,
                    &params);
            }
        }
        return actorOK || controlOK;
    }

    bool IsHidingSpotLockMatch(
        UObject* locked,
        UObject* requested,
        AActor* spot)
    {
        if (!locked)
            return false;
        if (locked == requested)
            return true;

        AActor** ownerField = reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(locked) + 0xE0);
        return Memory::IsReadable(ownerField, sizeof(AActor*)) &&
            *ownerField == spot;
    }

    bool ReleaseStaleJasonHidingInteraction(
        UObject* manager,
        AActor* jason,
        ULONGLONG now,
        const char* reason)
    {
        if (!manager || !jason)
            return false;

        static UClass* cachedManagerClass = nullptr;
        static UFunction* cancelFunction = nullptr;
        if (manager->Class != cachedManagerClass)
        {
            cachedManagerClass = manager->Class;
            cancelFunction = FindFunctionInHierarchyByName(
                manager->Class,
                "CLIENT_CancelInteractAttempt");
            if (!cancelFunction)
            {
                cancelFunction = FindFunctionInHierarchyByName(
                    manager->Class,
                    "CLIENT_UnlockInteraction");
            }
        }

        const bool cancelled = cancelFunction &&
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(manager),
                manager,
                cancelFunction,
                nullptr);

        UFunction* endStun = jason->Class
            ? FindFunctionInHierarchyByName(jason->Class, "EndStun")
            : nullptr;
        const bool endedStun = endStun &&
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(jason),
                jason,
                endStun,
                nullptr);

        g_JasonAIState.GrabKillReadyAt = 0;
        g_JasonAIState.PathLocked = false;
        g_JasonAIState.PathLockUntil = 0;
        g_JasonAIState.LastAcceptedMoveAt = 0;
        ResetStuckSamplingAfterNativeInteraction(now);
        Logger::Debug(
            std::string("18L-AZ hiding spot: stale native state released | reason=") +
            (reason ? reason : "unknown") +
            " | cancel=" + (cancelled ? "true" : "false") +
            " | endStun=" + (endedStun ? "true" : "false"));
        return cancelled || endedStun;
    }

    bool RepositionJasonForHidingInteraction(
        AActor* jason,
        const FVector& interactionLocation,
        const FVector& currentLocation)
    {
        if (!jason || !jason->Class)
            return false;

        UFunction* teleportFunction = FindFunctionInHierarchyByName(
            jason->Class,
            "K2_TeleportTo");
        if (!teleportFunction)
            return false;

        struct Rotation3 { float Pitch; float Yaw; float Roll; };
        struct TeleportParams
        {
            FVector DestLocation;
            Rotation3 DestRotation;
            bool ReturnValue;
        };
        static_assert(sizeof(TeleportParams) == 28,
            "Hiding TeleportParams must be 28 bytes");

        constexpr float RadiansToDegrees = 57.29577951308232f;
        const FVector offsets[] =
        {
            FVector{ 95.0f, 0.0f, 0.0f },
            FVector{ -95.0f, 0.0f, 0.0f },
            FVector{ 0.0f, 95.0f, 0.0f },
            FVector{ 0.0f, -95.0f, 0.0f }
        };

        StopJasonAIMovementForKnifeOnGameThread();
        for (const FVector& offset : offsets)
        {
            TeleportParams params{};
            params.DestLocation.X = interactionLocation.X + offset.X;
            params.DestLocation.Y = interactionLocation.Y + offset.Y;
            // Component origins can sit high inside a bed, closet or tent.
            // Preserve Jason's collision-tested floor height.
            params.DestLocation.Z = currentLocation.Z;
            params.DestRotation.Yaw = std::atan2(
                interactionLocation.Y - params.DestLocation.Y,
                interactionLocation.X - params.DestLocation.X) *
                RadiansToDegrees;
            if (SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(jason),
                    jason,
                    teleportFunction,
                    &params) &&
                params.ReturnValue)
            {
                FaceJasonAIAtLocationOnGameThread(interactionLocation);
                Logger::Success(
                    "18L-AZ hiding spot: collision-checked interaction reposition completed");
                return true;
            }
        }
        return false;
    }

    void ClearActiveHidingSpotState()
    {
        g_HidingSpotTarget = nullptr;
        g_HidingSpotCounselor = nullptr;
        g_HidingSpotInteractable = nullptr;
        g_HidingSpotStartedAt = 0;
        g_HidingSpotAttemptPendingUntil = 0;
        g_NextHidingSpotMoveAt = 0;
        g_HidingSpotAttempts = 0;
        g_HidingSpotRepositionAttempted = false;
        g_HidingSpotRepositionSucceeded = false;
    }

    void TemporarilyBlockHidingCounselor(
        AActor* counselor,
        ULONGLONG until)
    {
        const int32_t index = FindJasonAITargetIndex(counselor);
        if (index >= 0 && index < JasonAITargetCapacity)
            g_JasonAITargetBlockedUntil[index] = until;
    }

    bool AbandonRejectedHidingSpot(
        AActor* spot,
        AActor* counselor,
        const FVector& interactionLocation,
        const FVector& jasonLocation,
        ULONGLONG now)
    {
        ReleaseStaleJasonHidingInteraction(
            GetJasonInteractionManager(g_JasonAIState.Jason),
            g_JasonAIState.Jason,
            now,
            "bounded-abandon");
        const bool haveAlternative =
            HasAlternativeUsableJasonAITarget(counselor);
        const ULONGLONG retryDelay = haveAlternative ? 25000 : 7000;
        g_IgnoredHidingSpot = spot;
        g_IgnoredHidingCounselor = counselor;
        g_HidingSpotIgnoreUntil = now + retryDelay;
        TemporarilyBlockHidingCounselor(
            counselor,
            g_HidingSpotIgnoreUntil);

        g_JasonAIState.Target = nullptr;
        g_JasonAIState.PathLocked = false;
        g_JasonAIState.PathLockUntil = 0;
        g_JasonAIState.LastAcceptedMoveAt = 0;

        // When this is the last counselor, step away from the failed native
        // point before retrying. Repeating at the identical transform can
        // never satisfy the component's distance/yaw tests.
        if (!haveAlternative)
        {
            float awayX = jasonLocation.X - interactionLocation.X;
            float awayY = jasonLocation.Y - interactionLocation.Y;
            float awayLength = std::sqrt(awayX * awayX + awayY * awayY);
            if (!std::isfinite(awayLength) || awayLength < 1.0f)
            {
                awayX = 1.0f;
                awayY = 0.0f;
                awayLength = 1.0f;
            }
            FVector retreat = jasonLocation;
            retreat.X += awayX / awayLength * 450.0f;
            retreat.Y += awayY / awayLength * 450.0f;
            IssueAIMoveToLocationOnGameThread(
                g_JasonAIState.Controller,
                retreat,
                75.0f,
                "HidingRetryRetreat");
        }

        Logger::Debug(
            "18L-AX hiding spot: bounded native search exhausted; "
            "suppressing hidden-target combat and retargeting | spot=" +
            JasonAISafeName(reinterpret_cast<UObject*>(spot)) +
            " | counselor=" +
            JasonAISafeName(reinterpret_cast<UObject*>(counselor)) +
            " | attempts=" + std::to_string(g_HidingSpotAttempts) +
            " | retryMs=" + std::to_string(retryDelay) +
            " | alternative=" + (haveAlternative ? "true" : "false"));
        ClearActiveHidingSpotState();
        return true;
    }

    bool DriveHidingSpotInteraction(ULONGLONG now)
    {
        AActor* jason = g_JasonAIState.Jason;
        UObject* manager = GetJasonInteractionManager(jason);
        if (!jason || !manager)
            return false;

        if (g_HidingSpotIgnoreUntil != 0 &&
            now >= g_HidingSpotIgnoreUntil)
        {
            g_IgnoredHidingSpot = nullptr;
            g_IgnoredHidingCounselor = nullptr;
            g_HidingSpotIgnoreUntil = 0;
        }

        AActor* spot = nullptr;
        AActor* counselor = nullptr;
        UObject* killerInteractable = nullptr;

        // Keep a pending native interaction stable even if the manager clears
        // its hover candidate during the first animation/lock frames.
        if (g_HidingSpotInteractable)
        {
            ResolveHidingSpotInteraction(
                g_HidingSpotInteractable,
                spot,
                counselor,
                killerInteractable);
        }

        // The manager candidate is the cheapest and most authoritative route.
        const uintptr_t candidateOffsets[] = { 0x230, 0x210 };
        for (uintptr_t offset : candidateOffsets)
        {
            if (spot)
                break;
            UObject** candidate = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(manager) + offset);
            if (Memory::IsReadable(candidate, sizeof(UObject*)) &&
                *candidate &&
                ResolveHidingSpotInteraction(
                    *candidate,
                    spot,
                    counselor,
                    killerInteractable))
            {
                break;
            }
        }

        if (!spot &&
            g_JasonAIState.ConsecutiveStuckChecks >= 1 &&
            now >= g_NextHidingSpotScanAt)
        {
            g_NextHidingSpotScanAt = now + 1000;
            FindNearbyOccupiedHidingSpot(
                spot,
                counselor,
                killerInteractable);
        }

        if (!spot || !counselor || !killerInteractable)
        {
            ClearActiveHidingSpotState();
            return false;
        }

        if (spot == g_IgnoredHidingSpot &&
            counselor == g_IgnoredHidingCounselor &&
            now < g_HidingSpotIgnoreUntil)
        {
            TemporarilyBlockHidingCounselor(
                counselor,
                g_HidingSpotIgnoreUntil);
            return false;
        }

        if (spot != g_HidingSpotTarget ||
            counselor != g_HidingSpotCounselor ||
            killerInteractable != g_HidingSpotInteractable)
        {
            ClearActiveHidingSpotState();
            g_HidingSpotTarget = spot;
            g_HidingSpotCounselor = counselor;
            g_HidingSpotInteractable = killerInteractable;
            g_HidingSpotStartedAt = now;
            Logger::Success(
                "18L-AX hiding spot: occupied native interaction acquired | spot=" +
                JasonAISafeName(reinterpret_cast<UObject*>(spot)) +
                " | counselor=" +
                JasonAISafeName(reinterpret_cast<UObject*>(counselor)) +
                " | component=" +
                JasonAISafeName(killerInteractable));
        }

        // A verified hidden counselor must never fall through to ordinary
        // slash/grab combat. This helper owns either approach, interaction,
        // or bounded abandonment/retargeting.
        g_JasonAIState.Target = counselor;

        FVector jasonLocation{};
        FVector interactionLocation{};
        if (!GetJasonAIActorLocation(jason, jasonLocation))
            return true;
        if (!GetSceneComponentLocation(
                killerInteractable,
                interactionLocation) &&
            !GetJasonAIActorLocation(spot, interactionLocation))
        {
            return true;
        }

        const float dx = interactionLocation.X - jasonLocation.X;
        const float dy = interactionLocation.Y - jasonLocation.Y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(distance))
            return true;

        UObject* locked = GetLockedJasonInteractable(jason);
        if (IsHidingSpotLockMatch(
                locked,
                killerInteractable,
                spot))
        {
            // A real hiding kill completes quickly. A lock that survives this
            // bound is a failed montage/interaction, not useful progress.
            if (now >= g_HidingSpotStartedAt + 12000)
            {
                Logger::Debug(
                    "18L-AZ hiding spot: matching native lock timed out");
                return AbandonRejectedHidingSpot(
                    spot,
                    counselor,
                    interactionLocation,
                    jasonLocation,
                    now);
            }
            g_HidingSpotAttemptPendingUntil = 0;
            ResetStuckSamplingAfterNativeInteraction(now);
            return true;
        }
        if (locked)
        {
            // Never let an unrelated door, window, loot or rejected hiding
            // interaction own Jason forever. Give the stock transition a
            // brief grace period, then ask the manager to release it.
            if (now >= g_HidingSpotStartedAt + 2000)
            {
                Logger::Debug(
                    "18L-AZ hiding spot: foreign native lock timed out");
                return AbandonRejectedHidingSpot(
                    spot,
                    counselor,
                    interactionLocation,
                    jasonLocation,
                    now);
            }
            ResetStuckSamplingAfterNativeInteraction(now);
            return true;
        }

        if (now >= g_HidingSpotStartedAt + 14000 ||
            g_HidingSpotAttempts >= 6)
        {
            return AbandonRejectedHidingSpot(
                spot,
                counselor,
                interactionLocation,
                jasonLocation,
                now);
        }

        // Exported hiding assets use a 150 cm native DistanceLimit, measured
        // from a component offset inside the prop. Use a stronger margin than
        // the old 115 cm cutoff. If navmesh cannot reach that point, perform
        // one collision-tested local reposition after five seconds.
        if (distance > 85.0f &&
            !g_HidingSpotRepositionSucceeded)
        {
            if (!g_HidingSpotRepositionAttempted &&
                now >= g_HidingSpotStartedAt + 5000)
            {
                g_HidingSpotRepositionAttempted = true;
                if (RepositionJasonForHidingInteraction(
                        jason,
                        interactionLocation,
                        jasonLocation))
                {
                    g_HidingSpotRepositionSucceeded = true;
                    g_NextHidingSpotInteractAt = now + 250;
                    return true;
                }
            }
            if (now >= g_NextHidingSpotMoveAt)
            {
                g_NextHidingSpotMoveAt = now + 1000;
                IssueAIMoveToLocationOnGameThread(
                    g_JasonAIState.Controller,
                    interactionLocation,
                    65.0f,
                    "OccupiedHidingPoint");
            }
            return true;
        }

        if (g_HidingSpotAttemptPendingUntil != 0 &&
            now < g_HidingSpotAttemptPendingUntil)
        {
            return true;
        }
        g_HidingSpotAttemptPendingUntil = 0;

        if (now >= g_NextHidingSpotInteractAt)
        {
            if (g_HidingSpotAttempts >= 2 &&
                !g_HidingSpotRepositionAttempted)
            {
                g_HidingSpotRepositionAttempted = true;
                if (RepositionJasonForHidingInteraction(
                        jason,
                        interactionLocation,
                        jasonLocation))
                {
                    g_HidingSpotRepositionSucceeded = true;
                    g_NextHidingSpotInteractAt = now + 250;
                    return true;
                }
            }
            g_NextHidingSpotInteractAt = now + 850;
            StopJasonAIMovementForKnifeOnGameThread();
            FaceJasonAIAtLocationOnGameThread(interactionLocation);
            const bool dispatched =
                AttemptJasonHidingSpotComponent(killerInteractable);
            ++g_HidingSpotAttempts;
            if (dispatched)
                g_HidingSpotAttemptPendingUntil = now + 650;
            if (now >= g_NextHidingSpotLogAt)
            {
                g_NextHidingSpotLogAt = now + 2500;
                Logger::Debug(
                    "18L-AX hiding spot: native killer action dispatched | distanceCm=" +
                    std::to_string(distance) + " | attempt=" +
                    std::to_string(g_HidingSpotAttempts) + " | component=" +
                    JasonAISafeName(killerInteractable) + " | call=" +
                    (dispatched ? "true" : "false"));
            }
        }
        return true;
    }

    bool DriveOccupiedVehicleInterception(ULONGLONG now)
    {
        if (now < g_VehicleInterceptRetryAfter)
        {
            return g_VehicleInterceptCar != nullptr ||
                (g_IgnoredVehicleInterceptCar != nullptr &&
                 now < g_IgnoredVehicleInterceptUntil);
        }

        AActor* jason = g_JasonAIState.Jason;
        if (!jason)
            return false;

        UObject** extractionComponent = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(jason) + 0x1558);
        if (Memory::IsReadable(extractionComponent, sizeof(UObject*)) &&
            *extractionComponent)
        {
            // CurrentVehicleGrabKillComp means the stock pull-out action has
            // started, not that the driver has left the seat. The older
            // working route kept this car lane alive here. Clearing it at the
            // animation start let general chase/grab AI interrupt the pull.
            if (g_VehicleInterceptCar &&
                now >= g_NextVehicleInterceptLogAt)
            {
                g_NextVehicleInterceptLogAt = now + 2000;
                Logger::Debug(
                    "18L-BQ native driver pull-out active; preserving car lane until seat clears");
            }
            ResetStuckSamplingAfterNativeInteraction(now);
            return true;
        }

        if (now < g_NextVehicleInterceptActionAt)
            return g_VehicleInterceptCar != nullptr;

        // Keep the proven non-harmonic chase cadence. The 419 ms experiment
        // doubled hood/route work during driving without improving pursuit.
        // Cached-seat validation below still avoids the full car registry.
        g_NextVehicleInterceptActionAt = now +
            (g_VehicleInterceptCar ? 877 : 1379);
        UObject* seat = nullptr;
        AActor* occupant = nullptr;
        AActor* car = nullptr;
        // Once a chase is acquired, validate its exact cached seat directly.
        // Re-running the car registry and every seat at the pursuit
        // cadence caused the regular driving hitch even though neither the car
        // nor driver had changed. Fall back to discovery only when that cheap
        // relationship check fails.
        bool cachedSeatValid = false;
        if (g_VehicleInterceptCar && g_VehicleInterceptSeat)
        {
            cachedSeatValid = ReadCachedVehicleSeatFast(
                g_VehicleInterceptSeat,
                g_VehicleInterceptCar,
                occupant);
        }
        if (cachedSeatValid)
        {
            car = g_VehicleInterceptCar;
            seat = g_VehicleInterceptSeat;
            g_NextVehicleSeatRediscoveryAt = 0;
        }
        else
        {
            // Seat/exit animations can transiently invalidate the cached
            // relationship. Do not rescan every car and seat on each
            // chase sample while the same vehicle is still settling.
            if (g_VehicleInterceptCar &&
                now < g_NextVehicleSeatRediscoveryAt)
            {
                return true;
            }
            g_NextVehicleSeatRediscoveryAt = now + 1501;
            car = FindOccupiedEscapeCar(seat, occupant);
        }
        if (!car || !seat || !occupant)
        {
            if (g_VehicleInterceptCar &&
                g_VehicleExtractionInputAt != 0 &&
                now < g_VehicleExtractionInputAt + 7000)
            {
                // bLeavingSeat can temporarily hide the occupant from the
                // normal live-seat filter while the stock pull-out animation
                // is still running. Do not hand Jason to combat mid-action.
                return true;
            }
            if (g_VehicleInterceptCar)
            {
                Logger::Debug(
                    "18L-AI vehicle intercept: occupant left/died or car escaped; resuming counselor hunt");
                StopJasonAIMovementForKnifeOnGameThread();
            }
            ResetVehicleInterceptionState(now + 2000);
            g_JasonAIState.Target = nullptr;
            g_JasonAIState.PathLocked = false;
            return false;
        }

        if (car != g_VehicleInterceptCar)
        {
            ResetVehicleInterceptionState();
            g_VehicleInterceptCar = car;
            g_VehicleInterceptSeat = seat;
            g_VehicleInterceptDeadline = now + 12000;
            g_JasonAIState.Target = occupant;
            Logger::Success(
                "18L-AI vehicle intercept: occupied started car acquired | car=" +
                JasonAISafeName(reinterpret_cast<UObject*>(car)) +
                " | counselor=" +
                JasonAISafeName(reinterpret_cast<UObject*>(occupant)));
        }
        else
        {
            g_VehicleInterceptSeat = seat;
            g_JasonAIState.Target = occupant;
        }

        if (g_VehicleInterceptDeadline != 0 &&
            now >= g_VehicleInterceptDeadline &&
            g_VehicleHoodInputSentAt == 0 &&
            !g_VehicleExtractionCommitted)
        {
            // Do not renew the same failed approach forever. Temporarily
            // exclude this car so the next bounded discovery can acquire a
            // second occupied escape car; with only one car, normal pursuit
            // gets a short reset before Jason approaches it again.
            g_IgnoredVehicleInterceptCar = car;
            g_IgnoredVehicleInterceptUntil = now + 15000;
            Logger::Error(
                "18L-BD vehicle intercept stalled; abandoning current approach and retargeting");
            ResetVehicleInterceptionState(now + 500);
            g_JasonAIState.Target = nullptr;
            ResetStuckSamplingAfterNativeInteraction(now);
            return false;
        }

        uint8_t* beingSlammed = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(car) + 0x5BA);
        if (Memory::IsReadable(beingSlammed, 1) && *beingSlammed != 0)
        {
            if (g_VehicleHoodInputSentAt == 0 &&
                g_VehicleMovingHoodAttemptAt != 0)
            {
                g_VehicleHoodInputSentAt = g_VehicleMovingHoodAttemptAt;
            }
            g_VehicleSlamObserved = true;
            g_VehicleExtractionCommitted = true;
            if (g_VehicleExtractionCommittedAt == 0)
                g_VehicleExtractionCommittedAt = now;
            StopJasonAIMovementForKnifeOnGameThread();
            ResetStuckSamplingAfterNativeInteraction(now);
            return true;
        }

        // AttemptInteract is void. If a fallback input made only a grab
        // sound/lock while the authoritative seat still contains the same
        // counselor, release that false hold and retry the door approach.
        // A genuine extraction is handled by Jason's live extraction pointer
        // at the top of this function and never reaches this recovery.
        if (g_VehicleExtractionInputAt != 0 &&
            now >= g_VehicleExtractionInputAt + 5500)
        {
            AActor* stillSeated = nullptr;
            if (ReadLiveVehicleSeat(seat, car, stillSeated) &&
                stillSeated == occupant)
            {
                UObject* locked = GetLockedJasonInteractable(jason);
                if (locked == seat)
                    CancelJasonInteractionLockOnly(
                        GetJasonInteractionManager(jason));
                g_VehicleExtractionInputAt = 0;
                g_VehicleExtractionAttempts = 0;
                g_VehicleDriverReadySince = 0;
                g_VehicleHaveCachedDriverDoorPoint = false;
                g_VehicleHaveLastDriverMovePoint = false;
                g_VehicleLastDriverMoveAt = 0;
                Logger::Error(
                    "18L-BQ extraction input did not remove driver from seat; resetting door approach");
            }
        }

        // The native vehicle-movement speed query is not needed on every
        // centerline sample. Position deltas below still refresh at 877 ms;
        // sample native speed separately for stop/reverse decisions.
        if (now >= g_NextVehicleForwardSpeedSampleAt)
        {
            g_CachedVehicleForwardSpeed = SafeVehicleForwardSpeed(car);
            g_NextVehicleForwardSpeedSampleAt = now + 1201;
        }
        const float forwardSpeed = g_CachedVehicleForwardSpeed;
        const float absoluteSpeed = std::fabs(forwardSpeed);
        float* killerSlamSpeed = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(car) + 0x528);
        float* minDoorSpeed = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(car) + 0x604);
        const float liveSlamSpeed =
            Memory::IsReadable(killerSlamSpeed, sizeof(float)) &&
            std::isfinite(*killerSlamSpeed) && *killerSlamSpeed >= 0.0f
                ? *killerSlamSpeed
                : 5.0f;
        const float liveDoorSpeed =
            Memory::IsReadable(minDoorSpeed, sizeof(float)) &&
            std::isfinite(*minDoorSpeed) && *minDoorSpeed >= 0.0f
                ? *minDoorSpeed
                : 50.0f;
        uint8_t* started = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(car) + 0x47A);
        const bool carStarted =
            Memory::IsReadable(started, 1) && *started != 0;
        UObject* hood = nullptr;
        UObject** hoodComponent = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(car) + 0x3F0);
        if (Memory::IsReadable(hoodComponent, sizeof(UObject*)) &&
            *hoodComponent &&
            Memory::IsReadable(*hoodComponent, 0x2A1))
        {
            hood = *hoodComponent;
        }
        const bool hoodEnabled = hood &&
            *reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(hood) + 0x2A0) != 0;

        // A stopped car can be restarted before extraction completes. The
        // previous permanent latch then kept Jason in the stationary door
        // lane while the driver escaped. Only real forward motion re-arms
        // pursuit; a transient restart flag or a rolling stop does not.
        if (g_VehicleExtractionCommitted && carStarted &&
            g_VehicleExtractionCommittedAt != 0 &&
            now >= g_VehicleExtractionCommittedAt + 1500 &&
            absoluteSpeed > (std::max)(150.0f, liveSlamSpeed * 2.0f))
        {
            g_VehicleExtractionCommitted = false;
            g_VehicleExtractionCommittedAt = 0;
            g_VehicleSlamObserved = false;
            g_VehicleHoodInputSentAt = 0;
            g_VehicleMovingHoodAttemptAt = 0;
            g_VehicleMovingHoodAttempts = 0;
            g_VehicleExtractionInputAt = 0;
            g_VehicleExtractionAttempts = 0;
            g_VehicleDriverReadySince = 0;
            g_VehicleDriverDetourReached = false;
            g_VehicleDriverApproachStartedAt = 0;
            g_VehicleHaveCachedDriverDoorPoint = false;
            g_VehicleHaveCachedDriverDetourPoint = false;
            g_VehicleHaveLastDriverMovePoint = false;
            g_VehicleInterceptMorphUsed = false;
            g_VehicleHeadingStableSince = 0;
            g_VehicleInterceptDeadline = now + 12000;
            Logger::Debug(
                "18L-BM vehicle intercept: driver restarted and accelerated; resuming moving-car chase");
        }

        FVector jasonLocation{};
        FVector carLocation{};
        if (!GetJasonAIActorLocation(jason, jasonLocation) ||
            !GetJasonAIActorLocation(car, carLocation))
        {
            return true;
        }

        const float dx = carLocation.X - jasonLocation.X;
        const float dy = carLocation.Y - jasonLocation.Y;
        const float horizontalDistance = std::sqrt(dx * dx + dy * dy);
        FVector travelDirection{};
        float horizontalSpeed = 0.0f;
        bool haveDirection = false;
        if (g_VehicleMotionSampleCar == car &&
            g_VehicleMotionSampleAt != 0 &&
            now > g_VehicleMotionSampleAt)
        {
            const float sampleDX = carLocation.X -
                g_VehicleMotionSampleLocation.X;
            const float sampleDY = carLocation.Y -
                g_VehicleMotionSampleLocation.Y;
            const float sampleDistance = std::sqrt(
                sampleDX * sampleDX + sampleDY * sampleDY);
            const float sampleSeconds = static_cast<float>(
                now - g_VehicleMotionSampleAt) / 1000.0f;
            if (std::isfinite(sampleDistance) &&
                std::isfinite(sampleSeconds) &&
                sampleDistance > 4.0f &&
                sampleSeconds > 0.02f &&
                sampleSeconds < 2.0f)
            {
                travelDirection.X = sampleDX / sampleDistance;
                travelDirection.Y = sampleDY / sampleDistance;
                horizontalSpeed = sampleDistance / sampleSeconds;
                g_VehicleMotionSampleDirection = travelDirection;
                g_VehicleMotionSampleSpeed = horizontalSpeed;
                g_VehicleHaveMotionSample = true;
                haveDirection = true;
            }
        }
        if (!haveDirection &&
            g_VehicleMotionSampleCar == car &&
            g_VehicleHaveMotionSample)
        {
            travelDirection = g_VehicleMotionSampleDirection;
            horizontalSpeed = g_VehicleMotionSampleSpeed;
            haveDirection = true;
        }
        if (!haveDirection)
        {
            // Only acquisition/telemetry-loss needs the reflected velocity
            // and chassis calls. Normal pursuit direction comes from cheap
            // position deltas above.
            haveDirection = GetVehicleTravelDirection(
                car,
                forwardSpeed,
                travelDirection,
                horizontalSpeed);
            if (haveDirection)
            {
                g_VehicleMotionSampleDirection = travelDirection;
                g_VehicleMotionSampleSpeed = horizontalSpeed;
                g_VehicleHaveMotionSample = true;
            }
        }
        g_VehicleMotionSampleCar = car;
        g_VehicleMotionSampleLocation = carLocation;
        g_VehicleMotionSampleAt = now;

        if (!g_VehicleExtractionCommitted &&
            absoluteSpeed > liveSlamSpeed && haveDirection)
        {
            // A moving car has no stable driver-door route. Discard the
            // stopped-car cache once, rather than projecting a fresh door and
            // detour point on every 2.5-second maintenance pass.
            g_VehicleCachedDriverDoorCar = nullptr;
            g_VehicleCachedDriverDoorSeat = nullptr;
            g_VehicleHaveCachedDriverDoorPoint = false;
            g_VehicleHaveCachedDriverDetourPoint = false;
            g_VehicleHaveLastDriverMovePoint = false;
            g_VehicleLastDriverMoveAt = 0;
            const float headingDot =
                g_VehicleHeadingStableSince == 0
                    ? 1.0f
                    : (g_VehicleInterceptHeading.X * travelDirection.X +
                       g_VehicleInterceptHeading.Y * travelDirection.Y);
            if (g_VehicleHeadingStableSince == 0 || headingDot < 0.90f)
            {
                g_VehicleInterceptHeading = travelDirection;
                g_VehicleHeadingStableSince = now;
            }

            if (g_VehicleInterceptMorphUsed && headingDot < 0.25f)
            {
                Logger::Debug(
                    "18L-AI vehicle intercept: car reversed/turned; retaining priority and re-arming next charged Morph");
                g_VehicleInterceptMorphUsed = false;
                g_VehicleInterceptDeadline = now + 12000;
                g_VehicleInterceptHeading = travelDirection;
                g_VehicleHeadingStableSince = now;
            }

            if (!g_VehicleInterceptMorphUsed &&
                now >= g_VehicleHeadingStableSince + 2000 &&
                TeleportJasonAheadOfVehicle(
                    car,
                    travelDirection,
                    horizontalSpeed,
                    now))
            {
                g_VehicleInterceptMorphUsed = true;
                g_VehicleInterceptDeadline = now + 20000;
                return true;
            }

            const float carToJasonX = jasonLocation.X - carLocation.X;
            const float carToJasonY = jasonLocation.Y - carLocation.Y;
            const float aheadDot = horizontalDistance > 1.0f
                ? ((carToJasonX / horizontalDistance) * travelDirection.X +
                   (carToJasonY / horizontalDistance) * travelDirection.Y)
                : 0.0f;
            if (g_VehicleInterceptMorphUsed &&
                JasonAIMorphRemainingMs(now) == 0 &&
                aheadDot < -0.15f)
            {
                g_VehicleInterceptMorphUsed = false;
                g_VehicleHeadingStableSince = now;
                g_VehicleInterceptDeadline = now + 12000;
                Logger::Debug(
                    "18L-AI vehicle intercept: car passed/extended lead; re-arming charged road interception");
            }
            FVector hoodPoint{};
            if (ResolveOccupiedCarFrontHoodPoint(
                    car,
                    hood,
                    travelDirection,
                    hoodPoint,
                    false,
                    forwardSpeed))
            {
                const float hoodDX = hoodPoint.X - jasonLocation.X;
                const float hoodDY = hoodPoint.Y - jasonLocation.Y;
                const float hoodDistance = std::sqrt(
                    hoodDX * hoodDX + hoodDY * hoodDY);
                if (hoodDistance > 240.0f)
                {
                    // Aim approximately one refresh interval ahead along the
                    // travel axis while preserving the hood centerline.
                    // The distance gate above still uses the real hood point,
                    // so interaction cannot fire prematurely.
                    FVector pursuitPoint = hoodPoint;
                    const float pursuitLead = (std::min)(
                        750.0f,
                        (std::max)(0.0f, horizontalSpeed * 0.85f));
                    pursuitPoint.X += travelDirection.X * pursuitLead;
                    pursuitPoint.Y += travelDirection.Y * pursuitLead;
                    const float moveDX = pursuitPoint.X -
                        g_VehicleLastCenterlineMovePoint.X;
                    const float moveDY = pursuitPoint.Y -
                        g_VehicleLastCenterlineMovePoint.Y;
                    // Reissue only after material movement, using the
                    // responsive pre-regression centerline threshold.
                    constexpr float MaterialMoveCm = 1600.0f;
                    const bool targetMovedMaterially =
                        !g_VehicleHaveLastCenterlineMovePoint ||
                        moveDX * moveDX + moveDY * moveDY >=
                            MaterialMoveCm * MaterialMoveCm;
                    const bool safetyRefreshDue =
                        g_VehicleLastCenterlineMoveAt == 0 ||
                        now >= g_VehicleLastCenterlineMoveAt + 5003;
                    if (targetMovedMaterially || safetyRefreshDue)
                    {
                        const bool moveAccepted =
                            IssueAIMoveToLocationOnGameThread(
                                g_JasonAIState.Controller,
                                pursuitPoint,
                                90.0f,
                                "OccupiedCarCenterline",
                                true);
                        if (moveAccepted)
                        {
                            g_VehicleLastCenterlineMovePoint = pursuitPoint;
                            g_VehicleLastCenterlineMoveAt = now;
                            g_VehicleHaveLastCenterlineMovePoint = true;
                        }
                    }
                    return true;
                }
                // The real front overlap now owns the slam. Pressing interact
                // against a fast hood is rejected by stock code.
                StopJasonAIMovementForKnifeOnGameThread();
                FaceJasonAIAtStartupTrapObjectiveOnGameThread(car);
                if (hood &&
                    g_VehicleMovingHoodAttempts < 3 &&
                    (g_VehicleMovingHoodAttemptAt == 0 ||
                     now >= g_VehicleMovingHoodAttemptAt + 900) &&
                    AttemptJasonVehicleComponent(hood))
                {
                    g_VehicleMovingHoodAttemptAt = now;
                    ++g_VehicleMovingHoodAttempts;
                    Logger::Success(
                        "18L-AI vehicle intercept: moving-car hood stop input sent | attempt=" +
                        std::to_string(g_VehicleMovingHoodAttempts) +
                        " | hoodDistanceCm=" + std::to_string(hoodDistance));
                }
            }
            else
            {
                // Preserve the last accepted centerline path when a streamed
                // hood is temporarily unreadable. The actor-centered fallback
                // aims at the car's side and fought this controller live.
                if (now >= g_NextVehicleInterceptLogAt)
                {
                    g_NextVehicleInterceptLogAt = now + 3000;
                    Logger::Debug(
                        "18L-BH vehicle intercept: hood point unavailable; retaining prior centerline path");
                }
            }
            return true;
        }

        if (!g_VehicleExtractionCommitted &&
            !carStarted &&
            absoluteSpeed <= liveDoorSpeed &&
            (!hoodEnabled ||
             (g_VehicleHoodInputSentAt != 0 &&
              (g_VehicleSlamObserved ||
               now >= g_VehicleHoodInputSentAt + 4000))))
        {
            // Once the hood stop succeeds, car restart flags must not send
            // Jason back to pursuit/knife logic before he removes the driver.
            g_VehicleExtractionCommitted = true;
            if (g_VehicleExtractionCommittedAt == 0)
                g_VehicleExtractionCommittedAt = now;
            Logger::Success(
                "18L-BK vehicle intercept: stop confirmed; driver extraction latched");
        }

        const bool hoodSequenceComplete = g_VehicleExtractionCommitted;

        if (hoodSequenceComplete)
        {
            FVector doorPoint{};
            float doorDistance = FLT_MAX;
            bool haveDoorPoint =
                g_VehicleHaveCachedDriverDoorPoint &&
                g_VehicleCachedDriverDoorCar == car &&
                g_VehicleCachedDriverDoorSeat == seat;
            if (haveDoorPoint)
            {
                doorPoint = g_VehicleCachedDriverDoorPoint;
            }
            else
            {
                haveDoorPoint = ResolveDriverDoorNavPoint(
                    car,
                    seat,
                    doorPoint);
                if (haveDoorPoint)
                {
                    g_VehicleCachedDriverDoorPoint = doorPoint;
                    g_VehicleCachedDriverDoorCar = car;
                    g_VehicleCachedDriverDoorSeat = seat;
                    g_VehicleHaveCachedDriverDoorPoint = true;
                    g_VehicleHaveCachedDriverDetourPoint = false;
                    g_VehicleHaveLastDriverMovePoint = false;
                    g_VehicleLastDriverMoveAt = 0;
                }
            }
            if (haveDoorPoint)
            {
                const float ddx = doorPoint.X - jasonLocation.X;
                const float ddy = doorPoint.Y - jasonLocation.Y;
                doorDistance = std::sqrt(ddx * ddx + ddy * ddy);

                if (g_VehicleDriverApproachStartedAt == 0)
                {
                    g_VehicleDriverApproachStartedAt = now;
                    g_VehicleDriverLastProgressAt = now;
                    g_VehicleDriverBestDistance = doorDistance;
                }
                else if (doorDistance + 35.0f <
                         g_VehicleDriverBestDistance)
                {
                    g_VehicleDriverBestDistance = doorDistance;
                    g_VehicleDriverLastProgressAt = now;
                }
            }

            if (!haveDoorPoint)
            {
                StopJasonAIMovementForKnifeOnGameThread();
                if (g_VehicleDriverApproachStartedAt == 0)
                    g_VehicleDriverApproachStartedAt = now;
                if (now >= g_NextVehicleInterceptLogAt)
                {
                    g_NextVehicleInterceptLogAt = now + 2000;
                    Logger::Error(
                        "18L-AI vehicle intercept: driver seat had no reachable side-door nav point; holding instead of pathing into hood");
                }
                if (now >= g_VehicleDriverApproachStartedAt + 4000)
                {
                    Logger::Error(
                        "18L-BK vehicle driver door unavailable; retaining extraction priority and refreshing geometry");
                    g_VehicleCachedDriverDoorCar = nullptr;
                    g_VehicleCachedDriverDoorSeat = nullptr;
                    g_VehicleHaveCachedDriverDoorPoint = false;
                    g_VehicleHaveCachedDriverDetourPoint = false;
                    g_VehicleDriverApproachStartedAt = now;
                    g_VehicleDriverLastProgressAt = now;
                    g_VehicleDriverDetourReached = false;
                    g_VehicleDriverDetourAttempts = 0;
                    ResetStuckSamplingAfterNativeInteraction(now);
                }
                return true;
            }

            // A blocked hood/door navmesh used to hold this routine forever,
            // rebuilding the same MoveTo once per second. Release Jason back
            // to stock combat after a short no-progress window. The occupied
            // car remains his target and this route may reacquire it after the
            // cooldown, giving normal slash/path recovery a chance in between.
            const bool driverApproachStalled =
                doorDistance > 135.0f &&
                g_VehicleDriverLastProgressAt != 0 &&
                now >= g_VehicleDriverLastProgressAt + 2500;
            const bool driverApproachExpired =
                doorDistance > 135.0f &&
                g_VehicleDriverApproachStartedAt != 0 &&
                now >= g_VehicleDriverApproachStartedAt + 6500;
            if (driverApproachStalled || driverApproachExpired)
            {
                StopJasonAIMovementForKnifeOnGameThread();
                if (g_VehicleDriverDetourAttempts == 0)
                {
                    // The nearest end can be blocked by the hood, a fence, or
                    // the stopped car's collision. Retry immediately around
                    // the opposite end instead of running into the same point
                    // for the rest of the extraction window.
                    g_VehicleDriverDetourAttempts = 1;
                    g_VehicleDriverDetourReached = false;
                    g_VehicleDriverApproachStartedAt = now;
                    g_VehicleDriverLastProgressAt = now;
                    g_VehicleDriverBestDistance = doorDistance;
                    g_VehicleHaveCachedDriverDetourPoint = false;
                    g_VehicleHaveLastDriverMovePoint = false;
                    g_VehicleLastDriverMoveAt = 0;
                    Logger::Error(
                        "18L-BG vehicle driver-door approach stalled; switching to opposite-end detour");
                    ResetStuckSamplingAfterNativeInteraction(now);
                    return true;
                }
                Logger::Error(
                    "18L-BK vehicle driver-door approach stalled; cycling detour while extraction remains latched | distanceCm=" +
                    std::to_string(doorDistance));
                g_VehicleDriverDetourAttempts = 0;
                g_VehicleDriverDetourReached = false;
                g_VehicleDriverApproachStartedAt = now;
                g_VehicleDriverLastProgressAt = now;
                g_VehicleDriverBestDistance = doorDistance;
                g_VehicleHaveCachedDriverDoorPoint = false;
                g_VehicleHaveCachedDriverDetourPoint = false;
                g_VehicleHaveLastDriverMovePoint = false;
                g_VehicleLastDriverMoveAt = 0;
                ResetStuckSamplingAfterNativeInteraction(now);
                return true;
            }

            if (!g_VehicleDriverDetourReached)
            {
                const float sideX = doorPoint.X - carLocation.X;
                const float sideY = doorPoint.Y - carLocation.Y;
                const float sideLength = std::sqrt(
                    sideX * sideX + sideY * sideY);
                const float driverSideProgress =
                    sideLength > 1.0f
                        ? ((jasonLocation.X - carLocation.X) *
                               (sideX / sideLength) +
                           (jasonLocation.Y - carLocation.Y) *
                               (sideY / sideLength))
                        : 0.0f;

                if (driverSideProgress < 120.0f)
                {
                    FVector detourPoint = g_VehicleCachedDriverDetourPoint;
                    bool haveDetourPoint =
                        g_VehicleHaveCachedDriverDetourPoint;
                    if (!haveDetourPoint)
                    {
                        haveDetourPoint = ResolveDriverSideDetourPoint(
                                 car,
                                 doorPoint,
                                 jasonLocation,
                                 g_VehicleDriverDetourAttempts != 0,
                                 detourPoint);
                        if (haveDetourPoint)
                        {
                            g_VehicleCachedDriverDetourPoint = detourPoint;
                            g_VehicleHaveCachedDriverDetourPoint = true;
                        }
                    }
                    if (haveDetourPoint)
                    {
                        const float detourDX =
                            detourPoint.X - jasonLocation.X;
                        const float detourDY =
                            detourPoint.Y - jasonLocation.Y;
                        const float detourDistance = std::sqrt(
                            detourDX * detourDX + detourDY * detourDY);
                        if (detourDistance > 125.0f)
                        {
                            const float moveDX = detourPoint.X -
                                g_VehicleLastDriverMovePoint.X;
                            const float moveDY = detourPoint.Y -
                                g_VehicleLastDriverMovePoint.Y;
                            const bool moveChanged =
                                !g_VehicleHaveLastDriverMovePoint ||
                                moveDX * moveDX + moveDY * moveDY >=
                                    75.0f * 75.0f;
                            if (moveChanged ||
                                now >= g_VehicleLastDriverMoveAt + 5000)
                            {
                                if (IssueAIMoveToLocationOnGameThread(
                                        g_JasonAIState.Controller,
                                        detourPoint,
                                        85.0f,
                                        "DriverSideDetour"))
                                {
                                    g_VehicleLastDriverMovePoint = detourPoint;
                                    g_VehicleHaveLastDriverMovePoint = true;
                                    g_VehicleLastDriverMoveAt = now;
                                }
                            }
                            if (now >= g_NextVehicleInterceptLogAt)
                            {
                                g_NextVehicleInterceptLogAt = now + 2000;
                                Logger::Debug(
                                    "18L-AR vehicle intercept: routing around car body before driver-door approach");
                            }
                            return true;
                        }
                    }
                }

                g_VehicleDriverDetourReached = true;
                g_VehicleHaveLastDriverMovePoint = false;
                g_VehicleLastDriverMoveAt = 0;
                Logger::Success(
                    "18L-AR vehicle intercept: driver-side detour complete; closing on extraction point");
            }

            if (doorDistance > 90.0f)
            {
                g_VehicleDriverReadySince = 0;
                const float moveDX = doorPoint.X -
                    g_VehicleLastDriverMovePoint.X;
                const float moveDY = doorPoint.Y -
                    g_VehicleLastDriverMovePoint.Y;
                const bool moveChanged =
                    !g_VehicleHaveLastDriverMovePoint ||
                    moveDX * moveDX + moveDY * moveDY >= 75.0f * 75.0f;
                if (moveChanged || now >= g_VehicleLastDriverMoveAt + 5000)
                {
                    if (IssueAIMoveToLocationOnGameThread(
                            g_JasonAIState.Controller,
                            doorPoint,
                            45.0f,
                            "DriverDoorSide"))
                    {
                        g_VehicleLastDriverMovePoint = doorPoint;
                        g_VehicleHaveLastDriverMovePoint = true;
                        g_VehicleLastDriverMoveAt = now;
                    }
                }
                return true;
            }

            StopJasonAIMovementForKnifeOnGameThread();
            FaceJasonAIAtStartupTrapObjectiveOnGameThread(occupant);
            if (g_VehicleDriverReadySince == 0)
            {
                // Do not dispatch the native extraction on the same frame
                // path following reports arrival. Let movement settle and the
                // driver-side overlap publish before pressing interact.
                g_VehicleDriverReadySince = now;
                return true;
            }
            // The hood slam, seat overlap and counselor weapon state settle on
            // separate latent frames. Dispatching the raw vehicle interaction
            // after only 650 ms reproduced a stock null dereference one second
            // later. Require both the stop and door-side overlap to remain
            // stable before asking stock code to remove the driver.
            if (now < g_VehicleDriverReadySince + 1800 ||
                g_VehicleExtractionCommittedAt == 0 ||
                now < g_VehicleExtractionCommittedAt + 2500)
                return true;

            AActor* confirmedOccupant = nullptr;
            UObject** liveExtractionComponent = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(jason) + 0x1558);
            const bool stableDriverState =
                ReadLiveVehicleSeat(seat, car, confirmedOccupant) &&
                confirmedOccupant == occupant &&
                Memory::IsReadable(beingSlammed, 1) &&
                *beingSlammed == 0 &&
                std::fabs(SafeVehicleForwardSpeed(car)) <= liveDoorSpeed &&
                Memory::IsReadable(
                    liveExtractionComponent,
                    sizeof(UObject*)) &&
                *liveExtractionComponent == nullptr;
            if (!stableDriverState)
            {
                g_VehicleDriverReadySince = 0;
                if (now >= g_NextVehicleInterceptLogAt)
                {
                    g_NextVehicleInterceptLogAt = now + 2000;
                    Logger::Debug(
                        "18L-BL vehicle extraction deferred: stock car/seat state still settling");
                }
                return true;
            }

            UObject* extractionInteractComponent =
                ResolveJasonCarExtractionInteractComponent(car, hood);
            if (!extractionInteractComponent)
            {
                if (now >= g_NextVehicleInterceptLogAt)
                {
                    g_NextVehicleInterceptLogAt = now + 2000;
                    Logger::Error(
                        "18L-BM vehicle extraction deferred: verified car extraction component unavailable");
                }
                return true;
            }

            UObject* lockedVehicleInteraction =
                GetLockedJasonInteractable(jason);
            if (lockedVehicleInteraction &&
                lockedVehicleInteraction != seat &&
                lockedVehicleInteraction != extractionInteractComponent)
            {
                const bool released = CancelJasonInteractionLockOnly(
                    GetJasonInteractionManager(jason));
                g_NextVehicleInterceptActionAt = now + 150;
                Logger::Debug(
                    std::string("18L-AI vehicle intercept: cleared stale pre-extraction interaction lock | released=") +
                    (released ? "true" : "false"));
                return true;
            }

            if (g_VehicleExtractionInputAt != 0 &&
                g_VehicleExtractionAttempts >= 3 &&
                now >= g_VehicleExtractionInputAt + 5000)
            {
                // A missed overlap must not permanently exhaust the stop-car
                // action. Clear only the stale lock and start another bounded
                // set while keeping this driver as Jason's sole priority.
                CancelJasonInteractionLockOnly(
                    GetJasonInteractionManager(jason));
                g_VehicleExtractionAttempts = 0;
                g_VehicleExtractionInputAt = 0;
            }
            const bool extractionReadyNow =
                g_VehicleExtractionInputAt == 0 ||
                (now >= g_VehicleExtractionInputAt + 3000 &&
                 g_VehicleExtractionAttempts < 3);

            // The September 11 route dispatched the seat wrapper, which
            // actually removed drivers. Preserve the newer 2.5-second stop
            // and live-seat checks above; the generic car component sent a
            // valid-looking input but never acquired a driver-extraction lock.
            UObject* manager = GetJasonInteractionManager(jason);
            UObject* selectedVehicleInteraction = nullptr;
            if (manager)
            {
                UObject** selectedPtr = reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(manager) + 0x210);
                if (Memory::IsReadable(selectedPtr, sizeof(UObject*)) &&
                    *selectedPtr &&
                    Memory::IsReadable(*selectedPtr, sizeof(UObject)))
                {
                    selectedVehicleInteraction = *selectedPtr;
                }
            }

            if (lockedVehicleInteraction == seat ||
                lockedVehicleInteraction == extractionInteractComponent)
                return true;

            // AttemptInteract is a void input. The unselected seat fallback
            // can play a grab/choke sound while the counselor remains in the
            // authoritative seat. Never claim extraction from that input;
            // reposition until the stock manager selects the seat or the
            // actual Jason car component.
            const bool stockSeatReady =
                selectedVehicleInteraction == seat ||
                selectedVehicleInteraction == extractionInteractComponent;
            if (!stockSeatReady)
            {
                if (now >= g_VehicleDriverReadySince + 2500)
                {
                    g_VehicleDriverReadySince = 0;
                    g_VehicleDriverDetourReached = false;
                    g_VehicleHaveCachedDriverDoorPoint = false;
                    g_VehicleHaveLastDriverMovePoint = false;
                    g_NextVehicleInterceptActionAt = now + 600;
                    if (now >= g_NextVehicleInterceptLogAt)
                    {
                        g_NextVehicleInterceptLogAt = now + 2000;
                        Logger::Debug(
                            "18L-BQ extraction seat not selected; reapproaching driver door without false grab");
                    }
                }
                return true;
            }

            const bool sentToSelectedComponent = extractionReadyNow &&
                (selectedVehicleInteraction == seat
                    ? AttemptJasonVehicleSeatComponent(seat)
                    : AttemptJasonVehicleComponent(
                        extractionInteractComponent));
            if (sentToSelectedComponent)
            {
                ++g_VehicleExtractionAttempts;
                g_VehicleExtractionInputAt = now;
                // Recheck quickly for the native extraction component, then
                // retire this lane instead of waiting a full road-work period.
                g_NextVehicleInterceptActionAt = now + 100;
                Logger::Success(
                    "18L-AI vehicle intercept: driver-side extraction input sent | attempt=" +
                    std::to_string(g_VehicleExtractionAttempts) +
                    " | seatSelected=" +
                    (selectedVehicleInteraction == seat ? "true" : "false") +
                    " | stockSelected=" +
                    (stockSeatReady ? "true" : "false") +
                    " | counselor=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(occupant)));
            }
            return true;
        }

        if (g_VehicleHoodInputSentAt != 0)
        {
            // AttemptInteract is a void input dispatch. Once it has been sent,
            // wait for the stock slam pulse/completion instead of hammering A
            // every 800 ms. This latch survives the full stopped-car episode.
            // A void stock input can be accepted without ever publishing the
            // slam byte.  Never park Jason forever in that ambiguous state:
            // after five seconds, release the latch and let the next staggered
            // pass approach the hood again.
            if (!g_VehicleSlamObserved &&
                now >= g_VehicleHoodInputSentAt + 5000)
            {
                Logger::Error(
                    "18L-BD vehicle hood action timed out; releasing latch and retrying approach");
                g_VehicleHoodInputSentAt = 0;
                g_VehicleMovingHoodAttemptAt = 0;
                g_VehicleMovingHoodAttempts = 0;
                g_VehicleInterceptDeadline = now + 9000;
                g_VehicleHaveLastCenterlineMovePoint = false;
                return true;
            }

            StopJasonAIMovementForKnifeOnGameThread();
            FaceJasonAIAtStartupTrapObjectiveOnGameThread(car);
            if (now >= g_NextVehicleInterceptLogAt)
            {
                g_NextVehicleInterceptLogAt = now + 2000;
                Logger::Debug(
                    "18L-AI vehicle intercept: one hood input latched; waiting for stock slam completion | started=" +
                    std::to_string(carStarted ? 1 : 0) +
                    " | slamObserved=" +
                    std::to_string(g_VehicleSlamObserved ? 1 : 0));
            }
            return true;
        }

        if (horizontalDistance > 425.0f)
        {
            FVector hoodStopPoint{};
            FVector fallbackForward{};
            if (!std::isfinite(travelDirection.X) ||
                !std::isfinite(travelDirection.Y) ||
                (travelDirection.X == 0.0f && travelDirection.Y == 0.0f))
            {
                GetJasonAIActorForwardVectorOnGameThread(car, fallbackForward);
            }
            else
            {
                fallbackForward = travelDirection;
            }
            if (ResolveOccupiedCarFrontHoodPoint(
                    car,
                    hood,
                    fallbackForward,
                    hoodStopPoint))
            {
                IssueAIMoveToLocationOnGameThread(
                    g_JasonAIState.Controller,
                    hoodStopPoint,
                    85.0f,
                    "StoppedCarFront");
            }
            else
            {
                IssueJasonAITrapMoveToActorOnGameThread(
                    car,
                    "StoppedCar");
            }
            return true;
        }

        StopJasonAIMovementForKnifeOnGameThread();
        FaceJasonAIAtStartupTrapObjectiveOnGameThread(car);
        if (absoluteSpeed <= liveSlamSpeed && hood)
        {
            if (AttemptJasonVehicleComponent(hood))
            {
                g_VehicleHoodInputSentAt = now;
                Logger::Success(
                    "18L-AI vehicle intercept: single stock hood-destroy input sent and latched");
            }
            return true;
        }

        if (now >= g_NextVehicleInterceptLogAt)
        {
            g_NextVehicleInterceptLogAt = now + 2000;
            Logger::Debug(
                "18L-AI vehicle intercept: waiting for stock speed/slam transition | speed=" +
                std::to_string(forwardSpeed) +
                " | slamThreshold=" + std::to_string(liveSlamSpeed) +
                " | doorThreshold=" + std::to_string(liveDoorSpeed));
        }
        return true;
    }

    void LogFusePlacementDiagnostic(ULONGLONG now)
    {
        if (g_FuseDiagnosticLogged ||
            g_FuseDiagnosticAt == 0 ||
            now < g_FuseDiagnosticAt)
        {
            return;
        }

        g_FuseDiagnosticLogged = true;
        UWorld* world = g_JasonAIState.World;
        if (!world || !Memory::IsReadable(world, sizeof(UWorld)))
            return;

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return;
        }

        int32_t phoneFuseActors = 0;
        int32_t configuredFuseCount = -1;
        int32_t gasCanActors = 0;
        int32_t carriedGasCanActors = 0;
        int32_t vehicleGasRepairComponents = 0;
        int32_t vehicleGasRequiredEntries = 0;
        for (int32_t levelIndex = 0; levelIndex < levels->Count; ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;

            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data ||
                actors.Count <= 0 ||
                actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t i = 0; i < actors.Count; ++i)
            {
                AActor* actor = actors.Data[i];
                if (!actor || !Memory::IsReadable(actor, sizeof(UObject)))
                    continue;

                if (JasonAIObjectDerivesFromNameContaining(
                        reinterpret_cast<UObject*>(actor),
                        "SCWorldSettings"))
                {
                    int32_t* totalFuseCount = reinterpret_cast<int32_t*>(
                        reinterpret_cast<uintptr_t>(actor) + 0x7C0);
                    if (Memory::IsReadable(totalFuseCount, sizeof(int32_t)))
                        configuredFuseCount = *totalFuseCount;
                }

                const std::string actorName =
                    JasonAISafeName(reinterpret_cast<UObject*>(actor));
                const std::string className = actor->Class
                    ? JasonAISafeName(reinterpret_cast<UObject*>(actor->Class))
                    : std::string();

                // The donor and Resurrected CarGasCan assets are byte-exact,
                // and both vehicle blueprints still require CarGasCan_C.
                // Count the actual live world population before attempting
                // any spawn repair.  A carried repair item remains a world
                // actor; its reflected Owner identifies the counselor/bot.
                if (actorName.find("CarGasCan") != std::string::npos ||
                    className.find("CarGasCan") != std::string::npos)
                {
                    ++gasCanActors;
                    AActor* owner = nullptr;
                    if (actor->Class)
                    {
                        UPropertyLite* ownerProperty =
                            FindPropertyInHierarchyByName(
                                actor->Class,
                                "Owner");
                        if (ownerProperty &&
                            ownerProperty->Offset_Internal > 0 &&
                            ownerProperty->Offset_Internal < 0x10000)
                        {
                            owner = ReadActorField(
                                reinterpret_cast<UObject*>(actor),
                                static_cast<uintptr_t>(
                                    ownerProperty->Offset_Internal));
                        }
                    }
                    if (owner)
                        ++carriedGasCanActors;

                    FVector gasLocation{};
                    GetJasonAIActorLocation(actor, gasLocation);
                    Logger::Success(
                        "18L-AK gas census actor[" +
                        std::to_string(gasCanActors) + "]=" + actorName +
                        " | class=" + className +
                        " | owner=" +
                        (owner
                            ? JasonAISafeName(
                                reinterpret_cast<UObject*>(owner))
                            : std::string("<world>")) +
                        " | location=" +
                        std::to_string(gasLocation.X) + "," +
                        std::to_string(gasLocation.Y) + "," +
                        std::to_string(gasLocation.Z));
                }

                uint8_t vehicleKind = 0;
                int32_t vehicleSeats = 0;
                if (actor->Class &&
                    ClassifyRepairableCar(
                        actor,
                        vehicleKind,
                        vehicleSeats))
                {
                    UPropertyLite* gasTankProperty =
                        FindPropertyInHierarchyByName(
                            actor->Class,
                            "GasTankRepair");
                    UObject* gasTankRepair = nullptr;
                    if (gasTankProperty &&
                        gasTankProperty->Offset_Internal > 0 &&
                        gasTankProperty->Offset_Internal < 0x10000)
                    {
                        UObject** field = reinterpret_cast<UObject**>(
                            reinterpret_cast<uintptr_t>(actor) +
                            gasTankProperty->Offset_Internal);
                        if (Memory::IsReadable(field, sizeof(UObject*)))
                            gasTankRepair = *field;
                    }

                    int32_t requiredPartCount = -1;
                    if (gasTankRepair &&
                        Memory::IsReadable(gasTankRepair, sizeof(UObject)) &&
                        gasTankRepair->Class &&
                        Memory::IsReadable(
                            gasTankRepair->Class,
                            sizeof(UClass)))
                    {
                        ++vehicleGasRepairComponents;
                        UPropertyLite* requiredProperty =
                            FindPropertyInHierarchyByName(
                                gasTankRepair->Class,
                                "RequiredPartClasses");
                        if (requiredProperty &&
                            requiredProperty->Offset_Internal > 0 &&
                            requiredProperty->Offset_Internal < 0x10000)
                        {
                            TArray<uint8_t>* requiredParts =
                                reinterpret_cast<TArray<uint8_t>*>(
                                    reinterpret_cast<uintptr_t>(
                                        gasTankRepair) +
                                    requiredProperty->Offset_Internal);
                            if (Memory::IsReadable(
                                    requiredParts,
                                    sizeof(TArray<uint8_t>)) &&
                                requiredParts->Count >= 0 &&
                                requiredParts->Count <= 32)
                            {
                                requiredPartCount = requiredParts->Count;
                                vehicleGasRequiredEntries +=
                                    requiredPartCount;
                            }
                        }
                    }

                    Logger::Success(
                        "18L-AK gas repair vehicle=" + actorName +
                        " | GasTankRepair=" +
                        (gasTankRepair
                            ? JasonAISafeName(gasTankRepair)
                            : std::string("<missing>")) +
                        " | RequiredPartClasses=" +
                        std::to_string(requiredPartCount));
                }

                if (actorName.find("PhoneBoxFuse") == std::string::npos &&
                    className.find("PhoneBoxFuse") == std::string::npos)
                {
                    continue;
                }

                ++phoneFuseActors;
                FVector location{};
                GetJasonAIActorLocation(actor, location);
                Logger::Success(
                    "18L-DIAG phone fuse actor[" +
                    std::to_string(phoneFuseActors) + "]=" + actorName +
                    " | class=" + className +
                    " | location=" + std::to_string(location.X) + "," +
                    std::to_string(location.Y) + "," +
                    std::to_string(location.Z));
            }
        }

        Logger::Success(
            "18L-DIAG phone fuse summary: WorldSettings.TotalFuseCount=" +
            std::to_string(configuredFuseCount) +
            " | livePhoneBoxFuseActors=" +
            std::to_string(phoneFuseActors));

        Logger::Success(
            "18L-AK gas census summary: liveCarGasCanActors=" +
            std::to_string(gasCanActors) +
            " | counselorOrBotOwned=" +
            std::to_string(carriedGasCanActors) +
            " | vehicleGasRepairComponents=" +
            std::to_string(vehicleGasRepairComponents) +
            " | totalRequiredPartEntries=" +
            std::to_string(vehicleGasRequiredEntries));
    }

    bool ResolveCounselorRouteStartupObjectives(UWorld* world)
    {
        if (!world ||
            !Memory::IsReadable(world, sizeof(UWorld)) ||
            g_JasonAIState.StartupTrapObjectivesReady)
        {
            return g_JasonAIState.StartupTrapObjectivesReady;
        }

        // UWorld::GameState is authoritative for this route.  The previous
        // order still performed the expensive global-object fallback scan on
        // every new match even when this direct pointer was valid.  Only pay
        // for that legacy fallback when the authoritative field is missing.
        AActor* authoritativeGameState = ReadActorField(
            reinterpret_cast<UObject*>(world),
            0xF8);
        AActor* scannedGameState = nullptr;

        if (!authoritativeGameState ||
            !JasonAIObjectDerivesFromNameContaining(
                reinterpret_cast<UObject*>(authoritativeGameState),
                "SCGameState"))
        {
            scannedGameState = FindJasonAIStartupTrapGameStateOnce();
            authoritativeGameState = scannedGameState;
        }

        if (!authoritativeGameState)
            return false;

        g_JasonAIState.StartupTrapGameState = authoritativeGameState;
        g_JasonAIState.StartupTrapDiscoveryScanDone = true;

        AActor* phone = ReadActorField(
            reinterpret_cast<UObject*>(authoritativeGameState),
            0x590);
        AActor* car2 = ReadActorField(
            reinterpret_cast<UObject*>(authoritativeGameState),
            0x578);
        AActor* car4 = ReadActorField(
            reinterpret_cast<UObject*>(authoritativeGameState),
            0x570);
        std::string car2Source = car2 ? "GameStateCache" : "missing";
        std::string car4Source = car4 ? "GameStateCache" : "missing";

        uint8_t cachedKind = 0;
        int32_t cachedSeats = 0;
        if (car2 &&
            (!ClassifyRepairableCar(car2, cachedKind, cachedSeats) ||
             cachedKind != 2))
        {
            car2 = nullptr;
            car2Source = "invalid-cache";
        }
        if (car4 &&
            (!ClassifyRepairableCar(car4, cachedKind, cachedSeats) ||
             cachedKind != 3))
        {
            car4 = nullptr;
            car4Source = "invalid-cache";
        }

        // Prefer the GameMode's stock SpawnedVehicles array, then reproduce
        // the game's bounded SCDriveableVehicle/type/seat scan if needed.
        AActor* gameMode = ReadActorField(
            reinterpret_cast<UObject*>(world),
            0xF0);
        if ((!car2 || !car4) && gameMode)
        {
            TArray<AActor*>* spawnedVehicles =
                reinterpret_cast<TArray<AActor*>*>(
                    reinterpret_cast<uintptr_t>(gameMode) + 0x720);
            ScanVehicleArray(
                spawnedVehicles,
                "GameMode.SpawnedVehicles",
                car2,
                car4,
                car2Source,
                car4Source);
        }

        if (!car2 || !car4)
        {
            constexpr uintptr_t Offset_Levels = 0x110;
            TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
                reinterpret_cast<uintptr_t>(world) + Offset_Levels);

            if (Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) &&
                levels->Data &&
                levels->Count > 0 &&
                levels->Count <= 1024 &&
                Memory::IsReadable(
                    levels->Data,
                    sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
            {
                for (int32_t levelIndex = 0;
                    levelIndex < levels->Count && (!car2 || !car4);
                    ++levelIndex)
                {
                    ULevel* level = levels->Data[levelIndex];
                    if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                        continue;

                    ScanVehicleArray(
                        &level->Actors,
                        "World.LevelActors",
                        car2,
                        car4,
                        car2Source,
                        car4Source);
                }
            }
        }

        // The widened stock HUD widgets call SCGameState's real objective
        // getters. OfflineBots did not populate its inherited car caches in
        // the successful counselor run, even though both authoritative cars
        // were present in GameMode.SpawnedVehicles. Publish only the two
        // validated car actors into those inherited cache slots so the stock
        // repair/start/escape checkmarks query the same objects as Jason.
        bool car2CachePublished = false;
        bool car4CachePublished = false;
        if (car2)
        {
            AActor** car2Cache = reinterpret_cast<AActor**>(
                reinterpret_cast<uintptr_t>(authoritativeGameState) + 0x578);
            if (Memory::IsReadable(car2Cache, sizeof(AActor*)))
            {
                *car2Cache = car2;
                car2CachePublished = *car2Cache == car2;
            }
        }
        if (car4)
        {
            AActor** car4Cache = reinterpret_cast<AActor**>(
                reinterpret_cast<uintptr_t>(authoritativeGameState) + 0x570);
            if (Memory::IsReadable(car4Cache, sizeof(AActor*)))
            {
                *car4Cache = car4;
                car4CachePublished = *car4Cache == car4;
            }
        }

        g_JasonAIState.StartupTrapInteriorPhone = phone;
        g_JasonAIState.StartupTrapObjectiveCount = 0;
        g_JasonAIState.StartupTrapObjectiveIndex = 0;

        // Resolve the shack entrance once during the existing startup-objective
        // census. Jason_Shack's entrance is an embedded scene component, not a
        // separate BP_CabinDoor actor. Reading that component's world transform
        // gives an exact cross-map doorway anchor without any recurring scan.
        AActor* shack = FindNearestWorldActorByClass(
            "Jason_Shack_C",
            nullptr,
            0.0f);
        UObject* shackDoorComponent = nullptr;
        FVector shackLocation{};
        FVector shackDoorLocation{};
        FVector shackExteriorDirection{};
        bool shackDoorResolved = false;
        if (shack && GetJasonAIActorLocation(shack, shackLocation))
        {
            shackDoorComponent = ReadReflectedObjectProperty(
                reinterpret_cast<UObject*>(shack),
                "Shack_Jason_door_01");
            if (shackDoorComponent &&
                Memory::IsReadable(
                    shackDoorComponent,
                    Offsets::Scene_ComponentToWorld +
                        Offsets::FTransform_Translation +
                        sizeof(FVector)))
            {
                FVector* componentWorldLocation = reinterpret_cast<FVector*>(
                    reinterpret_cast<uintptr_t>(shackDoorComponent) +
                    Offsets::Scene_ComponentToWorld +
                    Offsets::FTransform_Translation);
                if (Memory::IsReadable(
                        componentWorldLocation,
                        sizeof(FVector)))
                {
                    shackDoorLocation = *componentWorldLocation;
                    const float dx = shackDoorLocation.X - shackLocation.X;
                    const float dy = shackDoorLocation.Y - shackLocation.Y;
                    const float horizontalLength = std::sqrt(dx * dx + dy * dy);
                    if (std::isfinite(horizontalLength) &&
                        horizontalLength >= 25.0f)
                    {
                        // The mesh component named Shack_Jason_door_01 is a
                        // stable rotation reference, but the physical test
                        // proved it is not the walkable exterior threshold.
                        // AJ's dropped-axe marker measured the real threshold
                        // as this blueprint-local offset from that component.
                        // Express it in the component/shack basis so the same
                        // calibration rotates with every map's shack.
                        FVector componentRadial{};
                        componentRadial.X = dx / horizontalLength;
                        componentRadial.Y = dy / horizontalLength;
                        FVector componentLateral{};
                        componentLateral.X = -componentRadial.Y;
                        componentLateral.Y = componentRadial.X;

                        constexpr float MarkerRadialOffset = -240.812576f;
                        constexpr float MarkerLateralOffset = -776.665588f;
                        constexpr float MarkerHeightOffset = 0.878663f;
                        shackDoorLocation.X +=
                            componentRadial.X * MarkerRadialOffset +
                            componentLateral.X * MarkerLateralOffset;
                        shackDoorLocation.Y +=
                            componentRadial.Y * MarkerRadialOffset +
                            componentLateral.Y * MarkerLateralOffset;
                        shackDoorLocation.Z += MarkerHeightOffset;

                        const float exteriorDX =
                            shackDoorLocation.X - shackLocation.X;
                        const float exteriorDY =
                            shackDoorLocation.Y - shackLocation.Y;
                        const float exteriorLength = std::sqrt(
                            exteriorDX * exteriorDX +
                            exteriorDY * exteriorDY);
                        if (std::isfinite(exteriorLength) &&
                            exteriorLength >= 25.0f)
                        {
                            shackExteriorDirection.X =
                                exteriorDX / exteriorLength;
                            shackExteriorDirection.Y =
                                exteriorDY / exteriorLength;
                            shackExteriorDirection.Z = 0.0f;
                            shackDoorResolved = true;
                        }
                    }
                }
            }
        }
        g_JasonAIState.StartupTrapShackDoorLocation = shackDoorLocation;
        g_JasonAIState.StartupTrapShackExteriorDirection =
            shackExteriorDirection;
        g_JasonAIState.StartupTrapShackDoorResolved = shackDoorResolved;

        auto addObjective = [](AActor* actor, uint8_t kind)
        {
            if (!actor || g_JasonAIState.StartupTrapObjectiveCount >= 4)
                return;

            const int32_t index = g_JasonAIState.StartupTrapObjectiveCount++;
            g_JasonAIState.StartupTrapObjectives[index] = actor;
            g_JasonAIState.StartupTrapObjectiveKinds[index] = kind;
        };

        // The stable shack actor owns the component and remains the objective;
        // kind 4 uses the separately cached doorway transform for placement.
        if (shackDoorResolved)
            addObjective(shack, 4);
        addObjective(phone, 1);
        addObjective(car2, 2);
        addObjective(car4, 3);
        ++g_JasonAIState.StartupTrapDiscoveryReads;
        g_JasonAIState.StartupTrapObjectivesReady = true;

        Logger::Success(
            "18L-AG counselor bridge objectives resolved: shackEntrance=" +
            (shackDoorResolved
                ? JasonAISafeName(shackDoorComponent)
                : "NULL") +
            " | shackDoor=" +
            std::to_string(shackDoorLocation.X) + "," +
            std::to_string(shackDoorLocation.Y) + "," +
            std::to_string(shackDoorLocation.Z) +
            " | shackExterior=" +
            std::to_string(shackExteriorDirection.X) + "," +
            std::to_string(shackExteriorDirection.Y) +
            " | phone=" +
            (phone ? JasonAISafeName(reinterpret_cast<UObject*>(phone)) : "NULL") +
            " | car2=" +
            (car2 ? JasonAISafeName(reinterpret_cast<UObject*>(car2)) : "NULL") +
            "(" + car2Source + ") | car4=" +
            (car4 ? JasonAISafeName(reinterpret_cast<UObject*>(car4)) : "NULL") +
            "(" + car4Source + ") | authoritativeGS=" +
            JasonAISafeName(reinterpret_cast<UObject*>(authoritativeGameState)) +
            " | scannedSame=" +
            (authoritativeGameState == scannedGameState ? "true" : "false") +
            " | carCaches=" +
            (car2CachePublished ? "2" : "-") + "/" +
            (car4CachePublished ? "4" : "-") +
            " | count=" +
            std::to_string(g_JasonAIState.StartupTrapObjectiveCount));
        return true;
    }

    bool IsCounselorRouteSoundBlipEmitter(UObject* killer)
    {
        return killer &&
            g_JasonAIState.Active &&
            g_JasonAIState.World &&
            g_JasonAIState.World == Engine::GetWorld() &&
            (killer == reinterpret_cast<UObject*>(g_JasonAIState.Jason) ||
             JasonAIObjectDerivesFromNameContaining(killer, "Jason_"));
    }

    __declspec(noinline) int32_t CallContextKillEligibilityWithTemporaryHunter(
        ContextKillCanInteractFn original,
        UObject* contextKillComponent,
        AActor* interactor,
        const FVector* viewLocation,
        const FVector* viewDirection,
        uint8_t* hunterFlag)
    {
        // Keep structured exception handling in this POD-only helper so the
        // temporary stock eligibility byte is restored even if native code
        // exits abnormally. Do not add C++ objects requiring unwinding here.
        const uint8_t savedHunterFlag = *hunterFlag;
        int32_t result = 0;
        __try
        {
            *hunterFlag = 1;
            result = original(
                contextKillComponent,
                interactor,
                viewLocation,
                viewDirection);
        }
        __finally
        {
            *hunterFlag = savedHunterFlag;
        }
        return result;
    }

    int32_t __fastcall ContextKillCanInteractHook(
        UObject* contextKillComponent,
        AActor* interactor,
        const FVector* viewLocation,
        const FVector* viewDirection)
    {
        if (!g_OriginalContextKillCanInteract)
            return 0;

        // SCContextKillComponent::CanInteractWith has one stock Hunter byte
        // gate at counselor +0x1A18 after its ordinary disabled, weapon and
        // context checks. Relax only that one predicate, only for the exact
        // validated JasonDeath component in the active counselor route. The
        // original function still enforces every other stock prerequisite.
        const bool universalFinalContext =
            g_JasonAIState.Active &&
            contextKillComponent &&
            contextKillComponent == g_LastAcceptedFinalKillComponent &&
            g_LastAcceptedFinalContext &&
            interactor &&
            Memory::IsReadable(interactor, 0x1A19) &&
            ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(interactor),
                "SCCounselorCharacter");
        if (!universalFinalContext)
        {
            return g_OriginalContextKillCanInteract(
                contextKillComponent,
                interactor,
                viewLocation,
                viewDirection);
        }

        uint8_t* hunterFlag = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(interactor) + 0x1A18);
        const int32_t result = CallContextKillEligibilityWithTemporaryHunter(
            g_OriginalContextKillCanInteract,
            contextKillComponent,
            interactor,
            viewLocation,
            viewDirection,
            hunterFlag);

        if (result != 0)
        {
            const bool firstAcceptanceForContext =
                g_LastUniversalFinalEligibilityContext !=
                    g_LastAcceptedFinalContext;
            // Preserve the first pawn that passes every stock predicate for
            // this context. Nearby bots may also be probed afterward, but
            // they must not steal the pending paired interaction.
            if (firstAcceptanceForContext ||
                !g_LastUniversalFinalEligibilityFinisher)
            {
                g_LastUniversalFinalEligibilityFinisher = interactor;
                g_LastUniversalFinalEligibilityContext =
                    g_LastAcceptedFinalContext;
                // Preserve the first full stock-predicate acceptance. Updating
                // this on every internal probe prevented the bounded
                // post-cinematic completion fallback from ever aging in.
                g_LastUniversalFinalEligibilityAcceptedAt = GetTickCount64();
            }
            if (firstAcceptanceForContext)
            {
                Logger::Success(
                    "18L-BD universal final-kill eligibility accepted by stock context | finisher=" +
                    JasonAISafeName(reinterpret_cast<UObject*>(interactor)));
            }
        }
        return result;
    }

    bool InstallUniversalFinalKillEligibilityHook()
    {
        if (g_ContextKillCanInteractHookInstalled)
            return true;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        constexpr uintptr_t RVA_ContextKillCanInteract = 0x003457B0;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) +
            RVA_ContextKillCanInteract);
        const uint8_t expected[] = {
            0x40, 0x55, 0x56, 0x48, 0x83, 0xEC, 0x28, 0x80,
            0xB9, 0xE9, 0x03, 0x00, 0x00, 0x00, 0x48, 0x8B,
            0xF2, 0x48, 0x8B, 0xE9, 0x0F, 0x85, 0xA5, 0x01
        };
        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            Logger::Error(
                "18L-BD universal final-kill eligibility signature mismatch; hook not installed");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK &&
            initStatus != MH_ERROR_ALREADY_INITIALIZED)
        {
            return false;
        }

        const MH_STATUS createStatus = MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(&ContextKillCanInteractHook),
            reinterpret_cast<LPVOID*>(
                &g_OriginalContextKillCanInteract));
        if (createStatus != MH_OK)
        {
            Logger::Error(
                "18L-BD failed to create universal final-kill eligibility hook");
            return false;
        }
        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_OriginalContextKillCanInteract = nullptr;
            return false;
        }

        g_ContextKillCanInteractTarget = target;
        g_ContextKillCanInteractHookInstalled = true;
        Logger::Success(
            "18L-BD universal proximity final-kill eligibility hook installed; stock weapon/context gates preserved");
        return true;
    }

    void RemoveUniversalFinalKillEligibilityHook()
    {
        if (!g_ContextKillCanInteractHookInstalled ||
            !g_ContextKillCanInteractTarget)
        {
            return;
        }
        MH_DisableHook(g_ContextKillCanInteractTarget);
        MH_RemoveHook(g_ContextKillCanInteractTarget);
        g_ContextKillCanInteractTarget = nullptr;
        g_OriginalContextKillCanInteract = nullptr;
        g_ContextKillCanInteractHookInstalled = false;
        g_LastUniversalFinalEligibilityFinisher = nullptr;
        g_LastUniversalFinalEligibilityAcceptedAt = 0;
    }

    int32_t __fastcall PamelaSweaterCanInteractHook(
        AActor* sweater,
        AActor* interactor,
        const FVector* viewLocation,
        const FVector* viewDirection)
    {
        if (!g_OriginalPamelaSweaterCanInteract)
            return 0;

        // ASCPamelaSweater::CanInteractWith normally rejects counselors whose
        // native female byte is false before delegating to the ordinary item
        // predicate. In offline counselor mode, skip only that gender test and
        // call the same stock base pickup predicate. Inventory capacity,
        // disabled/busy state, distance and ownership validation stay intact.
        const bool universalSweaterPickup =
            g_JasonAIState.Active &&
            sweater && interactor &&
            Memory::IsReadable(sweater, sizeof(UObject)) &&
            Memory::IsReadable(interactor, sizeof(UObject)) &&
            ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(sweater),
                "SCPamelaSweater") &&
            ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(interactor),
                "SCCounselorCharacter") &&
            g_BasePamelaPickupCanInteract;
        if (universalSweaterPickup)
        {
            return g_BasePamelaPickupCanInteract(
                sweater,
                interactor,
                viewLocation,
                viewDirection);
        }
        return g_OriginalPamelaSweaterCanInteract(
            sweater,
            interactor,
            viewLocation,
            viewDirection);
    }

    bool InstallUniversalPamelaSweaterPickupHook()
    {
        if (g_PamelaSweaterCanInteractHookInstalled)
            return true;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;
        constexpr uintptr_t RVA_PamelaSweaterCanInteract = 0x00382720;
        constexpr uintptr_t RVA_BasePamelaPickupCanInteract = 0x00382470;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) +
            RVA_PamelaSweaterCanInteract);
        const uint8_t expected[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
            0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
            0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xF9, 0x49
        };
        g_BasePamelaPickupCanInteract =
            reinterpret_cast<PamelaSweaterCanInteractFn>(
                reinterpret_cast<uintptr_t>(module) +
                RVA_BasePamelaPickupCanInteract);
        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0 ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(g_BasePamelaPickupCanInteract),
                1))
        {
            g_BasePamelaPickupCanInteract = nullptr;
            Logger::Error(
                "18L-BD universal Pamela sweater pickup signature mismatch; hook not installed");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK &&
            initStatus != MH_ERROR_ALREADY_INITIALIZED)
        {
            return false;
        }
        const MH_STATUS createStatus = MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(&PamelaSweaterCanInteractHook),
            reinterpret_cast<LPVOID*>(
                &g_OriginalPamelaSweaterCanInteract));
        if (createStatus != MH_OK)
        {
            g_BasePamelaPickupCanInteract = nullptr;
            Logger::Error(
                "18L-BD failed to create universal Pamela sweater pickup hook");
            return false;
        }
        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_OriginalPamelaSweaterCanInteract = nullptr;
            g_BasePamelaPickupCanInteract = nullptr;
            return false;
        }

        g_PamelaSweaterCanInteractTarget = target;
        g_PamelaSweaterCanInteractHookInstalled = true;
        Logger::Success(
            "18L-BD universal Pamela sweater pickup hook installed; stock item gates preserved");
        return true;
    }

    void RemoveUniversalPamelaSweaterPickupHook()
    {
        if (!g_PamelaSweaterCanInteractHookInstalled ||
            !g_PamelaSweaterCanInteractTarget)
        {
            return;
        }
        MH_DisableHook(g_PamelaSweaterCanInteractTarget);
        MH_RemoveHook(g_PamelaSweaterCanInteractTarget);
        g_PamelaSweaterCanInteractTarget = nullptr;
        g_OriginalPamelaSweaterCanInteract = nullptr;
        g_BasePamelaPickupCanInteract = nullptr;
        g_PamelaSweaterCanInteractHookInstalled = false;
    }

    LONG CALLBACK NativeInteractionNullExceptionGuard(
        EXCEPTION_POINTERS* exception)
    {
        if (!exception || !exception->ExceptionRecord ||
            !exception->ContextRecord ||
            exception->ExceptionRecord->ExceptionCode !=
                EXCEPTION_ACCESS_VIOLATION ||
            exception->ExceptionRecord->NumberParameters < 2 ||
            exception->ExceptionRecord->ExceptionInformation[0] != 0 ||
            exception->ExceptionRecord->ExceptionInformation[1] != 0 ||
            exception->ContextRecord->Rcx != 0 ||
            exception->ContextRecord->Rdi == 0 ||
            g_NativeInteractionGameBase == 0)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // The minidump's RIP is EXE+0x36E842, inside the native update.
        // Its first two subobjects are checked at function entry, but the
        // first component can disappear during the update itself. Skip only
        // that virtual call, not the rest of the car/grab transition.
        const uintptr_t rip = static_cast<uintptr_t>(
            exception->ContextRecord->Rip);
        if (rip == g_NativeInteractionGameBase + 0x0036E842)
            exception->ContextRecord->Rip =
                g_NativeInteractionGameBase + 0x0036E84B;
        else if (rip == g_NativeInteractionGameBase + 0x0036E852)
            exception->ContextRecord->Rip =
                g_NativeInteractionGameBase + 0x0036E85B;
        else
            return EXCEPTION_CONTINUE_SEARCH;

        InterlockedIncrement(&g_NativeInteractionExceptionGuardHits);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    bool InstallNativeInteractionExceptionGuard()
    {
        if (g_NativeInteractionExceptionGuard)
            return true;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        const uintptr_t base = reinterpret_cast<uintptr_t>(module);
        const uint8_t expected[] = {
            0x48, 0x8B, 0x01, 0xFF, 0x90, 0xA0, 0x02, 0x00, 0x00
        };
        const uint8_t* first = reinterpret_cast<const uint8_t*>(
            base + 0x0036E842);
        const uint8_t* second = reinterpret_cast<const uint8_t*>(
            base + 0x0036E852);
        if (!Memory::IsReadable(first, sizeof(expected)) ||
            !Memory::IsReadable(second, sizeof(expected)) ||
            std::memcmp(first, expected, sizeof(expected)) != 0 ||
            std::memcmp(second, expected, sizeof(expected)) != 0)
        {
            Logger::Error(
                "18L-BN native interaction exception guard signature mismatch; not installed");
            return false;
        }

        g_NativeInteractionGameBase = base;
        g_NativeInteractionExceptionGuard =
            AddVectoredExceptionHandler(
                1, &NativeInteractionNullExceptionGuard);
        if (!g_NativeInteractionExceptionGuard)
        {
            g_NativeInteractionGameBase = 0;
            return false;
        }
        Logger::Success(
            "18L-BN native interaction mid-update null guard installed");
        return true;
    }

    void RemoveNativeInteractionExceptionGuard()
    {
        if (!g_NativeInteractionExceptionGuard)
            return;
        RemoveVectoredExceptionHandler(g_NativeInteractionExceptionGuard);
        g_NativeInteractionExceptionGuard = nullptr;
        g_NativeInteractionGameBase = 0;
        InterlockedExchange(
            &g_NativeInteractionExceptionGuardHits, 0);
    }

    void __fastcall NativeInteractionUpdateHook(UObject* interaction)
    {
        if (!g_OriginalNativeInteractionUpdate || !interaction)
            return;

        // Three crash dumps (including the latest car extraction) stop at
        // EXE+0x36E842: the stock routine dereferences [this+0x2A8] without
        // checking it. Its other two required pointers are checked by stock
        // code at entry. Skip only the incomplete native transition frame;
        // keep the successful hood/seat interaction and normal update path.
        const uintptr_t address = reinterpret_cast<uintptr_t>(interaction);
        if (*reinterpret_cast<UObject**>(address + 0x2A0) &&
            *reinterpret_cast<UObject**>(address + 0x3D8) &&
            !*reinterpret_cast<UObject**>(address + 0x2A8))
        {
            if (!g_NativeInteractionNullGuardLogged)
            {
                g_NativeInteractionNullGuardLogged = true;
                Logger::Error(
                    "18L-BN native interaction update skipped incomplete car/grab transition; null +0x2A8 would crash stock EXE");
            }
            return;
        }

        g_OriginalNativeInteractionUpdate(interaction);
        if (g_NativeInteractionExceptionGuardHits > 0)
        {
            const LONG guarded = InterlockedExchange(
                &g_NativeInteractionExceptionGuardHits, 0);
            if (guarded > 0)
            {
                Logger::Error(
                    "18L-BN native interaction null virtual call bypassed | count=" +
                    std::to_string(guarded));
            }
        }
    }

    bool InstallNativeInteractionNullGuard()
    {
        if (g_NativeInteractionUpdateHookInstalled)
            return true;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        constexpr uintptr_t RVA_NativeInteractionUpdate = 0x0036E300;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) +
            RVA_NativeInteractionUpdate);
        const uint8_t expected[] = {
            0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56,
            0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x70
        };
        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            Logger::Error(
                "18L-BN native interaction crash guard signature mismatch; hook not installed");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK &&
            initStatus != MH_ERROR_ALREADY_INITIALIZED)
            return false;

        if (MH_CreateHook(target,
                reinterpret_cast<LPVOID>(&NativeInteractionUpdateHook),
                reinterpret_cast<LPVOID*>(
                    &g_OriginalNativeInteractionUpdate)) != MH_OK)
            return false;
        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_OriginalNativeInteractionUpdate = nullptr;
            return false;
        }

        g_NativeInteractionUpdateTarget = target;
        g_NativeInteractionUpdateHookInstalled = true;
        Logger::Success(
            "18L-BN native interaction null guard installed for packed EXE");
        return true;
    }

    void RemoveNativeInteractionNullGuard()
    {
        if (!g_NativeInteractionUpdateHookInstalled ||
            !g_NativeInteractionUpdateTarget)
            return;

        MH_DisableHook(g_NativeInteractionUpdateTarget);
        MH_RemoveHook(g_NativeInteractionUpdateTarget);
        g_NativeInteractionUpdateTarget = nullptr;
        g_OriginalNativeInteractionUpdate = nullptr;
        g_NativeInteractionUpdateHookInstalled = false;
        g_NativeInteractionNullGuardLogged = false;
    }

    void __fastcall UpdateSoundBlipsHook(
        UObject* killer,
        float deltaSeconds)
    {
        if (IsCounselorRouteSoundBlipEmitter(killer))
            return;

        if (g_OriginalUpdateSoundBlips)
            g_OriginalUpdateSoundBlips(killer, deltaSeconds);
    }

    bool InstallCounselorRouteSoundBlipSuppression()
    {
        if (g_UpdateSoundBlipsHookInstalled)
            return true;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        constexpr uintptr_t RVA_UpdateSoundBlips = 0x0042DC00;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) + RVA_UpdateSoundBlips);
        const uint8_t expected[] = {
            0x4C, 0x8B, 0xDC, 0x55, 0x49, 0x8D, 0xAB, 0xE8,
            0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x10, 0x02,
            0x00
        };

        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            Logger::Error(
                "18L-AG counselor bridge: UpdateSoundBlips signature mismatch; suppression not installed");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
            return false;

        MH_STATUS createStatus = MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(&UpdateSoundBlipsHook),
            reinterpret_cast<LPVOID*>(&g_OriginalUpdateSoundBlips));
        if (createStatus != MH_OK)
        {
            Logger::Error(
                "18L-AG counselor bridge: failed to create UpdateSoundBlips hook");
            return false;
        }

        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_OriginalUpdateSoundBlips = nullptr;
            return false;
        }

        g_UpdateSoundBlipsTarget = target;
        g_UpdateSoundBlipsHookInstalled = true;
        Logger::Success(
            "18L-AG counselor bridge: AI-Jason sound-blip emitter suppressed route-locally");
        return true;
    }

    void RemoveCounselorRouteSoundBlipSuppression()
    {
        if (!g_UpdateSoundBlipsHookInstalled || !g_UpdateSoundBlipsTarget)
            return;

        MH_DisableHook(g_UpdateSoundBlipsTarget);
        MH_RemoveHook(g_UpdateSoundBlipsTarget);
        g_UpdateSoundBlipsTarget = nullptr;
        g_OriginalUpdateSoundBlips = nullptr;
        g_UpdateSoundBlipsHookInstalled = false;
    }

    bool IsLiveCounselorRouteVictim(AActor* victim)
    {
        if (!victim ||
            !Memory::IsReadable(victim, sizeof(UObject)) ||
            !IsCounselorOrHero(victim))
        {
            return false;
        }

        uint8_t* dead = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(victim) + 0x1031);
        UObject** controller = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(victim) + 0x3A0);
        UWorld* world = Engine::GetWorld();

        // This is called from the trap overlap callback itself. The callback,
        // valid controller and current-world equality already prove liveness;
        // a complete world actor scan here only adds an event-time hitch.
        return Memory::IsReadable(dead, 1) &&
            *dead == 0 &&
            !HasCounselorEscaped(victim) &&
            Memory::IsReadable(controller, sizeof(UObject*)) &&
            *controller &&
            Memory::IsReadable(*controller, sizeof(UObject)) &&
            world &&
            world == g_JasonAIState.World;
    }

    void __fastcall TrapTriggeredHook(AActor* trap, AActor* victim)
    {
        if (g_OriginalTrapTriggered)
            g_OriginalTrapTriggered(trap, victim);

        if (!g_JasonAIState.Active ||
            !g_JasonAIState.Jason ||
            !trap ||
            !victim ||
            !Memory::IsReadable(trap, sizeof(UObject)) ||
            !JasonAIObjectDerivesFromNameContaining(
                reinterpret_cast<UObject*>(trap),
                "SCTrap") ||
            !IsLiveCounselorRouteVictim(victim))
        {
            return;
        }

        uint8_t* localRole = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(trap) + 0x110);
        AActor* trapArmer = ReadActorField(
            reinterpret_cast<UObject*>(trap),
            0x670);
        AActor* triggeredTrap = ReadActorField(
            reinterpret_cast<UObject*>(victim),
            0x10F8);

        if (!Memory::IsReadable(localRole, 1) ||
            *localRole != 3 ||
            trapArmer != g_JasonAIState.Jason ||
            triggeredTrap != trap)
        {
            return;
        }

        // The overlap callback can run inside trap damage/stun processing.
        // Queue only; the controller Tick performs movement/Morph work safely.
        g_QueuedTrapMorphBaseline.store(
            g_JasonAIState.LastMorphTeleportAt,
            std::memory_order_release);
        g_QueuedTriggeredTrap.store(trap, std::memory_order_release);
        g_QueuedTrapVictim.store(victim, std::memory_order_release);
        Logger::Success(
            "18L-AG counselor bridge: Jason trap triggered; victim queued=" +
            JasonAISafeName(reinterpret_cast<UObject*>(victim)) +
            " | trap=" +
            JasonAISafeName(reinterpret_cast<UObject*>(trap)));
    }

    bool InstallCounselorRouteTrapTriggerHook()
    {
        if (g_TrapTriggeredHookInstalled)
            return true;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;

        constexpr uintptr_t RVA_TrapTriggered = 0x003AC360;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) + RVA_TrapTriggered);
        const uint8_t expected[] = {
            0x48, 0x85, 0xD2, 0x0F, 0x84, 0xEB, 0x02, 0x00,
            0x00, 0x48, 0x8B, 0xC4, 0x55, 0x56, 0x41, 0x56,
            0x48, 0x8D, 0x68, 0xA1
        };

        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            Logger::Error(
                "18L-AG counselor bridge: trap-trigger signature mismatch; priority hook not installed");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
            return false;

        MH_STATUS createStatus = MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(&TrapTriggeredHook),
            reinterpret_cast<LPVOID*>(&g_OriginalTrapTriggered));
        if (createStatus != MH_OK)
        {
            Logger::Error(
                "18L-AG counselor bridge: failed to create trap-trigger priority hook");
            return false;
        }

        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_OriginalTrapTriggered = nullptr;
            return false;
        }

        g_TrapTriggeredTarget = target;
        g_TrapTriggeredHookInstalled = true;
        Logger::Success(
            "18L-AG counselor bridge: owned-trap trigger priority hook installed");
        return true;
    }

    void RemoveCounselorRouteTrapTriggerHook()
    {
        if (!g_TrapTriggeredHookInstalled || !g_TrapTriggeredTarget)
            return;

        MH_DisableHook(g_TrapTriggeredTarget);
        MH_RemoveHook(g_TrapTriggeredTarget);
        g_TrapTriggeredTarget = nullptr;
        g_OriginalTrapTriggered = nullptr;
        g_TrapTriggeredHookInstalled = false;
    }

    void __fastcall CounselorRouteGiveStartingItemHook(
        AActor* pawn,
        UClass* requestedClass)
    {
        static thread_local int32_t callDepth = 0;
        UClass* effectiveClass = requestedClass;

        if (callDepth == 0 &&
            g_HunterAxeLoadoutRouteEnabled &&
            g_JasonAIState.Active &&
            pawn && requestedClass && g_HunterSpawnAxeClass &&
            Memory::IsReadable(pawn, sizeof(UObject)) &&
            Memory::IsReadable(requestedClass, sizeof(UClass)) &&
            Memory::IsReadable(g_HunterSpawnAxeClass, sizeof(UClass)) &&
            pawn != g_LocalCounselorTarget &&
            pawn != g_JasonAIState.Jason &&
            ObjectClassDerivesFromExact(
                reinterpret_cast<UObject*>(pawn),
                "Hunter_Counselor_C") &&
            JasonAISafeName(reinterpret_cast<UObject*>(requestedClass)) ==
                "Shotgun_C")
        {
            UWorld* world = Engine::GetWorld();
            UObject** gameModeField = world
                ? reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(world) + 0xF0)
                : nullptr;
            UObject* gameMode = gameModeField &&
                Memory::IsReadable(gameModeField, sizeof(UObject*))
                ? *gameModeField
                : nullptr;
            UObject** pendingHunterControllerField = gameMode &&
                Memory::IsReadable(gameMode, 0x908)
                ? reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(gameMode) + 0x900)
                : nullptr;
            UObject* pendingHunterController =
                pendingHunterControllerField &&
                Memory::IsReadable(
                    pendingHunterControllerField,
                    sizeof(UObject*))
                ? *pendingHunterControllerField
                : nullptr;

            if (world == g_JasonAIState.World &&
                gameMode &&
                JasonAIObjectDerivesFromNameContaining(
                    gameMode,
                    "SCGameMode") &&
                pendingHunterController &&
                pendingHunterController != g_LocalPlayerController &&
                JasonAIObjectDerivesFromNameContaining(
                    pendingHunterController,
                    "AIController"))
            {
                effectiveClass = g_HunterSpawnAxeClass;
                Logger::Success(
                    "18L-AT AI Tommy native loadout: stock shotgun replaced with permanent-route axe");
            }
        }

        if (!g_OriginalGiveStartingItem)
            return;

        ++callDepth;
        g_OriginalGiveStartingItem(pawn, effectiveClass);
        --callDepth;

        if (effectiveClass == g_HunterSpawnAxeClass && pawn)
        {
            UObject* weapon = GetCounselorCurrentWeapon(pawn);
            if (weapon &&
                ObjectClassDerivesFromExact(
                    weapon,
                    "CounselorTwoHandedAxe_C"))
            {
                g_HunterSpawnAxeItem = reinterpret_cast<AActor*>(weapon);
            }
        }
    }

    bool InstallCounselorRouteHunterAxeLoadoutHook()
    {
        if (g_GiveStartingItemHookInstalled)
        {
            g_HunterAxeLoadoutRouteEnabled = true;
            return true;
        }

        AActor* worldAxe = FindNearestWorldActorByClass(
            "CounselorTwoHandedAxe_C",
            nullptr,
            0.0f);
        UClass* axeClass = worldAxe &&
            Memory::IsReadable(worldAxe, sizeof(UObject))
            ? worldAxe->Class
            : nullptr;
        if (!axeClass ||
            JasonAISafeName(reinterpret_cast<UObject*>(axeClass)) !=
                "CounselorTwoHandedAxe_C")
        {
            Logger::Error(
                "18L-AT AI Tommy native loadout: axe class unavailable; stock shotgun retained");
            return false;
        }

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return false;
        constexpr uintptr_t RVA_GiveStartingItem = 0x002EF980;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) + RVA_GiveStartingItem);
        const uint8_t expected[] = {
            0x48, 0x85, 0xD2, 0x0F, 0x84, 0xDF, 0x00, 0x00,
            0x00, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48
        };
        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            Logger::Error(
                "18L-AT AI Tommy native loadout: GiveStartingItem signature mismatch; stock shotgun retained");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
            return false;
        MH_STATUS createStatus = MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(&CounselorRouteGiveStartingItemHook),
            reinterpret_cast<LPVOID*>(&g_OriginalGiveStartingItem));
        if (createStatus != MH_OK)
            return false;
        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_OriginalGiveStartingItem = nullptr;
            return false;
        }

        g_HunterSpawnAxeClass = axeClass;
        g_GiveStartingItemTarget = target;
        g_GiveStartingItemHookInstalled = true;
        g_HunterAxeLoadoutRouteEnabled = true;
        Logger::Success(
            "18L-AT AI Tommy native loadout hook installed: AI-return shotgun will become axe");
        return true;
    }

    void RemoveCounselorRouteHunterAxeLoadoutHook()
    {
        g_HunterAxeLoadoutRouteEnabled = false;
        if (g_GiveStartingItemHookInstalled && g_GiveStartingItemTarget)
        {
            MH_DisableHook(g_GiveStartingItemTarget);
            MH_RemoveHook(g_GiveStartingItemTarget);
        }
        g_GiveStartingItemTarget = nullptr;
        g_OriginalGiveStartingItem = nullptr;
        g_GiveStartingItemHookInstalled = false;
        g_HunterSpawnAxeClass = nullptr;
        g_HunterSpawnAxeItem = nullptr;
    }

    void DisableExistingCounselorRouteSoundBlips(AActor* jason)
    {
        HMODULE module = GetModuleHandle(nullptr);
        if (!module || !jason)
            return;

        constexpr uintptr_t RVA_SetSoundBlipVisibility = 0x00423BD0;
        uint8_t* target = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(module) + RVA_SetSoundBlipVisibility);
        const uint8_t expected[] = {
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x81,
            0xEC, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05,
            0xEC, 0xF9, 0xB6, 0x02
        };

        if (!Memory::IsReadable(target, sizeof(expected)) ||
            std::memcmp(target, expected, sizeof(expected)) != 0)
        {
            Logger::Error(
                "18L-AG counselor bridge: SetSoundBlipVisibility signature mismatch");
            return;
        }

        reinterpret_cast<SetSoundBlipVisibilityFn>(target)(
            reinterpret_cast<UObject*>(jason),
            false);
        Logger::Success(
            "18L-AG counselor bridge: existing AI-Jason sound blips cleared");
    }

    UObject* GetJasonInteractionManager(AActor* jason)
    {
        if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
            return nullptr;

        UObject** manager = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(jason) + 0xE18);
        if (!Memory::IsReadable(manager, sizeof(UObject*)) ||
            !*manager ||
            !Memory::IsReadable(*manager, sizeof(UObject)))
        {
            return nullptr;
        }

        return *manager;
    }

    UObject* GetLockedJasonInteractable(AActor* jason)
    {
        UObject* manager = GetJasonInteractionManager(jason);
        if (!manager)
            return nullptr;

        UObject** locked = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(manager) + 0x230);
        if (!Memory::IsReadable(locked, sizeof(UObject*)) ||
            !*locked ||
            !Memory::IsReadable(*locked, sizeof(UObject)))
        {
            return nullptr;
        }

        return *locked;
    }

    int32_t ReadJasonKnifeCount(AActor* jason)
    {
        if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
            return -1;

        int32_t* count = reinterpret_cast<int32_t*>(
            reinterpret_cast<uintptr_t>(jason) + 0x15EC);
        if (!Memory::IsReadable(count, sizeof(int32_t)) ||
            *count < 0 ||
            *count > 256)
        {
            return -1;
        }

        return *count;
    }

    bool IsJasonInNativeSpecialMove(AActor* jason)
    {
        if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
            return false;

        uintptr_t vtable = *reinterpret_cast<uintptr_t*>(jason);
        uintptr_t* slot = reinterpret_cast<uintptr_t*>(vtable + 0x8F8);
        if (!vtable ||
            !Memory::IsReadable(slot, sizeof(uintptr_t)) ||
            !*slot ||
            !Memory::IsReadable(reinterpret_cast<void*>(*slot), 1))
        {
            return false;
        }

        using IsInSpecialMoveFn = bool(__fastcall*)(AActor*);
        return reinterpret_cast<IsInSpecialMoveFn>(*slot)(jason);
    }

    bool IsJasonBridgeCombatBusy(ULONGLONG now)
    {
        AActor* jason = g_JasonAIState.Jason;
        if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
            return true;

        bool busy =
            g_JasonAIState.KnifeSequenceActive ||
            g_JasonAIState.AttackPressed ||
            g_JasonAIState.GrabKillReadyAt != 0 ||
            g_JasonAIState.DoorBreakActive ||
            g_JasonAIState.StartupTrapSetupActive ||
            g_JasonAIState.StartupTrapAttemptSent ||
            g_JasonAIState.StartupTrapCountConsumed ||
            g_JasonAIState.StartupTrapPhoneApproachActive ||
            GetLockedJasonInteractable(jason) != nullptr ||
            IsJasonInNativeSpecialMove(jason);

        uint8_t* stateE98 = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(jason) + 0xE98);
        uint8_t* stateEE8 = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(jason) + 0xEE8);
        uint8_t* morphActive = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(jason) + 0x1419);
        uint8_t* shiftActive = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(jason) + 0x14E0);

        if ((Memory::IsReadable(stateE98, 1) && *stateE98 != 0) ||
            (Memory::IsReadable(stateEE8, 1) && *stateEE8 != 0) ||
            (Memory::IsReadable(morphActive, 1) && *morphActive != 0) ||
            (Memory::IsReadable(shiftActive, 1) && *shiftActive != 0))
        {
            busy = true;
        }

        HMODULE module = GetModuleHandle(nullptr);
        if (module)
        {
            using IsStunnedFn = bool(__fastcall*)(AActor*);
            using GetGrabbedCounselorFn = AActor*(__fastcall*)(AActor*);
            IsStunnedFn isStunned = reinterpret_cast<IsStunnedFn>(
                reinterpret_cast<uintptr_t>(module) + 0x002F00E0);
            GetGrabbedCounselorFn getGrabbed =
                reinterpret_cast<GetGrabbedCounselorFn>(
                    reinterpret_cast<uintptr_t>(module) + 0x004055E0);

            if ((Memory::IsReadable(reinterpret_cast<void*>(isStunned), 1) &&
                 isStunned(jason)) ||
                (Memory::IsReadable(reinterpret_cast<void*>(getGrabbed), 1) &&
                 getGrabbed(jason) != nullptr))
            {
                busy = true;
            }
        }

        AActor* nearest = FindNearestJasonAICounselorTarget(jason);
        FVector jasonLocation{};
        FVector counselorLocation{};
        if (nearest &&
            GetJasonAIActorLocation(jason, jasonLocation) &&
            GetJasonAIActorLocation(nearest, counselorLocation))
        {
            const float dx = counselorLocation.X - jasonLocation.X;
            const float dy = counselorLocation.Y - jasonLocation.Y;
            const float dz = counselorLocation.Z - jasonLocation.Z;
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(distanceSquared) &&
                distanceSquared <= 300.0f * 300.0f)
            {
                busy = true;
            }
        }

        if (busy)
            g_CombatBusyUntil = now + 1000;

        return busy || now < g_CombatBusyUntil;
    }

    bool IsUsablePriorityVictim(AActor* victim)
    {
        if (!victim ||
            !Memory::IsReadable(victim, sizeof(UObject)) ||
            !IsCounselorOrHero(victim))
        {
            return false;
        }

        uint8_t* dead = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(victim) + 0x1031);
        UObject** controller = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(victim) + 0x3A0);
        return Memory::IsReadable(dead, 1) &&
            *dead == 0 &&
            Memory::IsReadable(controller, sizeof(UObject*)) &&
            *controller &&
            Memory::IsReadable(*controller, sizeof(UObject));
    }

    bool TeleportJasonToPriorityVictim(AActor* victim)
    {
        if (!IsUsablePriorityVictim(victim))
            return false;

        AActor* savedTargets[JasonAITargetCapacity]{};
        ULONGLONG savedBlockedUntil[JasonAITargetCapacity]{};
        uint8_t savedBlockStrikes[JasonAITargetCapacity]{};
        std::memcpy(savedTargets, g_JasonAITargets, sizeof(savedTargets));
        std::memcpy(
            savedBlockedUntil,
            g_JasonAITargetBlockedUntil,
            sizeof(savedBlockedUntil));
        std::memcpy(
            savedBlockStrikes,
            g_JasonAITargetBlockStrikes,
            sizeof(savedBlockStrikes));
        const int32_t savedTargetCount = g_JasonAITargetCount;

        g_JasonAITargets[0] = victim;
        g_JasonAITargetBlockedUntil[0] = 0;
        g_JasonAITargetBlockStrikes[0] = 0;
        g_JasonAITargetCount = 1;

        const ULONGLONG previousMorphAt = g_JasonAIState.LastMorphTeleportAt;
        bool teleported = false;
        // A triggered owned trap is the one counselor-targeted Morph that is
        // always urgent, even inside the ordinary 10m walk radius.  Scope the
        // exemption across both branches because Finish... clears the startup
        // flag before issuing its counselor-ring teleport.
        ++g_TrapTeleportExemptionDepth;
        if (g_JasonAIState.StartupTrapSetupActive)
        {
            FinishJasonAIStartupTrapSetupOnGameThread(
                "trap-triggered-priority");
            teleported =
                g_JasonAIState.LastMorphTeleportAt != previousMorphAt;
        }
        else
        {
            teleported = RunOfflineBotsInitialTeleportOnGameThread();
            if (teleported)
                MarkJasonAIMorphTeleportUsed("TrapTriggered");
        }
        --g_TrapTeleportExemptionDepth;

        std::memcpy(g_JasonAITargets, savedTargets, sizeof(savedTargets));
        std::memcpy(
            g_JasonAITargetBlockedUntil,
            savedBlockedUntil,
            sizeof(savedBlockedUntil));
        std::memcpy(
            g_JasonAITargetBlockStrikes,
            savedBlockStrikes,
            sizeof(savedBlockStrikes));
        g_JasonAITargetCount = savedTargetCount;

        if (teleported)
            g_JasonAIState.Target = victim;
        return teleported;
    }

    void ProcessQueuedTrapPriority(ULONGLONG now)
    {
        AActor* queued = g_QueuedTrapVictim.exchange(
            nullptr,
            std::memory_order_acq_rel);
        if (queued)
        {
            const ULONGLONG queuedMorphBaseline =
                g_QueuedTrapMorphBaseline.exchange(
                    0,
                    std::memory_order_acq_rel);
            g_QueuedTriggeredTrap.store(nullptr, std::memory_order_release);
            if (IsUsablePriorityVictim(queued))
            {
                g_TrapPriorityVictim = queued;
                g_TrapPriorityUntil = now + 60000;
                g_TrapPriorityBaselineMorphAt = queuedMorphBaseline;
                g_TrapPriorityMorphCompleted = false;
                g_NextTrapPriorityAttemptAt = now;
                g_NextTrapPriorityLogAt = 0;
            }
        }

        if (!g_TrapPriorityVictim)
            return;

        if (now >= g_TrapPriorityUntil ||
            !IsUsablePriorityVictim(g_TrapPriorityVictim))
        {
            Logger::Debug(
                "18L-AG counselor bridge: trap-trigger victim priority ended");
            g_TrapPriorityVictim = nullptr;
            g_TrapPriorityUntil = 0;
            g_TrapPriorityBaselineMorphAt = 0;
            g_TrapPriorityMorphCompleted = false;
            return;
        }

        // The frozen distance-Morph path runs later in this same controller
        // Tick with its target registry restricted to the trapped counselor.
        // Count that shared-cooldown Morph as the emergency response so the
        // bridge does not fire a redundant second Morph twenty seconds later.
        if (!g_TrapPriorityMorphCompleted &&
            g_JasonAIState.LastMorphTeleportAt != 0 &&
            g_JasonAIState.LastMorphTeleportAt !=
                g_TrapPriorityBaselineMorphAt)
        {
            g_TrapPriorityBaselineMorphAt =
                g_JasonAIState.LastMorphTeleportAt;
            g_TrapPriorityMorphCompleted = true;
            Logger::Success(
                "18L-AG counselor bridge: frozen Morph satisfied trap-trigger priority | counselor=" +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(g_TrapPriorityVictim)));
        }

        if (g_TrapPriorityMorphCompleted)
            return;

        if (now < g_NextTrapPriorityAttemptAt ||
            IsJasonBridgeCombatBusy(now))
        {
            return;
        }

        const ULONGLONG morphRemaining = JasonAIMorphRemainingMs(now);
        if (morphRemaining > 0)
        {
            if (now >= g_NextTrapPriorityLogAt)
            {
                g_NextTrapPriorityLogAt = now + 2000;
                Logger::Debug(
                    "18L-AG counselor bridge: trap-trigger Morph waiting for shared cooldown | remainingMs=" +
                    std::to_string(morphRemaining));
            }
            return;
        }

        StopJasonAIMovementForKnifeOnGameThread();
        if (TeleportJasonToPriorityVictim(g_TrapPriorityVictim))
        {
            Logger::Success(
                "18L-AG counselor bridge: emergency Morph completed to trapped counselor=" +
                JasonAISafeName(
                    reinterpret_cast<UObject*>(g_TrapPriorityVictim)) +
                " | pursuitPriorityMs=60000");
            g_TrapPriorityMorphCompleted = true;
            g_NextTrapPriorityAttemptAt = now + 3000;
        }
        else
        {
            Logger::Debug(
                "18L-AG counselor bridge: emergency Morph had no valid exterior point; retrying");
            g_NextTrapPriorityAttemptAt = now + 3000;
        }
    }

    void ShortenConfirmedTrapPlacementHold(ULONGLONG now)
    {
        if (!g_JasonAIState.StartupTrapSetupActive ||
            !g_JasonAIState.StartupTrapCountConsumed ||
            !g_JasonAIState.StartupTrapActorConfirmed ||
            g_JasonAIState.StartupTrapPlacedAt == 0)
        {
            return;
        }

        const int32_t objectiveIndex =
            g_JasonAIState.StartupTrapObjectiveIndex;
        if (objectiveIndex == g_LastShortenedPlacementObjective)
            return;

        // The frozen implementation deliberately emulates a human's five
        // second placement pause.  Counselor-mode Jason only needs enough
        // time for the stock interaction to settle after the trap actor is
        // visible; then the frozen state machine can advance and issue its
        // walking MoveTo toward the next phone/car objective.
        constexpr ULONGLONG CounselorPlacementSettleMs = 1000;
        constexpr ULONGLONG FrozenPlacementHoldMs = 5000;
        if (now <
            g_JasonAIState.StartupTrapPlacedAt +
                CounselorPlacementSettleMs)
        {
            return;
        }

        const ULONGLONG observedHoldMs =
            now - g_JasonAIState.StartupTrapPlacedAt;
        g_JasonAIState.StartupTrapPlacedAt =
            now - FrozenPlacementHoldMs;
        g_LastShortenedPlacementObjective = objectiveIndex;
        Logger::Success(
            "18L-AG counselor bridge: confirmed trap released into next-objective walk | objectiveIndex=" +
            std::to_string(objectiveIndex) +
            " | observedHoldMs=" + std::to_string(observedHoldMs));
    }

    void UpdatePendingKnifePickup(ULONGLONG now)
    {
        if (!g_PendingKnifePickup)
            return;

        const int32_t currentCount = ReadJasonKnifeCount(g_JasonAIState.Jason);
        uint8_t enabled = 1;
        if (g_PendingKnifeComponent &&
            Memory::IsReadable(
                reinterpret_cast<uint8_t*>(g_PendingKnifeComponent) + 0x2A0,
                1))
        {
            enabled = *(reinterpret_cast<uint8_t*>(
                g_PendingKnifeComponent) + 0x2A0);
        }

        const bool inventoryIncreased =
            currentCount >= 0 &&
            g_KnifeCountBeforePickup >= 0 &&
            currentCount > g_KnifeCountBeforePickup;
        const bool managerReleased =
            GetLockedJasonInteractable(g_JasonAIState.Jason) !=
                g_PendingKnifeComponent;

        if (managerReleased && (inventoryIncreased || enabled == 0))
        {
            Logger::Success(
                "18L-AG counselor bridge: stock throwing-knife pickup completed | count=" +
                std::to_string(g_KnifeCountBeforePickup) + "->" +
                std::to_string(currentCount));
            g_PendingKnifePickup = nullptr;
            g_PendingKnifeComponent = nullptr;
            g_KnifeCountBeforePickup = -1;
            g_KnifePickupStartedAt = 0;
        }
        else if (managerReleased && now >= g_KnifePickupStartedAt + 500)
        {
            Logger::Debug(
                "18L-AG counselor bridge: stock throwing-knife pickup aborted");
            g_PendingKnifePickup = nullptr;
            g_PendingKnifeComponent = nullptr;
            g_KnifeCountBeforePickup = -1;
            g_KnifePickupStartedAt = 0;
        }
        else if (now >= g_KnifePickupStartedAt + 8000)
        {
            Logger::Debug(
                "18L-AG counselor bridge: stock throwing-knife pickup timed out");
            g_PendingKnifePickup = nullptr;
            g_PendingKnifeComponent = nullptr;
            g_KnifeCountBeforePickup = -1;
            g_KnifePickupStartedAt = 0;
        }
    }

    void RefreshKnifePickupRegistryIncremental(UWorld* world, ULONGLONG now)
    {
        if (!world ||
            !Memory::IsReadable(world, sizeof(UWorld)) ||
            g_WorldInteractionRegistryComplete ||
            now < g_NextKnifeRegistryRefreshAt)
        {
            return;
        }
        g_NextKnifeRegistryRefreshAt = now + 211;

        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + 0x110);
        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return;
        }

        if (g_KnifeRegistryRefreshLevel >= levels->Count)
        {
            g_WorldInteractionRegistryComplete = true;
            g_KnifeRegistryRefreshActor = 0;
            Logger::Success(
                "18L-AS bounded interaction registry complete | knives=" +
                std::to_string(g_KnifePickupRegistryCount) +
                " | hidingSpots=" +
                std::to_string(g_HidingSpotRegistryCount));
            return;
        }
        ULevel* level = levels->Data[g_KnifeRegistryRefreshLevel];
        if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
        {
            ++g_KnifeRegistryRefreshLevel;
            g_KnifeRegistryRefreshActor = 0;
            return;
        }

        TArray<AActor*>& actors = level->Actors;
        if (!actors.Data ||
            actors.Count <= 0 ||
            actors.Count > 100000 ||
            !Memory::IsReadable(
                actors.Data,
                sizeof(AActor*) * static_cast<size_t>(actors.Count)))
        {
            ++g_KnifeRegistryRefreshLevel;
            g_KnifeRegistryRefreshActor = 0;
            return;
        }

        if (g_KnifeRegistryRefreshActor >= actors.Count)
        {
            ++g_KnifeRegistryRefreshLevel;
            g_KnifeRegistryRefreshActor = 0;
            return;
        }

        // Never ancestry-walk a whole streamed level on one presentation
        // frame. Eight actor slots is a fixed, sub-frame unit of work;
        // the cursor resumes on later ticks until all levels are covered.
        const int32_t actorEnd = (std::min)(
            actors.Count,
            g_KnifeRegistryRefreshActor + 8);
        for (int32_t i = g_KnifeRegistryRefreshActor;
            i < actorEnd;
            ++i)
        {
            AActor* actor = actors.Data[i];
            if (!actor || !Memory::IsReadable(actor, sizeof(UObject)))
                continue;

            if (g_KnifePickupRegistryCount < 128 &&
                JasonAIObjectDerivesFromNameContaining(
                    reinterpret_cast<UObject*>(actor),
                    "SCThrowingKnifePickup"))
            {
                bool duplicate = false;
                for (int32_t k = 0; k < g_KnifePickupRegistryCount; ++k)
                {
                    if (g_KnifePickupRegistry[k] == actor)
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    g_KnifePickupRegistry[g_KnifePickupRegistryCount++] = actor;
            }

            if (g_HidingSpotRegistryCount < 256 &&
                ObjectClassDerivesFromExact(
                    reinterpret_cast<UObject*>(actor),
                    "SCHidingSpot"))
            {
                g_HidingSpotRegistry[g_HidingSpotRegistryCount++] = actor;
            }
        }
        g_KnifeRegistryRefreshActor = actorEnd;
        if (g_KnifeRegistryRefreshActor >= actors.Count)
        {
            ++g_KnifeRegistryRefreshLevel;
            g_KnifeRegistryRefreshActor = 0;
        }
    }

    void TryPickupNearbyThrowingKnife(ULONGLONG now)
    {
        UpdatePendingKnifePickup(now);
        const bool startupInteractionBusy =
            g_JasonAIState.StartupTrapSetupActive &&
            !g_JasonAIState.StartupTrapTransitActive;
        if (g_PendingKnifePickup ||
            startupInteractionBusy ||
            g_TrapPriorityVictim ||
            IsJasonBridgeCombatBusy(now))
        {
            return;
        }

        // Do not stack two actor-array maintenance jobs on the same startup
        // frames. Loot cleanup finishes its bounded slices first; only then
        // does the knife/hiding registry consume background slices.
        if (g_LootCleanupComplete)
            RefreshKnifePickupRegistryIncremental(g_JasonAIState.World, now);

        if (now < g_NextKnifePickupScanAt)
            return;

        g_NextKnifePickupScanAt = now + 1700;
        AActor* jason = g_JasonAIState.Jason;
        UWorld* world = g_JasonAIState.World;
        UObject* manager = GetJasonInteractionManager(jason);
        if (!jason || !world || !manager || GetLockedJasonInteractable(jason))
            return;

        FVector jasonLocation{};
        if (!GetJasonAIActorLocation(jason, jasonLocation))
            return;

        HMODULE module = GetModuleHandle(nullptr);
        if (!module)
            return;

        constexpr uintptr_t RVA_CanInteractWithKnife = 0x00382920;
        constexpr uintptr_t RVA_AttemptInteract = 0x0025AC50;
        AActor* bestPickup = nullptr;
        UObject* bestComponent = nullptr;
        float bestDistanceSquared = FLT_MAX;

        for (int32_t i = 0; i < g_KnifePickupRegistryCount; ++i)
        {
            AActor* pickup = g_KnifePickupRegistry[i];
            if (!pickup ||
                !Memory::IsReadable(pickup, sizeof(UObject)) ||
                !JasonAIObjectDerivesFromNameContaining(
                    reinterpret_cast<UObject*>(pickup),
                    "SCThrowingKnifePickup"))
            {
                continue;
            }

            UObject** componentField = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(pickup) + 0x4F8);
            if (!Memory::IsReadable(componentField, sizeof(UObject*)) ||
                !*componentField ||
                !Memory::IsReadable(*componentField, sizeof(UObject)))
            {
                continue;
            }

            UObject* component = *componentField;
            uint8_t* enabled = reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(component) + 0x2A0);
            float* distanceLimit = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(component) + 0x304);
            if (!Memory::IsReadable(enabled, 1) ||
                *enabled == 0 ||
                !Memory::IsReadable(distanceLimit, sizeof(float)))
            {
                continue;
            }

            FVector pickupLocation{};
            if (!GetJasonAIActorLocation(pickup, pickupLocation))
                continue;

            const float dx = pickupLocation.X - jasonLocation.X;
            const float dy = pickupLocation.Y - jasonLocation.Y;
            const float dz = pickupLocation.Z - jasonLocation.Z;
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            float allowedDistance =
                std::isfinite(*distanceLimit) && *distanceLimit > 0.0f
                ? (std::min)(200.0f, *distanceLimit)
                : 200.0f;

            if (!std::isfinite(distanceSquared) ||
                distanceSquared > allowedDistance * allowedDistance ||
                distanceSquared >= bestDistanceSquared)
            {
                continue;
            }

            uintptr_t pickupVTable = *reinterpret_cast<uintptr_t*>(pickup);
            uintptr_t* canInteractSlot = reinterpret_cast<uintptr_t*>(
                pickupVTable + 0x648);
            const uintptr_t expectedCanInteract =
                reinterpret_cast<uintptr_t>(module) +
                RVA_CanInteractWithKnife;
            if (!pickupVTable ||
                !Memory::IsReadable(canInteractSlot, sizeof(uintptr_t)) ||
                *canInteractSlot != expectedCanInteract)
            {
                continue;
            }

            using CanInteractWithFn = int32_t(__fastcall*)(
                AActor*, AActor*, const FVector&, const FRotator&);
            const float horizontal = std::sqrt(dx * dx + dy * dy);
            constexpr float RadiansToDegrees = 57.29577951308232f;
            FRotator viewRotation(
                std::atan2(dz, horizontal) * RadiansToDegrees,
                std::atan2(dy, dx) * RadiansToDegrees,
                0.0f);
            int32_t canInteract =
                reinterpret_cast<CanInteractWithFn>(*canInteractSlot)(
                    pickup,
                    jason,
                    jasonLocation,
                    viewRotation);
            if (canInteract == 0)
                continue;

            bestPickup = pickup;
            bestComponent = component;
            bestDistanceSquared = distanceSquared;
        }

        if (!bestPickup || !bestComponent)
            return;

        uintptr_t managerVTable = *reinterpret_cast<uintptr_t*>(manager);
        uintptr_t* attemptSlot = reinterpret_cast<uintptr_t*>(
            managerVTable + 0x430);
        const uintptr_t expectedAttempt =
            reinterpret_cast<uintptr_t>(module) + RVA_AttemptInteract;
        if (!managerVTable ||
            !Memory::IsReadable(attemptSlot, sizeof(uintptr_t)) ||
            *attemptSlot != expectedAttempt)
        {
            return;
        }

        const int32_t countBefore = ReadJasonKnifeCount(jason);
        reinterpret_cast<AttemptInteractFn>(*attemptSlot)(
            manager,
            bestComponent,
            1,
            true);

        UObject* locked = GetLockedJasonInteractable(jason);
        if (locked == bestComponent)
        {
            g_PendingKnifePickup = bestPickup;
            g_PendingKnifeComponent = bestComponent;
            g_KnifeCountBeforePickup = countBefore;
            g_KnifePickupStartedAt = now;
            g_NextKnifePickupScanAt = now + 2500;
            StopJasonAIMovementForKnifeOnGameThread();
            Logger::Success(
                "18L-AG counselor bridge: stock throwing-knife pickup accepted | actor=" +
                JasonAISafeName(reinterpret_cast<UObject*>(bestPickup)) +
                " | countBefore=" + std::to_string(countBefore));
        }
    }

    bool RunBaseAIControllerTick(
        UObject* controller,
        float deltaSeconds)
    {
        HMODULE module = GetModuleHandle(nullptr);
        if (!module || !controller)
            return false;

        constexpr uintptr_t RVA_BaseAIControllerTick = 0x011360F0;
        using BaseTickFn = void(__fastcall*)(UObject*, float);
        BaseTickFn baseTick = reinterpret_cast<BaseTickFn>(
            reinterpret_cast<uintptr_t>(module) + RVA_BaseAIControllerTick);
        if (!Memory::IsReadable(reinterpret_cast<void*>(baseTick), 1))
            return false;

        baseTick(controller, deltaSeconds);
        return true;
    }

    void PrepareNonAggroStartupTick()
    {
        if (!g_JasonAIState.StartupTrapSetupActive ||
            !g_JasonAIState.Jason)
        {
            return;
        }

        UWorld* world = Engine::GetWorld();
        const ULONGLONG now = GetTickCount64();
        // The full fuse/gas/vehicle actor census is diagnostic-only and used
        // to run synchronously during the first minute of every match.  Keep
        // the diagnostic implementation available for targeted debugging,
        // but do not burden normal gameplay with a whole-world scan.

        if (!g_JasonAIState.StartupTrapObjectivesReady &&
            now >= g_JasonAIState.StartupTrapNextActionAt)
        {
            ResolveCounselorRouteStartupObjectives(world);
        }

        const ULONGLONG readyAt = JasonAIMorphReadyAt();
        const bool betweenObjectives =
            !g_JasonAIState.StartupTrapTeleported &&
            !g_JasonAIState.StartupTrapAttemptSent &&
            !g_JasonAIState.StartupTrapCountConsumed &&
            !g_JasonAIState.StartupTrapPhoneApproachActive;
        const bool allObjectivesVisited =
            g_JasonAIState.StartupTrapObjectivesReady &&
            g_JasonAIState.StartupTrapObjectiveIndex >=
                g_JasonAIState.StartupTrapObjectiveCount;
        const bool outOfTraps =
            g_JasonAIState.StartupTrapObjectivesReady &&
            ReadJasonAITrapCount(g_JasonAIState.Jason) == 0;
        const bool finalHuntWait =
            betweenObjectives &&
            (allObjectivesVisited || outOfTraps);

        // Preserve the frozen donor-style behavior between real trap targets:
        // walk toward the next phone/car, fight only inside its existing combat
        // windows, and Morph there when the 20-second charge completes.  Hold
        // still only after objective setup is finished, so Jason cannot use the
        // remaining recharge time to begin hunting counselors prematurely.
        if (finalHuntWait && readyAt != 0 && now < readyAt)
        {
            if (g_LastNonAggroReadyAt != readyAt)
                StopJasonAIMovementForKnifeOnGameThread();
            g_JasonAIState.StartupTrapTransitActive = false;
            g_JasonAIState.StartupTrapTransitGoal = nullptr;
            g_JasonAIState.StartupTrapTransitNextMoveAt = 0;
            if (g_JasonAIState.StartupTrapNextActionAt < readyAt)
                g_JasonAIState.StartupTrapNextActionAt = readyAt;

            if (g_LastNonAggroReadyAt != readyAt)
            {
                g_LastNonAggroReadyAt = readyAt;
                Logger::Success(
                    "18L-AG counselor bridge: Jason held non-aggro before final counselor hunt | remainingMs=" +
                    std::to_string(readyAt - now));
            }
        }

        if (!finalHuntWait ||
            readyAt == 0 ||
            now < readyAt)
        {
            return;
        }

        if (allObjectivesVisited || outOfTraps)
        {
            StopJasonAIMovementForKnifeOnGameThread();
            FinishJasonAIStartupTrapSetupOnGameThread(
                allObjectivesVisited
                    ? "all-objectives-visited-bridge-nonaggro"
                    : "out-of-traps-bridge-nonaggro");
        }
    }

    void __fastcall CounselorRouteControllerTickHook(
        UObject* controller,
        float deltaSeconds)
    {
        const bool counselorRouteTick =
            controller == g_JasonAIState.Controller &&
            g_JasonAIState.Active;

        if (!counselorRouteTick)
        {
            OfflineBotsKillerControllerTickHook(controller, deltaSeconds);
            return;
        }

        // This is a game-thread key-edge check only; it performs no scans.
        // Native controller cycling remains primary, while F4 guarantees a
        // deterministic Jason POV when this shipping build bypasses the
        // Blueprint CanSpectate event.
        const ULONGLONG now = GetTickCount64();
        // One cheap clock read around each startup lane identifies whether a
        // reported hitch belongs to the frozen trap route, loot cleanup, or
        // counselor bridge. Log only a slow lane, at most once per 2 seconds.
        static ULONGLONG nextStartupCostLogAt = 0;
        auto logStartupCost = [&](const char* lane, ULONGLONG startedAt)
        {
            if (!g_JasonAIState.StartupTrapSetupActive)
                return;
            const ULONGLONG finishedAt = GetTickCount64();
            const ULONGLONG elapsed = finishedAt - startedAt;
            if (elapsed < 16 || finishedAt < nextStartupCostLogAt)
                return;
            nextStartupCostLogAt = finishedAt + 2000;
            Logger::Debug(std::string("18L-BP startup lane cost | lane=") +
                lane + " | ms=" + std::to_string(elapsed) +
                " | trapObjective=" +
                std::to_string(g_JasonAIState.StartupTrapObjectiveIndex));
        };
        // The stock game keeps its killer controller ticking while it tears
        // down pawns for WaitingPostMatchOutro/PostMatchOutro.  The counselor
        // bridge must retire before touching cached Jason animation,
        // interaction, knife, trap, or vehicle pointers.  The previous build
        // continued for 46 seconds after InProgress ended and dereferenced a
        // destroyed AnimInstance in native IsStunned.
        const CounselorRouteMatchPhase matchPhase =
            ReadCounselorRouteMatchPhase();
        if (matchPhase == CounselorRouteMatchPhase::NotInProgress)
        {
            ReleaseJasonSpectatorOverrideForCinematic("postmatch");
            SetJasonHighPriorityPursuitBoost(false, "postmatch");
            g_JasonAIState.Active = false;
            if (!g_PostMatchAIRetired)
            {
                g_PostMatchAIRetired = true;
                Logger::Success(
                    "18L-AJ POSTMATCH SAFETY: counselor-route Jason AI retired before gameplay-object teardown");
            }
            OfflineBotsKillerControllerTickHook(controller, deltaSeconds);
            return;
        }

        // Once an occupied-car route is active, the seat already supplies the
        // authoritative victim.  Do not run the 250 ms possession refresh or
        // the one-second full counselor validation pass during that route;
        // their repeated class/controller checks were the remaining rhythmic
        // driving hitch.  Both services resume immediately when the car route
        // ends, so deaths, escapes and Tommy possession still reconcile.
        if (!g_VehicleInterceptCar)
        {
            const ULONGLONG refreshStartedAt = GetTickCount64();
            RefreshLocalCounselorTarget(now);
            PruneAndRefreshCounselorTargets(g_JasonAIState.World, now);
            logStartupCost("counselor-refresh", refreshStartedAt);
        }
        MaintainLocalShotgunAimReticleState(now);

        const ULONGLONG killTeamStartedAt = GetTickCount64();
        DriveCounselorKillTeamAI(now);
        logStartupCost("kill-team", killTeamStartedAt);
        // Pamela's activation, accepted slash, kneel retry and final context
        // are one exclusive lane. The prior packed build checked only the
        // final-context timer, allowing chase, grab, combat and stuck recovery
        // to overwrite every successful kneel request before JasonDeath_C
        // could publish.
        if (IsPamelaExclusiveKillLaneActive(now))
        {
            SetJasonHighPriorityPursuitBoost(false, "final-kill");
            if (g_NativeJasonDeathCinematicObservedAt == 0)
                StopJasonAIMovementForKnifeOnGameThread();
            RunBaseAIControllerTick(controller, deltaSeconds);
            return;
        }

        // Spectator camera repair must start only after the authoritative
        // match-phase check above. On the final counselor death, the previous
        // ordering issued one more ClientSetSpectatorCamera call against a
        // tearing-down Jason/root component, moving the spectator pawn tens of
        // millions of units outside the map and flooding Unreal's quadtree.
        ObserveStockSpectatorCycleOnGameThread(now);
        PollJasonSpectatorHotkeyOnGameThread();
        MaintainJasonSpectatorViewOnGameThread(now);
        MaintainJasonDeathVisualNormalization(now);

        // Native grab/pocket-knife state owns both pawns. Detect it before any
        // helper protection, pickup, movement or blackboard work can mutate
        // Tommy during the paired animation.
        if (DriveHeldCounselorGrabKill(now))
        {
            SetJasonHighPriorityPursuitBoost(false, "grab-kill");
            RunBaseAIControllerTick(controller, deltaSeconds);
            return;
        }

        // A moving escape car outranks trap response, knife scavenging and
        // ordinary counselor pursuit. Keep its early return ahead of unrelated
        // loot cleanup, objective publication and flee-state maintenance as
        // well, so those lanes cannot align with the instant a car starts.
        const bool vehicleInterceptActive =
            DriveOccupiedVehicleInterception(now);
        // Keep the boost latched for the complete occupied-car interception.
        // Sampling the native forward speed on every controller frame made
        // the 4x movement/cooldown values oscillate when the car braked or
        // crossed zero while turning.  The interception state already owns
        // the started/occupied validation on its staggered cadence.
        const bool movingEscapeCar =
            vehicleInterceptActive && g_VehicleInterceptCar;
        const bool policePursuit =
            !vehicleInterceptActive && HasPoliceArrived();
        const bool highPriorityPursuit =
            movingEscapeCar || policePursuit;
        SetJasonHighPriorityPursuitBoost(
            highPriorityPursuit,
            movingEscapeCar ? "occupied-escape-car" : "police-arrived",
            movingEscapeCar ? 4.0f : (policePursuit ? 3.0f : 2.0f),
            movingEscapeCar ? 4000ULL :
                (policePursuit ? 5000ULL : 10000ULL));
        if (vehicleInterceptActive)
        {
            RunBaseAIControllerTick(controller, deltaSeconds);
            return;
        }

        const ULONGLONG lootStartedAt = GetTickCount64();
        CleanupCounselorRouteWalkieTalkies(now);
        logStartupCost("loot-cleanup", lootStartedAt);
        EnsureTommyRadioObjectivePublished(now);
        RefreshUnarmedCounselorFleeState(now);

        // Beds, closets and tents expose a dedicated killer interactable.
        // Dispatch that native action before the frozen close-combat loop can
        // mistake the hidden counselor for a slash/grab target.
        if (DriveHidingSpotInteraction(now))
        {
            SetJasonHighPriorityPursuitBoost(false, "hiding-kill");
            RunBaseAIControllerTick(controller, deltaSeconds);
            return;
        }

        ProcessQueuedTrapPriority(now);
        TryPickupNearbyThrowingKnife(now);
        ExtendTraversalGraceForActiveDoor(now);

        // Stock interaction/special-move code owns Jason until the knife
        // pickup completes or aborts.  Keep the native base pawn/controller
        // tick alive, but do not let the custom chase overwrite its movement.
        if (g_PendingKnifePickup ||
            GetLockedJasonInteractable(g_JasonAIState.Jason))
        {
            ResetStuckSamplingAfterNativeInteraction(now);
            RunBaseAIControllerTick(controller, deltaSeconds);
            return;
        }

        ShortenConfirmedTrapPlacementHold(now);
        PrepareNonAggroStartupTick();

        ApplyHumanPursuitBalance(now);

        // Chase/combat helpers independently choose the nearest registered
        // counselor.  Restrict that choice synchronously while responding to
        // an owned trap, then restore the complete registry after this Tick.
        const bool restrictToTrapVictim =
            g_TrapPriorityVictim &&
            now < g_TrapPriorityUntil &&
            IsUsablePriorityVictim(g_TrapPriorityVictim);
        AActor* savedTargets[JasonAITargetCapacity]{};
        ULONGLONG savedBlockedUntil[JasonAITargetCapacity]{};
        uint8_t savedBlockStrikes[JasonAITargetCapacity]{};
        const int32_t savedTargetCount = g_JasonAITargetCount;

        if (restrictToTrapVictim)
        {
            std::memcpy(savedTargets, g_JasonAITargets, sizeof(savedTargets));
            std::memcpy(
                savedBlockedUntil,
                g_JasonAITargetBlockedUntil,
                sizeof(savedBlockedUntil));
            std::memcpy(
                savedBlockStrikes,
                g_JasonAITargetBlockStrikes,
                sizeof(savedBlockStrikes));
            g_JasonAITargets[0] = g_TrapPriorityVictim;
            g_JasonAITargetBlockedUntil[0] = 0;
            g_JasonAITargetBlockStrikes[0] = 0;
            g_JasonAITargetCount = 1;
            g_JasonAIState.Target = g_TrapPriorityVictim;
        }

        // Strategic setup owns navigation/facing. Starting a throwing-knife
        // montage during the phone/car trap route made the projectile inherit
        // a stale aim driver and spin in place, while both routines competed
        // for the same controller frame. Melee/grab defense remains available;
        // defer only new knife starts until setup has cleanly completed.
        if (g_JasonAIState.StartupTrapSetupActive &&
            g_JasonAIState.NextKnifeAttemptAt < now + 1500)
        {
            g_JasonAIState.NextKnifeAttemptAt = now + 1500;
        }

        // The frozen trap driver only calls StopMovement before returning
        // during a Morph/placement hold. Run the base controller tick during
        // that idle wait and issue StopMovement once on entry, rather than
        // dispatching the same native stop every render frame for ~20 seconds.
        static ULONGLONG lastStartupSetupAt = 0;
        static bool previousStartupIdle = false;
        if (lastStartupSetupAt != g_JasonAIState.StartupTrapSetupStartedAt)
        {
            lastStartupSetupAt = g_JasonAIState.StartupTrapSetupStartedAt;
            previousStartupIdle = false;
        }
        const bool startupIdle =
            g_JasonAIState.StartupTrapSetupActive &&
            !g_JasonAIState.StartupTrapPhoneApproachActive &&
            !g_JasonAIState.StartupTrapTransitActive &&
            now < g_JasonAIState.StartupTrapNextActionAt;
        if (startupIdle && !previousStartupIdle)
            StopJasonAIMovementForKnifeOnGameThread();
        previousStartupIdle = startupIdle;

        // The phone placement probe invokes stock collision geometry. Its
        // precise 263 ms sampling matters only near the 85-125 uu placement
        // band. While Jason is still several meters away, leave the accepted
        // MoveTo running but postpone that expensive query.
        if (g_JasonAIState.StartupTrapSetupActive &&
            g_JasonAIState.StartupTrapPhoneApproachActive &&
            !g_JasonAIState.StartupTrapPhoneCandidateHoldActive &&
            now >= g_JasonAIState.StartupTrapPhoneNextProbeAt)
        {
            const int32_t objectiveIndex =
                g_JasonAIState.StartupTrapObjectiveIndex;
            if (objectiveIndex >= 0 &&
                objectiveIndex < g_JasonAIState.StartupTrapObjectiveCount &&
                g_JasonAIState.StartupTrapObjectiveKinds[objectiveIndex] == 1)
            {
                FVector jasonLocation{};
                FVector phoneLocation{};
                if (GetJasonAIActorLocation(g_JasonAIState.Jason,
                        jasonLocation) &&
                    GetJasonAIActorLocation(
                        g_JasonAIState.StartupTrapObjectives[objectiveIndex],
                        phoneLocation))
                {
                    const float dx = jasonLocation.X - phoneLocation.X;
                    const float dy = jasonLocation.Y - phoneLocation.Y;
                    if (dx * dx + dy * dy > 400.0f * 400.0f)
                        g_JasonAIState.StartupTrapPhoneNextProbeAt =
                            now + 421;
                }
            }
        }

        const ULONGLONG startupTransitMoveBefore =
            g_JasonAIState.StartupTrapTransitNextMoveAt;
        const ULONGLONG frozenTickStartedAt = GetTickCount64();
        if (startupIdle)
            RunBaseAIControllerTick(controller, deltaSeconds);
        else
            OfflineBotsKillerControllerTickHook(controller, deltaSeconds);
        logStartupCost("frozen-trap-tick", frozenTickStartedAt);

        // The frozen startup logic used to rebuild the same static
        // phone/car-objective path every 650 ms throughout the opening trap
        // route.  An accepted UE4 MoveTo remains active, and this build already
        // gives it a 6.1-second recovery lock.  Detect only a freshly-issued
        // transit request and phase its next repair to a non-harmonic interval;
        // do not keep pushing an existing deadline forward every frame.
        const ULONGLONG startupTransitMoveAfter =
            g_JasonAIState.StartupTrapTransitNextMoveAt;
        if (g_JasonAIState.StartupTrapSetupActive &&
            g_JasonAIState.StartupTrapTransitActive &&
            startupTransitMoveAfter != 0 &&
            startupTransitMoveAfter != startupTransitMoveBefore &&
            startupTransitMoveAfter >= now &&
            startupTransitMoveAfter <= now + 800)
        {
            g_JasonAIState.StartupTrapTransitNextMoveAt = now + 3103;
        }

        if (restrictToTrapVictim)
        {
            std::memcpy(g_JasonAITargets, savedTargets, sizeof(savedTargets));
            std::memcpy(
                g_JasonAITargetBlockedUntil,
                savedBlockedUntil,
                sizeof(savedBlockedUntil));
            std::memcpy(
                g_JasonAITargetBlockStrikes,
                savedBlockStrikes,
                sizeof(savedBlockStrikes));
            g_JasonAITargetCount = savedTargetCount;
        }
    }

    bool InstallCounselorRouteTickWrapper()
    {
        if (g_CounselorRouteTickWrapperInstalled)
            return true;

        const uintptr_t frozenHook = reinterpret_cast<uintptr_t>(
            &OfflineBotsKillerControllerTickHook);
        if (!g_KillerControllerTickHookInstalled ||
            !g_KillerControllerTickSlot ||
            !Memory::IsReadable(g_KillerControllerTickSlot, sizeof(uintptr_t)) ||
            *g_KillerControllerTickSlot != frozenHook)
        {
            return false;
        }

        DWORD oldProtection = 0;
        if (!VirtualProtect(
                g_KillerControllerTickSlot,
                sizeof(uintptr_t),
                PAGE_READWRITE,
                &oldProtection))
        {
            return false;
        }

        *g_KillerControllerTickSlot = reinterpret_cast<uintptr_t>(
            &CounselorRouteControllerTickHook);
        DWORD ignoredProtection = 0;
        VirtualProtect(
            g_KillerControllerTickSlot,
            sizeof(uintptr_t),
            oldProtection,
            &ignoredProtection);
        g_CounselorRouteTickWrapperInstalled = true;
        Logger::Success(
            "18L-AG counselor bridge: non-aggro startup Tick wrapper installed");
        return true;
    }

    void RestoreFrozenControllerTickHook()
    {
        if (!g_CounselorRouteTickWrapperInstalled ||
            !g_KillerControllerTickSlot ||
            !Memory::IsReadable(g_KillerControllerTickSlot, sizeof(uintptr_t)))
        {
            g_CounselorRouteTickWrapperInstalled = false;
            return;
        }

        if (*g_KillerControllerTickSlot == reinterpret_cast<uintptr_t>(
                &CounselorRouteControllerTickHook))
        {
            DWORD oldProtection = 0;
            if (VirtualProtect(
                    g_KillerControllerTickSlot,
                    sizeof(uintptr_t),
                    PAGE_READWRITE,
                    &oldProtection))
            {
                *g_KillerControllerTickSlot = reinterpret_cast<uintptr_t>(
                    &OfflineBotsKillerControllerTickHook);
                DWORD ignoredProtection = 0;
                VirtualProtect(
                    g_KillerControllerTickSlot,
                    sizeof(uintptr_t),
                    oldProtection,
                    &ignoredProtection);
            }
        }

        g_CounselorRouteTickWrapperInstalled = false;
    }
}

extern "C" void FrozenBridgeRegisterAdditionalJason(
    void* rawPawn,
    void* rawController)
{
    auto* pawn = reinterpret_cast<AActor*>(rawPawn);
    auto* controller = reinterpret_cast<UObject*>(rawController);
    if (!pawn || !controller)
        return;

    for (auto& entry : g_AdditionalCombatJasons)
    {
        if (entry.Jason == pawn || entry.Controller == controller)
        {
            entry = AdditionalCombatJasonState{};
            entry.World = Engine::GetWorld();
            entry.Jason = pawn;
            entry.Controller = controller;
            entry.NextMoveAt = GetTickCount64() + 317;
            entry.NextCombatAt = GetTickCount64() + 719;
            return;
        }
    }

    AdditionalCombatJasonState entry{};
    entry.World = Engine::GetWorld();
    entry.Jason = pawn;
    entry.Controller = controller;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG phase =
        static_cast<ULONGLONG>(g_AdditionalCombatJasons.size() % 7) * 431ULL;
    entry.NextMoveAt = now + 317ULL + phase;
    entry.NextCombatAt = now + 719ULL + phase;
    g_AdditionalCombatJasons.push_back(entry);
}

extern "C" bool FrozenBridgeDriveAdditionalJason(
    void* rawController,
    float deltaSeconds)
{
    auto* controller = reinterpret_cast<UObject*>(rawController);
    if (!controller)
        return false;

    AdditionalCombatJasonState* state = nullptr;
    size_t stateIndex = 0;
    for (size_t i = 0; i < g_AdditionalCombatJasons.size(); ++i)
    {
        if (g_AdditionalCombatJasons[i].Controller == controller)
        {
            state = &g_AdditionalCombatJasons[i];
            stateIndex = i;
            break;
        }
    }
    if (!state)
        return false;

    UWorld* world = Engine::GetWorld();
    if (!world || state->World != world || !state->Jason ||
        !Memory::IsReadable(state->Jason, sizeof(UObject)) ||
        !Memory::IsReadable(controller, sizeof(UObject)))
    {
        *state = AdditionalCombatJasonState{};
        return false;
    }

    RunBaseAIControllerTick(controller, deltaSeconds);
    const ULONGLONG now = GetTickCount64();

    auto stopMovement = [&]()
    {
        UFunction* stop = controller->Class
            ? FindFunctionInHierarchyByName(controller->Class, "StopMovement")
            : nullptr;
        if (stop)
        {
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(controller),
                controller,
                stop,
                nullptr);
        }
    };

    // The validated Pamela/death lane owns every killer once it starts.
    // Do not let reinforcement movement or attacks disturb the proven final
    // sequence; stop each added Jason only once for that state transition.
    if (IsPamelaExclusiveKillLaneActive(now) ||
        (g_PermanentHumanSweaterActivationUntil != 0 &&
            now < g_PermanentHumanSweaterActivationUntil))
    {
        if (!state->FinalKillStopped)
        {
            stopMovement();
            state->FinalKillStopped = true;
        }
        return true;
    }
    state->FinalKillStopped = false;

    HMODULE module = GetModuleHandle(nullptr);
    if (state->AttackPressed && now >= state->AttackReleaseAt)
    {
        constexpr uintptr_t RVA_AttackRelease = 0x003B5F00;
        const uintptr_t releaseAddress = module
            ? reinterpret_cast<uintptr_t>(module) + RVA_AttackRelease
            : 0;
        if (releaseAddress &&
            Memory::IsReadable(reinterpret_cast<void*>(releaseAddress), 1))
        {
            SafeJasonInteractionCall(state->Jason, releaseAddress);
        }
        state->AttackPressed = false;
        state->AttackReleaseAt = 0;
    }

    AActor* target = FindNearestJasonAICounselorTarget(state->Jason);
    if (!target)
    {
        if (state->Target)
            stopMovement();
        state->Target = nullptr;
        return true;
    }

    FVector jasonLocation{};
    FVector targetLocation{};
    if (!GetJasonAIActorLocation(state->Jason, jasonLocation) ||
        !GetJasonAIActorLocation(target, targetLocation))
    {
        return true;
    }

    const float dx = targetLocation.X - jasonLocation.X;
    const float dy = targetLocation.Y - jasonLocation.Y;
    const float dz = targetLocation.Z - jasonLocation.Z;
    const float distanceSquared = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(distanceSquared))
        return true;

    if (state->Target != target || now >= state->NextMoveAt)
    {
        IssueAIMoveToLocationOnGameThread(
            controller,
            targetLocation,
            150.0f,
            "AdditionalJasonChase");
        state->Target = target;
        // Each added killer receives a different non-harmonic phase.  A
        // moving target remains tracked without stacking nav rebuilds on the
        // primary Jason's maintenance frame.
        state->NextMoveAt = now + 4211ULL +
            static_cast<ULONGLONG>(stateIndex % 5) * 379ULL;
    }

    if (distanceSquared <= 275.0f * 275.0f &&
        now >= state->NextCombatAt && !state->AttackPressed)
    {
        constexpr uintptr_t RVA_AttackPress = 0x003B5F84;
        const uintptr_t pressAddress = module
            ? reinterpret_cast<uintptr_t>(module) + RVA_AttackPress
            : 0;
        if (pressAddress &&
            Memory::IsReadable(reinterpret_cast<void*>(pressAddress), 1) &&
            SafeJasonInteractionCall(state->Jason, pressAddress))
        {
            state->AttackPressed = true;
            state->AttackReleaseAt = now + 120;
        }
        state->NextCombatAt = now + 911ULL +
            static_cast<ULONGLONG>(stateIndex % 4) * 113ULL;
    }

    return true;
}

namespace FrozenJasonBridge
{
    void QueueMaskPickupDiagnostic()
    {
        g_MaskPickupDiagnosticRequested.store(
            true,
            std::memory_order_release);
    }

    void ReplaceOfflineCabinetLootBeforeSpawn(
        UObject* object,
        UFunction* function)
    {
        if (!object || !function || !object->Class)
            return;

        // The frozen engine bridge calls this before stock ProcessEvent and
        // AllowCounselorToSpectateJason afterward. Preserve the pickup event's
        // object across that stock call without changing the protected engine.
        static int32_t pickupItemName = -2;
        static int32_t pickupCompleteName = -2;
        static int32_t performPickupName = -2;
        if (pickupItemName == -2)
        {
            pickupItemName = FindFNameIndexExact("OnPickupItem");
            pickupCompleteName = FindFNameIndexExact("OnPickupCompleted");
            performPickupName = FindFNameIndexExact("PerformPickup");
        }
        if (function->NameIndex == pickupItemName ||
            function->NameIndex == pickupCompleteName ||
            function->NameIndex == performPickupName)
        {
            g_MaskPickupEventObject = object;
        }

        // This function is called from the global ProcessEvent bridge. Keep
        // its normal rejection path to one cached integer comparison.
        static int32_t constructionScriptNameIndex = -2;
        if (constructionScriptNameIndex == -2)
        {
            constructionScriptNameIndex =
                FindFNameIndexExact("UserConstructionScript");
        }
        if (function->NameIndex != constructionScriptNameIndex)
            return;

        // Only the game's cabinet hierarchy owns the hidden drawer item.
        if (!ObjectClassDerivesFromExact(object, "SCCabinet"))
            return;

        UObject* itemSpawner =
            ReadReflectedObjectProperty(object, "ItemSpawner");
        if (!itemSpawner || !itemSpawner->Class ||
            !Memory::IsReadable(itemSpawner, sizeof(UObject)))
        {
            return;
        }

        UPropertyLite* actorListProperty =
            FindPropertyInHierarchyByName(
                itemSpawner->Class,
                "ActorList");
        if (!actorListProperty ||
            actorListProperty->Offset_Internal <= 0 ||
            actorListProperty->Offset_Internal >= 0x10000)
        {
            return;
        }

        auto* actorList = reinterpret_cast<TArray<UClass*>*>(
            reinterpret_cast<uintptr_t>(itemSpawner) +
            actorListProperty->Offset_Internal);
        if (!Memory::IsReadable(actorList, sizeof(TArray<UClass*>)) ||
            !actorList->Data ||
            actorList->Count <= 0 ||
            actorList->Count > 128 ||
            !Memory::IsReadable(
                actorList->Data,
                sizeof(UClass*) *
                    static_cast<size_t>(actorList->Count)))
        {
            return;
        }

        UClass* usefulClasses[3]{};
        int32_t usefulClassCount = 0;
        for (int32_t index = 0;
            index < actorList->Count;
            ++index)
        {
            UClass* itemClass = actorList->Data[index];
            if (!itemClass ||
                !Memory::IsReadable(itemClass, sizeof(UObject)))
            {
                continue;
            }

            const std::string className = JasonAISafeName(
                reinterpret_cast<UObject*>(itemClass));
            const bool useful =
                className.find("FirstAid") != std::string::npos ||
                className.find("PocketKnife") != std::string::npos ||
                className.find("Firecracker") != std::string::npos;
            if (!useful)
                continue;

            bool duplicate = false;
            for (int32_t usefulIndex = 0;
                usefulIndex < usefulClassCount;
                ++usefulIndex)
            {
                if (usefulClasses[usefulIndex] == itemClass)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate && usefulClassCount < 3)
                usefulClasses[usefulClassCount++] = itemClass;
        }

        if (usefulClassCount == 0)
            return;

        int32_t replaced = 0;
        const uint32_t cabinetSeed =
            static_cast<uint32_t>(object->InternalIndex) * 1664525u +
            static_cast<uint32_t>(object->NameNumber) * 1013904223u;
        for (int32_t index = 0;
            index < actorList->Count;
            ++index)
        {
            UClass* itemClass = actorList->Data[index];
            if (!itemClass ||
                !Memory::IsReadable(itemClass, sizeof(UObject)))
            {
                continue;
            }

            const std::string className = JasonAISafeName(
                reinterpret_cast<UObject*>(itemClass));
            const bool unwanted =
                className == "WalkieTalkie_C" ||
                className == "PamelaTape_C" ||
                className == "TommyTape_C" ||
                className.find("Pamela_Tape") != std::string::npos ||
                className.find("Tommy_Tape") != std::string::npos;
            if (!unwanted)
                continue;

            const uint32_t selection =
                cabinetSeed +
                static_cast<uint32_t>(index) * 2654435761u;
            actorList->Data[index] =
                usefulClasses[selection %
                    static_cast<uint32_t>(usefulClassCount)];
            ++replaced;
        }

        static bool routeLogged = false;
        if (replaced > 0 && !routeLogged)
        {
            routeLogged = true;
            Logger::Success(
                "18L-BH cabinet loot route active: tapes and walkie-talkies replaced before drawer spawn");
        }
    }

    bool SuppressPamelaKneelEndStun(
        UObject* object,
        UFunction* function)
    {
        const ULONGLONG now = GetTickCount64();
        const bool openingTranceHeld =
            g_PermanentHumanSweaterActivationUntil != 0 &&
            now <= g_PermanentHumanSweaterActivationUntil;
        if ((!g_PamelaTranceKneelRequested && !openingTranceHeld) ||
            g_KillTeamFinalInteractionCommitted ||
            g_PamelaTranceKillWindowUntil == 0 ||
            now > g_PamelaTranceKillWindowUntil ||
            !object || !function ||
            object != reinterpret_cast<UObject*>(g_JasonAIState.Jason))
        {
            return false;
        }

        static int32_t endStunNameIndex = -2;
        if (endStunNameIndex == -2)
            endStunNameIndex = FindFNameIndexExact("EndStun");
        return function->NameIndex == endStunNameIndex;
    }

    void SkipDeadJasonCabinOutroAfterStart(
        UObject* object,
        UFunction* function)
    {
        static ULONGLONG pendingSkipAt = 0;
        static ULONGLONG pendingDeathAt = 0;
        static ULONGLONG lastSkippedDeathAt = 0;
        if (!g_KillTeamFinalInteractionCommitted ||
            g_NativeJasonDeathCinematicObservedAt == 0 ||
            !object || !function)
        {
            pendingSkipAt = 0;
            return;
        }

        static int32_t clientsPlayOutroNameIndex = -2;
        if (clientsPlayOutroNameIndex == -2)
            clientsPlayOutroNameIndex = FindFNameIndexExact("ClientsPlayOutro");
        const ULONGLONG now = GetTickCount64();
        if (function->NameIndex == clientsPlayOutroNameIndex &&
            lastSkippedDeathAt != g_NativeJasonDeathCinematicObservedAt)
        {
            // The prior exact ClientPlayOutro hook never fired: that log name
            // is native, while ClientsPlayOutro is the reflected event that
            // the earlier suppressor actually intercepted. Do not suppress
            // it; its completion callback advances PostMatchOutro.
            pendingDeathAt = g_NativeJasonDeathCinematicObservedAt;
            pendingSkipAt = now + 250;
            Logger::Debug(
                "18L-BS confirmed Jason death: stock ClientsPlayOutro completed; cabin skip queued");
            return;
        }
        if (pendingSkipAt == 0 || now < pendingSkipAt ||
            pendingDeathAt != g_NativeJasonDeathCinematicObservedAt)
            return;
        pendingSkipAt = 0;
        lastSkippedDeathAt = pendingDeathAt;

        UObject* worldSettings = reinterpret_cast<UObject*>(
            FindNearestWorldActorByClass("SCWorldSettings", nullptr, 0.0f));
        UFunction* skipOutro = worldSettings && worldSettings->Class
            ? FindFunctionInHierarchyByName(
                worldSettings->Class, "SkipLevelIntroOutro")
            : nullptr;
        alignas(16) uint8_t params[0x40]{};
        const bool dispatched = skipOutro && SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(worldSettings),
            worldSettings, skipOutro, params);
        Logger::Success(
            std::string("18L-BS confirmed Jason death: requested native cabin skip after ClientsPlayOutro | dispatched=") +
            (dispatched ? "true" : "false"));
    }

    bool ObservePamelaTranceDamageEvent(
        UObject* object,
        UFunction* function)
    {
        if (object && function &&
            object == reinterpret_cast<UObject*>(g_JasonAIState.Jason) &&
            g_KillTeamFinalSequenceStartedAt != 0)
        {
            // The stock death notification is stronger evidence than the
            // transient counselor-manager lock. Observe it after the
            // original ProcessEvent without ending the match reentrantly.
            static int32_t onCharacterDeathNameIndex = -2;
            static int32_t clientOnCharacterDeathNameIndex = -2;
            static int32_t clientsOnDeathNameIndex = -2;
            if (onCharacterDeathNameIndex == -2)
            {
                onCharacterDeathNameIndex = FindFNameIndexExact(
                    "OnCharacterDeath");
                clientOnCharacterDeathNameIndex = FindFNameIndexExact(
                    "CLIENT_OnCharacterDeath");
                clientsOnDeathNameIndex = FindFNameIndexExact(
                    "ClientsOnDeath");
            }
            const int32_t nameIndex = function->NameIndex;
            if (nameIndex == onCharacterDeathNameIndex ||
                nameIndex == clientOnCharacterDeathNameIndex ||
                nameIndex == clientsOnDeathNameIndex)
            {
                g_NativeJasonDeathEventObserved.store(
                    true, std::memory_order_release);
            }
        }

        if (g_PamelaTranceKillWindowUntil == 0 ||
            g_KillTeamFinalInteractionCommitted ||
            !object || !function ||
            object != reinterpret_cast<UObject*>(g_JasonAIState.Jason))
        {
            return false;
        }

        // These Blueprint damage notifications are emitted by the stock
        // AActor damage path even when Jason's displayed health is clamped.
        // Exact cached FName comparisons keep the active trance-window hook
        // effectively free for every unrelated ProcessEvent call.
        static int32_t receiveAnyDamageNameIndex = -2;
        static int32_t receivePointDamageNameIndex = -2;
        static int32_t receiveRadialDamageNameIndex = -2;
        if (receiveAnyDamageNameIndex == -2)
        {
            receiveAnyDamageNameIndex = FindFNameIndexExact(
                "ReceiveAnyDamage");
            receivePointDamageNameIndex = FindFNameIndexExact(
                "ReceivePointDamage");
            receiveRadialDamageNameIndex = FindFNameIndexExact(
                "ReceiveRadialDamage");
        }

        const int32_t nameIndex = function->NameIndex;
        if (nameIndex != receiveAnyDamageNameIndex &&
            nameIndex != receivePointDamageNameIndex &&
            nameIndex != receiveRadialDamageNameIndex)
        {
            return false;
        }

        // The callback is on the game thread immediately after the stock
        // damage notification. A local melee strike necessarily puts the
        // controlled counselor near Jason; preserve that pawn as the native
        // final-interaction owner instead of letting the AI-first target scan
        // steal the action. The stock ContextKill predicate remains the final
        // range/weapon authority, so a distant damage source is not promoted.
        AActor* local = g_LocalCounselorTarget;
        FVector localLocation{};
        FVector jasonLocation{};
        if (IsValidatedLiveCounselorPawn(local) &&
            GetCounselorInteractionManager(local) &&
            GetJasonAIActorLocation(local, localLocation) &&
            GetJasonAIActorLocation(g_JasonAIState.Jason, jasonLocation))
        {
            const float dx = localLocation.X - jasonLocation.X;
            const float dy = localLocation.Y - jasonLocation.Y;
            const float distanceSquared = dx * dx + dy * dy;
            if (std::isfinite(distanceSquared) &&
                distanceSquared <= 900.0f * 900.0f)
            {
                const ULONGLONG now = GetTickCount64();
                g_PamelaTrancePreferredFinisher = local;
                g_PamelaTrancePreferredFinisherUntil = now + 60000;
                g_NextKillTeamTickAt = now;
            }
        }

        g_PamelaTranceDamageEventObserved.store(
            true,
            std::memory_order_release);
        return true;
    }

    bool RewriteJasonSpectatorCameraUpdate(
        UFunction* function,
        void* params)
    {
        if (!g_JasonSpectatorForced || !function)
            return false;

        // This runs from ProcessEvent. Keep the rejection path to a cached
        // integer comparison so normal gameplay pays no reflection/string cost.
        static int32_t spectatorCameraNameIndex = -2;
        if (spectatorCameraNameIndex == -2)
        {
            spectatorCameraNameIndex =
                FindFNameIndexExact("ClientSetSpectatorCamera");
        }
        if (function->NameIndex != spectatorCameraNameIndex)
            return false;

        if (!params)
            return false;

        struct SpectatorCameraParams
        {
            FVector CameraLocation;
            FRotator CameraRotation;
        };
        if (!Memory::IsReadable(params, sizeof(SpectatorCameraParams)))
            return false;

        FVector location{};
        FRotator rotation{};
        if (!BuildJasonSpectatorCameraPose(location, rotation))
            return false;

        SpectatorCameraParams* camera =
            reinterpret_cast<SpectatorCameraParams*>(params);
        camera->CameraLocation = location;
        camera->CameraRotation = rotation;
        return true;
    }

    bool OverrideJasonSpectatorCameraResult(
        UFunction* function,
        void* params)
    {
        if (!g_JasonSpectatorForced || !function)
            return false;

        // This shares ProcessEvent's hottest path. Reject everything except
        // the one camera-manager event with a cached FName integer.
        static int32_t updateCameraNameIndex = -2;
        if (updateCameraNameIndex == -2)
        {
            updateCameraNameIndex =
                FindFNameIndexExact("BlueprintUpdateCamera");
        }
        if (function->NameIndex != updateCameraNameIndex || !params)
            return false;

        // Verified live layout (UE4 PlayerCameraManager): target 0x00,
        // location 0x08, rotation 0x14, FOV 0x20, return bool 0x24.
        struct BlueprintCameraParams
        {
            UObject* CameraTarget;
            FVector NewCameraLocation;
            FRotator NewCameraRotation;
            float NewCameraFOV;
            bool ReturnValue;
            uint8_t Padding[3];
        };
        FVector location{};
        FRotator rotation{};
        if (!BuildJasonSpectatorCameraPose(location, rotation))
            return false;

        __try
        {
            BlueprintCameraParams* camera =
                reinterpret_cast<BlueprintCameraParams*>(params);
            // CameraTarget is an input. Do not rewrite it after stock camera
            // evaluation; only replace the returned pose and success flag.
            camera->NewCameraLocation = location;
            camera->NewCameraRotation = rotation;
            if (!std::isfinite(camera->NewCameraFOV) ||
                camera->NewCameraFOV < 30.0f ||
                camera->NewCameraFOV > 150.0f)
            {
                camera->NewCameraFOV = 80.0f;
            }
            camera->ReturnValue = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    void ObserveMaskPickupEvent(
        UObject* object,
        UFunction* function,
        void* params)
    {
        if (!g_JasonAIState.Active || !object || !function || !params)
            return;

        // ProcessEvent is hot. All ordinary calls return after three integer
        // comparisons; reflection and names are used only for pickup events.
        static int32_t pickupItemName = -2;
        static int32_t pickupCompleteName = -2;
        static int32_t performPickupName = -2;
        if (pickupItemName == -2)
        {
            pickupItemName = FindFNameIndexExact("OnPickupItem");
            pickupCompleteName = FindFNameIndexExact("OnPickupCompleted");
            performPickupName = FindFNameIndexExact("PerformPickup");
        }
        if (function->NameIndex != pickupItemName &&
            function->NameIndex != pickupCompleteName &&
            function->NameIndex != performPickupName)
        {
            return;
        }
        g_MaskPickupEventObject = nullptr;

        AActor* counselor = nullptr;
        bool sawMask = false;
        auto inspect = [&](UObject* candidate)
        {
            if (!candidate || !Memory::IsReadable(candidate, sizeof(UObject)) ||
                !candidate->Class)
            {
                return;
            }
            if (ObjectClassDerivesFromExact(candidate, "SCCounselorCharacter"))
                counselor = reinterpret_cast<AActor*>(candidate);
            if (ObjectClassDerivesFromExact(candidate, "SCKillerMask"))
            {
                sawMask = true;
                return;
            }
            const std::string objectName = JasonAISafeName(candidate);
            const std::string className = JasonAISafeName(
                reinterpret_cast<UObject*>(candidate->Class));
            if (objectName.find("KillerMask") != std::string::npos ||
                className.find("KillerMask") != std::string::npos ||
                objectName.find("MaskPickup") != std::string::npos ||
                className.find("MaskPickup") != std::string::npos ||
                objectName.find("Mask_Pickup") != std::string::npos ||
                className.find("Mask_Pickup") != std::string::npos ||
                objectName.find("JasonMask") != std::string::npos ||
                className.find("JasonMask") != std::string::npos)
            {
                sawMask = true;
            }
        };
        inspect(object);
        int32_t inspectedProperties = 0;
        for (UField* field = function->Children, *next = nullptr;
             field && inspectedProperties++ < 64;
             field = next)
        {
            if (!Memory::IsReadable(field, sizeof(UPropertyLite)))
                break;
            next = field->Next;
            UPropertyLite* property = reinterpret_cast<UPropertyLite*>(field);
            if (property->ElementSize != sizeof(UObject*) ||
                property->Offset_Internal < 0 ||
                property->Offset_Internal >= 0x100)
            {
                continue;
            }
            UObject** value = reinterpret_cast<UObject**>(
                reinterpret_cast<uint8_t*>(params) +
                property->Offset_Internal);
            if (Memory::IsReadable(value, sizeof(UObject*)))
                inspect(*value);
        }
        if (!sawMask || !IsValidatedLiveCounselorPawn(counselor))
            return;

        g_MaskSweaterPickupOwnerPending = counselor;
        g_MaskSweaterPickupPendingUntil = GetTickCount64() + 5000;
        g_NextMaskSweaterPowerCheckAt = 0;
        Logger::Success(
            "18L-BJ native mask pickup observed; Pamela recovery queued | owner=" +
            JasonAISafeName(reinterpret_cast<UObject*>(counselor)));
    }

    bool HandleCounselorSpectatorCycleEvent(
        UObject* object,
        UFunction* function)
    {
        if (!g_JasonAIState.Active ||
            !g_JasonAIState.Jason ||
            !object || !function ||
            object != Engine::GetLocalPlayerController() ||
            !Memory::IsReadable(function, sizeof(UFunction)))
        {
            return false;
        }

        // ProcessEvent is one of UE4's hottest paths.  Resolve these FName
        // indices once, then reject unrelated events with two integer
        // comparisons instead of formatting/reflection on every event.
        static int32_t nextNameIndex = -2;
        static int32_t prevNameIndex = -2;
        if (nextNameIndex == -2)
        {
            nextNameIndex = FindFNameIndexExact("ServerViewNextPlayer");
            prevNameIndex = FindFNameIndexExact("ServerViewPrevPlayer");
        }
        const int32_t functionNameIndex = function->NameIndex;
        if (functionNameIndex != nextNameIndex &&
            functionNameIndex != prevNameIndex)
        {
            return false;
        }

        const ULONGLONG now = GetTickCount64();
        if (now - g_LastSpectatorCycleEventAt < 150)
            return false;
        g_LastSpectatorCycleEventAt = now;

        // The direct observer owns insertion at the completed counselor-cycle
        // boundary. If this shipping path does route the input through
        // ProcessEvent, use it only to release Jason immediately after stock
        // has already selected the next counselor.
        if (g_JasonSpectatorForced)
        {
            UObject* currentPlayerState = ReadLocalSpectatingPlayer();
            UObject* jasonPlayerState = ReadJasonSpectatorPlayerState();
            if (currentPlayerState && jasonPlayerState &&
                currentPlayerState != jasonPlayerState)
            {
                g_InsertJasonOnNextSpectatorCycle = true;
                g_JasonSpectatorForced = false;
                g_JasonSpectatorPreviousTarget = nullptr;
                g_JasonSpectatorPreviousPlayerState = nullptr;
                g_NextJasonSpectatorRepairAt = 0;
                g_NextJasonSpectatorPlayerStateRepairAt = 0;
                g_JasonSpectatorOrbitInitialized = false;
                g_LastObservedSpectatorPlayerState = currentPlayerState;
                ResetObservedSpectatorCounselorCycle();
                MarkObservedSpectatorCounselor(currentPlayerState);
            }
        }
        return false;
    }

    bool AllowCounselorToSpectateJason(
        UFunction* function,
        void* params)
    {
        ObserveMaskPickupEvent(
            g_MaskPickupEventObject,
            function,
            params);
        if (!g_JasonAIState.Active ||
            !g_JasonAIState.Jason ||
            !function ||
            !params)
        {
            return false;
        }

        // See HandleCounselorSpectatorCycleEvent: this hook is invoked from
        // ProcessEvent, so unrelated calls must remain allocation- and
        // reflection-free.
        static int32_t canSpectateNameIndex = -2;
        if (canSpectateNameIndex == -2)
            canSpectateNameIndex = FindFNameIndexExact("CanSpectate");
        if (function->NameIndex != canSpectateNameIndex)
            return false;

        static UFunction* cachedFunction = nullptr;
        static int32_t viewTargetOffset = -1;
        static int32_t returnValueOffset = -1;
        static bool layoutFailureLogged = false;
        if (cachedFunction != function)
        {
            cachedFunction = function;
            viewTargetOffset = -1;
            returnValueOffset = -1;

            UField* field = function->Children;
            for (int32_t guard = 0;
                 field && guard < 64;
                 ++guard)
            {
                if (!Memory::IsReadable(field, sizeof(UPropertyLite)))
                    break;

                UPropertyLite* property =
                    reinterpret_cast<UPropertyLite*>(field);
                const std::string name =
                    JasonAISafeName(reinterpret_cast<UObject*>(field));
                if (property->Offset_Internal >= 0 &&
                    property->Offset_Internal < 0x100)
                {
                    if (name == "ViewTarget" &&
                        property->ElementSize == sizeof(UObject*))
                    {
                        viewTargetOffset = property->Offset_Internal;
                    }
                    else if (name == "ReturnValue" &&
                        property->ElementSize == sizeof(bool))
                    {
                        returnValueOffset = property->Offset_Internal;
                    }
                }
                field = field->Next;
            }
        }

        if (viewTargetOffset < 0 || returnValueOffset < 0)
        {
            if (!layoutFailureLogged)
            {
                layoutFailureLogged = true;
                Logger::Error(
                    "18L-BL Jason spectator: CanSpectate parameter layout unavailable");
            }
            return false;
        }

        UObject** requestedPlayerState = reinterpret_cast<UObject**>(
            reinterpret_cast<uint8_t*>(params) + viewTargetOffset);
        uint8_t* result = reinterpret_cast<uint8_t*>(params) +
            returnValueOffset;
        if (!Memory::IsReadable(requestedPlayerState, sizeof(UObject*)) ||
            !Memory::IsReadable(result, sizeof(uint8_t)) ||
            !*requestedPlayerState)
        {
            return false;
        }

        UObject* jasonPlayerState = ReadReflectedObjectProperty(
            reinterpret_cast<UObject*>(g_JasonAIState.Jason),
            "PlayerState");
        if (!jasonPlayerState &&
            Memory::IsReadable(g_JasonAIState.Jason, 0x390))
        {
            UObject** directPlayerState = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(g_JasonAIState.Jason) + 0x388);
            if (Memory::IsReadable(directPlayerState, sizeof(UObject*)) &&
                *directPlayerState &&
                Memory::IsReadable(*directPlayerState, sizeof(UObject)))
            {
                jasonPlayerState = *directPlayerState;
            }
        }

        if (!jasonPlayerState ||
            *requestedPlayerState != jasonPlayerState)
        {
            return false;
        }

        const bool wasAllowed = *result != 0;
        *result = 1;
        static bool spectatorWidenLogged = false;
        if (!spectatorWidenLogged)
        {
            spectatorWidenLogged = true;
            Logger::Success(
                std::string("18L-BL Jason spectator eligible in stock next/previous cycle | stockAllowed=") +
                (wasAllowed ? "true" : "false"));
        }
        return true;
    }

    void ResetCounselorModeJason()
    {
        RestoreFrozenControllerTickHook();
        RemoveOfflineBotsControllerTickHook();
        RemoveNativeInteractionExceptionGuard();
        RemoveNativeInteractionNullGuard();
        RemoveCounselorRouteSoundBlipSuppression();
        RemoveCounselorRouteTrapTriggerHook();
        RemoveCounselorRouteHunterAxeLoadoutHook();
        RemoveUniversalFinalKillEligibilityHook();
        RemoveUniversalPamelaSweaterPickupHook();
        g_JasonAIState = JasonAIState{};
        g_JasonAICache = JasonAICache{};
        g_JasonRequestPending.store(false);
        g_JasonRequestUsed.store(false);
        g_CounselorRequestPending.store(false);
        g_JasonSpectatorPreviousTarget = nullptr;
        g_JasonSpectatorHotkeyHeld = false;
        g_JasonSpectatorForced = false;
        g_InsertJasonOnNextSpectatorCycle = true;
        g_LastSpectatorCycleEventAt = 0;
        g_LastObservedSpectatorPlayerState = nullptr;
        g_CachedJasonSpectatorPawn = nullptr;
        g_CachedJasonSpectatorPlayerState = nullptr;
        ResetObservedSpectatorCounselorCycle();
        g_NextSpectatorCyclePollAt = 0;
        g_NextJasonSpectatorRepairAt = 0;
        g_NextJasonSpectatorPlayerStateRepairAt = 0;
        g_JasonSpectatorOrbitInitialized = false;
        g_CounselorBotsSpawned.store(0);
        ResetJasonAITargets();
        ResetLoadedCounselorClassCache();
        g_CounselorClassPreloadRequested = false;
        g_CounselorClassPreloadRequestedAt = 0;
        g_LastNonAggroReadyAt = 0;
        g_NextTrapPriorityAttemptAt = 0;
        g_NextTrapPriorityLogAt = 0;
        g_TrapPriorityUntil = 0;
        g_TrapPriorityBaselineMorphAt = 0;
        g_NextKnifePickupScanAt = 0;
        g_NextKnifeRegistryRefreshAt = 0;
        g_KnifeRegistryRefreshLevel = 0;
        g_KnifeRegistryRefreshActor = 0;
        std::memset(g_KnifePickupRegistry, 0, sizeof(g_KnifePickupRegistry));
        g_KnifePickupRegistryCount = 0;
        std::memset(g_HidingSpotRegistry, 0, sizeof(g_HidingSpotRegistry));
        g_HidingSpotRegistryCount = 0;
        g_WorldInteractionRegistryComplete = false;
        g_KnifePickupStartedAt = 0;
        g_CombatBusyUntil = 0;
        g_QueuedTrapMorphBaseline.store(0, std::memory_order_release);
        g_QueuedTrapVictim.store(nullptr, std::memory_order_release);
        g_QueuedTriggeredTrap.store(nullptr, std::memory_order_release);
        g_TrapPriorityVictim = nullptr;
        g_TrapPriorityMorphCompleted = false;
        g_LastShortenedPlacementObjective = -1;
        g_PendingKnifePickup = nullptr;
        g_PendingKnifeComponent = nullptr;
        g_KnifeCountBeforePickup = -1;
        g_LastHeldCounselor = nullptr;
        g_HeldCounselorObservedAt = 0;
        g_GrabKillInputCommittedAt = 0;
        g_PostGrabTransitionCounselor = nullptr;
        g_PostGrabTransitionUntil = 0;
        g_GrabKillSlotsLoggedFor = nullptr;
        g_NextAdapterGrabKillAttemptAt = 0;
        g_NextGrabKillStateLogAt = 0;
        g_NextAdapterGrabKillSlot = 0;
        g_StaleGrabReleaseAttempted = false;
        g_LocalCounselorTarget = nullptr;
        g_LocalPlayerController = nullptr;
        g_NextLocalCounselorRefreshAt = 0;
        g_HumanPursuitStartedAt = 0;
        g_NextCounselorTargetPruneAt = 0;
        for (PendingDeadCounselorRetire& pending :
             g_PendingDeadCounselorRetires)
            pending = PendingDeadCounselorRetire{};
        g_FinalCounselorDeathObserved = false;
        g_NextCounselorRosterRefreshAt = 0;
        g_CounselorRosterRefreshLevel = 0;
        std::memset(
            g_CounselorControllers,
            0,
            sizeof(g_CounselorControllers));
        g_CounselorControllerCount = 0;
        g_CounselorConvergenceActive = false;
        g_NextCounselorConvergenceAt = 0;
        g_NextConvergenceCombatSweepAt = 0;
        g_SweaterPreconvergenceCursor = 0;
        g_CounselorConvergenceCursor = 0;
        g_CounselorConvergenceMeleeClass = nullptr;
        g_CounselorConvergenceMacheteClass = nullptr;
        std::memset(
            g_ConvergenceArmedCounselors,
            0,
            sizeof(g_ConvergenceArmedCounselors));
        g_ConvergenceArmedCounselorCount = 0;
        std::memset(
            g_ConvergenceTravelCounselors,
            0,
            sizeof(g_ConvergenceTravelCounselors));
        std::memset(g_ConvergenceTravelLastLocations, 0,
            sizeof(g_ConvergenceTravelLastLocations));
        std::memset(g_ConvergenceTravelLastProgressAt, 0,
            sizeof(g_ConvergenceTravelLastProgressAt));
        std::memset(g_ConvergenceTravelRecoveryUntil, 0,
            sizeof(g_ConvergenceTravelRecoveryUntil));
        std::memset(g_ConvergenceTravelHaveLocation, 0,
            sizeof(g_ConvergenceTravelHaveLocation));
        std::memset(g_ConvergenceTravelLastGoals, 0,
            sizeof(g_ConvergenceTravelLastGoals));
        std::memset(g_ConvergenceTravelLastRouteAt, 0,
            sizeof(g_ConvergenceTravelLastRouteAt));
        std::memset(g_ConvergenceTravelHaveGoal, 0,
            sizeof(g_ConvergenceTravelHaveGoal));
        g_ConvergenceTravelCounselorCount = 0;
        ResetVehicleInterceptionState();
        ResetVehicleRoadPointRegistry();
        g_LocalAimReticleWeapon = nullptr;
        g_LocalAimReticleWasAiming = false;
        g_LocalAimReticleEndSent = false;
        g_NextLocalAimReticleCheckAt = 0;
        g_IgnoredVehicleInterceptCar = nullptr;
        g_IgnoredVehicleInterceptUntil = 0;
        g_HidingSpotTarget = nullptr;
        g_HidingSpotCounselor = nullptr;
        g_HidingSpotInteractable = nullptr;
        g_IgnoredHidingSpot = nullptr;
        g_IgnoredHidingCounselor = nullptr;
        g_HidingSpotStartedAt = 0;
        g_HidingSpotAttemptPendingUntil = 0;
        g_HidingSpotIgnoreUntil = 0;
        g_NextHidingSpotMoveAt = 0;
        g_HidingSpotAttempts = 0;
        g_HidingSpotRepositionAttempted = false;
        g_HidingSpotRepositionSucceeded = false;
        g_NextHidingSpotScanAt = 0;
        g_NextHidingSpotInteractAt = 0;
        g_NextHidingSpotLogAt = 0;
        g_NextWalkieCleanupAt = 0;
        g_WalkieCleanupLevel = 0;
        g_WalkieCleanupActor = 0;
        g_WalkiesRemoved = 0;
        g_TapesRemoved = 0;
        g_InvalidPropellersRemoved = 0;
        g_SurplusKeysRemoved = 0;
        g_SurplusFusesRemoved = 0;
        g_UsefulPickupsSpawned = 0;
        g_LootCleanupPassActive = false;
        g_LootCleanupComplete = false;
        g_LootCensusSawBoat = false;
        g_LootCensusCarCount = 0;
        g_LootNonLooseKeyCount = 0;
        g_LootNonLooseFuseCount = 0;
        g_LootKeysKeptThisPass = 0;
        g_LootFusesKeptThisPass = 0;
        g_LootReplacementRandomState = 0;
        std::memset(
            g_LootReplacementClasses,
            0,
            sizeof(g_LootReplacementClasses));
        g_NextCounselorFleeRefreshAt = 0;
        g_CounselorFleeRefreshCursor = 0;
        g_CounselorBlackboardNameResolutionComplete = false;
        g_CounselorBlackboardNameResolveCursor = 0;
        g_SCWeaponNameIndex = -1;
        g_ShouldFleeKillerNameIndex = -1;
        g_ShouldFightBackNameIndex = -1;
        g_ShouldArmedFightBackNameIndex = -1;
        g_ShouldMeleeFightBackNameIndex = -1;
        g_SeekWeaponWhileFleeingNameIndex = -1;
        g_ShouldHideNameIndex = -1;
        g_ShouldOrientTowardKillerNameIndex = -1;
        g_JasonCharacterNameIndex = -1;
        g_KillTeamRoute = KillTeamRoute::None;
        g_KillTeamHelper = nullptr;
        g_KillTeamShack = nullptr;
        g_KillTeamSweater = nullptr;
        g_KillTeamAxe = nullptr;
        g_KillTeamMask = nullptr;
        g_NextKillTeamTickAt = 0;
        g_NextKillTeamDiscoveryAt = 0;
        g_NextKillTeamInteractAt = 0;
        g_NextHelperKnifeGrantAt = 0;
        g_NextSweaterUseAt = 0;
        g_NextFinalKillInteractAt = 0;
        g_NextKillTeamMoveAt = 0;
        g_NextKillTeamFollowAt = 0;
        g_KillTeamMoveTarget = nullptr;
        g_TommyJasonObjectiveOwner = nullptr;
        g_NextTommyJasonObjectiveRepairAt = 0;
        g_OrphanJasonStunStartedAt = 0;
        g_KillTeamHelperArmed = false;
        g_KillTeamHelperProtected = false;
        g_KillTeamMaskAcquired = false;
        g_KillTeamSweaterUseDispatched = false;
        g_KillTeamAxeDiscoveryAttempted = false;
        g_KillTeamAxePursuitStartedAt = 0;
        g_KillTeamAxeLastProgressAt = 0;
        g_KillTeamAxeBestDistance = FLT_MAX;
        g_KillTeamHelperNativeBusyUntil = 0;
        g_KillTeamHelperStableObservations = 0;
        g_KillTeamFinalContextUntil = 0;
        g_KillTeamFinalInteractionDispatched = false;
        g_KillTeamFinalInteractionPending = false;
        g_KillTeamFinalInteractionAttempts = 0;
        g_KillTeamFinalInteractionStartedAt = 0;
        g_KillTeamFinalSequenceStartedAt = 0;
        g_KillTeamFinalInteractionCommitted = false;
        g_FinalKillCompletionDeadline = 0;
        g_NativeJasonDeathEventObserved.store(
            false, std::memory_order_release);
        g_NativeJasonDeathCinematicObservedAt = 0;
        g_NativeJasonDeathMatchEndAt = 0;
        g_KillTeamFinalRecoveryCooldownUntil = 0;
        g_KillTeamFinalMoveFailures = 0;
        g_KillTeamFinalRepositionAttempted = false;
        g_KillTeamLastCancelledInteraction = nullptr;
        g_NextKillTeamInteractionCancelAt = 0;
        g_KillTeamPendingFinalContext = nullptr;
        g_KillTeamPendingFinalComponent = nullptr;
        g_LastRejectedFinalContext = nullptr;
        g_LastAcceptedFinalContext = nullptr;
        g_LastAcceptedFinalComponent = nullptr;
        g_LastAcceptedFinalKillComponent = nullptr;
        g_LastUniversalFinalEligibilityContext = nullptr;
        g_PermanentHumanSweaterCarrier = nullptr;
        g_PermanentHumanSweaterAbility = nullptr;
        g_PermanentHumanInnateActiveAbility = nullptr;
        g_PermanentHumanSweaterLatched = false;
        g_PermanentHumanSweaterRestoreLogged = false;
        g_PermanentHumanUnlimitedSweaterArmed = false;
        g_PermanentHumanSweaterRearmAt = 0;
        g_PermanentHumanSweaterRearmAttempts = 0;
        g_PermanentHumanSweaterRearmSucceeded = false;
        g_PermanentHumanSweaterActivationUntil = 0;
        g_PermanentHumanSweaterAttackUnlockAt = 0;
        g_PermanentHumanSweaterUseCount = 0;
        g_MaskSweaterPowerLastOwner = nullptr;
        g_MaskSweaterPickupOwnerPending = nullptr;
        g_MaskSweaterPickupPendingUntil = 0;
        g_MaskPickupEventObject = nullptr;
        g_NextMaskSweaterPowerCheckAt = 0;
        g_PamelaTranceKillWindowUntil = 0;
        g_PamelaTranceJasonHealthBaseline = 0.0f;
        g_PamelaTranceJasonHealthBaselineValid = false;
        g_PamelaTranceDamageEventObserved.store(
            false,
            std::memory_order_release);
        g_PamelaTranceKneelRequested = false;
        g_PamelaTranceFinalSlashAccepted = false;
        g_PamelaTrancePreferredFinisher = nullptr;
        g_PamelaTrancePreferredFinisherUntil = 0;
        g_RepeatJasonKillStanceAt = 0;
        g_RepeatJasonKillStanceDeadline = 0;
        g_RepeatJasonKillStanceAttempts = 0;
        g_PamelaExclusiveKillLaneActive = false;
        g_NextPamelaExclusiveStopAt = 0;
        g_UnlimitedSweaterWorldRuleLogged = false;
        g_NextFinalContextDiagnosticAt = 0;
        g_NextAITommyProtectionAt = 0;
        g_ProtectedAITommy = nullptr;
        g_NextTommyObjectivePublishAt = 0;
        g_TommyObjectiveRadio = nullptr;
        g_TommyObjectivePublished = false;
        g_FuseDiagnosticAt = 0;
        g_FuseDiagnosticLogged = false;
        g_MatchStateOffset = -1;
        g_PostMatchAIRetired = false;
        g_LastExtendedDoorTarget = nullptr;
        g_TrapTeleportExemptionDepth = 0;
        g_MinimumMorphTeleportFunction = nullptr;
        g_NextMinimumMorphLogAt = 0;
        g_NextMorphCooldownRefusalLogAt = 0;
    }

    bool AdoptCounselorModeJason(
        AActor* jason,
        UObject* killerController,
        AActor* localCounselor)
    {
        UWorld* world = Engine::GetWorld();

        if (!world ||
            !jason ||
            !killerController ||
            !localCounselor ||
            !Memory::IsReadable(world, sizeof(UWorld)) ||
            !Memory::IsReadable(jason, sizeof(UObject)) ||
            !Memory::IsReadable(killerController, sizeof(UObject)) ||
            !Memory::IsReadable(localCounselor, sizeof(UObject)))
        {
            Logger::Error(
                "18L-AD frozen bridge: donor-style Jason/controller/counselor state is invalid");
            return false;
        }

        RestoreFrozenControllerTickHook();
        RemoveOfflineBotsControllerTickHook();
        RemoveNativeInteractionExceptionGuard();
        RemoveNativeInteractionNullGuard();
        RemoveCounselorRouteHunterAxeLoadoutHook();
        RemoveUniversalFinalKillEligibilityHook();
        RemoveUniversalPamelaSweaterPickupHook();
        g_JasonAIState = JasonAIState{};
        g_JasonAICache = JasonAICache{};
        ResetJasonAITargets();
        ResetLoadedCounselorClassCache();

        g_JasonRequestPending.store(false);
        g_JasonRequestUsed.store(true);
        g_CounselorRequestPending.store(false);
        g_CounselorBotsSpawned.store(0);
        g_CounselorClassPreloadRequested = false;
        g_CounselorClassPreloadRequestedAt = 0;
        g_LastNonAggroReadyAt = 0;
        g_NextTrapPriorityAttemptAt = 0;
        g_NextTrapPriorityLogAt = 0;
        g_TrapPriorityUntil = 0;
        g_TrapPriorityBaselineMorphAt = 0;
        g_NextKnifePickupScanAt = 0;
        g_NextKnifeRegistryRefreshAt = 0;
        g_KnifeRegistryRefreshLevel = 0;
        g_KnifeRegistryRefreshActor = 0;
        std::memset(g_KnifePickupRegistry, 0, sizeof(g_KnifePickupRegistry));
        g_KnifePickupRegistryCount = 0;
        std::memset(g_HidingSpotRegistry, 0, sizeof(g_HidingSpotRegistry));
        g_HidingSpotRegistryCount = 0;
        g_WorldInteractionRegistryComplete = false;
        g_KnifePickupStartedAt = 0;
        g_CombatBusyUntil = 0;
        g_QueuedTrapMorphBaseline.store(0, std::memory_order_release);
        g_QueuedTrapVictim.store(nullptr, std::memory_order_release);
        g_QueuedTriggeredTrap.store(nullptr, std::memory_order_release);
        g_TrapPriorityVictim = nullptr;
        g_TrapPriorityMorphCompleted = false;
        g_LastShortenedPlacementObjective = -1;
        g_PendingKnifePickup = nullptr;
        g_PendingKnifeComponent = nullptr;
        g_KnifeCountBeforePickup = -1;
        g_LastHeldCounselor = nullptr;
        g_HeldCounselorObservedAt = 0;
        g_GrabKillInputCommittedAt = 0;
        g_PostGrabTransitionCounselor = nullptr;
        g_PostGrabTransitionUntil = 0;
        g_GrabKillSlotsLoggedFor = nullptr;
        g_NextAdapterGrabKillAttemptAt = 0;
        g_NextGrabKillStateLogAt = 0;
        g_NextAdapterGrabKillSlot = 0;
        g_StaleGrabReleaseAttempted = false;
        g_LocalCounselorTarget = localCounselor;
        UObject** localControllerField = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(localCounselor) + 0x3A0);
        g_LocalPlayerController =
            Memory::IsReadable(localControllerField, sizeof(UObject*))
                ? *localControllerField
                : nullptr;
        g_NextLocalCounselorRefreshAt = 0;
        g_HumanPursuitStartedAt = 0;
        g_NextCounselorTargetPruneAt = 0;
        for (PendingDeadCounselorRetire& pending :
             g_PendingDeadCounselorRetires)
            pending = PendingDeadCounselorRetire{};
        g_FinalCounselorDeathObserved = false;
        g_NextCounselorRosterRefreshAt = 0;
        g_CounselorRosterRefreshLevel = 0;
        std::memset(
            g_CounselorControllers,
            0,
            sizeof(g_CounselorControllers));
        g_CounselorControllerCount = 0;
        g_CounselorConvergenceActive = false;
        g_NextCounselorConvergenceAt = 0;
        g_NextConvergenceCombatSweepAt = 0;
        g_SweaterPreconvergenceCursor = 0;
        g_CounselorConvergenceCursor = 0;
        g_CounselorConvergenceMeleeClass = nullptr;
        g_CounselorConvergenceMacheteClass = nullptr;
        std::memset(
            g_ConvergenceArmedCounselors,
            0,
            sizeof(g_ConvergenceArmedCounselors));
        g_ConvergenceArmedCounselorCount = 0;
        std::memset(
            g_ConvergenceTravelCounselors,
            0,
            sizeof(g_ConvergenceTravelCounselors));
        std::memset(g_ConvergenceTravelLastLocations, 0,
            sizeof(g_ConvergenceTravelLastLocations));
        std::memset(g_ConvergenceTravelLastProgressAt, 0,
            sizeof(g_ConvergenceTravelLastProgressAt));
        std::memset(g_ConvergenceTravelRecoveryUntil, 0,
            sizeof(g_ConvergenceTravelRecoveryUntil));
        std::memset(g_ConvergenceTravelHaveLocation, 0,
            sizeof(g_ConvergenceTravelHaveLocation));
        std::memset(g_ConvergenceTravelLastGoals, 0,
            sizeof(g_ConvergenceTravelLastGoals));
        std::memset(g_ConvergenceTravelLastRouteAt, 0,
            sizeof(g_ConvergenceTravelLastRouteAt));
        std::memset(g_ConvergenceTravelHaveGoal, 0,
            sizeof(g_ConvergenceTravelHaveGoal));
        g_ConvergenceTravelCounselorCount = 0;
        ResetVehicleInterceptionState();
        g_LocalAimReticleWeapon = nullptr;
        g_LocalAimReticleWasAiming = false;
        g_LocalAimReticleEndSent = false;
        g_NextLocalAimReticleCheckAt = 0;
        ResetVehicleRoadPointRegistry();
        g_IgnoredVehicleInterceptCar = nullptr;
        g_IgnoredVehicleInterceptUntil = 0;
        g_HidingSpotTarget = nullptr;
        g_HidingSpotCounselor = nullptr;
        g_HidingSpotInteractable = nullptr;
        g_IgnoredHidingSpot = nullptr;
        g_IgnoredHidingCounselor = nullptr;
        g_HidingSpotStartedAt = 0;
        g_HidingSpotAttemptPendingUntil = 0;
        g_HidingSpotIgnoreUntil = 0;
        g_NextHidingSpotMoveAt = 0;
        g_HidingSpotAttempts = 0;
        g_HidingSpotRepositionAttempted = false;
        g_HidingSpotRepositionSucceeded = false;
        g_NextHidingSpotScanAt = 0;
        g_NextHidingSpotInteractAt = 0;
        g_NextHidingSpotLogAt = 0;
        g_NextWalkieCleanupAt = 0;
        g_WalkieCleanupLevel = 0;
        g_WalkieCleanupActor = 0;
        g_WalkiesRemoved = 0;
        g_TapesRemoved = 0;
        g_InvalidPropellersRemoved = 0;
        g_SurplusKeysRemoved = 0;
        g_SurplusFusesRemoved = 0;
        g_UsefulPickupsSpawned = 0;
        g_LootCleanupPassActive = false;
        g_LootCleanupComplete = false;
        g_LootCensusSawBoat = false;
        g_LootCensusCarCount = 0;
        g_LootNonLooseKeyCount = 0;
        g_LootNonLooseFuseCount = 0;
        g_LootKeysKeptThisPass = 0;
        g_LootFusesKeptThisPass = 0;
        g_LootReplacementRandomState = 0;
        std::memset(
            g_LootReplacementClasses,
            0,
            sizeof(g_LootReplacementClasses));
        g_NextCounselorFleeRefreshAt = 0;
        g_CounselorFleeRefreshCursor = 0;
        g_CounselorBlackboardNameResolutionComplete = false;
        g_CounselorBlackboardNameResolveCursor = 0;
        g_SCWeaponNameIndex = -1;
        g_ShouldFleeKillerNameIndex = -1;
        g_ShouldFightBackNameIndex = -1;
        g_ShouldArmedFightBackNameIndex = -1;
        g_ShouldMeleeFightBackNameIndex = -1;
        g_SeekWeaponWhileFleeingNameIndex = -1;
        g_ShouldHideNameIndex = -1;
        g_ShouldOrientTowardKillerNameIndex = -1;
        g_JasonCharacterNameIndex = -1;
        g_KillTeamRoute = KillTeamRoute::None;
        g_KillTeamHelper = nullptr;
        g_KillTeamShack = nullptr;
        g_KillTeamSweater = nullptr;
        g_KillTeamAxe = nullptr;
        g_KillTeamMask = nullptr;
        g_NextKillTeamTickAt = 0;
        g_NextKillTeamDiscoveryAt = 0;
        g_NextKillTeamInteractAt = 0;
        g_NextHelperKnifeGrantAt = 0;
        g_NextSweaterUseAt = 0;
        g_NextFinalKillInteractAt = 0;
        g_NextKillTeamMoveAt = 0;
        g_NextKillTeamFollowAt = 0;
        g_KillTeamMoveTarget = nullptr;
        g_TommyJasonObjectiveOwner = nullptr;
        g_NextTommyJasonObjectiveRepairAt = 0;
        g_OrphanJasonStunStartedAt = 0;
        g_KillTeamHelperArmed = false;
        g_KillTeamHelperProtected = false;
        g_KillTeamMaskAcquired = false;
        g_KillTeamSweaterUseDispatched = false;
        g_KillTeamAxeDiscoveryAttempted = false;
        g_KillTeamAxePursuitStartedAt = 0;
        g_KillTeamAxeLastProgressAt = 0;
        g_KillTeamAxeBestDistance = FLT_MAX;
        g_KillTeamHelperNativeBusyUntil = 0;
        g_KillTeamHelperStableObservations = 0;
        g_KillTeamFinalContextUntil = 0;
        g_KillTeamFinalInteractionDispatched = false;
        g_KillTeamFinalInteractionPending = false;
        g_KillTeamFinalInteractionAttempts = 0;
        g_KillTeamFinalInteractionStartedAt = 0;
        g_KillTeamFinalSequenceStartedAt = 0;
        g_KillTeamFinalInteractionCommitted = false;
        g_FinalKillCompletionDeadline = 0;
        g_NativeJasonDeathEventObserved.store(
            false, std::memory_order_release);
        g_NativeJasonDeathCinematicObservedAt = 0;
        g_NativeJasonDeathMatchEndAt = 0;
        g_KillTeamFinalRecoveryCooldownUntil = 0;
        g_KillTeamFinalMoveFailures = 0;
        g_KillTeamFinalRepositionAttempted = false;
        g_KillTeamLastCancelledInteraction = nullptr;
        g_NextKillTeamInteractionCancelAt = 0;
        g_KillTeamPendingFinalContext = nullptr;
        g_KillTeamPendingFinalComponent = nullptr;
        g_LastRejectedFinalContext = nullptr;
        g_LastAcceptedFinalContext = nullptr;
        g_LastAcceptedFinalComponent = nullptr;
        g_LastAcceptedFinalKillComponent = nullptr;
        g_LastUniversalFinalEligibilityContext = nullptr;
        g_PermanentHumanSweaterCarrier = nullptr;
        g_PermanentHumanSweaterAbility = nullptr;
        g_PermanentHumanInnateActiveAbility = nullptr;
        g_PermanentHumanSweaterLatched = false;
        g_PermanentHumanSweaterRestoreLogged = false;
        g_PermanentHumanUnlimitedSweaterArmed = false;
        g_PermanentHumanSweaterRearmAt = 0;
        g_PermanentHumanSweaterRearmAttempts = 0;
        g_PermanentHumanSweaterRearmSucceeded = false;
        g_PermanentHumanSweaterActivationUntil = 0;
        g_PermanentHumanSweaterAttackUnlockAt = 0;
        g_PermanentHumanSweaterUseCount = 0;
        g_MaskSweaterPowerLastOwner = nullptr;
        g_MaskSweaterPickupOwnerPending = nullptr;
        g_MaskSweaterPickupPendingUntil = 0;
        g_MaskPickupEventObject = nullptr;
        g_NextMaskSweaterPowerCheckAt = 0;
        g_PamelaTranceKillWindowUntil = 0;
        g_PamelaTranceJasonHealthBaseline = 0.0f;
        g_PamelaTranceJasonHealthBaselineValid = false;
        g_PamelaTranceDamageEventObserved.store(
            false,
            std::memory_order_release);
        g_PamelaTranceKneelRequested = false;
        g_PamelaTranceFinalSlashAccepted = false;
        g_PamelaTrancePreferredFinisher = nullptr;
        g_PamelaTrancePreferredFinisherUntil = 0;
        g_RepeatJasonKillStanceAt = 0;
        g_RepeatJasonKillStanceDeadline = 0;
        g_RepeatJasonKillStanceAttempts = 0;
        g_PamelaExclusiveKillLaneActive = false;
        g_NextPamelaExclusiveStopAt = 0;
        g_UnlimitedSweaterWorldRuleLogged = false;
        g_NextFinalContextDiagnosticAt = 0;
        g_NextAITommyProtectionAt = 0;
        g_ProtectedAITommy = nullptr;
        g_NextTommyObjectivePublishAt = 0;
        g_TommyObjectiveRadio = nullptr;
        g_TommyObjectivePublished = false;
        g_FuseDiagnosticAt = 0;
        g_FuseDiagnosticLogged = false;
        g_MatchStateOffset = -1;
        g_PostMatchAIRetired = false;
        g_LastExtendedDoorTarget = nullptr;
        g_TrapTeleportExemptionDepth = 0;
        g_MinimumMorphTeleportFunction = nullptr;
        g_NextMinimumMorphLogAt = 0;
        g_NextMorphCooldownRefusalLogAt = 0;

        // Match the inherited flags used by the working OfflineBots killer
        // controller.  The controller was created with the ordinary path
        // follower and possessed through the base AAIController body.
        uint8_t* actorFlags = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(killerController) + 0x34);
        uint8_t* aiFlags = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(killerController) + 0x408);

        if (Memory::IsReadable(actorFlags, 1))
            *actorFlags |= 0x02;
        if (Memory::IsReadable(aiFlags, 1))
            *aiFlags |= 0x10;

        g_JasonAICache.World = world;
        g_JasonAIState.World = world;
        g_JasonAIState.Jason = jason;
        g_JasonAIState.Controller = killerController;
        g_JasonAIState.Target = nullptr;
        ArmUnlimitedSweaterRuleForWorld();
        // Native MoveToActor keeps tracking its goal. Rebuilding an accepted
        // path every three seconds was unnecessary and produced a visible
        // cadence when combined with the injected overlay. Retain a bounded
        // six-second recovery refresh for failed/stale paths.
        g_JasonAIState.PathLockDurationSeconds = 6.1f;

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG openingGraceEndsAt =
            now + JasonAIMorphCooldownMs;

        // Phase noncritical maintenance lanes across the one-second window.
        // They keep their existing frequencies but never all mature on the
        // same game frame after world adoption.
        g_NextLocalCounselorRefreshAt = now + 90;
        g_NextVehicleInterceptActionAt = now + 180;
        g_NextCounselorTargetPruneAt = now + 290;
        g_NextAITommyProtectionAt = now + 490;
        g_NextKillTeamTickAt = now + 690;
        g_NextKnifePickupScanAt = now + 890;
        g_NextCounselorFleeRefreshAt = now + 1090;
        // The fuse/gas census was a one-time reverse-engineering diagnostic
        // that walked every actor twelve seconds into gameplay. Its findings
        // are established; keep it retired in playable builds.
        g_FuseDiagnosticAt = 0;

        // Give counselors the same full Morph recharge window at match start
        // that Jason receives after every later teleport.  Delaying the
        // startup state machine also protects the no-objectives fallback,
        // which otherwise could jump directly to a counselor after discovery.
        g_JasonAIState.LastMorphTeleportAt = now;
        g_JasonAIState.NextDistanceTeleportAt = openingGraceEndsAt;
        g_JasonAIState.InitialTeleportAt = openingGraceEndsAt;
        g_JasonAIState.StartupTrapSetupStartedAt = now;
        g_JasonAIState.StartupTrapNextActionAt = openingGraceEndsAt;
        g_JasonAIState.StartupTrapSetupActive = true;
        g_JasonAIState.NextKnifeAttemptAt = now + 12000;
        g_JasonAIState.InitialTeleportAttempted = false;
        g_JasonAIState.PathLocked = false;
        g_JasonAIState.PathLockUntil = 0;
        g_JasonAIState.NextStuckCheckAt = now + 1375;
        g_JasonAIState.LastAcceptedMoveAt = 0;
        g_JasonAIState.ConsecutiveStuckChecks = 0;
        g_JasonAIState.HaveLastLocation = false;

        RegisterJasonAITarget(localCounselor);
        RegisterLiveCounselorTargets(world);

        if (!InstallOfflineBotsControllerTickHook(killerController))
        {
            Logger::Error(
                "18L-AD frozen bridge: SCKillerAIController +0x410 Tick hook failed");
            g_JasonAIState = JasonAIState{};
            return false;
        }

        if (!InstallCounselorRouteTickWrapper())
        {
            Logger::Error(
                "18L-AG frozen bridge: counselor-route Tick wrapper failed");
            RemoveOfflineBotsControllerTickHook();
            g_JasonAIState = JasonAIState{};
            return false;
        }

        g_JasonAIState.Active = true;

        InstallNativeInteractionNullGuard();
        InstallNativeInteractionExceptionGuard();
        if (InstallCounselorRouteSoundBlipSuppression())
            DisableExistingCounselorRouteSoundBlips(jason);
        InstallCounselorRouteTrapTriggerHook();
        InstallUniversalFinalKillEligibilityHook();
        InstallUniversalPamelaSweaterPickupHook();

        Logger::Success(
            "18L-AG FROZEN AI ADOPTED: donor-style AI Jason is active | targets=" +
            std::to_string(g_JasonAITargetCount) +
            " | openingMorphGraceMs=" +
            std::to_string(JasonAIMorphCooldownMs) +
            " | recurringMorphCooldownMs=" +
            std::to_string(JasonAIMorphCooldownMs) +
            " | no Sandbox or UClass spoof used");
        return true;
    }
}
