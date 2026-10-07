# Fire Emblem Awakening save format

All multi-byte values are little-endian. **Confirmed** means checked against real
USA save dumps (Chapter0/1/2 + Map0, 2026-10-01) by `tests/host_test.c`.
**Reference only** means it comes from community code and hasn't been checked independently.

Sources:
- SciresM/FEAST (GPLv2): Huffman-8 codec, COMP header, CRC32
- RainThunder/FEST (GPLv2): single-file variant of FEAST
- RainThunder/Fire-Emblem-3DS-Convoy-Editor (GPLv2): block lookup via index table
- Danius88/fire-editor-awakening (GPLv3): open Java rewrite of Olmectron's FireEditor,
  with full unit/user block parsing and the character/class tables in `tools/data/`

## 1. Title & save archive

| Region | Title ID | Product code | Status |
|---|---|---|---|
| USA | `00040000000A0500` | CTR-P-AFEE | confirmed (user's copy) |
| EUR | `000400000009F100` | CTR-P-AFEP | reference only |
| JPN | ? | CTR-P-AFEJ | found by product code; header differs (see below) |

The app finds the game by product code (`CTR-?-AFE?`) on SD and cartridge, only
accepting base applications (title ID high word `00040000`; updates and DLC share the code).

Archive: `FSUSER_OpenArchive(ARCHIVE_USER_SAVEDATA, {mediatype, lowID, highID})`.
Writes must be followed by `FSUSER_ControlArchive(..., ARCHIVE_ACTION_COMMIT_SAVE_DATA)`.

Files in the archive root:
- `Chapter0`, `Chapter1`, `Chapter2`: the three save slots (**what v1 edits**)
- `Map0`, `Map1`: suspend / battle saves (same container, 2 extra blocks)
- `Global`: shared data, header-less variant

## 2. Container: confirmed

```
0x000  header          0xC0 bytes (US/EU), 0x80 bytes (JP)      [kept verbatim]
+0x00  "PMOC"          bytes 50 4D 4F 43
+0x04  u32 = 2         Huffman-8
+0x08  u32             decompressed payload length
+0x0C  u32             CRC32 (IEEE/zlib) over header || decompressed payload
+0x10  Huffman-8 stream: u32 0x28 | len<<8, tree, 32-bit LE words read MSB first
```

Our encoder (CUE/FEAST layout) produces the **same size and the same bitstream**
as the game's; only the node order inside the tree table differs.

## 3. Decompressed layout: confirmed

At `hdr` (0xC0) is `EDNI` ("INDE"), then u32 absolute offsets of each block.
**Each offset points directly at the block's 4-byte magic** (FireEditor's
"magic - 4" is an artifact of its search helper).

| Index slot | Chapter file | Map file |
|---|---|---|
| +0x04 | USER `RESU` | PERS `SREP` |
| +0x08 | GMAP `PAMG` | USER |
| +0x0C | UNIT `TINU` | GMAP |
| +0x10 | REFI `IFER` | UNIT |
| +0x14 | TRAN `NART` | REFI |
| +0x18 | DU26 `62UD` | TRAN |
| +0x1C | EVST `TSVE` | MAP `PAM ` |
| +0x20 | 0 | DU26 |
| +0x24 | 0 | EVST (non-zero here means Map file) |

v1 only patches fixed-size fields in place, so block sizes and the index never change.

## 4. Gold (USER block): confirmed

```
USER+0x00  magic + 1 byte
USER+0x05  0x10 bytes (0x11 on JP)
USER+0x15  u8 chapterCount, then chapterCount * 0x10 progress records
next       tail block:  +0x69 u32 gold, +0x6D u32 renown
```
chapterCount must equal (USER size - 0xEE) / 0x10. Checked on load.

## 5. Units (UNIT block): confirmed

```
UNIT+0x00  magic + 1 byte
then groups: u8 groupId, u8 count, records...   (empty groups omitted)
  0 deployed (blue), 1 enemy, 2 other, 3 army, 4 fallen, 5 other
then exactly one 0xFF byte (the loader refuses the save otherwise)
```

| Offset | Size | Part |
|---|---|---|
| 0x00 | 0x1A | block1 |
| 0x1A | 0x19 | inventory |
| 0x33 | 0x10 | equipped skills (5 × u16, low byte = id) / weapon ranks |
| 0x43 | 1+n  | supports (n = first byte) |
| …    | 1+m  | unknown (m = first byte) |
| …    | 0x2A + 0x0D + 0x3F | flags, learned skills, AI |
| …    | opt 0x188 | logbook trailer (US/EU; 0xFC JP) when fixed part ends `01 06` |
| …    | opt 0x25  | child trailer when fixed part ends `00 01`, or logbook's last byte is `01` |

Fixed part = 0xBB + n + m.

block1: `+0x01` u16 character, `+0x03` class, `+0x0A..0x11` stat **gains**,
`+0x12` level, `+0x13` EXP, `+0x14` current HP, `+0x15` Boots movement.

Logbook trailer: `+0x00` UTF-16 name (0x1A bytes), `+0x1A` asset, `+0x1B` flaw.
Child trailer: father record at +0x01, mother at +0x12; each has 3 entries of
5 bytes from +0x02 (u16 id, asset, flaw, extra): parent, grandfather, grandmother.

## 6. Displayed stats: formula confirmed on base units, children unverified

**Hardware test 2026-10-02:** gold and stat edits on Chrom, Lissa, Lon'qu, Donnel and
others showed up exactly in game. Two corrections came out of it:

1. Avatar asset/flaw base changes are HP +5/−3, Luck +4/−2, others +2/−1 (game table,
   fireemblemwiki Robin/Stats). FireEditor's +2/−1 made an HP-flaw Robin read 2 HP high.
2. The stat screen includes bonuses stored per unit: flags+0x1A Pure Water (Res),
   +0x1C..0x26 rally/map effects, +0x27 tonic bits (bit i = stat i; +5 HP / +2 other),
   +0x28 barracks bits (+4), plus equipped stat skills (1 HP+5, 2–7 +2, 53 Lck+4,
   99 Res+10, 88 all+2). Lon'qu showed Str 12 after being set to 10 (a +2 bonus).
   Weapon stat bonuses are not modelled yet.

**Children (hardware test 2026-10-02, slot 2, a modded save):** Str–Res and caps
confirmed (Lucina all-capped shows all green; Morgan M/F values match and aren't green).
**Max HP is NOT confirmed:** we compute 60 for both, the game shows Lucina 60/50 and
Morgan 60/55. FireEditor's child "additions" (Lucina HP 12, Morgan 9) are recruitment bases,
and HP is the stat that disagrees. The save was edited by another tool (current HP 60 >
max is impossible in normal play), so this needs a clean save with a child unit.
The UI marks child HP with "?".

**Game mods (found 2026-10-02):** the tester's Luma setup had a game mod for Awakening
that changes base stats. With Luma "game patching" off, Bond (asset Mag, flaw Def) shows
exactly the computed 25/14/13/8/12/9/6/7; with it on, Str and Mag are +1. The mismatches
seen on Robin, Lon'qu and Donnel (and possibly children's HP) were measured with the mod on,
so they're not evidence against the formula. The app now checks for
`sdmc:/luma/titles/<TID>/` and warns that it shows unmodded stats.

**Asset/flaw base values confirmed (game patching off):** fresh Avatar, asset Luck /
flaw Skill, Lv1: 19/6/5/4/6/8/6/4 = prologue base 19/6/5/5/6/4/6/4 with Lck +4, Skl −1,
matching the wiki table. Also confirmed: HP flaw −3 (Robin), Mag asset / Def flaw (Bond),
Spd asset / HP flaw incl. caps (Robin, slot 2), child stats + caps (Lucina, Morgan M/F),
Limit Breaker (skill 91) adding +10 to non-HP caps. Every rule in section 6 is now verified
on hardware.

Self-check in `tests/host_test.c`: units in chapter saves are at full HP, so stored
current HP must equal the computed max HP. 67/68 units across the dumps match; the
exception is Lucina in a cheat-edited all-Dancer save (+5).

```
base      = character additions + class base   (Avatar/logbook: asset/flaw values above)
displayed = min(base + gain, cap)
cap       = class max + modifiers (+10 non-HP with Limit Breaker, skill 91, equipped)
modifiers = character modifiers (+ asset/flaw table for Avatar/logbook)
          children: own + both parents + parents' asset/flaw (+ grandparents if valid) + 1; HP modifier 0
```

Check: Chrom in Chapter2, Lord (M): 18 + 2 + 9 gain = 29, matching the stored current HP of 29.

**Deviation from FireEditor:** its child-modifier code has three copy-paste slips
(flaw index 4 used with asset 3, flaw 3 with asset 4, and mother's father counted
twice). This port uses the symmetric formula. **Verify a child unit's caps in game.**

Setting a stat writes `gain = target - base`, with the target clamped to base..cap.
Level caps: 30 for Dancer, Manakete, Taguel, Villager, Lodestar, Grima,
Dread Fighter, Bride; otherwise 20. Gold is clamped to 999,999.

## 7. Skills: confirmed on real saves and on hardware

- **Equipped:** 5 slots at unit +0x33, 2 bytes each (skill id in the low byte, high byte 0).
  Filled slots must come first; an empty slot before a filled one glitches the in-game menu
  (FireEditor note), so the editor always re-packs them.
- **Learned:** 13 bytes immediately after the 0x2A-byte flags block. Skill *n* is bit
  *n % 8* of byte *n / 8*. Ids 1–102 are real skills (0 = none, 103 = unused).
  The in-game skill menu offers every learned skill, so learning a skill is what makes it
  swappable in game.
- Check on the dumps: 0 equipped-but-not-learned slots in every normal save (53 units).
  The cheat-edited all-Dancer save has 40, which is how a skill gets "lost" after
  unequipping in game. The editor always marks equipped skills as learned.
- Names: `tools/data/skills.xml` (FireEditor), generated into `fe_skills[]`.
- **Hardware test 2026-10-02:** Chrom (slot 3) with Luna equipped and Hawkeye/Luna+/Vantage+/
  Pavise+/Aegis+ learned: the game shows Luna in slot 3 and lists the learned skills under
  "Available Skills" in Equip Skills, so they can be swapped in game.

## 8. Classes

- Unit +0x03 is the class id (0–82, `tools/data/classes.xml`). Class flags used by the UI:
  0 = female class, 3 = promoted, 21 = enemy-only. **The editor allows every class**
  (gender/enemy classes are only tagged, by user request).
- Changing the class keeps the stored level-up gains, so stats shift by the class base
  difference, like an in-game reclass. The level is clamped to the new cap.
- **Confirmed on hardware 2026-10-02**, including the enemy-only Grima class on a player unit.

## 9. Supports

- Unit +0x43: u8 n, then n bytes of support points, one per partner in
  `fe_supports[char]` order (`units.xml` `<supports>`). **n is often shorter than the
  partner list** (13/18 units in slot 3); missing partners count as 0.
- Values are stored on both units of a pair. The unmodded saves are 100% symmetric under
  FireEditor's partner order, which confirms the mapping. The modded saves have longer
  blocks (Chrom 25–35 vs 10 partners): the mod changes the support tables.
- Raising a support past n grows the unit record. `fe_support_set` inserts the bytes,
  shifts every block-index entry after the insertion point, re-parses the whole layout on a
  copy and only then swaps it in (tested: all other data byte-identical).
- **Confirmed on hardware 2026-10-02:** "ready" points (C 3 for non-romantic: Frederick-Bond
  showed "!"), editor-set pairs showed "!" in game, and a grown support block (save got
  bigger) loaded and played fine.
- Rank names use only the "ready" values: non-romantic 3/9/17, slow 4/9/15/21, medium
  3/8/13/19, fast 2/7/12/17 (C/B/A/S). FireEditor's table (C-pend/C/B-pend/B/A-pend/A/
  S-pend/S) per type: non-romantic 3,4,9,16,17,18; slow 4,5,9,16,15,16,21,22 (inconsistent:
  B 16 > A-pending 15); medium 3,4,8,9,13,14,19,20; fast 2,3,7,8,12,13,17,18.
- **Stages the editor steps through:** - > C ready > C > B ready > B > A ready > A (> S ready > S).
  A plain rank is ready + 1 (the value after watching), so it is set without a conversation.
  Setting S directly lets a unit have several S supports (experimental, by user request).
- **Marriage guard (hardware finding 2026-10-02):** watching an S conversation while either
  unit is already married undoes that marriage and allows a remarriage. With the guard ON
  (default, `sdmc:/3ds/FEAEditor/settings.cfg`, toggle X on the slot screen), opening a slot
  lowers such "S ready" pairs to A (ready + 1) on both sides and lists them; the user must
  save to keep it. The Supports screen only warns, never blocks.

## 10. Renown

- u32 right after gold (USER tail +0x6D). Max 99,999 (slot 3 holds exactly that).
  Values seen: 10, 140, 390, 99,999. Not yet confirmed on hardware.

## 11. Inventory, forged weapons, convoy (confirmed on real saves; hardware test pending)

- **Unit inventory:** 5 slots × 5 bytes at unit +0x1A: `04`, u16 item id, uses, flags
  (0x10 equipped, 0x20 enemy drop). Unbreakable items store 0 uses (Falchion). The editor
  keeps slots packed and moves an equipped item to the top, like the game.
- **Items:** ids 0–201 (`tools/data/items.xml`, `fe_items[]`); ids ≥ 202 are forged weapons:
  forge slot = id − 202.
- **REFI (forges):** magic + count at +5, records of 0x2C bytes from +7 (US): +0 slot,
  +2 UTF-16 name, +0x26 u16 base weapon id, +0x28 might/hit/crit bonuses. Slot 2's first
  forge is "Rexcalibur".
- **TRAN (convoy):** magic, +5 u16 count (352 = 202 items + 150 forge slots), then one u16
  per entry = **total uses** held (one Bronze Sword = 50). Fixed size, so editing never
  grows the save.

## 12. Avatar look & name (Extras)

Logbook trailer (Avatar, logbook units, DLC heroes), offsets from its start:
`+0x00` UTF-16 name (12 chars + 0), `+0x1A` asset, `+0x1B` flaw, `+0x1C` gender,
`+0x1D` build 0–2, `+0x1E` face, `+0x1F` hairstyle 0–4, `+0x20` hair color R,G,B,A,
`+0x24` voice 0–4, `+0x25` birthday day, `+0x26` month. Checked on 8 real logbook units.

- **Face:** 0–4 are the creator's faces. DLC heroes use the Avatar model with a special face
  id: 11, 16, 24, 56, 67, 72, 80, 91, 96, 105, 114, 121, 128, 136, 144, 155 (FireEditor).
  The tester's old save has the DLC unit "Pr. Marth" with face 11, which confirms the
  mechanism: setting a hero face id on the Avatar gives it that hero's head. Arbitrary
  characters (Chrom etc.) use full models of their own and cannot be selected this way.
- **Name** is also stored in the slot header at 0x10 (0x1A bytes, shown on the game's
  file-select screen). Renaming the main Avatar updates both, but only if they matched.
- Gender here changes the body/voice set only; supports follow character id 0/1.
- Not yet confirmed on hardware.
- **Hero look presets:** `tools/data/einherjar.xml` (FireEditor) lists 138 SpotPass/DLC hero
  units with gender, build, face, hairstyle, voice and hair color. 16 of them have unique hero
  heads: 11 Marth, 16 Roy, 24 Micaiah, 56 Leif, 67 Alm, 72 Seliph, 80 Elincia, 91 Eirika,
  96 Lyn, 105 Ephraim, 114 Celica, 121 Ike, 128 Palla, 136 Catria, 144 Est, 155 Katarina.
  The Pr. Marth preset (face 11, hair 0, build 1, voice 4, #325AB4) matches the real DLC
  unit in the tester's save byte for byte (checked in host_test).
- **Hardware test 2026-10-02 (face/hair):** setting face 11 changed the **portrait** to Marth,
  but the **3D model** kept the default head and the old hair color.
  - **Hair color has a second copy** in the unit's main record (AI block +0x39, RGBA). It equals
    the logbook color on every Avatar in the test saves, including Pr. Marth (#325AB4). The
    editor now writes both.
  - **Hero identity:** Pr. Marth's logbook has id byte 0xC9 (= 201, his entry in
    einherjar.xml) at mainBlock 0x0D and bit 1 set at mainBlock 0x1D; Robin has a random
    13-byte personal id and the bit clear. The 3D head most likely follows this identity. The
    editor offers it as an experimental option, saving the Avatar's own id to
    `sdmc:/3ds/FEAEditor/avatar_ids/` first so it can be restored. **Tested: no effect on the
    3D head.** The DLC unit's 3D head follows its character id (Pr. Marth = 62, a DLC hero
    character); the Avatar is character 0, so its 3D head cannot be swapped by save editing
    without turning it into that unit. The editor now only offers restoring the own id.
  - **Confirmed working:** hair color (both copies) changes the 3D hair; hero face changes the
    portrait.
  - Roy's preset showed a **silhouette** portrait, consistent with the Roy DLC not being
    installed; hero heads appear to need their DLC.

## 13. Global file: Support Log, Unit Gallery, Game Clear (confirmed on the real file)

- `Global` has no 0xC0 header: COMP block at offset 0; CRC32 over the decompressed data.
- Decompressed: `EDNI` index (u32 offsets: +4 USER, +8 next block `81GD`), then USER (`RESU`):
  +4 13 bytes of flags/settings. **+5 bit 1 = "Game Clear"** (Support Log, Theater and Unit
  Gallery menus appear). Then three bit lists, each `u32 count` + `count/8+1` bytes:
  Unit Gallery (64 bits in the real file), Support Log (1920), hair-color flags (64).
- Tester's file: Game Clear off, 39 support conversations, 32 gallery units.
- "Complete" (as FireEditor): set the first 53 gallery bits, 52 hair-color flags and 1830
  support-log bits. Sizes never change. The editor writes Global together with the slot on
  START (after the full backup). Not yet confirmed on hardware.
- **Undo:** turning "Complete" on first writes the decompressed Global to
  `sdmc:/3ds/FEAEditor/global_undo.bin`; turning it off copies that copy's Support Log,
  Gallery and hair-color bits back (the Game Clear flag keeps its current setting). "Restore
  from a backup" copies the same lists **and** the Game Clear flag from any automatic backup's
  Global. Tested on the real file: complete then undo gives the original bytes back exactly.
- The Support Log can't be rebuilt from a slot: its 1830 entries don't line up with the
  partner lists (316 pairs = 1170 C/B/A/S conversations), so the bit-to-conversation mapping
  is unknown.

## 14. Adding units (Extras → Add a unit)

- Model: the tester's slot 2 has both Avatar (M) (char 0, with logbook) and Avatar (F) (char 1,
  **no** logbook, plain record), plus Morgan (M)/(F) whose child blocks list Avatar (M) and
  Avatar (F) as parents. The tester reports these units work in the unmodded game.
- `fe_unit_add` builds a level-1 recruit from an existing record (the main Avatar's): support
  block emptied, trailers removed (fixed part ends `00 00`, or `00 01` + child block for
  children), char id, starting class and personal skills from units.xml, gains 0, empty
  inventory (`04 00 00 00 00` ×5), deployment slot 0xFF, random bytes 6–9, full HP. It is
  appended to the army group (count byte +1); the save grows and the index is rewritten
  through the same splice used by support growth.
- Children: fixed parent from units.xml `parent` (Morgan (M) → Avatar (F), Morgan (F) →
  Avatar (M)); the other parent is the fixed parent's S-rank spouse, if any. Parent
  asset/flaw come from their logbook data. The child block template is the real Morgan's.
- Tested on 3 saves: Avatar (F) at Lv1 has 19 HP (Tactician (F) 16 + Avatar +3 = her
  Prologue HP); every other unit record and every later block byte-identical; round trip.
  Not yet confirmed on hardware.
- **Hardware test 2026-10-02:** an added Avatar (F) without logbook data showed as "Unknown"
  with a silhouette (the game draws Avatars from the logbook block). Added Dummy/Maiden and
  Merchant came out as male-Robin lookalikes, so ids 2 and 53–56 are no longer offered.
- Fix: `fe_unit_add_logbook` inserts a 0x188-byte logbook block copied from the main
  Avatar's (name "Robin", gender from the character, default look, white hair in both copies,
  new random logbook id, hero flag off, no child after it) and sets the fixed-part marker to
  `01 06`. Added Avatars get it automatically; opening Avatar look & name repairs Avatars
  that lack it. Tested on 3 saves (other data untouched, HP 19). Not yet confirmed on hardware.

## 15. Support-scene Avatar profiles (Global)

- After the four flag lists in Global's USER block: `[06][male logbook 0x187][06][female
  logbook 0x187]`, then the hair-color values. The tester's file: male = "Robin" (its logbook
  id matches the vanilla slot 1 Robin), **female = empty** (all zero).
- Hardware test 2026-10-02: an added (F) Robin's support conversations skipped straight to
  "attained support level"; an added (M) Robin's scenes used the main Avatar's (Bond's)
  portrait. Filling the female Global profile did **not** help.
- **What decides it: the main Avatar's logbook gender byte (+0x1C).** Setting Bond to
  female (Avatar look & name -> Gender, char id still 0) switched the support list to the
  female set (S with male units, e.g. Virion/Stahl) and made (F) Robin's scenes play, drawn
  with Bond's look. All Avatar scenes follow the main Avatar: only the conversation set of its
  gender plays, and every Avatar in a scene shows its look. There is no separate gender flag
  (a female new game differs only in char id 1 + female class; nothing gender-like in USER).
- The editor still fills an empty profile from the matching Robin (on add / opening Avatar
  look & name) and offers Scenes/SELECT; harmless, but not what makes scenes play.

## 16. Character ids past 56

- Hardware test 2026-10-02: a unit with char id 57 (or any of ~100 ids from 57 up) crashes the
  game when the save loads. The unit's char id indexes the playable-character table only
  (0-56, ending with Merchant); bosses and NPCs such as Phila or Raimi have no entry there.
  Enemy units in map saves use map-local ids and can't be moved into the army either.
- Ids 332-352 crash on load too (second hardware test, 2026-10-02).

## 17. Weapon ranks, Boots, battle records, fallen units (FireEditor offsets, checked on dumps)

- Weapon exp: unit +0x3D..+0x42 (after the 5 equipped skills): sword, lance, axe, bow, tome,
  staff. Stored as half the game's points: E 0, D 15, C 35, B 60, A 90 (A is the top rank in
  Awakening). Frederick starts at 15/60/15 = D/B/D.
- Boots (extra movement): unit +0x15.
- Battles / victories: u16 at end block +0x31 / +0x33 (end block = flags + 0x2A + 0x0D).
- Fallen (Classic): the unit sits in group 4 and flags +0x0F has bits 3 (fallen) and 7 (fell on
  this map) set (Map0 dump: Donnel, Ricken = 0x88). Revive = clear both bits, full HP, move the
  record to the end of group 3. Groups with no units are omitted from the UNIT block.
- Moving/removing units rebuilds the UNIT body (group id, count, records) and shifts the index.

## 18. World map and story chapter

- GMAP: 0x3E-byte header ("PAMG", location count at +0x3D: 51 vanilla, 53 with the tester's
  mod), then 0x1D bytes per location; entry +1 = state 0 locked, 1 beaten, 2 open. Location i =
  map id i (0 Prologue ... 25 Chapter 25, 26 Endgame, 27-49 Paralogues 1-23, 50 Outrealm Gate).
  Entry +3.. = two 0x0D encounter slots (Risen/merchant/wireless teams).
- USER +0x0F = current story chapter id (= map id + 2; 3 = Chapter 1). Dumps agree: the
  current chapter's location is the one "open" story location.
- Untested on hardware: whether reopening a beaten chapter lets it be replayed, and what the
  story does afterwards.

## 19. StreetPass/SpotPass teams (DU26) and the Global logbook (DG18)

- Team/logbook unit ("DU", FireEditor UnitDu): +1 char id u16, +3 class, +4 flags, +5 level,
  +6 boots, +7 hidden level, +0x0A hair RGBA, +0x0E gains[8], +0x16 equipped skills[5] (1 byte
  each), +0x1B weapon exp[6], +0x21 five items of 0x26 (id byte, might/hit/crit, forge name),
  +0xDF parents[8], +0xE7 learned skills (0x0D), +0xF4 3 unknown, +0xF7 logbook data.
- DU26: "62UD" + 2 bytes, team count, then per team: slot byte, 0x0B header (unit count at +9),
  10 units of 0x12F (logbook part = name + first 0x1E bytes), team name 0x2A, profile card 0x47,
  4 messages of 0x42 (card + messages = the rest of every member's logbook data). Then 2 bytes,
  0x29-byte records, and the player's own team (no slot byte). Avatars are the units with a name
  (char id 2). Checkpoint Chapter2 (slot 3): Alexander, Josh, Melody (StreetPass), Nyna (SpotPass).
- Global: index +8 -> "81GD" block, unit count at +0x0A, entries from +0x0C: 5-byte header +
  0xF7 unit bytes + 0x187 logbook bytes (0x283 each).
- Import (feimport.c) turns any of these into an army unit with char id 0/1 by logbook gender
  (Logbook Unit id 2 has the same personal bases as the Avatar, so stats are unchanged).
  Supports from a save come along capped at A; with another same-gender Avatar in the army only
  the imported unit's side is written, since partners keep one value per character.

## 20. Difficulty, mode, parents, forges

- Difficulty/mode (FireEditor HeaderBlock/UserBlock, matches all dumps): header +0x08 = USER
  tail +0x63 mode flags (0x04 Casual), header +0x09 = tail +0x64 (bit 0 Lunatic+), header +0x0D
  = tail +0x68 difficulty (0 Normal, 1 Hard, 2 Lunatic). Tail +0x69 = gold. Dumps: old slots
  1/2 = Lunatic Classic, slot 3 and the female new game = Normal Casual (to confirm in game).
- Child data (0x25): +0x01 father block, +0x12 mother block (0x11 each: support byte, then
  3 x [u16 char id or 0xFFFF, asset, flaw, extra] = parent, parent's father, parent's mother),
  +0x23 2 bytes (sibling support at +0x24).
- Forge record: index, UTF-16 name, base weapon u16, might/hit/crit, enemy flag. No
  durability field: a forge's uses always come from its base weapon.
- Forge creation (editor): a new 0x2C record is appended to REFI (count byte +5 incremented),
  slot = lowest unused 0..149; the item id is 202 + slot, and convoy entry 202 + slot holds its
  total uses. A forged Falchion (base 15, 0 uses) stays unbreakable (host test).
- Hair color of any unit: end block +0x39 (RGBA). units.xml "color" = each character's hair;
  customColor children (Owain, Inigo, Brady, Kjelle, Cynthia, Severa, Gerome, Morgans, Yarne,
  Laurent, Noire, Nah) take the non-fixed parent's hair. units.xml lists Cynthia's fixed parent
  as Lon'qu (13); the generator corrects it to Sumia (12).

## 21. Play time, chapter records, map encounters, hero units

- Play time: u32 frames (60/s) at USER +5 and file header +1 (identical in every dump).
- Chapter records: USER +0x15 count, then 0x10 each: [1][chapter id][u16 turns][u32 frames]
  [u16 unit][u16 class][u16 unit][u16 class] (0xFFFF = none). Prologue's record has no units.
- Map encounter slots (entry +3 and +0x10, 0x0D each): Risen 01 01, class (21/25/26/44/54 seen),
  pool 0x0C-0x17, days left 1-5 in real saves; merchant 03 02 class 73; team 02 02 (team id at
  +7). Slot flag byte +0x0C = 2 for the first slot, 0 for the second. The editor writes 16 days.
- Hero units (einherjar.xml) import as DU data: char id 2 (52 for Marth), gains as 8-bit
  (255 = -1), skills learned + equipped, logbook with name/look/asset/flaw and log id at +0x27.
  "(DLC)" entries are level 0 with zero gains (scaled by the game on recruit); imported at Lv 1.
- SpotPass logbook entries (Global: Caeda, Nyna) have logbook +0x27 = hero log id (Caeda 1,
  Nyna 0, rest zero), +0x37 = 0x02 (hero flag: the game shows that hero's own portrait),
  +0x39 = class, birthday 0/0, face 0. DU block1 +8 is the map sprite (shared by many heroes),
  not the portrait. The DLC Marth in the tester's army is char id 62 (einherjar.xml says 52).

## 22. Renown rewards, Barracks events, renaming regular characters

- Renown rewards: USER tail +0x7B (gold + 0x12), 5 bytes of "claimed" bits. Slot 3 dump (renown
  99,999) has ff ff ff ff 01 = 33 rewards claimed. Clearing them = rewards claimable again.
- Barracks (EVST, "TSVE"): 0x50-byte header (RNG state?) then 5 events of 8 bytes: [0] ?, [1]
  unit count, [2] type (1 stat boost, 2 exp, 3 weapon exp, 4 item, 5 conversation, 6 birthday),
  u16 unit 1, u16 unit 2 (char ids, 0xFFFF none), [7] icon. Every dump has all 5 empty
  (00 00 00 ff ff ff ff ff), so filled events are untested on hardware.
- Renaming Chrom & co. (experiment): the editor gives the unit logbook data (copied from an
  Avatar, then own id, no asset/flaw, own hair color, the typed name). Char id is unchanged.
  Hardware test pending: whether the game shows the name, and which portrait it uses.
- Hardware result (2026-10-02): renaming Chrom via added logbook data shows the new name and
  keeps his 3D model, but the portrait becomes Robin's (logbook face). Rename is therefore
  limited to units that already have logbook data.
