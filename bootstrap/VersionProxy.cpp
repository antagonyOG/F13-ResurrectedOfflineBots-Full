#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    INIT_ONCE g_VersionInit = INIT_ONCE_STATIC_INIT;
    HMODULE g_SystemVersion = nullptr;

    using GetFileVersionInfoAFn = BOOL (WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID);
    using GetFileVersionInfoWFn = BOOL (WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using GetFileVersionInfoByHandleFn = BOOL (WINAPI*)(DWORD, HANDLE, DWORD, LPVOID);
    using GetFileVersionInfoExAFn = BOOL (WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
    using GetFileVersionInfoExWFn = BOOL (WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
    using GetFileVersionInfoSizeAFn = DWORD (WINAPI*)(LPCSTR, LPDWORD);
    using GetFileVersionInfoSizeWFn = DWORD (WINAPI*)(LPCWSTR, LPDWORD);
    using GetFileVersionInfoSizeExAFn = DWORD (WINAPI*)(DWORD, LPCSTR, LPDWORD);
    using GetFileVersionInfoSizeExWFn = DWORD (WINAPI*)(DWORD, LPCWSTR, LPDWORD);
    using VerFindFileAFn = DWORD (WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
    using VerFindFileWFn = DWORD (WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
    using VerInstallFileAFn = DWORD (WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
    using VerInstallFileWFn = DWORD (WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
    using VerLanguageNameAFn = DWORD (WINAPI*)(DWORD, LPSTR, DWORD);
    using VerLanguageNameWFn = DWORD (WINAPI*)(DWORD, LPWSTR, DWORD);
    using VerQueryValueAFn = BOOL (WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT);
    using VerQueryValueWFn = BOOL (WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

#define DECLARE_VERSION_API(name) name##Fn g_##name = nullptr
    DECLARE_VERSION_API(GetFileVersionInfoA);
    DECLARE_VERSION_API(GetFileVersionInfoW);
    DECLARE_VERSION_API(GetFileVersionInfoByHandle);
    DECLARE_VERSION_API(GetFileVersionInfoExA);
    DECLARE_VERSION_API(GetFileVersionInfoExW);
    DECLARE_VERSION_API(GetFileVersionInfoSizeA);
    DECLARE_VERSION_API(GetFileVersionInfoSizeW);
    DECLARE_VERSION_API(GetFileVersionInfoSizeExA);
    DECLARE_VERSION_API(GetFileVersionInfoSizeExW);
    DECLARE_VERSION_API(VerFindFileA);
    DECLARE_VERSION_API(VerFindFileW);
    DECLARE_VERSION_API(VerInstallFileA);
    DECLARE_VERSION_API(VerInstallFileW);
    DECLARE_VERSION_API(VerLanguageNameA);
    DECLARE_VERSION_API(VerLanguageNameW);
    DECLARE_VERSION_API(VerQueryValueA);
    DECLARE_VERSION_API(VerQueryValueW);
#undef DECLARE_VERSION_API

    struct GameWindowSearch
    {
        DWORD ProcessId;
        HWND Window;
    };

    BOOL CALLBACK FindVisibleGameWindow(HWND window, LPARAM value)
    {
        auto* search = reinterpret_cast<GameWindowSearch*>(value);
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId != search->ProcessId || !IsWindowVisible(window))
            return TRUE;

        RECT client{};
        if (!GetClientRect(window, &client) ||
            client.right - client.left < 800 ||
            client.bottom - client.top < 600)
        {
            return TRUE;
        }

        search->Window = window;
        return FALSE;
    }

    bool WaitForStableGameWindow()
    {
        const DWORD processId = GetCurrentProcessId();
        const ULONGLONG deadline = GetTickCount64() + 120000;
        while (GetTickCount64() < deadline)
        {
            GameWindowSearch search{ processId, nullptr };
            EnumWindows(&FindVisibleGameWindow,
                reinterpret_cast<LPARAM>(&search));
            if (search.Window)
            {
                // The UE window exists several seconds before GWorld and the
                // object arrays are safe for the frozen initializer.  Match
                // the proven late-injection timing used by the old controller.
                Sleep(8000);
                return true;
            }
            Sleep(250);
        }
        return false;
    }

    BOOL CALLBACK InitializeSystemVersion(PINIT_ONCE, PVOID, PVOID*)
    {
        wchar_t systemDirectory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return FALSE;

        std::filesystem::path versionPath(systemDirectory);
        versionPath /= L"version.dll";
        g_SystemVersion = LoadLibraryW(versionPath.c_str());
        if (!g_SystemVersion)
            return FALSE;

#define RESOLVE_VERSION_API(name) \
        g_##name = reinterpret_cast<name##Fn>(GetProcAddress(g_SystemVersion, #name))
        RESOLVE_VERSION_API(GetFileVersionInfoA);
        RESOLVE_VERSION_API(GetFileVersionInfoW);
        RESOLVE_VERSION_API(GetFileVersionInfoByHandle);
        RESOLVE_VERSION_API(GetFileVersionInfoExA);
        RESOLVE_VERSION_API(GetFileVersionInfoExW);
        RESOLVE_VERSION_API(GetFileVersionInfoSizeA);
        RESOLVE_VERSION_API(GetFileVersionInfoSizeW);
        RESOLVE_VERSION_API(GetFileVersionInfoSizeExA);
        RESOLVE_VERSION_API(GetFileVersionInfoSizeExW);
        RESOLVE_VERSION_API(VerFindFileA);
        RESOLVE_VERSION_API(VerFindFileW);
        RESOLVE_VERSION_API(VerInstallFileA);
        RESOLVE_VERSION_API(VerInstallFileW);
        RESOLVE_VERSION_API(VerLanguageNameA);
        RESOLVE_VERSION_API(VerLanguageNameW);
        RESOLVE_VERSION_API(VerQueryValueA);
        RESOLVE_VERSION_API(VerQueryValueW);
#undef RESOLVE_VERSION_API

        return g_GetFileVersionInfoA && g_GetFileVersionInfoW &&
            g_GetFileVersionInfoByHandle &&
            g_GetFileVersionInfoExA && g_GetFileVersionInfoExW &&
            g_GetFileVersionInfoSizeA && g_GetFileVersionInfoSizeW &&
            g_GetFileVersionInfoSizeExA && g_GetFileVersionInfoSizeExW &&
            g_VerFindFileA && g_VerFindFileW &&
            g_VerInstallFileA && g_VerInstallFileW &&
            g_VerLanguageNameA && g_VerLanguageNameW &&
            g_VerQueryValueA && g_VerQueryValueW;
    }

    bool EnsureSystemVersion()
    {
        return InitOnceExecuteOnce(
            &g_VersionInit,
            &InitializeSystemVersion,
            nullptr,
            nullptr) != FALSE;
    }

    void WriteBootstrapStatus(const wchar_t* message)
    {
        wchar_t temporary[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temporary);
        if (!length || length >= MAX_PATH)
            return;

        std::filesystem::path logPath(temporary);
        logPath /= L"ResurrectedOfflineBots-Bootstrap.log";
        std::wofstream output(logPath, std::ios::app);
        if (output)
            output << GetTickCount64() << L" | " << message << L"\n";
    }

    DWORD WINAPI LoadOfflineBots(LPVOID moduleValue)
    {
        if (!WaitForStableGameWindow())
        {
            WriteBootstrapStatus(L"ERROR: stable Resurrected window did not appear");
            return 4;
        }

        HMODULE proxyModule = reinterpret_cast<HMODULE>(moduleValue);
        wchar_t proxyPath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(
            proxyModule,
            proxyPath,
            MAX_PATH);
        if (!length || length >= MAX_PATH)
        {
            WriteBootstrapStatus(L"ERROR: proxy module path unavailable");
            return 1;
        }

        std::filesystem::path backendPath(proxyPath);
        backendPath = backendPath.parent_path() / L"ResurrectedOfflineBots.dll";

        if (GetModuleHandleW(L"ResurrectedOfflineBots.dll"))
        {
            WriteBootstrapStatus(L"Offline Bots backend was already loaded");
            return 0;
        }

        if (!std::filesystem::exists(backendPath))
        {
            WriteBootstrapStatus(L"ERROR: adjacent ResurrectedOfflineBots.dll is missing");
            return 2;
        }

        if (!LoadLibraryW(backendPath.c_str()))
        {
            WriteBootstrapStatus(L"ERROR: LoadLibraryW failed for Offline Bots backend");
            return 3;
        }

        WriteBootstrapStatus(L"Offline Bots backend loaded automatically");
        return 0;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI ProxyGetFileVersionInfoA(
    LPCSTR fileName, DWORD handle, DWORD length, LPVOID data)
{
    return EnsureSystemVersion() &&
        g_GetFileVersionInfoA(fileName, handle, length, data);
}

extern "C" __declspec(dllexport) BOOL WINAPI ProxyGetFileVersionInfoByHandle(
    DWORD flags, HANDLE file, DWORD length, LPVOID data)
{
    return EnsureSystemVersion() &&
        g_GetFileVersionInfoByHandle(flags, file, length, data);
}

extern "C" __declspec(dllexport) BOOL WINAPI ProxyGetFileVersionInfoExA(
    DWORD flags, LPCSTR fileName, DWORD handle, DWORD length, LPVOID data)
{
    return EnsureSystemVersion() &&
        g_GetFileVersionInfoExA(flags, fileName, handle, length, data);
}

extern "C" __declspec(dllexport) BOOL WINAPI ProxyGetFileVersionInfoExW(
    DWORD flags, LPCWSTR fileName, DWORD handle, DWORD length, LPVOID data)
{
    return EnsureSystemVersion() &&
        g_GetFileVersionInfoExW(flags, fileName, handle, length, data);
}

extern "C" __declspec(dllexport) DWORD WINAPI ProxyGetFileVersionInfoSizeA(
    LPCSTR fileName, LPDWORD handle)
{
    if (!EnsureSystemVersion())
        return 0;
    return g_GetFileVersionInfoSizeA(fileName, handle);
}

extern "C" __declspec(dllexport) DWORD WINAPI ProxyGetFileVersionInfoSizeExA(
    DWORD flags, LPCSTR fileName, LPDWORD handle)
{
    if (!EnsureSystemVersion())
        return 0;
    return g_GetFileVersionInfoSizeExA(flags, fileName, handle);
}

extern "C" __declspec(dllexport) DWORD WINAPI ProxyGetFileVersionInfoSizeExW(
    DWORD flags, LPCWSTR fileName, LPDWORD handle)
{
    if (!EnsureSystemVersion())
        return 0;
    return g_GetFileVersionInfoSizeExW(flags, fileName, handle);
}

extern "C" __declspec(dllexport) DWORD WINAPI ProxyGetFileVersionInfoSizeW(
    LPCWSTR fileName,
    LPDWORD handle)
{
    if (!EnsureSystemVersion())
        return 0;
    return g_GetFileVersionInfoSizeW(fileName, handle);
}

extern "C" __declspec(dllexport) BOOL WINAPI ProxyGetFileVersionInfoW(
    LPCWSTR fileName,
    DWORD handle,
    DWORD length,
    LPVOID data)
{
    if (!EnsureSystemVersion())
        return FALSE;
    return g_GetFileVersionInfoW(fileName, handle, length, data);
}

extern "C" __declspec(dllexport) DWORD WINAPI ProxyVerFindFileA(
    DWORD flags, LPCSTR fileName, LPCSTR windowsDirectory,
    LPCSTR appDirectory, LPSTR currentDirectory, PUINT currentLength,
    LPSTR destinationDirectory, PUINT destinationLength)
{
    if (!EnsureSystemVersion())
        return 0;
    return g_VerFindFileA(flags, fileName, windowsDirectory, appDirectory,
        currentDirectory, currentLength, destinationDirectory, destinationLength);
}

extern "C" __declspec(dllexport) DWORD WINAPI ProxyVerFindFileW(
    DWORD flags, LPCWSTR fileName, LPCWSTR windowsDirectory,
    LPCWSTR appDirectory, LPWSTR currentDirectory, PUINT currentLength,
    LPWSTR destinationDirectory, PUINT destinationLength)
{
    if (!EnsureSystemVersion())
        return 0;
    return g_VerFindFileW(flags, fileName, windowsDirectory, appDirectory,
        currentDirectory, currentLength, destinationDirectory, destinationLength);
}

extern "C" __declspec(dllexport) DWORD WINAPI ProxyVerInstallFileA(
    DWORD flags, LPCSTR sourceFileName, LPCSTR destinationFileName,
    LPCSTR sourceDirectory, LPCSTR destinationDirectory,
    LPCSTR currentDirectory, LPSTR temporaryFile, PUINT temporaryLength)
{
    if (!EnsureSystemVersion())
        return 0;
    return g_VerInstallFileA(flags, sourceFileName, destinationFileName,
        sourceDirectory, destinationDirectory, currentDirectory,
        temporaryFile, temporaryLength);
}

extern "C" __declspec(dllexport) DWORD WINAPI ProxyVerInstallFileW(
    DWORD flags, LPCWSTR sourceFileName, LPCWSTR destinationFileName,
    LPCWSTR sourceDirectory, LPCWSTR destinationDirectory,
    LPCWSTR currentDirectory, LPWSTR temporaryFile, PUINT temporaryLength)
{
    if (!EnsureSystemVersion())
        return 0;
    return g_VerInstallFileW(flags, sourceFileName, destinationFileName,
        sourceDirectory, destinationDirectory, currentDirectory,
        temporaryFile, temporaryLength);
}

extern "C" __declspec(dllexport) DWORD WINAPI ProxyVerLanguageNameA(
    DWORD language, LPSTR buffer, DWORD size)
{
    if (!EnsureSystemVersion())
        return 0;
    return g_VerLanguageNameA(language, buffer, size);
}

extern "C" __declspec(dllexport) DWORD WINAPI ProxyVerLanguageNameW(
    DWORD language, LPWSTR buffer, DWORD size)
{
    if (!EnsureSystemVersion())
        return 0;
    return g_VerLanguageNameW(language, buffer, size);
}

extern "C" __declspec(dllexport) BOOL WINAPI ProxyVerQueryValueA(
    LPCVOID block, LPCSTR subBlock, LPVOID* buffer, PUINT length)
{
    return EnsureSystemVersion() &&
        g_VerQueryValueA(block, subBlock, buffer, length);
}

extern "C" __declspec(dllexport) BOOL WINAPI ProxyVerQueryValueW(
    LPCVOID block,
    LPCWSTR subBlock,
    LPVOID* buffer,
    PUINT length)
{
    if (!EnsureSystemVersion())
        return FALSE;
    return g_VerQueryValueW(block, subBlock, buffer, length);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        HANDLE worker = CreateThread(
            nullptr,
            0,
            &LoadOfflineBots,
            module,
            0,
            nullptr);
        if (worker)
            CloseHandle(worker);
    }
    return TRUE;
}
