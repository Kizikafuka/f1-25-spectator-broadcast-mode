// SPDX-License-Identifier: MIT
// Spectator/Broadcast Mode (id kizi.spectator-hud): watch the whole grid with the game's own spectator director, keep the
// race running after your crash, park your car and your teammate in the garage. The Swofty 1.6 calls do the work
// (spectator::set_active, follow, the pit lane binding), plus a few small patches of the game's code made while you spectate.
// The idea of keeping the car after terminal damage comes from Terminal Damage by nyannoying
// (https://github.com/nyannoying1337/f1-25-terminal-damage); the code here is our own.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <string_view>
#include <mutex>
#include <vector>

#include <swofty/events.hpp>
#include <swofty/hud.hpp>
#include <swofty/mixin.hpp>
#include <swofty/systems/sessions.hpp>
#include <swofty/systems/spectator.hpp>
#include <swofty/telemetry.hpp>

namespace swofty::game {
using Game_Pitlane_SetVehicleStatus_t = uint64_t(void *, uint64_t, uint64_t);
inline constexpr fn<"Game_Pitlane_SetVehicleStatus", Game_Pitlane_SetVehicleStatus_t> Game_Pitlane_SetVehicleStatus{};
using Sess_SpectatorHud_Hide_t = void();
inline constexpr fn<"Sess_SpectatorHud_Hide", Sess_SpectatorHud_Hide_t> Sess_SpectatorHud_Hide{};
using Ne_Post_BusMessage_t = uint64_t(void *, void *);
inline constexpr fn<"Ne_Post_BusMessage", Ne_Post_BusMessage_t> Ne_Post_BusMessage{};
}

#define ADDON_STR2(x) #x
#define ADDON_STR(x) ADDON_STR2(x)
#ifdef ADDON_VERSION_RAW
#define ADDON_VERSION ADDON_STR(ADDON_VERSION_RAW)
#else
#define ADDON_VERSION "?"
#endif
#define TAG "spectator"

SWOFTY_SETTING(bool, auto_on, true);
SWOFTY_SETTING(std::string_view, hotkey, "F4");
SWOFTY_SETTING(bool, leave_own_car, true);
SWOFTY_SETTING(std::string_view, camera_key, "C");
SWOFTY_SETTING(bool, keep_race, true);
SWOFTY_SETTING(std::string_view, garage_after, "6");
SWOFTY_SETTING(bool, start_garage, false);
SWOFTY_SETTING(bool, park_teammate, true);
SWOFTY_SETTING(bool, bg_run, true);
SWOFTY_SETTING(std::string_view, udp_port, "20777");
SWOFTY_SETTING(std::string_view, udp_forward, "off");
SWOFTY_SETTING(bool, debug_log, true);

namespace {

#define LOG(...) swofty::log("[" TAG "] " __VA_ARGS__)

double now()
{
    return double(GetTickCount64()) / 1000.0;
}

double g_spec_t0 = 0;   // when the director was switched on (0 = off): log times are "+seconds" since then

std::string tstamp()
{
    char b[24];
    if (g_spec_t0 > 0)
        snprintf(b, sizeof b, "+%.2fs", now() - g_spec_t0);
    else
        snprintf(b, sizeof b, "off");
    return b;
}

// Names of the HUD hide reason bits, from the exe (who sets them).
const char *hm_name(int b)
{
    switch (b) {
    case 0: return "spectating-state(0)";
    case 1: return "pause(1)";
    case 4: return "pre-race(4)";
    case 5: return "doughnut(5)";
    case 6: return "spectator-hud-hide(6)";
    case 8: return "ai-pit-state(8)";
    case 9: return "safety-car-tether(9)";
    case 10: return "controller-flag(10)";
    default: return nullptr;
    }
}

std::string hm_text(uint32_t m)
{
    if (!m)
        return "none";
    std::string r;
    for (int b = 0; b < 16; b++)
        if (m >> b & 1) {
            if (!r.empty())
                r += ",";
            r += hm_name(b) ? hm_name(b) : ("reason" + std::to_string(b)).c_str();
        }
    return r;
}

bool game_in_front()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

int key_vk(std::string_view k)
{
    if (k.size() >= 2 && k[0] == 'F') {
        int n = std::atoi(std::string(k.substr(1)).c_str());
        if (n >= 1 && n <= 12)
            return VK_F1 + n - 1;
    }
    return 0;   // "none"
}

const char *rname(swofty::sessions::Result r)
{
    using R = swofty::sessions::Result;
    return r == R::ok ? "ok" : r == R::late ? "late" : r == R::unavailable ? "unavailable" : r == R::blocked ? "blocked" : "invalid";
}

std::atomic<int> g_player{-1};   // your vehicle index, learnt from the session events

std::atomic<bool> kc_broken{false};  // the terminal damage message was dropped for your car
double kc_since = 0;                 // when the message was dropped
std::atomic<uint32_t> kc_parked{0};  // bit mask of the cars parked in the garage on purpose: they stay there
std::atomic<bool> kc_garaged{false}; // your wreck was sent to the garage: it stays there
// The entrants the game reports with its session events: car index, team id, human flag. Used to find your car and your
// teammate before the race (the telemetry feed is not available on every setup).
struct EntrantInfo {
    int car = -1;
    std::string team;
    bool human = false;
};
std::vector<EntrantInfo> g_entrants;
std::mutex g_entrants_mu;

double sg_t0 = 0;                    // race.start seen at this time: the start-in-garage countdown (0 = off / done)
int sg_tries_log = 0;
std::atomic<bool> sg_parked{false};   // start-in-garage: your car was parked (maybe before the race started)
std::atomic<bool> sg_race{false};     // race.start was seen in this session
std::atomic<bool> g_reset_req{false};   // race.start / session.start seen: reset everything on the next tick

void note_player(const swofty::events::Context &ctx, const char *event)
{
    const auto &p = ctx.player();
    if (!p)
        return;
    int car = int(p->car);
    if (g_player.exchange(car) != car)
        LOG("your car is vehicle %d (learnt at %s)", car, event);
}

void spectate_off(const char *why);

void clear_player(const char *event)
{
    // switch the director off before the session is torn down (the game crashed on exit with it still on)
    spectate_off(event);
    if (g_player.exchange(-1) != -1)
        LOG("%s: session over, your car forgotten", event);
}

void watch_player_events()
{
    for (const char *pt : {"session.start", "race.start", "session.state.enter", "session.grid", "session.formation_lap"}) {
        std::string name = pt;
        auto r = swofty::events::on(pt).after([name](std::span<const std::byte> pl, const swofty::events::Context &ctx) {
            if (debug_log)
                LOG("%s GAME EVENT %s (payload %zu bytes)", tstamp().c_str(), name.c_str(), pl.size());
            // a restart or a new session fires race.start / session.start again: start from a clean state
            if (name == "race.start" || name == "session.start") {
                g_reset_req = true;
                if (name == "session.start") {
                    // a new session: the start-in-garage countdown begins now, so the car can be parked while the pre-race
                    // menus (setup, strategy) are still up
                    sg_parked = false;
                    sg_race = false;
                    sg_t0 = now();
                } else {
                    sg_race = true;
                }
                if (start_garage && name == "race.start" && sg_parked.load()) {
                    // parked before the start (or kept parked over a restart): stay in the garage and spectate from now on
                    kc_broken = true;
                    kc_since = now();
                    LOG("%s START IN GARAGE: race.start with your car parked: spectating starts", tstamp().c_str());
                } else {
                    if (name == "race.start" && !sg_parked.load())
                        sg_t0 = now();   // not parked yet (or the option was off before): try again now
                    kc_broken = false;    // at once, before the tick can send the car to the garage again
                    kc_since = 0;
                    kc_garaged = false;
            kc_parked = 0;   // at once: the game puts the car back on the grid now, the wrap must not hold it
                }
            }
            // the entrants list (car, team, human) comes with the session events once the grid is known
            auto parts = ctx.participants();
            if (!parts.empty()) {
                std::vector<EntrantInfo> list;
                for (const auto &e : parts)
                    list.push_back({int(e.car), std::string(e.team.id()), e.human});
                int humans = 0;
                for (const auto &e : list)
                    humans += e.human ? 1 : 0;
                {
                    std::lock_guard<std::mutex> g(g_entrants_mu);
                    g_entrants = std::move(list);
                }
                LOG("%s ENTRANTS from %s: %zu car(s), %d human", tstamp().c_str(), name.c_str(), parts.size(), humans);
            }
            note_player(ctx, name.c_str());
        });
        if (r != swofty::events::Result::ok)
            LOG("event %s: %s", pt, swofty::events::name(r));
    }
    for (const char *pt : {"session.end", "session.unload"}) {
        std::string name = pt;
        auto r = swofty::events::on(pt).after([name](std::span<const std::byte>, const swofty::events::Context &) {
            clear_player(name.c_str());
        });
        if (r != swofty::events::Result::ok)
            LOG("event %s: %s", pt, swofty::events::name(r));
    }
}

bool manual_on = false;          // switched on with the key
bool user_off = false;           // the key switched spectating off: nothing starts it again until the next race start
bool race_over = false;          // the race ended (podium/cutscene): no spectating until the next race start
double cam_kick_at = 0;           // when to press the game's camera key once after spectating went off
double after_off_log = 0;         // when to log the HUD masks after a switch-off
double lost_since = 0;           // the director has followed nobody since then (0 = it follows a car)
bool active = false;             // the director is on (we switched it on)
std::atomic<bool> hud_keep{false};   // keep the spectator HUD up (read by the hud.spectator.visible handler)
double act_started = 0, next_try = 0, last_log = 0, next_follow = 0;
int tries = 0, last_following = -2;
bool key_was_down = false;
HANDLE ai_event = nullptr;

// ---- the game's UDP telemetry: who is on which team (participants packet) ----------------------------------------------
// Settings > Telemetry: UDP on, IP 127.0.0.1, the port below, format 2026. Only the participants packet (id 4) is read: per
// car a flag "AI controlled", the team id (byte 3, or byte 5 on custom grids) and the name. That gives your car (the human
// one) and your teammate (same team) even before the race. If another program (another telemetry tool) already listens on
// the port, change the port of one of them.
std::atomic<int> udp_packets{0};

DWORD WINAPI udp_main(void *)
{
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0)
        return 0;
    SOCKET us = INVALID_SOCKET;
    int bound_port = 0;
    std::string last_sig;
    uint8_t buf[2048];
    for (;;) {
        int port = std::atoi(std::string(std::string_view(udp_port)).c_str());
        if (us != INVALID_SOCKET && port != bound_port) {
            closesocket(us);
            us = INVALID_SOCKET;
        }
        if (us == INVALID_SOCKET) {
            us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (us != INVALID_SOCKET) {
                BOOL yes = TRUE;
                setsockopt(us, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof yes);
                DWORD to = 500;
                setsockopt(us, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&to), sizeof to);
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_port = htons(uint16_t(port));
                a.sin_addr.s_addr = htonl(INADDR_ANY);
                if (bind(us, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
                    LOG("UDP telemetry: port %d is busy (another program uses it): team data not available, change the port in the settings", port);
                    closesocket(us);
                    us = INVALID_SOCKET;
                    Sleep(10000);
                    continue;
                }
                bound_port = port;
                LOG("UDP telemetry: listening on port %d for the participants (teams)", port);
            } else {
                Sleep(5000);
                continue;
            }
        }
        int n = recv(us, reinterpret_cast<char *>(buf), sizeof buf, 0);
        if (n <= 0)
            continue;
        udp_packets++;
        // pass every packet on to another program (SimHub or similar) that listens on its own port: the game sends its
        // telemetry to one port only
        {
            std::string_view fw = udp_forward;
            int fport = fw == "off" ? 0 : std::atoi(std::string(fw).c_str());
            if (fport > 0 && fport != bound_port) {
                static SOCKET fs = INVALID_SOCKET;
                if (fs == INVALID_SOCKET)
                    fs = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_port = htons(uint16_t(fport));
                to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                sendto(fs, reinterpret_cast<const char *>(buf), n, 0, reinterpret_cast<sockaddr *>(&to), sizeof to);
                static bool said = false;
                if (!said) {
                    said = true;
                    LOG("UDP telemetry: every packet is also passed on to 127.0.0.1:%d", fport);
                }
            }
        }
        uint16_t fmt = uint16_t(buf[0] | buf[1] << 8);
        if (fmt != 2026 || n < 30 || buf[6] != 4)
            continue;
        int num = std::min<int>(buf[29], 24);
        if (n < 30 + num * 60)
            continue;
        std::vector<EntrantInfo> list;
        std::string sig;
        for (int i = 0; i < num; i++) {
            const uint8_t *e = buf + 30 + i * 60;
            EntrantInfo en;
            en.car = i;
            en.human = e[0] == 0;
            en.team = std::to_string(e[3] != 255 ? e[3] : e[5]);
            list.push_back(en);
            sig += en.human ? 'H' : 'a';
            sig += en.team;
            sig += ',';
        }
        if (sig == last_sig)
            continue;
        last_sig = sig;
        int humans = 0, me = -1;
        for (const auto &e : list)
            if (e.human) {
                humans++;
                if (me < 0)
                    me = e.car;
            }
        {
            std::lock_guard<std::mutex> g(g_entrants_mu);
            g_entrants = std::move(list);
        }
        LOG("ENTRANTS from the UDP participants packet: %d car(s), %d human, first human car %d", num, humans, me);
    }
}

// ---- keep the game running when its window loses focus (ported from AI Drive) ----------------------------------------
// Keeps the game running when its window loses focus (you click the web panel in a browser on the same PC).
// Included by ai_drive.cpp inside its anonymous namespace.
//
// The game's window procedure turns WM_ACTIVATEAPP into NeApplication2::onFocusChanged, which sends
// MessageGameWindowFocusLost and suspends the game (it pauses, and the audio engine is suspended). A filter in front of
// that procedure swallows the "you lost focus" messages while the web panel is on, so the game never learns it was
// deactivated. Gaining focus passes through as usual. The filter only drops messages; it changes nothing else.

std::atomic<bool> keep_running_now{false};   // the filter is active (web panel on and the option on)
std::atomic<int> keep_swallowed{0};
HWND keep_hwnd = nullptr;
WNDPROC keep_orig = nullptr;
bool keep_unicode = true;

// The window messages that can tell the game it lost focus, for the log (the first 80 only).
const char *keep_msg_name(UINT m)
{
    switch (m) {
    case WM_ACTIVATEAPP: return "ACTIVATEAPP";
    case WM_ACTIVATE: return "ACTIVATE";
    case WM_NCACTIVATE: return "NCACTIVATE";
    case WM_KILLFOCUS: return "KILLFOCUS";
    case WM_SETFOCUS: return "SETFOCUS";
    case WM_SIZE: return "SIZE";
    case WM_SHOWWINDOW: return "SHOWWINDOW";
    case WM_SYSCOMMAND: return "SYSCOMMAND";
    case WM_ENTERSIZEMOVE: return "ENTERSIZEMOVE";
    case WM_DISPLAYCHANGE: return "DISPLAYCHANGE";
    default: return nullptr;
    }
}

std::atomic<int> keep_traced{0};

LRESULT CALLBACK keep_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    bool drop = keep_running_now && ((m == WM_ACTIVATEAPP && !w) || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) ||
                                     m == WM_KILLFOCUS);
    if (const char *nm = keep_msg_name(m))
        if (keep_traced++ < 80)
            LOG("window message %s w=0x%llx l=0x%llx%s", nm, (unsigned long long)w, (unsigned long long)l,
                drop ? " - dropped, the game is not told" : keep_running_now ? "" : " (filter off)");
    if (drop) {
        keep_swallowed++;
        return 0;
    }
    return keep_unicode ? CallWindowProcW(keep_orig, h, m, w, l) : CallWindowProcA(keep_orig, h, m, w, l);
}

// The game's main window: the biggest visible top-level window of this process.
HWND keep_find_window()
{
    struct Best {
        HWND h = nullptr;
        long area = 0;
    } best;
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER))
                return TRUE;
            RECT r;
            if (!GetWindowRect(h, &r))
                return TRUE;
            long area = (r.right - r.left) * (r.bottom - r.top);
            auto *b = reinterpret_cast<Best *>(p);
            if (area > b->area)
                b->h = h, b->area = area;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&best));
    return best.area > 100 * 100 ? best.h : nullptr;
}

DWORD WINAPI keep_main(void *)
{
    for (;;) {
        bool want = bg_run;
        if (want && !keep_hwnd) {
            if (HWND h = keep_find_window()) {
                keep_unicode = IsWindowUnicode(h);
                WNDPROC old = keep_unicode ? WNDPROC(SetWindowLongPtrW(h, GWLP_WNDPROC, LONG_PTR(&keep_proc)))
                                           : WNDPROC(SetWindowLongPtrA(h, GWLP_WNDPROC, LONG_PTR(&keep_proc)));
                if (old) {
                    keep_orig = old;
                    keep_hwnd = h;
                    char title[80] = {};
                    GetWindowTextA(h, title, sizeof title);
                    LOG("keep running: game window found (\"%s\")", title);
                } else {
                    LOG("keep running in the background: could not hook the game window (error %lu)", GetLastError());
                    Sleep(10000);
                }
            }
        }
        keep_running_now = want && keep_hwnd;
        Sleep(want && !keep_hwnd ? 1000 : 500);
    }
}

// ---- keep the race running after a crash (our own method, also needs no other addon) ----------------------------------
// The game ends a single-player race when your car is terminally damaged: VehicleTerminalDamageSystem posts a
// MessageVehicleTerminallyDamaged on the message bus and the session reacts to it. The Ne_Post_BusMessage wrap at the end
// of this file drops that one message for your car (a car in human control, or the one we know is yours), so the race goes
// on; the wrecked car stays where it is and we spectate. The message's vtable is found from a code pattern at load.
uintptr_t kc_term_vt = 0;
std::atomic<int> kc_vehicle{-1};     // your car, when the message for it was dropped
std::atomic<void *> kc_pit_system{nullptr};   // the pit lane system, seen on the game's own calls
thread_local bool kc_own_call = false;
double kc_session_gone = 0;

bool mem_read(uintptr_t a, void *out, size_t n)
{
    SIZE_T got = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a), out, n, &got) && got == n;
}

void kc_find()
{
    uintptr_t at = 0;
    int hits = swofty::api->scan(".xtls",
        "48 C7 44 24 28 00 00 00 00 48 8D 0D ?? ?? ?? ?? 48 89 4C 24 20 48 8B 4C 24 48 48 89 4C 24 30 48 8B 48 08 "
        "48 8D 54 24 20 E8 ?? ?? ?? ??", &at);
    if (hits == 1) {
        int32_t rel;
        if (mem_read(at + 12, &rel, 4))
            kc_term_vt = at + 16 + intptr_t(rel);
    }
    LOG("keep race: terminal damage message %s (matches %d)", kc_term_vt ? "found" : "NOT found", hits);
}

// ---- the UI registry (ported from AI Drive) ------------------------------------------------------------------------
// #session.spectator_mode.active decides whether the game shows the spectator HUD (hud_spectator_2). After the director
// is switched off the HUD stayed on screen, so the flag is also set to false through the game's own VM call.
using VmPath_t = void **(void *registry, void *root, const char *path);
using VmSetBool_t = void *(void *registry, void *parent, const char *name, uint8_t *value, int overwrite);
uintptr_t vm_registry = 0;
VmSetBool_t *vm_set_bool = nullptr;
VmPath_t *vm_path = nullptr;
bool vm_ready = false;

void vm_find()
{
    vm_registry = swofty::api->resolve("g_Vm_Registry");
    vm_set_bool = reinterpret_cast<VmSetBool_t *>(swofty::api->resolve("Vm_Set_Bool"));
    uintptr_t at = 0;
    int hits = swofty::api->scan(".xtls", "E9 ?? ?? ?? ?? 18 36 B8 0F 88 D1 41", &at);
    if (hits != 1) {
        uintptr_t reg = 0;
        hits = swofty::api->scan(".xtls", "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 88 02 00 00 4C 89 C3", &reg);
        auto *t = hits == 1 ? reinterpret_cast<const uint8_t *>(reg - 0xCCE0) : nullptr;
        at = (t && t[-1] == 0xcc && t[0] == 0xe9) ? uintptr_t(t) : 0;
        hits = at ? 1 : 0;
    }
    vm_path = hits == 1 ? reinterpret_cast<VmPath_t *>(at) : nullptr;
    vm_ready = vm_registry && vm_set_bool && vm_path;
    LOG("UI registry: %s (registry %s, Vm_Set_Bool %s, node path %s)", vm_ready ? "ready" : "NOT available", vm_registry ? "ok" : "missing",
        vm_set_bool ? "ok" : "missing", vm_path ? "ok" : "missing");
}

bool vm_bool(const char *path, const char *leaf, bool value)
{
    if (!vm_ready)
        return false;
    void *reg = *reinterpret_cast<void **>(vm_registry);
    auto *root = reg ? *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(reg) + 8) : nullptr;
    void **node = root ? vm_path(reg, root + 8, path) : nullptr;
    if (!node || !*node)
        return false;
    uint8_t v = value;
    vm_set_bool(reg, static_cast<uint8_t *>(*node) + 8, leaf, &v, 1);
    return true;
}

// ---- HUD hide reasons (diagnostic) ------------------------------------------------------------------------------------
// The game keeps two dword masks of "reasons to hide the HUD" (struct at one fixed place, found from the code of
// Sess_SpectatorHud_Hide); the HUD shows only while its mask is 0. Known reason bits from the exe (who sets them):
// 0 session states (in game, spectating, garage, start lights), 1 pause, 4 pre-race, 5 doughnut, 6 spectator HUD hide (the
// one the Swofty event covers), 8 AI pit state change, 9 safety car tether, 10 controller flag change, 11-12 other.
std::atomic<int> hv_calls{0}, hv_forced{0};
std::atomic<int> hv_last_following{-1}, hv_last_visible{-1};
using HudUpdateFn = void (*)(void *);
HudUpdateFn hm_update = nullptr;   // the game's HUD visibility update, takes the struct
double hm_fix_at = 0;
int hm_fixes = 0;
uintptr_t hm_addr = 0;   // first mask; the second one is at +4
uint32_t hm_last[2] = {0xffffffff, 0xffffffff};
double hm_log = 0;

void hb_find();

void hm_find()
{
    uintptr_t at = 0;
    int hits = swofty::api->scan(".xtls",
        "8B 05 ?? ?? ?? ?? A8 40 0F 85 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 01 0F 85 ?? ?? ?? ?? B9 D4 00 00 00", &at);
    if (hits == 1) {
        int32_t rel;
        if (mem_read(at + 2, &rel, 4))
            hm_addr = at + 6 + intptr_t(rel) - 4;
    }
    LOG("HUD hide masks: %s (matches %d)", hm_addr ? "found" : "NOT found", hits);
    uintptr_t fn = 0;
    int h2 = swofty::api->scan(".xtls", "56 57 53 48 83 EC 30 48 89 CE 83 39 00 0F 94 44 24 2F 48 8B 79 68 48 85 FF 74 ?? 48 3B 3D ?? ?? ?? ??", &fn);
    if (h2 == 1)
        hm_update = reinterpret_cast<HudUpdateFn>(fn);
    LOG("HUD visibility update: %s (matches %d)", hm_update ? "found" : "NOT found", h2);
    hb_find();
}

void hm_watch()
{
    uint32_t m[2] = {0, 0};
    if (!hm_addr || !mem_read(hm_addr, m, 8))
        return;
    if (m[0] == hm_last[0] && m[1] == hm_last[1])
        return;
    hm_last[0] = m[0];
    hm_last[1] = m[1];
    static double window = 0;
    static int in_window = 0;
    if (now() - window > 1.0) {
        window = now();
        in_window = 0;
    }
    if (++in_window > 25)   // a burst: keep the log readable
        return;
    LOG("%s HUD HIDE REASONS: race-HUD mask %#x [%s] | spectator-HUD mask %#x [%s]", tstamp().c_str(), m[0], hm_text(m[0]).c_str(), m[1],
        hm_text(m[1]).c_str());
}


// The game sets reason 0 again and again from its session-state code ("or eax, 1; mov [mask], eax; ...; call update").
// Clearing the bit from outside lost every time (back within 0.3 s). So while we spectate, the immediate 1 in each of those
// instructions that writes one of the two masks is changed to 0, and changed back to 1 when spectating stops. The sites are
// found at load by scanning the game's code for exactly that instruction pair aimed at the two masks.
std::vector<uintptr_t> hb_sites;   // address of the immediate byte of each "or eax, 1" that writes a mask
bool hb_on = false;

void hb_find()
{
    if (!hm_addr)
        return;
    auto mod = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(mod);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(mod + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        auto *b = reinterpret_cast<const uint8_t *>(mod + sec->VirtualAddress);
        size_t n = sec->Misc.VirtualSize;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(b, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
            continue;
        for (size_t k = 0; k + 9 <= n; k++) {
            // found live with the write watchpoint (F1_25.exe+0x2d66395 on the 2026-10-30 Steam build), the writer that
            // sets reason 0 every frame: 48 0F 44 DF = cmove rbx, rdi ; 80 0B 01 = or byte ptr [rbx], 1 ; EB = jmp
            if (b[k] == 0x48 && b[k + 1] == 0x0F && b[k + 2] == 0x44 && b[k + 3] == 0xDF && b[k + 4] == 0x80 && b[k + 5] == 0x0B &&
                b[k + 6] == 0x01 && b[k + 7] == 0xEB) {
                hb_sites.push_back(uintptr_t(b + k + 6));
                continue;
            }
            // 83 C8 01 = or eax, 1 ; 89 05 rel32 = mov [rip+rel32], eax
            if (b[k] != 0x83 || b[k + 1] != 0xC8 || b[k + 2] != 0x01 || b[k + 3] != 0x89 || b[k + 4] != 0x05)
                continue;
            int32_t rel;
            std::memcpy(&rel, b + k + 5, 4);
            uintptr_t target = uintptr_t(b + k + 9) + intptr_t(rel);
            if (target == hm_addr || target == hm_addr + 4)
                hb_sites.push_back(uintptr_t(b + k + 2));
        }
    }
    std::string list;
    for (auto a : hb_sites) {
        char t[24];
        snprintf(t, sizeof t, " %#llx", (unsigned long long)(a - 2 - mod));
        list += t;
    }
    LOG("HUD block: %zu place(s) in the game code set hide reason 0 (rva:%s)", hb_sites.size(), list.c_str());
}

void hb_set(bool on)
{
    if (on == hb_on || hb_sites.empty())
        return;
    int ok = 0;
    for (auto a : hb_sites) {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void *>(a), 1, PAGE_EXECUTE_READWRITE, &old))
            continue;
        *reinterpret_cast<volatile uint8_t *>(a) = on ? 0x00 : 0x01;
        VirtualProtect(reinterpret_cast<void *>(a), 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(a), 1);
        ok++;
    }
    hb_on = on;
    LOG("%s HUD block %s: %d of %zu place(s) %s", tstamp().c_str(), on ? "ON" : "OFF", ok, hb_sites.size(),
        on ? "no longer set reason 0" : "restored to the game's own code");
}

// ---- writer trace (live reverse engineering) -----------------------------------------------------------------------
// Something still sets hide reason 0 on the race HUD mask although the 19 known instructions are blocked. To find it, a
// hardware write watchpoint (debug register 0) is put on that mask in every game thread for a few seconds; a vectored
// exception handler records the address of each instruction that wrote it, then the watchpoint is removed again.
struct TraceHit {
    std::atomic<uintptr_t> rip{0};
    std::atomic<int> count{0};
};
TraceHit tr_hits[32];
std::atomic<bool> tr_armed{false};
PVOID tr_veh = nullptr;
double tr_until = 0;
int tr_runs = 0;

LONG CALLBACK tr_handler(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Dr6 = 0;
    if (!tr_armed.load())
        return EXCEPTION_CONTINUE_EXECUTION;
    uintptr_t rip = uintptr_t(e->ExceptionRecord->ExceptionAddress);
    for (auto &h : tr_hits) {
        uintptr_t cur = h.rip.load();
        if (cur == rip) {
            h.count++;
            break;
        }
        if (cur == 0) {
            uintptr_t zero = 0;
            if (h.rip.compare_exchange_strong(zero, rip) || h.rip.load() == rip) {
                h.count++;
                break;
            }
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Puts (on) or removes (off) the watchpoint on the race-HUD mask in every thread of the game except this one.
int tr_set_all(bool on)
{
    int done = 0;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
            continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t)
            continue;
        if (SuspendThread(t) != DWORD(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                if (on) {
                    c.Dr0 = hm_addr;
                    c.Dr7 = (c.Dr7 & ~(DWORD64(0xF) << 16)) | 1 | (DWORD64(1) << 16) | (DWORD64(3) << 18);   // L0, write, 4 bytes
                } else {
                    c.Dr0 = 0;
                    c.Dr7 &= ~(DWORD64(1) | (DWORD64(0xF) << 16));
                }
                if (SetThreadContext(t, &c))
                    done++;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return done;
}

void tr_start()
{
    if (!hm_addr || tr_runs >= 1 || tr_armed.load())
        return;
    tr_runs++;
    if (!tr_veh)
        tr_veh = AddVectoredExceptionHandler(1, tr_handler);
    for (auto &h : tr_hits) {
        h.rip = 0;
        h.count = 0;
    }
    tr_armed = true;
    int n = tr_set_all(true);
    tr_until = now() + 4.0;
    LOG("%s TRACE #%d ON: write watchpoint on the race-HUD mask in %d thread(s) for 4 s", tstamp().c_str(), tr_runs, n);
}

void tr_tick()
{
    if (!tr_armed.load() || now() < tr_until)
        return;
    int n = tr_set_all(false);
    tr_armed = false;
    auto mod = uintptr_t(GetModuleHandleW(nullptr));
    LOG("%s TRACE #%d OFF (%d thread(s) cleared). Instructions that wrote the race-HUD mask:", tstamp().c_str(), tr_runs, n);
    int shown = 0;
    for (auto &h : tr_hits) {
        uintptr_t rip = h.rip.load();
        if (!rip)
            continue;
        shown++;
        if (rip >= mod && rip < mod + 0x20000000)
            LOG("   writer after F1_25.exe+%#llx: %d write(s)", (unsigned long long)(rip - mod), h.count.load());
        else
            LOG("   writer after %#llx (outside the exe): %d write(s)", (unsigned long long)rip, h.count.load());
    }
    if (!shown)
        LOG("   none caught");
}

// While we spectate, reason 0 (set by the game's "in-game spectating" state) hides both HUDs: clear it and let the game
// refresh the HUD with its own update function. Only reason 0 is touched, and only while the director is on.
void hm_fix()
{
    if (!hm_addr || !hm_update)
        return;
    uint32_t m[2] = {0, 0};
    if (!mem_read(hm_addr, m, 8) || !((m[0] | m[1]) & 1u))
        return;
    if (now() - hm_fix_at < 0.25)
        return;
    hm_fix_at = now();
    InterlockedAnd(reinterpret_cast<volatile LONG *>(hm_addr), ~1L);
    InterlockedAnd(reinterpret_cast<volatile LONG *>(hm_addr + 4), ~1L);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    hm_update(reinterpret_cast<void *>(hm_addr));
    QueryPerformanceCounter(&t1);
    uint32_t after[2] = {0, 0};
    mem_read(hm_addr, after, 8);
    hm_fixes++;
    if (hm_fixes <= 40 || hm_fixes % 20 == 0)
        LOG("%s HUD FIX #%d on thread %lu: reason 0 cleared (was %#x/%#x, now %#x/%#x), refresh call took %.3f ms", tstamp().c_str(), hm_fixes,
            GetCurrentThreadId(), m[0], m[1], after[0], after[1], double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));
}

bool car_racing(uint32_t car)
{
    // a car that is still racing: not retired (bit 4), not in the pit lane (bit 0), no terminal damage (bit 9)
    auto st = swofty::cars::live::Car(car).control();
    return st && (st->flags & ((1u << 0) | (1u << 4) | (1u << 9))) == 0;
}

bool ai_driving()
{
    if (!ai_event)
        ai_event = OpenEventA(SYNCHRONIZE, FALSE, "kizi.ai-drive.active");
    return ai_event && WaitForSingleObject(ai_event, 0) == WAIT_OBJECT_0;
}

swofty::spectator::Camera camera_wanted()
{
    // only the starting camera: change it in the game with its own key, it is not set again while you spectate
    return swofty::spectator::Camera::broadcast;
}

void toast(const char *text, const char *detail)
{
    auto r = swofty::hud::toast(text, detail, 3.0f);
    if (r != swofty::hud::Result::ok)
        LOG("toast not shown (%s): %s", swofty::hud::name(r), text);
}

// Presses the game's change-camera key once (as a scan code, so the game sees it like a real key): after the director is
// off the camera stays where spectating left it until that key is pressed (as the old AI Drive found).
DWORD WINAPI camera_key_thread(void *vk)
{
    UINT code = MapVirtualKeyW(UINT(reinterpret_cast<uintptr_t>(vk)), MAPVK_VK_TO_VSC);
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wScan = in[1].ki.wScan = WORD(code);
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(60);
    SendInput(1, &in[1], sizeof(INPUT));
    return 0;
}

int camera_vk()
{
    std::string_view k = camera_key;
    if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z')
        return k[0];
    if (k.size() >= 2 && k[0] == 'F') {
        int n = std::atoi(std::string(k.substr(1)).c_str());
        if (n >= 1 && n <= 12)
            return VK_F1 + n - 1;
    }
    return 0;   // "none"
}

// Sends your wrecked car to its garage with the game's own pit lane call (status 9 = in the garage), the same call the game
// makes for a car that ends its session in the pits. The pit lane system pointer is taken from the game's own calls to it.
void toast(const char *text, const char *detail);

// Sends car v to its garage (status 9 = in the garage) with the game's own pit lane call and keeps it there.
bool kc_garage_car(int v, const char *why)
{
    void *sys = kc_pit_system.load();
    if (v < 0 || v >= 32)
        return false;
    if (!sys || !swofty::game::Game_Pitlane_SetVehicleStatus.address()) {
        static double last = 0;
        if (now() - last > 5.0) {
            last = now();
            LOG("%s GARAGE: can't send car %d yet (%s)", tstamp().c_str(), v, sys ? "call not bound" : "the pit lane system has not been seen yet");
        }
        return false;
    }
    kc_own_call = true;
    uint64_t r = swofty::game::Game_Pitlane_SetVehicleStatus(sys, uint64_t(v), 9);
    kc_own_call = false;
    kc_parked |= 1u << v;
    LOG("%s GARAGE: car %d sent to the garage (%s), call returned %llu", tstamp().c_str(), v, why, (unsigned long long)r);
    return true;
}

// Your car: the player index of the telemetry feed (needs the game's UDP telemetry), else the car in human control.
int sg_find_me(char *how, size_t n)
{
    {
        std::lock_guard<std::mutex> g(g_entrants_mu);
        for (const auto &e : g_entrants)
            if (e.human && e.car >= 0 && e.car < 32) {
                snprintf(how, n, "entrants list: human car %d, team '%s'", e.car, e.team.c_str());
                return e.car;
            }
    }
    swofty::telemetry::SessionState ss{};
    auto r = swofty::telemetry::session(&ss);
    if (r == swofty::telemetry::Result::ok && ss.participant_count > 0 && ss.player_index < 32) {
        snprintf(how, n, "telemetry player index %u of %u", ss.player_index, ss.participant_count);
        return int(ss.player_index);
    }
    for (int c = 0; c < 32; c++)
        if (auto st = swofty::cars::live::Car(uint32_t(c)).control())
            if (st->mode == swofty::sessions::Control::human) {
                snprintf(how, n, "human controller on car %d", c);
                return c;
            }
    size_t count = 0;
    {
        std::lock_guard<std::mutex> g(g_entrants_mu);
        count = g_entrants.size();
    }
    snprintf(how, n, "no entrants list yet (%zu), telemetry %s, no human controller seen", count, swofty::telemetry::name(r));
    return -1;
}

// Your teammate: the other car with your team id in the entrants list.
int sg_find_mate(int me, bool *guessed)
{
    *guessed = false;
    {
        std::lock_guard<std::mutex> g(g_entrants_mu);
        std::string team;
        for (const auto &e : g_entrants)
            if (e.car == me)
                team = e.team;
        if (!team.empty())
            for (const auto &e : g_entrants)
                if (e.car != me && e.car >= 0 && e.car < 32 && e.team == team)
                    return e.car;
    }
    // No team data (the entrants list and the telemetry feed are empty on this setup). The cars of a team sit next to each
    // other in the game's car order (0-1, 2-3, ... and the F1 World cars 22-23): your teammate is the other car of your pair.
    // Your own car was the second of its pair in every log so far (21, 19, 21, 23), so this guess is the first one.
    int mate = me ^ 1;
    if (mate < 32)
        if (auto st = swofty::cars::live::Car(uint32_t(mate)).control())
            if (st->mode != swofty::sessions::Control::human) {
                *guessed = true;
                return mate;
            }
    return -1;
}

void kc_garage(const char *why)
{
    int v = kc_vehicle.load();
    void *sys = kc_pit_system.load();
    if (v < 0)
        return;
    if (!sys || !swofty::game::Game_Pitlane_SetVehicleStatus.address()) {
        static double last = 0;
        if (now() - last > 5.0) {
            last = now();
            LOG("%s GARAGE: can't send car %d yet (%s)", tstamp().c_str(), v, sys ? "call not bound" : "the pit lane system has not been seen yet");
        }
        return;
    }
    kc_own_call = true;
    uint64_t r = swofty::game::Game_Pitlane_SetVehicleStatus(sys, uint64_t(v), 9);
    kc_own_call = false;
    kc_garaged = true;
    LOG("%s GARAGE: car %d sent to the garage (%s), call returned %llu", tstamp().c_str(), v, why, (unsigned long long)r);
    toast("CAR IN THE GARAGE", "THE RACE GOES ON");
}

// Gives the game its own HUD back: spectator mode flag off in the UI registry, then the game recomputes the HUD visibility.
void hud_cleanup(const char *why, bool was_active);

void spectate_off(const char *why)
{
    if (!active) {
        hud_cleanup(why, false);
        return;
    }
    auto r = swofty::spectator::set_active(false);
    LOG("spectate OFF (%s): set_active(false) -> %s", why, swofty::result_name(r));
    hb_set(false);
    if (tr_armed.load()) {
        tr_set_all(false);
        tr_armed = false;
    }
    active = false;
    hud_keep = false;
    tries = 0;
    last_following = -2;
    hud_cleanup(why, true);
    g_spec_t0 = 0;
}

void hud_cleanup(const char *why, bool was_active)
{
    bool ok = vm_bool("session.spectator_mode", "active", false);
    bool menu = vm_bool("view_model_data.hud_spectator_2.content.spectator_menu_panel", "is_open", false);
    uint32_t m[2] = {0, 0};
    if (hm_addr)
        mem_read(hm_addr, m, 8);
    if (hm_update && hm_addr)
        hm_update(reinterpret_cast<void *>(hm_addr));
    // With the director off nothing calls the game's "hide the spectator HUD" any more, so the last "show" stays on screen:
    // call it once (hud_keep is already false, so our own hud.spectator.visible handler leaves it alone).
    bool hid = false;
    if (was_active && swofty::game::Sess_SpectatorHud_Hide.address()) {
        swofty::game::Sess_SpectatorHud_Hide();
        hid = true;
    }
    // The game's hide function leaves reason 6 on in both masks (nothing clears it while the director is off): that hid the
    // race HUD for good. Clear it again and let the game recompute the visibility.
    if (hid && hm_addr) {
        InterlockedAnd(reinterpret_cast<volatile LONG *>(hm_addr), ~0x40L);
        InterlockedAnd(reinterpret_cast<volatile LONG *>(hm_addr + 4), ~0x40L);
        if (hm_update)
            hm_update(reinterpret_cast<void *>(hm_addr));
    }
    LOG("%s HUD CLEANUP (%s): spectator_mode.active=false %s, menu closed %s, masks %#x/%#x, visibility refreshed %s, game's HUD hide called %s",
        tstamp().c_str(), why, ok ? "set" : "FAILED", menu ? "yes" : "no", m[0], m[1], hm_update ? "yes" : "no", hid ? "yes" : "NO (not bound)");
    if (was_active) {   // the camera key and the mask log only make sense when the director really was on
        cam_kick_at = now() + 0.5;
        after_off_log = now() + 0.6;
    }
}

void spectate_on(const char *why)
{
    if (active || now() < next_try)
        return;
    next_try = now() + 1.0;
    auto r = swofty::spectator::set_active(true);
    if (r != swofty::Result::ok) {
        LOG("spectate ON (%s): set_active(true) -> %s (will try again)", why, swofty::result_name(r));
        return;
    }
    LOG("spectate ON (%s): set_active(true) -> ok", why);
    active = true;
    hud_keep = true;
    act_started = now();
    g_spec_t0 = now();
    hm_fixes = 0;
    cam_kick_at = 0;   // spectating is on again: no camera key from an earlier switch-off
    auto c = swofty::spectator::set_camera(camera_wanted());
    LOG("starting camera broadcast: set_camera -> %s", swofty::result_name(c));
}

void tick()
{
    if (cam_kick_at && now() >= cam_kick_at) {
        cam_kick_at = 0;
        if (int vk = camera_vk()) {
            if (game_in_front())
                if (HANDLE t = CreateThread(nullptr, 0, camera_key_thread, reinterpret_cast<void *>(uintptr_t(vk)), 0, nullptr)) {
                    CloseHandle(t);
                    LOG("camera key %.*s pressed once after spectating", int(std::string_view(camera_key).size()), std::string_view(camera_key).data());
                }
        }
    }
    if (after_off_log && now() >= after_off_log) {
        after_off_log = 0;
        uint32_t m[2] = {0, 0};
        mem_read(hm_addr, m, 8);
        LOG("HUD masks 0.6 s after switching off: race-HUD %#x [%s] | spectator-HUD %#x [%s]", m[0], hm_text(m[0]).c_str(), m[1], hm_text(m[1]).c_str());
    }
    {
        // the session is over when your car no longer reports a controller: stop spectating and forget it
        int me0 = g_player.load();
        bool gone = me0 >= 0 && !swofty::cars::live::Car(uint32_t(me0)).control();
        if (!gone)
            kc_session_gone = 0;
        else if (kc_session_gone == 0)
            kc_session_gone = now();
        else if (now() - kc_session_gone > 3.0) {
            kc_session_gone = 0;
            LOG("session over (your car %d has no controller any more): spectate and state cleared", me0);
            spectate_off("session over");
            g_player = -1;
            kc_broken = false;
            kc_garaged = false;
            kc_parked = 0;
            kc_since = 0;
            sg_parked = false;
            kc_vehicle = -1;
            return;
        }
    }
    if (start_garage && sg_t0 > 0 && now() - sg_t0 >= 2.0 && !kc_garaged.load()) {
        // spectator mode: park your car in the garage from the start of the race (no crash needed) and spectate
        char how[160] = "";
        int found = sg_find_me(how, sizeof how);
        void *sys = kc_pit_system.load();
        if (found >= 0 && sys) {
            kc_vehicle = found;
            g_player = found;
            const char *w = sg_race.load() ? "start in the garage (spectator mode)" : "start in the garage, parked before the race (pre-race)";
            LOG("%s START IN GARAGE: your car is %d (%s)", tstamp().c_str(), found, how);
            kc_garage(w);
            if (kc_garaged.load()) {
                kc_parked |= 1u << found;
                if (park_teammate) {
                    bool guessed = false;
                    int mate = sg_find_mate(found, &guessed);
                    if (mate >= 0) {
                        LOG("%s START IN GARAGE: your teammate is car %d (%s)", tstamp().c_str(), mate,
                            guessed ? "a guess: the other car of your pair, no team data" : "same team in the entrants list");
                        kc_garage_car(mate, "your teammate, start in the garage");
                    } else
                        LOG("%s START IN GARAGE: teammate not found (no team data, and car %d is not an AI car)", tstamp().c_str(), found ^ 1);
                }
                sg_parked = true;
                if (sg_race.load()) {
                    kc_broken = true;   // the race is on: spectating starts by itself, like after a crash
                    kc_since = now();
                }
                sg_t0 = 0;
            }
        } else if (now() - sg_t0 > 240.0) {
            LOG("START IN GARAGE: gave up after 240 s (%s; pit lane system %s)", how, sys ? "seen" : "not seen");
            sg_t0 = 0;
        } else if (now() - sg_t0 > 2.0 + sg_tries_log * 10.0) {
            sg_tries_log++;
            LOG("START IN GARAGE: waiting (%s; pit lane system %s)", how, sys ? "seen" : "not seen");
        }
    }
    {
        std::string_view ga = garage_after;
        double limit = ga == "3" ? 3 : ga == "6" ? 6 : ga == "10" ? 10 : 0;
        if (kc_broken.load() && !kc_garaged.load() && limit > 0 && kc_since > 0 && now() - kc_since >= limit)
            kc_garage("the crash message was dropped earlier");
    }
    bool ai = ai_driving();
    bool out = kc_broken.load();
    if (g_reset_req.exchange(false)) {
        bool was = active;
        spectate_off("race restart or new session");
        kc_broken = false;
            kc_garaged = false;
            kc_parked = 0;
            kc_since = 0;
        kc_vehicle = -1;
        manual_on = false;
        user_off = false;
        race_over = false;
        lost_since = 0;
        sg_tries_log = 0;
        LOG("reset: race (re)start or new session, spectating %s, all state cleared", was ? "switched off" : "was off");
        return;
    }
    bool want = !user_off && !race_over && (manual_on || kc_broken.load() || (auto_on && (ai || out)));
    const char *reason = manual_on ? "key" : out ? "your car is out" : "AI Drive is driving";
    // your car is also known from the crash message we dropped for it
    if (g_player.load() < 0 && kc_vehicle.load() >= 0) {
        g_player = kc_vehicle.load();
        LOG("your car is vehicle %d (learnt from the crash message)", g_player.load());
    }
    if (want && !active)
        spectate_on(reason);
    else if (!want && active)
        spectate_off(manual_on ? "key" : "your car is racing again / AI Drive stopped");
    if (!active)
        return;
    hm_watch();
    hb_set(true);
    tr_tick();
    if (now() - act_started > 2.0) {
        uint32_t m0 = 0;
        if (mem_read(hm_addr, &m0, 4) && (m0 & 1u))
            tr_start();
    }
    hm_fix();
    int me = g_player.load();
    auto view = swofty::spectator::current();
    int following = view ? int(view->following.index()) : -1;
    bool pit = false;
    if (me >= 0)
        if (auto st = swofty::cars::live::Car(uint32_t(me)).control())
            pit = st->pit_lane();
    // the director follows nobody for 3 s: the race is over (podium, results, a cutscene). Stop and give the game its HUD back.
    if (following < 0 && now() - act_started > 3.0) {
        if (lost_since == 0)
            lost_since = now();
        else if (now() - lost_since > 3.0) {
            race_over = true;
            lost_since = 0;
            spectate_off("the director follows nobody for 3 s: race over / cutscene");
            return;
        }
    } else {
        lost_since = 0;
    }
    // your own car is out or sits in the pit lane or garage: watch another car that is still racing
    // (the game's spectator keys can't leave it). Also move on when the followed car has retired.
    bool own_bad = following == me && (pit || out);
    // following -1 = the director has no target (your car vanished after the retire): pick a car too
    bool other_bad = now() - act_started > 3.0 && (following < 0 || (following != me && !car_racing(uint32_t(following))));
    if (leave_own_car && me >= 0 && (own_bad || other_bad) && now() - act_started > 1.0 && now() >= next_follow) {
        next_follow = now() + (tries < 30 ? 2.0 : 10.0);
        tries++;
        int start = following >= 0 ? following : me;
        int target = -1;
        for (int i = 1; i < 32 && target < 0; i++) {
            int c = (start + i) % 32;
            if (c != me && car_racing(uint32_t(c)))
                target = c;
        }
        if (target < 0) {
            LOG("AUTO-PICK #%d: no car is racing right now (followed car %d)", tries, following);
        } else {
            auto r = swofty::spectator::follow(swofty::cars::live::Car(uint32_t(target)));
            LOG("AUTO-PICK #%d: %s, follow(car %d) -> %s", tries, own_bad ? "your car is out/in the pits" : "followed car is out", target,
                swofty::result_name(r));
        }
    }
    if (following != last_following || now() - last_log > 10.0) {
        last_log = now();
        last_following = following;
        LOG("%s STATE: following car %d (%s), camera %d, your car %d%s, AI Drive %d, manual %d, out %d", tstamp().c_str(),
            following, following == me ? "your car" : "another car", view ? int(view->camera) : -1, me, pit ? " (pit lane flag)" : "", ai ? 1 : 0,
            manual_on ? 1 : 0, out ? 1 : 0);
    }
    if (debug_log) {
        // one compact line whenever anything below changes (at most 6 a second) and every 5 s
        auto flags_of = [](int c, const char **mode) -> int64_t {
            *mode = "-";
            if (c < 0)
                return -1;
            auto st = swofty::cars::live::Car(uint32_t(c)).control();
            if (!st)
                return -1;
            *mode = st->mode == swofty::sessions::Control::human ? "human" : "ai";
            return int64_t(st->flags);
        };
        const char *mm = "-", *fm = "-";
        int64_t mf = flags_of(me, &mm), ff = flags_of(following, &fm);
        uint32_t mk[2] = {0, 0};
        mem_read(hm_addr, mk, 8);
        char line[420];
        snprintf(line, sizeof line,
                 "follow %d cam %d | me %d %s flags %#llx | followed %s flags %#llx racing %d | out %d kc %d | masks %#x/%#x | hv calls %d forced %d last(%d,%d) | fixes %d picks %d",
                 following, view ? int(view->camera) : -1, me, mm, (unsigned long long)(mf < 0 ? 0 : mf), fm, (unsigned long long)(ff < 0 ? 0 : ff),
                 following >= 0 && car_racing(uint32_t(following)) ? 1 : 0, out ? 1 : 0, kc_broken.load() ? 1 : 0, mk[0], mk[1],
                 hv_calls.load(), hv_forced.load(), hv_last_following.load(), hv_last_visible.load(), hm_fixes, tries);
        static std::string last;
        static double last_t = 0, last_hb = 0;
        std::string cur = line;
        bool changed = cur != last;
        if ((changed && now() - last_t > 0.5) || now() - last_hb > 5.0) {
            last = cur;
            last_t = now();
            last_hb = now();
            LOG("%s SNAP %s", tstamp().c_str(), line);
        }
    }
}

void handle_key()
{
    int vk = key_vk(hotkey);
    bool down = vk && game_in_front() && (GetAsyncKeyState(vk) & 0x8000);
    if (down && !key_was_down) {
        if (active || manual_on) {
            manual_on = false;
            user_off = true;
            spectate_off("key");
        } else {
            manual_on = true;
            user_off = false;
            race_over = false;
        }
        LOG("hotkey: spectate %s", manual_on ? "ON" : "OFF (stays off until you press the key again or the race restarts)");
        toast(manual_on ? "SPECTATE ON" : "SPECTATE OFF", "WATCH THE WHOLE GRID");
    }
    key_was_down = down;
}

void frame(double, void *)
{
    handle_key();
    static double last_tick = 0;
    if (now() - last_tick < 0.2)
        return;
    last_tick = now();
    tick();
}

}  // namespace

SWOFTY_ON_LOAD
{
    LOG("v" ADDON_VERSION " loading: auto %d, hotkey %.*s, leave own car %d", auto_on.get() ? 1 : 0, int(std::string_view(hotkey).size()),
        std::string_view(hotkey).data(), leave_own_car.get() ? 1 : 0);
    watch_player_events();
    if (HANDLE t = CreateThread(nullptr, 0, keep_main, nullptr, 0, nullptr))
        CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, udp_main, nullptr, 0, nullptr))
        CloseHandle(t);
    if (keep_race)
        kc_find();
    hm_find();
    vm_find();
    // the spectator HUD is hidden by the game while the director follows another car: keep it up
    {
        auto r = swofty::events::on<swofty::spectator::events::HudVisible>().before(
            [](swofty::spectator::events::HudVisible &e, const swofty::events::Context &) {
                static int shown = 0;
                int calls = ++hv_calls;
                bool was = e.visible;
                hv_last_following = int(e.following.index());
                hv_last_visible = was ? 1 : 0;
                bool forced = hud_keep.load() && !e.visible;
                if (forced) {
                    e.visible = true;
                    hv_forced++;
                }
                if (debug_log && (shown++ < 60))
                    LOG("%s EVENT hud.spectator.visible #%d: following car %u, game says visible=%d -> %s", tstamp().c_str(), calls,
                        e.following.index(), was ? 1 : 0, forced ? "kept visible" : "left as it is");
            });
        LOG("event hud.spectator.visible: %s", swofty::events::name(r));
    }
    // a new session starts without the director
    {
        auto r = swofty::events::on("session.start").after([](std::span<const std::byte>, const swofty::events::Context &ctx) {
            note_player(ctx, "session.start");
            active = false;
            hud_keep = false;
            tries = 0;
            kc_broken = false;
            kc_garaged = false;
            kc_parked = 0;
            kc_since = 0;
        });
        if (r != swofty::events::Result::ok)
            LOG("event session.start: %s", swofty::events::name(r));
    }
    auto r = swofty::events::on_frame(frame);
    if (r != swofty::events::Result::ok)
        LOG("frame callback %s: the addon can't run", swofty::events::name(r));
}

SWOFTY_WRAP(Ne_Post_BusMessage, uint64_t, void *bus, void *msg)
{
    uint64_t vt = 0;
    if (keep_race && kc_term_vt && msg && mem_read(uintptr_t(msg), &vt, 8) && vt == kc_term_vt) {
        uint8_t veh = 0xff;
        mem_read(uintptr_t(msg) + 0x10, &veh, 1);
        bool mine = false;
        if (veh < 32) {
            auto st = swofty::cars::live::Car(uint32_t(veh)).control();
            mine = (st && st->mode == swofty::sessions::Control::human) || int(veh) == g_player.load();
        }
        if (mine) {
            kc_vehicle = int(veh);
            if (!kc_broken.exchange(true)) {
                kc_since = now();
                LOG("keep race: terminal damage message dropped for your car %d: the race goes on", int(veh));
            }
            return 0;
        }
    }
    return orig(bus, msg);
}

SWOFTY_WRAP(Game_Pitlane_SetVehicleStatus, uint64_t, void *sys, uint64_t vehicle, uint64_t status)
{
    kc_pit_system = sys;
    // a wreck that was sent to the garage stays there: the box-stop logic would otherwise release it again
    if (!kc_own_call && status != 9 && vehicle < 32 && !g_reset_req.load() &&
        ((kc_garaged.load() && int(vehicle) == kc_vehicle.load()) || (kc_parked.load() >> vehicle & 1)))
        return 0;
    return orig(sys, vehicle, status);
}

SWOFTY_ADDON()
