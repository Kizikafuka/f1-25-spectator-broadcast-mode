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
// Presets (set_preset): broadcast = the original numbers; top10 = overtakes, pit stops, battles and the quiet shots only for the first 10
// (collisions and retirements are shown at any position); action = every car on the lead lap, shorter shots, battles from 1.0 s.
// Lapped cars are never shown in a battle or an overtake. A battle shot ends when the gap is over 1.2 s for 2.5 s.
// Holding (0.1.9): a fight or a pass that is still going is not left for another fight or pass. A newcomer of the same kind never cuts in
// (unless it is 5 places nearer the front and the current pair is not about to swap); a higher kind (a pass over a fight, a leader's pit
// stop over a fight) waits 25 s (preset every action: 17 s). Incidents, the start, the last lap and the finish still cut in.
// Manual camera changes (the game's keys or the web panel) pause the director for 60 s; every further manual change starts the 60 s again.
// Every shot is held at least 20 s (incidents, the start, the last lap and the finish are the exceptions). Retired cars are never shown
// except at the moment they retire.
// QUALIFYING (session types 5-14: qualifying, short/one-shot qualifying, sprint shootouts), same switch, same 60 s pause after a manual change:
//   flying lap   a car that starts a flying lap (driver status 1) is shown, the better qualifying position first; it is followed for up to 25 s
//   purple       a car that sets the fastest sector 1, 2 or 3, or the fastest lap, so far (valid laps only) is shown for 9 s
//   line         a flying car in sector 3 past 80% of the lap is HELD until it has crossed the line, plus 3 s; nothing but a crash cuts it
//   next         then the flying car that is closest to the line (the one behind it), and so on
//   incident     a collision or a retirement, as in the race
// Practice and time trial stay idle.
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

enum class Kind { none, quiet, pit_other, battle, overtake, pit_front, start, last_lap, finish, incident, q_fly, q_line, q_purple };

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
    case Kind::q_fly: return 2;
    case Kind::q_line: return 5;
    case Kind::q_purple: return 6;
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
    case Kind::q_fly: return "flying lap";
    case Kind::q_line: return "finish line";
    case Kind::q_purple: return "purple";
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
        bool quali = type >= 5 && type <= 14;   // qualifying 5-9, sprint shootout 10-14
        if (type != session_type_ || laps != total_laps_ || race != race_ || quali != quali_) {
            char b[200];
            snprintf(b, sizeof b, "session packet: type %d (%s), total laps %d, paused %d", type,
                     race ? "race" : quali ? "qualifying: flying-lap director" : "not a race: director idle", laps, paused ? 1 : 0);
            logs_.push_back(b);
        }
        if (type != session_type_)
            q_reset_locked();
        quali_ = quali;
        track_len_ = float(u16(p + 33));   // track length in metres
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
            c.s1 = u16(e + 8) + 60000u * e[10], c.s2 = u16(e + 11) + 60000u * e[13], c.inv = e[37];
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
    // "broadcast", "top10" or "action" (anything else = broadcast)
    void set_preset(const std::string &m)
    {
        std::lock_guard<std::mutex> g(mu_);
        if (m == preset_)
            return;
        preset_ = m;
        top_n_ = 0, battle_top_ = 11, quiet_n_ = 3;
        hold_ = 20.0, quiet_len_ = 25.0, ovt_len_ = 12.0, ovt_win_ = 14.0;
        bat_start_ = 700.0, bat_keep_ = 1200.0, bat_apart_ = 2.5;
        if (m == "top10") {
            top_n_ = 10, battle_top_ = 10, quiet_n_ = 5;
        } else if (m == "action") {
            battle_top_ = 24, quiet_n_ = 3;
            hold_ = 12.0, quiet_len_ = 15.0, ovt_len_ = 9.0;
            bat_start_ = 1000.0, bat_keep_ = 1400.0, bat_apart_ = 3.0;
        }
        logs_.push_back("preset: " + (m.empty() ? std::string("broadcast") : m));
    }

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
        cur_ = -1, kind_ = Kind::none, since_ = 0, last_req_ = -1, req_t_ = 0, paused_until_ = 0, seen_follow_ = -1;
        pend_ = {};
        std::fill(std::begin(inc_cd_), std::end(inc_cd_), 0.0);
        std::fill(std::begin(pit_cd_), std::end(pit_cd_), 0.0);
        pair_cd_.clear();
        battle_front_ = -1, battle_gap_best_ = 1e9, battle_progress_ = 0, battle_apart_ = 0;
        ll_start_ = 0;
        q_reset_locked();
        lap_logged_ = false;
        events_logged_ = 0;
    }

    // forget the current shot (the caller stopped spectating): the next decision starts fresh
    void drop_shot()
    {
        std::lock_guard<std::mutex> g(mu_);
        cur_ = -1, kind_ = Kind::none, last_req_ = -1, seen_follow_ = -1;
    }

    // somebody else (the caller's own fallback) moved the camera to this car: not the viewer's doing
    void note_follow(int car, double t)
    {
        std::lock_guard<std::mutex> g(mu_);
        cur_ = car, kind_ = Kind::quiet, since_ = t, last_req_ = car, req_t_ = t, seen_follow_ = car;
    }

    std::vector<std::string> take_logs()
    {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<std::string> out;
        out.swap(logs_);
        return out;
    }

    // seconds the director still waits after a manual camera change (0 = not paused)
    double paused_left(double t)
    {
        std::lock_guard<std::mutex> g(mu_);
        return t < paused_until_ ? paused_until_ - t : 0.0;
    }

    // the viewer asked it to carry on (the web panel's Resume)
    void resume()
    {
        std::lock_guard<std::mutex> g(mu_);
        paused_until_ = 0;
        seen_follow_ = -1;
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
        if (!(race_ || quali_) || cars_n_ <= 0 || t - last_data_ > 1.5 || paused_)
            return out;
        // A manual camera change = the followed car changed and it is not the car this director asked for (the game's own keys, the web panel,
        // the leaderboard). It pauses the director for user_pause_ seconds; each further change starts the wait again. A request the game
        // ignored does not count (the followed car did not change).
        if (following >= 0) {
            bool ours = following == last_req_ && t - req_t_ < 8.0;
            if (seen_follow_ >= 0 && following != seen_follow_ && !ours) {
                paused_until_ = t + user_pause_;
                last_req_ = -1, cur_ = -1, kind_ = Kind::none;
                seen_follow_ = following;
                out.user_override = true;
                out.why = "you changed the camera: the director waits " + std::to_string(int(user_pause_)) + " s";
                return out;
            }
            seen_follow_ = following;
        }
        if (t < paused_until_)
            return out;
        if (quali_)
            return decide_quali(t, valid);
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
        // a car a full lap or more behind the leader (never shown in a fight or a pass); a car out of the preset's positions
        auto lapped = [&](int c) {
            return leader >= 0 && c >= 0 && c < cars_n_ && cars_[c].lap < cars_[leader].lap && cars_[leader].last_ms > 0 &&
                   cars_[c].gap_leader >= cars_[leader].last_ms;
        };
        auto inrange = [&](int c) { return c >= 0 && c < cars_n_ && (top_n_ == 0 || (cars_[c].pos >= 1 && cars_[c].pos <= top_n_)); };

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
            case Kind::overtake: ended = age >= ovt_len_; break;
            case Kind::incident: ended = age >= 9.0; break;
            case Kind::start: ended = age >= 10.0; break;      // the rotation of the leaders moves on every 10 s
            case Kind::quiet: ended = age >= quiet_len_; break;
            case Kind::finish: ended = age >= 6.0; break;
            case Kind::last_lap: ended = age >= 20.0 && rem > 25.0; break;
            case Kind::pit_front:
            case Kind::pit_other: ended = (cars_[cur_].pit == 0 && age >= 5.0) || age >= 40.0; break;
            case Kind::battle: {
                const Car &c = cars_[cur_];
                bool close = c.gap_front > 0 && c.gap_front <= bat_keep_;
                if (close)
                    battle_apart_ = 0;
                else if (battle_apart_ == 0)
                    battle_apart_ = t;
                bool stale = age >= 6.0 && t - battle_progress_ >= 25.0;
                ended = (age >= 8.0 && battle_apart_ > 0 && t - battle_apart_ >= bat_apart_) || age >= 45.0 || stale;
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
            if (!in_pits(i) || !valid(i) || pit_cd_[i] > t || !inrange(i))
                continue;
            bool front = cars_[i].pos >= 1 && cars_[i].pos <= 6;
            offer(i, front ? Kind::pit_front : Kind::pit_other, "P" + std::to_string(cars_[i].pos) + " is in the pits");
        }
        // overtakes: a recent pass that was not shown yet
        for (auto &e : ovtk_) {
            if (e.used || t - e.t > ovt_win_) {
                continue;
            }
            if (pair_cd_[pair_key(e.a, e.b)] > t || !racing(e.a) || !inrange(e.a) || lapped(e.a) || lapped(e.b))
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
            if ((!chaos || top_n_ > 0) && cars_[i].pos > battle_top_)
                continue;                                       // the front of the field only
            if (lapped(i) || lapped(front))
                continue;
            double gap = cars_[i].gap_front;
            bool continuing = i == cur_ && kind_ == Kind::battle;
            if (gap <= 0 || gap > (continuing ? bat_keep_ : bat_start_))
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
            int top[8], nt = 0;
            for (int pos = 1; pos <= quiet_n_ && nt < 8; pos++)
                if (int c = by_pos(pos); racing(c))
                    top[nt++] = c;
            if (preset_ == "action") {   // every action: the car closest to the one in front instead of the leaders in turn
                int close = -1;
                for (int i = 0; i < cars_n_; i++)
                    if (racing(i) && i != cur_ && !lapped(i) && cars_[i].pos >= 2 && cars_[i].gap_front > 0 &&
                        (close < 0 || cars_[i].gap_front < cars_[close].gap_front))
                        close = i;
                if (close >= 0)
                    offer(close, Kind::quiet, "quiet moment: the closest gap, P" + std::to_string(cars_[close].pos));
            }
            if (nt > 0 && best.car < 0) {
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
        // The shot is not over (ended is false): a fight or a pass that is still going is HELD. Only something bigger takes the camera, and a
        // newcomer of the same kind never does (except one 5 places nearer the front while the current pair is not about to swap).
        if (!ended && cur_ >= 0) {
            double age = t - since_;
            int cp = prio(kind_), np = prio(best.kind);
            const bool live = kind_ == Kind::battle || kind_ == Kind::overtake;                       // a fight that is still going
            const bool about_to_pass = kind_ == Kind::battle && cars_[cur_].gap_front > 0 && cars_[cur_].gap_front < 500;
            const int cpos = cars_[cur_].pos, bpos = cars_[best.car].pos;
            const bool front_more = bpos > 0 && cpos > 0 && bpos + 3 <= cpos;                        // 3 places nearer the front
            const double hold_live = hold_ + 5.0;
            bool allow = false;
            if (best.car == cur_) {
                allow = true;                                   // same car: only the label changes
            } else if (np > cp) {
                if (best.kind == Kind::incident)
                    allow = kind_ != Kind::incident || age >= 6.0;
                else if (np >= 7)
                    allow = kind_ != Kind::incident || age >= 6.0;
                else if (best.kind == Kind::pit_front)
                    allow = live ? age >= hold_live : age >= std::min(10.0, hold_);
                else if (live)
                    allow = age >= hold_live || (front_more && !about_to_pass && age >= 10.0 && (best.kind == Kind::overtake || best.kind == Kind::battle));
                else
                    allow = age >= hold_ || (front_more && age >= 5.0 && cp <= 4);
            } else if (np == cp && np >= 3 && np <= 4) {
                allow = bpos > 0 && cpos > 0 && bpos + 5 <= cpos && !about_to_pass && age >= 12.0;
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
                if (e.a == best.car && !e.used && t - e.t <= ovt_win_) {
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
    // ---- qualifying ----------------------------------------------------------------------------------------------------------------
    struct QCar {
        bool fly = false, fly_lap = false, inv = false;
        double fly_since = 0, cross_t = 0;
        uint8_t lap = 0;
        uint32_t s1 = 0, s2 = 0, stale1 = 0, stale2 = 0;
    };
    struct QEv {
        double t;
        int car, kind;   // 1..3 = fastest sector so far, 4 = fastest lap so far
        bool used;
    };

    void q_reset_locked()
    {
        for (QCar &q : qc_)
            q = QCar{};
        qev_.clear();
        qbest_s1_ = qbest_s2_ = qbest_s3_ = qbest_lap_ = 0;
        cur_ = -1, kind_ = Kind::none, last_req_ = -1, pend_ = {};
    }

    void q_event(double t, int car, int kind)
    {
        qev_.push_back({t, car, kind, false});
        if (qev_.size() > 40)
            qev_.erase(qev_.begin());
    }

    // a time against the best so far: the first one is only the baseline (no event), a better one is an event
    void q_best(double t, int car, int kind, uint32_t ms, uint32_t &best)
    {
        if (ms == 0)
            return;
        if (best != 0 && ms < best)
            q_event(t, car, kind);
        if (best == 0 || ms < best)
            best = ms;
    }

    // flying laps, sector times and the lap boundary of every car, from the lap data
    void quali_update(double t)
    {
        for (int i = 0; i < cars_n_; i++) {
            Car &c = cars_[i];
            QCar &q = qc_[i];
            if (!c.seen)
                continue;
            bool flying = c.result == 2 && c.status == 1 && c.pit == 0;   // driver status 1 = on a flying lap
            if (flying && !q.fly)
                q.fly_since = t;
            q.fly = flying;
            if (flying)
                q.fly_lap = true;
            if (c.inv)
                q.inv = true;
            if (q.lap != 0 && c.lap != q.lap) {   // the car crossed the line
                if (q.fly_lap && !q.inv && c.last_ms > 0) {
                    uint32_t s3 = q.s1 > 0 && q.s2 > 0 && c.last_ms > q.s1 + q.s2 ? c.last_ms - q.s1 - q.s2 : 0;
                    q_best(t, i, 3, s3, qbest_s3_);
                    q_best(t, i, 4, c.last_ms, qbest_lap_);
                }
                q.cross_t = t;
                q.stale1 = c.s1, q.stale2 = c.s2;   // the packet may still show the last lap's sector times for a moment
                q.s1 = q.s2 = 0;
                q.fly_lap = flying;
                q.inv = c.inv != 0;
            }
            q.lap = c.lap;
            if (q.s1 == 0 && c.s1 > 0 && c.s1 != q.stale1) {
                q.s1 = c.s1;
                if (flying && !q.inv)
                    q_best(t, i, 1, c.s1, qbest_s1_);
            }
            if (q.s2 == 0 && c.s2 > 0 && c.s2 != q.stale2) {
                q.s2 = c.s2;
                if (flying && !q.inv)
                    q_best(t, i, 2, c.s2, qbest_s2_);
            }
        }
        for (auto &e : qev_)
            if (!e.used && t - e.t > 9.0)
                e.used = true;
    }

    template <class Valid> Pick decide_quali(double t, Valid valid)
    {
        Pick out;
        auto ok = [&](int c) { return c >= 0 && c < cars_n_ && cars_[c].seen && cars_[c].result == 2 && valid(c); };
        auto flying = [&](int c) { return ok(c) && cars_[c].status == 1 && cars_[c].pit == 0; };
        auto frac = [&](int c) { return track_len_ > 0 ? std::max(0.0f, std::min(1.0f, cars_[c].dist / track_len_)) : 0.0f; };
        auto near_line = [&](int c) { return flying(c) && cars_[c].sector == 2 && frac(c) >= 0.80f; };
        auto by_pos = [&](int pos) {
            for (int i = 0; i < cars_n_; i++)
                if (cars_[i].seen && cars_[i].pos == pos)
                    return i;
            return -1;
        };
        auto rank = [&](int c) { return cars_[c].pos > 0 ? int(cars_[c].pos) : 99; };

        quali_update(t);
        track(t, ok, by_pos);   // collisions and retirements (the "stopped on the track" part needs a race start, so it stays off)

        // ---- is the current shot over? ----
        bool ended = cur_ < 0;
        if (!ended) {
            double age = t - since_;
            bool cur_ok = cur_ < cars_n_ && cars_[cur_].seen && cars_[cur_].result == 2 && (valid(cur_) || kind_ == Kind::incident);
            switch (kind_) {
            case Kind::incident: ended = age >= 9.0; break;
            case Kind::q_purple: ended = age >= 9.0; break;
            case Kind::q_line: {   // until the car has crossed the line, then 3 s
                bool crossed = qc_[cur_].cross_t > since_;
                ended = (crossed && t - qc_[cur_].cross_t >= 3.0) || age >= 45.0 || (!crossed && !flying(cur_) && age >= 6.0);
                break;
            }
            case Kind::q_fly: ended = age >= 25.0 || (!flying(cur_) && age >= 3.0); break;
            default: ended = true;
            }
            if (!cur_ok)
                ended = true;
        }

        // ---- candidates ----
        Cand best;
        auto offer = [&](int car, Kind k, std::string why) {
            if (car < 0)
                return;
            if (prio(k) > prio(best.kind))
                best = {car, k, std::move(why)};
        };
        if (pend_.car >= 0 && t >= pend_.due) {
            if (inc_cd_[pend_.car] > t)
                pend_ = {};
            else
                offer(pend_.car, Kind::incident, pend_.why);
        }
        for (auto &e : qev_)
            if (!e.used && ok(e.car))
                offer(e.car, Kind::q_purple, e.kind == 4 ? "purple: the fastest lap so far" : "purple sector " + std::to_string(e.kind));
        int line = -1;
        for (int i = 0; i < cars_n_; i++)
            if (near_line(i) && (line < 0 || frac(i) > frac(line)))
                line = i;
        offer(line, Kind::q_line, "flying lap: about to cross the line");
        int started = -1, nextc = -1;
        for (int i = 0; i < cars_n_; i++) {
            if (!flying(i))
                continue;
            if (t - qc_[i].fly_since < 8.0 && (started < 0 || rank(i) < rank(started)))
                started = i;
            if (nextc < 0 || frac(i) > frac(nextc))
                nextc = i;
        }
        if (started >= 0)
            offer(started, Kind::q_fly, "starts a flying lap (P" + std::to_string(rank(started)) + ")");
        else if (nextc >= 0)
            offer(nextc, Kind::q_fly, "the next car on a flying lap");
        if (best.car < 0)
            return out;

        // ---- may it cut away? a car near the line is held until it has crossed ----
        if (!ended && cur_ >= 0) {
            double age = t - since_;
            int cp = prio(kind_), np = prio(best.kind);
            bool allow = false;
            if (best.car == cur_)
                allow = true;
            else if (best.kind == Kind::incident)
                allow = kind_ != Kind::incident || age >= 6.0;
            else if (kind_ == Kind::q_line)
                allow = false;
            else if (np > cp)
                allow = age >= (best.kind == Kind::q_line ? 3.0 : 6.0);
            if (!allow)
                return out;
        }
        if (best.car == cur_ && !ended) {
            kind_ = best.kind;
            return out;
        }
        if (best.kind == Kind::incident) {
            inc_cd_[best.car] = t + 50.0;
            pend_ = {};
        } else if (best.kind == Kind::q_purple) {
            for (auto &e : qev_)
                if (e.car == best.car)
                    e.used = true;
        }
        cur_ = best.car, kind_ = best.kind, since_ = t;
        last_req_ = best.car, req_t_ = t;
        out.car = best.car, out.kind = best.kind;
        out.why = best.why;
        return out;
    }

    struct Car {
        bool seen = false, kmh_known = false;
        uint8_t pos = 0, lap = 0, pit = 0, result = 0, sector = 0, status = 0, inv = 0;
        uint32_t s1 = 0, s2 = 0;
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
            if (!e.used && t - e.t > ovt_win_)
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
    int cur_ = -1, last_req_ = -1, seen_follow_ = -1;   // seen_follow_ = the car followed at the last decision (to spot manual changes)
    Kind kind_ = Kind::none;
    double since_ = 0, req_t_ = 0, paused_until_ = 0;
    Pending pend_;
    double inc_cd_[MAXC] = {}, pit_cd_[MAXC] = {};
    std::map<int, double> pair_cd_;
    int battle_front_ = -1;
    double battle_gap_best_ = 1e9, battle_progress_ = 0, battle_apart_ = 0, ll_start_ = 0;
    std::string preset_ = "";
    int top_n_ = 0, battle_top_ = 11, quiet_n_ = 3;   // top_n_ 0 = every position
    double user_pause_ = 60.0;
    double hold_ = 20.0, quiet_len_ = 25.0, ovt_len_ = 10.0, ovt_win_ = 14.0, bat_start_ = 700.0, bat_keep_ = 1200.0, bat_apart_ = 2.5;
    bool quali_ = false;
    float track_len_ = 0;
    QCar qc_[MAXC];
    std::vector<QEv> qev_;
    uint32_t qbest_s1_ = 0, qbest_s2_ = 0, qbest_s3_ = 0, qbest_lap_ = 0;
    std::vector<std::string> logs_;
    bool lap_logged_ = false;
    int events_logged_ = 0;
};

}  // namespace tvd
