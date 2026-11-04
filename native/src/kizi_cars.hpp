// SPDX-License-Identifier: MIT
// Every car's ControllerSet (who drives it), found in the game's code at run time (from the F1 hub, core.py):
//   ControllerSet +0x20 human controller, +0x28 AI controller, +0x30 flags dword (a set flag = the AI drives the car,
//   e.g. PIT_LANE, TERMINAL_DAMAGE), mode dword stored xor-ed with a key dword (mode 0 = human, 1 = AI).
// The car table is a vector of ControllerSet*, indexed by vehicle id. Used by Manual Pit Lane and DNF Parking.
#pragma once
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include "kizi_scan.hpp"

namespace kizi {

constexpr uint32_t CS_FLAGS = 0x30;

struct CarTable {
    uintptr_t global;                     // manager pointer
    uint8_t vec, mode, key;               // manager+vec = vector of ControllerSet*; set+mode ^ set+key = mode
};

inline uint64_t q64(uintptr_t a)
{
    uint64_t v = 0;
    return read(a, v) ? v : 0;
}

inline uint32_t u32(uintptr_t a)
{
    uint32_t v = 0;
    return read(a, v) ? v : 0;
}

// Vehicle input code: mov rcx,[rip+G] ... mov rcx,[rcx+V]; mov rcx,[rcx+rdx*8]; mov edx,[rcx+M]; xor edx,[rcx+K]
inline std::vector<CarTable> find_car_tables(const std::vector<Range> &code)
{
    std::vector<CarTable> out;
    find_all(code, Pattern("48 8B 49 ?? 48 8B 0C D1 8B 51 ?? 33 51 ??"), [&](uintptr_t a) {
        for (uintptr_t s = a - 8; s >= a - 7 - 24; s--) {              // nearest `mov rcx,[rip+G]` 1..24 bytes before
            if (Pattern("48 8B 0D").at(reinterpret_cast<const uint8_t *>(s))) {
                CarTable t{s + 7 + rel32(s + 3), *reinterpret_cast<uint8_t *>(a + 3),
                           *reinterpret_cast<uint8_t *>(a + 10), *reinterpret_cast<uint8_t *>(a + 13)};
                bool dup = false;
                for (auto &o : out)
                    dup |= o.global == t.global && o.vec == t.vec && o.mode == t.mode && o.key == t.key;
                if (!dup && in_image(t.global))
                    out.push_back(t);
                break;
            }
        }
        return true;
    });
    return out;
}

// Bit number of a ControllerSet flag by name (a pointer array in the exe whose entry N names bit N), or -1.
inline int controller_flag_bit(const std::vector<Range> &data, const char *name)
{
    uintptr_t first = find_string(data, "PIT_LANE");
    if (!first)
        return -1;
    for (auto r : data)
        for (uintptr_t a = (r.a + 7) & ~uintptr_t(7); a + 8 <= r.b; a += 8) {
            if (*reinterpret_cast<const uint64_t *>(a) != first)
                continue;
            for (uintptr_t b = a; b >= r.a && b + 8 * 32 > a; b -= 8) {     // the table may start before PIT_LANE
                std::vector<std::string> names;
                for (int i = 0; i < 32 && b + 8 * i + 8 <= r.b; i++) {
                    std::string s = image_str(*reinterpret_cast<const uint64_t *>(b + 8 * i), 32);
                    if (s.size() < 2 || s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ_") != std::string::npos)
                        break;
                    names.push_back(s);
                }
                bool td = std::find(names.begin(), names.end(), "TERMINAL_DAMAGE") != names.end();
                if (td && names.size() >= 10)
                    for (size_t i = 0; i < names.size(); i++)
                        if (names[i] == name)
                            return int(i);
                if (b == r.a)
                    break;
            }
        }
    return -1;
}

// The table's ControllerSets by vehicle id (empty slots 0); empty if no race is loaded.
inline std::vector<uintptr_t> car_sets(const CarTable &t)
{
    std::vector<uintptr_t> out;
    uintptr_t mgr = q64(t.global);
    uint64_t b = mgr ? q64(mgr + t.vec) : 0, e = mgr ? q64(mgr + t.vec + 8) : 0;
    if (!b || e <= b || (e - b) / 8 > 64 || (e - b) / 8 < 8)
        return out;
    for (uint64_t p = b; p < e; p += 8)
        out.push_back(q64(p));
    return out;
}

// Your car (one human car, offline): its human controller's vtable differs from the AI one every car shares.
inline uintptr_t player_set(const std::vector<uintptr_t> &sets, int &vid)
{
    vid = -1;
    std::vector<std::pair<uint64_t, int>> ai;
    for (auto s : sets)
        if (uint64_t v = s ? q64(q64(s + 0x28)) : 0) {
            bool f = false;
            for (auto &x : ai)
                if (x.first == v)
                    x.second++, f = true;
            if (!f)
                ai.push_back({v, 1});
        }
    if (ai.empty())
        return 0;
    uint64_t ai_vt = std::max_element(ai.begin(), ai.end(), [](auto &a, auto &b) { return a.second < b.second; })->first;
    uintptr_t found = 0;
    int n = 0;
    for (size_t i = 0; i < sets.size(); i++) {
        uintptr_t s = sets[i];
        uint64_t hp = s ? q64(s + 0x20) : 0, ap = s ? q64(s + 0x28) : 0;
        uint64_t hv = hp ? q64(hp) : 0;
        if (hp && ap && q64(ap) == ai_vt && hv && hv != ai_vt && in_image(hv))
            found = s, vid = int(i), n++;
    }
    if (n != 1)
        vid = -1;
    return n == 1 ? found : 0;
}

// Hand a car back to the human driver (mode 0).
inline void set_human(const CarTable &t, uintptr_t set)
{
    uint32_t mode = 0 ^ u32(set + t.key);
    write(set + t.mode, &mode, 4);
}

// The game database's hash map: `mov rax,[rip+MAP]; mov rcx,[rax+0B8h]; imul rcx,...`; entries {hash, node*} at
// [map+0C0h], mask at [map+0D8h]; a value sits decoded at node+8. Returns the value's address, or 0.
inline uint64_t fnv1a64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++)
        h = (h ^ uint8_t(*s)) * 0x100000001b3ull;
    return h;
}

inline uintptr_t db_value(const std::vector<Range> &code, const char *key)
{
    uint64_t want = fnv1a64(key);
    uintptr_t found = 0;
    std::vector<uintptr_t> maps;
    find_all(code, Pattern("48 8B 05 ?? ?? ?? ?? 48 8B 88 B8 00 00 00 48 0F AF"), [&](uintptr_t a) {
        uintptr_t g = a + 7 + rel32(a + 3);
        if (std::find(maps.begin(), maps.end(), g) == maps.end())
            maps.push_back(g);
        return true;
    });
    for (uintptr_t g : maps) {
        uintptr_t mp = q64(g), base = mp ? q64(mp + 0xc0) : 0, mask = mp ? q64(mp + 0xd8) : 0;
        if (!base || !mask || mask >= (1u << 24))
            continue;
        std::vector<uint64_t> buf(2 * 4096);               // 4096 entries {hash, node*} per read
        uint64_t total = mask + 1 + 512;
        for (uint64_t e = 0; e < total && !found; e += 4096) {
            uint64_t n = std::min<uint64_t>(4096, total - e);
            if (!read(base + e * 16, buf.data(), size_t(n * 16)))
                break;
            for (uint64_t i = 0; i < n; i++)
                if (buf[2 * i] == want && buf[2 * i + 1]) {
                    found = buf[2 * i + 1] + 8;
                    break;
                }
        }
        if (found)
            break;
    }
    return found;
}

}  // namespace kizi
