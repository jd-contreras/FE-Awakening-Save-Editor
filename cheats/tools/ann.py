import struct,capstone,re,sys
D=open('G:/dev/Cheats/work/code.bin','rb').read();B=0x100000
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM); md.skipdata=True
def u32(a): return struct.unpack_from('<I',D,a-B)[0]
def cstr(a):
    try: e=D.index(b'\0',a-B); return D[a-B:e].decode('shift_jis','replace')
    except: return ''
def ann(a0,a1):
    for i in md.disasm(D[a0-B:a1-B],a0):
        s=f"{i.address:08X} {i.mnemonic} {i.op_str}"
        m=re.match(r'\w+, \[pc, #(0x[0-9a-f]+|\d+)\]$',i.op_str)
        if i.mnemonic=='ldr' and m:
            v=u32(i.address+8+int(m.group(1),0)); s+=f'   ; ={v:#x}'
            if 0x100000<v<0x4e8000:
                c=cstr(v)
                if c and c.isprintable(): s+=' "'+c[:30]+'"'
        m=re.match(r'\w+, pc, #(0x[0-9a-f]+|\d+)$',i.op_str)
        if i.mnemonic=='add' and m: s+='   ; "'+cstr(i.address+8+int(m.group(1),0))[:30]+'"'
        print(s)
if __name__=='__main__':
    a=int(sys.argv[1],16); ann(a-4*int(sys.argv[2]), a+4*int(sys.argv[3]))
