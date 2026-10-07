import sys; sys.path.insert(0, 'G:/dev/Cheats/work')
from person import PersonFile
V = PersonFile('G:/dev/Cheats/work/romfs/static.bin')
BS = chr(92)
def esc(s):
    return ''.join(chr(b) if 32 <= b < 127 and b not in (34, 92) else BS + '%03o' % b for b in s.encode('shift_jis'))
lines = ['// Generated from the vanilla (USA v1.0) data/person/static.bin: the PID (Shift-JIS) of each',
         '// character id. Romfs mods are matched to the built-in tables by PID.',
         '#include "core/femod.h"', '', 'const char *const fe_vanilla_pids[FE_CHAR_COUNT] = {']
for i in range(V.count):
    lines.append('    "%s",  // %d' % (esc(V.pid(i)), i))
lines.append('};')
open('G:/dev/fea-save-editor/source/core/femod_pids.c', 'w', encoding='ascii', newline='\n').write('\n'.join(lines) + '\n')
