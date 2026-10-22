"""The benchmark must retry a suspended trial with the original preparation."""

import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch


spec = importlib.util.spec_from_file_location(
    "benchmark_mcpp", Path(__file__).resolve().parents[2] / "scripts/benchmark-mcpp.py")
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class CleanRunTest(unittest.TestCase):
    def test_suspend_retries_preparation_and_records_only_clean_duration(self):
        records, prepare, output = [], Mock(), io.StringIO()
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(output), \
                patch.object(benchmark.time, "clock_gettime",
                             side_effect=[10, 100, 15, 125, 20, 130, 23, 133]), \
                patch.object(benchmark.subprocess, "check_output",
                             side_effect=["0", "25", "30", "33"]), \
                patch.object(benchmark.subprocess, "run",
                             return_value=subprocess.CompletedProcess(["test"], 0)) as run:
            seconds, _ = benchmark.run_clean(
                ["test"], Path(tmp) / "test.log", cwd=tmp, env={}, records=records,
                prepare=prepare)
        self.assertEqual(seconds, 3)
        self.assertEqual(prepare.call_count, 2)
        self.assertEqual(run.call_count, 2)
        self.assertEqual([r["accepted"] for r in records], [False, True])
        self.assertEqual(records[0]["monotonic_seconds"], 5)
        self.assertEqual(records[0]["boottime_seconds"], 25)
        self.assertIn("DISCARD", output.getvalue())
        self.assertIn("retrying", output.getvalue())

    def test_clean_command_failure_is_not_hidden_by_retry(self):
        records = []
        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(benchmark.time, "clock_gettime", side_effect=[0, 100, 2, 102]), \
                patch.object(benchmark.subprocess, "check_output", side_effect=["0", "2"]), \
                patch.object(benchmark.subprocess, "run",
                             return_value=subprocess.CompletedProcess(["test"], 7)), \
                self.assertRaisesRegex(RuntimeError, "failed"):
            benchmark.run_clean(["test"], Path(tmp) / "test.log", cwd=tmp,
                                env={}, records=records)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["exit"], 7)

    def test_one_second_difference_does_not_exceed_threshold(self):
        records = []
        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(benchmark.time, "clock_gettime", side_effect=[0, 100, 2, 103]), \
                patch.object(benchmark.subprocess, "check_output", side_effect=["0", "3"]), \
                patch.object(benchmark.subprocess, "run",
                             return_value=subprocess.CompletedProcess(["test"], 0)):
            seconds, _ = benchmark.run_clean(["test"], Path(tmp) / "test.log", cwd=tmp,
                                            env={}, records=records)
        self.assertEqual(seconds, 2)
        self.assertTrue(records[0]["accepted"])


if __name__ == "__main__":
    unittest.main()
