#!/usr/bin/env python3
"""Regression checks for non-destructive dependency acquisition."""

import importlib.util
import csv
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("upstreams", ROOT / "scripts/upstreams.py")
upstreams = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upstreams)


class UpstreamsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.git("init", "--quiet")
        (self.repo / "source.txt").write_text("committed\n")
        self.git("add", "source.txt")
        self.git("-c", "user.name=Workflow Test", "-c", "user.email=test@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "fixture")
        self.revision = self.git("rev-parse", "HEAD")
        self.pin = {"revision": self.revision, "url": str(self.repo)}
        self.args = type("Args", (), {"memsim_archive": None, "memsim_url": None})()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args],
                                       text=True, stderr=subprocess.DEVNULL).strip()

    def archive(self):
        archive = self.root / "source.tar.gz"
        with tarfile.open(archive, "w:gz", format=tarfile.PAX_FORMAT,
                          pax_headers={"comment": self.revision}) as output:
            entry = tarfile.TarInfo("mem_sim/CMakeLists.txt")
            data = b"project(test)\n"
            entry.size = len(data)
            entry.mode = 0o644
            output.addfile(entry, io.BytesIO(data))
        return archive, self.pin

    def test_fetch_exact_commit(self):
        dest = self.root / "cloned"
        with patch.dict(upstreams.LOCK, {"fixture": self.pin}):
            upstreams.fetch("fixture", dest, self.args)
        self.assertEqual(upstreams.revision(dest), self.revision)

    def test_existing_dirty_tree_preserved(self):
        (self.repo / "source.txt").write_text("user change\n")
        with patch.dict(upstreams.LOCK, {"fixture": self.pin}):
            upstreams.fetch("fixture", self.repo, self.args)
        self.assertEqual((self.repo / "source.txt").read_text(), "user change\n")

    def test_wrong_revision_not_checked_out(self):
        with patch.dict(upstreams.LOCK, {"fixture": dict(self.pin, revision="0" * 40)}):
            with self.assertRaises(ValueError):
                upstreams.fetch("fixture", self.repo, self.args)
        self.assertEqual(upstreams.revision(self.repo), self.revision)

    def test_failed_fetch_leaves_no_destination(self):
        dest = self.root / "failed"
        with patch.dict(upstreams.LOCK, {"fixture": dict(self.pin, revision="0" * 40)}):
            with self.assertRaises(ValueError):
                upstreams.fetch("fixture", dest, self.args)
        self.assertFalse(dest.exists())

    def test_enclosing_git_repository_rejected(self):
        nested = self.repo / "not-a-repo"
        nested.mkdir()
        with self.assertRaises(ValueError):
            upstreams.revision(nested)

    def test_wrong_archive_revision_not_extracted(self):
        archive, pin = self.archive()
        dest = self.root / "snapshot"
        with patch.dict(upstreams.LOCK, {"memsim": dict(pin, revision="0" * 40)}):
            with self.assertRaises(ValueError):
                upstreams.import_memsim(archive, dest)
        self.assertFalse(dest.exists())

    def test_snapshot_import_and_missing_file_detection(self):
        archive, pin = self.archive()
        dest = self.root / "snapshot"
        with patch.dict(upstreams.LOCK, {"memsim": pin}):
            upstreams.import_memsim(archive, dest)
            upstreams.check("memsim", dest)
            (dest / "CMakeLists.txt").unlink()
            with self.assertRaises(ValueError):
                upstreams.check("memsim", dest)
        self.assertFalse((dest / ".git").exists())
        self.assertEqual(json.loads((dest / upstreams.RECEIPT).read_text())["revision"],
                         self.revision)

    def test_archive_path_escape_rejected(self):
        archive = self.root / "escape.tar.gz"
        with tarfile.open(archive, "w:gz", format=tarfile.PAX_FORMAT,
                          pax_headers={"comment": self.revision}) as output:
            entry = tarfile.TarInfo("mem_sim/../escaped.txt")
            output.addfile(entry, io.BytesIO(b""))
        dest = self.root / "snapshot"
        with patch.dict(upstreams.LOCK, {"memsim": self.pin}):
            with self.assertRaises(ValueError):
                upstreams.import_memsim(archive, dest)
        self.assertFalse(dest.exists())
        self.assertFalse((self.root / "escaped.txt").exists())

    def test_package_exports_commit_not_dirty_worktree(self):
        (self.repo / "source.txt").write_text("user change\n")
        archive, dest = self.root / "delivery.tar.gz", self.root / "snapshot"
        with patch.dict(upstreams.LOCK, {"memsim": self.pin}):
            upstreams.package_memsim(self.repo, archive)
            upstreams.import_memsim(archive, dest)
        self.assertEqual((dest / "source.txt").read_text(), "committed\n")
        self.assertEqual((self.repo / "source.txt").read_text(), "user change\n")

    def test_package_existing_output_preserved(self):
        archive = self.root / "delivery.tar.gz"
        archive.write_bytes(b"user output")
        with patch.dict(upstreams.LOCK, {"memsim": self.pin}):
            with self.assertRaises(ValueError):
                upstreams.package_memsim(self.repo, archive)
        self.assertEqual(archive.read_bytes(), b"user output")

    def test_package_excludes_only_declared_document(self):
        (self.repo / "upstream-guide.md").write_text("upstream guide\n")
        self.git("add", "upstream-guide.md")
        self.git("-c", "user.name=Workflow Test", "-c", "user.email=test@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "guide")
        pin = dict(self.pin, revision=self.git("rev-parse", "HEAD"),
                   excluded_delivery_files=["upstream-guide.md"])
        archive, dest = self.root / "delivery.tar.gz", self.root / "snapshot"
        with patch.dict(upstreams.LOCK, {"memsim": pin}):
            upstreams.package_memsim(self.repo, archive)
            upstreams.import_memsim(archive, dest)
        self.assertTrue((dest / "source.txt").is_file())
        self.assertFalse((dest / "upstream-guide.md").exists())
        self.assertTrue((self.repo / "upstream-guide.md").is_file())


class ResponseAuditTest(unittest.TestCase):
    def audit(self, status, arrival=1, completion=2, latency=1, allow=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({"source_stats": {"host": {"requests": 1, "bytes": 4}}}))
            mapping, responses = root / "map.csv", root / "responses.csv"
            rows = [
                (mapping, dict(host_request_id=0, trace_line=1, cycle=1,
                               src_name="host", chan="AR", addr="0x80000000",
                               size=4, projection="AR")),
                (responses, dict(host_request_id=0, type="Read", system_address="0x80000000",
                                 arrival_cycle=arrival, completion_cycle=completion,
                                 latency_cycles=latency, status=status)),
            ]
            for path, row in rows:
                with path.open("w", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(row))
                    writer.writeheader()
                    writer.writerow(row)
            command = [sys.executable, str(ROOT / "workloads/llm_memory/compare_results.py"),
                       str(manifest), str(mapping), str(responses)]
            if allow:
                command.append("--allow-uninitialized")
            return subprocess.run(command, capture_output=True, text=True)

    def test_uninitialized_requires_explicit_policy(self):
        self.assertEqual(self.audit("uninitialized_data").returncode, 1)
        allowed = self.audit("uninitialized_data", allow=True)
        self.assertEqual(allowed.returncode, 0, allowed.stderr)
        self.assertIn("uninitialized_data=1", allowed.stdout)
        self.assertEqual(self.audit("data_mismatch", allow=True).returncode, 1)

    def test_negative_latency_rejected_even_when_arithmetic_matches(self):
        result = self.audit("ok", arrival=2, completion=1, latency=-1)
        self.assertEqual(result.returncode, 1)
        self.assertIn("因果", result.stderr)


if __name__ == "__main__":
    unittest.main()
