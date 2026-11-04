// vrdual configuration: defaults < [global] < profile < [app:NAME] < VRDUAL_* env.
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
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <cctype>
#include <cerrno>
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
    {"shader_rules", "", "per-shader treatment, comma list KEY:noshift|hud|hide (KEY = hex prefix of the shader hash, 8-16 digits; find it with `_ hunt`)"},
    {"shader_catalog", "", "file of known shader keys (KEY = 16 hex digits first on a line, rest free text): shaders not in it are logged once as new"},
    {"shader_dump", "", "directory: unpatched shaders that have a vertex / tess eval / geometry entry point are written there as KEY.spv (debugging)"},
    {"hud_depth_scope", "all", "which draws hud_depth moves: all (every w == 1 vertex) | rules (only shaders with a hud rule; post-process passes stay put)"},
    {"hud_depth", "0", "show w == 1 draws (HUD) at this distance, meters (0 = infinity; live mode)"},
    {"live", "1", "IPD changeable at runtime (shader reads a GPU buffer)"},
    {"export", "1", "publish frames to /dev/shm for the compositor"},
    {"hunt_fragment", "0", "fragment shaders join the shader hunt (mode hide: what they draw disappears, e.g. a shadow or lighting pass); live params only"},
    {"auto_noshift", "0", "shaders drawn only in off-screen passes (another size than the swapchain and the main view: shadow maps, baked textures) are not shifted per eye; live params only"},
    {"pass_trace", "0", "debugging (auto_noshift): log every render pass and pipeline bind recorded after present N (0 = off)"},
    {"depth_export", "0", "also publish the game's depth buffer with each frame (one process per eye or one GPU; not in mirror mode yet)"},
    {"game_fixes", "1", "byte-checked fixes of known game binaries, applied in the game's process (docs/CONFIG.md: Civ V thread crash); 0 = off"},
    {"cull_widen", "1", "game fix: the game culls with a view this many times wider (tan of the half angles) than it draws, so head_look clip / scroll shows more past the edge (Civ V); 1 = off"},
    {"cursor", "1", "export the X11 cursor (position + image) with the frames; vrdual-xr draws it"},
    {"cursor_depth", "auto", "vrdual-xr cursor depth: auto (the scene under it) | hud (at hud_depth)"},
    {"lockstep", "1", "hold each frame until the other eye has it"},
    {"lockstep_timeout_ms", "250", "give up waiting for the other eye after this"},
    {"edge_mask", "outer", "vrdual-xr: mask the strip at each eye's edge its shifted view gets wrong: outer | both | off"},
    {"edge_mask_dist", "1", "edge_mask: strip width = the eye shift of things this near, meters"},
    {"cached_wp", "0", "mirror: write tracking for cached memory (fast; breaks games that read files straight into it)"},
    {"left_gpu", "auto", "GPU for the left eye (auto | pci:... | index | name; auto: of the first two discrete GPUs by PCI address, the one with a monitor)"},
    {"right_gpu", "auto", "GPU for the right eye (auto: the other one, the headless one if just one has a monitor)"},
    {"wivrn_gpu", "auto", "GPU for wivrn-server (`vrdual wivrn`; vrdual-xr follows): auto (a GPU without a monitor, if there are two) | pci:... | default (Vulkan's choice)"},
    {"log", "1", "layer logging to stderr"},
    // input (vrdual-xr): VR controllers -> virtual pad / mouse / keys; map_<button> = KEY_*|BTN_*
    {"input", "gamepad", "controllers as: gamepad | mouse | tune (sticks: world_scale/hud_depth/hfov; grips: move/scale the screen) | map (edit button maps in the headset) | hunt (find a shader by bisection in the headset) | off. Last used per game wins over [global]; [app:NAME] wins over that"},
    {"input_modes", "all", "control modes menu + a / b (and the mouse menu tap) cycle through: all | none | list of gamepad stick motion aim move tune map hunt (mouse = the 4 mouse modes); none = controllers do nothing"},
    {"mouse_mode", "stick", "mouse from controllers: stick | motion | aim | move (menu button cycles)"},
    {"mouse_speed", "1", "pointer speed factor (per game in [app:NAME]; vrdual xr reloads it live)"},
    {"mouse_curve", "2.5", "stick response exponent (stick mode)"},
    {"deadzone", "0.15", "stick dead zone, 0..1"},
    {"lstick", "wasd", "left stick in mouse mode: wasd | arrows | none"},
    {"head_look", "off", "head rotation turns the game camera: off | mouse | edge | clip (rendered view only, head-locked screen) | scroll (clip + arrow keys past head_rot_max)"},
    {"head_deadzone", "15", "edge mode: degrees from the screen centre without turning"},
    {"head_rate", "120", "edge mode: game turn rate at 45 degrees past the dead zone, deg/s"},
    {"head_counts_per_deg", "10", "mouse counts per degree of game camera turn (calibrate per game)"},
    {"head_lean", "off", "head translation moves the rendered viewpoint: off | on"},
    {"lean_max", "0.1", "head offset soft limit, meters (L in L*tanh(d/L))"},
    {"head_rot_max", "20", "head_look = clip / scroll: rotation soft limit, degrees (L in L*tanh(a/L))"},
    {"head_look_menus", "1", "head_look / head_lean also in frames without a separate 3D view (menus, videos); 0 = only while the game draws its main view apart from the swapchain (Civ V; needs auto_noshift)"},
    {"hud_follow", "0", "head_look = clip / scroll: the HUD stays in place until the head turned this far, then moves along, degrees (0 = moves with the head; 90 = stays)"},
    {"scroll_angle", "0", "head_look = scroll: head angle from the start direction where scrolling starts, degrees (0 = head_rot_max); screen fixed in the room: degrees past its edge (may be < 0)"},
    {"scroll_hold_angle", "0", "head_look = scroll: past this angle the key is held until the head comes back; between scroll_angle and it: short taps (0 = scroll_angle + 10; fixed screen: not above scroll_angle = + 10)"},
    {"scroll_tap_ms", "120", "head_look = scroll: length of one tap, ms"},
    {"scroll_tap_repeat_ms", "500", "head_look = scroll: next tap while the head stays in the tap band, ms (0 = one tap per turn)"},
    {"scroll_keys", "KEY_UP,KEY_DOWN,KEY_LEFT,KEY_RIGHT", "head_look = scroll: keys for up, down, left, right (KEY_* names as in map_*, none = nothing)"},
    {"lock", "world", "vrdual-xr picture: world (stays where it was put) | head (follows the head; head_look clip / scroll need it); vrdual xr --lock wins"},
    {"screen", "curved", "vrdual-xr picture: curved (virtual cinema screen) | flat | full (game FOV)"},
    {"screen_dist", "2.5", "flat/curved screen distance, meters"},
    {"screen_fov", "60", "flat/curved screen width as seen from the centre, degrees"},
    {"screen_scale_x", "1", "picture width scale (all screen modes; two-hand grab changes it live)"},
    {"screen_scale_y", "1", "picture height scale"},
    {"frame_timeout", "1", "vrdual-xr: no new game frame this long (s) -> black (game exited / hung); longer for loading screens that present nothing"},
    {"overlay_time", "4", "vrdual-xr notice (event, current settings) shown this long, seconds (0 = off)"},
    {"lean_y_sign", "1", "1 = y-up clip space (GL/D3D games), -1 = Vulkan apps flipping y in the projection"},
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

inline std::string data_home() {
    if (const char* x = getenv("XDG_DATA_HOME"); x && *x) return x;
    const char* h = getenv("HOME");
    return std::string(h ? h : ".") + "/.local/share";
}

// Curated per-game profiles: one file per game, PROFILES_DIR/*.conf
// (`vrdual profiles sync` / `vrdual install` copy the repo's profiles/ there):
//   [game]
//   exe = mirrorsedge.exe          # names matched like [app:NAME], comma list
//   steam = 17410                  # optional alias: matches $SteamAppId
//   intro = Video/Title_*.bik      # optional: logo videos (`vrdual intro`, read by `_` only)
//   [settings]
//   world_scale = 50
// Precedence: defaults < [global] < profile < [app:NAME] < env.
inline std::string profiles_dir() {
    if (const char* p = getenv("VRDUAL_PROFILES"); p && *p) return p;
    return data_home() + "/vrdual/profiles";
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

struct Profile {
    std::string file;                          // basename without .conf
    std::vector<std::string> exe;              // lower case
    std::string steam;
    std::map<std::string, std::string> settings;
};

inline std::vector<Profile> load_profiles(const std::string& dir = profiles_dir()) {
    std::vector<Profile> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 5 && n[0] != '.' && n.compare(n.size() - 5, 5, ".conf") == 0) names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        Ini ini = parse_ini(dir + "/" + n);
        Profile p;
        p.file = n.substr(0, n.size() - 5);
        std::stringstream ex(ini["game"]["exe"]);
        for (std::string x; std::getline(ex, x, ',');)
            if (!trim(x).empty()) p.exe.push_back(lower(trim(x)));
        p.steam = ini["game"]["steam"];
        p.settings = ini["settings"];
        out.push_back(std::move(p));
    }
    return out;
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
    // profile: the first name with one wins, then the Steam app id
    std::vector<Profile> profs = load_profiles();
    const Profile* hit = nullptr;
    for (auto& n : app_names)
        for (auto& p : profs)
            if (!hit && !n.empty() && std::find(p.exe.begin(), p.exe.end(), lower(n)) != p.exe.end()) hit = &p;
    const char* sid = getenv("SteamAppId");
    for (auto& p : profs)
        if (!hit && sid && *sid && strcmp(sid, "0") && p.steam == sid) hit = &p;
    if (hit)
        for (auto& kv : hit->settings)
            if (st.val.count(kv.first)) { st.val[kv.first] = kv.second; st.src[kv.first] = "profile " + hit->file; }
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

inline std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream f(path);
    std::string l;
    while (std::getline(f, l)) lines.push_back(l);
    return lines;
}

// Replaces the file in one step (temp file + rename): a game starting while
// the config is saved reads the old or the new file, never a truncated one.
// A symlinked config (dotfiles) stays a symlink: the target is replaced.
inline bool write_lines(const std::string& path, const std::vector<std::string>& lines) {
    std::string real = path;
    if (char* r = realpath(path.c_str(), nullptr)) { real = r; free(r); }
    for (size_t s = real.find('/', 1); s != std::string::npos; s = real.find('/', s + 1))  // mkdir -p of the directory
        if (mkdir(real.substr(0, s).c_str(), 0755) != 0 && errno != EEXIST) return false;
    std::string tmp = real + ".tmp." + std::to_string(getpid());
    {
        std::ofstream o(tmp, std::ios::trunc);
        for (auto& l : lines) o << l << "\n";
        o.flush();
        if (!o) { unlink(tmp.c_str()); return false; }
    }
    struct stat st;
    if (stat(real.c_str(), &st) == 0) chmod(tmp.c_str(), st.st_mode & 07777);
    if (rename(tmp.c_str(), real.c_str()) != 0) { unlink(tmp.c_str()); return false; }
    return true;
}

// Sets key=value inside [section] of `lines`, keeping comments and every other line.
inline void set_key(std::vector<std::string>& lines, const std::string& section, const std::string& key, const std::string& value) {
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
}

// Sets several keys of [section] with one write of the file.
inline bool save_keys(const std::string& path, const std::string& section, const std::vector<std::pair<std::string, std::string>>& kvs) {
    std::vector<std::string> lines = read_lines(path);
    for (auto& kv : kvs) set_key(lines, section, kv.first, kv.second);
    return write_lines(path, lines);
}

inline bool save_key(const std::string& path, const std::string& section, const std::string& key, const std::string& value) {
    return save_keys(path, section, {{key, value}});
}

// Removes key from [section] (no-op if absent), keeping every other line.
inline bool remove_key(const std::string& path, const std::string& section, const std::string& key) {
    std::vector<std::string> lines = read_lines(path);
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
    return !hit || write_lines(path, lines);
}

// Anchor/IPD -> eye displacement along view +X in meters.
inline float eye_dx(const std::string& anchor, int eye, float ipd) {
    if (anchor == "left") return eye == 0 ? 0 : ipd;
    if (anchor == "right") return eye == 0 ? -ipd : 0;
    return eye == 0 ? -ipd / 2 : ipd / 2;
}

}  // namespace vrdual
