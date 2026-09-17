#include <Windows.h>
#include "Game/Engine/Engine.hpp"
#include "Game/Features/Features.hpp"
#include "Game/Setup/OfflineSetup.hpp"
#include "Game/Setup/FrozenJasonBridge.hpp"
#include "Utils/Logger/Logger.hpp"
#include "Utils/Memory.hpp"
#include "../vendor/minhook/include/MinHook.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <intrin.h>
#include <string>

static volatile LONG g_Running = 1;

using OfflineLifecycleFn = void(__fastcall*)(void*);
using RequestExitFn = void(__fastcall*)(bool);

static OfflineLifecycleFn g_OriginalOfflineBots818 = nullptr;
static OfflineLifecycleFn g_OriginalOfflineBots958 = nullptr;
static OfflineLifecycleFn g_OfflineParent818 = nullptr;
static OfflineLifecycleFn g_OfflineParent958 = nullptr;
static RequestExitFn g_OriginalRequestExit = nullptr;
static std::atomic<bool> g_LifecycleTraceHooksInstalled{ false };
static std::atomic<uint32_t> g_818TraceCount{ 0 };
static std::atomic<uint32_t> g_958TraceCount{ 0 };
static volatile LONG g_RequestExitLogging = 0;

static constexpr uintptr_t RVA_OfflineBots818 = 0x38EA60;
static constexpr uintptr_t RVA_OfflineBots958 = 0x390640;
static constexpr uintptr_t RVA_OfflineParent818 = 0x38E3D0;
static constexpr uintptr_t RVA_OfflineParent958 = 0x38F180;
static constexpr uintptr_t RVA_RequestExit = 0x600030;
static constexpr uintptr_t RVA_GErrorHist = 0x2FE0600;
static constexpr uintptr_t RVA_GIsCriticalError = 0x2FEC603;
static constexpr uintptr_t RVA_LooseFileAlignmentGate = 0x5ECF93;

static bool InstallDonorLooseFileReadFallback()
{
    const wchar_t* commandLine = GetCommandLineW();
    if (!commandLine ||
        (wcsstr(commandLine, L"-NoPak") == nullptr &&
         wcsstr(commandLine, L"-nopak") == nullptr))
    {
        Logger::Debug(
            "18L-AF: packed-file launch detected; donor loose-file fallback not required");
        return true;
    }

    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base)
        return false;

    uint8_t* gate = reinterpret_cast<uint8_t*>(
        base + RVA_LooseFileAlignmentGate);
    const uint8_t* fallback = reinterpret_cast<const uint8_t*>(
        base + 0x5ECFD8);

    // Resurrected's FWindowsReadRequest constructor force-exits on the
    // unaligned texture-mip reads produced by -NoPak loose files. The working
    // community build routes this exact case through a native aligned bounce
    // buffer. Resurrected still contains that same fallback at +0x5ECFD8; only
    // its gate differs. Branch directly to the preserved fallback.
    const uint8_t expected[] =
    {
        0x8B,0x05,0xFB,0x0F,0xB0,0x02
    };
    const uint8_t patched[] =
    {
        0xE9,0x40,0x00,0x00,0x00,0x90
    };
    const uint8_t fallbackExpected[] =
    {
        0x33,0xD2,0xE8,0x01,0x0E,0xF5,0xFF,0x48,
        0x89,0x83,0xB0,0x00,0x00,0x00,0xEB,0x10
    };

    if (!Memory::IsReadable(gate, sizeof(expected)) ||
        !Memory::IsReadable(fallback, sizeof(fallbackExpected)) ||
        std::memcmp(
            fallback,
            fallbackExpected,
            sizeof(fallbackExpected)) != 0)
    {
        Logger::Error(
            "18L-AF: native bounce-buffer fallback signature mismatch; patch refused");
        return false;
    }

    if (std::memcmp(gate, patched, sizeof(patched)) == 0)
        return true;

    if (std::memcmp(gate, expected, sizeof(expected)) != 0)
    {
        Logger::Error(
            "18L-AF: loose-file alignment gate signature mismatch; donor fallback NOT enabled");
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(
            gate,
            sizeof(patched),
            PAGE_EXECUTE_READWRITE,
            &oldProtection))
    {
        Logger::Error(
            "18L-AF: VirtualProtect failed for donor loose-file fallback");
        return false;
    }

    std::memcpy(gate, patched, sizeof(patched));
    FlushInstructionCache(
        GetCurrentProcess(),
        gate,
        sizeof(patched));

    DWORD ignored = 0;
    VirtualProtect(gate, sizeof(patched), oldProtection, &ignored);

    Logger::Success(
        "18L-AF: donor loose-file async-read fallback enabled | unaligned texture mips now use Resurrected's native aligned bounce buffer");
    return true;
}

static void AppendEmergencyLog(const char* text)
{
    wchar_t tempPath[MAX_PATH]{};
    wchar_t logPath[MAX_PATH]{};

    DWORD pathLength = GetTempPathW(MAX_PATH, tempPath);
    if (pathLength == 0 || pathLength >= MAX_PATH)
        return;

    if (swprintf_s(
            logPath,
            L"%sResurrectedOfflineBots-18L-AC.log",
            tempPath) < 0)
    {
        return;
    }

    HANDLE file = CreateFileW(
        logPath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(
        file,
        text,
        static_cast<DWORD>(strlen(text)),
        &written,
        nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

static void __fastcall RequestExitTraceHook(bool force)
{
    // Stop the polling worker before UE starts destroying the world. The
    // previous build kept walking GWorld/level actor arrays for several
    // seconds after RequestExit, overlapping object destruction.
    InterlockedExchange(&g_Running, 0);

    // This hook is diagnostic only. The donor-style lifecycle currently gets
    // through ClientPlayIntro, after which Resurrected calls force-exit without
    // producing a crash dump. Record the exact caller and hidden fatal state,
    // then preserve the original behavior unchanged.
    if (InterlockedCompareExchange(&g_RequestExitLogging, 1, 0) == 0)
    {
        const uintptr_t base =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const uintptr_t caller =
            reinterpret_cast<uintptr_t>(_ReturnAddress());

        void* frames[32]{};
        const USHORT frameCount =
            CaptureStackBackTrace(0, 32, frames, nullptr);

        uint8_t critical = 0xFF;
        if (base && Memory::IsReadable(
                reinterpret_cast<void*>(base + RVA_GIsCriticalError),
                sizeof(critical)))
        {
            critical = *reinterpret_cast<volatile uint8_t*>(
                base + RVA_GIsCriticalError);
        }

        char errorUtf8[4096]{};
        const wchar_t* errorHist = base
            ? reinterpret_cast<const wchar_t*>(base + RVA_GErrorHist)
            : nullptr;

        if (errorHist &&
            Memory::IsReadable(errorHist, 1024 * sizeof(wchar_t)))
        {
            int wideLength = 0;
            while (wideLength < 1023 && errorHist[wideLength] != L'\0')
                ++wideLength;

            if (wideLength > 0)
            {
                WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    errorHist,
                    wideLength,
                    errorUtf8,
                    static_cast<int>(sizeof(errorUtf8) - 1),
                    nullptr,
                    nullptr);

                for (char* p = errorUtf8; *p; ++p)
                {
                    if (*p == '\r' || *p == '\n')
                        *p = ' ';
                }
            }
        }

        char line[8192]{};
        int used = sprintf_s(
            line,
            "[REQUEST_EXIT_TRACE] force=%u thread=%lu caller=%p callerRva=0x%llX critical=%u errorHist=\"%s\" frames=",
            force ? 1u : 0u,
            GetCurrentThreadId(),
            reinterpret_cast<void*>(caller),
            static_cast<unsigned long long>(base && caller >= base
                ? caller - base
                : 0),
            static_cast<unsigned>(critical),
            errorUtf8[0] ? errorUtf8 : "<empty>");

        if (used < 0)
            used = 0;

        for (USHORT i = 0;
             i < frameCount && used < static_cast<int>(sizeof(line) - 64);
             ++i)
        {
            const uintptr_t address =
                reinterpret_cast<uintptr_t>(frames[i]);
            const int appended = sprintf_s(
                line + used,
                sizeof(line) - static_cast<size_t>(used),
                "%s%p(rva=0x%llX)",
                i == 0 ? "" : ",",
                frames[i],
                static_cast<unsigned long long>(base && address >= base
                    ? address - base
                    : 0));

            if (appended <= 0)
                break;

            used += appended;
        }

        if (used < static_cast<int>(sizeof(line) - 3))
        {
            line[used++] = '\r';
            line[used++] = '\n';
            line[used] = '\0';
        }

        AppendEmergencyLog(line);
    }

    if (g_OriginalRequestExit)
        g_OriginalRequestExit(force);
}

static std::string DescribeLocalState()
{
    APlayerController* controller =
        Engine::GetLocalPlayerController();

    if (!controller)
        return "controller=<none> | pawn=<none>";

    std::string controllerClass =
        controller->Class
        ? controller->Class->GetName()
        : std::string("<unknown>");

    std::string pawnClass = "<none>";

    if (controller->AcknowledgedPawn &&
        controller->AcknowledgedPawn->Class)
    {
        pawnClass =
            controller->AcknowledgedPawn->Class->GetName();
    }

    return "controller=" + controllerClass +
        " | pawn=" + pawnClass;
}

static void __fastcall OfflineBots818TraceHook(void* gameMode)
{
    const bool counsel =
        OfflineSetup::IsCounselorMenuRouteLatched();

    if (counsel)
    {
        uint32_t n = g_818TraceCount.fetch_add(1) + 1;
        if (n <= 4)
        {
            Logger::Success(
                "18L-AD COUNSEL HandleMatchHasStarted ENTER | " +
                DescribeLocalState());
        }
    }

    if (counsel)
    {
        // Donor OfflineBotsC is a sibling of OfflineBots. Its InProgress
        // override calls the common Offline parent, then creates AI Jason.
        // Never call the stock OfflineBots override on this route because it
        // creates the human-Jason lifecycle we are replacing.
        // RoundTime is copied from SCGameMode +0x4E0 into GameState by the
        // common parent below.  Apply the requested 60-minute duration only
        // to this counselor-mode instance; the stock Jason route remains at
        // its configured 20 minutes.
        int32_t* roundTime = reinterpret_cast<int32_t*>(
            reinterpret_cast<uintptr_t>(gameMode) + 0x4E0);
        if (Memory::IsReadable(roundTime, sizeof(int32_t)))
        {
            const int32_t previousRoundTime = *roundTime;
            *roundTime = 3600;
            if (g_818TraceCount.load() <= 4)
            {
                Logger::Success(
                    "18L-AK COUNSEL ROUND TIME: " +
                    std::to_string(previousRoundTime) +
                    " -> 3600 seconds before native GameState initialization");
            }
        }

        if (g_OfflineParent818)
            g_OfflineParent818(gameMode);

        OfflineSetup::MarkCounselorMatchInProgress();

        if (!OfflineSetup::SpawnCounselorModeJasonAfterMatch(gameMode))
        {
            Logger::Error(
                "18L-AD COUNSEL HandleMatchHasStarted: native AI Jason creation/adoption failed");
        }
    }
    else if (g_OriginalOfflineBots818)
    {
        g_OriginalOfflineBots818(gameMode);
    }

    if (counsel)
    {
        uint32_t n = g_818TraceCount.load();
        if (n <= 4)
        {
            Logger::Success(
                "18L-AD COUNSEL HandleMatchHasStarted RETURN: Offline parent preserved; donor-style AI Jason handed to frozen behavior | " +
                DescribeLocalState());
        }
    }
}

static void __fastcall OfflineBots958TraceHook(void* gameMode)
{
    const bool counsel =
        OfflineSetup::IsCounselorMenuRouteLatched();

    if (counsel)
    {
        uint32_t n = g_958TraceCount.fetch_add(1) + 1;
        if (n <= 4)
        {
            Logger::Success(
                "18L-AD COUNSEL HandlePreMatchIntro ENTER | " +
                DescribeLocalState());
        }
    }

    if (counsel)
    {
        // Donor ordering is counselor population/human RestartPlayer first,
        // followed by the shared Offline parent PreMatchIntro handler.
        const bool born =
            OfflineSetup::BirthSelectedCounselorForPreMatch(gameMode);

        if (!born)
        {
            Logger::Error(
                "18L-AD COUNSEL HandlePreMatchIntro: counselor birth failed; refusing the stock human-Jason override");
        }

        if (g_OfflineParent958)
            g_OfflineParent958(gameMode);
    }
    else if (g_OriginalOfflineBots958)
    {
        g_OriginalOfflineBots958(gameMode);
    }

    if (counsel)
    {
        uint32_t n = g_958TraceCount.load();
        if (n <= 4)
        {
            Logger::Success(
                "18L-AD COUNSEL HandlePreMatchIntro RETURN: local counselor birth + Offline parent path complete | " +
                DescribeLocalState());
        }
    }
}

static bool InstallLifecycleTraceHooks()
{
    if (g_LifecycleTraceHooksInstalled.load())
        return true;

    uintptr_t base =
        (uintptr_t)GetModuleHandleW(nullptr);

    if (!base)
        return false;

    uint8_t* fn818 =
        (uint8_t*)(base + RVA_OfflineBots818);

    uint8_t* fn958 =
        (uint8_t*)(base + RVA_OfflineBots958);

    uint8_t* parent818 =
        (uint8_t*)(base + RVA_OfflineParent818);

    uint8_t* parent958 =
        (uint8_t*)(base + RVA_OfflineParent958);

    uint8_t* requestExit =
        (uint8_t*)(base + RVA_RequestExit);

    const uint8_t sig818[] =
    {
        0x48,0x89,0x4C,0x24,0x08,0x55,0x53,0x56
    };

    const uint8_t sig958[] =
    {
        0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74
    };

    const uint8_t sigParent818[] =
    {
        0x48,0x8B,0xC4,0x48,0x81,0xEC,0x88,0x00
    };

    const uint8_t sigParent958[] =
    {
        0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83
    };

    const uint8_t sigRequestExit[] =
    {
        0x40,0x53,0x48,0x83,0xEC,0x30,0x80,0x3D,
        0xCB,0xC9,0x9E,0x02,0x05,0x0F,0xB6,0xD9
    };

    if (!Memory::IsReadable(fn818, sizeof(sig818)) ||
        !Memory::IsReadable(fn958, sizeof(sig958)) ||
        !Memory::IsReadable(parent818, sizeof(sigParent818)) ||
        !Memory::IsReadable(parent958, sizeof(sigParent958)) ||
        !Memory::IsReadable(requestExit, sizeof(sigRequestExit)) ||
        std::memcmp(fn818, sig818, sizeof(sig818)) != 0 ||
        std::memcmp(fn958, sig958, sizeof(sig958)) != 0 ||
        std::memcmp(parent818, sigParent818, sizeof(sigParent818)) != 0 ||
        std::memcmp(parent958, sigParent958, sizeof(sigParent958)) != 0 ||
        std::memcmp(requestExit, sigRequestExit, sizeof(sigRequestExit)) != 0)
    {
        Logger::Error(
            "18L-AD: OfflineBots/Offline-parent lifecycle signatures mismatch; counselor hooks NOT installed");
        return false;
    }

    g_OfflineParent818 =
        reinterpret_cast<OfflineLifecycleFn>(parent818);
    g_OfflineParent958 =
        reinterpret_cast<OfflineLifecycleFn>(parent958);

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK &&
        initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        Logger::Error(
            "18L-AC: MH_Initialize failed for lifecycle trace hooks");
        return false;
    }

    if (MH_CreateHook(
            reinterpret_cast<LPVOID>(fn818),
            reinterpret_cast<LPVOID>(&OfflineBots818TraceHook),
            reinterpret_cast<LPVOID*>(&g_OriginalOfflineBots818)) != MH_OK)
    {
        Logger::Error(
            "18L-AC: failed to create +0x818 pass-through trace hook");
        return false;
    }

    if (MH_CreateHook(
            reinterpret_cast<LPVOID>(fn958),
            reinterpret_cast<LPVOID>(&OfflineBots958TraceHook),
            reinterpret_cast<LPVOID*>(&g_OriginalOfflineBots958)) != MH_OK)
    {
        Logger::Error(
            "18L-AC: failed to create +0x958 pass-through trace hook");
        return false;
    }

    if (MH_CreateHook(
            reinterpret_cast<LPVOID>(requestExit),
            reinterpret_cast<LPVOID>(&RequestExitTraceHook),
            reinterpret_cast<LPVOID*>(&g_OriginalRequestExit)) != MH_OK)
    {
        Logger::Error(
            "18L-AE: failed to create RequestExit diagnostic hook");
        return false;
    }

    if (MH_EnableHook(fn818) != MH_OK ||
        MH_EnableHook(fn958) != MH_OK ||
        MH_EnableHook(requestExit) != MH_OK)
    {
        Logger::Error(
            "18L-AC: failed to enable lifecycle trace hooks");
        return false;
    }

    g_LifecycleTraceHooksInstalled.store(true);

    Logger::Success(
        "18L-AE: donor-sequence counselor lifecycle hooks + forced-exit caller trace installed | COUNSEL +0x958=birth before intro | COUNSEL +0x818=parent then AI Jason | stock Jason route untouched");

    return true;
}

static DWORD WINAPI BackendWorker(LPVOID)
{
    Logger::Success("ResurrectedOfflineBots backend loaded");
    Logger::Debug(
        "18L-AC: CORRECT NATIVE COUNSEL ROUTE foundation | ILLBackendBlueprintLibrary::RequestOfflineMode redirect | OfflineBots lifecycle trace only | no Jason->counselor possession swap | frozen AI auto-start disabled");

    bool engineReady = false;
    bool f1WasDown = false;
    bool f3WasDown = false;
    bool f6WasDown = false;

    while (InterlockedCompareExchange(&g_Running, 1, 1) != 0)
    {
        if (!engineReady)
        {
            engineReady = Engine::Initialize();
            if (!engineReady)
            {
                Logger::Debug("Engine not ready; retrying");
                Sleep(1000);
                continue;
            }

            Logger::Success("Resurrected engine ready");
            InstallDonorLooseFileReadFallback();
            InstallLifecycleTraceHooks();
            Logger::Success(
                "18L-AF SAFETY: donor loose-file fallback uses Resurrected's intact aligned buffer/copy completion path; lifecycle hooks remain route-scoped");
        }

        // Frontend counselor selection + synchronous COUNSEL route coordinator.
        // Deliberately do NOT call frozen AI TickAIOnly in this foundation test.
        // IsInGame also installs the existing game-thread setup-request bridge.
        // Calling it only after the route latch left the selected counselor
        // queued throughout the frontend and native map/settings picker.
        const bool inGame = Engine::IsInGame();
        if (inGame && func)
        {
            // Cache spawn assets only; queued F1/F3 work is still consumed by
            // the established game-thread ProcessEvent bridge.
            func->TickAIOnly();

            const SHORT f1State = GetAsyncKeyState(VK_F1);
            const SHORT f3State = GetAsyncKeyState(VK_F3);
            const SHORT f6State = GetAsyncKeyState(VK_F6);
            const bool f1Down = (f1State & 0x8000) != 0;
            const bool f3Down = (f3State & 0x8000) != 0;
            const bool f6Down = (f6State & 0x8000) != 0;
            const bool f1Pressed = (f1State & 0x0001) != 0;
            const bool f3Pressed = (f3State & 0x0001) != 0;
            const bool f6Pressed = (f6State & 0x0001) != 0;
            if ((f1Pressed || f1Down) && !f1WasDown)
            {
                const bool queued = func->QueueFirstJasonSandbox();
                Logger::Debug(std::string("F1 additional Jason request ") +
                    (queued ? "queued" : "rejected/pending"));
            }
            if ((f3Pressed || f3Down) && !f3WasDown)
            {
                const bool queued = QueueCounselorBotRequest();
                Logger::Debug(std::string("F3 additional counselor request ") +
                    (queued ? "queued" : "rejected/pending"));
            }
            if ((f6Pressed || f6Down) && !f6WasDown)
            {
                FrozenJasonBridge::QueueMaskPickupDiagnostic();
                Logger::Debug("F6 mask pickup diagnostic queued");
            }
            f1WasDown = f1Down;
            f3WasDown = f3Down;
            f6WasDown = f6Down;
        }
        else
        {
            f1WasDown = false;
            f3WasDown = false;
            f6WasDown = false;
        }
        OfflineSetup::TickWorker();

        Sleep(50);
    }

    return 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_QueueJason(LPVOID)
{
    return (func && func->QueueFirstJasonSandbox()) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_QueueCounselor(LPVOID)
{
    return QueueCounselorBotRequest() ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_Ping(LPVOID)
{
    return 0x524F424Fu; // "ROBO"
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_SetMapIndex(LPVOID value)
{
    return OfflineSetup::SetMapIndex((int32_t)(uintptr_t)value) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_SetJasonIndex(LPVOID value)
{
    return OfflineSetup::SetJasonIndex((int32_t)(uintptr_t)value) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_SetPlayerCounselorIndex(LPVOID value)
{
    return OfflineSetup::SetPlayerCounselorIndex((int32_t)(uintptr_t)value) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_SetDifficulty(LPVOID value)
{
    return OfflineSetup::SetDifficulty((int32_t)(uintptr_t)value) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_SetCounselorCount(LPVOID value)
{
    return OfflineSetup::SetCounselorCount((int32_t)(uintptr_t)value) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_SetWeather(LPVOID value)
{
    return OfflineSetup::SetWeather((int32_t)(uintptr_t)value) ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_ArmSelectedSetup(LPVOID)
{
    return OfflineSetup::QueueArmSelectedSetup() ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_QueueSelectedJason(LPVOID)
{
    return OfflineSetup::QueueSelectedJason() ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_QueueSelectedCounselor(LPVOID)
{
    return OfflineSetup::QueueSelectedCounselor() ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_ApplyGameSetupPreset(LPVOID)
{
    return OfflineSetup::QueueApplyGameSetupPreset() ? 1u : 0u;
}

extern "C" __declspec(dllexport) DWORD WINAPI ROB_DumpCounselorRoster(LPVOID)
{
    return OfflineSetup::QueueDumpCounselorRoster() ? 1u : 0u;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, BackendWorker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        InterlockedExchange(&g_Running, 0);
    }

    return TRUE;
}
