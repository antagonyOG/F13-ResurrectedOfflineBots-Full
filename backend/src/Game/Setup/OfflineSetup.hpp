#pragma once
#include <cstdint>

namespace OfflineSetup
{
    bool SetMapIndex(int32_t value);
    bool SetJasonIndex(int32_t value);
    bool SetPlayerCounselorIndex(int32_t value);
    bool SetDifficulty(int32_t value);
    bool SetCounselorCount(int32_t value);
    bool SetWeather(int32_t value);

    bool QueueArmSelectedSetup();
    bool QueueSelectedJason();
    bool QueueSelectedCounselor();
    bool QueueApplyGameSetupPreset();
    bool QueueDumpCounselorRoster();

    // Counselor-mode coordinator. The temporary Sandbox menu row is redirected
    // into OfflineBots synchronously; the frozen AI implementation remains
    // untouched.
    bool QueueSandboxCounselorSync();
    bool IsCounselorMenuRouteLatched();
    bool BirthSelectedCounselorForPreMatch(void* gameMode);
    bool IsCounselorBirthComplete();
    void MarkCounselorMatchInProgress();
    bool IsCounselorMatchInProgress();
    bool SpawnCounselorModeJasonAfterMatch(void* gameMode);
    bool BeginFrozenAICompatibilityShim();
    void EndFrozenAICompatibilityShim();
    bool IsCounselorModeAutoStartArmed();
    bool IsSandboxCounselorReady();
    int32_t GetCounselorModeBotCount();
    void MarkCounselorModeAutoStartFinished();

    bool HasPendingObjectivesPatch();
    bool TryPatchObjectivesWidgetOnGameThread();
    bool TryPatchObjectivesWidgetForEventOnGameThread(void* objectValue);
    bool HasPendingRequest();
    void TickWorker();
    bool ConsumePendingRequestOnGameThread();
}
