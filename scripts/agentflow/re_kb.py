#!/usr/bin/env python3
"""ND-RE knowledge queue - the half an agent may run without touching IDA.

The RE knowledge base is text under ``docs/agent/re/``.  The ``.i64`` is a
*derived, single-writer* artifact: 296 MiB exe -> 3.67 GiB ``.i64``, plus
~3.8 GiB of live ``.id0``/``.id1`` while it is open.  IDA opens databases
read-write, so a second opener is a lock fight or a full copy - it is never
opened just to read something an agent could get from text.

    re_kb.py status      KB / builds / request counts, and whether the idb is in use
    re_kb.py req add     enqueue one function (offline signature check first)
    re_kb.py req ls      pending vs served
    re_kb.py req check   re-verify pending signatures against the current exe
    re_kb.py served ls   what the anchor session has produced
    re_kb.py idb set     register the .i64 path once (machine-specific, git-ignored)

The other half is ``re_serve.py``, which runs *inside* the one anchor IDA
session (File > Script file... / Alt+F7).  It never saves, renames or retypes.

Note the module name: it must not be ``re.py``.  This directory is ``sys.path[0]``
when any script here runs, so an ``re.py`` here shadows the standard library and
takes ``flow.py`` down with it.
"""

from __future__ import annotations

import argparse
import datetime
import json
import mmap
import re
import sys
from pathlib import Path

KEY_RE = re.compile(r"^[a-z][a-z0-9_]{1,47}$")
ROLES = ("pseudo", "proto", "xrefs", "disasm", "callees", "strings")
DEFAULT_WANT = "pseudo,proto,xrefs"
IDB_IN_USE_MINUTES = 30


# --------------------------------------------------------------------------- io

def now() -> str:
    return datetime.datetime.now().astimezone().isoformat(timespec="milliseconds")


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def re_dir(root: Path) -> Path:
    return root / "docs" / "agent" / "re"


def read_json(path: Path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def write_json(path: Path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def load_jsonl(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except ValueError:
            print(f"re: skipping malformed line in {path}: {line[:80]}", file=sys.stderr)
    return out


def append_jsonl(path: Path, event: dict) -> dict:
    path.parent.mkdir(parents=True, exist_ok=True)
    event = {"ts": now(), **event}
    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(event, ensure_ascii=False) + "\n")
    return event


def requests(re_dir_: Path) -> list[dict]:
    """Every request, oldest first, one row per id (last line wins so a rename can be
    appended instead of editing a line somebody else wrote)."""
    rows: list[dict] = []
    for path in sorted((re_dir_ / "requests").glob("*.jsonl")):
        for item in load_jsonl(path):
            item.setdefault("_src", path.name)
            rows.append(item)
    latest: dict[str, dict] = {}
    for item in rows:
        latest[item.get("id") or ""] = item
    out = list(latest.values())
    out.sort(key=lambda r: (r.get("ts") or "", r.get("id") or ""))
    return out


def served(re_dir_: Path) -> list[dict]:
    return load_jsonl(re_dir_ / "served.jsonl")


def outcome_by_req(re_dir_: Path) -> dict[str, dict]:
    """Last outcome per request id; failures included so a bad request stops spinning."""
    out: dict[str, dict] = {}
    for item in served(re_dir_):
        out[item.get("req") or ""] = item
    return out


def pending(re_dir_: Path) -> list[dict]:
    seen = outcome_by_req(re_dir_)
    out = []
    for req in requests(re_dir_):
        if seen.get(req.get("id") or "") and not req.get("force"):
            continue
        out.append(req)
    return out


# ------------------------------------------------------------------ the module

def instance(root: Path) -> Path | None:
    for name in ("config.local.json", "config.json"):
        raw = read_json(root / "scripts" / "agentflow" / name, {}).get("instance") or ""
        if raw:
            return Path(raw)
    return None


def exe_path(root: Path, args) -> Path | None:
    if getattr(args, "exe", None):
        return Path(args.exe)
    inst = instance(root)
    if inst:
        cand = inst / "Minecraft.Windows.exe"
        if cand.is_file():
            return cand
    return None


def scan_matches(path: Path, pattern: str, limit: int = 8) -> list[int]:
    """File offsets where a `48 89 ?? ..` style pattern matches.  Pure offline."""
    tokens = pattern.split()
    if not tokens:
        raise ValueError("empty pattern")
    parts = []
    for tok in tokens:
        if tok in ("?", "??", "**"):
            parts.append(b".")
        else:
            if len(tok) != 2:
                raise ValueError(f"bad pattern token {tok!r} (want 2 hex digits or ??)")
            parts.append(re.escape(bytes([int(tok, 16)])))
    rx = re.compile(b"".join(parts), re.DOTALL)
    hits: list[int] = []
    with path.open("rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
        for m in rx.finditer(mm):
            hits.append(m.start())
            if len(hits) >= limit:
                break
    return hits


def sig_check(root: Path, args, pattern: str) -> dict:
    exe = exe_path(root, args)
    if exe is None or not exe.is_file():
        return {"checked": False,
                "reason": "no exe (pass --exe or configure scripts/agentflow/config.local.json)"}
    hits = scan_matches(exe, pattern)
    st = exe.stat()
    return {
        "checked": True,
        "hits": len(hits),
        "file_off": hex(hits[0]) if hits else None,
        "exe": str(exe),
        "exe_size": st.st_size,
        "exe_mtime": int(st.st_mtime),
    }


def idb_record(re_dir_: Path) -> dict:
    return read_json(re_dir_ / "idb.local.json", {})


def idb_live(re_dir_: Path) -> dict | None:
    """Loose .id0/.id1 beside the .i64 = a session is (or was) open on it."""
    rec = idb_record(re_dir_)
    raw = rec.get("path") or ""
    if not raw:
        return None
    idb = Path(raw)
    base = idb.with_suffix("") if idb.suffix else idb
    loose = {}
    for suffix in (".id0", ".id1", ".id2", ".nam", ".til"):
        side = Path(str(base) + suffix)
        if side.is_file():
            st = side.stat()
            loose[suffix] = {"size": st.st_size, "mtime": int(st.st_mtime)}
    total = (idb.stat().st_size if idb.is_file() else 0) + sum(v["size"] for v in loose.values())
    newest = max((v["mtime"] for v in loose.values()), default=0)
    age_min = (datetime.datetime.now().timestamp() - newest) / 60 if newest else None
    return {
        "path": str(idb),
        "exists": idb.is_file(),
        "size": idb.stat().st_size if idb.is_file() else 0,
        "loose": loose,
        "total": total,
        "newest_age_min": age_min,
        "in_use": bool(loose) and (age_min is not None and age_min < IDB_IN_USE_MINUTES),
    }


def human(n: int) -> str:
    return f"{n / (1024 ** 3):.2f} GiB" if n >= 1024 ** 3 else f"{n / (1024 ** 2):.1f} MiB"


# ------------------------------------------------------------------- commands

def cmd_status(root: Path, args) -> int:
    kb = re_dir(root)
    reqs = requests(kb)
    done = served(kb)
    pend = pending(kb)
    ok = [d for d in done if d.get("ok")]
    fail = [d for d in done if not d.get("ok")]

    print(f"repo      : {root}")
    print(f"kb        : {kb}")

    builds = sorted((kb / "builds").glob("*")) if (kb / "builds").is_dir() else []
    builds = [b for b in builds if b.is_dir()]
    print(f"builds    : {len(builds)}")
    for b in builds:
        meta = read_json(b / "meta.json", {})
        files = list((b / "decomp").glob("*")) if (b / "decomp").is_dir() else []
        print(f"  {b.name}  {meta.get('version') or '?'}  {len(files)} decomp files  "
              f"{meta.get('exe') or '?'}")

    print(f"requests  : {len(reqs)} total / {len(pend)} pending / {len(ok)} served / {len(fail)} failed")
    for req in pend[:10]:
        print(f"  pending {req.get('id')} {str(req.get('key')):<28} {fmt_resolve(req)}")
    if len(pend) > 10:
        print(f"  ... and {len(pend) - 10} more")

    canon = list(canon_active(kb).values())
    if canon:
        resolved = [e for e in canon if e.get("status") == "resolved-in-26.51"]
        located = [e for e in canon if e.get("status") == "located-in-26.51"]
        dead = len([e for e in canon_by_key(kb).values() if e.get("status") == "superseded"])
        print(f"canon     : {len(canon)} function(s) - {len(resolved)} by sig, "
              f"{len(located)} by rva, {len(canon) - len(resolved) - len(located)} name-only; "
              f"{dead} renamed; {len(canon_refuted(kb))} refuted")
    else:
        print("canon     : empty (phase 2 draft: `re_kb.py canon import`)")

    exe = exe_path(root, args)
    if exe:
        st = exe.stat()
        stamp = datetime.datetime.fromtimestamp(st.st_mtime)
        print(f"exe       : {exe}  {human(st.st_size)}  mtime {stamp:%Y-%m-%d %H:%M}")
    else:
        print("exe       : (none configured)")

    live = idb_live(kb)
    if live is None:
        print("idb       : 未登记 —— `re_kb.py idb set --path <...i64>` 一次即可（machine-specific）")
    elif not live["exists"]:
        print(f"idb       : {live['path']}  (missing)")
    else:
        print(f"idb       : {live['path']}  {human(live['size'])}")
        if live["loose"]:
            parts = ", ".join(f"{k} {human(v['size'])}" for k, v in sorted(live["loose"].items()))
            flag = "有人在用（别开）" if live["in_use"] else "上次会话的残留（未干净关闭）"
            print(f"  loose   : {parts}  -> {flag}, 合计 {human(live['total'])}")
        else:
            print("  loose   : none -> 空闲；但 agent 仍应只跑 re_serve，不新开 db")
    return 0


def fmt_resolve(req: dict) -> str:
    spec = req.get("resolve") or {}
    value = spec.get("value")
    if spec.get("kind") == "sig" and isinstance(value, str) and len(value) > 24:
        value = value[:24] + "..."
    return f"{spec.get('kind')}:{value}"


def _next_id(kb: Path) -> str:
    n = 0
    for req in requests(kb):
        m = re.match(r"RQ-(\d+)$", req.get("id") or "")
        if m:
            n = max(n, int(m.group(1)))
    return f"RQ-{n + 1:04d}"


def _norm(sig: str) -> str:
    return "".join(sig.split()).upper()


def cmd_req_add(root: Path, args) -> int:
    kb = re_dir(root)
    if not KEY_RE.match(args.key):
        print(f"re: bad --key {args.key!r}: lowercase snake_case, 2-48 chars "
              f"(it becomes the file name: builds/<sha>/decomp/{args.key}.c)")
        return 1

    given = [(k, v) for k, v in (("sig", args.sig), ("rva", args.rva),
                                ("addr", args.addr), ("name", args.name)) if v]
    if len(given) != 1:
        print("re: exactly one of --sig / --rva / --addr / --name is required")
        return 1
    kind, value = given[0]
    if not args.why:
        print("re: --why is required: what question does reading this function answer?")
        return 1
    want = [w.strip() for w in (args.want or DEFAULT_WANT).split(",") if w.strip()]
    bad = [w for w in want if w not in ROLES]
    if bad or not want:
        print(f"re: unknown --want {bad}; allowed: {', '.join(ROLES)}")
        return 1

    for req in requests(kb):
        if req.get("key") == args.key:
            print(f"re: key {args.key!r} already requested as {req.get('id')} "
                  f"({req.get('_src')}) - one key maps to one function")
            return 1
        other = req.get("resolve") or {}
        if other.get("kind") == kind:
            same = (_norm(other.get("value") or "") == _norm(value) if kind == "sig"
                    else str(other.get("value")).lower() == str(value).lower())
            if same:
                print(f"re: the same {kind} is already {req.get('id')} under key "
                      f"{req.get('key')!r}; reuse that key instead of adding a synonym")
                return 1

    check = {"checked": False, "reason": "skipped (--no-check)"}
    if kind == "sig" and not args.no_check:
        check = sig_check(root, args, value)
        if check.get("checked") and check.get("hits") != 1:
            hits = check.get("hits")
            if hits == 0:
                why = "matches nothing"
            elif hits >= 8:
                why = "matches 8+ places (ambiguous)"
            else:
                why = f"matches {hits} places (ambiguous)"
            print(f"re: signature {why} in {check['exe']}")
            print("    a fact keyed on an ambiguous pattern is worse than no fact; "
                  "extend it until it is unique, or pass --allow-ambiguous knowing it is weak")
            if not args.allow_ambiguous:
                return 1

    by = re.sub(r"[^a-z0-9_.-]", "-", (args.by or "agent").lower()) or "agent"
    rec = {
        "kind": "re-req",
        "id": _next_id(kb),
        "key": args.key,
        "resolve": {"kind": kind, "value": value},
        "want": want,
        "why": args.why,
        "by": by,
        "sig_check": check,
    }
    if args.force:
        rec["force"] = True
    append_jsonl(kb / "requests" / f"{by}.jsonl", rec)
    print(f"re: {rec['id']} {args.key} queued ({kind}) -> docs/agent/re/requests/{by}.jsonl")
    if check.get("checked"):
        print(f"    offline check: {check['hits']} hit(s) at {check.get('file_off')}")
    else:
        print(f"    offline check: {check.get('reason')}")
    print("    next: 锚点会话里 File > Script file... (Alt+F7) 跑 scripts/agentflow/re_serve.py")
    return 0


def cmd_req_ls(root: Path, args) -> int:
    kb = re_dir(root)
    seen = outcome_by_req(kb)
    rows = requests(kb)
    if not rows:
        print("re: no requests yet")
        return 0
    print(f"{'id':<8} {'key':<30} {'resolve':<34} {'want':<18} status")
    shown = 0
    for req in rows:
        done = seen.get(req.get("id") or "")
        if done:
            status = "served" if done.get("ok") else f"failed: {done.get('note') or '?'}"
        else:
            status = "pending"
        if args.pending_only and status != "pending":
            continue
        print(f"{req.get('id'):<8} {str(req.get('key')):<30} {fmt_resolve(req):<34} "
              f"{','.join(req.get('want') or []):<18} {status}")
        shown += 1
    if not shown:
        print("re: nothing to show")
    return 0


def cmd_req_check(root: Path, args) -> int:
    kb = re_dir(root)
    pend = [r for r in pending(kb) if (r.get("resolve") or {}).get("kind") == "sig"]
    if not pend:
        print("re: no pending signature requests")
        return 0
    bad = 0
    for req in pend:
        check = sig_check(root, args, req["resolve"]["value"])
        if not check.get("checked"):
            print(f"  {req.get('id')} {str(req.get('key')):<28} skipped: {check.get('reason')}")
            continue
        hits = check["hits"]
        old = (req.get("sig_check") or {})
        drift = (old.get("exe_mtime") and old.get("exe_mtime") != check["exe_mtime"]) or \
                (old.get("hits") is not None and old.get("hits") != hits)
        verdict = "ok" if hits == 1 else ("NOT FOUND" if hits == 0 else f"AMBIGUOUS({hits})")
        if hits != 1:
            bad += 1
        note = "  <- exe changed since enqueue" if drift else ""
        print(f"  {req.get('id')} {str(req.get('key')):<28} hits={hits} {verdict}"
              f" {check.get('file_off') or ''}{note}")
    if bad:
        print(f"re: {bad} signature(s) no longer resolve uniquely - likely a new build; "
              f"fix the patterns before the anchor session spends a pass")
        return 1
    return 0


def cmd_served_ls(root: Path, args) -> int:
    kb = re_dir(root)
    rows = served(kb)
    if not rows:
        print("re: nothing served yet")
        return 0
    if args.failed:
        rows = [r for r in rows if not r.get("ok")]
        if not rows:
            print("re: no failures")
            return 0
    print(f"{'req':<8} {'key':<30} {'build':<14} {'rva':<12} files")
    for r in rows:
        files = ", ".join(f.get("role", "?") for f in (r.get("files") or []))
        print(f"{str(r.get('req')):<8} {str(r.get('key')):<30} {(r.get('build') or '?'):<14} "
              f"{r.get('rva') or '-':<12} {files or (r.get('note') or '')}")
    return 0


def cmd_idb(root: Path, args) -> int:
    kb = re_dir(root)
    if args.action == "set":
        if not args.path:
            print("re: idb set needs --path")
            return 1
        path = Path(args.path)
        if not path.is_file():
            print(f"re: {path} does not exist")
            return 1
        write_json(kb / "idb.local.json", {"path": str(path), "set_at": now()})
        print(f"re: registered {path} in docs/agent/re/idb.local.json (git-ignored)")
        return 0
    live = idb_live(kb)
    if live is None:
        print("re: no idb registered")
        return 0
    print(json.dumps(live, ensure_ascii=False, indent=2))
    return 0


# -------------------------------------------------------------------- canon
#
# Phase 2.  Phase 1 (builds/<sha12>/decomp/) is raw material keyed by a name you
# already had to know.  Canon answers the other question - "which function is the
# one I am looking for" - and carries the naming, the cross-build alias and the
# dead ends, so the same reasoning is not done twice by two different agents.
#
# Drift strategy (lightweight): `sig` is the primary key, `anchors` the fallback.
# `canon verify` re-scans every sig against the current exe and says what broke;
# the anchors are what you search with when it does.

MAC_RE = re.compile(r"0x1[0-9a-fA-F]{8,10}")
NUM_RE = re.compile(r"^[0-9]+$|^0x[0-9a-fA-F]+$|^\d+(\.\d+)?\s*[KM]i?B$")
DOC_ROOTS = ("docs/research", "docs/specs", "docs/archive")
CONFIDENCE = ("observed", "confirmed", "inferred")
WIN_BASE_FALLBACK = 0x140000000


def pe_layout(path: Path) -> tuple[int, int] | None:
    """(ImageBase, SizeOfImage) straight out of the PE header - no idb, no tool."""
    try:
        with path.open("rb") as f:
            head = f.read(0x1000)
    except OSError:
        return None
    if len(head) < 0x40 or head[:2] != b"MZ":
        return None
    import struct

    pe = struct.unpack_from("<I", head, 0x3C)[0]
    if head[pe:pe + 4] != b"PE\0\0":
        return None
    magic = struct.unpack_from("<H", head, pe + 24)[0]
    if magic == 0x20B:  # PE32+: ImageBase at +24 (8B), SizeOfImage at +56 (4B)
        base = struct.unpack_from("<Q", head, pe + 24 + 24)[0]
        size = struct.unpack_from("<I", head, pe + 24 + 56)[0]
        return base, size
    if magic == 0x10B:  # PE32: ImageBase at +28 (4B), SizeOfImage at +56 (4B)
        base = struct.unpack_from("<I", head, pe + 24 + 28)[0]
        size = struct.unpack_from("<I", head, pe + 24 + 56)[0]
        return base, size
    return None


def pe_imagebase(path: Path) -> int | None:
    layout = pe_layout(path)
    return layout[0] if layout else None


def pe_sections(path: Path) -> list[dict]:
    """Section table, for rva -> file offset.  The idb is not needed to read bytes."""
    import struct

    try:
        with path.open("rb") as f:
            head = f.read(0x1000)
    except OSError:
        return []
    if len(head) < 0x40 or head[:2] != b"MZ":
        return []
    pe = struct.unpack_from("<I", head, 0x3C)[0]
    nsec = struct.unpack_from("<H", head, pe + 6)[0]
    opt_size = struct.unpack_from("<H", head, pe + 20)[0]
    off = pe + 24 + opt_size
    out = []
    for i in range(nsec):
        base = off + i * 40
        if base + 40 > len(head):
            break
        name = head[base:base + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", head, base + 8)
        out.append({"name": name, "va": va, "vsize": vsize,
                    "rawsize": rawsize, "rawptr": rawptr})
    return out


def rva_to_file_off(sections: list[dict], rva: int) -> tuple[dict | None, int | None]:
    for sec in sections:
        span = max(sec["vsize"], sec["rawsize"])
        if sec["va"] <= rva < sec["va"] + span:
            return sec, rva - sec["va"] + sec["rawptr"]
    return None, None


def addr_space(addr: str, imagebase: int, image_size: int = 0) -> str:
    """Which module an address belongs to.  Conflating them is how a 26.51 address
    ends up filed as a macOS one, an nvngx.dll address as a game function, and a
    heap pointer from a log as something that can be a key at all."""
    try:
        value = int(addr, 16)
    except (TypeError, ValueError):
        return "unknown"
    if 0x100000000 <= value < 0x120000000:
        return "mac-1.26.32"
    if imagebase <= value < imagebase + (image_size or 0x08000000):
        return "win-26.51"
    if 0x180000000 <= value < 0x500000000:
        return "other-module"
    if value >= 0x1000000000:
        return "runtime-pointer"
    return "unknown"


def canon_funcs(kb: Path) -> list[dict]:
    return load_jsonl(kb / "canon" / "funcs.jsonl")


def canon_refuted(kb: Path) -> list[dict]:
    return load_jsonl(kb / "canon" / "refuted.jsonl")


def canon_by_key(kb: Path) -> dict[str, dict]:
    """Last line wins, so a correction can be appended instead of edited in place."""
    out: dict[str, dict] = {}
    for entry in canon_funcs(kb):
        out[entry.get("key") or ""] = entry
    return out


def canon_active(kb: Path) -> dict[str, dict]:
    """Live entries only.  A renamed key leaves a tombstone line behind, and every
    reader has to agree that a tombstone is not a fact."""
    return {k: v for k, v in canon_by_key(kb).items() if v.get("status") != "superseded"}


def _clean(text: str) -> str:
    out = text.replace("**", "").replace("`", "").strip(" \t|")
    out = re.sub(r"^[-\s]+", "", out)
    return out.strip("()（） ").strip()


def _is_junk_label(label: str) -> bool:
    """An address, a size or a range is not a name - those rows only add noise."""
    s = (label or "").strip()
    if not s or s == "-":
        return True
    if NUM_RE.match(s) or MAC_RE.fullmatch(s):
        return True
    return bool(re.fullmatch(r"[0-9a-fA-FxX,\s\-–~]+", s))


def _snake(text: str) -> str:
    """`a::b::c` -> `a_b_c`, dropping leading namespaces until it fits (the tail is
    the identity; a key truncated mid-word is unusable in a search box)."""
    parts = [p for p in _clean(text).split("::") if p.strip()]
    parts = [re.sub(r"[^a-zA-Z0-9]+", "_", p).strip("_").lower() for p in parts]
    parts = [p for p in parts if p]
    if not parts:
        return "unnamed"
    while len(parts) > 1 and len("_".join(parts)) > 46:
        parts.pop(0)
    out = ("_".join(parts) or "unnamed")[:46]
    return f"at_{out}" if out[0].isdigit() else out


def _label_from_cells(cells: list[str], idx: int) -> str:
    for i in sorted((i for i in range(len(cells)) if i != idx), key=lambda i: (abs(i - idx), i)):
        cand = _clean(cells[i])
        if cand and not NUM_RE.match(cand) and not MAC_RE.fullmatch(cand):
            return cand
    return ""


def extract_addresses(text: str, doc: str) -> list[dict]:
    """Address rows out of a research doc.  Tables first, inline `Sym (0x..)` second."""
    out = []
    for line_no, line in enumerate(text.splitlines(), 1):
        for addr in MAC_RE.findall(line):
            label, desc = "", ""
            if "|" in line:
                cells = line.split("|")
                idx = next((i for i, c in enumerate(cells) if addr in c), None)
                if idx is not None:
                    cleaned = [_clean(c) for c in cells]
                    label = _label_from_cells(cells, idx)
                    tail = [c for i, c in enumerate(cleaned)
                            if i != idx and c and c != label and not NUM_RE.match(c)]
                    desc = tail[-1] if tail else ""
            if not label:
                head = _clean(line.split(addr)[0])
                head = head.rstrip("(（ ")
                label = head.rsplit("（", 1)[-1].rsplit("(", 1)[-1].strip()
            out.append({"addr": addr, "label": label, "desc": desc, "doc": doc, "line": line_no})
    return out


def cmd_canon_import(root: Path, args) -> int:
    kb = re_dir(root)
    existing = canon_by_key(kb)
    seen_addr = {e.get("mac_addr") for e in existing.values() if e.get("mac_addr")}
    found: dict[str, dict] = {}
    files = []
    for rel in DOC_ROOTS:
        base = root / rel
        if base.is_dir():
            files.extend(sorted(base.rglob("*.md")))
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = str(path.relative_to(root)).replace("\\", "/")
        for hit in extract_addresses(text, rel):
            if hit["addr"].lower() == "0x100000000":
                continue  # that is the imagebase, not a function
            if _is_junk_label(hit["label"]):
                continue
            if hit["addr"] in seen_addr:
                continue  # already canon (re-runs are idempotent)
            entry = found.setdefault(hit["addr"], {
                "addr": hit["addr"], "label": hit["label"], "desc": hit["desc"], "evidence": [],
            })
            if len(hit["label"]) > len(entry["label"]):
                entry["label"] = hit["label"]
            if not entry["desc"] and hit["desc"]:
                entry["desc"] = hit["desc"]
            entry["evidence"].append({"doc": hit["doc"], "line": hit["line"]})

    if not found:
        print(f"re: nothing new to import ({len(existing)} canon entries already)")
        return 0

    keys = set(existing)
    out_path = kb / "canon" / "funcs.jsonl"
    exe = exe_path(root, args)
    layout = pe_layout(exe) if exe else None
    imagebase, image_size = layout if layout else (WIN_BASE_FALLBACK, 0)
    count = 0
    skipped = 0
    by_space: dict[str, int] = {}
    for hit in sorted(found.values(), key=lambda h: h["addr"]):
        space = addr_space(hit["addr"], imagebase, image_size)
        value = int(hit["addr"], 16)
        if space in ("runtime-pointer", "unknown"):
            skipped += 1
            continue  # a heap pointer from a log cannot be a key: it moves every launch
        if space == "win-26.51" and value == imagebase:
            continue  # the imagebase row, not a function
        key = _snake(hit["label"] or f"addr_{hit['addr'][2:]}")
        if key in keys:
            key = f"{key}_{hit['addr'][-4:]}"
        keys.add(key)
        entry = {
            "kind": "re-func",
            "key": key,
            "label": hit["label"],
            "addr": hit["addr"],
            "addr_space": space,
            "rva": hex(value - imagebase) if space == "win-26.51" else None,
            "mac_addr": hit["addr"] if space == "mac-1.26.32" else None,
            "mac_alias": hit["label"] if space == "mac-1.26.32" and "::" in hit["label"] else None,
            "proposed_name": None,
            "proto": None,
            "sig": None,
            "anchors": [],
            "semantics": hit["desc"][:200] if hit["desc"] else "",
            "confidence": "observed",
            "status": {"win-26.51": "located-in-26.51",
                       "other-module": "located-in-other-module"}.get(space,
                                                                     "unresolved-in-26.51"),
            "build_scope": {"mac-1.26.32": "mac-1.26.32",
                            "win-26.51": "26.51.01"}.get(space, space),
            "origin": "imported-from-docs",
            "evidence": hit["evidence"][:6],
            "by": args.by,
        }
        append_jsonl(out_path, entry)
        count += 1
        by_space[space] = by_space.get(space, 0) + 1
    print(f"re: imported {count} address entries into docs/agent/re/canon/funcs.jsonl")
    detail = ", ".join(f"{n} {s}" for s, n in sorted(by_space.items()))
    print(f"    {len(files)} doc(s) scanned; {detail} (imagebase {hex(imagebase)})")
    if skipped:
        print(f"    skipped {skipped} runtime/heap address(es): they change every launch")
    print("    mac entries have no 26.51 signature yet; win entries carry a build-pinned rva")
    return 0


def cmd_canon_add(root: Path, args) -> int:
    kb = re_dir(root)
    if not KEY_RE.match(args.key):
        print(f"re: bad --key {args.key!r}: lowercase snake_case, 2-48 chars")
        return 1
    existing = canon_by_key(kb)
    base = existing.get(args.key)
    if base and not args.amend:
        print(f"re: canon already has {args.key!r}. A correction is "
              f"`canon add --key {args.key} --amend ...` (append a line, last wins); "
              f"a different function needs a different key")
        return 1
    base = base or {}
    for entry in existing.values():
        if args.sig and entry.get("sig") and _norm(entry["sig"]) == _norm(args.sig) \
                and entry.get("key") != args.key:
            print(f"re: this sig is already canon entry {entry['key']!r} - one function, one key")
            return 1

    sig_status = None
    if args.sig:
        check = sig_check(root, args, args.sig)
        if not check.get("checked"):
            print(f"re: cannot verify the sig offline ({check.get('reason')}); "
                  f"pass --exe or drop --sig and add it later")
            return 1
        if check["hits"] != 1:
            print(f"re: sig matches {check['hits']} places in {check['exe']} - "
                  f"a key that resolves to several functions is not a key")
            return 1
        sig_status = "resolved-in-26.51"

    evidence = []
    for item in args.evidence or []:
        doc, _, line = item.partition(":")
        evidence.append({"doc": doc, "line": int(line) if line.isdigit() else None})
    if not evidence:
        if not base:
            print("re: --evidence is required (docs/...:line or the decomp file you read)")
            return 1
        evidence = list(base.get("evidence") or [])

    exe = exe_path(root, args)
    layout = pe_layout(exe) if exe else None
    imagebase, image_size = layout if layout else (WIN_BASE_FALLBACK, 0)
    addr = args.addr if args.addr is not None else base.get("addr")
    space = addr_space(addr, imagebase, image_size) if addr else base.get("addr_space")
    if space == "runtime-pointer":
        print(f"re: {addr} is a runtime/heap address (it moves every launch) - "
              f"not a fact that can be keyed. Use the module RVA or a signature.")
        return 1

    def pick(value, field, default=None):
        if value is not None:
            return value
        return base.get(field, default)

    status = sig_status or base.get("status")
    if status is None or base.get("addr") != addr:
        if sig_status:
            status = sig_status
        elif space == "win-26.51":
            status = "located-in-26.51"
        elif space == "other-module":
            status = "located-in-other-module"
        else:
            status = "unresolved-in-26.51"

    label = pick(args.label, "label", args.key)
    entry = {
        "kind": "re-func",
        "key": args.key,
        "label": label,
        "addr": addr,
        "addr_space": space,
        "rva": (hex(int(addr, 16) - imagebase) if space == "win-26.51" and addr
                else base.get("rva")),
        "mac_addr": addr if space == "mac-1.26.32" else None,
        "mac_alias": pick(args.mac_alias, "mac_alias",
                          label if label and "::" in label and space == "mac-1.26.32" else None),
        "proposed_name": pick(args.proposed_name, "proposed_name"),
        "proto": pick(args.proto, "proto"),
        "sig": pick(args.sig, "sig"),
        "anchors": args.anchor if args.anchor else list(base.get("anchors") or []),
        "semantics": pick(args.semantics, "semantics", ""),
        "confidence": pick(args.confidence, "confidence", "inferred"),
        "status": status,
        "build_scope": {"mac-1.26.32": "mac-1.26.32", "win-26.51": "26.51.01"}
        .get(space or "", args.build_scope or base.get("build_scope")),
        "origin": base.get("origin") or "authored",
        "evidence": evidence,
        "by": args.by,
    }
    append_jsonl(kb / "canon" / "funcs.jsonl", entry)
    verb = "amended" if base else "added"
    print(f"re: canon {verb} {args.key} [{status}] confidence={entry['confidence']}")
    if args.sig:
        print("    sig resolves uniquely in the current exe")
    return 0


def cmd_sig(root: Path, args) -> int:
    """rva -> file offset -> shortest unique signature.  Pure offline; this is what
    turns a canon entry from `located-in-26.51` into `resolved-in-26.51`."""
    exe = exe_path(root, args)
    if exe is None or not exe.is_file():
        print("re: no exe (pass --exe or configure scripts/agentflow/config.local.json)")
        return 1
    layout = pe_layout(exe)
    if layout is None:
        print(f"re: {exe} is not a PE file")
        return 1
    imagebase, _ = layout
    if args.rva is not None:
        try:
            rva = int(args.rva, 16) if isinstance(args.rva, str) else int(args.rva)
        except (TypeError, ValueError):
            print(f"re: bad --rva {args.rva!r}")
            return 1
    elif args.addr is not None:
        rva = int(args.addr, 16) - imagebase
    else:
        print("re: pass --rva or --addr")
        return 1
    if rva < 0:
        print(f"re: rva {hex(rva)} is below the imagebase {hex(imagebase)}")
        return 1

    sections = pe_sections(exe)
    section, off = rva_to_file_off(sections, rva)
    if section is None:
        print(f"re: rva {hex(rva)} is not inside any section of {exe.name}")
        return 1
    with exe.open("rb") as f:
        f.seek(off)
        head = f.read(max(args.max_len, 32))
    if len(head) < 8:
        print(f"re: rva {hex(rva)} is too close to the end of {section['name']}")
        return 1

    print(f"exe       : {exe}")
    print(f"rva       : {hex(rva)}  addr {hex(imagebase + rva)}  "
          f"{section['name']} file_off {hex(off)}")
    candidates = sorted({args.min_len, 16, 24, 32, 40, 48, 56, 64, 80, args.max_len})
    candidates = [n for n in candidates if args.min_len <= n <= args.max_len and n <= len(head)]
    for length in candidates:
        pattern = " ".join(f"{b:02X}" for b in head[:length])
        hits = scan_matches(exe, pattern, limit=16)
        if len(hits) == 1:
            print(f"unique at : {length} bytes")
            print(f"sig       : {pattern}")
            print(f"canon     : --addr {hex(imagebase + rva)} --sig \"{pattern}\"")
            return 0
        shown = "16+" if len(hits) >= 16 else str(len(hits))
        print(f"  {length:>3} bytes -> {shown} hit(s)")
    print(f"re: still not unique at {args.max_len} bytes - this address is inside a "
          f"repeated block (a prologue shared by thousands of functions looks like this).")
    print("    Either take the address of the function entry instead, or record structural "
          "anchors (str:/import:/vtable:) rather than a signature.")
    return 1


def cmd_canon_rename(root: Path, args) -> int:
    """Key rename = new entry + tombstone.  Keys are identity; renaming must leave a
    trace or the old name reappears in someone's notes and splits the facts again."""
    kb = re_dir(root)
    if not KEY_RE.match(args.new):
        print(f"re: bad --new {args.new!r}: lowercase snake_case, 2-48 chars")
        return 1
    entries = canon_by_key(kb)
    old = entries.get(args.old)
    if old is None:
        print(f"re: canon has no {args.old!r}")
        return 1
    if old.get("status") == "superseded":
        print(f"re: {args.old!r} was already renamed to {old.get('superseded_by')!r}")
        return 1
    if args.new in entries:
        print(f"re: canon already has {args.new!r}; pick a free key")
        return 1

    new_entry = dict(old)
    new_entry["key"] = args.new
    new_entry["renamed_from"] = args.old
    new_entry["by"] = args.by
    append_jsonl(kb / "canon" / "funcs.jsonl", new_entry)

    tombstone = {k: old.get(k) for k in ("addr", "addr_space", "rva", "mac_addr", "label",
                                         "sig", "anchors")}
    tombstone.update({
        "kind": "re-func",
        "key": args.old,
        "status": "superseded",
        "superseded_by": args.new,
        "renamed_at": now(),
        "by": args.by,
    })
    append_jsonl(kb / "canon" / "funcs.jsonl", tombstone)
    moved = rename_requests(kb, args.old, args.new, args.by)
    print(f"re: canon renamed {args.old} -> {args.new}")
    print("    the old key keeps a tombstone line so `find <old key>` still answers")
    if moved:
        print(f"    pending request(s) repointed: {', '.join(moved)}")
    return 0


def rename_requests(kb: Path, old_key: str, new_key: str, by: str = "agent") -> list[str]:
    """Point pending requests at the new key.  Append-only: a new line for the same
    request id, which `requests()` resolves as last-wins."""
    done = outcome_by_req(kb)
    moved = []
    for req in requests(kb):
        if req.get("key") != old_key:
            continue
        rid = req.get("id") or ""
        if done.get(rid):
            continue  # already served: the artifact keeps the old name, canon is the index
        append_jsonl(kb / "requests" / (req.get("_src") or f"{by}.jsonl"),
                     {k: v for k, v in req.items() if not k.startswith("_")} |
                     {"key": new_key, "renamed_from": old_key, "by": by})
        moved.append(rid)
    return moved


def cmd_req_rename(root: Path, args) -> int:
    kb = re_dir(root)
    by_id = {r.get("id"): r for r in requests(kb)}
    req = by_id.get(args.id)
    if req is None:
        print(f"re: no request {args.id!r}")
        return 1
    if outcome_by_req(kb).get(args.id):
        print(f"re: {args.id} is already settled; rename the canon key instead "
              f"(`canon rename`), the artifact keeps its file name")
        return 1
    if not KEY_RE.match(args.key):
        print(f"re: bad --key {args.key!r}: lowercase snake_case, 2-48 chars")
        return 1
    if req.get("key") == args.key:
        print(f"re: {args.id} already asks for {args.key}")
        return 0
    append_jsonl(kb / "requests" / (req.get("_src") or f"{args.by}.jsonl"),
                 {k: v for k, v in req.items() if not k.startswith("_")} |
                 {"key": args.key, "renamed_from": req.get("key"), "by": args.by})
    print(f"re: {args.id} now targets {args.key} (was {req.get('key')}); "
          f"one more line in the same file, nothing edited")
    return 0


def cmd_canon_ls(root: Path, args) -> int:
    kb = re_dir(root)
    everything = canon_by_key(kb)
    rows = list((everything if args.all else canon_active(kb)).values())
    if not everything:
        print("re: canon is empty (run `re_kb.py canon import` for the draft)")
        return 0
    dead = [e for e in everything.values() if e.get("status") == "superseded"]
    resolved = [e for e in canon_active(kb).values() if e.get("status") == "resolved-in-26.51"]
    located = [e for e in canon_active(kb).values() if e.get("status") == "located-in-26.51"]
    live = len(canon_active(kb))
    print(f"canon     : {live} live function(s) - {len(resolved)} resolved by sig, "
          f"{len(located)} located by rva (build-pinned), "
          f"{live - len(resolved) - len(located)} name-only; "
          f"{len(dead)} renamed; {len(canon_refuted(kb))} refuted")
    if args.unresolved:
        rows = [e for e in rows if e.get("status") not in ("resolved-in-26.51",)]
    print(f"{'key':<30} {'status':<22} {'addr':<14} {'rva':<12} label")
    for e in rows:
        print(f"{str(e.get('key')):<30} {str(e.get('status')):<22} "
              f"{(e.get('addr') or '-'):<14} {(e.get('rva') or '-'):<12} {e.get('label') or ''}")
    return 0


def cmd_canon_verify(root: Path, args) -> int:
    kb = re_dir(root)
    rows = [e for e in canon_active(kb).values() if e.get("sig")]
    if not rows:
        print("re: no canon entry carries a 26.51 signature yet")
        return 0
    bad = 0
    for entry in rows:
        check = sig_check(root, args, entry["sig"])
        if not check.get("checked"):
            print(f"  {entry['key']:<30} skipped: {check.get('reason')}")
            continue
        hits = check["hits"]
        verdict = "ok" if hits == 1 else ("NOT FOUND" if hits == 0 else f"AMBIGUOUS({hits})")
        if hits != 1:
            bad += 1
        anchors = " ".join(entry.get("anchors") or []) or "-"
        print(f"  {entry['key']:<30} hits={hits} {verdict:<12} "
              f"{check.get('file_off') or ''}  anchors: {anchors[:60]}")
    if bad:
        print(f"re: {bad} canon signature(s) broke - a new build or a bad pattern. "
              f"Re-search with the anchors above, then append a corrected line (same key).")
        return 1
    print(f"re: all {len(rows)} canon signature(s) still resolve uniquely")
    return 0


def cmd_canon_refute(root: Path, args) -> int:
    kb = re_dir(root)
    rec = {
        "kind": "re-refuted",
        "key": args.key,
        "claim": args.claim,
        "why": args.why,
        "evidence": args.evidence or [],
        "exp": args.exp,
        "by": args.by,
    }
    append_jsonl(kb / "canon" / "refuted.jsonl", rec)
    print(f"re: recorded a dead end: {args.claim}")
    print("    (this is the most expensive knowledge in an unsymbolised binary - "
          "it stops the next agent from walking it again)")
    return 0


def cmd_find(root: Path, args) -> int:
    """The anti-redundancy gate: stay here before opening anything."""
    kb = re_dir(root)
    needle = args.query.strip().lower()
    sig_like = bool(re.fullmatch(r"[0-9a-fA-F?]{2}([ ?][0-9a-fA-F?]{2})*", args.query.strip()))
    norm_q = _norm(args.query) if sig_like else ""

    canon_all = []
    for entry in canon_by_key(kb).values():
        fields = ("key", "label", "mac_alias", "proposed_name", "semantics", "sig",
                  "status", "addr", "addr_space", "rva", "mac_addr")
        hay = " ".join(str(entry.get(f) or "") for f in fields).lower()
        hay += " " + " ".join(entry.get("anchors") or []).lower()
        sig_hit = bool(sig_like and entry.get("sig") and norm_q in _norm(entry["sig"]))
        if needle in hay or sig_hit:
            canon_all.append(entry)
    superseded = [e for e in canon_all if e.get("status") == "superseded"]
    canon_hits = [e for e in canon_all if e.get("status") != "superseded"]

    refuted_hits = [r for r in canon_refuted(kb)
                    if needle in " ".join(str(r.get(f) or "") for f in ("key", "claim", "why")).lower()]
    req_hits = [r for r in requests(kb)
                if needle in " ".join(str(r.get(f) or "") for f in ("key", "why")).lower()
                or needle in str((r.get("resolve") or {}).get("value") or "").lower()]
    served_hits = [s for s in served(kb) if s.get("ok")
                   and needle in " ".join(str(s.get(f) or "") for f in ("key", "func")).lower()]
    decomp_hits = sorted((kb / "builds").glob(f"*/decomp/*{args.query.strip()}*")) \
        if (kb / "builds").is_dir() else []

    doc_hits = []
    for rel in DOC_ROOTS:
        base = root / rel
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*.md")):
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for line_no, line in enumerate(text.splitlines(), 1):
                if needle in line.lower():
                    doc_hits.append((str(path.relative_to(root)).replace("\\", "/"),
                                     line_no, line.strip()[:110]))
                    if len(doc_hits) >= args.limit:
                        break
            if len(doc_hits) >= args.limit:
                break

    if args.query:
        print(f"query     : {args.query}")
    print(f"canon     : {len(canon_hits)} hit(s)")
    for e in canon_hits[:args.limit]:
        print(f"  {str(e.get('key')):<28} [{e.get('status')}] {e.get('addr_space') or '?'} "
              f"{e.get('addr') or '-'} {('rva ' + e['rva']) if e.get('rva') else ''}  "
              f"{e.get('label') or ''}")
        if e.get("semantics"):
            print(f"      {e['semantics'][:100]}")
        for ev in (e.get("evidence") or [])[:2]:
            print(f"      evidence: {ev.get('doc')}:{ev.get('line') or '?'}")
    for e in superseded[:3]:
        print(f"  renamed: {e.get('key')} -> {e.get('superseded_by')} (use the new key)")
    print(f"refuted   : {len(refuted_hits)} hit(s)")
    for r in refuted_hits[:min(3, args.limit)]:
        print(f"  {r.get('claim')}  <- {r.get('why')}")
    print(f"requests  : {len(req_hits)}  /  served: {len(served_hits)}")
    for r in req_hits[:3]:
        print(f"  {r.get('id')} {r.get('key')} (pending or done - check `req ls`)")
    if decomp_hits:
        print("decomp    : " + ", ".join(str(p.relative_to(kb)) for p in decomp_hits[:5]))
    else:
        print("decomp    : none (nobody has served a snapshot for this yet)")
    if doc_hits:
        print(f"docs      : {len(doc_hits)} line(s)")
        for doc, line_no, text in doc_hits[:4]:
            print(f"  {doc}:{line_no}  {text}")
    else:
        print("docs      : no line mentions it")

    if served_hits:
        print("next      : already served - read the snapshot, do not re-analyse")
    elif req_hits:
        print("next      : already queued - wait for the anchor session (Alt+F7), do not re-request")
    elif canon_hits:
        located = [e for e in canon_hits if e.get("status") == "located-in-26.51"]
        if located:
            e = located[0]
            print(f"next      : located in 26.51 at rva {e.get('rva')} (build-pinned, no sig) -> "
                  f"`req add --key {e.get('key')} --rva {e.get('rva')}` for a snapshot, "
                  f"or derive a sig from the prologue anchor")
        else:
            e = canon_hits[0]
            print("next      : canon knows the name but 26.51 has no signature yet -> "
                  f"`req add --key {e.get('key')} --sig ...` "
                  f"(anchors: {' '.join(e.get('anchors') or []) or '-'})")
    elif superseded:
        print(f"next      : this key was renamed to {superseded[0].get('superseded_by')!r} - "
              f"use the new key")
    else:
        print("next      : nothing known. Read the docs above, then either "
              "`canon add` (if you can name it) or `req add` (if you need the decompile)")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="re_kb.py", description="ND-RE knowledge queue (offline half)")
    p.add_argument("--exe", help="override the module path (default: instance/Minecraft.Windows.exe)")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="KB / builds / requests / idb occupancy")

    req = sub.add_parser("req", help="decompile request queue")
    rsub = req.add_subparsers(dest="action", required=True)
    add = rsub.add_parser("add", help="enqueue one function")
    add.add_argument("--key", required=True, help="snake_case id; becomes decomp/<key>.c")
    add.add_argument("--sig", help='byte pattern, e.g. "48 89 5C 24 ?? ..."')
    add.add_argument("--rva", help="image-relative address, e.g. 0x412B40")
    add.add_argument("--addr", help="absolute effective address in the current idb")
    add.add_argument("--name", help="symbol name in the current idb")
    add.add_argument("--want", default=DEFAULT_WANT, help=f"comma list from: {', '.join(ROLES)}")
    add.add_argument("--why", required=True, help="what question this function answers")
    add.add_argument("--by", default="agent", help="who is asking (becomes requests/<by>.jsonl)")
    add.add_argument("--force", action="store_true", help="serve again even if already served")
    add.add_argument("--no-check", action="store_true", help="skip the offline signature check")
    add.add_argument("--allow-ambiguous", action="store_true",
                     help="accept a signature that matches 0 or >1 places")
    ls = rsub.add_parser("ls", help="pending vs served")
    ls.add_argument("--pending", dest="pending_only", action="store_true")
    chk = rsub.add_parser("check", help="re-verify pending signatures offline")
    chk.add_argument("--allow-ambiguous", action="store_true")
    rren = rsub.add_parser("rename", help="repoint a pending request at a new key")
    rren.add_argument("--id", required=True)
    rren.add_argument("--key", required=True)
    rren.add_argument("--by", default="agent")

    srv = sub.add_parser("served", help="what the anchor session produced")
    ssub = srv.add_subparsers(dest="action", required=True)
    sls = ssub.add_parser("ls")
    sls.add_argument("--failed", action="store_true", help="only failures")

    idb = sub.add_parser("idb", help="register / inspect the .i64 (never opens it)")
    isub = idb.add_subparsers(dest="action", required=True)
    iset = isub.add_parser("set")
    iset.add_argument("--path", required=True)
    isub.add_parser("status")

    find = sub.add_parser("find", help="search canon / snapshots / docs before analysing anything")
    find.add_argument("query")
    find.add_argument("--limit", type=int, default=8)

    sig = sub.add_parser("sig", help="rva -> shortest unique byte signature (offline)")
    sig.add_argument("--rva", help="image-relative address, e.g. 0xb74f860")
    sig.add_argument("--addr", help="absolute address, e.g. 0x14b74f860")
    sig.add_argument("--min-len", dest="min_len", type=int, default=16)
    sig.add_argument("--max-len", dest="max_len", type=int, default=96)

    canon = sub.add_parser("canon", help="phase 2: named functions, anchors, dead ends")
    csub = canon.add_subparsers(dest="action", required=True)

    cimp = csub.add_parser("import", help="draft entries from the addresses already in docs/")
    cimp.add_argument("--by", default="agent")

    cad = csub.add_parser("add", help="add one function card")
    cad.add_argument("--key", required=True)
    cad.add_argument("--label", help="the name as written in the source doc or IDB")
    cad.add_argument("--sig", help="26.51 byte pattern (offline-verified unique)")
    cad.add_argument("--addr", help="address in either module (space is auto-detected "
                                    "from the PE imagebase)")
    cad.add_argument("--mac-alias", dest="mac_alias", help="symbol in the macOS IDB")
    cad.add_argument("--proposed-name", dest="proposed_name", help="name we want in the Windows idb")
    cad.add_argument("--proto", help="one-line prototype")
    cad.add_argument("--anchor", action="append", help="structural anchor, e.g. str:<literal> "
                                                       "or import:<dll!sym> or vtable:<slot>")
    cad.add_argument("--semantics", help="one sentence: what this function is for")
    cad.add_argument("--confidence", choices=CONFIDENCE,
                     help="observed (seen in a doc/decompile) / confirmed (own evidence) / "
                          "inferred (guess); default inferred, kept on --amend")
    cad.add_argument("--build-scope", dest="build_scope",
                     help="build this address belongs to; derived from the address by default")
    cad.add_argument("--amend", action="store_true",
                     help="append a corrected line for an existing key (last line wins)")
    cad.add_argument("--evidence", action="append", help="docs/...:line or decomp file; repeatable")
    cad.add_argument("--by", default="agent")

    cls = csub.add_parser("ls")
    cls.add_argument("--unresolved", action="store_true", help="only entries without a 26.51 sig")
    cls.add_argument("--all", action="store_true", help="include renamed (superseded) keys")

    cren = csub.add_parser("rename", help="rename a key (old key keeps a tombstone line)")
    cren.add_argument("--old", required=True)
    cren.add_argument("--new", required=True)
    cren.add_argument("--by", default="agent")

    cver = csub.add_parser("verify", help="re-scan every canon sig against the current exe")
    cver.add_argument("--allow-ambiguous", action="store_true")

    cref = csub.add_parser("refute", help="record a dead end so nobody walks it again")
    cref.add_argument("--key", required=True)
    cref.add_argument("--claim", required=True, help="the hypothesis that is now dead")
    cref.add_argument("--why", required=True, help="what proved it dead")
    cref.add_argument("--evidence", action="append")
    cref.add_argument("--exp", help="the EXP-xxxx this came from, if any")
    cref.add_argument("--by", default="agent")
    return p


def main(argv: list[str] | None = None) -> int:
    # git speaks UTF-8, this console is cp936 (same reason as flow.py:1687).
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    args = build_parser().parse_args(argv)
    root = repo_root()
    if not (root / "docs" / "agent").is_dir():
        print(f"re: {root} does not look like an agentflow checkout (docs/agent missing)")
        return 1
    if not re_dir(root).is_dir():
        print(f"re: {re_dir(root)} missing - it should be committed; "
              f"recreate it rather than pointing elsewhere")
        return 1
    table = {
        ("status", None): cmd_status,
        ("req", "add"): cmd_req_add,
        ("req", "ls"): cmd_req_ls,
        ("req", "check"): cmd_req_check,
        ("req", "rename"): cmd_req_rename,
        ("served", "ls"): cmd_served_ls,
        ("idb", "set"): cmd_idb,
        ("idb", "status"): cmd_idb,
        ("find", None): cmd_find,
        ("sig", None): cmd_sig,
        ("canon", "import"): cmd_canon_import,
        ("canon", "add"): cmd_canon_add,
        ("canon", "ls"): cmd_canon_ls,
        ("canon", "verify"): cmd_canon_verify,
        ("canon", "refute"): cmd_canon_refute,
        ("canon", "rename"): cmd_canon_rename,
    }
    fn = table.get((args.cmd, getattr(args, "action", None)))
    if fn is None:
        print(f"re: unsupported command {args.cmd} {getattr(args, 'action', '')}")
        return 1
    return fn(root, args)


if __name__ == "__main__":
    sys.exit(main())
