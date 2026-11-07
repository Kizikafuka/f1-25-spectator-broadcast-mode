// SPDX-License-Identifier: MIT
// Broadcast Control (TEST): a small web page on this PC with the live race (timing tower + telemetry of the watched car) where a
// click on a driver's name makes the spectator camera follow that car. Included by spectator_hud.cpp inside its anonymous
// namespace (it uses `active`, `now`, `tstamp`, `LOG`, `panel_mode`, `panel_port`, `is_safety_car`).
//
// - Data: swofty::telemetry (launcher API): no UDP port is opened, so nothing here can steal packets from the director or another tool.
//   Pit / out / safety car come from the controller flags of every car (the bits spectator_hud.cpp already uses).
//   Not in that API (shown as "?" / "-"): tyre compound, penalties, events.
// - Server: plain HTTP on 127.0.0.1 only, own thread, one request at a time with short timeouts. The game thread only builds a JSON
//   snapshot (10 Hz, and only while a page polled in the last 3 s) and runs the click commands (follow / camera).
// - Safety: Host header must be 127.0.0.1/localhost:port (no DNS rebinding); control calls need the token header and a matching or
//   missing Origin; no CORS headers.
#include <memory>
#include "broadcast_page.hpp"

std::atomic<int> bp_status{0};          // 0 off, 1 on, 2 port busy
std::atomic<bool> bp_new_listen{false}; // the listener was (re)started: the address goes to the clipboard again
std::atomic<double> bp_last_req{0};     // when a page last asked for the state (now() seconds)
std::atomic<int> bp_cmd_car{-1}, bp_cmd_cam{0};
std::atomic<bool> bp_dir_on{false};     // the spectator director is on (the clicks can work)
std::mutex bp_mu;
std::string bp_state = "{\"ready\":false,\"reason\":\"starting\"}";
int bp_clients = 0;

// ---- snapshot ---------------------------------------------------------------------------------------------------------

void bp_json_str(std::string &o, const char *s)
{
    o += '"';
    for (; *s; s++) {
        unsigned char c = uint8_t(*s);
        if (c == '"' || c == '\\') {
            o += '\\';
            o += char(c);
        } else if (c < 0x20) {
            o += ' ';
        } else {
            o += char(c);
        }
    }
    o += '"';
}

void bp_num(std::string &o, double v, int decimals)
{
    char b[40];
    if (!std::isfinite(v))
        v = 0;
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    o += b;
}

// seconds from a lap time that may be in seconds or in milliseconds (the unit is not documented)
double bp_secs(float t)
{
    return t > 1000.0f ? double(t) / 1000.0 : double(t);
}

// ---- UDP source (used when the launcher's telemetry API is not there) -------------------------------------------------------
// The packets of the game's own UDP telemetry (format 2025/2026, header 29 bytes, packet id at byte 6), handed over by udp_main:
// participants (4, names), lap data (2), car telemetry (6), car status (7), car damage (10), session (1). Layouts as in the old
// AI Drive web panel (checked on live data there); tyre temperatures and fuel laps use the standard F1 UDP offsets (not checked).
struct BpU {
    char name[33];
    uint8_t team, pos, lap, pit, sector, drv, res, cmp;
    uint8_t raw[60];   // the participants entry as received (for the log when a car has no name)
    uint32_t last, cur, best, gap, lead;
    float dist, thr, brk, steer, fuel_laps;
    uint16_t kmh, rpm;
    int8_t gear;
    uint8_t tt[4];   // tyre surface temperature FL FR RL RR
    float wear[4];   // tyre wear % FL FR RL RR
};
struct BpS {
    bool ok;
    uint8_t weather, type, laps, track, sc, nz;
    int8_t ttemp, atemp;
    uint16_t left, len;   // len = track length in metres
    float zs[21];         // marshal zone starts (fraction of the lap)
    int8_t zf[21];        // zone flag: 0 none, 1 green, 2 blue, 3 yellow
};
// The feed of what happens: the game's own events plus ones we derive (pit in/out, a car slow or stopped on track, yellow zones).
// Layouts of the event packet are the standard F1 UDP ones (a, b = the car indexes in the payload), not all checked on live data.
struct BpE {
    unsigned id;
    double t;
    char k[5];
    uint8_t a, b;
};
std::vector<BpE> be;
unsigned be_next = 1;

// bu_mu must be held
void bp_event(const char *k, int a, int b)
{
    BpE e{be_next++, now(), {}, uint8_t(a), uint8_t(b)};
    std::snprintf(e.k, sizeof e.k, "%s", k);
    be.push_back(e);
    if (be.size() > 60)
        be.erase(be.begin(), be.begin() + 20);
}
BpU bu[24];
BpS bs;
std::mutex bu_mu;
std::atomic<unsigned long long> bu_last{0};   // GetTickCount64 of the last packet

float bp_f(const uint8_t *p)
{
    float v;
    std::memcpy(&v, p, 4);
    return v;
}
uint32_t bp_u32(const uint8_t *p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t bp_u16(const uint8_t *p)
{
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

void bp_packet(const uint8_t *d, int n)
{
    if (n < 30 || d[6] > 20)
        return;
    uint8_t id = d[6];
    std::lock_guard<std::mutex> g(bu_mu);
    bu_last = GetTickCount64();
    if (id == 4 && n >= 30 + 24 * 60) {
        int num = std::min<int>(d[29], 24);
        for (int i = 0; i < 24; i++) {
            if (i >= num) {
                bu[i].name[0] = 0;
                continue;
            }
            const uint8_t *e = d + 30 + i * 60;
            std::memcpy(bu[i].raw, e, 60);
            bu[i].team = e[3] != 255 ? e[3] : e[5];
            std::memcpy(bu[i].name, e + 10, 32);
            bu[i].name[32] = 0;
        }
    } else if (id == 2 && n >= 29 + 24 * 57) {
        for (int i = 0; i < 24; i++) {
            const uint8_t *e = d + 29 + i * 57;
            BpU &c = bu[i];
            c.last = bp_u32(e);
            c.cur = bp_u32(e + 4);
            c.gap = uint32_t(bp_u16(e + 14)) + uint32_t(e[16]) * 60000u;
            c.lead = uint32_t(bp_u16(e + 17)) + uint32_t(e[19]) * 60000u;
            c.dist = bp_f(e + 20);
            c.pos = e[32];
            c.lap = e[33];
            c.pit = e[34];
            c.sector = e[36];
            c.drv = e[44];
            c.res = e[45];
            if (c.last > 0 && (c.best == 0 || c.last < c.best))
                c.best = c.last;
        }
    } else if (id == 6 && n >= 29 + 24 * 59) {
        for (int i = 0; i < 24; i++) {
            const uint8_t *e = d + 29 + i * 59;
            BpU &c = bu[i];
            c.kmh = bp_u16(e);
            c.thr = bp_f(e + 2);
            c.steer = bp_f(e + 6);
            c.brk = bp_f(e + 10);
            c.gear = int8_t(e[15]);
            c.rpm = bp_u16(e + 16);
            c.tt[0] = e[32], c.tt[1] = e[33], c.tt[2] = e[30], c.tt[3] = e[31];   // the packet order is RL RR FL FR
        }
    } else if (id == 7 && n >= 29 + 24 * 59) {
        for (int i = 0; i < 24; i++) {
            const uint8_t *e = d + 29 + i * 59;
            bu[i].cmp = e[26];
            bu[i].fuel_laps = bp_f(e + 13);
        }
    } else if (id == 10 && n >= 29 + 24 * 46) {
        for (int i = 0; i < 24; i++) {
            const uint8_t *e = d + 29 + i * 46;
            bu[i].wear[0] = bp_f(e + 8), bu[i].wear[1] = bp_f(e + 12), bu[i].wear[2] = bp_f(e), bu[i].wear[3] = bp_f(e + 4);
        }
    } else if (id == 1 && n >= 29 + 127) {
        const uint8_t *p = d + 29;
        bs.ok = true;
        bs.weather = p[0];
        bs.ttemp = int8_t(p[1]);
        bs.atemp = int8_t(p[2]);
        bs.laps = p[3];
        bs.type = p[6];
        bs.track = p[7];
        bs.left = bp_u16(p + 9);
        bs.sc = p[124];   // 0 none, 1 safety car, 2 virtual, 3 formation lap
        bs.len = bp_u16(p + 4);
        bs.nz = std::min<uint8_t>(p[18], 21);
        for (int k = 0; k < bs.nz; k++) {
            bs.zs[k] = bp_f(p + 19 + 5 * k);
            bs.zf[k] = int8_t(p[19 + 5 * k + 4]);
        }
    } else if (id == 3 && n >= 34) {
        char code[5] = {char(d[29]), char(d[30]), char(d[31]), char(d[32]), 0};
        static const char *keep[] = {"OVTK", "COLL", "RTMT", "FTLP", "SCAR", "FLBK", "CHQF", "LGOT", "PENA", "DTSV", "SGSV"};
        for (const char *k : keep)
            if (!std::strcmp(code, k)) {
                int a = d[33], b = n > 34 ? d[34] : 255;
                if (!std::strcmp(code, "PENA") && n > 35)
                    a = d[35];   // penalty: type, infringement, then the car
                if (!std::strcmp(code, "FTLP") || !std::strcmp(code, "RTMT"))
                    b = 255;
                bp_event(code, a, b);
                break;
            }
    }
}

std::string bp_not_ready(bool api_ok)
{
    int us = g_udp_state.load();
    std::string why;
    if (us == 2)
        why = "the UDP telemetry port is busy (another program uses it): change the port in the settings";
    else if (bu_last.load() && GetTickCount64() - bu_last.load() > 3000)
        why = "no telemetry for a few seconds: no race running (or the game's UDP telemetry was switched off)";
    else
        why = "waiting for data: start a race. The launcher's telemetry API is " + std::string(api_ok ? "on, no session yet" : "not available") +
              ", so turn the game's UDP telemetry on (Settings > Telemetry: UDP on, IP 127.0.0.1, port " + std::string(std::string_view(udp_port)) +
              ", format 2025 or 2026) or use the Force Telemetry addon";
    std::string o = "{\"ready\":false,\"reason\":";
    bp_json_str(o, why.c_str());
    return o + "}";
}

std::mutex bp_build_mu;
std::atomic<bool> bp_api_mode{false};   // the launcher's telemetry API has a session: the page is built on the game thread
std::atomic<long> bp_pages_n{0};
std::atomic<double> bp_pages_us{0};
long bp_pages_built() { return bp_pages_n.load(); }
double bp_pages_avg_us() { return bp_pages_n.load() ? bp_pages_us.load() / double(bp_pages_n.load()) : 0.0; }
void bp_pages_reset()
{
    bp_pages_n = 0;
    bp_pages_us = 0;
}

void bp_build(bool allow_api)
{
    std::lock_guard<std::mutex> build_guard(bp_build_mu);
    struct Row {
        int i;
        swofty::telemetry::CarState c;
        char name[48];
        uint32_t team;
        bool pit, out, sc, has_gap, inactive;
        double frac;
        char cmp;
        double kmh, total, gap, interval;
    };
    struct Ses {
        uint32_t type, track, weather, laps;
        float left, ttemp, atemp;
        const char *flag;
    };
    static std::vector<Row> rows;
    static float track_len = 0, max_speed = 0;
    rows.clear();
    Ses si{};
    bool udp = false;
    swofty::telemetry::SessionState ss{};
    bool api = allow_api && swofty::telemetry::session(&ss) == swofty::telemetry::Result::ok && ss.participant_count > 0;
    if (api) {
        int n = int(std::min<uint32_t>(ss.participant_count, 24));
        for (int i = 0; i < n; i++) {
            Row r{};
            r.i = i;
            swofty::telemetry::Participant pp{};
            if (swofty::telemetry::car(uint32_t(i), &r.c) != swofty::telemetry::Result::ok)
                continue;
            if (swofty::telemetry::participant(uint32_t(i), &pp) == swofty::telemetry::Result::ok) {
                std::snprintf(r.name, sizeof r.name, "%s", pp.name);
                r.team = pp.team;
            }
            if (!r.name[0])
                std::snprintf(r.name, sizeof r.name, "Car %d", i);
            if (auto st = swofty::cars::live::Car(uint32_t(i)).control()) {
                r.pit = (st->flags & 1u) != 0;
                r.out = (st->flags & ((1u << 4) | (1u << 9))) != 0;
                r.sc = (st->flags & (1u << 11)) != 0;
            }
            if (r.sc)
                continue;   // the safety car is a vehicle too: not a row
            if (r.c.lap_distance > track_len)
                track_len = r.c.lap_distance;
            if (r.c.speed > max_speed)
                max_speed = r.c.speed;
            rows.push_back(r);
        }
        bool sc_out = false;
        for (uint32_t c = 0; c < 32 && !sc_out; c++)
            sc_out = is_safety_car(c);
        si = {ss.session_type, ss.track, ss.weather, ss.total_laps, ss.session_time_left, ss.track_temp, ss.air_temp, sc_out ? "sc" : "green"};
    } else {
        std::lock_guard<std::mutex> g(bu_mu);
        if (!bu_last.load() || GetTickCount64() - bu_last.load() > 3000 || !bs.ok) {
            std::string nr;
            nr = bp_not_ready(false);
            std::lock_guard<std::mutex> g2(bp_mu);
            bp_state = nr;
            return;
        }
        udp = true;
        for (int i = 0; i < 24; i++) {
            const BpU &u = bu[i];
            bool named = u.name[0] != 0;
            if (!named && !(u.res >= 2 && u.pos > 0))
                continue;
            Row r{};
            r.i = i;
            std::snprintf(r.name, sizeof r.name, "%s", named ? u.name : ("Car " + std::to_string(i)).c_str());
            static bool unnamed_logged[24];
            if (!named && !unnamed_logged[i]) {
                unnamed_logged[i] = true;
                char hex[130] = "";
                for (int k = 0; k < 60; k++)
                    std::snprintf(hex + k * 2, 3, "%02x", u.raw[k]);
                LOG("%s PANEL: car %d is in the race (P%d) but its participants entry has no name: shown as 'Car %d'. Entry: %s", tstamp().c_str(), i, u.pos, i, hex);
            }
            r.team = u.team;
            r.pit = u.pit != 0;
            r.out = u.res >= 4;   // 4 DNF, 5 DSQ, 7 retired
            r.inactive = u.res < 2 || u.pos == 0;   // not in the session list yet (0 invalid, 1 inactive), or no position
            r.frac = bs.len > 0 ? double(u.dist) / double(bs.len) : 0.0;
            // derived events: pit in/out, a car that was fast and is now slow on the track
            static bool prev_pit[24];
            static double hi_t[24], slow_t[24], stop_cd[24];
            static bool seen_once[24];
            if (seen_once[i] && prev_pit[i] != r.pit && !r.inactive)
                bp_event(r.pit ? "PITI" : "PITO", i, 255);
            seen_once[i] = true;
            prev_pit[i] = r.pit;
            double nowt = now();
            if (u.kmh >= 120)
                hi_t[i] = nowt;
            if (u.kmh < 25 && !r.pit && !r.inactive && !r.out && u.drv != 0 && nowt - hi_t[i] < 5.0) {
                if (slow_t[i] == 0)
                    slow_t[i] = nowt;
                else if (nowt - slow_t[i] > 1.5 && stop_cd[i] < nowt) {
                    stop_cd[i] = nowt + 25.0;
                    bp_event("STOP", i, 255);
                }
            } else {
                slow_t[i] = 0;
            }
            r.has_gap = true;
            r.gap = double(u.lead) / 1000.0;
            r.interval = double(u.gap) / 1000.0;
            r.cmp = u.cmp == 16 ? 'S' : u.cmp == 17 ? 'M' : u.cmp == 18 ? 'H' : u.cmp == 7 ? 'I' : u.cmp == 8 ? 'W' : '?';
            auto &c = r.c;
            c.speed = u.kmh;
            c.rpm = u.rpm;
            c.gear = u.gear;
            c.throttle = u.thr;
            c.brake = u.brk;
            c.steer = u.steer;
            for (int j = 0; j < 4; j++) {
                c.tyre_temp[j] = u.tt[j];
                c.tyre_wear[j] = u.wear[j];
            }
            c.fuel_laps = u.fuel_laps;
            c.position = u.pos;
            c.lap = u.lap;
            c.sector = u.sector;
            c.last_lap_time = float(double(u.last) / 1000.0);
            c.current_lap_time = float(double(u.cur) / 1000.0);
            c.best_lap_time = float(double(u.best) / 1000.0);
            c.lap_distance = u.dist;
            rows.push_back(r);
        }
        // yellow zones appear / clear
        static int8_t prev_zf[21];
        static bool zf_init = false;
        bool any_yellow = false;
        for (int k = 0; k < bs.nz; k++) {
            if (zf_init && prev_zf[k] != bs.zf[k] && (bs.zf[k] == 3 || prev_zf[k] == 3))
                bp_event(bs.zf[k] == 3 ? "YELL" : "CLR", k, uint8_t(std::min(255, int(bs.zs[k] * 100.0f))));
            prev_zf[k] = bs.zf[k];
            any_yellow |= bs.zf[k] == 3;
        }
        zf_init = true;
        const char *fl = bs.sc == 1 ? "sc" : bs.sc == 2 ? "vsc" : bs.sc == 3 ? "formation" : any_yellow ? "yellow" : "green";
        si = {bs.type, bs.track, bs.weather, bs.laps, float(bs.left), float(bs.ttemp), float(bs.atemp), fl};
    }
    if (rows.empty()) {
        std::lock_guard<std::mutex> g(bp_mu);
        bp_state = bp_not_ready(api);
        return;
    }
    // the unit of "speed" in the launcher API is not documented: above 110 it can only be km/h (110 m/s = 396 km/h)
    double to_kmh = udp || max_speed > 110.0f ? 1.0 : 3.6;
    float L = track_len > 500.0f ? track_len : 5000.0f;
    for (auto &r : rows) {
        r.kmh = double(r.c.speed) * to_kmh;
        r.total = double(r.c.lap) * L + double(r.c.lap_distance);
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.inactive != b.inactive)
            return !a.inactive;
        int pa = a.c.position > 0 ? a.c.position : 99, pb = b.c.position > 0 ? b.c.position : 99;
        return pa != pb ? pa < pb : a.total > b.total;
    });
    for (size_t k = 0; k < rows.size(); k++) {
        if (rows[k].has_gap)
            continue;
        double v = std::max(rows[k].kmh / 3.6, 20.0);
        rows[k].gap = k ? std::max(0.0, (rows[0].total - rows[k].total) / v) : 0.0;
        rows[k].interval = k ? std::max(0.0, (rows[k - 1].total - rows[k].total) / v) : 0.0;
    }
    int lead_lap = int(rows[0].c.lap);

    std::string o;
    o.reserve(8192);
    o += std::string("{\"ready\":true,\"src\":\"") + (udp ? "udp" : "api") + "\",\"seq\":" + std::to_string(unsigned(now() * 10)) +
         ",\"session\":{\"type\":" + std::to_string(si.type) + ",\"track\":" + std::to_string(si.track) + ",\"flag\":\"" + si.flag +
         "\",\"weather\":" + std::to_string(si.weather) + ",\"lap\":" + std::to_string(lead_lap) + ",\"laps\":" + std::to_string(si.laps) +
         ",\"time_left\":";
    bp_num(o, si.left, 0);
    o += ",\"track_temp\":";
    bp_num(o, si.ttemp, 0);
    o += ",\"air_temp\":";
    bp_num(o, si.atemp, 0);
    o += "},\"watch\":{";
    o += std::string("\"active\":") + (active ? "true" : "false") + ",\"car\":" + std::to_string(g_view_car.load()) +
         ",\"camera\":" + std::to_string(g_view_cam.load()) + "},\"dir\":" + std::to_string(g_dir_paused.load()) + ",\"cars\":[";
    for (size_t k = 0; k < rows.size(); k++) {
        const Row &r = rows[k];
        const auto &c = r.c;
        if (k)
            o += ',';
        o += "{\"i\":" + std::to_string(r.i) + ",\"name\":";
        bp_json_str(o, r.name);
        o += ",\"team\":" + std::to_string(r.team) + ",\"pos\":" + (r.inactive ? std::string("0") : std::to_string(c.position > 0 ? int(c.position) : int(k + 1))) + ",\"lap\":" + std::to_string(c.lap) +
             ",\"sector\":" + std::to_string(c.sector + 1) + ",\"gap\":";
        bp_num(o, r.gap, 1);
        o += ",\"int\":";
        bp_num(o, r.interval, 1);
        o += ",\"speed\":";
        bp_num(o, r.kmh, 0);
        o += ",\"rpm\":";
        bp_num(o, c.rpm, 0);
        o += ",\"gear\":" + std::to_string(c.gear) + ",\"thr\":";
        bp_num(o, c.throttle, 2);
        o += ",\"brk\":";
        bp_num(o, c.brake, 2);
        o += ",\"steer\":";
        bp_num(o, c.steer, 2);
        o += ",\"tyre_temp\":[";
        for (int j = 0; j < 4; j++) {
            if (j)
                o += ',';
            bp_num(o, c.tyre_temp[j], 0);
        }
        o += "],\"tyre_wear\":[";
        for (int j = 0; j < 4; j++) {
            if (j)
                o += ',';
            bp_num(o, c.tyre_wear[j], 1);
        }
        o += "],\"fuel_laps\":";
        bp_num(o, c.fuel_laps, 1);
        o += ",\"last\":";
        bp_num(o, bp_secs(c.last_lap_time), 3);
        o += ",\"best\":";
        bp_num(o, bp_secs(c.best_lap_time), 3);
        o += ",\"cur\":";
        bp_num(o, bp_secs(c.current_lap_time), 3);
        o += ",\"d\":";
        bp_num(o, r.frac, 4);
        o += std::string(",\"compound\":\"") + (r.cmp ? r.cmp : '?') + "\",\"pit\":" + (r.pit ? "true" : "false") + ",\"out\":" + (r.out ? "true" : "false") + "}";
    }
    o += "],\"zones\":[";
    if (udp) {
        std::lock_guard<std::mutex> g(bu_mu);
        for (int k = 0; k < bs.nz; k++) {
            if (k)
                o += ',';
            o += "{\"s\":";
            bp_num(o, bs.zs[k], 4);
            o += ",\"f\":" + std::to_string(bs.zf[k]) + "}";
        }
    }
    o += "],\"events\":[";
    {
        std::lock_guard<std::mutex> g(bu_mu);
        double nowt = now();
        int shown = 0;
        for (size_t k = be.size(); k-- > 0 && shown < 30; shown++) {
            const BpE &e = be[k];
            if (shown)
                o += ',';
            o += "{\"id\":" + std::to_string(e.id) + ",\"ago\":";
            bp_num(o, nowt - e.t, 0);
            o += std::string(",\"k\":\"") + e.k + "\",\"a\":" + std::to_string(e.a) + ",\"b\":" + std::to_string(e.b) + "}";
        }
    }
    o += "]}";
    {
        std::lock_guard<std::mutex> g(bp_mu);
        bp_state = std::move(o);
    }
    // the units are not documented: one sample line for the log, so a wrong guess can be fixed
    static double sample_at = 0;
    if (now() >= sample_at) {
        sample_at = now() + (sample_at == 0 ? 20.0 : 300.0);
        const auto &c = rows[0].c;
        if (udp) {
            std::lock_guard<std::mutex> g(bu_mu);
            std::string who;
            int named = 0;
            for (int i = 0; i < 24; i++)
                if (bu[i].name[0]) {
                    named++;
                    if (bu[i].res < 2 || bu[i].pos == 0) {
                        char t[96];
                        std::snprintf(t, sizeof t, " [%d %s res%d pos%d]", i, bu[i].name, bu[i].res, bu[i].pos);
                        who += t;
                    }
                }
            LOG("%s PANEL cars: %d named, %zu shown; not in the race list:%s", tstamp().c_str(), named, rows.size(), who.empty() ? " none" : who.c_str());
        }
        LOG("%s PANEL sample (leader): source %s, speed raw %.1f (max seen %.1f -> %s), rpm %.0f, last lap %.3f, cur lap %.3f, tyre wear %.2f %.2f, "
            "tyre temp %.0f %.0f, fuel laps %.2f, lap %u dist %.0f, %zu car(s), session type %u",
            tstamp().c_str(), udp ? "UDP" : "launcher API", c.speed, max_speed, to_kmh == 1.0 ? "km/h" : "m/s", c.rpm, c.last_lap_time, c.current_lap_time,
            c.tyre_wear[0], c.tyre_wear[1], c.tyre_temp[0], c.tyre_temp[1], c.fuel_laps, c.lap, c.lap_distance, rows.size(), si.type);
    }
}

// The panel's address, copied to the clipboard when the panel starts (the settings screen has no way to copy it) and opened in the
// default browser by the "open" switch or the auto-open option. The game window is kept running when it loses focus (bg_run).
std::string bp_url()
{
    return "http://127.0.0.1:" + std::string(std::string_view(panel_port)) + "/";
}

void bp_copy_url()
{
    std::string u = bp_url();
    if (!OpenClipboard(nullptr))
        return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, u.size() + 1)) {
        std::memcpy(GlobalLock(h), u.c_str(), u.size() + 1);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_TEXT, h))
            GlobalFree(h);
    }
    CloseClipboard();
}

void bp_open_browser(const char *why)
{
    std::string u = bp_url();
    HINSTANCE r = ShellExecuteA(nullptr, "open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    LOG("%s PANEL browser opened (%s): %s -> %s", tstamp().c_str(), why, u.c_str(), reinterpret_cast<INT_PTR>(r) > 32 ? "ok" : "failed");
}

// A click that the game has not followed yet (read by the frame's timing)
bool want_car_pending()
{
    return bp_cmd_car.load() >= 0;
}

// Runs on the game's frame: the click commands, and the snapshot at 10 Hz while a page is polling.
void bp_frame()
{
    static double next = 0;
    static bool url_copied = false, auto_done = false;
    if (bp_new_listen.exchange(false))
        url_copied = false;   // a new port: copy the new address
    if (bp_status.load() != 1) {
        url_copied = false;
        return;
    }
    if (!url_copied) {
        url_copied = true;
        bp_copy_url();
        LOG("%s PANEL: the address %s is on the clipboard", tstamp().c_str(), bp_url().c_str());
        if (std::string_view(panel_mode) == "on_open" && !auto_done) {
            auto_done = true;
            bp_open_browser("auto-open");
        }
    }
    double t = now();
    if (t < next)
        return;
    bool client = t - bp_last_req.load() < 3.0;
    next = t + (client || want_car_pending() ? 0.1 : 0.5);
    bp_dir_on = active;
    // A click is kept until the game really follows that car: follow() is repeated every 0.4 s (for up to 4 s), because one call can be
    // lost (the game is busy, the director is not ready yet, or something moved the camera back).
    static int want_car = -1, want_tries = 0;
    static double want_until = 0, want_next = 0;
    int car = bp_cmd_car.exchange(-1);
    if (car >= 0) {
        want_car = car;
        want_tries = 0;
        want_until = t + 4.0;
        want_next = 0;
    }
    if (want_car >= 0) {
        int following = g_view_car.load();   // updated by the tick (5 times a second while spectating)
        if (following == want_car) {
            LOG("%s PANEL click: now following car %d (%d call%s)", tstamp().c_str(), want_car, want_tries, want_tries == 1 ? "" : "s");
            want_car = -1;
        } else if (t > want_until) {
            LOG("%s PANEL click: the game did not switch to car %d in 4 s (%d calls, following %d, spectating %s)", tstamp().c_str(), want_car,
                want_tries, following, active ? "on" : "off");
            want_car = -1;
        } else if (active && t >= want_next) {
            want_next = t + 0.4;
            want_tries++;
            auto r = swofty::spectator::follow(swofty::cars::live::Car(uint32_t(want_car)));
            if (want_tries == 1 || r != swofty::Result::ok)
                LOG("%s PANEL click: follow car %d (try %d) -> %s", tstamp().c_str(), want_car, want_tries, swofty::result_name(r));
        }
    }
    int cam = bp_cmd_cam.exchange(0);
    if (cam == 1 || cam == 2) {
        auto r = swofty::spectator::set_camera(cam == 1 ? swofty::spectator::Camera::tv_pod : swofty::spectator::Camera::broadcast);
        LOG("%s PANEL click: camera %s -> %s", tstamp().c_str(), cam == 1 ? "TV pod" : "broadcast", swofty::result_name(r));
    }
    if (client) {
        // the launcher's telemetry API is looked at every 5 s: if it has a session the page is built here (it needs the game thread),
        // otherwise (UDP) on the HTTP thread when a page asks, so the game thread does not pay for it
        static double api_next = 0;
        if (t >= api_next) {
            api_next = t + 5.0;
            swofty::telemetry::SessionState ss{};
            bp_api_mode = swofty::telemetry::session(&ss) == swofty::telemetry::Result::ok && ss.participant_count > 0;
        }
        if (bp_api_mode.load())
            bp_build(true);
    }
}

// ---- HTTP ---------------------------------------------------------------------------------------------------------------

std::string bp_lower(std::string s)
{
    for (auto &c : s)
        c = char(std::tolower(uint8_t(c)));
    return s;
}

std::string bp_header(const std::string &lreq, const std::string &req, const std::string &name)
{
    size_t p = lreq.find("\r\n" + name + ":");
    if (p == std::string::npos)
        return "";
    p += name.size() + 3;
    size_t e = req.find("\r\n", p);
    std::string v = req.substr(p, e == std::string::npos ? std::string::npos : e - p);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
        v.erase(v.begin());
    return v;
}

void bp_send(SOCKET c, const char *status, const char *type, const std::string &body)
{
    std::string h = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
                    "Content-Security-Policy: default-src 'self' 'unsafe-inline'\r\nConnection: close\r\n\r\n";
    h += body;
    const char *p = h.data();
    size_t left = h.size();
    while (left) {
        int n = send(c, p, int(std::min<size_t>(left, 65536)), 0);
        if (n <= 0)
            break;
        p += n;
        left -= size_t(n);
    }
}

void bp_serve(SOCKET c, int port, const std::string &token)
{
    DWORD timeout = 400;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof timeout);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof timeout);
    std::string req;
    char buf[2048];
    while (req.size() < 8192 && req.find("\r\n\r\n") == std::string::npos) {
        int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0)
            break;
        req.append(buf, size_t(n));
    }
    size_t le = req.find("\r\n");
    if (le == std::string::npos || req.find("\r\n\r\n") == std::string::npos)
        return;
    std::string line = req.substr(0, le), lreq = bp_lower(req.substr(le));
    size_t s1 = line.find(' '), s2 = line.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos)
        return;
    std::string method = line.substr(0, s1), target = line.substr(s1 + 1, s2 - s1 - 1);
    std::string path = target.substr(0, target.find('?')), query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
    std::string host = bp_lower(bp_header(lreq, req.substr(le), "host"));
    std::string origin = bp_lower(bp_header(lreq, req.substr(le), "origin"));
    std::string xtoken = bp_header(lreq, req.substr(le), "x-token");
    auto qparam = [&](const std::string &k) {
        std::string q = "&" + query;
        size_t p = q.find("&" + k + "=");
        if (p == std::string::npos)
            return std::string();
        size_t b = p + k.size() + 2, e = q.find('&', b);
        return q.substr(b, e == std::string::npos ? std::string::npos : e - b);
    };
    std::string ps = std::to_string(port);
    if (!(host == "127.0.0.1:" + ps || host == "localhost:" + ps || host == "[::1]:" + ps)) {   // this PC only, and no DNS rebinding
        bp_send(c, "403 Forbidden", "text/plain", "wrong host");
        return;
    }
    if (method == "GET" && (path == "/" || path == "/index.html")) {
        std::string page = BP_PAGE;
        size_t t = page.find("@@TOKEN@@");
        if (t != std::string::npos)
            page.replace(t, 9, token);
        bp_send(c, "200 OK", "text/html; charset=utf-8", page);
    } else if (method == "GET" && path == "/api/state") {
        bp_last_req = now();
        if (!bp_api_mode.load()) {
            static std::atomic<double> built_at{0};
            if (now() - built_at.load() >= 0.1) {
                built_at = now();
                LARGE_INTEGER f, t0, t1;
                QueryPerformanceFrequency(&f);
                QueryPerformanceCounter(&t0);
                bp_build(false);
                QueryPerformanceCounter(&t1);
                bp_pages_n++;
                bp_pages_us = bp_pages_us.load() + double(t1.QuadPart - t0.QuadPart) * 1e6 / double(f.QuadPart);
            }
        }
        std::string s;
        {
            std::lock_guard<std::mutex> g(bp_mu);
            s = bp_state;
        }
        bp_send(c, "200 OK", "application/json", s);
    } else if (method == "POST" && path == "/api/director") {
        bool origin_ok = origin.empty() || origin.find("//" + host) != std::string::npos;
        if (xtoken != token || !origin_ok) {
            bp_send(c, "403 Forbidden", "application/json", "{\"ok\":false,\"error\":\"not allowed\"}");
            return;
        }
        if (qparam("mode") != "resume") {
            bp_send(c, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":\"bad request\"}");
            return;
        }
        g_tvd_resume = true;
        bp_send(c, "200 OK", "application/json", "{\"ok\":true}");
    } else if (method == "POST" && (path == "/api/watch" || path == "/api/cam")) {
        bool origin_ok = origin.empty() || origin.find("//" + host) != std::string::npos;
        if (xtoken != token || !origin_ok) {
            bp_send(c, "403 Forbidden", "application/json", "{\"ok\":false,\"error\":\"not allowed\"}");
            return;
        }
        if (!bp_dir_on.load()) {
            bp_send(c, "200 OK", "application/json", "{\"ok\":false,\"error\":\"Spectate is off in the game: press the spectate key first.\"}");
            return;
        }
        if (path == "/api/watch") {
            int car = std::atoi(qparam("car").c_str());
            if (car < 0 || car >= 32) {
                bp_send(c, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":\"bad car\"}");
                return;
            }
            bp_cmd_car = car;
        } else {
            int mode = std::atoi(qparam("mode").c_str());
            if (mode != 1 && mode != 2) {
                bp_send(c, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":\"bad camera\"}");
                return;
            }
            bp_cmd_cam = mode;
        }
        bp_send(c, "200 OK", "application/json", "{\"ok\":true}");
    } else {
        bp_send(c, "404 Not Found", "text/plain", "not found");
    }
}

SOCKET bp_listen(int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(s, 8) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

std::string bp_token()
{
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    uint64_t b = uint64_t(qpc.QuadPart) * 0x9E3779B97F4A7C15ull ^ (uint64_t(GetCurrentProcessId()) << 32) ^ GetTickCount64();
    char t[24];
    std::snprintf(t, sizeof t, "%08x%08x", unsigned(b >> 32), unsigned(b * 2654435761u));
    return t;
}

struct BpConn {
    SOCKET c;
    int port;
    std::string token;
};
std::atomic<int> bp_conns{0};

DWORD WINAPI bp_conn_main(void *p)
{
    std::unique_ptr<BpConn> k(static_cast<BpConn *>(p));
    bp_serve(k->c, k->port, k->token);
    closesocket(k->c);
    bp_conns--;
    return 0;
}

DWORD WINAPI bp_main(void *)
{
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0)
        return 0;
    std::string token = bp_token();
    SOCKET ls = INVALID_SOCKET;
    int bound = 0;
    bool busy_logged = false;
    for (;;) {
        bool want = panel_on();
        int port = std::atoi(std::string(std::string_view(panel_port)).c_str());
        if (ls != INVALID_SOCKET && (!want || port != bound)) {
            closesocket(ls);
            ls = INVALID_SOCKET;
            bp_status = 0;
            LOG("web panel off");
        }
        if (!want) {
            busy_logged = false;
            Sleep(500);
            continue;
        }
        if (ls == INVALID_SOCKET) {
            ls = bp_listen(port);
            if (ls == INVALID_SOCKET) {
                bp_status = 2;
                if (!busy_logged)
                    LOG("web panel: port %d is busy; pick another port in the settings", port);
                busy_logged = true;
                Sleep(2000);
                continue;
            }
            busy_logged = false;
            bound = port;
            bp_status = 1;
            bp_new_listen = true;
            LOG("web panel on: http://127.0.0.1:%d/ (this PC only; no UDP port is used)", port);
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        timeval tv{0, 200000};
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0)
            continue;
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c != INVALID_SOCKET) {
            if (bp_conns.load() >= 16) {
                closesocket(c);
                continue;
            }
            bp_conns++;
            auto *k = new BpConn{c, port, token};
            if (HANDLE h = CreateThread(nullptr, 0, bp_conn_main, k, 0, nullptr))
                CloseHandle(h);
            else {
                bp_conns--;
                closesocket(c);
                delete k;
            }
        }
    }
}
