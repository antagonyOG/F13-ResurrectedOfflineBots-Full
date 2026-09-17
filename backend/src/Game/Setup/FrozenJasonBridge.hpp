#pragma once

class UObject;
class AActor;
class UFunction;

namespace FrozenJasonBridge
{
    // F6 queues one local-inventory diagnostic for the game thread. The
    // worker thread never dereferences game objects.
    void QueueMaskPickupDiagnostic();

    // Runs immediately before a cabinet's construction script. Replace only
    // tape/walkie class entries in its native item-spawner list, so drawer
    // loot is born as a useful pickup and no attached actor is destroyed.
    void ReplaceOfflineCabinetLootBeforeSpawn(
        UObject* object,
        UFunction* function);

    // Adopt the donor-style AI Jason created by the dedicated counselor
    // lifecycle.  The frozen Features.cpp implementation remains byte-for-byte
    // unchanged; this translation-unit adapter is the only integration seam.
    bool AdoptCounselorModeJason(
        AActor* jason,
        UObject* killerController,
        AActor* localCounselor);

    void ResetCounselorModeJason();

    // Called after the stock BlueprintNativeEvent has evaluated CanSpectate.
    // Counselor mode keeps every stock spectator rule, widening only the
    // active AI Jason PlayerState so next/previous cycling can reach him.
    bool AllowCounselorToSpectateJason(
        UFunction* function,
        void* params);

    // The stock native CanSpectate path does not dispatch through the local
    // controller's ProcessEvent vtable in this build.  Observe the actual
    // next/previous spectator RPC instead and insert the active AI Jason into
    // that otherwise-stock cycle.  F4 remains a direct toggle fallback.
    bool HandleCounselorSpectatorCycleEvent(
        UObject* object,
        UFunction* function);

    // Observe a real damage event against Jason during the earned Pamela
    // trance window. This is a cheap ProcessEvent name/pointer gate and avoids
    // relying solely on Jason's clamped health value to recognize the hit.
    bool ObservePamelaTranceDamageEvent(
        UObject* object,
        UFunction* function);

    // Preserve the authored Jason kill stance between the first and second
    // melee hits. This is a pointer/name gate on ProcessEvent, active only for
    // the bounded Pamela kneel window.
    bool SuppressPamelaKneelEndStun(
        UObject* object,
        UFunction* function);

    // Keep ClientsPlayOutro's stock completion path, but skip its cabin
    // sequence shortly after it starts when Jason's death was confirmed.
    void SkipDeadJasonCabinOutroAfterStart(
        UObject* object,
        UFunction* function);

    // Reuse the stock spectator-camera update, but replace only its pose while
    // F4 is locked to the active AI Jason. This avoids competing camera calls.
    bool RewriteJasonSpectatorCameraUpdate(
        UFunction* function,
        void* params);

    // BlueprintUpdateCamera is the final camera-manager handoff. Override its
    // returned pose after stock evaluation so Jason spectating stays stable.
    bool OverrideJasonSpectatorCameraResult(
        UFunction* function,
        void* params);
}
