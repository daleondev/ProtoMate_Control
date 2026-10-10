"""Check raw lint output, saved reports and analysis failure handling."""
import importlib.util
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


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
            self.assertIn("-stdlib=libstdc++", prepared[0]["arguments"])
            self.assertIn(str(output / "reflection-include"), prepared[0]["arguments"])
            self.assertEqual(prepared[0]["arguments"][-6:], app["arguments"][-6:])
            self.assertEqual(database.read_text(), original)

    def test_reflection_adaptation_preserves_code_and_original_headers(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            library = output / "library"
            (library / "__type_traits").mkdir(parents=True)
            meta = ('// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception\n'
                    '#ifndef _LIBCPP_META\n#define _LIBCPP_META\n'
                    '#include <__config>\n#include <__ranges/access.h>\n'
                    '#include <__ranges/concepts.h>\n#include <__ranges/size.h>\n'
                    '_LIBCPP_BEGIN_NAMESPACE_REFLECTION_V2\n'
                    'using info = decltype(^^int);\n_LIBCPP_END_NAMESPACE_REFLECTION_V2\n'
                    '_LIBCPP_BEGIN_NAMESPACE_STD\n_LIBCPP_END_NAMESPACE_STD\n#endif\n')
            trait = ('#include <__config>\n#include <__type_traits/integral_constant.h>\n'
                     '_LIBCPP_BEGIN_NAMESPACE_STD\n#if _LIBCPP_STD_VER >= 26\n'
                     'template<class T> _LIBCPP_NO_SPECIALIZATIONS\n'
                     'inline constexpr bool is_consteval_only_v = __is_consteval_only(T);\n'
                     '#endif\n_LIBCPP_END_NAMESPACE_STD\n')
            (library / "meta").write_text(meta)
            trait_path = library / "__type_traits/is_consteval_only.h"
            trait_path.write_text(trait)
            tidy.prepare_reflection_headers(output, library)
            adapted = (output / "reflection-include/meta").read_text()
            adapted_trait = (output / "reflection-include/clang_consteval_traits.hpp").read_text()
            self.assertIn('SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception', adapted)
            self.assertIn('#define _LIBCPP_META', adapted)
            self.assertIn('using info = decltype(^^int);', adapted)
            self.assertIn('namespace std { namespace meta { inline namespace reflection_v2 {', adapted)
            self.assertNotIn('#include <__config>', adapted)
            self.assertIn('__is_consteval_only(T)', adapted_trait)
            self.assertIn('#include <type_traits>', adapted_trait)
            self.assertEqual((library / "meta").read_text(), meta)
            self.assertEqual(trait_path.read_text(), trait)

    def test_changed_reflection_headers_require_revalidation(self):
        with self.assertRaisesRegex(ValueError, "Revalidate the adapter"):
            tidy.adapt_reflection_header("different header", {"expected wrapper": "replacement"})

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

    def test_compile_error_is_printed_without_prechecking_or_skipping(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            fixes = output / "src__main.cpp.yaml"
            fixes.write_text("stale replacements")
            entry = {"file": str(tidy.ROOT / "src/main.cpp"), "directory": temporary,
                     "arguments": [tidy.COMPILER, "-c", "relative.cpp", "-o", "object.o"]}
            error = (f"{entry['file']}:1:1: error: unsupported reflection API [clang-diagnostic-error]\n"
                     "    1 | reflection();\n      | ^\nFound compiler error(s).\n")
            console = io.StringIO()
            result = subprocess.CompletedProcess([], 1, error)
            with contextlib.redirect_stdout(console), patch.object(tidy.subprocess, "run", return_value=result) as run:
                report = tidy.check(entry, SimpleNamespace(output=output, timeout=5))
            self.assertEqual(run.call_count, 1)
            self.assertEqual(run.call_args.args[0][0], tidy.TIDY)
            self.assertEqual(run.call_args.kwargs["cwd"], temporary)
            self.assertEqual(report["status"], "blocked")
            self.assertEqual(report["diagnostics"], [])
            self.assertEqual(report["blockers"], [error.splitlines()[0]])
            self.assertFalse(fixes.exists())
            self.assertFalse((output / "src__main.cpp.preflight.log").exists())
            self.assertEqual((output / "src__main.cpp.log").read_text(), error)
            self.assertEqual(console.getvalue(), error)

    def test_tidy_runs_once_and_preserves_raw_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            entry = {"file": str(tidy.ROOT / "src/main.cpp"), "directory": temporary,
                     "arguments": [tidy.COMPILER, "-c", "relative.cpp"]}
            finding = (f"{entry['file']}:2:1: error: bad name [readability-identifier-naming,-warnings-as-errors]\n"
                       "    2 | int BadName; // μ\n      |     ^~~~~~~\n"
                       f"{entry['file']}:3:1: note: declared here\n1 warning treated as error\n")
            console = io.StringIO()
            result = subprocess.CompletedProcess([], 1, finding)
            with contextlib.redirect_stdout(console), patch.object(tidy.subprocess, "run", return_value=result) as run:
                report = tidy.check(entry, SimpleNamespace(output=output, timeout=5))
            self.assertEqual(run.call_count, 1)
            self.assertEqual(run.call_args.args[0][0], tidy.TIDY)
            self.assertEqual(run.call_args.kwargs["stderr"], subprocess.STDOUT)
            self.assertEqual(report["status"], "parsed")
            self.assertEqual(len(report["diagnostics"]), 1)
            self.assertEqual((output / "src__main.cpp.log").read_text(), finding)
            self.assertFalse((output / "src__main.cpp.preflight.log").exists())
            self.assertEqual(console.getvalue(), finding)

    def test_reports_are_saved_without_adding_console_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            entries = [{"file": str(tidy.ROOT / filename), "directory": temporary}
                       for filename in ("src/main.cpp", "src/system_threads.cpp")]
            (output / "compile_commands.json").write_text(json.dumps(entries))
            finding = (f"{tidy.ROOT}/src/example.hpp:2:1: error: bad name "
                       "[readability-identifier-naming,-warnings-as-errors]\n")
            results = [subprocess.CompletedProcess([], 0, "configuration valid", ""),
                       subprocess.CompletedProcess([], 1, finding),
                       subprocess.CompletedProcess([], 1, finding)]
            console = io.StringIO()
            with contextlib.redirect_stdout(console), \
                 patch.object(tidy, "prepare_reflection_headers"), \
                 patch.object(tidy.subprocess, "check_output", return_value="test clang-tidy"), \
                 patch.object(tidy.subprocess, "run", side_effect=results) as run:
                status = tidy.run_inside(SimpleNamespace(output=output, timeout=5, jobs=1, strict=False))
            self.assertEqual(status, 1)
            self.assertEqual(run.call_count, 3)
            self.assertEqual(console.getvalue(), finding * 2)
            report = json.loads((output / "report.json").read_text())
            self.assertEqual(report["coverage"], "complete")
            self.assertEqual(len(report["files"]), 2)
            self.assertEqual(len(report["findings"]), 1)
            self.assertIn("1 unique src/ findings", (output / "summary.txt").read_text())
            self.assertEqual(len((output / "findings.txt").read_text().splitlines()), 1)

    def test_raw_output_preserves_stdout_stderr_order(self):
        code, log, _ = tidy.run_command(
            [sys.executable, "-c", "import sys; print('first', flush=True); "
             "print('second', file=sys.stderr, flush=True); print('third', flush=True)"],
            str(tidy.ROOT), 5)
        self.assertEqual(code, 0)
        self.assertEqual(log, "first\nsecond\nthird\n")

    def test_timeout_and_missing_tool_are_not_treated_as_blocked(self):
        for error, expected in [(subprocess.TimeoutExpired("clang", 1, output=b"partial output"), 124),
                                (FileNotFoundError("clang not installed"), 127)]:
            with self.subTest(error=error), patch.object(tidy.subprocess, "run", side_effect=error):
                code, log, elapsed = tidy.run_command(["clang"], str(tidy.ROOT), 1)
                self.assertEqual(code, expected)
                self.assertEqual(tidy.classify(code, log), "tool-failure")
                self.assertGreaterEqual(elapsed, 0)


if __name__ == "__main__":
    unittest.main()
