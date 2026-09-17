#include "OfflineSetup.hpp"
#include "FrozenJasonBridge.hpp"
#include "../Engine/Engine.hpp"
#include "../../Utils/Memory.hpp"
#include "../../Utils/Logger/Logger.hpp"
#include <Windows.h>
#include <atomic>
#include <array>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cwchar>
#include "../../../vendor/minhook/include/MinHook.h"

extern "C" __declspec(noinline) bool SafeProcessEventCall(
    uintptr_t vptr,
    void* obj,
    void* func,
    void* params
);

namespace
{
    enum class SetupStage : int
    {
        Idle = 0,
        NeedPreload,
        WaitingForAssets,
        ReadyToApply,
        Sticky,
        Done,
        Failed
    };

    enum class CounselorSelectStage : int
    {
        Idle = 0,
        NeedRequest,
        Monitoring,
        Done,
        Failed
    };

    enum class SandboxCounselorSyncStage : int
    {
        Idle = 0,
        NeedRequest,
        Waiting,
        Done,
        Failed
    };

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

    struct RawArray
    {
        uint8_t* Data;
        int32_t Count;
        int32_t Max;
    };

    struct RawFString
    {
        wchar_t* Data;
        int32_t Count;
        int32_t Max;
    };

    // Verified in this Shipping EXE at RVA 0xBD860 and 0x2040FE:
    // GObjects indexes (index * 3) * 8, not an array of UObject pointers.
    // Treating its count as a pointer count silently omitted the final 2/3
    // of objects, including widgets loaded after the frontend had started.
    struct NativeObjectItem
    {
        UObject* Object;
        int32_t Flags;
        int32_t ClusterRootIndex;
        int32_t SerialNumber;
        int32_t Padding;
    };
    static_assert(sizeof(NativeObjectItem) == 0x18,
        "Resurrected GObjects records must have their native 24-byte stride");

    std::atomic<int> g_Stage{ (int)SetupStage::Idle };
    std::atomic<bool> g_BackgroundScanStarted{ false };
    std::atomic<uintptr_t> g_SelectionSaveObject{ 0 };
    std::atomic<uintptr_t> g_SelectionSaveClass{ 0 };
    std::atomic<uintptr_t> g_TargetJasonClass{ 0 };
    std::atomic<int32_t> g_KillerPickOffset{ -1 };
    std::atomic<int32_t> g_CounselorPickOffset{ -1 };
    std::atomic<ULONGLONG> g_PreloadAt{ 0 };
    std::atomic<ULONGLONG> g_LastTargetScanAt{ 0 };
    std::atomic<uintptr_t> g_SelectionWorld{ 0 };
    std::atomic<uint32_t> g_StickyRewriteCount{ 0 };

    // 18K player-counselor selection remains deliberately separate from the
    // proven Jason setup state machine so both selections can coexist.
    // 18K keeps the proven stock SCPlayerState::RequestCounselorClass path with
    // the exact native 40-byte TSoftClassPtr.  No manual LoadAssetClass.
    std::atomic<int> g_CounselorStage{ (int)CounselorSelectStage::Idle };
    // 18K: once Sandbox has spawned its default local counselor (normally
    // Vanessa/Athlete), use Sandbox's own SERVER_RequestNextCharacter API
    // until SCPlayerState::SpawnedCharacterClass matches the launcher choice.
    std::atomic<int> g_SandboxCounselorSyncStage{ (int)SandboxCounselorSyncStage::Idle };
    std::atomic<uintptr_t> g_SandboxSyncWorld{ 0 };
    std::atomic<uintptr_t> g_SandboxSyncPlayerState{ 0 };
    std::atomic<int32_t> g_SandboxSpawnedClassOffset{ -1 };
    std::atomic<uint32_t> g_SandboxNextCharacterAttempts{ 0 };
    std::atomic<ULONGLONG> g_SandboxNextCharacterAt{ 0 };
    // Separate Offline Bots - Counselor architecture coordinator. 18L-AB no longer
    // uses Sandbox as the gameplay foundation; the Sandbox menu row is only the
    // temporary frontend click source and is synchronously redirected into
    // stock OfflineBots before Blueprint execution resumes.
    std::atomic<bool> g_CounselorModeAutoStartArmed{ false };
    std::atomic<bool> g_CounselorBirthComplete{ false };
    std::atomic<bool> g_CounselorMatchInProgress{ false };
    std::atomic<bool> g_CounselorJasonActive{ false };
    std::atomic<int32_t> g_CounselorBotsCreated{ 0 };
    std::atomic<uintptr_t> g_CounselorBirthPawn{ 0 };
    // 18L-AB: stock Sandbox menu row is repurposed as the counselor entry.
    std::atomic<bool> g_CounselorMenuRouteEnabled{ false };
    std::atomic<bool> g_CounselorMenuRouteLatched{ false };
    std::atomic<uintptr_t> g_CounselorMenuWorld{ 0 };
    std::atomic<uintptr_t> g_CounselorMenuObject{ 0 };
    std::atomic<uintptr_t> g_CounselorOfflineBotsModeClass{ 0 };
    std::atomic<uintptr_t> g_CounselorSandboxModeClass{ 0 };
    // The SteamRIP reference menu has a dedicated counselor row whose picker
    // sets ModeDef_OfflineBotsC_C. Keep Sandbox as a compatibility fallback
    // for the earlier two-row patched menu, but prefer the explicit mode.
    std::atomic<uintptr_t> g_CounselorReferenceModeClass{ 0 };
    std::atomic<uint32_t> g_CounselorMenuRewriteCount{ 0 };

    // 18L-AC: stop racing PendingOfflineMode from a worker thread.  Instead
    // detour the native exec wrapper for
    // ILLBackendBlueprintLibrary::RequestOfflineMode and
    // rewrite Sandbox -> OfflineBots synchronously before Blueprint execution
    // resumes.  This is route-only infrastructure; it does not alter gameplay
    // lifecycle or frozen AI.
    UFunction::FNativeFuncPtr g_OriginalRequestOfflineModeExec = nullptr;
    std::atomic<bool> g_RequestOfflineModeHookInstalled{ false };
    std::atomic<uintptr_t> g_RequestOfflineModeHookTarget{ 0 };
    std::atomic<uint32_t> g_RequestOfflineModeHookHits{ 0 };

    // The native five-entry asset reuses Resurrected's dormant VC3 row and
    // gives it the stock Offline Bots click bytecode. Mark that one event here
    // so the shared Offline Bots picker can still distinguish Counselor from
    // Jason, while the real Sandbox row remains completely stock.
    UFunction::FNativeFuncPtr g_OriginalCounselorEntryExec = nullptr;
    std::atomic<bool> g_CounselorEntryHookInstalled{ false };
    std::atomic<bool> g_CounselorEntryHookPending{ false };
    std::atomic<ULONGLONG> g_CounselorEntryHookLastAttempt{ 0 };
    std::atomic<bool> g_CounselorEntryClickPending{ false };
    std::atomic<ULONGLONG> g_CounselorEntryClickedAt{ 0 };

    // Packed-install menu presentation.  The stock Resurrected asset already
    // owns a fifth VC3 button, its slot, focus wiring, visibility binding and
    // click event.  Keep the signed stock PAK untouched and expose only that
    // dormant row at runtime.
    UFunction::FNativeFuncPtr g_OriginalVC3VisibilityExec = nullptr;
    std::atomic<bool> g_VC3VisibilityHookInstalled{ false };
    std::atomic<uintptr_t> g_RuntimeMenuLabeledWidget{ 0 };
    std::atomic<ULONGLONG> g_RuntimeMenuLabelLastAttempt{ 0 };
    std::atomic<bool> g_RuntimeMenuLabelFailureLogged{ false };
    std::atomic<int32_t> g_RuntimeMenuLabelFailureCode{ 0 };
    std::atomic<uintptr_t> g_RuntimeMenuOrderedWidget{ 0 };
    std::atomic<bool> g_RuntimeMenuOrderFailureLogged{ false };
    std::atomic<bool> g_RuntimeMenuOrderRequested{ false };
    std::atomic<uintptr_t> g_RuntimeMenuOrderWidget{ 0 };
    std::atomic<int32_t> g_RuntimeMenuOrderFailureCode{ 0 };

    // Integrated counselor picker.  Reuse Resurrected's native Customize
    // counselor widget so its portraits, roster navigation, profile save and
    // SCPlayerState::RequestCounselorClass behavior stay authoritative.  Only
    // the dedicated Offline Bots - Counselor row activates this continuation;
    // normal Customize usage is untouched.
    UFunction::FNativeFuncPtr g_OriginalCounselorPickerAcceptExec = nullptr;
    UFunction::FNativeFuncPtr g_OriginalCounselorPickerConstructExec = nullptr;
    std::atomic<bool> g_CounselorPickerAcceptHookInstalled{ false };
    std::atomic<bool> g_CounselorPickerConstructHookInstalled{ false };
    std::atomic<bool> g_CounselorPickerTransitionHookInstalled{ false };
    std::atomic<bool> g_CounselorPickerActive{ false };
    std::atomic<bool> g_CounselorPickerAccepted{ false };
    std::atomic<uintptr_t> g_CounselorPickerWidget{ 0 };
    std::atomic<uintptr_t> g_CounselorPickerSourceMenu{ 0 };
    std::atomic<bool> g_CounselorPickerOfflineCastPatched{ false };
    std::atomic<uintptr_t> g_CounselorPickerCastOperand{ 0 };
    std::atomic<uintptr_t> g_CounselorPickerCastOriginal{ 0 };
    // The proven loose build widens the SCGameState_Hunt import in eight
    // objective widgets.  Mirror those same import substitutions in the
    // runtime-expanded Ubergraphs so the stock signed PAK remains untouched.
    struct ObjectiveWidgetPatchSpec
    {
        const char* ClassName;
        const char* UbergraphName;
    };
    static constexpr ObjectiveWidgetPatchSpec g_ObjectiveWidgetPatchSpecs[] =
    {
        { "Objective_Boat_Widget_C", "ExecuteUbergraph_Objective_Boat_Widget" },
        { "Objective_Car_Widget_C", "ExecuteUbergraph_Objective_Car_Widget" },
        { "Objective_GrendelBridgeRepairWidget_C", "ExecuteUbergraph_Objective_GrendelBridgeRepairWidget" },
        { "Objective_GrendelPolice_Widget_C", "ExecuteUbergraph_Objective_GrendelPolice_Widget" },
        { "Objective_GrendelRadio_Widget_C", "ExecuteUbergraph_Objective_GrendelRadio_Widget" },
        { "Objective_Police_Widget_C", "ExecuteUbergraph_Objective_Police_Widget" },
        { "Objective_Radio_Widget_C", "ExecuteUbergraph_Objective_Radio_Widget" },
        { "ObjectivesWidget_C", "ExecuteUbergraph_ObjectivesWidget" }
    };
    static constexpr uint32_t g_AllObjectiveWidgetPatchBits =
        (1u << 8) - 1u;
    std::atomic<bool> g_ObjectivesGameStateCastPatched{ false };
    std::atomic<bool> g_ObjectivesGameStateCastPatchPending{ false };
    std::atomic<ULONGLONG> g_ObjectivesGameStateCastNextAttempt{ 0 };
    std::atomic<uint32_t> g_ObjectivesWidgetPatchMask{ 0 };
    std::atomic<uint32_t> g_ObjectivesWidgetPatchFailureLoggedMask{ 0 };
    // The stock setup widget labels this value "Counselors" (1-7). In
    // counselor mode it is the total counselor population, including the
    // local human, so the native selection maps to zero-to-six AI bots.
    UFunction::FNativeFuncPtr g_OriginalGameSetupStartExec = nullptr;
    std::atomic<bool> g_GameSetupStartHookInstalled{ false };
    std::atomic<int32_t> g_NativeSelectedCounselorTotal{ 0 };
    // Once the user presses Start, the picker/profile values are final.  The
    // selection save can briefly fall back to its default J5 entry while the
    // frontend tears down; do not let that stale value overwrite the choice
    // that was visible on the committed Game Setup screen.
    std::atomic<bool> g_GameSetupSelectionLocked{ false };
    using PlayTransitionOutFn = void(__fastcall*)(UObject*, UClass*);
    PlayTransitionOutFn g_OriginalPlayTransitionOut = nullptr;
    static constexpr uintptr_t RVA_ILLUserWidgetPlayTransitionOut = 0x2AC730;

    static void TickNativeProfileSelection();
    static bool PatchCounselorPickerConstructPlayerStateCast(
        UClass* counselorMenuClass,
        bool applyPatch);
    static void RestoreCounselorPickerPlayerStateCast();
    static bool PatchObjectivesWidgetGameStateCast(
        const char* requestedClassName = nullptr);
    static bool InstallGameSetupStartHook(UClass* settingsClass);

    static void ArmObjectivesWidgetGameStatePatch()
    {
        // ObjectivesWidget is constructed before HandleMatchHasStarted on
        // the packed Counselor route. Arm the one-shot widening when the row
        // is committed so its first Construct sees SCGameState_OfflineBots.
        g_ObjectivesGameStateCastNextAttempt.store(0);
        g_ObjectivesWidgetPatchFailureLoggedMask.store(0);
        g_ObjectivesGameStateCastPatchPending.store(
            !g_ObjectivesGameStateCastPatched.load());

        // The route commit itself is a game-thread ProcessEvent.  Patch any
        // objective classes that are already resident now; classes streamed
        // later are still handled by their first exact widget event.
        if (g_ObjectivesGameStateCastPatchPending.load() &&
            PatchObjectivesWidgetGameStateCast() &&
            g_ObjectivesGameStateCastPatched.load())
        {
            g_ObjectivesGameStateCastPatchPending.store(false);
        }
    }

    // Resurrected does not register the donor's native OfflineBotsC class or
    // its OFLBC GameMode alias in a stock packed installation. Counselor mode
    // therefore travels through the stock OFLB host and is distinguished by
    // the dedicated row latch; the hooked OfflineBots lifecycle supplies the
    // donor-style counselor birth before the intro. The legacy alias state is
    // retained only for rollback compatibility and is not armed by this path.
    std::atomic<bool> g_CounselorAliasArmed{ false };
    std::atomic<bool> g_CounselorAliasRestorePending{ false };
    std::atomic<uintptr_t> g_CounselorAliasOwner{ 0 };
    std::atomic<int32_t> g_CounselorAliasOffset{ -1 };
    RawFString g_CounselorAliasOriginal{};
    wchar_t* g_CounselorAliasAllocation = nullptr;

    // Compatibility shim for the frozen AI's initial Sandbox-only cache lookup.
    std::atomic<uintptr_t> g_AISpoofObject{ 0 };
    std::atomic<uintptr_t> g_AISpoofOriginalClass{ 0 };
    std::atomic<uintptr_t> g_AISpoofController{ 0 };
    std::atomic<uintptr_t> g_AISpoofControllerOriginalClass{ 0 };
    std::atomic<int32_t> g_SelectedPlayerCounselorIndex{ 0 };
    std::atomic<uintptr_t> g_TargetPlayerCounselorClass{ 0 };
    std::atomic<uintptr_t> g_CounselorSelectionWorld{ 0 };
    std::atomic<uintptr_t> g_CounselorPlayerState{ 0 };
    std::atomic<int32_t> g_PickedCounselorClassOffset{ -1 };
    std::atomic<ULONGLONG> g_CounselorRequestSubmittedAt{ 0 };
    std::atomic<bool> g_CounselorPickedConfirmed{ false };
    std::atomic<bool> g_CounselorInitialMismatchLogged{ false };
    std::atomic<bool> g_CounselorNaturalClassScanDone{ false };
    std::atomic<bool> g_CounselorSaveScanAttempted{ false };
    std::atomic<bool> g_CounselorFallbackApplied{ false };
    std::atomic<bool> g_CounselorStockPickLogged{ false };
    std::atomic<uint32_t> g_CounselorSoftStickyRewriteCount{ 0 };
    std::atomic<uint32_t> g_CounselorClassStickyRewriteCount{ 0 };
    std::atomic<ULONGLONG> g_NextProfileSelectionSyncAt{ 0 };
    std::atomic<ULONGLONG> g_NextAutomaticRouteArmAt{ 0 };
    std::atomic<uintptr_t> g_LastProfileCounselorClass{ 0 };
    std::atomic<uintptr_t> g_LastProfileKillerClass{ 0 };
    // The Resurrected beta notice must stay alive long enough for its native
    // StartWidget to finish Construct, but it should not require a player
    // click.  The worker only discovers the live widget; its native Accept
    // event is invoked later by the existing ProcessEvent game-thread bridge.
    std::atomic<bool> g_StartSplashAcceptRequested{ false };
    std::atomic<bool> g_StartSplashFinished{ false };
    std::atomic<uintptr_t> g_StartSplashWidget{ 0 };
    std::atomic<ULONGLONG> g_NextStartSplashScanAt{ 0 };
    std::atomic<uint32_t> g_StartSplashInactiveChecks{ 0 };
    std::array<uint8_t, 40> g_SelectedPlayerCounselorSoftClass{};
    std::string g_SelectedPlayerCounselorPath;

    static const char* kPlayerCounselorClassNames[] =
    {
        "Athlete_Counselor_C",
        "Prep_Counselor_C",
        "Bookworm_Counselor_C",
        "Flirt_Counselor_C",
        "Nerd_Counselor_C",
        "Tough_Counselor_C",
        "Hero_Counselor_C",
        "Rocker_Counselor_C",
        "Jock_Counselor_C",
        "Head_Counselor_C",
        "Stoner_Counselor_C",
        "Biker_Counselor_C",
        "Shelly_Counselor_C",
        "Catty_Counselor_C",
        "Rob_Counselor_C",
        "Tina_Counselor_C",
        "Hunter_Counselor_C"
    };

    // Unified in-process setup preset (18A):
    // Packanack Small / Hard / 3 counselors / Rain.
    //
    // This deliberately starts with the exact combination already proven by
    // the external Prototype 11 helper.  The goal of 18A is to prove those
    // same writes now work entirely inside ResurrectedOfflineBots.dll.
    std::atomic<bool> g_ArmPresetRequested{ false };
    std::atomic<bool> g_ApplyGameSetupRequested{ false };
    std::atomic<bool> g_DumpCounselorRosterRequested{ false };
    std::atomic<bool> g_MapStickyActive{ false };
    std::atomic<uintptr_t> g_MapStickyWorld{ 0 };
    std::atomic<uintptr_t> g_MapStickyMenu{ 0 };
    std::atomic<uintptr_t> g_MapStickyMapClass{ 0 };
    std::atomic<uintptr_t> g_MapStickyModeClass{ 0 };
    std::atomic<uint32_t> g_MapStickyRewriteCount{ 0 };

    // Selectable setup state. Defaults match the already-proven 18A preset.
    std::atomic<int32_t> g_SelectedMapIndex{ 5 };       // Packanack Small
    std::atomic<int32_t> g_SelectedJasonIndex{ 4 };     // Jason 5
    std::atomic<int32_t> g_SelectedDifficulty{ 2 };     // Hard
    std::atomic<int32_t> g_SelectedCounselorCount{ 3 }; // 3
    std::atomic<int32_t> g_SelectedWeather{ 1 };        // Rain

    struct MapChoice
    {
        const char* Display;
        const char* ClassName;
    };

    // Start with the six map-definition class names already proven by the
    // external setup prototypes. Jarvis/Pinehurst can be added after this
    // selectable path is verified.
    static const MapChoice kMapChoices[] =
    {
        { "Crystal Lake",        "MapDef_CrystalLake_C" },
        { "Higgins Haven",       "MapDef_HigginsHaven_C" },
        { "Packanack",           "MapDef_Packanack_C" },
        { "Crystal Lake Small",  "MapDef_CrystalLake_Small_C" },
        { "Higgins Haven Small", "MapDef_HigginsHaven_Small_C" },
        { "Packanack Small",     "MapDef_Packanack_Small_C" }
    };

    static const char* kDifficultyNames[] =
    {
        "Easy", "Normal", "Hard"
    };

    static const char* kWeatherNames[] =
    {
        "Off", "Rain", "Fog"
    };

    static const wchar_t* kWeatherStrings[] =
    {
        L"off", L"rain", L"fog"
    };

    constexpr uintptr_t GOBJECTS_OFFSET = 0x030F6EF0;
    constexpr int32_t SoftClassSize = 40;

    static bool SafeTouchObject(UObject* obj)
    {
        if (!obj)
            return false;

        __try
        {
            volatile uintptr_t vtable = *(uintptr_t*)obj;
            return vtable != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static std::string SafeName(UObject* obj)
    {
        if (!obj || !Memory::IsReadable(obj, sizeof(UObject)) || !SafeTouchObject(obj))
            return {};

        // Do not call UObject::GetName on arbitrary scan candidates.  Some
        // non-UObject-looking memory can pass the coarse pointer checks and
        // produce noisy "Bad NameIndex" diagnostics.  Validate through the
        // known Resurrected GNames table first.
        if (!GNames || !GNames->IsValidIndex(obj->NameIndex))
            return {};

        const FNameEntry* entry = GNames->GetById(obj->NameIndex);
        if (!entry || !Memory::IsReadable(entry, 0x20))
            return {};

        return std::string(entry->AnsiName);
    }

    static std::string ReadSoftClassAssetPath(const uint8_t* softClass)
    {
        if (!softClass || !Memory::IsReadable((void*)softClass, SoftClassSize))
            return {};

        // UE4 TSoftClassPtr on this build is 40 bytes.  The persistent object
        // pointer header occupies 0x10 bytes and FSoftObjectPath::AssetPathName
        // begins at +0x10 as an FName.
        const FName* assetPathName = (const FName*)(softClass + 0x10);
        if (!GNames || !GNames->IsValidIndex(assetPathName->ComparisonIndex))
            return {};

        const FNameEntry* entry = GNames->GetById(assetPathName->ComparisonIndex);
        if (!entry || !Memory::IsReadable(entry, 0x20))
            return {};

        return std::string(entry->AnsiName);
    }

    static bool SafeNameEquals(UObject* obj, const char* wanted)
    {
        // Exact-name scans need neither allocations nor several VirtualQuery
        // calls per object. Keep the whole speculative dereference guarded;
        // loaded objects can still disappear during a worker-thread scan.
        if (!obj || !wanted || !GNames)
            return false;
        __try
        {
            const FNameEntry* entry = GNames->GetById(obj->NameIndex);
            return entry && strcmp(entry->AnsiName, wanted) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool GetGObjects(NativeObjectItem** objectsOut, int32_t* countOut)
    {
        if (!objectsOut || !countOut)
            return false;

        uintptr_t moduleBase = (uintptr_t)GetModuleHandle(nullptr);
        if (!moduleBase)
            return false;

        uintptr_t base = moduleBase + GOBJECTS_OFFSET;
        if (!Memory::IsReadable((void*)base, 0x10))
            return false;

        NativeObjectItem* objects = *(NativeObjectItem**)(base + 0x00);
        int32_t count = *(int32_t*)(base + 0x0C);

        if (!objects || count <= 0 || count > 500000)
            return false;

        if (!Memory::IsReadable(objects, sizeof(NativeObjectItem) * (size_t)count))
            return false;

        *objectsOut = objects;
        *countOut = count;
        return true;
    }

    static UObject* FindObjectExact(const char* wanted)
    {
        if (!wanted)
            return nullptr;

        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!GetGObjects(&objects, &count))
            return nullptr;

        for (int32_t i = 0; i < count; ++i)
        {
            UObject* obj = objects[i].Object;
            if (SafeNameEquals(obj, wanted) &&
                Memory::IsReadable(obj, sizeof(UObject)))
                return obj;
        }

        return nullptr;
    }

    static UClass* FindClassExact(const char* wanted)
    {
        if (!wanted)
            return nullptr;

        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!GetGObjects(&objects, &count))
            return nullptr;

        for (int32_t i = 0; i < count; ++i)
        {
            UObject* obj = objects[i].Object;
            if (!SafeNameEquals(obj, wanted) ||
                !Memory::IsReadable(obj, sizeof(UObject)) || !obj->Class)
            {
                continue;
            }

            std::string meta = SafeName((UObject*)obj->Class);
            if (meta == "Class" ||
                meta.find("GeneratedClass") != std::string::npos)
            {
                return (UClass*)obj;
            }
        }

        return nullptr;
    }

    static UObject* FindFirstObjectAnyName(
        const char* a,
        const char* b,
        const char* c)
    {
        if (a)
        {
            if (auto* obj = FindObjectExact(a))
                return obj;
        }
        if (b)
        {
            if (auto* obj = FindObjectExact(b))
                return obj;
        }
        if (c)
        {
            if (auto* obj = FindObjectExact(c))
                return obj;
        }
        return nullptr;
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
            if (!Memory::IsReadable(current, sizeof(UStruct)))
                break;

            UField* field = current->Children;
            int guard = 0;

            while (field && guard++ < 2048)
            {
                if (!Memory::IsReadable(field, sizeof(UField)))
                    break;

                if (field->GetName() == targetName)
                    return (UFunction*)field;

                field = field->Next;
            }
        }

        return nullptr;
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
            if (!Memory::IsReadable(current, sizeof(UStruct)))
                break;

            UField* field = current->Children;
            int guard = 0;

            while (field && guard++ < 2048)
            {
                if (!Memory::IsReadable(field, sizeof(UField)))
                    break;

                UObject* fieldClass = (UObject*)field->ClassPrivate;
                if (fieldClass && Memory::IsReadable(fieldClass, sizeof(UObject)))
                {
                    std::string typeName = SafeName(fieldClass);
                    if (typeName.find("Property") != std::string::npos &&
                        field->GetName() == targetName)
                    {
                        auto* prop = (UPropertyLite*)field;
                        if (Memory::IsReadable(prop, sizeof(UPropertyLite)))
                            return prop;
                    }
                }

                field = field->Next;
            }
        }

        return nullptr;
    }

    static UPropertyLite* FindPropertyInStructByName(
        UStruct* owner,
        const char* targetName)
    {
        if (!owner || !targetName ||
            !Memory::IsReadable(owner, sizeof(UStruct)))
        {
            return nullptr;
        }

        UField* field = owner->Children;
        int guard = 0;
        while (field && guard++ < 256)
        {
            if (!Memory::IsReadable(field, sizeof(UField)))
                break;

            UObject* fieldClass =
                reinterpret_cast<UObject*>(field->ClassPrivate);
            if (fieldClass &&
                Memory::IsReadable(fieldClass, sizeof(UObject)) &&
                SafeName(fieldClass).find("Property") != std::string::npos &&
                field->GetName() == targetName)
            {
                auto* property = reinterpret_cast<UPropertyLite*>(field);
                return Memory::IsReadable(
                    property,
                    sizeof(UPropertyLite))
                    ? property
                    : nullptr;
            }

            field = field->Next;
        }

        return nullptr;
    }

    static UPropertyLite* FindFunctionPropertyByType(
        UFunction* function,
        const char* typeName,
        bool requireReturnProperty)
    {
        if (!function || !typeName ||
            !Memory::IsReadable(function, sizeof(UFunction)))
        {
            return nullptr;
        }

        constexpr uint64_t CPF_Parm = 0x0000000000000080ull;
        constexpr uint64_t CPF_ReturnParm = 0x0000000000000400ull;
        UField* field = function->Children;
        int guard = 0;
        while (field && guard++ < 64)
        {
            if (!Memory::IsReadable(field, sizeof(UField)))
                break;

            UObject* fieldClass =
                reinterpret_cast<UObject*>(field->ClassPrivate);
            if (fieldClass &&
                Memory::IsReadable(fieldClass, sizeof(UObject)) &&
                SafeName(fieldClass) == typeName)
            {
                auto* property = reinterpret_cast<UPropertyLite*>(field);
                if (Memory::IsReadable(property, sizeof(UPropertyLite)))
                {
                    const bool isParameter =
                        (property->PropertyFlags & CPF_Parm) != 0;
                    const bool isReturn =
                        (property->PropertyFlags & CPF_ReturnParm) != 0;
                    if (isParameter &&
                        isReturn == requireReturnProperty)
                    {
                        return property;
                    }
                }
            }

            field = field->Next;
        }

        return nullptr;
    }

    static bool InitializeCounselorActiveCamera(AActor* counselor)
    {
        if (!counselor || !counselor->Class ||
            !Memory::IsReadable(counselor, sizeof(UObject)))
        {
            return false;
        }

        // Counselor mode creates the human through RestartPlayer before the
        // OfflineBots intro. On this route Hunter_Counselor can reach its
        // first grab/pocket-knife blend without SetActiveCamera ever having
        // established a source camera; UE then chooses GrabCamera as the
        // "first" camera and leaves the player viewing an off-map transform.
        // Seed the same ordinary outdoor camera that normal counselor birth
        // uses. This changes only the character's internal active-camera
        // bookkeeping; the intro remains free to own the controller view.
        UObject* camera = nullptr;
        const char* cameraPropertyNames[] =
        {
            "OutdoorCamera",
            "OutdoorCamera_GEN_VARIABLE",
            "ChaseCamera",
            "ChaseCamera_GEN_VARIABLE"
        };
        for (const char* propertyName : cameraPropertyNames)
        {
            UPropertyLite* property = FindPropertyInHierarchyByName(
                counselor->Class,
                propertyName);
            if (!property || property->Offset_Internal <= 0 ||
                property->Offset_Internal >= 0x10000 ||
                property->ElementSize < static_cast<int32_t>(sizeof(UObject*)))
            {
                continue;
            }

            UObject** cameraSlot = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(counselor) +
                property->Offset_Internal);
            if (!Memory::IsReadable(cameraSlot, sizeof(UObject*)) ||
                !*cameraSlot ||
                !Memory::IsReadable(*cameraSlot, sizeof(UObject)))
            {
                continue;
            }

            UObject* cameraClass = reinterpret_cast<UObject*>(
                (*cameraSlot)->Class);
            if (!cameraClass ||
                !Memory::IsReadable(cameraClass, sizeof(UObject)) ||
                SafeName(cameraClass).find("Camera") == std::string::npos)
            {
                continue;
            }
            camera = *cameraSlot;
            break;
        }

        UFunction* setActiveCamera = FindFunctionInHierarchyByName(
            counselor->Class,
            "SetActiveCamera");
        if (!camera || !setActiveCamera)
        {
            Logger::Error(
                "18L-BL counselor camera seed unavailable; native camera lifecycle retained");
            return false;
        }

        constexpr uint64_t CPF_Parm = 0x0000000000000080ull;
        constexpr uint64_t CPF_ReturnParm = 0x0000000000000400ull;
        alignas(16) uint8_t params[0x100]{};
        bool parameterWritten = false;
        for (UField* field = setActiveCamera->Children;
            field;
            field = field->Next)
        {
            if (!Memory::IsReadable(field, sizeof(UField)))
                break;
            UObject* fieldClass = reinterpret_cast<UObject*>(
                field->ClassPrivate);
            if (!fieldClass ||
                !Memory::IsReadable(fieldClass, sizeof(UObject)) ||
                SafeName(fieldClass).find("ObjectProperty") ==
                    std::string::npos)
            {
                continue;
            }

            UPropertyLite* property = reinterpret_cast<UPropertyLite*>(field);
            if (!Memory::IsReadable(property, sizeof(UPropertyLite)) ||
                (property->PropertyFlags & CPF_Parm) == 0 ||
                (property->PropertyFlags & CPF_ReturnParm) != 0 ||
                property->Offset_Internal < 0 ||
                property->Offset_Internal >
                    static_cast<int32_t>(sizeof(params) - sizeof(UObject*)))
            {
                continue;
            }

            memcpy(
                params + property->Offset_Internal,
                &camera,
                sizeof(camera));
            parameterWritten = true;
            break;
        }

        if (!parameterWritten)
        {
            Logger::Error(
                "18L-BL counselor SetActiveCamera object parameter unavailable");
            return false;
        }

        const bool initialized = SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(counselor),
            counselor,
            setActiveCamera,
            params);
        Logger::Success(
            std::string("18L-BL counselor active camera seeded before intro | call=") +
            (initialized ? "true" : "false") +
            " | camera=" + SafeName(camera));
        return initialized;
    }

    static bool ResolveSelectionSaveMetadata()
    {
        if (g_SelectionSaveClass.load() &&
            g_KillerPickOffset.load() >= 0 &&
            g_CounselorPickOffset.load() >= 0)
        {
            return true;
        }

        UClass* cls = FindClassExact("SCCharacterSelectionsSaveGame");
        if (!cls || !Memory::IsReadable(cls, sizeof(UClass)))
            return false;

        auto* killerPick =
            FindPropertyInHierarchyByName(cls, "KillerPick");

        auto* counselorPick =
            FindPropertyInHierarchyByName(cls, "CounselorPick");

        if (!killerPick ||
            !counselorPick ||
            killerPick->Offset_Internal <= 0 ||
            killerPick->Offset_Internal >= 0x10000 ||
            counselorPick->Offset_Internal <= 0 ||
            counselorPick->Offset_Internal >= 0x10000)
        {
            return false;
        }

        g_SelectionSaveClass.store((uintptr_t)cls);
        g_KillerPickOffset.store(killerPick->Offset_Internal);
        g_CounselorPickOffset.store(counselorPick->Offset_Internal);

        Logger::Debug(
            "Counselor selection 18K: CounselorPick offset=0x" +
            std::to_string((uint32_t)counselorPick->Offset_Internal));

        return true;
    }

    static UObject* RawFindSelectionSaveObject(UClass* targetClass)
    {
        if (!targetClass)
            return nullptr;

        // Every live UObject is already present in GUObjectArray.  The old
        // implementation swept the process address space looking for an
        // embedded class pointer; that caused long frontend pauses and could
        // mistake arbitrary memory for a save object.  Use the verified
        // 24-byte object records and accept only an exact live class match.
        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!GetGObjects(&objects, &count))
            return nullptr;

        for (int32_t i = 0; i < count; ++i)
        {
            UObject* candidate = objects[i].Object;
            if (!candidate ||
                !Memory::IsReadable(candidate, sizeof(UObject)) ||
                candidate->Class != targetClass)
            {
                continue;
            }

            const std::string name = SafeName(candidate);
            if (!name.empty() && name.rfind("Default__", 0) != 0)
                return candidate;
        }

        return nullptr;
    }

    static bool ClassIsOrDerivesFrom(UClass* cls, UClass* target)
    {
        if (!cls || !target)
            return false;

        for (UStruct* current = (UStruct*)cls;
            current;
            current = current->Super)
        {
            if (!Memory::IsReadable(current, sizeof(UStruct)))
                break;

            if ((UClass*)current == target)
                return true;
        }

        return false;
    }

    static UObject* FindFirstLiveObjectByClass(UClass* targetClass)
    {
        if (!targetClass)
            return nullptr;

        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!GetGObjects(&objects, &count))
            return nullptr;

        for (int32_t i = 0; i < count; ++i)
        {
            UObject* candidate = objects[i].Object;
            if (!candidate ||
                !Memory::IsReadable(candidate, sizeof(UObject)) ||
                !candidate->Class ||
                !ClassIsOrDerivesFrom(candidate->Class, targetClass))
            {
                continue;
            }

            const std::string name = SafeName(candidate);
            if (!name.empty() && name.rfind("Default__", 0) != 0)
                return candidate;
        }

        return nullptr;
    }

    static void TickStartSplashAutoAccept()
    {
        if (g_StartSplashFinished.load() ||
            g_StartSplashAcceptRequested.load())
        {
            return;
        }

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG next = g_NextStartSplashScanAt.load();
        if (next && now < next)
            return;

        g_NextStartSplashScanAt.store(now + 250);

        UClass* startWidgetClass = FindClassExact("StartWidget_C");
        if (!startWidgetClass)
            return;

        UObject* widget = FindFirstLiveObjectByClass(startWidgetClass);
        if (!widget)
            return;

        g_StartSplashWidget.store((uintptr_t)widget);
        g_StartSplashAcceptRequested.store(true);
    }

    static bool AcceptStartSplashOnGameThread()
    {
        struct IsInViewportParams
        {
            uint8_t ReturnValue;
        };

        UClass* startWidgetClass = FindClassExact("StartWidget_C");
        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!startWidgetClass || !GetGObjects(&objects, &count))
            return false;

        bool sawLiveWidget = false;
        // Frontend transitions leave older hidden StartWidget instances in
        // GUObjectArray. Search newest-to-oldest and accept the one that is
        // actually visible instead of repeatedly selecting the first stale
        // instance and giving up after twelve polls.
        for (int32_t i = count - 1; i >= 0; --i)
        {
            UObject* widget = objects[i].Object;
            if (!widget ||
                !Memory::IsReadable(widget, sizeof(UObject)) ||
                !widget->Class ||
                !ClassIsOrDerivesFrom(widget->Class, startWidgetClass))
            {
                continue;
            }

            const std::string name = SafeName(widget);
            if (name.empty() || name.rfind("Default__", 0) == 0)
                continue;

            sawLiveWidget = true;
            UFunction* isInViewport =
                FindFunctionInHierarchyByName(widget->Class, "IsInViewport");
            UFunction* accept =
                FindFunctionInHierarchyByName(widget->Class, "OnAcceptClick");
            if (!isInViewport || !accept)
                continue;

            IsInViewportParams params{};
            if (!SafeProcessEventCall(
                    (uintptr_t)widget,
                    widget,
                    isInViewport,
                    &params) ||
                !params.ReturnValue)
            {
                continue;
            }

            if (!SafeProcessEventCall(
                    (uintptr_t)widget,
                    widget,
                    accept,
                    nullptr))
            {
                return false;
            }

            g_StartSplashWidget.store((uintptr_t)widget);
            g_StartSplashFinished.store(true);
            Logger::Success(
                "STARTUP SPLASH BYPASSED: visible StartWidget accepted on the game thread");
            return true;
        }

        if (sawLiveWidget)
        {
            const uint32_t checks =
                g_StartSplashInactiveChecks.fetch_add(1) + 1;
            if (checks >= 12)
            {
                g_StartSplashFinished.store(true);
                Logger::Debug(
                    "Startup splash bypass: no visible StartWidget remains");
            }
            return true;
        }
        return false;
    }

    static UObject* GetActiveFrontendMenu()
    {
        UWorld* world = Engine::GetWorld();
        if (!world)
            return nullptr;

        uintptr_t authorityGameModeAddress =
            (uintptr_t)world + 0xF0;

        if (!Memory::IsReadable(
            (void*)authorityGameModeAddress,
            sizeof(UObject*)))
        {
            return nullptr;
        }

        UObject* menu =
            *(UObject**)authorityGameModeAddress;

        if (!menu ||
            !Memory::IsReadable(menu, sizeof(UObject)) ||
            !menu->Class)
        {
            return nullptr;
        }

        UClass* scGameMenu =
            FindClassExact("SCGame_Menu");

        if (scGameMenu &&
            ClassIsOrDerivesFrom(menu->Class, scGameMenu))
        {
            return menu;
        }

        // Empirically the frontend authority GameMode is EntryGame_C.
        // Keep this fallback narrow rather than accepting arbitrary objects.
        std::string objectName = SafeName(menu);
        std::string className = SafeName((UObject*)menu->Class);

        if (objectName.find("EntryGame") != std::string::npos ||
            className.find("EntryGame") != std::string::npos)
        {
            return menu;
        }

        return nullptr;
    }

    static int32_t ClampMapIndex(int32_t value)
    {
        constexpr int32_t count =
            (int32_t)(sizeof(kMapChoices) / sizeof(kMapChoices[0]));

        if (value < 0) return 0;
        if (value >= count) return count - 1;
        return value;
    }

    static int32_t ClampJasonIndex(int32_t value)
    {
        if (value < 0) return 0;
        if (value > 14) return 14;
        return value;
    }

    static int32_t ClampDifficulty(int32_t value)
    {
        if (value < 0) return 0;
        if (value > 2) return 2;
        return value;
    }

    static int32_t ClampCounselorCount(int32_t value)
    {
        if (value < 1) return 1;
        if (value > 7) return 7;
        return value;
    }

    static int32_t GetRequestedCounselorBotCount()
    {
        const int32_t nativeTotal =
            g_NativeSelectedCounselorTotal.load();
        if (nativeTotal >= 1 && nativeTotal <= 7)
            return nativeTotal - 1;

        // Compatibility fallback for the retired controller/export path,
        // whose selector was explicitly labelled "Counselor Bots".
        int32_t fallback = ClampCounselorCount(
            g_SelectedCounselorCount.load());
        return fallback > 6 ? 6 : fallback;
    }

    static int32_t ClampWeather(int32_t value)
    {
        if (value < 0) return 0;
        if (value > 2) return 2;
        return value;
    }

    static int32_t ClampPlayerCounselorIndex(int32_t value);

    static bool ArmDedicatedCounselorAlias(UClass* modeClass)
    {
        if (g_CounselorAliasArmed.load())
            return true;

        if (!modeClass ||
            !Memory::IsReadable(modeClass, sizeof(UClass)) ||
            !modeClass->DefaultObject ||
            !Memory::IsReadable(modeClass->DefaultObject, sizeof(UObject)))
        {
            Logger::Error(
                "18L-AD OFLBC route: OfflineBots mode CDO is unavailable");
            return false;
        }

        UPropertyLite* aliasProperty =
            FindPropertyInHierarchyByName(modeClass, "Alias");

        if (!aliasProperty ||
            aliasProperty->Offset_Internal <= 0 ||
            aliasProperty->Offset_Internal >= 0x10000 ||
            aliasProperty->ElementSize < (int32_t)sizeof(RawFString))
        {
            Logger::Error(
                "18L-AD OFLBC route: SCModeDefinition::Alias property is unavailable");
            return false;
        }

        UObject* modeCDO = modeClass->DefaultObject;
        RawFString* alias =
            reinterpret_cast<RawFString*>(
                reinterpret_cast<uintptr_t>(modeCDO) +
                aliasProperty->Offset_Internal);

        if (!Memory::IsReadable(alias, sizeof(RawFString)) ||
            !alias->Data ||
            alias->Count <= 0 ||
            alias->Count > 64 ||
            alias->Max < alias->Count ||
            !Memory::IsReadable(
                alias->Data,
                sizeof(wchar_t) * (size_t)alias->Count))
        {
            Logger::Error(
                "18L-AD OFLBC route: stock OfflineBots Alias FString is invalid");
            return false;
        }

        constexpr wchar_t DedicatedAlias[] = L"oflbc";
        constexpr size_t DedicatedAliasChars =
            sizeof(DedicatedAlias) / sizeof(DedicatedAlias[0]);

        wchar_t* replacement =
            reinterpret_cast<wchar_t*>(
                HeapAlloc(
                    GetProcessHeap(),
                    HEAP_ZERO_MEMORY,
                    sizeof(DedicatedAlias)));

        if (!replacement)
        {
            Logger::Error(
                "18L-AD OFLBC route: could not allocate dedicated alias storage");
            return false;
        }

        std::wmemcpy(
            replacement,
            DedicatedAlias,
            DedicatedAliasChars);

        g_CounselorAliasOriginal = *alias;
        g_CounselorAliasAllocation = replacement;
        g_CounselorAliasOwner.store((uintptr_t)modeCDO);
        g_CounselorAliasOffset.store(aliasProperty->Offset_Internal);

        alias->Data = replacement;
        alias->Count = (int32_t)DedicatedAliasChars;
        alias->Max = (int32_t)DedicatedAliasChars;

        g_CounselorAliasArmed.store(true);
        g_CounselorAliasRestorePending.store(false);

        Logger::Success(
            "18L-AD DEDICATED ROUTE ARMED: ModeDef_OfflineBots alias is temporarily OFLBC for the Counselor row only");
        return true;
    }

    static void RestoreDedicatedCounselorAlias()
    {
        if (!g_CounselorAliasArmed.load())
            return;

        UObject* owner = reinterpret_cast<UObject*>(
            g_CounselorAliasOwner.load());
        UClass* modeClass = reinterpret_cast<UClass*>(
            g_CounselorOfflineBotsModeClass.load());
        int32_t offset = g_CounselorAliasOffset.load();
        wchar_t* allocation = g_CounselorAliasAllocation;

        if (!owner ||
            !modeClass ||
            offset <= 0 ||
            !allocation ||
            !Memory::IsReadable(owner, sizeof(UObject)) ||
            !Memory::IsReadable(modeClass, sizeof(UClass)) ||
            modeClass->DefaultObject != owner)
        {
            Logger::Error(
                "18L-AO OFLBC alias restore deferred: original mode CDO identity is no longer exact");
            return;
        }

        RawFString* alias =
            reinterpret_cast<RawFString*>(
                reinterpret_cast<uintptr_t>(owner) + offset);
        if (!Memory::IsReadable(alias, sizeof(RawFString)) ||
            alias->Data != allocation ||
            alias->Count != 6 ||
            alias->Max != 6)
        {
            Logger::Error(
                "18L-AO OFLBC alias restore deferred: live Alias no longer owns our exact replacement");
            return;
        }

        *alias = g_CounselorAliasOriginal;

        // Native travel may retain a raw pointer to the alias after the mode
        // CDO is restored. Keep this 12-byte buffer valid for process lifetime
        // rather than guessing when every asynchronous reader has finished.
        g_CounselorAliasAllocation = nullptr;
        g_CounselorAliasOwner.store(0);
        g_CounselorAliasOffset.store(-1);
        g_CounselorAliasArmed.store(false);
        g_CounselorAliasRestorePending.store(false);

        g_CounselorAliasOriginal = {};
        Logger::Success(
            "18L-AO OFLBC route: restored stock OfflineBots alias on game thread after counselor birth; retired replacement retained for process lifetime");
    }

    static bool CommitCounselorPickerRoute()
    {
        UObject* menu = reinterpret_cast<UObject*>(
            g_CounselorMenuObject.load());
        UClass* offlineBotsMode = reinterpret_cast<UClass*>(
            g_CounselorOfflineBotsModeClass.load());
        constexpr uintptr_t PendingOfflineModeOffset = 0x4D0;

        if (!menu ||
            !Memory::IsReadable(menu, sizeof(UObject)) ||
            !offlineBotsMode ||
            !Memory::IsReadable(offlineBotsMode, sizeof(UClass)))
        {
            return false;
        }

        UClass** pendingMode = reinterpret_cast<UClass**>(
            reinterpret_cast<uintptr_t>(menu) +
            PendingOfflineModeOffset);
        if (!Memory::IsReadable(pendingMode, sizeof(UClass*)))
            return false;

        *pendingMode = offlineBotsMode;
        g_CounselorEntryClickPending.store(false);
        g_CounselorMenuRouteLatched.store(true);
        ArmObjectivesWidgetGameStatePatch();
        Logger::Success(
            "PACKED COUNSELOR ROUTE COMMITTED: stock OFLB host selected; dedicated in-process lifecycle latch retained for counselor birth");
        return true;
    }

    static bool CaptureNativeGameSetupCounselorCount(UObject* settings)
    {
        if (!settings ||
            !Memory::IsReadable(settings, sizeof(UObject)) ||
            !settings->Class ||
            SafeName(reinterpret_cast<UObject*>(settings->Class)) !=
                "OfflineBotsSettingsMenuWidget_C")
        {
            return false;
        }

        constexpr uintptr_t CounselorComboOffset = 0x400;
        constexpr uintptr_t CurrentOptionIndexOffset = 0x458;
        UObject** comboAddress = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(settings) +
            CounselorComboOffset);
        if (!Memory::IsReadable(comboAddress, sizeof(UObject*)) ||
            !*comboAddress)
        {
            return false;
        }

        UObject* combo = *comboAddress;
        if (!Memory::IsReadable(combo, sizeof(UObject)) ||
            SafeName(combo) != "CounselorCountComboBox")
        {
            return false;
        }

        int32_t* optionIndex = reinterpret_cast<int32_t*>(
            reinterpret_cast<uintptr_t>(combo) +
            CurrentOptionIndexOffset);
        if (!Memory::IsReadable(optionIndex, sizeof(int32_t)) ||
            *optionIndex < 0 ||
            *optionIndex > 6)
        {
            return false;
        }

        const int32_t totalCounselors = *optionIndex + 1;
        g_NativeSelectedCounselorTotal.store(totalCounselors);
        Logger::Success(
            "NATIVE GAME SETUP COMMITTED: Counselors=" +
            std::to_string(totalCounselors) +
            " | human=1 | AI counselor bots=" +
            std::to_string(totalCounselors - 1));
        return true;
    }

    static void GameSetupStartExecHook(
        void* context,
        void* stack,
        void* result)
    {
        if (g_CounselorMenuRouteLatched.load())
        {
            // Force one last synchronous read before native Start begins world
            // travel, then freeze both picker selections for this match.
            g_NextProfileSelectionSyncAt.store(0);
            TickNativeProfileSelection();
            g_GameSetupSelectionLocked.store(true);

            UClass* committedJason = reinterpret_cast<UClass*>(
                g_TargetJasonClass.load());
            Logger::Success(
                "NATIVE GAME SETUP SELECTION LOCKED: Jason=" +
                (committedJason
                    ? SafeName(reinterpret_cast<UObject*>(committedJason))
                    : std::string("NULL")));

            if (!CaptureNativeGameSetupCounselorCount(
                    reinterpret_cast<UObject*>(context)))
            {
                Logger::Error(
                    "Native Game Setup Start: selected counselor count could not be captured");
            }
        }

        if (g_OriginalGameSetupStartExec)
            g_OriginalGameSetupStartExec(context, stack, result);
    }

    static bool InstallGameSetupStartHook(UClass* settingsClass)
    {
        if (!settingsClass ||
            !Memory::IsReadable(settingsClass, sizeof(UClass)))
        {
            return false;
        }

        UFunction* start = FindFunctionInHierarchyByName(
            settingsClass,
            "OnClicked_Start");
        if (!start ||
            !Memory::IsReadable(start, sizeof(UFunction)) ||
            !start->ExecFunction)
        {
            return false;
        }

        // Frontend travel can reconstruct/relink this Blueprint UFunction.
        // The process-wide flag is therefore not sufficient proof that the
        // currently loaded Game Setup screen still points at our wrapper.
        if (start->ExecFunction == &GameSetupStartExecHook)
        {
            g_GameSetupStartHookInstalled.store(true);
            return true;
        }

        const bool repairingRelinkedEvent =
            g_GameSetupStartHookInstalled.exchange(false);
        g_OriginalGameSetupStartExec = start->ExecFunction;
        DWORD oldProtect = 0;
        void* targetSlot = &start->ExecFunction;
        if (!VirtualProtect(
                targetSlot,
                sizeof(start->ExecFunction),
                PAGE_READWRITE,
                &oldProtect))
        {
            g_OriginalGameSetupStartExec = nullptr;
            return false;
        }

        start->ExecFunction = &GameSetupStartExecHook;
        DWORD ignoredProtect = 0;
        VirtualProtect(
            targetSlot,
            sizeof(start->ExecFunction),
            oldProtect,
            &ignoredProtect);
        g_GameSetupStartHookInstalled.store(true);
        Logger::Success(
            repairingRelinkedEvent
                ? "Native Game Setup Start hook relinked for replay: the newly selected Jason and Counselor count are authoritative"
                : "Native Game Setup Start hook installed: the in-game Counselors selection is authoritative");
        return true;
    }

    static void CounselorPickerAcceptExecHook(
        void* context,
        void* stack,
        void* result)
    {
        const uintptr_t trackedPicker = g_CounselorPickerWidget.load();
        const bool routePickerAccept =
            g_CounselorPickerActive.load() &&
            context &&
            (!trackedPicker ||
                trackedPicker == reinterpret_cast<uintptr_t>(context));
        if (routePickerAccept)
        {
            // Set before entering the original Blueprint. Its synchronous
            // branch calls RequestCounselorClass/profile save and then
            // PlayTransitionOut before this wrapper returns.
            g_CounselorPickerAccepted.store(true);
            Logger::Success(
                "18L-AK COUNSELOR PICKER ACCEPT: native selection/profile path is continuing into Offline Bots setup");
        }

        if (g_OriginalCounselorPickerAcceptExec)
        {
            g_OriginalCounselorPickerAcceptExec(
                context,
                stack,
                result);
        }

        // OnClick_Accept synchronously invokes RequestCounselorClass and the
        // profile-save path before it returns. Capture that native result now,
        // rather than waiting for the 500 ms frontend poll (the route has
        // already latched by then). This makes the character the player just
        // accepted authoritative for counselor birth.
        if (routePickerAccept &&
            g_CounselorMenuRouteLatched.load())
        {
            g_NextProfileSelectionSyncAt.store(0);
            g_CounselorMenuRouteLatched.store(false);
            TickNativeProfileSelection();
            g_CounselorMenuRouteLatched.store(true);
            ArmObjectivesWidgetGameStatePatch();
            Logger::Success(
                "18L-AK COUNSELOR PICKER CAPTURED: accepted native profile selection is locked for this match");
        }
    }

    static void CounselorPickerConstructExecHook(
        void* context,
        void* stack,
        void* result)
    {
        UObject* widget = reinterpret_cast<UObject*>(context);
        const bool routePickerConstruct =
            g_CounselorPickerActive.load() &&
            widget &&
            Memory::IsReadable(widget, sizeof(UObject)) &&
            widget->Class &&
            SafeName(reinterpret_cast<UObject*>(widget->Class)) ==
                "Counselor_Menu_C";

        bool castPatched = false;
        if (routePickerConstruct)
        {
            g_CounselorPickerWidget.store(
                reinterpret_cast<uintptr_t>(widget));
            castPatched =
                PatchCounselorPickerConstructPlayerStateCast(
                    widget->Class,
                    true);
            if (!castPatched)
            {
                Logger::Error(
                    "18L-AN counselor picker: route-scoped Construct cast patch failed");
            }
        }

        if (g_OriginalCounselorPickerConstructExec)
        {
            g_OriginalCounselorPickerConstructExec(
                context,
                stack,
                result);
        }

        if (castPatched)
            RestoreCounselorPickerPlayerStateCast();

        if (routePickerConstruct &&
            g_CounselorPickerActive.load())
        {
            Logger::Success(
                "18L-AN COUNSELOR PICKER CONSTRUCTED: menu-stack-owned native selector retained with offline player-state support");
        }
    }

    static void __fastcall CounselorPickerPlayTransitionOutHook(
        UObject* widget,
        UClass* destination)
    {
        if (g_CounselorMenuRouteLatched.load() &&
            destination &&
            Memory::IsReadable(destination, sizeof(UClass)) &&
            SafeName(reinterpret_cast<UObject*>(destination)) ==
                "OfflineBotsSettingsMenuWidget_C")
        {
            if (!InstallGameSetupStartHook(destination))
            {
                Logger::Error(
                    "Native Game Setup Start hook was unavailable during settings transition");
            }
        }

        const bool active = g_CounselorPickerActive.load();
        const uintptr_t trackedPicker = g_CounselorPickerWidget.load();
        const bool isCounselorPicker =
            active &&
            widget &&
            Memory::IsReadable(widget, sizeof(UObject)) &&
            widget->Class &&
            ((!trackedPicker &&
                SafeName(reinterpret_cast<UObject*>(widget->Class)) ==
                    "Counselor_Menu_C") ||
                trackedPicker == reinterpret_cast<uintptr_t>(widget));

        if (isCounselorPicker)
        {
            Logger::Debug(
                "18L-AM counselor picker transition observed | accepted=" +
                std::string(g_CounselorPickerAccepted.load() ? "true" : "false") +
                " | destination=" +
                (destination
                    ? SafeName(reinterpret_cast<UObject*>(destination))
                    : std::string("<null>")));

            if (g_CounselorPickerAccepted.load())
            {
                UClass* mapPicker =
                    FindClassExact("PickOfflineBotsMapMenuWidget_C");
                if (mapPicker && CommitCounselorPickerRoute())
                {
                    destination = mapPicker;
                    Logger::Success(
                        "18L-AK COUNSELOR PICKER COMPLETE: native Counselor_Menu -> map -> Jason -> game setup flow armed");
                }
                else
                {
                    Logger::Error(
                        "18L-AK counselor picker could not resolve the stock Offline Bots map picker/route state; preserving the native transition");
                }
            }
            else
            {
                // Back/cancel uses another event and therefore never sets the
                // accepted flag. Return through the ordinary menu stack.
                g_CounselorEntryClickPending.store(false);
                Logger::Debug(
                    "18L-AK counselor picker cancelled; returning without arming counselor gameplay");
            }

            g_CounselorPickerActive.store(false);
            g_CounselorPickerAccepted.store(false);
            g_CounselorPickerWidget.store(0);
            g_CounselorPickerSourceMenu.store(0);
        }

        if (g_OriginalPlayTransitionOut)
            g_OriginalPlayTransitionOut(widget, destination);
    }

    static bool InstallCounselorPickerHooks(UClass* counselorMenuClass)
    {
        if (!counselorMenuClass ||
            !Memory::IsReadable(counselorMenuClass, sizeof(UClass)))
        {
            return false;
        }

        if (!g_CounselorPickerConstructHookInstalled.load())
        {
            UFunction* construct = FindFunctionInHierarchyByName(
                counselorMenuClass,
                "Construct");
            if (!construct ||
                !Memory::IsReadable(construct, sizeof(UFunction)) ||
                !construct->ExecFunction)
            {
                Logger::Error(
                    "18L-AN counselor picker: Counselor_Menu.Construct is unavailable");
                return false;
            }

            g_OriginalCounselorPickerConstructExec =
                construct->ExecFunction;
            DWORD oldProtect = 0;
            void* targetSlot = &construct->ExecFunction;
            if (!VirtualProtect(
                    targetSlot,
                    sizeof(construct->ExecFunction),
                    PAGE_READWRITE,
                    &oldProtect))
            {
                g_OriginalCounselorPickerConstructExec = nullptr;
                return false;
            }
            construct->ExecFunction =
                &CounselorPickerConstructExecHook;
            DWORD ignoredProtect = 0;
            VirtualProtect(
                targetSlot,
                sizeof(construct->ExecFunction),
                oldProtect,
                &ignoredProtect);
            g_CounselorPickerConstructHookInstalled.store(true);
        }

        if (!g_CounselorPickerAcceptHookInstalled.load())
        {
            UFunction* accept = FindFunctionInHierarchyByName(
                counselorMenuClass,
                "OnClick_Accept");
            if (!accept ||
                !Memory::IsReadable(accept, sizeof(UFunction)) ||
                !accept->ExecFunction)
            {
                Logger::Error(
                    "18L-AK counselor picker: Counselor_Menu.OnClick_Accept is unavailable");
                return false;
            }

            g_OriginalCounselorPickerAcceptExec = accept->ExecFunction;
            DWORD oldProtect = 0;
            void* targetSlot = &accept->ExecFunction;
            if (!VirtualProtect(
                    targetSlot,
                    sizeof(accept->ExecFunction),
                    PAGE_READWRITE,
                    &oldProtect))
            {
                g_OriginalCounselorPickerAcceptExec = nullptr;
                return false;
            }
            accept->ExecFunction = &CounselorPickerAcceptExecHook;
            DWORD ignoredProtect = 0;
            VirtualProtect(
                targetSlot,
                sizeof(accept->ExecFunction),
                oldProtect,
                &ignoredProtect);
            g_CounselorPickerAcceptHookInstalled.store(true);
        }

        if (!g_CounselorPickerTransitionHookInstalled.load())
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(
                GetModuleHandleW(nullptr));
            LPVOID target = reinterpret_cast<LPVOID>(
                base + RVA_ILLUserWidgetPlayTransitionOut);
            if (!base || !Memory::IsReadable(target, 16))
                return false;

            const MH_STATUS initStatus = MH_Initialize();
            if (initStatus != MH_OK &&
                initStatus != MH_ERROR_ALREADY_INITIALIZED)
            {
                return false;
            }

            const MH_STATUS createStatus = MH_CreateHook(
                target,
                reinterpret_cast<LPVOID>(
                    &CounselorPickerPlayTransitionOutHook),
                reinterpret_cast<LPVOID*>(
                    &g_OriginalPlayTransitionOut));
            if (createStatus != MH_OK)
                return false;
            if (MH_EnableHook(target) != MH_OK)
                return false;

            g_CounselorPickerTransitionHookInstalled.store(true);
        }

        return true;
    }

    struct NativeScriptArray
    {
        uint8_t* Data;
        int32_t Count;
        int32_t Max;
    };

    static bool PatchCounselorPickerConstructPlayerStateCast(
        UClass* counselorMenuClass,
        bool applyPatch)
    {
        if (applyPatch &&
            g_CounselorPickerOfflineCastPatched.load())
            return true;

        UClass* lobbyPlayerState = FindClassExact("SCPlayerState_Lobby");
        UClass* basePlayerState = FindClassExact("SCPlayerState");
        UFunction* ubergraph = FindFunctionInHierarchyByName(
            counselorMenuClass,
            "ExecuteUbergraph_Counselor_Menu");

        if (!lobbyPlayerState ||
            !basePlayerState ||
            !ubergraph ||
            !Memory::IsReadable(ubergraph, sizeof(UFunction)))
        {
            Logger::Error(
                "18L-AM counselor picker: Ubergraph/player-state classes are unavailable");
            return false;
        }

        // UE4 Shipping UStruct stores its runtime-expanded Script TArray at
        // +0x48 in this 0x30-byte UField layout. Object operands in that
        // buffer are native UObject pointers.
        // Widen only Counselor_Menu's one DynamicCast operand from
        // SCPlayerState_Lobby to its SCPlayerState base inside the class
        // Ubergraph (Construct itself is only a small entry-point wrapper).
        // This is the in-memory equivalent of the proven one-import asset
        // change, without reserializing the fragile cooked widget package.
        constexpr uintptr_t ScriptArrayOffset = 0x48;
        auto* script = reinterpret_cast<NativeScriptArray*>(
            reinterpret_cast<uintptr_t>(ubergraph) + ScriptArrayOffset);
        if (!Memory::IsReadable(script, sizeof(NativeScriptArray)) ||
            !script->Data ||
            script->Count <= 0 ||
            script->Count > 0x10000 ||
            script->Max < script->Count ||
            !Memory::IsReadable(script->Data, script->Count))
        {
            Logger::Error(
                "18L-AM counselor picker: Ubergraph runtime Script buffer is invalid");
            return false;
        }

        uint8_t* operand = nullptr;
        uint32_t matches = 0;
        const uintptr_t lobbyPointer =
            reinterpret_cast<uintptr_t>(lobbyPlayerState);
        constexpr uint8_t ExDynamicCast = 0x16;
        for (int32_t i = 0;
             i <= script->Count -
                1 - static_cast<int32_t>(sizeof(uintptr_t));
             ++i)
        {
            if (script->Data[i] != ExDynamicCast)
                continue;

            uintptr_t candidate = 0;
            std::memcpy(
                &candidate,
                script->Data + i + 1,
                sizeof(candidate));
            if (candidate == lobbyPointer)
            {
                operand = script->Data + i + 1;
                ++matches;
            }
        }

        if (matches != 1 || !operand)
        {
            Logger::Error(
                "18L-AM counselor picker: expected one SCPlayerState_Lobby operand in Counselor_Menu Ubergraph; found " +
                std::to_string(matches) +
                " | ScriptBytes=" + std::to_string(script->Count));
            return false;
        }

        if (!applyPatch)
        {
            Logger::Success(
                "18L-AM COUNSELOR PICKER BYTECODE VERIFIED: one 0x16 SCPlayerState_Lobby DynamicCast is ready for route-scoped widening");
            return true;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(
                operand,
                sizeof(uintptr_t),
                PAGE_READWRITE,
                &oldProtect))
        {
            Logger::Error(
                "18L-AM counselor picker: could not make the Ubergraph cast operand writable");
            return false;
        }

        const uintptr_t basePointer =
            reinterpret_cast<uintptr_t>(basePlayerState);
        g_CounselorPickerCastOperand.store(
            reinterpret_cast<uintptr_t>(operand));
        g_CounselorPickerCastOriginal.store(lobbyPointer);
        std::memcpy(operand, &basePointer, sizeof(basePointer));
        FlushInstructionCache(
            GetCurrentProcess(),
            operand,
            sizeof(basePointer));
        DWORD ignoredProtect = 0;
        VirtualProtect(
            operand,
            sizeof(uintptr_t),
            oldProtect,
            &ignoredProtect);

        g_CounselorPickerOfflineCastPatched.store(true);
        Logger::Success(
            "18L-AM COUNSELOR PICKER OFFLINE CAST READY: Counselor_Menu Ubergraph now accepts the frontend SCPlayerState at runtime");
        return true;
    }

    static void RestoreCounselorPickerPlayerStateCast()
    {
        if (!g_CounselorPickerOfflineCastPatched.exchange(false))
            return;

        uint8_t* operand = reinterpret_cast<uint8_t*>(
            g_CounselorPickerCastOperand.exchange(0));
        const uintptr_t original =
            g_CounselorPickerCastOriginal.exchange(0);
        if (!operand ||
            !original ||
            !Memory::IsReadable(operand, sizeof(original)))
        {
            Logger::Error(
                "18L-AM counselor picker: could not restore the route-scoped cast operand");
            return;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(
                operand,
                sizeof(original),
                PAGE_READWRITE,
                &oldProtect))
        {
            Logger::Error(
                "18L-AM counselor picker: cast-operand restore protection failed");
            return;
        }

        std::memcpy(operand, &original, sizeof(original));
        FlushInstructionCache(
            GetCurrentProcess(),
            operand,
            sizeof(original));
        DWORD ignoredProtect = 0;
        VirtualProtect(
            operand,
            sizeof(original),
            oldProtect,
            &ignoredProtect);
        Logger::Debug(
            "18L-AM counselor picker: stock SCPlayerState_Lobby cast restored after offline widget construction");
    }

    static bool PatchObjectivesWidgetGameStateCast(
        const char* requestedClassName)
    {
        if (g_ObjectivesGameStateCastPatched.load())
            return true;

        UClass* huntGameState = FindClassExact("SCGameState_Hunt");
        UClass* baseGameState = FindClassExact("SCGameState");
        if (!huntGameState || !baseGameState)
            return false;

        const uintptr_t huntPointer =
            reinterpret_cast<uintptr_t>(huntGameState);
        const uintptr_t basePointer =
            reinterpret_cast<uintptr_t>(baseGameState);
        constexpr uintptr_t ScriptArrayOffset = 0x48;
        bool requestedClassMatched = requestedClassName == nullptr;
        bool patchedAny = false;

        for (uint32_t specIndex = 0; specIndex < 8; ++specIndex)
        {
            const ObjectiveWidgetPatchSpec& spec =
                g_ObjectiveWidgetPatchSpecs[specIndex];
            if (requestedClassName &&
                std::strcmp(requestedClassName, spec.ClassName) != 0)
            {
                continue;
            }
            requestedClassMatched = true;

            const uint32_t bit = 1u << specIndex;
            if ((g_ObjectivesWidgetPatchMask.load() & bit) != 0)
            {
                patchedAny = true;
                continue;
            }

            UClass* widgetClass = FindClassExact(spec.ClassName);
            if (!widgetClass ||
                !Memory::IsReadable(widgetClass, sizeof(UClass)))
            {
                continue;
            }

            struct ObjectiveClassOperand
            {
                uint8_t* Address = nullptr;
                uintptr_t Original = 0;
            };

            std::vector<ObjectiveClassOperand> huntOperands;
            uint32_t baseMatches = 0;
            uint32_t functionsScanned = 0;
            std::vector<std::string> referenceFunctions;
            constexpr uint8_t ExDynamicCast = 0x16;
            for (UField* field = widgetClass->Children;
                 field && Memory::IsReadable(field, sizeof(UField));
                 field = field->Next)
            {
                // The cast sites used by the pause objectives are in helper
                // UFunctions directly owned by each generated widget class
                // (FindPhone, FindCBRadio, IsStarted, and similar), not only
                // in ExecuteUbergraph.  Restrict enumeration to exact,
                // directly-owned UFunctions so no unrelated scripts change.
                if (field->OuterPrivate !=
                        reinterpret_cast<UField*>(widgetClass) ||
                    !field->ClassPrivate ||
                    !Memory::IsReadable(field->ClassPrivate, sizeof(UObject)) ||
                    SafeName(reinterpret_cast<UObject*>(field->ClassPrivate)) !=
                        "Function")
                {
                    continue;
                }

                UFunction* function = reinterpret_cast<UFunction*>(field);
                auto* script = reinterpret_cast<NativeScriptArray*>(
                    reinterpret_cast<uintptr_t>(function) + ScriptArrayOffset);
                if (!Memory::IsReadable(script, sizeof(NativeScriptArray)) ||
                    !script->Data || script->Count <= 0 ||
                    script->Count > 0x40000 || script->Max < script->Count ||
                    !Memory::IsReadable(script->Data, script->Count))
                {
                    continue;
                }

                ++functionsScanned;
                bool functionHasReference = false;
                for (int32_t i = 0;
                     i <= script->Count - 1 -
                        static_cast<int32_t>(sizeof(uintptr_t));
                     ++i)
                {
                    if (script->Data[i] != ExDynamicCast)
                        continue;

                    uintptr_t candidate = 0;
                    std::memcpy(
                        &candidate,
                        script->Data + i + 1,
                        sizeof(candidate));
                    if (candidate == huntPointer)
                    {
                        huntOperands.push_back(
                            { script->Data + i + 1, candidate });
                        functionHasReference = true;
                    }
                    else if (candidate == basePointer)
                    {
                        ++baseMatches;
                        functionHasReference = true;
                    }
                }

                if (functionHasReference)
                    referenceFunctions.push_back(SafeName(
                        reinterpret_cast<UObject*>(function)));
            }

            // The loose development build already contains the same import
            // widening. Count that widget as complete without writing it.
            if (huntOperands.empty() && baseMatches > 0)
            {
                g_ObjectivesWidgetPatchMask.fetch_or(bit);
                patchedAny = true;
                Logger::Success(
                    std::string("PACKED OBJECTIVES READY: ") +
                    spec.ClassName +
                    " helper functions already accept SCGameState modes | functions=" +
                    std::to_string(functionsScanned) +
                    " | references=" + std::to_string(baseMatches));
                continue;
            }

            if (huntOperands.empty())
            {
                if ((g_ObjectivesWidgetPatchFailureLoggedMask.fetch_or(bit) & bit) == 0)
                {
                    Logger::Error(
                        std::string("PACKED OBJECTIVES: SCGameState_Hunt runtime reference not found | class=") +
                        spec.ClassName +
                        " | helperFunctionsScanned=" +
                        std::to_string(functionsScanned));
                }
                continue;
            }

            std::vector<ObjectiveClassOperand> patchedOperands;
            bool writeFailed = false;
            for (const ObjectiveClassOperand& operand : huntOperands)
            {
                DWORD oldProtect = 0;
                if (!VirtualProtect(
                        operand.Address,
                        sizeof(basePointer),
                        PAGE_READWRITE,
                        &oldProtect))
                {
                    writeFailed = true;
                    break;
                }

                std::memcpy(
                    operand.Address,
                    &basePointer,
                    sizeof(basePointer));
                FlushInstructionCache(
                    GetCurrentProcess(),
                    operand.Address,
                    sizeof(basePointer));
                DWORD ignoredProtect = 0;
                VirtualProtect(
                    operand.Address,
                    sizeof(basePointer),
                    oldProtect,
                    &ignoredProtect);
                patchedOperands.push_back(operand);
            }

            if (writeFailed)
            {
                for (const ObjectiveClassOperand& patched : patchedOperands)
                {
                    DWORD rollbackProtect = 0;
                    if (VirtualProtect(
                            patched.Address,
                            sizeof(patched.Original),
                            PAGE_READWRITE,
                            &rollbackProtect))
                    {
                        std::memcpy(
                            patched.Address,
                            &patched.Original,
                            sizeof(patched.Original));
                        DWORD ignoredProtect = 0;
                        VirtualProtect(
                            patched.Address,
                            sizeof(patched.Original),
                            rollbackProtect,
                            &ignoredProtect);
                    }
                }
                continue;
            }

            g_ObjectivesWidgetPatchMask.fetch_or(bit);
            patchedAny = true;
            Logger::Success(
                std::string("PACKED OBJECTIVES READY: ") +
                spec.ClassName +
                " helper SCGameState_Hunt references widened to SCGameState | functions=" +
                std::to_string(functionsScanned) +
                " | referenceFunctions=" +
                std::to_string(referenceFunctions.size()) +
                " | references=" + std::to_string(patchedOperands.size()));
        }

        if (!requestedClassMatched)
            return false;

        if ((g_ObjectivesWidgetPatchMask.load() &
             g_AllObjectiveWidgetPatchBits) ==
            g_AllObjectiveWidgetPatchBits)
        {
            g_ObjectivesGameStateCastPatched.store(true);
            Logger::Success(
                "PACKED OBJECTIVES COMPLETE: all eight proven loose-build widget imports are active in memory");
        }
        return patchedAny;
    }

    static void RestoreCounselorEntryFocus(UObject* sourceMenu)
    {
        if (!sourceMenu ||
            !Memory::IsReadable(sourceMenu, sizeof(UObject)) ||
            !sourceMenu->Class)
        {
            return;
        }

        UFunction* setKeyboardFocus = FindFunctionInHierarchyByName(
            sourceMenu->Class,
            "SetKeyboardFocus");
        if (setKeyboardFocus &&
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(sourceMenu),
                sourceMenu,
                setKeyboardFocus,
                nullptr))
        {
            Logger::Debug(
                "18L-AM counselor picker: restored keyboard/controller focus to Offline Play");
        }
    }

    static bool OpenIntegratedCounselorPicker(UObject* sourceMenu)
    {
        if (!sourceMenu ||
            !Memory::IsReadable(sourceMenu, sizeof(UObject)) ||
            !sourceMenu->Class)
        {
            return false;
        }

        UClass* counselorMenuClass = FindClassExact("Counselor_Menu_C");
        if (!counselorMenuClass ||
            !InstallCounselorPickerHooks(counselorMenuClass))
        {
            Logger::Error(
                "18L-AK counselor picker: native Counselor_Menu_C is not loaded/usable");
            return false;
        }

        if (!g_OriginalPlayTransitionOut ||
            !PatchCounselorPickerConstructPlayerStateCast(
                counselorMenuClass,
                false))
        {
            Logger::Error(
                "18L-AN counselor picker: transition/cast surface is unavailable");
            return false;
        }

        g_CounselorPickerAccepted.store(false);
        g_CounselorPickerWidget.store(0);
        g_CounselorPickerSourceMenu.store(
            reinterpret_cast<uintptr_t>(sourceMenu));
        g_CounselorPickerActive.store(true);
        g_OriginalPlayTransitionOut(
            sourceMenu,
            counselorMenuClass);

        Logger::Success(
            "18L-AN COUNSELOR PICKER TRANSITION REQUESTED: current Offline Play menu is transitioning to the native selector through the authoritative menu stack");
        return true;
    }

    struct PersistentRuntimeText
    {
        uint8_t* Value = nullptr;
        int32_t Size = 0;
    };

    static bool CreatePersistentRuntimeText(
        const wchar_t* text,
        PersistentRuntimeText* output)
    {
        if (!text || !output)
        {
            g_RuntimeMenuLabelFailureCode.store(101);
            return false;
        }

        UObject* textLibrary =
            FindObjectExact("Default__KismetTextLibrary");
        if (!textLibrary || !textLibrary->Class)
        {
            g_RuntimeMenuLabelFailureCode.store(102);
            return false;
        }

        UFunction* convert = FindFunctionInHierarchyByName(
            textLibrary->Class,
            "Conv_StringToText");
        auto* inputProperty = FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(convert),
            "InString");
        if (!inputProperty)
        {
            inputProperty = FindFunctionPropertyByType(
                convert,
                "StrProperty",
                false);
        }
        auto* returnProperty = FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(convert),
            "ReturnValue");
        if (!returnProperty)
        {
            returnProperty = FindFunctionPropertyByType(
                convert,
                "TextProperty",
                true);
        }

        if (!convert || !inputProperty || !returnProperty ||
            inputProperty->Offset_Internal < 0 ||
            returnProperty->Offset_Internal < 0 ||
            inputProperty->ElementSize < (int32_t)sizeof(RawFString) ||
            returnProperty->ElementSize < 16 ||
            returnProperty->ElementSize > 64)
        {
            g_RuntimeMenuLabelFailureCode.store(
                !convert ? 103 :
                (!inputProperty ? 104 :
                (!returnProperty ? 105 : 106)));
            return false;
        }

        size_t parameterBytes = static_cast<size_t>(convert->Size);
        const size_t requiredBytes = std::max(
            static_cast<size_t>(inputProperty->Offset_Internal) +
                sizeof(RawFString),
            static_cast<size_t>(returnProperty->Offset_Internal) +
                static_cast<size_t>(returnProperty->ElementSize));
        parameterBytes = std::max(parameterBytes, requiredBytes);
        if (parameterBytes == 0 || parameterBytes > 0x400)
        {
            g_RuntimeMenuLabelFailureCode.store(107);
            return false;
        }

        // FText contains shared state. Keep this tiny one-time parameter block
        // alive for the process lifetime so the copied menu label can never
        // reference a temporary conversion result.
        auto* params = reinterpret_cast<uint8_t*>(HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            parameterBytes));
        if (!params)
        {
            g_RuntimeMenuLabelFailureCode.store(108);
            return false;
        }

        RawFString input{};
        input.Data = const_cast<wchar_t*>(text);
        input.Count = static_cast<int32_t>(wcslen(text)) + 1;
        input.Max = input.Count;
        memcpy(
            params + inputProperty->Offset_Internal,
            &input,
            sizeof(input));

        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(textLibrary),
                textLibrary,
                convert,
                params))
        {
            HeapFree(GetProcessHeap(), 0, params);
            g_RuntimeMenuLabelFailureCode.store(109);
            return false;
        }

        output->Value = params + returnProperty->Offset_Internal;
        output->Size = returnProperty->ElementSize;
        g_RuntimeMenuLabelFailureCode.store(0);
        return true;
    }

    static bool SetWideMenuButtonText(
        UObject* button,
        const wchar_t* text,
        PersistentRuntimeText* cachedText)
    {
        if (!button || !button->Class || !text || !cachedText)
        {
            g_RuntimeMenuLabelFailureCode.store(201);
            return false;
        }

        auto* textProperty = FindPropertyInHierarchyByName(
            button->Class,
            "Text");
        if (!textProperty ||
            textProperty->Offset_Internal <= 0 ||
            textProperty->Offset_Internal >= 0x10000)
        {
            g_RuntimeMenuLabelFailureCode.store(202);
            return false;
        }

        if (!cachedText->Value &&
            !CreatePersistentRuntimeText(text, cachedText))
        {
            if (!g_RuntimeMenuLabelFailureCode.load())
                g_RuntimeMenuLabelFailureCode.store(203);
            return false;
        }

        if (!cachedText->Value || cachedText->Size <= 0 ||
            textProperty->ElementSize != cachedText->Size)
        {
            g_RuntimeMenuLabelFailureCode.store(204);
            return false;
        }

        auto* titleTextProperty = FindPropertyInHierarchyByName(
            button->Class,
            "TitleText");
        if (!titleTextProperty ||
            titleTextProperty->Offset_Internal <= 0 ||
            titleTextProperty->Offset_Internal >= 0x10000)
        {
            g_RuntimeMenuLabelFailureCode.store(206);
            return false;
        }

        UObject** titleTextSlot = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(button) +
            titleTextProperty->Offset_Internal);
        if (!Memory::IsReadable(titleTextSlot, sizeof(UObject*)) ||
            !*titleTextSlot || !(*titleTextSlot)->Class)
        {
            g_RuntimeMenuLabelFailureCode.store(207);
            return false;
        }

        UObject* titleText = *titleTextSlot;
        UFunction* setText = FindFunctionInHierarchyByName(
            titleText->Class,
            "SetText");
        auto* inputTextProperty = FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(setText),
            "InText");
        if (!inputTextProperty)
        {
            inputTextProperty = FindFunctionPropertyByType(
                setText,
                "TextProperty",
                false);
        }
        if (!setText || !inputTextProperty ||
            inputTextProperty->Offset_Internal < 0 ||
            inputTextProperty->ElementSize != cachedText->Size)
        {
            g_RuntimeMenuLabelFailureCode.store(
                !setText ? 208 :
                (!inputTextProperty ? 209 : 210));
            return false;
        }

        size_t parameterBytes = std::max(
            static_cast<size_t>(setText->Size),
            static_cast<size_t>(inputTextProperty->Offset_Internal) +
                static_cast<size_t>(inputTextProperty->ElementSize));
        if (parameterBytes == 0 || parameterBytes > 0x200)
        {
            g_RuntimeMenuLabelFailureCode.store(211);
            return false;
        }

        std::vector<uint8_t> params(parameterBytes, 0);
        memcpy(
            params.data() + inputTextProperty->Offset_Internal,
            cachedText->Value,
            cachedText->Size);
        const bool set = SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(titleText),
            titleText,
            setText,
            params.data());
        g_RuntimeMenuLabelFailureCode.store(set ? 0 : 212);
        return set;
    }

    static bool ReorderRuntimeOfflinePlayMenu(UObject* menuWidget)
    {
        if (!menuWidget || !menuWidget->Class)
        {
            g_RuntimeMenuOrderFailureCode.store(301);
            return false;
        }

        if (g_RuntimeMenuOrderedWidget.load() ==
            reinterpret_cast<uintptr_t>(menuWidget))
        {
            return true;
        }

        const auto readObjectProperty = [menuWidget](
            const char* propertyName) -> UObject*
        {
            auto* property = FindPropertyInHierarchyByName(
                menuWidget->Class,
                propertyName);
            if (!property || property->Offset_Internal <= 0 ||
                property->Offset_Internal >= 0x10000)
            {
                return nullptr;
            }

            UObject** slot = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(menuWidget) +
                property->Offset_Internal);
            return Memory::IsReadable(slot, sizeof(UObject*))
                ? *slot
                : nullptr;
        };

        UObject* verticalBox = readObjectProperty("VerticalBox_95");
        UObject* jason = readObjectProperty("OfflineBotsButton");
        UObject* counselor = readObjectProperty("VC3Button");
        UObject* challenges = readObjectProperty("ChallengesButton");
        UObject* virtualCabin = readObjectProperty("VirtualCabinButton");
        UObject* sandbox = readObjectProperty("SandboxButton");

        // VerticalBox_95 is not marked IsVariable in the stock generated
        // class, so it has no direct UObject property on the menu instance.
        // Resolve the same live panel through the counselor widget's native
        // UWidget::Slot -> UPanelSlot::Parent relationship instead.
        if (!verticalBox && counselor && counselor->Class)
        {
            auto* slotProperty = FindPropertyInHierarchyByName(
                counselor->Class,
                "Slot");
            UObject* panelSlot = nullptr;
            if (slotProperty && slotProperty->Offset_Internal > 0 &&
                slotProperty->Offset_Internal < 0x10000)
            {
                UObject** slotAddress = reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(counselor) +
                    slotProperty->Offset_Internal);
                if (Memory::IsReadable(slotAddress, sizeof(UObject*)))
                    panelSlot = *slotAddress;
            }

            if (panelSlot && panelSlot->Class)
            {
                auto* parentProperty = FindPropertyInHierarchyByName(
                    panelSlot->Class,
                    "Parent");
                if (parentProperty && parentProperty->Offset_Internal > 0 &&
                    parentProperty->Offset_Internal < 0x10000)
                {
                    UObject** parentAddress = reinterpret_cast<UObject**>(
                        reinterpret_cast<uintptr_t>(panelSlot) +
                        parentProperty->Offset_Internal);
                    if (Memory::IsReadable(parentAddress, sizeof(UObject*)))
                        verticalBox = *parentAddress;
                }

                if (!verticalBox &&
                    Memory::IsReadable(panelSlot->OuterPrivate, sizeof(UObject)))
                {
                    verticalBox = panelSlot->OuterPrivate;
                }
            }
        }

        if (!verticalBox || !verticalBox->Class || !jason || !counselor ||
            !challenges || !virtualCabin || !sandbox)
        {
            g_RuntimeMenuOrderFailureCode.store(302);
            return false;
        }

        UFunction* clearChildren = FindFunctionInHierarchyByName(
            verticalBox->Class,
            "ClearChildren");
        UFunction* addChild = FindFunctionInHierarchyByName(
            verticalBox->Class,
            "AddChildToVerticalBox");
        UFunction* getChildAt = FindFunctionInHierarchyByName(
            verticalBox->Class,
            "GetChildAt");
        auto* contentProperty = FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(addChild),
            "Content");
        if (!contentProperty)
        {
            contentProperty = FindFunctionPropertyByType(
                addChild,
                "ObjectProperty",
                false);
        }

        auto* addReturnProperty = FindFunctionPropertyByType(
            addChild,
            "ObjectProperty",
            true);
        auto* childIndexProperty = FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(getChildAt),
            "Index");
        if (!childIndexProperty)
        {
            childIndexProperty = FindFunctionPropertyByType(
                getChildAt,
                "IntProperty",
                false);
        }
        auto* childReturnProperty = FindFunctionPropertyByType(
            getChildAt,
            "ObjectProperty",
            true);

        if (!clearChildren || !addChild || !getChildAt ||
            !contentProperty || !addReturnProperty ||
            !childIndexProperty || !childReturnProperty ||
            contentProperty->Offset_Internal < 0 ||
            contentProperty->ElementSize != sizeof(UObject*) ||
            addReturnProperty->Offset_Internal < 0 ||
            addReturnProperty->ElementSize != sizeof(UObject*) ||
            childIndexProperty->Offset_Internal < 0 ||
            childIndexProperty->ElementSize != sizeof(int32_t) ||
            childReturnProperty->Offset_Internal < 0 ||
            childReturnProperty->ElementSize != sizeof(UObject*))
        {
            g_RuntimeMenuOrderFailureCode.store(303);
            return false;
        }

        size_t addParameterBytes = std::max(
            static_cast<size_t>(addChild->Size),
            static_cast<size_t>(contentProperty->Offset_Internal) +
                sizeof(UObject*));
        addParameterBytes = std::max(
            addParameterBytes,
            static_cast<size_t>(addReturnProperty->Offset_Internal) +
                sizeof(UObject*));
        size_t getParameterBytes = std::max(
            static_cast<size_t>(getChildAt->Size),
            static_cast<size_t>(childIndexProperty->Offset_Internal) +
                sizeof(int32_t));
        getParameterBytes = std::max(
            getParameterBytes,
            static_cast<size_t>(childReturnProperty->Offset_Internal) +
                sizeof(UObject*));
        if (addParameterBytes == 0 || addParameterBytes > 0x200 ||
            getParameterBytes == 0 || getParameterBytes > 0x200)
        {
            g_RuntimeMenuOrderFailureCode.store(304);
            return false;
        }

        const auto addOne = [verticalBox, addChild, contentProperty,
            addReturnProperty, addParameterBytes](UObject* child) -> bool
        {
            std::vector<uint8_t> params(addParameterBytes, 0);
            memcpy(
                params.data() + contentProperty->Offset_Internal,
                &child,
                sizeof(child));
            if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(verticalBox),
                verticalBox,
                addChild,
                params.data()))
            {
                return false;
            }

            UObject* returnedSlot = nullptr;
            memcpy(
                &returnedSlot,
                params.data() + addReturnProperty->Offset_Internal,
                sizeof(returnedSlot));
            return returnedSlot &&
                Memory::IsReadable(returnedSlot, sizeof(UObject));
        };

        const auto getChild = [verticalBox, getChildAt,
            childIndexProperty, childReturnProperty,
            getParameterBytes](int32_t index) -> UObject*
        {
            std::vector<uint8_t> params(getParameterBytes, 0);
            memcpy(
                params.data() + childIndexProperty->Offset_Internal,
                &index,
                sizeof(index));
            if (!SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(verticalBox),
                    verticalBox,
                    getChildAt,
                    params.data()))
            {
                return nullptr;
            }

            UObject* child = nullptr;
            memcpy(
                &child,
                params.data() + childReturnProperty->Offset_Internal,
                sizeof(child));
            return child;
        };

        // The verified stock slots contain only Parent and Content, so native
        // clear/re-add preserves their default layout while safely rebuilding
        // the live Slate order. No raw Slate pointers or asset bytes change.
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(verticalBox),
                verticalBox,
                clearChildren,
                nullptr))
        {
            g_RuntimeMenuOrderFailureCode.store(305);
            return false;
        }

        const std::array<UObject*, 5> desired = {
            jason, counselor, challenges, virtualCabin, sandbox
        };
        bool complete = true;
        for (UObject* child : desired)
        {
            if (!addOne(child))
            {
                complete = false;
                break;
            }
        }

        if (!complete)
        {
            // Restore the original stock order if an unexpected native add
            // fails, rather than leaving a partially populated menu.
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(verticalBox),
                verticalBox,
                clearChildren,
                nullptr);
            const std::array<UObject*, 5> original = {
                jason, challenges, virtualCabin, counselor, sandbox
            };
            for (UObject* child : original)
                addOne(child);
            g_RuntimeMenuOrderFailureCode.store(306);
            return false;
        }

        bool verified = true;
        for (int32_t index = 0;
            index < static_cast<int32_t>(desired.size());
            ++index)
        {
            if (getChild(index) != desired[static_cast<size_t>(index)])
            {
                verified = false;
                break;
            }
        }
        if (!verified)
        {
            g_RuntimeMenuOrderFailureCode.store(307);
            return false;
        }

        // UMG can retain cached Slate geometry after children are rebuilt at
        // runtime. Invalidate both the panel and owning menu, then force a
        // prepass so the verified logical order is also the rendered order.
        UFunction* invalidatePanel = FindFunctionInHierarchyByName(
            verticalBox->Class,
            "InvalidateLayoutAndVolatility");
        UFunction* invalidateMenu = FindFunctionInHierarchyByName(
            menuWidget->Class,
            "InvalidateLayoutAndVolatility");
        UFunction* forcePrepass = FindFunctionInHierarchyByName(
            menuWidget->Class,
            "ForceLayoutPrepass");
        if (invalidatePanel)
        {
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(verticalBox),
                verticalBox,
                invalidatePanel,
                nullptr);
        }
        if (invalidateMenu)
        {
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(menuWidget),
                menuWidget,
                invalidateMenu,
                nullptr);
        }
        if (forcePrepass)
        {
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(menuWidget),
                menuWidget,
                forcePrepass,
                nullptr);
        }

        g_RuntimeMenuOrderedWidget.store(
            reinterpret_cast<uintptr_t>(menuWidget));
        g_RuntimeMenuOrderFailureLogged.store(false);
        g_RuntimeMenuOrderFailureCode.store(0);
        Logger::Success(
            "PACKED MENU ORDER VERIFIED: live child order is Jason, Counselor, Challenges, Virtual Cabin, Sandbox");
        return true;
    }

    static bool LabelRuntimeOfflinePlayMenu(UObject* menuWidget)
    {
        if (!menuWidget || !menuWidget->Class ||
            !Memory::IsReadable(menuWidget, sizeof(UObject)))
        {
            return false;
        }

        if (g_RuntimeMenuLabeledWidget.load() ==
            reinterpret_cast<uintptr_t>(menuWidget))
        {
            if (g_RuntimeMenuOrderedWidget.load() !=
                reinterpret_cast<uintptr_t>(menuWidget))
            {
                const ULONGLONG now = GetTickCount64();
                const ULONGLONG lastAttempt =
                    g_RuntimeMenuLabelLastAttempt.load();
                if ((!lastAttempt || now - lastAttempt >= 500) &&
                    !g_RuntimeMenuOrderRequested.exchange(true))
                {
                    g_RuntimeMenuLabelLastAttempt.store(now);
                    g_RuntimeMenuOrderWidget.store(
                        reinterpret_cast<uintptr_t>(menuWidget));
                }
            }
            return true;
        }

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG lastAttempt =
            g_RuntimeMenuLabelLastAttempt.load();
        if (lastAttempt && now - lastAttempt < 500)
            return false;
        g_RuntimeMenuLabelLastAttempt.store(now);

        auto* counselorButtonProperty = FindPropertyInHierarchyByName(
            menuWidget->Class,
            "VC3Button");
        auto* jasonButtonProperty = FindPropertyInHierarchyByName(
            menuWidget->Class,
            "OfflineBotsButton");
        if (!counselorButtonProperty || !jasonButtonProperty)
            return false;

        const auto readButton = [menuWidget](
            UPropertyLite* property) -> UObject*
        {
            if (property->Offset_Internal <= 0 ||
                property->Offset_Internal >= 0x10000)
            {
                return nullptr;
            }

            UObject** slot = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(menuWidget) +
                property->Offset_Internal);
            return Memory::IsReadable(slot, sizeof(UObject*))
                ? *slot
                : nullptr;
        };

        UObject* counselorButton = readButton(counselorButtonProperty);
        UObject* jasonButton = readButton(jasonButtonProperty);
        if (!counselorButton || !jasonButton)
            return false;

        // FText owns reference-counted history. A prior menu instance releases
        // its text when the frontend world is replaced, so never reuse that
        // instance's raw FText bytes for a replay menu. Each successful label
        // gets its own intentionally process-lifetime conversion buffer, and
        // the native TextBlock::SetText call performs the owned copy.
        PersistentRuntimeText counselorText{};
        PersistentRuntimeText jasonText{};
        g_RuntimeMenuLabelFailureCode.store(0);
        const bool counselorLabeled = SetWideMenuButtonText(
            counselorButton,
            L"Offline Bots - Counselor",
            &counselorText);
        const int32_t counselorFailure =
            g_RuntimeMenuLabelFailureCode.load();
        g_RuntimeMenuLabelFailureCode.store(0);
        const bool jasonLabeled = SetWideMenuButtonText(
            jasonButton,
            L"Offline Bots - Jason",
            &jasonText);
        const int32_t jasonFailure =
            g_RuntimeMenuLabelFailureCode.load();

        if (!counselorLabeled || !jasonLabeled)
        {
            if (!g_RuntimeMenuLabelFailureLogged.exchange(true))
            {
                Logger::Error(
                    "Packed menu label diagnostic | CounselorCode=" +
                    std::to_string(counselorFailure) +
                    " | JasonCode=" +
                    std::to_string(jasonFailure));
            }
            return false;
        }

        g_RuntimeMenuLabeledWidget.store(
            reinterpret_cast<uintptr_t>(menuWidget));
        g_RuntimeMenuLabelFailureLogged.store(false);
        Logger::Success(
            "PACKED MENU READY: stock VC3 row labeled Offline Bots - Counselor and stock Jason row labeled without a replacement PAK");
        const uintptr_t menuAddress =
            reinterpret_cast<uintptr_t>(menuWidget);
        if (g_RuntimeMenuOrderedWidget.load() != menuAddress)
        {
            g_RuntimeMenuOrderWidget.store(menuAddress);
            g_RuntimeMenuOrderRequested.store(true);
        }
        return true;
    }

    static void VC3VisibilityExecHook(
        void* context,
        void* stack,
        void* result)
    {
        if (g_OriginalVC3VisibilityExec)
            g_OriginalVC3VisibilityExec(context, stack, result);

        if (!g_CounselorMenuRouteEnabled.load())
            return;

        UObject* menuWidget = reinterpret_cast<UObject*>(context);
        if (result && Memory::IsReadable(result, sizeof(uint8_t)))
        {
            // UE4 ESlateVisibility::Visible is the zero-valued enum entry.
            *reinterpret_cast<uint8_t*>(result) = 0;
        }

        LabelRuntimeOfflinePlayMenu(menuWidget);
    }

    static bool InstallVC3VisibilityHook(bool logUnavailable = true)
    {
        UClass* offlinePlayMenuClass =
            FindClassExact("OfflinePlayMenuWidget_C");
        if (!offlinePlayMenuClass)
            return false;

        UFunction* visibility = FindFunctionInHierarchyByName(
            offlinePlayMenuClass,
            "Get_VC3Button_Visibility");
        if (!visibility ||
            !Memory::IsReadable(visibility, sizeof(UFunction)) ||
            !visibility->ExecFunction)
        {
            if (logUnavailable)
            {
                Logger::Error(
                    "Packed menu: stock VC3 visibility binding was unavailable");
            }
            return false;
        }

        if (visibility->ExecFunction == &VC3VisibilityExecHook)
        {
            g_VC3VisibilityHookInstalled.store(true);
            return true;
        }

        g_OriginalVC3VisibilityExec = visibility->ExecFunction;
        DWORD oldProtect = 0;
        void* targetSlot = &visibility->ExecFunction;
        if (!VirtualProtect(
                targetSlot,
                sizeof(visibility->ExecFunction),
                PAGE_READWRITE,
                &oldProtect))
        {
            g_OriginalVC3VisibilityExec = nullptr;
            return false;
        }

        visibility->ExecFunction = &VC3VisibilityExecHook;
        DWORD ignoredProtect = 0;
        VirtualProtect(
            targetSlot,
            sizeof(visibility->ExecFunction),
            oldProtect,
            &ignoredProtect);

        g_VC3VisibilityHookInstalled.store(true);
        Logger::Success(
            "Packed menu: dormant stock VC3 row visibility hook installed");
        return true;
    }

    static void CounselorEntryExecHook(
        void* context,
        void* stack,
        void* result)
    {
        if (g_CounselorMenuRouteEnabled.load())
        {
            g_CounselorEntryClickPending.store(true);
            g_CounselorEntryClickedAt.store(GetTickCount64());

            Logger::Success(
                "18L-AK dedicated Offline Bots - Counselor row clicked; opening Resurrected's native counselor picker");

            if (OpenIntegratedCounselorPicker(
                    reinterpret_cast<UObject*>(context)))
            {
                return;
            }

            Logger::Error(
                "18L-AL counselor picker unavailable; blocking the dedicated Counselor row instead of silently starting the stock Jason route");
            g_CounselorEntryClickPending.store(false);
            return;
        }

        if (g_OriginalCounselorEntryExec)
            g_OriginalCounselorEntryExec(context, stack, result);

        if (!g_CounselorMenuRouteEnabled.load())
            return;

        UObject* menu = reinterpret_cast<UObject*>(
            g_CounselorMenuObject.load());
        UClass* offlineBotsMode = reinterpret_cast<UClass*>(
            g_CounselorOfflineBotsModeClass.load());

        constexpr uintptr_t PendingOfflineModeOffset = 0x4D0;
        UClass** pendingMode = menu
            ? reinterpret_cast<UClass**>(
                reinterpret_cast<uintptr_t>(menu) +
                PendingOfflineModeOffset)
            : nullptr;

        if (!menu ||
            !Memory::IsReadable(menu, sizeof(UObject)) ||
            !offlineBotsMode ||
            !Memory::IsReadable(offlineBotsMode, sizeof(UClass)) ||
            !pendingMode ||
            !Memory::IsReadable(pendingMode, sizeof(UClass*)))
        {
            Logger::Error(
                "18L-AD dedicated Counselor click: live menu/mode state became unavailable after the native row event");
            return;
        }

        // The native row bytecode has already opened Resurrected's compatible
        // OfflineBots picker. Commit the distinct route before any subsequent
        // picker/settings Blueprint can build the travel URL.
        *pendingMode = offlineBotsMode;

        g_CounselorEntryClickPending.store(false);
        g_CounselorMenuRouteLatched.store(true);
        ArmObjectivesWidgetGameStatePatch();

        Logger::Success(
            "18L-AD SYNCHRONOUS COUNSEL ROUTE LATCH: dedicated row retained stock OFLB travel and armed the counselor lifecycle override");
    }

    static bool InstallCounselorEntryClickHook(
        UObject* menu,
        bool logUnavailable = true)
    {
        if (!menu || !menu->Class)
            return false;

        UClass* offlinePlayMenuClass =
            FindClassExact("OfflinePlayMenuWidget_C");

        if (!offlinePlayMenuClass)
        {
            if (logUnavailable)
            {
                Logger::Debug(
                    "18L-AC: OfflinePlayMenuWidget_C is not loaded yet; Counselor row hook remains pending");
            }
            return false;
        }

        UFunction* entryFunction =
            FindFunctionInHierarchyByName(
                offlinePlayMenuClass,
                "BndEvt__VC3Button_K2Node_ComponentBoundEvent_86_OnClicked__DelegateSignature");

        if (!entryFunction ||
            !Memory::IsReadable(entryFunction, sizeof(UFunction)) ||
            !entryFunction->ExecFunction)
        {
            if (logUnavailable)
            {
                Logger::Error(
                    "18L-AC: dedicated Counselor row event was not available; row-specific route hook NOT installed");
            }
            return false;
        }

        // Blueprint functions can be relinked while the frontend completes
        // its async load.  The installed flag alone is not proof that the
        // currently active event still points at this hook.
        if (entryFunction->ExecFunction == &CounselorEntryExecHook)
        {
            g_CounselorEntryHookInstalled.store(true);
            g_CounselorEntryHookPending.store(false);
            return true;
        }

        const bool repairingRelinkedEvent =
            g_CounselorEntryHookInstalled.exchange(false);

        g_OriginalCounselorEntryExec = entryFunction->ExecFunction;

        DWORD oldProtect = 0;
        void* targetSlot = &entryFunction->ExecFunction;
        if (!VirtualProtect(
                targetSlot,
                sizeof(entryFunction->ExecFunction),
                PAGE_READWRITE,
                &oldProtect))
        {
            Logger::Error(
                "18L-AC: dedicated Counselor row ExecFunction slot could not be made writable");
            g_OriginalCounselorEntryExec = nullptr;
            return false;
        }

        entryFunction->ExecFunction = &CounselorEntryExecHook;

        DWORD ignoredProtect = 0;
        VirtualProtect(
            targetSlot,
            sizeof(entryFunction->ExecFunction),
            oldProtect,
            &ignoredProtect);

        g_CounselorEntryHookInstalled.store(true);
        g_CounselorEntryHookPending.store(false);

        Logger::Success(
            repairingRelinkedEvent
                ? "18L-AX: relinked Offline Bots - Counselor row hook repaired before input"
                : "18L-AC: dedicated Offline Bots - Counselor row hook installed; Jason and Sandbox remain distinct");
        return true;
    }

    static void CounselRequestOfflineModeExecHook(
        void* context,
        void* stack,
        void* result)
    {
        // Let the game's real RequestOfflineMode execute first.  The old
        // O/P/R builds proved that the stock COUNSEL/Sandbox click overwrites
        // PendingOfflineMode inside its own synchronous path.  We therefore
        // inspect the value immediately on RETURN from the native exec wrapper,
        // while the same Blueprint call is still on the stack.
        if (g_OriginalRequestOfflineModeExec)
            g_OriginalRequestOfflineModeExec(context, stack, result);

        if (!g_CounselorMenuRouteEnabled.load())
            return;

        UObject* menu =
            (UObject*)g_CounselorMenuObject.load();

        // RequestOfflineMode is a static ILLBackendBlueprintLibrary call, so
        // Context is the library's default object rather than SCGame_Menu.
        // The frontend menu captured while arming remains the authoritative
        // owner of PendingOfflineMode.
        if (!menu ||
            !Memory::IsReadable(menu, sizeof(UObject)))
        {
            return;
        }

        UClass* offlineBotsMode =
            (UClass*)g_CounselorOfflineBotsModeClass.load();

        UClass* sandboxMode =
            (UClass*)g_CounselorSandboxModeClass.load();

        UClass* counselorReferenceMode =
            (UClass*)g_CounselorReferenceModeClass.load();

        if (!offlineBotsMode ||
            (!counselorReferenceMode && !sandboxMode))
            return;

        constexpr uintptr_t PendingOfflineModeOffset = 0x4D0;

        UClass** pendingMode =
            (UClass**)((uintptr_t)menu +
                PendingOfflineModeOffset);

        if (!Memory::IsReadable(pendingMode, sizeof(UClass*)))
        {
            Logger::Error(
                "18L-AC synchronous route: PendingOfflineMode unreadable on RequestOfflineMode return");
            return;
        }

        UClass* currentMode = *pendingMode;
        std::string currentName =
            currentMode
            ? SafeName((UObject*)currentMode)
            : std::string("<null>");

        uint32_t hit =
            g_RequestOfflineModeHookHits.fetch_add(1) + 1;

        if (hit <= 8)
        {
            Logger::Debug(
                "18L-AC RequestOfflineMode RETURN: PendingOfflineMode=" +
                currentName);
        }

        const bool dedicatedCounselorClick =
            g_CounselorEntryClickPending.load();

        // The native Counselor row intentionally opens the stock Offline Bots
        // picker so map, Jason and game-setup selection all remain available.
        // Its row hook is the only distinction from the Jason entry.
        if ((currentMode == offlineBotsMode ||
                currentName == "ModeDef_OfflineBots_C") &&
            dedicatedCounselorClick)
        {
            g_CounselorEntryClickPending.store(false);
            g_CounselorMenuRouteLatched.store(true);
            ArmObjectivesWidgetGameStatePatch();

            Logger::Success(
                "18L-AC SYNCHRONOUS COUNSEL ROUTE LATCH: dedicated Counselor row submitted ModeDef_OfflineBots_C through the native picker; Jason row remains stock");
        }
        // Keep compatibility with the reference package's explicit counselor
        // mode. Never treat Sandbox as Counselor in the final five-entry menu.
        else if ((counselorReferenceMode && currentMode == counselorReferenceMode) ||
            currentName == "ModeDef_OfflineBotsC_C")
        {
            *pendingMode = offlineBotsMode;
            g_CounselorEntryClickPending.store(false);
            g_CounselorMenuRouteLatched.store(true);
            ArmObjectivesWidgetGameStatePatch();

            Logger::Success(
                "18L-AC SYNCHRONOUS COUNSEL ROUTE LATCH: RequestOfflineMode selected " +
                currentName +
                " -> replaced with ModeDef_OfflineBots_C before Blueprint resumed");
        }
        else if (currentMode == offlineBotsMode ||
            currentName == "ModeDef_OfflineBots_C")
        {
            g_CounselorEntryClickPending.store(false);
            Logger::Debug(
                "18L-AC RequestOfflineMode: direct OfflineBots request observed; leaving PLAY JASON untouched");
        }
        else if (currentMode == sandboxMode ||
            currentName == "ModeDef_Sandbox_C")
        {
            g_CounselorEntryClickPending.store(false);
            Logger::Debug(
                "18L-AC RequestOfflineMode: real Sandbox request observed; leaving Sandbox untouched");
        }
    }

    static bool InstallSynchronousCounselRouteHook(
        UObject* menu)
    {
        if (g_RequestOfflineModeHookInstalled.load())
            return true;

        if (!menu || !menu->Class)
            return false;

        UClass* backendLibrary =
            FindClassExact("ILLBackendBlueprintLibrary");

        if (!backendLibrary)
        {
            Logger::Error(
                "18L-AC: ILLBackendBlueprintLibrary class was not loaded; synchronous route hook NOT installed");
            return false;
        }

        UFunction* requestOfflineMode =
            FindFunctionInHierarchyByName(
                backendLibrary,
                "RequestOfflineMode");

        if (!requestOfflineMode ||
            !Memory::IsReadable(requestOfflineMode, sizeof(UFunction)) ||
            !requestOfflineMode->ExecFunction)
        {
            Logger::Error(
                "18L-AC: ILLBackendBlueprintLibrary::RequestOfflineMode native exec wrapper was not available; synchronous route hook NOT installed");
            return false;
        }

        LPVOID target =
            reinterpret_cast<LPVOID>(
                requestOfflineMode->ExecFunction);

        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(target, &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & PAGE_NOACCESS) ||
            (mbi.Protect & PAGE_GUARD))
        {
            Logger::Error(
                "18L-AC: RequestOfflineMode ExecFunction address is not executable/readable");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK &&
            initStatus != MH_ERROR_ALREADY_INITIALIZED)
        {
            Logger::Error(
                "18L-AC: MH_Initialize failed while installing synchronous COUNSEL route hook");
            return false;
        }

        MH_STATUS createStatus =
            MH_CreateHook(
                target,
                reinterpret_cast<LPVOID>(
                    &CounselRequestOfflineModeExecHook),
                reinterpret_cast<LPVOID*>(
                    &g_OriginalRequestOfflineModeExec));

        if (createStatus != MH_OK)
        {
            Logger::Error(
                "18L-AC: MH_CreateHook failed for RequestOfflineMode ExecFunction");
            return false;
        }

        if (MH_EnableHook(target) != MH_OK)
        {
            Logger::Error(
                "18L-AC: MH_EnableHook failed for RequestOfflineMode ExecFunction");
            return false;
        }

        g_RequestOfflineModeHookTarget.store(
            (uintptr_t)target);
        g_RequestOfflineModeHookInstalled.store(true);

        Logger::Success(
            "18L-AC: synchronous ILLBackendBlueprintLibrary::RequestOfflineMode hook installed; no worker-thread PendingOfflineMode rewrite is used");

        return true;
    }

    static bool ArmSelectedPreset()
    {
        const ULONGLONG armStartedAt = GetTickCount64();
        if (g_CounselorAliasArmed.load())
        {
            Logger::Debug(
                "18L-AO counselor route arm deferred: previous OFLBC alias still awaits game-thread restoration");
            return false;
        }
        UObject* menu = GetActiveFrontendMenu();
        UWorld* world = Engine::GetWorld();

        if (!menu || !world)
        {
            Logger::Error(
                "Counselor menu 18L-AC: frontend SCGame_Menu/EntryGame_C not ready. Return to Offline Play and try ARM again.");
            return false;
        }

        UClass* offlineBotsMode =
            FindClassExact("ModeDef_OfflineBots_C");

        UClass* sandboxMode =
            FindClassExact("ModeDef_Sandbox_C");

        UClass* counselorReferenceMode =
            FindClassExact("ModeDef_OfflineBotsC_C");

        if (!offlineBotsMode ||
            (!counselorReferenceMode && !sandboxMode))
        {
            Logger::Error(
                "Counselor menu 18L-AC: ModeDef_OfflineBots_C is missing, or neither ModeDef_OfflineBotsC_C nor ModeDef_Sandbox_C is loaded.");
            return false;
        }

        g_CounselorMenuWorld.store((uintptr_t)world);
        g_CounselorMenuObject.store((uintptr_t)menu);
        g_CounselorOfflineBotsModeClass.store(
            (uintptr_t)offlineBotsMode);
        g_CounselorSandboxModeClass.store(
            (uintptr_t)sandboxMode);
        g_CounselorReferenceModeClass.store(
            (uintptr_t)counselorReferenceMode);
        g_CounselorMenuRewriteCount.store(0);
        g_RequestOfflineModeHookHits.store(0);
        g_CounselorMenuRouteLatched.store(false);
        g_CounselorBirthComplete.store(false);
        g_CounselorMatchInProgress.store(false);
        g_CounselorJasonActive.store(false);
        g_CounselorBotsCreated.store(0);
        g_CounselorBirthPawn.store(0);
        g_CounselorEntryClickPending.store(false);
        g_CounselorEntryClickedAt.store(0);
        g_CounselorEntryHookPending.store(false);
        g_CounselorEntryHookLastAttempt.store(0);
        g_CounselorPickerActive.store(false);
        g_CounselorPickerAccepted.store(false);
        g_CounselorPickerWidget.store(0);
        g_CounselorPickerSourceMenu.store(0);
        g_NativeSelectedCounselorTotal.store(0);
        g_GameSetupSelectionLocked.store(false);
        g_RuntimeMenuLabeledWidget.store(0);
        g_RuntimeMenuLabelLastAttempt.store(0);
        g_RuntimeMenuLabelFailureLogged.store(false);
        g_RuntimeMenuOrderedWidget.store(0);
        g_RuntimeMenuOrderFailureLogged.store(false);
        g_RuntimeMenuOrderRequested.store(false);
        g_RuntimeMenuOrderWidget.store(0);
        g_RuntimeMenuOrderFailureCode.store(0);

        if (!InstallSynchronousCounselRouteHook(menu))
        {
            g_CounselorMenuRouteEnabled.store(false);
            g_CounselorModeAutoStartArmed.store(false);
            return false;
        }

        const bool runtimeMenuVisibilityReady =
            InstallVC3VisibilityHook();
        const bool counselorEntryHookReady =
            InstallCounselorEntryClickHook(menu);
        g_CounselorEntryHookPending.store(!counselorEntryHookReady);

        UClass* counselorMenuClass =
            FindClassExact("Counselor_Menu_C");
        const bool integratedPickerReady =
            counselorMenuClass &&
            InstallCounselorPickerHooks(counselorMenuClass) &&
            PatchCounselorPickerConstructPlayerStateCast(
                counselorMenuClass,
                false);
        if (integratedPickerReady)
        {
            Logger::Success(
                "18L-AM counselor picker runtime route ready: offline Construct cast + native Accept continuation are installed");
        }
        else
        {
            Logger::Error(
                "18L-AK counselor picker hooks pending: Counselor_Menu_C is not loaded yet; the row will retry when clicked");
        }

        constexpr uintptr_t PendingOfflineModeOffset = 0x4D0;
        UClass** pendingMode =
            (UClass**)((uintptr_t)menu +
                PendingOfflineModeOffset);

        std::string initialMode = "<unreadable>";
        if (Memory::IsReadable(pendingMode, sizeof(UClass*)))
        {
            initialMode =
                *pendingMode
                ? SafeName((UObject*)*pendingMode)
                : std::string("<null>");
        }

        // IMPORTANT: unlike O/P/R/Q, AB does NOT pre-write PendingOfflineMode.
        // It waits for the actual COUNSEL/Sandbox RequestOfflineMode call and
        // changes the result synchronously on that same call stack.
        g_CounselorMenuRouteEnabled.store(true);
        g_CounselorModeAutoStartArmed.store(true);

        int32_t counselors =
            ClampCounselorCount(
                g_SelectedCounselorCount.load());

        int32_t counselorIndex =
            ClampPlayerCounselorIndex(
                g_SelectedPlayerCounselorIndex.load());

        Logger::Success(
            std::string("Counselor menu 18L-AC ARMED: dedicated Counselor row is waiting to submit ModeDef_OfflineBots_C through the native map/settings picker") +
            (runtimeMenuVisibilityReady
                ? " | PackedMenu=visible"
                : " | PackedMenu=pending") +
            (counselorEntryHookReady
                ? " | RowHook=ready"
                : " | RowHook=pending until Offline Play opens") +
            (integratedPickerReady
                ? " | CounselorPicker=ready"
                : " | CounselorPicker=pending") +
            " | InitialPendingMode=" + initialMode +
            " | Player=" +
            kPlayerCounselorClassNames[counselorIndex] +
            " | CounselorBots=" +
            std::to_string(counselors) +
            " | ObjectStride=24 | ArmMs=" +
            std::to_string(GetTickCount64() - armStartedAt));

        Logger::Debug(
            "Counselor menu 18L-AC: now click Offline Bots - Counselor. Offline Bots - Jason and Sandbox are not redirected.");

        return true;
    }

    static void TickMapSticky()
    {
        if (!g_CounselorMenuRouteEnabled.load())
            return;

        UWorld* world = Engine::GetWorld();
        uintptr_t originalWorld =
            g_CounselorMenuWorld.load();

        if (!world ||
            !originalWorld ||
            (uintptr_t)world != originalWorld)
        {
            g_CounselorMenuRouteEnabled.store(false);
            g_CounselorEntryClickPending.store(false);
            if (g_CounselorAliasArmed.load())
                g_CounselorAliasRestorePending.store(true);

            if (g_CounselorMenuRouteLatched.load())
            {
                Logger::Success(
                    "Counselor menu 18L-AO: frontend world changed AFTER synchronous latch; stock OFLB host will enter the counselor lifecycle hooks");
            }
            else
            {
                Logger::Error(
                    "Counselor menu 18L-AC: frontend world changed before synchronous RequestOfflineMode latch fired");
            }

            return;
        }

        // UE can relink this Blueprint event after the initial frontend arm.
        // Re-resolve and validate the exact slot at a bounded menu-only
        // cadence.  This work ends as soon as the frontend world changes.
        const ULONGLONG hookNow = GetTickCount64();
        if (hookNow - g_CounselorEntryHookLastAttempt.load() >= 250)
        {
            g_CounselorEntryHookLastAttempt.store(hookNow);
            InstallVC3VisibilityHook(false);
            UObject* menu = reinterpret_cast<UObject*>(
                g_CounselorMenuObject.load());
            if (!InstallCounselorEntryClickHook(menu, false))
            {
                g_CounselorEntryHookPending.store(true);
            }
        }

        // Route-only monitor.  Deliberately no asynchronous PendingOfflineMode
        // writes here.  O/P/R already proved that worker-thread sticky writes
        // lose to the stock click path.
    }

    static UObject* RawFindExactClassObject(
        UClass* targetClass,
        const char* exactObjectName)
    {
        if (!targetClass || !exactObjectName)
            return nullptr;

        SYSTEM_INFO si{};
        GetSystemInfo(&si);

        uintptr_t address =
            (uintptr_t)si.lpMinimumApplicationAddress;

        uintptr_t maxAddress =
            (uintptr_t)si.lpMaximumApplicationAddress;

        MEMORY_BASIC_INFORMATION mbi{};

        while (address < maxAddress)
        {
            SIZE_T queried =
                VirtualQuery(
                    (LPCVOID)address,
                    &mbi,
                    sizeof(mbi));

            if (!queried)
                break;

            uintptr_t regionStart =
                (uintptr_t)mbi.BaseAddress;

            uintptr_t regionEnd =
                regionStart + mbi.RegionSize;

            bool readable =
                mbi.State == MEM_COMMIT &&
                !(mbi.Protect & PAGE_GUARD) &&
                !(mbi.Protect & PAGE_NOACCESS);

            if (readable &&
                mbi.RegionSize >= sizeof(uintptr_t))
            {
                constexpr size_t ChunkSize =
                    1u << 20;

                std::vector<uint8_t> buffer;
                buffer.resize(ChunkSize);

                for (uintptr_t p = regionStart;
                    p < regionEnd;)
                {
                    size_t remain =
                        (size_t)(regionEnd - p);

                    size_t want =
                        remain < ChunkSize ?
                        remain :
                        ChunkSize;

                    SIZE_T readBytes = 0;

                    if (ReadProcessMemory(
                        GetCurrentProcess(),
                        (LPCVOID)p,
                        buffer.data(),
                        want,
                        &readBytes) &&
                        readBytes >= sizeof(uintptr_t))
                    {
                        for (size_t i = 0;
                            i + sizeof(uintptr_t) <= readBytes;
                            ++i)
                        {
                            uintptr_t value = 0;

                            memcpy(
                                &value,
                                buffer.data() + i,
                                sizeof(value));

                            if (value !=
                                (uintptr_t)targetClass)
                            {
                                continue;
                            }

                            if (p + i < 0x10)
                                continue;

                            UObject* candidate =
                                (UObject*)(p + i - 0x10);

                            if (!Memory::IsReadable(
                                candidate,
                                sizeof(UObject)))
                            {
                                continue;
                            }

                            if (candidate->Class != targetClass)
                                continue;

                            if (SafeName(candidate) ==
                                exactObjectName)
                            {
                                return candidate;
                            }
                        }
                    }

                    if (!want)
                        break;

                    p += want;
                }
            }

            if (regionEnd <= address)
                break;

            address = regionEnd;
        }

        return nullptr;
    }

    static std::string ReadFStringAscii(
        UObject* object,
        uintptr_t offset)
    {
        if (!object)
            return {};

        RawFString* value =
            (RawFString*)((uintptr_t)object + offset);

        if (!Memory::IsReadable(value, sizeof(RawFString)) ||
            !value->Data ||
            value->Count <= 0 ||
            value->Count > 256 ||
            value->Max < value->Count ||
            !Memory::IsReadable(
                value->Data,
                sizeof(wchar_t) * (size_t)value->Count))
        {
            return {};
        }

        std::string out;

        for (int32_t i = 0;
            i < value->Count;
            ++i)
        {
            wchar_t ch = value->Data[i];

            if (!ch)
                break;

            if (ch >= 0 &&
                ch <= 0x7F)
            {
                out.push_back((char)ch);
            }
            else
            {
                out.push_back('?');
            }
        }

        return out;
    }

    static bool WriteFStringAsciiInPlace(
        UObject* object,
        uintptr_t offset,
        const wchar_t* text)
    {
        if (!object || !text)
            return false;

        RawFString* value =
            (RawFString*)((uintptr_t)object + offset);

        if (!Memory::IsReadable(value, sizeof(RawFString)) ||
            !value->Data ||
            value->Max <= 0 ||
            value->Max > 4096)
        {
            return false;
        }

        size_t chars =
            std::wcslen(text) + 1;

        if (chars > (size_t)value->Max ||
            !Memory::IsReadable(
                value->Data,
                sizeof(wchar_t) * chars))
        {
            return false;
        }

        std::wmemcpy(
            value->Data,
            text,
            chars);

        value->Count =
            (int32_t)chars;

        return true;
    }

    static bool ValidateSettingsCombo(
        UObject* object,
        const char* exactName)
    {
        if (!object ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }

        return SafeName(object) == exactName;
    }

    static bool ApplyGameSetupPreset()
    {
        UObject* menu = GetActiveFrontendMenu();

        if (!menu)
        {
            Logger::Error(
                "Unified setup 18B: frontend menu not ready. Use F5 while the stock Game Setup screen is visible.");
            return false;
        }

        constexpr uintptr_t PendingOfflineSettingsWidgetOffset =
            0x4D8;

        UClass** settingsClassAddress =
            (UClass**)((uintptr_t)menu +
                PendingOfflineSettingsWidgetOffset);

        if (!Memory::IsReadable(
            settingsClassAddress,
            sizeof(UClass*)) ||
            !*settingsClassAddress)
        {
            Logger::Error(
                "Unified setup 18B: PendingOfflineSettingsWidget class is null/unreadable");
            return false;
        }

        UClass* settingsClass =
            *settingsClassAddress;

        Logger::Debug(
            "Unified setup 18B: searching for live OfflineBotsSettingsMenuWidget_C");

        UObject* settings =
            RawFindExactClassObject(
                settingsClass,
                "OfflineBotsSettingsMenuWidget_C");

        if (!settings)
        {
            Logger::Error(
                "Unified setup 18B: live OfflineBotsSettingsMenuWidget_C not found. Leave Game Setup visible and press F5 again.");
            return false;
        }

        constexpr uintptr_t CounselorComboOffset = 0x400;
        constexpr uintptr_t DifficultyComboOffset = 0x408;
        constexpr uintptr_t WeatherComboOffset = 0x440;
        constexpr uintptr_t WeatherStringOffset = 0x448;
        constexpr uintptr_t TravelPathOffset = 0x458;
        constexpr uintptr_t CurrentOptionIndexOffset = 0x458;

        UObject** counselorComboAddress =
            (UObject**)((uintptr_t)settings +
                CounselorComboOffset);

        UObject** difficultyComboAddress =
            (UObject**)((uintptr_t)settings +
                DifficultyComboOffset);

        UObject** weatherComboAddress =
            (UObject**)((uintptr_t)settings +
                WeatherComboOffset);

        if (!Memory::IsReadable(
                counselorComboAddress,
                sizeof(UObject*)) ||
            !Memory::IsReadable(
                difficultyComboAddress,
                sizeof(UObject*)) ||
            !Memory::IsReadable(
                weatherComboAddress,
                sizeof(UObject*)))
        {
            Logger::Error(
                "Unified setup 18B: Game Setup combo pointers unreadable");
            return false;
        }

        UObject* counselorCombo =
            *counselorComboAddress;

        UObject* difficultyCombo =
            *difficultyComboAddress;

        UObject* weatherCombo =
            *weatherComboAddress;

        if (!ValidateSettingsCombo(
                counselorCombo,
                "CounselorCountComboBox") ||
            !ValidateSettingsCombo(
                difficultyCombo,
                "DifficultyComboBox") ||
            !ValidateSettingsCombo(
                weatherCombo,
                "WeatherComboBox"))
        {
            Logger::Error(
                "Unified setup 18B: live Game Setup combo validation failed");
            return false;
        }

        int32_t* counselorIndex =
            (int32_t*)((uintptr_t)counselorCombo +
                CurrentOptionIndexOffset);

        int32_t* difficultyIndex =
            (int32_t*)((uintptr_t)difficultyCombo +
                CurrentOptionIndexOffset);

        int32_t* weatherIndex =
            (int32_t*)((uintptr_t)weatherCombo +
                CurrentOptionIndexOffset);

        if (!Memory::IsReadable(
                counselorIndex,
                sizeof(int32_t)) ||
            !Memory::IsReadable(
                difficultyIndex,
                sizeof(int32_t)) ||
            !Memory::IsReadable(
                weatherIndex,
                sizeof(int32_t)))
        {
            Logger::Error(
                "Unified setup 18B: Game Setup CurrentOptionIndex fields unreadable");
            return false;
        }

        int32_t beforeCounselors =
            *counselorIndex;

        int32_t beforeDifficulty =
            *difficultyIndex;

        int32_t beforeWeather =
            *weatherIndex;

        std::string beforeWeatherString =
            ReadFStringAscii(
                settings,
                WeatherStringOffset);

        std::string travelPath =
            ReadFStringAscii(
                settings,
                TravelPathOffset);

        int32_t selectedCounselors =
            ClampCounselorCount(g_SelectedCounselorCount.load());

        int32_t selectedDifficulty =
            ClampDifficulty(g_SelectedDifficulty.load());

        int32_t selectedWeather =
            ClampWeather(g_SelectedWeather.load());

        *counselorIndex =
            selectedCounselors - 1;

        *difficultyIndex =
            selectedDifficulty;

        bool weatherWrite =
            WriteFStringAsciiInPlace(
                settings,
                WeatherStringOffset,
                kWeatherStrings[selectedWeather]);

        Logger::Debug(
            "Unified setup 18B BEFORE: counselorIndex=" +
            std::to_string(beforeCounselors) +
            " difficultyIndex=" +
            std::to_string(beforeDifficulty) +
            " weatherIndex=" +
            std::to_string(beforeWeather) +
            " weather=\"" +
            beforeWeatherString +
            "\" travelPath=\"" +
            travelPath +
            "\"");

        std::string expectedWeather;
        const wchar_t* weatherText =
            kWeatherStrings[selectedWeather];

        for (size_t i = 0; weatherText[i]; ++i)
            expectedWeather.push_back((char)weatherText[i]);

        Logger::Success(
            "Unified setup 18B APPLY: Counselors=" +
            std::to_string(selectedCounselors) +
            " index=" +
            std::to_string(*counselorIndex) +
            " | Difficulty=" +
            kDifficultyNames[selectedDifficulty] +
            " index=" +
            std::to_string(*difficultyIndex) +
            " | Weather=" +
            kWeatherNames[selectedWeather] +
            " string=\"" +
            ReadFStringAscii(settings, WeatherStringOffset) +
            "\" write=" +
            (weatherWrite ? std::string("true") :
                std::string("false")));

        return
            *counselorIndex == selectedCounselors - 1 &&
            *difficultyIndex == selectedDifficulty &&
            weatherWrite &&
            ReadFStringAscii(
                settings,
                WeatherStringOffset) == expectedWeather;
    }

    static int32_t ClampPlayerCounselorIndex(int32_t value)
    {
        constexpr int32_t count =
            (int32_t)(sizeof(kPlayerCounselorClassNames) /
                sizeof(kPlayerCounselorClassNames[0]));

        if (value < 0)
            return 0;

        if (value >= count)
            return count - 1;

        return value;
    }

    static void TickNativeProfileSelection()
    {
        if (g_GameSetupSelectionLocked.load())
            return;

        // Until the dedicated Counselor row is committed, the game's own
        // The native profile remains authoritative throughout the frontend
        // setup flow. In particular, Jason_Select_Widget writes KillerPick
        // after the counselor route has already latched. Stopping here merely
        // because the route was latched left the earlier Jason (normally J5)
        // cached for both the intro and the independent AI spawn. Keep this
        // bounded read-only sync alive while EntryGame/SCGame_Menu is still
        // authoritative, then stop before gameplay world travel.
        if (g_CounselorMenuRouteLatched.load() &&
            !GetActiveFrontendMenu())
            return;

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG next = g_NextProfileSelectionSyncAt.load();
        if (next && now < next)
            return;
        g_NextProfileSelectionSyncAt.store(now + 500);

        if (!ResolveSelectionSaveMetadata())
            return;

        UObject* saveObject = reinterpret_cast<UObject*>(
            g_SelectionSaveObject.load());
        if (!saveObject ||
            !Memory::IsReadable(saveObject, sizeof(UObject)))
        {
            saveObject = RawFindSelectionSaveObject(
                reinterpret_cast<UClass*>(g_SelectionSaveClass.load()));
            if (!saveObject)
                return;
            g_SelectionSaveObject.store(reinterpret_cast<uintptr_t>(saveObject));
        }

        const int32_t counselorOffset = g_CounselorPickOffset.load();
        const int32_t killerOffset = g_KillerPickOffset.load();
        if (counselorOffset > 0)
        {
            UClass** counselorPick = reinterpret_cast<UClass**>(
                reinterpret_cast<uintptr_t>(saveObject) + counselorOffset);
            if (Memory::IsReadable(counselorPick, sizeof(UClass*)) &&
                *counselorPick &&
                Memory::IsReadable(*counselorPick, sizeof(UClass)))
            {
                UClass* selectedClass = *counselorPick;
                const std::string selectedName = SafeName(
                    reinterpret_cast<UObject*>(selectedClass));

                constexpr int32_t counselorCount =
                    static_cast<int32_t>(sizeof(kPlayerCounselorClassNames) /
                        sizeof(kPlayerCounselorClassNames[0]));
                for (int32_t i = 0; i < counselorCount; ++i)
                {
                    if (selectedName != kPlayerCounselorClassNames[i])
                        continue;

                    g_SelectedPlayerCounselorIndex.store(i);
                    g_TargetPlayerCounselorClass.store(
                        reinterpret_cast<uintptr_t>(selectedClass));

                    const uintptr_t previous =
                        g_LastProfileCounselorClass.exchange(
                            reinterpret_cast<uintptr_t>(selectedClass));
                    if (previous != reinterpret_cast<uintptr_t>(selectedClass))
                    {
                        Logger::Success(
                            "IN-GAME COUNSELOR SELECTION: Customize profile -> " +
                            selectedName);
                    }
                    break;
                }
            }
        }

        if (killerOffset > 0)
        {
            UClass** killerPick = reinterpret_cast<UClass**>(
                reinterpret_cast<uintptr_t>(saveObject) + killerOffset);
            if (Memory::IsReadable(killerPick, sizeof(UClass*)) &&
                *killerPick &&
                Memory::IsReadable(*killerPick, sizeof(UClass)))
            {
                UClass* selectedClass = *killerPick;
                g_TargetJasonClass.store(
                    reinterpret_cast<uintptr_t>(selectedClass));

                const uintptr_t previous =
                    g_LastProfileKillerClass.exchange(
                        reinterpret_cast<uintptr_t>(selectedClass));
                if (previous != reinterpret_cast<uintptr_t>(selectedClass))
                {
                    Logger::Success(
                        "IN-GAME JASON SELECTION: native picker/profile -> " +
                        SafeName(reinterpret_cast<UObject*>(selectedClass)));
                }
            }
        }
    }

    static UObject* FindDefaultGameModeForCounselorArray(
        NativeObjectItem* objects,
        int32_t objectCount)
    {
        UObject* offlineBots = nullptr;
        UObject* sandbox = nullptr;
        UObject* base = nullptr;

        for (int32_t i = 0; i < objectCount; ++i)
        {
            UObject* obj = objects[i].Object;

            if (!obj ||
                !Memory::IsReadable(obj, sizeof(UObject)))
            {
                continue;
            }

            std::string name = SafeName(obj);

            if (!offlineBots &&
                name == "Default__SCGameMode_OfflineBots")
            {
                offlineBots = obj;
            }
            else if (!sandbox &&
                name == "Default__SCGameMode_Sandbox")
            {
                sandbox = obj;
            }
            else if (!base &&
                name == "Default__SCGameMode")
            {
                base = obj;
            }

            if (offlineBots)
                break;
        }

        return offlineBots ? offlineBots :
            (sandbox ? sandbox : base);
    }

    static bool RequestSelectedCounselorSoftClassOnGameThread()
    {
        NativeObjectItem* objects = nullptr;
        int32_t objectCount = 0;

        if (!GetGObjects(&objects, &objectCount))
        {
            Logger::Error(
                "Counselor selection 18K: GObjects unavailable");
            return false;
        }

        UObject* gameModeDefault =
            FindDefaultGameModeForCounselorArray(
                objects,
                objectCount);

        if (!gameModeDefault ||
            !gameModeDefault->Class)
        {
            Logger::Error(
                "Counselor selection 18K: Offline Bots GameMode CDO not ready");
            return false;
        }

        int32_t index =
            ClampPlayerCounselorIndex(
                g_SelectedPlayerCounselorIndex.load());

        uint8_t* selectedSoftClass = nullptr;

        if (std::string(kPlayerCounselorClassNames[index]) ==
            "Hunter_Counselor_C")
        {
            // Tommy is deliberately not an entry in the normal counselor
            // roster. Use the GameMode's real HunterCharacterClass soft class
            // so the special hero can be selected without inventing an index.
            UPropertyLite* hunterProperty =
                FindPropertyInHierarchyByName(
                    gameModeDefault->Class,
                    "HunterCharacterClass");

            if (!hunterProperty ||
                hunterProperty->Offset_Internal <= 0 ||
                hunterProperty->Offset_Internal >= 0x10000 ||
                hunterProperty->ElementSize < SoftClassSize)
            {
                Logger::Error(
                    "Counselor selection 18K: HunterCharacterClass property unavailable for Tommy");
                return false;
            }

            selectedSoftClass =
                (uint8_t*)((uintptr_t)gameModeDefault +
                    hunterProperty->Offset_Internal);
        }
        else
        {
            UPropertyLite* counselorClassesProperty =
                FindPropertyInHierarchyByName(
                    gameModeDefault->Class,
                    "CounselorCharacterClasses");

            if (!counselorClassesProperty ||
                counselorClassesProperty->Offset_Internal <= 0 ||
                counselorClassesProperty->Offset_Internal >= 0x10000)
            {
                Logger::Error(
                    "Counselor selection 18K: CounselorCharacterClasses property unavailable");
                return false;
            }

            RawArray* counselorClasses =
                (RawArray*)((uintptr_t)gameModeDefault +
                    counselorClassesProperty->Offset_Internal);

            if (!Memory::IsReadable(
                    counselorClasses,
                    sizeof(RawArray)) ||
                !counselorClasses->Data ||
                counselorClasses->Count <= 0 ||
                counselorClasses->Count > 64 ||
                counselorClasses->Max < counselorClasses->Count ||
                counselorClasses->Max > 128 ||
                !Memory::IsReadable(
                    counselorClasses->Data,
                    (size_t)counselorClasses->Count * SoftClassSize))
            {
                Logger::Error(
                    "Counselor selection 18K: counselor soft-class array unavailable");
                return false;
            }

            if (index >= counselorClasses->Count)
            {
                Logger::Error(
                    "Counselor selection 18K: selected index outside native roster");
                return false;
            }

            selectedSoftClass =
                counselorClasses->Data +
                ((uintptr_t)index * SoftClassSize);
        }

        if (!Memory::IsReadable(
                selectedSoftClass,
                SoftClassSize))
        {
            Logger::Error(
                "Counselor selection 18K: selected native soft class unreadable");
            return false;
        }

        g_SelectedPlayerCounselorPath =
            ReadSoftClassAssetPath(selectedSoftClass);

        memcpy(
            g_SelectedPlayerCounselorSoftClass.data(),
            selectedSoftClass,
            SoftClassSize);

        if (g_SelectedPlayerCounselorPath.empty())
        {
            Logger::Error(
                "Counselor selection 18K: selected native soft-class path is empty");
            return false;
        }

        auto localController =
            Engine::GetLocalPlayerController();

        if (!localController ||
            !Memory::IsReadable(
                localController,
                sizeof(UObject)) ||
            !localController->Class)
        {
            Logger::Error(
                "Counselor selection 18K: local PlayerController unavailable");
            return false;
        }

        // Do not guess a PlayerState offset.  Resolve the inherited Controller
        // property from reflection, exactly as the 18G->18H handoff requires.
        UPropertyLite* controllerPlayerStateProperty =
            FindPropertyInHierarchyByName(
                localController->Class,
                "PlayerState");

        if (!controllerPlayerStateProperty ||
            controllerPlayerStateProperty->Offset_Internal <= 0 ||
            controllerPlayerStateProperty->Offset_Internal >= 0x10000 ||
            controllerPlayerStateProperty->ElementSize < (int32_t)sizeof(UObject*))
        {
            Logger::Error(
                "Counselor selection 18K: reflected Controller::PlayerState property unavailable");
            return false;
        }

        UObject** playerStateSlot =
            (UObject**)((uintptr_t)localController +
                controllerPlayerStateProperty->Offset_Internal);

        if (!Memory::IsReadable(
                playerStateSlot,
                sizeof(UObject*)))
        {
            Logger::Error(
                "Counselor selection 18K: reflected PlayerState slot unreadable");
            return false;
        }

        UObject* playerState =
            *playerStateSlot;

        if (!playerState ||
            !Memory::IsReadable(
                playerState,
                sizeof(UObject)) ||
            !playerState->Class)
        {
            Logger::Error(
                "Counselor selection 18K: local PlayerState unavailable");
            return false;
        }

        UClass* scPlayerStateClass =
            FindClassExact("SCPlayerState");

        if (!scPlayerStateClass ||
            !ClassIsOrDerivesFrom(
                playerState->Class,
                scPlayerStateClass))
        {
            Logger::Error(
                "Counselor selection 18K: local PlayerState is not SCPlayerState-derived | class=" +
                SafeName((UObject*)playerState->Class));
            return false;
        }

        UFunction* requestFunction =
            FindFunctionInHierarchyByName(
                playerState->Class,
                "RequestCounselorClass");

        UPropertyLite* pickedProperty =
            FindPropertyInHierarchyByName(
                playerState->Class,
                "PickedCounselorClass");

        if (!requestFunction ||
            !pickedProperty ||
            pickedProperty->Offset_Internal <= 0 ||
            pickedProperty->Offset_Internal >= 0x10000 ||
            pickedProperty->ElementSize != SoftClassSize)
        {
            Logger::Error(
                "Counselor selection 18K: RequestCounselorClass/PickedCounselorClass unavailable or wrong size");
            return false;
        }

        UPropertyLite* newCounselorClassProperty = nullptr;
        UField* field = requestFunction->Children;
        int guard = 0;

        while (field && guard++ < 64)
        {
            if (!Memory::IsReadable(
                    field,
                    sizeof(UField)))
            {
                break;
            }

            if (SafeName((UObject*)field) == "NewCounselorClass")
            {
                UObject* fieldClass =
                    (UObject*)field->ClassPrivate;

                if (fieldClass &&
                    Memory::IsReadable(
                        fieldClass,
                        sizeof(UObject)) &&
                    SafeName(fieldClass) == "SoftClassProperty")
                {
                    UPropertyLite* candidate =
                        (UPropertyLite*)field;

                    if (Memory::IsReadable(
                            candidate,
                            sizeof(UPropertyLite)))
                    {
                        newCounselorClassProperty =
                            candidate;
                    }
                }

                break;
            }

            field = field->Next;
        }

        if (!newCounselorClassProperty ||
            newCounselorClassProperty->Offset_Internal < 0 ||
            newCounselorClassProperty->ElementSize != SoftClassSize ||
            newCounselorClassProperty->Offset_Internal +
                SoftClassSize > 0x100)
        {
            Logger::Error(
                "Counselor selection 18K: NewCounselorClass parameter layout unavailable or not 40-byte SoftClassProperty");
            return false;
        }

        uint8_t* pickedSoftClass =
            (uint8_t*)((uintptr_t)playerState +
                pickedProperty->Offset_Internal);

        if (!Memory::IsReadable(
                pickedSoftClass,
                SoftClassSize))
        {
            Logger::Error(
                "Counselor selection 18K: PickedCounselorClass storage unreadable");
            return false;
        }

        std::string oldPickedPath =
            ReadSoftClassAssetPath(
                pickedSoftClass);

        Logger::Success(
            "Counselor selection 18K: local PlayerState object=" +
            std::to_string((uintptr_t)playerState) +
            " class=" +
            SafeName((UObject*)playerState->Class));

        Logger::Success(
            "Counselor selection 18K: RequestCounselorClass found | NewCounselorClass offset=" +
            std::to_string(newCounselorClassProperty->Offset_Internal) +
            " size=" +
            std::to_string(newCounselorClassProperty->ElementSize) +
            " | PickedCounselorClass offset=" +
            std::to_string(pickedProperty->Offset_Internal) +
            " size=" +
            std::to_string(pickedProperty->ElementSize));

        Logger::Debug(
            "Counselor selection 18K: PickedCounselorClass old=" +
            (oldPickedPath.empty() ?
                std::string("<empty>") :
                oldPickedPath));

        Logger::Success(
            "Counselor selection 18K: requested path=" +
            g_SelectedPlayerCounselorPath +
            " | native roster entry[" +
            std::to_string(index) +
            "]=" +
            kPlayerCounselorClassNames[index]);

        alignas(16) uint8_t params[0x100]{};

        memcpy(
            params +
                newCounselorClassProperty->Offset_Internal,
            g_SelectedPlayerCounselorSoftClass.data(),
            SoftClassSize);

        // Cache only after every reflected address has been validated.  The
        // raw monitor below performs no global scans and no ProcessEvent calls.
        g_CounselorPlayerState.store(
            (uintptr_t)playerState);

        g_PickedCounselorClassOffset.store(
            pickedProperty->Offset_Internal);

        g_CounselorSelectionWorld.store(
            (uintptr_t)Engine::GetWorld());

        bool callOK =
            SafeProcessEventCall(
                (uintptr_t)playerState,
                playerState,
                requestFunction,
                params);

        if (!callOK)
        {
            Logger::Error(
                "Counselor selection 18K: RequestCounselorClass call failed");
            return false;
        }

        g_CounselorRequestSubmittedAt.store(
            GetTickCount64());

        std::string newPickedPath =
            ReadSoftClassAssetPath(
                pickedSoftClass);

        Logger::Debug(
            "Counselor selection 18K: PickedCounselorClass new=" +
            (newPickedPath.empty() ?
                std::string("<empty>") :
                newPickedPath));

        if (newPickedPath ==
            g_SelectedPlayerCounselorPath)
        {
            g_CounselorPickedConfirmed.store(true);

            Logger::Success(
                "Counselor selection 18K SUCCESS: PickedCounselorClass=" +
                newPickedPath);
        }
        else
        {
            Logger::Debug(
                "Counselor selection 18K: stock request submitted; monitoring PickedCounselorClass without force-loading the generated UClass");
        }

        return true;
    }

    static void TryResolveCounselorSelectionSaveOnce()
    {
        if (g_SelectionSaveObject.load() ||
            g_CounselorSaveScanAttempted.exchange(true))
        {
            return;
        }

        if (!ResolveSelectionSaveMetadata() ||
            !g_SelectionSaveClass.load())
        {
            Logger::Debug(
                "Counselor selection 18K: selection-save metadata not ready; CounselorPick fallback will remain disabled");
            return;
        }

        Logger::Debug(
            "Counselor selection 18K: one-shot search for live SCCharacterSelectionsSaveGame");

        UObject* live =
            RawFindSelectionSaveObject(
                (UClass*)g_SelectionSaveClass.load());

        if (live)
        {
            g_SelectionSaveObject.store(
                (uintptr_t)live);

            Logger::Success(
                "Counselor selection 18K: live SCCharacterSelectionsSaveGame found");
        }
        else
        {
            Logger::Debug(
                "Counselor selection 18K: live SCCharacterSelectionsSaveGame not found in one-shot search; stock soft selection remains primary");
        }
    }

    static void TickCounselorSelection()
    {
        int stage =
            g_CounselorStage.load();

        if (stage !=
            (int)CounselorSelectStage::Monitoring)
        {
            return;
        }

        UWorld* world =
            Engine::GetWorld();

        uintptr_t originalWorld =
            g_CounselorSelectionWorld.load();

        if (!world ||
            !originalWorld ||
            (uintptr_t)world != originalWorld)
        {
            Logger::Success(
                "Counselor selection 18K: frontend world changed; counselor selection monitor released | softConfirmed=" +
                std::string(g_CounselorPickedConfirmed.load() ? "true" : "false") +
                " classResident=" +
                std::string(g_TargetPlayerCounselorClass.load() ? "true" : "false") +
                " CounselorPickFallback=" +
                std::string(g_CounselorFallbackApplied.load() ? "true" : "false"));

            g_CounselorStage.store(
                (int)CounselorSelectStage::Done);
            return;
        }

        UObject* playerState =
            (UObject*)g_CounselorPlayerState.load();

        int32_t pickedOffset =
            g_PickedCounselorClassOffset.load();

        if (!playerState ||
            !Memory::IsReadable(
                playerState,
                sizeof(UObject)) ||
            pickedOffset < 0)
        {
            Logger::Error(
                "Counselor selection 18K: cached PlayerState/PickedCounselorClass state lost");

            g_CounselorStage.store(
                (int)CounselorSelectStage::Failed);
            return;
        }

        uint8_t* pickedSoftClass =
            (uint8_t*)((uintptr_t)playerState +
                pickedOffset);

        if (!Memory::IsReadable(
                pickedSoftClass,
                SoftClassSize))
        {
            Logger::Error(
                "Counselor selection 18K: PickedCounselorClass storage became unreadable");

            g_CounselorStage.store(
                (int)CounselorSelectStage::Failed);
            return;
        }

        std::string pickedPath =
            ReadSoftClassAssetPath(
                pickedSoftClass);

        bool matches =
            !g_SelectedPlayerCounselorPath.empty() &&
            pickedPath ==
                g_SelectedPlayerCounselorPath;

        bool wasConfirmed =
            g_CounselorPickedConfirmed.load();

        if (matches &&
            !wasConfirmed)
        {
            g_CounselorPickedConfirmed.store(true);

            Logger::Success(
                "Counselor selection 18K SUCCESS: PickedCounselorClass=" +
                pickedPath);
        }
        else if (!matches &&
            wasConfirmed)
        {
            // The stock request succeeded earlier, so a later mismatch is an
            // actual frontend overwrite.  Keep the SOFT CLASS sticky by
            // restoring the exact native 40-byte TSoftClassPtr.  No UClass
            // load is forced and no ProcessEvent/global scan is repeated.
            memcpy(
                pickedSoftClass,
                g_SelectedPlayerCounselorSoftClass.data(),
                SoftClassSize);

            std::string restoredPath =
                ReadSoftClassAssetPath(
                    pickedSoftClass);

            uint32_t n =
                g_CounselorSoftStickyRewriteCount.fetch_add(1) + 1;

            if (n <= 12)
            {
                Logger::Debug(
                    "Counselor selection 18K sticky soft class: stock frontend changed PickedCounselorClass to " +
                    (pickedPath.empty() ?
                        std::string("<empty>") :
                        pickedPath) +
                    " -> restored " +
                    restoredPath);
            }
        }
        else if (!matches &&
            !wasConfirmed)
        {
            ULONGLONG requestedAt =
                g_CounselorRequestSubmittedAt.load();

            ULONGLONG now =
                GetTickCount64();

            if (requestedAt &&
                now - requestedAt >= 5000 &&
                !g_CounselorInitialMismatchLogged.exchange(true))
            {
                Logger::Debug(
                    "Counselor selection 18K: PickedCounselorClass has not matched after 5 seconds; continuing stock frontend without LoadAssetClass or CounselorPick force-write");
            }
        }

        // CounselorPick is only an optional fallback.  Locate the live save
        // object at most once, and only scan GObjects for the generated class
        // once after the stock soft selection has succeeded.  This is setup
        // work only; nothing is added to the frozen Jason AI tick.
        TryResolveCounselorSelectionSaveOnce();

        UObject* saveObject =
            (UObject*)g_SelectionSaveObject.load();

        int32_t counselorPickOffset =
            g_CounselorPickOffset.load();

        UClass** counselorPick = nullptr;

        if (saveObject &&
            counselorPickOffset >= 0 &&
            Memory::IsReadable(
                saveObject,
                sizeof(UObject)))
        {
            counselorPick =
                (UClass**)((uintptr_t)saveObject +
                    counselorPickOffset);

            if (!Memory::IsReadable(
                    counselorPick,
                    sizeof(UClass*)))
            {
                counselorPick = nullptr;
            }
        }

        int32_t index =
            ClampPlayerCounselorIndex(
                g_SelectedPlayerCounselorIndex.load());

        UClass* targetClass =
            (UClass*)g_TargetPlayerCounselorClass.load();

        // If the stock save updated itself, that pointer proves the generated
        // class is naturally resident and avoids any GObjects scan.
        if (!targetClass &&
            counselorPick &&
            *counselorPick &&
            SafeName((UObject*)*counselorPick) ==
                kPlayerCounselorClassNames[index])
        {
            targetClass =
                *counselorPick;

            g_TargetPlayerCounselorClass.store(
                (uintptr_t)targetClass);

            if (!g_CounselorStockPickLogged.exchange(true))
            {
                Logger::Success(
                    "Counselor selection 18K: stock CounselorPick automatically resolved to naturally loaded " +
                    SafeName((UObject*)targetClass));
            }
        }

        // One global class lookup only, after the primary soft-class request
        // is known-good.  If the class is still not resident, we simply let
        // the stock frontend continue and do not keep scanning or extend a
        // timeout.
        ULONGLONG counselorRequestAt =
            g_CounselorRequestSubmittedAt.load();

        ULONGLONG counselorNow =
            GetTickCount64();

        if (!targetClass &&
            g_CounselorPickedConfirmed.load() &&
            counselorRequestAt &&
            counselorNow - counselorRequestAt >= 1000 &&
            !g_CounselorNaturalClassScanDone.exchange(true))
        {
            UClass* found =
                FindClassExact(
                    kPlayerCounselorClassNames[index]);

            if (found &&
                Memory::IsReadable(
                    found,
                    sizeof(UClass)))
            {
                targetClass = found;

                g_TargetPlayerCounselorClass.store(
                    (uintptr_t)found);

                Logger::Success(
                    "Counselor selection 18K: selected generated counselor class is naturally resident: " +
                    SafeName((UObject*)found));
            }
            else
            {
                Logger::Debug(
                    "Counselor selection 18K: selected generated class is not resident yet; no force-load and no recurring class scan");
            }
        }

        // Only after stock PickedCounselorClass success AND natural UClass
        // residency may we use CounselorPick as a fallback.
        if (targetClass &&
            counselorPick &&
            g_CounselorPickedConfirmed.load())
        {
            UClass* current =
                *counselorPick;

            if (current == targetClass)
            {
                if (!g_CounselorStockPickLogged.exchange(true))
                {
                    Logger::Success(
                        "Counselor selection 18K: stock CounselorPick already matches selected counselor; no fallback write needed");
                }
            }
            else if (!g_CounselorFallbackApplied.load())
            {
                std::string before =
                    SafeName((UObject*)current);

                *counselorPick =
                    targetClass;

                if (*counselorPick == targetClass)
                {
                    g_CounselorFallbackApplied.store(true);

                    Logger::Success(
                        "Counselor selection 18K: naturally loaded class available but CounselorPick had not updated; fallback set CounselorPick from " +
                        before +
                        " to " +
                        SafeName((UObject*)targetClass));
                }
            }
            else if (current != targetClass)
            {
                std::string before =
                    SafeName((UObject*)current);

                *counselorPick =
                    targetClass;

                uint32_t n =
                    g_CounselorClassStickyRewriteCount.fetch_add(1) + 1;

                if (n <= 12)
                {
                    Logger::Debug(
                        "Counselor selection 18K sticky CounselorPick fallback: stock frontend changed CounselorPick to " +
                        before +
                        " -> restored " +
                        SafeName((UObject*)targetClass));
                }
            }
        }
    }

    static bool RequestSandboxSelectedCounselorOnGameThread()
    {
        APlayerController* controller = Engine::GetLocalPlayerController();
        if (!controller || !controller->Class ||
            controller->Class->GetName().find("Sandbox") == std::string::npos)
        {
            Logger::Debug("Counselor mode 18K character sync: Sandbox controller not ready yet");
            g_SandboxNextCharacterAt.store(GetTickCount64() + 750);
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Waiting);
            return true;
        }

        AActor* pawn = controller->AcknowledgedPawn;
        if (!pawn || !pawn->Class ||
            !Memory::IsReadable(pawn, sizeof(UObject)) ||
            !Memory::IsReadable(pawn->Class, sizeof(UObject)))
        {
            Logger::Debug("Counselor mode 18K character sync: acknowledged pawn not ready yet");
            g_SandboxNextCharacterAt.store(GetTickCount64() + 750);
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Waiting);
            return true;
        }

        int32_t index = ClampPlayerCounselorIndex(g_SelectedPlayerCounselorIndex.load());
        const std::string targetName = kPlayerCounselorClassNames[index];
        const std::string currentName = SafeName((UObject*)pawn->Class);

        g_SandboxSyncWorld.store((uintptr_t)Engine::GetWorld());

        if (currentName == targetName)
        {
            Logger::Success("Counselor mode 18K CHARACTER READY: actual pawn class=" + currentName);
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Done);
            return true;
        }

        UFunction* nextFunction =
            FindFunctionInHierarchyByName(controller->Class, "SERVER_RequestNextCharacter");
        if (!nextFunction)
        {
            Logger::Error("Counselor mode 18K character sync: SERVER_RequestNextCharacter not found");
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Failed);
            return false;
        }

        uint8_t params[8]{};
        bool ok = SafeProcessEventCall(
            (uintptr_t)controller,
            controller,
            nextFunction,
            params);

        uint32_t attempt = g_SandboxNextCharacterAttempts.fetch_add(1) + 1;
        Logger::Debug(
            "Counselor mode 18K character sync: actualPawn=" +
            (currentName.empty() ? std::string("<none>") : currentName) +
            " target=" + targetName +
            " -> SERVER_RequestNextCharacter attempt=" + std::to_string(attempt) +
            " call=" + std::string(ok ? "true" : "false"));

        if (!ok)
        {
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Failed);
            return false;
        }

        // Sandbox performs a real pawn replacement here.  18J called this every
        // 500 ms, which outran the game's replacement/possession work.  Give the
        // stock Sandbox controller time to finish one character change before
        // deciding whether another request is needed.
        g_SandboxNextCharacterAt.store(GetTickCount64() + 1500);
        g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Waiting);
        return true;
    }

    static void TickSandboxCounselorSync()
    {
        if (g_SandboxCounselorSyncStage.load() !=
            (int)SandboxCounselorSyncStage::Waiting)
            return;

        ULONGLONG now = GetTickCount64();
        ULONGLONG nextAt = g_SandboxNextCharacterAt.load();
        if (nextAt && now < nextAt)
            return;

        UWorld* world = Engine::GetWorld();
        uintptr_t originalWorld = g_SandboxSyncWorld.load();
        if (originalWorld && world && (uintptr_t)world != originalWorld)
        {
            Logger::Error("Counselor mode 18K character sync: Sandbox world changed before selected counselor was reached");
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Failed);
            return;
        }

        APlayerController* controller = Engine::GetLocalPlayerController();
        if (!controller || !controller->Class ||
            controller->Class->GetName().find("Sandbox") == std::string::npos)
        {
            g_SandboxNextCharacterAt.store(now + 750);
            return;
        }

        AActor* pawn = controller->AcknowledgedPawn;
        if (pawn && pawn->Class &&
            Memory::IsReadable(pawn, sizeof(UObject)) &&
            Memory::IsReadable(pawn->Class, sizeof(UObject)))
        {
            int32_t index = ClampPlayerCounselorIndex(g_SelectedPlayerCounselorIndex.load());
            std::string currentName = SafeName((UObject*)pawn->Class);
            if (currentName == kPlayerCounselorClassNames[index])
            {
                Logger::Success("Counselor mode 18K CHARACTER READY: actual pawn class=" + currentName);
                g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Done);
                return;
            }
        }

        if (g_SandboxNextCharacterAttempts.load() >= 20)
        {
            Logger::Error("Counselor mode 18K character sync: exhausted 20 stock Sandbox character-cycle requests");
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Failed);
            return;
        }

        g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::NeedRequest);
    }

    static bool DumpCounselorRosterOnGameThread()
    {
        NativeObjectItem* objects = nullptr;
        int32_t objectCount = 0;

        if (!GetGObjects(&objects, &objectCount))
        {
            Logger::Error(
                "Counselor discovery 18F: GObjects unavailable");
            return false;
        }

        UObject* kismetDefault = nullptr;
        UObject* offlineBotsGameModeDefault = nullptr;
        UObject* sandboxGameModeDefault = nullptr;
        UObject* baseGameModeDefault = nullptr;
        UClass* scWorldSettingsClass = nullptr;

        for (int32_t i = 0; i < objectCount; ++i)
        {
            UObject* obj = objects[i].Object;
            if (!obj || !Memory::IsReadable(obj, sizeof(UObject)))
                continue;

            std::string name = SafeName(obj);

            if (!kismetDefault && name == "Default__KismetSystemLibrary")
                kismetDefault = obj;
            else if (!offlineBotsGameModeDefault && name == "Default__SCGameMode_OfflineBots")
                offlineBotsGameModeDefault = obj;
            else if (!sandboxGameModeDefault && name == "Default__SCGameMode_Sandbox")
                sandboxGameModeDefault = obj;
            else if (!baseGameModeDefault && name == "Default__SCGameMode")
                baseGameModeDefault = obj;
            else if (!scWorldSettingsClass && name == "SCWorldSettings")
            {
                UObject* meta = (UObject*)obj->Class;
                if (meta && SafeName(meta) == "Class")
                    scWorldSettingsClass = (UClass*)obj;
            }
        }

        UObject* gameModeDefault =
            offlineBotsGameModeDefault ? offlineBotsGameModeDefault :
            (sandboxGameModeDefault ? sandboxGameModeDefault : baseGameModeDefault);

        if (!kismetDefault || !kismetDefault->Class ||
            !gameModeDefault || !gameModeDefault->Class)
        {
            Logger::Error(
                "Counselor discovery 18F: required Kismet/GameMode CDOs not ready");
            return false;
        }

        UFunction* convertFunction =
            FindFunctionInHierarchyByName(
                kismetDefault->Class,
                "Conv_SoftClassReferenceToClass");

        UPropertyLite* counselorClassesProperty =
            FindPropertyInHierarchyByName(
                gameModeDefault->Class,
                "CounselorCharacterClasses");

        if (!convertFunction || !counselorClassesProperty ||
            counselorClassesProperty->Offset_Internal <= 0 ||
            counselorClassesProperty->Offset_Internal >= 0x10000)
        {
            Logger::Error(
                "Counselor discovery 18F: converter or CounselorCharacterClasses not resolved");
            return false;
        }

        RawArray* counselorClasses =
            (RawArray*)((uintptr_t)gameModeDefault +
                counselorClassesProperty->Offset_Internal);

        if (!Memory::IsReadable(counselorClasses, sizeof(RawArray)) ||
            !counselorClasses->Data ||
            counselorClasses->Count <= 0 ||
            counselorClasses->Count > 64 ||
            counselorClasses->Max < counselorClasses->Count ||
            counselorClasses->Max > 128 ||
            !Memory::IsReadable(
                counselorClasses->Data,
                (size_t)counselorClasses->Count * SoftClassSize))
        {
            Logger::Error(
                "Counselor discovery 18F: CounselorCharacterClasses array unavailable/empty");
            return false;
        }

        Logger::Success(
            "========== COUNSELOR DISCOVERY 18F BEGIN ==========");
        Logger::Success(
            "Counselor discovery 18F: GameMode=" +
            SafeName(gameModeDefault) +
            " | native counselor entries=" +
            std::to_string(counselorClasses->Count));

        struct ResolveParams
        {
            uint8_t SoftClass[40];
            UClass* ReturnValue;
        };

        static_assert(
            sizeof(ResolveParams) == 48,
            "ResolveParams must be 48 bytes");

        int32_t resolvedCount = 0;

        for (int32_t index = 0;
            index < counselorClasses->Count;
            ++index)
        {
            uint8_t* softClass =
                counselorClasses->Data +
                ((uintptr_t)index * SoftClassSize);

            std::string path =
                ReadSoftClassAssetPath(softClass);

            ResolveParams params{};
            memcpy(params.SoftClass, softClass, SoftClassSize);

            bool resolveOK =
                SafeProcessEventCall(
                    (uintptr_t)kismetDefault,
                    kismetDefault,
                    convertFunction,
                    &params);

            std::string resolvedName =
                (resolveOK && params.ReturnValue &&
                    Memory::IsReadable(params.ReturnValue, sizeof(UClass)))
                ? SafeName((UObject*)params.ReturnValue)
                : std::string("<not-loaded>");

            if (resolvedName != "<not-loaded>")
                ++resolvedCount;

            Logger::Success(
                "COUNSELOR ROSTER [" +
                std::to_string(index) +
                "] path=" +
                (path.empty() ? std::string("<empty>") : path) +
                " | resolved=" + resolvedName);
        }

        Logger::Success(
            "Counselor discovery 18F: resolved=" +
            std::to_string(resolvedCount) +
            "/" +
            std::to_string(counselorClasses->Count));

        // Also enumerate any currently loaded SCWorldSettings-derived objects
        // and print their HeroCharacterClass soft reference.  This is read-only
        // discovery for the next selectable Hero step.
        int32_t heroWorldSettingsCount = 0;

        if (scWorldSettingsClass)
        {
            for (int32_t i = 0;
                i < objectCount && heroWorldSettingsCount < 32;
                ++i)
            {
                UObject* obj = objects[i].Object;
                if (!obj || !obj->Class ||
                    !Memory::IsReadable(obj, sizeof(UObject)) ||
                    !Memory::IsReadable(obj->Class, sizeof(UClass)) ||
                    !ClassIsOrDerivesFrom(obj->Class, scWorldSettingsClass))
                {
                    continue;
                }

                UPropertyLite* heroProperty =
                    FindPropertyInHierarchyByName(
                        obj->Class,
                        "HeroCharacterClass");

                if (!heroProperty ||
                    heroProperty->Offset_Internal <= 0 ||
                    heroProperty->Offset_Internal >= 0x10000)
                {
                    continue;
                }

                uint8_t* heroSoftClass =
                    (uint8_t*)((uintptr_t)obj +
                        heroProperty->Offset_Internal);

                std::string heroPath =
                    ReadSoftClassAssetPath(heroSoftClass);

                if (heroPath.empty())
                    continue;

                ResolveParams heroParams{};
                memcpy(heroParams.SoftClass, heroSoftClass, SoftClassSize);

                bool heroResolveOK =
                    SafeProcessEventCall(
                        (uintptr_t)kismetDefault,
                        kismetDefault,
                        convertFunction,
                        &heroParams);

                std::string heroResolved =
                    (heroResolveOK && heroParams.ReturnValue &&
                        Memory::IsReadable(heroParams.ReturnValue, sizeof(UClass)))
                    ? SafeName((UObject*)heroParams.ReturnValue)
                    : std::string("<not-loaded>");

                Logger::Success(
                    "HERO WORLDSETTINGS [" +
                    std::to_string(heroWorldSettingsCount) +
                    "] object=" +
                    SafeName(obj) +
                    " | class=" +
                    SafeName((UObject*)obj->Class) +
                    " | path=" + heroPath +
                    " | resolved=" + heroResolved);

                ++heroWorldSettingsCount;
            }
        }

        if (heroWorldSettingsCount == 0)
        {
            Logger::Debug(
                "Counselor discovery 18F: no non-empty HeroCharacterClass soft references found in loaded SCWorldSettings objects");
        }

        Logger::Success(
            "========== COUNSELOR DISCOVERY 18F END ==========");
        return true;
    }

    static bool ResolveJason5ClassOnGameThread()
    {
        // Prototype 17C proved the exact stock soft-class path:
        // /Game/Characters/Killers/Jason/J5/Jason_J5.Jason_J5_C
        //
        // LoadAssetClass did not materialize the generated class even after
        // 30 seconds.  The existing counselor-bot code already has a proven
        // path that resolves TSoftClassPtr entries synchronously:
        //
        //   KismetSystemLibrary::Conv_SoftClassReferenceToClass
        //
        // Use that exact stock conversion here instead of the latent loader.

        UObject* kismetDefault = nullptr;
        UObject* gameModeDefault = nullptr;

        // Resolve both required CDOs in ONE GObjects pass.  Prototype 17C
        // performed several full scans and spent ~20 seconds before it even
        // submitted the target request.
        NativeObjectItem* objects = nullptr;
        int32_t objectCount = 0;

        if (!GetGObjects(&objects, &objectCount))
        {
            Logger::Debug(
                "Jason resolve 18B: GObjects unavailable");
            return false;
        }

        for (int32_t i = 0; i < objectCount; ++i)
        {
            UObject* obj = objects[i].Object;
            if (!obj || !Memory::IsReadable(obj, sizeof(UObject)))
                continue;

            std::string name = SafeName(obj);

            if (!kismetDefault &&
                name == "Default__KismetSystemLibrary")
            {
                kismetDefault = obj;
            }

            if (!gameModeDefault)
            {
                if (name == "Default__SCGameMode_OfflineBots" ||
                    name == "Default__SCGameMode_Sandbox" ||
                    name == "Default__SCGameMode")
                {
                    gameModeDefault = obj;
                }
            }

            if (kismetDefault && gameModeDefault)
                break;
        }

        if (!kismetDefault || !kismetDefault->Class ||
            !gameModeDefault || !gameModeDefault->Class)
        {
            Logger::Debug(
                "Jason resolve 18B: required Kismet/GameMode CDOs not ready");
            return false;
        }

        UFunction* convertFunction =
            FindFunctionInHierarchyByName(
                kismetDefault->Class,
                "Conv_SoftClassReferenceToClass");

        UPropertyLite* killerClassesProperty =
            FindPropertyInHierarchyByName(
                gameModeDefault->Class,
                "KillerCharacterClasses");

        if (!convertFunction || !killerClassesProperty ||
            killerClassesProperty->Offset_Internal <= 0 ||
            killerClassesProperty->Offset_Internal >= 0x10000)
        {
            Logger::Debug(
                "Jason resolve 18B: converter or KillerCharacterClasses not resolved");
            return false;
        }

        RawArray* killerClasses =
            (RawArray*)((uintptr_t)gameModeDefault +
                killerClassesProperty->Offset_Internal);

        if (!Memory::IsReadable(killerClasses, sizeof(RawArray)) ||
            !killerClasses->Data ||
            killerClasses->Count <= 0 ||
            killerClasses->Count > 64 ||
            killerClasses->Max < killerClasses->Count ||
            killerClasses->Max > 128 ||
            !Memory::IsReadable(
                killerClasses->Data,
                (size_t)killerClasses->Count * SoftClassSize))
        {
            Logger::Debug(
                "Jason resolve 18B: KillerCharacterClasses array unavailable/empty");
            return false;
        }

        int32_t targetIndex =
            ClampJasonIndex(g_SelectedJasonIndex.load());

        if (targetIndex < 0 ||
            targetIndex >= killerClasses->Count)
        {
            Logger::Error(
                "Jason resolve 18B: selected Jason index is outside KillerCharacterClasses");
            return false;
        }

        uint8_t* selectedSoftClass =
            killerClasses->Data + ((uintptr_t)targetIndex * SoftClassSize);

        std::string targetPath =
            ReadSoftClassAssetPath(selectedSoftClass);

        if (targetPath.empty())
        {
            Logger::Error(
                "Jason resolve 18B: selected Jason soft-class path is empty");
            return false;
        }

        struct ResolveParams
        {
            uint8_t SoftClass[40];
            UClass* ReturnValue;
        };

        static_assert(
            sizeof(ResolveParams) == 48,
            "ResolveParams must be 48 bytes");

        ResolveParams params{};

        memcpy(
            params.SoftClass,
            selectedSoftClass,
            SoftClassSize);

        Logger::Debug(
            "Jason resolve 18B: converting entry[" +
            std::to_string(targetIndex) + "] " + targetPath);

        bool resolveOK =
            SafeProcessEventCall(
                (uintptr_t)kismetDefault,
                kismetDefault,
                convertFunction,
                &params);

        UClass* resolved = params.ReturnValue;

        if (!resolveOK ||
            !resolved ||
            !Memory::IsReadable(resolved, sizeof(UClass)))
        {
            Logger::Error(
                "Jason resolve 18B: Conv_SoftClassReferenceToClass returned null/invalid");
            return false;
        }

        std::string resolvedName = SafeName((UObject*)resolved);

        Logger::Debug(
            "Jason resolve 18B: converter returned " +
            (resolvedName.empty() ? std::string("<unnamed>") : resolvedName) +
            " @ " + std::to_string((uintptr_t)resolved));

        if (resolvedName.empty())
        {
            Logger::Error(
                "Jason resolve 18B: selected Jason converter returned unnamed class");
            return false;
        }

        g_TargetJasonClass.store((uintptr_t)resolved);

        Logger::Success(
            "Jason resolve 18B SUCCESS: selected Jason class resolved synchronously: " +
            resolvedName);
        return true;
    }

    static bool TryResolveTargetJasonClass()
    {
        return g_TargetJasonClass.load() != 0;
    }

    struct LifecycleRawArray
    {
        uint8_t* Data;
        int32_t Count;
        int32_t Max;
    };

    static constexpr int32_t LifecycleSoftClassSize = 40;

    static uintptr_t ShippingAddress(uintptr_t rva)
    {
        HMODULE module = GetModuleHandleW(nullptr);
        return module ? reinterpret_cast<uintptr_t>(module) + rva : 0;
    }

    static bool MatchesBytes(
        uintptr_t address,
        const uint8_t* expected,
        size_t size)
    {
        return address &&
            expected &&
            Memory::IsReadable(reinterpret_cast<void*>(address), size) &&
            std::memcmp(reinterpret_cast<void*>(address), expected, size) == 0;
    }

    static bool ValidateCounselorLifecycleNativeSurface()
    {
        static std::atomic<int> state{ 0 };
        const int current = state.load();
        if (current != 0)
            return current > 0;

        const uint8_t spawnSig[] =
        {
            0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x70,
            0x48,0x8B,0x05,0xB1,0x98,0xA7,0x01,0x48
        };
        const uint8_t paramsSig[] =
        {
            0x33,0xC0,0x48,0x89,0x01,0x48,0x89,0x41,
            0x08,0x48,0x89,0x41,0x10,0x48,0x89,0x41
        };
        const uint8_t possessSig[] =
        {
            0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x74,
            0x24,0x20,0x57,0x48,0x83,0xEC,0x40,0x33
        };
        const uint8_t activeSig[] =
        {
            0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,
            0xEC,0x50,0x33,0xC0,0x48,0x8B,0xDA,0x48
        };
        const uint8_t copySig[] =
        {
            0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,
            0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57
        };

        const bool valid =
            MatchesBytes(ShippingAddress(0x1519D10), spawnSig, sizeof(spawnSig)) &&
            MatchesBytes(ShippingAddress(0x17F1160), paramsSig, sizeof(paramsSig)) &&
            MatchesBytes(ShippingAddress(0x112F3F0), possessSig, sizeof(possessSig)) &&
            MatchesBytes(ShippingAddress(0x3B7B70), activeSig, sizeof(activeSig)) &&
            MatchesBytes(ShippingAddress(0x3B16D0), copySig, sizeof(copySig));

        state.store(valid ? 1 : -1);
        if (!valid)
        {
            Logger::Error(
                "18L-AD lifecycle native signature mismatch; counselor replacement is disabled");
        }
        return valid;
    }

    static const uint8_t* GetLifecycleSoftClass(
        UObject* gameMode,
        uintptr_t arrayOffset,
        int32_t index)
    {
        if (!gameMode || index < 0)
            return nullptr;

        LifecycleRawArray* classes =
            reinterpret_cast<LifecycleRawArray*>(
                reinterpret_cast<uintptr_t>(gameMode) + arrayOffset);

        if (!Memory::IsReadable(classes, sizeof(LifecycleRawArray)) ||
            !classes->Data ||
            classes->Count <= index ||
            classes->Count <= 0 ||
            classes->Count > 64 ||
            classes->Max < classes->Count ||
            classes->Max > 128 ||
            !Memory::IsReadable(
                classes->Data,
                static_cast<size_t>(classes->Count) * LifecycleSoftClassSize))
        {
            return nullptr;
        }

        return classes->Data +
            static_cast<uintptr_t>(index) * LifecycleSoftClassSize;
    }

    static int32_t GetLifecycleSoftClassCount(
        UObject* gameMode,
        uintptr_t arrayOffset)
    {
        if (!gameMode)
            return 0;

        LifecycleRawArray* classes =
            reinterpret_cast<LifecycleRawArray*>(
                reinterpret_cast<uintptr_t>(gameMode) + arrayOffset);

        if (!Memory::IsReadable(classes, sizeof(LifecycleRawArray)) ||
            !classes->Data ||
            classes->Count <= 0 ||
            classes->Count > 64 ||
            classes->Max < classes->Count ||
            classes->Max > 128)
        {
            return 0;
        }
        return classes->Count;
    }

    static UClass* LoadLifecycleSoftClass(const uint8_t* softClass)
    {
        if (!softClass ||
            !Memory::IsReadable(softClass, LifecycleSoftClassSize))
        {
            return nullptr;
        }

        using LoadSoftClassFn = UClass* (__fastcall*)(const void*);
        LoadSoftClassFn load = reinterpret_cast<LoadSoftClassFn>(
            ShippingAddress(0x2A6310));

        if (!load ||
            !Memory::IsReadable(reinterpret_cast<void*>(load), 1))
        {
            return nullptr;
        }

        return load(softClass);
    }

    static bool ClassDerivesFrom(UClass* cls, const char* baseName)
    {
        if (!cls || !baseName)
            return false;

        for (UStruct* current = reinterpret_cast<UStruct*>(cls);
            current;
            current = current->Super)
        {
            if (!Memory::IsReadable(current, sizeof(UStruct)))
                break;
            if (SafeName(reinterpret_cast<UObject*>(current)) == baseName)
                return true;
        }
        return false;
    }

    static bool ResolveNativeSelectedKiller(
        UObject* gameMode,
        const uint8_t** softClassOut,
        UClass** classOut)
    {
        if (!gameMode || !softClassOut || !classOut)
            return false;

        *softClassOut = nullptr;
        *classOut = nullptr;

        const int32_t killerCount =
            GetLifecycleSoftClassCount(gameMode, 0x6B0);
        if (killerCount <= 0)
            return false;

        UClass* nativePick = reinterpret_cast<UClass*>(
            g_TargetJasonClass.load());
        const std::string nativePickName =
            nativePick && Memory::IsReadable(nativePick, sizeof(UClass))
            ? SafeName(reinterpret_cast<UObject*>(nativePick))
            : std::string();

        if (!nativePickName.empty())
        {
            for (int32_t i = 0; i < killerCount; ++i)
            {
                const uint8_t* candidateSoft =
                    GetLifecycleSoftClass(gameMode, 0x6B0, i);
                UClass* candidateClass = LoadLifecycleSoftClass(candidateSoft);
                if (!candidateClass ||
                    !ClassDerivesFrom(candidateClass, "SCKillerCharacter"))
                {
                    continue;
                }

                if (candidateClass == nativePick ||
                    SafeName(reinterpret_cast<UObject*>(candidateClass)) ==
                        nativePickName)
                {
                    *softClassOut = candidateSoft;
                    *classOut = candidateClass;
                    Logger::Success(
                        "IN-GAME JASON SELECTION COMMITTED: " + nativePickName);
                    return true;
                }
            }
        }

        const int32_t fallbackIndex =
            ClampJasonIndex(g_SelectedJasonIndex.load()) % killerCount;
        const uint8_t* fallbackSoft =
            GetLifecycleSoftClass(gameMode, 0x6B0, fallbackIndex);
        UClass* fallbackClass = LoadLifecycleSoftClass(fallbackSoft);
        if (!fallbackClass ||
            !ClassDerivesFrom(fallbackClass, "SCKillerCharacter"))
        {
            return false;
        }

        *softClassOut = fallbackSoft;
        *classOut = fallbackClass;
        return true;
    }

    static bool ResolveNativeSelectedCounselor(
        UObject* gameMode,
        const uint8_t** softClassOut,
        UClass** classOut)
    {
        if (!gameMode || !softClassOut || !classOut)
            return false;

        *softClassOut = nullptr;
        *classOut = nullptr;

        UClass* nativePick = reinterpret_cast<UClass*>(
            g_TargetPlayerCounselorClass.load());
        const std::string nativePickName =
            nativePick && Memory::IsReadable(nativePick, sizeof(UClass))
            ? SafeName(reinterpret_cast<UObject*>(nativePick))
            : std::string();

        if (nativePickName == "Hunter_Counselor_C")
        {
            UPropertyLite* hunterProperty = FindPropertyInHierarchyByName(
                gameMode->Class,
                "HunterCharacterClass");
            if (hunterProperty &&
                hunterProperty->Offset_Internal > 0 &&
                hunterProperty->Offset_Internal < 0x10000 &&
                hunterProperty->ElementSize >= LifecycleSoftClassSize)
            {
                const uint8_t* hunterSoft = reinterpret_cast<const uint8_t*>(
                    reinterpret_cast<uintptr_t>(gameMode) +
                    hunterProperty->Offset_Internal);
                UClass* hunterClass = LoadLifecycleSoftClass(hunterSoft);
                if (hunterClass &&
                    ClassDerivesFrom(hunterClass, "SCCounselorCharacter"))
                {
                    *softClassOut = hunterSoft;
                    *classOut = hunterClass;
                    return true;
                }
            }
        }

        const int32_t counselorCount =
            GetLifecycleSoftClassCount(gameMode, 0x510);
        if (counselorCount <= 0)
            return false;

        if (!nativePickName.empty())
        {
            for (int32_t i = 0; i < counselorCount; ++i)
            {
                const uint8_t* candidateSoft =
                    GetLifecycleSoftClass(gameMode, 0x510, i);
                UClass* candidateClass = LoadLifecycleSoftClass(candidateSoft);
                if (!candidateClass ||
                    !ClassDerivesFrom(candidateClass, "SCCounselorCharacter"))
                {
                    continue;
                }

                if (candidateClass == nativePick ||
                    SafeName(reinterpret_cast<UObject*>(candidateClass)) ==
                        nativePickName)
                {
                    *softClassOut = candidateSoft;
                    *classOut = candidateClass;
                    Logger::Success(
                        "IN-GAME COUNSELOR SELECTION COMMITTED: " +
                        nativePickName);
                    return true;
                }
            }
        }

        const int32_t preferred =
            ClampPlayerCounselorIndex(
                g_SelectedPlayerCounselorIndex.load());
        for (int32_t i = 0; i < counselorCount; ++i)
        {
            const int32_t index = (preferred + i) % counselorCount;
            const uint8_t* candidateSoft =
                GetLifecycleSoftClass(gameMode, 0x510, index);
            UClass* candidateClass = LoadLifecycleSoftClass(candidateSoft);
            if (candidateClass &&
                ClassDerivesFrom(candidateClass, "SCCounselorCharacter"))
            {
                *softClassOut = candidateSoft;
                *classOut = candidateClass;
                return true;
            }
        }

        return false;
    }

    static bool GrantHunterStartingLoadout(
        UObject* gameMode,
        APawn* bornPawn)
    {
        if (!gameMode ||
            !bornPawn ||
            !Memory::IsReadable(gameMode, 0x6A0) ||
            !Memory::IsReadable(bornPawn, 0x1A19) ||
            !bornPawn->Class)
        {
            return false;
        }

        const std::string pawnClass =
            SafeName(reinterpret_cast<UObject*>(bornPawn->Class));
        const bool isHunter =
            *reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(bornPawn) + 0x1A18) != 0 ||
            pawnClass == "Hunter_Counselor_C";

        // Tommy/Hunter is the stock hero with this four-item starting kit.
        // Ordinary counselors and Hero_Counselor_C (the female hero class)
        // retain their own normal match inventory.
        if (!isHunter)
            return true;

        APlayerController* localController =
            Engine::GetLocalPlayerController();
        UObject* pawnController =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(bornPawn) + 0x3A0);
        UObject* pickingItem =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(bornPawn) + 0xF68);
        UObject* specialInventory =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(bornPawn) + 0x1520);
        UObject* smallInventory =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(bornPawn) + 0x1528);

        if (!localController ||
            pawnController != localController ||
            pickingItem ||
            !specialInventory ||
            !smallInventory ||
            !Memory::IsReadable(specialInventory, sizeof(UObject)) ||
            !Memory::IsReadable(smallInventory, sizeof(UObject)))
        {
            Logger::Error(
                "18L-AH HERO LOADOUT: born Hunter is not ready for the stock grant path");
            return false;
        }

        UClass* shotgunClass = LoadLifecycleSoftClass(
            reinterpret_cast<uint8_t*>(bornPawn) + 0x1450);
        UClass* mapClass = LoadLifecycleSoftClass(
            reinterpret_cast<uint8_t*>(gameMode) + 0x5E8);
        UClass* sprayClass = LoadLifecycleSoftClass(
            reinterpret_cast<uint8_t*>(gameMode) + 0x660);
        UClass* pocketKnifeClass = LoadLifecycleSoftClass(
            reinterpret_cast<uint8_t*>(gameMode) + 0x638);

        UClass* itemClasses[] =
        {
            shotgunClass,
            mapClass,
            sprayClass,
            pocketKnifeClass
        };
        const char* itemLabels[] =
        {
            "Shotgun",
            "Map",
            "HealthSpray",
            "PocketKnife"
        };
        const bool smallItems[] =
        {
            false,
            false,
            true,
            true
        };

        for (UClass* itemClass : itemClasses)
        {
            if (!itemClass ||
                !Memory::IsReadable(itemClass, sizeof(UClass)) ||
                !ClassDerivesFrom(itemClass, "SCItem"))
            {
                Logger::Error(
                    "18L-AH HERO LOADOUT: a stock item soft class did not resolve as SCItem");
                return false;
            }
        }

        using GiveStartingItemFn = void(__fastcall*)(APawn*, UClass*);
        using IsSmallInventoryFullFn = bool(__fastcall*)(APawn*);
        using CountItemFn = int32_t(__fastcall*)(APawn*, UClass*);

        GiveStartingItemFn giveStartingItem =
            reinterpret_cast<GiveStartingItemFn>(ShippingAddress(0x2EF980));
        IsSmallInventoryFullFn isSmallInventoryFull =
            reinterpret_cast<IsSmallInventoryFullFn>(
                ShippingAddress(0x3D4060));

        uintptr_t* pawnVtable =
            *reinterpret_cast<uintptr_t**>(bornPawn);
        CountItemFn countItem = nullptr;
        if (pawnVtable &&
            Memory::IsReadable(
                pawnVtable,
                0xE98 + sizeof(uintptr_t)))
        {
            countItem = reinterpret_cast<CountItemFn>(
                pawnVtable[0xE98 / sizeof(uintptr_t)]);
        }

        if (!giveStartingItem ||
            !isSmallInventoryFull ||
            !countItem ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(giveStartingItem), 1) ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(isSmallInventoryFull), 1) ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(countItem), 1))
        {
            Logger::Error(
                "18L-AH HERO LOADOUT: verified stock inventory functions are unavailable");
            return false;
        }

        bool complete = true;
        std::string counts;
        for (int32_t i = 0; i < 4; ++i)
        {
            int32_t count = countItem(bornPawn, itemClasses[i]);
            if (count <= 0)
            {
                if (smallItems[i] && isSmallInventoryFull(bornPawn))
                {
                    Logger::Error(
                        std::string("18L-AH HERO LOADOUT: stock small inventory unexpectedly full before ") +
                        itemLabels[i]);
                    complete = false;
                }
                else
                {
                    // This is the exact native helper used by stock Hunter
                    // spawn: spawn, mark picked, and route through
                    // AddOrSwapPickingItem.  No inventory arrays are patched.
                    giveStartingItem(bornPawn, itemClasses[i]);
                    count = countItem(bornPawn, itemClasses[i]);
                    if (count <= 0)
                        complete = false;
                }
            }

            if (!counts.empty())
                counts += ",";
            counts += std::string(itemLabels[i]) + "=" +
                std::to_string(count);
        }

        if (complete)
        {
            Logger::Success(
                "18L-AH HERO LOADOUT COMPLETE: stock Hunter grant path | " +
                counts + " | no refill after consumption");
        }
        else
        {
            Logger::Error(
                "18L-AH HERO LOADOUT INCOMPLETE: " + counts);
        }
        return complete;
    }

    static bool SetLifecycleActiveCharacter(
        UObject* playerState,
        const uint8_t* borrowedSoftClass)
    {
        if (!playerState ||
            !borrowedSoftClass ||
            !Memory::IsReadable(playerState, sizeof(UObject)) ||
            !Memory::IsReadable(borrowedSoftClass, LifecycleSoftClassSize))
        {
            return false;
        }

        using CopySoftClassFn = void* (__fastcall*)(void*, const void*);
        using SetActiveFn = void(__fastcall*)(UObject*, void*);

        CopySoftClassFn copy = reinterpret_cast<CopySoftClassFn>(
            ShippingAddress(0x3B16D0));
        SetActiveFn setActive = reinterpret_cast<SetActiveFn>(
            ShippingAddress(0x3B7B70));

        if (!copy || !setActive)
            return false;

        alignas(16) uint8_t ownedSoftClass[LifecycleSoftClassSize]{};
        copy(ownedSoftClass, borrowedSoftClass);
        setActive(playerState, ownedSoftClass);
        return true;
    }

    static void ApplyLifecycleKillerCosmetics(
        UObject* playerState,
        const uint8_t* borrowedSoftClass)
    {
        if (!playerState || !borrowedSoftClass)
            return;

        using CosmeticRpcFn = void(__fastcall*)(UObject*, const void*);
        CosmeticRpcFn grabKills = reinterpret_cast<CosmeticRpcFn>(
            ShippingAddress(0x4DEF90));
        CosmeticRpcFn weapon = reinterpret_cast<CosmeticRpcFn>(
            ShippingAddress(0x4DF0B0));

        if (grabKills)
            grabKills(playerState, borrowedSoftClass);
        if (weapon)
            weapon(playerState, borrowedSoftClass);
    }

    static bool CopyLifecycleKillerPresentation(
        UObject* sourcePlayerState,
        UObject* destinationPlayerState)
    {
        if (!sourcePlayerState ||
            !destinationPlayerState ||
            !Memory::IsReadable(sourcePlayerState, 0x7E0) ||
            !Memory::IsReadable(destinationPlayerState, 0x7E0))
        {
            return false;
        }

        // SCPlayerState layout and setters verified in this Resurrected EXE:
        //   +0x7A0 PickedKillerSkin (SCJasonSkin class)
        //   +0x7B8 PickedKillerWeapon (40-byte TSoftClassPtr<SCWeapon>)
        // PlayOutro requires both values on GameState.KillerPlayerState.  An
        // AI PlayerState cannot answer CLIENT_RequestLoadPlayerSettings, so
        // transferring ownership before these fields are complete leaves the
        // game in PostMatchOutro forever.
        UClass* skin = *reinterpret_cast<UClass**>(
            reinterpret_cast<uintptr_t>(sourcePlayerState) + 0x7A0);
        const uint8_t* weapon = reinterpret_cast<const uint8_t*>(
            reinterpret_cast<uintptr_t>(sourcePlayerState) + 0x7B8);
        const std::string weaponPath = ReadSoftClassAssetPath(weapon);

        if (!skin ||
            !Memory::IsReadable(skin, sizeof(UClass)) ||
            weaponPath.empty())
        {
            Logger::Error(
                "18L-AK OUTRO METADATA: human intro owner has no selected Jason skin/weapon to copy");
            return false;
        }

        using SetPickedKillerSkinFn =
            void(__fastcall*)(UObject*, UClass*);
        using SetPickedKillerWeaponFn =
            void(__fastcall*)(UObject*, const void*);

        SetPickedKillerSkinFn setSkin =
            reinterpret_cast<SetPickedKillerSkinFn>(
                ShippingAddress(0x3E8FE0));
        SetPickedKillerWeaponFn setWeapon =
            reinterpret_cast<SetPickedKillerWeaponFn>(
                ShippingAddress(0x3E9100));
        if (!setSkin ||
            !setWeapon ||
            !Memory::IsReadable(reinterpret_cast<void*>(setSkin), 1) ||
            !Memory::IsReadable(reinterpret_cast<void*>(setWeapon), 1))
        {
            Logger::Error(
                "18L-AK OUTRO METADATA: native Jason skin/weapon setters are unavailable");
            return false;
        }

        // Skin must be committed first.  The weapon setter deep-copies its
        // soft path and notifies WorldSettings::SettingsLoaded, which is also
        // the stock retry path used by PlayOutro.
        setSkin(destinationPlayerState, skin);
        setWeapon(destinationPlayerState, weapon);

        UClass* committedSkin = *reinterpret_cast<UClass**>(
            reinterpret_cast<uintptr_t>(destinationPlayerState) + 0x7A0);
        const uint8_t* committedWeapon = reinterpret_cast<const uint8_t*>(
            reinterpret_cast<uintptr_t>(destinationPlayerState) + 0x7B8);
        const std::string committedWeaponPath =
            ReadSoftClassAssetPath(committedWeapon);
        const bool complete =
            committedSkin == skin &&
            committedWeaponPath == weaponPath;

        if (complete)
        {
            Logger::Success(
                "18L-AK OUTRO METADATA READY: AI Jason PlayerState skin=" +
                SafeName(reinterpret_cast<UObject*>(skin)) +
                " | weapon=" + weaponPath);
        }
        else
        {
            Logger::Error(
                "18L-AK OUTRO METADATA INCOMPLETE: refusing an AI killer-owner handoff that would stall PostMatchOutro");
        }
        return complete;
    }

    static bool DeepAssignLifecycleSoftClass(
        uint8_t* destination,
        const uint8_t* source)
    {
        if (!destination ||
            !source ||
            !Memory::IsReadable(destination, LifecycleSoftClassSize) ||
            !Memory::IsReadable(source, LifecycleSoftClassSize))
        {
            return false;
        }

        using AssignSoftPathFn = void(__fastcall*)(void*, const void*);
        AssignSoftPathFn assignPath = reinterpret_cast<AssignSoftPathFn>(
            ShippingAddress(0x78DCA0));
        if (!assignPath)
            return false;

        std::memcpy(destination, source, 12);
        assignPath(destination + 0x10, source + 0x10);
        return true;
    }

    static AActor* SpawnLifecycleActor(
        UWorld* world,
        UClass* actorClass,
        const FVector* location,
        const FRotator* rotation,
        UObject* owner,
        uint8_t collisionMode)
    {
        if (!world || !actorClass)
            return nullptr;

        using SpawnParamsCtorFn = void* (__fastcall*)(void*);
        using SpawnActorFn = AActor* (__fastcall*)(
            UWorld*, UClass*, const FVector*, const FRotator*, const void*);

        SpawnParamsCtorFn construct = reinterpret_cast<SpawnParamsCtorFn>(
            ShippingAddress(0x17F1160));
        SpawnActorFn spawn = reinterpret_cast<SpawnActorFn>(
            ShippingAddress(0x1519D10));
        if (!construct || !spawn)
            return nullptr;

        alignas(16) uint8_t parameters[0x30]{};
        construct(parameters);
        *reinterpret_cast<UObject**>(parameters + 0x10) = owner;
        parameters[0x28] = collisionMode;

        return spawn(world, actorClass, location, rotation, parameters);
    }

    static UObject* GetControllerPlayerState(UObject* controller)
    {
        if (!controller || !Memory::IsReadable(controller, 0x390))
            return nullptr;
        return *reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(controller) + 0x388);
    }

    static bool RestartLifecyclePlayer(UObject* gameMode, UObject* controller)
    {
        using RestartFn = void(__fastcall*)(UObject*, UObject*);
        RestartFn restart = reinterpret_cast<RestartFn>(
            ShippingAddress(0x3A4B70));
        if (!restart || !gameMode || !controller)
            return false;
        restart(gameMode, controller);
        return true;
    }

    static bool SpawnNativeCounselorBots(
        UObject* gameMode,
        UClass* humanCounselorClass)
    {
        using ClassGetterFn = UClass* (__fastcall*)();
        using HasFullyTraveledFn = void(__fastcall*)(UObject*, bool);

        ClassGetterFn controllerClassGetter =
            reinterpret_cast<ClassGetterFn>(ShippingAddress(0x48E5C0));
        HasFullyTraveledFn fullyTraveled =
            reinterpret_cast<HasFullyTraveledFn>(ShippingAddress(0x2A3A60));

        UClass* controllerClass = controllerClassGetter
            ? controllerClassGetter()
            : nullptr;
        UWorld* world = Engine::GetWorld();
        const int32_t classCount =
            GetLifecycleSoftClassCount(gameMode, 0x510);
        const int32_t requested =
            GetRequestedCounselorBotCount();

        if (!world || !controllerClass || classCount <= 0)
            return false;

        int32_t created = 0;
        const int32_t start = static_cast<int32_t>(GetTickCount64() % classCount);

        for (int32_t candidate = 0;
            candidate < classCount && created < requested;
            ++candidate)
        {
            const int32_t index = (start + candidate) % classCount;
            const uint8_t* softClass =
                GetLifecycleSoftClass(gameMode, 0x510, index);
            UClass* counselorClass = LoadLifecycleSoftClass(softClass);

            if (!counselorClass ||
                counselorClass == humanCounselorClass ||
                !ClassDerivesFrom(counselorClass, "SCCounselorCharacter"))
            {
                continue;
            }

            AActor* aiController = SpawnLifecycleActor(
                world,
                controllerClass,
                nullptr,
                nullptr,
                gameMode,
                1);
            UObject* playerState = GetControllerPlayerState(aiController);

            if (!aiController ||
                !playerState ||
                !SetLifecycleActiveCharacter(playerState, softClass) ||
                !RestartLifecyclePlayer(gameMode, aiController))
            {
                Logger::Error(
                    "18L-AD PREMATCH: native counselor bot creation failed at roster index " +
                    std::to_string(index));
                continue;
            }

            if (fullyTraveled)
                fullyTraveled(playerState, true);

            int32_t* numBots = reinterpret_cast<int32_t*>(
                reinterpret_cast<uintptr_t>(gameMode) + 0x40C);
            if (Memory::IsReadable(numBots, sizeof(int32_t)))
                ++(*numBots);

            ++created;
            Logger::Success(
                "18L-AD PREMATCH: native counselor bot born | class=" +
                SafeName(reinterpret_cast<UObject*>(counselorClass)) +
                " | count=" + std::to_string(created) + "/" +
                std::to_string(requested));
        }

        g_CounselorBotsCreated.store(created);
        return created == requested;
    }

    static bool GetActorStartTransform(
        AActor* actor,
        FVector& location,
        FRotator& rotation)
    {
        if (!actor || !actor->Class)
            return false;

        UFunction* getLocation = FindFunctionInHierarchyByName(
            actor->Class, "K2_GetActorLocation");
        UFunction* getRotation = FindFunctionInHierarchyByName(
            actor->Class, "K2_GetActorRotation");
        if (!getLocation || !getRotation)
            return false;

        struct LocationParams { FVector ReturnValue; } locationParams{};
        struct RotationParams { FRotator ReturnValue; } rotationParams{};

        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(actor),
                actor,
                getLocation,
                &locationParams) ||
            !SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(actor),
                actor,
                getRotation,
                &rotationParams))
        {
            return false;
        }

        location = locationParams.ReturnValue;
        rotation = rotationParams.ReturnValue;
        return true;
    }

    static AActor* FindKillerPlayerStart(UWorld* world)
    {
        if (!world)
            return nullptr;

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
            return nullptr;
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

            for (int32_t actorIndex = 0; actorIndex < actors.Count; ++actorIndex)
            {
                AActor* actor = actors.Data[actorIndex];
                if (actor &&
                    Memory::IsReadable(actor, sizeof(UObject)) &&
                    actor->Class &&
                    ClassDerivesFrom(actor->Class, "SCKillerPlayerStart"))
                {
                    return actor;
                }
            }
        }
        return nullptr;
    }

    static AActor* SpawnKillerControllerWithStockCrowdBypass(
        UWorld* world,
        UClass* controllerClass)
    {
        uint8_t* crowdCall = reinterpret_cast<uint8_t*>(
            ShippingAddress(0x2FE340));
        const uint8_t expected[5] = { 0xE8,0xEB,0x58,0xF9,0xFF };

        if (!MatchesBytes(
                reinterpret_cast<uintptr_t>(crowdCall),
                expected,
                sizeof(expected)))
        {
            Logger::Error(
                "18L-AD AI Jason: SCCrowdFollowing constructor signature mismatch");
            return nullptr;
        }

        DWORD oldProtection = 0;
        if (!VirtualProtect(
                crowdCall,
                sizeof(expected),
                PAGE_EXECUTE_READWRITE,
                &oldProtection))
        {
            return nullptr;
        }

        std::memset(crowdCall, 0x90, sizeof(expected));
        FlushInstructionCache(GetCurrentProcess(), crowdCall, sizeof(expected));

        FVector zero{};
        FRotator zeroRotation{};
        AActor* controller = SpawnLifecycleActor(
            world,
            controllerClass,
            &zero,
            &zeroRotation,
            nullptr,
            2);

        std::memcpy(crowdCall, expected, sizeof(expected));
        FlushInstructionCache(GetCurrentProcess(), crowdCall, sizeof(expected));
        DWORD unusedProtection = 0;
        VirtualProtect(
            crowdCall,
            sizeof(expected),
            oldProtection,
            &unusedProtection);

        return controller;
    }
}

namespace OfflineSetup
{
    bool SetMapIndex(int32_t value)
    {
        g_SelectedMapIndex.store(ClampMapIndex(value));
        return true;
    }

    bool SetJasonIndex(int32_t value)
    {
        g_SelectedJasonIndex.store(ClampJasonIndex(value));
        return true;
    }

    bool SetPlayerCounselorIndex(int32_t value)
    {
        g_SelectedPlayerCounselorIndex.store(
            ClampPlayerCounselorIndex(value));
        return true;
    }

    bool SetDifficulty(int32_t value)
    {
        g_SelectedDifficulty.store(ClampDifficulty(value));
        return true;
    }

    bool SetCounselorCount(int32_t value)
    {
        g_SelectedCounselorCount.store(ClampCounselorCount(value));
        return true;
    }

    bool SetWeather(int32_t value)
    {
        g_SelectedWeather.store(ClampWeather(value));
        return true;
    }

    bool QueueArmSelectedSetup()
    {
        if (g_ArmPresetRequested.exchange(true))
        {
            Logger::Debug(
                "Counselor mode 18K: setup arm already queued");
            return false;
        }

        Logger::Debug(
            "Counselor mode 18K: selected map + Sandbox counselor mode arm queued");
        return true;
    }

    bool QueueApplyGameSetupPreset()
    {
        if (g_ApplyGameSetupRequested.exchange(true))
        {
            Logger::Debug(
                "Unified setup 18B: Game Setup apply already queued");
            return false;
        }

        Logger::Debug(
            "Unified setup 18B: selected Game Setup apply queued");
        return true;
    }

    bool QueueSelectedCounselor()
    {
        int stage =
            g_CounselorStage.load();

        if (stage != (int)CounselorSelectStage::Idle &&
            stage != (int)CounselorSelectStage::Done &&
            stage != (int)CounselorSelectStage::Failed)
        {
            Logger::Debug(
                "Counselor selection 18K: request already active");
            return false;
        }

        g_TargetPlayerCounselorClass.store(0);
        g_CounselorSelectionWorld.store(0);
        g_CounselorPlayerState.store(0);
        g_PickedCounselorClassOffset.store(-1);
        g_CounselorRequestSubmittedAt.store(0);
        g_CounselorPickedConfirmed.store(false);
        g_CounselorInitialMismatchLogged.store(false);
        g_CounselorNaturalClassScanDone.store(false);
        g_CounselorSaveScanAttempted.store(false);
        g_CounselorFallbackApplied.store(false);
        g_CounselorStockPickLogged.store(false);
        g_CounselorSoftStickyRewriteCount.store(0);
        g_CounselorClassStickyRewriteCount.store(0);
        g_SelectedPlayerCounselorSoftClass.fill(0);
        g_SelectedPlayerCounselorPath.clear();

        g_CounselorStage.store(
            (int)CounselorSelectStage::NeedRequest);

        int32_t index =
            ClampPlayerCounselorIndex(
                g_SelectedPlayerCounselorIndex.load());

        Logger::Debug(
            "Counselor selection 18K: selected counselor queued | index=" +
            std::to_string(index) +
            " | class=" +
            kPlayerCounselorClassNames[index] +
            " | next=SCPlayerState::RequestCounselorClass");

        return true;
    }

    bool QueueSelectedJason()
    {
        int expected = (int)SetupStage::Idle;
        if (!g_Stage.compare_exchange_strong(
            expected,
            (int)SetupStage::NeedPreload))
        {
            if (expected == (int)SetupStage::Done ||
                expected == (int)SetupStage::Failed)
            {
                g_SelectionSaveObject.store(0);
                g_TargetJasonClass.store(0);
                g_LastTargetScanAt.store(0);
                g_PreloadAt.store(0);
                g_SelectionWorld.store(0);
                g_StickyRewriteCount.store(0);
                g_BackgroundScanStarted.store(false);
                g_Stage.store((int)SetupStage::NeedPreload);
            }
            else
            {
                Logger::Debug(
                    "Jason resolve 18B request already active");
                return false;
            }
        }

        Logger::Debug(
            "Jason resolve 18B: selected Jason queued for game-thread conversion | index=" +
            std::to_string(ClampJasonIndex(g_SelectedJasonIndex.load())));
        return true;
    }

    bool QueueDumpCounselorRoster()
    {
        bool expected = false;
        if (!g_DumpCounselorRosterRequested.compare_exchange_strong(
            expected,
            true))
        {
            Logger::Debug(
                "Counselor discovery 18F: roster dump already queued");
            return false;
        }

        Logger::Debug(
            "Counselor discovery 18F: roster dump queued");
        return true;
    }

    bool QueueSandboxCounselorSync()
    {
        int stage = g_SandboxCounselorSyncStage.load();
        if (stage == (int)SandboxCounselorSyncStage::NeedRequest ||
            stage == (int)SandboxCounselorSyncStage::Waiting)
            return true;

        g_SandboxSyncWorld.store(0);
        g_SandboxSyncPlayerState.store(0);
        g_SandboxSpawnedClassOffset.store(-1);
        g_SandboxNextCharacterAttempts.store(0);
        g_SandboxNextCharacterAt.store(0);
        g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::NeedRequest);

        int32_t index = ClampPlayerCounselorIndex(g_SelectedPlayerCounselorIndex.load());
        Logger::Debug(
            "Counselor mode 18K: Sandbox local-character sync queued | target=" +
            std::string(kPlayerCounselorClassNames[index]));
        return true;
    }

    bool IsCounselorMenuRouteLatched()
    {
        return g_CounselorMenuRouteLatched.load();
    }

    static bool BirthSelectedCounselorForPreMatchLegacy(void* gameModeValue)
    {
        if (!g_CounselorMenuRouteLatched.load())
            return false;

        if (g_CounselorBirthComplete.load())
            return true;

        UObject* gameMode = reinterpret_cast<UObject*>(gameModeValue);
        UWorld* world = Engine::GetWorld();
        APlayerController* controller = Engine::GetLocalPlayerController();

        if (!gameMode ||
            !Memory::IsReadable(gameMode, sizeof(UObject)) ||
            !gameMode->Class ||
            !world ||
            !controller ||
            !Memory::IsReadable(controller, sizeof(UObject)) ||
            !controller->Class)
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: GameMode/world/local controller is unavailable");
            return false;
        }

        const std::string gameModeClass = SafeName((UObject*)gameMode->Class);
        const std::string controllerClass = SafeName((UObject*)controller->Class);

        if (gameModeClass != "SCGameMode_OfflineBots" ||
            controllerClass.find("OfflineBots") == std::string::npos)
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: unexpected infrastructure | GameMode=" +
                gameModeClass + " | Controller=" + controllerClass);
            return false;
        }

        const int32_t counselorIndex =
            ClampPlayerCounselorIndex(
                g_SelectedPlayerCounselorIndex.load());

        UClass* counselorClass = reinterpret_cast<UClass*>(
            g_TargetPlayerCounselorClass.load());

        if (!counselorClass ||
            !Memory::IsReadable(counselorClass, sizeof(UClass)) ||
            SafeName((UObject*)counselorClass) !=
                kPlayerCounselorClassNames[counselorIndex])
        {
            counselorClass = FindClassExact(
                kPlayerCounselorClassNames[counselorIndex]);
        }

        // Tommy and uncached roster entries may still be represented only by
        // the 40-byte soft class selected in the frontend. Resolve that exact
        // native value synchronously before RestartPlayer.
        if ((!counselorClass ||
                !Memory::IsReadable(counselorClass, sizeof(UClass))) &&
            !g_SelectedPlayerCounselorPath.empty())
        {
            UClass* kismetClass = FindClassExact("KismetSystemLibrary");
            UFunction* convertFunction = kismetClass
                ? FindFunctionInHierarchyByName(
                    kismetClass,
                    "Conv_SoftClassReferenceToClass")
                : nullptr;

            if (kismetClass &&
                kismetClass->DefaultObject &&
                convertFunction)
            {
                struct ResolveParams
                {
                    uint8_t SoftClass[SoftClassSize];
                    UClass* ReturnValue;
                };

                static_assert(sizeof(ResolveParams) == 48,
                    "Counselor ResolveParams must be 48 bytes");

                ResolveParams params{};
                memcpy(
                    params.SoftClass,
                    g_SelectedPlayerCounselorSoftClass.data(),
                    SoftClassSize);

                if (SafeProcessEventCall(
                        (uintptr_t)kismetClass->DefaultObject,
                        kismetClass->DefaultObject,
                        convertFunction,
                        &params))
                {
                    counselorClass = params.ReturnValue;
                }
            }
        }

        if (!counselorClass ||
            !Memory::IsReadable(counselorClass, sizeof(UClass)))
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: selected counselor class is not resident/resolvable | target=" +
                std::string(kPlayerCounselorClassNames[counselorIndex]));
            return false;
        }

        g_TargetPlayerCounselorClass.store((uintptr_t)counselorClass);

        // Match the donor's HandlePreMatchIntro ordering: make the selected
        // counselor the pawn class first, then RestartPlayer while no human
        // Jason pawn has ever been created.
        UPropertyLite* defaultPawnProperty =
            FindPropertyInHierarchyByName(
                gameMode->Class,
                "DefaultPawnClass");

        if (!defaultPawnProperty ||
            defaultPawnProperty->Offset_Internal <= 0 ||
            defaultPawnProperty->Offset_Internal >= 0x10000 ||
            defaultPawnProperty->ElementSize < (int32_t)sizeof(UClass*))
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: DefaultPawnClass property is unavailable");
            return false;
        }

        UClass** defaultPawnSlot = reinterpret_cast<UClass**>(
            reinterpret_cast<uintptr_t>(gameMode) +
            defaultPawnProperty->Offset_Internal);

        if (!Memory::IsReadable(defaultPawnSlot, sizeof(UClass*)))
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: DefaultPawnClass slot is unreadable");
            return false;
        }

        *defaultPawnSlot = counselorClass;

        UPropertyLite* playerStateProperty =
            FindPropertyInHierarchyByName(
                controller->Class,
                "PlayerState");

        UObject* playerState = nullptr;

        if (playerStateProperty &&
            playerStateProperty->Offset_Internal > 0 &&
            playerStateProperty->Offset_Internal < 0x10000)
        {
            UObject** playerStateSlot = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(controller) +
                playerStateProperty->Offset_Internal);

            if (Memory::IsReadable(playerStateSlot, sizeof(UObject*)))
                playerState = *playerStateSlot;
        }

        if (!playerState ||
            !Memory::IsReadable(playerState, sizeof(UObject)) ||
            !playerState->Class)
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: local SCPlayerState_OfflineBots is unavailable");
            return false;
        }

        auto writeSelectedSoftClass =
            [&](const char* propertyName) -> bool
        {
            UPropertyLite* property =
                FindPropertyInHierarchyByName(
                    playerState->Class,
                    propertyName);

            if (!property ||
                property->Offset_Internal <= 0 ||
                property->Offset_Internal >= 0x10000 ||
                property->ElementSize != SoftClassSize)
            {
                return false;
            }

            uint8_t* destination = reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(playerState) +
                property->Offset_Internal);

            if (!Memory::IsReadable(destination, SoftClassSize))
                return false;

            memcpy(
                destination,
                g_SelectedPlayerCounselorSoftClass.data(),
                SoftClassSize);
            return true;
        };

        const bool pickedWritten =
            writeSelectedSoftClass("PickedCounselorClass");
        const bool activeWritten =
            writeSelectedSoftClass("ActiveCharacterClass");

        UFunction* restartPlayer =
            FindFunctionInHierarchyByName(
                gameMode->Class,
                "RestartPlayer");

        if (!restartPlayer)
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: RestartPlayer UFunction is unavailable");
            return false;
        }

        alignas(16) uint8_t restartParams[0x40]{};
        int32_t newPlayerOffset = 0;

        for (UField* field = restartPlayer->Children;
            field;
            field = field->Next)
        {
            if (!Memory::IsReadable(field, sizeof(UField)))
                break;

            if (SafeName((UObject*)field) == "NewPlayer")
            {
                UPropertyLite* property =
                    reinterpret_cast<UPropertyLite*>(field);

                if (Memory::IsReadable(property, sizeof(UPropertyLite)) &&
                    property->Offset_Internal >= 0 &&
                    property->Offset_Internal <=
                        (int32_t)(sizeof(restartParams) - sizeof(void*)))
                {
                    newPlayerOffset = property->Offset_Internal;
                }
                break;
            }
        }

        memcpy(
            restartParams + newPlayerOffset,
            &controller,
            sizeof(controller));

        Logger::Success(
            "18L-AD PREMATCH COUNSELOR BIRTH: calling native RestartPlayer before PreMatchIntro completes | Counselor=" +
            SafeName((UObject*)counselorClass) +
            " | PickedSoft=" + (pickedWritten ? "true" : "false") +
            " | ActiveSoft=" + (activeWritten ? "true" : "false"));

        if (!SafeProcessEventCall(
                (uintptr_t)gameMode,
                gameMode,
                restartPlayer,
                restartParams))
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: RestartPlayer ProcessEvent failed");
            return false;
        }

        APawn* bornPawn = controller->AcknowledgedPawn;

        if (!bornPawn ||
            !Memory::IsReadable(bornPawn, sizeof(UObject)) ||
            !bornPawn->Class)
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: RestartPlayer returned without an acknowledged pawn");
            return false;
        }

        const std::string bornClass =
            SafeName((UObject*)bornPawn->Class);

        if (bornClass.find("_Counselor_C") == std::string::npos)
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: RestartPlayer produced non-counselor pawn=" +
                bornClass);
            return false;
        }

        InitializeCounselorActiveCamera(
            reinterpret_cast<AActor*>(bornPawn));

        UPropertyLite* spawnedClassProperty =
            FindPropertyInHierarchyByName(
                playerState->Class,
                "SpawnedCharacterClass");

        bool spawnedClassWritten = false;

        if (spawnedClassProperty &&
            spawnedClassProperty->Offset_Internal > 0 &&
            spawnedClassProperty->Offset_Internal < 0x10000 &&
            spawnedClassProperty->ElementSize >= (int32_t)sizeof(UClass*))
        {
            UClass** spawnedClassSlot = reinterpret_cast<UClass**>(
                reinterpret_cast<uintptr_t>(playerState) +
                spawnedClassProperty->Offset_Internal);

            if (Memory::IsReadable(spawnedClassSlot, sizeof(UClass*)))
            {
                *spawnedClassSlot = counselorClass;
                spawnedClassWritten = true;
            }
        }

        // The donor records the human PlayerState as the intro metadata owner
        // even though the human pawn is already a counselor. Preserve that
        // relationship without changing ActiveCharacterClass back to killer.
        UObject* gameState = reinterpret_cast<UObject*>(world->GameState);
        bool introOwnerWritten = false;

        if (gameState &&
            Memory::IsReadable(gameState, sizeof(UObject)) &&
            gameState->Class &&
            SafeName((UObject*)gameState->Class) ==
                "SCGameState_OfflineBots")
        {
            UObject** introOwner = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(gameState) + 0x490);

            if (Memory::IsReadable(introOwner, sizeof(UObject*)))
            {
                *introOwner = playerState;
                introOwnerWritten = true;
            }
        }

        g_CounselorBirthPawn.store((uintptr_t)bornPawn);
        g_CounselorBirthComplete.store(true);

        Logger::Success(
            "18L-AD COUNSELOR BIRTH COMPLETE: local player was born as " +
            bornClass +
            " before intro | no human Jason pawn created | SpawnedClass=" +
            (spawnedClassWritten ? "true" : "false") +
            " | IntroOwner=" + (introOwnerWritten ? "true" : "false"));
        return true;
    }

    bool BirthSelectedCounselorForPreMatch(void* gameModeValue)
    {
        if (!g_CounselorMenuRouteLatched.load())
            return false;
        if (g_CounselorBirthComplete.load())
            return true;
        if (!ValidateCounselorLifecycleNativeSurface())
            return false;

        UObject* gameMode = reinterpret_cast<UObject*>(gameModeValue);
        APlayerController* humanController = Engine::GetLocalPlayerController();

        if (!gameMode ||
            !Memory::IsReadable(gameMode, 0x940) ||
            !gameMode->Class ||
            SafeName(reinterpret_cast<UObject*>(gameMode->Class)) !=
                "SCGameMode_OfflineBots" ||
            !humanController ||
            !Memory::IsReadable(humanController, 0x390))
        {
            Logger::Error(
                "18L-AD PREMATCH: dedicated OfflineBots host or human controller is unavailable");
            return false;
        }

        UObject* humanPlayerState = GetControllerPlayerState(humanController);
        if (!humanPlayerState ||
            !Memory::IsReadable(humanPlayerState, sizeof(UObject)))
        {
            Logger::Error(
                "18L-AD PREMATCH: human SCPlayerState_OfflineBots is unavailable");
            return false;
        }

        const uint8_t* counselorSoftClass = nullptr;
        UClass* counselorClass = nullptr;

        if (!g_SelectedPlayerCounselorPath.empty())
        {
            UClass* selected = LoadLifecycleSoftClass(
                g_SelectedPlayerCounselorSoftClass.data());
            if (selected &&
                ClassDerivesFrom(selected, "SCCounselorCharacter"))
            {
                counselorSoftClass =
                    g_SelectedPlayerCounselorSoftClass.data();
                counselorClass = selected;
            }
        }

        if (!counselorSoftClass)
        {
            ResolveNativeSelectedCounselor(
                gameMode,
                &counselorSoftClass,
                &counselorClass);
        }

        if (!counselorSoftClass || !counselorClass)
        {
            Logger::Error(
                "18L-AD PREMATCH: selected counselor soft class is unavailable");
            return false;
        }

        // Donor order: counselor bots first, then the human counselor.
        const bool allBotsCreated =
            SpawnNativeCounselorBots(gameMode, counselorClass);

        if (!SetLifecycleActiveCharacter(
                humanPlayerState,
                counselorSoftClass) ||
            !RestartLifecyclePlayer(gameMode, humanController))
        {
            Logger::Error(
                "18L-AD PREMATCH: native human counselor RestartPlayer failed");
            return false;
        }

        APawn* bornPawn = humanController->AcknowledgedPawn;
        if (!bornPawn ||
            !Memory::IsReadable(bornPawn, 0x3A8) ||
            !bornPawn->Class ||
            !ClassDerivesFrom(bornPawn->Class, "SCCounselorCharacter"))
        {
            Logger::Error(
                "18L-AD PREMATCH: human RestartPlayer did not produce a counselor pawn");
            return false;
        }

        InitializeCounselorActiveCamera(
            reinterpret_cast<AActor*>(bornPawn));

        // Stock Hunter spawning normally grants these before the match intro.
        // Our counselor-specific lifecycle supplies the same native loadout
        // immediately after RestartPlayer, once only; failure is logged but
        // does not discard an otherwise valid counselor birth.
        GrantHunterStartingLoadout(gameMode, bornPawn);

        UObject* pawnControllerBefore =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(bornPawn) + 0x3A0);

        const uint8_t* killerSoftClass = nullptr;
        UClass* killerClass = nullptr;
        ResolveNativeSelectedKiller(
            gameMode,
            &killerSoftClass,
            &killerClass);

        if (!killerSoftClass ||
            !killerClass ||
            !ClassDerivesFrom(killerClass, "SCKillerCharacter"))
        {
            Logger::Error(
                "18L-AD PREMATCH: selected killer soft class is unavailable for intro metadata");
            return false;
        }

        g_TargetJasonClass.store(reinterpret_cast<uintptr_t>(killerClass));

        UObject* gameState =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(gameMode) + 0x3C0);
        if (!gameState || !Memory::IsReadable(gameState, 0x560))
        {
            Logger::Error(
                "18L-AD PREMATCH: SCGameState_OfflineBots is unavailable");
            return false;
        }

        *reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(gameState) + 0x490) =
            humanPlayerState;

        if (!SetLifecycleActiveCharacter(
                humanPlayerState,
                killerSoftClass))
        {
            Logger::Error(
                "18L-AD PREMATCH: killer intro metadata setter failed");
            return false;
        }

        ApplyLifecycleKillerCosmetics(humanPlayerState, killerSoftClass);

        const bool killerClassAssigned =
            DeepAssignLifecycleSoftClass(
                reinterpret_cast<uint8_t*>(gameState) + 0x538,
                killerSoftClass);

        APawn* pawnAfterMetadata = humanController->AcknowledgedPawn;
        UObject* pawnControllerAfter =
            pawnAfterMetadata && Memory::IsReadable(pawnAfterMetadata, 0x3A8)
            ? *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(pawnAfterMetadata) + 0x3A0)
            : nullptr;

        if (pawnAfterMetadata != bornPawn ||
            pawnControllerAfter != pawnControllerBefore ||
            pawnControllerAfter != humanController)
        {
            Logger::Error(
                "18L-AD PREMATCH: intro metadata unexpectedly changed human counselor possession");
            return false;
        }

        g_CounselorBirthPawn.store(reinterpret_cast<uintptr_t>(bornPawn));
        g_CounselorBirthComplete.store(true);

        Logger::Success(
            "18L-AD COUNSELOR BIRTH COMPLETE: donor order reproduced | human=" +
            SafeName(reinterpret_cast<UObject*>(bornPawn->Class)) +
            " | bots=" + std::to_string(g_CounselorBotsCreated.load()) +
            " | requestedBots=" +
            std::to_string(GetRequestedCounselorBotCount()) +
            " | selectedCounselors=" +
            std::to_string(g_NativeSelectedCounselorTotal.load()) +
            " | allBots=" + (allBotsCreated ? "true" : "false") +
            " | introKiller=" +
            SafeName(reinterpret_cast<UObject*>(killerClass)) +
            " | GameStateKillerClass=" +
            (killerClassAssigned ? "true" : "false") +
            " | possessionPreserved=true");

        // HandlePreMatchIntro runs on the game thread after the OfflineBots
        // world/controller and selected mode are fully established. This is
        // the first safe point to restore the temporary CDO alias.
        if (g_CounselorAliasRestorePending.load())
            RestoreDedicatedCounselorAlias();
        return true;
    }

    bool IsCounselorBirthComplete()
    {
        if (!g_CounselorBirthComplete.load())
            return false;

        APlayerController* controller = Engine::GetLocalPlayerController();
        return controller &&
            controller->AcknowledgedPawn &&
            (uintptr_t)controller->AcknowledgedPawn ==
                g_CounselorBirthPawn.load();
    }

    void MarkCounselorMatchInProgress()
    {
        if (g_CounselorMenuRouteLatched.load())
        {
            g_CounselorMatchInProgress.store(true);
            ArmObjectivesWidgetGameStatePatch();
        }
    }

    bool IsCounselorMatchInProgress()
    {
        return g_CounselorMatchInProgress.load();
    }

    bool SpawnCounselorModeJasonAfterMatch(void* gameModeValue)
    {
        if (g_CounselorJasonActive.load())
            return true;
        if (!g_CounselorMenuRouteLatched.load() ||
            !g_CounselorBirthComplete.load() ||
            !g_CounselorMatchInProgress.load() ||
            !ValidateCounselorLifecycleNativeSurface())
        {
            return false;
        }

        UObject* gameMode = reinterpret_cast<UObject*>(gameModeValue);
        UWorld* world = Engine::GetWorld();
        AActor* localCounselor = reinterpret_cast<AActor*>(
            g_CounselorBirthPawn.load());

        if (!gameMode ||
            !world ||
            !localCounselor ||
            !Memory::IsReadable(gameMode, 0x940) ||
            !Memory::IsReadable(world, sizeof(UWorld)) ||
            !Memory::IsReadable(localCounselor, sizeof(UObject)))
        {
            Logger::Error(
                "18L-AD AI Jason: match infrastructure is unavailable");
            return false;
        }

        const uint8_t* killerSoftClass = nullptr;
        UClass* killerClass = nullptr;
        ResolveNativeSelectedKiller(
            gameMode,
            &killerSoftClass,
            &killerClass);

        if (!killerSoftClass ||
            !killerClass ||
            !ClassDerivesFrom(killerClass, "SCKillerCharacter"))
        {
            Logger::Error(
                "18L-AD AI Jason: selected killer class did not resolve");
            return false;
        }

        AActor* killerStart = FindKillerPlayerStart(world);
        FVector startLocation{};
        FRotator startRotation{};

        if (!killerStart ||
            !GetActorStartTransform(
                killerStart,
                startLocation,
                startRotation))
        {
            Logger::Error(
                "18L-AD AI Jason: no usable SCKillerPlayerStart transform; failing closed");
            return false;
        }

        AActor* jason = SpawnLifecycleActor(
            world,
            killerClass,
            &startLocation,
            &startRotation,
            nullptr,
            2);

        if (!jason ||
            !Memory::IsReadable(jason, 0x3A8))
        {
            Logger::Error(
                "18L-AD AI Jason: selected killer pawn SpawnActor failed");
            return false;
        }

        using ClassGetterFn = UClass* (__fastcall*)();
        ClassGetterFn killerControllerGetter =
            reinterpret_cast<ClassGetterFn>(ShippingAddress(0x4C8FF0));
        UClass* killerControllerClass = killerControllerGetter
            ? killerControllerGetter()
            : nullptr;

        if (!killerControllerClass)
        {
            Logger::Error(
                "18L-AD AI Jason: SCKillerAIController class getter failed");
            return false;
        }

        AActor* killerControllerActor =
            SpawnKillerControllerWithStockCrowdBypass(
                world,
                killerControllerClass);
        UObject* killerController =
            reinterpret_cast<UObject*>(killerControllerActor);

        if (!killerController ||
            !Memory::IsReadable(killerController, 0x410))
        {
            Logger::Error(
                "18L-AD AI Jason: SCKillerAIController SpawnActor failed");
            return false;
        }

        using PossessFn = void(__fastcall*)(UObject*, AActor*);
        PossessFn basePossess = reinterpret_cast<PossessFn>(
            ShippingAddress(0x112F3F0));
        basePossess(killerController, jason);

        UObject* pawnController =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(jason) + 0x3A0);
        UObject* killerPlayerState =
            GetControllerPlayerState(killerController);

        if (pawnController != killerController ||
            !killerPlayerState ||
            !Memory::IsReadable(killerPlayerState, sizeof(UObject)))
        {
            Logger::Error(
                "18L-AD AI Jason: base AAIController possession did not establish pawn/PlayerState links");
            return false;
        }

        if (!SetLifecycleActiveCharacter(
                killerPlayerState,
                killerSoftClass))
        {
            Logger::Error(
                "18L-AD AI Jason: PlayerState killer metadata setter failed");
            return false;
        }

        ApplyLifecycleKillerCosmetics(
            killerPlayerState,
            killerSoftClass);

        UObject* humanPlayerState = GetControllerPlayerState(
            Engine::GetLocalPlayerController());
        const bool killerPresentationReady =
            CopyLifecycleKillerPresentation(
                humanPlayerState,
                killerPlayerState);

        if (!FrozenJasonBridge::AdoptCounselorModeJason(
                jason,
                killerController,
                localCounselor))
        {
            Logger::Error(
                "18L-AD AI Jason: frozen behavior adapter rejected the native pawn/controller pair");
            return false;
        }

        // The pre-match intro has to borrow the human PlayerState as its
        // killer metadata owner because stock OfflineBots assumes that the
        // local player is Jason.  Once the independent AI Jason exists, hand
        // that ownership to its real PlayerState and restore the human's
        // ActiveCharacterClass to the counselor that is still possessed.
        // Leaving the borrowed killer metadata in place makes the HUD/result
        // path report Jason's "You killed" summary to the counselor player.
        const uint8_t* activeCounselorSoftClass = nullptr;
        const int32_t counselorClassCount =
            GetLifecycleSoftClassCount(gameMode, 0x510);

        for (int32_t i = 0; i < counselorClassCount; ++i)
        {
            const uint8_t* candidate =
                GetLifecycleSoftClass(gameMode, 0x510, i);
            UClass* loaded = LoadLifecycleSoftClass(candidate);
            if (loaded == localCounselor->Class)
            {
                activeCounselorSoftClass = candidate;
                break;
            }
        }

        UObject* gameState =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(gameMode) + 0x3C0);
        bool killerOwnerTransferred = false;
        if (killerPresentationReady &&
            gameState &&
            Memory::IsReadable(gameState, 0x498))
        {
            UObject** killerOwner = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(gameState) + 0x490);
            if (Memory::IsReadable(killerOwner, sizeof(UObject*)))
            {
                *killerOwner = killerPlayerState;
                killerOwnerTransferred = *killerOwner == killerPlayerState;
            }
        }

        const bool humanCounselorMetadataRestored =
            humanPlayerState &&
            activeCounselorSoftClass &&
            SetLifecycleActiveCharacter(
                humanPlayerState,
                activeCounselorSoftClass);

        if (killerOwnerTransferred && humanCounselorMetadataRestored)
        {
            Logger::Success(
                "18L-AI ROLE HANDOFF COMPLETE: GameState killer owner=AI Jason PlayerState | human ActiveCharacterClass=" +
                SafeName(reinterpret_cast<UObject*>(localCounselor->Class)));
        }
        else
        {
            Logger::Error(
                "18L-AI ROLE HANDOFF INCOMPLETE: killerOwnerTransferred=" +
                std::string(killerOwnerTransferred ? "true" : "false") +
                " | humanCounselorMetadataRestored=" +
                (humanCounselorMetadataRestored ? "true" : "false"));
        }

        g_CounselorJasonActive.store(true);

        Logger::Success(
            "18L-AD AI JASON COMPLETE: independent " +
            SafeName(reinterpret_cast<UObject*>(killerClass)) +
            " spawned at SCKillerPlayerStart and possessed by SCKillerAIController | human counselor untouched");
        return true;
    }

    bool BeginFrozenAICompatibilityShim()
    {
        if (g_AISpoofObject.load() &&
            g_AISpoofController.load())
        {
            return true;
        }

        if (!g_CounselorMenuRouteLatched.load() ||
            !g_CounselorBirthComplete.load() ||
            !g_CounselorMatchInProgress.load() ||
            !Engine::IsInGame())
        {
            return false;
        }

        UWorld* world = Engine::GetWorld();
        if (!world)
            return false;

        uintptr_t gameModeSlot =
            (uintptr_t)world + 0xF0;

        if (!Memory::IsReadable(
                (void*)gameModeSlot,
                sizeof(UObject*)))
        {
            return false;
        }

        UObject* gameMode =
            *(UObject**)gameModeSlot;

        if (!gameMode ||
            !Memory::IsReadable(
                gameMode,
                sizeof(UObject)) ||
            !gameMode->Class ||
            !Memory::IsReadable(
                gameMode->Class,
                sizeof(UClass)))
        {
            return false;
        }

        std::string className =
            SafeName((UObject*)gameMode->Class);

        if (className != "SCGameMode_OfflineBots")
            return false;

        UClass* sandboxClass =
            FindClassExact("SCGameMode_Sandbox");

        APlayerController* controller =
            Engine::GetLocalPlayerController();

        UClass* sandboxControllerClass =
            FindClassExact("SCPlayerController_Sandbox");

        if (!sandboxClass ||
            !Memory::IsReadable(
                sandboxClass,
                sizeof(UClass)) ||
            !controller ||
            !Memory::IsReadable(
                controller,
                sizeof(UObject)) ||
            !controller->Class ||
            !sandboxControllerClass ||
            !Memory::IsReadable(
                sandboxControllerClass,
                sizeof(UClass)))
        {
            return false;
        }

        const std::string controllerClass =
            SafeName((UObject*)controller->Class);

        if (controllerClass.find("OfflineBots") ==
            std::string::npos)
        {
            return false;
        }

        g_AISpoofObject.store(
            (uintptr_t)gameMode);

        g_AISpoofOriginalClass.store(
            (uintptr_t)gameMode->Class);

        g_AISpoofController.store(
            (uintptr_t)controller);

        g_AISpoofControllerOriginalClass.store(
            (uintptr_t)controller->Class);

        gameMode->Class = sandboxClass;
        controller->Class = sandboxControllerClass;

        Logger::Success(
            "18L-AD frozen-AI compatibility shim: dedicated OFLBC host GameMode/controller temporarily exposed as Sandbox for frozen resource/spawn APIs");
        return true;
    }

    void EndFrozenAICompatibilityShim()
    {
        UObject* gameMode =
            (UObject*)g_AISpoofObject.exchange(0);

        UClass* originalClass =
            (UClass*)g_AISpoofOriginalClass.exchange(0);

        UObject* controller =
            (UObject*)g_AISpoofController.exchange(0);

        UClass* controllerOriginalClass =
            (UClass*)g_AISpoofControllerOriginalClass.exchange(0);

        if (gameMode &&
            originalClass &&
            Memory::IsReadable(
                gameMode,
                sizeof(UObject)))
        {
            gameMode->Class = originalClass;
        }

        if (controller &&
            controllerOriginalClass &&
            Memory::IsReadable(
                controller,
                sizeof(UObject)))
        {
            controller->Class = controllerOriginalClass;
        }

        Logger::Success(
            "18L-AD frozen-AI compatibility shim restored native OfflineBots GameMode/controller classes");
    }

    bool IsCounselorModeAutoStartArmed()
    {
        return g_CounselorModeAutoStartArmed.load();
    }

    bool IsSandboxCounselorReady()
    {
        return g_SandboxCounselorSyncStage.load() ==
            (int)SandboxCounselorSyncStage::Done;
    }

    int32_t GetCounselorModeBotCount()
    {
        return GetRequestedCounselorBotCount();
    }

    void MarkCounselorModeAutoStartFinished()
    {
        g_CounselorModeAutoStartArmed.store(false);
        g_CounselorMenuRouteEnabled.store(false);
        g_CounselorMenuRouteLatched.store(false);
        EndFrozenAICompatibilityShim();
    }

    bool HasPendingRequest()
    {
        if (g_StartSplashAcceptRequested.load())
            return true;

        if (g_RuntimeMenuOrderRequested.load())
            return true;

        if (g_DumpCounselorRosterRequested.load())
            return true;

        if (g_SandboxCounselorSyncStage.load() ==
            (int)SandboxCounselorSyncStage::NeedRequest)
            return true;

        int counselorStage =
            g_CounselorStage.load();

        if (counselorStage == (int)CounselorSelectStage::NeedRequest)
        {
            return true;
        }

        int stage = g_Stage.load();
        return stage == (int)SetupStage::NeedPreload ||
            stage == (int)SetupStage::WaitingForAssets ||
            stage == (int)SetupStage::ReadyToApply ||
            stage == (int)SetupStage::Sticky;
    }

    void TickWorker()
    {
        TickStartSplashAutoAccept();

        // A completed/abandoned counselor match can return to a fresh frontend
        // without traversing the older auto-start-finished callback. Retire the
        // stale route here so the same process can immediately arm a brand-new
        // counselor picker. GetActiveFrontendMenu is deliberately narrow and
        // cannot match an in-world pause menu.
        UObject* activeFrontend = GetActiveFrontendMenu();
        if (activeFrontend && g_CounselorMatchInProgress.exchange(false))
        {
            const uintptr_t activeFrontendAddress =
                reinterpret_cast<uintptr_t>(activeFrontend);
            // Rebuilding the VerticalBox a second time in one process is
            // unsafe after the post-match frontend reconstructs its Slate
            // resources. Keep replay widgets in native order instead of
            // clearing/re-adding their children. A fresh process still receives
            // the requested Jason/Counselor-first order.
            const bool suppressingReplayMenuRebuild =
                g_RuntimeMenuOrderedWidget.load() != 0;

            RestoreCounselorPickerPlayerStateCast();
            EndFrozenAICompatibilityShim();

            g_CounselorModeAutoStartArmed.store(false);
            g_CounselorBirthComplete.store(false);
            g_CounselorJasonActive.store(false);
            g_CounselorBotsCreated.store(0);
            g_CounselorBirthPawn.store(0);
            g_CounselorMenuRouteEnabled.store(false);
            g_CounselorMenuRouteLatched.store(false);
            g_CounselorMenuWorld.store(0);
            g_CounselorMenuObject.store(0);
            g_CounselorMenuRewriteCount.store(0);
            g_RequestOfflineModeHookHits.store(0);
            g_CounselorEntryClickPending.store(false);
            g_CounselorEntryClickedAt.store(0);
            g_CounselorPickerActive.store(false);
            g_CounselorPickerAccepted.store(false);
            g_CounselorPickerWidget.store(0);
            g_CounselorPickerSourceMenu.store(0);
            g_NativeSelectedCounselorTotal.store(0);
            g_GameSetupSelectionLocked.store(false);
            g_RuntimeMenuLabeledWidget.store(0);
            g_RuntimeMenuLabelLastAttempt.store(0);
            g_RuntimeMenuLabelFailureLogged.store(false);
            g_RuntimeMenuOrderedWidget.store(
                suppressingReplayMenuRebuild
                    ? activeFrontendAddress
                    : 0);
            g_RuntimeMenuOrderFailureLogged.store(false);
            g_RuntimeMenuOrderRequested.store(false);
            g_RuntimeMenuOrderWidget.store(0);
            g_RuntimeMenuOrderFailureCode.store(0);
            g_NextAutomaticRouteArmAt.store(0);
            g_ObjectivesGameStateCastPatchPending.store(false);
            g_ObjectivesGameStateCastNextAttempt.store(0);

            Logger::Success(
                suppressingReplayMenuRebuild
                    ? "18L-AC replay lifecycle: counselor match returned to frontend; destructive menu reorder suppressed and fresh character picker re-armed"
                    : "18L-AC replay lifecycle: counselor match returned to frontend; stale route retired and fresh character picker re-armed");
        }

        // Normal builds arm themselves as soon as Resurrected's frontend
        // SCGame_Menu exists.  The dedicated row hook still distinguishes the
        // Counselor entry, so Offline Bots - Jason and Sandbox remain stock.
        // The old controller/export path stays available only for diagnostics.
        if (!g_CounselorMenuRouteEnabled.load() &&
            !g_CounselorMenuRouteLatched.load())
        {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG next = g_NextAutomaticRouteArmAt.load();
            if ((!next || now >= next) && GetActiveFrontendMenu())
            {
                g_NextAutomaticRouteArmAt.store(now + 1000);
                if (ArmSelectedPreset())
                {
                    Logger::Success(
                        "CONTROLLER-FREE MODE READY: Offline Bots - Counselor is armed automatically; use the native game menus only");
                }
            }
        }

        // Route installation must always win the frontend race.  The native
        // profile lookup can perform a full live-object scan the first time it
        // resolves the selection save.  Running that scan before ArmSelectedPreset
        // allowed a fast user to enter the stock Jason picker while this worker
        // was still busy, and a late injection could scan during map teardown.
        // Only sync the profile once the dedicated frontend route is armed.
        if (g_CounselorMenuRouteEnabled.load() ||
            g_CounselorMenuRouteLatched.load())
        {
            TickNativeProfileSelection();
        }

        // The unified setup preset is independent from the frozen AI tick and
        // from the Jason-selection state machine.  Service only explicit
        // one-shot requests here, then maintain the already-validated map
        // pointer while still in the frontend world.
        if (g_ArmPresetRequested.exchange(false))
        {
            ArmSelectedPreset();
        }

        if (g_ApplyGameSetupRequested.exchange(false))
        {
            ApplyGameSetupPreset();
        }

        TickMapSticky();
        TickCounselorSelection();
        TickSandboxCounselorSync();

        int stage = g_Stage.load();
        if (stage == (int)SetupStage::Idle ||
            stage == (int)SetupStage::Done ||
            stage == (int)SetupStage::Failed)
        {
            return;
        }

        ResolveSelectionSaveMetadata();

        if (!g_SelectionSaveObject.load() &&
            g_SelectionSaveClass.load() &&
            !g_BackgroundScanStarted.exchange(true))
        {
            Logger::Debug(
                "Jason resolve 18B: background search for live SCCharacterSelectionsSaveGame started");

            UObject* live = RawFindSelectionSaveObject(
                (UClass*)g_SelectionSaveClass.load());

            if (live)
            {
                g_SelectionSaveObject.store((uintptr_t)live);
                Logger::Success(
                    "Jason resolve 18B: live SCCharacterSelectionsSaveGame found");
            }
            else
            {
                Logger::Error(
                    "Jason resolve 18B: live SCCharacterSelectionsSaveGame not found");
            }
        }

        if (stage == (int)SetupStage::Sticky)
        {
            UWorld* world = Engine::GetWorld();
            uintptr_t originalWorld = g_SelectionWorld.load();

            // Prototype 16 proved the stock frontend can overwrite KillerPick
            // after our initial write. Keep it sticky ONLY while we remain in
            // the same frontend world. Stop immediately when travel/teardown
            // swaps the UWorld pointer.
            if (!world || !originalWorld ||
                (uintptr_t)world != originalWorld)
            {
                Logger::Success(
                    "Jason resolve 18B: frontend world changed; sticky KillerPick override released");
                g_Stage.store((int)SetupStage::Done);
                return;
            }

            UObject* saveObject =
                (UObject*)g_SelectionSaveObject.load();
            UClass* jasonClass =
                (UClass*)g_TargetJasonClass.load();
            int32_t killerPickOffset =
                g_KillerPickOffset.load();

            if (!saveObject || !jasonClass ||
                killerPickOffset < 0 ||
                !Memory::IsReadable(saveObject, sizeof(UObject)))
            {
                Logger::Error(
                    "Jason resolve 18B: sticky state lost required pointers");
                g_Stage.store((int)SetupStage::Failed);
                return;
            }

            UClass** killerPick =
                (UClass**)((uintptr_t)saveObject + killerPickOffset);

            if (!Memory::IsReadable(killerPick, sizeof(UClass*)))
            {
                Logger::Error(
                    "Jason resolve 18B: sticky KillerPick address unreadable");
                g_Stage.store((int)SetupStage::Failed);
                return;
            }

            UClass* current = *killerPick;

            if (current != jasonClass)
            {
                std::string beforeName =
                    SafeName((UObject*)current);

                *killerPick = jasonClass;

                uint32_t n =
                    g_StickyRewriteCount.fetch_add(1) + 1;

                if (n <= 12)
                {
                    Logger::Debug(
                        "Jason resolve 18B sticky: stock frontend changed KillerPick to " +
                        beforeName +
                        " -> restoring selected Jason");
                }
            }

            return;
        }

        if (!g_TargetJasonClass.load() &&
            stage == (int)SetupStage::WaitingForAssets)
        {
            ULONGLONG now = GetTickCount64();
            ULONGLONG lastScan = g_LastTargetScanAt.load();

            if (!lastScan || now - lastScan >= 500)
            {
                g_LastTargetScanAt.store(now);

                if (TryResolveTargetJasonClass())
                {
                    Logger::Success(
                        "Jason resolve 18B: selected Jason class is available");
                }
            }

        }

        if (g_TargetJasonClass.load() &&
            g_SelectionSaveObject.load() &&
            g_KillerPickOffset.load() >= 0 &&
            stage == (int)SetupStage::WaitingForAssets)
        {
            g_Stage.store((int)SetupStage::ReadyToApply);
        }
    }

    bool ConsumePendingRequestOnGameThread()
    {
        if (g_StartSplashAcceptRequested.exchange(false))
        {
            if (!AcceptStartSplashOnGameThread())
            {
                // Retry discovery rather than retaining a possibly stale
                // widget pointer across a frontend transition.
                g_StartSplashWidget.store(0);
                g_NextStartSplashScanAt.store(GetTickCount64() + 250);
                return false;
            }
            return true;
        }

        if (g_RuntimeMenuOrderRequested.exchange(false))
        {
            UObject* menuWidget = reinterpret_cast<UObject*>(
                g_RuntimeMenuOrderWidget.load());
            const bool ordered =
                ReorderRuntimeOfflinePlayMenu(menuWidget);
            if (!ordered &&
                !g_RuntimeMenuOrderFailureLogged.exchange(true))
            {
                Logger::Error(
                    "Packed menu game-thread reorder failed | Code=" +
                    std::to_string(
                        g_RuntimeMenuOrderFailureCode.load()));
            }
            return ordered;
        }

        if (g_SandboxCounselorSyncStage.load() ==
            (int)SandboxCounselorSyncStage::NeedRequest)
        {
            return RequestSandboxSelectedCounselorOnGameThread();
        }

        int counselorStage =
            g_CounselorStage.load();

        if (counselorStage ==
            (int)CounselorSelectStage::NeedRequest)
        {
            Logger::Success(
                "Counselor selection 18K executing SCPlayerState::RequestCounselorClass on game thread | thread=" +
                std::to_string(GetCurrentThreadId()));

            if (!RequestSelectedCounselorSoftClassOnGameThread())
            {
                Logger::Error(
                    "Counselor selection 18K: native counselor soft-class request failed");

                g_CounselorStage.store(
                    (int)CounselorSelectStage::Failed);

                return false;
            }

            g_CounselorStage.store(
                (int)CounselorSelectStage::Monitoring);

            Logger::Success(
                "Counselor selection 18K: native request submitted; monitoring PickedCounselorClass until frontend travel");

            return true;
        }

        if (g_DumpCounselorRosterRequested.exchange(false))
        {
            Logger::Success(
                "Counselor discovery 18F executing on game thread | thread=" +
                std::to_string(GetCurrentThreadId()));
            return DumpCounselorRosterOnGameThread();
        }

        int stage = g_Stage.load();

        if (stage == (int)SetupStage::NeedPreload)
        {
            Logger::Success(
                "Jason resolve 18B executing synchronous soft-class conversion on game thread | thread=" +
                std::to_string(GetCurrentThreadId()));

            if (!ResolveJason5ClassOnGameThread())
            {
                Logger::Error(
                    "Jason resolve 18B: selected Jason soft-class conversion failed");
                g_Stage.store((int)SetupStage::Failed);
                return false;
            }

            g_Stage.store((int)SetupStage::WaitingForAssets);
            return true;
        }

        if (stage == (int)SetupStage::WaitingForAssets)
        {
            // Worker thread is resolving the live save object and loaded class.
            // Keep this game-thread path intentionally cheap.
            return true;
        }

        if (stage == (int)SetupStage::ReadyToApply)
        {
            UObject* saveObject =
                (UObject*)g_SelectionSaveObject.load();
            UClass* jasonClass =
                (UClass*)g_TargetJasonClass.load();
            int32_t killerPickOffset =
                g_KillerPickOffset.load();

            if (!saveObject || !jasonClass || killerPickOffset < 0 ||
                !Memory::IsReadable(saveObject, sizeof(UObject)))
            {
                Logger::Error(
                    "Jason resolve 18B: ready state lost required pointers");
                g_Stage.store((int)SetupStage::Failed);
                return false;
            }

            UClass** killerPick =
                (UClass**)((uintptr_t)saveObject + killerPickOffset);

            if (!Memory::IsReadable(killerPick, sizeof(UClass*)))
            {
                Logger::Error(
                    "Jason resolve 18B: KillerPick address unreadable");
                g_Stage.store((int)SetupStage::Failed);
                return false;
            }

            UClass* before = *killerPick;
            *killerPick = jasonClass;
            UClass* after = *killerPick;

            Logger::Debug(
                "Jason resolve 18B: KillerPick before=" + SafeName((UObject*)before) +
                " after=" + SafeName((UObject*)after));

            if (after == jasonClass)
            {
                Logger::Success(
                    "Jason resolve 18B SUCCESS: selected Jason KillerPick set to " +
                    SafeName((UObject*)after));

                UWorld* world = Engine::GetWorld();
                g_SelectionWorld.store((uintptr_t)world);
                g_StickyRewriteCount.store(0);

                if (world)
                {
                    Logger::Success(
                        "Jason resolve 18B: sticky KillerPick override armed until frontend world travel");
                    g_Stage.store((int)SetupStage::Sticky);
                }
                else
                {
                    Logger::Error(
                        "Jason resolve 18B: could not capture frontend world for sticky override");
                    g_Stage.store((int)SetupStage::Failed);
                }

                return true;
            }

            Logger::Error(
                "Jason resolve 18B: KillerPick write did not persist");
            g_Stage.store((int)SetupStage::Failed);
            return false;
        }

        return false;
    }

    bool HasPendingObjectivesPatch()
    {
        return g_ObjectivesGameStateCastPatchPending.load() &&
            GetTickCount64() >=
                g_ObjectivesGameStateCastNextAttempt.load();
    }

    bool TryPatchObjectivesWidgetOnGameThread()
    {
        if (!HasPendingObjectivesPatch())
            return false;

        if (PatchObjectivesWidgetGameStateCast())
        {
            if (g_ObjectivesGameStateCastPatched.load())
                g_ObjectivesGameStateCastPatchPending.store(false);
            return true;
        }

        // ObjectivesWidget is first loaded by the pause/map UI. That UI also
        // suspends gameplay ReceiveTick, so Engine's ProcessEvent bridge calls
        // this helper from the first confirmed game-thread UI event as well.
        g_ObjectivesGameStateCastNextAttempt.store(
            GetTickCount64() + 250);
        return false;
    }

    bool TryPatchObjectivesWidgetForEventOnGameThread(void* objectValue)
    {
        if (!g_ObjectivesGameStateCastPatchPending.load())
            return false;

        UObject* object = reinterpret_cast<UObject*>(objectValue);
        if (!object ||
            !Memory::IsReadable(object, sizeof(UObject)) ||
            !object->Class ||
            !Memory::IsReadable(object->Class, sizeof(UClass)))
        {
            return false;
        }

        const std::string eventClassName =
            SafeName(reinterpret_cast<UObject*>(object->Class));
        bool isObjectiveWidget = false;
        for (const ObjectiveWidgetPatchSpec& spec :
             g_ObjectiveWidgetPatchSpecs)
        {
            if (eventClassName == spec.ClassName)
            {
                isObjectiveWidget = true;
                break;
            }
        }
        if (!isObjectiveWidget)
            return false;

        // This event proves the class is resident. Patch before forwarding it
        // so Construct and the first ExecuteUbergraph call see the widened
        // SCGameState cast. No recurring class scan is needed during play.
        g_ObjectivesGameStateCastNextAttempt.store(0);
        const bool patched =
            PatchObjectivesWidgetGameStateCast(eventClassName.c_str());
        if (g_ObjectivesGameStateCastPatched.load())
            g_ObjectivesGameStateCastPatchPending.store(false);
        return patched;
    }

}
