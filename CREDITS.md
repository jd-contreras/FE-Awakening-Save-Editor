# Credits and sources

Where the information behind the save editor and the cheats came from.

## Cheats (Fire Emblem Awakening USA, 00040000000A0500)

- **ymyn** (GBAtemp), "Gateway Cheats" thread: the EUR and USA codes for Move Range Max,
  Anywhere Starting Position, Instant Support, Critical Rate Max, Able to Attack Ally,
  Proficient Max, Forge Any Weapon, Bookmark Does Not Disappear, Growth rate, ALL Stats Up,
  Inf HP, EXP Multiplier, Control Enemy, Infinite Movement, item trade, enemy item drop,
  convoy, forging codes.
  - USA list: https://gbatemp.net/threads/gateway-cheats.402900/page-217 (post #4,327)
  - EUR list: https://gbatemp.net/threads/gateway-cheats.402900/page-215 (post #4,295)
  - Earlier USA posts: https://gbatemp.net/threads/gateway-cheats.402900/page-134 (posts #2,663, #2,665)
- **JourneyOver**, CTRPF / Action Replay cheat database (checked for existing Awakening codes):
  https://github.com/JourneyOver/CTRPF-AR-CHEAT-CODES
- Support cheats (Male and Female Avatar can support, Chrom and Lissa can support Emmeryn,
  Chrom x Cordelia) and the Avatar slot codes: original author not recorded yet.
- New codes found for this project by disassembling the USA `code.bin` (notes in cheats/NOTES.md):
  Infinite Durability, Avatar Support Conversations Follow Each Avatar, Player Always Hits,
  Heal or Rescue Anyone, Any Support Pair Can Reach S, and the rebuilt Avatar support code.
- **Thabes**, romfs mod: its `data/person/static.bin` and
  messages were checked so the support codes don't overwrite the mod's own supports.

## Save editor

- Awakening save layout and stat tables: **Danius88**, fire-editor-awakening (GPLv3),
  https://github.com/Danius88/fire-editor-awakening, after **Olmectron**'s FireEditor
  (https://gbatemp.net/threads/release-fire-editor-fire-emblem-awakenings-save-editor.397493/).
- Huffman encoder layout: **CUE**'s huffman.c via **SciresM**'s FEAST, https://github.com/SciresM/FEAST
- Convoy format: **RainThunder**, Fire Emblem 3DS Convoy Editor,
  https://github.com/RainThunder/Fire-Emblem-3DS-Convoy-Editor (also FEST, https://github.com/RainThunder/FEST)
- Avatar stat formulas checked against Serenes Forest (https://serenesforest.net/awakening/) and
  Fire Emblem Wiki (https://fireemblemwiki.org/wiki/Robin/Stats).

## Tools

- devkitPro, libctru, citro2d: https://devkitpro.org
- makerom (Project_CTR, **3DSGuy**): https://github.com/3DSGuy/Project_CTR
- bannertool (diasurgical fork): https://github.com/diasurgical/bannertool
- Luma3DS / Rosalina cheat engine: https://github.com/LumaTeam/Luma3DS
- GodMode9 (dumping `.code`): https://github.com/d0k3/GodMode9
- Capstone disassembler: https://www.capstone-engine.org
- Keystone assembler (encoding the new code caves): https://www.keystone-engine.org
- Paragon (Fire Emblem 3DS modding toolkit, used to extract the romfs for research)
- CTR Studio (**KillzXGaming**): editing the 3D HOME Menu banner model (CGFX/BCRES)
- etcpak (ETC1 encoding) and texture2ddecoder (ETC1 decoding) for the banner texture tool
