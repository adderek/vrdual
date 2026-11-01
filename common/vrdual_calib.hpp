// vrdual_calib.hpp — guided IPD calibration (pure, no I/O): bisection driven
// by "the post looks nearer / farther than it really is / right".
//
// Perceived distance ~ real distance * true IPD / rendered IPD, so a post that
// looks nearer (a small nearby toy) means the rendered IPD is too large.
// Phase far: the 20 m post, bisects `ipd` over [50, 80] mm. Phase near: the
// 0.5 m post, bisects `ipd_near` over far IPD +-8 mm, starting at the far IPD;
// "right" at that first probe keeps near/far IPD off (ipd_near = 0).
// Each answer halves the range; a phase ends at 0.5 mm or below (at most 6
// answers per phase) or on "right".
#pragma once

#include <cstdio>
#include <string>

namespace vrdual {

struct GuidedCalib {
    enum Phase { Off, Far, Near, Done };
    enum Answer { Nearer = -1, Right = 0, Farther = 1 };
    static constexpr float kFarLo = 0.050f, kFarHi = 0.080f, kNearSpan = 0.008f, kStop = 0.0005f;

    Phase phase = Off;
    float lo = 0, hi = 0;
    float ipd = 0, ipd_near = 0;  // results (ipd_near 0 = off)
    int answers = 0, near_from = 0;  // answers so far, count when phase near began

    void start() { phase = Far, lo = kFarLo, hi = kFarHi, ipd = ipd_near = 0, answers = near_from = 0; }
    bool active() const { return phase == Far || phase == Near; }
    float probe() const { return 0.5f * (lo + hi); }
    // Live values to show now.
    float show_ipd() const { return phase == Far ? probe() : ipd; }
    float show_ipd_near() const { return phase == Near ? probe() : ipd_near; }

    void answer(Answer a) {
        if (!active()) return;
        bool first_near = phase == Near && answers == near_from;
        ++answers;
        if (a == Nearer) hi = probe();
        else if (a == Farther) lo = probe();
        if (a != Right && hi - lo > kStop) return;
        float r = a == Right ? probe() : 0.5f * (lo + hi);
        if (phase == Far) {
            ipd = r;
            phase = Near, lo = ipd - kNearSpan, hi = ipd + kNearSpan, near_from = answers;
        } else {
            ipd_near = a == Right && first_near ? 0 : r;
            phase = Done;
        }
    }

    std::string prompt() const {
        char b[160];
        if (phase == Far)
            snprintf(b, sizeof b, "GUIDED %d: 20 m post at %.1f mm. Left: looks near/small  Right: far/huge  Enter: right", answers + 1, probe() * 1e3);
        else if (phase == Near)
            snprintf(b, sizeof b, "GUIDED %d: 0.5 m post at %.1f mm. Left: looks near  Right: far  Enter: right", answers + 1, probe() * 1e3);
        else if (phase == Done && ipd_near > 0)
            snprintf(b, sizeof b, "GUIDED done: ipd %.1f mm, near %.1f mm", ipd * 1e3, ipd_near * 1e3);
        else if (phase == Done)
            snprintf(b, sizeof b, "GUIDED done: ipd %.1f mm, near/far IPD off", ipd * 1e3);
        else return "";
        return b;
    }
};

}  // namespace vrdual
