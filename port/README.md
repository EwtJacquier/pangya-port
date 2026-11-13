# Pangya: Fantasy Golf — port (static recompilation, no emulator)

The PSP executable (MIPS Allegrex) is translated to C and compiled as native code (Linux first,
then WebAssembly). The PSP system functions the game uses (202 imports) are reimplemented in this
project's own runtime — nothing depends on PPSSPP.

## Pipeline

```
EBOOT_decrypted.elf ──prx_load.py──▶ generated/image.bin + meta.json   (relocated at 0x08804000)
Ghidra (re/ghidra_project) ──ExportProgram.java──▶ re/export/program.json (functions, jump tables)
image + program.json ──recomp.py──▶ generated/code_*.c, funcs.h, func_table.c, hle_stubs.c
generated/*.c + runtime/*.c ──▶ native binary / wasm
```

Regenerate everything:

```bash
python3 tools/prx_load.py ../EBOOT_decrypted.elf ../re/export/imports.json generated
python3 tools/recomp.py . ../re/export/program.json
```

Analysis fixes Ghidra doesn't find by itself (methods only called through vtables, unresolved
`switch` tables) live in `re/export/fixups.json` and are applied by
`re/ghidra_scripts/FixupCode.java`. `tools/rebuild.sh` runs the whole chain
(`SKIP_GHIDRA=1` for recompile + build only; needed after changing `hooks.txt`).

## Generated code model

- Each MIPS function becomes `void f_XXXXXXXX(CPU *c)`; registers live in `CPU` (`runtime/cpu.h`).
- PSP memory: a single block `rt_mem`, addresses masked with `0x0FFFFFFF`
  (covers RAM `0x08…`, VRAM `0x04…`, scratchpad and the uncached mirrors `0x4…`).
- Branches → `goto`; delay slots emitted in the right order; `jal` → direct C call;
  `jalr`/`jr reg` → `rt_call` / jump-table `switch` / `rt_tail_call`.
- VFPU (0.5% of the code) → `rt_vfpu(c, pc, op)` in the runtime.
- PSP imports → `hle_<name>(c)`; the default (weak) implementation only logs "not implemented".
- Mods replace game functions: `hooks.txt` lists the addresses; the hand-written `f_XXXXXXXX` in
  `mods/*.c` takes the place of the generated one, which stays callable as `f_XXXXXXXX_orig`.

## Running

```bash
make -j8 && ./build/pangya "../Pangya - Fantasy Golf (USA).iso"
```

Keyboard: arrows, Z=✕, X=◯, A=□, S=△, Q/W=L/R, Enter=Start (USB gamepads work too).
Saves go to `build/ms0/PSP/SAVEDATA`.

Useful variables: `PANGYA_HEADLESS=1`, `PANGYA_DUMP=dir:N` (frames, `PANGYA_DUMP_FROM=F`),
`PANGYA_INPUT="F:btn,F1-F2/N:btn"`, `PANGYA_FRAMES=N`, `PANGYA_TRACE=1|2` (+`PANGYA_TRACE_FILTER`),
`PANGYA_GEDEBUG`, `PANGYA_GEDUMP_FRAME=N`, `PANGYA_TEXDUMP=dir`, `PANGYA_PROFILE=1`,
`PANGYA_WAVOUT=f.wav`, `PANGYA_SAS_SOLO=1`, `PANGYA_FPS=1`, `PANGYA_AUDIO_SLACK=frames`,
`PANGYA_AUDIO_BUFFER=frames` (native), `PANGYA_SCALE=N`, `PANGYA_STARTMODE/MAP/HOLE`,
`PANGYA_UI_FILTER=0|1|2` (2D UI: original, xBR, bicubic), `PANGYA_DUMP_FULL=1` (frames at the
internal resolution), `PANGYA_RESCALE_AT=frame:scale` (changes the internal resolution mid-run,
like the web menu does).

Don't run a parallel build (`make -j16`, `tools/rebuild.sh`) while two copies of the game run
under gdb: on a 16 GB machine that froze the computer.

## Browser (WebAssembly)

```bash
tools/get_ffmpeg.sh            # once: FFmpeg libraries (x86_64 + wasm32)
tools/build_web.sh             # builds build/web/{index.html,index.js,index.wasm,index.data}
python3 tools/serve_web.py     # http://127.0.0.1:8642/  (the page loads the ISO from the server)
```

- The ISO is read in slices: the page uses the server's `/game.iso` (or `?iso=URL`); without it,
  it asks for a local file (read in the browser, never uploaded). URL parameters become
  environment variables (`?PANGYA_FPS=1`).
- Fills the window (F11 or double click: real fullscreen). Internal rendering at 4x with the
  original UI filtering (fixed; `?PANGYA_SCALE=1..6` and `?PANGYA_UI_FILTER` are for tests; native
  uses the same default).
- **Esc** (or the gear in the corner, or Home on a gamepad) opens the settings: fullscreen, keys
  and gamepad buttons, kept in the browser's `localStorage`.
- PSP threads = Emscripten fibers (Asyncify); WebGL2; WebAudio.
- Saves in the project folder: `serve_web.py` keeps the main save in `build/ms0/PSP/SAVEDATA` (the
  same as native; the previous version stays as `.bak`). Without that server the save lives in
  the browser's IndexedDB.
- Players: the page asks for a player name and password (or "Play without an account", the main
  save). Each player has their own save in `build/profiles/<name>/ms0` (a new one can start from a
  copy of the main save, with their name written into it), and that name is their network
  nickname. The session belongs to the window (`sessionStorage`): two windows, two players.
  Passwords are PBKDF2 hashes in `build/profiles/<name>/account.json`.
- The build contains translated game code: host it privately only.
- **F9** writes a diagnostic of the current frame (every GE draw + the image) and sends it to
  `serve_web.py`, which saves it in `build/webdump/` (nothing leaves the machine). Native:
  `PANGYA_GEDUMP_FRAME=N`.
- **Ad hoc (Network Mode)** over LiveKit: each tab is a PSP with a random virtual MAC
  (`?PANGYA_MAC=` sets one) and joins the LiveKit room `pangya-<room>` (`?PANGYA_ROOM=room`,
  default `pangya`; `?PANGYA_NICK=` sets the nickname). `serve_web.py` mints the token with
  `LIVEKIT_URL`, `LIVEKIT_API_KEY` and `LIVEKIT_API_SECRET` from `port/.env`; the library is a
  pinned copy of the npm package `livekit-client` 2.22.4 (`web/livekit-client.umd.js`,
  Apache-2.0), loaded only when the game turns ad hoc on. Without LiveKit the game is alone on the
  network. Only small binary messages travel (the game's own PDP datagrams, at most 128 bytes,
  and the mods' messages), about 3 KB/s per player; every copy runs the whole game. A window held
  back (hidden, minimized, a background tab) stops its game but not the messages: they wait in
  `web/net.js`, and when they pile up the oldest of the game's own make room, never the mods'
  (the queue held 2048 messages and dropped the oldest of any kind: about half a minute of a
  held-back window, with the others' state messages coming every frame, was enough to lose a
  shot, and then only its result showed). The game's own sign of a player silent for 30 seconds:
  the player list flickers between their status ("Hole Out") and their name. A hidden page
  gets its timers about once a second, and the game paces its frames with timers: during a
  network game the page's timer runs in a worker while the page is hidden (the worker's timers
  keep their pace), so a covered or minimized window plays on. Before, the window of a player who
  had just holed out stood still in that hole out while the other window was in front, and the
  other player's next shot showed there only once it was looked at again.

## HD textures (texture packs)

A texture pack replaces the game's textures with bigger versions while the game runs, without
changing any of the game's files: `python3 tools/textures.py install pack-*.zip` puts one in
`build/textures` (QOI images + `index.txt`), `uninstall` takes it out, and the web menu's "HD
textures" switch turns it off and on. The game reads it natively (`PANGYA_TEXTURES=dir`, `0` turns
it off) and in the browser (`serve_web.py` serves `/textures/`; images are fetched when first
needed and kept by the browser), and reads `index.txt` again within a few seconds while it runs.
The pack's HD font (`runtime/fonthd.c`, fed by `mods/font.c`, which reads each string the game's
font engine writes into a texture): `font-<key>.qoi` + `font-<key>.txt` hold the font's glyphs
upscaled (key: CRC-32 + Adler-32 of the font's glyph table), and those lines are drawn again at the
render scale with them, cell by cell where the game put its glyphs. `PANGYA_FONT_DUMP=dir` writes
the glyph table of each font the game uses; `PANGYA_FONTHD_LOG=1` logs the strings and which text
textures got the HD font. Replacements get mipmaps: shown smaller than their size (3D billboards
such as the yards, far sprites) they stay smooth instead of losing thin outlines letter by letter
(`PANGYA_TEXPACK_MIPS=0` turns that off, for comparing), read with a slight bias toward the
full-size picture so text drawn a little smaller stays sharp (`PANGYA_LOD_BIAS`, default -0.25).
How a replacement finds its texture (`runtime/texpack.c`): the key is CRC-32 + Adler-32 of the
picture's RGBA texels as the GE samples them, over the picture's own width x height at the top
left of the uploaded texture (pictures that are not a power of two sit in a larger texture whose
other rows are whatever follows in memory). Textures the game rewrites frame after frame (movies,
font strings) are not looked up. A replacement is drawn with smooth filtering and the sprite clamp
at its own resolution. `PANGYA_TEXPACK_LOG=1` logs every upload's key and every replacement.

## Status

- [x] Stage 1 — full translation: ~10,950 functions compile.
- [x] Stage 2 — runtime: threads, memory, ISO files, UMD copy protection.
- [x] Stage 3 — graphics (GE → OpenGL ES 3), sound (mixer, SAS, ATRAC3/3+), videos (PSMF/H.264),
  input.
- [x] Stage 4 — WebAssembly build: runs in Chrome at 60 vblanks/s (video, audio, input, saves).
- [x] Stage 5 — mods (`mods/*.c`):
  - Boot (`mods/boot.c`, `PANGYA_MOD_SKIP_INTRO=0` turns it off): no autosave notice, no
    TOMY/SK/NTREEV logos and no opening movie: straight to the title.
  - Start Game (`mods/autoload.c`, `PANGYA_MOD_AUTOLOAD=0`): a single "Start Game" button on the
    title (instead of "New Game / Load Game"): with a save it loads it; without one it starts a new
    game (name). The d-pad doesn't move the cursor. The button keeps its own word, "Load Game" with
    a save and "New Game" without one, which is what it does.
    (With the interface's vector text on, it reads "Start Game".)
  - Unlock (`mods/unlock.c`, `PANGYA_MOD_UNLOCK=0`): all 18 characters available, the shops sell
    every class-gated item (only while they build their lists; the player's class doesn't change)
    and the whole Pangya Collection gallery (4 videos, 15 artworks, 20 music tracks). The flags go
    into the save.
  - The yards over the club's landing mark (`mods/spot.c`, `PANGYA_MOD_YARDS=0` turns it off):
    with the camera behind the player the game wrote them as letters in the course, small and far
    away, and their dark outline went pale and blurred; they are drawn as the view from above
    draws them (a label of the interface, with its font), over the mark's arrow.
  - Network scores (`mods/netplay.c`, `PANGYA_MOD_NETFIX=0`; `PANGYA_NET_TRACE=1` logs the state
    messages and the players' records): strokes and scores arriving from another player land on
    the message's hole, not on the hole this copy is playing (when copies changed holes at
    different moments, one hole's numbers fell on the other).
  - Turn-based network rounds (`mods/turns.c`, `PANGYA_NET_TURNS=0` turns it off): a "single"
    Network Mode room with 2 players plays in turns and everyone watches the current player
    (character, swing, ball flight). The player farthest from the pin plays next (at the tee, the
    tee order). It is game type 8, which the game has but its menus never offer (left from a
    version with a server): the current player sends the shot (message 0x20) and the other copies
    replay it. The mod:
    - builds that round's players (the game's setup only knew types 2-7) and puts both characters
      on the course;
    - exchanges each player's profile in the room (character, costume, ball, caddie, clubs,
      stats; the game's own messages 0x14-0x16 for this are never sent);
    - hands each shot to the other copies itself, in order, once a copy stands ready for that
      player's shot (the game took message 0x20 in whenever it came, even while a copy still
      showed the shot before: a copy that had fallen behind replayed a putt the opposite way, then
      missed the next shot and waited for it while the player had given up), with what the replay
      needs to land on the same spot: the wind it was played in (each hole's wind comes from the
      room, but every turn adds a few random tenths of m/s on each copy and the ball landed up to
      20 yards apart), the character's exact direction and the stroke's number; then who plays
      next (the shooter's copy decides) and where the ball rests, both tagged with the stroke and
      taken in only once a copy has replayed that shot, so a copy that fell behind replays every
      shot in order;
    - lets the board after each hole go on by itself as soon as it takes the button (it waited
      for the button, then ten more seconds of a network round's own);
    - gives each player their place (1st, 2nd...) at each hole's board, where a network round
      takes them for the results and the end scene: the network board that works the places out
      never shows in turns, and everyone came 9th, in the rain;
    - ends a hole out early (after a second and a half) on a copy that still shows it when the
      next player's shot has already come: that shot showed there only once the hole out was
      over;
    - ends a hole when every player is done: after a give-up the game ended the hole for the copy
      whose own player had given up (a "single" round's rule, where each copy's player is player
      0), so the first player's give-up ended it on both copies while the other still played; and
      gives a shot's prize to the shooter (the first player's went to both, the second's to none);
    - shows "Your Turn" to the player whose turn it is;
    - draws the HUD of a match against the computer (whose player is player 0) as this copy's
      player's: the power-shot sign on the gauge showed on the second player's copy in the other
      player's turn, from the other player's power; each player's ball (by the gauge and in the
      spin circle) and club set showed in the other one's turn, the wind of a Silent Wind and the
      round's pang counter were another player's (while it draws, the players read through the
      getter FUN_0889a22c, their things in the round, G + 0x270 * player from +0x2a9, and the
      per-player pictures swap with this copy's player's); and shows only the card (picture, strokes, score, power gauge) of the
      player whose turn it is, on the left (the second player's own card was on the right, the
      other player's in its place), with that player's nickname (the computer's opponent showed
      its character's name, player 0 this copy's nickname);
    - sends the items: an item used before a shot (Silent Wind, Strength Boost, the masteries...)
      took effect only on that player's copy, through FUN_0890afb4; the others get it with its
      random number (an item may fail) and take it in the same way once they stand in that
      player's turn for that stroke, before the shot. Each player brings their own items: the
      equipment screen puts this copy's player's in player 0's list (G+0x41b), which on the second
      player's copy was the first player's and went back to the save at the end; each list now
      sits with its player (the others' come with the profile) and this copy's goes back to
      player 0's at the game over. The Mulligan Rose (the shot taken back) counts in the
      statistics of this copy's player only (they counted the first player's on every copy).
      `PANGYA_TEST_ITEMS=id,id` (hex) and `PANGYA_TEST_BALL=n` give a test copy items and a
      ball, `PANGYA_TEST_ITEM=1` uses the first item before each shot;
    - sends the club a player picks with L/R while aiming (the others saw the one the game picks
      by itself until the shot came); `PANGYA_TEST_CLUB=1` presses L before each stance;
    - draws no ball markers (`mods/spot.c`): they show a "single" round's players where the
      others' balls are, but in turns everyone watches every ball and they stood in front of the
      view;
    - shows the distance to the hole from the start of every turn: a network round hid it when a
      turn started and showed it again only once the player took the stance (or with the view
      from above), so it was missing while the player aimed;
    - fills the state message each copy sends from its own player (the game took player 0 and the
      ball in play: the second player sent the first one's strokes and "hole out", and the other
      players' ball markers jumped to the current player);
    - ends the round as an ordinary network one with this copy's player as player 0 (their pose,
      results, experience and pang), lays the results out as type 5's (type 8 had no panels and
      the screen never let anyone leave), lets the end scene take one character as at the end of
      a match against the computer, and keeps resources two models of the same character share
      from being released twice (two players on the same character crashed back in the menus);
    - shows each copy its own character once the last hole is over (the board and "Loading
      results" showed the last player to finish on every copy): the turn goes to this copy's
      player and the character gets the message a turn's start sends, in the spot where the last
      player stood, without the check whether it can stand there (when that failed, in the cup
      or with another character's stance, the game drew it see-through);
    - shows each copy its own character in the end scene: the scene's per-player things (look,
      model, animation, bones, position) and the two lists of facial expressions swap with this
      copy's player's (player 0's list is global and the menu avatar's; with the other's list a
      face was made without its picture and the scene crashed), and swap back before the
      Player actor frees its own.
    With 3 or 4 players the room keeps everyone playing at once: the round's player list
    (FUN_0889a154) and the characters' models (FUN_089bce98) have room for two whatever the
    count, and a third player wrote past both (the game crashed on the first turn). Mods exchange
    their own messages
    through `runtime/net.c` (`net_mod_send` / `net_mod_recv`). Tests: `PANGYA_AUTOPLAY=1` plays by
    itself, `PANGYA_TEST_HOLES=1` shortens the round, `PANGYA_TEST_CHAR=n` changes this copy's
    character, `PANGYA_AUTOPLAY_POWER=0.2` hits weak shots (a share of the bar; to reach a
    give-up; putts follow the distance), `PANGYA_TEST_STALL=s` freezes this copy that long at each
    other player's turn (a window held back; `PANGYA_TEST_STALL_BACK=s` after each shot),
    `PANGYA_TEST_LAG=s` holds the others' shots that long, `PANGYA_TEST_BATCH=1` holds each one
    until its result came, `PANGYA_TEST_FF=1` speeds the others' shots up (cross),
    `PANGYA_NET_TRACE=1` (2: with each mode change's caller) logs the turns, `PANGYA_GAME_LOG=1`
    prints the game's own debug messages (a printf the release build left empty).
  - Vector text (`mods/font.c` + `runtime/text.c`): the lines the game's font engine writes into
    textures (menus, descriptions, messages) are redrawn with TeX Gyre Adventor (`assets/fonts`,
    GUST license) at the render scale, same positions and colors. Together with the vector text of
    the interface (the atlases below: menu titles, tutorial pages, HUD digits) it is one switch on
    the web page (Esc → Vector text, on by default; with the HD textures on, the words are drawn
    over the pack's art). Natively both are off unless `PANGYA_MOD_TEXT=1` and `PANGYA_UITEXT=1`
    (on the web they also work as URL parameters).
  - Text baked into the UI atlases (HUD digits and letters, labels): `make generated/ui_text.spec`
    (`ISO_PATH=...` if the ISO is somewhere else; needs Pillow) runs `tools/uitext/build.py`, which
    reads
    the `.gim` + `.txt` files of `pspdata.pak` and, for each sprite with text, fits the style by
    rendering candidates and comparing pixel by pixel with the original (`tools/uitext/fit.py`:
    regular/bold font, weight, up to 3 outlines + glow, a deeper outline at the bottom, a gradient
    only where there is one; each line's size and position refined; border colors by least
    squares in `tools/uitext/edt.c`). It writes `generated/ui_text.spec`; the runtime recognizes
    the atlas by its CRC and redraws those sprites as vectors with distance-field outlines.
    `tools/uitext/review.py` makes side-by-side sheets (original | vector); `PANGYA_UITEXT_DUMP=dir`
    writes each atlas before/after. The text of ~1,250 labels (menus, buttons, messages, names,
    tutorial tips, banners) is transcribed by hand in `tools/uitext/labels.tsv` ("|" splits lines;
    `@x,y,w,h` marks where the text sits in a sprite with art around it, and a sprite may have
    several boxes); the generator only fits style and position. Glyph sets (digits, letters) come
    from the sprite names (`font2_7`, `num_07`, `middle-`...) when they all use the same colors
    (numbered icons such as `gift_1`..`gift_9` don't qualify); atlases without a sprite list get
    one in `tools/uitext/sprites.tsv`. Text over buttons/panels is redrawn after rebuilding the
    background (the letters' mask comes from the difference to the local background, from the
    panel color, or from the white outline when the background is a photo); icons next to the text
    stay intact. Sprites of one group (a menu's items, a glyph set) share one style: the most
    common one or, on a tie, the one with the least error over the whole group, glow included
    (digits of one set all get the same shadow). The result doesn't depend on the process order.
    Texts with button icons and highlighted words (tutorial explanations, game tips) or aligned
    line by line are cut into runs by `tools/uitext/segment.py` from the transcription in
    `tools/uitext/segments.tsv` (`[]` icon, `{}` red or yellow highlight). Running text (tutorial,
    tips, missions) uses only the regular font; the yards of the 3D view and the digits of each
    hole's intro use the style of the top view's (`STYLE_LIKE` in build.py). Large words built
    letter by letter (SUCCESS, BIRDIE, WATER HAZARD...) have one label per letter. The ground words
    under the ball (Fairway, Rough, Green...), the result letters and the notices "Approach Shot",
    "Your Turn" and "For BIRDIE/EAGLE/WIN" have the gradient, the white outline and the dark one
    measured on the sprite itself (`GLOSSY`; the notices each with their own colors: `SOLO`); the
    scoreboard's stroke digits are hollow, outline only (`HOLLOW`: a transparent fill that cuts
    the outlines, in the shader too); the digits of `score_01`/`score_02` (scoreboard) are listed
    in `sprites.tsv`.
    In the game the GPU draws these texts: glyph distance fields built once from the font and a
    shader that enlarges the atlas and composes letters, outlines, glow and gradient (1–10 ms per
    atlas; the CPU path stays as reference and fallback, `PANGYA_UITEXT_CPU=1`). Only above 1x.
- [x] Stage 6 — ad hoc over LiveKit. `runtime/net.c` emulates sceNetAdhocctl/sceNetAdhoc (groups,
  scan, peer list, PDP) on top of messages between the copies of the game: each announces its
  group and nickname (HELLO), a scan asks everyone (PROBE), and PDP datagrams carry the sender's
  group. In the browser the transport is LiveKit (`web/net.js`); natively, UDP on 127.0.0.1 between
  copies on the same machine (for tests: create room, join, "Ready" and the network round work
  with two copies). The game reads its datagrams into one slot per player and handles them after,
  so a sender's datagrams go to it one per frame, and one per such receive round
  (`net_recv_round`): a round that straddled two frames could wipe a datagram before it was read.
- [x] Stage 7 — mod 3: turn-based network rounds (above).
