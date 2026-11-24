# Pangya: Fantasy Golf — PC & browser port

If you enjoy this port and feel like it, you can [buy me a coffee](https://buymeacoffee.com/ewtjacquier). Thank you!

A fan-made port of **Pangya: Fantasy Golf** (PSP, 2008) that runs natively on Linux and **in the
browser**, at high resolution, with HD textures, accounts for each player and **online play with
friends**. It is not an emulator: the game's own program was translated, function by function,
into C code that compiles for your PC or for WebAssembly.

> **Bring your own game.** This repository has no part of the game: no ISO, no game code, no game
> files (only the logo and icon of the web page). You build the port from your own copy, dumped from
> your own UMD. Not affiliated with Ntreev Soft, Tomy or anyone who owns Pangya.

Made by fans, for fans: to keep a game we love alive, playable on today's machines and in a
browser tab, and to play it together again.

## Contents

1. [What you get](#what-you-get)
2. [How it works](#how-it-works)
3. [Technologies](#technologies)
4. [What you need](#what-you-need)
5. [Step by step](#step-by-step)
   1. [Get the code](#1-get-the-code)
   2. [Get the decrypted EBOOT](#2-get-the-decrypted-eboot)
   3. [Translate the game to C](#3-translate-the-game-to-c)
   4. [Get FFmpeg](#4-get-ffmpeg)
   5. [Build and play on Linux](#5-build-and-play-on-linux)
   6. [Build and play in the browser](#6-build-and-play-in-the-browser)
   7. [HD textures](#7-hd-textures)
6. [Online play with LiveKit](#online-play-with-livekit)
7. [Sharing it with friends (Cloudflare Tunnel)](#sharing-it-with-friends-cloudflare-tunnel)
8. [Accounts and saves: where everything is kept](#accounts-and-saves-where-everything-is-kept)
9. [Things to know](#things-to-know)
10. [Repository layout](#repository-layout)
11. [Credits](#credits)

## What you get

- **The whole game**, translated to native code: story, tour, the 18 characters, every course,
  shops, collection, movies, music and voices.
- **4x internal resolution** (1920x1088), smooth 60 frames per second.
- **HD textures**: every one of the game's ~5,000 textures was remade at 4x, most with the
  [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) anime model; menu illustrations, interface
  art and lettering, and course textures where that fell short were redrawn with OpenAI's image
  models. Each one was checked against its original. They come as a separate download and never change the game's files: turn
  them off and on in the settings menu.
- **Vector text**: the game's words redrawn sharp with a font at the render scale, in its own
  colors and outlines: the lines it writes as it runs (menus, descriptions, messages) and the
  words baked into its interface art (titles, buttons, the HUD's digits), over the HD textures
  when they are on. The big lettering (a shot's result: PAR, BIRDIE, HOLE IN ONE, OB...; For
  BIRDIE, Your Turn...) and the course names on Free Round's banners keep the textures' own art.
- **A browser version**: the game in a tab, with a settings menu (Esc): fullscreen, keyboard keys
  and gamepad buttons, internal resolution, HD textures, vector text.
- **Players**: each player logs in and keeps their own save; their name is their nickname online.
- **Network Mode online**: the PSP's ad hoc Wi-Fi play, over the internet, through
  [LiveKit](https://livekit.io).
- **Mods** (C code in `port/mods/`, each can be turned off):
  - straight to the title: no logos, no notices, no opening movie;
  - one button on the title: it loads your save, or starts a new game if you have none;
  - all 18 characters, every shop item and the whole Pangya Collection unlocked;
  - everything in the shops (and the Papel Shop's machine) at a third of the price, rounded down;
  - network rounds that keep the right score on every hole;
  - turn-based network rounds of two to four players (everyone plays the same hole in turns,
    like a match), which the original only had in an unfinished form for two.

## How it works

The PSP runs MIPS machine code. Emulators interpret it (or compile it on the fly) while the game
runs. This port does it once, ahead of time: **static recompilation**.

```
your UMD ─▶ ISO ─▶ EBOOT.BIN (encrypted) ─▶ EBOOT_decrypted.elf
                                              │
           re/export/program.json ────────────┤  where each function and jump table is
           (made once with Ghidra)            │  (addresses only: no game code)
                                              ▼
                            tools/prx_load.py + tools/recomp.py
                                              │
                                              ▼
                  port/generated/*.c  ── about 11,000 functions as C code (stays on your PC)
                                              │
              port/runtime/*.c  ──────────────┤  the PSP's system, rewritten: threads, files,
              port/mods/*.c                   │  graphics, sound, video, saves, network
                                              ▼
                        native binary (GCC)  or  WebAssembly (Emscripten)
```

- Each MIPS function becomes a C function; registers live in a struct, the PSP's memory is one
  block of memory, branches become `goto`s.
- The 202 PSP system functions the game calls are implemented by this project's runtime (nothing
  comes from an emulator): graphics go to OpenGL ES 3 / WebGL 2, sound to SDL or WebAudio, the
  game's files are read straight from your ISO.
- Mods replace game functions by address (`port/hooks.txt`) with hand-written C that can still
  call the original; a few change single instructions (`port/patches.txt`).

`port/README.md` has the technical details.

## Technologies

| What | Used for |
|---|---|
| **C** (GNU C11), **GCC**, **Make** | the generated game code, the runtime, the mods |
| **Python 3** | the recompiler, the build tools, the local web server (standard library only); Pillow for the vector text's generator |
| **stb_truetype**, **TeX Gyre Adventor** | the vector text: the font's outlines at any size |
| **SDL2** | window, input and sound in the native build |
| **OpenGL ES 3 / WebGL 2** | the PSP's graphics chip, rendered at 4x |
| **Emscripten** (WebAssembly, Asyncify) | the browser build; the PSP's threads become fibers |
| **FFmpeg** (PPSSPP's builds) | ATRAC3/ATRAC3+ music and voices, H.264 movies |
| **LiveKit** (WebRTC) | online play: the game's network messages between players |
| **Ghidra** (optional) | the analysis that found every function (already done: `re/export/`) |
| **Real-ESRGAN**, **OpenAI image models** | how the HD textures were made (`realesrgan-x4plus-anime` at 4x; `gpt-image` redraws) |

## What you need

**The game**: *Pangya: Fantasy Golf* (USA), **ULUS-10438**, as an ISO dumped from your own UMD
(with a PSP running custom firmware, for example).

**To build** (Linux; on Windows use WSL2 with Ubuntu):

```bash
sudo apt install build-essential pkg-config git python3 python3-pil libsdl2-dev libgles-dev
```

- For the browser build, the **Emscripten SDK** (tested with 3.1.46):
  ```bash
  git clone https://github.com/emscripten-core/emsdk ~/emsdk
  cd ~/emsdk && ./emsdk install 3.1.46 && ./emsdk activate 3.1.46
  export EMSDK=~/emsdk          # the build scripts use it (or: source ~/emsdk/emsdk_env.sh)
  ```
- About **8 GB of RAM** and **4 GB of disk** for building (plus ~2 GB for the HD textures).

**To play**: Linux for the native build; for the browser build, any desktop browser with WebGL 2
(Chrome, Edge, Firefox) on any system. A gamepad is optional.

## Step by step

### 1. Get the code

```bash
git clone https://github.com/<you>/pangya-port.git
```

Put your ISO **next to** the `pangya-port` folder, named `Pangya - Fantasy Golf (USA).iso` (or
pass its path to the commands below):

```
somewhere/
├── Pangya - Fantasy Golf (USA).iso
├── EBOOT_decrypted.elf          ← step 2
└── pangya-port/
    ├── port/
    └── re/
```

### 2. Get the decrypted EBOOT

The game's program is `PSP_GAME/SYSDIR/EBOOT.BIN` inside the ISO, and it is encrypted. Two ways
to decrypt it:

- **With [pspdecrypt](https://github.com/John-K/pspdecrypt)**: take `EBOOT.BIN` out of the ISO
  (any ISO tool, e.g. `7z x "Pangya - Fantasy Golf (USA).iso" PSP_GAME/SYSDIR/EBOOT.BIN`), then
  `pspdecrypt EBOOT.BIN` writes the decrypted file.
- **With [PPSSPP](https://www.ppsspp.org)**: in *Settings → Tools → Developer tools*, turn on the
  option that dumps the decrypted EBOOT when a game boots, then start the game once; the file
  lands in PPSSPP's memory stick folder, under `PSP/SYSTEM/DUMP/`.

Save it as `EBOOT_decrypted.elf` next to the ISO. Its SHA-1 must be
`b868e8e7204d9588b85baea3b78304979ca0694c` (step 3 checks it).

### 3. Translate the game to C

```bash
cd pangya-port/port
tools/generate.sh            # or: tools/generate.sh path/to/EBOOT_decrypted.elf
```

This is the "decompiling" part, done for you: it loads the program the way the PSP's kernel
would (`tools/prx_load.py`: relocations, imports) and translates every function to C
(`tools/recomp.py`), using the map of the program in `re/export/program.json`. It takes about a
minute and writes `port/generated/` (about 30 MB of C). **That folder is the game's code: it is
yours to build, never commit or share it.**

Redoing the analysis yourself is optional: it needs Ghidra, see `re/README.md`.

Then the vector text's description of the interface art:

```bash
make generated/ui_text.spec        # ISO_PATH="path/to/the.iso" if it isn't next to pangya-port
```

It reads the interface's pictures from your ISO and, for every word in them (transcribed in
`tools/uitext/labels.tsv`), finds the font size, weight, colors, outlines and glow that match the
original pixels: the game uses it to redraw those words sharp at any resolution. The first run is
long: about 4 hours of CPU spread over every core (about 20 minutes with 16 threads, an hour with
4); the results are kept in `build/uitext_cache`, so the next ones take seconds. The result,
`generated/ui_text.spec`, is made from the game's art like the rest of `generated/`: yours only.
Without it, only the lines the game writes as it runs get the vector font.

### 4. Get FFmpeg

```bash
tools/get_ffmpeg.sh
```

It downloads [PPSSPP's FFmpeg builds](https://github.com/hrydgard/ppsspp-ffmpeg) (only the
decoding libraries), uses the prebuilt ones for Linux and builds the WebAssembly ones with
Emscripten (a few minutes, once).

### 5. Build and play on Linux

```bash
make -j8
./build/pangya "../../Pangya - Fantasy Golf (USA).iso"
```

The first build takes a while (11,000 functions; `-j8` is fine with 16 GB of RAM, use fewer jobs
with less). Keys: arrows, **Z** = ✕, **X** = ◯, **A** = □, **S** = △, **Q**/**W** = L/R,
**Enter** = Start; USB gamepads work too. Saves go to `build/ms0/PSP/SAVEDATA`.

### 6. Build and play in the browser

```bash
tools/build_web.sh                 # writes build/web/
python3 tools/serve_web.py         # http://127.0.0.1:8642/
```

Open **http://127.0.0.1:8642/**. The server streams the ISO to the page in small slices (only
what the game reads), keeps the saves in the project folder and handles the players' accounts.

- **Log in** or create a player (a new one starts from a copy of the main save), or "Play without
  an account" (the main save). Each browser *window* is one player: two windows, two players. The
  sign-in screen shows the game's title illustration, read from your ISO (in HD when the texture
  pack is installed).
- **Esc** (or the gear in the corner, or Home on a gamepad) opens the settings: fullscreen (also
  F11 or a double click), the volume (30% of the game's own by default), keys, gamepad buttons,
  internal resolution (1x to 4x), HD textures, how the original textures are filtered (smooth,
  sharp or pixels) and the vector text (on by default).
- Without the server (the page opened some other way) the page asks for the ISO file and keeps
  the save in the browser.

### 7. HD textures

Download the texture pack (the `pangya-hd-textures-*.zip` files on this repository's Releases
page) and install it, from `port/`:

```bash
python3 tools/textures.py install ~/Downloads/pangya-hd-textures-*.zip
python3 tools/textures.py status
```

Every texture of the game was upscaled 4x and compared with its original. Most (4,849) come from
Real-ESRGAN's anime model (`realesrgan-x4plus-anime`); 185 (menu illustrations, interface art and
lettering, course textures where the plain upscale fell short) were redrawn with OpenAI's image
models (`gpt-image`), fitted back onto the original's place and outline and reviewed one by one;
8 plain ones are a sharp resize. The pack also brings an **HD font**: the glyphs of the game's own font, upscaled the same way, for the
lines the game writes as it runs (menus, descriptions, messages), so they are sharp too and keep
their look. The pack only **adds** files in `build/textures`: the game looks each texture up while
it runs and draws the HD version in its place, so your ISO and the game's files never change.

**Going back to the originals** (the rollback):

- in the browser, Esc → **HD textures: Off** (instant, and back On whenever you like);
- or remove the pack: `python3 tools/textures.py uninstall`.

No copy of the original textures is needed for that: they always come from your ISO.

## Online play with LiveKit

The PSP plays Network Mode over **ad hoc Wi-Fi**: consoles near each other exchange small
messages. The port emulates that network (`port/runtime/net.c`) and, in the browser, carries the
messages through a **LiveKit room**: LiveKit is a service for real-time apps (WebRTC); here it
only relays the game's tiny messages (about 3 KB/s per player), so its **free plan** is plenty.

1. Create a free account at **[cloud.livekit.io](https://cloud.livekit.io)** and a project.
2. In the project's settings, create an **API key**. Note the three values: the project's
   **URL** (`wss://<your-project>.livekit.cloud`), the **API Key** and the **API Secret**.
3. Write them in `port/.env` (the file is ignored by git: never commit it):
   ```ini
   LIVEKIT_URL=wss://your-project.livekit.cloud
   LIVEKIT_API_KEY=<your API key>
   LIVEKIT_API_SECRET=<your API secret>
   ```
4. Restart `serve_web.py`. The login screen shows **Network play: ready**.
5. In the game: **Network Mode**, one player creates a room and the others join it, as on PSPs.

How it fits together: the page asks *your* server for a pass (`/livekit-token`), and your server
signs it with the secret, which never leaves your PC. Everyone who opens the game from the same
server meets in the same room (`?PANGYA_ROOM=name` in the address makes a separate one). To try
it alone, open two browser windows and log in as two players.

## Sharing it with friends (Cloudflare Tunnel)

To play with friends who are not on your network, one person (**the host**) runs the server
and shares it through a **[Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/)**:
a secure HTTPS address for your local server, with no router setup.

What happens then: your friends open the address; their browser loads the game from your server
and runs it **on their own computer**; their saves and accounts live on yours; online play goes
through LiveKit with passes your server signs (they need no LiveKit account).

**Quick test** (a temporary random address):

```bash
# install cloudflared: https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/downloads/
cloudflared tunnel --url http://127.0.0.1:8642
```

It prints an address like `https://something-random.trycloudflare.com`: send it to your friends.
It lasts while the command runs.

**Your own address, open only to your friends** (free, needs a domain on Cloudflare):

```bash
cloudflared tunnel login                                     # pick your domain
cloudflared tunnel create pangya                             # prints the tunnel's id
cloudflared tunnel route dns pangya pangya.your-domain.com
```

`~/.cloudflared/config.yml`:

```yaml
tunnel: pangya
credentials-file: /home/<you>/.cloudflared/<tunnel-id>.json
ingress:
  - hostname: pangya.your-domain.com
    service: http://127.0.0.1:8642
  - service: http_status:404
```

```bash
cloudflared tunnel run pangya
```

Then, in Cloudflare's dashboard, **Zero Trust → Access → Applications → Add a self-hosted
application** for `pangya.your-domain.com`, with a policy that allows your friends' e-mail
addresses: each one logs in with a code sent to their e-mail before the game opens.

Or a simpler lock, one name and password for everyone (the browser asks for them before the page
opens):

```bash
python3 tools/serve_web.py --set-password pangya   # asks for the password (kept hashed in port/.htpasswd)
```

and restart `serve_web.py`. `--set-password` again changes it; delete `port/.htpasswd` to open the
page again. Some in-app browsers (chat apps') never show the browser's password prompt: open the
link in a regular browser.

**Please read this before sharing:**

- Whoever opens the address downloads the game's data from **your** ISO and the translated game
  code. Share it **only with friends who own the game**, never publicly: use the Cloudflare Access
  policy above for anything longer than a quick test.
- Anyone who reaches the page can create a player on your server: another reason for Access.
- The game streams from your PC: your upload speed is everyone's loading speed (the page reads
  only what the game asks for, as it goes, and HD textures as they are first needed).

## Accounts and saves: where everything is kept

There is no database server: everything is plain files in your `port/build/` folder, written by
`serve_web.py` (and by the native build, for the main save).

| What | Where |
|---|---|
| Main save (native build, and "Play without an account") | `build/ms0/PSP/SAVEDATA/` |
| A player's account (name, password hash) | `build/profiles/<name>/account.json` |
| A player's save | `build/profiles/<name>/ms0/PSP/SAVEDATA/` |
| Logged-in sessions | `build/profiles/sessions.json` |
| HD textures | `build/textures/` |
| Settings of the page (keys, gamepad, switches) | the browser's `localStorage` |
| Saves when the page runs without the server | the browser's IndexedDB |

- Passwords are never stored: only a salted **PBKDF2-SHA256** hash (200,000 rounds).
- Every time a save is written, the previous one is kept as `.bak` next to it.
- A new player starts from a copy of the main save, with their name written into it.
- **Back up**: copy `build/ms0` and `build/profiles`.
- **Forgotten password**: the host deletes `build/profiles/<name>/account.json`; the player then
  creates the same name again with a new password, and keeps their save.

## Things to know

- **Legal**: this repository holds only the port's own code, the analysis data (addresses and
  names), the transcription of the words in the interface's pictures (for the vector text), the
  game's logo for the sign-in page and Pangya's icon (from pangya.community) for the browser tab. The game, its code and its art belong to their owners. Don't publish builds, generated
  code, ISOs or EBOOTs. The HD texture pack is derived from the game's art and meant for people
  who own the game.
- **Native builds**: Linux only for now (the browser build runs everywhere). Native online play
  only works between copies on the same machine (for testing); online play is a browser feature.
- **Performance**: full speed on a laptop with integrated graphics (where it was made). The HD
  textures need more video memory; turn them off if a weak machine stutters.
- **Phones and tablets** get a note asking for a computer instead of the game (nothing of the
  game is downloaded there): it needs a keyboard or a gamepad, and a computer's memory.
- **Sound** starts after the first click or key press (browsers' rule).
- **Saves** are the game's own files: the native build and the browser build share the main one.
- Something wrong? In the browser, **F9** writes a diagnostic of the current frame into
  `build/webdump/` on the host (only from the host's own browser). The browser's console (F12)
  shows the game's log.

## Repository layout

```
port/
  runtime/        the PSP's system, rewritten (CPU helpers, threads, files, graphics, sound,
                  video, saves, network, texture packs)
  mods/           the mods (C), hooked by address in hooks.txt (and patches.txt)
  web/            the browser page (shell.html), audio and LiveKit glue
  tools/          generate.sh, recomp.py, prx_load.py, serve_web.py, textures.py, build scripts;
                  uitext/: the vector text's generator and the transcribed interface words
  assets/fonts/   TeX Gyre Adventor, the vector text's font
  third_party/    FFmpeg's headers (the libraries come from tools/get_ffmpeg.sh), stb_truetype
re/
  export/         the analysis: program.json (functions, jump tables), imports, fixups
  ghidra_scripts/ how program.json was made (Ghidra)
  mods/, tools/   patches and cheats for the original game on a PSP or PPSSPP
```

## Credits

- **Ntreev Soft** and **Tomy**, for Pangya: Fantasy Golf.
- [PPSSPP](https://www.ppsspp.org): the reference for how the PSP behaves, its FFmpeg builds and
  its tables of PSP system function names.
- [Ghidra](https://ghidra-sre.org), [Emscripten](https://emscripten.org),
  [SDL](https://www.libsdl.org), [FFmpeg](https://ffmpeg.org), [LiveKit](https://livekit.io)
  (client library, Apache-2.0), [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) (BSD-3-Clause),
  [TeX Gyre Adventor](https://www.gust.org.pl/projects/e-foundry/tex-gyre/adventor) (GUST Font
  License), [stb_truetype](https://github.com/nothings/stb) (public domain / MIT).
- Everyone keeping Pangya alive at [pangya.community](https://pangya.community).

## License

The port's own code: see [LICENSE](LICENSE).
