#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {
INIT_ONCE g_init = INIT_ONCE_STATIC_INIT;
HMODULE g_system = nullptr;
using InitializeFn = HRESULT (WINAPI*)(unsigned int, float, void*);
using CalculateFn = void (WINAPI*)(const void*, const void*, const void*, unsigned int, void*);
InitializeFn g_initialize = nullptr;
CalculateFn g_calculate = nullptr;

std::wstring ModulePath(HMODULE module) {
    std::vector<wchar_t> buffer(32768);
    DWORD count = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    return count && count < buffer.size() ? std::wstring(buffer.data(), count) : L"<unavailable>";
}

std::wstring Sha256(const std::wstring& path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) return L"<file unreadable>";
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, resultSize = 0;
    std::vector<unsigned char> object, digest(32);
    std::wstring value = L"<hash failed>";
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) goto done;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0) < 0) goto done;
    object.resize(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) goto done;
    {
        char block[65536];
        while (input.read(block, sizeof(block)) || input.gcount()) {
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(block), static_cast<ULONG>(input.gcount()), 0) < 0) goto done;
        }
    }
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) goto done;
    {
        std::wostringstream stream;
        stream << std::uppercase << std::hex << std::setfill(L'0');
        for (unsigned char byte : digest) stream << std::setw(2) << static_cast<unsigned int>(byte);
        value = stream.str();
    }
done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return value;
}

void Log(const std::wstring& message) {
    wchar_t temp[MAX_PATH]{};
    DWORD count = GetTempPathW(MAX_PATH, temp);
    if (!count || count >= MAX_PATH) return;
    std::wofstream output(std::filesystem::path(temp) / L"ResurrectedOfflineBots-Bootstrap.log", std::ios::app);
    if (output) output << GetTickCount64() << L" | " << message << L'\n';
}

BOOL CALLBACK InitSystem(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t directory[MAX_PATH]{};
    UINT count = GetSystemDirectoryW(directory, MAX_PATH);
    if (!count || count >= MAX_PATH) return FALSE;
    const auto path = std::filesystem::path(directory) / L"X3DAudio1_7.dll";
    g_system = LoadLibraryW(path.c_str());
    if (!g_system) return FALSE;
    g_initialize = reinterpret_cast<InitializeFn>(GetProcAddress(g_system, "X3DAudioInitialize"));
    g_calculate = reinterpret_cast<CalculateFn>(GetProcAddress(g_system, "X3DAudioCalculate"));
    return g_initialize && g_calculate;
}

bool EnsureSystem() {
    return InitOnceExecuteOnce(&g_init, InitSystem, nullptr, nullptr) != FALSE;
}

struct WindowSearch { DWORD pid; HWND window; };
BOOL CALLBACK FindWindow(HWND window, LPARAM value) {
    auto* search = reinterpret_cast<WindowSearch*>(value);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid || !IsWindowVisible(window)) return TRUE;
    RECT rect{};
    if (!GetClientRect(window, &rect) || rect.right - rect.left < 800 || rect.bottom - rect.top < 600) return TRUE;
    search->window = window;
    return FALSE;
}

DWORD WINAPI LoadBackend(LPVOID moduleValue) {
    auto module = reinterpret_cast<HMODULE>(moduleValue);
    const auto modulePath = ModulePath(module);
    const auto exePath = ModulePath(nullptr);
    Log(L"bootstrap entry point ran: DLL_PROCESS_ATTACH");
    Log(L"selected bootstrap method: application-local X3DAudio1_7.dll proxy");
    Log(L"bootstrap module path: " + modulePath);
    Log(L"Resurrected EXE path: " + exePath);
    typedef LONG (WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtlVersion = reinterpret_cast<RtlGetVersionFn>(ntdll ? GetProcAddress(ntdll, "RtlGetVersion") : nullptr);
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtlVersion && rtlVersion(&version) == 0)
        Log(L"Windows version: " + std::to_wstring(version.dwMajorVersion) + L"." + std::to_wstring(version.dwMinorVersion) + L" build " + std::to_wstring(version.dwBuildNumber));
    else
        Log(L"Windows version: unavailable");

    const auto deadline = GetTickCount64() + 120000;
    bool ready = false;
    while (GetTickCount64() < deadline) {
        WindowSearch search{GetCurrentProcessId(), nullptr};
        EnumWindows(FindWindow, reinterpret_cast<LPARAM>(&search));
        if (search.window) { ready = true; break; }
        Sleep(250);
    }
    if (!ready) { Log(L"backend NOT LOADED: stable game window timeout"); return 4; }
    Sleep(8000);
    // Hash after startup settles so reading the 53 MB EXE does not compete
    // with the game's first asset loads.
    const auto exeHash = Sha256(exePath);
    Log(L"EXE SHA-256: " + exeHash);
    if (exeHash != L"5541268C88B6C02BFB8BDA2D4B07E3E04BB6A03CEF1C5E89163B1E9FBC32A430") {
        Log(L"backend NOT LOADED: unsupported EXE SHA-256");
        return 5;
    }
    const auto backend = std::filesystem::path(modulePath).parent_path() / L"ResurrectedOfflineBots.dll";
    Log(L"backend path: " + backend.wstring());
    if (GetModuleHandleW(L"ResurrectedOfflineBots.dll")) {
        Log(L"backend LOADED: already present in process");
        return 0;
    }
    if (!std::filesystem::exists(backend)) {
        Log(L"backend NOT LOADED: DLL missing");
        return 2;
    }
    if (!LoadLibraryW(backend.c_str())) {
        Log(L"backend NOT LOADED: LoadLibraryW error " + std::to_wstring(GetLastError()));
        return 3;
    }
    Log(L"backend LOADED: LoadLibraryW succeeded");
    return 0;
}
}

extern "C" HRESULT WINAPI ProxyX3DAudioInitialize(unsigned int mask, float speed, void* handle) {
    return EnsureSystem() ? g_initialize(mask, speed, handle) : HRESULT_FROM_WIN32(GetLastError());
}
extern "C" void WINAPI ProxyX3DAudioCalculate(const void* handle, const void* listener,
    const void* emitter, unsigned int flags, void* settings) {
    if (EnsureSystem()) g_calculate(handle, listener, emitter, flags, settings);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE worker = CreateThread(nullptr, 0, LoadBackend, module, 0, nullptr);
        if (worker) CloseHandle(worker);
    }
    return TRUE;
}
