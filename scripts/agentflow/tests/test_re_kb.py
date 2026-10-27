#!/usr/bin/env python3
"""Adversarial tests for the RE knowledge base gates.

Every test drives the real CLI in a throw-away workspace, with a **synthetic PE**
as the module, so nothing here needs the 296 MiB client, an .i64, or IDA.  The
synthetic binary carries three deliberate features, and most tests exist because
one of them can fool a naive implementation:

  1. a 16-byte unique marker   -> a signature can be derived offline and verified
  2. a 1 KB block of one 5-byte motif repeated -> "must be unique" has teeth
  3. addresses in four spaces  -> mac / win / other-module / heap pointer must not
                                  be conflated (a 26.51 fact filed as a macOS one
                                  is how an agent re-analyses the wrong binary)

Run:  python -m unittest discover -s scripts/agentflow/tests -v
"""

from __future__ import annotations

import json
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

AGENTFLOW = Path(__file__).resolve().parents[1]
REAL_CLI = AGENTFLOW / "re_kb.py"

IMAGEBASE = 0x140000000
TEXT_VA = 0x1000
TEXT_RAWSIZE = 0x1000
TEXT_RAWPTR = 0x400
MARKER_AT = 0x100          # text offset -> rva 0x1100
BLOCK_AT = 0x800           # text offset -> rva 0x1800
BLOCK_LEN = 1000
MOTIF = bytes.fromhex("48895C2408")
MARKER = bytes(range(0xE0, 0xF0))


def build_pe(path: Path) -> None:
    """A valid-enough PE32+ with one .text section.  Only what re_kb.py reads."""
    text = bytearray(random.Random(1234).randbytes(TEXT_RAWSIZE))
    text[MARKER_AT:MARKER_AT + len(MARKER)] = MARKER
    text[BLOCK_AT:BLOCK_AT + BLOCK_LEN] = MOTIF * (BLOCK_LEN // len(MOTIF))

    head = bytearray(TEXT_RAWPTR)
    head[0:2] = b"MZ"
    struct.pack_into("<I", head, 0x3C, 0x80)          # e_lfanew
    head[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", head, 0x84, 0x8664)        # machine = x86-64
    struct.pack_into("<H", head, 0x86, 1)             # NumberOfSections
    struct.pack_into("<H", head, 0x94, 0xF0)          # SizeOfOptionalHeader
    opt = 0x98
    struct.pack_into("<H", head, opt, 0x20B)          # PE32+
    struct.pack_into("<Q", head, opt + 24, IMAGEBASE)
    struct.pack_into("<I", head, opt + 56, 0x4000)    # SizeOfImage
    sec = opt + 0xF0
    head[sec:sec + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", head, sec + 8, TEXT_RAWSIZE, TEXT_VA,
                     TEXT_RAWSIZE, TEXT_RAWPTR)
    path.write_bytes(bytes(head) + bytes(text))


def marker_sig() -> str:
    return " ".join(f"{b:02X}" for b in MARKER)


class Workspace:
    """A throw-away checkout that re_kb.py will consider its own root."""

    def __init__(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="nd-re-test-"))
        agentflow = self.tmp / "scripts" / "agentflow"
        agentflow.mkdir(parents=True)
        shutil.copy2(REAL_CLI, agentflow / "re_kb.py")
        inst = self.tmp / "instance"
        inst.mkdir()
        build_pe(inst / "Minecraft.Windows.exe")
        (agentflow / "config.local.json").write_text(
            json.dumps({"instance": str(inst)}), encoding="utf-8")
        for sub in ("requests", "builds", "canon"):
            (self.tmp / "docs" / "agent" / "re" / sub).mkdir(parents=True)
        (self.tmp / "docs" / "research").mkdir(parents=True)

    def run(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(self.tmp / "scripts" / "agentflow" / "re_kb.py"), *args],
            cwd=str(self.tmp), capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=120,
        )

    def kb(self) -> Path:
        return self.tmp / "docs" / "agent" / "re"

    def exe(self) -> Path:
        return self.tmp / "instance" / "Minecraft.Windows.exe"

    def write_doc(self, rel: str, text: str) -> None:
        path = self.tmp / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def canon(self) -> dict:
        out = {}
        path = self.kb() / "canon" / "funcs.jsonl"
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                row = json.loads(line)
                out[row["key"]] = row
        return out

    def cleanup(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)


class ReTests(unittest.TestCase):
    def setUp(self) -> None:
        self.ws = Workspace()

    def tearDown(self) -> None:
        self.ws.cleanup()

    # -- baseline ---------------------------------------------------------
    def test_status_on_fresh_workspace(self) -> None:
        r = self.ws.run("status")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("canon     : empty", r.stdout)
        self.assertIn("Minecraft.Windows.exe", r.stdout)

    # -- signature derivation (offline rva -> bytes -> unique) ------------
    def test_sig_from_rva_finds_the_marker(self) -> None:
        r = self.ws.run("sig", "--rva", hex(TEXT_VA + MARKER_AT))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(marker_sig(), r.stdout)
        self.assertIn("unique at", r.stdout)

    def test_sig_reports_a_repeated_block_instead_of_lying(self) -> None:
        r = self.ws.run("sig", "--rva", hex(TEXT_VA + BLOCK_AT), "--max-len", "96")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("not unique", r.stdout)
        self.assertIn("anchors", r.stdout)

    def test_sig_outside_any_section_is_refused(self) -> None:
        r = self.ws.run("sig", "--rva", "0x9000")
        self.assertEqual(r.returncode, 1)
        self.assertIn("not inside any section", r.stdout)

    # -- request queue gates ---------------------------------------------
    def test_req_refuses_an_ambiguous_signature(self) -> None:
        r = self.ws.run("req", "add", "--key", "block_fn",
                        "--sig", "48 89 5C 24 08", "--why", "test")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("ambiguous", r.stdout)

    def test_req_accepts_a_unique_signature_and_counts_it(self) -> None:
        r = self.ws.run("req", "add", "--key", "marker_fn",
                        "--sig", marker_sig(), "--why", "read it")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("offline check: 1 hit", r.stdout)
        ls = self.ws.run("req", "ls")
        self.assertIn("marker_fn", ls.stdout)
        self.assertIn("pending", ls.stdout)
        self.assertEqual(self.ws.run("req", "check").returncode, 0)

    def test_req_refuses_one_key_for_two_functions(self) -> None:
        self.ws.run("req", "add", "--key", "marker_fn", "--sig", marker_sig(),
                    "--why", "a")
        r = self.ws.run("req", "add", "--key", "marker_fn", "--rva", "0x1100",
                        "--why", "b")
        self.assertEqual(r.returncode, 1)
        self.assertIn("already requested", r.stdout)

    def test_req_refuses_two_keys_for_one_signature(self) -> None:
        self.ws.run("req", "add", "--key", "marker_fn", "--sig", marker_sig(),
                    "--why", "a")
        r = self.ws.run("req", "add", "--key", "marker_fn_2", "--sig", marker_sig(),
                        "--why", "b")
        self.assertEqual(r.returncode, 1)
        self.assertIn("reuse that key", r.stdout)

    def test_req_rename_moves_the_request_without_editing_the_file(self) -> None:
        self.ws.run("req", "add", "--key", "old_name", "--sig", marker_sig(),
                    "--why", "a")
        r = self.ws.run("req", "rename", "--id", "RQ-0001", "--key", "new_name")
        self.assertEqual(r.returncode, 0, r.stdout)
        ls = self.ws.run("req", "ls")
        self.assertIn("new_name", ls.stdout)
        self.assertNotIn("old_name", ls.stdout)
        # append-only: two lines, one row
        lines = (self.ws.kb() / "requests" / "agent.jsonl").read_text(
            encoding="utf-8").splitlines()
        self.assertEqual(len([ln for ln in lines if ln.strip()]), 2)

    # -- canon gates ------------------------------------------------------
    def test_canon_add_refuses_a_fact_without_evidence(self) -> None:
        r = self.ws.run("canon", "add", "--key", "taau", "--label", "x")
        self.assertEqual(r.returncode, 1)
        self.assertIn("--evidence is required", r.stdout)

    def test_canon_add_refuses_a_runtime_pointer(self) -> None:
        r = self.ws.run("canon", "add", "--key", "slot8",
                        "--addr", "0x1f7aacc6010", "--evidence", "docs/x.md:1")
        self.assertEqual(r.returncode, 1)
        self.assertIn("runtime", r.stdout)

    def test_canon_add_refuses_an_ambiguous_signature(self) -> None:
        r = self.ws.run("canon", "add", "--key", "block_fn",
                        "--sig", "48 89 5C 24 08", "--evidence", "docs/x.md:1")
        self.assertEqual(r.returncode, 1)
        self.assertIn("not a key", r.stdout)

    def test_canon_add_resolves_and_verify_agrees(self) -> None:
        r = self.ws.run("canon", "add", "--key", "marker_fn",
                        "--addr", hex(IMAGEBASE + TEXT_VA + MARKER_AT),
                        "--sig", marker_sig(), "--confidence", "observed",
                        "--evidence", "docs/research/x.md:12")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("resolved-in-26.51", r.stdout)
        ls = self.ws.run("canon", "ls")
        self.assertIn("1 resolved by sig", ls.stdout)
        v = self.ws.run("canon", "verify")
        self.assertEqual(v.returncode, 0, v.stdout)
        self.assertIn("still resolve uniquely", v.stdout)

    def test_canon_verify_reports_drift_after_the_binary_changes(self) -> None:
        self.ws.run("canon", "add", "--key", "marker_fn",
                    "--addr", hex(IMAGEBASE + TEXT_VA + MARKER_AT),
                    "--sig", marker_sig(), "--evidence", "docs/research/x.md:12")
        exe = self.ws.exe()
        data = bytearray(exe.read_bytes())
        off = TEXT_RAWPTR + MARKER_AT
        data[off:off + len(MARKER)] = b"\x90" * len(MARKER)
        exe.write_bytes(bytes(data))
        v = self.ws.run("canon", "verify")
        self.assertEqual(v.returncode, 1, v.stdout)
        self.assertIn("NOT FOUND", v.stdout)
        self.assertIn("anchors", v.stdout)

    def test_canon_import_splits_address_spaces_and_drops_heap_pointers(self) -> None:
        self.ws.write_doc("docs/research/sample.md", "\n".join([
            "# sample",
            "| 10 | `0x10b6a9ac1` | `modules::taau` | TAAU 上采样 |",
            "| 11 | `0x140001234` | `win_fn` | 26.51 的某个函数 |",
            "| 12 | `0x1800666E0` | `nvngx_slot` | nvngx.dll 里的东西 |",
            "运行期指针 `0x1f7aacc6010` 不是事实",
            "ImageBase `0x140000000` 也不是函数",
        ]))
        r = self.ws.run("canon", "import")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("1 mac-1.26.32, 1 other-module, 1 win-26.51", r.stdout)
        self.assertIn("skipped 1 runtime", r.stdout)
        spaces = {row["addr_space"] for row in self.ws.canon().values()}
        self.assertEqual(spaces, {"mac-1.26.32", "win-26.51", "other-module"})
        win = [row for row in self.ws.canon().values() if row["addr_space"] == "win-26.51"][0]
        self.assertEqual(win["rva"], "0x1234")
        self.assertEqual(win["status"], "located-in-26.51")

    def test_canon_rename_leaves_a_tombstone_and_repoints_the_queue(self) -> None:
        self.ws.run("canon", "add", "--key", "old_key",
                    "--addr", hex(IMAGEBASE + TEXT_VA + MARKER_AT),
                    "--sig", marker_sig(), "--evidence", "docs/research/x.md:1")
        self.ws.run("req", "add", "--key", "old_key", "--sig", marker_sig(),
                    "--why", "queued before the rename")
        r = self.ws.run("canon", "rename", "--old", "old_key", "--new", "new_key")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("RQ-0001", r.stdout)
        rows = self.ws.canon()
        self.assertEqual(rows["old_key"]["status"], "superseded")
        self.assertEqual(rows["old_key"]["superseded_by"], "new_key")
        self.assertEqual(rows["new_key"]["status"], "resolved-in-26.51")
        self.assertEqual(rows["new_key"]["renamed_from"], "old_key")
        ls = self.ws.run("canon", "ls")
        keys = [ln.split()[0] for ln in ls.stdout.splitlines()[1:] if ln.strip()]
        self.assertNotIn("old_key", keys)          # the key column, not the label
        self.assertIn("new_key", keys)
        self.assertIn("1 renamed", ls.stdout)
        found = self.ws.run("find", "old_key")
        self.assertIn("renamed: old_key -> new_key", found.stdout)
        self.assertIn("new_key", self.ws.run("req", "ls").stdout)

    def test_find_answers_before_anybody_opens_anything(self) -> None:
        self.ws.run("canon", "add", "--key", "marker_fn",
                    "--addr", hex(IMAGEBASE + TEXT_VA + MARKER_AT),
                    "--sig", marker_sig(), "--semantics", "the TAAU entry",
                    "--evidence", "docs/research/x.md:12")
        r = self.ws.run("find", "taau")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("marker_fn", r.stdout)
        self.assertIn("the TAAU entry", r.stdout)
        self.assertIn("next", r.stdout)

    def test_find_says_nothing_known_when_it_is_true(self) -> None:
        r = self.ws.run("find", "volumeScattering")
        self.assertEqual(r.returncode, 0)
        self.assertIn("nothing known", r.stdout)

    # -- the seam re_serve.py depends on (no IDA needed for these) --------
    def test_first_literal_run_proves_the_file_matches_the_database(self) -> None:
        sys.path.insert(0, str(AGENTFLOW))
        import re_serve

        self.assertEqual(re_serve._first_literal("48 89 5C 24 08"),
                         (0, bytes.fromhex("48895C2408")))
        # wildcards split the pattern: only runs of >= 4 literal bytes are usable
        self.assertEqual(re_serve._first_literal("48 89 ?? ?? 12 34 56 78"),
                         (4, bytes.fromhex("12345678")))
        self.assertIsNone(re_serve._first_literal("48 89 ?? ??"))
        self.assertIsNone(re_serve._first_literal("not hex"))

    def test_serve_pending_dedupes_a_renamed_request(self) -> None:
        sys.path.insert(0, str(AGENTFLOW))
        import re_serve

        self.ws.run("req", "add", "--key", "old_name", "--sig", marker_sig(),
                    "--why", "queued")
        self.ws.run("req", "rename", "--id", "RQ-0001", "--key", "new_name")
        queue = re_serve.pending(self.ws.kb())
        self.assertEqual([q["id"] for q in queue], ["RQ-0001"])
        self.assertEqual(queue[0]["key"], "new_name")

    def test_reference_database_does_not_answer_address_requests(self) -> None:
        """The queue is shared with the 26.51 session.  If the symbolized macOS .i64
        answered a sig request it would search PE bytes in a Mach-O, find nothing, and
        mark it failed - the queue would silently empty."""
        sys.path.insert(0, str(AGENTFLOW))
        import re_serve

        self.assertTrue(re_serve.request_allowed("sig", True))
        self.assertTrue(re_serve.request_allowed("name", True))
        self.assertFalse(re_serve.request_allowed("sig", False))
        self.assertFalse(re_serve.request_allowed("rva", False))
        self.assertTrue(re_serve.request_allowed("name", False))
        self.assertTrue(re_serve.request_allowed("addr", False))

    def test_every_ida_api_used_by_re_serve_exists_in_the_stub(self) -> None:
        """The 9.3 failure that motivated this: `idc.find_binary` was removed and
        `ida_bytes.find_bytes` grew a new argument order.  re_serve.py runs inside
        IDA, where a wrong attribute is a traceback in someone's Output window, so
        check the names against the installed stubs instead of trusting memory."""
        import re as _re

        candidates = [Path(r"D:\Program Files\IDA Professional 9.3\python"),
                      Path(r"D:\Program Files\IDA Professional 8.4\python"),
                      Path(r"D:\Program Files\IDA Professional 8.3\python"),
                      Path(r"D:\Program Files\IDA 9.0\python")]
        ida_py = next((p for p in candidates if p.is_dir()), None)
        if ida_py is None:
            self.skipTest("no IDA python stubs on this machine")

        source = (AGENTFLOW / "re_serve.py").read_text(encoding="utf-8")
        used = set(_re.findall(
            r"self\.(ida_[a-z_]+|idc|idautils|hexrays|kb)\.([A-Za-z_]\w*)", source))
        star = {"ida_hexrays": ["ida_hexrays.py", "ida_hexrays_ctree.py",
                                "ida_hexrays_defs.py", "ida_hexrays_micro.py"],
                "hexrays": ["ida_hexrays.py", "ida_hexrays_ctree.py",
                            "ida_hexrays_defs.py", "ida_hexrays_micro.py"]}
        missing = []
        for module, attr in sorted(used):
            if module == "kb":  # our own re_kb.py, checked where it lives
                text = (AGENTFLOW / "re_kb.py").read_text(encoding="utf-8")
                if not _re.search(rf"^def {attr}\b", text, _re.M):
                    missing.append(f"kb.{attr}")
                continue
            files = star.get(module, [f"{module}.py"])
            text = ""
            for name in files:
                path = ida_py / name
                if path.is_file():
                    text += path.read_text(encoding="utf-8", errors="replace")
            if not text:
                missing.append(f"{module}.{attr} (no stub for {module})")
                continue
            if not _re.search(rf"^(?:def {attr}\b|{attr}\s*=|class {attr}\b)", text, _re.M):
                missing.append(f"{module}.{attr}")
        self.assertEqual(missing, [], "re_serve.py uses IDA names that do not exist: "
                                     + ", ".join(missing))


if __name__ == "__main__":
    unittest.main(verbosity=2)
