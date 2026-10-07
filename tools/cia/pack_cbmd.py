"""Builds a 3DS banner (CBMD) from CGFX models and a BCWAV jingle.

    python pack_cbmd.py <folder> <out.bnr>

<folder> holds common.cgfx (required), optional region10.cgfx .. region13.cgfx (per-language
versions; the slot number is the CBMD region index, 10-13 = USA English/French/Spanish/Portuguese)
and banner.bcwav (the jingle). CGFX files are LZ11-compressed into the banner. Also accepts the
.bcres extension (CTR Studio saves CGFX as .bcres).
"""
import os
import struct
import sys


def lz11_compress(src):
    """Greedy LZ11 (Nintendo) compressor."""
    out = bytearray([0x11, len(src) & 0xFF, (len(src) >> 8) & 0xFF, (len(src) >> 16) & 0xFF])
    if len(src) > 0xFFFFFF:
        raise ValueError('too large for LZ11')
    heads = {}
    i, n = 0, len(src)
    while i < n:
        flag_pos = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= n:
                break
            best_len, best_disp = 0, 0
            if i + 3 <= n:
                key = src[i:i + 3]
                for j in reversed(heads.get(key, [])[-64:]):
                    disp = i - j
                    if disp > 0x1000:
                        break
                    length = 3
                    limit = min(0x10110, n - i)
                    while length < limit and src[j + length] == src[i + length]:
                        length += 1
                    if length > best_len:
                        best_len, best_disp = length, disp
                        if length == limit:
                            break
            if best_len >= 3:
                flags |= 0x80 >> bit
                d = best_disp - 1
                if best_len <= 0x10:
                    out += bytes([((best_len - 1) << 4) | (d >> 8), d & 0xFF])
                elif best_len <= 0x110:
                    l = best_len - 0x11
                    out += bytes([l >> 4, ((l & 0xF) << 4) | (d >> 8), d & 0xFF])
                else:
                    l = best_len - 0x111
                    out += bytes([0x10 | (l >> 12), (l >> 4) & 0xFF, ((l & 0xF) << 4) | (d >> 8), d & 0xFF])
                step = best_len
            else:
                out.append(src[i])
                step = 1
            for k in range(i, min(i + step, n - 2)):
                heads.setdefault(src[k:k + 3], []).append(k)
            i += step
        out[flag_pos] = flags
    while len(out) % 4:
        out.append(0)
    return bytes(out)


def find(folder, base):
    for ext in ('.cgfx', '.bcres'):
        p = os.path.join(folder, base + ext)
        if os.path.exists(p):
            return p
    return None


def main():
    folder, out_path = sys.argv[1], sys.argv[2]
    common = find(folder, 'common')
    if not common:
        sys.exit('pack_cbmd: %s/common.cgfx is missing' % folder)
    models = {0: common}
    for r in range(1, 14):
        p = find(folder, 'region%02d' % r)
        if p:
            models[r] = p
    header = bytearray(0x88)
    header[0:4] = b'CBMD'
    body = bytearray()
    for r in sorted(models):
        cg = open(models[r], 'rb').read()
        if cg[:4] != b'CGFX':
            sys.exit('pack_cbmd: %s is not a CGFX file' % models[r])
        struct.pack_into('<I', header, 0x08 + 4 * r, 0x88 + len(body))
        body += lz11_compress(cg)
        print('  %-10s %s (%d bytes)' % ('common' if r == 0 else 'region%02d' % r, os.path.basename(models[r]), len(cg)))
    wav = os.path.join(folder, 'banner.bcwav')
    if os.path.exists(wav):
        struct.pack_into('<I', header, 0x84, 0x88 + len(body))
        body += open(wav, 'rb').read()
    open(out_path, 'wb').write(bytes(header) + bytes(body))
    print('  banner: %s (%d bytes)' % (out_path, 0x88 + len(body)))


if __name__ == '__main__':
    main()
