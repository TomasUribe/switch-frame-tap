#!/usr/bin/env python3
"""nvrec.py - decode sdmc:/nvenc-grc.bin, the M72 grc recorder's output.

The recorder is a logging-only mitm on nvdrv:s, accepted for grc (the system
game recorder) only. It records grc's device opens, its encoder and nvmap
ioctls, and - for its first few msenc submits - the command buffer words and
the drv_pic_setup they point at, read straight out of grc's memory.

    python3 tools/nvrec.py nvenc-grc.bin             # timeline + decoded cmdbufs
    python3 tools/nvrec.py nvenc-grc.bin --out dir   # also write setup_N.bin

Each setup_N.bin decodes with tools/nvsetup-dump (built from nvsetup-dump.c
against NVIDIA's own nvenc_drv.h, so the bitfield layout is exact).
"""
import argparse, os, struct, sys

NVENC_METHODS = {
    0x100: "NOP", 0x200: "SET_APPLICATION_ID", 0x204: "SET_WATCHDOG_TIMER",
    0x240: "SEMAPHORE_A", 0x244: "SEMAPHORE_B", 0x248: "SEMAPHORE_C",
    0x24C: "CTX_SAVE_AREA", 0x250: "CTX_SWITCH", 0x300: "EXECUTE",
    0x304: "SEMAPHORE_D",
}
for i in range(16):
    NVENC_METHODS[0x400 + 4 * i] = f"SET_IN_REF_PIC{i}_LUMA"
    NVENC_METHODS[0x440 + 4 * i] = f"SET_IN_REF_PIC{i}_CHROMA"
NVENC_METHODS.update({
    0x500: "SET_UCODE_STATE_OR_ENC_REGS?", 0x50C: "SET_UCODE_STATE",
    0x700: "SET_CONTROL_PARAMS", 0x704: "SET_PICTURE_INDEX",
    0x708: "SET_IN_RCDATA", 0x70C: "SET_IN_RCDEBUG",
    0x710: "SET_IN_DRV_PIC_SETUP", 0x714: "SET_IN_CEAHINTS_DATA",
    0x718: "SET_OUT_ENC_STATUS", 0x71C: "SET_OUT_BITSTREAM",
    0x720: "SET_IOHISTORY", 0x724: "SET_IO_RC_PROCESS",
    0x728: "SET_OUT_COLOC_DATA", 0x72C: "SET_IN_COLOC_DATA",
    0x730: "SET_OUT_REF_PIC_LUMA", 0x734: "SET_IN_CUR_PIC",
    0x738: "SET_IN_MEPRED_DATA", 0x73C: "SET_OUT_MEPRED_DATA",
    0x740: "SET_IN_CUR_PIC_CHROMA_U", 0x744: "SET_IN_CUR_PIC_CHROMA_V",
    0x748: "SET_IN_QP_MAP", 0x74C: "SET_OUT_REF_PIC_CHROMA",
})
# Overridden at runtime from NVIDIA's header when ref/ is present - the table
# above is only a fallback for machines without the open-gpu-doc checkout.
HDR = os.path.join(os.path.dirname(__file__), "..", "ref", "open-gpu-doc", "classes", "video", "clc5b7.h")
if os.path.exists(HDR):
    import re
    found = {}
    for line in open(HDR):
        m = re.match(r"#define NVC5B7_([A-Z0-9_]+)\s+\((0x[0-9A-F]{8})U\)", line)
        if m:
            v = int(m.group(2), 16)
            if 0x100 <= v <= 0x2000 and v % 4 == 0:
                if v not in found or len(m.group(1)) < len(found[v]):
                    found[v] = m.group(1)
    NVENC_METHODS.update(found)

KIND = {1: "OPEN", 2: "IOCTL>", 3: "IOCTL<", 4: "IOCTL2>", 5: "IOCTL3>",
        6: "CLOSE", 7: "CMDBUF", 8: "SETUP", 9: "NOTE"}
CLASS = {0x01: "HOST1X", 0x21: "NVENC", 0x5D: "VIC", 0xC0: "NVJPG", 0xF0: "NVDEC"}


def ioc(rq):
    d = rq >> 30
    return f"{'-WRX'[d] if d < 4 else '?'} sz={(rq >> 16) & 0x3FFF:#x} type={(rq >> 8) & 0xFF:#04x} nr={rq & 0xFF:#04x}"


def decode_cmdbuf(words, out):
    """Replay host1x opcodes; show every method write by name."""
    method, cls, i = 0, None, 0
    def reg(r, v):
        nonlocal method
        if r == 0x10:
            method = v
        elif r == 0x11:
            m = method << 2
            out(f"      {CLASS.get(cls, hex(cls or 0)):6} {m:#06x} {NVENC_METHODS.get(m, '?'):32} = {v:#010x}"
                + (f"   (iova {v << 8:#x})" if 0x400 <= m < 0x800 and m not in (0x700, 0x704) else ""))
        elif r == 0x00:
            out(f"      INCR_SYNCPT cond={(v >> 8) & 0xFF} syncpt={v & 0xFF}")
        else:
            out(f"      host1x reg {r:#05x} = {v:#010x}")
    while i < len(words):
        w = words[i]; i += 1
        op, o, n = w >> 28, (w >> 16) & 0xFFF, w & 0xFFFF
        if op == 0:
            cls = (w >> 6) & 0x3FF
            out(f"    SETCL class={cls:#x} ({CLASS.get(cls, '?')}) offset={o:#x} mask={w & 0x3F:#x}")
        elif op == 1:
            for j in range(n):
                if i < len(words): reg(o + j, words[i]); i += 1
        elif op == 2:
            for j in range(n):
                if i < len(words): reg(o, words[i]); i += 1
        elif op == 3:
            for b in range(16):
                if n & (1 << b) and i < len(words): reg(o + b, words[i]); i += 1
        elif op == 4:
            reg(o, n)
        else:
            out(f"    opcode {op} word={w:#010x}  (not decoded)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rec")
    ap.add_argument("--out", help="directory for setup_N.bin")
    ap.add_argument("--all", action="store_true", help="show every ioctl, not just msenc")
    a = ap.parse_args()

    data = open(a.rec, "rb").read()
    off, nset, fds = 0, 0, {}
    say = print
    while off + 32 <= len(data):
        magic, kind, hl, fd, rq, x, y, ln, ms = struct.unpack_from("<IHHIIIIII", data, off)
        if magic != 0x4352564E:
            say(f"!! bad magic at {off:#x} - stream corrupt, stopping"); break
        p = data[off + hl: off + hl + ln]
        off += hl + ln
        k = KIND.get(kind, f"?{kind}")
        t = f"[{ms / 1000:9.3f}]"
        if kind == 1:
            path = p.decode(errors="replace"); fds[x] = path
            say(f"{t} OPEN   {path} -> fd {x} err {y}")
        elif kind in (2, 3, 4, 5):
            dev = fds.get(fd, f"fd{fd}")
            if not a.all and "msenc" not in dev and kind != 2:
                continue
            extra = f" in={x} out={y}" if kind != 3 else f" nverr={x}"
            say(f"{t} {k:7} {dev:18} {rq:#010x} {ioc(rq)}{extra}")
            say("          " + p[:64].hex(" ", 4))
        elif kind == 6:
            say(f"{t} CLOSE  {fds.get(fd, fd)}")
        elif kind == 7:
            words = list(struct.unpack_from(f"<{len(p) // 4}I", p))
            say(f"{t} CMDBUF handle={x:#x} offset={y:#x} words={len(words)}")
            decode_cmdbuf(words, say)
        elif kind == 8:
            say(f"{t} SETUP  iova={x:#x} (grc va low {y:#x}) {len(p)} B  magic={struct.unpack_from('<I', p)[0]:#010x}")
            if a.out:
                os.makedirs(a.out, exist_ok=True)
                fn = os.path.join(a.out, f"setup_{nset}.bin")
                open(fn, "wb").write(p)
                say(f"          -> {fn}")
            nset += 1
        elif kind == 9:
            say(f"{t} NOTE   {p.decode(errors='replace')}")
    say(f"\n{off} of {len(data)} bytes decoded, {nset} setup blob(s)")


if __name__ == "__main__":
    main()
