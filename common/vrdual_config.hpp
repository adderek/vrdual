// vrdual configuration: defaults < [global] < [app:NAME] < VRDUAL_* env.
// Shared by the layer, the compositor and vrdual-ctl. Header-only, no deps.
//
// File: $VRDUAL_CONFIG, else $XDG_CONFIG_HOME/vrdual/vrdual.conf,
//       else ~/.config/vrdual/vrdual.conf
//
//   [global]
//   ipd = 0.064
//   [app:vrdual-cube]          # matches VkApplicationInfo name or exe basename
//   hfov = 90
#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace vrdual {

struct Key {
    const char* name;
    const char* def;
    const char* help;
};

// Order = order of `vrdual config show`.
static const Key kKeys[] = {
    {"ipd", "0.064", "interpupillary distance for far objects, meters"},
    {"ipd_near", "0", "optional IPD for near objects, meters (0 = same as ipd)"},
    {"near_dist", "0.5", "distance where ipd_near applies fully, meters"},
    {"far_dist", "5", "distance where ipd applies fully, meters"},
    {"anchor", "center", "original camera is: center | left | right eye"},
    {"world_scale", "1", "app units per meter"},
    {"hfov", "90", "assumed horizontal FOV of the app camera, degrees"},
    {"stereo", "1", "0 = layer passive in this app (launchers): no mirror, shader patch or export"},
    {"skip_w1", "1", "leave w == 1 vertices (HUD/post/ortho) unshifted"},
    {"sparse", "0", "1 = sparse binding/residency visible to the app (D3D12 feature level 12_0 needs it); mirror replays the binds"},
    {"hud_depth", "0", "show w == 1 draws (HUD) at this distance, meters (0 = infinity; live mode)"},
    {"live", "1", "IPD changeable at runtime (shader reads a GPU buffer)"},
    {"export", "1", "publish frames to /dev/shm for the compositor"},
    {"cursor", "1", "export the X11 cursor (position + image) with the frames; vrdual-xr draws it"},
    {"cursor_depth", "auto", "vrdual-xr cursor depth: auto (the scene under it) | hud (at hud_depth)"},
    {"lockstep", "1", "hold each frame until the other eye has it"},
    {"lockstep_timeout_ms", "250", "give up waiting for the other eye after this"},
    {"edge_mask", "outer", "vrdual-xr: mask the strip at each eye's edge its shifted view gets wrong: outer | both | off"},
    {"edge_mask_dist", "1", "edge_mask: strip width = the eye shift of things this near, meters"},
    {"cached_wp", "0", "mirror: write tracking for cached memory (fast; breaks games that read files straight into it)"},
    {"left_gpu", "pci:0000:06:00.0", "GPU for the left eye (pci:..., index or name)"},
    {"right_gpu", "pci:0000:0b:00.0", "GPU for the right eye"},
    {"wivrn_gpu", "pci:0000:0b:00.0", "GPU for wivrn-server (`vrdual wivrn`; vrdual-xr follows): pci:... | default (Vulkan's choice)"},
    {"log", "1", "layer logging to stderr"},
    // input (vrdual-xr): VR controllers -> virtual pad / mouse / keys; map_<button> = KEY_*|BTN_*
    {"input", "gamepad", "controllers as: gamepad | mouse | tune (sticks: world_scale/hud_depth/hfov; grips: move/scale the screen) | map (edit button maps in the headset) | off. Last used per game wins over [global]; [app:NAME] wins over that"},
    {"mouse_mode", "stick", "mouse from controllers: stick | motion | aim (menu button cycles)"},
    {"mouse_speed", "1", "pointer speed factor"},
    {"mouse_curve", "2.5", "stick response exponent (stick mode)"},
    {"deadzone", "0.15", "stick dead zone, 0..1"},
    {"lstick", "wasd", "left stick in mouse mode: wasd | arrows | none"},
    {"head_look", "off", "head rotation turns the game camera: off | mouse | edge"},
    {"head_deadzone", "15", "edge mode: degrees from the screen centre without turning"},
    {"head_rate", "120", "edge mode: game turn rate at 45 degrees past the dead zone, deg/s"},
    {"head_counts_per_deg", "10", "mouse counts per degree of game camera turn (calibrate per game)"},
    {"head_lean", "off", "head translation moves the rendered viewpoint: off | on"},
    {"lean_max", "0.1", "head offset soft limit, meters (L in L*tanh(d/L))"},
    {"screen", "curved", "vrdual-xr picture: curved (virtual cinema screen) | flat | full (game FOV)"},
    {"screen_dist", "2.5", "flat/curved screen distance, meters"},
    {"screen_fov", "60", "flat/curved screen width as seen from the centre, degrees"},
    {"screen_scale_x", "1", "picture width scale (all screen modes; two-hand grab changes it live)"},
    {"screen_scale_y", "1", "picture height scale"},
    {"overlay_time", "4", "vrdual-xr notice (event, current settings) shown this long, seconds (0 = off)"},
    {"lean_y_sign", "1", "1 = y-up clip space (GL/D3D games), -1 = Vulkan apps flipping y in the projection"},
};

// Built-in per-game values (user tested unless noted). Precedence:
// defaults < [global] < these < [app:NAME] < env. Matched like [app:NAME].
struct AppValue {
    const char* app;
    const char* key;
    const char* value;
};
static const AppValue kAppProfiles[] = {
    {"psychonauts", "skip_w1", "0"},          // draws the scene with w == 1: unshifted = no stereo
    {"mirrorsedge.exe", "world_scale", "50"},
    {"mirrorsedge.exe", "hud_depth", "1"},    // crosshair fuses at 1 m (user, 2026-10-20)
    {"tesv.exe", "world_scale", "70"},        // Skyrim: 1 unit = 1.43 cm (not tuned yet)
    {"falloutnv.exe", "world_scale", "70"},   // same engine family (not tuned yet)
    {"upc.exe", "stereo", "0"},               // Ubisoft Connect: CEF UI full of artefacts when mirrored
    {"uplaywebcore.exe", "stereo", "0"},
    {"eadesktop.exe", "stereo", "0"},         // EA app: a launcher, nothing to see in 3D
    {"explorer.exe", "stereo", "0"},          // Wine desktop / tray (Zink), streamed the empty desktop
    {"xalia.exe", "stereo", "0"},             // Proton's accessibility helper
};

inline std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
inline std::string lower(std::string s) {
    for (auto& c : s) c = char(tolower((unsigned char)c));
    return s;
}

inline std::string config_path() {
    if (const char* p = getenv("VRDUAL_CONFIG"); p && *p) return p;
    if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/vrdual/vrdual.conf";
    const char* h = getenv("HOME");
    return std::string(h ? h : ".") + "/.config/vrdual/vrdual.conf";
}

// section -> key -> value
using Ini = std::map<std::string, std::map<std::string, std::string>>;

inline Ini parse_ini(const std::string& path) {
    Ini ini;
    std::ifstream f(path);
    std::string line, sec = "global";
    while (std::getline(f, line)) {
        if (auto h = line.find('#'); h != std::string::npos) line.erase(h);
        if (auto h = line.find(';'); h != std::string::npos) line.erase(h);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') { sec = lower(trim(line.substr(1, line.size() - 2))); continue; }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        ini[sec][lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
    }
    return ini;
}

struct Settings {
    std::map<std::string, std::string> val, src;  // value and where it came from
    std::string profile;                           // matched [app:...] section, or ""
    std::string app;                               // app name to save a profile under

    std::string s(const char* k) const { auto it = val.find(k); return it == val.end() ? "" : it->second; }
    float f(const char* k) const { return strtof(s(k).c_str(), nullptr); }
    int i(const char* k) const { return atoi(s(k).c_str()); }
};

// app_names: candidates to match against [app:NAME] (case-insensitive), first match wins.
inline Settings load_settings(const std::vector<std::string>& app_names) {
    Settings st;
    for (auto& k : kKeys) { st.val[k.name] = k.def; st.src[k.name] = "default"; }
    std::string path = config_path();
    Ini ini = parse_ini(path);
    auto apply = [&](const std::string& sec, const std::string& label) {
        auto it = ini.find(sec);
        if (it == ini.end()) return;
        for (auto& kv : it->second)
            if (st.val.count(kv.first)) { st.val[kv.first] = kv.second; st.src[kv.first] = label; }
    };
    apply("global", path + " [global]");
    for (auto& n : app_names) {
        if (n.empty()) continue;
        bool hit = false;
        for (auto& a : kAppProfiles)
            if (lower(n) == a.app) { st.val[a.key] = a.value; st.src[a.key] = std::string("built-in [app:") + a.app + "]"; hit = true; }
        if (hit) break;
    }
    for (auto& n : app_names) {
        if (n.empty()) continue;
        if (st.app.empty()) st.app = n;
        std::string sec = "app:" + lower(n);
        if (ini.count(sec)) { st.profile = st.app = n; apply(sec, path + " [app:" + n + "]"); break; }
    }
    for (auto& k : kKeys) {
        std::string env = "VRDUAL_";
        for (const char* c = k.name; *c; ++c) env += char(toupper((unsigned char)*c));
        if (const char* v = getenv(env.c_str()); v && *v) { st.val[k.name] = v; st.src[k.name] = "env " + env; }
    }
    return st;
}

// Sets key=value inside [section], keeping comments and every other line.
inline bool save_key(const std::string& path, const std::string& section, const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        std::string l;
        while (std::getline(f, l)) lines.push_back(l);
    }
    std::string want = lower(section);
    int sec_start = -1, sec_end = int(lines.size()), key_line = -1;
    std::string cur = "global";
    bool in_sec = want == "global";
    if (in_sec) sec_start = 0;
    for (int n = 0; n < int(lines.size()); ++n) {
        std::string t = trim(lines[n]);
        if (!t.empty() && t.front() == '[' && t.back() == ']') {
            if (in_sec) { sec_end = n; in_sec = false; }
            cur = lower(trim(t.substr(1, t.size() - 2)));
            if (cur == want) { in_sec = true; sec_start = n + 1; sec_end = int(lines.size()); }
            continue;
        }
        if (!in_sec) continue;
        auto eq = t.find('=');
        if (eq != std::string::npos && t[0] != '#' && t[0] != ';' && lower(trim(t.substr(0, eq))) == lower(key)) key_line = n;
    }
    std::string kv = key + " = " + value;
    if (key_line >= 0) lines[key_line] = kv;
    else if (sec_start >= 0) {
        // insert after the last non-empty line of the section
        int at = sec_end;
        while (at > sec_start && trim(lines[at - 1]).empty()) --at;
        lines.insert(lines.begin() + at, kv);
    } else {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
        lines.push_back("[" + section + "]");
        lines.push_back(kv);
    }
    if (auto slash = path.rfind('/'); slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        std::string cmd = "mkdir -p '" + dir + "'";
        if (system(cmd.c_str()) != 0) return false;
    }
    std::ofstream o(path, std::ios::trunc);
    for (auto& l : lines) o << l << "\n";
    return bool(o);
}

// Removes key from [section] (no-op if absent), keeping every other line.
inline bool remove_key(const std::string& path, const std::string& section, const std::string& key) {
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        std::string l;
        while (std::getline(f, l)) lines.push_back(l);
    }
    std::string want = lower(section), cur = "global";
    bool hit = false;
    for (size_t n = 0; n < lines.size();) {
        std::string t = trim(lines[n]);
        if (!t.empty() && t.front() == '[' && t.back() == ']') cur = lower(trim(t.substr(1, t.size() - 2)));
        auto eq = t.find('=');
        if (cur == want && eq != std::string::npos && t[0] != '#' && t[0] != ';' && lower(trim(t.substr(0, eq))) == lower(key)) {
            lines.erase(lines.begin() + n);
            hit = true;
            continue;
        }
        ++n;
    }
    if (!hit) return true;
    std::ofstream o(path, std::ios::trunc);
    for (auto& l : lines) o << l << "\n";
    return bool(o);
}

// Anchor/IPD -> eye displacement along view +X in meters.
inline float eye_dx(const std::string& anchor, int eye, float ipd) {
    if (anchor == "left") return eye == 0 ? 0 : ipd;
    if (anchor == "right") return eye == 0 ? -ipd : 0;
    return eye == 0 ? -ipd / 2 : ipd / 2;
}

}  // namespace vrdual
