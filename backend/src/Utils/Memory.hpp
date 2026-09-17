#pragma once
#include <windows.h>
#include <vector>
#include <string>

namespace Memory {
    uintptr_t PatternScan(const char* signature);
    bool IsReadable(const void* ptr, size_t size);
    bool IsValidPointer(uintptr_t ptr);
    bool IsLikelyPointer(void* ptr);
}
