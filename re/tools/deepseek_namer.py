#!/usr/bin/env python3
"""Suggest names for Ghidra-exported functions with DeepSeek, bottom-up through the call graph.

Leaf functions are named first, so when a caller is sent, the names of the functions it
calls are already known. Output is append-only and resumable.

  input : functions.jsonl  (ghidra_scripts/ExportFunctions.java)
  output: names.jsonl      (ghidra_scripts/ApplyNames.java)
  key   : $DEEPSEEK_API_KEY or ~/.config/deepseek/key

Examples:
  python3 deepseek_namer.py export/functions.jsonl export/names.jsonl --sample 40
  python3 deepseek_namer.py export/functions.jsonl export/names.jsonl --workers 16
"""
import argparse
import json
import os
import random
import re
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

BASE_URL = os.environ.get("DEEPSEEK_BASE_URL", "https://api.deepseek.com")
DEFAULT_MODEL = os.environ.get("DEEPSEEK_MODEL", "deepseek-chat")
MAX_CODE_CHARS = 14000
MAX_STRINGS = 40
DEFAULT_NAME_RE = re.compile(r"^(FUN|thunk_FUN)_[0-9a-fA-F]+$")
VALID_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)?$")

SYSTEM_PROMPT = """You are an expert reverse engineer helping decompile the PSP game \
"Pangya: Fantasy Golf" (ULUS10438, 2008, developer Ntreev). The executable is C++ \
(STLport, internal project "projectg_psp", engine library "wangreal"; source files include \
GolfDoc.cpp, FxBox.cpp, FxSequence.cpp, wsound.cpp, TaskManager.cpp, QuadTree.cpp, \
ClientSetting.cpp). The CPU is MIPS Allegrex; you get Ghidra pseudo-C. PSP OS calls appear \
as sce* functions (sceIo*, sceGu*, sceKernel*, ...).

Propose a name for the given function.
Rules:
- Strings such as "CFxBox::LoadSequence", log messages and source paths are strong evidence; use them.
- Use "Class::Method" only with real evidence of the class (a string naming it, or callees \
already named as methods of that class whose `this` is passed through). Otherwise a plain name.
- PascalCase names (e.g. LoadSequence, UpdateBallFlight, FreeTextureCache).
- Do not invent specifics without evidence. Trivial functions get honest names \
(e.g. GetField_0x24, MemcpyWrapper, StubReturnZero).
- confidence is your realistic probability (0.0-1.0) that the name reflects what the function does.

Reply ONLY with JSON: {"name": string, "summary": string (1-2 sentences in Brazilian \
Portuguese), "confidence": number}"""


class FatalApiError(Exception):
    pass


def load_key():
    key = os.environ.get("DEEPSEEK_API_KEY")
    if key:
        return key.strip()
    path = Path.home() / ".config" / "deepseek" / "key"
    if path.exists():
        return path.read_text().strip()
    sys.exit("No API key: set DEEPSEEK_API_KEY or create ~/.config/deepseek/key")


def call_deepseek(key, model, user_msg, retries=6):
    body = json.dumps({
        "model": model,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_msg},
        ],
        "response_format": {"type": "json_object"},
        "temperature": 0.2,
        "max_tokens": 400,
    }).encode()
    delay = 2.0
    for attempt in range(retries):
        req = urllib.request.Request(
            BASE_URL + "/chat/completions",
            data=body,
            headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(req, timeout=180) as resp:
                return json.loads(resp.read())
        except urllib.error.HTTPError as e:
            detail = e.read()[:300].decode("utf-8", "replace")
            if e.code in (401, 402, 403):
                raise FatalApiError(f"HTTP {e.code}: {detail}") from None
            if e.code not in (429, 500, 502, 503, 504) or attempt == retries - 1:
                raise RuntimeError(f"HTTP {e.code}: {detail}") from None
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            if attempt == retries - 1:
                raise RuntimeError(f"network: {e}") from None
        time.sleep(delay)
        delay = min(delay * 2, 60)
    raise RuntimeError("unreachable")


def sanitize_name(raw):
    if not isinstance(raw, str):
        return None
    name = raw.strip().replace(" ", "_")
    name = re.sub(r"[^A-Za-z0-9_:]", "_", name)
    if "::" in name:
        cls, _, meth = name.rpartition("::")
        name = f"{cls.replace('::', '_')}::{meth}"
    return name if VALID_NAME_RE.match(name) and not DEFAULT_NAME_RE.match(name) else None


def build_prompt(rec, display_name):
    code = rec.get("c") or ""
    if len(code) > MAX_CODE_CHARS:
        code = code[:MAX_CODE_CHARS] + "\n/* ... truncated ... */"
    callees = sorted({display_name(c) for c in rec.get("callees", [])})
    callers = [display_name(c) for c in rec.get("callers", [])]
    strings = rec.get("strings", [])[:MAX_STRINGS]
    parts = [
        f"Address: {rec['addr']}  Size: {rec['size']} bytes",
        f"Current signature: {rec.get('signature')}",
        f"Calls ({len(callees)}): " + (", ".join(callees[:60]) or "none"),
        f"Called by ({len(callers)}): " + (", ".join(callers[:12]) or "none"),
        "Referenced strings:\n" + ("\n".join(f"  {json.dumps(s)}" for s in strings) or "  none"),
        "Pseudo-C:\n" + code,
    ]
    return "\n\n".join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("functions", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--limit", type=int, default=0, help="stop after N functions (0 = all)")
    ap.add_argument("--sample", type=int, default=0,
                    help="name N random functions that reference strings (quality check), ignoring order")
    ap.add_argument("--min-confidence", type=float, default=0.5,
                    help="suggested names below this are not propagated to callers")
    args = ap.parse_args()

    recs = {}
    with args.functions.open(encoding="utf-8") as fh:
        for line in fh:
            r = json.loads(line)
            recs[r["addr"]] = r

    results = {}
    if args.out.exists():
        with args.out.open(encoding="utf-8") as fh:
            for line in fh:
                if line.strip():
                    r = json.loads(line)
                    results[r["addr"]] = r

    todo_all = {
        a for a, r in recs.items()
        if not r.get("thunk") and r.get("c") and DEFAULT_NAME_RE.match(r["name"].split("::")[-1])
    }
    remaining = todo_all - results.keys()
    used_names = {r["name"] for r in recs.values()} | {r["name"] for r in results.values() if r.get("name")}
    lock = threading.Lock()
    usage = {"prompt": 0, "completion": 0, "cache_hit": 0, "calls": 0, "errors": 0}

    def display_name(ref):
        res = results.get(ref["addr"])
        if res and res.get("name") and (res.get("confidence") or 0) >= args.min_confidence:
            return res["name"]
        return ref["name"]

    pending = {a: {c["addr"] for c in recs[a].get("callees", []) if c["addr"] in todo_all and c["addr"] != a}
               for a in remaining}

    key = load_key()
    print(f"{len(todo_all)} functions with default names, {len(results)} already done, "
          f"{len(remaining)} to go. model={args.model}", flush=True)

    out_fh = args.out.open("a", encoding="utf-8")

    def work(addr):
        rec = recs[addr]
        resp = call_deepseek(key, args.model, build_prompt(rec, display_name))
        content = resp["choices"][0]["message"]["content"]
        try:
            parsed = json.loads(content)
        except json.JSONDecodeError:
            parsed = {}
        name = sanitize_name(parsed.get("name"))
        try:
            conf = max(0.0, min(1.0, float(parsed.get("confidence", 0))))
        except (TypeError, ValueError):
            conf = 0.0
        u = resp.get("usage", {})
        with lock:
            if name:
                base, n = name, 2
                while name in used_names:
                    name, n = f"{base}_{n}", n + 1
                used_names.add(name)
            row = {"addr": addr, "old_name": rec["name"], "name": name,
                   "summary": parsed.get("summary"), "confidence": conf, "model": args.model}
            results[addr] = row
            out_fh.write(json.dumps(row, ensure_ascii=False) + "\n")
            out_fh.flush()
            usage["calls"] += 1
            usage["prompt"] += u.get("prompt_tokens", 0)
            usage["completion"] += u.get("completion_tokens", 0)
            usage["cache_hit"] += u.get("prompt_cache_hit_tokens", 0)
        return row

    def run_batch(batch):
        with ThreadPoolExecutor(max_workers=args.workers) as ex:
            futs = {ex.submit(work, a): a for a in batch}
            for fut in as_completed(futs):
                a = futs[fut]
                try:
                    row = fut.result()
                    print(f"  {a} {recs[a]['name']} -> {row['name']} ({row['confidence']:.2f})", flush=True)
                except FatalApiError as e:
                    ex.shutdown(cancel_futures=True)
                    sys.exit(f"Fatal API error, stopping: {e}")
                except Exception as e:  # keep going; failed ones are retried on the next run
                    with lock:
                        usage["errors"] += 1
                    print(f"  {a} ERROR {e}", file=sys.stderr, flush=True)

    budget = args.limit or len(remaining)
    try:
        if args.sample:
            pool = sorted(a for a in remaining if recs[a].get("strings"))
            random.seed(1)
            run_batch(random.sample(pool, min(args.sample, len(pool))))
        else:
            wave = 0
            while remaining and budget > 0:
                ready = [a for a in remaining if not (pending[a] & remaining)]
                if not ready:  # recursion cycle: release the least-blocked functions
                    least = min(len(pending[a] & remaining) for a in remaining)
                    ready = [a for a in remaining if len(pending[a] & remaining) == least]
                ready = sorted(ready)[:budget]
                wave += 1
                print(f"wave {wave}: {len(ready)} functions ({len(remaining)} left)", flush=True)
                run_batch(ready)
                remaining -= set(ready)
                budget -= len(ready)
    finally:
        out_fh.close()
        print(f"\ncalls={usage['calls']} errors={usage['errors']} prompt_tokens={usage['prompt']} "
              f"(cache hits {usage['cache_hit']}) completion_tokens={usage['completion']}")


if __name__ == "__main__":
    main()
