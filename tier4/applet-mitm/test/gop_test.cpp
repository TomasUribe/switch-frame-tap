// The keyframe spacing the stream loop picks (applet_mitm_gop.hpp): USB gets
// usb_keyframe_interval only when it is longer than keyframe_interval, never
// the webcam, never a test run; 0 turns it off; the network is as before.
#include "applet_mitm_gop.hpp"
#include <cstdio>

namespace kf = ams::mitm::applet::keyframe;
static int g_fail = 0;
static void Expect(unsigned got, unsigned want, const char *what) {
    if (got != want) { std::printf("FAIL %s: %u, want %u\n", what, got, want); ++g_fail; }
}

int main() {
    //                 net    live   uvc    gop  ask    wired  usbgop
    Expect(kf::Choose(false, true,  false, 60,  false, false, 240), 240, "USB live: the long interval");
    Expect(kf::Choose(false, true,  false, 60,  true,  false, 240), 240, "USB live, viewer can ask: the long interval");
    Expect(kf::Choose(false, true,  false, 60,  false, false, 0),   60,  "usb_keyframe_interval = 0: as set");
    Expect(kf::Choose(false, true,  false, 60,  false, false, 30),  60,  "never shorter than the setting");
    Expect(kf::Choose(false, true,  false, 250, false, false, 240), 250, "keyframe_interval longer: as set");
    Expect(kf::Choose(false, true,  false, 60,  false, false, 999), 250, "at most 250");
    Expect(kf::Choose(false, true,  false, 0,   false, false, 240), 0,   "keyframe_interval 0 (all IDR) stays");
    Expect(kf::Choose(false, true,  true,  60,  false, false, 240), 60,  "the webcam: as set");
    Expect(kf::Choose(false, false, false, 60,  false, false, 240), 60,  "not live (a test run): as set");
    Expect(kf::Choose(true,  true,  false, 60,  true,  false, 0),   240, "network Wi-Fi, viewer can ask (v0.7.13)");
    Expect(kf::Choose(true,  true,  false, 60,  true,  true,  0),   60,  "network wired (exp/net-ethernet)");
    Expect(kf::Choose(true,  true,  false, 60,  false, false, 240), 60,  "network, viewer never asked: as set (usbgop ignored)");
    std::printf("%s (%d failure%s)\n", g_fail == 0 ? "keyframe spacing OK" : "FAILED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
