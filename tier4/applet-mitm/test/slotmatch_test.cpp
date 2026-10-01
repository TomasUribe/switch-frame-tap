/*
 * Host test for applet_mitm_slotmatch.hpp: a game with one buffer per
 * swapchain slot at shuffled addresses, among other buffers the same size (a
 * render target drawn every frame, one never touched), presents frames the
 * way a GPU finishes them; the matcher has to find which buffer is which slot
 * - and must never settle on a wrong match.
 *
 *   g++ -std=c++20 -Wall -Wextra -I../source slotmatch_test.cpp -o slotmatch_test && ./slotmatch_test
 */
#include "applet_mitm_slotmatch.hpp"
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <random>

namespace sm = ams::mitm::applet::slotmatch;

static int g_fail = 0;

struct Game {
    uint32_t nslots, ncand;
    uint32_t perm[sm::MaxSlots];         /* slot -> candidate */
    int rt = -1, still_buf = -1;         /* a render target drawn every frame, a buffer never drawn */
    uint32_t content[sm::MaxCand] = {};
    uint32_t picture = 0;                /* what the game draws; equal frames = a still picture */
    uint32_t next_value = 1;

    uint32_t Draw(bool still) { if (!still) { picture = next_value++; } return picture; }
};

struct Opts {
    uint32_t nslots, ncand;
    bool rt, still_buf;
    double early;      /* chance the next slot is already partly drawn when we sample */
    double skip;       /* chance the capture misses a present */
    double still;      /* chance a frame repeats the previous picture */
    uint32_t frames;
};

/* frames until a match (0 = none); *wrong set if the match is not the truth */
static uint32_t Run(const Opts &o, uint32_t seed, bool *wrong) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    Game g{ o.nslots, o.ncand, {} };
    uint32_t order[sm::MaxCand];
    for (uint32_t c = 0; c < o.ncand; ++c) { order[c] = c; }
    std::shuffle(order, order + o.ncand, rng);
    for (uint32_t s = 0; s < o.nslots; ++s) { g.perm[s] = order[s]; }
    uint32_t k = o.nslots;
    if (o.rt) { g.rt = static_cast<int>(order[k++]); }
    if (o.still_buf) { g.still_buf = static_cast<int>(order[k++]); }

    sm::Matcher m;
    m.Reset(o.nslots, o.ncand);
    *wrong = false;
    for (uint32_t n = 1; n <= o.frames; ++n) {
        const uint32_t slot = n % o.nslots;
        const bool still = u(rng) < o.still;
        g.content[g.perm[slot]] = g.Draw(still);                     /* the GPU finished present n */
        if (g.rt >= 0) { g.content[g.rt] = 0x80000000u | n; }       /* drawn every frame regardless */
        if (u(rng) < o.early) {                                      /* the next frame already started */
            g.content[g.perm[(slot + 1) % o.nslots]] ^= 0x40000000u | n;
        }
        if (u(rng) < o.skip) { continue; }                           /* the capture never looked at present n */
        uint32_t sig[sm::MaxCand];
        for (uint32_t c = 0; c < o.ncand; ++c) { sig[c] = g.content[c] * 2654435761u + c; }
        m.Add(n, slot, sig);
        uint8_t out[sm::MaxSlots];
        int32_t weakest = 0;
        if (m.Match(out, &weakest)) {
            for (uint32_t s = 0; s < o.nslots; ++s) { if (out[s] != g.perm[s]) { *wrong = true; } }
            return n;
        }
    }
    return 0;
}

static void Expect(const char *what, const Opts &o, uint32_t max_frames, uint32_t seeds = 200) {
    uint32_t worst = 0, failed = 0, wrong = 0;
    for (uint32_t seed = 1; seed <= seeds; ++seed) {
        bool w = false;
        const uint32_t n = Run(o, seed, &w);
        if (w) { ++wrong; }
        if (n == 0 || n > max_frames) { ++failed; }
        if (n > worst) { worst = n; }
    }
    if (wrong != 0 || failed != 0) {
        std::printf("FAIL %s: %u wrong match(es), %u run(s) not matched within %u frames (of %u)\n", what, wrong, failed, max_frames, seeds);
        ++g_fail;
    } else {
        std::printf("ok   %s: matched within %u frames\n", what, worst);
    }
}

static void ExpectNoMatch(const char *what, const Opts &o) {
    for (uint32_t seed = 1; seed <= 50; ++seed) {
        bool w = false;
        if (Run(o, seed, &w) != 0) { std::printf("FAIL %s: matched (seed %u)\n", what, seed); ++g_fail; return; }
    }
    std::printf("ok   %s: no match\n", what);
}

int main() {
    /* Smash Ultimate: three slots, three buffers, nothing else */
    Expect("3 slots, 3 buffers",                    { 3, 3,  false, false, 0.0,  0.0,  0.0,  600 }, 60);
    Expect("3 slots among 6 buffers",               { 3, 6,  true,  true,  0.0,  0.0,  0.0,  600 }, 60);
    Expect("+ next slot drawn early 30%",           { 3, 6,  true,  true,  0.30, 0.0,  0.0,  1200 }, 300);
    Expect("+ 20% presents missed",                 { 3, 6,  true,  true,  0.10, 0.20, 0.0,  1200 }, 300);
    Expect("+ 40% still frames",                    { 3, 6,  true,  true,  0.10, 0.10, 0.40, 2000 }, 600);
    Expect("double-buffered game",                  { 2, 4,  true,  true,  0.10, 0.10, 0.0,  1200 }, 300);
    Expect("12 candidates",                         { 3, 12, true,  true,  0.10, 0.10, 0.10, 1200 }, 300);
    /* a picture that never moves says nothing about which buffer is which */
    ExpectNoMatch("still picture",                  { 3, 6,  true,  true,  0.0,  0.0,  1.0,  2000 });
    /* the next slot drawn early EVERY time: every slot's buffer and the
     * next one change together - ambiguous, so no match rather than a wrong one */
    {
        uint32_t wrong = 0;
        for (uint32_t seed = 1; seed <= 200; ++seed) { bool w = false; Run({ 3, 6, true, true, 1.0, 0.0, 0.0, 2000 }, seed, &w); wrong += w; }
        if (wrong) { std::printf("FAIL always-early: %u wrong match(es)\n", wrong); ++g_fail; }
        else { std::printf("ok   always-early: never a wrong match\n"); }
    }
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
