import struct
class PersonFile:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        fs, ds, p1, p2 = struct.unpack_from('<4I', d, 0)
        self.DATA = 0x20; self.pt = 0x20 + ds; self.st = self.pt + p1 * 4 + p2 * 8
        self.ptrfields = set(struct.unpack_from('<I', d, self.pt + 4 * i)[0] for i in range(p1))
        self.count = self.u32(4)
        self.recs = []
        for i in range(self.count):
            off = 8 + i * 0x238
            self.recs.append(off)
    def u32(self, off): return struct.unpack_from('<I', self.d, self.DATA + off)[0]
    def str_at(self, v):
        a = self.DATA + v
        if not (self.st <= a < len(self.d)): return None
        e = self.d.index(b'\0', a); return self.d[a:e].decode('shift_jis', 'replace')
    def pid(self, i): return self.str_at(self.u32(self.recs[i] + 8))
    def pid_strptr(self, i): return self.u32(self.recs[i] + 8)
    def supports(self, i):
        out = []
        for k in range(52):
            o = self.recs[i] + 0x88 + 8 * k
            if o in self.ptrfields:
                v = self.u32(o); t = self.d[self.DATA + o + 4:self.DATA + o + 8]
                out.append((k, o, self.str_at(v), list(t)))
            else:
                out.append((k, o, None, list(self.d[self.DATA + o + 4:self.DATA + o + 8])))
        return out
    def find(self, pid):
        for i in range(self.count):
            if self.pid(i) == pid: return i
