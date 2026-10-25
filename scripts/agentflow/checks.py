#!/usr/bin/env python3
"""Deterministic pre-flight checks for the NeuroDragon worktree.

Every check here exists because the 2026-10-25 VV/DLSS session lost real-machine
rounds to the class of mistake it catches.  A check that cannot see what it is
supposed to look at reports itself as *blind* instead of passing: "no programs
found" is a failure, never a green light.

Checks:
  ngx.params    The NGX parameter names the source writes must exist in the
                runtime that will receive them (nvngx_dlss.dll / nvngx_dlssnr.dll,
                captured in ngx_keys.txt).  On 2026-10-25 "JitterOffsetX" was
                written where the runtime only knows "Jitter.Offset.X"; every
                evaluate answered 0xBAD00005 and three real-machine runs went to
                finding that out.
  ngx.required  The DLSS-SR evaluate path must actually set the parameters DLSS
                requires.  The same session first deleted the float writes and
                then lost another round to the resulting "missing Jitter" error.
  shader        Every inline HLSL program is compiled offline with d3dcompiler_47
                exactly as the mod compiles it at runtime (entry main, cs_5_0).
                A shader typo otherwise costs a game start to discover.
"""

from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

# --------------------------------------------------------------------------- #
# findings
# --------------------------------------------------------------------------- #


def finding(check: str, fid: str, level: str, message: str, where: str = "") -> dict:
    return {"check": check, "id": fid, "level": level, "message": message, "where": where}


def _rel(root: Path, path: Path) -> str:
    try:
        return str(path.relative_to(root)).replace("\\", "/")
    except ValueError:
        return str(path)


def _sources(root: Path) -> list[Path]:
    out = []
    for sub in ("src",):
        base = root / sub
        if not base.is_dir():
            continue
        for ext in ("*.cpp", "*.h", "*.hpp"):
            out.extend(base.rglob(ext))
    return sorted(out)


# --------------------------------------------------------------------------- #
# NGX parameters
# --------------------------------------------------------------------------- #

# `paramSetU32(params, "Name", ...)` / `paramF32(params, "Name", ...)` and friends.
PARAM_CALL = re.compile(r"\bparam[A-Za-z0-9_]*\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*\"([^\"]+)\"")

# Names that legitimately are not NGX keys but sit in the same call position.
NOT_A_PARAMETER = {"main"}


def _runtime_blobs(root: Path) -> list[bytes]:
    """The NGX feature DLLs of the configured test instance, when they are here.

    The committed key list is the oracle on machines that have no runtime; where the
    runtime *is* present, the bytes win - that closes the "just add the name to the
    list" hole without making the build depend on the game being installed.
    """
    config = root / "scripts" / "agentflow" / "config.json"
    if not config.is_file():
        return []
    try:
        instance = json.loads(config.read_text(encoding="utf-8")).get("instance", "")
    except json.JSONDecodeError:
        return []
    if not instance:
        return []
    blobs = []
    for name in ("nvngx_dlss.dll", "nvngx_dlssnr.dll"):
        path = Path(instance) / name
        if path.is_file():
            blobs.append(path.read_bytes())
    return blobs


def _ngx_keys(root: Path) -> list[str]:
    path = root / "scripts" / "agentflow" / "ngx_keys.txt"
    if not path.is_file():
        return []
    keys = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        keys.append(line)
    return keys


def _function_containing(text: str, needle: str) -> str:
    """Return the body of the top-level block (function) that contains `needle`."""
    at = text.find(needle)
    if at < 0:
        return ""
    depth = 0
    start = -1
    for i, ch in enumerate(text):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start >= 0:
                if start <= at <= i:
                    return text[start : i + 1]
                start = -1
    return ""


def check_ngx_params(root: Path) -> list[dict]:
    findings: list[dict] = []
    keys = _ngx_keys(root)
    if not keys:
        findings.append(
            finding(
                "ngx.params",
                "ngx.keys-blind",
                "error",
                "scripts/agentflow/ngx_keys.txt is missing or empty: the parameter-name "
                "check cannot see the runtime, so it is not allowed to pass. Regenerate "
                "with `flow.py keys --dll <nvngx_dlss.dll> --dll <nvngx_dlssnr.dll>`.",
            )
        )
        return findings

    known = set(keys)
    seen: dict[str, list[str]] = {}
    for path in _sources(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        for name in PARAM_CALL.findall(text):
            if name in NOT_A_PARAMETER:
                continue
            seen.setdefault(name, []).append(_rel(root, path))

    for name in sorted(seen):
        if name not in known:
            where = seen[name][0]
            findings.append(
                finding(
                    "ngx.params",
                    "ngx.unknown-name",
                    "error",
                    f"parameter {name!r} is not a name the NGX runtime knows "
                    f"(used in {where}; first failure of this kind: JitterOffsetX vs "
                    f"Jitter.Offset.X, 2026-10-25)",
                    where,
                )
            )

    # Second oracle: the actual runtime bytes, when this machine has them.
    blobs = _runtime_blobs(root)
    for name in sorted(seen) if blobs else []:
        if name in known and not any(name.encode() in blob for blob in blobs):
            findings.append(
                finding(
                    "ngx.params",
                    "ngx.name-not-in-runtime",
                    "error",
                    f"parameter {name!r} is listed in ngx_keys.txt but does not occur in the "
                    f"configured instance's NGX DLLs - regenerate the list with `flow.py keys "
                    f"--dll ... --verify` instead of adding names by hand",
                    seen[name][0],
                )
            )

    # Required parameters for a DLSS-SR evaluation, checked inside the function
    # that actually calls the feature.
    required_path = root / "scripts" / "agentflow" / "ngx_required.json"
    required: list[dict] = []
    if required_path.is_file():
        try:
            required = json.loads(required_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            findings.append(
                finding("ngx.required", "ngx.required-unreadable", "error", f"ngx_required.json: {exc}")
            )
    for path in _sources(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        if "gEvaluate(" not in text:
            continue
        body = _function_containing(text, "gEvaluate(")
        if not body:
            findings.append(
                finding("ngx.required", "ngx.evaluate-not-found", "error",
                        "found a gEvaluate( call but could not isolate its function",
                        _rel(root, path))
            )
            continue
        for item in required:
            name = item["name"]
            if f'"{name}"' not in body:
                findings.append(
                    finding(
                        "ngx.required",
                        "ngx.missing-required",
                        item.get("level", "error"),
                        f"DLSS-SR evaluate never sets {name!r} ({item.get('why', 'required by the model')})",
                        _rel(root, path),
                    )
                )
    return findings


# --------------------------------------------------------------------------- #
# HLSL
# --------------------------------------------------------------------------- #

CHUNK = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*R\"hlsl\((.*?)\)hlsl\"", re.S)
ENTRY = b"main"
TARGET = b"cs_5_0"


def _compile(src: str, entry: bytes = ENTRY, target: bytes = TARGET) -> tuple[int, str]:
    import ctypes

    d3d = ctypes.WinDLL("d3dcompiler_47.dll")
    fn = d3d.D3DCompile
    fn.restype = ctypes.c_long
    fn.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_uint, ctypes.c_uint,
        ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p),
    ]
    getbuf = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)
    getsize = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)

    def blob_bytes(blob: int) -> bytes:
        vt = ctypes.cast(blob, ctypes.POINTER(ctypes.c_void_p))
        table = ctypes.cast(vt[0], ctypes.POINTER(ctypes.c_void_p))
        size = getsize(table[4])(blob)
        return b"" if not size else ctypes.string_at(getbuf(table[3])(blob), size)

    code = ctypes.c_void_p()
    err = ctypes.c_void_p()
    data = src.encode("utf-8")
    hr = fn(data, len(data), b"agentflow", None, None, entry, target, 0, 0,
            ctypes.byref(code), ctypes.byref(err))
    message = blob_bytes(err).decode("utf-8", "replace") if err else ""
    return hr & 0xFFFFFFFF, message


def _programs(root: Path) -> tuple[list[tuple[str, str, str]], list[str], list[str]]:
    """Return (programs, blind-spots, out-of-scope-chunks).

    Only `src/neurodragon/vv/` is compiled here: every HLSL program in that folder
    is self-contained and carries the class of mistake this check exists for.  The
    NR/SR bridges compose their shader source with engine-provided code, so they
    are reported as out of scope instead of being silently assumed covered.
    """
    vv = root / "src" / "neurodragon" / "vv"
    chunks: dict[str, tuple[str, str]] = {}
    for path in sorted(vv.glob("*.cpp")) + sorted(vv.glob("*.h")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for name, body in CHUNK.findall(text):
            chunks[name] = (body, _rel(root, path))

    outside: list[str] = []
    for path in _sources(root):
        if vv in path.parents:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        outside.extend(f"{name} ({_rel(root, path)})" for name, _ in CHUNK.findall(text))

    tone = chunks.get("kHlsl", ("", ""))[0]
    programs: list[tuple[str, str, str]] = []
    used: set[str] = set()

    for name, (body, where) in chunks.items():
        if not name.endswith("Head"):
            continue
        body_name = name[: -len("Head")] + "Body"
        if body_name in chunks:
            used.update({name, body_name, "kHlsl"})
            programs.append((f"{name[:-4]} (Head + tone + Body)", body + tone + chunks[body_name][0], where))

    for name, (body, where) in chunks.items():
        if name in used or name == "kHlsl":
            continue
        if name.endswith("Shader"):
            used.add(name)
            programs.append((name, body, where))

    blind = [name for name in chunks if name not in used and name != "kHlsl"]
    return programs, blind, outside


def check_shaders(root: Path) -> list[dict]:
    findings: list[dict] = []
    if os.name != "nt":
        findings.append(
            finding("shader", "shader.platform-skip", "warn",
                    "d3dcompiler_47 is only available on Windows; the shader check cannot run here")
        )
        return findings

    programs, blind, outside = _programs(root)
    for item in outside:
        findings.append(
            finding("shader", "shader.out-of-scope", "warn",
                    f"HLSL chunk {item} is outside src/neurodragon/vv and is NOT compiled by this check")
        )
    if not programs:
        findings.append(
            finding("shader", "shader.no-programs", "error",
                    "no inline HLSL programs were found under src/neurodragon/vv: the extractor went "
                    "blind, which is a failure, not a pass. Update CHUNK/_programs() if the layout changed.")
        )
    for name in sorted(blind):
        findings.append(
            finding("shader", "shader.orphan-chunk", "error",
                    f"HLSL chunk {name!r} is neither a standalone *Shader nor half of a Head/Body pair, "
                    f"so it is not compiled by this check")
        )
    for label, source, where in programs:
        hr, message = _compile(source)
        if hr != 0:
            first = message.strip().splitlines()[0] if message.strip() else f"hr=0x{hr:08X}"
            findings.append(finding("shader", "shader.compile", "error", f"{label}: {first}", where))
    return findings


# --------------------------------------------------------------------------- #


def run(root: Path) -> list[dict]:
    return check_ngx_params(root) + check_shaders(root)


if __name__ == "__main__":
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    found = run(root)
    for item in found:
        print(f"{item['level']:5} {item['check']:12} {item['id']:24} {item['message']}")
    sys.exit(1 if any(i["level"] == "error" for i in found) else 0)
