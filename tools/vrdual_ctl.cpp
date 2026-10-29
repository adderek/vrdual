// vrdual-ctl — configuration and live control (machine contract: no prompts,
// stdout is data, prose on stderr, exit code is the answer).
//
//   vrdual-ctl path                       config file location
//   vrdual-ctl init                       write a commented default config if none exists
//   vrdual-ctl show [APP]                 effective settings for APP, with their source
//   vrdual-ctl live [--session S]         current live values of a running session
//   vrdual-ctl set KEY VALUE [--session S]   change a live value (ipd accepts "64.5mm")
//   vrdual-ctl save [--global] [--session S] write live values into the APP profile (or [global])
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../common/vrdual_version.h"
#include "../common/vrdual_ctl.hpp"
#include "../common/vrdual_input.hpp"

using namespace vrdual;

static float parse_len(const std::string& v) {  // meters, or "64.5mm"
    float f = strtof(v.c_str(), nullptr);
    return v.size() > 2 && v.compare(v.size() - 2, 2, "mm") == 0 ? f / 1000.0f : f;
}

static int usage() {
    fprintf(stderr, "usage: vrdual-ctl version | path | init | show [APP] | map [APP] | map-set APP|--global gamepad|mouse|both BUTTON CODE|none|default\n"
                    "                  | live | set KEY VALUE | save [--global]   [--session S]\n");
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

    if (cmd == "show") {
        Settings st = load_settings({args.size() > 1 ? args[1] : ""});
        fprintf(stderr, "config: %s   profile: %s\n", config_path().c_str(), st.profile.empty() ? "(none)" : st.profile.c_str());
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
        if (!known) { fprintf(stderr, "not a live key: %s (live keys: ipd ipd_near near_dist far_dist anchor hfov world_scale hud_depth head_x head_y head_z)\n", k.c_str()); return 2; }
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
        });
        vrdual_ctl now = ctl_read(c);
        if (!now.live) fprintf(stderr, "note: session runs in baked mode, change applies after app restart\n");
        printf("%s %s\n", k.c_str(), live_value(now, k).c_str());
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
