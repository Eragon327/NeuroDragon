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
import shutil
import subprocess
import sys
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
    try:
        out = subprocess.run(
            ["git", *args], cwd=root, capture_output=True, text=True, timeout=30
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    return out.stdout if out.returncode == 0 else ""


def paths(root: Path) -> dict[str, Path]:
    agent = root / "docs" / "agent"
    return {
        "agent": agent,
        "contract": agent / "contract.json",
        "ledger": agent / "ledger.jsonl",
        "evidence": agent / "evidence",
        "config": root / "scripts" / "agentflow" / "config.json",
        "waivers": root / "scripts" / "agentflow" / "waivers.json",
        "bin": root / "bin" / "NeuroDragon",
    }


def config(root: Path) -> dict:
    cfg = dict(DEFAULT_CONFIG)
    cfg.update(read_json(paths(root)["config"], {}))
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


def _tracked_sources(root: Path) -> list[Path]:
    return _files_under(root, DIGEST_DIRS)


def tree_digest(root: Path) -> str:
    h = hashlib.sha256()
    head = git(root, "rev-parse", "HEAD").strip()
    h.update(head.encode())
    diff = git(root, "diff", "HEAD", "--", *DIGEST_DIRS, "xmake.lua", "manifest.json", "tooth.json")
    h.update(diff.encode("utf-8", "replace"))
    untracked = git(root, "ls-files", "--others", "--exclude-standard", "--", *DIGEST_DIRS)
    for rel in sorted(x for x in untracked.splitlines() if x.strip()):
        p = root / rel
        if p.is_file():
            h.update(rel.encode())
            h.update(p.read_bytes())
    if not head and not diff:
        # not a git checkout (test fixture): hash the files themselves
        for p in sorted(_tracked_sources(root)):
            h.update(str(p.relative_to(root)).encode())
            h.update(p.read_bytes())
    return h.hexdigest()[:16]


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
        attempt = int(previous.get("attempt", 0)) + 1
        if previous.get("status") == "open":
            print(f"agentflow: {previous['id']} is still open on {args.unknown!r}; close it with "
                  f"its evidence before opening another one.")
            return 1
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
        "unknown": args.unknown, "hypothesis": args.hypothesis, "change": args.change,
        "expect_pass": args.expect_pass, "expect_fail": args.expect_fail,
        "evidence_plan": args.evidence_plan, "tree": tree_digest(root),
        "postmortem": args.postmortem or "",
        "authorization": args.user_authorization or "",
    }
    if args.from_evidence:
        path = Path(args.from_evidence)
        if not path.is_absolute():
            path = root / path
        event["from_evidence"] = evidence_entry(path)
    append(root, event)
    print(f"{exp_id} open  unknown={args.unknown} attempt={attempt} tree={event['tree']}")
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
    append(root, {"kind": "exp", "id": args.id, "status": status, "result": args.result,
                  "conclusion": args.conclusion, "next": args.next or "", "evidence": entries})
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
        append(root, {"kind": "verify", "tree": tree_digest(root), "ok": ok,
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
        print("agentflow: no instance configured (scripts/agentflow/config.json or --instance)")
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
        age = (stamp() - datetime.datetime.fromisoformat(exp["ts"]).timestamp()) / 3600
        if age > limit_hours:
            problems.append(f"{exp['id']} has been open for {age:.1f}h (limit {limit_hours}h)")

    if problems:
        print("agentflow doctor: found problems")
        for p in problems:
            print(f"  - {p}")
        return 1
    print("agentflow doctor: clean")
    return 0



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

    gate = sub.add_parser("gate", help="pre-build checks (wired into xmake)")
    gate.add_argument("what", choices=["build"])
    gate.set_defaults(fn=cmd_gate)

    sub.add_parser("verify", help="run the checks and record them for this tree").set_defaults(fn=cmd_verify)

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
