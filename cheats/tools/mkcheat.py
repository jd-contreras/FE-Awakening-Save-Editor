"""Assemble an ARM code cave + hook into Rosalina/AR cheat lines, checked against code.bin."""
import keystone, capstone, struct
D = open('G:/dev/Cheats/work/code.bin', 'rb').read(); B = 0x100000
ks = keystone.Ks(keystone.KS_ARCH_ARM, keystone.KS_MODE_ARM)
cs = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
def u32(a): return struct.unpack_from('<I', D, a - B)[0]
def asm(src, addr):
    enc, _ = ks.asm(src, addr)
    return [struct.unpack_from('<I', bytes(enc), i)[0] for i in range(0, len(enc), 4)]
def cheat(name, cave_addr, cave_src, hooks, note=None):
    """hooks: list of (addr, asm_src, expected_original_word)"""
    lines = [f'[{name}]', 'D3000000 00000000']
    if cave_src:
        words = asm(cave_src, cave_addr)
        for i, w in enumerate(words):
            a = cave_addr + 4 * i
            assert u32(a) == 0, f'cave not empty at {a:#x}'
            lines.append(f'{a:08X} {w:08X}')
    for a, src, orig in hooks:
        assert u32(a) == orig, f'hook {a:#x}: found {u32(a):08X}, expected {orig:08X}'
        w = asm(src, a)[0]
        lines.append(f'{a:08X} {w:08X}')
    if note: lines.append('*' + note)
    for l in lines[2:]:
        if l.startswith('*'): continue
        a, w = (int(x, 16) for x in l.split())
        i = next(cs.disasm(struct.pack('<I', w), a))
        print(f'   {l}   {i.mnemonic} {i.op_str}')
    return '\n'.join(lines) + '\n'
