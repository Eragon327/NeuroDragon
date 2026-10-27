#!/usr/bin/env python3
"""ND-RE batch server - runs INSIDE the one anchor IDA session.

    File > Script file...   (Alt+F7)   ->   scripts/agentflow/re_serve.py

Reads ``docs/agent/re/requests/*.jsonl``, resolves each request to a function,
dumps text snapshots into ``docs/agent/re/builds/<sha12>/decomp/`` and appends
the outcome to ``docs/agent/re/served.jsonl``.

Why a script inside IDA instead of agents opening the idb: the 296 MiB client
becomes a 3.67 GiB ``.i64`` plus ~3.8 GiB of live ``.id0``/``.id1`` while open,
and IDA opens databases read-write - a second opener is a lock fight or a full
copy.  One session serving a queue is the only shape that scales.

This script is a *read*: it never saves the database, never renames, never
retypes, never patches.  What it leaves behind is text, so the next agent does
not need the idb at all.

Batch size is capped because this runs on IDA's main thread (the GUI is frozen
while it works).  Requests are resumable: status lives in served.jsonl, so a
second Alt+F7 continues where the first stopped.
"""

from __future__ import annotations

import datetime
import hashlib
import json
import os
import sys
from pathlib import Path

MAX_PER_RUN = 40
MAX_BYTES = 400_000
MAX_SIG_HITS = 8
SERVE_VERSION = "2"


# ------------------------------------------------------------------ utilities

def _log(text: str) -> None:
    """IDA's Output window when inside IDA, stdout otherwise."""
    try:
        import ida_kernwin

        ida_kernwin.msg(text + "\n")
    except Exception:
        print(text)


def now() -> str:
    return datetime.datetime.now().astimezone().isoformat(timespec="milliseconds")


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
        if line:
            try:
                out.append(json.loads(line))
            except ValueError:
                _log(f"[re_serve] malformed line in {path.name}: {line[:70]}")
    return out


def append_jsonl(path: Path, event: dict) -> dict:
    path.parent.mkdir(parents=True, exist_ok=True)
    event = {"ts": now(), **event}
    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(event, ensure_ascii=False) + "\n")
    return event


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_text(path: Path, text: str) -> dict:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = text.encode("utf-8")
    truncated = len(data) > MAX_BYTES
    if truncated:
        data = data[:MAX_BYTES] + b"\n/* truncated at 400 KB by re_serve.py */\n"
    path.write_bytes(data)
    return {
        "path": str(path),
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "truncated": truncated,
    }


def _first_literal(pattern: str) -> tuple[int, bytes] | None:
    """Longest contiguous literal run of a `48 89 ?? ..` pattern, as (offset, bytes).

    Used to prove that a file offset really is the same instruction in the open
    database - a file that changed after the .i64 was built would otherwise send
    every hit to the wrong function, silently."""
    best: tuple[int, bytes] | None = None
    run = bytearray()
    start = None
    for i, tok in enumerate(pattern.split()):
        if tok in ("?", "??", "**"):
            if start is not None and (best is None or len(run) > len(best[1])):
                best = (start, bytes(run))
            run, start = bytearray(), None
            continue
        try:
            value = int(tok, 16)
        except ValueError:
            return None
        if start is None:
            start = i
        run.append(value)
    if start is not None and (best is None or len(run) > len(best[1])):
        best = (start, bytes(run))
    if best is None or len(best[1]) < 4:
        return None
    return best


# ---------------------------------------------------------------- locations

def find_re_dir() -> Path | None:
    """docs/agent/re, from the script location (IDA runs us with __file__ set)."""
    env = os.environ.get("ND_RE_DIR")
    if env and Path(env).is_dir():
        return Path(env)
    here = Path(__file__).resolve()
    for base in list(here.parents)[:4]:
        cand = base / "docs" / "agent" / "re"
        if cand.is_dir():
            return cand
    cwd = Path.cwd()
    for base in [cwd, *cwd.parents][:6]:
        cand = base / "docs" / "agent" / "re"
        if cand.is_dir():
            return cand
    return None


def configured_exe(re_dir: Path) -> Path | None:
    for name in ("config.local.json", "config.json"):
        raw = read_json(re_dir.parent.parent.parent / "scripts" / "agentflow" / name, {}).get("instance")
        if raw:
            cand = Path(raw) / "Minecraft.Windows.exe"
            if cand.is_file():
                return cand
    return None


def build_identity(re_dir: Path, exe: Path, recorded_sha: bytes | None,
                   recorded_size: int) -> dict:
    """Identify the module this database was built from.

    ``ida_nalt.retrieve_input_file_sha256()`` is the authoritative answer and keeps
    working when the input file is not on this machine - the macOS .i64 was imported
    elsewhere, so its Mach-O path does not exist here.  Hashing the file is only the
    fallback, and 296 MiB is not free.
    """
    index_path = re_dir / "builds" / "index.json"
    index = read_json(index_path, {})
    sha = recorded_sha.hex() if recorded_sha else ""
    st = exe.stat() if exe.is_file() else None
    key = f"{exe}|{sha or 'nohash'}|{recorded_size or (st.st_size if st else 0)}"
    if key in index:
        return index[key]
    if not sha:
        if st is None:
            raise RuntimeError(f"cannot identify {exe}: not on disk and the database "
                               f"recorded no input hash")
        _log(f"[re_serve] hashing {exe.name} ({st.st_size / 1048576:.0f} MiB) once "
             f"(the database recorded no input hash)")
        sha = sha256_file(exe)
    rec = {
        "sha256": sha,
        "sha12": sha[:12],
        "exe": str(exe),
        "size": st.st_size if st else recorded_size,
        "recorded_size": recorded_size or None,
        "input_on_disk": st is not None,
        "mtime": int(st.st_mtime) if st else None,
        "version": exe.parent.name,
        "id_source": "db-recorded" if recorded_sha else "file",
        "indexed_at": now(),
    }
    index[key] = rec
    write_json(index_path, index)
    return rec


def build_paths(re_dir: Path, build: dict) -> tuple[Path, Path]:
    base = re_dir / "builds" / build["sha12"]
    if not (base / "meta.json").is_file():
        write_json(base / "meta.json", {
            "build": build["sha12"],
            "sha256": build["sha256"],
            "exe": build["exe"],
            "version": build["version"],
            "id_source": build.get("id_source"),
            "input_on_disk": build.get("input_on_disk"),
            "created": now(),
            "note": "decomp/ is text produced by re_serve.py from the .i64; "
                    "the .i64 itself is a derived artifact and is never committed",
        })
    return base, base / "decomp"


# --------------------------------------------------------------- IDA access

class Ida:
    """Everything that touches IDA, in one place, so the rest stays testable."""

    def __init__(self) -> None:
        import ida_bytes
        import ida_funcs
        import ida_idaapi
        import ida_lines
        import ida_loader
        import ida_name
        import ida_nalt
        import ida_typeinf
        import ida_ua
        import idautils
        import idc

        self.ida_bytes = ida_bytes
        self.ida_funcs = ida_funcs
        self.ida_lines = ida_lines
        self.ida_loader = ida_loader
        self.ida_name = ida_name
        self.ida_nalt = ida_nalt
        self.ida_typeinf = ida_typeinf
        self.ida_ua = ida_ua
        self.idautils = idautils
        self.idc = idc  # 9.3 still ships the shim; only names it still exposes are used
        self.BADADDR = ida_idaapi.BADADDR
        self.kb = self._kb_module()
        self.hexrays = None
        try:
            import ida_hexrays

            if not ida_hexrays.init_hexrays_plugin():
                _log("[re_serve] init_hexrays_plugin() returned false; pseudo may fail")
            self.hexrays = ida_hexrays
        except Exception as exc:  # no licence, no decompiler, older IDA
            _log(f"[re_serve] Hex-Rays unavailable ({exc}); pseudo will fall back to disasm")

    @staticmethod
    def _kb_module():
        """re_kb.py is the tested offline half (PE tables + byte scanning).  Reuse it
        instead of reimplementing a search API that IDA keeps re-shaping: 9.3 dropped
        idc.find_binary and moved find_bytes' pattern to the first argument.

        A failure here is not fatal - find_sig falls back to the IDA search API."""
        here = Path(__file__).resolve().parent
        try:
            if str(here) not in sys.path:
                sys.path.insert(0, str(here))
            import re_kb

            return re_kb
        except Exception as exc:
            _log(f"[re_serve] cannot import re_kb ({exc}); signature search falls back "
                 f"to the IDA API and the file/db cross-check is off")
            return None

    # -- identity ---------------------------------------------------------
    def input_path(self) -> Path | None:
        raw = self.ida_nalt.get_input_file_path()
        return Path(raw) if raw else None

    def imagebase(self) -> int:
        try:
            return int(self.ida_nalt.get_imagebase())
        except Exception:
            return 0

    def idb_path(self) -> str | None:
        """The real .i64 path.  Deriving it as `input_path + ".i64"` is wrong: the
        database can live anywhere, and the input path is wherever it was imported from."""
        try:
            return self.ida_loader.get_path(self.ida_loader.PATH_TYPE_IDB) or None
        except Exception:
            return None

    def input_sha256(self) -> bytes | None:
        try:
            raw = self.ida_nalt.retrieve_input_file_sha256()
            return raw if raw and len(raw) == 32 else None
        except Exception:
            return None

    def input_size(self) -> int:
        try:
            return int(self.ida_nalt.retrieve_input_file_size())
        except Exception:
            return 0

    def rva(self, ea: int) -> str:
        return hex(ea - self.imagebase())

    def file_off(self, ea: int) -> str | None:
        """Prefer IDA's own mapping; fall back to the section table we already parse."""
        try:
            off = self.ida_loader.get_fileregion_offset(ea)
            if off not in (None, -1):
                return hex(off)
        except Exception:
            pass
        path = self.input_path()
        if path is None or self.kb is None:
            return None
        rva = ea - self.imagebase()
        for sec in self.kb.pe_sections(path):
            span = max(sec["vsize"], sec["rawsize"])
            if sec["va"] <= rva < sec["va"] + span:
                return hex(rva - sec["va"] + sec["rawptr"])
        return None

    def func_of(self, ea: int):
        try:
            return self.ida_funcs.get_func(ea)
        except Exception:
            return None

    def name_of(self, ea: int) -> str:
        for get in (lambda: self.ida_funcs.get_func_name(ea),
                    lambda: self.ida_name.get_name(ea),
                    lambda: self.idc.get_name(ea)):
            try:
                name = get()
            except Exception:
                continue
            if name:
                return name
        return "?"

    # -- resolution -------------------------------------------------------
    def _file_sig_hits(self, pattern: str) -> list[int] | None:
        """Scan the module on disk and translate file offsets to EAs.

        The bytes on disk do not move between IDA releases; the search API does.
        Returns None when there is no usable file, so the caller can fall back.
        """
        path = self.input_path()
        if path is None or not path.is_file() or self.kb is None:
            return None
        try:
            offsets = self.kb.scan_matches(path, pattern, limit=MAX_SIG_HITS)
            sections = self.kb.pe_sections(path)
        except Exception as exc:
            _log(f"[re_serve] file scan failed ({exc}); falling back to the IDA search API")
            return None
        if not sections:
            return None
        base = self.imagebase()
        hits = []
        for off in offsets:
            for sec in sections:
                if sec["rawptr"] <= off < sec["rawptr"] + sec["rawsize"]:
                    hits.append(base + sec["va"] + (off - sec["rawptr"]))
                    break
        return hits

    def _matches_db(self, ea: int, pattern: str) -> bool:
        """A file that changed after the .i64 was built would send every hit to a wrong
        function.  Read the first literal run back through the database to prove it."""
        run = _first_literal(pattern)
        if run is None:
            return True
        offset, want = run
        try:
            got = self.ida_bytes.get_bytes(ea + offset, len(want))
        except Exception:
            return False
        return bytes(got or b"") == want

    def _api_find_next(self, start: int, end: int, pattern: str) -> int | None:
        attempts = [
            # 9.x: find_bytes(bs, range_start, range_size=None, range_end=BADADDR, ...)
            lambda: self.ida_bytes.find_bytes(pattern, start, range_end=end),
            # 8.x and earlier kept the ea-first order
            lambda: self.ida_bytes.find_bytes(start, end, pattern),
        ]
        legacy = getattr(self.idc, "find_binary", None)  # removed in 9.x
        if legacy is not None:
            attempts.append(
                lambda: legacy(start, getattr(self.idc, "SEARCH_DOWN", 1), pattern))
        for attempt in attempts:
            try:
                found = attempt()
            except Exception:
                continue
            if isinstance(found, tuple):
                found = found[0] if found else None
            if found in (None, -1, self.BADADDR):
                return None
            return int(found)
        return None

    def find_sig(self, pattern: str) -> tuple[list[int], str]:
        raw = self._file_sig_hits(pattern)
        if raw is not None:
            if not raw:
                return [], "no match in the module on disk"
            kept = [ea for ea in raw if self._matches_db(ea, pattern)]
            if not kept:
                return [], ("the module on disk no longer matches this .i64 at that pattern "
                            "- the database was built from a different file")
            return kept, "file scan"
        lo = self.ida_ida_min()
        hi = self.ida_ida_max()
        hits: list[int] = []
        ea = lo
        while ea < hi and len(hits) < MAX_SIG_HITS:
            found = self._api_find_next(ea, hi, pattern)
            if found is None:
                break
            hits.append(found)
            ea = found + 1
        return hits, "IDA search API"

    def ida_ida_min(self) -> int:
        import ida_ida

        return int(ida_ida.inf_get_min_ea())

    def ida_ida_max(self) -> int:
        import ida_ida

        return int(ida_ida.inf_get_max_ea())

    def resolve(self, spec: dict) -> tuple[int | None, str, dict]:
        kind = spec.get("kind")
        value = spec.get("value") or ""
        if kind == "sig":
            hits, note = self.find_sig(value)
            if not hits:
                return None, note, {"hits": 0}
            if len(hits) > 1:
                return None, (f"ambiguous: {len(hits)}+ hits at "
                              f"{', '.join(self.rva(h) for h in hits[:4])}; extend the pattern"),
                {"hits": len(hits)}
            return hits[0], note, {"hits": 1}
        if kind in ("rva", "addr"):
            try:
                raw = int(value, 16) if isinstance(value, str) else int(value)
            except (TypeError, ValueError):
                return None, f"unparsable {kind} {value!r}", {}
            ea = raw + self.imagebase() if kind == "rva" else raw
            return ea, kind, {}
        if kind == "name":
            ea = self.BADADDR
            try:
                ea = self.ida_name.get_name_ea(self.BADADDR, value)
            except Exception:
                pass
            if ea in (None, self.BADADDR):
                try:
                    ea = self.idc.get_name_ea_simple(value)
                except Exception:
                    ea = self.BADADDR
            if ea in (None, self.BADADDR):
                return None, f"name {value!r} not found in this database", {}
            return int(ea), kind, {}
        return None, f"unknown resolve kind {kind!r}", {}

    # -- collection -------------------------------------------------------
    def proto(self, ea: int) -> str:
        try:
            text = self.ida_typeinf.print_type(ea, self.ida_typeinf.PRTYPE_1LINE)
            if text:
                return " ".join(text.split())
        except Exception:
            pass
        try:
            info = self.ida_typeinf.tinfo_t()
            if self.ida_nalt.get_tinfo(info, ea):
                return " ".join(str(info).split())
        except Exception:
            pass
        return "?"

    def pseudo(self, ea: int) -> tuple[str | None, str]:
        if self.hexrays is None:
            return None, "no Hex-Rays"
        try:
            cfunc = self.hexrays.decompile(ea)
        except Exception as exc:
            return None, f"decompile raised: {exc}"
        if cfunc is None:
            return None, "decompile returned nothing (not a function?)"
        try:
            return str(cfunc), "ok"
        except Exception as exc:
            return None, f"cfunc str() failed: {exc}"

    def xrefs_to(self, ea: int) -> str:
        lines = []
        try:
            for xref in self.idautils.XrefsTo(ea, 0):
                func = self.func_of(xref.frm)
                fname = self.name_of(func.start_ea) if func else "-"
                kind = "code" if xref.iscode else "data"
                lines.append(f"{hex(xref.frm):>18}  rva {self.rva(xref.frm):<10} {kind:<4} {xref.type:>3}  {fname}")
        except Exception as exc:
            lines.append(f"(xrefs failed: {exc})")
        return "\n".join(lines) if lines else "(no xrefs) _"

    def callees(self, func) -> str:
        seen: dict[int, int] = {}
        try:
            for item in self.idautils.FuncItems(func.start_ea):
                if self.ida_ua.print_insn_mnem(item) not in ("call", "jmp", "callq"):
                    continue
                for target in self.idautils.CodeRefsFrom(item, 0):
                    if target in seen:
                        continue
                    seen[target] = item
        except Exception as exc:
            return f"(callees failed: {exc})"
        rows = [f"{hex(t):>18}  rva {self.rva(t):<10} {self.name_of(t)}" for t in sorted(seen)]
        return "\n".join(rows) if rows else "(no direct calls) _"

    def strings(self, func) -> str:
        seen: set[str] = set()
        try:
            for item in self.idautils.FuncItems(func.start_ea):
                try:
                    drefs = list(self.idautils.DataRefsFrom(item))
                except Exception:
                    continue
                for dref in drefs:
                    raw = None
                    try:
                        raw = self.ida_bytes.get_strlit_contents(
                            dref, -1, getattr(self.ida_nalt, "STRTYPE_C", 1))
                    except Exception:
                        try:
                            raw = self.idc.get_strlit_contents(dref)
                        except Exception:
                            raw = None
                    if not raw:
                        continue
                    text = raw.decode("utf-8", "replace") if isinstance(raw, bytes) else str(raw)
                    if 2 <= len(text) <= 200:
                        seen.add(f"{self.rva(dref):<10} {text!r}")
        except Exception as exc:
            return f"(strings failed: {exc})"
        return "\n".join(sorted(seen)) if seen else "(no string literals) _"

    def disasm(self, func) -> str:
        lines = []
        try:
            for item in self.idautils.FuncItems(func.start_ea):
                try:
                    line = self.ida_lines.generate_disasm_line(item, 0) or ""
                except Exception:
                    line = "?"
                lines.append(f"{self.rva(item):<10}  {line}")
        except Exception as exc:
            return f"(disasm failed: {exc})"
        return "\n".join(lines) if lines else "(no instructions) _"


# ---------------------------------------------------------------- rendering

def _header(req: dict, res: dict, style: str) -> str:
    rows = [
        ("key", req.get("key")),
        ("build", f"{res['build']} ({res.get('version') or '?'})"),
        ("sha256", res.get("sha256")),
        ("exe", res.get("exe")),
        ("idb", res.get("idb")),
        ("resolve", f"{(req.get('resolve') or {}).get('kind')}: {(req.get('resolve') or {}).get('value')}"),
        ("rva", res.get("rva")),
        ("file_off", res.get("file_off") or "-"),
        ("ea", res.get("ea")),
        ("func", f"{res.get('func')}  [{res.get('func_range')}]"),
        ("proto", res.get("proto")),
        ("why", req.get("why")),
        ("requested_by", req.get("by")),
        ("served", f"{res.get('served_at')} by re_serve.py v{SERVE_VERSION} on IDA {res.get('ida')}"),
    ]
    width = max(len(k) for k, _ in rows)
    if style == "c":
        body = "\n".join(f" * {k.ljust(width)} : {v}" for k, v in rows)
        return f"/* ND-RE v{SERVE_VERSION} - generated, do not edit\n{body}\n */\n\n"
    prefix = ";" if style == "asm" else "#"
    body = "\n".join(f"{prefix} {k.ljust(width)} : {v}" for k, v in rows)
    return f"{body}\n\n"


def render(req: dict, res: dict, role: str, payload: str) -> str:
    style = {"pseudo": "c", "disasm": "asm"}.get(role, "hash")
    return _header(req, res, style) + payload


# --------------------------------------------------------------------- main

def pending(re_dir: Path) -> list[dict]:
    """Requests with no outcome yet, one row per id (last line wins - a rename or a
    correction appends a line instead of editing one, exactly like re_kb.py does)."""
    served = {}
    for item in load_jsonl(re_dir / "served.jsonl"):
        served[item.get("req") or ""] = item
    latest: dict[str, dict] = {}
    for path in sorted((re_dir / "requests").glob("*.jsonl")):
        for item in load_jsonl(path):
            item.setdefault("_src", path.name)
            latest[item.get("id") or ""] = item
    out = [req for req in latest.values()
           if not (served.get(req.get("id") or "") and not req.get("force"))]
    out.sort(key=lambda r: (r.get("ts") or "", r.get("id") or ""))
    return out


def request_allowed(kind: str, is_target: bool) -> bool:
    """A reference database (the symbolized macOS .i64) must not answer address-based
    requests: the queue is shared with the 26.51 session, and searching PE signatures
    in a Mach-O marks them failed - which would quietly empty the queue."""
    return is_target or kind not in ("sig", "rva")


def _serve_one(ida, req: dict, re_dir: Path, build: dict, decomp: Path,
               addr_index: dict, index_path: Path, exe: Path, idb: str,
               ida_version: str, is_target: bool = True) -> str:
    """Serve one request.  Returns ok / failed / skipped.  Any exception is the
    caller's problem on purpose: one unguarded API call must not take the batch down
    silently, and the caller records the traceback in served.jsonl."""
    key = req.get("key") or "?"
    kind = (req.get("resolve") or {}).get("kind") or "?"
    if not request_allowed(kind, is_target):
        _log(f"[re_serve] {req.get('id')} {key}: SKIPPED - this is a reference database, "
             f"a {kind} request belongs to the target build's session. Nothing recorded; "
             f"it stays pending.")
        return "skipped"
    ea, how, meta = ida.resolve(req.get("resolve") or {})
    if ea is None:
        append_jsonl(re_dir / "served.jsonl", {
            "kind": "re-served", "req": req.get("id"), "key": key,
            "build": build["sha12"], "ok": False, "note": how,
        })
        _log(f"[re_serve] {req.get('id')} {key}: FAILED {how}")
        return "failed"

    func = ida.func_of(ea)
    if func is None:
        append_jsonl(re_dir / "served.jsonl", {
            "kind": "re-served", "req": req.get("id"), "key": key,
            "build": build["sha12"], "ok": False,
            "note": f"{how} resolved to {hex(ea)}, which is not inside a function",
        })
        _log(f"[re_serve] {req.get('id')} {key}: FAILED not a function at {hex(ea)}")
        return "failed"

    res = {
        "build": build["sha12"], "version": build["version"], "sha256": build["sha256"],
        "exe": str(exe), "idb": idb,
        "rva": ida.rva(func.start_ea), "file_off": ida.file_off(func.start_ea),
        "ea": hex(func.start_ea), "func": ida.name_of(func.start_ea),
        "func_range": f"{ida.rva(func.start_ea)} - {ida.rva(func.end_ea)}, "
                      f"{func.end_ea - func.start_ea} bytes",
        "proto": ida.proto(func.start_ea), "served_at": now(), "ida": ida_version,
    }

    files = []
    for role in req.get("want") or ["pseudo", "proto", "xrefs"]:
        if role == "proto":
            continue  # already in the header of every file
        if role == "pseudo":
            text, note = ida.pseudo(func.start_ea)
            if text is None:
                text = f"(no pseudocode: {note})\n\n{ida.disasm(func)}"
            files.append({"role": "pseudo",
                          **write_text(decomp / f"{key}.c", render(req, res, "pseudo", text))})
        elif role == "disasm":
            files.append({"role": "disasm", **write_text(
                decomp / f"{key}.asm", render(req, res, "disasm", ida.disasm(func)))})
        elif role == "xrefs":
            files.append({"role": "xrefs", **write_text(
                decomp / f"{key}.xrefs.txt", render(req, res, "xrefs", ida.xrefs_to(func.start_ea)))})
        elif role == "callees":
            files.append({"role": "callees", **write_text(
                decomp / f"{key}.callees.txt", render(req, res, "callees", ida.callees(func)))})
        elif role == "strings":
            files.append({"role": "strings", **write_text(
                decomp / f"{key}.strings.txt", render(req, res, "strings", ida.strings(func)))})
        else:
            _log(f"[re_serve] {req.get('id')} {key}: unknown want {role!r}, ignored")

    if not files:
        append_jsonl(re_dir / "served.jsonl", {
            "kind": "re-served", "req": req.get("id"), "key": key,
            "build": build["sha12"], "ok": False, "note": "no known `want` role produced a file",
        })
        _log(f"[re_serve] {req.get('id')} {key}: SKIPPED nothing was requested that I can write")
        return "skipped"

    addr_index[key] = {
        "build": build["sha12"], "rva": res["rva"], "func": res["func"],
        "proto": res["proto"], "files": [Path(f["path"]).name for f in files],
        "served_at": res["served_at"],
    }
    write_json(index_path, addr_index)

    append_jsonl(re_dir / "served.jsonl", {
        "kind": "re-served", "req": req.get("id"), "key": key,
        "build": build["sha12"], "ok": True,
        "rva": res["rva"], "ea": res["ea"], "func": res["func"],
        "files": [{"role": f["role"], "path": f["path"], "size": f["size"],
                   "sha256": f["sha256"], "truncated": f["truncated"]} for f in files],
    })
    _log(f"[re_serve] {req.get('id')} {key}: {res['func']} {res['rva']} -> "
         f"{', '.join(f['role'] for f in files)}")
    return "ok"


def main() -> int:
    re_dir = find_re_dir()
    if re_dir is None:
        _log("[re_serve] cannot find docs/agent/re - set ND_RE_DIR or run from the checkout")
        return 1

    try:
        ida = Ida()
    except ImportError as exc:
        _log(f"[re_serve] this must run inside IDA (Alt+F7), not plain Python: {exc}")
        return 2

    exe = ida.input_path()
    if exe is None:
        _log("[re_serve] this database has no input file path; cannot bind served facts "
             "to a build")
        return 1
    recorded_sha = ida.input_sha256()
    recorded_size = ida.input_size()
    if not recorded_sha and not exe.is_file():
        _log(f"[re_serve] cannot identify {exe}: not on disk and the database recorded no hash")
        return 1

    expected = configured_exe(re_dir)
    is_target = bool(expected is not None and expected.is_file()
                     and expected.resolve() == exe.resolve())
    if expected is not None and expected.is_file():
        if expected.resolve() == exe.resolve() and recorded_size and \
                expected.stat().st_size != recorded_size:
            _log(f"[re_serve] WARNING this .i64 was built from a different copy of "
                 f"{exe.name}: recorded {recorded_size:,} B, on disk "
                 f"{expected.stat().st_size:,} B - facts would be filed under a build the "
                 f"hooks do not target")
        elif expected.resolve() != exe.resolve():
            _log(f"[re_serve] reference database: {exe} (this checkout deploys {expected}). "
                 f"Only --name / --addr requests are served from here.")

    build = build_identity(re_dir, exe, recorded_sha, recorded_size)
    base, decomp = build_paths(re_dir, build)
    idb = ida.idb_path() or "-"
    index_path = base / "addr_index.json"
    addr_index = read_json(index_path, {})

    queue = pending(re_dir)
    _log(f"[re_serve] idb: {exe}")
    _log(f"[re_serve] build {build['sha12']} ({build['version']}), {len(queue)} pending request(s)")
    if not queue:
        _log("[re_serve] queue empty - nothing to do")
        return 0

    ida_version = "?"
    try:
        import ida_kernwin

        ida_version = ida_kernwin.get_kernel_version()
    except Exception:
        pass

    ok = failed = skipped = 0
    for req in queue[:MAX_PER_RUN]:
        try:
            outcome = _serve_one(ida, req, re_dir, build, decomp, addr_index,
                                 index_path, exe, idb, ida_version, is_target)
        except Exception as exc:
            import traceback

            append_jsonl(re_dir / "served.jsonl", {
                "kind": "re-served", "req": req.get("id"), "key": req.get("key"),
                "build": build["sha12"], "ok": False,
                "note": f"crashed: {type(exc).__name__}: {exc}",
            })
            _log(f"[re_serve] {req.get('id')} {req.get('key')}: CRASHED {type(exc).__name__}: {exc}")
            _log(traceback.format_exc())
            failed += 1
            continue
        if outcome == "ok":
            ok += 1
        elif outcome == "skipped":
            skipped += 1
        else:
            failed += 1

    left = max(0, len(queue) - MAX_PER_RUN)
    if left:
        _log(f"[re_serve] served {ok}, failed {failed}, {left} still queued - "
             f"run Alt+F7 again to continue")
    else:
        _log(f"[re_serve] served {ok}, failed {failed}, queue drained")
    return 0


if __name__ == "__main__" or "idaapi" in sys.modules:
    _rc = main()
else:
    _rc = None
