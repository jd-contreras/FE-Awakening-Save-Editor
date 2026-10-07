import struct
D=open('code.bin','rb').read(); B=0x100000
def u32(a): return struct.unpack_from('<I',D,a-B)[0]
def cstr(a):
    e=D.index(b'\0',a-B); raw=D[a-B:e]
    try: return raw.decode('shift_jis')
    except: return repr(raw)
TARGET=0x2f55b0
seen={}
for off in range(0,len(D)-4,4):
    w=struct.unpack_from('<I',D,off)[0]
    if (w>>24)==0xEB:
        imm=w&0xFFFFFF
        if imm&0x800000: imm-=0x1000000
        a=off+B
        if a+8+imm*4==TARGET:
            # find preceding 'add r0, pc, #imm' (E28F0xxx) within 6 instrs
            for k in range(1,7):
                p=u32(a-4*k)
                if (p&0xFFFFF000)==0xE28F0000:
                    rot=(p>>8)&0xF; v=p&0xFF; v=((v>>(2*rot))|(v<<(32-2*rot)))&0xFFFFFFFF
                    s=cstr(a-4*k+8+v); seen.setdefault(s,[]).append(hex(a)); break
for s,l in sorted(seen.items(), key=lambda x:-len(x[1])): print(len(l), s, l[:4])
