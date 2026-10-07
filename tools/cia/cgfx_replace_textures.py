"""Replaces ETC1 image textures in a CGFX/BCRES file by name (same size), e.g.
    python cgfx_replace_textures.py in.cgfx out.cgfx logoA=logoA.png logoAbg=logoAbg.png
PNGs are used as shown in CTR Studio exports (no flipping); alpha is ignored (ETC1 has none)."""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image
import etc1_3ds

src, dst = sys.argv[1], sys.argv[2]
want = dict(a.split('=', 1) for a in sys.argv[3:])
d = bytearray(open(src, 'rb').read())
u32 = lambda o: struct.unpack_from('<I', d, o)[0]
done = set()
for m in re.finditer(b'TXOB', d):
    p = m.start()
    if u32(p - 4) != 0x20000011:  # image texture (not a reference)
        continue
    np_ = p + 8 + u32(p + 8)
    name = bytes(d[np_:d.index(b'\0', np_)]).decode()
    if name not in want:
        continue
    fmt, h, w, size = u32(p + 0x30), u32(p + 0x38), u32(p + 0x3C), u32(p + 0x40)
    data = p + 0x44 + u32(p + 0x44)
    if fmt != 0x0C:
        sys.exit('%s: format %#x is not ETC1' % (name, fmt))
    im = Image.open(want[name])
    if im.size != (w, h):
        sys.exit('%s: the PNG is %dx%d, the texture is %dx%d' % (name, im.size[0], im.size[1], w, h))
    enc = etc1_3ds.encode(im)
    assert len(enc) == size
    d[data:data + size] = enc
    done.add(name)
    print('  replaced %s (%dx%d ETC1)' % (name, w, h))
missing = set(want) - done
if missing:
    sys.exit('textures not found: %s' % ', '.join(sorted(missing)))
open(dst, 'wb').write(d)
