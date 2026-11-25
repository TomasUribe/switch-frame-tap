/* v0.7.20: what a viewer sends the console - over USB, one bulk transfer at a
 * time; over the network, a byte stream that can split a message anywhere.
 *
 *   "SFTK"            a keyframe, please (v0.7.13)
 *   "SFTC" / "SFTD"   it draws crops itself / it records: crop here (v0.7.16, v0.7.17)
 *   "SFTL" / "SFTU"   one stream size while it records / follow the game (v0.7.18)
 *   "SFTQ"            goodbye (v0.7.12, USB)
 *   "SFTI" + 28 bytes the controller on the viewer's computer (PadMsg, v0.7.20)
 *   "SFTG"            the log, please - Report a problem (v0.7.24)
 *
 * Anything else is skipped a byte at a time, as the scans before did. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sft::viewermsg {

    /* little-endian on the wire, as every SFT header */
    struct PadMsg {
        char magic[4];             /* "SFTI" */
        uint32_t flags;            /* PadOn: a controller is there; 0 = let the virtual one go */
        uint64_t buttons;          /* HidNpadButton bits, plus bit 18 HOME and bit 19 Capture */
        int32_t lx, ly, rx, ry;    /* -32767..32767, up is +y */
    };
    static_assert(sizeof(PadMsg) == 32, "PadMsg is 32 bytes on the wire");

    constexpr uint32_t PadOn = 1;

    /* the buttons a viewer may press: A..Down (bits 0-15), HOME and Capture.
     * The stick directions (bits 16-17, 20-27) come from the sticks. */
    constexpr uint64_t PadButtons = 0xFFFFull | (1ull << 18) | (1ull << 19);

    enum class Kind { Skip, Partial, Key, Crop, NoCrop, Lock, Unlock, Bye, Pad, Report };

    struct Msg { Kind kind; size_t len; };

    /* the message that starts at p (n bytes there): Partial when it is a
     * PadMsg not all here yet (the network: wait for the rest) */
    inline Msg Next(const uint8_t *p, size_t n) {
        if (n < 4) { return { Kind::Partial, 0 }; }
        if (std::memcmp(p, "SFT", 3) != 0) { return { Kind::Skip, 1 }; }
        switch (p[3]) {
            case 'K': return { Kind::Key, 4 };
            case 'C': return { Kind::Crop, 4 };
            case 'D': return { Kind::NoCrop, 4 };
            case 'L': return { Kind::Lock, 4 };
            case 'U': return { Kind::Unlock, 4 };
            case 'Q': return { Kind::Bye, 4 };
            case 'G': return { Kind::Report, 4 };
            case 'I': return n < sizeof(PadMsg) ? Msg{ Kind::Partial, 0 } : Msg{ Kind::Pad, sizeof(PadMsg) };
            default:  return { Kind::Skip, 1 };
        }
    }

    inline PadMsg ReadPad(const uint8_t *p) {
        PadMsg m;
        std::memcpy(&m, p, sizeof(m));
        return m;
    }

    inline int32_t ClampStick(int32_t v) { return v < -32767 ? -32767 : v > 32767 ? 32767 : v; }

}
