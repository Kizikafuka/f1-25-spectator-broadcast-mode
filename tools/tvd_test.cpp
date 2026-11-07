// Offline test of the TV director: feeds it made-up UDP packets of a race and prints what it would show.
// zig c++ -std=c++23 -I ../native/src -o tvd_test.exe tvd_test.cpp   (then run it)
#include <cstdio>
#include <cstring>
#include <vector>

#include "tv_director.hpp"

using tvd::Director;

struct Row {
    int pos, lap, pit = 0, result = 2;
    unsigned gap = 5000;
    int kmh = 250;
    unsigned gl = 0;   // gap to the leader, ms
    int drv = 0, sector = 0;   // driver status (1 = flying lap), sector 0..2
    float dist = 0;
    unsigned s1 = 0, s2 = 0, last = 0;   // sector times and last lap time, ms (0 = use the default)
};

static std::vector<uint8_t> session_pkt(int type, int laps, int paused = 0)
{
    std::vector<uint8_t> p(200, 0);
    p[0] = 2026 & 255, p[1] = 2026 >> 8, p[6] = 1;
    p[32] = uint8_t(laps), p[35] = uint8_t(type), p[43] = uint8_t(paused);
    p[33] = 5000 & 255, p[34] = 5000 >> 8;   // track length 5000 m
    return p;
}

static std::vector<uint8_t> lap_pkt(const std::vector<Row> &rows, unsigned cur_ms = 30000, unsigned last_ms = 90000)
{
    std::vector<uint8_t> p(29 + 57 * 24 + 2, 0);
    p[0] = 2026 & 255, p[1] = 2026 >> 8, p[6] = 2;
    for (size_t i = 0; i < rows.size(); i++) {
        uint8_t *e = &p[29 + 57 * i];
        memcpy(e, &last_ms, 4);
        memcpy(e + 4, &cur_ms, 4);
        e[14] = rows[i].gap & 255, e[15] = (rows[i].gap >> 8) & 255;
        e[17] = (rows[i].gl % 60000) & 255, e[18] = ((rows[i].gl % 60000) >> 8) & 255, e[19] = uint8_t(rows[i].gl / 60000);
        e[32] = uint8_t(rows[i].pos), e[33] = uint8_t(rows[i].lap), e[34] = uint8_t(rows[i].pit), e[45] = uint8_t(rows[i].result);
        e[44] = uint8_t(rows[i].drv), e[36] = uint8_t(rows[i].sector);
        memcpy(e + 20, &rows[i].dist, 4);
        e[8] = rows[i].s1 % 60000 & 255, e[9] = (rows[i].s1 % 60000) >> 8, e[10] = uint8_t(rows[i].s1 / 60000);
        e[11] = rows[i].s2 % 60000 & 255, e[12] = (rows[i].s2 % 60000) >> 8, e[13] = uint8_t(rows[i].s2 / 60000);
        if (rows[i].last)
            memcpy(e, &rows[i].last, 4);
    }
    return p;
}

static std::vector<uint8_t> tel_pkt(const std::vector<Row> &rows)
{
    std::vector<uint8_t> p(29 + 59 * 24 + 3, 0);
    p[0] = 2026 & 255, p[1] = 2026 >> 8, p[6] = 6;
    for (size_t i = 0; i < rows.size(); i++) {
        uint16_t k = uint16_t(rows[i].kmh);
        memcpy(&p[29 + 59 * i], &k, 2);
    }
    return p;
}

static std::vector<uint8_t> event_pkt(const char *code, int a, int b = 255)
{
    std::vector<uint8_t> p(60, 0);
    p[0] = 2026 & 255, p[1] = 2026 >> 8, p[6] = 3;
    memcpy(&p[29], code, 4);
    p[33] = uint8_t(a), p[34] = uint8_t(b);
    return p;
}

static int g_fail = 0;
#define CHECK(c, msg)                                                         \
    do {                                                                      \
        if (!(c)) {                                                           \
            printf("  FAIL: %s\n", msg);                                      \
            g_fail++;                                                         \
        } else                                                                \
            printf("  ok: %s\n", msg);                                        \
    } while (0)

struct Sim {
    Director d;
    double t = 0;
    int following = -1;
    std::vector<Row> rows;
    bool valid_all = true;
    int stype = 15;   // session type of the session packet (15 = race, 5..7 = qualifying)
    std::vector<std::pair<double, tvd::Pick>> picks;
    void feed_all()
    {
        auto lp = lap_pkt(rows);
        d.feed_lap(lp.data(), int(lp.size()), t);
        auto tp = tel_pkt(rows);
        d.feed_telemetry(tp.data(), int(tp.size()), t);
    }
    void step(double dt, bool obey = true)
    {
        t += dt;
        feed_all();
        auto s = session_pkt(stype, 5);
        d.feed_session(s.data(), int(s.size()), t);
        auto pk = d.decide(t, following, [&](int c) { return c != 21 && valid_all; });
        if (pk.car >= 0) {
            picks.push_back({t, pk});
            printf("  t=%6.1f -> car %d  [%s] %s\n", t, pk.car, tvd::kind_name(pk.kind), pk.why.c_str());
            if (obey)
                following = pk.car;
        } else if (pk.user_override)
            printf("  t=%6.1f    %s\n", t, pk.why.c_str());
    }
    void run(double secs)
    {
        for (double e = t + secs; t < e;)
            step(0.2);
    }
    void ev(const char *code, int a, int b = 255)
    {
        auto p = event_pkt(code, a, b);
        d.feed_event(p.data(), int(p.size()), t);
    }
};

static std::vector<Row> grid()
{
    std::vector<Row> r;
    for (int i = 0; i < 20; i++)
        r.push_back({i + 1, 1, 0, 2, unsigned(3000 + i * 100), 250});   // car i is in position i+1
    r[0].gap = 0;
    return r;
}

int main()
{
    printf("1. before the lights: nothing\n");
    {
        Sim s;
        s.rows = grid();
        s.run(5);
        CHECK(s.picks.empty(), "no pick before the race starts");
    }
    printf("2. the start: the leaders in turn for 30 s, then it carries on\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(34);
        CHECK(s.picks.size() >= 3, "at least 3 shots in 34 s");
        CHECK(s.picks.size() >= 1 && s.picks[0].second.car == 0 && s.picks[0].second.kind == tvd::Kind::start, "first shot: the leader");
    }
    printf("3. a battle (car 7 within 400 ms of car 6) after the start phase\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(35);
        s.rows[6].gap = 400;
        s.run(30);
        bool saw = false;
        for (auto &p : s.picks)
            if (p.second.kind == tvd::Kind::battle && p.second.car == 6)
                saw = true;
        CHECK(saw, "it shows the car in the battle (car 6 is P7)");
    }
    printf("4. a collision: cut after about 1.8 s, even in the middle of a shot\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(45);
        double t0 = s.t;
        s.ev("COLL", 9, 10);
        s.run(5);
        const tvd::Pick *inc = nullptr;
        double when = 0;
        for (auto &p : s.picks)
            if (p.second.kind == tvd::Kind::incident) {
                inc = &p.second;
                when = p.first;
            }
        CHECK(inc && inc->car >= 9 && inc->car <= 10, "the incident shows car 9 or 10");
        CHECK(inc && when - t0 >= 1.6 && when - t0 <= 2.4, "after the reaction delay of about 1.8 s");
        s.run(10);
        s.ev("COLL", 9, 10);
        s.run(5);
        int n = 0;
        for (auto &p : s.picks)
            if (p.second.kind == tvd::Kind::incident)
                n++;
        CHECK(n == 1, "the same car's incident is not shown again within 50 s");
    }
    printf("5. an overtake: lost in the middle of a shot, shown when the hold ends soon after it\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(40);
        auto wait_new_shot = [&]() {
            size_t n = s.picks.size();
            while (s.picks.size() == n && s.t < 400)
                s.step(0.2);
        };
        wait_new_shot();
        double since = s.picks.back().first;
        while (s.t < since + 2.0)
            s.step(0.2);
        size_t n0 = s.picks.size();
        s.ev("OVTK", 12, 11);                  // 2 s into a shot: the 20 s hold is over only after the event has expired
        while (s.t < since + 11.0)
            s.step(0.2);
        bool early = false;
        for (size_t i = n0; i < s.picks.size(); i++)
            if (s.picks[i].second.kind == tvd::Kind::overtake)
                early = true;
        CHECK(!early, "an overtake early in a shot does not cut away");
        wait_new_shot();
        since = s.picks.back().first;
        while (s.t < since + 12.0)
            s.step(0.2);
        n0 = s.picks.size();
        s.ev("OVTK", 15, 14);                  // 12 s into a shot: the hold ends 8 s later, the event is still valid
        while (s.t < since + 22.0)
            s.step(0.2);
        bool seen2 = false;
        for (size_t i = n0; i < s.picks.size(); i++)
            if (s.picks[i].second.kind == tvd::Kind::overtake && s.picks[i].second.car == 15)
                seen2 = true;
        CHECK(seen2, "an overtake that fits the end of the hold is shown");
    }
    printf("6. you change the camera: the director waits 60 s\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(10);
        size_t n0 = s.picks.size();
        s.following = 15;     // you click another driver
        s.run(1);
        s.run(55);
        CHECK(s.picks.size() == n0, "no pick for the next 56 s");
        s.following = 7;      // another manual change: the 60 s start again
        s.run(30);
        CHECK(s.picks.size() == n0, "a second manual change restarts the wait");
        s.run(40);
        CHECK(s.picks.size() > n0, "it carries on after the wait");
    }
    printf("7. a leader pit stop cuts in; a back marker's stop does not interrupt\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(60);
        s.rows[2].pit = 1;
        s.run(14);
        bool pit = false;
        for (auto &p : s.picks)
            if (p.second.kind == tvd::Kind::pit_front && p.second.car == 2)
                pit = true;
        CHECK(pit, "the stop of P3 is shown");
    }
    printf("8. last lap and the finish\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(60);
        for (auto &r : s.rows)
            r.lap = 5;
        s.picks.clear();
        s.run(3);
        CHECK(!s.picks.empty() && s.picks.back().second.kind == tvd::Kind::last_lap && s.picks.back().second.car == 0, "last lap: the leader");
        size_t n1 = s.picks.size();
        s.ev("CHQF", 0);
        s.run(5);
        CHECK(s.picks.size() == n1, "chequered flag: the camera stays on the winner for 6 s");
        s.rows[0].result = 3;
        s.run(10);
        bool shown_finished = false;
        for (auto &p : s.picks)
            if (p.first > s.t - 9 && p.second.car == 0)
                shown_finished = true;
        CHECK(!shown_finished, "a finished car is not shown again");
    }
    printf("9. a retired car is never shown (valid() says no)\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.rows[0].result = 4;
        s.run(60);
        bool bad = false;
        for (auto &p : s.picks)
            if (p.second.car == 0 && p.second.kind != tvd::Kind::incident)
                bad = true;
        CHECK(!bad, "car 0 (retired) is not shown as a normal shot");
    }
    printf("11. a car stopped on track becomes an incident\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(40);
        s.rows[14].kmh = 0;
        s.run(14);
        bool saw = false;
        for (auto &p : s.picks)
            if (p.second.kind == tvd::Kind::incident && p.second.car == 14)
                saw = true;
        CHECK(saw, "the stopped car 14 is shown");
    }
    printf("10. paused game: no picks\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(2);
        size_t n0 = s.picks.size();
        for (int i = 0; i < 20; i++) {
            s.t += 0.2;
            auto sp = session_pkt(15, 5, 1);
            s.d.feed_session(sp.data(), int(sp.size()), s.t);
            auto pk = s.d.decide(s.t, s.following, [](int) { return true; });
            if (pk.car >= 0)
                s.picks.push_back({s.t, pk});
        }
        CHECK(s.picks.size() == n0, "nothing while paused");
    }
    printf("9. a fight at P10 is on the screen, then a pass in the top 3 happens: the top is shown within 8 s\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(35);
        s.rows[9].gap = 650;   // car 9 (P10) closes on P9 (not about to pass: 0.65 s)
        s.run(25);
        bool on_fight = s.following == 9;
        CHECK(on_fight, "the camera is on the P10 fight");
        s.run(3);   // the shot has been up for a while but not 20 s
        std::swap(s.rows[1].pos, s.rows[2].pos);   // car 2 now P2
        double t0 = s.t;
        s.ev("OVTK", 2, 1);
        s.run(8);
        bool shown = false;
        for (auto &p : s.picks)
            if (p.second.kind == tvd::Kind::overtake && p.second.car == 2 && p.first - t0 <= 8.0)
                shown = true;
        CHECK(shown, "the pass at P2 is shown within 8 s although the shot of P10 was younger than 20 s");
    }
    printf("10. top 10 preset: a pass and a pit stop at the back are ignored, a crash is shown\n");
    {
        Sim s;
        s.d.set_preset("top10");
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(40);
        size_t n0 = s.picks.size();
        s.ev("OVTK", 15, 14);
        s.rows[16].pit = 1;
        s.run(30);
        bool bad = false;
        for (size_t i = n0; i < s.picks.size(); i++)
            if (s.picks[i].second.kind == tvd::Kind::overtake || s.picks[i].second.kind == tvd::Kind::pit_other || s.picks[i].second.kind == tvd::Kind::pit_front)
                bad = true;
        CHECK(!bad, "no overtake or pit stop shots outside the top 10");
        s.rows[16].pit = 0;
        s.ev("COLL", 17, 18);
        s.run(5);
        bool inc = false;
        for (size_t i = n0; i < s.picks.size(); i++)
            if (s.picks[i].second.kind == tvd::Kind::incident)
                inc = true;
        CHECK(inc, "a collision at the back is still shown");
    }
    printf("11. a battle that opens up to 1.3 s ends within 3 s\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(35);
        s.rows[6].gap = 400;
        s.run(25);
        bool on = s.following == 6;
        s.rows[6].gap = 1300;
        double t0 = s.t;
        s.run(4);
        bool moved = false;
        for (auto &p : s.picks)
            if (p.first > t0 && p.second.car != 6)
                moved = true;
        CHECK(on && moved, "the camera leaves the car once it is 1.3 s behind for more than 2.5 s");
    }
    printf("12. every action: lapped cars are never in a battle\n");
    {
        Sim s;
        s.d.set_preset("action");
        s.rows = grid();
        for (auto &r : s.rows)
            r.lap = 3;
        s.rows[15].lap = 1, s.rows[15].gl = 200000;   // car 15 is two laps down
        s.run(1);
        s.ev("LGOT", 0);
        s.run(35);
        s.rows[15].gap = 300;
        s.run(30);
        bool lapped_shown = false;
        for (auto &p : s.picks)
            if (p.second.kind == tvd::Kind::battle && p.second.car == 15)
                lapped_shown = true;
        CHECK(!lapped_shown, "a lapped car in a close fight is not shown");
    }
    printf("13. a fight on screen is held: another fight at P4 does not cut a fight at P7\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(35);
        s.rows[6].gap = 400;   // car 6 (P7) fights P6
        while (s.following != 6 && s.t < 120)
            s.step(0.2);
        bool on = s.following == 6;
        size_t n0 = s.picks.size();
        s.rows[3].gap = 300;   // car 3 (P4) closes on P3 as well
        s.run(15);
        bool cut = false;
        for (size_t i = n0; i < s.picks.size(); i++)
            if (s.picks[i].second.car == 3)
                cut = true;
        CHECK(on, "the camera is on the P7 fight");
        CHECK(!cut, "the new fight at P4 does not cut it while it lasts");
    }
    printf("14. a fight that is about to end in a pass (under 0.5 s) is held against a top-3 pass for 25 s\n");
    {
        Sim s;
        s.rows = grid();
        s.run(1);
        s.ev("LGOT", 0);
        s.run(35);
        s.rows[9].gap = 400;   // car 9 (P10) is 0.4 s behind P9
        while (s.following != 9 && s.t < 120)
            s.step(0.2);
        double since = s.t;
        std::swap(s.rows[1].pos, s.rows[2].pos);
        s.ev("OVTK", 2, 1);
        s.run(11);
        bool early = false;
        for (auto &p : s.picks)
            if (p.second.kind == tvd::Kind::overtake && p.second.car == 2 && p.first < since + 12.0)
                early = true;
        CHECK(!early, "the pass at the front waits while the P10 fight is about to be decided");
    }
    printf("Q1. qualifying: nothing while everybody is on out laps, then the car that starts a flying lap\n");
    {
        Sim s;
        s.stype = 6;
        s.rows = grid();
        for (auto &r : s.rows)
            r.drv = 3;   // out lap
        s.run(6);
        CHECK(s.picks.empty(), "idle while nobody is on a flying lap");
        s.rows[4].drv = 1;
        s.rows[4].dist = 400;
        s.run(2);
        CHECK(!s.picks.empty() && s.picks[0].second.car == 4 && s.picks[0].second.kind == tvd::Kind::q_fly, "the car that starts a flying lap is shown");
    }
    printf("Q2. qualifying: a purple sector takes the camera from a flying car, after 6 s\n");
    {
        Sim s;
        s.stype = 6;
        s.rows = grid();
        for (auto &r : s.rows)
            r.drv = 3;
        s.rows[4].drv = 1, s.rows[4].dist = 400;
        s.run(2);
        s.rows[4].s1 = 26000, s.rows[4].dist = 1800;      // car 4 sets the baseline sector 1
        s.rows[7].drv = 1, s.rows[7].dist = 300;
        s.run(8);
        size_t n0 = s.picks.size();
        s.rows[7].s1 = 25400, s.rows[7].dist = 1700;      // car 7 is faster: purple sector 1
        s.run(1);
        bool early = s.picks.size() > n0;                 // car 4 has been on screen for > 6 s here, so this may already cut
        s.run(8);
        bool purple = false;
        for (auto &p : s.picks)
            if (p.second.kind == tvd::Kind::q_purple && p.second.car == 7)
                purple = true;
        (void)early;
        CHECK(purple, "the car with the purple sector is shown");
    }
    printf("Q3. qualifying: a car near the line is held until it has crossed, then the next flying car\n");
    {
        Sim s;
        s.stype = 6;
        s.rows = grid();
        for (auto &r : s.rows)
            r.drv = 3, r.lap = 2;
        s.rows[4].drv = 1, s.rows[4].dist = 600;
        s.rows[2].drv = 1, s.rows[2].dist = 2500, s.rows[2].sector = 1;
        s.run(2);
        s.rows[9].drv = 1, s.rows[9].dist = 4300, s.rows[9].sector = 2;   // car 9: sector 3, 86% of the lap
        s.run(5);
        bool line = false;
        for (auto &p : s.picks)
            if (p.second.kind == tvd::Kind::q_line && p.second.car == 9)
                line = true;
        CHECK(line, "the car near the line is shown");
        size_t n0 = s.picks.size();
        s.rows[7].drv = 1, s.rows[7].dist = 300;           // another car starts a flying lap: must not cut the held car
        s.rows[7].s1 = 20000;                               // (and a baseline sector: no event)
        s.run(8);
        bool cut = false;
        for (size_t i = n0; i < s.picks.size(); i++)
            if (s.picks[i].second.car != 9)
                cut = true;
        CHECK(!cut, "nothing cuts the car near the line while it has not crossed");
        s.rows[9].lap = 3, s.rows[9].drv = 2, s.rows[9].dist = 20, s.rows[9].sector = 0, s.rows[9].last = 88000;   // crossed
        s.run(6);
        bool next = false;
        for (size_t i = n0; i < s.picks.size(); i++)
            if (s.picks[i].second.car == 2 && s.picks[i].second.kind == tvd::Kind::q_fly)
                next = true;
        CHECK(next, "after the finish, the flying car closest to the line is next");
    }
    printf("Q4. practice stays idle, and a manual change pauses the qualifying director too\n");
    {
        Sim s;
        s.stype = 2;
        s.rows = grid();
        s.rows[4].drv = 1, s.rows[4].dist = 400;
        s.run(8);
        CHECK(s.picks.empty(), "practice: no picks");
        Sim q;
        q.stype = 7;
        q.rows = grid();
        for (auto &r : q.rows)
            r.drv = 3;
        q.rows[4].drv = 1, q.rows[4].dist = 400;
        q.run(2);
        size_t n0 = q.picks.size();
        q.following = 15;
        q.rows[11].drv = 1, q.rows[11].dist = 4400, q.rows[11].sector = 2;
        q.run(30);
        CHECK(q.picks.size() == n0, "qualifying: no pick for 30 s after a manual change");
    }
    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL OK", g_fail);
    return g_fail ? 1 : 0;
}
