#!/usr/bin/env python3
"""agentflow - execution discipline for long, multi-step, real-machine tasks.

Why this exists
---------------
The 2026-10-25 VV/DLSS session shipped 26 builds and ~20 game runs in 2h50m, and
several of those runs only existed to discover a mistake that was already visible
in the source, in the NGX log, or in the previous run's evidence.  This tool turns
the rules that were being recited into state that a command can check:

  contract   goal / acceptance / stage / out-of-scope / final criteria, and a stage
             that can only advance through `claim` with fresh evidence.
  ledger     append-only JSONL of experiments and their evidence hashes.  A closed
             failure cannot be followed by another attempt until a *newer*
             evidence file exists (that is what "analysed the log" looks like).
  gate       deterministic checks before a build (`flow.py gate build`, wired into
             xmake) and a hard precondition for anything that touches the game.
  deploy     the only sanctioned way to put a build in front of a human: needs a
             passing verify for the exact tree, an open experiment, an unwaived
             check result, and a fresh build.
  review     re-executes the checks and re-hashes the evidence, so a reviewer can
             verify claims without reading the author's prose.
  doctor     finds the states that mean someone bypassed the flow.

Nothing here proves the game works; it proves the steps that can be checked were
checked, and it makes skipping them visible.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import checks  # noqa: E402  (local module, same folder)

STAGES = {
    1: "目标与接口契约确认",
    2: "静态检查与构建通过",
    3: "核心功能实际执行成功",
    4: "输出路径与用户可见结果",
    5: "最终目标语义（替换，不是覆盖）",
    6: "回归检查与交付",
}

DEFAULT_CONFIG = {
    "instance": "",
    "max_attempts_per_unknown": 3,
    "stale_experiment_hours": 12,
    # L1 launches restart the game on a shared machine; the budget is what makes
    # "default yes" safe, and the hint is when `doctor` suggests a commit.
    "l1_max_per_session": 4,
    "commit_hint_hours": 6,
}


# --------------------------------------------------------------------------- #
# small helpers
# --------------------------------------------------------------------------- #


def now() -> str:
    return datetime.datetime.now().astimezone().isoformat(timespec="milliseconds")


def stamp() -> float:
    return datetime.datetime.now().timestamp()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def read_json(path: Path, default):
    if not path.is_file():
        return default
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"agentflow: {path} is not valid JSON: {exc}")


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def git(root: Path, *args: str) -> str:
    # git speaks UTF-8; this console is cp936.  Without an explicit encoding a
    # Chinese commit message or source comment is decoded as cp936 - mojibake in
    # the digest, or a UnicodeDecodeError that then looks like a git failure.
    try:
        out = subprocess.run(
            ["git", *args], cwd=root, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    return out.stdout if out.returncode == 0 else ""


def paths(root: Path) -> dict[str, Path]:
    agent = root / "docs" / "agent"
    flow = root / "scripts" / "agentflow"
    return {
        "agent": agent,
        "contract": agent / "contract.json",
        "ledger": agent / "ledger.jsonl",
        "evidence": agent / "evidence",
        "config": flow / "config.json",
        "config_local": flow / "config.local.json",
        "waivers": flow / "waivers.json",
        "bin": root / "bin" / "NeuroDragon",
    }


def config(root: Path) -> dict:
    cfg = dict(DEFAULT_CONFIG)
    cfg.update(read_json(paths(root)["config"], {}))
    # `config.json` is tracked, so every checkout reads it - but the main and
    # agent2 checkouts deploy to different game instances.  The git-ignored local
    # file carries the machine/checkout-specific instance; with neither file
    # naming one, `instance` stays empty and `deploy` refuses instead of guessing
    # a target (a wrong guess would overwrite the other checkout's test instance).
    cfg.update(read_json(paths(root)["config_local"], {}))
    return cfg


def contract(root: Path) -> dict:
    data = read_json(paths(root)["contract"], None)
    if data is None:
        raise SystemExit("agentflow: docs/agent/contract.json is missing (run `flow.py init`)")
    for key in ("goal", "criteria", "stage"):
        if key not in data:
            raise SystemExit(f"agentflow: contract.json has no {key!r}")
    return data


def ledger(root: Path) -> list[dict]:
    path = paths(root)["ledger"]
    if not path.is_file():
        return []
    events = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            events.append({"kind": "corrupt", "raw": line[:120]})
    return events


def append(root: Path, event: dict) -> dict:
    event = {"ts": now(), **event}
    path = paths(root)["ledger"]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(event, ensure_ascii=False) + "\n")
    return event


def experiments(root: Path) -> dict[str, dict]:
    merged: dict[str, dict] = {}
    for event in ledger(root):
        if event.get("kind") == "exp":
            merged.setdefault(event["id"], {}).update(event)
    return merged


def experiment_opened(root: Path, exp_id: str) -> str:
    """When the attempt started.

    `exp update` appends events, so the merged record's ts is the last edit, not
    the open time.  Staleness must not reset just because a runbook was filled in
    twelve hours later.
    """
    for event in ledger(root):
        if (event.get("kind") == "exp" and event.get("id") == exp_id
                and event.get("status") == "open"):
            return event.get("opened") or event["ts"]
    return ""


def evidence_entry(path: Path) -> dict:
    return {"path": str(path), "sha256": sha256_file(path), "size": path.stat().st_size}


def evidence_still_matches(entry: dict) -> bool:
    path = Path(entry["path"])
    return path.is_file() and sha256_file(path) == entry["sha256"]


# --------------------------------------------------------------------------- #
# tree identity / build freshness
# --------------------------------------------------------------------------- #


# Two different questions, two different file sets:
#   identity  - "is this the same tree the verify ran on?"  -> also covers the gate,
#               so weakening a check invalidates the old verify.
#   staleness - "was the DLL built after the last code edit?" -> only build inputs,
#               because editing a check does not make the binary old.
DIGEST_DIRS = ("src", "scripts/agentflow")
BUILD_INPUT_DIRS = ("src",)
EXTRA_INPUTS = ("xmake.lua", "manifest.json", "tooth.json")


def _files_under(root: Path, dirs: tuple[str, ...]) -> list[Path]:
    out = []
    for name in dirs:
        base = root / name
        if base.is_dir():
            out.extend(p for p in base.rglob("*")
                       if p.is_file() and "__pycache__" not in p.parts and p.suffix != ".pyc")
    for extra in EXTRA_INPUTS:
        p = root / extra
        if p.is_file():
            out.append(p)
    return out


def _tree_files(root: Path) -> list[tuple[str, Path]]:
    """Files that define "the tree": everything the gate reads, tracked or not."""
    scopes = (*DIGEST_DIRS, *EXTRA_INPUTS)
    names: set[str] = set()
    if git(root, "rev-parse", "--is-inside-work-tree").strip() == "true":
        for args in (("ls-files", "--"), ("ls-files", "--others", "--exclude-standard", "--")):
            for rel in git(root, *args, *scopes).splitlines():
                rel = rel.strip()
                if rel and "__pycache__" not in rel and not rel.endswith(".pyc"):
                    names.add(rel)
    if not names:
        # not a git checkout (test fixture): walk the files themselves
        for p in _files_under(root, DIGEST_DIRS):
            names.add(p.relative_to(root).as_posix())
    return sorted((rel, root / rel) for rel in names)


def head_sha(root: Path) -> str:
    return git(root, "rev-parse", "HEAD").strip()


def tree_digest(root: Path) -> str:
    """Content hash of the working tree.

    Deliberately independent of HEAD and of whether the change is committed: the
    same bytes must hash the same before and after `git commit`.  Otherwise every
    commit invalidates the verify that justified it, and `flow.py commit` could
    never approve a tree it just checked.  Which commit a record belongs to is
    provenance, and each ledger event stores `head` separately.
    """
    h = hashlib.sha256()
    for rel, path in _tree_files(root):
        h.update(rel.encode("utf-8", "surrogateescape"))
        h.update(b"\0")
        # A deletion is a change too: a tracked file that is gone still has to
        # move the digest, or removing a file would look like "same tree".
        h.update(path.read_bytes() if path.is_file() else b"<deleted>")
        h.update(b"\0")
    return h.hexdigest()[:16]


def dirty_paths(root: Path) -> list[str]:
    """Working-tree paths that differ from HEAD, untracked files included."""
    return sorted({rel for _, rel in _dirty_status(root)})


def oneline(text: str, limit: int = 96) -> str:
    """Collapse a ledger field into a single printable line."""
    flat = " ".join(str(text or "").split())
    return flat if len(flat) <= limit else flat[: limit - 1] + "…"



def newest_source_mtime(root: Path) -> float:
    times = [p.stat().st_mtime for p in _files_under(root, BUILD_INPUT_DIRS) if p.is_file()]
    return max(times) if times else 0.0


# --------------------------------------------------------------------------- #
# waivers
# --------------------------------------------------------------------------- #


def apply_waivers(root: Path, findings: list[dict]) -> tuple[list[dict], list[dict]]:
    waivers = read_json(paths(root)["waivers"], [])
    today = datetime.date.today().isoformat()
    used, expired = [], []
    out = []
    for item in findings:
        match = None
        for waiver in waivers:
            if waiver.get("check") != item["check"]:
                continue
            if waiver.get("match") and waiver["match"] not in item["message"]:
                continue
            match = waiver
            break
        if not match:
            out.append(item)
            continue
        if match.get("expires", "0000-00-00") < today:
            expired.append(match)
            out.append(item)
            continue
        used.append({**match, "finding": item["id"], "message": item["message"]})
        item = {**item, "level": "warn", "waived": match.get("expires")}
        out.append(item)
    return out, used + [{"expired": True, **w} for w in expired]


def run_checks(root: Path) -> tuple[list[dict], list[dict]]:
    return apply_waivers(root, checks.run(root))


def print_findings(findings: list[dict]) -> None:
    order = {"error": 0, "warn": 1, "info": 2}
    for item in sorted(findings, key=lambda i: (order.get(i["level"], 3), i["id"])):
        tag = item["level"].upper()
        if item.get("waived"):
            tag = f"WAIVED->{item['waived']}"
        print(f"  {tag:16} {item['id']:24} {item['message']}")


# --------------------------------------------------------------------------- #
# commands
# --------------------------------------------------------------------------- #


def cmd_init(root: Path, args) -> int:
    p = paths(root)
    p["evidence"].mkdir(parents=True, exist_ok=True)
    if not p["ledger"].is_file():
        p["ledger"].write_text("", encoding="utf-8")
    if not p["contract"].is_file():
        template = {
            "goal": "",
            "criteria": [
                {"id": f"S{n}", "stage": n, "kind": "stage", "text": STAGES[n]} for n in STAGES
            ]
            + [{"id": "F1", "stage": 6, "kind": "final", "text": ""}],
            "out_of_scope": [],
            "stage": 1,
            "updated": now(),
        }
        write_json(p["contract"], template)
        print(f"wrote template {p['contract']} - fill in goal/criteria/out_of_scope")
    if not p["config"].is_file():
        write_json(p["config"], DEFAULT_CONFIG)
    print(f"agentflow ready under {p['agent']}")
    return 0


def cmd_status(root: Path, args) -> int:
    p = paths(root)
    con = contract(root)
    events = ledger(root)
    print(f"goal     : {con['goal']}")
    print(f"stage    : {con['stage']} - {STAGES.get(con['stage'], '?')}")
    if con.get("out_of_scope"):
        print(f"scope out: {', '.join(con['out_of_scope'])}")
    print(f"tree     : {tree_digest(root)}")

    accepts = {}
    for event in events:
        if event.get("kind") == "accept":
            accepts[event["criterion"]] = event
    print("\ncriteria :")
    for item in con["criteria"]:
        accept = accepts.get(item["id"])
        if not accept:
            state = "open"
        elif all(evidence_still_matches(e) for e in accept.get("evidence", [])):
            state = f"accepted {accept['ts'][:16]}"
        else:
            state = "STALE (evidence changed)"
        print(f"  {item['id']:4} stage {item.get('stage', '?')} {item.get('kind', 'stage'):5} {state:22} {item['text'][:70]}")

    exps = experiments(root)
    open_exps = [e for e in exps.values() if e.get("status") == "open"]
    print(f"\nexperiments: {len(exps)} total, {len(open_exps)} open")
    for exp in open_exps:
        print(f"  {exp['id']} unknown={exp['unknown']} attempt={exp.get('attempt')} tree={exp.get('tree')}")

    def last(kind):
        return next((e for e in reversed(events) if e.get("kind") == kind), None)

    for kind in ("verify", "review", "deploy", "claim"):
        event = last(kind)
        if event:
            extra = ""
            if kind in ("verify", "review"):
                extra = f" ok={event.get('ok')} tree={event.get('tree')}"
            if kind == "deploy":
                extra = f" exp={event.get('exp')} dll={event.get('dll_sha256', '')[:12]}"
            print(f"last {kind:7}: {event['ts'][:16]}{extra}")
    return 0


def cmd_contract(root: Path, args) -> int:
    p = paths(root)
    con = contract(root)
    if args.action == "show":
        print(json.dumps(con, ensure_ascii=False, indent=2))
        return 0
    if not args.user_authorization:
        print("agentflow: contract changes need --user-authorization \"<who/what>\".")
        print("           A plan document cannot authorise a goal or scope change.")
        return 1
    if args.goal:
        con["goal"] = args.goal
    if args.out_of_scope is not None:
        con["out_of_scope"] = [x.strip() for x in args.out_of_scope.split(";") if x.strip()]
    if args.criteria_file:
        con["criteria"] = json.loads(Path(args.criteria_file).read_text(encoding="utf-8"))
    con["updated"] = now()
    write_json(p["contract"], con)
    append(root, {"kind": "contract", "authorization": args.user_authorization,
                  "goal": con["goal"], "out_of_scope": con["out_of_scope"]})
    print("contract updated (recorded who authorised it)")
    return 0


def cmd_accept(root: Path, args) -> int:
    con = contract(root)
    item = next((c for c in con["criteria"] if c["id"] == args.criterion), None)
    if not item:
        print(f"agentflow: unknown criterion {args.criterion!r}")
        return 1
    entries = []
    for raw in args.evidence:
        path = Path(raw)
        if not path.is_absolute():
            path = root / path
        if not path.is_file() or path.stat().st_size == 0:
            print(f"agentflow: evidence {raw!r} does not exist or is empty")
            return 1
        entries.append(evidence_entry(path))
    if not entries:
        print("agentflow: accepting a criterion needs at least one evidence file")
        return 1
    append(root, {"kind": "accept", "criterion": args.criterion, "evidence": entries,
                  "note": args.note or ""})
    print(f"accepted {args.criterion} with {len(entries)} evidence file(s)")
    return 0


def _experiment_tail(root: Path, unknown: str) -> dict | None:
    # Events are append-ordered; the merged record of the last one is the tail.
    same = [e for e in experiments(root).values() if e["unknown"] == unknown]
    same.sort(key=lambda e: e["ts"])
    return same[-1] if same else None


def cmd_exp_new(root: Path, args) -> int:
    cfg = config(root)
    exps = experiments(root)
    previous = _experiment_tail(root, args.unknown)
    attempt = 1
    if previous:
        if previous.get("status") == "open":
            print(f"agentflow: {previous['id']} is still open on {args.unknown!r}; close it with "
                  f"its evidence, or cancel it, before opening another one.")
            return 1
        if previous.get("status") == "cancelled":
            # A cancelled run learned nothing and spent no attempt: same number,
            # and none of the "analyse the previous evidence" demands apply.
            attempt = int(previous.get("attempt") or 1)
        else:
            attempt = int(previous.get("attempt", 0)) + 1
            if not args.postmortem:
                print(f"agentflow: {previous['id']} already tested {args.unknown!r} "
                      f"({previous.get('status')}). Record what its evidence showed with --postmortem.")
                return 1
            if not args.from_evidence:
                print(f"agentflow: refusing a repeat without new evidence. --from-evidence must point at a "
                      f"file produced after {previous['id']} ({previous['ts'][:16]}).")
                return 1
            path = Path(args.from_evidence)
            if not path.is_absolute():
                path = root / path
            if not path.is_file():
                print(f"agentflow: --from-evidence {args.from_evidence!r} does not exist")
                return 1
            # Ledger stamps carry milliseconds and a filesystem may not; give the
            # comparison a small slack so a file written in the same second as the
            # failed attempt cannot masquerade as "produced after it".
            previous_ts = datetime.datetime.fromisoformat(previous["ts"]).timestamp()
            if path.stat().st_mtime <= previous_ts + 0.5:
                print(f"agentflow: --from-evidence {args.from_evidence!r} is older than {previous['id']} - "
                      f"it cannot be evidence that you analysed that result.")
                return 1
    limit = int(cfg.get("max_attempts_per_unknown", 3))
    if attempt > limit and not args.user_authorization:
        print(f"agentflow: attempt {attempt} on {args.unknown!r} exceeds the budget of {limit}. "
              f"A repeat run needs --user-authorization \"<who/what>\".")
        return 1

    exp_id = f"EXP-{len(exps) + 1:04d}"
    event = {
        "kind": "exp", "id": exp_id, "status": "open", "attempt": attempt,
        "opened": now(),
        "unknown": args.unknown, "hypothesis": args.hypothesis, "change": args.change,
        "expect_pass": args.expect_pass, "expect_fail": args.expect_fail,
        "evidence_plan": args.evidence_plan, "tree": tree_digest(root),
        "head": head_sha(root),
        "tier": args.tier,
        "runbook": [step.strip() for step in (args.runbook or "").split("|") if step.strip()],
        "expect_s": int(args.expect_s or 0),
        "postmortem": args.postmortem or "",
        "authorization": args.user_authorization or "",
    }
    if args.from_evidence:
        path = Path(args.from_evidence)
        if not path.is_absolute():
            path = root / path
        event["from_evidence"] = evidence_entry(path)
    append(root, event)
    print(f"{exp_id} open  unknown={args.unknown} attempt={attempt} tier={args.tier} "
          f"tree={event['tree']}")
    return 0


def cmd_exp_update(root: Path, args) -> int:
    """Fill in fields that were not known when the experiment was opened.

    The runbook is the obvious one: it belongs to the moment you decide to hand
    the run to a human, which is usually after the experiment exists.
    """
    exp = experiments(root).get(args.id)
    if not exp:
        print(f"agentflow: no experiment {args.id!r}")
        return 1
    if exp.get("status") != "open":
        print(f"agentflow: {args.id} is closed ({exp.get('status')}); history is not editable - "
              f"open a new attempt instead")
        return 1
    fields: dict = {}
    if args.tier:
        fields["tier"] = args.tier
    if args.runbook is not None:
        fields["runbook"] = [step.strip() for step in args.runbook.split("|") if step.strip()]
    if args.expect_s is not None:
        fields["expect_s"] = int(args.expect_s)
    if not fields:
        print("agentflow: exp update needs --tier, --runbook or --expect-s")
        return 1
    append(root, {"kind": "exp", "id": args.id, "updated": now(), **fields})
    print(f"{args.id} updated: " + ", ".join(f"{k}={v}" for k, v in fields.items()))
    return 0


def cmd_exp_cancel(root: Path, args) -> int:
    """Close an attempt that never happened.

    AGENTS.md §8:「没跑 / 现在不行」不算失败.  Before this command the only way to
    end such an attempt was to attach a fake evidence file to `exp close`, which
    poisons the ledger - so the honest path has to be a first-class one.
    """
    exp = experiments(root).get(args.id)
    if not exp:
        print(f"agentflow: no experiment {args.id!r}")
        return 1
    if exp.get("status") != "open":
        print(f"agentflow: {args.id} is already {exp.get('status')}")
        return 1
    append(root, {"kind": "exp", "id": args.id, "status": "cancelled",
                  "result": args.why, "conclusion": f"cancelled: {args.why}",
                  "next": "", "head": head_sha(root)})
    print(f"{args.id} cancelled (no evidence needed, no attempt spent)")
    return 0


def cmd_exp_close(root: Path, args) -> int:
    event = experiments(root).get(args.id)
    if not event:
        print(f"agentflow: no experiment {args.id!r}")
        return 1
    if event.get("status") != "open":
        print(f"agentflow: {args.id} is already closed ({event.get('status')})")
        return 1
    entries = []
    for raw in args.evidence or []:
        path = Path(raw)
        if not path.is_absolute():
            path = root / path
        if not path.is_file() or path.stat().st_size == 0:
            print(f"agentflow: evidence {raw!r} does not exist or is empty")
            return 1
        entries.append(evidence_entry(path))
    if not entries:
        print("agentflow: closing an experiment needs --evidence (a log, dump or screenshot)")
        return 1
    status = "pass" if args.pass_result else "fail"
    if status == "fail" and not args.next:
        print("agentflow: a failed experiment must record --next (the next hypothesis and its evidence)")
        return 1
    # The dirty set at close time is the cheapest record of "what this experiment
    # touched"; `brief` uses it to attribute uncommitted files to an experiment
    # without asking anyone to type a file list.
    append(root, {"kind": "exp", "id": args.id, "status": status, "result": args.result,
                  "conclusion": args.conclusion, "next": args.next or "",
                  "evidence": entries, "dirty": dirty_paths(root)[:60],
                  "head": head_sha(root)})
    print(f"{args.id} closed {status}")
    return 0


def _gate(root: Path, record: bool) -> int:
    findings, waived = run_checks(root)
    errors = [f for f in findings if f["level"] == "error"]
    print(f"agentflow gate: {len(findings)} finding(s), {len(errors)} error(s), "
          f"{sum(1 for f in findings if f.get('waived'))} waived")
    print_findings(findings)
    for waiver in waived:
        if waiver.get("expired"):
            print(f"  WAIVER EXPIRED {waiver.get('match')}: {waiver.get('reason', '')}")
    ok = not errors
    if record:
        append(root, {"kind": "verify", "tree": tree_digest(root), "head": head_sha(root), "ok": ok,
                      "findings": [{"id": f["id"], "level": f["level"], "waived": f.get("waived")}
                                   for f in findings]})
    return 0 if ok else 1


def cmd_gate(root: Path, args) -> int:
    if args.what != "build":
        print("agentflow: only `gate build` exists")
        return 2
    return _gate(root, record=False)


def cmd_verify(root: Path, args) -> int:
    return _gate(root, record=True)


def _resolve_instance(root: Path, args) -> Path | None:
    raw = getattr(args, "instance", None) or config(root).get("instance") or ""
    if not raw:
        return None
    path = Path(raw)
    return path if path.is_absolute() else (root / path)


def cmd_deploy(root: Path, args) -> int:
    cfg = config(root)
    inst = _resolve_instance(root, args)
    if inst is None:
        print("agentflow: no instance configured (set `instance` in "
              "scripts/agentflow/config.local.json, or pass --instance)")
        return 1
    exp = experiments(root).get(args.exp)
    if not exp:
        print(f"agentflow: no experiment {args.exp!r}")
        return 1
    if exp.get("status") != "open":
        print(f"agentflow: {args.exp} is closed; open a new experiment before another run")
        return 1

    findings, waived = run_checks(root)
    errors = [f for f in findings if f["level"] == "error"]
    waived_findings = [f for f in findings if f.get("waived")]

    tree = tree_digest(root)
    verifies = [e for e in ledger(root) if e.get("kind") == "verify" and e.get("tree") == tree]
    if not verifies:
        print("agentflow: no `flow.py verify` recorded for the current tree - run it first")
        return 1
    if not verifies[-1].get("ok"):
        print("agentflow: the last verify for this tree failed")
        return 1
    if errors:
        print("agentflow: the gate still has errors; fix them (or waive with a reason and expiry) first")
        print_findings(errors)
        return 1
    if waived_findings and not args.allow_waived:
        print("agentflow: this build is covered by waivers. Deploying a known deviation needs "
              "--allow-waived \"<why this run is still useful>\".")
        print_findings(waived_findings)
        return 1

    artifact = paths(root)["bin"] / "NeuroDragon.dll"
    if not artifact.is_file():
        print(f"agentflow: {artifact} is missing - build first")
        return 1
    if artifact.stat().st_mtime < newest_source_mtime(root):
        print("agentflow: the build is older than the newest source file - rebuild before deploying")
        return 1

    # A repeat of the same *deployed* tree after a failure is exactly the loop the
    # budget exists for.  The experiment records the tree it was opened on, which is
    # the pre-change baseline - the deployed bytes are what the game actually ran.
    exps = experiments(root)
    previous_deploys = [d for d in ledger(root)
                        if d.get("kind") == "deploy"
                        and exps.get(d.get("exp"), {}).get("unknown") == exp["unknown"]]
    if previous_deploys:
        last = previous_deploys[-1]
        last_exp = exps.get(last["exp"], {})
        if last_exp.get("status") == "fail" and last.get("tree") == tree:
            if not (args.replicate and args.why):
                print(f"agentflow: {last['exp']} already ran this exact tree ({tree}) and failed. "
                      f"Re-running it unchanged needs --replicate --why \"<what this run adds>\".")
                return 1

    mod_dir = inst / "mods" / "NeuroDragon"
    plan = [
        (paths(root)["bin"] / "NeuroDragon.dll", mod_dir / "NeuroDragon.dll"),
        (paths(root)["bin"] / "NeuroDragon.pdb", mod_dir / "NeuroDragon.pdb"),
        (paths(root)["bin"] / "manifest.json", mod_dir / "manifest.json"),
    ]
    print(f"deploy {exp['id']} -> {mod_dir}")
    for src, dst in plan:
        print(f"  {src.name:18} {'ok' if src.is_file() else 'MISSING'}  -> {dst}")
    print(f"  lang/              {'ok' if (paths(root)["bin"] / 'lang').is_dir() else 'MISSING'}")
    if args.dry_run:
        print("dry run: nothing copied, nothing recorded")
        return 0

    if not mod_dir.is_dir():
        print(f"agentflow: {mod_dir} does not exist - refusing to create a deployment target")
        return 1
    for src, dst in plan:
        if src.is_file():
            shutil.copy2(src, dst)
    lang_src = paths(root)["bin"] / "lang"
    if lang_src.is_dir():
        shutil.copytree(lang_src, mod_dir / "lang", dirs_exist_ok=True)

    dll = mod_dir / "NeuroDragon.dll"
    digest = sha256_file(dll)
    append(root, {"kind": "deploy", "exp": exp["id"], "instance": str(inst), "tree": tree,
                  "head": head_sha(root),
                  "dll_sha256": digest, "dll_size": dll.stat().st_size,
                  "waived": [f["id"] for f in waived_findings], "why": args.why or ""})
    print(f"deployed {dll.name} sha256={digest[:16]} (recorded; `doctor` will notice any bypass)")
    return 0


def cmd_review(root: Path, args) -> int:
    con = contract(root)
    findings, waived = run_checks(root)
    errors = [f for f in findings if f["level"] == "error"]

    accepts = {}
    for event in ledger(root):
        if event.get("kind") == "accept":
            accepts[event["criterion"]] = event

    stale = []
    for cid, event in accepts.items():
        for entry in event.get("evidence", []):
            if not evidence_still_matches(entry):
                stale.append((cid, entry["path"]))

    print("agentflow review")
    print(f"  tree            {tree_digest(root)}")
    print(f"  contract stage  {con['stage']} - {STAGES.get(con['stage'], '?')}")
    print(f"  checks          {len(errors)} error(s), {sum(1 for f in findings if f.get('waived'))} waived")
    print_findings(findings)
    if stale:
        for cid, path in stale:
            print(f"  STALE EVIDENCE  {cid}: {path} changed after it was accepted")
    ok = not errors and not stale
    if args.record:
        append(root, {"kind": "review", "ok": ok, "tree": tree_digest(root),
                      "checks": [f["id"] for f in findings],
                      "evidence": {cid: [e["sha256"] for e in ev.get("evidence", [])]
                                   for cid, ev in accepts.items()}})
        print(f"  recorded        ok={ok}")
    return 0 if ok else 1


def cmd_claim(root: Path, args) -> int:
    con = contract(root)
    events = ledger(root)
    accepts = {e["criterion"]: e for e in events if e.get("kind") == "accept"}

    if args.final:
        needed = [c for c in con["criteria"] if c.get("kind") == "final"]
        label = "final"
        if con["stage"] < 6:
            print(f"agentflow: final claim needs stage 6, contract is at {con['stage']}")
            return 1
    else:
        stage = int(args.stage)
        if stage < 1 or stage > 6:
            print("agentflow: --stage must be 1..6")
            return 1
        needed = [c for c in con["criteria"] if c.get("kind", "stage") == "stage" and c.get("stage", 0) <= stage]
        label = f"stage {stage}"

    missing = [c["id"] for c in needed if c["id"] not in accepts]
    if missing:
        print(f"agentflow: cannot claim {label}; these criteria have no accepted evidence: {missing}")
        return 1
    stale = [c["id"] for c in needed
             if any(not evidence_still_matches(e) for e in accepts[c["id"]].get("evidence", []))]
    if stale:
        print(f"agentflow: cannot claim {label}; evidence changed since acceptance: {stale}")
        return 1

    reviews = [e for e in events if e.get("kind") == "review"]
    if not reviews:
        print("agentflow: no review recorded - `flow.py review --record` is the independent check")
        return 1
    last_review = reviews[-1]
    if not last_review.get("ok"):
        print("agentflow: the last review did not pass")
        return 1

    latest_accept = max(datetime.datetime.fromisoformat(accepts[c["id"]]["ts"]).timestamp() for c in needed)
    review_ts = datetime.datetime.fromisoformat(last_review["ts"]).timestamp()
    if review_ts < latest_accept:
        print(f"agentflow: cannot claim {label}; the review is older than the evidence "
              f"({last_review['ts'][:16]}). Run `flow.py review --record` again.")
        return 1

    if args.final:
        append(root, {"kind": "claim", "final": True,
                      "criteria": [c["id"] for c in needed], "review": last_review["ts"]})
        print("claimed: final objective met (all final criteria accepted and reviewed)")
        return 0

    stage = int(args.stage)
    if stage < con["stage"]:
        print(f"agentflow: contract is already at stage {con['stage']}; stages do not move backwards")
        return 1
    con["stage"] = stage
    con["updated"] = now()
    write_json(paths(root)["contract"], con)
    append(root, {"kind": "claim", "stage": stage, "criteria": [c["id"] for c in needed],
                  "review": last_review["ts"]})
    print(f"claimed stage {stage} - {STAGES[stage]}")
    return 0


def cmd_doctor(root: Path, args) -> int:
    problems = []
    con = contract(root)
    events = ledger(root)
    accepts = {e["criterion"]: e for e in events if e.get("kind") == "accept"}
    claims = [e for e in events if e.get("kind") == "claim"]

    # A contract that is only at its starting stage has nothing to have claimed yet;
    # past that, every stage the contract says it reached needs accepted evidence.
    if con["stage"] > 1 or claims:
        for item in con["criteria"]:
            if item.get("kind") == "final":
                continue
            if item.get("stage", 0) <= con["stage"] and item["id"] not in accepts:
                problems.append(f"stage {con['stage']} is claimed but {item['id']} has no accepted evidence")

    deployments = [e for e in events if e.get("kind") == "deploy"]
    inst = _resolve_instance(root, args)
    if inst and deployments:
        last = deployments[-1]
        dll = Path(last["instance"]) / "mods" / "NeuroDragon" / "NeuroDragon.dll"
        if dll.is_file():
            actual = sha256_file(dll)
            if actual != last["dll_sha256"]:
                problems.append(
                    f"the deployed DLL does not match the last recorded deploy ({actual[:12]} != "
                    f"{last['dll_sha256'][:12]}): something was copied without `flow.py deploy`"
                )
        else:
            problems.append(f"the last deploy targeted {last['instance']} but the DLL is gone")
    elif deployments:
        problems.append("deploys are recorded but no instance is configured; cannot verify them")

    artifact = paths(root)["bin"] / "NeuroDragon.dll"
    if deployments and artifact.is_file():
        deployed_at = datetime.datetime.fromisoformat(deployments[-1]["ts"]).timestamp()
        if artifact.stat().st_mtime > deployed_at:
            problems.append("bin/NeuroDragon.dll is newer than the last recorded deploy - "
                            "a build exists that was never installed through flow.py")

    findings, waived = run_checks(root)
    for item in findings:
        if item["level"] == "error":
            problems.append(f"gate error {item['id']}: {item['message']}")
    for waiver in waived:
        if waiver.get("expired"):
            problems.append(f"waiver expired: {waiver.get('match')}")

    limit_hours = float(config(root).get("stale_experiment_hours", 12))
    for exp in experiments(root).values():
        if exp.get("status") != "open":
            continue
        opened = experiment_opened(root, exp["id"]) or exp["ts"]
        age = (stamp() - datetime.datetime.fromisoformat(opened).timestamp()) / 3600
        if age > limit_hours:
            problems.append(f"{exp['id']} has been open for {age:.1f}h (limit {limit_hours}h)")

    # Warnings are advice, not a failed doctor: formality that blocks work gets
    # bypassed, and a bypassed gate is worse than a missing one.
    warnings = []
    for exp in experiments(root).values():
        if exp.get("status") != "open":
            continue
        if not exp.get("tier"):
            warnings.append(f"{exp['id']} predates tiers; it is treated as L2 (needs your hands)")
        if (exp.get("tier") or "L2") == "L2" and not exp.get("runbook"):
            warnings.append(f"{exp['id']} is L2 but has no runbook - `flow.py runbook` cannot hand it over")

    tree = tree_digest(root)
    verifies = [e for e in events if e.get("kind") == "verify" and e.get("tree") == tree]
    dirty = dirty_paths(root)
    if dirty and verifies and verifies[-1].get("ok"):
        hint = float(config(root).get("commit_hint_hours", 6))
        last_ts = max((e["ts"] for e in events), default=now())
        age_h = (stamp() - datetime.datetime.fromisoformat(last_ts).timestamp()) / 3600
        if age_h > hint:
            warnings.append(f"{len(dirty)} uncommitted file(s), this tree verified {age_h:.1f}h ago - "
                            f"`flow.py commit propose`")

    handoff = next((e for e in reversed(events) if e.get("kind") == "handoff"), None)
    if handoff and handoff.get("next"):
        after = [e for e in events if e.get("ts", "") > handoff["ts"]]
        idle_h = (stamp() - datetime.datetime.fromisoformat(handoff["ts"]).timestamp()) / 3600
        if not after and idle_h > 24:
            warnings.append(f"the last handoff said next={oneline(handoff['next'], 60)} "
                            f"but nothing happened for {idle_h:.0f}h - the session may have died")

    if problems:
        print("agentflow doctor: found problems")
        for p in problems:
            print(f"  - {p}")
    if warnings:
        print("agentflow doctor: warnings")
        for w in warnings:
            print(f"  - {w}")
    if problems:
        return 1
    print("agentflow doctor: clean" if not warnings else "agentflow doctor: no problems")
    return 0




# --------------------------------------------------------------------------- #
# commit proposals
# --------------------------------------------------------------------------- #

PROPOSAL = ".commit-proposal.json"

# The commit is where history becomes auditable, so the subject has to name
# something a reviewer can check.  These words name nothing: the three commits
# before 2026-10-26 were "feat: enhance agent workflow", "feat: complete m6b0",
# "feat: get all necessary input in VV" - 2102 lines under a verb with no object.
SUBJECT_BANNED = re.compile(
    r"\b(update[sd]?|updating|improve[sd]?|improving|enhance[sd]?|enhancing|"
    r"misc|various|stuff|changes?|tweaks?|cleanup|fix(?:es)?\s+bugs?|wip|tmp|temp)\b",
    re.IGNORECASE,
)


def _dirty_status(root: Path) -> list[tuple[str, str]]:
    out = []
    for line in git(root, "status", "--porcelain", "-uall").splitlines():
        if len(line) < 4:
            continue
        code, rel = line[:2].strip(), line[3:]
        if " -> " in rel:                       # rename: the destination is the file
            rel = rel.split(" -> ", 1)[1]
        out.append((code, rel.strip().strip('"')))
    return out


def _infer_type(files: list[str]) -> str:
    if any(f.startswith("src/") for f in files):
        return "exp"
    if any(f.startswith("scripts/agentflow") or f in ("xmake.lua", "AGENTS.md") for f in files):
        return "flow"
    return "docs"


def _infer_scope(files: list[str]) -> str:
    subs = set()
    for f in files:
        parts = f.split("/")
        if len(parts) >= 4 and parts[0] == "src" and parts[1] == "neurodragon":
            subs.add(parts[2])
        elif len(parts) == 3 and parts[0] == "src" and parts[1] == "neurodragon":
            subs.add(Path(f).stem)
    if len(subs) == 1:
        return subs.pop().lower()
    if any(f.startswith("scripts/agentflow") for f in files):
        return "agentflow"
    return ""


def _proposal_files(root: Path, args) -> list[tuple[str, str]]:
    excludes = [e.rstrip("/") for e in (args.exclude or [])]
    files = []
    for code, rel in _dirty_status(root):
        if rel.endswith(PROPOSAL) or rel.endswith(".launch-lease.json"):
            continue
        if rel.startswith("bin/") or rel.startswith(".dsh/"):
            continue
        if any(rel == ex or rel.startswith(ex + "/") for ex in excludes):
            continue
        files.append((code, rel))
    return files


def _size_notes(root: Path, files: list[str]) -> list[str]:
    notes = []
    for rel in files:
        path = root / rel
        if not path.is_file():
            continue
        size = path.stat().st_size
        if size > 2 << 20:
            notes.append(f"{rel} is {size / 1048576:.1f} MB")
        elif size > 512 << 10 and path.suffix.lower() in {".log", ".json", ".jsonl", ".txt", ".md"}:
            notes.append(f"{rel} is {size // 1024} KB")
    return notes


def _commit_message(root: Path, header: str, args, tree: str, notes: list[str]) -> str:
    lines = [header, ""]
    if args.why:
        lines += [oneline(args.why, 200), ""]
    if args.exp:
        exp = experiments(root).get(args.exp) or {}
        lines.append(f"Exp: {args.exp}")
        if exp.get("hypothesis"):
            lines.append(f"Hypothesis: {oneline(exp['hypothesis'], 110)}")
        for entry in exp.get("evidence", []):
            rel = entry.get("path", "")
            try:
                rel = Path(rel).resolve().relative_to(root.resolve()).as_posix()
            except ValueError:
                pass
            lines.append(f"Evidence: {rel} sha256:{entry['sha256'][:12]}")
        if exp.get("result"):
            lines.append(f"Result: {oneline(exp['result'], 90)}")
    verifies = [e for e in ledger(root) if e.get("kind") == "verify" and e.get("tree") == tree]
    if verifies:
        waived = sum(1 for f in verifies[-1].get("findings", []) if f.get("waived"))
        lines.append(f"Verify: ok, {waived} waived (tree {tree})")
    if args.exp:
        deploys = [e for e in ledger(root) if e.get("kind") == "deploy" and e.get("exp") == args.exp]
        if deploys:
            last = deploys[-1]
            lines.append(f"Deploy: {last['ts'][5:16].replace('T', ' ')} dll {last['dll_sha256'][:8]}")
    for note in notes:
        lines.append(f"Note: {note}")
    return "\n".join(lines).rstrip() + "\n"


def cmd_commit(root: Path, args) -> int:
    if args.action == "apply":
        return _commit_apply(root, args)
    return _commit_propose(root, args)


def _commit_propose(root: Path, args) -> int:
    if not args.subject:
        print("agentflow: --subject is required (one imperative line, <= 72 chars)")
        return 1
    subject = " ".join(args.subject.split())
    if len(subject) > 72:
        print(f"agentflow: the subject is {len(subject)} chars; keep it <= 72")
        return 1
    if SUBJECT_BANNED.search(subject):
        print(f"agentflow: {subject!r} names no checkable behaviour. Say what changed, "
              f"not that something was enhanced (AGENTS.md §9).")
        return 1
    if subject.endswith("."):
        print("agentflow: no trailing period in the subject")
        return 1

    dirty = _proposal_files(root, args)
    files = [rel for _, rel in dirty]
    if not files:
        print("agentflow: nothing to commit")
        return 1

    actual_type = _infer_type(files)
    type_ = args.type or actual_type
    if type_ != actual_type:
        print(f"agentflow: these files are a {actual_type!r} change, not {type_!r} "
              f"(src -> exp, scripts/agentflow+xmake+AGENTS -> flow, docs -> docs)")
        return 1

    exps = experiments(root)
    open_exps = [e for e in exps.values() if e.get("status") == "open"]
    if type_ == "exp":
        if not args.exp:
            print("agentflow: an exp commit must name its experiment (--exp EXP-xxxx)")
            return 1
        exp = exps.get(args.exp)
        if not exp:
            print(f"agentflow: no experiment {args.exp!r}")
            return 1
        if exp.get("status") == "open":
            print(f"agentflow: {args.exp} is still open - a commit records a result, "
                  f"not a checkpoint. Close it first, or pass --checkpoint.")
            return 1
        others = [e["id"] for e in open_exps if e["id"] != args.exp]
        if others and not args.checkpoint:
            print(f"agentflow: {', '.join(others)} still open; this commit would freeze "
                  f"half of another experiment. Close it, or pass --checkpoint.")
            return 1

    tree = tree_digest(root)
    verifies = [e for e in ledger(root) if e.get("kind") == "verify" and e.get("tree") == tree]
    if not verifies or not verifies[-1].get("ok"):
        print("agentflow: no passing verify for this exact content - running it now")
        if _gate(root, record=True):
            print("agentflow: the gate failed; nothing to propose")
            return 1

    # The verify above may have appended to the ledger, which is part of the
    # commit; recompute so the record of the check lands with the change it
    # justifies instead of drifting into the next commit.
    dirty = _proposal_files(root, args)
    files = [rel for _, rel in dirty]
    notes = _size_notes(root, files)
    scope = (args.scope if args.scope is not None else _infer_scope(files)).lower()
    if args.checkpoint:
        header = f"wip({args.exp or 'checkpoint'}): {subject}"
    elif scope:
        header = f"{type_}({scope}): {subject}"
    else:
        header = f"{type_}: {subject}"
    message = _commit_message(root, header, args, tree, notes)

    proposal = {"ts": now(), "tree": tree, "head": head_sha(root), "type": type_,
                "scope": scope, "subject": subject, "checkpoint": bool(args.checkpoint),
                "exp": args.exp or "", "files": files, "message": message, "notes": notes}
    write_json(root / "docs" / "agent" / PROPOSAL, proposal)

    print(f"proposal  {header}")
    print(f"  tree    {tree}  ({len(files)} file(s))")
    for code, rel in dirty:
        print(f"    {code:2} {rel}")
    leftovers = [rel for _, rel in _dirty_status(root) if rel not in set(files)]
    src_left = [rel for rel in leftovers if rel.startswith("src/")]
    if src_left:
        print(f"  warn    {len(src_left)} src/ file(s) stay uncommitted but were part of the "
              f"verified tree; this commit's own snapshot does not contain them")
    for note in notes:
        print(f"  note    {note}")
    print("\n----- commit message -----")
    print(message, end="")
    print("--------------------------")
    print('approve: 用户回 ok 后运行 `flow.py commit apply --approved "<原话>"`')
    return 0


def _commit_apply(root: Path, args) -> int:
    path = root / "docs" / "agent" / PROPOSAL
    proposal = read_json(path, None)
    if not isinstance(proposal, dict):
        print("agentflow: no open proposal - run `flow.py commit propose ...` first")
        return 1
    if not args.approved:
        print('agentflow: applying a proposal needs --approved "<what the user said>"')
        return 1
    tree = tree_digest(root)
    if tree != proposal.get("tree"):
        print(f"agentflow: the content changed since the proposal "
              f"({proposal.get('tree')} -> {tree}). Propose again; the approval was for the old bytes.")
        return 1
    files = list(proposal.get("files") or [])
    if not files:
        print("agentflow: the proposal has no files")
        return 1
    added = subprocess.run(["git", "add", "-A", "--", *files], cwd=root, capture_output=True,
                           text=True, encoding="utf-8", errors="replace")
    if added.returncode != 0:
        print("agentflow: git add failed: " + (added.stderr.strip() or added.stdout.strip()))
        return 1
    commit = subprocess.run(["git", "commit", "-F", "-", "--", *files], cwd=root,
                            input=proposal.get("message", ""), capture_output=True,
                            text=True, encoding="utf-8", errors="replace")
    if commit.returncode != 0:
        print("agentflow: git commit failed (proposal kept):")
        print((commit.stderr or commit.stdout).strip())
        return 1
    sha = git(root, "rev-parse", "HEAD").strip()
    append(root, {"kind": "commit", "sha": sha, "tree": tree, "type": proposal.get("type"),
                  "scope": proposal.get("scope"), "exp": proposal.get("exp", ""),
                  "files": len(files), "approved": args.approved,
                  "checkpoint": bool(proposal.get("checkpoint"))})
    path.unlink(missing_ok=True)
    print(f"committed {sha[:8]}  {proposal.get('message', '').splitlines()[0]}")
    print(f"  {len(files)} file(s), tree {tree}")
    return 0


# --------------------------------------------------------------------------- #
# handoff / brief
# --------------------------------------------------------------------------- #


def _last(events: list[dict], kind: str, **match) -> dict | None:
    for event in reversed(events):
        if event.get("kind") != kind:
            continue
        if any(event.get(key) != value for key, value in match.items()):
            continue
        return event
    return None


def _open_exps(root: Path) -> list[dict]:
    exps = [e for e in experiments(root).values() if e.get("status") == "open"]
    exps.sort(key=lambda e: e["ts"])
    return exps


def _session_since(events: list[dict]) -> str:
    handoff = _last(events, "handoff")
    if handoff:
        return handoff["ts"]
    return (datetime.datetime.now().astimezone()
            - datetime.timedelta(hours=8)).isoformat(timespec="milliseconds")


def cmd_handoff(root: Path, args) -> int:
    if args.action == "show":
        event = _last(ledger(root), "handoff")
        if not event:
            print("agentflow: no handoff recorded")
            return 0
        print(json.dumps(event, ensure_ascii=False, indent=2))
        return 0
    if not args.next:
        print('agentflow: handoff write needs --next "<the single next action>"')
        return 1
    event = append(root, {
        "kind": "handoff", "next": args.next, "awaiting": args.awaiting or [],
        "note": args.note or "", "tree": tree_digest(root), "head": head_sha(root),
        "open": [e["id"] for e in _open_exps(root)],
    })
    print(f"handoff written: next={oneline(event['next'], 80)}")
    return 0


def cmd_brief(root: Path, args) -> int:
    con = contract(root)
    events = ledger(root)
    tree = tree_digest(root)
    verifies = [e for e in events if e.get("kind") == "verify" and e.get("tree") == tree]
    if verifies and verifies[-1].get("ok"):
        vstate = f"verify ok {verifies[-1]['ts'][11:16]}"
    elif verifies:
        vstate = "verify FAILED"
    else:
        vstate = "no verify for this tree"
    print(f"goal   : {oneline(con['goal'], 88)}")
    print(f"stage  : {con['stage']} - {STAGES.get(con['stage'], '?')} · tree {tree} · {vstate}")

    deploy = _last(events, "deploy")
    if deploy:
        print(f"build  : deployed {deploy['ts'][11:16]} {deploy['exp']} dll {deploy['dll_sha256'][:8]}")
    else:
        print("build  : no deploy recorded")

    open_exps = _open_exps(root)
    if not open_exps:
        print("open   : none")
    for exp in open_exps[:3]:
        opened = experiment_opened(root, exp["id"]) or exp["ts"]
        minutes = (stamp() - datetime.datetime.fromisoformat(opened).timestamp()) / 60
        tier = exp.get("tier") or "L2?"
        tail = ""
        if tier == "L2":
            step = (exp.get("runbook") or [None])[0]
            tail = f"  → 等你跑: {oneline(step, 40)}" if step else "  → 等你跑 (缺 runbook)"
        print(f"open   : {exp['id']} {exp['unknown']} {tier} attempt {exp.get('attempt')} "
              f"{minutes:.0f}m{tail}")

    closed = sorted((e for e in experiments(root).values() if e.get("status") in ("pass", "fail")),
                    key=lambda e: e["ts"])
    for exp in closed[-2:]:
        print(f"last   : {exp['id']} {exp['status']}「{oneline(exp.get('result', ''), 40)}」")
    for exp in [e for e in closed if e["status"] == "fail"][-2:]:
        print(f"avoid  : {exp['id']} {oneline(exp.get('conclusion', ''), 76)}")

    handoff = _last(events, "handoff")
    if handoff:
        after = [e for e in events if e.get("ts", "") > handoff["ts"]]
        suffix = "" if after else "  (未动)"
        print(f"handoff: next={oneline(handoff.get('next', ''), 76)}{suffix}")
        if handoff.get("awaiting"):
            print(f"         awaiting={'; '.join(handoff['awaiting'])}")
        if handoff.get("note"):
            print(f"         note={oneline(handoff['note'], 88)}")
    else:
        print("handoff: none recorded")

    dirty = dirty_paths(root)
    if dirty:
        owned: dict[str, str] = {}
        for exp in closed[-3:]:
            for rel in exp.get("dirty", []) or []:
                owned.setdefault(rel, exp["id"])
        groups: dict[str, int] = {}
        unowned = []
        for rel in dirty:
            if rel in owned:
                key = owned[rel]
            elif rel.startswith("docs/"):
                key = "docs"
            elif rel.startswith("scripts/agentflow") or rel in ("xmake.lua", "AGENTS.md"):
                key = "flow"
            else:
                key = "未归属"
                unowned.append(rel)
            groups[key] = groups.get(key, 0) + 1
        summary = " / ".join(f"{k} {v}" for k, v in sorted(groups.items(), key=lambda kv: -kv[1]))
        print(f"dirty  : {len(dirty)} 文件 → {summary}")
        for rel in unowned[:4]:
            print(f"         未归属: {rel}")

    since = _session_since(events)
    deploys = [e for e in events if e.get("kind") == "deploy" and e.get("ts", "") > since]
    smokes = [e for e in events if e.get("kind") == "smoke" and e.get("ts", "") > since]
    print(f"spend  : 部署 {len(deploys)} 次 · L1 smoke {len(smokes)} 次（自 {since[11:16]} 起）")
    return 0


def cmd_runbook(root: Path, args) -> int:
    events = ledger(root)
    open_l2 = [e for e in _open_exps(root) if (e.get("tier") or "L2") == "L2"]
    if not open_l2:
        print("agentflow runbook: 没有等你的 L2 实验")
        return 0
    total = 0
    for exp in open_l2:
        deploy = _last(events, "deploy", exp=exp["id"])
        seconds = int(exp.get("expect_s") or 120)
        total += seconds
        build = (f"{deploy['ts'][5:16].replace('T', ' ')} dll {deploy['dll_sha256'][:8]}"
                 if deploy else "未部署")
        print(f"\n{exp['id']}  {exp['unknown']}  构建 {build}")
        print(f"  未知项: {oneline(exp.get('hypothesis', ''), 96)}")
        steps = exp.get("runbook") or []
        if steps:
            print("  步骤: " + " → ".join(f"{i + 1}) {s}" for i, s in enumerate(steps)))
            print("        不要做: 别点别的按钮、别改设置；偏离了就说明")
        else:
            print("  步骤: 缺 runbook —— 先补，别凭记忆让人跑")
        print(f"  预期: {oneline(exp.get('expect_pass', ''), 100)}")
        print(f"  失败意味着: {oneline(exp.get('expect_fail', ''), 100)}（由我读日志判定）")
        print("  出问题: 什么都别做，说一声；退回 = 关游戏重开")
        print(f"  耗时: 约 {seconds} 秒")
    print(f"\n合计约 {total // 60} 分 {total % 60} 秒；回我一个结论词即可")
    return 0


# --------------------------------------------------------------------------- #
# L1 smoke: launch, assert on the log, close the experiment
# --------------------------------------------------------------------------- #

SMOKE_DEFAULTS = {"name": "smoke", "tier": "L1", "timeout_s": 180,
                  "requires": [], "forbids": [], "min_occurrences": {}}


def _load_profile(root: Path, name: str) -> tuple[dict | None, str]:
    path = Path(name)
    if not path.is_file():
        path = root / "scripts" / "agentflow" / "smoke" / f"{name}.json"
    if not path.is_file():
        return None, f"no smoke profile {name!r}"
    profile = read_json(path, None)
    if not isinstance(profile, dict) or not profile.get("requires"):
        return None, f"{path} needs a non-empty `requires`"
    merged = dict(SMOKE_DEFAULTS)
    merged.update(profile)
    merged["name"] = profile.get("name") or path.stem
    merged["_path"] = str(path)
    return merged, ""


def _eval_profile(profile: dict, text: str) -> tuple[bool, list[str], list[str]]:
    text = text.replace("\x00", "")
    # `window_after` scopes the assertions to what was logged *after* the state change
    # under test. Without it an absence assertion also counts the lines from before the
    # change (taau running normally, then switching it off, is a pass - but the prefix
    # would fail a plain `forbids`). The window is the text after the last marker; an
    # unopened window has nothing to assert on, so it reads as "requires missing".
    window = profile.get("window_after")
    if window:
        matches = list(re.finditer(window, text, re.IGNORECASE | re.MULTILINE))
        text = text[matches[-1].end():] if matches else ""
    missing = []
    for pattern in profile["requires"]:
        if not re.search(pattern, text, re.IGNORECASE | re.MULTILINE):
            missing.append(pattern)
    for pattern, need in (profile.get("min_occurrences") or {}).items():
        got = len(re.findall(pattern, text, re.IGNORECASE | re.MULTILINE))
        if got < int(need):
            missing.append(f"{pattern} x{got}/{need}")
    forbidden = [p for p in profile["forbids"] if re.search(p, text, re.IGNORECASE | re.MULTILINE)]
    return (not missing and not forbidden), missing, forbidden


def _read_since(path: Path, offset: int, limit: int = 8 << 20) -> tuple[str, int]:
    if not path.is_file():
        return "", offset
    size = path.stat().st_size
    if size < offset:                 # the log rotated: everything is new
        offset = 0
    with path.open("rb") as f:
        f.seek(offset)
        data = f.read(limit)
    return data.decode("utf-8", "replace"), size


def _smoke_stop(ok: bool, forbidden: bool, first_ok: float | None, now: float, dwell: float) -> bool:
    """Whether a smoke run has read enough to decide.

    A forbidden pattern ends the run immediately. A profile with ``dwell_s`` keeps
    reading for that long *after the requires first matched*: without it an absence
    assertion ("taau never logged") passes on the first frames, before the renderer
    has logged anything at all. 2026-10-26 EXP-0016 did exactly that - the pass came
    17 s in and the first ``[vv] taau`` line landed a second later.
    """
    if forbidden:
        return True
    if not ok:
        return False
    if first_ok is None:
        return False
    return dwell <= 0 or now - first_ok >= dwell


def _game_running() -> bool:
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Minecraft.Windows.exe", "/NH"],
                             capture_output=True, text=True, encoding="utf-8",
                             errors="replace", timeout=20)
    except (OSError, subprocess.SubprocessError):
        return False
    return "minecraft.windows.exe" in out.stdout.lower()


def cmd_smoke(root: Path, args) -> int:
    profile, error = _load_profile(root, args.profile)
    if profile is None:
        print(f"agentflow: {error}")
        return 1

    if args.replay:
        path = Path(args.replay)
        if not path.is_absolute():
            path = root / path
        if not path.is_file():
            print(f"agentflow: {path} does not exist")
            return 1
        text = path.read_text(encoding="utf-8", errors="replace")
        if args.tail:
            text = text[-int(args.tail):]
        ok, missing, forbidden = _eval_profile(profile, text)
        print(f"agentflow smoke replay {profile['name']} on {path.name}: "
              f"{'PASS' if ok else 'FAIL'}")
        for item in missing:
            print(f"  missing   {item}")
        for item in forbidden:
            print(f"  forbidden {item}")
        print("  (replay only proves the profile's regexes; it says nothing about this build)")
        return 0 if ok else 1

    if not args.exp:
        print("agentflow: smoke closes an experiment; pass --exp EXP-xxxx")
        return 1
    exp = experiments(root).get(args.exp)
    if not exp:
        print(f"agentflow: no experiment {args.exp!r}")
        return 1
    if exp.get("status") != "open":
        print(f"agentflow: {args.exp} is closed ({exp.get('status')})")
        return 1
    tier = exp.get("tier") or "L2"
    if tier == "L0":
        print(f"agentflow: {args.exp} is L0 (offline); run `flow.py verify` instead")
        return 1

    inst = _resolve_instance(root, args)
    if inst is None:
        print("agentflow: no instance configured")
        return 1
    log = inst / "logs" / "latest.log"
    exe = Path(args.exe) if args.exe else inst / "Minecraft.Windows.exe"
    if not args.no_launch and not exe.is_file():
        print(f"agentflow: {exe} does not exist")
        return 1
    if not log.is_file():
        print(f"agentflow: {log} does not exist - has the game ever run here?")
        return 1

    events = ledger(root)
    budget = int(config(root).get("l1_max_per_session", 4))
    since = _session_since(events)
    used = len([e for e in events if e.get("kind") == "smoke" and e.get("ts", "") > since])
    if used >= budget and not args.user_authorization:
        print(f"agentflow: {used} automatic runs already this session (budget {budget}). "
              f"A repeat needs --user-authorization \"<who/what>\".")
        return 1

    lease_path = root / "docs" / "agent" / ".launch-lease.json"
    lease = read_json(lease_path, {}) if lease_path.is_file() else {}
    if isinstance(lease, dict) and lease.get("ts"):
        age = stamp() - datetime.datetime.fromisoformat(lease["ts"]).timestamp()
        if age < int(profile["timeout_s"]) + 300:
            print(f"agentflow: another run holds the launch lease ({lease.get('exp')} "
                  f"since {lease['ts'][11:16]}); wait or delete {lease_path}")
            return 1

    running = _game_running()
    if running and not args.interrupt:
        print("agentflow: Minecraft.Windows.exe is already running. I will not restart it "
              "without --interrupt (that ends your session).")
        return 1
    if running and args.interrupt:
        print("agentflow: --interrupt given; stopping the running client")
        subprocess.run(["taskkill", "/IM", "Minecraft.Windows.exe", "/F"], capture_output=True)
        time.sleep(3)

    offset = log.stat().st_size
    started = now()
    write_json(lease_path, {"exp": args.exp, "profile": profile["name"], "ts": started,
                            "offset": offset})
    try:
        if args.no_launch:
            print(f"agentflow: waiting for a launch you start; asserting on bytes after {offset}")
        else:
            print(f"agentflow: launching {exe.name}; asserting on bytes after {offset}")
            # The client inherits the parent's stdout otherwise, and then any pipe
            # (`| tail`, a job wrapper) stays open until the *game* exits - the
            # 2026-10-26 EXP-0016 run looked hung for four minutes for this reason.
            subprocess.Popen(
                [str(exe)], cwd=str(inst), close_fds=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
        if tier == "L2":
            print("agentflow: L2 profile - 现在按 runbook 操作（进世界/走动/回菜单）")

        dwell = float(profile.get("dwell_s") or 0)
        deadline = time.time() + float(args.timeout or profile["timeout_s"])
        text = ""
        first_ok: float | None = None
        while time.time() < deadline:
            text, _ = _read_since(log, offset)
            ok, missing, forbidden = _eval_profile(profile, text)
            if _smoke_stop(ok, forbidden, first_ok, time.time(), dwell):
                break
            if ok and first_ok is None:
                first_ok = time.time()
            time.sleep(1.0)
        text, size_now = _read_since(log, offset)
        ok, missing, forbidden = _eval_profile(profile, text)
    finally:
        lease_path.unlink(missing_ok=True)

    touched = any(re.search(p, text.replace("\x00", ""), re.IGNORECASE | re.MULTILINE)
                  for p in profile["requires"])
    aborted = (not ok) and (not touched) and (not forbidden) and tier == "L2"

    evidence_dir = paths(root)["evidence"]
    evidence_dir.mkdir(parents=True, exist_ok=True)
    evidence = evidence_dir / f"smoke-{profile['name']}-{args.exp}.log"
    deploy = _last(events, "deploy", exp=args.exp)
    elapsed = int(stamp() - datetime.datetime.fromisoformat(started).timestamp())
    header = [
        f"# agentflow smoke {profile['name']} on {args.exp}",
        f"# started {started}  verdict={'ABORTED' if aborted else ('PASS' if ok else 'FAIL')} "
        f"elapsed={elapsed}s",
        f"# tree {tree_digest(root)}  head {head_sha(root)[:8]}",
        f"# dll {deploy['dll_sha256'][:16] if deploy else 'not deployed for this exp'}",
        f"# profile {profile['_path']}",
        f"# requires={profile['requires']}",
        f"# missing={missing}  forbidden={forbidden}",
        f"# log {log} offset {offset} -> {size_now}",
        "",
    ]
    evidence.write_text("\n".join(header) + text[-2_000_000:], encoding="utf-8")

    append(root, {"kind": "smoke", "exp": args.exp, "profile": profile["name"],
                  "tree": tree_digest(root), "head": head_sha(root), "ok": ok,
                  "aborted": aborted, "missing": missing, "forbidden": forbidden,
                  "offset": offset, "elapsed_s": elapsed, "evidence": str(evidence)})

    print(f"agentflow smoke {profile['name']}: "
          f"{'ABORTED' if aborted else ('PASS' if ok else 'FAIL')}")
    for item in missing:
        print(f"  missing   {item}")
    for item in forbidden:
        print(f"  forbidden {item}")
    print(f"  evidence  {evidence}")

    if aborted:
        print(f"  触发没有发生（L2 步骤未完成/没进世界）-> {args.exp} 保持打开，不记失败")
        return 1

    result = f"smoke {profile['name']}: {'pass' if ok else 'fail'}"
    if ok:
        conclusion = "自动 smoke：全部断言命中"
        nxt = ""
    else:
        parts = []
        if missing:
            parts.append("缺 " + "; ".join(missing))
        if forbidden:
            parts.append("命中禁止 " + "; ".join(forbidden))
        conclusion = "自动 smoke 失败：" + ("；".join(parts) or "超时")
        nxt = ("读 evidence 尾部增量；先判断缺失断言是触发没发生（步骤没做/没进世界），"
               "还是功能真的没有")
    append(root, {"kind": "exp", "id": args.exp, "status": "pass" if ok else "fail",
                  "result": result, "conclusion": conclusion, "next": nxt,
                  "evidence": [evidence_entry(evidence)], "dirty": dirty_paths(root)[:60],
                  "head": head_sha(root), "auto": "smoke"})
    print(f"  {args.exp} closed {'pass' if ok else 'fail'}; the client is left running")
    return 0 if ok else 1


# --------------------------------------------------------------------------- #
# NGX key table
# --------------------------------------------------------------------------- #

def cmd_keys(root: Path, args) -> int:
    dlls = [Path(p) for p in (args.dll or [])]
    if not dlls:
        cfg = config(root)
        base = Path(cfg.get("instance", "")) if cfg.get("instance") else None
        if base and base.is_dir():
            dlls = sorted(p for p in base.glob("nvngx_dlss*.dll"))
    blobs = {p.name: p.read_bytes() for p in dlls if p.is_file()}
    if not blobs:
        print("agentflow: no NGX dll found; pass --dll <path>")
        return 1

    key_file = root / "scripts" / "agentflow" / "ngx_keys.txt"
    names = [line.strip() for line in key_file.read_text(encoding="utf-8").splitlines()
             if line.strip() and not line.startswith("#")]

    if args.add:
        if not any(args.add.encode() in blob for blob in blobs.values()):
            print(f"agentflow: {args.add!r} does not occur in any provided DLL - refusing to add it")
            return 1
        if args.add in names:
            print(f"{args.add} already listed")
            return 0
        with key_file.open("a", encoding="utf-8") as f:
            f.write(args.add + "\n")
        print(f"added {args.add}")
        return 0

    missing = [n for n in names if not any(n.encode() in blob for blob in blobs.values())]
    print(f"agentflow keys: {len(names)} names checked against {', '.join(blobs)}")
    if missing:
        print("  not found in the runtime (the list is stale or a name was added by hand):")
        for n in missing:
            print(f"    - {n}")
        return 1
    print("  all names occur in the runtime bytes")
    return 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(prog="flow.py", description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(HERE.parent.parent), help="workspace root")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("init", help="create the contract skeleton, ledger and config").set_defaults(fn=cmd_init)
    sub.add_parser("status", help="goal, stage, criteria, experiments").set_defaults(fn=cmd_status)

    con = sub.add_parser("contract", help="show or change the task contract")
    con.add_argument("action", choices=["show", "set"])
    con.add_argument("--goal")
    con.add_argument("--out-of-scope")
    con.add_argument("--criteria-file")
    con.add_argument("--user-authorization")
    con.set_defaults(fn=cmd_contract)

    acc = sub.add_parser("accept", help="attach evidence to a criterion")
    acc.add_argument("criterion")
    acc.add_argument("--evidence", nargs="+", default=[])
    acc.add_argument("--note")
    acc.set_defaults(fn=cmd_accept)

    exp = sub.add_parser("exp", help="experiment ledger")
    exp_sub = exp.add_subparsers(dest="action", required=True)
    new = exp_sub.add_parser("new")
    new.add_argument("--unknown", required=True)
    new.add_argument("--hypothesis", required=True)
    new.add_argument("--change", required=True)
    new.add_argument("--expect-pass", required=True)
    new.add_argument("--expect-fail", required=True)
    new.add_argument("--evidence-plan", required=True)
    new.add_argument("--postmortem")
    new.add_argument("--from-evidence")
    new.add_argument("--user-authorization")
    new.add_argument("--tier", choices=["L0", "L1", "L2"], default="L2",
                     help="L0 offline, L1 log assertions only, L2 needs a human (default)")
    new.add_argument("--runbook", help="3-5 steps for the human, separated by |")
    new.add_argument("--expect-s", type=int, help="how long the human run should take")
    new.set_defaults(fn=cmd_exp_new)
    close = exp_sub.add_parser("close")
    close.add_argument("id")
    close.add_argument("--pass", dest="pass_result", action="store_true")
    close.add_argument("--fail", dest="pass_result", action="store_false")
    close.set_defaults(pass_result=False)
    close.add_argument("--result", required=True)
    close.add_argument("--evidence", nargs="+", default=[])
    close.add_argument("--conclusion", required=True)
    close.add_argument("--next")
    close.set_defaults(fn=cmd_exp_close)
    upd = exp_sub.add_parser("update", help="fill in tier/runbook after the experiment exists")
    upd.add_argument("id")
    upd.add_argument("--tier", choices=["L0", "L1", "L2"])
    upd.add_argument("--runbook", help="3-5 steps for the human, separated by |")
    upd.add_argument("--expect-s", type=int)
    upd.set_defaults(fn=cmd_exp_update)
    can = exp_sub.add_parser("cancel", help="close an attempt that never ran (not a failure)")
    can.add_argument("id")
    can.add_argument("--why", required=True, help="why the run is not happening (kept verbatim)")
    can.set_defaults(fn=cmd_exp_cancel)

    gate = sub.add_parser("gate", help="pre-build checks (wired into xmake)")
    gate.add_argument("what", choices=["build"])
    gate.set_defaults(fn=cmd_gate)

    sub.add_parser("verify", help="run the checks and record them for this tree").set_defaults(fn=cmd_verify)

    sub.add_parser("brief", help="the one thing to read at session start").set_defaults(fn=cmd_brief)
    sub.add_parser("runbook", help="all open L2 experiments as one human session").set_defaults(fn=cmd_runbook)

    hd = sub.add_parser("handoff", help="record the one line the next session needs")
    hd.add_argument("action", choices=["write", "show"])
    hd.add_argument("--next")
    hd.add_argument("--awaiting", nargs="*", default=[])
    hd.add_argument("--note")
    hd.set_defaults(fn=cmd_handoff)

    cm = sub.add_parser("commit", help="propose a commit, then apply it after the user says ok")
    cm.add_argument("action", choices=["propose", "apply"])
    cm.add_argument("--type", choices=["exp", "flow", "docs", "revert"])
    cm.add_argument("--scope")
    cm.add_argument("--subject")
    cm.add_argument("--why")
    cm.add_argument("--exp")
    cm.add_argument("--exclude", nargs="*", default=[])
    cm.add_argument("--checkpoint", action="store_true")
    cm.add_argument("--approved")
    cm.set_defaults(fn=cmd_commit)

    sm = sub.add_parser("smoke", help="launch the client, assert on the log, close the experiment")
    sm.add_argument("--profile", required=True)
    sm.add_argument("--exp")
    sm.add_argument("--replay", help="score an existing log against the profile (no launch)")
    sm.add_argument("--tail", type=int, help="with --replay: only the last N bytes of the log")
    sm.add_argument("--no-launch", action="store_true")
    sm.add_argument("--interrupt", action="store_true")
    sm.add_argument("--timeout", type=float)
    sm.add_argument("--exe")
    sm.add_argument("--instance")
    sm.add_argument("--user-authorization")
    sm.set_defaults(fn=cmd_smoke)

    dep = sub.add_parser("deploy", help="copy the build into the game, with preconditions")
    dep.add_argument("--exp", required=True)
    dep.add_argument("--instance")
    dep.add_argument("--dry-run", action="store_true")
    dep.add_argument("--replicate", action="store_true")
    dep.add_argument("--why")
    dep.add_argument("--allow-waived")
    dep.set_defaults(fn=cmd_deploy)

    rev = sub.add_parser("review", help="re-run the checks and re-hash the evidence")
    rev.add_argument("--record", action="store_true")
    rev.set_defaults(fn=cmd_review)

    claim = sub.add_parser("claim", help="advance a stage or claim the final objective")
    claim.add_argument("--stage", type=int)
    claim.add_argument("--final", action="store_true")
    claim.set_defaults(fn=cmd_claim)

    doc = sub.add_parser("doctor", help="find bypassed or stale state")
    doc.add_argument("--instance")
    doc.set_defaults(fn=cmd_doctor)

    keys = sub.add_parser("keys", help="verify/update ngx_keys.txt against the runtime")
    keys.add_argument("--dll", action="append")
    keys.add_argument("--add")
    keys.set_defaults(fn=cmd_keys)

    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    if args.command == "claim" and not args.final and args.stage is None:
        parser.error("claim needs --stage N or --final")
    return args.fn(root, args)


if __name__ == "__main__":
    sys.exit(main())
