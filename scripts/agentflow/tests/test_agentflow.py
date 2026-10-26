#!/usr/bin/env python3
"""Adversarial tests for the agentflow gates.

Each test drives the real CLI in a throw-away workspace and asserts the exit code
and the message a naughty agent would get.  Behaviours under test, in the order the
task asked for them:

  1. calling an intermediate module success "the objective"
  2. repeating a real-machine experiment without new evidence
  3. skipping the pre-flight checks before touching the game
  4. changing a verified parameter without re-verifying the contract it belongs to
  5. recording an assumption as a fact
  6. widening the scope without authorisation
  7. claiming a pass that no execution produced
  8. guessing after a failure instead of reading the previous evidence

Run:  python -m unittest discover -s scripts/agentflow/tests -v
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
FLOW = REPO / "scripts" / "agentflow" / "flow.py"

CONTRACT = {
    "goal": "replace TAAU with our own DLSS in VV",
    "criteria": [
        {"id": "S1", "stage": 1, "kind": "stage", "text": "contract agreed"},
        {"id": "S2", "stage": 2, "kind": "stage", "text": "gate and build pass"},
        {"id": "S3", "stage": 3, "kind": "stage", "text": "evaluate returns 1"},
        {"id": "S4", "stage": 4, "kind": "stage", "text": "DLSS on screen"},
        {"id": "S5", "stage": 5, "kind": "stage", "text": "TAAU skipped, not painted over"},
        {"id": "S6", "stage": 6, "kind": "stage", "text": "regressions checked"},
        {"id": "F1", "stage": 6, "kind": "final", "text": "taau draw gone"},
    ],
    "out_of_scope": ["FG", "RR"],
    "stage": 1,
    "updated": "2026-10-25T00:00:00+08:00",
}


class Workspace:
    def __init__(self, tmp: str):
        self.root = Path(tmp)
        for rel in (
            "scripts/agentflow/checks.py",
            "scripts/agentflow/ngx_keys.txt",
            "scripts/agentflow/ngx_required.json",
            "scripts/agentflow/waivers.json",
        ):
            dst = self.root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(REPO / rel, dst)
        shutil.copytree(REPO / "src" / "neurodragon" / "vv", self.root / "src" / "neurodragon" / "vv")
        smoke = REPO / "scripts" / "agentflow" / "smoke"
        if smoke.is_dir():
            shutil.copytree(smoke, self.root / "scripts" / "agentflow" / "smoke")
        (self.root / "docs" / "agent" / "evidence").mkdir(parents=True)
        (self.root / "docs" / "agent" / "contract.json").write_text(
            json.dumps(CONTRACT, ensure_ascii=False), encoding="utf-8"
        )
        (self.root / "docs" / "agent" / "ledger.jsonl").write_text("", encoding="utf-8")
        (self.root / "bin" / "NeuroDragon").mkdir(parents=True)
        (self.root / "bin" / "NeuroDragon" / "NeuroDragon.dll").write_bytes(b"new-build")
        (self.root / "bin" / "NeuroDragon" / "manifest.json").write_text("{}", encoding="utf-8")
        (self.root / "inst" / "mods" / "NeuroDragon").mkdir(parents=True)
        (self.root / "scripts" / "agentflow" / "config.json").write_text(
            json.dumps({"instance": str(self.root / "inst"), "max_attempts_per_unknown": 3,
                        "stale_experiment_hours": 12}), encoding="utf-8")

    def run(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(FLOW), "--root", str(self.root), *args],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )

    def git_init(self) -> None:
        """Commit tests need a real repository; the fixture itself is not one."""
        def git(*args: str) -> None:
            subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True)
        git("init", "-q")
        git("config", "user.email", "agentflow@test.local")
        git("config", "user.name", "agentflow test")
        git("add", "-A")
        git("commit", "-qm", "fixture")

    # ---------------------------------------------------------------- helpers
    def evidence(self, name: str, text: str = "log") -> Path:
        path = self.root / "docs" / "agent" / "evidence" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def accept_all_stage(self, upto: int = 6) -> None:
        for n in range(1, upto + 1):
            ev = self.evidence(f"s{n}.log", f"evidence for S{n}")
            out = self.run("accept", f"S{n}", "--evidence", str(ev))
            assert out.returncode == 0, out.stdout + out.stderr

    def verify(self) -> subprocess.CompletedProcess:
        return self.run("verify")

    def open_exp(self, unknown: str, attempt_note: str = "first try", **extra: str) -> subprocess.CompletedProcess:
        args = [
            "exp", "new",
            "--unknown", unknown,
            "--hypothesis", "one unknown only",
            "--change", "touch one thing",
            "--expect-pass", "ret=1",
            "--expect-fail", "ret=0xbad00005",
            "--evidence-plan", "latest.log",
        ]
        for key, value in extra.items():
            args.extend([f"--{key.replace('_', '-')}", value])
        return self.run(*args)

    def exp_id(self) -> str:
        events = [json.loads(x) for x in
                  (self.root / "docs" / "agent" / "ledger.jsonl").read_text(encoding="utf-8").splitlines() if x.strip()]
        return [e for e in events if e.get("kind") == "exp"][-1]["id"]


class GateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="agentflow-")
        self.ws = Workspace(self.tmp)

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    # 3. skipping the pre-flight checks -------------------------------------
    def test_deploy_without_verify_is_refused(self):
        self.assertEqual(self.ws.open_exp("U-ngx").returncode, 0)
        out = self.ws.run("deploy", "--exp", self.ws.exp_id(), "--dry-run")
        self.assertEqual(out.returncode, 1)
        self.assertIn("no `flow.py verify` recorded", out.stdout)

    def test_broken_source_fails_the_gate_and_blocks_deploy(self):
        (self.ws.root / "src/neurodragon/vv/_probe.cpp").write_text(
            'void f(void* p) { paramU32(p, "NotARealKey", 1); }', encoding="utf-8")
        self.assertEqual(self.ws.verify().returncode, 1)
        self.ws.open_exp("U-ngx")
        out = self.ws.run("deploy", "--exp", self.ws.exp_id(), "--dry-run")
        self.assertEqual(out.returncode, 1)

    def test_verified_tree_can_deploy(self):
        self.assertEqual(self.ws.verify().returncode, 0, self.ws.verify().stdout)
        self.ws.open_exp("U-ngx")
        out = self.ws.run("deploy", "--exp", self.ws.exp_id(), "--dry-run", "--allow-waived", "test")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)

    def test_waived_deviation_requires_explicit_consent_to_deploy(self):
        # Self-contained: the repo's own waivers.json is empty now that the Jitter
        # defect is fixed, so a test that leans on it tests yesterday's tree.
        (self.ws.root / "src/neurodragon/vv/_probe.cpp").write_text(
            'void f(void* p) { paramU32(p, "NotARealKey", 1); }', encoding="utf-8")
        (self.ws.root / "scripts/agentflow/waivers.json").write_text(json.dumps([{
            "check": "ngx.params", "match": "NotARealKey", "reason": "known deviation",
            "expires": "2099-01-01", "owner": "user"}]), encoding="utf-8")
        dll = self.ws.root / "bin/NeuroDragon/NeuroDragon.dll"
        os.utime(dll, (time.time() + 60, time.time() + 60))
        out = self.ws.verify()
        self.assertEqual(out.returncode, 0, out.stdout)
        self.ws.open_exp("U-ngx")
        out = self.ws.run("deploy", "--exp", self.ws.exp_id(), "--dry-run")
        self.assertEqual(out.returncode, 1, "the build carries a waived deviation, so deploy must ask")
        self.assertIn("--allow-waived", out.stdout)
        out = self.ws.run("deploy", "--exp", self.ws.exp_id(), "--dry-run", "--allow-waived", "testing")
        self.assertEqual(out.returncode, 0, out.stdout)

    def test_expired_waiver_turns_back_into_an_error(self):
        (self.ws.root / "src/neurodragon/vv/_probe.cpp").write_text(
            'void f(void* p) { paramU32(p, "NotARealKey", 1); }', encoding="utf-8")
        (self.ws.root / "scripts/agentflow/waivers.json").write_text(json.dumps([{
            "check": "ngx.params", "match": "NotARealKey", "reason": "old",
            "expires": "2000-01-01", "owner": "user"}]), encoding="utf-8")
        out = self.ws.verify()
        self.assertEqual(out.returncode, 1)
        self.assertIn("NotARealKey", out.stdout)

    def test_shader_compile_error_is_caught_offline(self):
        (self.ws.root / "src/neurodragon/vv/VvBroken.cpp").write_text(
            'char const* kBrokenShader = R"hlsl(\n'
            'RWTexture2D<float4> D : register(u0);\n'
            '[numthreads(8,8,1)] void main(uint3 i : SV_DispatchThreadID) { D[i.xy] = nope; }\n'
            ')hlsl";\n', encoding="utf-8")
        out = self.ws.verify()
        self.assertEqual(out.returncode, 1)
        self.assertIn("shader.compile", out.stdout)

    # 1. intermediate success claimed as the objective ----------------------
    def test_final_claim_needs_final_criteria(self):
        self.ws.accept_all_stage(6)
        self.assertEqual(self.ws.run("review", "--record").returncode, 0)
        self.assertEqual(self.ws.run("claim", "--stage", "6").returncode, 0)
        out = self.ws.run("claim", "--final")
        self.assertEqual(out.returncode, 1)
        self.assertIn("['F1']", out.stdout)
        ev = self.ws.evidence("f1.log", "taau count 0")
        self.assertEqual(self.ws.run("accept", "F1", "--evidence", str(ev)).returncode, 0)
        self.assertEqual(self.ws.run("review", "--record").returncode, 0)
        self.assertEqual(self.ws.run("claim", "--final").returncode, 0)

    def test_stage_cannot_jump_without_accepted_evidence(self):
        out = self.ws.run("claim", "--stage", "3")
        self.assertEqual(out.returncode, 1)
        self.assertIn("no accepted evidence", out.stdout)

    # 7. claim a pass no execution produced ---------------------------------
    def test_claim_without_review_is_refused(self):
        self.ws.accept_all_stage(2)
        out = self.ws.run("claim", "--stage", "2")
        self.assertEqual(out.returncode, 1)
        self.assertIn("no review recorded", out.stdout)

    def test_review_must_be_newer_than_the_evidence(self):
        self.ws.accept_all_stage(2)
        self.assertEqual(self.ws.run("review", "--record").returncode, 0)
        time.sleep(1.1)
        ev = self.ws.evidence("s2b.log", "later evidence")
        self.assertEqual(self.ws.run("accept", "S2", "--evidence", str(ev)).returncode, 0)
        out = self.ws.run("claim", "--stage", "2")
        self.assertEqual(out.returncode, 1)
        self.assertIn("older than the evidence", out.stdout)

    # 4. verified contract invalidated by a later edit ----------------------
    def test_changed_evidence_makes_the_acceptance_stale(self):
        ev = self.ws.evidence("s1.log", "original")
        self.assertEqual(self.ws.run("accept", "S1", "--evidence", str(ev)).returncode, 0)
        self.assertEqual(self.ws.run("review", "--record").returncode, 0)
        ev.write_text("changed after acceptance", encoding="utf-8")
        out = self.ws.run("claim", "--stage", "1")
        self.assertEqual(out.returncode, 1)
        self.assertIn("evidence changed", out.stdout)

    # 2/8. repeat after failure without new evidence ------------------------
    def test_repeat_without_new_evidence_is_refused(self):
        self.assertEqual(self.ws.open_exp("U-repeat").returncode, 0)
        first = self.ws.exp_id()
        old = self.ws.evidence("run1.log", "first run log")
        self.assertEqual(self.ws.run("exp", "close", first, "--fail", "--result", "005",
                                     "--evidence", str(old), "--conclusion", "invalid parameter",
                                     "--next", "check names").returncode, 0)
        out = self.ws.open_exp("U-repeat")
        self.assertEqual(out.returncode, 1)
        self.assertIn("Record what its evidence showed", out.stdout)

        out = self.ws.open_exp("U-repeat", postmortem="log says jitter missing")
        self.assertEqual(out.returncode, 1)
        self.assertIn("--from-evidence", out.stdout)

        out = self.ws.open_exp("U-repeat", postmortem="log says jitter missing",
                               from_evidence=str(old))
        self.assertEqual(out.returncode, 1)
        self.assertIn("older than", out.stdout)

        time.sleep(0.05)
        fresh = self.ws.evidence("run2.log", "ngx feature log")
        os.utime(fresh, (time.time() + 5, time.time() + 5))
        out = self.ws.open_exp("U-repeat", postmortem="log says jitter missing",
                               from_evidence=str(fresh))
        self.assertEqual(out.returncode, 0, out.stdout)

    def test_second_open_experiment_on_the_same_unknown_is_refused(self):
        self.assertEqual(self.ws.open_exp("U-open").returncode, 0)
        out = self.ws.open_exp("U-open")
        self.assertEqual(out.returncode, 1)
        self.assertIn("still open", out.stdout)

    def test_same_tree_after_failure_needs_replicate_justification(self):
        self.assertEqual(self.ws.verify().returncode, 0)
        self.ws.open_exp("U-tree")
        first = self.ws.exp_id()
        out = self.ws.run("deploy", "--exp", first, "--allow-waived", "known defect")
        self.assertEqual(out.returncode, 0, out.stdout)
        log = self.ws.evidence("run1.log", "failed run")
        self.assertEqual(self.ws.run("exp", "close", first, "--fail", "--result", "005",
                                     "--evidence", str(log), "--conclusion", "no",
                                     "--next", "fix names").returncode, 0)

        os.utime(log, (time.time() + 5, time.time() + 5))
        self.assertEqual(self.ws.open_exp("U-tree", postmortem="names were wrong",
                                          from_evidence=str(log)).returncode, 0)
        self.assertEqual(self.ws.verify().returncode, 0)
        out = self.ws.run("deploy", "--exp", self.ws.exp_id(), "--allow-waived", "known defect")
        self.assertEqual(out.returncode, 1)
        self.assertIn("--replicate", out.stdout)
        out = self.ws.run("deploy", "--exp", self.ws.exp_id(), "--allow-waived", "known defect",
                          "--replicate", "--why", "same build, longer sample")
        self.assertEqual(out.returncode, 0, out.stdout)

    def test_attempt_budget_requires_authorisation(self):
        for i in range(3):
            log = self.ws.evidence(f"b{i}.log", f"run {i}")
            os.utime(log, (time.time() + 10 + i, time.time() + 10 + i))
            extra = {}
            if i:
                extra = {"postmortem": "analysed", "from_evidence": str(log)}
            out = self.ws.open_exp(f"U-budget", **extra) if i == 0 else self.ws.open_exp("U-budget", **extra)
            self.assertEqual(out.returncode, 0, out.stdout)
            self.assertEqual(self.ws.run("exp", "close", self.ws.exp_id(), "--fail", "--result", "x",
                                         "--evidence", str(log), "--conclusion", "x",
                                         "--next", "y").returncode, 0)
        log = self.ws.evidence("b4.log", "run 4")
        os.utime(log, (time.time() + 30, time.time() + 30))
        out = self.ws.open_exp("U-budget", postmortem="analysed", from_evidence=str(log))
        self.assertEqual(out.returncode, 1)
        self.assertIn("exceeds the budget", out.stdout)
        out = self.ws.open_exp("U-budget", postmortem="analysed", from_evidence=str(log),
                               user_authorization="user asked for one more run")
        self.assertEqual(out.returncode, 0, out.stdout)

    # 5. assumption recorded as fact ----------------------------------------
    def test_experiment_cannot_close_without_evidence(self):
        self.ws.open_exp("U-fact")
        out = self.ws.run("exp", "close", self.ws.exp_id(), "--pass", "--result", "worked",
                          "--conclusion", "the pipeline is fine")
        self.assertEqual(out.returncode, 1)
        self.assertIn("--evidence", out.stdout)

    def test_failed_experiment_must_state_the_next_hypothesis(self):
        self.ws.open_exp("U-fact2")
        log = self.ws.evidence("x.log", "log")
        out = self.ws.run("exp", "close", self.ws.exp_id(), "--fail", "--result", "005",
                          "--evidence", str(log), "--conclusion", "unknown")
        self.assertEqual(out.returncode, 1)
        self.assertIn("--next", out.stdout)

    def test_accept_requires_existing_non_empty_evidence(self):
        out = self.ws.run("accept", "S1", "--evidence", str(self.ws.root / "nope.log"))
        self.assertEqual(out.returncode, 1)
        empty = self.ws.evidence("empty.log", "")
        self.assertEqual(self.ws.run("accept", "S1", "--evidence", str(empty)).returncode, 1)

    # 6. scope expansion without authorisation ------------------------------
    def test_goal_change_requires_user_authorization(self):
        out = self.ws.run("contract", "set", "--goal", "also do frame generation")
        self.assertEqual(out.returncode, 1)
        self.assertIn("--user-authorization", out.stdout)
        out = self.ws.run("contract", "set", "--goal", "also do frame generation",
                          "--user-authorization", "user asked in chat 2026-10-25")
        self.assertEqual(out.returncode, 0)
        events = [json.loads(x) for x in
                  (self.ws.root / "docs/agent/ledger.jsonl").read_text(encoding="utf-8").splitlines() if x.strip()]
        self.assertTrue(any(e.get("kind") == "contract" and "user asked" in e.get("authorization", "") for e in events))

    # doctor: bypassed deploy -----------------------------------------------
    def test_doctor_notices_a_deploy_that_did_not_go_through_flow(self):
        self.assertEqual(self.ws.verify().returncode, 0)
        self.ws.open_exp("U-doctor")
        self.assertEqual(self.ws.run("deploy", "--exp", self.ws.exp_id(), "--allow-waived", "x").returncode, 0)
        self.assertEqual(self.ws.run("doctor").returncode, 0, self.ws.run("doctor").stdout)
        (self.ws.root / "inst/mods/NeuroDragon/NeuroDragon.dll").write_bytes(b"hand-copied")
        out = self.ws.run("doctor")
        self.assertEqual(out.returncode, 1)
        self.assertIn("something was copied without", out.stdout)


class CheckUnitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="agentflow-checks-")
        self.ws = Workspace(self.tmp)
        sys.path.insert(0, str(self.ws.root / "scripts" / "agentflow"))

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_ngx_key_list_matches_the_runtime_bytes(self):
        lines = (self.ws.root / "scripts/agentflow/ngx_keys.txt").read_text(encoding="utf-8").splitlines()
        names = {line.strip() for line in lines if line.strip() and not line.startswith("#")}
        self.assertIn("Jitter.Offset.X", names)
        self.assertIn("MV.Scale.X", names)
        self.assertNotIn("JitterOffsetX", names)

    def test_required_parameter_gap_is_reported_with_evidence(self):
        # The check must still fire when a future edit drops a required key; the
        # repo's own evaluate now writes them, so the fixture has to omit one.
        (self.ws.root / "src/neurodragon/vv/_probe.cpp").write_text(
            'static void evaluateSr(void* ctx) {\n'
            '    gEvaluate(ctx, 1);\n'
            '}\n', encoding="utf-8")
        import checks  # noqa: PLC0415
        ids = [f["id"] for f in checks.check_ngx_params(self.ws.root) if f["level"] == "error"]
        self.assertIn("ngx.missing-required", ids)

    def test_unknown_parameter_name_is_reported(self):
        (self.ws.root / "src/neurodragon/vv/VvBad.cpp").write_text(
            'void f(void* p) { paramU32(p, "JitterOffsetX", 1); }', encoding="utf-8")
        import checks  # noqa: PLC0415
        msgs = [f["message"] for f in checks.check_ngx_params(self.ws.root)]
        self.assertTrue(any("JitterOffsetX" in m for m in msgs))

    def test_hand_edited_key_list_is_caught_by_the_runtime_bytes(self):
        # An agent could "fix" an unknown name by appending it to ngx_keys.txt.
        # When the instance DLLs are present, the bytes are the oracle instead.
        (self.ws.root / "inst" / "nvngx_dlss.dll").write_bytes(
            b"Color" + bytes([0]) + b"Output" + bytes([0]) + b"Jitter.Offset.X")
        key_file = self.ws.root / "scripts/agentflow/ngx_keys.txt"
        with key_file.open("a", encoding="utf-8") as handle:
            handle.write("Bogus.Name" + "\n")
        (self.ws.root / "src/neurodragon/vv/VvBogus.cpp").write_text(
            'void f(void* p) { paramU32(p, "Bogus.Name", 1); }', encoding="utf-8")
        import checks  # noqa: PLC0415
        ids = [f["id"] for f in checks.check_ngx_params(self.ws.root)]
        self.assertIn("ngx.name-not-in-runtime", ids)

    def test_shaders_compile(self):
        import checks  # noqa: PLC0415
        self.assertEqual([f for f in checks.check_shaders(self.ws.root) if f["level"] == "error"], [])


class FlowV2Tests(unittest.TestCase):
    """commit / brief / runbook / smoke - the handoff layer added 2026-10-26."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="agentflow-v2-")
        self.ws = Workspace(self.tmp)

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _ledger(self) -> list[dict]:
        raw = (self.ws.root / "docs/agent/ledger.jsonl").read_text(encoding="utf-8")
        return [json.loads(line) for line in raw.splitlines() if line.strip()]

    # commit ---------------------------------------------------------------
    def test_commit_subject_must_name_something_checkable(self):
        self.ws.git_init()
        (self.ws.root / "docs/notes.md").write_text("x", encoding="utf-8")
        out = self.ws.run("commit", "propose", "--subject", "enhance the workflow")
        self.assertEqual(out.returncode, 1)
        self.assertIn("names no checkable behaviour", out.stdout)

    def test_commit_refuses_while_the_named_experiment_is_open(self):
        self.ws.git_init()
        self.ws.open_exp("U-commit")
        (self.ws.root / "src/neurodragon/vv/_probe.cpp").write_text("void f(){}", encoding="utf-8")
        out = self.ws.run("commit", "propose", "--subject", "gate sr on the level state",
                          "--exp", self.ws.exp_id())
        self.assertEqual(out.returncode, 1)
        self.assertIn("is still open", out.stdout)

    def test_commit_apply_refuses_a_changed_tree(self):
        self.ws.git_init()
        (self.ws.root / "docs/notes.md").write_text("one", encoding="utf-8")
        self.assertEqual(
            self.ws.run("commit", "propose", "--subject", "record the probe notes").returncode, 0)
        (self.ws.root / "src/neurodragon/vv/_probe.cpp").write_text("void g(){}", encoding="utf-8")
        out = self.ws.run("commit", "apply", "--approved", "ok")
        self.assertEqual(out.returncode, 1)
        self.assertIn("content changed", out.stdout)

    def test_commit_apply_commits_with_trailers_and_records_it(self):
        self.ws.git_init()
        os.makedirs(self.ws.root / "docs/specs", exist_ok=True)
        (self.ws.root / "docs/specs/vv.md").write_text("colour path", encoding="utf-8")
        out = self.ws.run("commit", "propose", "--subject", "document the vv colour path")
        self.assertEqual(out.returncode, 0, out.stdout)
        self.assertIn("Verify: ok", out.stdout)
        out = self.ws.run("commit", "apply", "--approved", "ok in test")
        self.assertEqual(out.returncode, 0, out.stdout)
        message = subprocess.run(["git", "log", "-1", "--format=%B"], cwd=self.ws.root,
                                 capture_output=True, text=True, encoding="utf-8").stdout
        self.assertIn("docs: document the vv colour path", message)
        self.assertIn("Verify: ok", message)
        self.assertTrue(any(e.get("kind") == "commit" and e.get("approved") == "ok in test"
                            for e in self._ledger()))
        out = self.ws.run("doctor")
        self.assertEqual(out.returncode, 0, out.stdout)

    def test_verify_stays_valid_across_a_commit(self):
        # tree_digest used to hash HEAD + diff, so the act of committing
        # invalidated the verify the commit was approved on.
        self.ws.git_init()
        (self.ws.root / "docs/notes.md").write_text("one", encoding="utf-8")
        self.assertEqual(
            self.ws.run("commit", "propose", "--subject", "record the first note").returncode, 0)
        self.assertEqual(self.ws.run("commit", "apply", "--approved", "ok").returncode, 0)
        before = len([e for e in self._ledger() if e.get("kind") == "verify"])
        (self.ws.root / "docs/notes.md").write_text("two", encoding="utf-8")
        self.assertEqual(
            self.ws.run("commit", "propose", "--subject", "record the second note").returncode, 0)
        after = len([e for e in self._ledger() if e.get("kind") == "verify"])
        self.assertEqual(before, after, "committing must not force another verify")

    # handoff / brief ------------------------------------------------------
    def test_brief_shows_the_handoff_and_the_l2_waiting_on_a_human(self):
        self.ws.open_exp("U-mv", tier="L2", runbook="进世界|走动|回菜单")
        self.assertEqual(self.ws.run("handoff", "write", "--next", "读 dump 定 VP 方向").returncode, 0)
        out = self.ws.run("brief")
        self.assertEqual(out.returncode, 0, out.stdout)
        self.assertIn("读 dump 定 VP 方向", out.stdout)
        self.assertIn("L2", out.stdout)
        self.assertIn("等你跑", out.stdout)
        self.assertIn("进世界", out.stdout)

    def test_brief_marks_an_unmoved_handoff(self):
        self.assertEqual(self.ws.run("handoff", "write", "--next", "读最后一帧").returncode, 0)
        out = self.ws.run("brief")
        self.assertIn("(未动)", out.stdout)

    def test_runbook_hands_over_all_open_l2_as_one_session(self):
        self.ws.open_exp("U-one", tier="L2", runbook="进世界|看 HUD", expect_s="90")
        self.ws.open_exp("U-two", tier="L2", runbook="回菜单")
        out = self.ws.run("runbook")
        self.assertEqual(out.returncode, 0, out.stdout)
        self.assertIn("EXP-0001", out.stdout)
        self.assertIn("进世界", out.stdout)
        self.assertIn("合计约", out.stdout)

    def test_doctor_warns_about_an_l2_experiment_without_a_runbook(self):
        self.ws.open_exp("U-bare")
        out = self.ws.run("doctor")
        self.assertEqual(out.returncode, 0, out.stdout)
        self.assertIn("no runbook", out.stdout)

    # smoke ----------------------------------------------------------------
    def test_smoke_replay_scores_a_log_against_a_profile(self):
        good = self.ws.evidence(
            "boot-ok.log",
            "[vv] session (install): shim=0x1\n"
            "[vv] level listeners join=true exit=true\n")
        out = self.ws.run("smoke", "--profile", "boot", "--replay", str(good))
        self.assertEqual(out.returncode, 0, out.stdout)

        partial = self.ws.evidence("boot-partial.log", "[vv] session (install): shim=0x1\n")
        out = self.ws.run("smoke", "--profile", "boot", "--replay", str(partial))
        self.assertEqual(out.returncode, 1)
        self.assertIn("level listeners", out.stdout)

        crashed = self.ws.evidence(
            "boot-crash.log",
            "[vv] session (install): shim=0x1\n"
            "[vv] level listeners join=true exit=true\n"
            "Unhandled exception at 0x1234\n")
        out = self.ws.run("smoke", "--profile", "boot", "--replay", str(crashed))
        self.assertEqual(out.returncode, 1)
        self.assertIn("forbidden", out.stdout)

    def test_smoke_refuses_to_restart_a_running_client(self):
        self.ws.open_exp("U-smoke", tier="L1")
        # No instance exe / log in the fixture, so the guard must trip first or
        # the command must fail without touching anything.
        out = self.ws.run("smoke", "--profile", "boot", "--exp", self.ws.exp_id())
        self.assertEqual(out.returncode, 1)
        self.assertIn("does not exist", out.stdout)


if __name__ == "__main__":
    unittest.main()
