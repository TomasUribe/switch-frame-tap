#!/usr/bin/env python3
"""
vic_csc.py - M82: read sdmc:/vic-csc.bin and measure the VIC's colour matrix.

Three attempts to program the VIC's RGB->YUV matrix (M64, M66) produced a
constant: the offset column landed and every coefficient term vanished. The
console now runs one VIC job per "probe" (a whole 3x4 matrix, see
CscProbes in applet_mitm_nv.cpp) on a 64x64 card of 16 flat 16x16 patches,
and saves the source card and every NV12 output here.

For every probe this fits each output plane (Y, U, V) as

    plane = a*R + b*G + c*B + d          (8-bit units, R/G/B = the card's bytes)

over the patch centres, leaving out clipped samples (0-1 or 254-255). Flat
patches make the chroma filter irrelevant. What the fits say, together:

  - the diagonal probes (one coefficient K per row, shift 0) give, for each
    row, the plane it lands in, the input it reads and its gain; gain / K is
    what one coefficient unit is worth, the number M64-M66 were missing
  - out_k16_s8 against out_k16: whether matrix_r_shift divides the products
  - out_off: the unit of the offset column
  - out_neg: whether a negative coefficient works (two's complement)
  - out_dense: whether all nine coefficient fields sit where the struct says
  - slot_k16: the slot matrix under the same law
  - m64_bt601 must reproduce M64's constant, or the harness is not
    measuring what M64 did

  python3 tools/vic_csc.py vic-csc.bin
  python3 tools/vic_csc.py selftest
"""
import math
import struct
import sys

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


# ------------------------------------------------------------------ selftest

def synth(law_f=16, clip=True):
    """A file as the console would write it, under a made-up law:
    out = clip((sum c*in) / 2^law_f + off / 4) in 8-bit, rows -> (Y, U, V)
    with input order (B, G, R), and the documented pass-through for 'none'."""
    w = h = 64
    patches = [(40, 40, 40), (200, 40, 40), (40, 200, 40), (40, 40, 200), (200, 200, 40), (200, 40, 200),
               (40, 200, 200), (200, 200, 200), (120, 120, 120), (200, 120, 40), (40, 200, 120), (120, 40, 200),
               (160, 80, 200), (80, 160, 40), (200, 200, 120), (60, 100, 160)]
    card = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            r, g, b = patches[(y // 16) * 4 + x // 16]
            card[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((r, g, b, 255))
    k8, k11, k12, k16, k19 = 1 << 8, 1 << 11, 1 << 12, 1 << 16, (1 << 19) - 1
    probes = [
        ("none", 0, 0, [[0] * 4] * 3),
        ("out_k8", 1, 0, [[k8, 0, 0, 0], [0, k8, 0, 0], [0, 0, k8, 0]]),
        ("out_k12", 1, 0, [[k12, 0, 0, 0], [0, k12, 0, 0], [0, 0, k12, 0]]),
        ("out_k16", 1, 0, [[k16, 0, 0, 0], [0, k16, 0, 0], [0, 0, k16, 0]]),
        ("out_k19", 1, 0, [[k19, 0, 0, 0], [0, k19, 0, 0], [0, 0, k19, 0]]),
        ("out_k16_s8", 1, 8, [[k16, 0, 0, 0], [0, k16, 0, 0], [0, 0, k16, 0]]),
        ("out_off", 1, 0, [[0, 0, 0, 256], [0, 0, 0, 512], [0, 0, 0, 768]]),
        ("out_neg", 1, 0, [[k16, 0, 0, 0], [0, -k16, 0, 768], [0, 0, k16, 0]]),
        ("out_dense", 1, 0, [[k11, 2 * k11, 3 * k11, 0], [4 * k11, 5 * k11, 6 * k11, 0], [7 * k11, 8 * k11, 9 * k11, 0]]),
        ("slot_k16", 2, 0, [[k16, 0, 0, 0], [0, k16, 0, 0], [0, 0, k16, 0]]),
        ("m64_bt601", 1, 8, [[66, 129, 25, 4096], [-38, -74, 112, 32768], [112, -94, -18, 32768]]),
    ]
    data = bytearray(HDR.pack(MAGIC, 1, len(probes), w, h, 32, 67, 0)) + card
    for name, where, shift, c in probes:
        data += REC.pack(name.encode().ljust(16, b"\0"), where, shift, *[v for row in c for v in row], 1)
        yp = bytearray(w * h)
        uvp = bytearray(w * h // 2)
        for y in range(h):
            for x in range(w):
                r, g, b = patches[(y // 16) * 4 + x // 16]
                if where == 0:
                    yuv = (b, r, g)
                else:
                    ins = (b, g, r)
                    yuv = []
                    for i in range(3):
                        v = sum(c[i][j] * ins[j] for j in range(3)) / (2 ** law_f) / (2 ** shift) + c[i][3] / 4 / (2 ** shift)
                        yuv.append(max(0, min(255, round(v))) if clip else round(v) & 255)
                yp[y * w + x] = yuv[0]
                if y % 2 == 0 and x % 2 == 0:
                    uvp[(y // 2) * w + x] = yuv[1]
                    uvp[(y // 2) * w + x + 1] = yuv[2]
        data += yp + uvp
    return bytes(data)


def selftest():
    f = parse(synth())
    lines = []
    res = analyse(f, out=lines.append)
    # the pass-through is Y=B, U=R, V=G with gain 1
    fits = res["none"][1]
    assert [dominant(ft)[0] for ft in fits] == [2, 0, 1], [dominant(ft) for ft in fits]
    # under the synthetic law (inputs B,G,R; unit 2^-16), out_k16 is gain 1:
    #   row 0 (Y) reads B, row 1 (U) G, row 2 (V) R
    fits = res["out_k16"][1]
    got = [(dominant(ft)[0], round(dominant(ft)[1], 3)) for ft in fits]
    assert got == [(2, 1.0), (1, 1.0), (0, 1.0)], got
    # the offset column: 256/4 = 64 per the law
    fits = res["out_off"][1]
    assert abs(fits[0]["lo"] - 64) < 0.6, fits[0]
    # the negative row: V... U = 192 - G
    fits = res["out_neg"][1]
    assert abs(fits[1]["coef"][1] + 1) < 0.02 and abs(fits[1]["coef"][3] - 192) < 1, fits[1]["coef"]
    print("\n".join(lines))
    print("selftest OK")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "selftest":
        selftest()
    elif len(sys.argv) >= 2:
        analyse(parse(open(sys.argv[1], "rb").read()))
    else:
        print(__doc__)
