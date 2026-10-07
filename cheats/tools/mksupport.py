"""Build Rosalina support cheats that detect vanilla (57 characters) vs Thabes' mod (67) at runtime."""
import sys; sys.path.insert(0, 'G:/dev/Cheats/work')
from person import PersonFile
V = PersonFile('G:/dev/Cheats/work/romfs/static.bin')
U = PersonFile('G:/dev/Cheats/work/mod/static.bin')
GUARD = ['60502738 00000000', 'B0502738 00000000']        # character data loaded -> offset = its base
DETECT = {'V': '50000004 00000039', 'U': '50000004 00000043'}  # character count 57 / 67

def entries_block(f, ver, pairs):
    """pairs: list of (person_pid, slot, partner_pid, thresholds 4 bytes). Checks slot is free."""
    out = GUARD + [DETECT[ver]]
    for pid, slot, partner, thr in pairs:
        i = f.find(pid); k = slot
        e = f.supports(i)[k]
        assert e[2] is None, f'{ver}: {pid} slot {k} is used by {e[2]}'
        off = f.recs[i] + 0x88 + 8 * k
        word = thr[0] | thr[1] << 8 | thr[2] << 16 | thr[3] << 24
        K = f.pid_strptr(f.find(partner)) - f.pid_strptr(0)
        out += [f'{off + 4:08X} {word:08X}', 'D9000000 00000010']
        if K: out.append(f'D4000000 {K:08X}')
        out += [f'D6000000 {off:08X}', 'DC000000 FFFFFFFC']
    out.append('D2000000 00000000')
    return out

def wrap_original(lines, ver='V'):
    """Add the loaded-check and version check to a pasted B0502738 cheat."""
    out = []
    for l in lines:
        if l == 'B0502738 00000000':
            out += GUARD + [DETECT[ver]]
        else:
            out.append(l)
    return out

def cheat(title, body, note=None):
    s = f'[{title}]\nD3000000 00000000\n' + '\n'.join(body) + '\n'
    if note: s += '*' + note + '\n'
    return s
