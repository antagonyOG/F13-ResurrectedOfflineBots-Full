

#include "Engine.hpp"
#include "../../Utils/Logger/Logger.hpp"
#include "../../Utils/Memory.hpp"
#include "../Features/Features.hpp"
#include "../Setup/OfflineSetup.hpp"
#include "../Setup/FrozenJasonBridge.hpp"
#include "../../../vendor/minhook/include/MinHook.h"
#include <Psapi.h>
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <vector>

using namespace Memory;

UEngine** GEngine = nullptr;
UWorld** GWorld = nullptr;
TNameEntryArray* GNames = nullptr;
static void* FindGObjects();

namespace {
    // VirtualQuery is useful as a coarse probe, but the allocation can be
    // released after it returns.  UE4 replaces the world graph during travel,
    // so leaf reads made by the backend worker need an actual SEH boundary.
    __declspec(noinline) bool TryReadPod(
        const void* source,
        void* destination,
        size_t size)
    {
        if (!source || !destination || size == 0)
            return false;

        __try {
            std::memcpy(destination, source, size);
            return true;
        }
        __except (
            GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
            GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                ? EXCEPTION_EXECUTE_HANDLER
                : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    template<typename T>
    bool TryReadValue(const void* source, T& value)
    {
        return TryReadPod(source, &value, sizeof(value));
    }
}

namespace Patterns {
    const char* GEngine1 = "\x48\x8B\x05\x00\x00\x00\x00\x48\x85\xC0\x74\x00\x48\x8B\x10";
    const char* GEngine1Mask = "xxx????xxxx?xxx";

    const char* GEngine2 = "\x48\x89\x05\x00\x00\x00\x00\x48\x85\xC9";
    const char* GEngine2Mask = "xxx????xxx";

    const char* GEngine3 = "\x48\x8B\x05\x00\x00\x00\x00\x48\x8B\x88";
    const char* GEngine3Mask = "xxx????xxx";

    const char* GWorld1 = "\x48\x8B\x1D\x00\x00\x00\x00\x48\x85\xDB\x74";
    const char* GWorld1Mask = "xxx????xxxx";

    const char* GWorld2 = "\x48\x8B\x05\x00\x00\x00\x00\x48\x8B\x88\x00\x00\x00\x00\x48\x85\xC9";
    const char* GWorld2Mask = "xxx????xxx????xxx";

    
    const char* GNames1 = "\x48\x8B\x05\x00\x00\x00\x00\x48\x85\xC0\x75\x50\xB9\x00\x40\x00\x00"; 
    const char* GNames1Mask = "xxx????xxxxxx?x??";

    
    const char* GNames2 = "\x48\x8B\x05\x00\x00\x00\x00\x48\x85\xC0\x75\x00\xB9\x00\x40\x00\x00\x48\x89\x05"; 
    const char* GNames2Mask = "xxx????xxxx?x?x??xxx";
    
    
    const char* GNames3 = "\x48\x8B\x05\x00\x00\x00\x00\x48\x85\xC0\x75\x00\x48\x8D\x15";
    const char* GNames3Mask = "xxx????xxxx?xxx";
}

UFunction* UClass::GetFunction(const char* ClassName, const char* FuncName) const {
    
    bool foundAny = false;
    for (UField* field = this->Children; field; field = field->Next) {
        UFunction* func = (UFunction*)field;
        if (func) {
            std::string fname = func->GetName();
            Logger::Debug("UClass::GetFunction: found function '" + fname + "'");
            foundAny = true;
            if (fname == FuncName) {
                Logger::Success("UClass::GetFunction: matched '" + fname + "'");
                return func;
            }
        }
    }
    if (!foundAny) {
        Logger::Error("UClass::GetFunction: No functions found in Children linked list");
    } else {
        Logger::Error(std::string("UClass::GetFunction: No match for '") + FuncName + "'");
    }
    
    Logger::Error(std::string("UClass::GetFunction: not found '") + FuncName + "'");
    return nullptr;
}


static uintptr_t PatternScan(const char* pattern, const char* mask) {
    MODULEINFO modInfo = { 0 };
    HMODULE hModule = GetModuleHandle(nullptr);
    if (!hModule) return 0;

    GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(MODULEINFO));

    uintptr_t base = (uintptr_t)hModule;
    uintptr_t size = (uintptr_t)modInfo.SizeOfImage;
    size_t patternLength = strlen(mask);

    for (uintptr_t i = 0; i < size - patternLength; i++) {
        bool found = true;
        for (size_t j = 0; j < patternLength; j++) {
            if (mask[j] != '?' && pattern[j] != *(char*)(base + i + j)) {
                found = false;
                break;
            }
        }
        if (found) return base + i;
    }
    return 0;
}

std::string UObject::GetName() const {
    if (!GNames) return "NoGNames";
    
    
    if (!GNames->IsValidIndex(NameIndex)) {
        
        static bool once = false;
        if (!once) { 
             Logger::Debug("Bad NameIndex: " + std::to_string(NameIndex) + " Max: " + std::to_string(GNames->NumElements));
             once = true; 
        }
        return "NullEntry";
    }

    auto entry = GNames->GetById(NameIndex);
    if (!entry) {
        static bool once2 = false;
        if (!once2) {
             Logger::Debug("Null entry for index: " + std::to_string(NameIndex));
             once2 = true;
        }
        return "NullEntry";
    }

    if (IsReadable(entry, 0x20)) {
         return std::string(entry->AnsiName);
    }
    return "UnreadableEntry";
}

static bool IsValidGNames(TNameEntryArray* names) {
    if (!names) return false;
    if (!Memory::IsReadable(names, 0x420)) return false;

    
    if (names->NumElements < 1000 || names->NumElements > 500000) return false;
    if (names->NumChunks < 1 || names->NumChunks > 128) return false;

    
    if (!Memory::IsValidPointer((uintptr_t)names->Chunks[0])) return false;
    if (!Memory::IsReadable(names->Chunks[0], sizeof(void*))) return false;

    
    const FNameEntry* entry0 = names->GetById(0);
    if (!entry0 || !Memory::IsReadable(entry0, 0x10)) return false;
    
    
    if (strcmp(entry0->AnsiName, "None") != 0) return false;

    return true;
}

static bool TryInitGNames(uintptr_t addr) {
    if (!addr) return false;
    
    int offset = *(int*)(addr + 3);
    uintptr_t gNamesPtrAddr = addr + offset + 7; 
    
    if (!Memory::IsReadable((void*)gNamesPtrAddr, 8)) return false;
    TNameEntryArray* candidate = *(TNameEntryArray**)gNamesPtrAddr;
    
    if (IsValidGNames(candidate)) {
         GNames = candidate;
         Logger::Success("GNames validated via pattern. NumElements: " + std::to_string(GNames->NumElements));
         return true;
    }
    return false;
}

static void BruteForceGNames() {
    Logger::Debug("Starting GNames brute-force...");
    MODULEINFO modInfo = { 0 };
    HMODULE hModule = GetModuleHandle(nullptr);
    GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(MODULEINFO));
    
    uintptr_t start = (uintptr_t)hModule;
    uintptr_t end = start + modInfo.SizeOfImage;

    for (uintptr_t p = start; p < end; p += 8) {
        
        
        
        uintptr_t val = *(uintptr_t*)p;
        
        if (!Memory::IsValidPointer(val)) continue;
        
        
        if (Memory::IsReadable((void*)val, 0x410)) {
             if (IsValidGNames((TNameEntryArray*)val)) {
                 GNames = (TNameEntryArray*)val;
                 Logger::Success("GNames found via brute-force at offset: " + std::to_string(p - start));
                 return;
             }
        }
    }
    Logger::Error("GNames brute-force failed.");
}

static bool FindGNames() {
    
    uintptr_t modBase =
        (uintptr_t)GetModuleHandle(nullptr);

    constexpr uintptr_t knownOffset = 0x02FA5EB8;

    uintptr_t knownAddr =
        modBase + knownOffset;

    for (int i = 0; i < 50; i++) {
        if (Memory::IsReadable((void*)knownAddr, 8)) {
            TNameEntryArray* candidate =
                *(TNameEntryArray**)knownAddr;

            if (Memory::IsValidPointer(
                (uintptr_t)candidate) &&
                IsValidGNames(candidate))
            {
                GNames = candidate;

                Logger::Success(
                    "engine: gnames found via Resurrected known offset (0x2FA5EB8)"
                );

                return true;
            }
        }
        Sleep(100);
    }
    
    if (TryInitGNames(PatternScan(Patterns::GNames1, Patterns::GNames1Mask))) return true;
    
    
    
    BruteForceGNames();
    
    if (GNames) return true;
    
    return false;
}

static bool FindGEngine() {
    uintptr_t addr = PatternScan(Patterns::GEngine1, Patterns::GEngine1Mask);
    if (addr) {
        int offset = *(int*)(addr + 3);
        GEngine = (UEngine**)(addr + offset + 7);
        return true;
    }

    addr = PatternScan(Patterns::GEngine2, Patterns::GEngine2Mask);
    if (addr) {
        int offset = *(int*)(addr + 3);
        GEngine = (UEngine**)(addr + offset + 7);
        return true;
    }

    addr = PatternScan(Patterns::GEngine3, Patterns::GEngine3Mask);
    if (addr) {
        int offset = *(int*)(addr + 3);
        GEngine = (UEngine**)(addr + offset + 7);
        return true;
    }

    return false;
}

static bool FindGWorld() {
    uintptr_t modBase =
        (uintptr_t)GetModuleHandle(nullptr);

    constexpr uintptr_t knownOffset = 0x03245C28;

    uintptr_t knownAddr =
        modBase + knownOffset;

    if (Memory::IsReadable(
        (void*)knownAddr,
        sizeof(UWorld*)))
    {
        GWorld =
            (UWorld**)knownAddr;

        Logger::Success(
            "engine: gworld global found at Resurrected offset 0x3245C28"
        );

        return true;
    }

    Logger::Error(
        "engine: known gworld global unreadable, trying patterns"
    );

    uintptr_t addr = PatternScan(Patterns::GWorld1, Patterns::GWorld1Mask);
    if (addr) {
        int offset = *(int*)(addr + 3);
        GWorld = (UWorld**)(addr + offset + 7);
        Logger::Success("engine: gworld found via pattern GWorld1");
        if (GWorld && *GWorld) return true;
    } else {
        Logger::Error("FindGWorld: Pattern GWorld1 not found");
    }

    addr = PatternScan(Patterns::GWorld2, Patterns::GWorld2Mask);
    if (addr) {
        int offset = *(int*)(addr + 3);
        GWorld = (UWorld**)(addr + offset + 7);
        Logger::Success("engine: gworld found via pattern GWorld2");
        if (GWorld && *GWorld) return true;
    } else {
        Logger::Error("FindGWorld: Pattern GWorld2 not found");
    }

    Logger::Error("engine: gworld not found by any method");
    return false;
}


static void* FindGObjects() {
    constexpr uintptr_t GOBJECTS_OFFSET = 0x030F6EF0;

    HMODULE hModule = GetModuleHandle(nullptr);
    if (!hModule) return nullptr;
    uintptr_t base = (uintptr_t)hModule;

    
    uintptr_t cand = base + GOBJECTS_OFFSET;
    
    if (!Memory::IsReadable((void*)cand, sizeof(uintptr_t) + sizeof(int32_t) * 2)) return nullptr;
    uintptr_t objectsPtr = *(uintptr_t*)(cand + 0x00);
    int32_t num = *(int32_t*)(cand + 0x0C);
    if (num <= 0 || num > 20000000) return nullptr;
    if (!Memory::IsValidPointer(objectsPtr)) return nullptr;
    if (!Memory::IsReadable((void*)objectsPtr, sizeof(uintptr_t) * 2)) return nullptr;

    
    uintptr_t firstItem = *(uintptr_t*)objectsPtr;
    if (!Memory::IsValidPointer(firstItem)) return nullptr;
    if (!Memory::IsReadable((void*)firstItem, sizeof(UObject))) return nullptr;

    return (void*)cand;
}


static UFunction* FindFunctionInGObjects(const char* funcName, const char* className = nullptr, int maxInspect = 100000) {
    
    Logger::Error(std::string("FindFunctionInGObjects: DISABLED (requested '") + (funcName?funcName:"<null>") + "')");
    return nullptr;
} 


extern "C" __declspec(noinline) bool SafeProcessEventCall(uintptr_t vptr, void* obj, void* func, void* params);



static UFunction* FindFunctionInGObjectsSafe(const char* funcName, const char* className = nullptr, int maxInspect = 5000, bool substringClassMatch = false, bool substringFuncMatch = false) {
    void* gObjectsCand = FindGObjects();
    if (!gObjectsCand) return nullptr;

    uintptr_t cand = (uintptr_t)gObjectsCand;
    
    if (!Memory::IsReadable((void*)cand, sizeof(uintptr_t) + sizeof(int32_t) * 2)) return nullptr;

    uintptr_t objectsPtr = *(uintptr_t*)(cand + 0x00);
    int32_t num = *(int32_t*)(cand + 0x0C);
    if (num <= 0) return nullptr;

    
    int cap = 200000; 
    int limit = 0;
    if (maxInspect < 0) limit = std::min<int>(num, cap); else limit = std::min<int>(num, maxInspect);

    for (int i = 0; i < limit; ++i) {
        uintptr_t addrPtr = objectsPtr + (uintptr_t)i * sizeof(uintptr_t);
        if (!Memory::IsReadable((void*)addrPtr, sizeof(uintptr_t))) continue;
        uintptr_t entryAddr = *(uintptr_t*)addrPtr;

        if (!entryAddr) continue;
        if (!Memory::IsReadable((void*)entryAddr, sizeof(UObject))) continue;

        UObject* obj = (UObject*)entryAddr;
        std::string nm = obj->GetName();

        bool funcMatch = false;
        if (substringFuncMatch) {
            if (nm.find(funcName) != std::string::npos) funcMatch = true;
        } else {
            if (nm == funcName) funcMatch = true;
        }
        if (!funcMatch) continue;

        
        if (!Memory::IsReadable((void*)obj, sizeof(UFunction))) continue;
        UFunction* f = (UFunction*)obj;

        if (className) {
            
            UObject* outer = (UObject*)f->OuterPrivate;
            if (!outer) continue;
            if (!Memory::IsReadable((void*)outer, sizeof(UObject))) continue;
            std::string outerName = outer->GetName();
            if (substringClassMatch) {
                if (outerName.find(className) == std::string::npos) continue;
            } else {
                if (outerName != className) continue;
            }
        }

        Logger::Debug(std::string("FindFunctionInGObjectsSafe: found '") + funcName + "' (entry=" + std::to_string(entryAddr) + ")");
        return f;
    }

    
    static uint64_t lastNotFoundLog = 0;
    uint64_t now = GetTickCount64();
    if (now - lastNotFoundLog > 5000) {
        Logger::Debug(std::string("FindFunctionInGObjectsSafe: not found '") + (funcName?funcName:"<null>") + "' (scanned=" + std::to_string(limit) + ")");
        lastNotFoundLog = now;
    }
    return nullptr;
} 


using ProcessEventFn =
void(__fastcall*)(
    UObject*,
    UFunction*,
    void*
    );

static ProcessEventFn
g_ProcessEventOriginal = nullptr;

static bool
g_ProcessEventBridgeInstalled = false;

static DWORD
g_GameThreadId = 0;

static bool IsReceiveTickFunction(
    UFunction* function)
{
    if (!function ||
        !Memory::IsReadable(
            function,
            sizeof(UFunction)) ||
        !GNames)
    {
        return false;
    }

    int32_t nameIndex =
        function->NameIndex;

    if (!GNames->IsValidIndex(
        nameIndex))
    {
        return false;
    }

    const FNameEntry* entry =
        GNames->GetById(
            nameIndex
        );

    if (!entry ||
        !Memory::IsReadable(
            entry,
            0x20))
    {
        return false;
    }

    return
        strcmp(
            entry->AnsiName,
            "ReceiveTick"
        ) == 0;
}

static void __fastcall ProcessEventGameThreadHook(
    UObject* object,
    UFunction* function,
    void* params)
{
    bool aiRequestPending =
        func &&
        func->HasQueuedJasonRequest();

    bool setupRequestPending =
        OfflineSetup::HasPendingRequest();

    bool requestPending =
        aiRequestPending ||
        setupRequestPending;

    bool needFunctionCheck =
        g_GameThreadId == 0 ||
        requestPending;

    bool isReceiveTick =
        needFunctionCheck &&
        IsReceiveTickFunction(
            function
        );

    if (isReceiveTick &&
        g_GameThreadId == 0)
    {
        g_GameThreadId =
            GetCurrentThreadId();

        Logger::Success(
            "Game thread confirmed | thread=" +
            std::to_string(
                g_GameThreadId
            )
        );
    }

    bool consumeJasonRequest =
        requestPending &&
        isReceiveTick &&
        g_GameThreadId != 0 &&
        GetCurrentThreadId() ==
        g_GameThreadId;

    const bool onConfirmedGameThread =
        g_GameThreadId != 0 &&
        GetCurrentThreadId() == g_GameThreadId;

    // ObjectivesWidget is loaded by the pause/map screen, which can suspend
    // ReceiveTick. Gate on the widget instance itself and patch its bytecode
    // before forwarding its first event; all other setup work keeps the
    // established ReceiveTick-only dispatch path below.
    // Objective widgets can construct while the pause screen is bringing the
    // gameplay UI up, before ReceiveTick has supplied the normal game-thread
    // The first valid Pamela-window slash deliberately holds Jason in the
    // stock kill stance for at most thirty seconds. Suppress only its EndStun
    // event during that bounded interval; every unrelated ProcessEvent keeps
    // the original path unchanged.
    // Cabinet construction is also an exact-name event gate. Patch its native
    // spawn list before the original construction script creates/attaches the
    // hidden drawer item; this avoids the stale-pointer freeze caused by
    // destroying container-owned loot afterward.
    FrozenJasonBridge::ReplaceOfflineCabinetLootBeforeSpawn(
        object,
        function);

    if (FrozenJasonBridge::SuppressPamelaKneelEndStun(object, function))
    {
        return;
    }

    if (g_ProcessEventOriginal)
    {
        g_ProcessEventOriginal(
            object,
            function,
            params
        );
    }

    // ClientsPlayOutro is the reflected event. Preserve its stock completion
    // path, then skip the cabin sequence on a later game-thread event.
    if (onConfirmedGameThread)
        FrozenJasonBridge::SkipDeadJasonCabinOutroAfterStart(object, function);

    // Do not rewrite BlueprintUpdateCamera's parameter block here. Its
    // Blueprint-generated layout is not ABI-stable across the packed builds,
    // and treating it as a native struct can corrupt adjacent event memory.
    // The Jason spectator camera is advanced through the stock
    // ClientSetSpectatorCamera call from the game-thread maintenance path.

    FrozenJasonBridge::ObservePamelaTranceDamageEvent(
        object,
        function);

    // ServerViewNext/PrevPlayer asks GameMode::CanSpectate for every
    // PlayerState after the local counselor dies. Preserve the complete
    // stock cycle and widen only the active counselor-mode AI Jason entry.
    FrozenJasonBridge::AllowCounselorToSpectateJason(
        function,
        params);
    FrozenJasonBridge::HandleCounselorSpectatorCycleEvent(
        object,
        function);

    if (consumeJasonRequest)
    {
        if (aiRequestPending && func)
        {
            func->
                ConsumeQueuedJasonRequestOnGameThread();
        }

        if (setupRequestPending)
        {
            OfflineSetup::
                ConsumePendingRequestOnGameThread();
        }
    }
}

static bool InstallProcessEventGameThreadBridge()
{
    if (g_ProcessEventBridgeInstalled)
        return true;

    static ULONGLONG lastAttempt = 0;

    ULONGLONG now =
        GetTickCount64();

    if (now - lastAttempt < 2000)
        return false;

    lastAttempt = now;

    APlayerController* controller =
        Engine::GetLocalPlayerController();

    if (!controller ||
        !Memory::IsReadable(
            controller,
            sizeof(UObject)) ||
        !controller->VTable)
    {
        return false;
    }

    constexpr size_t ProcessEventSlot =
        0x3F;

    uintptr_t* vtable =
        reinterpret_cast<uintptr_t*>(
            controller->VTable
            );

    if (!Memory::IsReadable(
        vtable,
        sizeof(uintptr_t) *
        (ProcessEventSlot + 1)))
    {
        return false;
    }

    uintptr_t target =
        vtable[ProcessEventSlot];

    if (!target ||
        !Memory::IsReadable(
            reinterpret_cast<void*>(
                target
                ),
            16))
    {
        return false;
    }

    MH_STATUS initStatus =
        MH_Initialize();

    if (initStatus != MH_OK &&
        initStatus !=
        MH_ERROR_ALREADY_INITIALIZED)
    {
        Logger::Error(
            "ProcessEvent bridge: MH_Initialize failed"
        );

        return false;
    }

    MH_STATUS createStatus =
        MH_CreateHook(
            reinterpret_cast<LPVOID>(
                target
                ),
            reinterpret_cast<LPVOID>(
                &ProcessEventGameThreadHook
                ),
            reinterpret_cast<LPVOID*>(
                &g_ProcessEventOriginal
                )
        );

    if (createStatus != MH_OK &&
        createStatus !=
        MH_ERROR_ALREADY_CREATED)
    {
        Logger::Error(
            "ProcessEvent bridge: MH_CreateHook failed"
        );

        return false;
    }

    MH_STATUS enableStatus =
        MH_EnableHook(
            reinterpret_cast<LPVOID>(
                target
                )
        );

    if (enableStatus != MH_OK &&
        enableStatus !=
        MH_ERROR_ENABLED)
    {
        Logger::Error(
            "ProcessEvent bridge: MH_EnableHook failed"
        );

        return false;
    }

    g_ProcessEventBridgeInstalled = true;

    Logger::Success(
        "ProcessEvent game-thread bridge installed"
    );

    return true;
}

namespace Engine {
    bool Initialize() {


        bool foundGNames = FindGNames();
        bool foundGWorld = FindGWorld();
        bool foundGEngine = FindGEngine();
        void* foundGObjects = FindGObjects();

        if (foundGNames) Logger::Success("engine: gnames found");
        else Logger::Error("engine: gnames not found");

        if (foundGWorld) Logger::Success("engine: gworld found");
        else Logger::Error("engine: gworld not found");

        if (foundGEngine) Logger::Success("engine: gengine found");
        else Logger::Error("engine: gengine not found");

        if (foundGObjects)
        {
            Logger::Success(
                "engine: gobjects found at Resurrected offset 0x30F6EF0"
            );
        }
        else
        {
            Logger::Error(
                "engine: gobjects not found"
            );
        }

        if (foundGWorld || foundGEngine) {
            Logger::Debug("engine: waiting for world");
            int attempts = 0;
            while (!GetWorld() && attempts < 30) {
                Sleep(1000);
                attempts++;
            }
        }

        if (GetWorld()) {
            Logger::Success("engine: world ready");


            return true;
        }

        Logger::Error("engine: init failed");
        return false;
    }



    static void DebugOnceTag(const char* tag, bool& flag) {
        if (!flag) { Logger::Debug(tag); flag = true; }
    }

    bool IsInGame() {
        static bool lastState = false;
        static bool log_world_bad = false;
        static bool log_level_bad = false;
        static bool log_instance_bad = false;
        static bool log_players_bad = false;
        static bool log_localplayer_bad = false;
        static bool log_controller_bad = false;
        static bool log_levelactors_bad = false;

        UWorld* world = GetWorld();
        if (!world) {
            DebugOnceTag("ingame: world_bad", log_world_bad);
            if (lastState) { Logger::Debug("state: ingame_off"); lastState = false; }
            return false;
        }

        ULevel* level = nullptr;
        if (!TryReadValue(&world->PersistentLevel, level) || !level) {
            DebugOnceTag("ingame: level_bad", log_level_bad);
            if (lastState) { Logger::Debug("state: ingame_off"); lastState = false; }
            return false;
        }

        TArray<AActor*> actors = {};
        if (!TryReadValue(&level->Actors, actors) ||
            !actors.Data || actors.Count <= 0 ||
            actors.Max < actors.Count || actors.Count > 1000000) {
            DebugOnceTag("ingame: levelactors_bad", log_levelactors_bad);
            if (lastState) { Logger::Debug("state: ingame_off"); lastState = false; }
            return false;
        }
        AActor* firstActor = nullptr;
        if (!TryReadValue(actors.Data, firstActor)) {
            DebugOnceTag("ingame: levelactors_unreadable", log_levelactors_bad);
            if (lastState) { Logger::Debug("state: ingame_off"); lastState = false; }
            return false;
        }

        UGameInstance* gameInstance = nullptr;
        if (!TryReadValue(&world->OwningGameInstance, gameInstance) ||
            !gameInstance) {
            DebugOnceTag("ingame: instance_bad", log_instance_bad);
            if (lastState) { Logger::Debug("state: ingame_off"); lastState = false; }
            return false;
        }

        TArray<void*> players = {};
        if (!TryReadValue(&gameInstance->LocalPlayers, players) ||
            !players.Data || players.Count <= 0 || players.Count > 16 ||
            players.Max < players.Count || players.Max > 16) {
            DebugOnceTag("ingame: players_bad", log_players_bad);
            if (lastState) { Logger::Debug("state: ingame_off"); lastState = false; }
            return false;
        }

        ULocalPlayer* localPlayer = nullptr;
        if (!TryReadValue(players.Data, localPlayer) || !localPlayer) {
            DebugOnceTag("ingame: localplayer_bad", log_localplayer_bad);
            if (lastState) { Logger::Debug("state: ingame_off"); lastState = false; }
            return false;
        }

        APlayerController* controller = nullptr;
        UClass* controllerClass = nullptr;
        if (!TryReadValue(&localPlayer->PlayerController, controller) ||
            !controller ||
            !TryReadValue(&controller->Class, controllerClass) ||
            !controllerClass) {
            DebugOnceTag("ingame: controller_bad", log_controller_bad);
            if (lastState) { Logger::Debug("state: ingame_off"); lastState = false; }
            return false;
        }

        // Reject a snapshot straddling the frontend/gameplay world swap.
        if (GetWorld() != world) {
            if (lastState) { Logger::Debug("state: ingame_off"); lastState = false; }
            return false;
        }

        if (!lastState) {
            Logger::Debug("state: ingame_on");
            lastState = true;
        }

        if (!g_ProcessEventBridgeInstalled)
            InstallProcessEventGameThreadBridge();

        return true;
    }

    UWorld* GetWorld() {
        // Once GWorld has been resolved, its transient null is authoritative:
        // falling back to the viewport here can resurrect the destroyed
        // frontend world during travel.
        if (GWorld) {
            UWorld* world = nullptr;
            return TryReadValue(GWorld, world) ? world : nullptr;
        }

        UEngine* engine = nullptr;
        if (!GEngine || !TryReadValue(GEngine, engine) || !engine)
            return nullptr;

        UGameViewportClient* gameViewport = nullptr;
        if (!TryReadValue(&engine->GameViewport, gameViewport) ||
            !gameViewport)
            return nullptr;

        UWorld* world = nullptr;
        return TryReadValue(&gameViewport->World, world) ? world : nullptr;
    }

    ULocalPlayer* GetLocalPlayer() {
        UWorld* world = GetWorld();
        if (!world)
            return nullptr;

        UGameInstance* gameInstance = nullptr;
        if (!TryReadValue(&world->OwningGameInstance, gameInstance) ||
            !gameInstance)
            return nullptr;

        TArray<void*> players = {};
        if (!TryReadValue(&gameInstance->LocalPlayers, players) ||
            !players.Data || players.Count <= 0 || players.Count > 16 ||
            players.Max < players.Count || players.Max > 16)
            return nullptr;

        ULocalPlayer* localPlayer = nullptr;
        if (!TryReadValue(players.Data, localPlayer) || !localPlayer)
            return nullptr;

        return GetWorld() == world ? localPlayer : nullptr;
    }

    APlayerController* GetPlayerController() {
        auto localPlayer = GetLocalPlayer();
        if (!localPlayer)
            return nullptr;

        APlayerController* controller = nullptr;
        return TryReadValue(&localPlayer->PlayerController, controller)
            ? controller
            : nullptr;
    }

    APlayerController* GetLocalPlayerController() {
        return GetPlayerController();
    }

    ACharacter* GetLocalCharacter() {
        auto Controller = GetPlayerController();
        if (!Controller) return nullptr;

        return (ACharacter*)Controller->AcknowledgedPawn;
    }

    ASCCharacter* GetSCCharacter() {
        auto Controller = GetPlayerController();
        if (!Controller) return nullptr;

        return (ASCCharacter*)Controller->AcknowledgedPawn;
    }

    UCharacterMovementComponent* GetMovementComponent() {
        __try {
            auto Character = GetLocalCharacter();
            if (!Character) return nullptr;

            return (UCharacterMovementComponent*)Character->CharacterMovement;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    APlayerCameraManager* GetCameraManager() {
        __try {
            auto Controller = GetPlayerController();
            if (!Controller) return nullptr;
            if (!Controller->PlayerCameraManager) return nullptr;

            return (APlayerCameraManager*)Controller->PlayerCameraManager;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    USCFearComponent* GetFearComponent() {
        __try {
            auto Character = GetSCCharacter();
            if (!Character) return nullptr;

            return (USCFearComponent*)Character->FearManager;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    void SetPlayerLookInput(bool bIgnore) {
        __try {
            auto Controller = GetPlayerController();
            if (!Controller) return;




            bool* ignoreLookInput = (bool*)((uintptr_t)Controller + 0x590);
            *ignoreLookInput = bIgnore;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {

        }
    }


}



extern "C" __declspec(noinline) bool SafeInvokeProcessEvent(void* procPtr, void* obj, void* func, void* params) {
    using ProcessEventFunc = void(*)(void*, void*, void*);
    if (!procPtr) return false;
    ProcessEventFunc processEvent = (ProcessEventFunc)procPtr;
    __try {
        processEvent(obj, func, params);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


extern "C" __declspec(noinline) bool SafeProcessEventCall(
    uintptr_t vptr,
    void* obj,
    void* func,
    void* params)
{
    if (!vptr || !obj || !func)
        return false;

    if (!Memory::IsReadable(
        (void*)vptr,
        sizeof(uintptr_t)))
        return false;

    uintptr_t vtable =
        *(uintptr_t*)vptr;

    if (!vtable)
        return false;

    constexpr size_t ProcessEventSlot = 0x3F;

    if (!Memory::IsReadable(
        (void*)vtable,
        sizeof(uintptr_t) * (ProcessEventSlot + 1)))
        return false;

    void* procPtr =
        ((void**)vtable)[ProcessEventSlot];

    if (!procPtr ||
        !Memory::IsReadable(procPtr, 16))
        return false;

    return SafeInvokeProcessEvent(
        procPtr,
        obj,
        func,
        params
    );
}


extern "C" __declspec(noinline) bool SafeWriteMemory(void* dst, const void* src, size_t len) {
    if (!dst || !src || len == 0) return false;
    __try {
        memcpy(dst, src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static UClass* FindUClassByName(const char* className, int maxInspect = 50000) {
    
    Logger::Error(std::string("FindUClassByName: DISABLED (requested '") + (className?className:"<null>") + "')");
    return nullptr;
}


FVector AActor::K2_GetActorLocation() const {
    return { 0.0f, 0.0f, 0.0f };
}

bool APlayerController::ProjectWorldLocationToScreen(const FVector& WorldLocation, FVector2D* ScreenLocation, bool ) {
    if (ScreenLocation) {
        
        ScreenLocation->X = 960.0f;
        ScreenLocation->Y = 540.0f;
    }
    
    return false;
}
