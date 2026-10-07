"""Decompress a 3DS ExeFS .code (backwards LZ77)."""
import struct, sys
def blz_decompress(d):
    d = bytearray(d)
    top_bot, extra = struct.unpack_from('<II', d, len(d) - 8)
    enc_len = top_bot & 0xFFFFFF
    hdr_len = top_bot >> 24
    out = bytearray(len(d) + extra)
    out[:len(d)] = d
    src = len(d) - hdr_len
    dst = len(out)
    end = len(d) - enc_len
    while src > end:
        src -= 1; flags = d[src]
        for _ in range(8):
            if src <= end: break
            if flags & 0x80:
                src -= 2
                v = d[src] | (d[src + 1] << 8)
                n = (v >> 12) + 3
                disp = (v & 0xFFF) + 3
                for _ in range(n):
                    dst -= 1
                    out[dst] = out[dst + disp]
            else:
                src -= 1; dst -= 1
                out[dst] = d[src]
            flags = (flags << 1) & 0xFF
    return bytes(out)
if __name__ == '__main__':
    o = blz_decompress(open(sys.argv[1], 'rb').read())
    open(sys.argv[2], 'wb').write(o)
    print(hex(len(o)))
