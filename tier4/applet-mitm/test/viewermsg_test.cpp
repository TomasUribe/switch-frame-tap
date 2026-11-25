/* v0.7.20: the viewer's messages (applet_mitm_viewermsg.hpp) as the USB
 * reader takes them (whole transfers) and as the network thread does (a byte
 * stream split anywhere, with a carry buffer). */
#include "applet_mitm_viewermsg.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vm = sft::viewermsg;

static std::vector<uint8_t> Pad(uint32_t flags, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    vm::PadMsg m;
    std::memcpy(m.magic, "SFTI", 4);
    m.flags = flags; m.buttons = buttons; m.lx = lx; m.ly = ly; m.rx = rx; m.ry = ry;
    std::vector<uint8_t> v(sizeof(m));
    std::memcpy(v.data(), &m, sizeof(m));
    return v;
}

/* every message in one buffer, as the USB reader does: kinds as letters */
static std::string Whole(const std::vector<uint8_t> &b, std::vector<vm::PadMsg> *pads = nullptr) {
    std::string out;
    for (size_t k = 0; k < b.size(); ) {
        const vm::Msg m = vm::Next(b.data() + k, b.size() - k);
        if (m.kind == vm::Kind::Partial) { out += '~'; break; }
        switch (m.kind) {
            case vm::Kind::Key: out += 'K'; break;
            case vm::Kind::Crop: out += 'C'; break;
            case vm::Kind::NoCrop: out += 'D'; break;
            case vm::Kind::Lock: out += 'L'; break;
            case vm::Kind::Unlock: out += 'U'; break;
            case vm::Kind::Bye: out += 'Q'; break;
            case vm::Kind::Report: out += 'G'; break;
            case vm::Kind::Pad: out += 'I'; if (pads) pads->push_back(vm::ReadPad(b.data() + k)); break;
            default: break;
        }
        k += m.len;
    }
    return out;
}

/* the network thread's loop, fed in pieces of `step` bytes */
static std::string Stream(const std::vector<uint8_t> &b, size_t step, std::vector<vm::PadMsg> *pads) {
    std::string out;
    uint8_t got[96];
    size_t have = 0;
    for (size_t at = 0; at < b.size(); ) {
        const size_t n = std::min(step, std::min(sizeof(got) - have, b.size() - at));
        std::memcpy(got + have, b.data() + at, n);
        at += n;
        have += n;
        size_t k = 0;
        while (k < have) {
            const vm::Msg m = vm::Next(got + k, have - k);
            if (m.kind == vm::Kind::Partial) { break; }
            if (m.kind == vm::Kind::Pad) { out += 'I'; pads->push_back(vm::ReadPad(got + k)); }
            else if (m.kind == vm::Kind::Key) { out += 'K'; }
            else if (m.kind == vm::Kind::Unlock) { out += 'U'; }
            else if (m.kind == vm::Kind::Crop) { out += 'C'; }
            k += m.len;
        }
        std::memmove(got, got + k, have - k);
        have -= k;
        assert(have < sizeof(vm::PadMsg));   /* the carry never fills the buffer */
    }
    return out;
}

static std::vector<uint8_t> Bytes(const char *s) { return std::vector<uint8_t>(s, s + std::strlen(s)); }

static void Append(std::vector<uint8_t> &a, const std::vector<uint8_t> &b) { a.insert(a.end(), b.begin(), b.end()); }

int main() {
    /* the old words, as v0.7.13-0.7.19 viewers send them */
    assert(Whole(Bytes("SFTKSFTCSFTU")) == "KCU");
    assert(Whole(Bytes("SFTKSFTDSFTL")) == "KDL");
    assert(Whole(Bytes("SFTQ")) == "Q");
    /* v0.7.24: Report a problem, alone and after a keyframe request */
    assert(Whole(Bytes("SFTG")) == "G");
    assert(Whole(Bytes("SFTKSFTCSFTUSFTG")) == "KCUG");
    /* garbage is skipped a byte at a time, and a word after it still counts */
    assert(Whole(Bytes("xxSFTKzSFTQ")) == "KQ");
    assert(Whole(Bytes("SFTZSFTK")) == "K");
    /* a short tail is not a message */
    assert(Whole(Bytes("SFTKSF")) == "K~");

    /* a pad message, whole */
    std::vector<vm::PadMsg> pads;
    auto p1 = Pad(vm::PadOn, (1u << 0) | (1u << 18), 32767, -32767, 100, -100);
    assert(Whole(p1, &pads) == "I" && pads.size() == 1);
    assert(pads[0].flags == vm::PadOn && pads[0].buttons == ((1u << 0) | (1u << 18)));
    assert(pads[0].lx == 32767 && pads[0].ly == -32767 && pads[0].rx == 100 && pads[0].ry == -100);
    /* cut short: waits */
    std::vector<uint8_t> cut(p1.begin(), p1.begin() + 20);
    assert(Whole(cut) == "~");
    /* stick values never look like a word (they stay within +-32767) */
    auto p2 = Pad(vm::PadOn, 0, -32767, 32767, -1, 1);
    pads.clear();
    std::vector<uint8_t> mixed = Bytes("SFTK");
    Append(mixed, p1); Append(mixed, Bytes("SFTU")); Append(mixed, p2); Append(mixed, Bytes("SFTC"));
    assert(Whole(mixed, &pads) == "KIUIC" && pads.size() == 2 && pads[1].lx == -32767 && pads[1].ry == 1);

    /* the network: the same bytes split at every size from 1 to 96 */
    for (size_t step = 1; step <= 96; ++step) {
        std::vector<vm::PadMsg> sp;
        std::vector<uint8_t> many;
        for (int i = 0; i < 20; ++i) { Append(many, Pad(vm::PadOn, static_cast<uint64_t>(i), i, -i, 2 * i, -2 * i)); if (i % 3 == 0) Append(many, Bytes("SFTK")); }
        const std::string got = Stream(many, step, &sp);
        assert(sp.size() == 20);
        for (int i = 0; i < 20; ++i) { assert(sp[i].buttons == static_cast<uint64_t>(i) && sp[i].lx == i && sp[i].ry == -2 * i); }
        assert(std::count(got.begin(), got.end(), 'K') == 7);
    }

    /* the buttons a viewer may press, and the stick range */
    assert((vm::PadButtons & (1ull << 16)) == 0 && (vm::PadButtons & (1ull << 18)) != 0 && (vm::PadButtons & (1ull << 19)) != 0);
    assert(vm::ClampStick(40000) == 32767 && vm::ClampStick(-40000) == -32767 && vm::ClampStick(5) == 5);

    std::printf("OK\n");
    return 0;
}
