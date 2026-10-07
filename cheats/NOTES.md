# Awakening USA code.bin notes (00040000000A0500, v1.0)

`code.bin` = BLZ-decompressed ExeFS `.code` (work/blz.py). Text base 0x100000, size 0x3E8000.
Helpers: work/armdis.py (capstone), work/attrs.py (ISID_ attribute lookups).
Checked against ymyn's USA codes: all hook sites hold the expected original instructions.

## Items
- 0x2EBDD4 `UseItem(item*)`: item = [u16 id][u8 uses]; item data = table[id] (0x38 bytes, u64 attr
  flags at +0). If ISID_無限回数 set -> return 0, else uses-- and return 1 when it hits 0.
  Infinite Durability: 002EBE1C 1A000006 (bne) -> EA000006 (b). 10 callers (battle, staves, items).
- 0x2F55B0 / 0x302FC4: look up an ISID_/SID_ attribute by name; 0x308E50 builds the bit mask.

## Characters / supports
- 0x502738: 4 pointers to loaded `data/person/%s.bin.lz` blocks (loader around 0x2F0010).
  0x2D0B34: handle -> block[h>>12] + 8 + (h & 0xFFF) * 0x238. Person record = 0x238 bytes.
  +0x08 PID string pointer. Support entries are 8 bytes: [partner PID ptr][C,B,A,S thresholds].
- The pasted support cheats (B0502738 ...) match this layout, so they are USA codes.
- 0x2EFB34 -> 0x2BE6B0: person lookup by PID string. 0x2DE558: class lookup by JID string.
- 0x2BE73C IsAvatar(person): person == PID_プレイヤー男 or PID_プレイヤー女.
- 0x27DA40 per-unit gender fix-up from the unit's own logbook (+0x74 -> +0x1C gender):
  female -> set SID_女性, person M avatar -> F avatar, JID_戦術師男 -> 女; male -> the reverse.
- 0x2F2484 unit init: if IsAvatar(person) -> person = PID_プレイヤー援軍 (generic reinforcement
  Avatar). Callers not traced yet (likely StreetPass/logbook/team recruits).
- 0x20A958 support conversation file name: strip "PID_" from both units' persons (unit+0x64),
  "%s_%s%s" with suffix "" / "_親子" (parent-child) / "_兄弟" (siblings), then "m/%s.bin.lz";
  if missing, swaps the two names. Called from 0x20A3E8.
- Hardware result so far (docs/SAVE_FORMAT.md §15 of fea-save-editor): added (F) Robin's scenes skip
  while the main Avatar is male; setting the main Avatar female makes them play.
- Open: find where the main Avatar's gender overrides an added Avatar (needs the romfs m/ file list
  and data/person files).

## Strings of interest
- MID_支援_%s_%s (0x1B7380, 0x20A670) support message labels.
- Reliance = support internally: data/RelianceList.bin.lz, event::ProcReliance, sortieRelianceMenu.

## Added 2026-10-02 (second pass)
- Support conversation label suffix: 0x2C6960 appends _PCM1/_PCF1 (table 0x4D650C: 3 M, 3 F) chosen
  by the MAIN Avatar's logbook gender (0x2F1A10 main unit, logbook +0x74, gender +0x1C). Female
  files only have _PCF labels -> scenes skip. Fix: hook 0x2C69C8 -> cave 0x459B90 picks M/F by the
  label text (ー男 / ー女). Thabes' mod keeps the same label scheme.
- Final hit: 0x3EEB98 hit(+0x38) - target avoid(+0x3C), clamp 0..100 -> +0x24. Crit -> +0x28.
  Always hit: hook 0x3EEBB8 -> cave 0x459BF0 (player force [unit+0x6C]+8 == 0).
- Staff targets: 0x26F030(user, staff, target): side check 0x26F178, full-HP checks 0x26F1A0/0x26F1C8.
- Supports: person +0x88, 52 entries x 8 [PID ptr][C B A S]; 99 = none. 0x2AFF34 FindSupport.
  Marry anyone: hook 0x2AFF78 -> cave 0x459C20 sets S = A+5 when S is 99.
- static.bin: record i at data 8 + i*0x238 (runtime block same). Thabes' mod: 67 persons, new 52-61
  (Mustafa, Phila, Morgan2 M/F, Raimi, Fels, VASTO, GRIMA, GRIMAF, FAUDER), Pr. Marth = 62 (vanilla 52).
- Caves used: 0x459B90-0x459BE0, 0x459BF0-0x459C18, 0x459C20-0x459C40 (ymyn: 0x459CC0+).
