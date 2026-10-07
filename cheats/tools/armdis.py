import sys, struct, capstone
D = open('G:/dev/Cheats/work/code.bin', 'rb').read()
BASE = 0x100000
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM); md.skipdata = True
def dis(a, n):
    for i in md.disasm(D[a - BASE:a - BASE + n * 4], a):
        print(f"{i.address:08X}  {struct.unpack_from('<I', D, i.address - BASE)[0]:08X}  {i.mnemonic} {i.op_str}")
if __name__ == '__main__':
    a = int(sys.argv[1], 16); before = int(sys.argv[2]) if len(sys.argv) > 2 else 16; after = int(sys.argv[3]) if len(sys.argv) > 3 else 24
    dis(a - before * 4, before + after)
