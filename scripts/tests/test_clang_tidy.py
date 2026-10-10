"""Check that partial lint never disguises tool failures or gates on broken ASTs."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "clang_tidy.py"
SPEC = importlib.util.spec_from_file_location("clang_tidy", SCRIPT)
tidy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tidy)


class PartialLintTests(unittest.TestCase):
    def test_database_preserves_defines_includes_and_original_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            output = build / "lint"
            output.mkdir()
            app = {"directory": str(tidy.ROOT), "file": "src/main.cpp",
                   "arguments": ["g++", "-Ipath with spaces", "-DFLAG=7", "-freflection",
                                 "-std=c++26", "-c", "src/main.cpp"]}
            dependency = {**app, "file": "external/dependency.cpp"}
            original = json.dumps([app, dependency])
            database = build / "compile_commands.json"
            database.write_text(original)
            tidy.prepare_database(build, output)
            prepared = json.loads((output / "compile_commands.json").read_text())
            self.assertEqual(len(prepared), 1)
            self.assertEqual(prepared[0]["arguments"][0], tidy.COMPILER)
            self.assertEqual(prepared[0]["arguments"][-6:], app["arguments"][-6:])
            self.assertEqual(database.read_text(), original)

    def test_parse_error_is_blocked_even_with_lint_findings(self):
        self.assertEqual(tidy.classify(1, "x.cpp:1:1: error: unsupported [clang-diagnostic-error]"), "blocked")

    def test_warnings_as_errors_are_findings(self):
        log = "x.cpp:1:1: error: bad name [readability-identifier-naming,-warnings-as-errors]"
        self.assertEqual(tidy.classify(1, log), "parsed")

    def test_invalid_command_and_timeout_are_not_skipped(self):
        self.assertEqual(tidy.classify(1, "error: unknown argument"), "tool-failure")
        self.assertEqual(tidy.classify(124, "ANALYSIS TIMED OUT"), "tool-failure")
        self.assertEqual(tidy.classify(-11, ""), "tool-failure")

    def test_only_application_diagnostics_are_collected(self):
        root = tidy.ROOT
        log = (f"{root}/src/cli/../main.cpp:12:3: error: bad name "
               "[readability-identifier-naming,-warnings-as-errors]\n"
               f"{root}/external/example.hpp:1:1: warning: name [readability-identifier-naming]\n"
               f"{root}/src/main.cpp:2:1: error: missing type [clang-diagnostic-error]\n")
        self.assertEqual(tidy.collect_diagnostics(log), [{"file": "src/main.cpp", "line": 12,
            "column": 3, "message": "bad name", "checks": "readability-identifier-naming"}])

    def test_partial_mode_and_strict_mode(self):
        reports = [{"status": "parsed"}, {"status": "blocked"}]
        self.assertEqual(tidy.exit_status(reports, [], False), 0)
        self.assertEqual(tidy.exit_status(reports, [{"message": "finding"}], False), 1)
        self.assertEqual(tidy.exit_status(reports, [], True), 2)

    def test_empty_or_entirely_blocked_runs_fail(self):
        self.assertEqual(tidy.exit_status([], [], False), 2)
        self.assertEqual(tidy.exit_status([{"status": "blocked"}], [], False), 2)
        self.assertEqual(tidy.exit_status([{"status": "parsed"}, {"status": "tool-failure"}], [], False), 2)


if __name__ == "__main__":
    unittest.main()
