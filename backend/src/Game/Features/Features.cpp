#include "Features.hpp"
#include "../Engine/Engine.hpp"
#include <string>
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <algorithm>


extern "C" __declspec(noinline) bool SafeProcessEventCall(
    uintptr_t vptr,
    void* obj,
    void* func,
    void* params
);
static __declspec(noinline) bool
SafeNativeJasonMoveCall(
    uintptr_t requestCtorAddress,
    uintptr_t nativeMoveAddress,
    UObject* controller,
    AActor* goalActor,
    UClass* filterClass,
    uint8_t* resultOut)
{
    if (!requestCtorAddress ||
        !nativeMoveAddress ||
        !controller ||
        !goalActor ||
        !filterClass ||
        !resultOut)
    {
        return false;
    }

    __try
    {
        //
        // The real Resurrected FAIMoveRequest
        // constructor writes through +0x38.
        //
        alignas(16)
            uint8_t moveRequest[0x40]{};

        using MoveRequestCtorFn =
            void* (__fastcall*)(
                void*,
                AActor*
                );

        MoveRequestCtorFn constructRequest =
            (MoveRequestCtorFn)
            requestCtorAddress;

        constructRequest(
            moveRequest,
            goalActor
        );

        //
        // Match OfflineBots immediately after
        // construction.
        //
        // +0x18 NavigationFilter
        // +0x20 request bitfield
        // +0x24 AcceptanceRadius
        //
        *(UClass**)
            (moveRequest + 0x18) =
            filterClass;

        *(uint32_t*)
            (moveRequest + 0x20) |=
            0x4;

        *(float*)
            (moveRequest + 0x24) =
            5.0f;

        //
        // OfflineBots call convention:
        //
        // RCX = controller
        // RDX = FPathFollowingRequestResult output
        // R8  = FAIMoveRequest
        // R9  = nullptr
        //
        using NativeMoveFn =
            void(__fastcall*)(
                UObject*,
                void*,
                void*,
                void*
                );

        NativeMoveFn nativeMove =
            (NativeMoveFn)
            nativeMoveAddress;

        nativeMove(
            controller,
            resultOut,
            moveRequest,
            nullptr
        );

        return true;
    }
    __except (
        EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
static __declspec(noinline) bool
SafeJasonDriverCall(
    AActor* jason,
    uintptr_t driverAddress)
{
    if (!jason ||
        !driverAddress)
    {
        return false;
    }

    __try
    {
        using JasonDriverFn =
            void(__fastcall*)(
                AActor*,
                bool
                );

        JasonDriverFn driver =
            (JasonDriverFn)
            driverAddress;

        driver(
            jason,
            true
        );

        return true;
    }
    __except (
        EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static __declspec(noinline) bool
SafeJasonInteractionCall(
    AActor* jason,
    uintptr_t interactionAddress)
{
    if (!jason ||
        !interactionAddress)
    {
        return false;
    }

    __try
    {
        using JasonInteractionFn =
            void(__fastcall*)(
                AActor*
                );

        JasonInteractionFn interaction =
            (JasonInteractionFn)
            interactionAddress;

        interaction(
            jason
        );

        return true;
    }
    __except (
        EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static __declspec(noinline) bool
SafeJasonCanPlaceTrapCall(
    AActor* jason,
    uintptr_t canPlaceAddress,
    FVector* outLocation,
    FVector* outNormal,
    bool* outCanPlace)
{
    if (!jason ||
        !canPlaceAddress ||
        !outLocation ||
        !outNormal ||
        !outCanPlace)
    {
        return false;
    }

    __try
    {
        using CanPlaceTrapFn =
            bool(__fastcall*)(
                AActor*,
                FVector*,
                FVector*);

        CanPlaceTrapFn canPlace =
            (CanPlaceTrapFn)canPlaceAddress;

        *outCanPlace =
            canPlace(
                jason,
                outLocation,
                outNormal);

        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *outCanPlace = false;
        return false;
    }
}


static __declspec(noinline) bool
SafeSCThrowableMulticastImplementationCall(
    void* throwable,
    AActor* jason,
    const FVector* velocity,
    const FVector* launchLocation,
    uintptr_t* implementationAddressOut = nullptr)
{
    if (implementationAddressOut)
        *implementationAddressOut = 0;

    if (!throwable ||
        !jason ||
        !velocity ||
        !launchLocation)
    {
        return false;
    }

    __try
    {
        uintptr_t vtable =
            *(uintptr_t*)throwable;

        if (!vtable ||
            !Memory::IsReadable(
                (void*)(vtable + 0x5F0),
                sizeof(uintptr_t)))
        {
            return false;
        }

        uintptr_t implementationAddress =
            *(uintptr_t*)
            (vtable + 0x5F0);

        if (implementationAddressOut)
            *implementationAddressOut =
                implementationAddress;

        if (!implementationAddress ||
            !Memory::IsReadable(
                (void*)implementationAddress,
                1))
        {
            return false;
        }

        //
        // Confirmed from the Resurrected SCThrowable native-registration
        // table and MULTICAST_Use exec thunk:
        //
        //   MULTICAST_Use -> exec thunk RVA 0x004F0C90
        //   exec thunk    -> vtable +0x5F0
        //
        // Therefore +0x5F0 is the actual local
        // MULTICAST_Use_Implementation virtual. Calling the UFunction/RPC
        // wrapper at 0x004EE240 can report success while merely routing a
        // NetMulticast call. For autonomous offline Jason, invoke the
        // implementation directly so the throwable actor is initialized
        // locally exactly where the multicast ultimately dispatches.
        //
        using MulticastImplementationFn =
            void(__fastcall*)(
                void*,
                AActor*,
                const FVector*,
                const FVector*
                );

        MulticastImplementationFn fn =
            (MulticastImplementationFn)
            implementationAddress;

        fn(
            throwable,
            jason,
            velocity,
            launchLocation
        );

        return true;
    }
    __except (
        EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static __declspec(noinline) bool
SafeSCThrowableBuildVelocityCall(
    uintptr_t helperAddress,
    void* throwable,
    FVector* outVelocity,
    AActor* jason)
{
    if (!helperAddress ||
        !throwable ||
        !outVelocity ||
        !jason)
    {
        return false;
    }

    __try
    {
        // Resurrected 0x14040A7B0, called by SCThrowable::Use:
        //   RCX = throwable
        //   RDX = FVector* outVelocity
        //   R8  = Jason
        //   R9b = true
        // It also writes throwable +0x3E8 = Jason.
        using BuildVelocityFn =
            FVector* (__fastcall*)(
                void*,
                FVector*,
                AActor*,
                bool
                );

        BuildVelocityFn fn =
            (BuildVelocityFn)
            helperAddress;

        FVector* result =
            fn(
                throwable,
                outVelocity,
                jason,
                true
            );

        return result != nullptr;
    }
    __except (
        EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static __declspec(noinline) bool
SafeSCThrowableServerUseImplementationCall(
    uintptr_t implementationAddress,
    void* throwable,
    AActor* jason,
    const FVector* velocity,
    const FVector* launchLocation)
{
    if (!implementationAddress ||
        !throwable ||
        !jason ||
        !velocity ||
        !launchLocation ||
        !Memory::IsReadable(
            (void*)implementationAddress,
            1))
    {
        return false;
    }

    __try
    {
        // SCThrowable::SERVER_Use_Implementation begins at 0x140421A00.
        // The RPC wrapper routes through player ownership and silently drops
        // autonomous Jason's throw. This direct implementation performs the
        // stock authority, distance, active-driver and multicast checks.
        using ServerUseImplementationFn =
            void(__fastcall*)(
                void*,
                AActor*,
                const FVector*,
                const FVector*
                );

        ServerUseImplementationFn fn =
            (ServerUseImplementationFn)
            implementationAddress;

        fn(
            throwable,
            jason,
            velocity,
            launchLocation
        );

        return true;
    }
    __except (
        EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static bool
RunOfflineBotsControllerTickOnGameThread();

static void __fastcall
OfflineBotsKillerControllerTickHook(
    UObject* controller,
    float deltaSeconds);

static uintptr_t*
g_KillerControllerTickSlot =
nullptr;

static uintptr_t
g_OriginalKillerControllerTick =
0;

static bool
g_KillerControllerTickHookInstalled =
false;

static bool
InstallOfflineBotsControllerTickHook(
    UObject* controller)
{
    if (!controller ||
        !Memory::IsReadable(
            controller,
            sizeof(UObject)))
    {
        return false;
    }

    uintptr_t controllerVTable =
        *(uintptr_t*)controller;

    if (!controllerVTable ||
        !Memory::IsReadable(
            (void*)controllerVTable,
            sizeof(uintptr_t)))
    {
        return false;
    }

    //
    // SCKillerAIController::Tick
    //
    constexpr uintptr_t
        KillerControllerTickOffset =
        0x410;

    uintptr_t* tickSlot =
        (uintptr_t*)
        (controllerVTable +
            KillerControllerTickOffset);

    if (!Memory::IsReadable(
        tickSlot,
        sizeof(uintptr_t)))
    {
        return false;
    }

    uintptr_t hookAddress =
        (uintptr_t)
        &OfflineBotsKillerControllerTickHook;

    //
    // Already installed from this DLL.
    //
    if (g_KillerControllerTickHookInstalled)
    {
        return
            g_KillerControllerTickSlot ==
            tickSlot &&
            *tickSlot ==
            hookAddress;
    }

    HMODULE module =
        GetModuleHandle(nullptr);

    if (!module)
        return false;

    //
    // Confirmed Resurrected stock
    // SCKillerAIController Tick:
    //
    // FUN_14032BD80
    //
    constexpr uintptr_t
        RVA_StockKillerControllerTick =
        0x0032BD80;

    uintptr_t expectedStockTick =
        (uintptr_t)module +
        RVA_StockKillerControllerTick;

    if (*tickSlot !=
        expectedStockTick)
    {
        Logger::Debug(
            "Jason AI OfflineBots +0x410 verification failed"
        );

        return false;
    }

    DWORD oldProtection =
        0;

    if (!VirtualProtect(
        tickSlot,
        sizeof(uintptr_t),
        PAGE_READWRITE,
        &oldProtection))
    {
        return false;
    }

    g_OriginalKillerControllerTick =
        *tickSlot;

    g_KillerControllerTickSlot =
        tickSlot;

    *tickSlot =
        hookAddress;

    DWORD unusedProtection =
        0;

    VirtualProtect(
        tickSlot,
        sizeof(uintptr_t),
        oldProtection,
        &unusedProtection
    );

    g_KillerControllerTickHookInstalled =
        true;

    Logger::Success(
        "Jason AI OfflineBots SCKillerAIController +0x410 Tick hooked"
    );

    return true;
}

static void
RemoveOfflineBotsControllerTickHook()
{
    if (!g_KillerControllerTickHookInstalled)
        return;

    uintptr_t hookAddress =
        (uintptr_t)
        &OfflineBotsKillerControllerTickHook;

    if (g_KillerControllerTickSlot &&
        g_OriginalKillerControllerTick &&
        Memory::IsReadable(
            g_KillerControllerTickSlot,
            sizeof(uintptr_t)) &&
        *g_KillerControllerTickSlot ==
        hookAddress)
    {
        DWORD oldProtection =
            0;

        if (VirtualProtect(
            g_KillerControllerTickSlot,
            sizeof(uintptr_t),
            PAGE_READWRITE,
            &oldProtection))
        {
            *g_KillerControllerTickSlot =
                g_OriginalKillerControllerTick;

            DWORD unusedProtection =
                0;

            VirtualProtect(
                g_KillerControllerTickSlot,
                sizeof(uintptr_t),
                oldProtection,
                &unusedProtection
            );
        }
    }

    g_KillerControllerTickSlot =
        nullptr;

    g_OriginalKillerControllerTick =
        0;

    g_KillerControllerTickHookInstalled =
        false;
}

// Development-only human knife/trap tracers removed from AI-only build.
static Features g_features_instance;
Features* func = &g_features_instance;
static std::atomic<bool>
g_JasonRequestPending{ false };

static std::atomic<bool>
g_JasonRequestUsed{ false };

static std::atomic<int32_t>
g_AdditionalCombatJasonCount{ 0 };

// One counselor bot can be queued at a time from the
// render/input thread and consumed by the existing
// ProcessEvent game-thread bridge.
static std::atomic<bool>
g_CounselorRequestPending{ false };

static std::atomic<int32_t>
g_CounselorBotsSpawned{ 0 };

// F3 is intentionally not capped at the stock six-bot lobby size. Keep a
// generous fixed registry so added sandbox counselors remain visible to the
// proven AI without introducing vector allocation or full-world scans in the
// controller hot path. The engine/host remains the practical spawn limit.
static constexpr int32_t JasonAITargetCapacity = 64;
static AActor* g_JasonAITargets[JasonAITargetCapacity]{};
static int32_t g_JasonAITargetCount = 0;
static ULONGLONG g_JasonAITargetBlockedUntil[JasonAITargetCapacity]{};
static uint8_t g_JasonAITargetBlockStrikes[JasonAITargetCapacity]{};

static bool g_CounselorClassPreloadRequested = false;
static ULONGLONG g_CounselorClassPreloadRequestedAt = 0;

static void ResetJasonAITargets()
{
    for (int32_t i = 0; i < JasonAITargetCapacity; ++i)
    {
        g_JasonAITargets[i] = nullptr;
        g_JasonAITargetBlockedUntil[i] = 0;
        g_JasonAITargetBlockStrikes[i] = 0;
    }

    g_JasonAITargetCount = 0;
}

static int32_t FindJasonAITargetIndex(
    AActor* actor)
{
    if (!actor)
        return -1;

    for (int32_t i = 0;
        i < g_JasonAITargetCount;
        ++i)
    {
        if (g_JasonAITargets[i] == actor)
            return i;
    }

    return -1;
}

static bool IsJasonAITargetBlocked(
    AActor* actor,
    ULONGLONG now)
{
    int32_t index =
        FindJasonAITargetIndex(actor);

    if (index < 0)
        return false;

    return
        g_JasonAITargetBlockedUntil[index] > now;
}

static void BlockJasonAITarget(
    AActor* actor)
{
    int32_t index =
        FindJasonAITargetIndex(actor);

    if (index < 0)
        return;

    uint8_t strikes =
        ++g_JasonAITargetBlockStrikes[index];

    ULONGLONG duration =
        (strikes >= 2) ?
        600000ULL :
        90000ULL;

    g_JasonAITargetBlockedUntil[index] =
        GetTickCount64() + duration;
}

static bool HasAlternativeUsableJasonAITarget(
    AActor* excludedTarget)
{
    constexpr uintptr_t
        Offset_PawnController =
        0x3A0;

    ULONGLONG now =
        GetTickCount64();

    for (int32_t i = 0;
        i < g_JasonAITargetCount;
        ++i)
    {
        AActor* candidate =
            g_JasonAITargets[i];

        if (!candidate ||
            candidate == excludedTarget ||
            IsJasonAITargetBlocked(
                candidate,
                now) ||
            !Memory::IsReadable(
                candidate,
                sizeof(UObject)))
        {
            continue;
        }

        UObject** controllerPtr =
            (UObject**)
            ((uintptr_t)candidate +
                Offset_PawnController);

        if (!Memory::IsReadable(
            controllerPtr,
            sizeof(UObject*)) ||
            !*controllerPtr ||
            !Memory::IsReadable(
                *controllerPtr,
                sizeof(UObject)))
        {
            continue;
        }

        return true;
    }

    return false;
}

static void RegisterJasonAITarget(AActor* actor)
{
    if (!actor ||
        !Memory::IsReadable(
            actor,
            sizeof(UObject)))
    {
        return;
    }

    for (int32_t i = 0;
        i < g_JasonAITargetCount;
        ++i)
    {
        if (g_JasonAITargets[i] == actor)
            return;
    }

    if (g_JasonAITargetCount >= JasonAITargetCapacity)
        return;

    g_JasonAITargets[
        g_JasonAITargetCount++] =
        actor;
}

// One-time cache of counselor BlueprintGeneratedClass objects that are
// already loaded.  This is intentionally NOT a per-frame scan.
static UClass* g_LoadedCounselorClasses[32]{};
static bool g_LoadedCounselorClassUsed[32]{};
static int32_t g_LoadedCounselorClassCount = 0;
static bool g_LoadedCounselorClassScanDone = false;

static void ResetLoadedCounselorClassCache()
{
    for (int32_t i = 0; i < 32; ++i)
    {
        g_LoadedCounselorClasses[i] = nullptr;
        g_LoadedCounselorClassUsed[i] = false;
    }

    g_LoadedCounselorClassCount = 0;
    g_LoadedCounselorClassScanDone = false;
    g_CounselorClassPreloadRequested = false;
    g_CounselorClassPreloadRequestedAt = 0;
}

// POD-only helpers so SEH stays isolated from C++ object unwinding.
static __declspec(noinline) bool
FastRawObjectNameEquals(
    UObject* object,
    const char* expected)
{
    if (!object || !expected || !GNames)
        return false;

    __try
    {
        int32_t nameIndex =
            object->NameIndex;

        if (!GNames->IsValidIndex(
            nameIndex))
        {
            return false;
        }

        const FNameEntry* entry =
            GNames->GetById(
                nameIndex);

        if (!entry)
            return false;

        const char* actual =
            entry->AnsiName;

        if (!actual)
            return false;

        int32_t i = 0;

        for (; i < 127; ++i)
        {
            char a = actual[i];
            char b = expected[i];

            if (a != b)
                return false;

            if (a == '\0')
                return true;
        }

        return false;
    }
    __except (
        EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static __declspec(noinline) bool
FastRawObjectNameEndsWith(
    UObject* object,
    const char* suffix)
{
    if (!object || !suffix || !GNames)
        return false;

    __try
    {
        int32_t nameIndex =
            object->NameIndex;

        if (!GNames->IsValidIndex(
            nameIndex))
        {
            return false;
        }

        const FNameEntry* entry =
            GNames->GetById(
                nameIndex);

        if (!entry)
            return false;

        const char* actual =
            entry->AnsiName;

        if (!actual)
            return false;

        int32_t actualLength = 0;
        int32_t suffixLength = 0;

        while (actualLength < 127 &&
            actual[actualLength] != '\0')
        {
            ++actualLength;
        }

        while (suffixLength < 63 &&
            suffix[suffixLength] != '\0')
        {
            ++suffixLength;
        }

        if (actualLength < suffixLength ||
            suffixLength <= 0)
        {
            return false;
        }

        int32_t start =
            actualLength -
            suffixLength;

        for (int32_t i = 0;
            i < suffixLength;
            ++i)
        {
            if (actual[start + i] !=
                suffix[i])
            {
                return false;
            }
        }

        return true;
    }
    __except (
        EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static __declspec(noinline) UObject*
SafeReadGObjectPointer(
    uintptr_t objectsPtr,
    int32_t index)
{
    if (!objectsPtr ||
        index < 0)
    {
        return nullptr;
    }

    __try
    {
        return
            *(UObject**)
            (objectsPtr +
                ((uintptr_t)index *
                    sizeof(uintptr_t)));
    }
    __except (
        EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static __declspec(noinline) UObject*
SafeReadUObjectClass(
    UObject* object)
{
    if (!object)
        return nullptr;

    __try
    {
        return
            (UObject*)object->Class;
    }
    __except (
        EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static __declspec(noinline) UStruct*
SafeReadSuperStruct(
    UStruct* object)
{
    if (!object)
        return nullptr;

    __try
    {
        return object->Super;
    }
    __except (
        EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// Forward declarations used by the counselor cache/spawn helpers below.
static std::string JasonAISafeName(
    UObject* object);

static bool ProjectJasonAINavPointOnGameThread(
    const FVector& point,
    const FVector& extent,
    FVector& outLocation);

static bool RequestCounselorClassPreloadOnGameThread();

static bool
BuildLoadedCounselorClassCacheFast()
{
    if (g_LoadedCounselorClassScanDone)
    {
        if (g_LoadedCounselorClassCount > 0)
            return true;

        if (!g_CounselorClassPreloadRequested ||
            GetTickCount64() <
                g_CounselorClassPreloadRequestedAt +
                750)
        {
            return false;
        }
    }

    g_LoadedCounselorClassScanDone =
        true;

    HMODULE module =
        GetModuleHandle(nullptr);

    if (!module || !GNames)
        return false;

    constexpr uintptr_t
        GOBJECTS_OFFSET =
        0x030F6EF0;

    uintptr_t gObjectsAddress =
        (uintptr_t)module +
        GOBJECTS_OFFSET;

    if (!Memory::IsReadable(
        (void*)gObjectsAddress,
        0x10))
    {
        return false;
    }

    uintptr_t objectsPtr =
        *(uintptr_t*)
        (gObjectsAddress + 0x00);

    int32_t objectCount =
        *(int32_t*)
        (gObjectsAddress + 0x0C);

    if (!objectsPtr ||
        !Memory::IsValidPointer(
            objectsPtr) ||
        objectCount <= 0 ||
        objectCount >= 20000000)
    {
        return false;
    }

    ULONGLONG scanStarted =
        GetTickCount64();

    for (int32_t i = 0;
        i < objectCount &&
        g_LoadedCounselorClassCount < 32;
        ++i)
    {
        UObject* candidate =
            SafeReadGObjectPointer(
                objectsPtr,
                i
            );

        if (!candidate)
            continue;

        if (!FastRawObjectNameEndsWith(
            candidate,
            "_Counselor_C"))
        {
            continue;
        }

        UObject* metaClass =
            SafeReadUObjectClass(
                candidate
            );

        if (!metaClass ||
            (!FastRawObjectNameEquals(
                metaClass,
                "BlueprintGeneratedClass") &&
             !FastRawObjectNameEquals(
                metaClass,
                "Class")))
        {
            continue;
        }

        UClass* candidateClass =
            (UClass*)candidate;

        bool derivesFromCounselor =
            false;

        UStruct* current =
            (UStruct*)candidateClass;

        for (int guard = 0;
            current && guard < 64;
            ++guard)
        {
            if (FastRawObjectNameEquals(
                (UObject*)current,
                "SCCounselorCharacter"))
            {
                derivesFromCounselor =
                    true;
                break;
            }

            current =
                SafeReadSuperStruct(
                    current
                );
        }

        if (!derivesFromCounselor)
            continue;

        bool duplicate =
            false;

        for (int32_t c = 0;
            c <
            g_LoadedCounselorClassCount;
            ++c)
        {
            if (g_LoadedCounselorClasses[c] ==
                candidateClass)
            {
                duplicate =
                    true;
                break;
            }
        }

        if (duplicate)
            continue;

        g_LoadedCounselorClasses[
            g_LoadedCounselorClassCount++] =
            candidateClass;
    }

    ULONGLONG elapsed =
        GetTickCount64() -
        scanStarted;

    Logger::Debug(
        "Counselor AI loaded class cache: " +
        std::to_string(
            g_LoadedCounselorClassCount) +
        " classes | scanMs=" +
        std::to_string(
            elapsed)
    );

    return
        g_LoadedCounselorClassCount > 0;
}

static UClass*
PickUnusedLoadedCounselorClass(
    UClass* localCounselorClass)
{
    if (!BuildLoadedCounselorClassCacheFast())
        return nullptr;

    int32_t available = 0;

    for (int32_t i = 0;
        i < g_LoadedCounselorClassCount;
        ++i)
    {
        if (g_LoadedCounselorClasses[i] &&
            g_LoadedCounselorClasses[i] !=
                localCounselorClass &&
            !g_LoadedCounselorClassUsed[i])
        {
            ++available;
        }
    }

    // Once every non-local class has been used, allow a new cycle.
    if (available == 0)
    {
        for (int32_t i = 0;
            i < g_LoadedCounselorClassCount;
            ++i)
        {
            g_LoadedCounselorClassUsed[i] =
                false;
        }

        for (int32_t i = 0;
            i < g_LoadedCounselorClassCount;
            ++i)
        {
            if (g_LoadedCounselorClasses[i] &&
                g_LoadedCounselorClasses[i] !=
                    localCounselorClass)
            {
                ++available;
            }
        }
    }

    if (available <= 0)
        return nullptr;

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(
        &qpc);

    uint64_t randomValue =
        (uint64_t)qpc.QuadPart;

    randomValue ^=
        (uint64_t)GetTickCount64();

    randomValue ^=
        ((uint64_t)
            g_CounselorBotsSpawned.load() +
            1ULL) *
        0x9E3779B97F4A7C15ULL;

    randomValue ^= randomValue << 13;
    randomValue ^= randomValue >> 7;
    randomValue ^= randomValue << 17;

    int32_t wanted =
        (int32_t)
        (randomValue %
            (uint64_t)available);

    for (int32_t i = 0;
        i < g_LoadedCounselorClassCount;
        ++i)
    {
        UClass* candidate =
            g_LoadedCounselorClasses[i];

        if (!candidate ||
            candidate ==
                localCounselorClass ||
            g_LoadedCounselorClassUsed[i])
        {
            continue;
        }

        if (wanted-- != 0)
            continue;

        g_LoadedCounselorClassUsed[i] =
            true;

        Logger::Debug(
            std::string(
                "Counselor AI random class: loaded cache="
            ) +
            std::to_string(
                g_LoadedCounselorClassCount) +
            " | selected=" +
            JasonAISafeName(
                (UObject*)candidate)
        );

        return candidate;
    }

    return nullptr;
}

static bool
FindSafeCounselorSpawnLocation(
    const FVector& localLocation,
    int32_t slot,
    FVector& outLocation)
{
    // Shorter ring than the old +/-1200 offsets.  More importantly,
    // projection is deliberately shallow in Z so a cabin roof cannot
    // win over the floor/ground near the player.
    static const float
        offsets[12][2] =
        {
            { 700.0f, 0.0f },
            { -700.0f, 0.0f },
            { 0.0f, 700.0f },
            { 0.0f, -700.0f },
            { 500.0f, 500.0f },
            { -500.0f, 500.0f },
            { 500.0f, -500.0f },
            { -500.0f, -500.0f },
            { 350.0f, 0.0f },
            { -350.0f, 0.0f },
            { 0.0f, 350.0f },
            { 0.0f, -350.0f }
        };

    //
    // Character actor locations are around the capsule center, while
    // navmesh projection returns the walkable surface.  Estimate the
    // player's floor and insist on that same vertical nav layer.
    //
    constexpr float
        CounselorCapsuleLift =
        90.0f;

    float expectedGroundZ =
        localLocation.Z -
        CounselorCapsuleLift;

    FVector extent{};
    extent.X = 220.0f;
    extent.Y = 220.0f;
    extent.Z = 110.0f;

    int32_t start =
        (slot * 2) % 12;

    for (int32_t attempt = 0;
        attempt < 12;
        ++attempt)
    {
        int32_t index =
            (start + attempt) % 12;

        FVector candidate =
            localLocation;

        candidate.X +=
            offsets[index][0];

        candidate.Y +=
            offsets[index][1];

        candidate.Z =
            expectedGroundZ +
            25.0f;

        FVector projected{};

        if (!ProjectJasonAINavPointOnGameThread(
            candidate,
            extent,
            projected))
        {
            continue;
        }

        float verticalDelta =
            std::fabs(
                projected.Z -
                expectedGroundZ);

        if (!std::isfinite(
            verticalDelta) ||
            verticalDelta >
                120.0f)
        {
            continue;
        }

        //
        // SpawnAIFromClass receives an actor location, not a feet
        // location.  Lift the counselor capsule center above the
        // projected navmesh so it cannot begin embedded in the floor.
        //
        outLocation =
            projected;

        outLocation.Z +=
            CounselorCapsuleLift;

        return true;
    }

    return false;
}

// AI-only counselor spawn request entry point.
// The request itself is consumed by the existing game-thread bridge.
bool QueueCounselorBotRequest()
{
    bool expected = false;

    if (!g_CounselorRequestPending.compare_exchange_strong(
        expected,
        true))
    {
        Logger::Debug(
            "Counselor AI request already pending"
        );
        return false;
    }

    Logger::Debug(
        "Counselor AI request queued"
    );
    return true;
}

struct JasonAIState
{
    UWorld* World = nullptr;

    AActor* Jason = nullptr;
    UObject* Controller = nullptr;
    AActor* Target = nullptr;

    FVector LastLocation{};

    ULONGLONG PathLockUntil = 0;
    ULONGLONG NextStuckCheckAt = 0;

    ULONGLONG LastAcceptedMoveAt = 0;

    // Cheap anti-fixation state. If the same counselor repeatedly
    // produces AlreadyAtGoal/no-path while Jason cannot make useful
    // progress, temporarily ignore that target and hunt another one.
    AActor* NoPathTarget = nullptr;
    int32_t ConsecutiveNoPathTargets = 0;

    // Native Jason combat scheduling.
    ULONGLONG NextCombatAttemptAt = 0;
    ULONGLONG AttackReleaseAt = 0;
    ULONGLONG GrabKillReadyAt = 0;

    // Lightweight throwing-knife scheduler.
    // No reflected property scans or world scans are used.
    ULONGLONG NextKnifeAttemptAt = 0;
    ULONGLONG KnifePerformAt = 0;
    ULONGLONG KnifeEndAt = 0;
    ULONGLONG KnifeReleaseSentAt = 0;

    int32_t KnifeCountAtRelease = -1;
    void* KnifeDriverAtRelease = nullptr;

    AActor* KnifeTarget = nullptr;

    bool KnifeSequenceActive = false;
    bool KnifePerformSent = false;
    bool KnifeNotifyLaunchSent = false;

    int32_t ConsecutiveStuckChecks = 0;
    int32_t ConsecutiveNearTargetStuckDeferrals = 0;
    int32_t CombatAttemptCounter = 0;
    int32_t NextGrabKillDirection = 0;

    // Fast confirmed-door break state.
    ULONGLONG NextDoorBreakPulseAt = 0;
    ULONGLONG DoorBreakPulseEndAt = 0;
    AActor* DoorBreakTarget = nullptr;
    bool DoorBreakActive = false;
    bool DoorBreakInterior = false;

    // Interior-door traversal protection.  This is intentionally tiny
    // per-door state, not a world scan.  After an interior-door pulse,
    // keep normal MoveTo alive long enough to cross the threshold instead
    // of interpreting one doorway pause as permission to teleport outside.
    ULONGLONG DoorTraversalGraceUntil = 0;
    ULONGLONG DoorRetryAfter = 0;
    AActor* DoorRetryTarget = nullptr;

    bool AttackPressed = false;


    // One-shot opening strategy: before normal counselor hunting begins,
    // Jason visits the police-phone repair objective and each spawned car,
    // then asks the game's own AttemptPlaceTrap path to place one stock trap.
    // Objective discovery is bounded to startup and is never repeated during
    // the normal chase hot path.
    AActor* StartupTrapGameState = nullptr;
    AActor* StartupTrapInteriorPhone = nullptr;
    AActor* StartupTrapConfirmedActor = nullptr;
    // Jason_Shack embeds its entrance as a scene component rather than a
    // separate door actor.  Resolve this once during startup and retain only
    // the door's world point plus the horizontal direction that leads outside.
    FVector StartupTrapShackDoorLocation{};
    FVector StartupTrapShackExteriorDirection{};
    bool StartupTrapShackDoorResolved = false;
    AActor* StartupTrapObjectives[4]{};
    uint8_t StartupTrapObjectiveKinds[4]{};
    int32_t StartupTrapObjectiveCount = 0;
    int32_t StartupTrapObjectiveIndex = 0;
    int32_t StartupTrapCountBefore = -1;
    int32_t StartupTrapRetryCount = 0;
    int32_t StartupTrapDiscoveryReads = 0;
    ULONGLONG StartupTrapSetupStartedAt = 0;
    ULONGLONG StartupTrapNextActionAt = 0;
    ULONGLONG StartupTrapAttemptAt = 0;
    ULONGLONG StartupTrapPlacedAt = 0;
    ULONGLONG StartupTrapLastConfirmScanAt = 0;
    ULONGLONG StartupTrapMorphChargeUntil = 0;
    int32_t StartupTrapMorphChargeKey = -1;

    // Features51: one real Morph cooldown shared by startup objective Morphs
    // and long-distance counselor Morphs.  Only SUCCESSFUL Morph teleports
    // start the 20-second timer; failed candidates do not.
    ULONGLONG LastMorphTeleportAt = 0;
    ULONGLONG NextMorphCooldownLogAt = 0;

    // Phone fuse-panel setup is different from cars: Morph near the cabin,
    // then WALK toward the panel until the game's own CanPlaceTrap solution
    // is close and centered.  This avoids guessing a perfect teleport point.
    ULONGLONG StartupTrapPhoneApproachStartedAt = 0;
    ULONGLONG StartupTrapPhoneNextMoveAt = 0;
    ULONGLONG StartupTrapPhoneNextProbeAt = 0;
    ULONGLONG StartupTrapPhoneCandidateHoldAt = 0;
    int32_t StartupTrapPhoneStableSamples = 0;
    bool StartupTrapPhoneApproachActive = false;
    bool StartupTrapPhoneCandidateHoldActive = false;

    // Features53: while Morph is recharging after a trap, Jason does not stand
    // still. He walks toward the next objective and remains combat-capable.
    AActor* StartupTrapTransitGoal = nullptr;
    ULONGLONG StartupTrapTransitNextMoveAt = 0;
    bool StartupTrapTransitActive = false;

    bool StartupTrapSetupActive = false;
    bool StartupTrapDiscoveryScanDone = false;
    bool StartupTrapObjectivesReady = false;
    bool StartupTrapTeleported = false;
    bool StartupTrapAttemptSent = false;
    bool StartupTrapMorphChargeActive = false;
    bool StartupTrapCountConsumed = false;
    bool StartupTrapActorConfirmed = false;
    
    ULONGLONG InitialTeleportAt = 0;
    ULONGLONG NextDistanceTeleportAt = 0;

    bool InitialTeleportAttempted = false;

    float PathLockDurationSeconds = 3.0f;
    float StuckThreshold = 30.0f;

    bool PathLocked = false;
    bool HaveLastLocation = false;
    bool Active = false;
    bool DriverTailLogged = false;
};

static JasonAIState
g_JasonAIState;

struct UPropertyLite : UField
{
    int32_t ArrayDim;
    int32_t ElementSize;
    uint64_t PropertyFlags;
    uint16_t RepIndex;
    uint8_t BlueprintReplicationCondition;
    uint8_t Pad_43;
    int32_t Offset_Internal;
};

static bool IsCounselorOrHero(AActor* actor)
{
    if (!actor)
        return false;

    std::string name = actor->GetName();

    return
        name.find("Counselor") != std::string::npos ||
        name.find("Hero") != std::string::npos;
}
static UFunction* FindFunctionInHierarchyByName(
    UClass* cls,
    const char* targetName)
{
    if (!cls || !targetName)
        return nullptr;

    for (UStruct* current = (UStruct*)cls;
        current;
        current = current->Super)
    {
        if (!Memory::IsReadable(
            current,
            sizeof(UStruct)))
            break;

        UField* field = current->Children;
        int guard = 0;

        while (field && guard++ < 2048)
        {
            if (!Memory::IsReadable(
                field,
                sizeof(UField)))
                break;

            UObject* fieldClass =
                (UObject*)field->ClassPrivate;

            if (fieldClass &&
                Memory::IsReadable(
                    fieldClass,
                    sizeof(UObject)) &&
                fieldClass->GetName() == "Function" &&
                field->GetName() == targetName)
            {
                return (UFunction*)field;
            }

            field = field->Next;
        }
    }

    return nullptr;
}

static std::string JasonAISafeName(
    UObject* object)
{
    if (!object ||
        !GNames ||
        !Memory::IsReadable(
            object,
            sizeof(UObject)))
    {
        return "";
    }

    int32_t index =
        object->NameIndex;

    if (!GNames->IsValidIndex(index))
        return "";

    const FNameEntry* entry =
        GNames->GetById(index);

    if (!entry ||
        !Memory::IsReadable(
            entry,
            0x20))
    {
        return "";
    }

    return std::string(
        entry->AnsiName
    );
}

struct JasonAICache
{
    UWorld* World = nullptr;
    bool SandboxMode = false;

    AActor* SandboxGameMode = nullptr;
    AActor* KillerStart = nullptr;

    UObject* KismetDefault = nullptr;
    UFunction* ConvertFunction = nullptr;

    UObject* AIBlueprintDefault = nullptr;
    UFunction* SpawnFunction = nullptr;

    UObject* BehaviorTree = nullptr;

    // Resurrected already ships the original counselor-bot
    // behavior tree. Cache it separately so AI Jason mode can
    // add native counselor bots without replacing Jason logic.
    UObject* CounselorBehaviorTree = nullptr;

    UClass* KillerAIControllerClass = nullptr;

    FVector KillerStartLocation{};

    uint8_t ZombieSoftClass[40]{};

    bool HaveZombieSoftClass = false;
    bool HaveKillerStartLocation = false;

    int32_t GObjectsScanIndex = 0;

    bool Ready = false;
    bool LoggedReady = false;
    bool LoggedScanExhausted = false;
};

static JasonAICache
g_JasonAICache;

static UObject* FindJasonAILoadedObjectByName(
    const char* wantedName,
    const char* wantedClassName)
{
    if (!wantedName)
        return nullptr;

    HMODULE module =
        GetModuleHandle(nullptr);

    if (!module)
        return nullptr;

    constexpr uintptr_t
        GOBJECTS_OFFSET =
        0x030F6EF0;

    uintptr_t gObjectsAddress =
        (uintptr_t)module +
        GOBJECTS_OFFSET;

    if (!Memory::IsReadable(
        (void*)gObjectsAddress,
        0x10))
    {
        return nullptr;
    }

    uintptr_t objectsPtr =
        *(uintptr_t*)
        (gObjectsAddress + 0x00);

    int32_t objectCount =
        *(int32_t*)
        (gObjectsAddress + 0x0C);

    if (!objectsPtr ||
        !Memory::IsValidPointer(objectsPtr) ||
        objectCount <= 0 ||
        objectCount >= 20000000)
    {
        return nullptr;
    }

    for (int32_t i = 0;
        i < objectCount;
        ++i)
    {
        uintptr_t itemAddress =
            objectsPtr +
            ((uintptr_t)i *
                sizeof(uintptr_t));

        if (!Memory::IsReadable(
            (void*)itemAddress,
            sizeof(uintptr_t)))
        {
            continue;
        }

        UObject* candidate =
            (UObject*)
            (*(uintptr_t*)itemAddress);

        if (!candidate ||
            !Memory::IsReadable(
                candidate,
                sizeof(UObject)))
        {
            continue;
        }

        if (JasonAISafeName(candidate) !=
            wantedName)
        {
            continue;
        }

        if (wantedClassName)
        {
            if (!candidate->Class ||
                !Memory::IsReadable(
                    candidate->Class,
                    sizeof(UObject)) ||
                JasonAISafeName(
                    (UObject*)candidate->Class
                ) != wantedClassName)
            {
                continue;
            }
        }

        return candidate;
    }

    return nullptr;
}

static void PrecacheJasonAIResources()
{
    auto controller =
        Engine::GetLocalPlayerController();

    if (!controller ||
        !controller->AcknowledgedPawn ||
        !controller->Class ||
        !Memory::IsReadable(
            controller->Class,
            sizeof(UObject)))
    {
        return;
    }

    std::string controllerClass =
        JasonAISafeName(
            (UObject*)controller->Class
        );

    const bool sandboxMode =
        controllerClass.find("Sandbox") != std::string::npos;
    const bool adoptedCounselorMode =
        g_JasonAIState.Active &&
        g_JasonAIState.World == Engine::GetWorld() &&
        g_JasonAIState.Jason &&
        Memory::IsReadable(g_JasonAIState.Jason, sizeof(UObject));
    if (!sandboxMode && !adoptedCounselorMode)
    {
        return;
    }

    UWorld* world =
        Engine::GetWorld();

    if (!world ||
        !Memory::IsReadable(
            world,
            sizeof(UWorld)))
    {
        return;
    }

    //
    // New Sandbox/world: reset the cache and hotkey spawn counters.
    //
    if (g_JasonAICache.World != world)
    {
        if (!adoptedCounselorMode)
            RemoveOfflineBotsControllerTickHook();

        g_JasonAICache =
            JasonAICache{};

        g_JasonAICache.World =
            world;
        g_JasonAICache.SandboxMode = sandboxMode;

        g_JasonRequestPending.store(false);
        g_JasonRequestUsed.store(false);
        g_AdditionalCombatJasonCount.store(0);
        g_CounselorRequestPending.store(false);
        g_CounselorBotsSpawned.store(0);
        ResetLoadedCounselorClassCache();
        if (!adoptedCounselorMode)
        {
            ResetJasonAITargets();
            g_JasonAIState = JasonAIState{};
            g_JasonAIState.World = world;
        }
    }

    constexpr uintptr_t
        Offset_Levels =
        0x110;

    TArray<ULevel*>* levels =
        (TArray<ULevel*>*)
        ((uintptr_t)world +
            Offset_Levels);

    if (!Memory::IsReadable(
        levels,
        sizeof(TArray<ULevel*>)) ||
        !levels->Data ||
        levels->Count <= 0 ||
        levels->Count > 1024 ||
        !Memory::IsReadable(
            levels->Data,
            sizeof(ULevel*) *
            levels->Count))
    {
        return;
    }

    //
    // Find the Sandbox GameMode and
    // Killer_Start. This is all read-only.
    //
    if (sandboxMode &&
        (!g_JasonAICache.SandboxGameMode ||
        !g_JasonAICache.KillerStart)
       )
    {
        for (int32_t levelIndex = 0;
            levelIndex < levels->Count;
            ++levelIndex)
        {
            ULevel* level =
                levels->Data[levelIndex];

            if (!level ||
                !Memory::IsReadable(
                    level,
                    sizeof(ULevel)))
            {
                continue;
            }

            TArray<AActor*>& actors =
                level->Actors;

            if (!actors.Data ||
                actors.Count <= 0 ||
                actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) *
                    actors.Count))
            {
                continue;
            }

            for (int32_t i = 0;
                i < actors.Count;
                ++i)
            {
                AActor* actor =
                    actors[i];

                if (!actor ||
                    !Memory::IsReadable(
                        actor,
                        sizeof(UObject)) ||
                    !actor->Class ||
                    !Memory::IsReadable(
                        actor->Class,
                        sizeof(UObject)))
                {
                    continue;
                }

                std::string actorName =
                    JasonAISafeName(
                        (UObject*)actor
                    );

                std::string className =
                    JasonAISafeName(
                        (UObject*)actor->Class
                    );

                if (!g_JasonAICache.SandboxGameMode &&
                    className ==
                    "SCGameMode_Sandbox")
                {
                    g_JasonAICache.SandboxGameMode =
                        actor;
                }

                if (!g_JasonAICache.KillerStart &&
                    className ==
                    "SCKillerPlayerStart" &&
                    actorName.find(
                        "Killer_Start") !=
                    std::string::npos)
                {
                    g_JasonAICache.KillerStart =
                        actor;
                }
            }
        }
    }

    //
    // Cache Jason Zombie's 40-byte
    // SoftClass reference.
    //
    if (g_JasonAICache.SandboxGameMode &&
        !g_JasonAICache.HaveZombieSoftClass)
    {
        struct RawArray
        {
            uint8_t* Data;
            int32_t Count;
            int32_t Max;
        };

        constexpr uintptr_t
            Offset_KillerCharacterClasses =
            0x6B0;

        RawArray* killerClasses =
            (RawArray*)
            ((uintptr_t)
                g_JasonAICache.SandboxGameMode +
                Offset_KillerCharacterClasses);

        constexpr int32_t
            SoftClassSize =
            40;

        constexpr int32_t
            JasonZombieIndex =
            6;

        if (Memory::IsReadable(
            killerClasses,
            sizeof(RawArray)) &&
            killerClasses->Data &&
            killerClasses->Count >
            JasonZombieIndex &&
            killerClasses->Count <= 64)
        {
            uint8_t* zombieSoftClass =
                killerClasses->Data +
                ((uintptr_t)
                    JasonZombieIndex *
                    SoftClassSize);

            if (Memory::IsReadable(
                zombieSoftClass,
                SoftClassSize))
            {
                for (int i = 0;
                    i < SoftClassSize;
                    ++i)
                {
                    g_JasonAICache.
                        ZombieSoftClass[i] =
                        zombieSoftClass[i];
                }

                g_JasonAICache.
                    HaveZombieSoftClass =
                    true;
            }
        }
    }

    //
    // Cache the game's own Jason BehaviorTree.
    //
    if (g_JasonAICache.SandboxGameMode &&
        !g_JasonAICache.BehaviorTree)
    {
        constexpr uintptr_t
            Offset_KillerBehaviorTree =
            0x940;

        UObject** behaviorTreePtr =
            (UObject**)
            ((uintptr_t)
                g_JasonAICache.SandboxGameMode +
                Offset_KillerBehaviorTree);

        if (Memory::IsReadable(
            behaviorTreePtr,
            sizeof(UObject*)))
        {
            UObject* behaviorTree =
                *behaviorTreePtr;

            if (behaviorTree &&
                Memory::IsReadable(
                    behaviorTree,
                    sizeof(UObject)))
            {
                g_JasonAICache.BehaviorTree =
                    behaviorTree;
            }
        }
    }

    // The full counselor route does not own SCGameMode_Sandbox, but both
    // native AI assets are already loaded by the packed Offline Bots flow.
    // Exact-name lookup avoids a Sandbox world scan and lets F1/F3 reuse the
    // same SpawnAIFromClass bridge without disturbing the adopted primary AI.
    if (!g_JasonAICache.BehaviorTree)
    {
        g_JasonAICache.BehaviorTree =
            FindJasonAILoadedObjectByName("JasonBehaviorTree", "BehaviorTree");
    }
    if (!g_JasonAICache.CounselorBehaviorTree)
    {
        g_JasonAICache.CounselorBehaviorTree =
            FindJasonAILoadedObjectByName(
                "OfflineBotsCounselorBehaviorTree",
                "BehaviorTree");
    }

    //
    // Cache Resurrected's native Offline Bots counselor
    // BehaviorTree from the Sandbox GameMode by reflection.
    // This avoids hardcoding another GameMode offset.
    //
    if (g_JasonAICache.SandboxGameMode &&
        !g_JasonAICache.CounselorBehaviorTree &&
        g_JasonAICache.SandboxGameMode->Class &&
        Memory::IsReadable(
            g_JasonAICache.SandboxGameMode->Class,
            sizeof(UStruct)))
    {
        for (UStruct* current =
            (UStruct*)g_JasonAICache.SandboxGameMode->Class;
            current;
            current = current->Super)
        {
            if (!Memory::IsReadable(
                current,
                sizeof(UStruct)))
            {
                break;
            }

            UField* field = current->Children;
            int guard = 0;

            while (field && guard++ < 2048)
            {
                if (!Memory::IsReadable(
                    field,
                    sizeof(UField)))
                {
                    break;
                }

                std::string fieldName =
                    JasonAISafeName(
                        (UObject*)field
                    );

                std::string fieldType =
                    JasonAISafeName(
                        (UObject*)field->ClassPrivate
                    );

                if (fieldType == "ObjectProperty" &&
                    fieldName.find("CounselorBehaviorTree") !=
                    std::string::npos)
                {
                    UPropertyLite* property =
                        (UPropertyLite*)field;

                    int32_t offset =
                        property->Offset_Internal;

                    if (offset > 0 &&
                        offset < 0x10000)
                    {
                        UObject** valuePtr =
                            (UObject**)
                            ((uintptr_t)
                                g_JasonAICache.SandboxGameMode +
                                offset);

                        if (Memory::IsReadable(
                            valuePtr,
                            sizeof(UObject*)) &&
                            *valuePtr &&
                            Memory::IsReadable(
                                *valuePtr,
                                sizeof(UObject)))
                        {
                            UObject* candidate =
                                *valuePtr;

                            std::string objectName =
                                JasonAISafeName(
                                    candidate
                                );

                            if (objectName.find(
                                "CounselorBehaviorTree") !=
                                std::string::npos)
                            {
                                g_JasonAICache.
                                    CounselorBehaviorTree =
                                    candidate;

                                Logger::Success(
                                    "Counselor AI behavior tree ready: " +
                                    objectName
                                );
                            }
                        }
                    }
                }

                field = field->Next;
            }

            if (g_JasonAICache.CounselorBehaviorTree)
                break;
        }
    }

    //
    // Cache Killer_Start's world location.
    //
    if (g_JasonAICache.KillerStart &&
        !g_JasonAICache.
        HaveKillerStartLocation)
    {
        void** rootPtr =
            (void**)
            ((uintptr_t)
                g_JasonAICache.KillerStart +
                Offsets::Actor_RootComponent);

        if (Memory::IsReadable(
            rootPtr,
            sizeof(void*)) &&
            *rootPtr)
        {
            void* root =
                *rootPtr;

            FVector* location =
                (FVector*)
                ((uintptr_t)root +
                    Offsets::
                    Scene_ComponentToWorld +
                    Offsets::
                    FTransform_Translation);

            if (Memory::IsReadable(
                location,
                sizeof(FVector)))
            {
                g_JasonAICache.
                    KillerStartLocation =
                    *location;

                g_JasonAICache.
                    HaveKillerStartLocation =
                    true;
            }
        }
    }

    //
    //
    // Locate only the two reflected functions.
    //
    // Their owning UClass objects give us the
    // correct DefaultObjects directly, so we
    // do NOT search GObjects for either CDO.
    //
    bool needStaticResources =
        !g_JasonAICache.KismetDefault ||
        !g_JasonAICache.ConvertFunction ||
        !g_JasonAICache.AIBlueprintDefault ||
        !g_JasonAICache.SpawnFunction;

    if (needStaticResources)
    {
        constexpr uintptr_t
            GOBJECTS_OFFSET =
            0x030F6EF0;

        HMODULE module =
            GetModuleHandle(nullptr);

        if (module)
        {
            uintptr_t gObjectsAddress =
                (uintptr_t)module +
                GOBJECTS_OFFSET;

            if (Memory::IsReadable(
                (void*)gObjectsAddress,
                0x10))
            {
                uintptr_t objectsPtr =
                    *(uintptr_t*)
                    (gObjectsAddress + 0x00);

                int32_t objectCount =
                    *(int32_t*)
                    (gObjectsAddress + 0x0C);

                if (objectsPtr &&
                    Memory::IsValidPointer(
                        objectsPtr) &&
                    objectCount > 0 &&
                    objectCount < 20000000)
                {
                    auto getObjectAtIndex =
                        [&](int32_t index)
                        -> UObject*
                        {
                            if (index < 0 ||
                                index >= objectCount)
                            {
                                return nullptr;
                            }

                            uintptr_t itemAddress =
                                objectsPtr +
                                ((uintptr_t)index *
                                    sizeof(uintptr_t));

                            if (!Memory::IsReadable(
                                (void*)itemAddress,
                                sizeof(uintptr_t)))
                            {
                                return nullptr;
                            }

                            uintptr_t objectAddress =
                                *(uintptr_t*)
                                itemAddress;

                            if (!objectAddress ||
                                !Memory::IsReadable(
                                    (void*)objectAddress,
                                    sizeof(UObject)))
                            {
                                return nullptr;
                            }

                            return
                                (UObject*)
                                objectAddress;
                        };

                    auto findExpectedNearIndex =
                        [&](int32_t knownIndex,
                            const char* expectedName)
                        -> UObject*
                        {
                            if (!expectedName)
                                return nullptr;

                            //
                            // Exact known GObjects index.
                            //
                            UObject* exact =
                                getObjectAtIndex(
                                    knownIndex
                                );

                            if (exact &&
                                JasonAISafeName(
                                    exact
                                ) ==
                                expectedName)
                            {
                                return exact;
                            }

                            //
                            // Small fallback only in case
                            // this run shifted slightly.
                            //
                            constexpr int32_t
                                SearchRadius =
                                256;

                            int32_t first =
                                knownIndex -
                                SearchRadius;

                            int32_t last =
                                knownIndex +
                                SearchRadius;

                            if (first < 0)
                                first = 0;

                            if (last >= objectCount)
                            {
                                last =
                                    objectCount - 1;
                            }

                            for (int32_t i = first;
                                i <= last;
                                ++i)
                            {
                                if (i == knownIndex)
                                    continue;

                                UObject* candidate =
                                    getObjectAtIndex(i);

                                if (!candidate)
                                    continue;

                                if (JasonAISafeName(
                                    candidate
                                ) ==
                                    expectedName)
                                {
                                    return candidate;
                                }
                            }

                            return nullptr;
                        };

                    //
                    // GObjects 10101:
                    // Conv_SoftClassReferenceToClass
                    //
                    if (!g_JasonAICache.
                        ConvertFunction)
                    {
                        UObject* candidate =
                            findExpectedNearIndex(
                                10101,
                                "Conv_SoftClassReferenceToClass"
                            );

                        if (candidate &&
                            candidate->Class &&
                            Memory::IsReadable(
                                candidate->Class,
                                sizeof(UObject)) &&
                            JasonAISafeName(
                                (UObject*)
                                candidate->Class
                            ) ==
                            "Function")
                        {
                            UObject* outer =
                                candidate->
                                OuterPrivate;

                            if (outer &&
                                Memory::IsReadable(
                                    outer,
                                    sizeof(UClass)) &&
                                JasonAISafeName(
                                    outer
                                ) ==
                                "KismetSystemLibrary")
                            {
                                g_JasonAICache.
                                    ConvertFunction =
                                    (UFunction*)
                                    candidate;
                            }
                        }
                    }

                    //
                    // Derive the Kismet CDO directly:
                    //
                    // Conv function
                    //   -> OuterPrivate UClass
                    //   -> DefaultObject
                    //
                    if (!g_JasonAICache.
                        KismetDefault &&
                        g_JasonAICache.
                        ConvertFunction)
                    {
                        UObject* outer =
                            (UObject*)
                            g_JasonAICache.
                            ConvertFunction->
                            OuterPrivate;

                        if (outer &&
                            Memory::IsReadable(
                                outer,
                                sizeof(UClass)) &&
                            JasonAISafeName(
                                outer
                            ) ==
                            "KismetSystemLibrary")
                        {
                            UClass* libraryClass =
                                (UClass*)outer;

                            UObject* defaultObject =
                                libraryClass->
                                DefaultObject;

                            if (defaultObject &&
                                Memory::IsReadable(
                                    defaultObject,
                                    sizeof(UObject)) &&
                                JasonAISafeName(
                                    defaultObject
                                ) ==
                                "Default__KismetSystemLibrary")
                            {
                                g_JasonAICache.
                                    KismetDefault =
                                    defaultObject;
                            }
                        }
                    }

                    //
                    // GObjects 86196:
                    // SpawnAIFromClass
                    //
                    if (!g_JasonAICache.
                        SpawnFunction)
                    {
                        UObject* candidate =
                            findExpectedNearIndex(
                                86196,
                                "SpawnAIFromClass"
                            );

                        if (candidate &&
                            candidate->Class &&
                            Memory::IsReadable(
                                candidate->Class,
                                sizeof(UObject)) &&
                            JasonAISafeName(
                                (UObject*)
                                candidate->Class
                            ) ==
                            "Function")
                        {
                            UObject* outer =
                                candidate->
                                OuterPrivate;

                            if (outer &&
                                Memory::IsReadable(
                                    outer,
                                    sizeof(UClass)) &&
                                JasonAISafeName(
                                    outer
                                ) ==
                                "AIBlueprintHelperLibrary")
                            {
                                g_JasonAICache.
                                    SpawnFunction =
                                    (UFunction*)
                                    candidate;
                            }
                        }
                    }

                    //
                    // Derive the AI helper CDO directly:
                    //
                    // SpawnAIFromClass
                    //   -> OuterPrivate UClass
                    //   -> DefaultObject
                    //
                    if (!g_JasonAICache.
                        AIBlueprintDefault &&
                        g_JasonAICache.
                        SpawnFunction)
                    {
                        UObject* outer =
                            (UObject*)
                            g_JasonAICache.
                            SpawnFunction->
                            OuterPrivate;

                        if (outer &&
                            Memory::IsReadable(
                                outer,
                                sizeof(UClass)) &&
                            JasonAISafeName(
                                outer
                            ) ==
                            "AIBlueprintHelperLibrary")
                        {
                            UClass* libraryClass =
                                (UClass*)outer;

                            UObject* defaultObject =
                                libraryClass->
                                DefaultObject;

                            if (defaultObject &&
                                Memory::IsReadable(
                                    defaultObject,
                                    sizeof(UObject)) &&
                                JasonAISafeName(
                                    defaultObject
                                ) ==
                                "Default__AIBlueprintHelperLibrary")
                            {
                                g_JasonAICache.
                                    AIBlueprintDefault =
                                    defaultObject;
                            }
                        }
                    }
                }
            }
        }
    }

    //
        //
        //
    // Incrementally locate the native
    // SCKillerAIController UClass.
    //
    //
    if (sandboxMode &&
        !g_JasonAICache.
        KillerAIControllerClass &&
        g_JasonAICache.
        GObjectsScanIndex >= 0)
    {
        constexpr uintptr_t
            GOBJECTS_OFFSET =
            0x030F6EF0;

        HMODULE module =
            GetModuleHandle(nullptr);

        if (module)
        {
            uintptr_t gObjectsAddress =
                (uintptr_t)module +
                GOBJECTS_OFFSET;

            if (Memory::IsReadable(
                (void*)gObjectsAddress,
                0x10))
            {
                uintptr_t objectsPtr =
                    *(uintptr_t*)
                    (gObjectsAddress + 0x00);

                int32_t objectCount =
                    *(int32_t*)
                    (gObjectsAddress + 0x0C);

                if (objectsPtr &&
                    Memory::IsValidPointer(
                        objectsPtr) &&
                    objectCount > 0 &&
                    objectCount < 20000000)
                {
                    constexpr int32_t
                        ObjectsPerFrame =
                        5000;

                    int32_t startIndex =
                        g_JasonAICache.
                        GObjectsScanIndex;

                    int32_t endIndex =
                        startIndex +
                        ObjectsPerFrame;

                    if (endIndex >
                        objectCount)
                    {
                        endIndex =
                            objectCount;
                    }

                    for (int32_t i =
                        startIndex;
                        i < endIndex;
                        ++i)
                    {
                        uintptr_t itemAddress =
                            objectsPtr +
                            ((uintptr_t)i *
                                sizeof(uintptr_t));

                        if (!Memory::IsReadable(
                            (void*)itemAddress,
                            sizeof(uintptr_t)))
                        {
                            continue;
                        }

                        uintptr_t objectAddress =
                            *(uintptr_t*)
                            itemAddress;

                        if (!objectAddress ||
                            !Memory::IsReadable(
                                (void*)objectAddress,
                                sizeof(UObject)))
                        {
                            continue;
                        }

                        UObject* candidate =
                            (UObject*)
                            objectAddress;

                        std::string candidateName =
                            JasonAISafeName(
                                candidate
                            );

                        //
                        //
                        // Find the native killer controller class.
                        //
                        if (!g_JasonAICache.
                            KillerAIControllerClass &&
                            candidateName ==
                            "SCKillerAIController")
                        {
                            if (candidate->Class &&
                                Memory::IsReadable(
                                    candidate->Class,
                                    sizeof(UObject)) &&
                                JasonAISafeName(
                                    (UObject*)
                                    candidate->Class
                                ) ==
                                "Class")
                            {
                                bool derivesFromAIController =
                                    false;

                                for (UStruct* current =
                                    (UStruct*)
                                    candidate;
                                    current;
                                    current =
                                    current->Super)
                                {
                                    if (!Memory::IsReadable(
                                        current,
                                        sizeof(UStruct)))
                                    {
                                        break;
                                    }

                                    if (JasonAISafeName(
                                        (UObject*)
                                        current
                                    ) ==
                                        "AIController")
                                    {
                                        derivesFromAIController =
                                            true;

                                        break;
                                    }
                                }

                                if (derivesFromAIController)
                                {
                                    g_JasonAICache.
                                        KillerAIControllerClass =
                                        (UClass*)
                                        candidate;

                                    Logger::Success(
                                        "Jason AI killer controller class ready: SCKillerAIController"
                                    );
                                }
                            }
                        }
                        if (g_JasonAICache.
                            KillerAIControllerClass)
                        {
                            break;
                        }
                    }

                    if (!g_JasonAICache.
                        KillerAIControllerClass)
                    {
                        g_JasonAICache.
                            GObjectsScanIndex =
                            endIndex;

                        if (endIndex >=
                            objectCount)
                        {
                            Logger::Debug(
                                "Jason AI cache: SCKillerAIController class not found"
                            );

                            g_JasonAICache.
                                GObjectsScanIndex =
                                -1;
                        }
                    }
                }
            }
        }
    }

    bool sandboxPiecesReady =
        g_JasonAICache.World ==
        world &&
        g_JasonAICache.SandboxGameMode &&
        g_JasonAICache.KillerStart &&
        g_JasonAICache.HaveZombieSoftClass &&
        g_JasonAICache.BehaviorTree &&
        g_JasonAICache.
        HaveKillerStartLocation;

    const bool fullRoutePiecesReady =
        adoptedCounselorMode &&
        g_JasonAICache.World == world &&
        g_JasonAICache.BehaviorTree &&
        g_JasonAICache.CounselorBehaviorTree &&
        g_JasonAICache.AIBlueprintDefault &&
        g_JasonAICache.SpawnFunction;

    g_JasonAICache.Ready = sandboxMode
        ? (sandboxPiecesReady &&
           g_JasonAICache.KismetDefault &&
           g_JasonAICache.ConvertFunction &&
           g_JasonAICache.AIBlueprintDefault &&
           g_JasonAICache.SpawnFunction &&
           g_JasonAICache.KillerAIControllerClass)
        : fullRoutePiecesReady;

    //
    // If all Sandbox-specific pieces exist
    // but one of the static Unreal objects
    // is still missing, report exactly which.
    //
    if (sandboxPiecesReady &&
        !g_JasonAICache.Ready &&
        !g_JasonAICache.LoggedScanExhausted)
    {
        g_JasonAICache.
            LoggedScanExhausted =
            true;

        Logger::Debug(
            "Jason AI static lookup status:"
        );

        Logger::Debug(
            std::string(
                "  KismetDefault: "
            ) +
            (g_JasonAICache.KismetDefault ?
                "YES" : "NO")
        );

        Logger::Debug(
            std::string(
                "  ConvertFunction: "
            ) +
            (g_JasonAICache.ConvertFunction ?
                "YES" : "NO")
        );

        Logger::Debug(
            std::string(
                "  AIBlueprintDefault: "
            ) +
            (g_JasonAICache.AIBlueprintDefault ?
                "YES" : "NO")
        );

        Logger::Debug(
            std::string(
                "  SpawnFunction: "
            ) +
            (g_JasonAICache.SpawnFunction ?
                "YES" : "NO")
        );
    }

    if (g_JasonAICache.Ready &&
        !g_JasonAICache.LoggedReady)
    {
        g_JasonAICache.LoggedReady =
            true;

        Logger::Success(
            "Jason AI resources ready"
        );

        Logger::Debug(
            "Jason AI spawn point: " +
            std::to_string(
                g_JasonAICache.
                KillerStartLocation.X
            ) +
            ", " +
            std::to_string(
                g_JasonAICache.
                KillerStartLocation.Y
            ) +
            ", " +
            std::to_string(
                g_JasonAICache.
                KillerStartLocation.Z
            )
        );
    }
}

static bool SpawnJasonZombieAIOnGameThread()
{
    if (!g_JasonAICache.Ready)
    {
        Logger::Debug(
            "Jason AI spawn: resources not ready"
        );

        return false;
    }

    UWorld* world =
        Engine::GetWorld();

    if (!world ||
        world != g_JasonAICache.World)
    {
        Logger::Debug(
            "Jason AI spawn: cached world changed"
        );

        return false;
    }

    auto controller =
        Engine::GetLocalPlayerController();

    if (!controller ||
        !controller->AcknowledgedPawn ||
        !controller->Class)
    {
        Logger::Debug(
            "Jason AI spawn: controller unavailable"
        );

        return false;
    }

    std::string controllerClass =
        JasonAISafeName(
            (UObject*)controller->Class
        );

    const bool sandboxMode =
        controllerClass.find("Sandbox") != std::string::npos;

    // A counselor-route match already owns one fully enhanced Jason through
    // the frozen bridge. Later F1 presses request lightweight combat-only
    // reinforcements and must not replace that proven singleton AI state.
    const bool combatOnlyReinforcement =
        g_JasonAIState.Active &&
        g_JasonAIState.Jason &&
        Memory::IsReadable(g_JasonAIState.Jason, sizeof(UObject));

    if (!sandboxMode && !combatOnlyReinforcement)
    {
        Logger::Debug(
            "Jason AI spawn blocked: neither Sandbox nor adopted counselor route"
        );

        return false;
    }

    AActor* localPawn =
        (AActor*)
        controller->AcknowledgedPawn;

    if (!IsCounselorOrHero(localPawn))
    {
        Logger::Debug(
            "Jason AI spawn blocked: local player is not counselor/hero"
        );

        return false;
    }

    if ((sandboxMode &&
         (!Memory::IsReadable(
              g_JasonAICache.KismetDefault,
              sizeof(UObject)) ||
          !Memory::IsReadable(
              g_JasonAICache.ConvertFunction,
              sizeof(UFunction)))) ||
        !Memory::IsReadable(
        g_JasonAICache.AIBlueprintDefault,
        sizeof(UObject)) ||
        !Memory::IsReadable(
            g_JasonAICache.SpawnFunction,
            sizeof(UFunction)) ||
        !Memory::IsReadable(
            g_JasonAICache.BehaviorTree,
            sizeof(UObject)))
    {
        Logger::Debug(
            "Jason AI spawn: cached resource unreadable"
        );

        return false;
    }

    // Sandbox resolves the requested zombie soft class. The full counselor
    // route instead clones the already selected/adopted Jason class so F1
    // preserves the user's Jason choice and does not depend on Sandbox data.
    struct ResolveParams
    {
        uint8_t SoftClass[40];
        UClass* ReturnValue;
    };

    static_assert(
        sizeof(ResolveParams) == 48,
        "ResolveParams must be 48 bytes"
        );

    ResolveParams resolveParams{};
    UClass* jasonClass = nullptr;
    if (sandboxMode)
    {
        for (int i = 0; i < 40; ++i)
            resolveParams.SoftClass[i] = g_JasonAICache.ZombieSoftClass[i];

        Logger::Debug("Jason AI spawn: resolving Jason_Zombie_C");
        const bool resolveOK = SafeProcessEventCall(
            (uintptr_t)g_JasonAICache.KismetDefault,
            g_JasonAICache.KismetDefault,
            g_JasonAICache.ConvertFunction,
            &resolveParams);
        if (!resolveOK || !resolveParams.ReturnValue)
        {
            Logger::Debug("Jason AI spawn: class resolve failed");
            return false;
        }
        jasonClass = resolveParams.ReturnValue;
    }
    else
    {
        jasonClass = g_JasonAIState.Jason
            ? g_JasonAIState.Jason->Class
            : nullptr;
    }

    if (!Memory::IsReadable(
        jasonClass,
        sizeof(UClass)))
    {
        Logger::Debug(
            "Jason AI spawn: resolved class unreadable"
        );

        return false;
    }

    std::string resolvedName =
        JasonAISafeName(
            (UObject*)jasonClass
        );

    Logger::Success(
        std::string(sandboxMode
            ? "Jason AI class resolved: "
            : "F1 counselor-route Jason class reused: ") +
        resolvedName);

    if (sandboxMode && resolvedName !=
        "Jason_Zombie_C")
    {
        Logger::Debug(
            "Jason AI spawn: unexpected resolved class"
        );

        return false;
    }

    //
    // Exact reflected SpawnAIFromClass layout:
    //
    // +0   WorldContextObject
    // +8   PawnClass
    // +16  BehaviorTree
    // +24  FVector Location
    // +36  FRotator Rotation
    // +48  bool bNoCollisionFail
    // +56  ReturnValue
    //
    struct Rotation3
    {
        float Pitch;
        float Yaw;
        float Roll;
    };

    struct SpawnAIParams
    {
        UObject* WorldContextObject;
        UClass* PawnClass;
        UObject* BehaviorTree;

        FVector Location;
        Rotation3 Rotation;

        bool bNoCollisionFail;
        uint8_t Padding[7];

        AActor* ReturnValue;
    };

    static_assert(
        sizeof(SpawnAIParams) == 64,
        "SpawnAIParams must be 64 bytes"
        );

    SpawnAIParams spawnParams{};

    spawnParams.WorldContextObject =
        (UObject*)world;

    spawnParams.PawnClass =
        jasonClass;

    spawnParams.BehaviorTree =
        g_JasonAICache.BehaviorTree;

    spawnParams.Location =
        g_JasonAICache.
        KillerStartLocation;

    if (combatOnlyReinforcement)
    {
        void** localRoot = reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(localPawn) +
            Offsets::Actor_RootComponent);
        if (Memory::IsReadable(localRoot, sizeof(void*)) && *localRoot)
        {
            FVector* localLocation = reinterpret_cast<FVector*>(
                reinterpret_cast<uintptr_t>(*localRoot) +
                Offsets::Scene_ComponentToWorld +
                Offsets::FTransform_Translation);
            FVector reinforcementLocation{};
            if (Memory::IsReadable(localLocation, sizeof(FVector)) &&
                FindSafeCounselorSpawnLocation(
                    *localLocation,
                    g_AdditionalCombatJasonCount.load(),
                    reinforcementLocation))
            {
                spawnParams.Location = reinforcementLocation;
            }
        }
    }

    spawnParams.Rotation.Pitch =
        0.0f;

    spawnParams.Rotation.Yaw =
        0.0f;

    spawnParams.Rotation.Roll =
        0.0f;

    spawnParams.bNoCollisionFail =
        true;

    //
    Logger::Debug(
        "Jason AI spawn: calling SpawnAIFromClass"
    );

    bool spawnOK =
        SafeProcessEventCall(
            (uintptr_t)
            g_JasonAICache.
            AIBlueprintDefault,
            g_JasonAICache.
            AIBlueprintDefault,
            g_JasonAICache.
            SpawnFunction,
            &spawnParams
        );

    if (!spawnOK)
    {
        Logger::Debug(
            "Jason AI spawn: ProcessEvent failed"
        );

        return false;
    }

    if (!spawnParams.ReturnValue)
    {
        Logger::Debug(
            "Jason AI spawn: SpawnAIFromClass returned NULL"
        );

        return false;
    }

    if (!Memory::IsReadable(
        spawnParams.ReturnValue,
        sizeof(UObject)))
    {
        Logger::Debug(
            "Jason AI spawn: returned pawn unreadable"
        );

        return false;
    }

    std::string spawnedName =
        JasonAISafeName(
            (UObject*)
            spawnParams.ReturnValue
        );

    std::string spawnedClass;

    if (spawnParams.ReturnValue->Class &&
        Memory::IsReadable(
            spawnParams.ReturnValue->Class,
            sizeof(UObject)))
    {
        spawnedClass =
            JasonAISafeName(
                (UObject*)
                spawnParams.ReturnValue->Class
            );
    }

    Logger::Success(
        "Jason AI spawned: " +
        spawnedName +
        " | class=" +
        spawnedClass
    );

    //
 // Replace the generic AIController created by
 // SpawnAIFromClass with the game's actual
 // SCKillerAIController.
 //
 // Reflected offsets confirmed in this build:
 // AIControllerClass = 0x380
 // Controller        = 0x3A0
 //
    constexpr uintptr_t
        Offset_PawnAIControllerClass =
        0x380;

    constexpr uintptr_t
        Offset_PawnController =
        0x3A0;

    UObject** spawnedControllerPtr =
        (UObject**)
        ((uintptr_t)
            spawnParams.ReturnValue +
            Offset_PawnController);

    UClass** aiControllerClassPtr =
        (UClass**)
        ((uintptr_t)
            spawnParams.ReturnValue +
            Offset_PawnAIControllerClass);

    UClass* desiredKillerControllerClass =
        g_JasonAICache.KillerAIControllerClass;
    if (combatOnlyReinforcement &&
        g_JasonAIState.Controller &&
        Memory::IsReadable(g_JasonAIState.Controller, sizeof(UObject)) &&
        g_JasonAIState.Controller->Class &&
        Memory::IsReadable(
            g_JasonAIState.Controller->Class,
            sizeof(UClass)))
    {
        // The packed counselor route deliberately avoids the Sandbox-wide
        // SCKillerAIController class scan. Reuse the class of the already-live
        // primary Jason controller instead; this is authoritative and free.
        desiredKillerControllerClass = g_JasonAIState.Controller->Class;
    }

    if (!Memory::IsReadable(
        spawnedControllerPtr,
        sizeof(UObject*)) ||
        !*spawnedControllerPtr ||
        !Memory::IsReadable(
            *spawnedControllerPtr,
            sizeof(UObject)))
    {
        Logger::Debug(
            "Jason AI handoff: initial controller unavailable"
        );

        return false;
    }

    if (!Memory::IsReadable(
        aiControllerClassPtr,
        sizeof(UClass*)) ||
        !desiredKillerControllerClass ||
        !Memory::IsReadable(
            desiredKillerControllerClass,
            sizeof(UClass)))
    {
        Logger::Debug(
            "Jason AI handoff: controller class unavailable"
        );

        return false;
    }

    UObject* genericController =
        *spawnedControllerPtr;

    std::string genericControllerClass;

    if (genericController->Class &&
        Memory::IsReadable(
            genericController->Class,
            sizeof(UObject)))
    {
        genericControllerClass =
            JasonAISafeName(
                (UObject*)
                genericController->Class
            );
    }

    Logger::Debug(
        "Jason AI handoff: initial controller=" +
        genericControllerClass
    );

    UFunction* unPossessFunction =
        FindFunctionInHierarchyByName(
            genericController->Class,
            "UnPossess"
        );

    UFunction* spawnDefaultControllerFunction =
        FindFunctionInHierarchyByName(
            spawnParams.ReturnValue->Class,
            "SpawnDefaultController"
        );

    if (!unPossessFunction ||
        !spawnDefaultControllerFunction)
    {
        Logger::Debug(
            "Jason AI handoff: required function missing"
        );

        return false;
    }

    //
    // Make sure the Jason instance's
    // AIControllerClass property is writable
    // before detaching his current controller.
    //
    MEMORY_BASIC_INFORMATION mbi{};

    SIZE_T queryResult =
        VirtualQuery(
            aiControllerClassPtr,
            &mbi,
            sizeof(mbi)
        );

    DWORD pageProtection =
        mbi.Protect & 0xFF;

    bool pageWritable =
        queryResult != 0 &&
        mbi.State == MEM_COMMIT &&
        (mbi.Protect & PAGE_GUARD) == 0 &&
        (
            pageProtection ==
            PAGE_READWRITE ||
            pageProtection ==
            PAGE_WRITECOPY ||
            pageProtection ==
            PAGE_EXECUTE_READWRITE ||
            pageProtection ==
            PAGE_EXECUTE_WRITECOPY
            );

    if (!pageWritable)
    {
        Logger::Debug(
            "Jason AI handoff: instance AIControllerClass not writable"
        );

        return false;
    }

    UClass* originalAIControllerClass =
        *aiControllerClassPtr;

    std::string originalAIControllerClassName =
        originalAIControllerClass ?
        JasonAISafeName(
            (UObject*)
            originalAIControllerClass
        ) :
        "NULL";

    //
    // First detach the generic AIController.
    //
    Logger::Debug(
        "Jason AI handoff: removing generic AIController"
    );

    bool unPossessOK =
        SafeProcessEventCall(
            (uintptr_t)
            genericController,
            genericController,
            unPossessFunction,
            nullptr
        );

    if (!unPossessOK)
    {
        Logger::Debug(
            "Jason AI handoff: UnPossess call failed"
        );

        return false;
    }

    //
    // Controller should now be null on Jason.
    //
    if (*spawnedControllerPtr)
    {
        std::string remainingClass;

        UObject* remainingController =
            *spawnedControllerPtr;

        if (remainingController &&
            Memory::IsReadable(
                remainingController,
                sizeof(UObject)) &&
            remainingController->Class &&
            Memory::IsReadable(
                remainingController->Class,
                sizeof(UObject)))
        {
            remainingClass =
                JasonAISafeName(
                    (UObject*)
                    remainingController->Class
                );
        }

        Logger::Debug(
            "Jason AI handoff: controller still attached after UnPossess: " +
            remainingClass
        );

        return false;
    }

    Logger::Success(
        "Jason AI handoff: generic controller detached"
    );

    //
    // Unlike the previous CDO experiment, patch
    // the LIVE Jason instance itself.
    //
    *aiControllerClassPtr =
        desiredKillerControllerClass;

    if (*aiControllerClassPtr !=
        desiredKillerControllerClass)
    {
        Logger::Debug(
            "Jason AI handoff: instance controller-class patch failed"
        );

        *aiControllerClassPtr =
            originalAIControllerClass;

        return false;
    }

    Logger::Debug(
        "Jason AI handoff: instance AIControllerClass " +
        originalAIControllerClassName +
        " -> SCKillerAIController"
    );

    //
// OfflineBots does NOT use Resurrected's
// SCKillerAIController OnPossess override.
//
// Temporarily replace this class's +0x678
// vtable slot with the base AAIController
// OnPossess implementation for the one
// SpawnDefaultController call below.
//
    Logger::Debug(
        "Jason AI handoff: bypassing stock Killer OnPossess"
    );

    HMODULE gameModule =
        GetModuleHandle(nullptr);

    UObject* killerControllerCDO =
        desiredKillerControllerClass
            ? desiredKillerControllerClass->DefaultObject
            : nullptr;

    if (!gameModule ||
        !killerControllerCDO ||
        !Memory::IsReadable(
            killerControllerCDO,
            sizeof(UObject)))
    {
        Logger::Debug(
            "Jason AI handoff: Killer controller CDO unavailable"
        );

        *aiControllerClassPtr =
            originalAIControllerClass;

        return false;
    }

    uintptr_t* killerVtable =
        *(uintptr_t**)killerControllerCDO;

    constexpr uintptr_t
        Offset_KillerOnPossessSlot =
        0x678;

    uintptr_t* killerOnPossessSlot =
        killerVtable ?
        (uintptr_t*)
        ((uintptr_t)killerVtable +
            Offset_KillerOnPossessSlot) :
        nullptr;

    //
    // Resurrected build confirmed in Ghidra:
    //
    // SCKillerAIController::OnPossess
    //   RVA 0x00327190
    //
    // AAIController::OnPossess
    //   RVA 0x0112F3F0
    //
    constexpr uintptr_t
        RVA_KillerOnPossess =
        0x00327190;

    constexpr uintptr_t
        RVA_BaseAIOnPossess =
        0x0112F3F0;

    uintptr_t expectedKillerOnPossess =
        (uintptr_t)gameModule +
        RVA_KillerOnPossess;

    uintptr_t baseAIOnPossess =
        (uintptr_t)gameModule +
        RVA_BaseAIOnPossess;

    if (!killerOnPossessSlot ||
        !Memory::IsReadable(
            killerOnPossessSlot,
            sizeof(uintptr_t)) ||
        *killerOnPossessSlot !=
        expectedKillerOnPossess ||
        !Memory::IsReadable(
            (void*)baseAIOnPossess,
            1))
    {
        Logger::Debug(
            "Jason AI handoff: OnPossess slot verification failed"
        );

        *aiControllerClassPtr =
            originalAIControllerClass;

        return false;
    }

    //
// OfflineBots creates SCKillerAIController with the
// ordinary AIModule PathFollowingComponent.
//
// Resurrected's SCKillerAIController constructor instead
// executes this instruction:
//
//   1402FE340  E8 EB 58 F9 FF
//              CALL FUN_140293C30
//
// which installs:
//
//   "PathFollowingComponent"
//       -> SCCrowdFollowingComponent
//
// Disable ONLY that call while SpawnDefaultController
// constructs our AI Jason controller.
//
    constexpr uintptr_t
        RVA_KillerCrowdFollowerOverrideCall =
        0x002FE340;

    uint8_t* crowdOverrideCall =
        (uint8_t*)
        ((uintptr_t)gameModule +
            RVA_KillerCrowdFollowerOverrideCall);

    bool crowdCallMatches =
        Memory::IsReadable(
            crowdOverrideCall,
            5) &&
        crowdOverrideCall[0] == 0xE8 &&
        crowdOverrideCall[1] == 0xEB &&
        crowdOverrideCall[2] == 0x58 &&
        crowdOverrideCall[3] == 0xF9 &&
        crowdOverrideCall[4] == 0xFF;

    if (!crowdCallMatches)
    {
        Logger::Debug(
            "Jason AI handoff: crowd follower override CALL verification failed"
        );

        *aiControllerClassPtr =
            originalAIControllerClass;

        return false;
    }

    DWORD oldCrowdProtection = 0;

    if (!VirtualProtect(
        crowdOverrideCall,
        5,
        PAGE_EXECUTE_READWRITE,
        &oldCrowdProtection))
    {
        Logger::Debug(
            "Jason AI handoff: could not unlock crowd follower override CALL"
        );

        *aiControllerClassPtr =
            originalAIControllerClass;

        return false;
    }

    const uint8_t originalCrowdCall[5] =
    {
        0xE8,
        0xEB,
        0x58,
        0xF9,
        0xFF
    };

    for (int i = 0; i < 5; ++i)
    {
        crowdOverrideCall[i] =
            0x90;
    }

    FlushInstructionCache(
        GetCurrentProcess(),
        crowdOverrideCall,
        5
    );

    Logger::Debug(
        "Jason AI handoff: SCCrowdFollowingComponent override bypass active"
    );

    //
    // Also retain our existing OfflineBots-style
    // OnPossess bypass.
    //
    DWORD oldPossessProtection = 0;

    if (!VirtualProtect(
        killerOnPossessSlot,
        sizeof(uintptr_t),
        PAGE_READWRITE,
        &oldPossessProtection))
    {
        for (int i = 0; i < 5; ++i)
        {
            crowdOverrideCall[i] =
                originalCrowdCall[i];
        }

        FlushInstructionCache(
            GetCurrentProcess(),
            crowdOverrideCall,
            5
        );

        DWORD unusedCrowdProtection = 0;

        VirtualProtect(
            crowdOverrideCall,
            5,
            oldCrowdProtection,
            &unusedCrowdProtection
        );

        Logger::Debug(
            "Jason AI handoff: could not unlock OnPossess slot"
        );

        *aiControllerClassPtr =
            originalAIControllerClass;

        return false;
    }

    uintptr_t originalKillerOnPossess =
        *killerOnPossessSlot;

    *killerOnPossessSlot =
        baseAIOnPossess;

    Logger::Debug(
        "Jason AI handoff: stock Killer OnPossess bypass active"
    );

    bool spawnControllerOK =
        SafeProcessEventCall(
            (uintptr_t)
            spawnParams.ReturnValue,
            spawnParams.ReturnValue,
            spawnDefaultControllerFunction,
            nullptr
        );

    //
    // Restore OnPossess immediately.
    //
    *killerOnPossessSlot =
        originalKillerOnPossess;

    DWORD unusedPossessProtection = 0;

    VirtualProtect(
        killerOnPossessSlot,
        sizeof(uintptr_t),
        oldPossessProtection,
        &unusedPossessProtection
    );

    Logger::Debug(
        "Jason AI handoff: stock Killer OnPossess restored"
    );

    //
    // Restore the Resurrected constructor instruction
    // immediately after this one controller is created.
    //
    for (int i = 0; i < 5; ++i)
    {
        crowdOverrideCall[i] =
            originalCrowdCall[i];
    }

    FlushInstructionCache(
        GetCurrentProcess(),
        crowdOverrideCall,
        5
    );

    DWORD unusedCrowdProtection = 0;

    VirtualProtect(
        crowdOverrideCall,
        5,
        oldCrowdProtection,
        &unusedCrowdProtection
    );

    Logger::Debug(
        "Jason AI handoff: SCCrowdFollowingComponent override restored"
    );

    //
    // Restore Jason's instance setting immediately.
    // The newly-created controller remains attached.
    //
    *aiControllerClassPtr =
        originalAIControllerClass;

    Logger::Debug(
        "Jason AI handoff: instance AIControllerClass restored to " +
        originalAIControllerClassName
    );

    if (!spawnControllerOK)
    {
        Logger::Debug(
            "Jason AI handoff: SpawnDefaultController call failed"
        );

        return false;
    }

    //
    // Confirm the Pawn now belongs to the game's
    // real killer AI controller.
    //
    if (!*spawnedControllerPtr ||
        !Memory::IsReadable(
            *spawnedControllerPtr,
            sizeof(UObject)))
    {
        Logger::Debug(
            "Jason AI handoff: replacement controller not created"
        );

        return false;
    }

    UObject* killerController =
        *spawnedControllerPtr;

    std::string killerControllerName =
        JasonAISafeName(
            killerController
        );

    std::string killerControllerClassName;

    if (killerController->Class &&
        Memory::IsReadable(
            killerController->Class,
            sizeof(UObject)))
    {
        killerControllerClassName =
            JasonAISafeName(
                (UObject*)
                killerController->Class
            );
    }

    Logger::Success(
        "Jason AI replacement controller: " +
        killerControllerName +
        " | class=" +
        killerControllerClassName
    );

    if (killerControllerClassName !=
        "SCKillerAIController")
    {
        Logger::Debug(
            "Jason AI handoff: replacement is not SCKillerAIController"
        );

        return false;
    }

    Logger::Success(
        "Jason AI handoff: SCKillerAIController confirmed"
    );

    //
// Match the inherited flags explicitly set
// by the working OfflineBots killer controller.
//
    uint8_t* offlineBotsActorFlags =
        (uint8_t*)
        ((uintptr_t)killerController +
            0x34);

    uint8_t* offlineBotsAIFlags =
        (uint8_t*)
        ((uintptr_t)killerController +
            0x408);

    if (Memory::IsReadable(
        offlineBotsActorFlags,
        1) &&
        Memory::IsReadable(
            offlineBotsAIFlags,
            1))
    {
        *offlineBotsActorFlags |=
            0x02;

        *offlineBotsAIFlags |=
            0x10;

        Logger::Success(
            "Jason AI OfflineBots inherited flags applied"
        );
    }

    // Reinforcements use the same proven OfflineBots controller construction
    // as the primary Jason: base AAIController OnPossess plus the ordinary
    // AIModule path follower.  The stock Resurrected crowd follower accepts
    // MoveTo requests here but intermittently never advances the spawned pawn,
    // which is why extra F1 Jasons appeared frozen.  Keep the singleton AI
    // state untouched and hand only this verified controller to the lightweight
    // reinforcement driver.
    if (combatOnlyReinforcement)
    {
        const int32_t count =
            g_AdditionalCombatJasonCount.fetch_add(1) + 1;
        FrozenBridgeRegisterAdditionalJason(
            spawnParams.ReturnValue,
            killerController);
        Logger::Success(
            "F1 additional combat-only Jason activated with OfflineBots path controller | count=" +
            std::to_string(count));
        return true;
    }

    //
// OfflineBots-style DLL AI state is now live.
//
    g_JasonAIState.World =
        world;

    g_JasonAIState.Jason =
        spawnParams.ReturnValue;

    g_JasonAIState.Controller =
        killerController;

    g_JasonAIState.Target =
        nullptr;

    ULONGLONG aiStartNow =
        GetTickCount64();

    g_JasonAIState.InitialTeleportAt =
        aiStartNow + 2000;

    // Features42 opening strategy begins at the same point where the old
    // one-shot counselor teleport used to fire. Normal chase/combat is held
    // until the phone/car trap pass finishes (or safely gives up).
    g_JasonAIState.StartupTrapSetupStartedAt =
        aiStartNow;

    g_JasonAIState.StartupTrapNextActionAt =
        aiStartNow + 2000;

    g_JasonAIState.StartupTrapSetupActive =
        true;

    // Trap setup suppresses knife/combat anyway; this is only a conservative
    // fallback if the strategic opening exits early.
    g_JasonAIState.NextKnifeAttemptAt =
        aiStartNow + 12000;

    g_JasonAIState.InitialTeleportAttempted =
        false;

    g_JasonAIState.PathLocked =
        false;

    g_JasonAIState.PathLockUntil =
        0;

    g_JasonAIState.NextStuckCheckAt =
        GetTickCount64() + 1000;

    g_JasonAIState.LastAcceptedMoveAt =
        0;

    g_JasonAIState.ConsecutiveStuckChecks =
        0;

    g_JasonAIState.ConsecutiveNearTargetStuckDeferrals =
        0;

    
    g_JasonAIState.HaveLastLocation =
        false;

    g_JasonAIState.Active =
        true;

    Logger::Success(
        "Jason AI OfflineBots state active"
    );

    if (!InstallOfflineBotsControllerTickHook(
        killerController))
    {
        Logger::Debug(
            "Jason AI OfflineBots +0x410 Tick hook failed"
        );

        g_JasonAIState.Active =
            false;

        return false;
    }

    //
    // Find the live NavigationSystem directly
    // from UWorld's reflected object properties.
    //
    UObject* navigationSystemInstance =
        nullptr;

    int32_t navigationSystemOffset =
        -1;

    std::string navigationSystemProperty;

    if (world->Class &&
        Memory::IsReadable(
            world->Class,
            sizeof(UStruct)))
    {
        for (UStruct* current =
            (UStruct*)world->Class;
            current;
            current = current->Super)
        {
            if (!Memory::IsReadable(
                current,
                sizeof(UStruct)))
            {
                break;
            }

            UField* field =
                current->Children;

            int guard = 0;

            while (field &&
                guard++ < 2048)
            {
                if (!Memory::IsReadable(
                    field,
                    sizeof(UField)))
                {
                    break;
                }

                UObject* fieldClass =
                    (UObject*)
                    field->ClassPrivate;

                std::string
                    fieldClassName;

                if (fieldClass &&
                    Memory::IsReadable(
                        fieldClass,
                        sizeof(UObject)))
                {
                    fieldClassName =
                        JasonAISafeName(
                            fieldClass
                        );
                }

                if (fieldClassName ==
                    "ObjectProperty")
                {
                    UPropertyLite* property =
                        (UPropertyLite*)field;

                    int32_t offset =
                        property->
                        Offset_Internal;

                    if (offset > 0 &&
                        offset < 0x10000)
                    {
                        UObject** objectPtr =
                            (UObject**)
                            ((uintptr_t)world +
                                offset);

                        if (Memory::IsReadable(
                            objectPtr,
                            sizeof(UObject*)) &&
                            *objectPtr &&
                            Memory::IsReadable(
                                *objectPtr,
                                sizeof(UObject)))
                        {
                            UObject* object =
                                *objectPtr;

                            if (object->Class &&
                                Memory::IsReadable(
                                    object->Class,
                                    sizeof(UObject)))
                            {
                                std::string objectClass =
                                    JasonAISafeName(
                                        (UObject*)
                                        object->Class
                                    );

                                if (objectClass.find(
                                    "NavigationSystem") !=
                                    std::string::npos)
                                {
                                    navigationSystemInstance =
                                        object;

                                    navigationSystemOffset =
                                        offset;

                                    navigationSystemProperty =
                                        JasonAISafeName(
                                            (UObject*)field
                                        );

                                    break;
                                }
                            }
                        }
                    }
                }

                field = field->Next;
            }

            if (navigationSystemInstance)
                break;
        }
    }

    if (!navigationSystemInstance)
    {
        Logger::Debug(
            "Jason AI UWorld NavigationSystem: NOT FOUND"
        );
    }
    else
    {
        std::string navigationClass =
            JasonAISafeName(
                (UObject*)
                navigationSystemInstance->Class
            );

        Logger::Success(
            "Jason AI UWorld NavigationSystem FOUND | property=" +
            navigationSystemProperty +
            " | class=" +
            navigationClass +
            " | offset=" +
            std::to_string(
                navigationSystemOffset
            )
        );

        //
// Reflect the NavigationSystem point-projection
// function before we attempt to call it.
//
        UFunction* projectPointFunction =
            FindFunctionInHierarchyByName(
                navigationSystemInstance->Class,
                "K2_ProjectPointToNavigation"
            );

        const char* projectPointName =
            "K2_ProjectPointToNavigation";

        if (!projectPointFunction)
        {
            projectPointFunction =
                FindFunctionInHierarchyByName(
                    navigationSystemInstance->Class,
                    "ProjectPointToNavigation"
                );

            projectPointName =
                "ProjectPointToNavigation";
        }

        Logger::Debug(
            std::string(
                "Jason AI nav projection function "
            ) +
            projectPointName +
            ": " +
            (projectPointFunction ?
                "FOUND" :
                "NOT FOUND")
        );

        if (projectPointFunction &&
            Memory::IsReadable(
                projectPointFunction,
                sizeof(UFunction)))
        {
            constexpr uint64_t
                CPF_Parm =
                0x80ULL;

            UField* projectField =
                projectPointFunction->Children;

            int projectGuard =
                0;

            int projectParamCount =
                0;

            while (projectField &&
                projectGuard++ < 128)
            {
                if (!Memory::IsReadable(
                    projectField,
                    sizeof(UField)))
                {
                    break;
                }

                std::string projectFieldType =
                    JasonAISafeName(
                        (UObject*)
                        projectField->
                        ClassPrivate
                    );

                if (projectFieldType.find(
                    "Property") !=
                    std::string::npos)
                {
                    UPropertyLite* property =
                        (UPropertyLite*)
                        projectField;

                    if (Memory::IsReadable(
                        property,
                        sizeof(UPropertyLite)) &&
                        (property->
                            PropertyFlags &
                            CPF_Parm))
                    {
                        std::string propertyName =
                            JasonAISafeName(
                                (UObject*)
                                projectField
                            );

                        Logger::Debug(
                            "Jason AI nav projection param " +
                            propertyName +
                            " | type=" +
                            projectFieldType +
                            " | offset=" +
                            std::to_string(
                                property->
                                Offset_Internal
                            ) +
                            " | size=" +
                            std::to_string(
                                property->
                                ElementSize
                            ) +
                            " | flags=" +
                            std::to_string(
                                property->
                                PropertyFlags
                            )
                        );

                        ++projectParamCount;
                    }
                }

                projectField =
                    projectField->Next;
            }

            Logger::Debug(
                "Jason AI nav projection parameter count=" +
                std::to_string(
                    projectParamCount
                )
            );
        }

        UFunction*
            registerNavInvokerFunction =
            FindFunctionInHierarchyByName(
                navigationSystemInstance->Class,
                "RegisterNavigationInvoker"
            );

        Logger::Debug(
            std::string(
                "Jason AI RegisterNavigationInvoker: "
            ) +
            (registerNavInvokerFunction ?
                "FOUND" : "NOT FOUND")
        );

        if (registerNavInvokerFunction)
        {
            struct
                RegisterNavigationInvokerParams
            {
                AActor* Invoker;
                float TileGenerationRadius;
                float TileRemovalRadius;
            };

            static_assert(
                sizeof(
                    RegisterNavigationInvokerParams
                    ) == 16,
                "RegisterNavigationInvokerParams must be 16 bytes"
                );

            RegisterNavigationInvokerParams
                navParams{};

            navParams.Invoker =
                spawnParams.ReturnValue;

            navParams.TileGenerationRadius =
                5000.0f;

            navParams.TileRemovalRadius =
                6000.0f;

            Logger::Debug(
                "Jason AI nav: registering Jason as navigation invoker"
            );

            bool registerOK =
                SafeProcessEventCall(
                    (uintptr_t)
                    navigationSystemInstance,
                    navigationSystemInstance,
                    registerNavInvokerFunction,
                    &navParams
                );

            Logger::Success(
                std::string(
                    "Jason AI RegisterNavigationInvoker ProcessEvent="
                ) +
                (registerOK ?
                    "true" : "false")
            );
                }
    }


    auto controllerAfter =
        Engine::GetLocalPlayerController();

    if (controllerAfter &&
        controllerAfter->AcknowledgedPawn ==
        localPawn)
    {
        RegisterJasonAITarget(
            (AActor*)localPawn
        );

        RequestCounselorClassPreloadOnGameThread();

        Logger::Success(
            "Local counselor possession preserved"
        );
    }
    else
    {
        Logger::Debug(
            "Jason AI spawn: local possession changed unexpectedly"
        );
    }

    return true;
}

static bool GetJasonAIActorLocation(
    AActor* actor,
    FVector& outLocation)
{
    if (!actor ||
        !Memory::IsReadable(
            actor,
            sizeof(UObject)))
    {
        return false;
    }

    void** rootPtr =
        (void**)
        ((uintptr_t)actor +
            Offsets::Actor_RootComponent);

    if (!Memory::IsReadable(
        rootPtr,
        sizeof(void*)) ||
        !*rootPtr)
    {
        return false;
    }

    FVector* location =
        (FVector*)
        ((uintptr_t)(*rootPtr) +
            Offsets::Scene_ComponentToWorld +
            Offsets::FTransform_Translation);

    if (!Memory::IsReadable(
        location,
        sizeof(FVector)))
    {
        return false;
    }

    outLocation =
        *location;

    return true;
}
static AActor* FindNearestJasonAICounselorTarget(
    AActor* jason)
{
    if (!jason ||
        !Memory::IsReadable(
            jason,
            sizeof(UObject)))
    {
        return nullptr;
    }

    FVector jasonLocation{};

    if (!GetJasonAIActorLocation(
        jason,
        jasonLocation))
    {
        return nullptr;
    }

    constexpr uintptr_t
        Offset_PawnController =
        0x3A0;

    ULONGLONG now =
        GetTickCount64();

    AActor* bestTarget =
        nullptr;

    float bestDistanceSquared =
        FLT_MAX;

    for (int32_t i = 0;
        i < g_JasonAITargetCount;
        ++i)
    {
        AActor* candidate =
            g_JasonAITargets[i];

        if (!candidate ||
            candidate == jason ||
            IsJasonAITargetBlocked(
                candidate,
                now) ||
            !Memory::IsReadable(
                candidate,
                sizeof(UObject)))
        {
            continue;
        }

        // Preserve the old cheap corpse/remnant filter:
        // a counselor without a readable controller is ignored.
        UObject** controllerPtr =
            (UObject**)
            ((uintptr_t)candidate +
                Offset_PawnController);

        if (!Memory::IsReadable(
            controllerPtr,
            sizeof(UObject*)) ||
            !*controllerPtr ||
            !Memory::IsReadable(
                *controllerPtr,
                sizeof(UObject)))
        {
            continue;
        }

        FVector candidateLocation{};

        if (!GetJasonAIActorLocation(
            candidate,
            candidateLocation))
        {
            continue;
        }

        float dx =
            candidateLocation.X -
            jasonLocation.X;

        float dy =
            candidateLocation.Y -
            jasonLocation.Y;

        float dz =
            candidateLocation.Z -
            jasonLocation.Z;

        float distanceSquared =
            (dx * dx) +
            (dy * dy) +
            (dz * dz);

        if (!std::isfinite(
            distanceSquared) ||
            distanceSquared < 0.0f)
        {
            continue;
        }

        if (!bestTarget ||
            distanceSquared <
            bestDistanceSquared)
        {
            bestTarget =
                candidate;

            bestDistanceSquared =
                distanceSquared;
        }
    }

    return bestTarget;
}

static bool ProjectJasonAINavPointOnGameThread(
    const FVector& point,
    const FVector& extent,
    FVector& outLocation)
{
    UWorld* world =
        Engine::GetWorld();

    if (!world ||
        !Memory::IsReadable(
            world,
            sizeof(UWorld)))
    {
        return false;
    }

    constexpr uintptr_t
        Offset_WorldNavigationSystem =
        0xE8;

    UObject** navigationSystemPtr =
        (UObject**)
        ((uintptr_t)world +
            Offset_WorldNavigationSystem);

    if (!Memory::IsReadable(
        navigationSystemPtr,
        sizeof(UObject*)) ||
        !*navigationSystemPtr ||
        !Memory::IsReadable(
            *navigationSystemPtr,
            sizeof(UObject)))
    {
        return false;
    }

    UObject* navigationSystem =
        *navigationSystemPtr;

    if (!navigationSystem->Class ||
        !Memory::IsReadable(
            navigationSystem->Class,
            sizeof(UObject)))
    {
        return false;
    }

    UFunction* projectFunction =
        FindFunctionInHierarchyByName(
            navigationSystem->Class,
            "K2_ProjectPointToNavigation"
        );

    if (!projectFunction)
    {
        projectFunction =
            FindFunctionInHierarchyByName(
                navigationSystem->Class,
                "ProjectPointToNavigation"
            );
    }

    if (!projectFunction)
        return false;

    struct ProjectPointParams
    {
        UObject* WorldContextObject;
        FVector Point;
        FVector ProjectedLocation;
        UObject* NavData;
        UClass* FilterClass;
        FVector QueryExtent;
        bool ReturnValue;
        uint8_t Padding[3];
    };

    static_assert(
        sizeof(ProjectPointParams) == 64,
        "ProjectPointParams must be 64 bytes"
        );

    ProjectPointParams params{};

    params.WorldContextObject =
        (UObject*)world;

    params.Point =
        point;

    params.NavData =
        nullptr;

    params.FilterClass =
        nullptr;

    params.QueryExtent =
        extent;

    bool callOK =
        SafeProcessEventCall(
            (uintptr_t)navigationSystem,
            navigationSystem,
            projectFunction,
            &params
        );

    if (!callOK ||
        !params.ReturnValue)
    {
        return false;
    }

    outLocation =
        params.ProjectedLocation;

    return true;
}
static UPropertyLite* FindPropertyInHierarchyByName(
    UClass* cls,
    const char* targetName)
{
    if (!cls || !targetName)
        return nullptr;

    for (UStruct* current = (UStruct*)cls;
        current;
        current = current->Super)
    {
        if (!Memory::IsReadable(
            current,
            sizeof(UStruct)))
        {
            break;
        }

        UField* field = current->Children;
        int guard = 0;

        while (field && guard++ < 2048)
        {
            if (!Memory::IsReadable(
                field,
                sizeof(UField)))
            {
                break;
            }

            UObject* fieldClass =
                (UObject*)field->ClassPrivate;

            if (fieldClass &&
                Memory::IsReadable(
                    fieldClass,
                    sizeof(UObject)))
            {
                std::string fieldType =
                    JasonAISafeName(fieldClass);

                if (fieldType.find(
                    "Property") !=
                    std::string::npos &&
                    JasonAISafeName(
                        (UObject*)field) ==
                    targetName)
                {
                    UPropertyLite* property =
                        (UPropertyLite*)field;

                    if (Memory::IsReadable(
                        property,
                        sizeof(UPropertyLite)))
                    {
                        return property;
                    }
                }
            }

            field = field->Next;
        }
    }

    return nullptr;
}

static bool RequestCounselorClassPreloadOnGameThread()
{
    if (g_CounselorClassPreloadRequested)
        return true;

    AActor* sandboxGameMode =
        g_JasonAICache.SandboxGameMode;

    UObject* kismetDefault =
        g_JasonAICache.KismetDefault;

    if (!sandboxGameMode ||
        !sandboxGameMode->Class ||
        !kismetDefault ||
        !kismetDefault->Class ||
        !Memory::IsReadable(
            sandboxGameMode,
            sizeof(UObject)) ||
        !Memory::IsReadable(
            kismetDefault,
            sizeof(UObject)))
    {
        return false;
    }

    UFunction* loadClassFunction =
        FindFunctionInHierarchyByName(
            kismetDefault->Class,
            "LoadAssetClass"
        );

    UPropertyLite* counselorClassesProperty =
        FindPropertyInHierarchyByName(
            sandboxGameMode->Class,
            "CounselorCharacterClasses"
        );

    if (!loadClassFunction ||
        !counselorClassesProperty ||
        counselorClassesProperty->Offset_Internal <= 0 ||
        counselorClassesProperty->Offset_Internal >= 0x10000)
    {
        return false;
    }

    struct RawArray
    {
        uint8_t* Data;
        int32_t Count;
        int32_t Max;
    };

    RawArray* counselorClasses =
        (RawArray*)
        ((uintptr_t)sandboxGameMode +
            counselorClassesProperty->Offset_Internal);

    if (!Memory::IsReadable(
        counselorClasses,
        sizeof(RawArray)) ||
        !counselorClasses->Data ||
        counselorClasses->Count <= 0 ||
        counselorClasses->Count > 64)
    {
        return false;
    }

    constexpr int32_t SoftClassSize = 40;

    if (!Memory::IsReadable(
        counselorClasses->Data,
        (size_t)counselorClasses->Count *
            SoftClassSize))
    {
        return false;
    }

    int32_t worldContextOffset = -1;
    int32_t assetClassOffset = -1;
    int32_t latentInfoOffset = -1;

    UField* field =
        loadClassFunction->Children;

    int guard = 0;

    while (field && guard++ < 64)
    {
        if (!Memory::IsReadable(
            field,
            sizeof(UField)))
        {
            break;
        }

        UObject* fieldClass =
            (UObject*)field->ClassPrivate;

        if (fieldClass &&
            Memory::IsReadable(
                fieldClass,
                sizeof(UObject)))
        {
            std::string fieldName =
                JasonAISafeName(
                    (UObject*)field);

            UPropertyLite* property =
                (UPropertyLite*)field;

            if (fieldName ==
                "WorldContextObject")
            {
                worldContextOffset =
                    property->Offset_Internal;
            }
            else if (fieldName ==
                "AssetClass")
            {
                assetClassOffset =
                    property->Offset_Internal;
            }
            else if (fieldName ==
                "LatentInfo")
            {
                latentInfoOffset =
                    property->Offset_Internal;
            }
        }

        field = field->Next;
    }

    if (assetClassOffset < 0 ||
        assetClassOffset + SoftClassSize > 0x100)
    {
        Logger::Debug(
            "Counselor AI class preload: LoadAssetClass parameters not resolved"
        );
        return false;
    }

    auto localController =
        Engine::GetLocalPlayerController();

    int32_t requested = 0;

    for (int32_t index = 0;
        index < counselorClasses->Count;
        ++index)
    {
        alignas(16) uint8_t params[0x100]{};

        if (worldContextOffset >= 0 &&
            worldContextOffset +
                (int32_t)sizeof(UObject*) <=
                (int32_t)sizeof(params))
        {
            *(UObject**)(params +
                worldContextOffset) =
                (UObject*)Engine::GetWorld();
        }

        uint8_t* softClass =
            counselorClasses->Data +
            ((uintptr_t)index *
                SoftClassSize);

        for (int32_t i = 0;
            i < SoftClassSize;
            ++i)
        {
            params[assetClassOffset + i] =
                softClass[i];
        }

        if (latentInfoOffset >= 0 &&
            latentInfoOffset + 24 <=
                (int32_t)sizeof(params))
        {
            *(int32_t*)(params +
                latentInfoOffset + 0) = 0;

            *(int32_t*)(params +
                latentInfoOffset + 4) =
                0x5C000 + index;

            if (localController)
            {
                *(UObject**)(params +
                    latentInfoOffset + 16) =
                    (UObject*)localController;
            }
        }

        if (SafeProcessEventCall(
            (uintptr_t)kismetDefault,
            kismetDefault,
            loadClassFunction,
            params))
        {
            ++requested;
        }
    }

    g_CounselorClassPreloadRequested =
        requested > 0;

    if (g_CounselorClassPreloadRequested)
    {
        g_CounselorClassPreloadRequestedAt =
            GetTickCount64();

        g_LoadedCounselorClassScanDone =
            false;

        Logger::Debug(
            "Counselor AI class preload requested: " +
            std::to_string(requested) +
            " entries"
        );
    }

    return
        g_CounselorClassPreloadRequested;
}


static UClass* FindRandomLoadedCounselorClass(
    UClass* localCounselorClass)
{
    // Counselor-route matches already have a small, authoritative live roster.
    // Prefer one of those loaded classes before touching the global class cache:
    // it is map-independent, bounded to eight entries, and avoids cloning the
    // locally controlled Tommy/hero when the packed build exposes no Sandbox
    // CounselorCharacterClasses array.
    UClass* rosterClasses[JasonAITargetCapacity]{};
    int32_t rosterClassCount = 0;
    for (int32_t i = 0;
        i < g_JasonAITargetCount && i < JasonAITargetCapacity;
        ++i)
    {
        AActor* counselor = g_JasonAITargets[i];
        UClass* candidate = counselor &&
            Memory::IsReadable(counselor, sizeof(UObject)) &&
            IsCounselorOrHero(counselor)
                ? counselor->Class
                : nullptr;
        if (!candidate || candidate == localCounselorClass ||
            !Memory::IsReadable(candidate, sizeof(UClass)))
        {
            continue;
        }

        bool duplicate = false;
        for (int32_t c = 0; c < rosterClassCount; ++c)
        {
            if (rosterClasses[c] == candidate)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            rosterClasses[rosterClassCount++] = candidate;
    }
    if (rosterClassCount > 0)
    {
        static uint32_t rosterCursor = 0;
        UClass* selected = rosterClasses[
            rosterCursor++ % static_cast<uint32_t>(rosterClassCount)];
        Logger::Debug(
            "Counselor AI spawn: random live-roster class selected=" +
            JasonAISafeName(reinterpret_cast<UObject*>(selected)));
        return selected;
    }

    RequestCounselorClassPreloadOnGameThread();

    // First choice: the already-loaded generated classes.  This costs
    // one optimized one-time scan per world, then gives us real variety
    // without the old repeated GObjects hitch.
    UClass* cachedClass =
        PickUnusedLoadedCounselorClass(
            localCounselorClass
        );

    if (cachedClass)
    {
        return cachedClass;
    }

    //
    // Do NOT walk all of GObjects looking for counselor classes.
    // Resurrected's Sandbox GameMode already owns the same
    // CounselorCharacterClasses soft-class array used by its native
    // offline-bot setup. Read that small array directly and resolve a
    // random entry with the Kismet soft-class converter we already use
    // for Jason_Zombie_C.
    //
    AActor* sandboxGameMode =
        g_JasonAICache.SandboxGameMode;

    UObject* kismetDefault =
        g_JasonAICache.KismetDefault;

    UFunction* convertFunction =
        g_JasonAICache.ConvertFunction;

    if (sandboxGameMode &&
        sandboxGameMode->Class &&
        Memory::IsReadable(
            sandboxGameMode,
            sizeof(UObject)) &&
        Memory::IsReadable(
            sandboxGameMode->Class,
            sizeof(UStruct)) &&
        kismetDefault &&
        convertFunction &&
        Memory::IsReadable(
            kismetDefault,
            sizeof(UObject)) &&
        Memory::IsReadable(
            convertFunction,
            sizeof(UFunction)))
    {
        UPropertyLite* counselorClassesProperty =
            FindPropertyInHierarchyByName(
                sandboxGameMode->Class,
                "CounselorCharacterClasses"
            );

        if (counselorClassesProperty &&
            counselorClassesProperty->Offset_Internal > 0 &&
            counselorClassesProperty->Offset_Internal < 0x10000)
        {
            struct RawArray
            {
                uint8_t* Data;
                int32_t Count;
                int32_t Max;
            };

            RawArray* counselorClasses =
                (RawArray*)
                ((uintptr_t)sandboxGameMode +
                    counselorClassesProperty->Offset_Internal);

            if (Memory::IsReadable(
                counselorClasses,
                sizeof(RawArray)) &&
                counselorClasses->Data &&
                counselorClasses->Count > 0 &&
                counselorClasses->Count <= 64 &&
                counselorClasses->Max >=
                    counselorClasses->Count &&
                counselorClasses->Max <= 128)
            {
                constexpr int32_t
                    SoftClassSize =
                    40;

                size_t byteCount =
                    (size_t)
                    counselorClasses->Count *
                    SoftClassSize;

                if (Memory::IsReadable(
                    counselorClasses->Data,
                    byteCount))
                {
                    struct ResolveParams
                    {
                        uint8_t SoftClass[40];
                        UClass* ReturnValue;
                    };

                    static_assert(
                        sizeof(ResolveParams) == 48,
                        "ResolveParams must be 48 bytes"
                    );

                    uint64_t randomValue =
                        (uint64_t)GetTickCount64();

                    randomValue ^=
                        (uint64_t)(uintptr_t)
                        localCounselorClass;

                    randomValue ^=
                        ((uint64_t)
                            g_CounselorBotsSpawned.load() +
                            1ULL) *
                        0x9E3779B97F4A7C15ULL;

                    randomValue ^= randomValue << 13;
                    randomValue ^= randomValue >> 7;
                    randomValue ^= randomValue << 17;

                    int32_t startIndex =
                        (int32_t)
                        (randomValue %
                            (uint64_t)
                            counselorClasses->Count);

                    UClass* localFallback =
                        nullptr;

                    for (int32_t attempt = 0;
                        attempt <
                        counselorClasses->Count;
                        ++attempt)
                    {
                        int32_t index =
                            (startIndex + attempt) %
                            counselorClasses->Count;

                        uint8_t* softClass =
                            counselorClasses->Data +
                            ((uintptr_t)index *
                                SoftClassSize);

                        ResolveParams resolveParams{};

                        for (int32_t i = 0;
                            i < SoftClassSize;
                            ++i)
                        {
                            resolveParams.
                                SoftClass[i] =
                                softClass[i];
                        }

                        bool resolveOK =
                            SafeProcessEventCall(
                                (uintptr_t)
                                kismetDefault,
                                kismetDefault,
                                convertFunction,
                                &resolveParams
                            );

                        UClass* resolved =
                            resolveParams.ReturnValue;

                        if (!resolveOK ||
                            !resolved ||
                            !Memory::IsReadable(
                                resolved,
                                sizeof(UClass)))
                        {
                            continue;
                        }

                        bool derivesFromCounselor =
                            false;

                        for (UStruct* current =
                            (UStruct*)resolved;
                            current;
                            current = current->Super)
                        {
                            if (!Memory::IsReadable(
                                current,
                                sizeof(UStruct)))
                            {
                                break;
                            }

                            if (JasonAISafeName(
                                (UObject*)current) ==
                                "SCCounselorCharacter")
                            {
                                derivesFromCounselor =
                                    true;
                                break;
                            }
                        }

                        if (!derivesFromCounselor)
                            continue;

                        if (resolved ==
                            localCounselorClass)
                        {
                            localFallback =
                                resolved;
                            continue;
                        }

                        Logger::Debug(
                            "Counselor AI random class: native pool=" +
                            std::to_string(
                                counselorClasses->Count) +
                            " | selected=" +
                            JasonAISafeName(
                                (UObject*)resolved)
                        );

                        return resolved;
                    }

                    if (localFallback)
                    {
                        Logger::Debug(
                            "Counselor AI random class: native pool resolved only local counselor; using local fallback"
                        );

                        return localFallback;
                    }

                    Logger::Debug(
                        "Counselor AI random class: native soft-class entries are not loaded yet"
                    );
                }
            }
        }
    }

    //
    // Reliability beats a failed button. Until we wire the game's
    // asynchronous counselor-class loader, fall back to the local
    // counselor class immediately. This keeps counselor spawning
    // responsive and avoids the old full-GObjects hitch.
    //
    if (localCounselorClass &&
        Memory::IsReadable(
            localCounselorClass,
            sizeof(UClass)))
    {
        Logger::Debug(
            "Counselor AI spawn: random class unavailable; cloning local counselor as safe fallback"
        );

        return localCounselorClass;
    }

    return nullptr;
}

static bool SpawnCounselorBotOnGameThread()
{
    UWorld* world =
        Engine::GetWorld();

    auto localController =
        Engine::GetLocalPlayerController();

    if (!world ||
        !localController ||
        !localController->AcknowledgedPawn ||
        !g_JasonAICache.AIBlueprintDefault ||
        !g_JasonAICache.SpawnFunction)
    {
        Logger::Debug(
            "Counselor AI spawn: resources unavailable"
        );

        return false;
    }

    AActor* localPawn =
        (AActor*)
        localController->AcknowledgedPawn;

    if (!localPawn ||
        !localPawn->Class ||
        !Memory::IsReadable(
            localPawn,
            sizeof(UObject)) ||
        !Memory::IsReadable(
            localPawn->Class,
            sizeof(UClass)) ||
        !IsCounselorOrHero(localPawn))
    {
        Logger::Debug(
            "Counselor AI spawn: local player is not a counselor"
        );

        return false;
    }

    // Prefer the reflected Sandbox GameMode property. If that
    // property has not resolved yet, fall back to the already
    // loaded BehaviorTree UObject by exact asset name.
    if (!g_JasonAICache.CounselorBehaviorTree)
    {
        g_JasonAICache.CounselorBehaviorTree =
            FindJasonAILoadedObjectByName(
                "OfflineBotsCounselorBehaviorTree",
                "BehaviorTree"
            );
    }

    UObject* counselorBehaviorTree =
        g_JasonAICache.CounselorBehaviorTree;

    if (!counselorBehaviorTree ||
        !Memory::IsReadable(
            counselorBehaviorTree,
            sizeof(UObject)))
    {
        Logger::Debug(
            "Counselor AI spawn: OfflineBotsCounselorBehaviorTree not loaded"
        );

        return false;
    }

    int32_t alreadySpawned =
        g_CounselorBotsSpawned.load();

    // The stock offline match owns fixed-size counselor/player-state arrays.
    // Live crash captures show those arrays become unsafe when F3 grows a
    // normal roster by seven or more actors and JasonDeath enumerates it.
    // Keep hotkey reinforcements deliberately small; this is an engine roster
    // limit, not a CPU-performance limit.
    constexpr int32_t MaxAdditionalCounselorsPerMatch = 3;
    if (alreadySpawned >= MaxAdditionalCounselorsPerMatch)
    {
        Logger::Error(
            "F3 counselor request rejected: safe additional-counselor cap reached (3)"
        );
        return false;
    }

    void** rootPtr =
        (void**)
        ((uintptr_t)localPawn +
            Offsets::Actor_RootComponent);

    if (!Memory::IsReadable(
        rootPtr,
        sizeof(void*)) ||
        !*rootPtr)
    {
        return false;
    }

    FVector* localLocationPtr =
        (FVector*)
        ((uintptr_t)(*rootPtr) +
            Offsets::Scene_ComponentToWorld +
            Offsets::FTransform_Translation);

    if (!Memory::IsReadable(
        localLocationPtr,
        sizeof(FVector)))
    {
        return false;
    }

    int32_t slot =
        alreadySpawned % 6;

    FVector spawnLocation{};

    if (!FindSafeCounselorSpawnLocation(
        *localLocationPtr,
        slot,
        spawnLocation))
    {
        Logger::Debug(
            "Counselor AI spawn: no safe same-level navmesh point"
        );

        return false;
    }

    UClass* counselorClass =
        FindRandomLoadedCounselorClass(
            localPawn->Class
        );

    if (!counselorClass)
    {
        Logger::Debug(
            "Counselor AI spawn: no alternate counselor class loaded"
        );

        return false;
    }

    // SpawnAIFromClass builds the controller from the pawn
    // class defaults before starting the supplied behavior tree.
    // Resurrected counselor blueprints normally point at
    // SCCounselorAIController. If this local counselor class does
    // not, temporarily substitute the native controller on its CDO
    // for this one construction and restore immediately afterward.
    constexpr uintptr_t
        Offset_PawnAIControllerClass =
        0x380;

    UClass* counselorAIClass =
        (UClass*)
        FindJasonAILoadedObjectByName(
            "SCCounselorAIController",
            "Class"
        );

    UClass** cdoAIClassPtr =
        nullptr;

    UClass* originalCDOAIClass =
        nullptr;

    bool patchedCDO =
        false;

    UObject* counselorCDO =
        counselorClass->DefaultObject;

    if (counselorAIClass &&
        counselorCDO &&
        Memory::IsReadable(
            counselorCDO,
            sizeof(UObject)))
    {
        cdoAIClassPtr =
            (UClass**)
            ((uintptr_t)counselorCDO +
                Offset_PawnAIControllerClass);

        if (Memory::IsReadable(
            cdoAIClassPtr,
            sizeof(UClass*)))
        {
            originalCDOAIClass =
                *cdoAIClassPtr;

            if (originalCDOAIClass !=
                counselorAIClass)
            {
                MEMORY_BASIC_INFORMATION mbi{};

                if (VirtualQuery(
                    cdoAIClassPtr,
                    &mbi,
                    sizeof(mbi)) != 0 &&
                    mbi.State == MEM_COMMIT &&
                    (mbi.Protect & PAGE_GUARD) == 0)
                {
                    DWORD protection =
                        mbi.Protect & 0xFF;

                    bool writable =
                        protection == PAGE_READWRITE ||
                        protection == PAGE_WRITECOPY ||
                        protection == PAGE_EXECUTE_READWRITE ||
                        protection == PAGE_EXECUTE_WRITECOPY;

                    if (writable)
                    {
                        *cdoAIClassPtr =
                            counselorAIClass;

                        patchedCDO =
                            true;
                    }
                }
            }
        }
    }

    struct Rotation3
    {
        float Pitch;
        float Yaw;
        float Roll;
    };

    struct SpawnAIParams
    {
        UObject* WorldContextObject;
        UClass* PawnClass;
        UObject* BehaviorTree;
        FVector Location;
        Rotation3 Rotation;
        bool bNoCollisionFail;
        uint8_t Padding[7];
        AActor* ReturnValue;
    };

    static_assert(
        sizeof(SpawnAIParams) == 64,
        "SpawnAIParams must be 64 bytes"
    );

    SpawnAIParams spawnParams{};
    spawnParams.WorldContextObject =
        (UObject*)world;
    spawnParams.PawnClass =
        counselorClass;
    spawnParams.BehaviorTree =
        counselorBehaviorTree;
    spawnParams.Location =
        spawnLocation;

    Logger::Debug(
        "Counselor AI spawn location: " +
        std::to_string(spawnLocation.X) +
        ", " +
        std::to_string(spawnLocation.Y) +
        ", " +
        std::to_string(spawnLocation.Z)
    );

    spawnParams.bNoCollisionFail =
        true;

    bool spawnOK =
        SafeProcessEventCall(
            (uintptr_t)
            g_JasonAICache.AIBlueprintDefault,
            g_JasonAICache.AIBlueprintDefault,
            g_JasonAICache.SpawnFunction,
            &spawnParams
        );

    if (patchedCDO &&
        cdoAIClassPtr)
    {
        *cdoAIClassPtr =
            originalCDOAIClass;
    }

    if (!spawnOK ||
        !spawnParams.ReturnValue ||
        !Memory::IsReadable(
            spawnParams.ReturnValue,
            sizeof(UObject)))
    {
        Logger::Debug(
            "Counselor AI spawn: SpawnAIFromClass failed"
        );

        return false;
    }

    constexpr uintptr_t
        Offset_PawnController =
        0x3A0;

    UObject** spawnedControllerPtr =
        (UObject**)
        ((uintptr_t)
            spawnParams.ReturnValue +
            Offset_PawnController);

    std::string spawnedControllerClass =
        "NULL";

    if (Memory::IsReadable(
        spawnedControllerPtr,
        sizeof(UObject*)) &&
        *spawnedControllerPtr &&
        Memory::IsReadable(
            *spawnedControllerPtr,
            sizeof(UObject)) &&
        (*spawnedControllerPtr)->Class &&
        Memory::IsReadable(
            (*spawnedControllerPtr)->Class,
            sizeof(UObject)))
    {
        spawnedControllerClass =
            JasonAISafeName(
                (UObject*)
                (*spawnedControllerPtr)->Class
            );
    }

    RegisterJasonAITarget(
        spawnParams.ReturnValue
    );

    int32_t newCount =
        g_CounselorBotsSpawned.fetch_add(1) +
        1;

    Logger::Success(
        "Counselor AI spawned: " +
        JasonAISafeName(
            (UObject*)spawnParams.ReturnValue
        ) +
        " | controller=" +
        spawnedControllerClass +
        " | bot=" +
        std::to_string(newCount)
    );

    return true;
}

static bool TryOfflineBotsFailsafeTeleportOnGameThread(
    const FVector& currentLocation)
{
    AActor* jason =
        g_JasonAIState.Jason;

    UWorld* world =
        Engine::GetWorld();

    if (!jason ||
        !world ||
        !jason->Class ||
        !Memory::IsReadable(
            jason,
            sizeof(UObject)) ||
        !Memory::IsReadable(
            jason->Class,
            sizeof(UObject)))
    {
        return false;
    }

    //
    // Small reflected-parameter helper.
    //
    // This lets us call K2_SetActorLocation without
    // hardcoding the large FHitResult parameter layout.
    //
    auto findParamOffset =
        [](UFunction* function,
            const char* paramName,
            int32_t minimumSize,
            int32_t bufferSize,
            int32_t& outOffset)
        -> bool
        {
            outOffset = -1;

            if (!function ||
                !paramName)
            {
                return false;
            }

            UField* field =
                function->Children;

            int guard =
                0;

            while (field &&
                guard++ < 128)
            {
                if (!Memory::IsReadable(
                    field,
                    sizeof(UField)))
                {
                    break;
                }

                if (JasonAISafeName(
                    (UObject*)field
                ) ==
                    paramName)
                {
                    UPropertyLite* property =
                        (UPropertyLite*)field;

                    if (!Memory::IsReadable(
                        property,
                        sizeof(UPropertyLite)))
                    {
                        return false;
                    }

                    int32_t offset =
                        property->
                        Offset_Internal;

                    if (offset >= 0 &&
                        property->ElementSize >=
                        minimumSize &&
                        offset + minimumSize <=
                        bufferSize)
                    {
                        outOffset =
                            offset;

                        return true;
                    }

                    return false;
                }

                field =
                    field->Next;
            }

            return false;
        };

    //
    // OfflineBots gets Jason's forward direction
    // and tries a point 500 units ahead.
    //
    UFunction* forwardFunction =
        FindFunctionInHierarchyByName(
            jason->Class,
            "GetActorForwardVector"
        );

    if (!forwardFunction)
    {
        forwardFunction =
            FindFunctionInHierarchyByName(
                jason->Class,
                "K2_GetActorForwardVector"
            );
    }

    if (!forwardFunction)
    {
        Logger::Debug(
            "Jason AI failsafe: GetActorForwardVector not found"
        );

        return false;
    }

    alignas(16)
        uint8_t forwardParams[0x40]{};

    int32_t forwardReturnOffset =
        -1;

    if (!findParamOffset(
        forwardFunction,
        "ReturnValue",
        sizeof(FVector),
        sizeof(forwardParams),
        forwardReturnOffset))
    {
        Logger::Debug(
            "Jason AI failsafe: forward-vector ReturnValue not found"
        );

        return false;
    }

    bool forwardOK =
        SafeProcessEventCall(
            (uintptr_t)jason,
            jason,
            forwardFunction,
            forwardParams
        );

    if (!forwardOK)
    {
        Logger::Debug(
            "Jason AI failsafe: forward-vector call failed"
        );

        return false;
    }

    FVector forward =
        *(FVector*)
        (forwardParams +
            forwardReturnOffset);

    float forwardLength =
        std::sqrt(
            (forward.X * forward.X) +
            (forward.Y * forward.Y) +
            (forward.Z * forward.Z)
        );

    if (!std::isfinite(
        forwardLength) ||
        forwardLength < 0.01f)
    {
        Logger::Debug(
            "Jason AI failsafe: invalid forward vector"
        );

        return false;
    }

    forward.X /=
        forwardLength;

    forward.Y /=
        forwardLength;

    forward.Z /=
        forwardLength;

    FVector candidate{};

    candidate.X =
        currentLocation.X +
        (forward.X * 500.0f);

    candidate.Y =
        currentLocation.Y +
        (forward.Y * 500.0f);

    candidate.Z =
        currentLocation.Z +
        (forward.Z * 500.0f);

    FVector projectedLocation{};

    FVector projectionExtent{};

    projectionExtent.X =
        100.0f;

    projectionExtent.Y =
        100.0f;

    projectionExtent.Z =
        10.0f;

    if (!ProjectJasonAINavPointOnGameThread(
        candidate,
        projectionExtent,
        projectedLocation))
    {
        Logger::Debug(
            "Jason AI failsafe: no safe navmesh location found"
        );

        return false;
    }

    //
    // OfflineBots ultimately uses
    // AActor::SetActorLocation.
    //
    // Use the reflected K2 wrapper here, but derive
    // its parameter offsets instead of hardcoding
    // FHitResult's layout.
    //
    UFunction* setLocationFunction =
        FindFunctionInHierarchyByName(
            jason->Class,
            "K2_SetActorLocation"
        );

    if (!setLocationFunction)
    {
        Logger::Debug(
            "Jason AI failsafe: K2_SetActorLocation not found"
        );

        return false;
    }

    alignas(16)
        uint8_t setLocationParams[0x200]{};

    int32_t newLocationOffset =
        -1;

    int32_t sweepOffset =
        -1;

    int32_t teleportOffset =
        -1;

    int32_t returnOffset =
        -1;

    if (!findParamOffset(
        setLocationFunction,
        "NewLocation",
        sizeof(FVector),
        sizeof(setLocationParams),
        newLocationOffset) ||
        !findParamOffset(
            setLocationFunction,
            "bSweep",
            sizeof(bool),
            sizeof(setLocationParams),
            sweepOffset) ||
        !findParamOffset(
            setLocationFunction,
            "bTeleport",
            sizeof(bool),
            sizeof(setLocationParams),
            teleportOffset) ||
        !findParamOffset(
            setLocationFunction,
            "ReturnValue",
            sizeof(bool),
            sizeof(setLocationParams),
            returnOffset))
    {
        Logger::Debug(
            "Jason AI failsafe: K2_SetActorLocation parameter layout incomplete"
        );

        return false;
    }

    *(FVector*)
        (setLocationParams +
            newLocationOffset) =
        projectedLocation;

    *(bool*)
        (setLocationParams +
            sweepOffset) =
        false;

    *(bool*)
        (setLocationParams +
            teleportOffset) =
        true;

    bool setLocationOK =
        SafeProcessEventCall(
            (uintptr_t)jason,
            jason,
            setLocationFunction,
            setLocationParams
        );

    bool moved =
        setLocationOK &&
        *(bool*)
        (setLocationParams +
            returnOffset);

    if (!moved)
    {
        Logger::Debug(
            "Jason AI failsafe: SetActorLocation failed"
        );

        return false;
    }

    Logger::Success(
        "Jason AI failsafe: teleported forward to navmesh point " +
        std::to_string(
            projectedLocation.X
        ) +
        ", " +
        std::to_string(
            projectedLocation.Y
        ) +
        ", " +
        std::to_string(
            projectedLocation.Z
        )
    );

    return true;
}
// The counselor-route bridge temporarily shortens this while an occupied
// escape car or arrived police create a high-priority pursuit.  It restores
// the stock 20-second value as soon as that state ends.
static ULONGLONG
    JasonAIMorphCooldownMs =
    20000ULL;

static ULONGLONG JasonAIMorphReadyAt()
{
    if (g_JasonAIState.LastMorphTeleportAt == 0)
        return 0;

    return
        g_JasonAIState.LastMorphTeleportAt +
        JasonAIMorphCooldownMs;
}

static ULONGLONG JasonAIMorphRemainingMs(
    ULONGLONG now)
{
    ULONGLONG readyAt =
        JasonAIMorphReadyAt();

    if (readyAt == 0 ||
        now >= readyAt)
    {
        return 0;
    }

    return
        readyAt - now;
}

static void MarkJasonAIMorphTeleportUsed(
    const char* reason)
{
    ULONGLONG now =
        GetTickCount64();

    g_JasonAIState.LastMorphTeleportAt =
        now;

    g_JasonAIState.NextDistanceTeleportAt =
        now + JasonAIMorphCooldownMs;

    g_JasonAIState.NextMorphCooldownLogAt =
        0;

    Logger::Debug(
        std::string(
            "Jason AI Morph used: reason=") +
        (reason ? reason : "Unknown") +
        " | cooldownMs=" +
        std::to_string(
            JasonAIMorphCooldownMs)
    );
}

static bool RunOfflineBotsInitialTeleportOnGameThread()
{
    AActor* jason =
        g_JasonAIState.Jason;

    if (!jason ||
        !Memory::IsReadable(
            jason,
            sizeof(UObject)) ||
        !jason->Class ||
        !Memory::IsReadable(
            jason->Class,
            sizeof(UObject)))
    {
        return false;
    }

    AActor* target =
        FindNearestJasonAICounselorTarget(
            jason
        );

    if (!target)
    {
        return false;
    }

    FVector targetLocation{};

    if (!GetJasonAIActorLocation(
        target,
        targetLocation))
    {
        return false;
    }

    UFunction* teleportFunction =
        FindFunctionInHierarchyByName(
            jason->Class,
            "K2_TeleportTo"
        );

    if (!teleportFunction)
    {
        Logger::Debug(
            "Jason AI initial teleport: K2_TeleportTo not found"
        );

        return false;
    }

    struct Rotation3
    {
        float Pitch;
        float Yaw;
        float Roll;
    };

    struct TeleportParams
    {
        FVector DestLocation;
        Rotation3 DestRotation;
        bool ReturnValue;
    };

    static_assert(
        sizeof(TeleportParams) == 28,
        "TeleportParams must be 28 bytes"
        );

    struct TeleportOffset
    {
        float X;
        float Y;
    };

    //
    // Try a ring around the counselor instead of
    // blindly assuming world +X is safe.
    //
    const TeleportOffset offsets[] =
    {
        // Primary ring: about 22 meters from the counselor.
        { 2200.0f,     0.0f },
        {-2200.0f,     0.0f },
        {    0.0f,  2200.0f },
        {    0.0f, -2200.0f },

        { 1556.0f,  1556.0f },
        { 1556.0f, -1556.0f },
        {-1556.0f,  1556.0f },
        {-1556.0f, -1556.0f },

        // Backup ring: about 30 meters out.
        { 3000.0f,     0.0f },
        {-3000.0f,     0.0f },
        {    0.0f,  3000.0f },
        {    0.0f, -3000.0f },

        { 2121.0f,  2121.0f },
        { 2121.0f, -2121.0f },
        {-2121.0f,  2121.0f },
        {-2121.0f, -2121.0f }
    };

    //
    // Target locations are near the capsule center.  Project around the
    // target's estimated floor instead of using a tall +/-Z search that
    // can select a cabin roof navmesh layer.
    //
    constexpr float
        CounselorCapsuleLift =
        90.0f;

    float targetGroundZ =
        targetLocation.Z -
        CounselorCapsuleLift;

    FVector projectionExtent{};

    projectionExtent.X =
        350.0f;

    projectionExtent.Y =
        350.0f;

    projectionExtent.Z =
        110.0f;

    for (const TeleportOffset& offset :
        offsets)
    {
        FVector candidate =
            targetLocation;

        candidate.X +=
            offset.X;

        candidate.Y +=
            offset.Y;

        candidate.Z =
            targetGroundZ +
            25.0f;

        FVector projectedLocation{};

        if (!ProjectJasonAINavPointOnGameThread(
            candidate,
            projectionExtent,
            projectedLocation))
        {
            continue;
        }

        float verticalDelta =
            std::fabs(
                projectedLocation.Z -
                targetGroundZ);

        if (!std::isfinite(
            verticalDelta) ||
            verticalDelta >
                120.0f)
        {
            Logger::Debug(
                "Jason AI teleport candidate rejected: wrong vertical nav layer"
            );

            continue;
        }

        //
        // Do not allow the nav projection to snap Jason back onto
        // an indoor/near-counselor nav point. Requiring at least
        // 17 meters of horizontal separation strongly biases the
        // teleport toward exterior ground around cabins/lodges.
        //
        float projectedDX =
            projectedLocation.X -
            targetLocation.X;

        float projectedDY =
            projectedLocation.Y -
            targetLocation.Y;

        float projectedHorizontalDistance =
            std::sqrt(
                (projectedDX * projectedDX) +
                (projectedDY * projectedDY)
            );

        if (!std::isfinite(
                projectedHorizontalDistance) ||
            projectedHorizontalDistance <
                1700.0f)
        {
            Logger::Debug(
                "Jason AI teleport candidate rejected: too close to counselor / possible indoor nav"
            );

            continue;
        }

        TeleportParams params{};

        params.DestLocation =
            projectedLocation;

        bool callOK =
            SafeProcessEventCall(
                (uintptr_t)jason,
                jason,
                teleportFunction,
                &params
            );

        if (!callOK ||
            !params.ReturnValue)
        {
            continue;
        }

        g_JasonAIState.Target =
            target;

        Logger::Success(
            "Jason AI initial teleport: navmesh destination accepted " +
            std::to_string(
                projectedLocation.X
            ) +
            ", " +
            std::to_string(
                projectedLocation.Y
            ) +
            ", " +
            std::to_string(
                projectedLocation.Z
            )
        );

        return true;
    }

    Logger::Debug(
        "Jason AI initial teleport: no valid destination around counselor"
    );

    return false;
}

static bool RunJasonAIDistanceTeleportOnGameThread()
{
    if (!g_JasonAIState.Active)
        return false;

    AActor* jason =
        g_JasonAIState.Jason;

    if (!jason ||
        !Memory::IsReadable(
            jason,
            sizeof(UObject)))
    {
        return false;
    }

    AActor* target =
        FindNearestJasonAICounselorTarget(
            jason
        );

    if (!target)
    {
        return false;
    }

    FVector jasonLocation{};
    FVector targetLocation{};

    if (!GetJasonAIActorLocation(
        jason,
        jasonLocation) ||
        !GetJasonAIActorLocation(
            target,
            targetLocation))
    {
        return false;
    }

    float dx =
        targetLocation.X -
        jasonLocation.X;

    float dy =
        targetLocation.Y -
        jasonLocation.Y;

    float dz =
        targetLocation.Z -
        jasonLocation.Z;

    float distance =
        std::sqrt(
            (dx * dx) +
            (dy * dy) +
            (dz * dz)
        );

    // Unreal units are centimeters. Trigger at ~90m so Jason
    // reliably teleports by the time the counselor is around
    // 100m away in gameplay.
    constexpr float
        DistanceTeleportThreshold =
        9000.0f;

    if (!std::isfinite(distance) ||
        distance <=
        DistanceTeleportThreshold)
    {
        return false;
    }

    ULONGLONG now =
        GetTickCount64();

    // Morph is a real ability now: being beyond 90/100m does NOT bypass
    // its recharge. Jason keeps chasing on foot until 20 seconds have elapsed
    // since the last successful Morph teleport.
    ULONGLONG morphRemaining =
        JasonAIMorphRemainingMs(
            now);

    if (morphRemaining > 0)
    {
        if (now >=
            g_JasonAIState.NextMorphCooldownLogAt)
        {
            g_JasonAIState.NextMorphCooldownLogAt =
                now + 2000;

            Logger::Debug(
                "Jason AI distance Morph charging: remainingMs=" +
                std::to_string(
                    morphRemaining) +
                " | distance=" +
                std::to_string(
                    distance)
            );
        }

        return false;
    }

    if (now <
        g_JasonAIState.NextDistanceTeleportAt)
    {
        return false;
    }

    bool teleported =
        RunOfflineBotsInitialTeleportOnGameThread();

    if (!teleported)
    {
        // Retry soon if every exterior-biased candidate failed.
        g_JasonAIState.NextDistanceTeleportAt =
            now + 3000;

        Logger::Debug(
            "Jason AI distance teleport: no valid exterior destination | distance=" +
            std::to_string(distance)
        );

        return false;
    }

    // Successful counselor-distance Morph starts the shared 20-second timer.
    MarkJasonAIMorphTeleportUsed(
        "DistanceCounselor");

    g_JasonAIState.PathLocked =
        false;

    g_JasonAIState.PathLockUntil =
        0;

    g_JasonAIState.LastAcceptedMoveAt =
        0;

    g_JasonAIState.ConsecutiveStuckChecks =
        0;

    g_JasonAIState.ConsecutiveNearTargetStuckDeferrals =
        0;

    g_JasonAIState.HaveLastLocation =
        false;

    Logger::Success(
        "Jason AI distance teleport: target was " +
        std::to_string(distance) +
        " units away"
    );

    return true;
}

static bool RunJasonAIDoorInteractionOnGameThread();

static bool RunJasonAIChaseUpdateOnGameThread()
{
    if (!g_JasonAIState.Active)
        return false;

    UWorld* world =
        Engine::GetWorld();

    if (!world ||
        world != g_JasonAIState.World ||
        !g_JasonAIState.Jason ||
        !g_JasonAIState.Controller ||
        !Memory::IsReadable(
            g_JasonAIState.Jason,
            sizeof(UObject)) ||
        !Memory::IsReadable(
            g_JasonAIState.Controller,
            sizeof(UObject)))
    {
        g_JasonAIState.Active =
            false;

        return false;
    }

    ULONGLONG now =
        GetTickCount64();

    //
// OfflineBots BeginPlay schedules one initial
// teleport roughly two seconds after startup.
//
    if (!g_JasonAIState.InitialTeleportAttempted)
    {
        if (now <
            g_JasonAIState.InitialTeleportAt)
        {
            return true;
        }

        g_JasonAIState.InitialTeleportAttempted =
            true;

        bool teleportOK =
            RunOfflineBotsInitialTeleportOnGameThread();

        if (teleportOK)
        {
            MarkJasonAIMorphTeleportUsed(
                "InitialCounselor");
        }

        //
        // Do not issue MoveTo in the same frame as
        // the teleport. Let the pawn/nav state settle.
        //
        g_JasonAIState.PathLocked =
            false;

        g_JasonAIState.PathLockUntil =
            0;

        Logger::Debug(
            std::string(
                "Jason AI OfflineBots initial teleport attempt="
            ) +
            (teleportOK ?
                "true" :
                "false")
        );

        return true;
    }

    // Keep Jason from spending long stretches on the
    // opposite side of the map. The helper now triggers
    // around 90m so he should teleport by roughly 100m.
    if (RunJasonAIDistanceTeleportOnGameThread())
    {
        return true;
    }

    //
    // OfflineBots CheckIfStuck:
    //
    // Sample movement often enough to catch a real obstruction without
    // synchronizing this job with the bridge's one-second maintenance lanes.
    // position against his previous position.
    //
    // The working mod uses a 30-unit threshold.
    //
    if (now >=
        g_JasonAIState.NextStuckCheckAt)
    {
        g_JasonAIState.NextStuckCheckAt =
            now + 1375;

        FVector currentLocation{};

        if (GetJasonAIActorLocation(
            g_JasonAIState.Jason,
            currentLocation))
        {
            if (g_JasonAIState.HaveLastLocation)
            {
                float dx =
                    currentLocation.X -
                    g_JasonAIState.LastLocation.X;

                float dy =
                    currentLocation.Y -
                    g_JasonAIState.LastLocation.Y;

                float dz =
                    currentLocation.Z -
                    g_JasonAIState.LastLocation.Z;

                float distanceMoved =
                    std::sqrt(
                        (dx * dx) +
                        (dy * dy) +
                        (dz * dz)
                    );

                //
                // Only perform the failsafe while
                // PathFollowingComponent says Jason is
                // actively moving.
                //
                // This prevents an AlreadyAtGoal Jason
                // from being pushed past the counselor.
                //
                int32_t pathStatus =
                    -1;

                uintptr_t pathFollowingField =
                    (uintptr_t)
                    g_JasonAIState.Controller +
                    0x410;

                if (Memory::IsReadable(
                    (void*)pathFollowingField,
                    sizeof(uintptr_t)))
                {
                    uintptr_t pfc =
                        *(uintptr_t*)
                        pathFollowingField;

                    if (pfc &&
                        Memory::IsReadable(
                            (void*)(pfc + 0x238),
                            sizeof(int32_t)))
                    {
                        pathStatus =
                            *(int32_t*)
                            (pfc + 0x238);
                    }
                }

                static ULONGLONG nextMovementDiagnosticAt = 0;
                const bool periodicMovementDiagnostic =
                    now >= nextMovementDiagnosticAt;
                if (periodicMovementDiagnostic)
                {
                    nextMovementDiagnosticAt = now + 10000;
                    Logger::Debug(
                        "Jason AI movement: distance=" +
                        std::to_string(distanceMoved) +
                        " | status=" + std::to_string(pathStatus) +
                        " | location=" +
                        std::to_string(currentLocation.X) + ", " +
                        std::to_string(currentLocation.Y) + ", " +
                        std::to_string(currentLocation.Z));
                }

                //
// Our sampler often runs after the PFC
// has already returned to Idle.
//
// Instead of requiring status == 3 at
// this exact instant, remember whether
// MoveTo was successfully accepted during
// the last few seconds.
//
                bool recentMoveAccepted =
                    g_JasonAIState.
                    LastAcceptedMoveAt != 0 &&
                    now >=
                    g_JasonAIState.
                    LastAcceptedMoveAt &&
                    (now -
                        g_JasonAIState.
                        LastAcceptedMoveAt) <=
                    5000;

                if (distanceMoved <
                    g_JasonAIState.StuckThreshold &&
                    recentMoveAccepted)
                {
                    ++g_JasonAIState.
                        ConsecutiveStuckChecks;
                }
                else
                {
                    g_JasonAIState.
                        ConsecutiveStuckChecks =
                        0;
                    g_JasonAIState.
                        ConsecutiveNearTargetStuckDeferrals =
                        0;
                }

                if (periodicMovementDiagnostic ||
                    g_JasonAIState.ConsecutiveStuckChecks > 0)
                {
                    Logger::Debug(
                        "Jason AI stuck check: count=" +
                        std::to_string(
                            g_JasonAIState.ConsecutiveStuckChecks) +
                        " | recentMove=" +
                        std::string(recentMoveAccepted ? "Y" : "N"));
                }

                //
                // Fast door response: one genuine low-movement
                // check is enough to test the currently selected
                // nearby SCDoor. If no valid door is selected,
                // normal failsafe recovery continues.
                //
                if (g_JasonAIState.
                    ConsecutiveStuckChecks >= 1)
                {
                    //
                    // Before teleporting a genuinely stuck Jason,
                    // give the currently selected SCDoor one chance.
                    // This makes door breaking a response to an
                    // actual obstruction instead of nearby scenery.
                    //
                    bool doorHandled =
                        RunJasonAIDoorInteractionOnGameThread();

                    if (doorHandled)
                    {
                        Logger::Debug(
                            g_JasonAIState.DoorBreakActive
                                ? "Jason AI obstruction: door break started, suppressing failsafe teleport"
                                : "Jason AI obstruction: unlocked door opening, suppressing break/teleport fallback"
                        );

                        g_JasonAIState.
                            ConsecutiveStuckChecks =
                            0;
                        g_JasonAIState.
                            ConsecutiveNearTargetStuckDeferrals =
                            0;

                        g_JasonAIState.
                            LastAcceptedMoveAt =
                            0;

                        g_JasonAIState.
                            HaveLastLocation =
                            false;

                        g_JasonAIState.
                            PathLocked =
                            false;

                        g_JasonAIState.
                            PathLockUntil =
                            0;

                        //
                        // Door-breaking animation/state needs time
                        // before another stuck measurement.
                        //
                        g_JasonAIState.
                            NextStuckCheckAt =
                            now + 750;

                        return true;
                    }

                    if (now <
                        g_JasonAIState.
                            DoorTraversalGraceUntil)
                    {
                        Logger::Debug(
                            "Jason AI door traversal: keeping MoveTo, suppressing failsafe teleport"
                        );

                        g_JasonAIState.
                            ConsecutiveStuckChecks =
                            0;
                        g_JasonAIState.
                            ConsecutiveNearTargetStuckDeferrals =
                            0;

                        // The interaction already releases the old path
                        // once, so the next chase request crosses the newly
                        // opened doorway. Keep that accepted path during
                        // traversal grace. Clearing its lock on every short
                        // movement sample restarted MoveTo every two seconds
                        // and left Jason circling the same threshold.
                    }
                    else if (g_JasonAIState.
                        ConsecutiveStuckChecks < 2)
                    {
                        // One low-movement sample is enough to test a
                        // blocking door, but not enough to teleport.
                        // Knife wind-up/release and doorway transitions can
                        // legitimately produce a single short interval.
                        Logger::Debug(
                            "Jason AI stuck sample 1: preserving active MoveTo, waiting for confirmation"
                        );

                        // A short close-combat pause is normal, especially
                        // after Tommy returns and repeatedly engages Jason.
                        // Clearing this lock rebuilt a valid native nav path
                        // every 1.375 seconds, producing the exact rhythmic
                        // video hitch reported in physical testing. Keep the
                        // accepted MoveTo alive; the second sample still owns
                        // door handling and the bounded genuine-stuck path.
                    }
                    else
                    {
                        // Do not pay synchronous navmesh projection and
                        // counselor-ring search costs when the adapter would
                        // reject the result anyway. Near-counselor recovery is
                        // owned by door interaction plus ordinary MoveTo; all
                        // teleports also wait for the shared Morph recharge.
                        bool recoveryEligible = false;
                        float targetDistance = FLT_MAX;
                        const ULONGLONG morphReadyAt = JasonAIMorphReadyAt();
                        AActor* recoveryTarget =
                            FindNearestJasonAICounselorTarget(
                                g_JasonAIState.Jason);
                        FVector recoveryTargetLocation{};
                        if (recoveryTarget &&
                            GetJasonAIActorLocation(
                                recoveryTarget,
                                recoveryTargetLocation))
                        {
                            const float targetDX =
                                recoveryTargetLocation.X - currentLocation.X;
                            const float targetDY =
                                recoveryTargetLocation.Y - currentLocation.Y;
                            const float targetDZ =
                                recoveryTargetLocation.Z - currentLocation.Z;
                            targetDistance = std::sqrt(
                                targetDX * targetDX +
                                targetDY * targetDY +
                                targetDZ * targetDZ);
                            recoveryEligible =
                                std::isfinite(targetDistance) &&
                                targetDistance >= 3000.0f &&
                                (morphReadyAt == 0 || now >= morphReadyAt);

                            if (std::isfinite(targetDistance) &&
                                targetDistance < 3000.0f &&
                                (morphReadyAt == 0 || now >= morphReadyAt))
                            {
                                ++g_JasonAIState.
                                    ConsecutiveNearTargetStuckDeferrals;
                                // Preserve ordinary close combat and short
                                // animation pauses.  Two additional confirmed
                                // no-movement samples prove the nearby block is
                                // persistent terrain/geometry, at which point
                                // the normal navmesh-projected failsafe may
                                // relocate Jason instead of looping forever.
                                recoveryEligible =
                                    g_JasonAIState.
                                        ConsecutiveNearTargetStuckDeferrals >=
                                    2;
                            }
                            else if (targetDistance >= 3000.0f)
                            {
                                g_JasonAIState.
                                    ConsecutiveNearTargetStuckDeferrals =
                                    0;
                            }
                        }

                        bool recovered = false;
                        if (recoveryEligible)
                        {
                            Logger::Debug(
                                "Jason AI confirmed stuck; eligible recovery attempt | nearTargetDeferrals=" +
                                std::to_string(
                                    g_JasonAIState.
                                        ConsecutiveNearTargetStuckDeferrals)
                            );
                            recovered =
                                TryOfflineBotsFailsafeTeleportOnGameThread(
                                    currentLocation);
                            if (!recovered)
                            {
                                Logger::Debug(
                                    "Jason AI stuck recovery: forward failsafe failed, trying same-level counselor ring"
                                );
                                recovered =
                                    RunOfflineBotsInitialTeleportOnGameThread();
                            }
                        }
                        else
                        {
                            static ULONGLONG nextDeferredRecoveryLogAt = 0;
                            if (now >= nextDeferredRecoveryLogAt)
                            {
                                nextDeferredRecoveryLogAt = now + 10000;
                                Logger::Debug(
                                    "Jason AI expensive stuck recovery deferred | distanceCm=" +
                                    std::to_string(targetDistance) +
                                    " | morphReadyInMs=" +
                                    std::to_string(
                                        morphReadyAt > now
                                            ? morphReadyAt - now
                                            : 0));
                            }
                        }

                        if (recovered)
                        {
                            Logger::Success(
                                "Jason AI stuck recovery succeeded");
                            g_JasonAIState.
                                ConsecutiveStuckChecks =
                                0;
                            g_JasonAIState.
                                ConsecutiveNearTargetStuckDeferrals =
                                0;

                            g_JasonAIState.
                                LastAcceptedMoveAt =
                                0;

                            //
                            // Let the relocated pawn settle
                            // before measuring another interval.
                            //
                            g_JasonAIState.HaveLastLocation =
                                false;

                            g_JasonAIState.PathLocked =
                                false;

                            g_JasonAIState.PathLockUntil =
                                0;

                            return true;
                        }

                        //
                        // A failed or currently ineligible recovery must not
                        // run again on the next sample. Ordinary MoveTo and
                        // door handling continue during this backoff.
                        //
                        g_JasonAIState.
                            ConsecutiveStuckChecks =
                            1;
                        g_JasonAIState.NextStuckCheckAt =
                            now + 4750;
                        // When Jason is already within ordinary chase/combat
                        // range, a Morph recovery is intentionally ineligible.
                        // Do not cancel the accepted actor-following path in
                        // that case. Door checks have already run above, and
                        // the normal six-second stale-path expiry remains the
                        // bounded fallback if the path truly stops tracking.
                    }
                }
            }

            g_JasonAIState.LastLocation =
                currentLocation;

            g_JasonAIState.HaveLastLocation =
                true;
        }
    }

    //
    // OfflineBots keeps a path locked for
    // roughly three seconds before issuing
    // another chase request.
    //
    if (g_JasonAIState.PathLocked)
    {
        if (now <
            g_JasonAIState.PathLockUntil)
        {
            return true;
        }

        g_JasonAIState.PathLocked =
            false;
    }

    //
    // Match OfflineBots' basic target policy: chase the
    // nearest currently controlled counselor, including
    // native counselor bots added by this DLL.
    //
    AActor* previousTarget =
        g_JasonAIState.Target;

    AActor* target =
        FindNearestJasonAICounselorTarget(
            g_JasonAIState.Jason
        );

    if (!target)
    {
        return true;
    }

    g_JasonAIState.Target = target;

    // Several route adapters may deliberately release PathLocked after an
    // interaction.  Do not synchronously rebuild the same accepted native
    // path on their next maintenance frame when the target has not changed.
    // This closes the observed 2-second MoveTo beat without delaying a real
    // target switch, failed request, knife completion, or vehicle handoff
    // (those paths clear LastAcceptedMoveAt or replace Target).
    if (target == previousTarget &&
        g_JasonAIState.LastAcceptedMoveAt != 0 &&
        now >= g_JasonAIState.LastAcceptedMoveAt &&
        now < g_JasonAIState.LastAcceptedMoveAt + 6107)
    {
        g_JasonAIState.PathLocked = true;
        g_JasonAIState.PathLockUntil =
            g_JasonAIState.LastAcceptedMoveAt + 6107;
        return true;
    }

    //
    // Lock immediately so a failed request
    // cannot be hammered every ProcessEvent.
    //
    g_JasonAIState.PathLocked =
        true;

    g_JasonAIState.PathLockUntil =
        now +
        (ULONGLONG)(
            g_JasonAIState.
            PathLockDurationSeconds *
            1000.0f
            );

    HMODULE module =
        GetModuleHandle(nullptr);

    if (!module)
        return true;

    //
    // Resurrected equivalent of
    // SCJasonOnlyNavigationQueryFilter::StaticClass.
    //
    constexpr uintptr_t
        JasonOnlyFilterStaticClassRVA =
        0x004C9410;

    uintptr_t staticClassAddress =
        (uintptr_t)module +
        JasonOnlyFilterStaticClassRVA;

    if (!Memory::IsReadable(
        (void*)staticClassAddress,
        1))
    {
        return true;
    }

    using StaticClassFn =
        UClass * (*)();

    StaticClassFn getFilterClass =
        (StaticClassFn)
        staticClassAddress;

    UClass* filterClass =
        getFilterClass();

    if (!filterClass ||
        !Memory::IsReadable(
            filterClass,
            sizeof(UClass)) ||
        JasonAISafeName(
            (UObject*)filterClass
        ) !=
        "SCJasonOnlyNavigationQueryFilter")
    {
        return true;
    }

    //
    // Resurrected FAIMoveRequest(AActor*).
    //
    constexpr uintptr_t
        MoveRequestCtorRVA =
        0x0111A310;

    uintptr_t moveRequestCtorAddress =
        (uintptr_t)module +
        MoveRequestCtorRVA;

    if (!Memory::IsReadable(
        (void*)moveRequestCtorAddress,
        1))
    {
        return true;
    }

    //
    // OfflineBots calls native AAIController
    // MoveTo through vtable +0x720.
    //
    uintptr_t controllerVTable =
        *(uintptr_t*)
        g_JasonAIState.Controller;

    if (!controllerVTable ||
        !Memory::IsReadable(
            (void*)controllerVTable,
            sizeof(uintptr_t)))
    {
        return true;
    }

    constexpr uintptr_t
        NativeMoveVTableOffset =
        0x720;

    uintptr_t nativeMoveSlot =
        controllerVTable +
        NativeMoveVTableOffset;

    if (!Memory::IsReadable(
        (void*)nativeMoveSlot,
        sizeof(uintptr_t)))
    {
        return true;
    }

    uintptr_t nativeMoveAddress =
        *(uintptr_t*)
        nativeMoveSlot;

    if (!nativeMoveAddress ||
        !Memory::IsReadable(
            (void*)nativeMoveAddress,
            1))
    {
        return true;
    }

    uint8_t nativeResult[16]{};

    //
    // Diagnostic: inspect the live PathFollowingComponent
    // immediately before and after native MoveTo.
    //
    uintptr_t pathFollowingComponent = 0;

    int32_t pathStatusBefore = -1;
    int32_t pathStatusAfter = -1;

    int32_t pfcRequestIdBefore = -1;
    int32_t pfcRequestIdAfter = -1;

    uintptr_t movementComponentBefore = 0;
    uintptr_t movementComponentAfter = 0;

    uintptr_t currentPathBefore = 0;
    uintptr_t currentPathAfter = 0;

    uintptr_t pathFollowingField =
        (uintptr_t)g_JasonAIState.Controller +
        0x410;

    if (Memory::IsReadable(
        (void*)pathFollowingField,
        sizeof(uintptr_t)))
    {
        pathFollowingComponent =
            *(uintptr_t*)pathFollowingField;
    }

    if (pathFollowingComponent)
    {
        if (Memory::IsReadable(
            (void*)(pathFollowingComponent + 0x220),
            sizeof(uintptr_t)))
        {
            movementComponentBefore =
                *(uintptr_t*)
                (pathFollowingComponent + 0x220);
        }

        if (Memory::IsReadable(
            (void*)(pathFollowingComponent + 0x238),
            sizeof(int32_t)))
        {
            pathStatusBefore =
                *(int32_t*)
                (pathFollowingComponent + 0x238);
        }

        if (Memory::IsReadable(
            (void*)(pathFollowingComponent + 0x240),
            sizeof(uintptr_t)))
        {
            currentPathBefore =
                *(uintptr_t*)
                (pathFollowingComponent + 0x240);
        }

        if (Memory::IsReadable(
            (void*)(pathFollowingComponent + 0x380),
            sizeof(int32_t)))
        {
            pfcRequestIdBefore =
                *(int32_t*)
                (pathFollowingComponent + 0x380);
        }
    }

    bool callOK =
        SafeNativeJasonMoveCall(
            moveRequestCtorAddress,
            nativeMoveAddress,
            g_JasonAIState.Controller,
            target,
            filterClass,
            nativeResult
        );

    if (pathFollowingComponent)
    {
        if (Memory::IsReadable(
            (void*)(pathFollowingComponent + 0x220),
            sizeof(uintptr_t)))
        {
            movementComponentAfter =
                *(uintptr_t*)
                (pathFollowingComponent + 0x220);
        }

        if (Memory::IsReadable(
            (void*)(pathFollowingComponent + 0x238),
            sizeof(int32_t)))
        {
            pathStatusAfter =
                *(int32_t*)
                (pathFollowingComponent + 0x238);
        }

        if (Memory::IsReadable(
            (void*)(pathFollowingComponent + 0x240),
            sizeof(uintptr_t)))
        {
            currentPathAfter =
                *(uintptr_t*)
                (pathFollowingComponent + 0x240);
        }

        if (Memory::IsReadable(
            (void*)(pathFollowingComponent + 0x380),
            sizeof(int32_t)))
        {
            pfcRequestIdAfter =
                *(int32_t*)
                (pathFollowingComponent + 0x380);
        }
    }

    uint32_t requestId =
        *(uint32_t*)nativeResult;

    uint8_t moveResult =
        nativeResult[4];

    //
// Remember whether Jason has a recently accepted
// chase request.
//
// result 2 = RequestSuccessful
// result 1 = AlreadyAtGoal
// result 0 = Failed
//
    if (callOK &&
        moveResult == 2)
    {
        g_JasonAIState.LastAcceptedMoveAt =
            GetTickCount64();

        // A real path was accepted, so this target is not currently
        // demonstrating the wall/window fixation pattern.
        g_JasonAIState.NoPathTarget =
            nullptr;

        g_JasonAIState.ConsecutiveNoPathTargets =
            0;
    }
    else
    {
        //
        // AlreadyAtGoal or a failed request means the
        // normal forward stuck failsafe should not push Jason.
        //
        g_JasonAIState.LastAcceptedMoveAt =
            0;

        g_JasonAIState.ConsecutiveStuckChecks =
            0;

        g_JasonAIState.ConsecutiveNearTargetStuckDeferrals =
            0;

        // The bad case seen in the log is result=1 with no path,
        // repeated against the same nearby counselor while Jason
        // attacks a wall/window. Three consecutive chase cycles is
        // roughly nine seconds, long enough to avoid abandoning a
        // normal close-range fight.
        if (callOK &&
            moveResult == 1 &&
            !currentPathAfter)
        {
            if (g_JasonAIState.NoPathTarget ==
                target)
            {
                ++g_JasonAIState.
                    ConsecutiveNoPathTargets;
            }
            else
            {
                g_JasonAIState.NoPathTarget =
                    target;

                g_JasonAIState.
                    ConsecutiveNoPathTargets =
                    1;
            }

            if (g_JasonAIState.
                ConsecutiveNoPathTargets >= 2)
            {
                //
                // Only blacklist this counselor when Jason actually
                // has another usable counselor to hunt.  With just
                // the local player registered, blacklisting the only
                // target leaves Jason with nothing to chase and he
                // appears permanently frozen.
                //
                if (HasAlternativeUsableJasonAITarget(
                    target))
                {
                    BlockJasonAITarget(
                        target
                    );

                    g_JasonAIState.Target =
                        nullptr;

                    Logger::Debug(
                        "Jason AI target blocked: repeated AlreadyAtGoal/no-path"
                    );
                }
                else
                {
                    Logger::Debug(
                        "Jason AI target retained: only usable counselor"
                    );
                }

                g_JasonAIState.NoPathTarget =
                    nullptr;

                g_JasonAIState.
                    ConsecutiveNoPathTargets =
                    0;

                g_JasonAIState.PathLocked =
                    false;

                g_JasonAIState.PathLockUntil =
                    0;
            }
        }
        else
        {
            g_JasonAIState.NoPathTarget =
                nullptr;

            g_JasonAIState.ConsecutiveNoPathTargets =
                0;
        }
    }


    Logger::Debug(
        "Jason AI chase: MoveTo call=" +
        std::string(
            callOK ? "true" : "false"
        ) +
        " | returnedId=" +
        std::to_string(requestId) +
        " | result=" +
        std::to_string(
            (int)moveResult
        ) +
        " | status=" +
        std::to_string(pathStatusBefore) +
        "->" +
        std::to_string(pathStatusAfter) +
        " | pfcId=" +
        std::to_string(pfcRequestIdBefore) +
        "->" +
        std::to_string(pfcRequestIdAfter) +
        " | moveComp=" +
        std::string(
            movementComponentBefore ? "Y" : "N"
        ) +
        "->" +
        std::string(
            movementComponentAfter ? "Y" : "N"
        ) +
        " | path=" +
        std::string(
            currentPathBefore ? "Y" : "N"
        ) +
        "->" +
        std::string(
            currentPathAfter ? "Y" : "N"
        )
    );

    return true;
}

static bool RunJasonAIDoorInteractionOnGameThread()
{
    AActor* jason =
        g_JasonAIState.Jason;

    if (!jason ||
        !Memory::IsReadable(
            jason,
            sizeof(UObject)))
    {
        return false;
    }

    //
    // Don't hammer the native interaction routine
    // every controller tick.
    //
    static ULONGLONG nextDoorAttemptAt =
        0;

    ULONGLONG now =
        GetTickCount64();

    if (now <
        nextDoorAttemptAt)
    {
        return false;
    }

    //
    // A new door break requires one genuine low-movement check.
    // Once that exact door is confirmed, follow-up pulses are allowed
    // immediately.
    //
    if (!g_JasonAIState.
            DoorBreakActive &&
        g_JasonAIState.
            ConsecutiveStuckChecks < 1)
    {
        return false;
    }

    //
    // Confirmed Resurrected:
    //
    // Jason +0xE18 =
    // SCInteractableManagerComponent*
    //
    UObject** managerPtr =
        (UObject**)
        ((uintptr_t)jason +
            0xE18);

    if (!Memory::IsReadable(
        managerPtr,
        sizeof(UObject*)))
    {
        return false;
    }

    UObject* manager =
        *managerPtr;

    if (!manager ||
        !Memory::IsReadable(
            manager,
            sizeof(UObject)))
    {
        return false;
    }

    //
    // Resurrected FUN_140410AF0:
    //
    // BestInteractable = manager +0x230
    // fallback         = manager +0x210
    //
    UObject* interactable =
        nullptr;

    UObject** bestPtr =
        (UObject**)
        ((uintptr_t)manager +
            0x230);

    if (Memory::IsReadable(
        bestPtr,
        sizeof(UObject*)))
    {
        interactable =
            *bestPtr;
    }

    if (!interactable)
    {
        UObject** fallbackPtr =
            (UObject**)
            ((uintptr_t)manager +
                0x210);

        if (Memory::IsReadable(
            fallbackPtr,
            sizeof(UObject*)))
        {
            interactable =
                *fallbackPtr;
        }
    }

    if (!interactable ||
        !Memory::IsReadable(
            interactable,
            sizeof(UObject)))
    {
        return false;
    }

    //
    // Interactable +0xE0 =
    // owning actor.
    //
    AActor** ownerPtr =
        (AActor**)
        ((uintptr_t)interactable +
            0xE0);

    if (!Memory::IsReadable(
        ownerPtr,
        sizeof(AActor*)))
    {
        return false;
    }

    AActor* owner =
        *ownerPtr;

    if (!owner ||
        !Memory::IsReadable(
            owner,
            sizeof(UObject)) ||
        !owner->Class ||
        !Memory::IsReadable(
            owner->Class,
            sizeof(UStruct)))
    {
        return false;
    }

    //
    // Only invoke the native interaction routine
    // when the selected owner derives from SCDoor.
    //
    bool isDoor =
        false;

    for (UStruct* current =
        (UStruct*)owner->Class;
        current;
        current = current->Super)
    {
        if (!Memory::IsReadable(
            current,
            sizeof(UStruct)))
        {
            break;
        }

        if (JasonAISafeName(
            (UObject*)current
        ) ==
            "SCDoor")
        {
            isDoor =
                true;

            break;
        }
    }

    if (!isDoor)
    {
        return false;
    }

    if (g_JasonAIState.
            DoorBreakActive &&
        g_JasonAIState.
            DoorBreakTarget &&
        owner !=
            g_JasonAIState.
                DoorBreakTarget)
    {
        return false;
    }

    //
    // Interior doors can remain the selected interactable for several
    // samples after a break/open pulse.  Do not immediately restart the
    // exact same interior-door sequence; let the accepted MoveTo advance
    // through the doorway first.
    //
    if (!g_JasonAIState.DoorBreakActive &&
        g_JasonAIState.DoorRetryTarget == owner &&
        now < g_JasonAIState.DoorRetryAfter)
    {
        return false;
    }

    //
    // Because we only reach this function after a genuine low-movement
    // stuck sample, the selected SCDoor is already a strong
    // obstruction candidate. Keep one additional cheap safeguard:
    // the door actor must actually be close to Jason.
    //
    FVector jasonLocation{};
    FVector doorLocation{};

    if (!GetJasonAIActorLocation(
            jason,
            jasonLocation) ||
        !GetJasonAIActorLocation(
            owner,
            doorLocation))
    {
        return false;
    }

    float doorDX =
        doorLocation.X -
        jasonLocation.X;

    float doorDY =
        doorLocation.Y -
        jasonLocation.Y;

    float doorDZ =
        doorLocation.Z -
        jasonLocation.Z;

    float doorDistanceSquared =
        (doorDX * doorDX) +
        (doorDY * doorDY) +
        (doorDZ * doorDZ);

    constexpr float
        MaxBlockedDoorDistance =
        500.0f;

    if (!std::isfinite(
            doorDistanceSquared) ||
        doorDistanceSquared >
            (MaxBlockedDoorDistance *
                MaxBlockedDoorDistance))
    {
        return false;
    }

    nextDoorAttemptAt =
        now + 140;

    //
    // Resurrected:
    //
    // FUN_140410AF0
    //
    // This is the native Jason interaction routine.
    // Its SCDoor branch:
    //
    //   -> FUN_14042C250
    //   -> SERVER_StartBreakingDownDoor
    //   -> manager +0x418
    //   -> Jason +0x1680 = Door
    //   -> SERVER_SetBreakDownActor
    //
    HMODULE module =
        GetModuleHandle(nullptr);

    if (!module)
    {
        return false;
    }

    constexpr uintptr_t
        RVA_JasonInteraction =
        0x00410AF0;

    uintptr_t interactionAddress =
        (uintptr_t)module +
        RVA_JasonInteraction;

    if (!Memory::IsReadable(
        (void*)interactionAddress,
        1))
    {
        return false;
    }

    // Jason +0x1680 is the stock door-break target. An already-finished door
    // can leave the same pointer behind after our pulse state has cleared;
    // without clearing that stale value, a later normal open is misread as a
    // fresh breakdown and the custom pulse tears through an unlocked door.
    AActor** nativeBreakTargetPtr =
        reinterpret_cast<AActor**>(
            reinterpret_cast<uintptr_t>(jason) +
            0x1680);
    const bool clearedStaleBreakTarget =
        !g_JasonAIState.DoorBreakActive &&
        Memory::IsReadable(nativeBreakTargetPtr, sizeof(AActor*)) &&
        *nativeBreakTargetPtr == owner;
    if (clearedStaleBreakTarget)
        *nativeBreakTargetPtr = nullptr;

    bool callOK =
        SafeJasonInteractionCall(
            jason,
            interactionAddress
        );

    if (callOK)
    {
        if (!g_JasonAIState.
                DoorBreakActive)
        {
            // The native interaction routine already chooses the correct
            // door action: unlocked doors are opened normally, while locked
            // or barricaded doors publish their actor at Jason +0x1680 and
            // begin the breakdown special move.  Do not force every
            // successful door interaction into our repeated break-pulse
            // state.
            AActor* nativeBreakTarget =
                nullptr;

            if (Memory::IsReadable(
                    nativeBreakTargetPtr,
                    sizeof(AActor*)))
            {
                nativeBreakTarget =
                    *nativeBreakTargetPtr;
            }

            const bool nativeBreakStarted =
                nativeBreakTarget == owner;

            if (!nativeBreakStarted)
            {
                // The door was accepted without a breakdown target, which is
                // the normal unlocked-door path. Give the animation and
                // threshold traversal time to complete, and suppress an
                // immediate same-door retry.
                g_JasonAIState.DoorRetryTarget =
                    owner;
                // The interaction manager can keep an already-open exterior
                // door selected long after Jason has crossed its threshold.
                // A twelve-second retry let a later stuck sample reinterpret
                // that same open door as a new breakdown target.  Retain the
                // known-open result for a full traversal/repath window.
                g_JasonAIState.DoorRetryAfter =
                    now + 60000;
                g_JasonAIState.DoorTraversalGraceUntil =
                    now + 12000;

                Logger::Debug(
                    std::string(
                        "Jason AI unlocked door opened through native interaction | door=") +
                    JasonAISafeName(
                        reinterpret_cast<UObject*>(owner)) +
                    " | clearedStaleBreakTarget=" +
                    (clearedStaleBreakTarget ? "true" : "false"));

                return true;
            }

            std::string doorName =
                JasonAISafeName(
                    (UObject*)owner
                );

            bool isInteriorDoor =
                doorName.find(
                    "Interior"
                ) !=
                std::string::npos;

            g_JasonAIState.
                DoorBreakActive =
                true;

            g_JasonAIState.
                DoorBreakTarget =
                owner;

            g_JasonAIState.
                DoorBreakInterior =
                isInteriorDoor;

            g_JasonAIState.
                DoorBreakPulseEndAt =
                now + 1250;

            if (isInteriorDoor)
            {
                // Cover the animation plus the first several seconds
                // of indoor threshold traversal.
                g_JasonAIState.
                    DoorTraversalGraceUntil =
                    now + 9000;
            }
        }

        g_JasonAIState.
            NextDoorBreakPulseAt =
            now + 160;
    }

    static AActor* lastLoggedDoor =
        nullptr;

    if (owner !=
        lastLoggedDoor)
    {
        lastLoggedDoor =
            owner;

        Logger::Debug(
            std::string(
                "Jason AI door interaction: native call="
            ) +
            (callOK ?
                "true" :
                "false") +
            " | door=" +
            JasonAISafeName(
                (UObject*)owner
            )
        );
    }

    return callOK;
}


static void ClearJasonAIDoorBreakState()
{
    g_JasonAIState.DoorBreakActive = false;
    g_JasonAIState.DoorBreakTarget = nullptr;
    g_JasonAIState.NextDoorBreakPulseAt = 0;
    g_JasonAIState.DoorBreakPulseEndAt = 0;
    g_JasonAIState.DoorBreakInterior = false;
}

static void FinishJasonAIDoorBreakState(
    ULONGLONG now)
{
    // Exterior doors are already behaving well, so their post-pulse
    // behavior remains unchanged.  Only interior doors get traversal
    // grace and a cheap same-door retry cooldown.
    if (g_JasonAIState.DoorBreakInterior &&
        g_JasonAIState.DoorBreakTarget)
    {
        g_JasonAIState.DoorRetryTarget =
            g_JasonAIState.DoorBreakTarget;

        g_JasonAIState.DoorRetryAfter =
            now + 2200;

        g_JasonAIState.DoorTraversalGraceUntil =
            now + 7000;
    }

    ClearJasonAIDoorBreakState();
}

static bool TickJasonAIDoorBreakOnGameThread()
{
    if (!g_JasonAIState.DoorBreakActive)
        return false;

    ULONGLONG now =
        GetTickCount64();

    if (now >=
        g_JasonAIState.DoorBreakPulseEndAt)
    {
        Logger::Debug(
            "Jason AI door break pulse window ended"
        );

        FinishJasonAIDoorBreakState(now);
        return false;
    }

    if (now <
        g_JasonAIState.NextDoorBreakPulseAt)
    {
        return true;
    }

    bool pulseOK =
        RunJasonAIDoorInteractionOnGameThread();

    if (!pulseOK)
    {
        Logger::Debug(
            "Jason AI door break pulse stopped: blocking door no longer selected"
        );

        FinishJasonAIDoorBreakState(now);
        return false;
    }

    Logger::Debug(
        "Jason AI door break pulse=true"
    );

    return true;
}



static bool PrepareJasonAIKnifeStock(
    AActor* jason,
    int32_t& outBefore,
    int32_t& outAfter)
{
    outBefore = -1;
    outAfter = -1;

    if (!jason ||
        !Memory::IsReadable(
            jason,
            sizeof(UObject)))
    {
        return false;
    }

    //
    // Resurrected GetKnifeCount reads:
    //     DWORD PTR [Jason + 0x15EC]
    //
    // ThrowKnife later decrements the same field.
    //
    constexpr uintptr_t
        Offset_KnifeCount =
        0x15EC;

    int32_t* knifeCount =
        (int32_t*)
        ((uintptr_t)jason +
            Offset_KnifeCount);

    if (!Memory::IsReadable(
        knifeCount,
        sizeof(int32_t)))
    {
        return false;
    }

    outBefore =
        *knifeCount;

    //
    // Sandbox-spawned Jason may start with no knife stock.
    // Give the AI a small finite supply.
    //
    if (outBefore <= 0 ||
        outBefore > 20)
    {
        *knifeCount = 6;
    }

    outAfter =
        *knifeCount;

    return
        outAfter > 0 &&
        outAfter <= 20;
}



// ------------------------------------------------------------
// Features38: AI knife visibility / flight diagnostic.
//
// Features37 proved the AI knife reaches the exact same ThrowingKnife_C
// +0x5F0 -> +0x5E8 launch path as a visible human throw, receives a real
// ~12000 velocity, and is detached from Jason afterward.  The remaining
// unknown is whether the projectile actor/component is actually ticking
// and rendering after that handoff.
//
// Keep the launched driver alive only as a NON-owning diagnostic pointer;
// Jason's stock +0x10F0 reference is still allowed to clear normally.
// ------------------------------------------------------------







static bool LaunchJasonAIKnifeProjectileOnGameThread(
    AActor* jason,
    AActor* target,
    float& outThrowSpeed,
    float& outDriverDistance,
    bool& outServerGateExpected)
{
    outThrowSpeed = 0.0f;
    outDriverDistance = -1.0f;
    outServerGateExpected = false;

    if (!jason ||
        !target ||
        !Memory::IsReadable(
            jason,
            sizeof(UObject)) ||
        !Memory::IsReadable(
            target,
            sizeof(UObject)))
    {
        return false;
    }

    HMODULE module =
        GetModuleHandle(nullptr);

    if (!module)
        return false;

    // The human-Jason baseline showed that a real throw has +0x15F8
    // cleared before SCThrowable::Use builds its launch vectors and enters
    // SERVER_Use. AI Jason has no player camera, so do not reuse the
    // potentially stale Jason +0x7D0 camera transform. Aim the already-
    // spawned ThrowingKnife_C at the counselor, then use the stock server
    // dispatch below.
    constexpr uintptr_t Offset_KnifeDriver = 0x10F0;
    constexpr uintptr_t Offset_ThrowSpeed = 0x10E8;

    void** driverPtr =
        (void**)
        ((uintptr_t)jason +
            Offset_KnifeDriver);

    if (!Memory::IsReadable(
            driverPtr,
            sizeof(void*)) ||
        !*driverPtr ||
        !Memory::IsReadable(
            *driverPtr,
            sizeof(UObject)))
    {
        return false;
    }

    void* knifeDriver =
        *driverPtr;

    FVector jasonLocation{};
    FVector driverLocation{};
    FVector targetLocation{};

    // Features41: the notify-time knife driver no longer has a reliable
    // world-space transform for AI Jason.  In the Features40 log the
    // driver was 8,000-11,000 uu away even though the knife was visibly
    // in Jason's hand.  Treat its actor location as diagnostic only.
    if (!GetJasonAIActorLocation(
            jason,
            jasonLocation) ||
        !GetJasonAIActorLocation(
            target,
            targetLocation))
    {
        return false;
    }

    bool haveDriverLocation =
        GetJasonAIActorLocation(
            (AActor*)knifeDriver,
            driverLocation);

    if (haveDriverLocation)
    {
        float driverDx =
            driverLocation.X - jasonLocation.X;
        float driverDy =
            driverLocation.Y - jasonLocation.Y;
        float driverDz =
            driverLocation.Z - jasonLocation.Z;

        float driverDistanceSquared =
            (driverDx * driverDx) +
            (driverDy * driverDy) +
            (driverDz * driverDz);

        if (std::isfinite(driverDistanceSquared) &&
            driverDistanceSquared >= 0.0f)
        {
            outDriverDistance =
                std::sqrt(driverDistanceSquared);
        }
    }

    outServerGateExpected =
        *driverPtr == knifeDriver &&
        haveDriverLocation &&
        std::isfinite(outDriverDistance) &&
        outDriverDistance >= 0.0f &&
        outDriverDistance < 400.0f;

    float* throwSpeedPtr =
        (float*)
        ((uintptr_t)jason +
            Offset_ThrowSpeed);

    if (!Memory::IsReadable(
            throwSpeedPtr,
            sizeof(float)))
    {
        return false;
    }

    float throwSpeed =
        *throwSpeedPtr;

    if (!std::isfinite(throwSpeed) ||
        throwSpeed < 100.0f ||
        throwSpeed > 20000.0f)
    {
        return false;
    }

    // Same upper-torso target used by the existing AI aim code and the
    // supplied Aimbot.cpp.  The aimbot's camera/mouse path is player-only,
    // but its world-space target math is appropriate here.
    targetLocation.Z += 60.0f;

    // Build a synthetic throwing-hand origin entirely in WORLD SPACE.
    // Actor location is the only transform we know is valid for this
    // non-local AI pawn at the stock notify frame.
    FVector launchBase =
        jasonLocation;

    constexpr float KnifeHandHeight = 75.0f;
    launchBase.Z += KnifeHandHeight;

    float aimX =
        targetLocation.X - launchBase.X;
    float aimY =
        targetLocation.Y - launchBase.Y;
    float aimZ =
        targetLocation.Z - launchBase.Z;

    float aimLengthSquared =
        (aimX * aimX) +
        (aimY * aimY) +
        (aimZ * aimZ);

    if (!std::isfinite(aimLengthSquared) ||
        aimLengthSquared < 1.0f)
    {
        return false;
    }

    float invAimLength =
        1.0f / std::sqrt(aimLengthSquared);

    FVector direction{};
    direction.X = aimX * invAimLength;
    direction.Y = aimY * invAimLength;
    direction.Z = aimZ * invAimLength;

    // Put the projectile just in front of Jason's chest/throwing hand so
    // it starts outside his capsule instead of at the driver's attachment-
    // relative coordinates.
    FVector launchLocation =
        launchBase;

    constexpr float KnifeForwardClearance = 110.0f;
    launchLocation.X += direction.X * KnifeForwardClearance;
    launchLocation.Y += direction.Y * KnifeForwardClearance;
    launchLocation.Z += direction.Z * KnifeForwardClearance;

    // Recalculate from the FINAL world-space launch point so the velocity
    // still intersects the counselor's upper torso exactly.
    aimX =
        targetLocation.X - launchLocation.X;
    aimY =
        targetLocation.Y - launchLocation.Y;
    aimZ =
        targetLocation.Z - launchLocation.Z;

    aimLengthSquared =
        (aimX * aimX) +
        (aimY * aimY) +
        (aimZ * aimZ);

    if (!std::isfinite(aimLengthSquared) ||
        aimLengthSquared < 1.0f)
    {
        return false;
    }

    invAimLength =
        1.0f / std::sqrt(aimLengthSquared);

    direction.X = aimX * invAimLength;
    direction.Y = aimY * invAimLength;
    direction.Z = aimZ * invAimLength;

    FVector velocity{};
    velocity.X = direction.X * throwSpeed;
    velocity.Y = direction.Y * throwSpeed;
    velocity.Z = direction.Z * throwSpeed;

    // The autonomous knife actor can retain an attachment-relative transform
    // whose reported world location is thousands of units from Jason. The
    // stock SERVER_Use_Implementation rejects that before multicast launch.
    // Put the already-created actor at the verified throwing-hand origin so
    // the normal native distance gate and launch path can succeed.
    bool driverPlacementOK = false;
    AActor* knifeActor = reinterpret_cast<AActor*>(knifeDriver);
    if (knifeActor->Class)
    {
        UFunction* teleportFunction =
            FindFunctionInHierarchyByName(
                knifeActor->Class,
                "K2_TeleportTo");
        if (teleportFunction)
        {
            struct Rotation3
            {
                float Pitch;
                float Yaw;
                float Roll;
            };
            struct TeleportParams
            {
                FVector DestLocation;
                Rotation3 DestRotation;
                bool ReturnValue;
            };
            static_assert(sizeof(TeleportParams) == 28,
                "Knife TeleportParams must be 28 bytes");

            constexpr float RadiansToDegrees =
                57.29577951308232f;
            const float horizontal = std::sqrt(
                (direction.X * direction.X) +
                (direction.Y * direction.Y));
            TeleportParams params{};
            params.DestLocation = launchLocation;
            params.DestRotation.Pitch =
                std::atan2(direction.Z, horizontal) *
                RadiansToDegrees;
            params.DestRotation.Yaw =
                std::atan2(direction.Y, direction.X) *
                RadiansToDegrees;
            driverPlacementOK =
                SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(knifeActor),
                    knifeActor,
                    teleportFunction,
                    &params) &&
                params.ReturnValue;
        }
    }

    // Human baseline difference: the real local SCThrowable::Use path
    // runs 0x40A7B0 before +0x5F0.  Besides building the stock forward
    // velocity, that helper sets throwable +0x3E8 = Jason before the
    // multicast implementation is entered.  Preserve that exact owner
    // ordering, then keep our counselor-directed velocity for aiming.
    FVector nativeVelocityScratch{};
    bool nativeOwnerPrepOK =
        SafeSCThrowableBuildVelocityCall(
            (uintptr_t)module + 0x0040A7B0,
            knifeDriver,
            &nativeVelocityScratch,
            jason);

    // Autonomous Jason is not player-owned, so the RPC wrapper drops this
    // request. Enter the stock native server implementation directly; it
    // retains validation and dispatches the normal multicast launch path.
    constexpr uintptr_t RVA_SCThrowableServerUseImplementation =
        0x00421A00;

    bool callOK =
        driverPlacementOK &&
        SafeSCThrowableServerUseImplementationCall(
            (uintptr_t)module +
                RVA_SCThrowableServerUseImplementation,
            knifeDriver,
            jason,
            &velocity,
            &launchLocation
        );

    // Preserve stock visibility, activation, tick, collision, and movement
    // ownership.  This helper supplies only the AI-safe world-space launch
    // vectors before handing control to the normal server path.

    outThrowSpeed =
        std::sqrt(
            (velocity.X * velocity.X) +
            (velocity.Y * velocity.Y) +
            (velocity.Z * velocity.Z)
        );

    Logger::Debug(
        std::string(
            "Jason AI knife local launch: ok=") +
        (callOK ? "true" : "false") +
        " | driverPlaced=" +
        (driverPlacementOK ? "true" : "false") +
        " | ownerPrep=" +
        (nativeOwnerPrepOK ? "true" : "false") +
        " | pressed=" +
        std::to_string(
            Memory::IsReadable(
                (void*)((uintptr_t)jason + 0x15F8),
                1) ?
            (int)*(uint8_t*)((uintptr_t)jason + 0x15F8) :
            -1) +
        " | driverDist=" +
        std::to_string(outDriverDistance) +
        " | launchFromJason=" +
        std::to_string(
            std::sqrt(
                ((launchLocation.X - jasonLocation.X) *
                 (launchLocation.X - jasonLocation.X)) +
                ((launchLocation.Y - jasonLocation.Y) *
                 (launchLocation.Y - jasonLocation.Y)) +
                ((launchLocation.Z - jasonLocation.Z) *
                 (launchLocation.Z - jasonLocation.Z))
            )
        ) +
        " | launch=" +
        std::to_string(launchLocation.X) + "," +
        std::to_string(launchLocation.Y) + "," +
        std::to_string(launchLocation.Z) +
        " | velocity=" +
        std::to_string(velocity.X) + "," +
        std::to_string(velocity.Y) + "," +
        std::to_string(velocity.Z)
    );

    return callOK;
}

static bool AimJasonAtKnifeTargetOnGameThread(
    AActor* target,
    bool logResult)
{
    AActor* jason =
        g_JasonAIState.Jason;

    UObject* controller =
        g_JasonAIState.Controller;

    if (!jason ||
        !target ||
        !Memory::IsReadable(jason, sizeof(UObject)) ||
        !Memory::IsReadable(target, sizeof(UObject)))
    {
        return false;
    }

    FVector jasonLocation{};
    FVector targetLocation{};

    if (!GetJasonAIActorLocation(jason, jasonLocation) ||
        !GetJasonAIActorLocation(target, targetLocation))
    {
        return false;
    }

    // Aim toward upper torso.  Keep Jason's body upright while the
    // controller/view rotation carries the vertical component.
    targetLocation.Z += 60.0f;

    float dx = targetLocation.X - jasonLocation.X;
    float dy = targetLocation.Y - jasonLocation.Y;
    float dz = targetLocation.Z - jasonLocation.Z;

    float horizontal =
        std::sqrt((dx * dx) + (dy * dy));

    if (!std::isfinite(horizontal) ||
        horizontal < 1.0f)
    {
        return false;
    }

    constexpr float RadiansToDegrees =
        57.29577951308232f;

    struct Rotation3
    {
        float Pitch;
        float Yaw;
        float Roll;
    };

    Rotation3 aimRotation{};
    aimRotation.Pitch =
        std::atan2(dz, horizontal) *
        RadiansToDegrees;
    aimRotation.Yaw =
        std::atan2(dy, dx) *
        RadiansToDegrees;
    aimRotation.Roll = 0.0f;

    bool controllerRotationOK = false;
    bool actorRotationOK = false;
    bool throwViewRotationOK = false;

    // Keep the AI controller's view/control rotation pointed at the
    // counselor throughout the wind-up.  Cache the reflected function.
    if (controller &&
        controller->Class &&
        Memory::IsReadable(controller, sizeof(UObject)))
    {
        static UClass* cachedControllerClass = nullptr;
        static UFunction* setControlRotationFunction = nullptr;

        if (cachedControllerClass != controller->Class)
        {
            cachedControllerClass = controller->Class;
            setControlRotationFunction =
                FindFunctionInHierarchyByName(
                    controller->Class,
                    "SetControlRotation"
                );
        }

        if (setControlRotationFunction)
        {
            struct SetControlRotationParams
            {
                Rotation3 NewRotation;
            };

            SetControlRotationParams params{};
            params.NewRotation = aimRotation;

            controllerRotationOK =
                SafeProcessEventCall(
                    (uintptr_t)controller,
                    controller,
                    setControlRotationFunction,
                    &params
                );
        }
    }

    // Also face the pawn's body toward the target.  Only yaw is applied
    // to the actor so Jason does not lean forward/backward unnaturally.
    if (jason->Class)
    {
        static UClass* cachedJasonClass = nullptr;
        static UFunction* setActorRotationFunction = nullptr;

        if (cachedJasonClass != jason->Class)
        {
            cachedJasonClass = jason->Class;
            setActorRotationFunction =
                FindFunctionInHierarchyByName(
                    jason->Class,
                    "K2_SetActorRotation"
                );
        }

        if (setActorRotationFunction)
        {
            struct SetActorRotationParams
            {
                Rotation3 NewRotation;
                bool bTeleportPhysics;
                uint8_t Padding0[3];
                bool ReturnValue;
                uint8_t Padding1[3];
            };

            SetActorRotationParams params{};
            params.NewRotation.Pitch = 0.0f;
            params.NewRotation.Yaw = aimRotation.Yaw;
            params.NewRotation.Roll = 0.0f;
            params.bTeleportPhysics = false;

            bool processOK =
                SafeProcessEventCall(
                    (uintptr_t)jason,
                    jason,
                    setActorRotationFunction,
                    &params
                );

            actorRotationOK =
                processOK && params.ReturnValue;
        }
    }

    // The stock local-player knife helper derives its forward direction
    // from Jason+0x7D0.  AI Jason has no player camera, so keep only the
    // rotator portion current while preserving the game's location data.
    Rotation3* throwViewRotation =
        (Rotation3*)
        ((uintptr_t)jason + 0x7D0 + 0x0C);

    if (Memory::IsReadable(
            throwViewRotation,
            sizeof(Rotation3)))
    {
        *throwViewRotation = aimRotation;
        throwViewRotationOK = true;
    }

    if (logResult)
    {
        Logger::Debug(
            std::string(
                "Jason AI knife aim: ControlRotation=") +
            (controllerRotationOK ? "true" : "false") +
            " | ActorRotation=" +
            (actorRotationOK ? "true" : "false") +
            " | ThrowView=" +
            (throwViewRotationOK ? "true" : "false") +
            " | pitch=" +
            std::to_string(aimRotation.Pitch) +
            " | yaw=" +
            std::to_string(aimRotation.Yaw)
        );
    }

    return
        controllerRotationOK ||
        actorRotationOK ||
        throwViewRotationOK;
}

static bool StopJasonAIMovementForKnifeOnGameThread()
{
    UObject* controller =
        g_JasonAIState.Controller;

    if (!controller ||
        !controller->Class ||
        !Memory::IsReadable(
            controller,
            sizeof(UObject)))
    {
        return false;
    }

    // Cache the reflected AIController StopMovement function once per
    // controller class.  This is not a world scan and keeps the knife
    // animation from being immediately blended against another MoveTo.
    static UClass* cachedControllerClass = nullptr;
    static UFunction* stopMovementFunction = nullptr;

    if (cachedControllerClass != controller->Class)
    {
        cachedControllerClass = controller->Class;
        stopMovementFunction =
            FindFunctionInHierarchyByName(
                controller->Class,
                "StopMovement"
            );
    }

    if (!stopMovementFunction)
        return false;

    return SafeProcessEventCall(
        (uintptr_t)controller,
        controller,
        stopMovementFunction,
        nullptr
    );
}

static bool TickJasonAIKnifeSequenceOnGameThread(
    ULONGLONG now)
{
    if (!g_JasonAIState.KnifeSequenceActive)
        return false;

    AActor* jason = g_JasonAIState.Jason;

    auto clearKnifeSequence = [&]()
    {
        g_JasonAIState.KnifeSequenceActive = false;
        g_JasonAIState.KnifePerformSent = false;
        g_JasonAIState.KnifeNotifyLaunchSent = false;
        g_JasonAIState.KnifeTarget = nullptr;
        g_JasonAIState.KnifePerformAt = 0;
        g_JasonAIState.KnifeEndAt = 0;
        g_JasonAIState.KnifeReleaseSentAt = 0;
        g_JasonAIState.KnifeCountAtRelease = -1;
        g_JasonAIState.KnifeDriverAtRelease = nullptr;
    };

    if (!jason ||
        !Memory::IsReadable(jason, sizeof(UObject)))
    {
        clearKnifeSequence();
        return false;
    }

    HMODULE module = GetModuleHandle(nullptr);
    if (!module)
        return true;

    // Features40 preserves Features39 stock animation timing.  The working
    // OfflineBots EXE showed the stock killer input path owns the throw
    // transition.  We therefore release the armed knife and wait for the
    // real montage/AnimNotify path to call ThrowKnife/+0x5F0/+0x5E8 itself.
    constexpr uintptr_t RVA_ReleaseKnifeThrow = 0x0041B0D0;
    constexpr uintptr_t RVA_FinalizeKnifeDriver = 0x0041FE60;
    constexpr uintptr_t RVA_ServerEndThrow = 0x004CA070;

    constexpr uintptr_t Offset_KnifeCount = 0x15EC;
    constexpr uintptr_t Offset_KnifePressed = 0x15F8;
    constexpr uintptr_t Offset_KnifeDriver = 0x10F0;

    int32_t* knifeCount =
        (int32_t*)((uintptr_t)jason + Offset_KnifeCount);
    uint8_t* pressedPtr =
        (uint8_t*)((uintptr_t)jason + Offset_KnifePressed);
    void** driverPtr =
        (void**)((uintptr_t)jason + Offset_KnifeDriver);

    // Features40: keep Jason facing the counselor during the entire
    // wind-up/release window instead of merely stopping MoveTo.
    if (g_JasonAIState.KnifeTarget)
    {
        AimJasonAtKnifeTargetOnGameThread(
            g_JasonAIState.KnifeTarget,
            false
        );
    }

    if (!g_JasonAIState.KnifePerformSent &&
        now >= g_JasonAIState.KnifePerformAt)
    {
        int32_t countBefore =
            Memory::IsReadable(knifeCount, sizeof(int32_t)) ?
            *knifeCount : -1;
        int32_t pressedBefore =
            Memory::IsReadable(pressedPtr, 1) ?
            (int32_t)*pressedPtr : -1;
        void* driverBefore =
            Memory::IsReadable(driverPtr, sizeof(void*)) ?
            *driverPtr : nullptr;

        uintptr_t releaseAddress =
            (uintptr_t)module + RVA_ReleaseKnifeThrow;

        bool releaseOK =
            Memory::IsReadable((void*)releaseAddress, 1) &&
            SafeJasonInteractionCall(jason, releaseAddress);

        int32_t countAfter =
            Memory::IsReadable(knifeCount, sizeof(int32_t)) ?
            *knifeCount : -1;
        int32_t pressedAfter =
            Memory::IsReadable(pressedPtr, 1) ?
            (int32_t)*pressedPtr : -1;
        void* driverAfter =
            Memory::IsReadable(driverPtr, sizeof(void*)) ?
            *driverPtr : nullptr;

        g_JasonAIState.KnifePerformSent = true;
        g_JasonAIState.KnifeReleaseSentAt = now;
        g_JasonAIState.KnifeCountAtRelease = countBefore;
        g_JasonAIState.KnifeDriverAtRelease = driverBefore;

        Logger::Debug(
            std::string(
                "Jason AI knife stock release: ReleaseKnife=") +
            (releaseOK ? "true" : "false") +
            " | pressed=" +
            std::to_string(pressedBefore) + "->" +
            std::to_string(pressedAfter) +
            " | count=" +
            std::to_string(countBefore) + "->" +
            std::to_string(countAfter) +
            " | driver=" +
            std::to_string((uintptr_t)driverBefore) + "->" +
            std::to_string((uintptr_t)driverAfter) +
            " | waiting-for-stock-notify=true"
        );

        return true;
    }

    if (g_JasonAIState.KnifePerformSent)
    {
        int32_t currentCount =
            Memory::IsReadable(knifeCount, sizeof(int32_t)) ?
            *knifeCount : -1;
        int32_t currentPressed =
            Memory::IsReadable(pressedPtr, 1) ?
            (int32_t)*pressedPtr : -1;
        void* currentDriver =
            Memory::IsReadable(driverPtr, sizeof(void*)) ?
            *driverPtr : nullptr;

        bool countConsumed =
            g_JasonAIState.KnifeCountAtRelease >= 0 &&
            currentCount >= 0 &&
            currentCount < g_JasonAIState.KnifeCountAtRelease;

        bool driverReleased =
            g_JasonAIState.KnifeDriverAtRelease != nullptr &&
            currentDriver != g_JasonAIState.KnifeDriverAtRelease;

        // Features40: the user confirmed Features39's montage/release
        // timing looks correct.  The stock AI path consumes one knife
        // roughly at the animation notify frame, but never calls the local
        // +0x5F0/+0x5E8 launch branch because this pawn is not locally
        // controlled.  Treat the stock count decrement as the exact notify
        // signal and inject ONLY that missing local launch transition here.
        // Do not call ThrowKnife again; the stock notify already consumed it.
        if (countConsumed &&
            !g_JasonAIState.KnifeNotifyLaunchSent)
        {
            g_JasonAIState.KnifeNotifyLaunchSent = true;

            float throwSpeed = 0.0f;
            float driverDistance = -1.0f;
            bool serverGateExpected = false;

            bool aimOK =
                g_JasonAIState.KnifeTarget &&
                AimJasonAtKnifeTargetOnGameThread(
                    g_JasonAIState.KnifeTarget,
                    false
                );

            bool localLaunchOK =
                g_JasonAIState.KnifeTarget &&
                LaunchJasonAIKnifeProjectileOnGameThread(
                    jason,
                    g_JasonAIState.KnifeTarget,
                    throwSpeed,
                    driverDistance,
                    serverGateExpected
                );

            void* driverAfterLaunch =
                Memory::IsReadable(driverPtr, sizeof(void*)) ?
                *driverPtr : nullptr;

            bool finalizeAttempted = false;
            bool finalizeOK = false;

            if (localLaunchOK &&
                g_JasonAIState.KnifeDriverAtRelease &&
                driverAfterLaunch ==
                    g_JasonAIState.KnifeDriverAtRelease)
            {
                uintptr_t finalizeAddress =
                    (uintptr_t)module +
                    RVA_FinalizeKnifeDriver;

                finalizeAttempted = true;
                finalizeOK =
                    Memory::IsReadable(
                        (void*)finalizeAddress,
                        1) &&
                    SafeJasonInteractionCall(
                        jason,
                        finalizeAddress
                    );
            }

            currentDriver =
                Memory::IsReadable(driverPtr, sizeof(void*)) ?
                *driverPtr : nullptr;

            driverReleased =
                g_JasonAIState.KnifeDriverAtRelease != nullptr &&
                currentDriver !=
                    g_JasonAIState.KnifeDriverAtRelease;

            Logger::Debug(
                std::string(
                    "Jason AI knife stock notify launch: Aim=") +
                (aimOK ? "true" : "false") +
                " | LocalLaunch=" +
                (localLaunchOK ? "true" : "false") +
                " | FinalizeDriver=" +
                (finalizeAttempted ?
                    (finalizeOK ? "true" : "false") :
                    "not-needed") +
                " | count=" +
                std::to_string(
                    g_JasonAIState.KnifeCountAtRelease) +
                "->" +
                std::to_string(currentCount) +
                " | driver=" +
                std::to_string(
                    (uintptr_t)
                    g_JasonAIState.KnifeDriverAtRelease) +
                "->" +
                std::to_string((uintptr_t)currentDriver) +
                " | speed=" +
                std::to_string(throwSpeed) +
                " | driverDist=" +
                std::to_string(driverDistance) +
                " | afterReleaseMs=" +
                std::to_string(
                    g_JasonAIState.KnifeReleaseSentAt ?
                    (now - g_JasonAIState.KnifeReleaseSentAt) :
                    0)
            );
        }

        if (countConsumed || driverReleased)
        {
            // The AI pawn does not receive the local-player montage's final
            // EndThrow callback.  The projectile/driver transition can happen
            // before the montage has finished, so give it a short settle
            // window, then invoke the same stock completion used by the
            // timeout recovery.  Without this, later knife presses stay
            // unarmed and melee/interaction inputs can be ignored.
            if (g_JasonAIState.KnifeReleaseSentAt != 0 &&
                now < g_JasonAIState.KnifeReleaseSentAt + 500)
            {
                return true;
            }

            uintptr_t endAddress =
                (uintptr_t)module + RVA_ServerEndThrow;
            bool endOK =
                Memory::IsReadable((void*)endAddress, 1) &&
                SafeJasonInteractionCall(jason, endAddress);

            Logger::Debug(
                std::string(
                    "Jason AI knife stock animation completed: countConsumed=") +
                (countConsumed ? "true" : "false") +
                " | driverReleased=" +
                (driverReleased ? "true" : "false") +
                " | count=" +
                std::to_string(g_JasonAIState.KnifeCountAtRelease) +
                "->" + std::to_string(currentCount) +
                " | pressed=" +
                std::to_string(currentPressed) +
                " | driver=" +
                std::to_string((uintptr_t)g_JasonAIState.KnifeDriverAtRelease) +
                "->" + std::to_string((uintptr_t)currentDriver) +
                " | EndThrow=" + (endOK ? "true" : "false") +
                " | afterReleaseMs=" +
                std::to_string(
                    g_JasonAIState.KnifeReleaseSentAt ?
                    (now - g_JasonAIState.KnifeReleaseSentAt) : 0)
            );

            clearKnifeSequence();
            // Re-open the chase path immediately after the stock throw has
            // consumed its driver.  This prevents an idle throw from leaving
            // Jason parked until some unrelated navigation event wakes him.
            g_JasonAIState.PathLocked = false;
            g_JasonAIState.PathLockUntil = 0;
            g_JasonAIState.NextCombatAttemptAt = now + 500;
            return true;
        }
    }

    // Only use EndThrow as a late recovery if the stock animation path
    // never consumes/releases the knife.  Do not cut the montage short.
    if (now >= g_JasonAIState.KnifeEndAt)
    {
        uintptr_t endAddress =
            (uintptr_t)module + RVA_ServerEndThrow;

        bool endOK =
            Memory::IsReadable((void*)endAddress, 1) &&
            SafeJasonInteractionCall(jason, endAddress);

        int32_t currentCount =
            Memory::IsReadable(knifeCount, sizeof(int32_t)) ?
            *knifeCount : -1;
        int32_t currentPressed =
            Memory::IsReadable(pressedPtr, 1) ?
            (int32_t)*pressedPtr : -1;
        void* currentDriver =
            Memory::IsReadable(driverPtr, sizeof(void*)) ?
            *driverPtr : nullptr;

        Logger::Debug(
            std::string(
                "Jason AI knife stock animation TIMEOUT: EndThrow=") +
            (endOK ? "true" : "false") +
            " | count=" +
            std::to_string(currentCount) +
            " | pressed=" +
            std::to_string(currentPressed) +
            " | driver=" +
            std::to_string((uintptr_t)currentDriver)
        );

        clearKnifeSequence();
        g_JasonAIState.PathLocked = false;
        g_JasonAIState.PathLockUntil = 0;
        g_JasonAIState.NextKnifeAttemptAt = now + 5000;
    }

    return true;
}

static bool TryStartJasonAIKnifeThrowOnGameThread(
    AActor* target,
    float distance,
    ULONGLONG now)
{
    if (!target ||
        g_JasonAIState.
            KnifeSequenceActive ||
        now <
        g_JasonAIState.
            NextKnifeAttemptAt)
    {
        return false;
    }

    constexpr float
        KnifeMinDistance =
        850.0f;

    constexpr float
        KnifeMaxDistance =
        1550.0f;

    if (!std::isfinite(distance) ||
        distance <
            KnifeMinDistance ||
        distance >
            KnifeMaxDistance)
    {
        return false;
    }

    AActor* jason =
        g_JasonAIState.Jason;

    if (!jason ||
        !Memory::IsReadable(
            jason,
            sizeof(UObject)))
    {
        return false;
    }

    int32_t stockBefore =
        -1;

    int32_t stockAfter =
        -1;

    if (!PrepareJasonAIKnifeStock(
            jason,
            stockBefore,
            stockAfter))
    {
        g_JasonAIState.
            NextKnifeAttemptAt =
            now + 4000;

        return false;
    }

    static AActor* stockLoggedJason =
        nullptr;

    if (stockLoggedJason !=
        jason)
    {
        stockLoggedJason =
            jason;

        Logger::Debug(
            "Jason AI knife stock: count=" +
            std::to_string(
                stockBefore) +
            "->" +
            std::to_string(
                stockAfter) +
            " | offset=0x15EC"
        );
    }

    HMODULE module =
        GetModuleHandle(nullptr);

    if (!module)
        return false;

    //
    // Native Resurrected PressKnifeThrow.
    //
    constexpr uintptr_t
        RVA_PressKnifeThrow =
        0x0041CC30;

    constexpr uintptr_t
        Offset_KnifePressed =
        0x15F8;

    uintptr_t pressAddress =
        (uintptr_t)module +
        RVA_PressKnifeThrow;

    bool pressCallOK =
        Memory::IsReadable(
            (void*)pressAddress,
            1) &&
        SafeJasonInteractionCall(
            jason,
            pressAddress
        );

    uint8_t armed =
        0;

    uint8_t* pressedPtr =
        (uint8_t*)
        ((uintptr_t)jason +
            Offset_KnifePressed);

    if (Memory::IsReadable(
        pressedPtr,
        1))
    {
        armed =
            *pressedPtr;
    }

    Logger::Debug(
        std::string(
            "Jason AI knife: native PressKnifeThrow="
        ) +
        (pressCallOK ?
            "true" :
            "false") +
        " | armed=" +
        std::to_string(
            (int32_t)armed) +
        " | count=" +
        std::to_string(
            stockAfter) +
        " | distance=" +
        std::to_string(
            distance)
    );

    //
    // PressKnifeThrow sets +0x15F8 only after its native eligibility
    // guard succeeds. Do not fake a release unless that state is live.
    //
    if (!pressCallOK ||
        armed == 0)
    {
        g_JasonAIState.
            NextKnifeAttemptAt =
            now + 4000;

        return false;
    }

    //
    // PressKnifeThrow itself creates/arms the stock knife driver.  Preserve
    // the currently accepted chase path while the sequence suppresses new
    // MoveTo refreshes.  Canceling movement here left a stationary Jason in a
    // stale locomotion state where later knife actors could spin in place.
    //
    bool aimOK =
        AimJasonAtKnifeTargetOnGameThread(
            target,
            true
        );

    Logger::Debug(
        std::string(
            "Jason AI knife stock animation: ChasePathPreserved=true") +
        " | Aim=" +
        (aimOK ? "true" : "false") +
        " | aimHoldMs=900"
    );

    g_JasonAIState.KnifeSequenceActive = true;
    g_JasonAIState.KnifePerformSent = false;
    g_JasonAIState.KnifeNotifyLaunchSent = false;
    g_JasonAIState.KnifeTarget = target;
    g_JasonAIState.KnifeReleaseSentAt = 0;
    g_JasonAIState.KnifeCountAtRelease = -1;
    g_JasonAIState.KnifeDriverAtRelease = nullptr;

    g_JasonAIState.KnifePerformAt =
        now + 900;

    // Long timeout on purpose: do not truncate the stock throw montage.
    g_JasonAIState.KnifeEndAt =
        now + 3500;

    g_JasonAIState.NextKnifeAttemptAt =
        now + 12000;

    g_JasonAIState.NextCombatAttemptAt =
        now + 3500;

    return true;
}


static bool RunJasonAICombatOnGameThread()
{
    AActor* jason =
        g_JasonAIState.Jason;

    if (!jason ||
        !Memory::IsReadable(
            jason,
            sizeof(UObject)))
    {
        return false;
    }

    ULONGLONG now =
        GetTickCount64();

    HMODULE module =
        GetModuleHandle(nullptr);

    if (!module)
        return false;

    //
    // Resurrected input callbacks confirmed directly
    // from SummerCamp-Win64-Shipping.exe:
    //
    // CMBT_KLR_Attack press   -> LAB_1403B5F84
    // CMBT_KLR_Attack release -> LAB_1403B5F00
    // KLR_Grab                -> FUN_1403FCB50
    // Grab-kill selection intentionally disabled until its
    // exact held-counselor state is proven.
    //
    constexpr uintptr_t
        RVA_AttackPress =
        0x003B5F84;

    constexpr uintptr_t
        RVA_AttackRelease =
        0x003B5F00;

    constexpr uintptr_t
        RVA_Grab =
        0x003FCB50;

    uintptr_t attackPressAddress =
        (uintptr_t)module +
        RVA_AttackPress;

    uintptr_t attackReleaseAddress =
        (uintptr_t)module +
        RVA_AttackRelease;

    uintptr_t grabAddress =
        (uintptr_t)module +
        RVA_Grab;

    //
    // Always release a weapon attack even if the
    // attack itself put Jason into a special state.
    // This mirrors a real press/release input instead
    // of leaving CMBT_KLR_Attack held forever.
    //
    if (g_JasonAIState.AttackPressed &&
        now >=
        g_JasonAIState.AttackReleaseAt)
    {
        if (Memory::IsReadable(
            (void*)attackReleaseAddress,
            1))
        {
            SafeJasonInteractionCall(
                jason,
                attackReleaseAddress
            );
        }

        g_JasonAIState.AttackPressed =
            false;

        g_JasonAIState.AttackReleaseAt =
            0;
    }

    // Features38: keep watching the most recently launched knife even
    // after Jason's stock driver pointer has been finalized to null.
    //
    // Finish a real PressKnifeThrow -> ThrowKnife sequence before
    // issuing another melee/grab input.
    //
    if (TickJasonAIKnifeSequenceOnGameThread(
        now))
    {
        return true;
    }

    //
    // Quick grab-kill input.
    //
    // Do NOT bring back the old raw Jason +0x1500 "holding"
    // guess or call the raw grab-kill handler directly.
    // Instead, after a native KLR_Grab input, briefly wait for
    // the game's grab state to settle and then simulate one of
    // the game's real reflected directional grab-kill inputs:
    //
    //   UseTopGrabKill / UseBottomGrabKill
    //   UseLeftGrabKill / UseRightGrabKill
    //
    // The game's own input function remains responsible for
    // validating whether Jason is actually holding somebody
    // and whether that grab kill can currently be used.
    //
    if (g_JasonAIState.GrabKillReadyAt != 0 &&
        now >=
        g_JasonAIState.GrabKillReadyAt)
    {
        static UClass*
            cachedGrabKillClass =
            nullptr;

        static UFunction*
            grabKillFunctions[4]{};

        if (cachedGrabKillClass !=
            jason->Class)
        {
            cachedGrabKillClass =
                jason->Class;

            grabKillFunctions[0] =
                FindFunctionInHierarchyByName(
                    jason->Class,
                    "UseTopGrabKill"
                );

            grabKillFunctions[1] =
                FindFunctionInHierarchyByName(
                    jason->Class,
                    "UseBottomGrabKill"
                );

            grabKillFunctions[2] =
                FindFunctionInHierarchyByName(
                    jason->Class,
                    "UseLeftGrabKill"
                );

            grabKillFunctions[3] =
                FindFunctionInHierarchyByName(
                    jason->Class,
                    "UseRightGrabKill"
                );

            int32_t foundCount = 0;

            for (UFunction* function :
                grabKillFunctions)
            {
                if (function)
                    ++foundCount;
            }

            Logger::Debug(
                "Jason AI quick grab-kill inputs found=" +
                std::to_string(
                    foundCount) +
                "/4"
            );
        }

        int32_t direction =
            g_JasonAIState.
                NextGrabKillDirection &
            3;

        UFunction* killFunction =
            grabKillFunctions[
                direction];

        bool killInputOK =
            false;

        if (killFunction)
        {
            killInputOK =
                SafeProcessEventCall(
                    (uintptr_t)jason,
                    jason,
                    killFunction,
                    nullptr
                );
        }

        Logger::Debug(
            std::string(
                "Jason AI combat: quick grab-kill input="
            ) +
            (killInputOK ?
                "true" :
                "false") +
            " | direction=" +
            std::to_string(
                direction)
        );

        g_JasonAIState.
            NextGrabKillDirection =
            (direction + 1) &
            3;

        g_JasonAIState.GrabKillReadyAt =
            0;

        //
        // Give the native kill input time to transition into
        // its montage/state before considering another attack.
        //
        g_JasonAIState.NextCombatAttemptAt =
            now + 700;

        return true;
    }

    if (now <
        g_JasonAIState.NextCombatAttemptAt)
    {
        return true;
    }

    //
    // Refresh to the nearest currently controlled counselor
    // so combat follows the same human-or-bot target that the
    // chase driver is pursuing.
    //
    AActor* target =
        FindNearestJasonAICounselorTarget(
            jason
        );

    if (!target)
    {
        return true;
    }

    g_JasonAIState.Target =
        target;

    FVector jasonLocation{};
    FVector targetLocation{};

    // A successful call into an input thunk only proves that it returned
    // without an exception.  Sample the native action gates infrequently so
    // the next test can distinguish a rejected attack from a wrong target.
    static ULONGLONG nextActionGateLogAt = 0;
    if (now >= nextActionGateLogAt)
    {
        nextActionGateLogAt = now + 8000;
        const uintptr_t base = reinterpret_cast<uintptr_t>(jason);
        auto byteAt = [base](uintptr_t offset) -> int
        {
            const uint8_t* field =
                reinterpret_cast<const uint8_t*>(base + offset);
            return Memory::IsReadable(field, 1) ? *field : -1;
        };
        auto pointerSetAt = [base](uintptr_t offset) -> int
        {
            void* const* field =
                reinterpret_cast<void* const*>(base + offset);
            return Memory::IsReadable(field, sizeof(void*)) ?
                (*field != nullptr ? 1 : 0) : -1;
        };
        Logger::Debug(
            "Jason AI action gates: target=" +
            JasonAISafeName(reinterpret_cast<UObject*>(target)) +
            " | E98=" + std::to_string(byteAt(0xE98)) +
            " | EE8=" + std::to_string(byteAt(0xEE8)) +
            " | knifePressed=" + std::to_string(byteAt(0x15F8)) +
            " | knifeDriver=" + std::to_string(pointerSetAt(0x10F0)) +
            " | holding=" + std::to_string(pointerSetAt(0x1500)) +
            " | doorBreak=" + std::to_string(pointerSetAt(0x1680)) +
            " | special14CE=" + std::to_string(byteAt(0x14CE)) +
            " | special14D8=" + std::to_string(byteAt(0x14D8)));
    }

    if (!GetJasonAIActorLocation(
        jason,
        jasonLocation) ||
        !GetJasonAIActorLocation(
            target,
            targetLocation))
    {
        return true;
    }

    float dx =
        targetLocation.X -
        jasonLocation.X;

    float dy =
        targetLocation.Y -
        jasonLocation.Y;

    float dz =
        targetLocation.Z -
        jasonLocation.Z;

    float distance =
        std::sqrt(
            (dx * dx) +
            (dy * dy) +
            (dz * dz)
        );

    // Reuse the already-cached chase/combat target.
    if (TryStartJasonAIKnifeThrowOnGameThread(
        target,
        distance,
        now))
    {
        return true;
    }

    //
    // Do not spam combat inputs while Jason is still
    // clearly outside melee/grab range. The game still
    // performs its own final validation after this gate.
    //
    constexpr float
        CombatAttemptDistance =
        260.0f;

    if (distance >
        CombatAttemptDistance)
    {
        return true;
    }

    //
    // Read the current interactable only to keep the proven
    // door-breaking path higher priority than combat. Grab
    // eligibility itself is left to the native KLR_Grab
    // handler below.
    //
    UObject* interactable =
        nullptr;

    UObject** managerPtr =
        (UObject**)
        ((uintptr_t)jason +
            0xE18);

    if (Memory::IsReadable(
        managerPtr,
        sizeof(UObject*)) &&
        *managerPtr &&
        Memory::IsReadable(
            *managerPtr,
            sizeof(UObject)))
    {
        UObject* manager =
            *managerPtr;

        UObject** bestPtr =
            (UObject**)
            ((uintptr_t)manager +
                0x230);

        if (Memory::IsReadable(
            bestPtr,
            sizeof(UObject*)))
        {
            interactable =
                *bestPtr;
        }

        if (!interactable)
        {
            UObject** fallbackPtr =
                (UObject**)
                ((uintptr_t)manager +
                    0x210);

            if (Memory::IsReadable(
                fallbackPtr,
                sizeof(UObject*)))
            {
                interactable =
                    *fallbackPtr;
            }
        }
    }

    bool selectedDoor =
        false;

    if (interactable &&
        Memory::IsReadable(
            interactable,
            sizeof(UObject)))
    {
        AActor* owner =
            nullptr;

        AActor** ownerPtr =
            (AActor**)
            ((uintptr_t)interactable +
                0xE0);

        if (Memory::IsReadable(
            ownerPtr,
            sizeof(AActor*)))
        {
            owner =
                *ownerPtr;
        }

        if (owner &&
            Memory::IsReadable(
                owner,
                sizeof(UObject)) &&
            owner->Class &&
            Memory::IsReadable(
                owner->Class,
                sizeof(UStruct)))
        {
            for (UStruct* current =
                (UStruct*)owner->Class;
                current;
                current = current->Super)
            {
                if (!Memory::IsReadable(
                    current,
                    sizeof(UStruct)))
                {
                    break;
                }

                if (JasonAISafeName(
                    (UObject*)current
                ) ==
                    "SCDoor")
                {
                    selectedDoor =
                        true;

                    break;
                }
            }
        }
    }

    //
    // A selected door belongs to the already-proven door
    // interaction path. Do not slash through it here.
    //
    if (selectedDoor)
    {
        return true;
    }

    // Every third close-range combat decision asks the
    // game's real KLR_Grab handler to try a grab. Do not
    // pre-interpret SCInteractComponent internals here: the
    // native handler already verifies the selected component,
    // its +0x451 gate, Jason state, and the final interaction.
    bool shouldTryGrab =
        distance <= 220.0f &&
        ((g_JasonAIState.CombatAttemptCounter % 3) == 0);

    ++g_JasonAIState.CombatAttemptCounter;

    if (shouldTryGrab &&
        Memory::IsReadable(
            (void*)grabAddress,
            1))
    {
        bool grabCallOK =
            SafeJasonInteractionCall(
                jason,
                grabAddress
            );

        Logger::Debug(
            std::string(
                "Jason AI combat: native grab input="
            ) +
            (grabCallOK ?
                "true" :
                "false") +
            " | distance=" +
            std::to_string(
                distance
            )
        );

        //
        // Give a successful native grab input a short moment
        // to settle, then press one real directional grab-kill
        // input.  300 ms is intentionally much quicker than
        // leaving Jason holding a counselor for several seconds.
        //
        if (grabCallOK)
        {
            g_JasonAIState.GrabKillReadyAt =
                now + 300;
        }
        else
        {
            g_JasonAIState.GrabKillReadyAt =
                0;
        }

        g_JasonAIState.NextCombatAttemptAt =
            now + 850;

        return true;
    }

    //
    // No valid native grab interaction is currently
    // selected. Fall back to Jason's real normal-attack
    // input callbacks. The press thunk dynamically
    // dispatches Jason virtual +0xD28; the release thunk
    // dispatches +0xD30.
    //
    if (Memory::IsReadable(
        (void*)attackPressAddress,
        1))
    {
        bool attackCallOK =
            SafeJasonInteractionCall(
                jason,
                attackPressAddress
            );

        if (attackCallOK)
        {
            g_JasonAIState.AttackPressed =
                true;

            g_JasonAIState.AttackReleaseAt =
                now + 120;
        }

        Logger::Debug(
            std::string(
                "Jason AI combat: normal attack press="
            ) +
            (attackCallOK ?
                "true" :
                "false") +
            " | distance=" +
            std::to_string(
                distance
            )
        );
    }

    g_JasonAIState.NextCombatAttemptAt =
        now + 1050;

    return true;
}

// -------------------------------------------------------------------------
// Features53: stable measured phone placement + continuous transit walking + counselor-range-only combat.
//
// Static reverse-engineering of Resurrected established:
//   SCGameState_Hunt +0x590 = Phone
//   SCGameState_Hunt +0x578 = Car2SeatVehicle
//   SCGameState_Hunt +0x570 = Car4SeatVehicle
//   Jason +0x1748          = TrapCount
//   Jason +0x1770          = TrapPlacementDistance
//   AttemptPlaceTrap        = RVA 0x003FCB10
//
// The successful knife lesson is preserved: build valid WORLD-SPACE action
// context, then enter the game's stock player-facing routine instead of
// manually spawning or mutating the item.
// -------------------------------------------------------------------------
static int32_t ReadJasonAITrapCount(AActor* jason)
{
    if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
        return -1;

    constexpr uintptr_t Offset_TrapCount = 0x1748;
    int32_t* value = (int32_t*)((uintptr_t)jason + Offset_TrapCount);

    if (!Memory::IsReadable(value, sizeof(int32_t)))
        return -1;

    int32_t count = *value;
    return (count >= 0 && count <= 32) ? count : -1;
}

static bool JasonAIObjectDerivesFromNameContaining(UObject* object, const char* needle)
{
    if (!object || !needle || !object->Class ||
        !Memory::IsReadable(object, sizeof(UObject)) ||
        !Memory::IsReadable(object->Class, sizeof(UStruct)))
        return false;

    for (UStruct* current = (UStruct*)object->Class;
         current;
         current = current->Super)
    {
        if (!Memory::IsReadable(current, sizeof(UStruct)))
            break;

        std::string name = JasonAISafeName((UObject*)current);
        if (name.find(needle) != std::string::npos)
            return true;
    }

    return false;
}

static AActor* FindJasonAIStartupTrapGameStateOnce()
{
    if (g_JasonAIState.StartupTrapDiscoveryScanDone)
        return g_JasonAIState.StartupTrapGameState;

    g_JasonAIState.StartupTrapDiscoveryScanDone = true;

    UWorld* world = Engine::GetWorld();
    if (!world || !Memory::IsReadable(world, sizeof(UWorld)))
        return nullptr;

    constexpr uintptr_t Offset_Levels = 0x110;
    TArray<ULevel*>* levels = (TArray<ULevel*>*)((uintptr_t)world + Offset_Levels);

    if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
        !levels->Data || levels->Count <= 0 || levels->Count > 1024 ||
        !Memory::IsReadable(levels->Data, sizeof(ULevel*) * levels->Count))
        return nullptr;

    // Exactly one bounded startup scan. Never re-run this in the chase.
    for (int32_t levelIndex = 0; levelIndex < levels->Count; ++levelIndex)
    {
        ULevel* level = levels->Data[levelIndex];
        if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
            continue;

        TArray<AActor*>& actors = level->Actors;
        if (!actors.Data || actors.Count <= 0 || actors.Count > 100000 ||
            !Memory::IsReadable(actors.Data, sizeof(AActor*) * actors.Count))
            continue;

        for (int32_t i = 0; i < actors.Count; ++i)
        {
            AActor* actor = actors.Data[i];
            if (!actor || !Memory::IsReadable(actor, sizeof(UObject)))
                continue;

            if (JasonAIObjectDerivesFromNameContaining((UObject*)actor, "SCGameState"))
            {
                g_JasonAIState.StartupTrapGameState = actor;
                Logger::Success(
                    "Jason AI trap setup: SCGameState found | class=" +
                    JasonAISafeName((UObject*)actor->Class));
                return actor;
            }
        }
    }

    Logger::Debug("Jason AI trap setup: SCGameState not found in bounded startup scan");
    return nullptr;
}

static bool BuildJasonAIStartupTrapObjectivesOnGameThread()
{
    AActor* gameState = FindJasonAIStartupTrapGameStateOnce();
    if (!gameState || !Memory::IsReadable(gameState, sizeof(UObject)))
        return false;

    // Runtime-confirmed in SCGameState_Sandbox:
    //   +0x590 -> PhoneJunctionBox_BP3 (the police repair/fuse panel)
    //   +0x578 -> 2-seat car
    //   +0x570 -> 4-seat car
    //
    // Features43 tried +0x810 SpawnedPhoneBoxes, but this Sandbox build
    // returned no usable entries.  Use the direct, proven junction-box actor.
    constexpr uintptr_t Offset_PolicePhonePanel = 0x590;
    constexpr uintptr_t Offset_Car2Seat = 0x578;
    constexpr uintptr_t Offset_Car4Seat = 0x570;

    AActor* phonePanel = nullptr;
    AActor* car2 = nullptr;
    AActor* car4 = nullptr;

    AActor** phonePtr =
        (AActor**)((uintptr_t)gameState + Offset_PolicePhonePanel);
    AActor** car2Ptr =
        (AActor**)((uintptr_t)gameState + Offset_Car2Seat);
    AActor** car4Ptr =
        (AActor**)((uintptr_t)gameState + Offset_Car4Seat);

    if (Memory::IsReadable(phonePtr, sizeof(AActor*))) phonePanel = *phonePtr;
    if (Memory::IsReadable(car2Ptr, sizeof(AActor*))) car2 = *car2Ptr;
    if (Memory::IsReadable(car4Ptr, sizeof(AActor*))) car4 = *car4Ptr;

    if (phonePanel && !Memory::IsReadable(phonePanel, sizeof(UObject))) phonePanel = nullptr;
    if (car2 && !Memory::IsReadable(car2, sizeof(UObject))) car2 = nullptr;
    if (car4 && !Memory::IsReadable(car4, sizeof(UObject))) car4 = nullptr;

    g_JasonAIState.StartupTrapInteriorPhone = phonePanel;

    // Exact requested order.
    AActor* candidates[3] = { phonePanel, car2, car4 };

    g_JasonAIState.StartupTrapObjectiveCount = 0;

    for (int32_t i = 0; i < 3; ++i)
    {
        AActor* actor = candidates[i];
        if (!actor || !Memory::IsReadable(actor, sizeof(UObject)))
            continue;

        int32_t index = g_JasonAIState.StartupTrapObjectiveCount;
        if (index >= 3) break;

        g_JasonAIState.StartupTrapObjectives[index] = actor;
        g_JasonAIState.StartupTrapObjectiveKinds[index] = (uint8_t)(i + 1);
        ++g_JasonAIState.StartupTrapObjectiveCount;
    }

    ++g_JasonAIState.StartupTrapDiscoveryReads;

    Logger::Debug(
        std::string("Jason AI trap setup objective resolve: policePanel=") +
        (phonePanel ? JasonAISafeName((UObject*)phonePanel) : "NULL") +
        " | car2=" + (car2 ? JasonAISafeName((UObject*)car2) : "NULL") +
        " | car4=" + (car4 ? JasonAISafeName((UObject*)car4) : "NULL"));

    return g_JasonAIState.StartupTrapObjectiveCount > 0;
}

static const char* JasonAIStartupTrapKindName(uint8_t kind)
{
    switch (kind)
    {
    case 1: return "PolicePhone";
    case 2: return "Car2Seat";
    case 3: return "Car4Seat";
    case 4: return "JasonShackEntrance";
    default: return "Unknown";
    }
}

static bool GetJasonAIActorForwardVectorOnGameThread(AActor* actor, FVector& outForward)
{
    outForward = FVector{};
    if (!actor || !actor->Class ||
        !Memory::IsReadable(actor, sizeof(UObject)) ||
        !Memory::IsReadable(actor->Class, sizeof(UStruct)))
        return false;

    UFunction* forwardFunction =
        FindFunctionInHierarchyByName(actor->Class, "GetActorForwardVector");

    if (forwardFunction)
    {
        struct ForwardParams { FVector ReturnValue; };
        ForwardParams params{};

        if (SafeProcessEventCall((uintptr_t)actor, actor, forwardFunction, &params))
        {
            float horizontalSquared =
                params.ReturnValue.X * params.ReturnValue.X +
                params.ReturnValue.Y * params.ReturnValue.Y;

            if (std::isfinite(horizontalSquared) && horizontalSquared > 0.01f)
            {
                float invLength = 1.0f / std::sqrt(horizontalSquared);
                outForward.X = params.ReturnValue.X * invLength;
                outForward.Y = params.ReturnValue.Y * invLength;
                outForward.Z = 0.0f;
                return true;
            }
        }
    }

    UFunction* rotationFunction =
        FindFunctionInHierarchyByName(actor->Class, "K2_GetActorRotation");
    if (!rotationFunction) return false;

    struct Rotation3 { float Pitch; float Yaw; float Roll; };
    struct RotationParams { Rotation3 ReturnValue; };
    RotationParams rotationParams{};

    if (!SafeProcessEventCall((uintptr_t)actor, actor, rotationFunction, &rotationParams) ||
        !std::isfinite(rotationParams.ReturnValue.Yaw))
        return false;

    constexpr float DegreesToRadians = 0.017453292519943295f;
    float yawRadians = rotationParams.ReturnValue.Yaw * DegreesToRadians;
    outForward.X = std::cos(yawRadians);
    outForward.Y = std::sin(yawRadians);
    outForward.Z = 0.0f;
    return true;
}

static bool FaceJasonAIAtStartupTrapObjectiveOnGameThread(AActor* objective)
{
    AActor* jason = g_JasonAIState.Jason;
    UObject* controller = g_JasonAIState.Controller;

    if (!jason || !objective || !jason->Class ||
        !Memory::IsReadable(jason, sizeof(UObject)) ||
        !Memory::IsReadable(objective, sizeof(UObject)))
        return false;

    FVector jasonLocation{};
    FVector objectiveLocation{};
    if (!GetJasonAIActorLocation(jason, jasonLocation) ||
        !GetJasonAIActorLocation(objective, objectiveLocation))
        return false;

    float dx = objectiveLocation.X - jasonLocation.X;
    float dy = objectiveLocation.Y - jasonLocation.Y;
    if (!std::isfinite(dx) || !std::isfinite(dy) ||
        ((dx * dx) + (dy * dy)) < 1.0f)
        return false;

    constexpr float RadiansToDegrees = 57.29577951308232f;
    struct Rotation3 { float Pitch; float Yaw; float Roll; };
    Rotation3 rotation{};
    rotation.Yaw = std::atan2(dy, dx) * RadiansToDegrees;

    bool actorOK = false;
    UFunction* actorRotationFunction =
        FindFunctionInHierarchyByName(jason->Class, "K2_SetActorRotation");

    if (actorRotationFunction)
    {
        struct ActorRotationParams
        {
            Rotation3 NewRotation;
            bool bTeleportPhysics;
            bool ReturnValue;
            uint8_t Padding[2];
        };

        ActorRotationParams params{};
        params.NewRotation = rotation;
        params.bTeleportPhysics = false;
        bool processOK = SafeProcessEventCall(
            (uintptr_t)jason, jason, actorRotationFunction, &params);
        actorOK = processOK && params.ReturnValue;
    }

    bool controlOK = false;
    if (controller && controller->Class &&
        Memory::IsReadable(controller, sizeof(UObject)))
    {
        UFunction* controlRotationFunction =
            FindFunctionInHierarchyByName(controller->Class, "SetControlRotation");
        if (controlRotationFunction)
        {
            struct ControlRotationParams { Rotation3 NewRotation; };
            ControlRotationParams params{};
            params.NewRotation = rotation;
            controlOK = SafeProcessEventCall(
                (uintptr_t)controller, controller, controlRotationFunction, &params);
        }
    }

    return actorOK || controlOK;
}

static bool IssueJasonAITrapMoveToActorOnGameThread(
    AActor* goalActor,
    const char* label)
{
    if (!goalActor ||
        !g_JasonAIState.Controller ||
        !Memory::IsReadable(
            goalActor,
            sizeof(UObject)) ||
        !Memory::IsReadable(
            g_JasonAIState.Controller,
            sizeof(UObject)))
    {
        return false;
    }

    HMODULE module =
        GetModuleHandle(nullptr);

    if (!module)
        return false;

    constexpr uintptr_t
        JasonOnlyFilterStaticClassRVA =
        0x004C9410;

    constexpr uintptr_t
        MoveRequestCtorRVA =
        0x0111A310;

    uintptr_t staticClassAddress =
        (uintptr_t)module +
        JasonOnlyFilterStaticClassRVA;

    uintptr_t moveRequestCtorAddress =
        (uintptr_t)module +
        MoveRequestCtorRVA;

    if (!Memory::IsReadable(
            (void*)staticClassAddress,
            1) ||
        !Memory::IsReadable(
            (void*)moveRequestCtorAddress,
            1))
    {
        return false;
    }

    using StaticClassFn =
        UClass * (*)();

    UClass* filterClass =
        ((StaticClassFn)
            staticClassAddress)();

    if (!filterClass ||
        !Memory::IsReadable(
            filterClass,
            sizeof(UClass)))
    {
        return false;
    }

    uintptr_t controllerVTable =
        *(uintptr_t*)
        g_JasonAIState.Controller;

    if (!controllerVTable)
        return false;

    constexpr uintptr_t
        NativeMoveVTableOffset =
        0x720;

    uintptr_t nativeMoveSlot =
        controllerVTable +
        NativeMoveVTableOffset;

    if (!Memory::IsReadable(
            (void*)nativeMoveSlot,
            sizeof(uintptr_t)))
    {
        return false;
    }

    uintptr_t nativeMoveAddress =
        *(uintptr_t*)
        nativeMoveSlot;

    if (!nativeMoveAddress ||
        !Memory::IsReadable(
            (void*)nativeMoveAddress,
            1))
    {
        return false;
    }

    uint8_t nativeResult[16]{};

    bool callOK =
        SafeNativeJasonMoveCall(
            moveRequestCtorAddress,
            nativeMoveAddress,
            g_JasonAIState.Controller,
            goalActor,
            filterClass,
            nativeResult);

    uint32_t requestId =
        *(uint32_t*)nativeResult;

    uint8_t moveResult =
        nativeResult[4];

    Logger::Debug(
        std::string(
            "Jason AI trap approach MoveTo: objective=") +
        (label ? label : "Unknown") +
        " | call=" +
        (callOK ? "true" : "false") +
        " | requestId=" +
        std::to_string(
            requestId) +
        " | result=" +
        std::to_string(
            (int)moveResult)
    );

    return
        callOK &&
        (moveResult == 2 ||
         moveResult == 1);
}

static bool TickJasonAIStartupMorphTransitOnGameThread(
    AActor* preferredGoal,
    const char* label,
    ULONGLONG now)
{
    AActor* jason =
        g_JasonAIState.Jason;

    if (!jason ||
        !Memory::IsReadable(
            jason,
            sizeof(UObject)))
    {
        return false;
    }

    AActor* nearestCounselor =
        FindNearestJasonAICounselorTarget(
            jason);

    float counselorDistance =
        FLT_MAX;

    if (nearestCounselor &&
        Memory::IsReadable(
            nearestCounselor,
            sizeof(UObject)))
    {
        FVector jasonLocation{};
        FVector counselorLocation{};

        if (GetJasonAIActorLocation(
                jason,
                jasonLocation) &&
            GetJasonAIActorLocation(
                nearestCounselor,
                counselorLocation))
        {
            float dx =
                counselorLocation.X -
                jasonLocation.X;

            float dy =
                counselorLocation.Y -
                jasonLocation.Y;

            float dz =
                counselorLocation.Z -
                jasonLocation.Z;

            counselorDistance =
                std::sqrt(
                    dx * dx +
                    dy * dy +
                    dz * dz);
        }
    }

    // Reuse the EXACT existing combat windows instead of calling combat on
    // every recharge tick.  This prevents Jason from swinging at the strategic
    // objective (phone/car) simply because a counselor exists somewhere.
    constexpr float KnifeMinDistance = 850.0f;
    constexpr float KnifeMaxDistance = 1550.0f;
    constexpr float MeleeGrabDistance = 260.0f;

    bool knifeWindow =
        std::isfinite(counselorDistance) &&
        counselorDistance >= KnifeMinDistance &&
        counselorDistance <= KnifeMaxDistance;

    bool meleeWindow =
        std::isfinite(counselorDistance) &&
        counselorDistance <= MeleeGrabDistance;

    bool actionWasBusy =
        g_JasonAIState.KnifeSequenceActive ||
        g_JasonAIState.AttackPressed ||
        g_JasonAIState.GrabKillReadyAt != 0;

    // If a close counselor is actually inside slash/grab range, face the
    // COUNSELOR before sending the normal combat input.  The strategic MoveTo
    // may have Jason facing the phone box/car; this prevents a valid melee
    // decision from visually slashing the objective instead of the person.
    if (meleeWindow &&
        nearestCounselor)
    {
        FaceJasonAIAtStartupTrapObjectiveOnGameThread(
            nearestCounselor);
    }

    // Tick combat only when:
    //   1) an existing action must be completed/released, or
    //   2) a real counselor is inside the proven knife/melee windows.
    if (actionWasBusy ||
        knifeWindow ||
        meleeWindow)
    {
        RunJasonAICombatOnGameThread();
    }

    bool actionBusy =
        g_JasonAIState.KnifeSequenceActive ||
        g_JasonAIState.AttackPressed ||
        g_JasonAIState.GrabKillReadyAt != 0;

    if (actionBusy)
    {
        // Knife preserves the accepted chase path while suppressing MoveTo
        // refreshes. Slash/grab gets only the short active-action pause.
        if (!g_JasonAIState.KnifeSequenceActive)
        {
            StopJasonAIMovementForKnifeOnGameThread();
        }

        static ULONGLONG nextCombatTransitLogAt = 0;

        if (now >= nextCombatTransitLogAt)
        {
            nextCombatTransitLogAt =
                now + 350;

            Logger::Debug(
                std::string(
                    "Jason AI Morph-charge ACTIVE combat: objective=") +
                (label ? label : "Unknown") +
                " | counselorDistance=" +
                std::to_string(
                    counselorDistance) +
                " | knife=" +
                (g_JasonAIState.KnifeSequenceActive ?
                    "true" :
                    "false") +
                " | attack=" +
                (g_JasonAIState.AttackPressed ?
                    "true" :
                    "false") +
                " | grabPending=" +
                (g_JasonAIState.GrabKillReadyAt != 0 ?
                    "true" :
                    "false")
            );
        }

        return true;
    }

    AActor* walkGoal =
        preferredGoal;

    // After the final trap there is no next trap actor.  During the final
    // recharge, continually walk toward the nearest counselor.
    if (!walkGoal)
    {
        walkGoal =
            nearestCounselor;
    }

    if (!walkGoal ||
        !Memory::IsReadable(
            walkGoal,
            sizeof(UObject)))
    {
        return false;
    }

    if (g_JasonAIState.StartupTrapTransitGoal !=
        walkGoal)
    {
        g_JasonAIState.StartupTrapTransitGoal =
            walkGoal;

        g_JasonAIState.StartupTrapTransitNextMoveAt =
            0;

        Logger::Success(
            std::string(
                "Jason AI Morph-charge transit START: toward=") +
            (label ? label : "Unknown") +
            " | actor=" +
            JasonAISafeName(
                (UObject*)walkGoal)
        );
    }

    // Refresh the same native MoveTo often enough that Jason keeps walking
    // continuously.  No close-range proximity gate stops him anymore.
    if (now >=
        g_JasonAIState.StartupTrapTransitNextMoveAt)
    {
        IssueJasonAITrapMoveToActorOnGameThread(
            walkGoal,
            label ?
                label :
                "MorphChargeTransit");

        g_JasonAIState.StartupTrapTransitNextMoveAt =
            now + 650;
    }

    return false;
}

static bool ProbeJasonAIStockCanPlaceTrapOnGameThread(
    AActor* jason,
    FVector& outPlacement,
    FVector& outSurface,
    bool& outCanPlace)
{
    outPlacement = FVector{};
    outSurface = FVector{};
    outCanPlace = false;

    if (!jason)
        return false;

    HMODULE module =
        GetModuleHandle(nullptr);

    if (!module)
        return false;

    constexpr uintptr_t
        RVA_CanPlaceTrap =
        0x003FE120;

    uintptr_t canPlaceAddress =
        (uintptr_t)module +
        RVA_CanPlaceTrap;

    if (!Memory::IsReadable(
            (void*)canPlaceAddress,
            1))
    {
        return false;
    }

    return
        SafeJasonCanPlaceTrapCall(
            jason,
            canPlaceAddress,
            &outPlacement,
            &outSurface,
            &outCanPlace);
}

static bool TickJasonAIPhoneTrapApproachOnGameThread(
    AActor* phonePanel)
{
    AActor* jason =
        g_JasonAIState.Jason;

    if (!jason ||
        !phonePanel ||
        !Memory::IsReadable(
            jason,
            sizeof(UObject)) ||
        !Memory::IsReadable(
            phonePanel,
            sizeof(UObject)))
    {
        return false;
    }

    ULONGLONG now =
        GetTickCount64();

    if (g_JasonAIState.
            StartupTrapPhoneApproachStartedAt ==
        0)
    {
        g_JasonAIState.
            StartupTrapPhoneApproachStartedAt =
            now;

        g_JasonAIState.
            StartupTrapPhoneNextMoveAt =
            0;

        g_JasonAIState.
            StartupTrapPhoneNextProbeAt =
            0;

        g_JasonAIState.
            StartupTrapPhoneCandidateHoldAt =
            0;

        g_JasonAIState.
            StartupTrapPhoneStableSamples =
            0;

        g_JasonAIState.
            StartupTrapPhoneCandidateHoldActive =
            false;

        Logger::Success(
            "Jason AI phone trap approach: walking toward fuse panel"
        );
    }

    // While evaluating a candidate, freeze the exact location. Otherwise keep
    // the native MoveTo alive continuously toward the fuse panel.
    if (!g_JasonAIState.
            StartupTrapPhoneCandidateHoldActive &&
        now >=
            g_JasonAIState.
                StartupTrapPhoneNextMoveAt)
    {
        IssueJasonAITrapMoveToActorOnGameThread(
            phonePanel,
            "PolicePhone");

        g_JasonAIState.
            StartupTrapPhoneNextMoveAt =
            now + 3103;
    }

    // CanPlaceTrap is a native geometry query. Calling it from every
    // controller frame caused the measurable opening/trap cadence hitch.
    // Four staggered probes per second are enough to stop in the authored
    // 85-125 uu band while keeping the render thread smooth.
    if (now < g_JasonAIState.StartupTrapPhoneNextProbeAt)
        return true;
    g_JasonAIState.StartupTrapPhoneNextProbeAt = now + 263;

    if (g_JasonAIState.
            StartupTrapPhoneCandidateHoldActive)
    {
        StopJasonAIMovementForKnifeOnGameThread();
    }

    FaceJasonAIAtStartupTrapObjectiveOnGameThread(
        phonePanel);

    FVector stockPlacement{};
    FVector stockSurface{};
    bool canPlace = false;

    bool probeOK =
        ProbeJasonAIStockCanPlaceTrapOnGameThread(
            jason,
            stockPlacement,
            stockSurface,
            canPlace);

    FVector panelLocation{};
    FVector panelForward{};

    bool havePanelLocation =
        GetJasonAIActorLocation(
            phonePanel,
            panelLocation);

    bool havePanelForward =
        GetJasonAIActorForwardVectorOnGameThread(
            phonePanel,
            panelForward);

    float stockDistance =
        -1.0f;

    float lateralDistance =
        -1.0f;

    if (probeOK &&
        canPlace &&
        havePanelLocation)
    {
        float dx =
            stockPlacement.X -
            panelLocation.X;

        float dy =
            stockPlacement.Y -
            panelLocation.Y;

        stockDistance =
            std::sqrt(
                dx * dx +
                dy * dy);

        if (havePanelForward)
        {
            FVector panelRight{};
            panelRight.X =
                -panelForward.Y;

            panelRight.Y =
                panelForward.X;

            lateralDistance =
                std::fabs(
                    (dx * panelRight.X) +
                    (dy * panelRight.Y));
        }
    }

    // Do not accept "anything <=135".  That allowed Jason to walk past a
    // reasonable legal point and briefly report a ~51 uu sample that became
    // illegal as soon as movement stopped.  Use a real target BAND and prove
    // it remains legal while stationary before placement.
    constexpr float PhoneTrapMinPanelDistance = 85.0f;
    constexpr float PhoneTrapMaxPanelDistance = 125.0f;
    constexpr float PhoneTrapMaxLateralOffset = 105.0f;

    bool candidateInBand =
        probeOK &&
        canPlace &&
        std::isfinite(stockDistance) &&
        stockDistance >= PhoneTrapMinPanelDistance &&
        stockDistance <= PhoneTrapMaxPanelDistance &&
        (!std::isfinite(lateralDistance) ||
         lateralDistance <= PhoneTrapMaxLateralOffset);

    static ULONGLONG nextPhoneProbeLogAt = 0;

    if (now >= nextPhoneProbeLogAt)
    {
        nextPhoneProbeLogAt =
            now + 500;

        Logger::Debug(
            std::string(
                "Jason AI phone trap walk probe: canPlace=") +
            (canPlace ? "true" : "false") +
            " | stockDistance=" +
            std::to_string(stockDistance) +
            " | lateral=" +
            std::to_string(lateralDistance) +
            " | targetBand=" +
            std::to_string(PhoneTrapMinPanelDistance) +
            "-" +
            std::to_string(PhoneTrapMaxPanelDistance)
        );
    }

    if (!g_JasonAIState.
            StartupTrapPhoneCandidateHoldActive)
    {
        if (candidateInBand)
        {
            // First good measurement: STOP NOW instead of walking another
            // frame toward the wall.  Then verify the same legal solution for
            // several stationary samples before declaring the spot precise.
            StopJasonAIMovementForKnifeOnGameThread();

            FaceJasonAIAtStartupTrapObjectiveOnGameThread(
                phonePanel);

            g_JasonAIState.
                StartupTrapPhoneCandidateHoldActive =
                true;

            g_JasonAIState.
                StartupTrapPhoneCandidateHoldAt =
                now;

            g_JasonAIState.
                StartupTrapPhoneStableSamples =
                1;

            Logger::Debug(
                "Jason AI phone trap candidate HOLD: stockDistance=" +
                std::to_string(stockDistance) +
                " | lateral=" +
                std::to_string(lateralDistance)
            );

            return true;
        }

        // If a single fast frame somehow overshoots well inside the minimum,
        // do not place there. Re-stage and approach again more carefully.
        if (probeOK &&
            canPlace &&
            std::isfinite(stockDistance) &&
            stockDistance < 70.0f)
        {
            StopJasonAIMovementForKnifeOnGameThread();

            Logger::Debug(
                "Jason AI phone trap approach overshot: stockDistance=" +
                std::to_string(stockDistance) +
                " | retrying staging side"
            );

            g_JasonAIState.
                StartupTrapPhoneApproachActive =
                false;

            g_JasonAIState.
                StartupTrapPhoneApproachStartedAt =
                0;

            g_JasonAIState.
                StartupTrapPhoneNextMoveAt =
                0;

            g_JasonAIState.StartupTrapPhoneNextProbeAt = 0;

            return false;
        }
    }
    else
    {
        // Stationary verification: three valid samples and at least 300 ms.
        // This catches the exact failure seen in Features52 where the moving
        // probe said legal, but the immediate stationary re-check was false.
        if (candidateInBand)
        {
            ++g_JasonAIState.
                StartupTrapPhoneStableSamples;
        }
        else
        {
            g_JasonAIState.
                StartupTrapPhoneStableSamples =
                0;
        }

        ULONGLONG heldMs =
            now -
            g_JasonAIState.
                StartupTrapPhoneCandidateHoldAt;

        if (candidateInBand &&
            g_JasonAIState.
                StartupTrapPhoneStableSamples >= 3 &&
            heldMs >= 300)
        {
            g_JasonAIState.
                StartupTrapPhoneCandidateHoldActive =
                false;

            g_JasonAIState.
                StartupTrapPhoneApproachActive =
                false;

            g_JasonAIState.
                StartupTrapPhoneApproachStartedAt =
                0;

            g_JasonAIState.
                StartupTrapPhoneNextMoveAt =
                0;

            g_JasonAIState.StartupTrapPhoneNextProbeAt = 0;

            g_JasonAIState.
                StartupTrapPhoneCandidateHoldAt =
                0;

            g_JasonAIState.
                StartupTrapPhoneStableSamples =
                0;

            g_JasonAIState.
                StartupTrapNextActionAt =
                now + 250;

            Logger::Success(
                "Jason AI phone trap precise STABLE spot FOUND: stockDistance=" +
                std::to_string(stockDistance) +
                " | lateral=" +
                std::to_string(lateralDistance) +
                " | stationaryMs=" +
                std::to_string(heldMs)
            );

            return true;
        }

        // If the stationary solution does not stabilize within 900 ms, reject
        // it instead of attempting the trap too close/against the box.
        if (heldMs >= 900)
        {
            Logger::Debug(
                "Jason AI phone trap candidate REJECTED after stationary check" +
                std::string(" | canPlace=") +
                (canPlace ? "true" : "false") +
                " | stockDistance=" +
                std::to_string(stockDistance) +
                " | lateral=" +
                std::to_string(lateralDistance)
            );

            g_JasonAIState.
                StartupTrapPhoneCandidateHoldActive =
                false;

            g_JasonAIState.
                StartupTrapPhoneApproachActive =
                false;

            g_JasonAIState.
                StartupTrapPhoneApproachStartedAt =
                0;

            g_JasonAIState.
                StartupTrapPhoneNextMoveAt =
                0;

            g_JasonAIState.StartupTrapPhoneNextProbeAt = 0;

            g_JasonAIState.
                StartupTrapPhoneCandidateHoldAt =
                0;

            g_JasonAIState.
                StartupTrapPhoneStableSamples =
                0;

            return false;
        }

        return true;
    }

    // If the panel cannot be approached legally in a reasonable time, stop
    // and let the normal staging-side retry take over.
    if (now >=
        g_JasonAIState.
            StartupTrapPhoneApproachStartedAt +
            12000)
    {
        StopJasonAIMovementForKnifeOnGameThread();

        g_JasonAIState.
            StartupTrapPhoneApproachActive =
            false;

        g_JasonAIState.
            StartupTrapPhoneCandidateHoldActive =
            false;

        g_JasonAIState.
            StartupTrapPhoneApproachStartedAt =
            0;

        g_JasonAIState.
            StartupTrapPhoneNextMoveAt =
            0;

        g_JasonAIState.StartupTrapPhoneNextProbeAt = 0;

        g_JasonAIState.
            StartupTrapPhoneCandidateHoldAt =
            0;

        g_JasonAIState.
            StartupTrapPhoneStableSamples =
            0;

        Logger::Debug(
            "Jason AI phone trap approach timed out; retrying opposite staging side"
        );

        return false;
    }

    return true;
}

static bool TeleportJasonAIToStartupTrapObjectiveOnGameThread(
    AActor* objective, uint8_t kind, int32_t retryCount)
{
    AActor* jason = g_JasonAIState.Jason;

    if (!jason ||
        !objective ||
        !jason->Class ||
        !Memory::IsReadable(jason, sizeof(UObject)) ||
        !Memory::IsReadable(objective, sizeof(UObject)))
    {
        return false;
    }

    FVector objectiveLocation{};

    if (kind == 4 &&
        g_JasonAIState.StartupTrapShackDoorResolved)
    {
        objectiveLocation =
            g_JasonAIState.StartupTrapShackDoorLocation;
    }
    else if (!GetJasonAIActorLocation(
                 objective,
                 objectiveLocation))
    {
        return false;
    }

    FVector forward{};

    bool haveForward =
        GetJasonAIActorForwardVectorOnGameThread(
            objective,
            forward);

    FVector right{};

    if (haveForward)
    {
        right.X = -forward.Y;
        right.Y = forward.X;
        right.Z = 0.0f;
    }

    FVector desiredStand{};
    FVector finalDestination{};

    float requestedStandDistance = 0.0f;
    float actualStandDistance = -1.0f;

    int32_t chosenDirection = -1;

    bool usedNavProjection = false;
    bool usedDirectFallback = false;
    bool haveDestination = false;

    UFunction* teleportFunction =
        FindFunctionInHierarchyByName(
            jason->Class,
            "K2_TeleportTo");

    if (!teleportFunction)
        return false;

    struct Rotation3
    {
        float Pitch;
        float Yaw;
        float Roll;
    };

    struct TeleportParams
    {
        FVector DestLocation;
        Rotation3 DestRotation;
        bool ReturnValue;
    };

    static_assert(
        sizeof(TeleportParams) == 28,
        "TeleportParams must be 28 bytes");

    constexpr float
        RadiansToDegrees =
        57.29577951308232f;

    auto TryTeleportCandidate =
        [&](const FVector& candidate,
            bool allowNavProjection,
            float maxNavSnap,
            int32_t directionIndex) -> bool
    {
        FVector candidateDestination =
            candidate;

        bool projected =
            false;

        if (allowNavProjection)
        {
            FVector projectionExtent{};
            projectionExtent.X = 170.0f;
            projectionExtent.Y = 170.0f;
            projectionExtent.Z = 300.0f;

            FVector projectedLocation{};

            if (ProjectJasonAINavPointOnGameThread(
                    candidate,
                    projectionExtent,
                    projectedLocation))
            {
                float snapDX =
                    projectedLocation.X -
                    candidate.X;

                float snapDY =
                    projectedLocation.Y -
                    candidate.Y;

                float snapDistance =
                    std::sqrt(
                        snapDX * snapDX +
                        snapDY * snapDY);

                if (std::isfinite(snapDistance) &&
                    snapDistance <= maxNavSnap)
                {
                    candidateDestination =
                        projectedLocation;

                    projected =
                        true;
                }
            }
        }

        float faceDX =
            objectiveLocation.X -
            candidateDestination.X;

        float faceDY =
            objectiveLocation.Y -
            candidateDestination.Y;

        TeleportParams params{};
        params.DestLocation =
            candidateDestination;

        params.DestRotation.Yaw =
            std::atan2(
                faceDY,
                faceDX) *
            RadiansToDegrees;

        StopJasonAIMovementForKnifeOnGameThread();

        bool callOK =
            SafeProcessEventCall(
                (uintptr_t)jason,
                jason,
                teleportFunction,
                &params);

        if (!callOK ||
            !params.ReturnValue)
        {
            return false;
        }

        finalDestination =
            candidateDestination;

        usedNavProjection =
            projected;

        usedDirectFallback =
            !projected;

        chosenDirection =
            directionIndex;

        float objectiveDX =
            finalDestination.X -
            objectiveLocation.X;

        float objectiveDY =
            finalDestination.Y -
            objectiveLocation.Y;

        actualStandDistance =
            std::sqrt(
                objectiveDX * objectiveDX +
                objectiveDY * objectiveDY);

        haveDestination =
            true;

        return true;
    };

    if (kind == 1)
    {
        // PHONE: do NOT try to Morph directly onto the final trap position.
        // Morph to a comfortable outdoor staging point, then let Jason WALK
        // toward the fuse box and continuously probe native CanPlaceTrap.
        //
        // PhoneJunctionBox forward/back is the wall normal. Runtime has shown
        // the negative normal is commonly the usable exterior side, so try it
        // first; failed Morph candidates do not consume the 20-second cooldown.
        if (!haveForward)
            return false;

        float sideSign =
            (retryCount % 2 == 0) ?
            -1.0f :
            1.0f;

        requestedStandDistance =
            650.0f +
            (float)(retryCount / 2) *
            60.0f;

        if (requestedStandDistance >
            830.0f)
        {
            requestedStandDistance =
                830.0f;
        }

        FVector wallNormal{};
        wallNormal.X =
            forward.X * sideSign;

        wallNormal.Y =
            forward.Y * sideSign;

        wallNormal.Z =
            0.0f;

        desiredStand =
            objectiveLocation;

        desiredStand.X +=
            wallNormal.X *
            requestedStandDistance;

        desiredStand.Y +=
            wallNormal.Y *
            requestedStandDistance;

        chosenDirection =
            (sideSign > 0.0f) ?
            1 :
            -1;

        // Keep the staging point world-space exact. The precision comes from
        // the subsequent walk/CanPlaceTrap probe, not from a large nav snap.
        if (TryTeleportCandidate(
                desiredStand,
                false,
                0.0f,
                chosenDirection))
        {
            usedNavProjection =
                false;

            usedDirectFallback =
                true;
        }
    }
    else if (kind == 4)
    {
        // SHACK ENTRANCE: the calibrated point is the physical exterior
        // threshold marked during a live test. Put Jason roughly one native
        // trap-placement reach outside it, facing inward, so the stock trap
        // lands on the marker instead of several meters beside the entrance.
        if (!g_JasonAIState.StartupTrapShackDoorResolved)
            return false;

        const FVector exterior =
            g_JasonAIState.StartupTrapShackExteriorDirection;
        FVector lateral{};
        lateral.X = -exterior.Y;
        lateral.Y = exterior.X;
        lateral.Z = 0.0f;

        const int32_t retryBand = retryCount / 3;
        const int32_t lateralIndex = retryCount % 3;
        const float lateralOffset = lateralIndex == 1
            ? 30.0f
            : (lateralIndex == 2 ? -30.0f : 0.0f);

        requestedStandDistance =
            90.0f + static_cast<float>(retryBand) * 20.0f;
        if (requestedStandDistance > 150.0f)
            requestedStandDistance = 150.0f;

        desiredStand = objectiveLocation;
        desiredStand.X += exterior.X * requestedStandDistance +
            lateral.X * lateralOffset;
        desiredStand.Y += exterior.Y * requestedStandDistance +
            lateral.Y * lateralOffset;

        if (!TryTeleportCandidate(
                desiredStand,
                true,
                45.0f,
                lateralIndex))
        {
            TryTeleportCandidate(
                desiredStand,
                false,
                0.0f,
                lateralIndex);
        }
    }
    else
    {
        if (!haveForward)
            return false;

        // CARS:
        // Car4Seat/yellow was perfect in Features46 at a 340 uu Jason stand
        // and a measured 265.3 uu final trap distance. Preserve that.
        //
        // Car2Seat/blue had ZERO nav-valid points even though the human trace
        // proves a legal front placement exists. Human blue-car geometry was
        // about 305 uu Jason->car center and ~243 uu trap->center, only ~6 deg
        // off the vehicle forward axis. Use that calibration for Car2Seat and
        // fall back to an exact world-space Morph when nav projection refuses
        // the otherwise legal front point.
        static const float FrontArcDegrees[10] =
        {
             0.0f,
             8.0f,
            -8.0f,
            15.0f,
           -15.0f,
            22.5f,
           -22.5f,
            30.0f,
           -30.0f,
            40.0f
        };

        int32_t arcIndex =
            retryCount;

        if (arcIndex < 0)
            arcIndex = 0;

        if (arcIndex > 9)
            arcIndex = 9;

        float baseDistance =
            (kind == 2) ?
            315.0f :
            340.0f;

        if (retryCount >= 6)
        {
            baseDistance +=
                (float)(retryCount - 5) *
                20.0f;
        }

        requestedStandDistance =
            baseDistance;

        constexpr float
            DegreesToRadians =
            0.017453292519943295f;

        float angleRadians =
            FrontArcDegrees[arcIndex] *
            DegreesToRadians;

        float cosAngle =
            std::cos(angleRadians);

        float sinAngle =
            std::sin(angleRadians);

        FVector frontDirection{};
        frontDirection.X =
            (forward.X * cosAngle) +
            (right.X * sinAngle);

        frontDirection.Y =
            (forward.Y * cosAngle) +
            (right.Y * sinAngle);

        frontDirection.Z =
            0.0f;

        desiredStand =
            objectiveLocation;

        desiredStand.X +=
            frontDirection.X *
            requestedStandDistance;

        desiredStand.Y +=
            frontDirection.Y *
            requestedStandDistance;

        // Prefer navmesh when it cooperates.
        FVector projectionExtent{};
        projectionExtent.X = 150.0f;
        projectionExtent.Y = 150.0f;
        projectionExtent.Z = 300.0f;

        FVector projected{};

        if (ProjectJasonAINavPointOnGameThread(
                desiredStand,
                projectionExtent,
                projected))
        {
            float snapDX =
                projected.X -
                desiredStand.X;

            float snapDY =
                projected.Y -
                desiredStand.Y;

            float snapDistance =
                std::sqrt(
                    snapDX * snapDX +
                    snapDY * snapDY);

            if (std::isfinite(snapDistance) &&
                snapDistance <= 180.0f)
            {
                if (TryTeleportCandidate(
                        projected,
                        false,
                        0.0f,
                        arcIndex))
                {
                    usedNavProjection =
                        true;

                    usedDirectFallback =
                        false;
                }
            }
        }

        // Blue-car fix: if projection says no nav point, do NOT skip the car.
        // Try the exact front-arc world position just like the user did as
        // human Jason. If it collides or is illegal, K2_TeleportTo /
        // CanPlaceTrap will reject it and the next slow retry moves a few
        // degrees around the FRONT only.
        if (!haveDestination)
        {
            TryTeleportCandidate(
                desiredStand,
                false,
                0.0f,
                arcIndex);
        }
    }

    if (!haveDestination)
    {
        Logger::Debug(
            std::string(
                "Jason AI trap setup: Morph candidate failed for ") +
            JasonAIStartupTrapKindName(kind) +
            " | retry=" +
            std::to_string(retryCount) +
            " | requestedStandDistance=" +
            std::to_string(
                requestedStandDistance));

        return false;
    }

    FaceJasonAIAtStartupTrapObjectiveOnGameThread(
        objective);

    g_JasonAIState.PathLocked = false;
    g_JasonAIState.PathLockUntil = 0;
    g_JasonAIState.LastAcceptedMoveAt = 0;
    g_JasonAIState.HaveLastLocation = false;

    // The local shack-door setup starts the same 20-second opening clock as a
    // strategic Morph. Jason can place the entrance trap during that window,
    // but cannot immediately jump from the shack to the phone box afterward.
    MarkJasonAIMorphTeleportUsed(
        JasonAIStartupTrapKindName(
            kind));

    Logger::Success(
        std::string(
            "Jason AI trap setup morph: objective=") +
        JasonAIStartupTrapKindName(kind) +
        " | actor=" +
        JasonAISafeName((UObject*)objective) +
        " | retry=" +
        std::to_string(retryCount) +
        " | destination=" +
        std::to_string(finalDestination.X) + "," +
        std::to_string(finalDestination.Y) + "," +
        std::to_string(finalDestination.Z) +
        " | requestedStandDistance=" +
        std::to_string(requestedStandDistance) +
        " | actualStandDistance=" +
        std::to_string(actualStandDistance) +
        " | wallSideOrArc=" +
        std::to_string(chosenDirection) +
        " | nav=" +
        (usedNavProjection ? "true" : "false") +
        " | directFallback=" +
        (usedDirectFallback ? "true" : "false"));

    return true;
}


static AActor* FindNearbyJasonAIPlacedKillerTrapOnGameThread(
    AActor* jason)
{
    if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
        return nullptr;

    UWorld* world = Engine::GetWorld();
    if (!world || !Memory::IsReadable(world, sizeof(UWorld)))
        return nullptr;

    FVector jasonLocation{};
    if (!GetJasonAIActorLocation(jason, jasonLocation))
        return nullptr;

    constexpr uintptr_t Offset_Levels = 0x110;
    TArray<ULevel*>* levels =
        (TArray<ULevel*>*)((uintptr_t)world + Offset_Levels);

    if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
        !levels->Data || levels->Count <= 0 || levels->Count > 1024 ||
        !Memory::IsReadable(levels->Data,
            sizeof(ULevel*) * levels->Count))
    {
        return nullptr;
    }

    AActor* bestTrap = nullptr;
    float bestDistance = FLT_MAX;

    for (int32_t levelIndex = 0;
        levelIndex < levels->Count;
        ++levelIndex)
    {
        ULevel* level = levels->Data[levelIndex];
        if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
            continue;

        TArray<AActor*>& actors = level->Actors;
        if (!actors.Data || actors.Count <= 0 || actors.Count > 100000 ||
            !Memory::IsReadable(actors.Data,
                sizeof(AActor*) * actors.Count))
            continue;

        for (int32_t i = 0; i < actors.Count; ++i)
        {
            AActor* actor = actors.Data[i];
            if (!actor || !actor->Class ||
                !Memory::IsReadable(actor, sizeof(UObject)) ||
                !Memory::IsReadable(actor->Class, sizeof(UObject)))
                continue;

            if (actor->Class->GetName() != "KillerTrap_C")
                continue;

            FVector trapLocation{};
            if (!GetJasonAIActorLocation(actor, trapLocation))
                continue;

            float dx = trapLocation.X - jasonLocation.X;
            float dy = trapLocation.Y - jasonLocation.Y;
            float dz = trapLocation.Z - jasonLocation.Z;
            float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

            // Human baseline was 123-129 uu 3D from Jason. Allow generous
            // terrain variance, but not an old trap elsewhere on the map.
            if (!std::isfinite(distance) || distance > 260.0f)
                continue;

            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestTrap = actor;
            }
        }
    }

    return bestTrap;
}

static void FinishJasonAIStartupTrapSetupOnGameThread(const char* reason)
{
    ULONGLONG now = GetTickCount64();
    g_JasonAIState.StartupTrapSetupActive = false;
    g_JasonAIState.StartupTrapAttemptSent = false;
    g_JasonAIState.StartupTrapTeleported = false;
    g_JasonAIState.StartupTrapMorphChargeActive = false;
    g_JasonAIState.StartupTrapMorphChargeUntil = 0;
    g_JasonAIState.StartupTrapMorphChargeKey = -1;
    g_JasonAIState.StartupTrapTransitActive = false;
    g_JasonAIState.StartupTrapTransitGoal = nullptr;
    g_JasonAIState.StartupTrapTransitNextMoveAt = 0;

    // Preserve the original OfflineBots opening after objective defense.
    bool counselorTeleportOK = RunOfflineBotsInitialTeleportOnGameThread();

    if (counselorTeleportOK)
    {
        MarkJasonAIMorphTeleportUsed(
            "CounselorHunt");
    }

    g_JasonAIState.InitialTeleportAttempted = counselorTeleportOK;
    if (!counselorTeleportOK)
        g_JasonAIState.InitialTeleportAt = now + 500;

    g_JasonAIState.NextKnifeAttemptAt = now + 3500;
    g_JasonAIState.NextCombatAttemptAt = now + 900;
    // MarkJasonAIMorphTeleportUsed owns the 20-second distance-Morph gate.

    Logger::Success(
        std::string("Jason AI startup trap setup complete: reason=") +
        (reason ? reason : "complete") +
        " | objectives=" + std::to_string(g_JasonAIState.StartupTrapObjectiveCount) +
        " | counselorTeleport=" + (counselorTeleportOK ? "true" : "false"));
}

static bool RunJasonAIStartupTrapSetupOnGameThread()
{
    if (!g_JasonAIState.StartupTrapSetupActive)
        return false;

    AActor* jason = g_JasonAIState.Jason;
    if (!jason || !Memory::IsReadable(jason, sizeof(UObject)))
    {
        g_JasonAIState.StartupTrapSetupActive = false;
        return false;
    }

    ULONGLONG now = GetTickCount64();

    // Normal trap phases/charges freeze Jason, but the phone objective has one
    // intentional walking phase so he can precisely find the legal fuse-box
    // trap position instead of guessing it with teleport coordinates.
    if (!g_JasonAIState.StartupTrapPhoneApproachActive &&
        !g_JasonAIState.StartupTrapTransitActive)
    {
        StopJasonAIMovementForKnifeOnGameThread();
    }

    if (now < g_JasonAIState.StartupTrapNextActionAt)
        return true;

    if (!g_JasonAIState.StartupTrapObjectivesReady)
    {
        bool haveObjectives = BuildJasonAIStartupTrapObjectivesOnGameThread();

        if (now < g_JasonAIState.StartupTrapSetupStartedAt + 3500 &&
            g_JasonAIState.StartupTrapDiscoveryReads < 4)
        {
            g_JasonAIState.StartupTrapNextActionAt = now + 400;
            return true;
        }

        g_JasonAIState.StartupTrapObjectivesReady = true;
        Logger::Debug(
            std::string("Jason AI trap setup objectives: found=") +
            std::to_string(g_JasonAIState.StartupTrapObjectiveCount) +
            " | haveAny=" + (haveObjectives ? "true" : "false"));

        if (!haveObjectives)
        {
            FinishJasonAIStartupTrapSetupOnGameThread("no-objectives");
            return true;
        }
    }

    // Real-Jason Morph pacing.  The previous builds waited mostly AFTER a
    // teleport, which still made the teleport itself appear instantaneous.
    // This charge happens BEFORE K2_TeleportTo, including the very first
    // objective and the final Morph that starts counselor hunting.
    auto HoldForStartupMorphCharge =
        [&](int32_t chargeKey,
            const char* label,
            AActor* walkGoal) -> bool
    {
        ULONGLONG readyAt =
            JasonAIMorphReadyAt();

        // First Morph of the round remains intentionally quick.
        if (readyAt == 0)
        {
            g_JasonAIState.StartupTrapMorphChargeActive = false;
            g_JasonAIState.StartupTrapMorphChargeUntil = 0;
            g_JasonAIState.StartupTrapMorphChargeKey = chargeKey;
            g_JasonAIState.StartupTrapTransitActive = false;
            g_JasonAIState.StartupTrapTransitGoal = nullptr;
            g_JasonAIState.StartupTrapTransitNextMoveAt = 0;
            return false;
        }

        // While charging, Jason advances toward the next strategic location
        // and attacks any counselor who enters his existing combat windows.
        if (now < readyAt)
        {
            ULONGLONG remaining =
                readyAt - now;

            if (!g_JasonAIState.StartupTrapMorphChargeActive ||
                g_JasonAIState.StartupTrapMorphChargeKey != chargeKey)
            {
                g_JasonAIState.StartupTrapMorphChargeActive = true;
                g_JasonAIState.StartupTrapMorphChargeKey = chargeKey;
                g_JasonAIState.StartupTrapMorphChargeUntil = readyAt;
                g_JasonAIState.StartupTrapTransitActive = true;
                g_JasonAIState.StartupTrapTransitGoal = nullptr;
                g_JasonAIState.StartupTrapTransitNextMoveAt = 0;

                Logger::Debug(
                    std::string(
                        "Jason AI Morph cooldown/charge START: objective=") +
                    (label ? label : "Unknown") +
                    " | remainingMs=" +
                    std::to_string(
                        remaining) +
                    " | fullCooldownMs=" +
                    std::to_string(
                        JasonAIMorphCooldownMs) +
                    " | walkingAndFighting=true"
                );
            }

            g_JasonAIState.StartupTrapTransitActive =
                true;

            TickJasonAIStartupMorphTransitOnGameThread(
                walkGoal,
                label,
                now);

            // Keep the startup state machine alive while walking/fighting.
            // Do not sleep all the way to readyAt.
            g_JasonAIState.StartupTrapNextActionAt =
                now + 100;

            return true;
        }

        // The 20 seconds are charged. If Jason is in the middle of a knife,
        // slash, grab, or has a counselor right on top of him, finish that
        // immediate threat before disappearing.
        bool combatPriority =
            TickJasonAIStartupMorphTransitOnGameThread(
                walkGoal,
                label,
                now);

        if (combatPriority)
        {
            g_JasonAIState.StartupTrapTransitActive =
                true;

            g_JasonAIState.StartupTrapNextActionAt =
                now + 100;

            return true;
        }

        if (g_JasonAIState.StartupTrapMorphChargeActive)
        {
            Logger::Success(
                std::string(
                    "Jason AI Morph cooldown COMPLETE: objective=") +
                (label ? label : "Unknown")
            );
        }

        StopJasonAIMovementForKnifeOnGameThread();

        g_JasonAIState.StartupTrapMorphChargeActive = false;
        g_JasonAIState.StartupTrapMorphChargeUntil = 0;
        g_JasonAIState.StartupTrapMorphChargeKey = chargeKey;
        g_JasonAIState.StartupTrapTransitActive = false;
        g_JasonAIState.StartupTrapTransitGoal = nullptr;
        g_JasonAIState.StartupTrapTransitNextMoveAt = 0;

        return false;
    };

    if (g_JasonAIState.StartupTrapObjectiveIndex >=
        g_JasonAIState.StartupTrapObjectiveCount)
    {
        const int32_t finalChargeKey =
            g_JasonAIState.StartupTrapObjectiveCount + 100;

        AActor* counselorWalkGoal =
            FindNearestJasonAICounselorTarget(
                jason);

        if (HoldForStartupMorphCharge(
                finalChargeKey,
                "CounselorHunt",
                counselorWalkGoal))
        {
            return true;
        }

        FinishJasonAIStartupTrapSetupOnGameThread("all-objectives-visited");
        return true;
    }

    int32_t index = g_JasonAIState.StartupTrapObjectiveIndex;
    AActor* objective = g_JasonAIState.StartupTrapObjectives[index];
    uint8_t kind = g_JasonAIState.StartupTrapObjectiveKinds[index];

    if (!objective || !Memory::IsReadable(objective, sizeof(UObject)))
    {
        Logger::Debug(
            std::string("Jason AI trap setup: objective invalid | kind=") +
            JasonAIStartupTrapKindName(kind));

        ++g_JasonAIState.StartupTrapObjectiveIndex;
        g_JasonAIState.StartupTrapTeleported = false;
        g_JasonAIState.StartupTrapRetryCount = 0;
        g_JasonAIState.StartupTrapAttemptSent = false;
        g_JasonAIState.StartupTrapCountConsumed = false;
        g_JasonAIState.StartupTrapActorConfirmed = false;
        g_JasonAIState.StartupTrapConfirmedActor = nullptr;
        g_JasonAIState.StartupTrapTransitActive = false;
        g_JasonAIState.StartupTrapTransitGoal = nullptr;
        g_JasonAIState.StartupTrapTransitNextMoveAt = 0;
        g_JasonAIState.StartupTrapNextActionAt = now + 750;
        return true;
    }

    // After the stock count changes, DO NOT morph away. Human Jason stayed
    // locked in the completed placement sequence for about 3.25-3.5 seconds.
    if (g_JasonAIState.StartupTrapCountConsumed)
    {
        if (!g_JasonAIState.StartupTrapActorConfirmed &&
            (g_JasonAIState.StartupTrapLastConfirmScanAt == 0 ||
             now >= g_JasonAIState.StartupTrapLastConfirmScanAt + 250))
        {
            g_JasonAIState.StartupTrapLastConfirmScanAt = now;

            AActor* placedTrap =
                FindNearbyJasonAIPlacedKillerTrapOnGameThread(jason);

            if (placedTrap)
            {
                g_JasonAIState.StartupTrapConfirmedActor = placedTrap;
                g_JasonAIState.StartupTrapActorConfirmed = true;

                FVector trapLocation{};
                GetJasonAIActorLocation(placedTrap, trapLocation);

                Logger::Success(
                    std::string("Jason AI trap VISIBLE actor confirmed: objective=") +
                    JasonAIStartupTrapKindName(kind) +
                    " | actor=" + JasonAISafeName((UObject*)placedTrap) +
                    " | location=" +
                    std::to_string(trapLocation.X) + "," +
                    std::to_string(trapLocation.Y) + "," +
                    std::to_string(trapLocation.Z) +
                    " | afterCountMs=" +
                    std::to_string(now - g_JasonAIState.StartupTrapPlacedAt));
            }
        }

        constexpr ULONGLONG HumanPlacementHoldMs = 5000;

        if (now < g_JasonAIState.StartupTrapPlacedAt + HumanPlacementHoldMs)
        {
            g_JasonAIState.StartupTrapNextActionAt = now + 100;
            return true;
        }

        Logger::Success(
            std::string("Jason AI trap placement sequence complete: objective=") +
            JasonAIStartupTrapKindName(kind) +
            " | visibleActor=" +
            (g_JasonAIState.StartupTrapActorConfirmed ? "true" : "false") +
            " | holdMs=" + std::to_string(HumanPlacementHoldMs));

        ++g_JasonAIState.StartupTrapObjectiveIndex;
        g_JasonAIState.StartupTrapAttemptSent = false;
        g_JasonAIState.StartupTrapTeleported = false;
        g_JasonAIState.StartupTrapRetryCount = 0;
        g_JasonAIState.StartupTrapCountConsumed = false;
        g_JasonAIState.StartupTrapActorConfirmed = false;
        g_JasonAIState.StartupTrapConfirmedActor = nullptr;
        g_JasonAIState.StartupTrapPlacedAt = 0;
        g_JasonAIState.StartupTrapLastConfirmScanAt = 0;
        g_JasonAIState.StartupTrapPhoneApproachActive = false;
        g_JasonAIState.StartupTrapPhoneApproachStartedAt = 0;
        g_JasonAIState.StartupTrapPhoneNextMoveAt = 0;
        g_JasonAIState.StartupTrapPhoneNextProbeAt = 0;
        g_JasonAIState.StartupTrapPhoneCandidateHoldAt = 0;
        g_JasonAIState.StartupTrapPhoneStableSamples = 0;
        g_JasonAIState.StartupTrapPhoneCandidateHoldActive = false;

        // Placement is done.  Pause briefly, then the NEXT objective begins a
        // real remaining 20-second Morph cooldown. The timer began at the last
        // successful Morph, so trap-placement time naturally counts as recharge.
        g_JasonAIState.StartupTrapMorphChargeActive = false;
        g_JasonAIState.StartupTrapMorphChargeUntil = 0;
        g_JasonAIState.StartupTrapTransitActive = false;
        g_JasonAIState.StartupTrapTransitGoal = nullptr;
        g_JasonAIState.StartupTrapTransitNextMoveAt = 0;

        // After the completed trap montage, start walking toward the next
        // objective almost immediately. The Morph timer has already been
        // charging since the previous successful teleport.
        g_JasonAIState.StartupTrapNextActionAt = now + 250;
        return true;
    }

    if (g_JasonAIState.StartupTrapAttemptSent)
    {
        int32_t currentCount = ReadJasonAITrapCount(jason);

        if (g_JasonAIState.StartupTrapCountBefore >= 0 &&
            currentCount >= 0 &&
            currentCount < g_JasonAIState.StartupTrapCountBefore)
        {
            g_JasonAIState.StartupTrapCountConsumed = true;
            g_JasonAIState.StartupTrapPlacedAt = now;
            g_JasonAIState.StartupTrapLastConfirmScanAt = 0;

            Logger::Success(
                std::string("Jason AI trap stock count consumed: objective=") +
                JasonAIStartupTrapKindName(kind) +
                " | trapCount=" +
                std::to_string(g_JasonAIState.StartupTrapCountBefore) + "->" +
                std::to_string(currentCount) +
                " | beginning real-Jason 5.0s placement/charge hold");

            g_JasonAIState.StartupTrapNextActionAt = now + 50;
            return true;
        }

        if (now < g_JasonAIState.StartupTrapAttemptAt + 5000)
        {
            g_JasonAIState.StartupTrapNextActionAt = now + 100;
            return true;
        }

        Logger::Debug(
            std::string("Jason AI trap setup attempt timed out: objective=") +
            JasonAIStartupTrapKindName(kind) +
            " | trapCount=" + std::to_string(currentCount));

        g_JasonAIState.StartupTrapAttemptSent = false;
        g_JasonAIState.StartupTrapTeleported = false;
        ++g_JasonAIState.StartupTrapRetryCount;

        if (g_JasonAIState.StartupTrapRetryCount >= 4)
        {
            Logger::Debug(
                std::string("Jason AI trap setup skipping objective after slow retries: ") +
                JasonAIStartupTrapKindName(kind));
            ++g_JasonAIState.StartupTrapObjectiveIndex;
            g_JasonAIState.StartupTrapRetryCount = 0;
        }

        g_JasonAIState.StartupTrapNextActionAt = now + 1500;
        return true;
    }

    // Only stop for zero inventory when there is NO placement already in
    // flight.  On the final trap, AttemptPlaceTrap can decrement 1->0 before
    // the next AI tick; the old ordering exited immediately and chopped off
    // visible confirmation / the placement hold for that last objective.
    int32_t trapCount = ReadJasonAITrapCount(jason);

    if (trapCount == 0)
    {
        const int32_t finalChargeKey =
            g_JasonAIState.StartupTrapObjectiveCount + 200;

        AActor* counselorWalkGoal =
            FindNearestJasonAICounselorTarget(
                jason);

        if (HoldForStartupMorphCharge(
                finalChargeKey,
                "CounselorHunt",
                counselorWalkGoal))
        {
            return true;
        }

        FinishJasonAIStartupTrapSetupOnGameThread("out-of-traps");
        return true;
    }

    if (!g_JasonAIState.StartupTrapTeleported)
    {
        // A failed placement/teleport retry must recharge too. Include the
        // retry number in the key so every actual Morph attempt gets its own
        // deliberate pre-teleport charge rather than chaining instantly.
        const int32_t objectiveChargeKey =
            (index * 100) +
            g_JasonAIState.StartupTrapRetryCount;

        if (HoldForStartupMorphCharge(
                objectiveChargeKey,
                JasonAIStartupTrapKindName(kind),
                objective))
        {
            return true;
        }

        bool teleportOK =
            TeleportJasonAIToStartupTrapObjectiveOnGameThread(
                objective,
                kind,
                g_JasonAIState.StartupTrapRetryCount);

        if (!teleportOK)
        {
            g_JasonAIState.StartupTrapPhoneApproachActive = false;
            g_JasonAIState.StartupTrapPhoneApproachStartedAt = 0;
            g_JasonAIState.StartupTrapPhoneNextMoveAt = 0;
            g_JasonAIState.StartupTrapPhoneNextProbeAt = 0;
            g_JasonAIState.StartupTrapPhoneCandidateHoldAt = 0;
            g_JasonAIState.StartupTrapPhoneStableSamples = 0;
            g_JasonAIState.StartupTrapPhoneCandidateHoldActive = false;
            g_JasonAIState.StartupTrapMorphChargeActive = false;
            g_JasonAIState.StartupTrapMorphChargeUntil = 0;
            g_JasonAIState.StartupTrapTransitActive = false;
            g_JasonAIState.StartupTrapTransitGoal = nullptr;
            g_JasonAIState.StartupTrapTransitNextMoveAt = 0;
            ++g_JasonAIState.StartupTrapRetryCount;

            if (g_JasonAIState.StartupTrapRetryCount >= 4)
            {
                Logger::Debug(
                    std::string("Jason AI trap setup: unable to find legal Morph point, skipping ") +
                    JasonAIStartupTrapKindName(kind));
                ++g_JasonAIState.StartupTrapObjectiveIndex;
                g_JasonAIState.StartupTrapRetryCount = 0;
            }

            g_JasonAIState.StartupTrapNextActionAt = now + 1500;
            return true;
        }

        g_JasonAIState.StartupTrapTeleported = true;
        g_JasonAIState.StartupTrapMorphChargeActive = false;
        g_JasonAIState.StartupTrapMorphChargeUntil = 0;
        g_JasonAIState.StartupTrapTransitActive = false;
        g_JasonAIState.StartupTrapTransitGoal = nullptr;
        g_JasonAIState.StartupTrapTransitNextMoveAt = 0;

        if (kind == 1)
        {
            g_JasonAIState.StartupTrapPhoneApproachActive = true;
            g_JasonAIState.StartupTrapPhoneApproachStartedAt = 0;
            g_JasonAIState.StartupTrapPhoneNextMoveAt = 0;
            g_JasonAIState.StartupTrapPhoneNextProbeAt = 0;
            g_JasonAIState.StartupTrapPhoneCandidateHoldAt = 0;
            g_JasonAIState.StartupTrapPhoneStableSamples = 0;
            g_JasonAIState.StartupTrapPhoneCandidateHoldActive = false;

            // Quick settle after the FIRST Morph, then Jason walks the final
            // few meters to the fuse panel instead of teleporting onto the trap.
            g_JasonAIState.StartupTrapNextActionAt = now + 500;
        }
        else
        {
            // Cars remain exactly as proven: arrive, settle, then stock trap.
            g_JasonAIState.StartupTrapNextActionAt = now + 1500;
        }

        return true;
    }

    if (kind == 1 &&
        g_JasonAIState.StartupTrapPhoneApproachActive)
    {
        bool approachStillValid =
            TickJasonAIPhoneTrapApproachOnGameThread(
                objective);

        if (g_JasonAIState.StartupTrapPhoneApproachActive)
        {
            // true = still walking; false = timed out and should retry.
            if (!approachStillValid)
            {
                g_JasonAIState.StartupTrapTeleported = false;
                ++g_JasonAIState.StartupTrapRetryCount;
                g_JasonAIState.StartupTrapNextActionAt = now + 1000;
            }

            return true;
        }

        // Precise legal spot was found. Tick helper has stopped movement and
        // scheduled a 500ms settle; do not fall through in this same frame.
        return true;
    }

    FaceJasonAIAtStartupTrapObjectiveOnGameThread(objective);
    StopJasonAIMovementForKnifeOnGameThread();

    int32_t countBefore = ReadJasonAITrapCount(jason);
    if (countBefore == 0)
    {
        FinishJasonAIStartupTrapSetupOnGameThread("out-of-traps");
        return true;
    }

    HMODULE module = GetModuleHandle(nullptr);
    if (!module)
    {
        FinishJasonAIStartupTrapSetupOnGameThread("module-unavailable");
        return true;
    }

    // Exact player path:
    // AttemptPlaceTrap(0x3FCB10) first calls CanPlaceTrap(0x3FE120).
    // Call the validator ourselves so an indoor / too-close position NEVER
    // consumes inventory. The two FVector outputs are the game's own legal
    // placement solution and surface normal/context.
    constexpr uintptr_t RVA_CanPlaceTrap = 0x003FE120;
    constexpr uintptr_t RVA_AttemptPlaceTrap = 0x003FCB10;

    FVector stockPlacement{};
    FVector stockSurface{};
    bool canPlace = false;

    uintptr_t canPlaceAddress =
        (uintptr_t)module + RVA_CanPlaceTrap;

    bool canPlaceCallOK =
        Memory::IsReadable((void*)canPlaceAddress, 1) &&
        SafeJasonCanPlaceTrapCall(
            jason,
            canPlaceAddress,
            &stockPlacement,
            &stockSurface,
            &canPlace);

    FVector objectiveWorld{};
    float stockDistanceFromObjective = -1.0f;

    if (GetJasonAIActorLocation(objective, objectiveWorld))
    {
        float stockDX =
            stockPlacement.X - objectiveWorld.X;

        float stockDY =
            stockPlacement.Y - objectiveWorld.Y;

        stockDistanceFromObjective =
            std::sqrt(
                stockDX * stockDX +
                stockDY * stockDY);
    }

    Logger::Debug(
        std::string("Jason AI trap stock CanPlaceTrap=") +
        (canPlace ? "true" : "false") +
        " | callOK=" + (canPlaceCallOK ? "true" : "false") +
        " | objective=" + JasonAIStartupTrapKindName(kind) +
        " | retry=" + std::to_string(g_JasonAIState.StartupTrapRetryCount) +
        " | stockLocation=" +
        std::to_string(stockPlacement.X) + "," +
        std::to_string(stockPlacement.Y) + "," +
        std::to_string(stockPlacement.Z) +
        " | stockDistanceFromObjective=" +
        std::to_string(stockDistanceFromObjective));

    if (!canPlaceCallOK || !canPlace)
    {
        // Do not call AttemptPlaceTrap at an illegal point. Morph to the next
        // candidate slowly and try again.
        g_JasonAIState.StartupTrapTeleported = false;
        g_JasonAIState.StartupTrapPhoneApproachActive = false;
        g_JasonAIState.StartupTrapPhoneApproachStartedAt = 0;
        g_JasonAIState.StartupTrapPhoneNextMoveAt = 0;
        g_JasonAIState.StartupTrapPhoneNextProbeAt = 0;
        g_JasonAIState.StartupTrapPhoneCandidateHoldAt = 0;
        g_JasonAIState.StartupTrapPhoneStableSamples = 0;
        g_JasonAIState.StartupTrapPhoneCandidateHoldActive = false;
        ++g_JasonAIState.StartupTrapRetryCount;

        if (g_JasonAIState.StartupTrapRetryCount >= 4)
        {
            Logger::Debug(
                std::string("Jason AI trap setup: stock CanPlaceTrap never accepted, skipping ") +
                JasonAIStartupTrapKindName(kind));
            ++g_JasonAIState.StartupTrapObjectiveIndex;
            g_JasonAIState.StartupTrapRetryCount = 0;
        }

        g_JasonAIState.StartupTrapNextActionAt = now + 1500;
        return true;
    }

    uintptr_t attemptAddress =
        (uintptr_t)module + RVA_AttemptPlaceTrap;

    bool attemptOK =
        Memory::IsReadable((void*)attemptAddress, 1) &&
        SafeJasonInteractionCall(jason, attemptAddress);

    Logger::Debug(
        std::string("Jason AI trap stock AttemptPlaceTrap=") +
        (attemptOK ? "true" : "false") +
        " | objective=" + JasonAIStartupTrapKindName(kind) +
        " | trapCount=" + std::to_string(countBefore));

    if (!attemptOK)
    {
        g_JasonAIState.StartupTrapTeleported = false;
        ++g_JasonAIState.StartupTrapRetryCount;
        g_JasonAIState.StartupTrapNextActionAt = now + 1000;
        return true;
    }

    g_JasonAIState.StartupTrapCountBefore = countBefore;
    g_JasonAIState.StartupTrapAttemptAt = now;
    g_JasonAIState.StartupTrapAttemptSent = true;
    g_JasonAIState.StartupTrapNextActionAt = now + 100;
    return true;
}

static bool RunOfflineBotsControllerTickOnGameThread()
{
    // Features45: the spawn-opening objective defense pass owns Jason until
    // stock trap placement at phone/cars completes or safely skips.
    if (g_JasonAIState.StartupTrapSetupActive)
    {
        RunJasonAIStartupTrapSetupOnGameThread();
        return true;
    }

    //
    // Finish a confirmed door break quickly before resuming
    // MoveTo/combat.
    //
    if (TickJasonAIDoorBreakOnGameThread())
    {
        return true;
    }

    // Features42 retains the proven knife rule: while a throw is active, do not issue another MoveTo.
    // The previous builds kept refreshing chase movement during the montage,
    // which matched the user's report that the throw looked rushed/skipped.
    if (g_JasonAIState.KnifeSequenceActive)
    {
        RunJasonAICombatOnGameThread();
        return true;
    }

    //
    // First half of OfflineBots custom Tick:
    // update the counselor chase/path request.
    //
    bool chaseOK =
        RunJasonAIChaseUpdateOnGameThread();

    if (!chaseOK)
        return false;

    AActor* jason =
        g_JasonAIState.Jason;

    if (!jason ||
        !Memory::IsReadable(
            jason,
            sizeof(UObject)))
    {
        return true;
    }

    //
    // Run combat before the special-state early-outs.
    // This is important because a successful attack/grab
    // may set those state bytes, and a pending attack
    // release / grab-kill input still has to be processed.
    //
    RunJasonAICombatOnGameThread();

    //
    // OfflineBots FUN_1403CCC80 directly checks
    // these two Jason state bytes before allowing
    // the +0xDA0(true) call.
    //
    uint8_t* stateE98 =
        (uint8_t*)
        ((uintptr_t)jason +
            0xE98);

    uint8_t* stateEE8 =
        (uint8_t*)
        ((uintptr_t)jason +
            0xEE8);

    if (!Memory::IsReadable(
        stateE98,
        1) ||
        !Memory::IsReadable(
            stateEE8,
            1))
    {
        return true;
    }

    //
    // Either flag means Jason is currently in
    // one of the action/special states where
    // OfflineBots skips this call.
    //
    if (*stateE98 != 0 ||
        *stateEE8 != 0)
    {
        return true;
    }

    //
    // Second half of the actual OfflineBots
    // SCKillerAIController Tick:
    //
    // Jason virtual +0xDA0(true)
    //
    uintptr_t jasonVTable =
        *(uintptr_t*)jason;

    if (!jasonVTable ||
        !Memory::IsReadable(
            (void*)jasonVTable,
            sizeof(uintptr_t)))
    {
        return true;
    }

    constexpr uintptr_t
        OfflineBotsDriverVTableOffset =
        0xDA0;

    uintptr_t driverSlot =
        jasonVTable +
        OfflineBotsDriverVTableOffset;

    if (!Memory::IsReadable(
        (void*)driverSlot,
        sizeof(uintptr_t)))
    {
        return true;
    }

    uintptr_t driverAddress =
        *(uintptr_t*)
        driverSlot;

    if (!driverAddress ||
        !Memory::IsReadable(
            (void*)driverAddress,
            1))
    {
        return true;
    }

    bool driverCallOK =
        SafeJasonDriverCall(
            jason,
            driverAddress
        );

    if (!g_JasonAIState.DriverTailLogged)
    {
        g_JasonAIState.DriverTailLogged =
            true;

        Logger::Success(
            std::string(
                "Jason AI OfflineBots Tick driver +0xDA0="
            ) +
            (driverCallOK ?
                "true" :
                "false")
        );
    }

    return true;
}

static void __fastcall
OfflineBotsKillerControllerTickHook(
    UObject* controller,
    float deltaSeconds)
{
    using TickFn =
        void(__fastcall*)(
            UObject*,
            float
            );

    if (!controller)
        return;

    //
    // The shared SCKillerAIController vtable is
    // hooked, so any unrelated killer controller
    // must retain the original Resurrected Tick.
    //
    if (controller != g_JasonAIState.Controller)
    {
        if (FrozenBridgeDriveAdditionalJason(controller, deltaSeconds))
            return;

        if (g_OriginalKillerControllerTick)
        {
            TickFn originalTick =
                (TickFn)g_OriginalKillerControllerTick;
            originalTick(controller, deltaSeconds);
        }
        return;
    }

    if (!g_JasonAIState.Active)
    {
        if (g_OriginalKillerControllerTick)
        {
            TickFn originalTick =
                (TickFn)
                g_OriginalKillerControllerTick;

            originalTick(
                controller,
                deltaSeconds
            );
        }

        return;
    }

    HMODULE module =
        GetModuleHandle(nullptr);

    if (!module)
        return;

    //
    // Confirmed in Resurrected Ghidra:
    //
    // FUN_1411360F0 =
    // inherited/base AAIController Tick.
    //
    constexpr uintptr_t
        RVA_BaseAIControllerTick =
        0x011360F0;

    uintptr_t baseTickAddress =
        (uintptr_t)module +
        RVA_BaseAIControllerTick;

    if (!Memory::IsReadable(
        (void*)baseTickAddress,
        1))
    {
        return;
    }

    TickFn baseTick =
        (TickFn)
        baseTickAddress;

    //
    // This reproduces the working OfflineBots
    // controller structure:
    //
    // base AAIController Tick
    // -> chase update
    // -> Jason state test
    // -> Jason +0xDA0(true)
    //
    baseTick(
        controller,
        deltaSeconds
    );

    RunOfflineBotsControllerTickOnGameThread();
}


bool Features::QueueFirstJasonSandbox()
{
    bool expected = false;
    if (!g_JasonRequestPending.compare_exchange_strong(expected, true))
    {
        Logger::Debug("Jason request already pending");
        return false;
    }

    Logger::Debug("Jason request queued for game thread");
    return true;
}

bool Features::HasQueuedJasonRequest() const
{
    // The ProcessEvent bridge uses one wake-up test for both bot types.
    return
        g_JasonRequestPending.load() ||
        g_CounselorRequestPending.load();
}

bool Features::RequestFirstJasonSandbox()
{
    return QueueFirstJasonSandbox();
}

bool Features::ConsumeQueuedJasonRequestOnGameThread()
{
    // Jason request has priority when both keys are pressed together.
    if (g_JasonRequestPending.exchange(false))
    {
        Logger::Success(
            "Jason AI spawn executing on game thread | thread=" +
            std::to_string(GetCurrentThreadId()));

        bool result = SpawnJasonZombieAIOnGameThread();
        if (result)
            g_JasonRequestUsed.store(true);

        return result;
    }

    if (g_CounselorRequestPending.exchange(false))
    {
        Logger::Success(
            "Counselor AI spawn executing on game thread | thread=" +
            std::to_string(GetCurrentThreadId()));

        return SpawnCounselorBotOnGameThread();
    }

    return false;
}

void Features::TickAIOnly()
{
    if (!Engine::IsInGame())
        return;

    // Read/cache only. Actual spawns are always consumed by the game-thread
    // ProcessEvent bridge; Jason AI itself is driven by SCKillerAIController Tick.
    PrecacheJasonAIResources();
}
