# FE Awakening Save Editor (3DS homebrew)

On-console save editor for Fire Emblem Awakening by Noble Zero. Runs on Luma3DS, as a .3dsx (Homebrew Launcher) or an installed .cia.
This app can work with modified game saves as well like Thabes Overwritten as long as those files are in your luma folder the app will read the modified stats 

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

## Options

Everything below can be changed from the console. Pick a game, pick a save slot, then a unit or
one of the menus. Edits stay in memory until you save (START), and every save is backed up first.

### Units

| Screen | What you can edit |
|---|---|
| **Stats** | Level and all eight stats (HP, Str, Mag, Skl, Spd, Lck, Def, Res), shown the way the game shows them. Step ±1 / ±5, jump to the cap, or type a value. Numbers turn green when raised and red when lowered compared to the save as loaded, with the old value shown. |
| **Class** | Change to any class, including the other gender's and enemy-only classes (marked with a warning), with a preview of the new stats. |
| **Skills** | Learn or forget any skill, equip up to five, or learn every skill at once. |
| **Supports** | Every support partner of the unit: step through C ready, C, B ready, B, A ready, A, S ready and S, or type the points. Several S supports at once are possible (experimental). |
| **Items** | All five inventory slots: any item (filter by weapon type), uses, equip, remove, and forge the weapon. |
| **Forge** | Create or edit a forged weapon: name, Might +, Hit +, Crit +. |
| **More** | Weapon ranks for all six weapon types, boots (Move +), battles and victories, revive a fallen unit, remove the unit (confirmed with a popup). |
| **Rename** | New name for Avatars, logbook units and imported heroes. |
| **Look & name** (Avatars) | Name, gender, build, face, hairstyle, hair color (any color, as hex), voice and birthday. Presets include the DLC hero heads (Marth, Roy, ...). |
| **Parents** (children) | Father and mother, hair color (or the child's default hair), and inheriting the parents' skills. |

### Army

| Screen | What you can edit |
|---|---|
| **Army** | Gold (up to 999,999), Renown (up to 99,999), difficulty (Normal, Hard, Lunatic, Lunatic+), mode (Classic or Casual), and making Renown rewards claimable again. |
| **Barracks** | The events waiting in the Barracks: stat boosts, experience, found items, weapon experience, conversations between two units, birthdays (experimental). |
| **Convoy** | Any item in any amount: ±1 / ±10, type a number, max (99), or set every listed item at once; filter by weapon type; forge convoy weapons in bulk. |
| **Army tools** | One button for every player unit: max every stat, learn every skill, max weapon ranks (A), raise supports to the next conversation, heal everyone, revive all fallen units. |

### Extras

| Screen | What you can edit |
|---|---|
| **Add a unit** | Add any playable character as a fresh level-1 recruit (children get their parents filled in). **Import an Avatar** brings in Avatars from your other save slots and their StreetPass / SpotPass teams, your logbook, Checkpoint backups, or the list of 138 SpotPass and DLC heroes, optionally with their supports (capped at A). |
| **Support Log** | Unlock every support conversation and Unit Gallery entry, and the Game Clear flag that shows those menus (shared by all slots). Can be restored from a backup. |
| **World map** | Each location (chapters and paralogues): locked, open, beaten, Risen battle or merchant; replay a beaten chapter; or set all locations at once. |
| **Records & time** | Total play time and each chapter's record: turns, time, and the two featured units and their classes. |

### Settings

| Setting | What it does |
|---|---|
| **Backup before saving** | On by default: the whole save is copied to the SD card before every write. Toggle on the Backups screen or on the save confirmation. |
| **Backups** | Restore a backup (the current save is backed up first), delete one, or delete all (Up, Down, Left, Right, B, A). |
| **Marriage guard** | Lowers "S ready" to A for units who are already married, so watching it doesn't undo a marriage (X on the slot screen). |
| **Mod data** | Use a romfs mod's characters, classes and items when one is found (SELECT on the slot screen); see Romfs mods below. |

All settings are remembered between launches.

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
