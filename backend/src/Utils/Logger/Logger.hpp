#pragma once
#include <string>

class Logger
{
public:
    static void Success(const std::string& text);
    static void Error(const std::string& text);
    static void Debug(const std::string& text);
    static void Prompt(const std::string& text);
    static void Countdown(int seconds);
    static void InlineSuccess(const std::string& text);

private:
    static void Write(const char* level, const std::string& text);
};
