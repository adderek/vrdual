// vrdual_hunt.hpp — shader hunt by bisection (docs/tasks/shader-rules.md).
// One implementation, two front ends: `vrdual-ctl hunt find` (answers on
// stdin, `_ hunt find`) and vrdual-xr's input = hunt (controllers, overlay;
// HuntUi). Pure: the front ends write range() into the control block.
//
// Keys: the high 32 bits of the shader hashes the layers saw (the session's
// shader list), unique, sorted. Each step applies MODE (1 hide | 2 noshift |
// 3 hud) to half of the candidates [lo, mid] and asks whether the one thing
// the user watches changed: yes keeps that half, no the other one. "Can't
// tell" (picture too broken) applies the other half instead and asks again.
// One candidate left: confirm it alone, then the front end offers the rule.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "vrdual_config.hpp"
#include "vrdual_input.hpp"

namespace vrdual {

static const char* const kHuntModes[4] = {"off", "hide", "noshift", "hud"};

// The session's shader list (16 hex digits per line, as the layers write it).
inline std::vector<std::string> shader_list(const std::string& path) {
    std::set<std::string> keys;
    std::ifstream f(path);
    for (std::string l; std::getline(f, l);)
        if (l.size() == 16 && l.find_first_not_of("0123456789abcdef") == std::string::npos) keys.insert(l);
    return {keys.begin(), keys.end()};
}

// Appends KEY:MODE to shader_rules of [app:APP]; the new value, "" on error.
inline std::string add_shader_rule(const std::string& app, const std::string& key, const std::string& mode) {
    std::string sec = "app:" + app, cur;
    Ini ini = parse_ini(config_path());
    if (ini.count(lower(sec)) && ini[lower(sec)].count("shader_rules")) cur = ini[lower(sec)]["shader_rules"];
    std::string val = cur.empty() ? key + ":" + mode : cur + ", " + key + ":" + mode;
    return save_key(config_path(), sec, "shader_rules", val) ? val : "";
}

struct Hunt {
    enum Phase { Start, Ask, AskOther, Confirm, Found, NotFound, Quit };
    enum Answer { Yes, No, Unsure, Abort };
    std::vector<std::string> keys;  // 8 hex digits, unique, sorted
    std::vector<std::string> full;  // a full 16-digit key per entry (the rule's key)
    int mode = 1;
    Phase phase = Start;
    int lo = 0, hi = -1, mid = 0, step = 0;
    bool on = true;  // false: the group shown unchanged (the user compares)
    std::string why;  // NotFound / Quit: one line for the user

    void begin(const std::vector<std::string>& list, int m) {
        keys.clear(), full.clear();
        for (auto& k : list)
            if (keys.empty() || keys.back() != k.substr(0, 8)) keys.push_back(k.substr(0, 8)), full.push_back(k);
        mode = m, step = 0, lo = 0, hi = int(keys.size()) - 1, why.clear();
        if (keys.empty()) { phase = NotFound, why = "no shaders recorded for this session (is the game running with the layer?)"; return; }
        next();
    }
    bool asking() const { return phase == Ask || phase == AskOther || phase == Confirm; }
    // Range to apply now: mode 0 = nothing (not asking, or toggled off).
    void range(uint32_t& a, uint32_t& b, uint32_t& m) const {
        a = b = m = 0;
        if (!asking() || !on) return;
        int x = phase == AskOther ? mid + 1 : lo, y = phase == Ask ? mid : hi;
        a = uint32_t(strtoul(keys[x].c_str(), nullptr, 16)), b = uint32_t(strtoul(keys[y].c_str(), nullptr, 16)), m = uint32_t(mode);
    }
    int group() const { return phase == Ask ? mid - lo + 1 : phase == AskOther ? hi - mid : 1; }
    int candidates() const { return hi - lo + 1; }
    const std::string& found() const { return full[lo]; }

    void answer(Answer a) {
        if (!asking()) return;
        on = true;
        if (a == Abort) { phase = Quit, why = "stopped, nothing changed"; return; }
        if (phase == Confirm) {
            if (a == Yes) phase = Found;
            else phase = NotFound, why = "not found: it may take several shaders together (hunt again, or another mode)";
            return;
        }
        if (phase == Ask && a == Unsure) { phase = AskOther; return; }
        if (phase == AskOther && a == Unsure) { phase = Quit, why = "can't tell either way: try another mode (noshift keeps the picture visible)"; return; }
        bool first_half = (phase == Ask) == (a == Yes);  // Ask yes / AskOther no: it is in [lo, mid]
        if (first_half) hi = mid;
        else lo = mid + 1;
        next();
    }

    // "[3] 12 of 47 candidates hidden" style status for both front ends.
    std::string status() const {
        static const char* const what[4] = {"", "hidden", "not shifted", "at hud_depth"};
        char b[128];
        if (phase == Confirm) snprintf(b, sizeof b, "only shader %s %s", full[lo].c_str(), what[mode]);
        else snprintf(b, sizeof b, "[%d] %d of %d candidates %s%s", step, group(), candidates(), what[mode], phase == AskOther ? " (the other half)" : "");
        return b;
    }

private:
    void next() {
        if (lo < hi) mid = (lo + hi) / 2, ++step, phase = Ask;
        else phase = Confirm;
    }
};

// vrdual-xr input = hunt: buttons -> Hunt, panel text. Results for the caller:
enum { HUNT_START = 1, HUNT_SAVE = 2, HUNT_CHANGED = 4 };
struct HuntUi {
    Hunt h;
    int mode = 1;  // picked on the start screen (x cycles)
    bool prev[4] = {};
    std::string note;  // last result line (saved / discarded)

    // a b x y edges, ignored while menu is held (menu + a / b leaves the mode).
    int step(const Controls& c) {
        bool now[4] = {c.a, c.b, c.x, c.y}, e[4];
        for (int i = 0; i < 4; ++i) e[i] = now[i] && !prev[i] && !c.menu, prev[i] = now[i];
        if (h.asking()) {
            if (e[3]) { h.on = !h.on; return HUNT_CHANGED; }
            for (int i = 0; i < 3; ++i)
                if (e[i]) { h.answer(i == 0 ? Hunt::Yes : i == 1 ? Hunt::No : Hunt::Unsure); return HUNT_CHANGED; }
            return 0;
        }
        if (h.phase == Hunt::Found && (e[0] || e[1])) {
            h.phase = Hunt::Start;
            note = e[0] ? "" : "discarded: " + h.found() + ":" + kHuntModes[h.mode];
            return HUNT_CHANGED | (e[0] ? HUNT_SAVE : 0);
        }
        if (h.phase == Hunt::Start) {
            if (e[2]) { mode = mode % 3 + 1; return HUNT_CHANGED; }
            if (e[0]) { note.clear(); return HUNT_START; }  // the caller loads the keys and calls h.begin
            return 0;
        }
        if ((h.phase == Hunt::NotFound || h.phase == Hunt::Quit) && (e[0] || e[1])) {
            note = h.why, h.phase = Hunt::Start;
            return HUNT_CHANGED;
        }
        return 0;
    }

    // Overlay panel (80 columns).
    std::vector<std::string> lines(const std::string& app) const {
        static const char* const what[4] = {"", "not drawn", "not shifted per eye", "at hud_depth"};
        std::vector<std::string> l;
        int m = h.phase == Hunt::Start ? mode : h.mode;
        std::string a = app.size() > 28 ? app.substr(0, 27) + "~" : app;
        l.push_back("shader hunt: " + a + "   mode: " + kHuntModes[m] + " (" + what[m] + ")");
        switch (h.phase) {
        case Hunt::Start:
            l.push_back("Finds the shader that draws one artefact (or HUD element) you pick.");
            l.push_back("Each step changes part of the shaders and asks about YOUR artefact only.");
            l.push_back("");
            l.push_back("a: start   x: mode (hide / noshift / hud)   menu + a / b: leave");
            if (!note.empty()) l.push_back(""), l.push_back(note);
            break;
        case Hunt::Ask:
        case Hunt::AskOther:
        case Hunt::Confirm:
            l.push_back(h.status() + (h.on ? "" : "   [group OFF: original picture]"));
            l.push_back(h.phase == Hunt::Confirm ? "Is exactly the artefact you hunt still gone / changed?"
                                                 : "Did exactly the artefact you hunt disappear / change?");
            l.push_back("(other things may appear or vanish too: judge only your artefact)");
            l.push_back("");
            l.push_back("a: yes   b: no   x: can't tell   y: group on / off (compare)");
            l.push_back("menu + a / b: leave (nothing saved)");
            break;
        case Hunt::Found:
            l.push_back("found: " + h.found() + " (" + kHuntModes[h.mode] + ")");
            l.push_back("a: save the rule for this game (applies at its next start)   b: discard");
            break;
        case Hunt::NotFound:
        case Hunt::Quit:
            l.push_back(h.why);
            l.push_back("a / b: back");
            break;
        }
        return l;
    }
};

}  // namespace vrdual
