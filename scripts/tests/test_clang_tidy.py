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
            hal = {**app, "file": "platform/hal/hal.cpp"}
            runtime = {**app, "file": "platform/runtime/threadx/Support.cpp"}
            excluded = [{**app, "file": filename} for filename in (
                "external/dependency.cpp", "platform/runtime/vendor/dependency.cpp",
                "platform/hal_extra/example.cpp", "src_other/example.cpp",
                "platform/runtime_extra/example.cpp", "platform/runtime/tests/libc_test.cpp",
                "src/control/tests/stepper_motor_test.cpp", "src/p2p/tests/bridge_test.cpp",
                "src/cli/test/test_parser.cpp", "platform/hal/tests/gpio_test.cpp",
                "platform/hal/tests/step_model/step_stm32_test.cpp")]
            original = json.dumps([app, hal, runtime, *excluded])
            database = build / "compile_commands.json"
            database.write_text(original)
            tidy.prepare_database(build, output)
            prepared = json.loads((output / "analysis.json").read_text())
            self.assertEqual(len(prepared), 3)
            self.assertEqual([entry["file"] for entry in prepared],
                             [str(tidy.ROOT / entry["file"]) for entry in (app, hal, runtime)])
            self.assertEqual(prepared[0]["arguments"][0], tidy.COMPILER)
            self.assertIn("-stdlib=libstdc++", prepared[0]["arguments"])
            self.assertIn(str(output / "reflection-include"), prepared[0]["arguments"])
            self.assertEqual(prepared[0]["arguments"][-6:], app["arguments"][-6:])
            self.assertEqual(database.read_text(), original)

    def test_header_filter_covers_application_hal_and_runtime_without_tests_or_vendor(self):
        pattern = tidy.re.compile(tidy.header_filter())
        for filename in ("src/main.hpp", "platform/hal/drivers/impl/stm32/Spi.hpp",
                         "platform/runtime/libstdcxx/backend.hpp"):
            self.assertIsNotNone(pattern.match(str(tidy.ROOT / filename)))
        for filename in ("platform/runtime_extra/Runtime.hpp", "external/pneumo/include/meta.hpp",
                         "platform/hal_other/other.hpp", "src_other/other.hpp"):
            self.assertIsNone(pattern.match(str(tidy.ROOT / filename)))
        excluded = tidy.re.compile(tidy.EXCLUDED_HEADERS)
        for filename in ("src/control/tests/Connection.hpp", "src/p2p/tests/CommunicationLoad.hpp",
                         "platform/hal/tests/transport_model/fakes/hal/hal.hpp", "src/test/helper.hpp",
                         "platform/runtime/tests/helper.hpp", "platform/runtime/libstdcxx/vendor/gcc-16/filesystem/dir-common.h"):
            source = tidy.ROOT / filename
            self.assertIsNotNone(excluded.search(str(source)))
            self.assertFalse(tidy.in_scope(source))
        for filename in ("src/control/StepperMotor.hpp", "platform/hal/drivers/impl/stm32/Spi.hpp",
                         "src/test_helpers/Test.hpp", "platform/runtime/libstdcxx/include/bits/gthr-default.h"):
            self.assertIsNone(excluded.search(str(tidy.ROOT / filename)))

    def test_arm_and_host_commands_keep_separate_databases_and_target_settings(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "platform/hal/hal.cpp"
            source.parent.mkdir(parents=True)
            source.touch()
            tests = root / "platform/hal/tests"
            tests.mkdir()
            (tests / "unlisted_test.cpp").touch()
            host, arm, output = (root / name for name in ("host", "arm", "lint"))
            for directory in (host, arm, output):
                directory.mkdir()
            native = {"directory": str(root), "file": str(source),
                      "arguments": ["g++", "-DHAL_PLATFORM_LINUX", "-c", str(source)]}
            target = {**native, "arguments": ["arm-none-eabi-g++", "-mcpu=cortex-m7",
                      "-mfpu=fpv5-d16", "-mfloat-abi=hard", "-DHAL_PLATFORM_STM32", "-c", str(source)]}
            (host / "compile_commands.json").write_text(json.dumps([native]))
            (arm / "compile_commands.json").write_text(json.dumps([target]))
            with patch.object(tidy, "ROOT", root), patch.object(tidy, "SOURCE_ROOTS", (root / "platform/hal",)):
                tidy.prepare_database(host, output, [("stm32", arm)], [root / "arm-headers"])
            entries = json.loads((output / "analysis.json").read_text())
            self.assertEqual([entry["context"] for entry in entries], ["linux", "stm32"])
            self.assertNotEqual(entries[0]["database"], entries[1]["database"])
            self.assertNotIn("--target=arm-none-eabi", entries[0]["arguments"])
            self.assertIn("--target=arm-none-eabi", entries[1]["arguments"])
            self.assertIn("-nostdinc++", entries[1]["arguments"])
            self.assertEqual(entries[1]["arguments"][-6:], target["arguments"][-6:])
            for entry in entries:
                commands = json.loads((Path(entry["database"]) / "compile_commands.json").read_text())
                self.assertEqual(len(commands), 1)
                self.assertEqual(commands[0]["arguments"], entry["arguments"])

    def test_missing_platform_source_is_not_silently_omitted(self):
        for directory in ("platform/hal", "platform/runtime"):
            with self.subTest(directory=directory), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                source = root / directory / "new_driver.cpp"
                source.parent.mkdir(parents=True)
                source.touch()
                (root / "compile_commands.json").write_text(json.dumps([
                    {"directory": str(root), "file": "src/main.cpp", "arguments": ["g++", "-c", "src/main.cpp"]}]))
                with patch.object(tidy, "ROOT", root), patch.object(tidy, "SOURCE_ROOTS", (root / "src", root / directory)):
                    with self.assertRaisesRegex(ValueError, "HAL/runtime sources missing compilation commands.*new_driver.cpp"):
                        tidy.prepare_database(root, root, [("stm32", root)])

    def test_runtime_c_sources_keep_c_language_in_both_contexts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "platform/runtime/libc/libc.c"
            source.parent.mkdir(parents=True)
            source.touch()
            # A copied GCC source is deliberately absent from the database.
            vendor = root / "platform/runtime/libstdcxx/vendor/gcc-16/atomic.cc"
            vendor.parent.mkdir(parents=True)
            vendor.touch()
            host, arm, output = (root / name for name in ("host", "arm", "lint"))
            for directory in (host, arm, output):
                directory.mkdir()
            native = {"directory": str(root), "file": str(source),
                      "arguments": ["gcc", "-DHAL_PLATFORM_LINUX", "-std=c11", "-c", str(source)]}
            target = {**native, "arguments": ["arm-none-eabi-gcc", "-mcpu=cortex-m7",
                      "-DHAL_PLATFORM_STM32", "-std=c11", "-c", str(source)]}
            (host / "compile_commands.json").write_text(json.dumps([native]))
            (arm / "compile_commands.json").write_text(json.dumps([target]))
            with patch.object(tidy, "ROOT", root), patch.object(tidy, "SOURCE_ROOTS", (root / "platform/runtime",)):
                tidy.prepare_database(host, output, [("stm32", arm)], [root / "arm-headers"])
            entries = json.loads((output / "analysis.json").read_text())
            self.assertEqual([entry["context"] for entry in entries], ["linux", "stm32"])
            for entry in entries:
                arguments = entry["arguments"]
                self.assertEqual(arguments[0], tidy.C_COMPILER)
                self.assertIn("-std=c11", arguments)
                for flag in ("-stdlib=libstdc++", "-nostdinc++", "-fexpansion-statements", "-fannotation-attributes"):
                    self.assertNotIn(flag, arguments)
            self.assertIn("--target=arm-none-eabi", entries[1]["arguments"])

    def test_startup_uses_application_command_instead_of_tests_or_examples(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            output = build / "lint"
            output.mkdir()
            source = tidy.ROOT / "platform/runtime/startup.cpp"
            entries = [{"directory": str(build), "file": str(source),
                        "arguments": ["g++", "-D" + label, "-o", obj, "-c", str(source)]}
                       for label, obj in (
                           ("TEST", "platform/runtime/tests/CMakeFiles/runtime_tests.dir/__/startup.cpp.o"),
                           ("EXAMPLE", "src/p2p/examples/CMakeFiles/example.dir/startup.cpp.o"),
                           ("APPLICATION", "CMakeFiles/Application.dir/platform/runtime/startup.cpp.o"))]
            (build / "compile_commands.json").write_text(json.dumps(entries))
            tidy.prepare_database(build, output)
            selected = json.loads((output / "analysis.json").read_text())
            self.assertEqual(len(selected), 1)
            self.assertIn("-DAPPLICATION", selected[0]["arguments"])

    def test_cross_compiler_headers_exclude_gcc_builtins(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            includes = [root / name for name in ("cxx", "multilib", "gcc/include", "gcc/include-fixed", "newlib")]
            for directory in includes:
                directory.mkdir(parents=True)
            entry = {"directory": str(tidy.ROOT), "file": "platform/hal/hal.cpp",
                     "arguments": ["arm-none-eabi-g++", "-mcpu=cortex-m7", "-mfpu=fpv5-d16",
                                   "-mfloat-abi=hard", "-DOTHER=1"]}
            (root / "compile_commands.json").write_text(json.dumps([entry]))
            stderr = "#include <...> search starts here:\n" + "\n".join(f" {path}" for path in includes)
            stderr += "\nEnd of search list.\n"
            with patch.object(tidy.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", stderr)) as run, \
                 patch.object(tidy.subprocess, "check_output", side_effect=[str(path) for path in includes[2:4]]):
                result = tidy.arm_include_directories(root)
            self.assertEqual(result, [*includes[:2], includes[4]])
            self.assertIn("-mcpu=cortex-m7", run.call_args.args[0])
            self.assertIn("-mfloat-abi=hard", run.call_args.args[0])
            self.assertNotIn("-DOTHER=1", run.call_args.args[0])

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

    def test_named_compiler_error_is_blocked(self):
        log = ("Error while processing x.cpp.\n"
               "x.hpp:1:1: error: cannot be narrowed [clang-diagnostic-c++11-narrowing]\n")
        self.assertEqual(tidy.classify(1, log), "blocked")

    def test_parse_error_is_blocked_even_with_lint_findings(self):
        self.assertEqual(tidy.classify(1, "x.cpp:1:1: error: unsupported [clang-diagnostic-error]"), "blocked")

    def test_warnings_as_errors_are_findings(self):
        log = "x.cpp:1:1: error: bad name [readability-identifier-naming,-warnings-as-errors]"
        self.assertEqual(tidy.classify(1, log), "parsed")

    def test_invalid_command_and_timeout_are_not_skipped(self):
        self.assertEqual(tidy.classify(1, "error: unknown argument"), "tool-failure")
        self.assertEqual(tidy.classify(124, "ANALYSIS TIMED OUT"), "tool-failure")
        self.assertEqual(tidy.classify(-11, ""), "tool-failure")

    def test_application_hal_and_runtime_diagnostics_are_collected_without_tests_or_vendor(self):
        root = tidy.ROOT
        log = (f"{root}/src/cli/../main.cpp:12:3: error: bad name "
               "[readability-identifier-naming,-warnings-as-errors]\n"
               f"{root}/external/example.hpp:1:1: warning: name [readability-identifier-naming]\n"
               f"{root}/platform/runtime/Runtime.hpp:1:1: warning: name [readability-identifier-naming]\n"
               f"{root}/platform/runtime/tests/test.cpp:1:1: warning: name [readability-identifier-naming]\n"
               f"{root}/platform/runtime/libstdcxx/vendor/gcc-16/atomic.cc:1:1: warning: name [readability-identifier-naming]\n"
               f"{root}/src/control/tests/Connection.hpp:1:1: warning: name [readability-identifier-naming]\n"
               f"{root}/platform/hal/tests/gpio_test.cpp:1:1: warning: name [readability-identifier-naming]\n"
               f"{root}/platform/hal/Gpio.hpp:3:4: warning: name [readability-identifier-naming]\n"
               f"{root}/src/main.cpp:2:1: error: missing type [clang-diagnostic-error]\n")
        self.assertEqual(tidy.collect_diagnostics(log), [{"file": "src/main.cpp", "line": 12,
            "column": 3, "message": "bad name", "checks": "readability-identifier-naming"},
            {"file": "platform/runtime/Runtime.hpp", "line": 1, "column": 1,
             "message": "name", "checks": "readability-identifier-naming"},
            {"file": "platform/hal/Gpio.hpp", "line": 3, "column": 4,
             "message": "name", "checks": "readability-identifier-naming"}])

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
            fixes = output / "linux/src__main.cpp.yaml"
            fixes.parent.mkdir()
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
            self.assertIn("--exclude-header-filter=" + tidy.EXCLUDED_HEADERS, run.call_args.args[0])
            self.assertEqual(run.call_args.kwargs["cwd"], temporary)
            self.assertEqual(report["status"], "blocked")
            self.assertEqual(report["diagnostics"], [])
            self.assertEqual(report["blockers"], [error.splitlines()[0]])
            self.assertFalse(fixes.exists())
            self.assertFalse((output / "linux/src__main.cpp.preflight.log").exists())
            self.assertEqual((output / "linux/src__main.cpp.log").read_text(), error)
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
            self.assertEqual((output / "linux/src__main.cpp.log").read_text(), finding)
            self.assertFalse((output / "linux/src__main.cpp.preflight.log").exists())
            self.assertEqual(console.getvalue(), finding)

    def test_reports_are_saved_without_adding_console_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            entries = [{"file": str(tidy.ROOT / filename), "directory": temporary}
                       for filename in ("src/main.cpp", "src/system_threads.cpp")]
            (output / "analysis.json").write_text(json.dumps(entries))
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
            self.assertIn("1 unique src/, platform/hal/ and platform/runtime/ findings", (output / "summary.txt").read_text())
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
