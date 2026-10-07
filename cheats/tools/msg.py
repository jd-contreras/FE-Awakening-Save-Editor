"""Fire Emblem Awakening message archive (m/E/*.bin.lz): label -> UTF-16 text."""
import struct, sys
sys.path.insert(0, 'G:/dev/Cheats/work')
import lz
def load(path):
    d = lz.decomp(path) if path.endswith('.lz') else open(path, 'rb').read()
    fs, ds, p1, p2 = struct.unpack_from('<4I', d, 0)
    DATA = 0x20; pt = DATA + ds; st = pt + p1 * 4 + p2 * 8
    out = {}
    for i in range(p2):
        tp, lab = struct.unpack_from('<II', d, pt + p1 * 4 + 8 * i)
        e = d.index(b'\0', st + lab); name = d[st + lab:e].decode('shift_jis', 'replace')
        a = DATA + struct.unpack_from('<I', d, DATA + tp)[0] if False else DATA + tp
        # text is UTF-16LE, zero terminated
        j = a
        while d[j:j + 2] != b'\0\0': j += 2
        out[name] = d[a:j].decode('utf-16le', 'replace')
    return out
