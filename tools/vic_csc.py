#!/usr/bin/env python3
"""
vic_csc.py - the VIC's colour matrix: measured (M82 Run H), modelled, designed.

M64-M66 tried three times to program the VIC's RGB->YUV matrix and got a
constant picture. M82 ran 11 probe jobs (CscProbes in applet_mitm_nv.cpp) on
a 64x64 card of 16 flat patches and saved every output to sdmc:/vic-csc.bin.
Run H's file is logs/m82-runH-vic-csc.bin.

THE LAW (derived from Run H; `verify` shows it reproduces all 528 observed
values - 11 probes x 16 patches x 3 planes - with zero error):

    inputs   in = (B, G, R)             the card's bytes R,G,B,A, declared
                                        A8R8G8B8 as the game path declares it
             in10 = in8 << 2            the pipeline is 10-bit
    per row  acc = (sum_j c[j] * in10[j]) >> matrix_r_shift  +  c[3]
             out10 = clamp(acc >> 8, 0, 1023)
             out8  = out10 >> 2
    rows ->  planes (Y, V, U)           row 1 is Cr, row 2 is Cb
    pass-through (matrix off) is the identity: Y = B, V = G, U = R

  so a coefficient has 8 fraction bits (256 = 1.0), matrix_r_shift divides
  the products only, and the offset is in 1/256ths of a 10-bit step (16 in
  8-bit terms is 16 * 4 * 256 = 16384). Both stages truncate.

What M64-M66 got wrong, all at once: coefficients scaled for "shift 8 means
/256" on top of the 8 fraction bits the hardware already has (so 256x too
small: 66..129 became 0.004), offsets scaled for 8-bit instead of 10-bit
units (4x too small), and rows/columns in R,G,B / Y,U,V order instead of the
B,G,R / Y,V,U the hardware uses. M82's m64_bt601 probe reproduced M64's
constant 4/32/32 exactly under this law.

  python3 tools/vic_csc.py FILE            fit every probe's planes
  python3 tools/vic_csc.py verify FILE     the law against every value in FILE
  python3 tools/vic_csc.py design          BT.709 / BT.601 limited-range matrices,
                                           checked through the law against the
                                           ideal float conversion
  python3 tools/vic_csc.py gen             write tier4/applet-mitm/source/vic_csc_bt709.h
  python3 tools/vic_csc.py selftest
"""
import math
import struct
import sys
from pathlib import Path

HDR = struct.Struct("<8I")                       # magic version count w h src_fmt out_fmt reserved
REC = struct.Struct("<16sII12iI")                # name where shift c[3][4] completed
MAGIC = 0x43534356
POISON = 0xAB                                    # RunOneJob's prefill
PLANES = ("Y", "U", "V")


def parse(data):
    magic, version, count, w, h, src_fmt, out_fmt, _ = HDR.unpack_from(data, 0)
    if magic != MAGIC:
        raise SystemExit(f"not a vic-csc file (magic {magic:#x})")
    off = HDR.size
    card = data[off:off + w * h * 4]
    off += w * h * 4
    probes = []
    for _ in range(count):
        if off + REC.size > len(data):
            break
        name, where, shift, *rest = REC.unpack_from(data, off)
        c = [rest[i * 4:(i + 1) * 4] for i in range(3)]
        completed = rest[12]
        off += REC.size
        y = data[off:off + w * h]
        off += w * h
        uv = data[off:off + w * h // 2]
        off += w * h // 2
        probes.append(dict(name=name.rstrip(b"\0").decode(), where=where, shift=shift, c=c,
                           completed=completed, y=y, uv=uv))
    return dict(version=version, w=w, h=h, src_fmt=src_fmt, out_fmt=out_fmt, card=card, probes=probes)


def patch_samples(f, p):
    """[(R, G, B, Y, U, V)] per patch, from the patch centres."""
    w, h = f["w"], f["h"]
    ps = 16
    out = []
    for py in range(h // ps):
        for px in range(w // ps):
            cx, cy = px * ps + ps // 2, py * ps + ps // 2
            r, g, b = f["card"][(cy * w + cx) * 4:(cy * w + cx) * 4 + 3]
            ys = [p["y"][yy * w + xx] for yy in range(cy - 4, cy + 4) for xx in range(cx - 4, cx + 4)]
            us, vs = [], []
            for yy in range(cy // 2 - 2, cy // 2 + 2):
                for xx in range(cx // 2 - 2, cx // 2 + 2):
                    us.append(p["uv"][yy * w + 2 * xx])
                    vs.append(p["uv"][yy * w + 2 * xx + 1])
            out.append((r, g, b, sum(ys) / len(ys), sum(us) / len(us), sum(vs) / len(vs)))
    return out


def lstsq4(rows):
    """Solve [R G B 1] x = out in the least-squares sense (normal equations)."""
    ata = [[0.0] * 4 for _ in range(4)]
    atb = [0.0] * 4
    for r, g, b, o in rows:
        v = (r, g, b, 1.0)
        for i in range(4):
            atb[i] += v[i] * o
            for j in range(4):
                ata[i][j] += v[i] * v[j]
    m = [ata[i] + [atb[i]] for i in range(4)]
    for col in range(4):
        piv = max(range(col, 4), key=lambda k: abs(m[k][col]))
        if abs(m[piv][col]) < 1e-9:
            return None
        m[col], m[piv] = m[piv], m[col]
        for k in range(4):
            if k != col:
                fct = m[k][col] / m[col][col]
                m[k] = [a - fct * b for a, b in zip(m[k], m[col])]
    return [m[i][4] / m[i][i] for i in range(4)]


def fit_plane(samples, idx):
    rows = [(s[0], s[1], s[2], s[3 + idx]) for s in samples if 1.5 < s[3 + idx] < 253.5]
    clipped = len(samples) - len(rows)
    vals = [s[3 + idx] for s in samples]
    if len(rows) < 5:
        return dict(coef=None, clipped=clipped, n=len(rows), rms=None, lo=min(vals), hi=max(vals))
    coef = lstsq4(rows)
    if coef is None:
        return dict(coef=None, clipped=clipped, n=len(rows), rms=None, lo=min(vals), hi=max(vals))
    rms = math.sqrt(sum((coef[0] * r + coef[1] * g + coef[2] * b + coef[3] - o) ** 2 for r, g, b, o in rows) / len(rows))
    return dict(coef=coef, clipped=clipped, n=len(rows), rms=rms, lo=min(vals), hi=max(vals))


def analyse(f, out=print):
    out(f"vic-csc v{f['version']}: {f['w']}x{f['h']} card, source fmt {f['src_fmt']}, output fmt {f['out_fmt']}, "
        f"{len(f['probes'])} probes")
    results = {}
    for p in f["probes"]:
        where = ("none", "out", "slot")[p["where"]] if p["where"] < 3 else str(p["where"])
        allpoison = all(v == POISON for v in p["y"]) and all(v == POISON for v in p["uv"])
        out(f"\n== {p['name']} ({where}, shift {p['shift']}) {'completed' if p['completed'] else 'DID NOT COMPLETE'}"
            + ("  [output never written: all 0xAB]" if allpoison else ""))
        if p["where"]:
            out("   programmed rows: " + "  ".join("[" + " ".join(str(v) for v in row) + "]" for row in p["c"]))
        samples = patch_samples(f, p)
        fits = []
        for i, pl in enumerate(PLANES):
            ft = fit_plane(samples, i)
            fits.append(ft)
            if ft["coef"] is None:
                out(f"   {pl}: {ft['n']} usable of {len(samples)} ({ft['clipped']} clipped), range {ft['lo']:.0f}..{ft['hi']:.0f} - no fit"
                    + (" (constant)" if ft["hi"] - ft["lo"] < 1 else ""))
            else:
                a, b, c, d = ft["coef"]
                out(f"   {pl} = {a:+.4f}*R {b:+.4f}*G {c:+.4f}*B {d:+.2f}   (rms {ft['rms']:.2f}, {ft['n']} used, {ft['clipped']} clipped)")
        results[p["name"]] = (p, fits)
        std = next((k for k in STANDARDS if p["name"].startswith(k)), None)
        if std is not None:
            worst = max(abs(o - i) for s_ in samples for o, i in zip(s_[3:], ideal(std, s_[:3])))
            law_bad = sum(1 for s_ in samples for o, i in zip(s_[3:], law(p["where"], p["shift"], p["c"], s_[:3])) if o != i)
            out(f"   against the float {std} conversion: worst {worst:.2f} steps over the 16 patches"
                f"{'  -> REAL ' + std.upper() + ' YUV' if worst <= 1.0 else '  -> NOT within one step'};"
                f" the law predicts {48 - law_bad} of 48 values exactly")
    out("")
    summarize(results, out)
    return results


def dominant(fit):
    if fit["coef"] is None:
        return None
    a = fit["coef"][:3]
    k = max(range(3), key=lambda i: abs(a[i]))
    return k, a[k]


def summarize(results, out=print):
    out("== what the probes say ==")
    if "none" in results:
        _, fits = results["none"]
        desc = []
        for pl, ft in zip(PLANES, fits):
            dm = dominant(ft)
            desc.append(f"{pl}={'RGB'[dm[0]]}x{dm[1]:.2f}" if dm else f"{pl}=?")
        out("   pass-through (no matrix): " + ", ".join(desc))
    for name in ("out_k8", "out_k12", "out_k16", "out_k19", "out_k16_s8", "slot_k16"):
        if name not in results:
            continue
        p, fits = results[name]
        k = abs(p["c"][0][0])
        parts = []
        for pl, ft in zip(PLANES, fits):
            dm = dominant(ft)
            if dm is None:
                parts.append(f"{pl}: const {ft['lo']:.0f}" if ft["hi"] - ft["lo"] < 1 else f"{pl}: no fit")
            else:
                unit = abs(dm[1]) / k
                parts.append(f"{pl}<-{'RGB'[dm[0]]} gain {dm[1]:.4f} = K x 2^{math.log2(unit):.2f}" if unit > 0 else f"{pl}: gain 0")
        out(f"   {name:11} K={k}: " + "; ".join(parts))
    if "out_off" in results:
        p, fits = results["out_off"]
        parts = []
        for i, (pl, ft) in enumerate(zip(PLANES, fits)):
            prog = p["c"][i][3]
            val = (ft["lo"] + ft["hi"]) / 2
            parts.append(f"{pl}={val:.1f} for offset {prog} ({val / prog:.4f}/unit)" if prog else f"{pl}={val:.1f}")
        out("   offsets alone: " + "; ".join(parts))


# ------------------------------------------------------------------ the law

def law(where, shift, c, rgb):
    """What the VIC writes for one pixel (R, G, B) under a probe/matrix,
    as (Y, U, V). where 0 = matrix off."""
    r, g, b = rgb
    ins10 = [b << 2, g << 2, r << 2]
    rows = []
    for i in range(3):
        if where == 0:
            out10 = ins10[i]
        else:
            acc = (sum(c[i][j] * ins10[j] for j in range(3)) >> shift) + c[i][3]
            out10 = max(0, min(1023, acc >> 8))
        rows.append(out10 >> 2)
    y, v, u = rows
    return y, u, v


def verify(f, out=print):
    """Every observed patch value against the law. Returns mismatches."""
    w = f["w"]
    total = bad = 0
    for p in f["probes"]:
        pbad = 0
        for py in range(f["h"] // 16):
            for px in range(w // 16):
                cx, cy = px * 16 + 8, py * 16 + 8
                rgb = tuple(f["card"][(cy * w + cx) * 4:(cy * w + cx) * 4 + 3])
                obs = (p["y"][cy * w + cx], p["uv"][(cy // 2) * w + (cx & ~1)], p["uv"][(cy // 2) * w + (cx & ~1) + 1])
                pred = law(p["where"], p["shift"], p["c"], rgb)
                for a, o in zip(pred, obs):
                    total += 1
                    if a != o:
                        bad += 1
                        pbad += 1
        out(f"   {p['name']:11} {'OK' if pbad == 0 else f'{pbad} MISMATCHES'}")
    out(f"the law reproduces {total - bad} of {total} observed values")
    return bad


# ------------------------------------------------------------------ design

STANDARDS = {"bt709": (0.2126, 0.0722), "bt601": (0.299, 0.114)}


def ideal(standard, rgb):
    """Float limited-range Y, Cb, Cr (8-bit) for an 8-bit R, G, B."""
    kr, kb = STANDARDS[standard]
    kg = 1 - kr - kb
    r, g, b = rgb
    yl = kr * r + kg * g + kb * b
    y = 16 + 219 / 255 * yl
    cb = 128 + 224 / 255 * (b - yl) / (2 * (1 - kb))
    cr = 128 + 224 / 255 * (r - yl) / (2 * (1 - kr))
    return y, cb, cr


DESIGN_SHIFT = 8   # proven by Run H's out_k16_s8 and m64_bt601 probes


def design(standard="bt709", shift=DESIGN_SHIFT):
    """The 3x4 matrix in the hardware's order: rows (Y, V, U), columns (B, G, R,
    offset). With matrix_r_shift 8 a coefficient has 8 + 8 = 16 fraction bits
    (65536 = 1.0), so quantisation error is negligible; the offsets are in
    1/256 of a 10-bit step whatever the shift, plus 512 (half an 8-bit step)
    so the two truncations round to nearest."""
    kr, kb = STANDARDS[standard]
    kg = 1 - kr - kb
    sy, sc = 219 / 255, 224 / 255
    rows_rgb = {
        "Y": (sy * kr, sy * kg, sy * kb),
        "U": (-sc * kr / (2 * (1 - kb)), -sc * kg / (2 * (1 - kb)), sc * (1 - kb) / (2 * (1 - kb))),
        "V": (sc * (1 - kr) / (2 * (1 - kr)), -sc * kg / (2 * (1 - kr)), -sc * kb / (2 * (1 - kr))),
    }
    offs = {"Y": 16 * 4 * 256 + 512, "U": 128 * 4 * 256 + 512, "V": 128 * 4 * 256 + 512}
    out = []
    for plane in ("Y", "V", "U"):
        fr, fg, fb = rows_rgb[plane]
        one = 256 << shift
        q = [round(fb * one), round(fg * one), round(fr * one)]
        if plane != "Y":
            # keep each chroma row summing to zero, so grey stays exactly neutral
            err = [fb * one - q[0], fg * one - q[1], fr * one - q[2]]
            while sum(q) != 0:
                k = max(range(3), key=lambda i: err[i]) if sum(q) < 0 else min(range(3), key=lambda i: err[i])
                q[k] += 1 if sum(q) < 0 else -1
                err[k] = [fb, fg, fr][k] * one - q[k]
        out.append(q + [offs[plane]])
    return out


def check_design(standard, m, out=print, shift=DESIGN_SHIFT):
    """Push every RGB triple on a 0..255 grid (step 5, plus the card's
    patches) through the law and compare with the float conversion."""
    worst = [0.0, 0.0, 0.0]
    for r in list(range(0, 256, 5)) + [255]:
        for g in list(range(0, 256, 5)) + [255]:
            for b in list(range(0, 256, 5)) + [255]:
                got = law(1, shift, m, (r, g, b))
                want = ideal(standard, (r, g, b))
                for i in range(3):
                    worst[i] = max(worst[i], abs(got[i] - want[i]))
    out(f"   {standard}: worst error against the float conversion over the RGB cube: "
        f"Y {worst[0]:.2f}, U {worst[1]:.2f}, V {worst[2]:.2f} (8-bit steps)")
    return max(worst)


HEADER = __import__("pathlib").Path(__file__).resolve().parent.parent / "tier4/applet-mitm/source/vic_csc_bt709.h"


def gen():
    m = design("bt709")
    assert check_design("bt709", m, out=lambda *_: None) <= 1.0
    rows = ",\n".join("        { " + ", ".join(f"{v:7d}" for v in row) + " }" for row in m)
    HEADER.write_text(f"""/*
 * vic_csc_bt709.h - GENERATED by tools/vic_csc.py gen. Do not edit.
 *
 * RGB -> BT.709 limited-range YCbCr for the VIC's output matrix, in the
 * encoding M82 Run H measured (see tools/vic_csc.py for the law and the
 * check): rows (Y, Cr, Cb), columns (B, G, R, offset) for a source declared
 * A8R8G8B8 whose bytes are R,G,B,A; matrix_r_shift {DESIGN_SHIFT}, so 16 fraction
 * bits (65536 = 1.0); offsets in 1/256 of a 10-bit step, +512 so the output
 * rounds.
 */
#pragma once

namespace ams::mitm::applet::vic_csc {{

    constexpr u32 Shift = {DESIGN_SHIFT};
    constexpr s32 Bt709[3][4] = {{
{rows},
    }};

}}
""")
    print(f"wrote {HEADER.name}: " + "  ".join(str(r) for r in m))


# ------------------------------------------------------------------ selftest

PATCHES = [(40, 40, 40), (200, 40, 40), (40, 200, 40), (40, 40, 200), (200, 200, 40), (200, 40, 200),
           (40, 200, 200), (200, 200, 200), (120, 120, 120), (200, 120, 40), (40, 200, 120), (120, 40, 200),
           (160, 80, 200), (80, 160, 40), (200, 200, 120), (60, 100, 160)]
K8, K11, K12, K16, K19 = 1 << 8, 1 << 11, 1 << 12, 1 << 16, (1 << 19) - 1
PROBES = [
    ("none", 0, 0, [[0] * 4] * 3),
    ("out_k8", 1, 0, [[K8, 0, 0, 0], [0, K8, 0, 0], [0, 0, K8, 0]]),
    ("out_k12", 1, 0, [[K12, 0, 0, 0], [0, K12, 0, 0], [0, 0, K12, 0]]),
    ("out_k16", 1, 0, [[K16, 0, 0, 0], [0, K16, 0, 0], [0, 0, K16, 0]]),
    ("out_k19", 1, 0, [[K19, 0, 0, 0], [0, K19, 0, 0], [0, 0, K19, 0]]),
    ("out_k16_s8", 1, 8, [[K16, 0, 0, 0], [0, K16, 0, 0], [0, 0, K16, 0]]),
    ("out_off", 1, 0, [[0, 0, 0, 256], [0, 0, 0, 512], [0, 0, 0, 768]]),
    ("out_neg", 1, 0, [[K16, 0, 0, 0], [0, -K16, 0, 768], [0, 0, K16, 0]]),
    ("out_dense", 1, 0, [[K11, 2 * K11, 3 * K11, 0], [4 * K11, 5 * K11, 6 * K11, 0], [7 * K11, 8 * K11, 9 * K11, 0]]),
    ("slot_k16", 2, 0, [[K16, 0, 0, 0], [0, K16, 0, 0], [0, 0, K16, 0]]),
    ("m64_bt601", 1, 8, [[66, 129, 25, 4096], [-38, -74, 112, 32768], [112, -94, -18, 32768]]),
]


def synth(probes=PROBES):
    """A file as the console writes it, produced by the law."""
    w = h = 64
    card = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            r, g, b = PATCHES[(y // 16) * 4 + x // 16]
            card[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((r, g, b, 255))
    data = bytearray(HDR.pack(MAGIC, 1, len(probes), w, h, 32, 67, 0)) + card
    for name, where, shift, c in probes:
        data += REC.pack(name.encode().ljust(16, b"\0"), where, shift, *[v for row in c for v in row], 1)
        yp = bytearray(w * h)
        uvp = bytearray(w * h // 2)
        for y in range(h):
            for x in range(w):
                yy, u, v = law(where, shift, c, PATCHES[(y // 16) * 4 + x // 16])
                yp[y * w + x] = yy
                if y % 2 == 0 and x % 2 == 0:
                    uvp[(y // 2) * w + x] = u
                    uvp[(y // 2) * w + x + 1] = v
        data += yp + uvp
    return bytes(data)


def selftest():
    lines = []
    f = parse(synth())
    assert verify(f, out=lines.append) == 0
    res = analyse(f, out=lines.append)
    # under the law, out_k8 is the identity: Y<-B, U<-R, V<-G at gain 1
    got = [(dominant(ft)[0], round(dominant(ft)[1], 3)) for ft in res["out_k8"][1]]
    assert got == [(2, 1.0), (0, 1.0), (1, 1.0)], got
    # M64's matrix gives a constant 4 / 32 / 32 (M64's observation)
    ft = res["m64_bt601"][1]
    assert round(ft[0]["coef"][3]) == 4 and abs(ft[1]["coef"][3] - 32) < 1.5, ft
    # the designed matrices land within one step of the float conversion
    for std in STANDARDS:
        assert check_design(std, design(std), out=lines.append) <= 1.0
    # and the M83 matrix, run as a probe, fits as BT.709 on the card
    m = design("bt709")
    fm = parse(synth([("bt709", 1, DESIGN_SHIFT, m)]))
    ft = analyse(fm, out=lines.append)["bt709"][1]
    assert any("REAL BT709 YUV" in ln for ln in lines), "bt709 probe check"
    kr, kb = STANDARDS["bt709"]
    assert abs(ft[0]["coef"][0] - 219 / 255 * kr) < 0.01 and abs(ft[0]["coef"][3] - 16) < 1.0, ft[0]
    # the real Run H file, if present, must still verify
    run_h = Path(__file__).resolve().parent.parent / "logs/m82-runH-vic-csc.bin"
    if run_h.exists():
        assert verify(parse(run_h.read_bytes()), out=lines.append) == 0
        lines.append("Run H's file verifies under the law")
    # the generated header matches the design
    if HEADER.exists():
        txt = HEADER.read_text()
        for row in design("bt709"):
            assert "{ " + ", ".join(f"{v:7d}" for v in row) + " }" in txt, "vic_csc_bt709.h is stale - run gen"
    print("\n".join(lines))
    print("selftest OK")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) >= 2 else ""
    if cmd == "selftest":
        selftest()
    elif cmd == "verify" and len(sys.argv) >= 3:
        sys.exit(1 if verify(parse(open(sys.argv[2], "rb").read())) else 0)
    elif cmd == "design":
        for std in STANDARDS:
            m = design(std)
            print(f"{std}: rows (Y, V, U) x columns (B, G, R, offset), shift {DESIGN_SHIFT}")
            for plane, row in zip("YVU", m):
                print(f"   {plane}: {row}")
            check_design(std, m)
    elif cmd == "gen":
        gen()
    elif cmd:
        analyse(parse(open(cmd, "rb").read()))
    else:
        print(__doc__)
