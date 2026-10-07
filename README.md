# FE Awakening Save Editor (3DS homebrew)

On-console save editor for Fire Emblem Awakening by Noble Zero. Runs on Luma3DS, as a .3dsx (Homebrew Launcher) or an installed .cia.

Edits: level, stats (as shown in game), class, skills (learned + equipped), supports, inventory, convoy, gold and renown.
See [docs/SAVE_FORMAT.md](docs/SAVE_FORMAT.md) for the format notes.

## Download

Get `FEAEditor.3dsx` (Homebrew Launcher) or `FEAEditor.cia` (install with FBI) from
[Releases](../../releases). Every save is backed up to the SD card before it is written.

## Cheats

[`cheats/00040000000A0500.txt`](cheats) is a Luma3DS (Rosalina) cheat file for Fire Emblem
Awakening USA. Copy it to `sdmc:/cheats/` and turn codes on from the Rosalina menu
(L + Down + SELECT, then Cheats). Codes marked "(Vanilla only)" do nothing on a modded game.
[`cheats/NOTES.md`](cheats/NOTES.md) and `cheats/tools/` hold the research behind the new codes.

## Build (.3dsx)

1. Install devkitPro (Windows: the graphical installer from devkitpro.org; tick **3DS development**).
2. In the devkitPro MSYS2 shell:
   ```
   pacman -S 3ds-dev
   cd FE-Awakening-Save-Editor
   make
   ```
3. Copy `FEAEditor.3dsx` to `sdmc:/3ds/` and launch it from the Homebrew Launcher.

The project path must not contain spaces (GNU make limitation).

## Controls

| Screen | Controls |
|---|---|
| Slot select | D-pad + A, or tap a slot. START exits |
| Unit list | D-pad moves (Left/Right page), A stats, L class, R items, X skills, SELECT supports, Y army (gold/renown), START save, B back; Convoy button |
| Items | Up/Down slot, A change item (picker, X = type filter), X equip, SELECT remove, Left/Right ±1 use, L/R ±10, Y type uses |
| Convoy | Up/Down item, Left/Right or −/+ one more/less, Y type count, X set all listed, SELECT type filter |
| Army | Gold and renown (A or tap to type), X convoy |
| Extras → Avatar | Up/Down field, Left/Right or −/+ change, A type (name, hex hair color, birthday), L/R other logbook units |
| Stats | Up/Down row, Left/Right ±1, L/R ±5, X max, Y type a value, SELECT class; or tap −/+/Max/Class |
| Class | Up/Down class, L/R page, A change (preview of new stats on top), B back. All classes allowed |
| Supports | Up/Down partner, Left/Right or −/+ step through - > C ready > C > B ready > B > A ready > A > S ready > S, Y type points, L/R page |
| Slot select (X) | Toggle the marriage guard (lowers "S ready" to A for already-married units when a slot opens) |
| Skills | Up/Down skill, L/R or Left/Right page, A learn/forget, Y equip/unequip, X learn all; or tap a name (learn) / Equip button |

## Safety

Every save:
1. copies the **whole** save (all files) to `sdmc:/3ds/FEAEditor/backups/<date_time>/` and reads each copy back,
2. recompresses the slot, decompresses it again and compares it byte for byte,
3. writes it, commits, then reads the file back from the save and compares.

If any step fails, nothing is committed. To restore a backup, copy that folder's files back with
Checkpoint (put the folder under `sdmc:/3ds/Checkpoint/saves/<Awakening folder>/` and restore it).

## Romfs mods

The editor reads a mod's character, class and item data, so names, stats, supports and items
match the modded game. It looks, in order, at:

1. `sdmc:/luma/titles/<title ID>/romfs/` (a Luma LayeredFS mod)
2. `sdmc:/3ds/FEAEditor/mod/` (copy the mod's files here yourself)
3. the installed game itself (a mod built into the game; needs the .cia build)

Files used: `data/person/static.bin.lz`, `data/GameData.bin.lz`, `m/E/GameData.bin.lz`. The slot
screen shows where the data came from; SELECT turns it off. Forged weapons are always read from
the save itself. `tests/femod_test.c` checks the loader against a vanilla and a modded romfs.

## Files on the SD card

- `sdmc:/3ds/FEAEditor/debug.log`: debug log; attach it when reporting problems
- `sdmc:/3ds/FEAEditor/backups/`: save backups
- `sdmc:/3ds/FEAEditor/settings.cfg`: remembered settings (backups, marriage guard, mod data)

## Tests (on a PC, no devkitPro needed)

```
tests/run_host_tests.sh <Checkpoint save folder> [more save files]
                                                   # core: load/edit/rebuild real saves, compare units to
                                                   # values read off the real game, Huffman stress
tests/check_3ds_syntax.sh                          # type-check 3DS code against stub headers
```
Both need a host gcc (e.g. MSYS2 mingw64: `PATH=/c/msys64/mingw64/bin:$PATH`).

## Layout

- `source/core/`: portable save logic (CRC32, Huffman, save parser, stat math), unit-tested on PC
- `source/fs.c`: title discovery, save archive I/O, backup, write + commit
- `source/ui.c`, `source/main.c`: citro2d UI and screen flow
- `tools/gen_fedata.py`: regenerates `source/core/fedata_tables.c` from `tools/data/*.xml`

## License & credits

GPL-3.0 (see [LICENSE](LICENSE)). Huffman encoder layout from CUE's huffman.c via SciresM's
FEAST; save layout and stat tables from Danius88's fire-editor-awakening (after Olmectron's
FireEditor). Many cheat codes are by ymyn. Every source is listed in [CREDITS.md](CREDITS.md).
Fire Emblem is © Nintendo / Intelligent Systems; no game files are included.

## Building the .cia

```
make cia
```

Needs `tools/cia/makerom.exe` and `tools/cia/bannertool.exe`, which are not in this repo: download
makerom v0.19.0 from https://github.com/3DSGuy/Project_CTR/releases and bannertool 1.2.0 from
https://github.com/diasurgical/bannertool/releases and put both exe files in `tools/cia/`.
If `python` is not on the devkitPro PATH, pass it: `make cia PYTHON=/c/path/to/python.exe`. The HOME Menu banner uses
`banner.png` (256x128) and `banner.wav` from the project folder; `tools/cia/make_banner.py`
creates them from `icon.png` if they are missing, so replace them to use your own art.

3D banner (optional): put `common.cgfx` (or `.bcres`), optional `region10.cgfx`..`region13.cgfx`
(per-language logo models) and `banner.bcwav` in a `banner_3d/` folder; `make cia` packs them
with `tools/cia/pack_cbmd.py`. `tools/cia/cgfx_replace_textures.py` swaps ETC1 textures in a
model by name (e.g. `logoA=logoA.png`). No banner models are included here.
Title ID `000400000FEA1300` (unique ID 0xFEA13), described in `tools/cia/cia.rsf`.
Install `FEAEditor.cia` with FBI (Luma3DS); it shares `sdmc:/3ds/FEAEditor/` (backups, log,
settings) with the .3dsx.
