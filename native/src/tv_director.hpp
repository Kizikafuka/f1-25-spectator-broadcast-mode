// SPDX-License-Identifier: MIT
// TV director (Broadcast style): decides WHICH driver the spectator camera should follow in a race, like a person clicking a name
// in the leaderboard. It never moves the camera itself. The rules follow the recipe in TVDirector_HowItWorks.md (written for
// F1 Manager 2024) with the numbers of its "Broadcast" preset. The data comes from the game's UDP telemetry (format 2026):
//   session packet (id 1): session type, total laps, paused       lap data (id 2): position, lap, gaps, lap distance, pit, result
//   car telemetry (id 6): speed                                     event packet (id 3): OVTK, COLL, RTMT, LGOT, CHQF, RCWN
// feed_* is called on the UDP thread, decide() on the game thread (every 0.2 s). All times are seconds on one clock the caller passes in.
//
// What it does (races only, nothing before the lights go out):
//   incident     a collision, a retirement or a car stopped on track: cut after a short reaction delay, interrupts almost anything
//   pit stop     the leaders' stops (top 6) cut away from a fight; the others only when nothing else is going on
//   overtake     show the car that made the pass for 10 s (a pass is only shown if the current shot is over within 10 s); the same pair is not shown again for 90 s
//   battle       a car within 0.7 s of the one in front (kept up to 1.5 s); stays while the fight lasts, dropped when it is stale
//   quiet        the leaders in turn, 25 s each
//   start        the first 30 s after the lights go out: the leaders, in turn
//   last lap     the leader for 20 s when he starts it, again for the last 25 s to the flag
//   chequered    the winner for 6 s, then fights for position still heading to the line; finished cars are not shown again
// Every shot is held at least 20 s (incidents, the start, the last lap and the finish are the exceptions). Retired cars are never shown
// except at the moment they retire. If you change the camera yourself, the director waits 10 s.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace tvd {

constexpr int MAXC = 24;

enum class Kind { none, quiet, pit_other, battle, overtake, pit_front, start, last_lap, finish, incident };

inline int prio(Kind k)
{
    switch (k) {
    case Kind::quiet: return 1;
    case Kind::pit_other: return 2;
    case Kind::battle: return 3;
    case Kind::overtake: return 4;
    case Kind::pit_front: return 5;
    case Kind::start: return 7;
    case Kind::last_lap: return 8;
    case Kind::finish: return 9;
    case Kind::incident: return 10;
    default: return 0;
    }
}

inline const char *kind_name(Kind k)
{
    switch (k) {
    case Kind::quiet: return "quiet moment";
    case Kind::pit_other: return "pit stop";
    case Kind::battle: return "battle";
    case Kind::overtake: return "overtake";
    case Kind::pit_front: return "pit stop (front)";
    case Kind::start: return "race start";
    case Kind::last_lap: return "last lap";
    case Kind::finish: return "finish";
    case Kind::incident: return "incident";
    default: return "-";
    }
}

struct Pick {
    int car = -1;                 // -1 = leave the camera alone
    Kind kind = Kind::none;
    std::string why;
    bool user_override = false;   // car is -1 and why says the director stepped back
};

class Director {
public:
    // ---- UDP thread -----------------------------------------------------------------------------------------------
    void feed_session(const uint8_t *p, int n, double t)
    {
        if (n < 47)
            return;
        std::lock_guard<std::mutex> g(mu_);
        int type = p[35], laps = p[32];
        bool paused = p[43] != 0;
        // F1 24/25 numbering: 15, 16, 17 = Race, Race 2, Race 3. 1..14 and 18 are practice, qualifying, shootouts and time trial.
        bool known_other = (type >= 1 && type <= 14) || type == 18;
        bool race = !known_other;
        if (type != session_type_ || laps != total_laps_ || race != race_) {
            char b[200];
            snprintf(b, sizeof b, "session packet: type %d (%s), total laps %d, paused %d", type, race ? "race" : "not a race: director idle", laps,
                     paused ? 1 : 0);
            logs_.push_back(b);
        }
        session_type_ = type;
        total_laps_ = laps;
        paused_ = paused;
        race_ = race;
        last_data_ = t;
    }

    void feed_lap(const uint8_t *p, int n, double t)
    {
        int cnt = std::min(MAXC, (n - 29) / 57);
        if (cnt <= 0)
            return;
        std::lock_guard<std::mutex> g(mu_);
        for (int i = 0; i < cnt; i++) {
            const uint8_t *e = p + 29 + 57 * i;
            Car &c = cars_[i];
            c.result = e[45];
            c.seen = c.result != 0;
            c.last_ms = u32(e), c.cur_ms = u32(e + 4);
            c.gap_front = u16(e + 14) + 60000u * e[16];
            c.gap_leader = u16(e + 17) + 60000u * e[19];
            c.dist = f32(e + 20);
            c.pos = e[32], c.lap = e[33], c.pit = e[34], c.sector = e[36], c.status = e[44];
            if (c.lap > max_lap_)
                max_lap_ = c.lap;
        }
        cars_n_ = cnt;
        last_data_ = t;
        if (!lap_logged_) {
            lap_logged_ = true;
            char b[240];
            std::string s = "lap data: " + std::to_string(cnt) + " car slot(s); first rows (pos lap pit result gapFront ms):";
            for (int i = 0; i < std::min(cnt, 4); i++) {
                snprintf(b, sizeof b, " [car %d: P%d L%d pit%d res%d %u]", i, cars_[i].pos, cars_[i].lap, cars_[i].pit, cars_[i].result,
                         cars_[i].gap_front);
                s += b;
            }
            logs_.push_back(s);
        }
    }

    void feed_telemetry(const uint8_t *p, int n, double t)
    {
        int cnt = std::min(MAXC, (n - 29) / 59);
        if (cnt <= 0)
            return;
        std::lock_guard<std::mutex> g(mu_);
        for (int i = 0; i < cnt; i++) {
            Car &c = cars_[i];
            c.kmh = u16(p + 29 + 59 * i);
            c.kmh_known = true;
        }
        (void)t;
    }

    void feed_event(const uint8_t *p, int n, double t)
    {
        if (n < 34)
            return;
        std::lock_guard<std::mutex> g(mu_);
        char code[5] = {char(p[29]), char(p[30]), char(p[31]), char(p[32]), 0};
        int a = p[33], b = n > 34 ? p[34] : 255;
        if (!std::strcmp(code, "OVTK"))
            ovtk_.push_back({t, a, b, false});
        else if (!std::strcmp(code, "COLL"))
            coll_.push_back({t, a, b, false});
        else if (!std::strcmp(code, "RTMT"))
            rtmt_.push_back({t, a, 255, false});
        else if (!std::strcmp(code, "LGOT")) {
            // lights out = a (new) race start: forget the last race's shots, cooldowns, events and the finish
            ovtk_.clear(), coll_.clear(), rtmt_.clear();
            lights_out_ = t;
            chequered_ = false, chequered_t_ = 0, winner_ = -1, ll_start_ = 0, max_lap_ = 0;
            cur_ = -1, kind_ = Kind::none, last_req_ = -1, paused_until_ = 0;
            pend_ = {};
            std::fill(std::begin(inc_cd_), std::end(inc_cd_), 0.0);
            std::fill(std::begin(pit_cd_), std::end(pit_cd_), 0.0);
            pair_cd_.clear();
            for (Car &c : cars_)
                c.still_since = 0;
            logs_.push_back("event LGOT: lights out, the director starts");
        } else if (!std::strcmp(code, "CHQF")) {
            chequered_ = true;
            chequered_t_ = t;
            winner_ = leader_locked();
            logs_.push_back("event CHQF: chequered flag, the leader is car " + std::to_string(winner_));
        } else if (!std::strcmp(code, "RCWN")) {
            winner_ = a;
            if (!chequered_) {
                chequered_ = true;
                chequered_t_ = t;
            }
        }
        if (events_logged_ < 12) {
            events_logged_++;
            if (!std::strcmp(code, "OVTK") || !std::strcmp(code, "COLL") || !std::strcmp(code, "RTMT"))
                logs_.push_back(std::string("event ") + code + ": car " + std::to_string(a) + (b < 255 ? " / car " + std::to_string(b) : ""));
        }
        while (ovtk_.size() > 40)
            ovtk_.erase(ovtk_.begin());
        while (coll_.size() > 40)
            coll_.erase(coll_.begin());
        while (rtmt_.size() > 40)
            rtmt_.erase(rtmt_.begin());
    }

    // ---- game thread ------------------------------------------------------------------------------------------------
    void reset()
    {
        std::lock_guard<std::mutex> g(mu_);
        for (Car &c : cars_)
            c = Car{};
        cars_n_ = 0;
        max_lap_ = 0;
        ovtk_.clear(), coll_.clear(), rtmt_.clear();
        lights_out_ = 0;
        chequered_ = false, chequered_t_ = 0, winner_ = -1;
        cur_ = -1, kind_ = Kind::none, since_ = 0, last_req_ = -1, req_t_ = 0, paused_until_ = 0;
        pend_ = {};
        std::fill(std::begin(inc_cd_), std::end(inc_cd_), 0.0);
        std::fill(std::begin(pit_cd_), std::end(pit_cd_), 0.0);
        pair_cd_.clear();
        battle_front_ = -1, battle_gap_best_ = 1e9, battle_progress_ = 0, battle_apart_ = 0;
        ll_start_ = 0;
        lap_logged_ = false;
        events_logged_ = 0;
    }

    // forget the current shot (the caller stopped spectating): the next decision starts fresh
    void drop_shot()
    {
        std::lock_guard<std::mutex> g(mu_);
        cur_ = -1, kind_ = Kind::none, last_req_ = -1;
    }

    // somebody else (the caller's own fallback) moved the camera to this car: not the viewer's doing
    void note_follow(int car, double t)
    {
        std::lock_guard<std::mutex> g(mu_);
        cur_ = car, kind_ = Kind::quiet, since_ = t, last_req_ = car, req_t_ = t;
    }

    std::vector<std::string> take_logs()
    {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<std::string> out;
        out.swap(logs_);
        return out;
    }

    // is there fresh race data? (for the caller's own fallback)
    bool has_data(double t)
    {
        std::lock_guard<std::mutex> g(mu_);
        return race_ && cars_n_ > 0 && t - last_data_ < 1.5;
    }

    // valid(car) -> whether the game says the car can be shown (not yours, still racing)
    template <class Valid> Pick decide(double t, int following, Valid valid)
    {
        std::lock_guard<std::mutex> g(mu_);
        Pick out;
        if (!race_ || cars_n_ <= 0 || t - last_data_ > 1.5 || paused_)
            return out;
        if (last_req_ >= 0 && following >= 0 && following != last_req_ && t - req_t_ > 2.0) {
            paused_until_ = t + 10.0;
            last_req_ = -1, cur_ = -1, kind_ = Kind::none;
            out.user_override = true;
            out.why = "you changed the camera: the director waits 10 s";
            return out;
        }
        if (t < paused_until_)
            return out;
        const bool started = lights_out_ > 0 || max_lap_ >= 2;
        if (!started)
            return out;

        auto racing = [&](int c) {
            return c >= 0 && c < cars_n_ && cars_[c].seen && cars_[c].result == 2 && cars_[c].pit == 0 && valid(c);
        };
        auto in_pits = [&](int c) { return c >= 0 && c < cars_n_ && cars_[c].seen && cars_[c].result == 2 && cars_[c].pit != 0; };
        auto by_pos = [&](int pos) {
            for (int i = 0; i < cars_n_; i++)
                if (cars_[i].seen && cars_[i].pos == pos)
                    return i;
            return -1;
        };
        int leader = by_pos(1);

        track(t, racing, by_pos);

        // the leader's time to the flag on the last lap (the previous lap's time minus this lap so far)
        double rem = 999.0;
        if (leader >= 0 && total_laps_ > 0 && cars_[leader].lap >= total_laps_ && cars_[leader].last_ms > cars_[leader].cur_ms && cars_[leader].cur_ms > 0)
            rem = double(cars_[leader].last_ms - cars_[leader].cur_ms) / 1000.0;

        // ---- is the current shot over? ----
        bool ended = cur_ < 0;
        if (!ended) {
            double age = t - since_;
            bool cur_ok;
            switch (kind_) {
            case Kind::incident:
            case Kind::finish: cur_ok = true; break;
            case Kind::pit_front:
            case Kind::pit_other: cur_ok = cars_[cur_].seen && cars_[cur_].result == 2 && valid(cur_); break;
            default: cur_ok = racing(cur_);
            }
            switch (kind_) {
            case Kind::overtake: ended = age >= 10.0; break;
            case Kind::incident: ended = age >= 9.0; break;
            case Kind::start: ended = age >= 10.0; break;      // the rotation of the leaders moves on every 10 s
            case Kind::quiet: ended = age >= 25.0; break;
            case Kind::finish: ended = age >= 6.0; break;
            case Kind::last_lap: ended = age >= 20.0 && rem > 25.0; break;
            case Kind::pit_front:
            case Kind::pit_other: ended = (cars_[cur_].pit == 0 && age >= 5.0) || age >= 40.0; break;
            case Kind::battle: {
                const Car &c = cars_[cur_];
                bool close = c.gap_front > 0 && c.gap_front <= 1500;
                if (close)
                    battle_apart_ = 0;
                else if (battle_apart_ == 0)
                    battle_apart_ = t;
                bool stale = age >= 6.0 && t - battle_progress_ >= 25.0;
                ended = (battle_apart_ > 0 && t - battle_apart_ >= 4.0) || age >= 45.0 || stale;
                if (ended && battle_front_ >= 0)
                    pair_cd_[pair_key(battle_front_, cur_)] = t + (stale ? 120.0 : 20.0);   // a fight where nothing happens: leave the pair alone
                break;
            }
            default: ended = true;
            }
            if (!cur_ok)
                ended = true;                                   // the shown car is out, in the pits or not valid any more
        }

        // ---- candidates ----
        Cand best;
        auto offer = [&](int car, Kind k, std::string why) {
            if (car < 0)
                return;
            if (prio(k) > prio(best.kind))
                best = {car, k, std::move(why)};
        };

        // incident: collision, retirement, a car stopped on track
        if (pend_.car >= 0 && t >= pend_.due) {
            if (inc_cd_[pend_.car] > t)
                pend_ = {};
            else
                offer(pend_.car, Kind::incident, pend_.why);
        }
        // finish: the winner, then the rest of the finish below
        if (chequered_ && winner_ >= 0 && t - chequered_t_ < 6.0)
            offer(winner_, Kind::finish, "chequered flag: the winner");
        // last lap
        if (!chequered_ && total_laps_ > 0 && leader >= 0 && cars_[leader].lap >= total_laps_ && racing(leader)) {
            if (ll_start_ == 0)
                ll_start_ = t;
            if (t - ll_start_ < 20.0)
                offer(leader, Kind::last_lap, "last lap: the leader");
            else if (rem <= 25.0)
                offer(leader, Kind::last_lap, "last lap: the leader to the flag");
        }
        // race start: the leaders in turn for 30 s
        if (lights_out_ > 0 && t - lights_out_ < 30.0) {
            int idx = int((t - lights_out_) / 10.0) % 3;
            int c = -1;
            for (int k = 0; k < 3 && c < 0; k++) {
                int cand = by_pos(1 + (idx + k) % 3);
                if (racing(cand))
                    c = cand;
            }
            offer(c, Kind::start, "race start: the leaders");
        }
        // pit stops
        for (int i = 0; i < cars_n_; i++) {
            if (!in_pits(i) || !valid(i) || pit_cd_[i] > t)
                continue;
            bool front = cars_[i].pos >= 1 && cars_[i].pos <= 6;
            offer(i, front ? Kind::pit_front : Kind::pit_other, "P" + std::to_string(cars_[i].pos) + " is in the pits");
        }
        // overtakes: a recent pass that was not shown yet
        for (auto &e : ovtk_) {
            if (e.used || t - e.t > 10.0) {
                continue;
            }
            if (pair_cd_[pair_key(e.a, e.b)] > t || !racing(e.a))
                continue;
            offer(e.a, Kind::overtake, "P" + std::to_string(cars_[e.a].pos) + " passed car " + std::to_string(e.b));
        }
        // battles
        bool chaos = (leader >= 0 && cars_[leader].lap <= 1 && lights_out_ > 0 && t - lights_out_ >= 30.0) || chequered_;
        int battle_car = -1;
        double battle_score = 0;
        for (int i = 0; i < cars_n_; i++) {
            if (!racing(i) || cars_[i].pos < 2)
                continue;
            int front = by_pos(cars_[i].pos - 1);
            if (front < 0 || !racing(front))
                continue;
            if (!chaos && cars_[i].pos - 1 > 10)
                continue;                                       // Broadcast: the front of the field
            double gap = cars_[i].gap_front;
            bool continuing = i == cur_ && kind_ == Kind::battle;
            if (gap <= 0 || gap > (continuing ? 1500.0 : 700.0))
                continue;
            if (!continuing && pair_cd_[pair_key(front, i)] > t)
                continue;
            double score = (1000.0 - std::min(gap, 1000.0)) / 500.0 + 1.5 / std::sqrt(double(cars_[i].pos));
            if (cars_[i].gap_old_t > 0 && cars_[i].gap_old > 0 && double(cars_[i].gap_old) - gap >= 150.0)
                score += 0.7;                                   // closing in
            for (auto &e : ovtk_)
                if ((e.a == i || e.b == i) && t - e.t < 20.0)
                    score += 1.0;
            if (continuing)
                score += 1.0;
            if (score > battle_score) {
                battle_score = score;
                battle_car = i;
            }
        }
        if (battle_car >= 0)
            offer(battle_car, Kind::battle, "P" + std::to_string(cars_[battle_car].pos) + " is " + std::to_string(int(cars_[battle_car].gap_front)) + " ms behind P" + std::to_string(cars_[battle_car].pos - 1));
        // quiet moment: the leaders in turn
        {
            int top[3], nt = 0;
            for (int pos = 1; pos <= 3 && nt < 3; pos++)
                if (int c = by_pos(pos); racing(c))
                    top[nt++] = c;
            if (nt > 0) {
                int pick = top[0];
                for (int k = 0; k < nt; k++)
                    if (top[k] == cur_)
                        pick = top[(k + 1) % nt];
                offer(pick, Kind::quiet, "quiet moment: the leaders");
            }
        }
        if (best.car < 0)
            return out;

        // ---- may it cut away from the current shot? ----
        if (!ended && cur_ >= 0) {
            double age = t - since_;
            int cp = prio(kind_), np = prio(best.kind);
            bool allow = false;
            if (best.car == cur_) {
                allow = true;                                   // same car: only the label changes
            } else if (np > cp) {
                if (best.kind == Kind::incident)
                    allow = kind_ != Kind::incident || age >= 6.0;
                else if (np >= 7)
                    allow = kind_ != Kind::incident || age >= 6.0;
                else if (best.kind == Kind::pit_front)
                    allow = age >= 10.0;
                else
                    allow = age >= 20.0;
            }
            if (!allow)
                return out;
        }
        if (best.car == cur_ && !ended) {
            kind_ = best.kind;
            return out;
        }
        // ---- take the shot ----
        if (best.kind == Kind::incident) {
            inc_cd_[best.car] = t + 50.0;
            pend_ = {};
        } else if (best.kind == Kind::overtake) {
            for (auto &e : ovtk_)
                if (e.a == best.car && !e.used && t - e.t <= 10.0) {
                    e.used = true;
                    pair_cd_[pair_key(e.a, e.b)] = t + 90.0;
                }
        } else if (best.kind == Kind::pit_front || best.kind == Kind::pit_other) {
            pit_cd_[best.car] = t + 60.0;
        } else if (best.kind == Kind::battle) {
            battle_front_ = cars_[best.car].pos >= 2 ? by_pos(cars_[best.car].pos - 1) : -1;
            battle_gap_best_ = cars_[best.car].gap_front;
            battle_progress_ = t;
            battle_apart_ = 0;
        }
        if (best.car == cur_) {                                  // the same car again: keep following, new label
            kind_ = best.kind;
            since_ = t;
            return out;
        }
        cur_ = best.car, kind_ = best.kind, since_ = t;
        last_req_ = best.car, req_t_ = t;
        out.car = best.car, out.kind = best.kind;
        out.why = best.why;
        return out;
    }

private:
    struct Car {
        bool seen = false, kmh_known = false;
        uint8_t pos = 0, lap = 0, pit = 0, result = 0, sector = 0, status = 0;
        uint32_t last_ms = 0, cur_ms = 0, gap_front = 0, gap_leader = 0, gap_old = 0;
        double gap_old_t = 0, still_since = 0;
        float dist = 0;
        uint16_t kmh = 0;
    };
    struct Ev {
        double t;
        int a, b;
        bool used;
    };
    struct Cand {
        int car = -1;
        Kind kind = Kind::none;
        std::string why;
    };
    struct Pending {
        int car = -1;
        double due = 0;
        int sev = 0;
        std::string why;
    };

    static uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
    static uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
    static float f32(const uint8_t *p)
    {
        float f;
        std::memcpy(&f, p, 4);
        return f;
    }
    static int pair_key(int a, int b) { return std::min(a, b) * 32 + std::max(a, b); }

    int leader_locked() const
    {
        for (int i = 0; i < cars_n_; i++)
            if (cars_[i].seen && cars_[i].pos == 1)
                return i;
        return -1;
    }

    void want_incident(double t, int car, int sev, double delay, const std::string &why)
    {
        if (car < 0 || car >= MAXC || inc_cd_[car] > t)
            return;
        if (pend_.car >= 0 && pend_.sev >= sev)
            return;
        pend_ = {car, t + delay, sev, why};
    }

    // bookkeeping each decision: new incidents, cars standing still, the gap history for "closing in", battle progress
    template <class Racing, class ByPos> void track(double t, Racing racing, ByPos by_pos)
    {
        for (auto &e : coll_)
            if (!e.used) {
                e.used = true;
                int c = racing(e.a) ? e.a : (racing(e.b) ? e.b : -1);
                if (c < 0)
                    c = e.a;
                want_incident(t, c, 2, 1.8, "collision of car " + std::to_string(e.a) + " and car " + std::to_string(e.b));
            }
        for (auto &e : rtmt_)
            if (!e.used) {
                e.used = true;
                want_incident(t, e.a, 3, 1.0, "car " + std::to_string(e.a) + " retired");
            }
        for (int i = 0; i < cars_n_; i++) {
            Car &c = cars_[i];
            if (!c.seen)
                continue;
            bool still = c.result == 2 && c.pit == 0 && c.kmh_known && c.kmh < 8 && c.lap >= 1 && lights_out_ > 0 && t - lights_out_ > 15.0;
            if (still) {
                if (c.still_since == 0)
                    c.still_since = t;
                else if (t - c.still_since >= 4.0)
                    want_incident(t, i, 1, 0.0, "car " + std::to_string(i) + " is stopped on the track");
            } else {
                c.still_since = 0;
            }
            if (c.gap_old_t == 0 || t - c.gap_old_t >= 2.0) {
                c.gap_old = c.gap_front;
                c.gap_old_t = t;
            }
        }
        // a car shown in a battle: any progress (it gets 100 ms closer than its best, or the pass happens) resets the stale timer
        if (kind_ == Kind::battle && cur_ >= 0 && cur_ < cars_n_) {
            double g = cars_[cur_].gap_front;
            if (g > 0 && g + 100.0 <= battle_gap_best_) {
                battle_gap_best_ = g;
                battle_progress_ = t;
            }
            if (battle_front_ >= 0 && cars_[cur_].pos < cars_[battle_front_].pos) {
                battle_front_ = by_pos(cars_[cur_].pos + 1);
                battle_progress_ = t;
                battle_gap_best_ = 1e9;
            }
        }
        for (auto &e : ovtk_)
            if (!e.used && t - e.t > 10.0)
                e.used = true;
    }

    std::mutex mu_;
    Car cars_[MAXC];
    int cars_n_ = 0, max_lap_ = 0, session_type_ = 0, total_laps_ = 0;
    bool paused_ = false, race_ = false;
    double last_data_ = 0;
    std::vector<Ev> ovtk_, coll_, rtmt_;
    double lights_out_ = 0, chequered_t_ = 0;
    bool chequered_ = false;
    int winner_ = -1;
    int cur_ = -1, last_req_ = -1;
    Kind kind_ = Kind::none;
    double since_ = 0, req_t_ = 0, paused_until_ = 0;
    Pending pend_;
    double inc_cd_[MAXC] = {}, pit_cd_[MAXC] = {};
    std::map<int, double> pair_cd_;
    int battle_front_ = -1;
    double battle_gap_best_ = 1e9, battle_progress_ = 0, battle_apart_ = 0, ll_start_ = 0;
    std::vector<std::string> logs_;
    bool lap_logged_ = false;
    int events_logged_ = 0;
};

}  // namespace tvd
