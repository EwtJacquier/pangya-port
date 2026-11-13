# Pangya: Fantasy Golf (ULUS10438) — reverse engineering and mods

Addresses are always in the PSP's runtime form (base `0x08804000`): the same in Ghidra, in
PPSSPP's debugger and in CWCheat codes (`0x2XXXXXXX` = address − `0x08800000`).

## Files

| Path | What it is |
|---|---|
| `../EBOOT_decrypted.elf` | Decrypted EBOOT.BIN (MIPS/Allegrex PRX) |
| `ghidra_project/` | Analyzed Ghidra project (open with `~/tools/ghidra_12.2_PUBLIC/ghidraRun`) |
| `export/functions.jsonl` | Every function: pseudo-C, callees, callers, strings |
| `export/imports.json` | 202 PSP system functions (NID → name, from PPSSPP's tables) |
| `export/hash_matches.txt` | libc functions identified by PPSSPP's hashes |
| `export/known.jsonl` | imports + libc with signatures (applied by `ApplySignatures.java`) |
| `export/names.jsonl` | Names suggested by DeepSeek (apply with `ApplyNames.java`) |

## Commands

Headless Ghidra environment:

```bash
export JAVA_HOME=$HOME/tools/jdk-25.0.5+8 PATH=$HOME/tools/jdk-25.0.5+8/bin:$PATH GHIDRA_HEADLESS_MAXMEM=6G
H=$HOME/tools/ghidra_12.2_PUBLIC/support/analyzeHeadless
P=$PWD   # this folder (re/)
```

Export the functions again after renaming things in Ghidra:

```bash
$H $P/ghidra_project pangya -process EBOOT_decrypted.elf -noanalysis -scriptPath $P/ghidra_scripts \
   -postScript ExportFunctions.java $P/export/functions.jsonl
```

Name functions with DeepSeek (key in `~/.config/deepseek/key`; resumable):

```bash
python3 tools/deepseek_namer.py export/functions.jsonl export/names.jsonl --sample 40   # test
python3 tools/deepseek_namer.py export/functions.jsonl export/names.jsonl --limit 500   # in batches
```

Apply the suggested names to the project (only renames functions still called `FUN_*`,
confidence ≥ 0.5):

```bash
$H $P/ghidra_project pangya -process EBOOT_decrypted.elf -noanalysis -scriptPath $P/ghidra_scripts \
   -postScript ApplyNames.java $P/export/names.jsonl 0.5
```

## Patch mods (for the original game on a PSP or PPSSPP)

The port's mods are C code (`../port/mods`); these are for the unmodified game.

1. Write a patch file (`mods/*.patch`):
   ```
   [Mod name]
   0x08A9FA74 00000000     # address  32-bit word(s) in hex
   ```
2. Make the patched ELF + a PPSSPP cheat:
   ```bash
   python3 tools/elf_patch.py ../EBOOT_decrypted.elf mods/x.patch -o ../build/EBOOT_mod.elf --cheats ../build/ULUS10438.ini
   ```
   - Quick test: copy the `.ini` lines into PPSSPP's `memstick/PSP/Cheats/ULUS10438.ini`.
   - If it prints `RELOCATED`, the loader adjusts that word; use the cheat or another spot.
3. Build the patched ISO (the original is never changed):
   ```bash
   python3 tools/iso_replace.py "../Pangya - Fantasy Golf (USA).iso" ../build/Pangya_mod.iso \
       PSP_GAME/SYSDIR/EBOOT.BIN=../build/EBOOT_mod.elf
   ```
   It also replaces files such as `PSP_GAME/USRDIR/clientsetting.txt` (up to 2048 bytes).

## Findings

### ClientSetting.txt (`FUN_08885fd0`, ClientSetting.cpp)

The game reads `PSP_GAME/USRDIR/ClientSetting.txt` at boot. Key → field of the settings object:

| Key | Field | Default in code | Value on the ISO |
|---|---|---|---|
| m_diStartMode | +0x5bc | 1 | 4 |
| m_diMap / m_diHole | +0x5c0 / +0x5c4 | 0 | 0 / 1 |
| m_diStartingStoryMode | +0xa84 | | 1 |
| m_diGameType / PspGameType | +0x5ec / +0x5f0 | 0 | 0 / 0 |
| m_diCharSelected / m_diBall / m_diClubSet | +0x10000−0x17d4 / −0x17d8 / −0x17dc | | 0 / 1 / 10 |
| m_bAutoMode / m_diAutoSpecial / m_diAutoClick | +0x10000−0x1724 / −0x171c / −0x1718 | | 0 / 0 / 2 |
| m_diTeams | +2000 | | 0 |
| m_localize / m_wingame | +0xe830 / +0xe831 (byte) | | 0 / 0 |
| vi* / vfGamma (video) | +0x28 … +0x70 | | |

Limits applied after reading (`FUN_08883594`): StartMode 0..4, Map ≥ 0, Hole 1..18, GameType 0..7.

### m_diStartMode → first task (`FUN_0887dfc4`, CProjectG::Ready)

| Value | Task / Doc |
|---|---|
| 0 | CLobbyTask |
| 1 | CGolfTask + CGolfDoc (a round straight away with m_diMap/m_diHole/the file's character) |
| 2 | CAvatarChatTask + CAvatarChatDoc |
| 3 | CShopTask |
| 4 | CStoryTask + CStoryDoc (the ISO's default) |
| 15 | CLunaTask + CGolfDoc — hidden; needs `mods/startmode_unlock.patch` (limit 4 → 15) |

Ready-made variants: `mods/clientsetting_{lobby,direct_golf,avatar_chat,shop,luna}.txt`.
The limit patch runs at boot, before PPSSPP's cheats — use the ELF version, not the `.ini`.

Debug modes compiled into the game: `modeDebugCamera`, `modeDebugReplay`.

### Game types (golf singleton `M32(0x08b60028)` + 0x13d0)

1 match against the computer (turn-based, two characters), 2-4 single-player modes (4 = Match,
turned into 1), 5 network "single" (everyone at once), 6 network "team", 8 turn-based network
round left from a server version (finished by `../port/mods/turns.c`). The game's debug printf
(`FUN_08af9f64`) is empty in the release build; the port's `PANGYA_GAME_LOG=1` prints it (Korean
text is EUC-KR).
