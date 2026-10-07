import sys
def lz11(d, off=0):
    t = d[off]; assert t == 0x11, hex(t)
    size = d[off+1] | d[off+2] << 8 | d[off+3] << 16; p = off + 4
    if size == 0: size = int.from_bytes(d[p:p+4], 'little'); p += 4
    out = bytearray()
    while len(out) < size:
        flags = d[p]; p += 1
        for bit in range(7, -1, -1):
            if len(out) >= size: break
            if not (flags >> bit) & 1:
                out.append(d[p]); p += 1; continue
            b = d[p]; ind = b >> 4
            if ind == 0:
                n = (((b & 0xF) << 4) | (d[p+1] >> 4)) + 0x11
                disp = ((d[p+1] & 0xF) << 8 | d[p+2]) + 1; p += 3
            elif ind == 1:
                n = (((b & 0xF) << 12) | (d[p+1] << 4) | (d[p+2] >> 4)) + 0x111
                disp = ((d[p+2] & 0xF) << 8 | d[p+3]) + 1; p += 4
            else:
                n = ind + 1
                disp = ((b & 0xF) << 8 | d[p+1]) + 1; p += 2
            for _ in range(n): out.append(out[-disp])
    return bytes(out)
def decomp(path):
    d = open(path, 'rb').read()
    if d[0] == 0x13: return lz11(d, 4)
    return lz11(d, 0)
if __name__ == '__main__':
    for f in sys.argv[1:]:
        o = decomp(f); out = f.replace('.lz', '')
        open(out, 'wb').write(o); print(f, '->', out, hex(len(o)))
