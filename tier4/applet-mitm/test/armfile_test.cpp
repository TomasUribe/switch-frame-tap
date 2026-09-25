/*
 * Host test for applet_mitm_armfile.hpp - the arm file decides which probes
 * touch hardware, so a parsing bug is a hardware bug. Run before every build
 * that changes flags:
 *
 *   g++ -std=c++20 -Wall -Wextra -I../source armfile_test.cpp -o armfile_test && ./armfile_test
 */
#include "applet_mitm_armfile.hpp"
#include <cstdio>
#include <cstring>

namespace af = ams::mitm::applet::armfile;

static int g_fail = 0;

static void ExpectContains(const char *file, const char *key, bool want) {
    const bool got = af::Contains(file, std::strlen(file), key);
    if (got != want) { std::printf("FAIL Contains(\"%s\", \"%s\") = %d, want %d\n", file, key, got, want); ++g_fail; }
}

static void ExpectNumber(const char *file, const char *key, unsigned def, unsigned want) {
    const unsigned got = af::Number(file, std::strlen(file), key, def);
    if (got != want) { std::printf("FAIL Number(\"%s\", \"%s\", %u) = %u, want %u\n", file, key, def, got, want); ++g_fail; }
}

int main() {
    /* M75's failure, and its whole class */
    ExpectContains("vic grcscan wait=90", "grc", false);
    ExpectContains("vic grcscan wait=90", "grcscan", true);
    ExpectContains("vic grcscan wait=90", "vic", true);
    ExpectContains("vic grcscan wait=90", "wait", true);
    ExpectContains("jpgdec", "jpg", false);
    /* M80: "nvgrc" must never arm the grc IPC interceptor */
    ExpectContains("vic nvgrc wait=60", "grc", false);
    ExpectContains("vic nvgrc wait=60", "nvgrc", true);
    ExpectContains("vic nvgrc wait=60", "nvenc", false);
    /* M81: Run G's exact arm file */
    ExpectContains("vic nvgrc grcscan wait=60\n", "grc", false);
    ExpectContains("vic nvgrc grcscan wait=60\n", "nvgrc", true);
    ExpectContains("vic nvgrc grcscan wait=60\n", "grcscan", true);
    ExpectContains("vic nvgrc grcscan wait=60\n", "jpgdec", false);
    ExpectNumber("vic nvgrc grcscan wait=60\n", "wait", 120, 60);
    /* M82: Run H's exact arm file */
    ExpectContains("vic exec dbg csc nvframe wait=60\n", "grc", false);
    ExpectContains("vic exec dbg csc nvframe wait=60\n", "nvframe", true);
    ExpectContains("vic exec dbg csc nvframe wait=60\n", "csc", true);
    ExpectContains("vic exec dbg csc nvframe wait=60\n", "exec", true);
    ExpectContains("vic exec dbg csc nvframe wait=60\n", "dbg", true);
    ExpectContains("vic exec dbg csc nvframe wait=60\n", "nvenc", false);
    ExpectContains("vic exec dbg csc nvframe wait=60\n", "stream", false);
    ExpectContains("vic exec dbg csc nvframe wait=60\n", "dump", false);
    ExpectNumber("vic exec dbg csc nvframe wait=60\n", "nvframe", 120, 120);
    ExpectNumber("vic exec dbg csc nvframe=60 wait=60\n", "nvframe", 120, 60);
    ExpectContains("jpg jpgdec", "jpg", true);
    ExpectContains("clk\n", "clk", true);
    ExpectContains("vic\r\nclk\r\n", "clk", true);
    ExpectContains("", "vic", false);
    ExpectContains("vic", "", false);

    /* M76: the same class in ArmFileNumber */
    ExpectNumber("sweep stream sw=768 sh=432", "sw", 0, 768);
    ExpectNumber("sweep stream sw=768 sh=432", "sh", 0, 432);
    ExpectNumber("sweep stream", "sw", 0, 0);
    ExpectNumber("vic grcscan wait=90", "wait", 120, 90);
    ExpectNumber("vic grcscan", "wait", 120, 120);
    ExpectNumber("sframes=600 q=70", "q", 85, 70);
    ExpectNumber("sequence=3 q=70", "q", 85, 70);
    ExpectNumber("mtx", "mtx", 1, 1);
    ExpectNumber("mtx=2", "mtx", 1, 2);
    ExpectNumber("wait=", "wait", 120, 120);
    ExpectNumber("wait=9x", "wait", 120, 120);
    ExpectNumber("clk=5", "clk", 0, 5);

    /* a NUL inside the buffer ends parsing, as the old C-string code did */
    {
        const char buf[] = { 'v', 'i', 'c', '\0', 'c', 'l', 'k' };
        if (af::Contains(buf, sizeof(buf), "clk")) { std::printf("FAIL: token after NUL was parsed\n"); ++g_fail; }
        if (!af::Contains(buf, sizeof(buf), "vic")) { std::printf("FAIL: token before NUL missed\n"); ++g_fail; }
    }

    std::printf("%s (%d failure%s)\n", g_fail == 0 ? "OK" : "FAILED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
