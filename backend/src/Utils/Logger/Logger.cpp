#include "Logger.hpp"
#include <Windows.h>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <cstdio>

static std::mutex g_LogMutex;
static HANDLE g_LogFile = INVALID_HANDLE_VALUE;

static std::string Timestamp()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32]{};
    sprintf_s(buf, "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

static std::string LogPath()
{
    char temp[MAX_PATH]{};
    DWORD n = GetTempPathA(MAX_PATH, temp);
    if (n == 0 || n >= MAX_PATH)
        return "ResurrectedOfflineBots-18L-AC.log";
    return std::string(temp) + "ResurrectedOfflineBots-18L-AC.log";
}

void Logger::Write(const char* level, const std::string& text)
{
    std::lock_guard<std::mutex> lock(g_LogMutex);
    const std::string line = "[" + Timestamp() + "] [" + level + "] " + text + "\r\n";
    // This logger is called from the hooked game thread. Opening and closing
    // the file for every AI trace line caused visible frame hitches. Keep one
    // append handle for the process lifetime; the Windows cache absorbs the
    // small writes without repeatedly paying filesystem-open latency.
    if (g_LogFile == INVALID_HANDLE_VALUE)
    {
        g_LogFile = CreateFileA(
            LogPath().c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
    }

    if (g_LogFile != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(
            g_LogFile,
            line.data(),
            static_cast<DWORD>(line.size()),
            &written,
            nullptr);
    }

    // Debug listeners can impose another synchronous cost. Preserve immediate
    // debugger visibility only for errors and prompts; normal trace data is
    // still written to the authoritative file above.
    if ((level[0] == '-' || level[0] == '?') && level[1] == '\0')
        OutputDebugStringA(line.c_str());
}

void Logger::Success(const std::string& text) { Write("+", text); }
void Logger::Error(const std::string& text)   { Write("-", text); }
void Logger::Debug(const std::string& text)   { Write("=", text); }
void Logger::Prompt(const std::string& text)  { Write("?", text); }
void Logger::InlineSuccess(const std::string& text) { Write("+", text); }
void Logger::Countdown(int seconds)
{
    for (int i = seconds; i >= 0; --i)
    {
        Write("+", "Closing in " + std::to_string(i) + " seconds");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
