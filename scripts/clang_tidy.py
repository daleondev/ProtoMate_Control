#!/usr/bin/env python3
"""Lint production src/ and platform/hal/ code with Pneumo's reflection compiler.

Each source runs directly through clang-tidy; its raw output goes to the console.
Parallel runs keep each file's output together. Reports are saved separately.
Lint findings in parsed files fail the run. --strict also fails on blocked files.
No fixes are applied. Neither the GCC database nor application sources are edited.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
from threading import Lock
import time


ROOT = Path(__file__).resolve().parents[1]
# Keep the compiler, clang-tidy and reflection headers from the same toolchain.
# Use its installed libstdc++ for the remaining standard library facilities.
IMAGE = ("ghcr.io/daleondev/pneumo-devcontainer@"
         "sha256:1c10d580f47f4fa402da038896027629ded2d94de4ddd0f382f8a63830fae3e7")
COMPILER = "/opt/clang-p2996/bin/clang++"
TIDY = "/opt/clang-p2996/bin/clang-tidy"
REFLECTION_HEADERS = Path("/opt/clang-p2996/include/c++/v1")
DIAGNOSTIC = re.compile(
    r"^(.+?):(\d+):(\d+): (?:warning|error|fatal error): (.*?) \[([^\]]+)\]$", re.MULTILINE)
OUTPUT_LOCK = Lock()
SOURCE_ROOTS = (ROOT / "src", ROOT / "platform/hal")
EXCLUDED_HEADERS = r"(^|/)(test|tests)/"


def in_scope(source):
    return any(source.is_relative_to(directory) and
               not {"test", "tests"}.intersection(source.relative_to(directory).parts[:-1])
               for directory in SOURCE_ROOTS)


def header_filter():
    return "^(" + "|".join(re.escape(str(directory)) for directory in SOURCE_ROOTS) + ")/"


def configure_database(output, name, arguments):
    """Obtain real CMake commands without building or changing firmware presets."""
    build = output / "configurations" / name
    result = subprocess.run(["cmake", *arguments, "-B", str(build),
                             "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (output / f"configure-{name}.log").write_text(result.stdout)
    if result.returncode:
        raise ValueError(f"CMake configuration {name} failed:\n{result.stdout}")
    return name, build


def arm_include_directories(build):
    """Ask the real cross-compiler for its C++ multilib and newlib headers."""
    entry = next(entry for entry in json.loads((build / "compile_commands.json").read_text())
                 if source_file(entry).is_relative_to(ROOT / "platform/hal"))
    arguments = entry.get("arguments") or shlex.split(entry["command"])
    architecture = [arg for arg in arguments if arg.startswith(("-mcpu=", "-mfpu=", "-mfloat-abi=", "-mthumb"))]
    result = subprocess.run([arguments[0], *architecture, "-E", "-x", "c++", "-v", "/dev/null"],
                            capture_output=True, text=True, check=True, env={**os.environ, "LC_ALL": "C"})
    marker = "#include <...> search starts here:\n"
    if marker not in result.stderr or "End of search list." not in result.stderr:
        raise ValueError("The Arm compiler did not report its header search paths")
    search = result.stderr.split(marker, 1)[1].split("End of search list.", 1)[0]
    # Clang provides its own builtin headers. GCC's arm intrinsics must not
    # replace those; retain the target's actual standard library and C headers.
    builtins = {Path(subprocess.check_output([arguments[0], f"-print-file-name={name}"], text=True).strip()).resolve()
                for name in ("include", "include-fixed")}
    includes = [Path(line.strip()).resolve() for line in search.splitlines() if line.strip()]
    includes = [directory for directory in includes if directory not in builtins]
    if not includes or any(not directory.is_dir() for directory in includes):
        raise ValueError("Cannot locate the Arm compiler's standard library headers")
    return includes


def adapt_reflection_header(source, replacements):
    """Adjust library plumbing only; retain the compiler's real reflection code."""
    for original, replacement in replacements.items():
        if original not in source:
            raise ValueError(f"Reflection header changed; missing {original!r}. Revalidate the adapter.")
        source = source.replace(original, replacement)
    return "// Generated for linting from the pinned compiler's headers.\n" + source


def prepare_reflection_headers(output, library=REFLECTION_HEADERS):
    # GCC's <meta> uses different compiler intrinsics. Clang's implementation
    # works with libstdc++ when its includes and namespace wrappers are adapted.
    # Do not replace reflection functions, standard types or application code.
    namespace = {"_LIBCPP_BEGIN_NAMESPACE_STD": "namespace std {",
                 "_LIBCPP_END_NAMESPACE_STD": "}"}
    meta = adapt_reflection_header((library / "meta").read_text(), {
        "#include <__config>": '#include <climits>\n#include <concepts>\n#include <ranges>\n'
                                '#include "clang_consteval_traits.hpp"',
        "#include <__ranges/access.h>": "",
        "#include <__ranges/concepts.h>": "",
        "#include <__ranges/size.h>": "",
        "_LIBCPP_BEGIN_NAMESPACE_REFLECTION_V2": "namespace std { namespace meta { inline namespace reflection_v2 {",
        "_LIBCPP_END_NAMESPACE_REFLECTION_V2": "} } }",
        **namespace})
    trait = adapt_reflection_header((library / "__type_traits/is_consteval_only.h").read_text(), {
        "#include <__config>": "",
        "#include <__type_traits/integral_constant.h>": "#include <type_traits>",
        "#if _LIBCPP_STD_VER >= 26": "#if __cplusplus > 202302L",
        "_LIBCPP_NO_SPECIALIZATIONS": "",
        **namespace})
    includes = output / "reflection-include"
    includes.mkdir(exist_ok=True)
    (includes / "meta").write_text(meta)
    (includes / "clang_consteval_traits.hpp").write_text(trait)


def source_file(entry):
    return (Path(entry["directory"]) / entry["file"]).resolve()


def prepare_database(build, output, extra_databases=(), arm_includes=()):
    selected = []
    for context, directory in [("linux", build), *extra_databases]:
        entries = json.loads((directory / "compile_commands.json").read_text())
        commands = []
        for entry in entries:
            source = source_file(entry)
            if not in_scope(source):
                continue
            if context != "linux" and not source.is_relative_to(ROOT / "platform/hal"):
                continue
            arguments = entry.get("arguments") or shlex.split(entry["command"])
            arm = any(arg.startswith("-mcpu=") for arg in arguments)
            if arm and not arm_includes:
                raise ValueError("Arm compilation commands require the cross-compiler's headers")
            target = (["--target=arm-none-eabi", "-nostdinc++"] if arm else ["-stdlib=libstdc++"])
            includes = arm_includes if arm else [output / "system-include"]
            for include in includes:
                target.extend(["-isystem", str(include)])
            commands.append({"directory": entry["directory"], "file": str(source), "arguments": [
                COMPILER, "-fexpansion-statements", "-fannotation-attributes",
                "-isystem", str(output / "reflection-include"), "-Wno-c23-extensions",
                *target, *arguments[1:]]})
        if len({item["file"] for item in commands}) != len(commands):
            raise ValueError(f"Multiple commands for one source in {context}")
        database = output / "databases" / context
        database.mkdir(parents=True, exist_ok=True)
        (database / "compile_commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        selected.extend({**command, "context": context, "database": str(database)} for command in commands)
    if not selected:
        raise ValueError("No src/ or platform/hal/ sources found; configure the debug-linux preset first")
    # An explicit inventory prevents new platform-only files from silently
    # falling outside the selected build configurations.
    if extra_databases:
        expected = {source.resolve() for source in (ROOT / "platform/hal").rglob("*")
                    if source.suffix in (".cpp", ".cc", ".c", ".cxx") and in_scope(source.resolve())}
        missing = expected - {Path(item["file"]) for item in selected}
        if missing:
            raise ValueError("HAL sources missing compilation commands: " + ", ".join(
                str(source.relative_to(ROOT)) for source in sorted(missing)))
    (output / "analysis.json").write_text(json.dumps(selected, indent=2) + "\n")
    # Older versions wrote one host-only database here. Keep editors and manual
    # invocations from accidentally selecting that obsolete analysis context.
    (output / "compile_commands.json").unlink(missing_ok=True)


def collect_diagnostics(log):
    diagnostics = []
    for match in DIAGNOSTIC.finditer(log):
        filename, line, column, message, checks = match.groups()
        filename = Path(filename).resolve()
        if not in_scope(filename) or "clang-diagnostic-error" in checks:
            continue
        diagnostics.append({"file": str(filename.relative_to(ROOT)), "line": int(line),
                            "column": int(column), "message": message,
                            "checks": checks.replace(",-warnings-as-errors", "")})
    return diagnostics


def classify(code, log):
    if code not in (0, 1):
        return "tool-failure"
    if "[clang-diagnostic-error]" in log or "Error while processing " in log:
        return "blocked"
    # A nonzero status is normal with WarningsAsErrors, but a command-line error,
    # invalid config, or missing compilation database must not pass unnoticed.
    if code and not DIAGNOSTIC.search(log):
        return "tool-failure"
    return "parsed"


def run_command(command, directory, timeout):
    started = time.monotonic()
    try:
        process = subprocess.run(command, cwd=directory, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, timeout=timeout)
        code, log = process.returncode, process.stdout
    except subprocess.TimeoutExpired as error:
        def decoded(value):
            return value.decode(errors="replace") if isinstance(value, bytes) else (value or "")
        code = 124
        log = decoded(error.stdout) + decoded(error.stderr) + "\nANALYSIS TIMED OUT\n"
    except OSError as error:
        code, log = 127, f"Could not run analysis tool: {error}\n"
    return code, log, time.monotonic() - started


def exit_status(reports, findings, strict):
    if any(report["status"] == "tool-failure" for report in reports):
        return 2
    if not any(report["status"] == "parsed" for report in reports):
        return 2
    if strict and any(report["status"] == "blocked" for report in reports):
        return 2
    return 1 if findings else 0


def check(entry, args):
    source = Path(entry["file"])
    filename = str(source.relative_to(ROOT))
    context = entry.get("context", "linux")
    output = args.output / context / filename.replace("/", "__")
    output.parent.mkdir(parents=True, exist_ok=True)
    fixes = output.with_suffix(output.suffix + ".yaml")
    fixes.unlink(missing_ok=True)
    command = [TIDY, "-p", entry.get("database", str(args.output)), "--config-file=" + str(ROOT / ".clang-tidy"),
               "--header-filter=" + header_filter(),
               "--exclude-header-filter=" + EXCLUDED_HEADERS,
               "--export-fixes=" + str(fixes), str(source)]
    code, log, tidy_seconds = run_command(command, entry["directory"], args.timeout)
    with OUTPUT_LOCK:
        print(log, end="", flush=True)
    output.with_suffix(output.suffix + ".log").write_text(log)
    blockers = sorted({line.strip() for line in log.splitlines()
                       if "[clang-diagnostic-error]" in line or ("error:" in line and "[clang-diagnostic-" in line)})
    return {"file": filename, "context": context, "status": classify(code, log), "exit_code": code,
            "tidy_seconds": tidy_seconds, "blockers": blockers, "diagnostics": collect_diagnostics(log)}


def run_inside(args):
    try:
        prepare_reflection_headers(args.output)
    except (OSError, ValueError) as error:
        print(f"Clang-tidy reflection setup failed: {error}", file=sys.stderr)
        return 2
    version = subprocess.check_output([TIDY, "--version"], text=True).strip()
    verification = subprocess.run([TIDY, "--verify-config", f"--config-file={ROOT / '.clang-tidy'}"],
                                  capture_output=True, text=True)
    if verification.returncode:
        print(verification.stdout + verification.stderr, file=sys.stderr)
        return 2
    entries = json.loads((args.output / "analysis.json").read_text())
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        reports = list(pool.map(lambda entry: check(entry, args), entries))
    parsed = [report for report in reports if report["status"] == "parsed"]
    unique = {}
    for report in parsed:
        for diagnostic in report["diagnostics"]:
            unique[json.dumps(diagnostic, sort_keys=True)] = diagnostic
    findings = sorted(unique.values(), key=lambda item: (item["file"], item["line"], item["checks"]))
    partial = {}
    for report in reports:
        if report["status"] != "blocked":
            continue
        for diagnostic in report["diagnostics"]:
            key = json.dumps(diagnostic, sort_keys=True)
            if key not in unique:
                partial[key] = diagnostic
    def formatted(items):
        return "\n".join(f"{item['file']}:{item['line']}:{item['column']}: "
                         f"{item['message']} [{item['checks']}]" for item in items) + "\n"
    findings_text = formatted(findings)
    (args.output / "findings.txt").write_text(findings_text)
    (args.output / "partial-findings.txt").write_text(
        "REVIEW ONLY: these files did not parse successfully; diagnostics can be misleading.\n"
        + formatted(partial.values()))
    blocked = [report for report in reports if report["status"] == "blocked"]
    failures = [report for report in reports if report["status"] == "tool-failure"]
    summary = (f"Clang-tidy: {len(parsed)}/{len(reports)} translation units fully parsed; "
               f"{len(blocked)} blocked; {len(failures)} tool failures.\n"
               f"{len(findings)} unique src/ and platform/hal/ findings in fully parsed units.\n"
               f"{len(partial)} additional diagnostics from blocked units (review only).\n"
               f"Coverage: {'PARTIAL' if blocked or failures else 'COMPLETE'}. "
               f"Mode: {'strict' if args.strict else 'partial coverage accepted'}.\n")
    blocked_text = "\n".join(report["context"] + ": " + report["file"] + "\n  " + "\n  ".join(report["blockers"])
                             for report in blocked)
    (args.output / "summary.txt").write_text(summary + "\nBlocked files:\n" + blocked_text + "\n")
    (args.output / "report.json").write_text(json.dumps({"image": IMAGE, "compiler": version,
        "standard_library": "libstdc++", "reflection_headers": "Clang/P2996; adapted includes and namespaces",
        "scope": [str(directory.relative_to(ROOT)) for directory in SOURCE_ROOTS],
        "excluded_directories": ["test", "tests"],
        "coverage": "partial" if blocked or failures else "complete",
        "findings": findings, "partial_findings": list(partial.values()), "files": reports}, indent=2) + "\n")
    return exit_status(reports, findings, args.strict)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/debug-linux")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--inside-container", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.build_dir = args.build_dir.resolve()
    args.output = args.build_dir / "clang-tidy"
    if args.jobs < 1 or args.timeout < 1:
        parser.error("jobs and timeout must be positive")
    if args.inside_container:
        return run_inside(args)
    try:
        if not args.build_dir.is_relative_to(ROOT / "build"):
            raise ValueError("Use a build directory under the project's build/ directory")
        if not shutil.which("docker"):
            raise ValueError("Docker is required for the reflection-capable Pneumo toolchain")
        args.output.mkdir(parents=True, exist_ok=True)
        for name in ("findings.txt", "partial-findings.txt", "summary.txt", "report.json"):
            (args.output / name).unlink(missing_ok=True)
        for stale in args.output.glob("*.preflight.log"):
            stale.unlink()
        includes = args.output / "system-include"
        includes.mkdir(exist_ok=True)
        # The Pneumo image has no ncurses development headers. Use the genuine
        # headers of the host Linux build, without importing host C++ headers.
        for name in ("curses.h", "ncurses_dll.h", "unctrl.h"):
            shutil.copyfile(Path("/usr/include") / name, includes / name)
        databases = [configure_database(args.output, "stm32", ["--preset", "debug-stm32"])]
        arm_includes = arm_include_directories(databases[0][1])
        prepare_database(args.build_dir, args.output, databases, arm_includes)
        # Mount only header trees, never the host /usr over the image toolchain.
        mounts = [directory for directory in arm_includes if not any(
            directory != parent and directory.is_relative_to(parent) for parent in arm_includes)]
        header_mounts = [argument for directory in mounts for argument in
                         ("--mount", f"type=bind,src={directory},dst={directory},readonly")]
        command = ["docker", "run", "--rm", "--pull=never", "--network=none", "--read-only",
                   "--cap-drop=ALL", "--security-opt=no-new-privileges", "--tmpfs", "/tmp",
                   "--user", f"{os.getuid()}:{os.getgid()}",
                   "--mount", f"type=bind,src={ROOT},dst={ROOT},readonly",
                   "--mount", f"type=bind,src={args.output},dst={args.output}",
                   *header_mounts,
                   "--workdir", str(ROOT), IMAGE,
                   "python3", str(Path(__file__).resolve()), "--inside-container",
                   "--build-dir", str(args.build_dir), "--jobs", str(args.jobs),
                   "--timeout", str(args.timeout)]
        if args.strict:
            command.append("--strict")
        return subprocess.run(command, check=False).returncode
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Clang-tidy setup failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
