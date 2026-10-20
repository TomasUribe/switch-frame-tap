/*
 * Host test for common/capture_mode.hpp - the capture.ini parser and mode
 * resolution. Run before a build that changes per-title capture:
 *
 *   g++ -std=c++20 -Wall -Wextra -I../../../common capture_mode_test.cpp -o capture_mode_test && ./capture_mode_test
 */
#include "capture_mode.hpp"
#include <cstdio>
#include <cstring>

namespace sc = sftcapture;
static int g_fail = 0;

static void Expect(bool ok, const char *what) {
    if (!ok) { std::printf("FAIL %s\n", what); ++g_fail; }
}

static void ExpectMode(const char *text, bool want_ok, uint32_t want) {
    uint32_t got = 99;
    const bool ok = sc::Parse(text, std::strlen(text), &got);
    if (ok != want_ok || (ok && got != want)) { std::printf("FAIL Parse(\"%s\") -> %d/%u, want %d/%u\n", text, ok, got, want_ok, want); ++g_fail; }
}

int main() {
    ExpectMode("auto", true, sc::Auto);
    ExpectMode("standard", true, sc::Standard);
    ExpectMode("composite", true, sc::Composite);
    ExpectMode("layer", true, sc::Layer);
    ExpectMode("AUTO", false, 0);
    ExpectMode("std", false, 0);

    uint64_t tid = 0;
    Expect(sc::ParseTid("01006c300e9f0000", 16, &tid) && tid == UINT64_C(0x01006c300e9f0000), "lowercase title id");
    Expect(sc::ParseTid("01006C300E9F0000", 16, &tid) && tid == UINT64_C(0x01006c300e9f0000), "uppercase title id");
    Expect(sc::ParseTid("0x01006c300e9f0000", 18, &tid) && tid == UINT64_C(0x01006c300e9f0000), "0x title id");
    Expect(!sc::ParseTid("01006c300e9f000", 15, &tid), "short title id");
    Expect(!sc::ParseTid("01006c300e9f000g", 16, &tid), "bad hex title id");
    Expect(!sc::ParseTid("0000000000000000", 16, &tid), "zero title id");

    const char *ini =
        "# overrides\n"
        "01006c300e9f0000 = composite\n"
        "; a comment\n"
        "  0100000000001000=layer  \n"
        "[capture]\n"
        "01006c300e9f0000 = auto\n"
        "not-a-title = layer\n"
        "0100000000001001 = nonsense\n";
    sc::Override ov[sc::MaxOverrides] = {};
    const size_t n = sc::ParseIni(ini, std::strlen(ini), ov, sc::MaxOverrides);
    Expect(n == 2, "two valid override lines");
    Expect(sc::Resolve(UINT64_C(0x01006c300e9f0000), sc::Auto, ov, n) == sc::Auto, "last line wins");
    Expect(sc::Resolve(UINT64_C(0x0100000000001000), sc::Auto, ov, n) == sc::Layer, "spaces trimmed");
    Expect(sc::Resolve(UINT64_C(0x0100000000001001), sc::Auto, ov, n) == sc::Auto, "bad value ignored");
    Expect(sc::Resolve(UINT64_C(0x0100000000001002), sc::Auto, ov, n) == sc::Auto, "missing title uses default");

    char line[64];
    const size_t m = sc::FormatLine(line, sizeof(line), UINT64_C(0x01006c300e9f0000), sc::Layer);
    Expect(m != 0 && std::strcmp(line, "01006c300e9f0000 = layer\n") == 0, "format line");

    sc::Override many[2] = {};
    const size_t k = sc::ParseIni("0100000000000001=standard\n0100000000000002=composite\n", 74, many, 2);
    Expect(k == 2, "capacity");

    {
        uint32_t changed[3] = { 40, 0, 0 };
        uint32_t seen[3] = { 60, 60, 60 };
        uint32_t live = 0, dead = 0, best = 9;
        sc::ClassifyCandidates(changed, seen, 3, 60, &live, &dead, &best);
        Expect(live == 1 && dead == 2 && best == 0, "classify DQXI: one live, two static");
        Expect(sc::IsMultiSurface(live, dead), "DQXI is multi-surface");

        changed[0] = changed[1] = changed[2] = 30;
        sc::ClassifyCandidates(changed, seen, 3, 60, &live, &dead, &best);
        Expect(live == 3 && dead == 0, "classify triple buffer: three live");
        Expect(!sc::IsMultiSurface(live, dead), "triple buffer stays standard");

        changed[0] = changed[1] = changed[2] = 0;
        sc::ClassifyCandidates(changed, seen, 3, 60, &live, &dead, &best);
        Expect(live == 0 && dead == 3, "classify still picture: none live");
        Expect(!sc::IsMultiSurface(live, dead), "still picture stays standard");

        seen[2] = 1;
        changed[0] = 40; changed[1] = 0; changed[2] = 40;
        sc::ClassifyCandidates(changed, seen, 3, 60, &live, &dead, &best);
        Expect(live == 1 && dead == 1, "a candidate not sampled enough is ignored");
        Expect(!sc::IsMultiSurface(live, dead), "one live and one dead is not enough");
    }

    if (g_fail == 0) { std::printf("capture_mode tests passed\n"); }
    return g_fail == 0 ? 0 : 1;
}
