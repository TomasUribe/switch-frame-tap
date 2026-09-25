#!/usr/bin/env python3
"""
nvenc_replay.py - M80: replay grc's own NVENC IDR job from our module, and
check what comes back.

M79 Run E read out of grc (the system recorder) both halves of a working
NVENC job on this firmware: the per-frame setup (an IDR one, pic_type 3,
frame_num 0) and the command buffer that submits it. M80 submits that job
from our own channel with our own buffers. The setup contains no addresses -
every surface is bound by a method - so grc's bytes are used verbatim.

  gen        check the captured IDR setup and write
             tier4/applet-mitm/source/nvenc_grc_idr.h
  selftest   prove the SPS/PPS writer (rewrites libx264's headers byte for
             byte from parsed fields) and the decode + band check (on an
             x264 encode of the same stripe input)
  check DIR  read the files the console writes (nvenc-status.bin,
             nvenc-bits.bin, nvenc-recon-y.bin, as copied off the card into
             DIR), print the status, list the NAL units, add SPS/PPS built
             from grc's setup if the stream has none, decode with PyAV and
             compare the picture with the input stripes

The input the console encodes is horizontal stripes, 32 rows each, luma
32 + 8*k for stripe k, chroma 128. A 32-row stripe is one contiguous byte
range in both pitch-linear and 32-row block-linear layouts, which is why the
console can write it without knowing the exact swizzle.
"""
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SETUP = REPO / "logs/m79-runE-setups/setup_8.bin"
HEADER = REPO / "tier4/applet-mitm/source/nvenc_grc_idr.h"

STRIPE_ROWS = 32


def stripe_luma(k):
    return min(32 + 8 * k, 235)


# ---------------------------------------------------------------- the setup

def parse_setup(b):
    """The fields of grc's nvenc_h264_drv_pic_setup_s this tool needs. Offsets
    and bit positions are NVIDIA's (tier4/applet-mitm/source/nvenc_drv_h264.h),
    checked against M78/M79 dumps decoded by tools/nvsetup-dump."""
    magic, = struct.unpack_from("<I", b, 0)
    w1, h1 = struct.unpack_from("<HH", b, 4)
    sps, = struct.unpack_from("<I", b, 0x64)
    pps0, pps1 = struct.unpack_from("<II", b, 0x68)
    pc0, = struct.unpack_from("<I", b, 0xC8)
    frame_num, poc_lsb, idr_pic_id = struct.unpack_from("<HHH", b, 0x17C)

    def sx(v, bits):
        return v - (1 << bits) if v & (1 << (bits - 1)) else v
    return dict(
        magic=magic, width=w1 + 1, height=h1 + 1,
        profile_idc=sps & 0xFF, level_idc=(sps >> 8) & 0xFF,
        chroma_format_idc=(sps >> 16) & 3, pic_order_cnt_type=(sps >> 18) & 3,
        log2_max_frame_num_minus4=(sps >> 20) & 0xF,
        log2_max_poc_lsb_minus4=(sps >> 24) & 0xF, frame_mbs_only=(sps >> 28) & 1,
        pps_id=pps0 & 0xFF, entropy=(pps0 >> 8) & 1,
        num_ref_idx_l0_minus1=(pps0 >> 9) & 0x1F, num_ref_idx_l1_minus1=(pps0 >> 14) & 0x1F,
        weighted_bipred_idc=(pps0 >> 19) & 3, pic_init_qp_minus26=sx((pps0 >> 21) & 0x3F, 6),
        chroma_qp_index_offset=sx((pps0 >> 27) & 0x1F, 5),
        second_chroma_qp_index_offset=sx(pps1 & 0x1F, 5), constrained_intra=(pps1 >> 5) & 1,
        deblocking_present=(pps1 >> 6) & 1, transform_8x8=(pps1 >> 7) & 1,
        pic_order_present=(pps1 >> 8) & 1, weighted_pred=(pps1 >> 9) & 1,
        pic_type=(pc0 >> 2) & 3, ref_pic_flag=(pc0 >> 4) & 1,
        frame_num=frame_num, poc_lsb=poc_lsb, idr_pic_id=idr_pic_id,
    )


# ------------------------------------------------------------- bit writer

class BitW:
    def __init__(self):
        self.bits = []

    def u(self, n, v):
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def ue(self, v):
        v += 1
        n = v.bit_length()
        self.u(n - 1, 0)
        self.u(n, v)

    def se(self, v):
        self.ue(2 * v - 1 if v > 0 else -2 * v)

    def raw(self, bits):
        self.bits.extend(bits)

    def trailing(self):
        self.bits.append(1)
        while len(self.bits) % 8:
            self.bits.append(0)

    def bytes(self):
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            v = 0
            for b in self.bits[i:i + 8]:
                v = (v << 1) | b
            out.append(v)
        return bytes(out)


class BitR:
    def __init__(self, data):
        self.bits = [(byte >> (7 - i)) & 1 for byte in data for i in range(8)]
        self.pos = 0

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.bits[self.pos]
            self.pos += 1
        return v

    def ue(self):
        z = 0
        while self.u(1) == 0:
            z += 1
        return (1 << z) - 1 + self.u(z)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)

    def rest_before_trailing(self):
        """Everything after pos up to (not including) the rbsp stop bit."""
        last = len(self.bits) - 1
        while self.bits[last] == 0:
            last -= 1
        return self.bits[self.pos:last]


def ep_remove(nal):
    out = bytearray()
    z = 0
    for b in nal:
        if z >= 2 and b == 3:
            z = 0
            continue
        out.append(b)
        z = z + 1 if b == 0 else 0
    return bytes(out)


def ep_add(rbsp):
    out = bytearray()
    z = 0
    for b in rbsp:
        if z >= 2 and b <= 3:
            out.append(3)
            z = 0
        out.append(b)
        z = z + 1 if b == 0 else 0
    return bytes(out)


HIGH_PROFILES = (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135)


def sps_parse(rbsp):
    r = BitR(rbsp)
    f = dict(profile_idc=r.u(8), constraint=r.u(8), level_idc=r.u(8), sps_id=r.ue())
    if f["profile_idc"] in HIGH_PROFILES:
        f["chroma_format_idc"] = r.ue()
        assert f["chroma_format_idc"] != 3
        f["bit_depth_luma_minus8"] = r.ue()
        f["bit_depth_chroma_minus8"] = r.ue()
        f["qpprime_bypass"] = r.u(1)
        f["scaling_matrix"] = r.u(1)
        assert f["scaling_matrix"] == 0
    f["log2_max_frame_num_minus4"] = r.ue()
    f["pic_order_cnt_type"] = r.ue()
    if f["pic_order_cnt_type"] == 0:
        f["log2_max_poc_lsb_minus4"] = r.ue()
    assert f["pic_order_cnt_type"] != 1
    f["max_num_ref_frames"] = r.ue()
    f["gaps"] = r.u(1)
    f["width_mbs_minus1"] = r.ue()
    f["height_map_units_minus1"] = r.ue()
    f["frame_mbs_only"] = r.u(1)
    if not f["frame_mbs_only"]:
        f["mb_adaptive"] = r.u(1)
    f["direct_8x8_inference"] = r.u(1)
    f["cropping"] = r.u(1)
    if f["cropping"]:
        f["crop"] = [r.ue() for _ in range(4)]
    f["vui_present"] = r.u(1)
    f["vui_bits"] = r.rest_before_trailing() if f["vui_present"] else []
    return f


def sps_write(f):
    w = BitW()
    w.u(8, f["profile_idc"]); w.u(8, f["constraint"]); w.u(8, f["level_idc"]); w.ue(f["sps_id"])
    if f["profile_idc"] in HIGH_PROFILES:
        w.ue(f["chroma_format_idc"]); w.ue(f["bit_depth_luma_minus8"]); w.ue(f["bit_depth_chroma_minus8"])
        w.u(1, f["qpprime_bypass"]); w.u(1, 0)
    w.ue(f["log2_max_frame_num_minus4"]); w.ue(f["pic_order_cnt_type"])
    if f["pic_order_cnt_type"] == 0:
        w.ue(f["log2_max_poc_lsb_minus4"])
    w.ue(f["max_num_ref_frames"]); w.u(1, f["gaps"])
    w.ue(f["width_mbs_minus1"]); w.ue(f["height_map_units_minus1"])
    w.u(1, f["frame_mbs_only"])
    if not f["frame_mbs_only"]:
        w.u(1, f["mb_adaptive"])
    w.u(1, f["direct_8x8_inference"]); w.u(1, f["cropping"])
    if f["cropping"]:
        for c in f["crop"]:
            w.ue(c)
    w.u(1, f["vui_present"])
    w.raw(f["vui_bits"])
    w.trailing()
    return w.bytes()


def pps_parse(rbsp):
    r = BitR(rbsp)
    f = dict(pps_id=r.ue(), sps_id=r.ue(), entropy=r.u(1), pic_order_present=r.u(1),
             num_slice_groups_minus1=r.ue())
    assert f["num_slice_groups_minus1"] == 0
    f.update(num_ref_idx_l0_minus1=r.ue(), num_ref_idx_l1_minus1=r.ue(), weighted_pred=r.u(1),
             weighted_bipred_idc=r.u(2), pic_init_qp_minus26=r.se(), pic_init_qs_minus26=r.se(),
             chroma_qp_index_offset=r.se(), deblocking_present=r.u(1), constrained_intra=r.u(1),
             redundant_pic_cnt=r.u(1))
    f["ext"] = r.rest_before_trailing() != []
    if f["ext"]:
        f["transform_8x8"] = r.u(1)
        f["pic_scaling"] = r.u(1)
        assert f["pic_scaling"] == 0
        f["second_chroma_qp_index_offset"] = r.se()
    return f


def pps_write(f):
    w = BitW()
    w.ue(f["pps_id"]); w.ue(f["sps_id"]); w.u(1, f["entropy"]); w.u(1, f["pic_order_present"]); w.ue(0)
    w.ue(f["num_ref_idx_l0_minus1"]); w.ue(f["num_ref_idx_l1_minus1"]); w.u(1, f["weighted_pred"])
    w.u(2, f["weighted_bipred_idc"]); w.se(f["pic_init_qp_minus26"]); w.se(f.get("pic_init_qs_minus26", 0))
    w.se(f["chroma_qp_index_offset"]); w.u(1, f["deblocking_present"]); w.u(1, f["constrained_intra"])
    w.u(1, f.get("redundant_pic_cnt", 0))
    if f["ext"]:
        w.u(1, f["transform_8x8"]); w.u(1, 0); w.se(f["second_chroma_qp_index_offset"])
    w.trailing()
    return w.bytes()


def nal(nal_ref_idc, nal_type, rbsp):
    return b"\x00\x00\x00\x01" + bytes([(nal_ref_idc << 5) | nal_type]) + ep_add(rbsp)


def headers_from_setup(s):
    """SPS/PPS for the stream NVENC writes from grc's setup. The setup has no
    max_num_ref_frames or direct_8x8_inference; 1 and 1 match its single L0
    reference and what High-profile encoders emit."""
    mbw, mbh = (s["width"] + 15) // 16, (s["height"] + 15) // 16
    crop_bottom = (mbh * 16 - s["height"]) // 2
    crop_right = (mbw * 16 - s["width"]) // 2
    sps = dict(profile_idc=s["profile_idc"], constraint=0, level_idc=s["level_idc"], sps_id=0,
               chroma_format_idc=s["chroma_format_idc"], bit_depth_luma_minus8=0,
               bit_depth_chroma_minus8=0, qpprime_bypass=0,
               log2_max_frame_num_minus4=s["log2_max_frame_num_minus4"],
               pic_order_cnt_type=s["pic_order_cnt_type"],
               log2_max_poc_lsb_minus4=s["log2_max_poc_lsb_minus4"],
               max_num_ref_frames=s["num_ref_idx_l0_minus1"] + 1, gaps=0,
               width_mbs_minus1=mbw - 1, height_map_units_minus1=mbh - 1,
               frame_mbs_only=s["frame_mbs_only"], mb_adaptive=0, direct_8x8_inference=1,
               cropping=int(bool(crop_bottom or crop_right)), crop=[0, crop_right, 0, crop_bottom],
               vui_present=0, vui_bits=[])
    pps = dict(pps_id=s["pps_id"], sps_id=0, entropy=s["entropy"], pic_order_present=s["pic_order_present"],
               num_ref_idx_l0_minus1=s["num_ref_idx_l0_minus1"], num_ref_idx_l1_minus1=s["num_ref_idx_l1_minus1"],
               weighted_pred=s["weighted_pred"], weighted_bipred_idc=s["weighted_bipred_idc"],
               pic_init_qp_minus26=s["pic_init_qp_minus26"], pic_init_qs_minus26=0,
               chroma_qp_index_offset=s["chroma_qp_index_offset"], deblocking_present=s["deblocking_present"],
               constrained_intra=s["constrained_intra"], redundant_pic_cnt=0,
               ext=bool(s["transform_8x8"] or s["second_chroma_qp_index_offset"] != s["chroma_qp_index_offset"]),
               transform_8x8=s["transform_8x8"],
               second_chroma_qp_index_offset=s["second_chroma_qp_index_offset"])
    return nal(3, 7, sps_write(sps)) + nal(3, 8, pps_write(pps))


# ------------------------------------------------------------ NAL listing

def split_nals(data):
    """Annex B start-code split: [(offset, payload incl. header byte)]."""
    i, starts = 0, []
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        starts.append(j + 3)
        i = j + 3
    out = []
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else len(data)
        while e > s and data[e - 1] == 0 and k + 1 < len(starts):
            e -= 1
        out.append((s, data[s:e]))
    return out


NAL_NAMES = {1: "slice", 5: "IDR slice", 6: "SEI", 7: "SPS", 8: "PPS", 9: "AUD", 12: "filler"}


def describe_nals(data):
    lines = []
    for off, n in split_nals(data):
        if not n:
            continue
        t, ref = n[0] & 0x1F, (n[0] >> 5) & 3
        extra = ""
        if t in (1, 5) and len(n) > 4:
            r = BitR(ep_remove(n[1:16]))
            try:
                extra = f"  first_mb={r.ue()} slice_type={r.ue()} pps_id={r.ue()}"
            except IndexError:
                pass
        lines.append(f"  NAL @ {off - 4:#07x}: type {t:2} ({NAL_NAMES.get(t, '?')}) ref_idc {ref} {len(n)} B{extra}")
    return lines


# ------------------------------------------------------------ decode check

def decode_first_frame(stream):
    import av
    ctx = av.CodecContext.create("h264", "r")
    frames = []
    # the parser keeps the last access unit until it is flushed with None
    for pkt in list(ctx.parse(stream)) + list(ctx.parse(None)):
        frames += ctx.decode(pkt)
    frames += ctx.decode(None)
    if not frames:
        return None
    # the raw Y plane: format="gray" would rescale limited-range luma to full
    # range (Y=208 comes back as ~224) and every band would look wrong
    f = frames[0]
    return f.to_ndarray(format="yuv420p")[:f.height]


def band_report(luma, height):
    worst = 0
    rows = []
    for k in range((height + STRIPE_ROWS - 1) // STRIPE_ROWS):
        band = luma[k * STRIPE_ROWS:min((k + 1) * STRIPE_ROWS, height)]
        mean = float(band.mean())
        d = abs(mean - stripe_luma(k))
        worst = max(worst, d)
        rows.append((k, stripe_luma(k), mean))
    return worst, rows


# ----------------------------------------------------------------- commands

def gen():
    b = SETUP.read_bytes()
    s = parse_setup(b)
    want = dict(magic=0xd0b70006, width=1280, height=720, profile_idc=100, pic_type=3, frame_num=0)
    for k, v in want.items():
        assert s[k] == v, f"{SETUP.name}: {k}={s[k]!r}, expected {v!r}"
    rows = [", ".join(f"0x{x:02x}" for x in b[i:i + 16]) for i in range(0, len(b), 16)]
    body = ",\n        ".join(rows)
    HEADER.write_text(f"""/*
 * nvenc_grc_idr.h - GENERATED by tools/nvenc_replay.py from
 * logs/m79-runE-setups/setup_8.bin. Do not edit.
 *
 * grc's own nvenc_h264_drv_pic_setup_s for an IDR frame (pic_type 3,
 * frame_num 0, idr_pic_id {s['idr_pic_id']}), read out of grc in M79 Run E together with
 * the command buffer that submits it: {s['width']}x{s['height']} H.264 profile
 * {s['profile_idc']} level {s['level_idc']}, CABAC={s['entropy']}, 8x8={s['transform_8x8']}.
 * The 512-byte header and the slice/MD/quant/ME control arrays at 0x300-0x6C0
 * are all here. It contains no addresses - every surface is bound by a method.
 */
#pragma once

namespace ams::mitm::applet::nvenc_grc_idr {{

    constexpr u32 Width  = {s['width']};
    constexpr u32 Height = {s['height']};

    alignas(0x100) constexpr u8 Setup[{len(b)}] = {{
        {body},
    }};

}}
""")
    print(f"wrote {HEADER.relative_to(REPO)} from {SETUP.relative_to(REPO)}: " +
          ", ".join(f"{k}={s[k]}" for k in ("width", "height", "profile_idc", "level_idc", "pic_type", "frame_num", "idr_pic_id")))


def selftest():
    import av
    import numpy as np
    # 1) the SPS/PPS writer reproduces libx264's headers byte for byte
    enc = av.CodecContext.create("libx264", "w")
    enc.width, enc.height, enc.pix_fmt = 1280, 720, "yuv420p"
    enc.options = {"profile": "high", "x264-params": "bframes=0:ref=1:keyint=15:cabac=1:8x8dct=1:scenecut=0"}
    y = np.zeros((720, 1280), np.uint8)
    for k in range((720 + 31) // 32):
        y[k * 32:(k + 1) * 32] = stripe_luma(k)
    frame = av.VideoFrame(1280, 720, "yuv420p")
    frame.planes[0].update(y.tobytes())
    frame.planes[1].update(np.full((360, 640), 128, np.uint8).tobytes())
    frame.planes[2].update(np.full((360, 640), 128, np.uint8).tobytes())
    stream = b"".join(bytes(p) for p in enc.encode(frame)) + b"".join(bytes(p) for p in enc.encode(None))
    nals = [n for _, n in split_nals(stream)]
    sps_n = next(n for n in nals if n and n[0] & 0x1F == 7)
    pps_n = next(n for n in nals if n and n[0] & 0x1F == 8)
    for name, n, parse, write in (("SPS", sps_n, sps_parse, sps_write), ("PPS", pps_n, pps_parse, pps_write)):
        f = parse(ep_remove(n[1:]))
        again = n[:1] + ep_add(write(f))
        assert again == n, f"{name} rewrite differs:\n  x264 {n.hex()}\n  ours {again.hex()}"
        print(f"{name}: rewritten from parsed fields, byte-identical to libx264's ({len(n)} B)")
    # 2) the decode + band check, on x264's own slices
    luma = decode_first_frame(stream)
    worst, _ = band_report(luma, 720)
    print(f"x264 stripe frame decoded: worst band deviation {worst:.2f}")
    assert worst < 3, worst
    # 3) headers built from grc's setup parse back to what the setup says
    s = parse_setup(SETUP.read_bytes())
    hdr = headers_from_setup(s)
    got = [n for _, n in split_nals(hdr)]
    f = sps_parse(ep_remove(got[0][1:]))
    assert (f["profile_idc"], f["level_idc"], f["pic_order_cnt_type"], f["log2_max_frame_num_minus4"],
            f["width_mbs_minus1"] + 1, f["height_map_units_minus1"] + 1) == \
        (s["profile_idc"], s["level_idc"], s["pic_order_cnt_type"], s["log2_max_frame_num_minus4"], 80, 45), f
    p = pps_parse(ep_remove(got[1][1:]))
    assert (p["entropy"], p["transform_8x8"], p["deblocking_present"]) == (1, 1, 1), p
    print(f"SPS/PPS from grc's setup: {len(hdr)} B, fields check out")
    print("selftest OK")


def check(d):
    d = Path(d)
    st_path, bits_path, recon_path = d / "nvenc-status.bin", d / "nvenc-bits.bin", d / "nvenc-recon-y.bin"
    s = parse_setup(SETUP.read_bytes())
    if st_path.exists():
        st = st_path.read_bytes()
        (pic_index, err_word, total_bits, type1_bits, pic_type, num_slices, ave_act, avg_qp,
         cycles, hrd, bs_start, last_valid, intra_mbs, inter_mbs) = struct.unpack_from("<IIIIHHHHIiIIHH", st, 0)
        print(f"status: picture_index={pic_index} error_status={err_word & 3} ucode_error_status={err_word >> 2:#x}"
              f" total_bit_count={total_bits} ({total_bits // 8} B) pic_type={pic_type} num_slices={num_slices}"
              f" avgQP={avg_qp} cycle_count={cycles} bitstream_start_pos={bs_start} last_valid_byte_offset={last_valid}"
              f" intra_mbs={intra_mbs} inter_mbs={inter_mbs}")
    if not bits_path.exists():
        print(f"{bits_path.name}: missing")
        return
    bits = bits_path.read_bytes()
    print(f"{bits_path.name}: {len(bits)} B, first bytes {bits[:16].hex(' ')}")
    for line in describe_nals(bits):
        print(line)
    have = {n[0] & 0x1F for _, n in split_nals(bits) if n}
    stream = bits if {7, 8} <= have else headers_from_setup(s) + bits
    if not {7, 8} <= have:
        print("no SPS/PPS in the engine's output (expected: grc writes those itself) - prepending ones built from grc's setup")
    try:
        luma = decode_first_frame(stream)
    except Exception as e:   # noqa: BLE001 - report whatever the decoder says
        print(f"decode FAILED: {type(e).__name__}: {e}")
        luma = None
    if luma is None:
        print("no picture decoded")
    else:
        worst, rows = band_report(luma, min(luma.shape[0], s["height"]))
        print(f"decoded {luma.shape[1]}x{luma.shape[0]}; stripe means (want/got): " +
              " ".join(f"{w}/{g:.0f}" for _, w, g in rows))
        print("DECODE MATCHES THE INPUT STRIPES" if worst < 8 else f"decoded picture differs (worst band {worst:.1f})")
        try:
            from PIL import Image
            png = d / "nvenc-decoded.png"
            Image.fromarray(luma).save(png)
            print(f"  -> {png}")
        except ImportError:
            pass
    if recon_path.exists():
        rec = recon_path.read_bytes()
        # 16-row tile rows are contiguous in the tiled16 layout, so band means
        # need no untiling
        tr = s["width"] * 16
        worst = 0.0
        for t in range(len(rec) // tr):
            m = sum(rec[t * tr:(t + 1) * tr]) / tr
            worst = max(worst, abs(m - stripe_luma(t // 2)))
        print(f"{recon_path.name}: {len(rec)} B, reconstructed-luma stripe check worst deviation {worst:.1f}"
              + ("  (matches)" if worst < 8 else ""))


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "gen":
        gen()
    elif len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        selftest()
    elif len(sys.argv) >= 3 and sys.argv[1] == "check":
        check(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(2)
