#include "Memory.hpp"
#include "Logger/Logger.hpp"
#include <Psapi.h>

#define INRANGE(x,a,b)    (x >= a && x <= b) 
#define getBits( x )    (INRANGE((x&(~0x20)),'A','F') ? ((x&(~0x20)) - 'A' + 0xa) : (INRANGE(x,'0','9') ? x - '0' : 0))
#define getByte( x )    (getBits(x[0]) << 4 | getBits(x[1]))

namespace Memory {
    bool IsReadable(const void* ptr, size_t size)
    {
        if (!ptr || size == 0) return false;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        DWORD prot = mbi.Protect & 0xFF;
        return (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE);
    }

    bool IsValidPointer(uintptr_t ptr) {
        return ptr > 0x10000 && ptr < 0x7FFFFFFFFFFF; 
    }

    bool IsLikelyPointer(void* ptr) {
        uintptr_t addr = (uintptr_t)ptr;
        return addr > 0x10000 && addr < 0x7FFFFFFFFFFF;
    }


    uintptr_t PatternScan(const char* signature)
    {
        static auto pattern_to_byte = [](const char* pattern) {
            auto bytes = std::vector<int>{};
            auto start = const_cast<char*>(pattern);
            auto end = const_cast<char*>(pattern) + strlen(pattern);

            for (auto current = start; current < end; ++current) {
                if (*current == '?') {
                    ++current;
                    if (*current == '?')
                        ++current;
                    bytes.push_back(-1);
                }
                else {
                    bytes.push_back(strtoul(current, &current, 16));
                }
            }
            return bytes;
        };

        auto dosHeader = (PIMAGE_DOS_HEADER)GetModuleHandleA(NULL);
        auto ntHeaders = (PIMAGE_NT_HEADERS)((std::uint8_t*)dosHeader + dosHeader->e_lfanew);

        auto sizeOfImage = ntHeaders->OptionalHeader.SizeOfImage;
        auto patternBytes = pattern_to_byte(signature);
        auto scanBytes = reinterpret_cast<std::uint8_t*>(dosHeader);

        auto s = patternBytes.size();
        auto d = patternBytes.data();

        for (auto i = 0ul; i < sizeOfImage - s; ++i) {
            bool found = true;
            for (auto j = 0ul; j < s; ++j) {
                if (scanBytes[i + j] != d[j] && d[j] != -1) {
                    found = false;
                    break;
                }
            }
            if (found) {
                return (uintptr_t)&scanBytes[i];
            }
        }
        return 0;
    }
}
