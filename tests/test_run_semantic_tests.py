"""scripts/run_semantic_tests.py on a throwaway suite: it finds what unittest discover finds, and a failing, crashing
or unimportable file always makes it exit 1 with that file named, in parallel and with -j 1.

    python -I tests/test_run_semantic_tests.py
"""
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import run_semantic_tests as r  # noqa: E402

PASS = "import unittest\nimport helper\nclass T(unittest.TestCase):\n    def test_a(self): self.assertEqual(helper.X, 1)\n" \
       "    def test_b(self): pass\n"
FAIL = "import unittest\nclass T(unittest.TestCase):\n    def test_a(self): self.assertEqual(1, 2, 'boom-msg')\n"
BROKEN = "import no_such_module_anywhere\n"


def suite(tmp, files):
    for rel, text in files.items():
        path = os.path.join(tmp, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
    return tmp


def run(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        code = r.main(list(argv))
    return code, out.getvalue()


def cli(*argv):
    return subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scripts", "run_semantic_tests.py"), *argv],
                          capture_output=True, text=True, timeout=15)


class RunnerTest(unittest.TestCase):
    def test_custom_patterns_find_only_python_modules_in_packages(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"case_one.py": "", "case_two.txt": "", "case_bad-name.py": "",
                        "pkg/__init__.py": "", "pkg/case_two.py": "", "plain/case_three.py": ""})
            self.assertEqual(r.test_modules(tmp, "case_*.py"), ["case_one", "pkg.case_two"])
            self.assertEqual(r.test_modules(tmp, "*"), ["case_one", "pkg.__init__", "pkg.case_two"])

    def test_pattern_selects_files_without_importing_excluded_modules(self):
        for jobs in (1, 2):
            for flag in ("-p", "--pattern"):
                with self.subTest(jobs=jobs, flag=flag), tempfile.TemporaryDirectory() as tmp:
                    suite(tmp, {"helper.py": "X = 1\n", "case_one.py": PASS,
                                "pkg/__init__.py": "", "pkg/case_two.py": PASS,
                                "test_excluded.py": "raise AssertionError('excluded module imported')\n",
                                "plain/case_three.py": BROKEN})
                    result = cli(tmp, "-j", str(jobs), flag, "case_*.py")
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertNotIn("excluded module imported", result.stdout + result.stderr)
                    if jobs == 1:
                        self.assertIn("Ran 4 tests", result.stderr)
                    else:
                        self.assertIn("2 files, 4 tests, 0 files failed", result.stdout)

    def test_pattern_no_matches_is_an_error_in_both_modes(self):
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"test_excluded.py": BROKEN})
                result = cli(tmp, "-j", str(jobs), "--pattern", "case_*.py")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("no case_*.py files", result.stdout)
                self.assertNotIn("Traceback", result.stderr)

    def test_matching_failure_is_not_hidden_by_pattern_selection(self):
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"case_fail.py": FAIL, "test_excluded.py": BROKEN})
                result = cli(tmp, "-j", str(jobs), "--pattern", "case_*.py")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("boom-msg", result.stdout + result.stderr)
                self.assertNotIn("no_such_module_anywhere", result.stdout + result.stderr)

    def test_omitting_pattern_keeps_the_default_selection(self):
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"helper.py": "X = 1\n", "test_pass.py": PASS,
                            "case_excluded.py": "raise AssertionError('custom module imported')\n"})
                result = cli(tmp, "-j", str(jobs))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn("custom module imported", result.stdout + result.stderr)

    def test_finds_what_discover_finds(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"test_one.py": "", "helper.py": "", "pkg/__init__.py": "", "pkg/test_two.py": "",
                        "plain_dir/test_three.py": "", "testx.py": ""})
            # discover enters packages only, and its pattern is test*.py
            self.assertEqual(r.test_modules(tmp), ["test_one", "testx", "pkg.test_two"])

    def test_all_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"helper.py": "X = 1\n", "test_p1.py": PASS, "test_p2.py": PASS})
            code, out = run(tmp, "-j", "2")
            self.assertEqual(code, 0, out)
            self.assertIn("2 files, 4 tests, 0 files failed", out)

    def test_a_failure_or_import_error_is_reported_and_fails_the_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"helper.py": "X = 1\n", "test_p.py": PASS, "test_f.py": FAIL, "test_b.py": BROKEN})
            code, out = run(tmp, "-j", "3")
            self.assertEqual(code, 1, out)
            self.assertIn("==== FAILED: test_f", out)
            self.assertIn("boom-msg", out)                         # the failing file's own output is shown
            self.assertIn("==== FAILED: test_b", out)
            self.assertIn("2 files failed", out)
            self.assertTrue(out.rstrip().endswith("test_b, test_f"), out)

    def test_one_job_runs_discover_and_keeps_its_exit_code(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"helper.py": "X = 1\n", "test_p.py": PASS, "test_f.py": FAIL})
            self.assertEqual(run(tmp, "-j", "1")[0], 1)
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"helper.py": "X = 1\n", "test_p.py": PASS})
            self.assertEqual(run(tmp, "-j", "1")[0], 0)

    def test_an_empty_suite_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, out = run(tmp, "-j", "2")
            self.assertEqual(code, 1)
            self.assertIn("no test*.py files", out)


if __name__ == "__main__":
    unittest.main()
