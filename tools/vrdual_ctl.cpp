// vrdual-ctl — configuration and live control (machine contract: no prompts,
// stdout is data, prose on stderr, exit code is the answer).
//
//   vrdual-ctl path                       config file location
//   vrdual-ctl init                       write a commented default config if none exists
//   vrdual-ctl show [APP]                 effective settings for APP, with their source
//   vrdual-ctl profiles [DIR]             game profiles: file, exe list, steam id, keys;
//                                         exit 1 if one has no exe or an unknown key
//   vrdual-ctl live [--session S]         current live values of a running session
//   vrdual-ctl set KEY VALUE [--session S]   change a live value (ipd accepts "64.5mm")
//   vrdual-ctl save [--global] [--session S] write live values into the APP profile (or [global])
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "../common/vrdual_version.h"
#include "../common/vrdual_ctl.hpp"
#include "../common/vrdual_input.hpp"
#include "../common/vrdual_hunt.hpp"

using namespace vrdual;

static float parse_len(const std::string& v) {  // meters, or "64.5mm"
    float f = strtof(v.c_str(), nullptr);
    return v.size() > 2 && v.compare(v.size() - 2, 2, "mm") == 0 ? f / 1000.0f : f;
}

static int usage() {
    fprintf(stderr, "usage: vrdual-ctl version | path | init | show [APP] | profiles [DIR] | map [APP] | map-set APP|--global gamepad|mouse|both BUTTON CODE|none|default\n"
                    "                  | live | set KEY VALUE | save [--global]   [--session S]\n"
                    "                  | shaders | hunt [off | hide|noshift|hud LO HI | find [MODE]]   (LO, HI: 8 hex digits, inclusive)\n"
                    "                  | rule KEY noshift|hud|hide   (adds to shader_rules in the session's [app:NAME])\n");
    return 2;
}

int main(int argc, char** argv) {
    std::vector<std::string> args;
    std::string session = getenv("VRDUAL_SESSION") ? getenv("VRDUAL_SESSION") : "default";
    bool global = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--session" && i + 1 < argc) session = argv[++i];
        else if (a == "--global") global = true;
        else args.push_back(a);
    }
    if (args.empty()) return usage();
    const std::string& cmd = args[0];
    if (cmd == "--version" || cmd == "version") { printf("%s\n", VRDUAL_BUILD_INFO); return 0; }

    if (cmd == "path") { printf("%s\n", config_path().c_str()); return 0; }

    if (cmd == "init") {
        std::string p = config_path();
        struct stat st;
        if (stat(p.c_str(), &st) == 0) { fprintf(stderr, "exists, not touched: %s\n", p.c_str()); printf("%s\n", p.c_str()); return 0; }
        std::string mk = "mkdir -p '" + p.substr(0, p.rfind('/')) + "'";
        FILE* f = system(mk.c_str()) == 0 ? fopen(p.c_str(), "w") : nullptr;
        if (!f) { fprintf(stderr, "cannot write %s\n", p.c_str()); return 1; }
        fprintf(f, "# vrdual configuration. Precedence: defaults < [global] < [app:NAME] < VRDUAL_* env.\n"
                   "# NAME = VkApplicationInfo name or exe basename (case-insensitive).\n\n[global]\n");
        for (auto& k : kKeys) fprintf(f, "# %s\n%s = %s\n", k.help, k.name, k.def);
        fprintf(f, "\n# Controller buttons -> game (vrdual xr): map_<mode>_<button> for one mode,\n"
                   "# map_<button> for both; KEY_* / BTN_* (linux/input-event-codes.h) or none.\n"
                   "# In [app:NAME] they override these for one game. Built-in defaults:\n");
        for (int m = 0; m < M_COUNT; ++m)
            for (int b = 0; b < B_COUNT; ++b) {
                int c = kMapDefault[m][b];
                const char* why = m == M_GAMEPAD && (b == B_LT || b == B_RT) ? "   (trigger axis)"
                                : m == M_MOUSE && b == B_MENU ? "   (none: cycles mouse_mode)" : "";
                fprintf(f, "# map_%s_%s = %s%s\n", kMapModes[m], kBtnNames[b], input_name(c), why);
            }
        fprintf(f, "\n# Example profile:\n# [app:vrdual-cube]\n# hfov = 90\n# world_scale = 1\n# map_mouse_x = KEY_E\n");
        fclose(f);
        printf("%s\n", p.c_str());
        return 0;
    }

    if (cmd == "profiles") {
        std::string dir = args.size() > 1 ? args[1] : profiles_dir();
        int bad = 0;
        for (auto& p : load_profiles(dir)) {
            std::string ex, ks;
            for (auto& e : p.exe) ex += (ex.empty() ? "" : ",") + e;
            for (auto& kv : p.settings) {
                ks += (ks.empty() ? "" : " ") + kv.first + "=" + kv.second;
                bool known = false;
                for (auto& k : kKeys) known |= kv.first == k.name;
                if (!known) fprintf(stderr, "%s: unknown key '%s'\n", p.file.c_str(), kv.first.c_str()), ++bad;
            }
            if (p.exe.empty()) fprintf(stderr, "%s: no exe in [game]\n", p.file.c_str()), ++bad;
            printf("%s\t%s\t%s\t%s\n", p.file.c_str(), ex.c_str(), p.steam.empty() ? "-" : p.steam.c_str(), ks.c_str());
        }
        return bad ? 1 : 0;
    }

    if (cmd == "show") {
        Settings st = load_settings({args.size() > 1 ? args[1] : ""});
        fprintf(stderr, "config: %s   [app:] section: %s   profiles: %s\n", config_path().c_str(), st.profile.empty() ? "(none)" : st.profile.c_str(),
                profiles_dir().c_str());
        for (auto& k : kKeys) printf("%-20s %-18s # %s\n", k.name, st.s(k.name).c_str(), st.src[k.name].c_str());
        return 0;
    }

    // effective button maps: "mode button code source" (source: built-in or [section] key)
    if (cmd == "map") {
        InputCfg ic = load_input_cfg(args.size() > 1 ? args[1] : "");
        for (int m = 0; m < M_COUNT; ++m)
            for (int b = 0; b < B_COUNT; ++b) {
                int code = ic.map[m][b] >= 0 ? ic.map[m][b] : kMapDefault[m][b];
                printf("%s\t%s\t%s\t%s\n", kMapModes[m], kBtnNames[b], input_name(code), ic.map_src[m][b].empty() ? "built-in" : ic.map_src[m][b].c_str());
            }
        return 0;
    }
    // map-set: writes map_<mode>_<button> (both: map_<button>) into [app:APP] or [global];
    // default removes that key, so the less specific setting applies again
    if (cmd == "map-set") {
        std::vector<std::string> a(args.begin() + 1, args.end());
        if (!global && a.empty()) return usage();
        std::string sec = global ? "global" : "app:" + a[0];
        if (!global) a.erase(a.begin());
        if (a.size() != 3) return usage();
        const std::string &mode = a[0], &btn = a[1], &val = a[2];
        bool okm = mode == "both", okb = false;
        for (auto n : kMapModes) okm |= mode == n;
        for (auto n : kBtnNames) okb |= btn == n;
        if (!okm || !okb) { fprintf(stderr, "mode: gamepad | mouse | both; button: a b x y l3 r3 lt rt lg rg menu\n"); return 2; }
        std::string key = "map_" + (mode == "both" ? "" : mode + "_") + btn;
        bool ok;
        if (lower(val) == "default") ok = remove_key(config_path(), sec, key);
        else if (lower(val) == "none" || input_code(val) >= 0) ok = save_key(config_path(), sec, key, lower(val) == "none" ? "none" : val);
        else { fprintf(stderr, "unknown code '%s' (KEY_* / BTN_* from linux/input-event-codes.h, none, default)\n", val.c_str()); return 2; }
        if (!ok) { fprintf(stderr, "cannot write %s\n", config_path().c_str()); return 1; }
        fprintf(stderr, "[%s] %s %s\n", sec.c_str(), key.c_str(), lower(val) == "default" ? "removed" : ("= " + val).c_str());
        return 0;
    }

    if (cmd == "shaders") {  // vertex shader keys the layers saw, sorted, unique
        std::ifstream f(shm_dir() + "/vrdual-" + session + "-shaders");
        if (!f) { fprintf(stderr, "no shader list for session '%s'\n", session.c_str()); return 1; }
        std::set<std::string> keys;
        for (std::string l; std::getline(f, l);)
            if (l.size() == 16) keys.insert(l);
        for (auto& k : keys) printf("%s\n", k.c_str());
        return 0;
    }

    vrdual_ctl* c = ctl_open(session, nullptr);
    if (!c) { fprintf(stderr, "no running session '%s' (%s)\n", session.c_str(), ctl_path(session).c_str()); return 1; }

    if (cmd == "live") {
        vrdual_ctl v = ctl_read(c);
        printf("app %s\nlive %u\n", v.app, v.live);
        for (auto k : kLiveKeys) printf("%s %s\n", k, live_value(v, k).c_str());
        for (auto k : kHeadKeys) printf("%s %s\n", k, live_value(v, k).c_str());
        return 0;
    }

    if (cmd == "set") {
        if (args.size() != 3) return usage();
        const std::string &k = args[1], &v = args[2];
        bool known = false;
        for (auto lk : kLiveKeys) known |= k == lk;
        for (auto lk : kHeadKeys) known |= k == lk;
        if (!known) { fprintf(stderr, "not a live key: %s (live keys: ipd ipd_near near_dist far_dist anchor hfov world_scale hud_depth head_x head_y head_z head_yaw head_pitch head_roll hud_yaw hud_pitch hud_roll hud)\n", k.c_str()); return 2; }
        ctl_write(c, [&](vrdual_ctl& x) {
            if (k == "ipd") x.ipd = parse_len(v);
            else if (k == "ipd_near") x.ipd_near = parse_len(v);
            else if (k == "near_dist") x.near_dist = parse_len(v);
            else if (k == "far_dist") x.far_dist = parse_len(v);
            else if (k == "anchor") x.anchor = vrdual_anchor_parse(v.c_str());
            else if (k == "hfov") x.hfov = strtof(v.c_str(), nullptr);
            else if (k == "world_scale") x.world_scale = strtof(v.c_str(), nullptr);
            else if (k == "hud_depth") x.hud_depth = parse_len(v);
            else if (k == "head_x") x.head_x = parse_len(v);
            else if (k == "head_y") x.head_y = parse_len(v);
            else if (k == "head_z") x.head_z = parse_len(v);
            else if (k == "head_yaw") x.head_yaw = strtof(v.c_str(), nullptr);
            else if (k == "head_pitch") x.head_pitch = strtof(v.c_str(), nullptr);
            else if (k == "head_roll") x.head_roll = strtof(v.c_str(), nullptr);
            else if (k == "hud_yaw") x.hud_yaw = strtof(v.c_str(), nullptr);
            else if (k == "hud_pitch") x.hud_pitch = strtof(v.c_str(), nullptr);
            else if (k == "hud_roll") x.hud_roll = strtof(v.c_str(), nullptr);
            else if (k == "hud") x.hud_mode = v == "hide" || v == "1" || v == "off";
        });
        vrdual_ctl now = ctl_read(c);
        if (!now.live) fprintf(stderr, "note: session runs in baked mode, change applies after app restart\n");
        printf("%s %s\n", k.c_str(), live_value(now, k).c_str());
        return 0;
    }

    if (cmd == "hunt" && args.size() >= 2 && args[1] == "find") {
        // Bisection (vrdual_hunt.hpp) answered on stdin, one line per question
        // (y yes, n no, ? can't tell, t group on / off, q quit); questions on
        // stderr. Found: its key on stdout, then s saves the rule, d doesn't.
        int m = 1;
        if (args.size() == 3) for (int k = 1; k < 4; ++k) if (args[2] == kHuntModes[k]) m = k;
        if (args.size() > 3 || (args.size() == 3 && !(args[2] == "hide" || args[2] == "noshift" || args[2] == "hud"))) return usage();
        Hunt h;
        h.begin(shader_list(shm_dir() + "/vrdual-" + session + "-shaders"), m);
        auto apply = [&] {
            uint32_t a, b, md;
            h.range(a, b, md);
            ctl_write(c, [&](vrdual_ctl& x) { x.hunt_lo = a; x.hunt_hi = b; x.hunt_mode = md; });
        };
        if (h.asking())
            fprintf(stderr, "hunt: %zu vertex shaders. Watch ONE thing (an artefact, a HUD element) in the HMD or 'vrdual view'.\n"
                            "Each step changes part of them (%s) and asks whether exactly your thing changed / disappeared;\n"
                            "other things may change too: ignore them. Too broken to judge: ? (tries the other half).\n",
                    h.keys.size(), kHuntModes[m]);
        std::string ans;
        while (h.asking()) {
            apply();
            fprintf(stderr, "%s%s; %s: yes (y), no (n), can't tell (?), group on/off (t), quit (q)\n", h.status().c_str(),
                    h.on ? "" : " [group off]", h.phase == Hunt::Confirm ? "your thing still gone/changed" : "your thing changed/gone");
            if (!std::getline(std::cin, ans)) ans = "q";
            char a = ans.empty() ? ' ' : ans[0];
            if (a == 't') h.on = !h.on;
            else if (a == 'y' || a == 'n' || a == '?' || a == 'q') h.answer(a == 'y' ? Hunt::Yes : a == 'n' ? Hunt::No : a == '?' ? Hunt::Unsure : Hunt::Abort);
        }
        apply();  // off
        if (h.phase != Hunt::Found) { fprintf(stderr, "%s\n", h.why.c_str()); return 1; }
        printf("%s\n", h.found().c_str());
        fflush(stdout);
        vrdual_ctl v = ctl_read(c);
        for (;;) {
            fprintf(stderr, "rule %s:%s for [app:%s]: save (s) / don't save (d)\n", h.found().c_str(), kHuntModes[m], v.app);
            if (!std::getline(std::cin, ans)) ans = "d";
            if (!ans.empty() && (ans[0] == 's' || ans[0] == 'd')) break;
        }
        if (ans[0] == 'd') { fprintf(stderr, "not saved; later: vrdual hunt rule %s %s\n", h.found().c_str(), kHuntModes[m]); return 0; }
        std::string val = v.app[0] ? add_shader_rule(v.app, h.found(), kHuntModes[m]) : "";
        if (val.empty()) { fprintf(stderr, "cannot save the rule (%s)\n", v.app[0] ? config_path().c_str() : "session has no app name"); return 1; }
        fprintf(stderr, "[app:%s] shader_rules = %s (applies at the next game start)\n", v.app, val.c_str());
        return 0;
    }

    if (cmd == "hunt") {  // docs/tasks/shader-rules.md
        static const char* const modes[] = {"off", "hide", "noshift", "hud"};
        if (args.size() == 2 && args[1] == "off") ctl_write(c, [](vrdual_ctl& x) { x.hunt_mode = 0; });
        else if (args.size() == 4) {
            int m = 0;
            for (int k = 1; k < 4; ++k) if (args[1] == modes[k]) m = k;
            char *e1, *e2;
            unsigned long lo = strtoul(args[2].c_str(), &e1, 16), hi = strtoul(args[3].c_str(), &e2, 16);
            if (!m || *e1 || *e2 || lo > 0xffffffffull || hi > 0xffffffffull) return usage();
            ctl_write(c, [&](vrdual_ctl& x) { x.hunt_lo = uint32_t(lo); x.hunt_hi = uint32_t(hi); x.hunt_mode = uint32_t(m); });
        } else if (args.size() != 1) return usage();
        vrdual_ctl now = ctl_read(c);
        if (!now.live) fprintf(stderr, "note: session runs in baked mode, the hunt needs live mode\n");
        printf("hunt %s %08x %08x\n", modes[now.hunt_mode & 3], now.hunt_lo, now.hunt_hi);
        return 0;
    }

    if (cmd == "rule") {
        if (args.size() != 3 || (args[2] != "noshift" && args[2] != "hud" && args[2] != "hide")) return usage();
        std::string k = lower(args[1]);
        if (k.size() < 8 || k.size() > 16 || k.find_first_not_of("0123456789abcdef") != std::string::npos) return usage();
        vrdual_ctl v = ctl_read(c);
        if (!v.app[0]) { fprintf(stderr, "session has no app name\n"); return 1; }
        std::string sec = std::string("app:") + v.app, val = add_shader_rule(v.app, k, args[2]);
        if (val.empty()) { fprintf(stderr, "cannot write %s\n", config_path().c_str()); return 1; }
        fprintf(stderr, "[%s] shader_rules = %s (applies at the next game start)\n", sec.c_str(), val.c_str());
        printf("%s\n", val.c_str());
        return 0;
    }

    if (cmd == "save") {
        std::string sec = ctl_save(ctl_read(c), global), p = config_path();
        if (sec.empty()) { fprintf(stderr, "cannot write %s\n", p.c_str()); return 1; }
        fprintf(stderr, "saved live values to %s [%s]\n", p.c_str(), sec.c_str());
        printf("%s\n", p.c_str());
        return 0;
    }
    return usage();
}
