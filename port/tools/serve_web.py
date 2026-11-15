#!/usr/bin/env python3
"""Local dev server for build/web with HTTP Range support (127.0.0.1 only).

  python3 tools/serve_web.py [--port 8642] [--iso PATH]

The ISO is served at /game.iso (the page reads only the byte ranges it needs). The game's save
lives in the project, in build/ms0/PSP/SAVEDATA (the same folder as the native build):
GET /saves/list, GET /saves/<path>, PUT /saves/<path> (the previous version is kept as .bak).
Players: POST /account/register {name, password, copy} and POST /account/login {name, password}
return {name, token}; with the header X-Pangya-Session: token the /saves/ calls use that player's
own folder, build/profiles/<name>/ms0 (a new player can start from a copy of the main save, with
their name in it). Without the header they use the main save.
Texture pack (HD textures): GET /textures/index.txt, /textures/<file>.qoi and the HD font's
/textures/font-<key>.qoi|txt from build/textures (see tools/textures.py). The sign-in screen's pictures come from the ISO: GET /art/<name>.png and
/art/<name>.json (tools/isoart.py; upscaled when the pack has them); GET /status says what this
server offers.

Ad hoc play: GET /livekit-token?room=R&identity=I returns {url, token} for the LiveKit room
"pangya-R", from LIVEKIT_URL / LIVEKIT_API_KEY / LIVEKIT_API_SECRET in port/.env (see web/net.js).

A password for the whole page (for a server others reach, e.g. through a tunnel):
python3 tools/serve_web.py --set-password NAME asks for it and keeps it in port/.htpasswd (one
"name:pbkdf2_sha256$rounds$salt$hash" line per name; --auth FILE for another file). While that
file exists every request needs one of its names and passwords (HTTP Basic: the browser asks).
"""
import argparse
import base64
import getpass
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import sys
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import isoart  # noqa: E402


def read_env(path):
    env = {}
    try:
        for line in Path(path).read_text().splitlines():
            k, sep, v = line.strip().partition("=")
            if sep and not k.startswith("#"):
                env[k.strip().removeprefix("export ").strip()] = v.strip().strip("'\"")
    except OSError:
        pass
    return env


def b64url(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def livekit_token(key, secret, room, identity, hours=6):
    """An access token (JWT, HS256) to join `room` as `identity` and exchange data messages."""
    now = int(time.time())
    claims = {"iss": key, "sub": identity, "name": identity, "nbf": now - 10, "exp": now + hours * 3600,
              "video": {"room": room, "roomJoin": True, "canPublish": True, "canSubscribe": True,
                        "canPublishData": True}}
    part = lambda d: b64url(json.dumps(d, separators=(",", ":")).encode())
    body = part({"alg": "HS256", "typ": "JWT"}) + "." + part(claims)
    return body + "." + b64url(hmac.new(secret.encode(), body.encode(), hashlib.sha256).digest())


NAME_RE = re.compile(r"[A-Za-z0-9]{1,6}")  # the game's player names: its name entry takes 6 characters
PBKDF2_ROUNDS = 200_000


def hash_password(password, salt):
    return hashlib.pbkdf2_hmac("sha256", password.encode(), salt, PBKDF2_ROUNDS).hex()


def read_htpasswd(path):
    """name -> (salt, hash) from an --auth file; empty without one."""
    users = {}
    try:
        for line in Path(path).read_text().splitlines():
            name, sep, rec = line.strip().partition(":")
            parts = rec.split("$")
            if sep and len(parts) == 4 and parts[0] == "pbkdf2_sha256" and int(parts[1]) == PBKDF2_ROUNDS:
                users[name] = (bytes.fromhex(parts[2]), parts[3])
    except (OSError, ValueError):
        pass
    return users


def set_password(path, name):
    if not name or ":" in name:
        raise SystemExit("a name without ':' please")
    pw = getpass.getpass(f"Password for {name}: ")
    if len(pw) < 4 or pw != getpass.getpass("Again: "):
        raise SystemExit("the two passwords differ, or it is shorter than 4 characters: nothing changed")
    salt = secrets.token_bytes(16)
    keep = [line for line in (Path(path).read_text().splitlines() if Path(path).is_file() else [])
            if line.strip() and line.partition(":")[0] != name]
    keep.append(f"{name}:pbkdf2_sha256${PBKDF2_ROUNDS}${salt.hex()}${hash_password(pw, salt)}")
    old = os.umask(0o077)
    try:
        Path(path).write_text("\n".join(keep) + "\n")
    finally:
        os.umask(old)
    print(f"{path}: the page now asks for {name}'s password (restart the server)")




class Accounts:
    """Players with a password and a save folder each, under build/profiles/<name in lowercase>/;
    sessions (token -> player) kept in build/profiles/sessions.json."""

    def __init__(self, root, main_ms0):
        self.root, self.main_ms0 = root, main_ms0
        self.lock = threading.Lock()
        self.sessions_file = root / "sessions.json"
        try:
            self.sessions = json.loads(self.sessions_file.read_text())
        except (OSError, ValueError):
            self.sessions = {}

    def _save_sessions(self):
        self.root.mkdir(parents=True, exist_ok=True)
        tmp = self.sessions_file.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.sessions))
        tmp.replace(self.sessions_file)

    def _account(self, key):
        try:
            return json.loads((self.root / key / "account.json").read_text())
        except (OSError, ValueError):
            return None

    def _new_session(self, key):
        token = secrets.token_urlsafe(24)
        self.sessions[token] = key
        self._save_sessions()
        return token

    def register(self, name, password, copy):
        if not NAME_RE.fullmatch(name or "") or len(password or "") < 4:
            return 400, "A player name has 1 to 6 letters or digits, and a password at least 4 characters."
        key = name.lower()
        with self.lock:
            if self._account(key):
                return 409, "That player name is taken: pick another one, or log in."
            home = self.root / key
            ms0 = home / "ms0"
            ms0.mkdir(parents=True, exist_ok=True)
            # a player made again (a forgotten password) keeps their own save
            if copy and not any(p.is_file() for p in (ms0 / "PSP" / "SAVEDATA").glob("*/*")):
                for f in sorted((self.main_ms0 / "PSP" / "SAVEDATA").glob("*/*")):
                    if f.is_file() and ".bak" not in f.name and not f.name.endswith(".tmp"):
                        dst = ms0 / f.relative_to(self.main_ms0)
                        dst.parent.mkdir(parents=True, exist_ok=True)
                        data = bytearray(f.read_bytes())
                        # the player's name: 8 characters and a NUL at the start of the game's save
                        if f.parent.name == "ULUS10438" and f.name == "SECURE.BIN" and len(data) > 12:
                            data[3:12] = name.encode().ljust(9, b"\0")
                        dst.write_bytes(bytes(data))
            salt = secrets.token_bytes(16)
            (home / "account.json").write_text(json.dumps(
                {"name": name, "salt": salt.hex(), "hash": hash_password(password, salt), "created": int(time.time())}))
            return 200, {"name": name, "token": self._new_session(key)}

    def login(self, name, password):
        key = (name or "").lower()
        acc = self._account(key) if NAME_RE.fullmatch(name or "") else None
        if not acc or not hmac.compare_digest(acc["hash"], hash_password(password or "", bytes.fromhex(acc["salt"]))):
            return 401, "Wrong player name or password."
        with self.lock:
            return 200, {"name": acc["name"], "token": self._new_session(key)}

    def who(self, token):
        key = self.sessions.get(token or "")
        acc = self._account(key) if key else None
        return (key, acc["name"]) if acc else (None, None)

    def logout(self, token):
        with self.lock:
            if self.sessions.pop(token or "", None):
                self._save_sessions()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8642)
    ap.add_argument("--iso", default=str(Path(__file__).resolve().parents[2] / "Pangya - Fantasy Golf (USA).iso"))
    ap.add_argument("--auth", default=str(Path(__file__).resolve().parents[1] / ".htpasswd"),
                    help="the page's password file (only used when it exists)")
    ap.add_argument("--set-password", metavar="NAME", help="ask for NAME's password, keep it in --auth and quit")
    args = ap.parse_args()
    if args.set_password:
        return set_password(args.auth, args.set_password)
    users = read_htpasswd(args.auth)  # the page's password (HTTP Basic)
    locked = bool(users)  # behind a password: answers never go to a shared cache (a CDN's)
    good_auth = set()  # Authorization headers already checked (PBKDF2 takes a while; the page asks a lot)
    web = Path(__file__).resolve().parents[1] / "build" / "web"
    dump = Path(__file__).resolve().parents[1] / "build" / "webdump"
    ms0 = Path(__file__).resolve().parents[1] / "build" / "ms0"
    textures = Path(__file__).resolve().parents[1] / "build" / "textures"  # texture pack (tools/textures.py)
    save_re = re.compile(r"PSP/SAVEDATA/[A-Za-z0-9_]{1,40}/[A-Za-z0-9_.]{1,40}")
    accounts = Accounts(Path(__file__).resolve().parents[1] / "build" / "profiles", ms0)

    def is_save(rel):  # a save file (not one of the server's own backups)
        return bool(save_re.fullmatch(rel)) and ".." not in rel and ".bak" not in rel and not rel.endswith(".tmp")
    iso = Path(args.iso)
    env_file = Path(__file__).resolve().parents[1] / ".env"

    # The page's pictures from the ISO, upscaled when the texture pack has them: made again only
    # when their own pack file changes (decoding an upscaled one takes a few seconds, so the ones
    # the page shows are made at start).
    art_orig, art_cache, art_lock = {}, {}, threading.Lock()

    def art(name):
        with art_lock:
            if name not in art_orig:
                art_orig[name] = isoart.original(iso, name)
            w, h, rgba, sprites = art_orig[name]
            up = isoart.pack_file(textures, w, h, rgba)
            hit = art_cache.get(name)
            if hit and hit[0] == up:
                return hit[1], hit[2]
            png, scale = isoart.picture(w, h, rgba, up)
            info = dict(w=w, h=h, scale=scale, sprites=sprites)
            art_cache[name] = (up, png, info)
            return png, info

    def pack_count():
        try:
            return sum(1 for line in (textures / "index.txt").read_text().splitlines() if line and not line.startswith("#"))
        except OSError:
            return 0

    pack_size = {}  # the pack's pictures' bytes, for the page's first-visit download (by the index's mtime)

    def pack_bytes():
        try:
            stamp = (textures / "index.txt").stat().st_mtime_ns
            if stamp not in pack_size:
                total = 0
                for line in (textures / "index.txt").read_text().splitlines():
                    f = line.split()
                    if len(f) >= 4 and not line.startswith("#") and (textures / f[3]).is_file():
                        total += (textures / f[3]).stat().st_size
                pack_size.clear()
                pack_size[stamp] = total
            return pack_size[stamp]
        except OSError:
            return 0

    def warm():
        for name in isoart.SHOWN:
            try:
                art(name)
            except Exception as e:  # noqa: BLE001 (no ISO here, or not this game's)
                print(f"art: {name}: {e}", flush=True)
                return
    if iso.is_file():
        threading.Thread(target=warm, daemon=True).start()

    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(web), **kw)

        def authorized(self):
            """Whether to answer the request (else it was answered already)."""
            # With a password file, the request's Basic credentials (else a 401 asking for them). The
            # tab's icons go without: the browser fetches them on its own, without the password just
            # typed, and their 401 made it ask for the password a second time.
            if not users or self.path.split("?")[0] in ("/favicon.ico", "/favicon-32.png", "/apple-touch-icon.png"):
                return True
            h = self.headers.get("Authorization", "")
            if h in good_auth:
                return True
            if h.startswith("Basic "):
                try:
                    name, _, pw = base64.b64decode(h[6:], validate=True).decode().partition(":")
                except (ValueError, UnicodeDecodeError):
                    name, pw = "", ""
                rec = users.get(name)
                if rec and hmac.compare_digest(rec[1], hash_password(pw, rec[0])):
                    good_auth.add(h)
                    return True
                time.sleep(1)  # a wrong password: slowly
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="Pangya", charset="UTF-8"')
            self.send_header("Content-Length", "0")
            self.end_headers()
            return False


        def end_headers(self):
            cache = self.__dict__.pop("cache", None) or ("max-age=31536000, immutable" if self.__dict__.pop("immutable", False) else "no-store")
            if locked and cache != "no-store":
                cache = "private, " + cache  # behind a password: never in a shared cache (a CDN's)
            self.send_header("Cache-Control", cache)
            self.send_header("Accept-Ranges", "bytes")
            super().end_headers()

        def send_bytes(self, data, ctype):
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def save_root(self):
            """The save folder of the player named by the session header (the main one without it);
            None when the header names no session (the page must log in again)."""
            token = self.headers.get("X-Pangya-Session")
            if not token:
                return ms0
            key, _ = accounts.who(token)
            return accounts.root / key / "ms0" if key else None

        def save_path(self):
            root = self.save_root()
            rel = self.path.split("?")[0][len("/saves/"):]
            return (root / rel if is_save(rel) else False) if root else None

        def send_json(self, code, obj):
            if code != 200:
                obj = {"error": obj}
            data = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if not self.authorized():
                return
            route = self.path.split("?")[0]
            m = re.fullmatch(r"/art/([a-z0-9_]{1,24})\.(png|json)", route)
            if m and m.group(1) in isoart.ART:
                try:
                    png, info = art(m.group(1))
                except (OSError, KeyError, ValueError):
                    return self.send_error(404)
                self.cache = "max-age=600"  # the sign-in screen and the phones' note share it
                return self.send_bytes(png, "image/png") if m.group(2) == "png" else self.send_bytes(json.dumps(info).encode(), "application/json")
            if route == "/status":
                env = read_env(env_file)
                return self.send_bytes(json.dumps({
                    "livekit": all(env.get(k) for k in ("LIVEKIT_URL", "LIVEKIT_API_KEY", "LIVEKIT_API_SECRET")),
                    "textures": pack_count(), "textures_bytes": pack_bytes(), "iso": iso.is_file()}).encode(), "application/json")
            if self.path.split("?")[0] == "/account/me":
                key, name = accounts.who(self.headers.get("X-Pangya-Session"))
                return self.send_json(200, {"name": name}) if key else self.send_json(401, "no session")
            if self.path.split("?")[0] == "/livekit-token":
                q = dict(re.findall(r"([a-z]+)=([^&]*)", self.path.partition("?")[2]))
                room, identity = q.get("room", ""), q.get("identity", "")
                if not re.fullmatch(r"[A-Za-z0-9_-]{1,40}", room) or not re.fullmatch(r"[0-9a-f]{12}", identity):
                    return self.send_error(400)
                env = read_env(env_file)
                if not all(env.get(k) for k in ("LIVEKIT_URL", "LIVEKIT_API_KEY", "LIVEKIT_API_SECRET")):
                    return self.send_error(503, "LiveKit is not configured (port/.env)")
                token = livekit_token(env["LIVEKIT_API_KEY"], env["LIVEKIT_API_SECRET"], "pangya-" + room, identity)
                print(f"ad hoc: token for {identity} in room pangya-{room}", flush=True)
                return self.send_bytes(json.dumps({"url": env["LIVEKIT_URL"], "token": token}).encode(), "application/json")
            if self.path.split("?")[0] == "/saves/list":
                base = self.save_root()
                if base is None:
                    return self.send_error(401)
                root = base / "PSP" / "SAVEDATA"
                files = [p.relative_to(base).as_posix() for p in sorted(root.glob("*/*")) if p.is_file()] if root.is_dir() else []
                files = [f for f in files if is_save(f)]
                return self.send_bytes(json.dumps(files).encode(), "application/json")
            if self.path.startswith("/saves/"):
                p = self.save_path()
                if p is None:
                    return self.send_error(401)
                if not p or not p.is_file():
                    return self.send_error(404)
                return self.send_bytes(p.read_bytes(), "application/octet-stream")
            if self.path.split("?")[0] == "/textures/index.txt":  # empty while there is no pack
                f = textures / "index.txt"
                return self.send_bytes(f.read_bytes() if f.is_file() else b"", "text/plain")
            m = re.fullmatch(r"/textures/([0-9a-f]{16}-[0-9a-z]{1,24}\.qoi|font-[0-9a-f]{16}\.(qoi|txt))", self.path.split("?")[0])
            if m:
                f = textures / m.group(1)
                if not f.is_file():
                    return self.send_error(404)
                data = f.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "text/plain" if m.group(2) == "txt" else "image/qoi")
                self.send_header("Content-Length", str(len(data)))
                # a texture's file name changes with its contents: kept by the browser (an HD font's
                # is named after the game's font, so it is asked for again)
                self.immutable = not m.group(1).startswith("font-")
                self.end_headers()
                return self.wfile.write(data)
            if self.path.split("?")[0] != "/game.iso":
                return super().do_GET()
            size = iso.stat().st_size
            m = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
            start, end = (int(m.group(1)), int(m.group(2)) if m and m.group(2) else size - 1) if m else (0, size - 1)
            end = min(end, size - 1)
            self.send_response(206 if m else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(end - start + 1))
            if m:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            with open(iso, "rb") as f:
                f.seek(start)
                left = end - start + 1
                while left > 0:
                    chunk = f.read(min(left, 1 << 20))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    left -= len(chunk)

        def do_PUT(self):
            if not self.authorized():
                return
            p = self.save_path() if self.path.startswith("/saves/") else False
            size = int(self.headers.get("Content-Length", "0"))
            if p is None:
                return self.send_error(401)
            if not p:
                return self.send_error(404)
            if size <= 0 or size > 4 << 20:
                return self.send_error(413)
            data = self.rfile.read(size)
            p.parent.mkdir(parents=True, exist_ok=True)
            if p.exists():
                p.replace(p.with_name(p.name + ".bak"))
            tmp = p.with_name(p.name + ".tmp")
            tmp.write_bytes(data)
            tmp.replace(p)
            print(f"save written: {p} ({size} bytes)", flush=True)
            self.send_response(204)
            self.end_headers()

        def do_POST(self):
            if not self.authorized():
                return
            route = self.path.split("?")[0]
            if route in ("/account/register", "/account/login", "/account/logout"):
                size = int(self.headers.get("Content-Length", "0"))
                try:
                    body = json.loads(self.rfile.read(size)) if 0 < size < 4096 else {}
                except ValueError:
                    body = {}
                if not isinstance(body, dict):
                    body = {}
                if route == "/account/logout":
                    accounts.logout(self.headers.get("X-Pangya-Session"))
                    return self.send_json(200, {})
                if route == "/account/register":
                    code, res = accounts.register(str(body.get("name", "")), str(body.get("password", "")), bool(body.get("copy")))
                else:
                    code, res = accounts.login(str(body.get("name", "")), str(body.get("password", "")))
                if code == 200:
                    print(f"player {res['name']}: {'new' if route.endswith('register') else 'logged in'}", flush=True)
                return self.send_json(code, res)
            # F9 debug captures from the page land in build/webdump/ (see plat_debug_upload); only
            # from this machine's own browser, not through a tunnel (cloudflared adds these headers).
            if route != "/debug-dump":
                self.send_error(404)
                return
            if self.headers.get("Cf-Connecting-Ip") or self.headers.get("X-Forwarded-For"):
                self.send_error(403)
                return
            name = re.sub(r"[^A-Za-z0-9_.-]", "_", (re.search(r"name=([^&]*)", self.path) or [None, "dump.bin"])[1])[:120]
            size = int(self.headers.get("Content-Length", "0"))
            if size <= 0 or size > 64 << 20:
                self.send_error(413)
                return
            dump.mkdir(exist_ok=True)
            (dump / name).write_bytes(self.rfile.read(size))
            print(f"debug capture saved: {dump / name} ({size} bytes)", flush=True)
            self.send_response(204)
            self.end_headers()

        def do_HEAD(self):
            if not self.authorized():
                return
            if self.path.split("?")[0] != "/game.iso":
                return super().do_HEAD()
            self.send_response(200)
            self.send_header("Content-Length", str(iso.stat().st_size))
            self.end_headers()

        def log_message(self, fmt, *a):
            pass

    os.chdir(web)
    print(f"serving {web} on http://127.0.0.1:{args.port}/  (ISO at /game.iso)"
          + (", behind the page's password" if locked else ""), flush=True)
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
