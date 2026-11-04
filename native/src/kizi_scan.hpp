// SPDX-License-Identifier: MIT
// Searches of the game exe from inside the game: readable section ranges, masked byte patterns, strings and
// rip-relative references. Works on any F1 25 build (code in .xtls on Steam, .bss on others), without bindings.
#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include "kizi_mem.hpp"

namespace kizi {

struct Range {
    uintptr_t a, b;
};

inline bool readable_protect(DWORD p)
{
    if (p & (PAGE_GUARD | PAGE_NOACCESS))
        return false;
    return p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                PAGE_EXECUTE_WRITECOPY);
}

// Readable parts of the image's sections. code = the sections the game runs code from (executable ones and the
// names .xtls / .bss / .text); otherwise every other section (strings, tables).
inline std::vector<Range> image_ranges(bool code)
{
    std::vector<Range> out;
    auto base = image_base();
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        char name[9] = {};
        memcpy(name, sec->Name, 8);
        std::string_view n(name);
        bool is_code = (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) || n == ".xtls" || n == ".bss" || n == ".text";
        if (is_code != code)
            continue;
        uintptr_t a = base + sec->VirtualAddress, end = a + sec->Misc.VirtualSize;
        while (a < end) {
            MEMORY_BASIC_INFORMATION m;
            if (!VirtualQuery(reinterpret_cast<void *>(a), &m, sizeof m))
                break;
            uintptr_t rend = std::min<uintptr_t>(end, reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize);
            if (m.State == MEM_COMMIT && readable_protect(m.Protect))
                out.push_back({a, rend});
            a = rend;
        }
    }
    return out;
}

// "48 8B ?? 05" style pattern.
struct Pattern {
    std::vector<uint8_t> b, m;                       // bytes, mask (0 = wildcard)
    explicit Pattern(const char *s)
    {
        while (*s) {
            if (*s == ' ') {
                s++;
                continue;
            }
            if (s[0] == '?') {
                b.push_back(0), m.push_back(0);
                s += s[1] == '?' ? 2 : 1;
                continue;
            }
            b.push_back(uint8_t(strtoul(std::string(s, 2).c_str(), nullptr, 16))), m.push_back(0xff);
            s += 2;
        }
    }
    bool at(const uint8_t *p) const
    {
        for (size_t i = 0; i < b.size(); i++)
            if ((p[i] & m[i]) != b[i])
                return false;
        return true;
    }
};

// Every match in the ranges (the pattern's first byte must not be a wildcard); fn(address) returns false to stop.
template <class F> void find_all(const std::vector<Range> &ranges, const Pattern &p, F fn)
{
    for (auto r : ranges) {
        if (r.b - r.a < p.b.size())
            continue;
        auto s = reinterpret_cast<const uint8_t *>(r.a), last = reinterpret_cast<const uint8_t *>(r.b - p.b.size());
        for (auto q = s; q <= last; q++) {
            q = static_cast<const uint8_t *>(memchr(q, p.b[0], last - q + 1));
            if (!q)
                break;
            if (p.at(q) && !fn(reinterpret_cast<uintptr_t>(q)))
                return;
        }
    }
}

// Address of a NUL-terminated string that is not the tail of a longer one.
inline uintptr_t find_string(const std::vector<Range> &data, const char *text)
{
    size_t n = strlen(text) + 1;
    for (auto r : data)
        for (auto q = reinterpret_cast<const char *>(r.a) + 1; q + n <= reinterpret_cast<const char *>(r.b); q++) {
            q = static_cast<const char *>(memchr(q, text[0], reinterpret_cast<const char *>(r.b) - q));
            if (!q || q + n > reinterpret_cast<const char *>(r.b))
                break;
            if (q[-1] == 0 && memcmp(q, text, n) == 0)
                return reinterpret_cast<uintptr_t>(q);
        }
    return 0;
}

inline int32_t rel32(uintptr_t a) { return *reinterpret_cast<const int32_t *>(a); }

// A short string read from the image (empty if `a` is not in it).
inline std::string image_str(uintptr_t a, size_t max = 64)
{
    std::string s;
    char c = 0;
    while (a && in_image(a) && s.size() < max && read(a++, c) && c)
        s += c;
    return s;
}

}  // namespace kizi
