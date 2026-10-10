#!/usr/bin/env python3
"""Run src/ clang-tidy checks with Pneumo's reflection compiler in Docker.

The default is a partial lint run: compiler-blocked files are reported separately.
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


ROOT = Path(__file__).resolve().parents[1]
# Keep the compiler, clang-tidy and libc++ from the same verified toolchain.
IMAGE = ("ghcr.io/daleondev/pneumo-devcontainer@"
         "sha256:1c10d580f47f4fa402da038896027629ded2d94de4ddd0f382f8a63830fae3e7")
COMPILER = "/opt/clang-p2996/bin/clang++"
TIDY = "/opt/clang-p2996/bin/clang-tidy"
DIAGNOSTIC = re.compile(
    r"^(.+?):(\d+):(\d+): (?:warning|error|fatal error): (.*?) \[([^\]]+)\]$", re.MULTILINE)


def source_file(entry):
    return (Path(entry["directory"]) / entry["file"]).resolve()


def prepare_database(build, output):
    entries = json.loads((build / "compile_commands.json").read_text())
    selected = []
    for entry in entries:
        source = source_file(entry)
        if not source.is_relative_to(ROOT / "src"):
            continue
        arguments = entry.get("arguments") or shlex.split(entry["command"])
        if any(argument.startswith(("-mcpu=", "-mthumb", "-mfpu=", "--sysroot="))
               for argument in arguments):
            raise ValueError("Use a Linux compilation database; this Clang image targets the host")
        selected.append({"directory": entry["directory"], "file": str(source), "arguments": [
            COMPILER, "-stdlib=libc++", "-fexpansion-statements", "-fannotation-attributes",
            "-Wno-c23-extensions", "-isystem", str(output / "system-include"), *arguments[1:]]})
    if not selected:
        raise ValueError("No src/ sources found; configure the debug-linux preset first")
    if len({item["file"] for item in selected}) != len(selected):
        raise ValueError("Multiple commands for one source; use a single Linux build configuration")
    (output / "compile_commands.json").write_text(json.dumps(selected, indent=2) + "\n")


def collect_diagnostics(log):
    diagnostics = []
    for match in DIAGNOSTIC.finditer(log):
        filename, line, column, message, checks = match.groups()
        filename = Path(filename).resolve()
        if not filename.is_relative_to(ROOT / "src") or "clang-diagnostic-error" in checks:
            continue
        diagnostics.append({"file": str(filename.relative_to(ROOT)), "line": int(line),
                            "column": int(column), "message": message,
                            "checks": checks.replace(",-warnings-as-errors", "")})
    return diagnostics


def classify(code, log):
    if code not in (0, 1):
        return "tool-failure"
    if "[clang-diagnostic-error]" in log:
        return "blocked"
    # A nonzero status is normal with WarningsAsErrors, but a command-line error,
    # invalid config, or missing compilation database must not pass unnoticed.
    if code and not DIAGNOSTIC.search(log):
        return "tool-failure"
    return "parsed"


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
    output = args.output / filename.replace("/", "__")
    fixes = output.with_suffix(output.suffix + ".yaml")
    fixes.unlink(missing_ok=True)
    command = [TIDY, "-p", str(args.output), "--config-file=" + str(ROOT / ".clang-tidy"),
               "--header-filter=^" + re.escape(str(ROOT / "src")) + "/",
               "--export-fixes=" + str(fixes), str(source)]
    try:
        process = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        code, log = process.returncode, process.stdout + process.stderr
    except subprocess.TimeoutExpired as error:
        code = 124
        log = (error.stdout or b"").decode(errors="replace")
        log += (error.stderr or b"").decode(errors="replace") + "\nANALYSIS TIMED OUT\n"
    output.with_suffix(output.suffix + ".log").write_text(log)
    status = classify(code, log)
    print(f"{status:12} {filename}", flush=True)
    blockers = sorted({line.strip() for line in log.splitlines() if "[clang-diagnostic-error]" in line})
    return {"file": filename, "status": status, "exit_code": code,
            "blockers": blockers, "diagnostics": collect_diagnostics(log)}


def run_inside(args):
    version = subprocess.check_output([TIDY, "--version"], text=True).strip()
    verification = subprocess.run([TIDY, "--verify-config", f"--config-file={ROOT / '.clang-tidy'}"],
                                  capture_output=True, text=True)
    if verification.returncode:
        print(verification.stdout + verification.stderr, file=sys.stderr)
        return 2
    entries = json.loads((args.output / "compile_commands.json").read_text())
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
    (args.output / "findings.txt").write_text(formatted(findings))
    (args.output / "partial-findings.txt").write_text(
        "REVIEW ONLY: these files did not parse successfully; diagnostics can be misleading.\n"
        + formatted(partial.values()))
    blocked = [report for report in reports if report["status"] == "blocked"]
    failures = [report for report in reports if report["status"] == "tool-failure"]
    summary = (f"Clang-tidy: {len(parsed)}/{len(reports)} translation units fully parsed; "
               f"{len(blocked)} blocked; {len(failures)} tool failures.\n"
               f"{len(findings)} unique src/ findings in fully parsed units.\n"
               f"{len(partial)} additional diagnostics from blocked units (review only).\n"
               f"Coverage: {'PARTIAL' if blocked or failures else 'COMPLETE'}. "
               f"Mode: {'strict' if args.strict else 'partial coverage accepted'}.\n")
    blocked_text = "\n".join(report["file"] + "\n  " + "\n  ".join(report["blockers"])
                             for report in blocked)
    (args.output / "summary.txt").write_text(summary + "\nBlocked files:\n" + blocked_text + "\n")
    (args.output / "report.json").write_text(json.dumps({"image": IMAGE, "compiler": version,
        "coverage": "partial" if blocked or failures else "complete",
        "findings": findings, "partial_findings": list(partial.values()), "files": reports}, indent=2) + "\n")
    print(summary, end="")
    print(f"Reports: {args.output}/{{findings.txt,partial-findings.txt,summary.txt,report.json}}")
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
        includes = args.output / "system-include"
        includes.mkdir(exist_ok=True)
        # The Pneumo image has no ncurses development headers. Use the genuine
        # headers of the host Linux build, without importing host C++ headers.
        for name in ("curses.h", "ncurses_dll.h", "unctrl.h"):
            shutil.copyfile(Path("/usr/include") / name, includes / name)
        prepare_database(args.build_dir, args.output)
        command = ["docker", "run", "--rm", "--pull=never", "--network=none", "--read-only",
                   "--cap-drop=ALL", "--security-opt=no-new-privileges", "--tmpfs", "/tmp",
                   "--user", f"{os.getuid()}:{os.getgid()}",
                   "--mount", f"type=bind,src={ROOT},dst={ROOT},readonly",
                   "--mount", f"type=bind,src={args.output},dst={args.output}",
                   "--workdir", str(ROOT), IMAGE,
                   "python3", str(Path(__file__).resolve()), "--inside-container",
                   "--build-dir", str(args.build_dir), "--jobs", str(args.jobs),
                   "--timeout", str(args.timeout)]
        if args.strict:
            command.append("--strict")
        return subprocess.run(command, check=False).returncode
    except (OSError, ValueError) as error:
        print(f"Clang-tidy setup failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
