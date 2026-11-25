#!/usr/bin/env python3
"""Extract the sysmodule's main NSO (text, rodata, data - LZ4-decompressed)
from a release's switch.zip, to check what was built before publishing:

    python3 tools/release_nso.py dist/switch-frame-tap-X-switch.zip /tmp/main.bin
    strings -n 8 /tmp/main.bin | grep "applet-mitm v"
"""
import struct
import sys
import zipfile


def lz4_block(src, size):
    out = bytearray()
    i = 0
    while i < len(src):
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255:
                    break
        out += src[i:i + lit]; i += lit
        if i >= len(src):
            break
        off = src[i] | src[i + 1] << 8; i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255:
                    break
        ml += 4
        st = len(out) - off
        for k in range(ml):
            out.append(out[st + k])
    if len(out) != size:
        raise ValueError(f'LZ4 segment: {len(out)} bytes, expected {size}')
    return bytes(out)


def main():
    nsp = zipfile.ZipFile(sys.argv[1]).read('atmosphere/contents/0100000000000C20/exefs.nsp')
    if nsp[:4] != b'PFS0':
        sys.exit('not a PFS0')
    n, strsz = struct.unpack_from('<II', nsp, 4)
    ents = [struct.unpack_from('<QQII', nsp, 16 + 24 * k) for k in range(n)]
    strs = nsp[16 + 24 * n:16 + 24 * n + strsz]
    base = 16 + 24 * n + strsz
    for off, sz, so, _ in ents:
        if strs[so:strs.index(b'\0', so)] != b'main':
            continue
        nso = nsp[base + off:base + off + sz]
        flags = struct.unpack_from('<I', nso, 0xC)[0]
        out = bytearray()
        for s in range(3):
            foff, moff, dsz = struct.unpack_from('<III', nso, 0x10 + 0x10 * s)
            csz = struct.unpack_from('<I', nso, 0x60 + 4 * s)[0]
            data = nso[foff:foff + csz]
            if flags & (1 << s):
                data = lz4_block(data, dsz)
            if len(out) < moff:
                out += bytes(moff - len(out))
            out[moff:moff + dsz] = data
        open(sys.argv[2], 'wb').write(out)
        print('main:', len(out), 'bytes')
        return
    sys.exit('no main in exefs.nsp')


if __name__ == '__main__':
    main()
