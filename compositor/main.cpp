// vrdual-compositor — pairs left/right frames from /dev/shm (or $VRDUAL_SHM_DIR) and shows them
// side by side (the PoC stand-in for an HMD output). Also the calibration UI:
// keys edit the session's live control block, both eyes follow next frame.
//
//   vrdual-compositor [--session S] [--swap] [--scale F]
//   vrdual-compositor --snapshot out.png [--at N] [--timeout SEC]   (no window)
//   vrdual-compositor --record out.mp4|DIR [--seconds N] [--fps N] [--half] [--audio SOURCE]
//                     SBS video through ffmpeg (no window); DIR: <app>_<time>_LR.mp4 in it,
//                     the path is printed on stdout
#include <SDL3/SDL.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "../common/vrdual_version.h"
#include "../common/vrdual_ctl.hpp"
#include "../common/vrdual_frames.hpp"
#include "../common/vrdual_shm.h"

using vrdual::Eye;
using vrdual::eye_map;
using vrdual::pair_newest;
using vrdual::now_s;

static SDL_Surface* sbs_surface(Eye& L, Eye& R) {
    int W = int(L.w + R.w), H = int(L.h > R.h ? L.h : R.h);
    SDL_Surface* s = SDL_CreateSurface(W, H, SDL_PIXELFORMAT_XRGB8888);
    SDL_ClearSurface(s, 0, 0, 0, 1);
    Eye* eyes[2] = {&L, &R};
    int x = 0;
    for (Eye* e : eyes) {
        SDL_PixelFormat pf = e->fmt == VRDUAL_FMT_BGRA8 ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888;
        SDL_Surface* src = SDL_CreateSurfaceFrom(int(e->w), int(e->h), pf, e->px.data(), int(e->w * 4));
        SDL_Rect dst{x, 0, int(e->w), int(e->h)};
        SDL_BlitSurface(src, nullptr, s, &dst);
        SDL_DestroySurface(src);
        x += int(e->w);
    }
    return s;
}

static const char* kHelp[] = {
    "Up/Down     IPD (far)  +-0.5mm   (Shift: 0.1mm)",
    "PgUp/PgDn   IPD near   +-0.5mm   (Shift: 0.1mm)",
    "N           near/far IPD on/off",
    "[ / ]       near dist  -/+ 0.1m",
    "A           anchor center/left/right",
    "- / =       assumed HFOV -/+ 1 deg",
    "W save to profile  S swap eyes  H hide  Q quit",
};

// Returns a status line for the overlay ("" = nothing to report).
static std::string handle_key(vrdual_ctl* c, SDL_Keycode k, SDL_Keymod mod) {
    if (!c) return "no control block yet";
    float step = (mod & SDL_KMOD_SHIFT) ? 0.0001f : 0.0005f;
    vrdual_ctl v = vrdual::ctl_read(c);
    switch (k) {
    case SDLK_UP: vrdual::ctl_write(c, [&](vrdual_ctl& x) { x.ipd += step; }); break;
    case SDLK_DOWN: vrdual::ctl_write(c, [&](vrdual_ctl& x) { x.ipd = std::max(0.0f, x.ipd - step); }); break;
    case SDLK_PAGEUP: case SDLK_PAGEDOWN: {
        float s = k == SDLK_PAGEUP ? step : -step;
        vrdual::ctl_write(c, [&](vrdual_ctl& x) { x.ipd_near = std::max(0.0f, (x.ipd_near > 0 ? x.ipd_near : x.ipd) + s); });
        break;
    }
    case SDLK_N: vrdual::ctl_write(c, [&](vrdual_ctl& x) { x.ipd_near = x.ipd_near > 0 ? 0 : x.ipd; }); break;
    case SDLK_LEFTBRACKET: vrdual::ctl_write(c, [&](vrdual_ctl& x) { x.near_dist = std::max(0.1f, x.near_dist - 0.1f); }); break;
    case SDLK_RIGHTBRACKET: vrdual::ctl_write(c, [&](vrdual_ctl& x) { x.near_dist = std::min(x.far_dist - 0.1f, x.near_dist + 0.1f); }); break;
    case SDLK_A: vrdual::ctl_write(c, [&](vrdual_ctl& x) { x.anchor = (x.anchor + 1) % 3; }); break;
    case SDLK_MINUS: vrdual::ctl_write(c, [&](vrdual_ctl& x) { x.hfov = std::max(10.0f, x.hfov - 1); }); break;
    case SDLK_EQUALS: vrdual::ctl_write(c, [&](vrdual_ctl& x) { x.hfov = std::min(170.0f, x.hfov + 1); }); break;
    case SDLK_W: {
        std::string sec = vrdual::ctl_save(v, false);
        return sec.empty() ? "save FAILED: " + vrdual::config_path() : "saved to [" + sec + "]";
    }
    default: return "";
    }
    return v.live ? "" : "baked mode: restart the app to apply";
}

static void draw_overlay(SDL_Renderer* ren, vrdual_ctl* c, float half_w, const std::string& status) {
    std::vector<std::string> lines;
    char b[160];
    if (c) {
        vrdual_ctl v = vrdual::ctl_read(c);
        snprintf(b, sizeof b, "app %s   %s", v.app[0] ? v.app : "?", v.live ? "LIVE" : "BAKED (restart to apply)");
        lines.push_back(b);
        snprintf(b, sizeof b, "IPD far  %.1f mm   anchor %s   hfov %g", v.ipd * 1e3, vrdual_anchor_name(v.anchor), v.hfov);
        lines.push_back(b);
        if (v.ipd_near > 0) snprintf(b, sizeof b, "IPD near %.1f mm   below %.1f m, blend to far at %.1f m", v.ipd_near * 1e3, v.near_dist, v.far_dist);
        else snprintf(b, sizeof b, "IPD near off (N to enable)");
        lines.push_back(b);
    } else {
        lines.push_back("waiting for control block...");
    }
    for (auto h : kHelp) lines.push_back(h);
    if (!status.empty()) lines.push_back("> " + status);
    // Same position in both halves = zero disparity (sits at screen depth).
    SDL_SetRenderScale(ren, 2, 2);
    for (int eye = 0; eye < 2; ++eye) {
        float x = (eye * half_w + 16) / 2, y = 8;
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 160);
        SDL_FRect bg{x - 4, y - 4, 8.0f * 52 + 8, 10.0f * lines.size() + 8};
        SDL_RenderFillRect(ren, &bg);
        SDL_SetRenderDrawColor(ren, 255, 255, 160, 255);
        for (auto& l : lines) { SDL_RenderDebugText(ren, x, y, l.c_str()); y += 10; }
    }
    SDL_SetRenderScale(ren, 1, 1);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
}

// Fixed-rate SBS video: at each output tick the newest pair (a slower game
// repeats its frame), raw pixels piped into ffmpeg (libx264 on the CPU: no
// load on either GPU). Video length is always seconds * fps frames.
static int record(Eye& L, Eye& R, bool swap, std::string out, double seconds, int fps, bool half, const std::string& audio, double timeout) {
    double t0 = now_s();
    uint64_t F = 0;
    for (;;) {
        if (now_s() - t0 > timeout) { fprintf(stderr, "[compositor] timeout waiting for game frames\n"); return 1; }
        if (eye_map(L) && eye_map(R) && pair_newest(L, R, 0, &F)) break;
        usleep(2000);
    }
    if (L.fmt != R.fmt || L.h != R.h) { fprintf(stderr, "[compositor] eyes differ in format or height, cannot record\n"); return 1; }
    struct stat st;
    if (stat(out.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        std::string app = L.hdr->app[0] ? std::string(L.hdr->app, strnlen(L.hdr->app, sizeof L.hdr->app)) : "game";
        for (char& c : app) if (!isalnum((unsigned char)c) && c != '-' && c != '.') c = '_';
        time_t t = time(nullptr);
        char ts[32];
        strftime(ts, sizeof ts, "%Y-%m-%d_%H%M%S", localtime(&t));
        out += "/" + app + "_" + ts + "_LR.mp4";
    }
    const uint32_t w0 = L.w, h = L.h, W = L.w + R.w, fmt = L.fmt;
    auto q = [](const std::string& a) {  // single-quoted for sh
        std::string r = "'";
        for (char c : a) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return r + "'";
    };
    std::string cmd = "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt " + std::string(fmt == VRDUAL_FMT_BGRA8 ? "bgra" : "rgba") +
                      " -s " + std::to_string(W) + "x" + std::to_string(h) + " -r " + std::to_string(fps) + " -i -";
    if (!audio.empty()) cmd += " -f pulse -i " + q(audio) + " -c:a aac -b:a 192k";
    // x264 needs even sizes; half = each eye squeezed to half width (half SBS)
    cmd += std::string(" -vf 'scale=") + (half ? "trunc(iw/4)*2" : "trunc(iw/2)*2") + ":trunc(ih/2)*2'" +
           " -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p -movflags +faststart -t " + std::to_string(seconds) + " " + q(out);
    signal(SIGPIPE, SIG_IGN);  // ffmpeg gone: fwrite fails, reported below
    FILE* ff = popen(cmd.c_str(), "w");
    if (!ff) { fprintf(stderr, "[compositor] cannot start ffmpeg\n"); return 1; }
    fprintf(stderr, "[compositor] recording %ux%u%s, %g s at %d fps -> %s\n", W, h, half ? " (half SBS)" : "", seconds, fps, out.c_str());
    std::vector<uint8_t> buf(size_t(W) * h * 4);
    auto compose = [&] {
        Eye* e[2] = {swap ? &R : &L, swap ? &L : &R};
        for (uint32_t y = 0; y < h; ++y) {
            memcpy(&buf[size_t(y) * W * 4], &e[0]->px[size_t(y) * e[0]->w * 4], size_t(e[0]->w) * 4);
            memcpy(&buf[(size_t(y) * W + e[0]->w) * 4], &e[1]->px[size_t(y) * e[1]->w * 4], size_t(e[1]->w) * 4);
        }
    };
    compose();
    const long n = lround(seconds * fps);
    long fresh = 1, written = 0;
    uint64_t shown = F + 1;
    double start = now_s(), behind = 0;
    bool ok = true;
    for (long i = 0; i < n; ++i) {
        double due = start + double(i) / fps, t = now_s();
        if (t < due) usleep(useconds_t((due - t) * 1e6));
        else behind = std::max(behind, t - due);
        if (eye_map(L) && eye_map(R)) {
            if (L.w != w0 || R.w != W - w0 || L.h != h || R.h != h || L.fmt != fmt) { fprintf(stderr, "[compositor] game swapchain changed, recording stopped\n"); break; }
            if (pair_newest(L, R, shown, &F)) shown = F + 1, ++fresh, compose();
        }
        if (fwrite(buf.data(), buf.size(), 1, ff) != 1) { ok = false; break; }
        ++written;
    }
    int rc = pclose(ff);
    fprintf(stderr, "[compositor] recorded %ld frames (%ld new game frames, %.1f/s), behind schedule at most %.0f ms%s; ffmpeg %s\n", written,
            fresh, fresh / std::max(1e-3, now_s() - start), behind * 1e3, behind > 1.0 / fps ? " (encoder too slow: try --half)" : "",
            rc == 0 && ok ? "ok" : "failed");
    if (rc != 0 || !ok) return 1;
    printf("%s\n", out.c_str());
    return 0;
}

int main(int argc, char** argv) {
    std::string session = "default", snapshot, rec, audio;
    double seconds = 5;
    int fps = 60;
    bool half = false;
    bool swap = false;
    float scale = 1.0f;
    uint64_t at = 0;
    double timeout = 30;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--session" && i + 1 < argc) session = argv[++i];
        else if (a == "--swap") swap = true;
        else if (a == "--scale" && i + 1 < argc) scale = strtof(argv[++i], nullptr);
        else if (a == "--snapshot" && i + 1 < argc) snapshot = argv[++i];
        else if (a == "--at" && i + 1 < argc) at = strtoull(argv[++i], nullptr, 10);
        else if (a == "--timeout" && i + 1 < argc) timeout = strtod(argv[++i], nullptr);
        else if (a == "--record" && i + 1 < argc) rec = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = strtod(argv[++i], nullptr);
        else if (a == "--fps" && i + 1 < argc) fps = atoi(argv[++i]);
        else if (a == "--half") half = true;
        else if (a == "--audio" && i + 1 < argc) audio = argv[++i];
        else if (a == "--version") { printf("%s\n", VRDUAL_BUILD_INFO); return 0; }
        else { fprintf(stderr, "usage: %s [--session S] [--swap] [--scale F] [--snapshot out.png [--at N] [--timeout SEC]] [--record out.mp4|DIR [--seconds N] [--fps N] [--half] [--audio SOURCE]]\n", argv[0]); return 2; }
    }
    std::string base = vrdual::shm_dir() + "/vrdual-" + session + "-";
    Eye L{0, base + "0"}, R{1, base + "1"};
    double t0 = now_s();

    if (!rec.empty()) return record(L, R, swap, rec, seconds, fps > 0 ? fps : 60, half, audio, timeout);
    if (!snapshot.empty()) {
        uint64_t F = 0;
        for (;;) {
            if (now_s() - t0 > timeout) { fprintf(stderr, "[compositor] timeout waiting for paired frame >= %llu\n", (unsigned long long)at); return 1; }
            if (eye_map(L) && eye_map(R) && pair_newest(L, R, at, &F)) break;
            usleep(2000);
        }
        if (!SDL_Init(0)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
        SDL_Surface* s = sbs_surface(swap ? R : L, swap ? L : R);
        bool ok = SDL_SavePNG(s, snapshot.c_str());
        fprintf(stderr, "[compositor] paired frame %llu -> %s (%dx%d) %s\n", (unsigned long long)F, snapshot.c_str(), s->w, s->h, ok ? "ok" : SDL_GetError());
        SDL_DestroySurface(s);
        return ok ? 0 : 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture *tex_l = nullptr, *tex_r = nullptr;
    uint64_t shown = 0, nshown = 0, last_report_frames = 0;
    double last_report = now_s();
    bool running = true, overlay = true;
    vrdual_ctl* ctl = nullptr;
    std::string status;
    fprintf(stderr, "[compositor] waiting for %s and %s\n", L.name.c_str(), R.name.c_str());
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_Q))) running = false;
            if (e.type != SDL_EVENT_KEY_DOWN) continue;
            if (e.key.key == SDLK_S) swap = !swap;
            else if (e.key.key == SDLK_H) overlay = !overlay;
            else {
                std::string st = handle_key(ctl, e.key.key, e.key.mod);
                if (!st.empty()) status = st;
                if (!st.empty() || e.key.key == SDLK_UP || e.key.key == SDLK_DOWN) {
                    vrdual_ctl v = ctl ? vrdual::ctl_read(ctl) : vrdual_ctl{};
                    if (ctl) fprintf(stderr, "[compositor] ipd %.1fmm near %.1fmm anchor %s hfov %g %s\n", v.ipd * 1e3, v.ipd_near * 1e3,
                                     vrdual_anchor_name(v.anchor), v.hfov, st.c_str());
                }
            }
        }
        if (!ctl) ctl = vrdual::ctl_open(session, nullptr);
        uint64_t F;
        if (!(eye_map(L) && eye_map(R) && pair_newest(L, R, shown, &F))) { usleep(1000); continue; }
        shown = F + 1;
        ++nshown;
        int W = int((L.w + R.w) * scale), H = int((L.h > R.h ? L.h : R.h) * scale);
        if (!win) {
            SDL_CreateWindowAndRenderer("vrdual — side by side (S: swap eyes, Q: quit)", W, H, SDL_WINDOW_RESIZABLE, &win, &ren);
            SDL_SetRenderVSync(ren, 0);
            SDL_SetRenderLogicalPresentation(ren, int(L.w + R.w), int(L.h > R.h ? L.h : R.h), SDL_LOGICAL_PRESENTATION_LETTERBOX);
        }
        Eye* eyes[2] = {swap ? &R : &L, swap ? &L : &R};
        float x = 0;
        SDL_RenderClear(ren);
        for (Eye* ey : eyes) {
            SDL_Texture*& tex = ey->eye ? tex_r : tex_l;
            if (ey->remapped && tex) { SDL_DestroyTexture(tex); tex = nullptr; }
            ey->remapped = false;
            if (!tex) {
                SDL_PixelFormat pf = ey->fmt == VRDUAL_FMT_BGRA8 ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888;
                tex = SDL_CreateTexture(ren, pf, SDL_TEXTUREACCESS_STREAMING, int(ey->w), int(ey->h));
            }
            SDL_UpdateTexture(tex, nullptr, ey->px.data(), int(ey->w * 4));
            SDL_FRect dst{x, 0, float(ey->w), float(ey->h)};
            SDL_RenderTexture(ren, tex, nullptr, &dst);
            x += float(ey->w);
        }
        if (overlay) draw_overlay(ren, ctl, float(eyes[0]->w), status);
        SDL_RenderPresent(ren);
        double t = now_s();
        if (t - last_report > 2.0) {
            fprintf(stderr, "[compositor] %.1f paired fps, frame %llu, L-R lag %lld\n", (nshown - last_report_frames) / (t - last_report),
                    (unsigned long long)F, (long long)L.hdr->last_frame - (long long)R.hdr->last_frame);
            last_report = t;
            last_report_frames = nshown;
        }
    }
    SDL_Quit();
    return 0;
}
