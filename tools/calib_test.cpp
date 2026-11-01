// vrdual-calib-test: guided IPD calibration (common/vrdual_calib.hpp) driven
// by a simulated viewer with a known true IPD (far) and near IPD. Every
// target from 50 to 80 mm in 0.1 mm steps, a viewer who says "right" within
// a tolerance and a picky one who never does: the result must be within
// 0.5 mm, a near IPD equal to the far one must come out as off. Exit 0 = pass.
#include <cmath>
#include <cstdio>

#include "../common/vrdual_calib.hpp"

using vrdual::GuidedCalib;

static GuidedCalib::Answer viewer(float probe, float truth, float tol) {
    // rendered IPD above the true one: the world looks smaller, i.e. nearer
    if (probe > truth + tol) return GuidedCalib::Nearer;
    if (probe < truth - tol) return GuidedCalib::Farther;
    return GuidedCalib::Right;
}

int main() {
    int bad = 0, runs = 0, most = 0;
    for (int t10 = 500; t10 <= 800; ++t10) {
        float far_ = t10 * 1e-4f;
        for (float dn : {0.0f, 0.003f, -0.005f, 0.0075f})
            for (float tol : {0.0002f, 0.0f}) {
                GuidedCalib g;
                g.start();
                int n = 0;
                while (g.active() && n < 50) {
                    float p = g.phase == GuidedCalib::Far ? g.show_ipd() : g.show_ipd_near();
                    float truth = g.phase == GuidedCalib::Far ? far_ : g.ipd + dn;  // near: relative to what phase far found
                    g.answer(viewer(p, truth, tol)), ++n;
                }
                ++runs;
                most = n > most ? n : most;
                bool ok = g.phase == GuidedCalib::Done && std::fabs(g.ipd - far_) <= 0.0005f;
                if (dn == 0 && tol > 0) ok = ok && g.ipd_near == 0;  // right at the first near probe: off
                else if (dn != 0) ok = ok && std::fabs(g.ipd_near - (g.ipd + dn)) <= 0.0005f;
                if (!ok && bad++ < 10)
                    fprintf(stderr, "FAIL far %.1f mm near %+.1f tol %.1f: ipd %.2f near %.2f after %d answers\n", far_ * 1e3, dn * 1e3, tol * 1e3,
                            g.ipd * 1e3, g.ipd_near * 1e3, n);
            }
    }
    if (most > 12) fprintf(stderr, "FAIL up to %d answers (expected <= 12)\n", most), ++bad;
    printf("%d runs, at most %d answers, %d bad\n", runs, most, bad);
    return bad ? 1 : 0;
}
