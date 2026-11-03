# file: driver.py
"""
Launch isolated C samples, retain raw measurements and aggregate comparable runs.
Use --environment managed for supervised host changes, or observe to change nothing.
"""

from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import select
import signal
import statistics
import subprocess
import sys
import time

from scripts.commons import load_config, log, project_path, project_root

# [CONFIGURATION] =======================
test_names = ("correctness", "matrix", "chase", "hash", "allocation")
policy_names = ("disabled", "always", "khugepaged", "selective")
pattern_names = ("dense", "sparse", "random", "phase")
metric_names = ("active_total_ns", "total_ns", "cold_ns", "steady_ns", "rss_kib", "anon_huge_kib", "collapse_ns")
# [CONFIGURATION] ENDs ==================


def save_json(path: Path, value: object) -> None:
    """
    Publish a complete report with an atomic rename in the same directory.
    """
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent = 2, allow_nan = False) + "\n")
    temporary.replace(path)


def host_settings() -> dict[str, str]:
    """
    Record relevant settings without changing permissions or kernel configuration.
    """
    root = Path("/sys/kernel/mm/transparent_hugepage")
    paths = [root / "enabled", root / "defrag", root / "hpage_pmd_size",
             Path("/proc/sys/vm/unprivileged_userfaultfd"), Path("/proc/sys/kernel/perf_event_paranoid"),
             *sorted(root.glob("hugepages-*kB/enabled")),
             *(root / "khugepaged" / name for name in ("defrag", "pages_to_scan", "scan_sleep_millisecs",
                                                       "alloc_sleep_millisecs", "max_ptes_none", "max_ptes_swap", "max_ptes_shared"))]
    result = {}
    for path in paths:
        try:
            result[str(path)] = path.read_text().strip()
        except OSError as error:
            result[str(path)] = f"unavailable: {error}"
    return result


def terminate_child_on_parent_death(parent: int) -> None:
    """
    Linux child hook: an abruptly killed driver must not leave its workload running.
    """
    libc = ctypes.CDLL(None, use_errno = True)
    if libc.prctl(1, int(signal.SIGKILL), 0, 0, 0) != 0 or os.getppid() != parent:
        os._exit(127)


def supervisor_reply(supervisor: subprocess.Popen) -> dict:
    """
    Bound protocol waits; a failed supervisor must not leave the driver waiting forever.
    """
    if not select.select([supervisor.stdout], [], [], 30)[0]:
        raise RuntimeError("Timed out waiting for the environment supervisor")
    line = supervisor.stdout.readline()
    if not line:
        raise RuntimeError("Environment supervisor exited; inspect its diagnostics")
    return json.loads(line)


def interrupted(signum: int, frame: object) -> None:
    """
    Unwind the active sample and close the environment lease on catchable signals.
    """
    raise KeyboardInterrupt(f"Received signal {signum}")


def summary(samples: list[dict]) -> dict:
    """
    Aggregate successful samples by workload, pattern, policy and allocator only.
    """
    groups = {}
    for sample in samples:
        if sample["status"] != "ok" or sample["test"] == "correctness":
            continue
        key = (sample["test"], sample["pattern"], sample["policy"], sample["allocator"])
        groups.setdefault(key, []).append(sample["measurements"])
    rows = []
    for key, values in groups.items():
        metrics = {}
        for metric in metric_names:
            numbers = [value[metric] for value in values if metric in value]
            if numbers:
                metrics[metric] = {"median": statistics.median(numbers), "min": min(numbers),
                                   "max": max(numbers), "count": len(numbers)}
        rows.append({"test": key[0], "pattern": key[1], "policy": key[2], "allocator": key[3], "metrics": metrics})
    # The five-percent comparison is descriptive, not a significance claim.
    for row in rows:
        if row["policy"] != "selective":
            continue
        baseline = next((other for other in rows if other["test"] == row["test"] and
                         other["pattern"] == row["pattern"] and other["policy"] == "always"), None)
        if baseline and baseline["metrics"]["active_total_ns"]["median"] > 0:
            ratio = row["metrics"]["active_total_ns"]["median"] / baseline["metrics"]["active_total_ns"]["median"]
            row["active_total_ratio_to_always"] = ratio
            row["within_five_percent_of_always"] = ratio <= 1.05
            row["fewer_process_huge_kib_than_always"] = (row["metrics"]["anon_huge_kib"]["median"] <
                                                        baseline["metrics"]["anon_huge_kib"]["median"])
    return {"groups": rows, "note": "Active totals exclude the observation wait. Medians are descriptive; inspect coverage, counter availability and raw runs."}


def main() -> int:
    """
    Build a reproducible schedule and execute each sample in a fresh unprivileged process.
    """
    config = load_config()
    defaults = config.get("evaluation", {})
    parser = argparse.ArgumentParser(description = __doc__)
    parser.add_argument("--binary", action = "append", default = [], metavar = "TEST=PATH")
    parser.add_argument("--test", nargs = "+", choices = test_names)
    parser.add_argument("--policy", nargs = "+", choices = policy_names, default = list(policy_names))
    parser.add_argument("--pattern", nargs = "+", choices = pattern_names, default = list(pattern_names))
    parser.add_argument("--allocator", nargs = "+", choices = ("glibc", "tcmalloc"), default = ["glibc", "tcmalloc"])
    parser.add_argument("--tcmalloc", type = Path, default = defaults.get("tcmalloc_library"))
    parser.add_argument("--environment", choices = ("managed", "observe"), default = "managed")
    parser.add_argument("--enable-userfaultfd", action = "store_true", help = "Explicitly request the privileged sysctl override")
    parser.add_argument("--regions", type = int, default = defaults.get("regions", 32))
    parser.add_argument("--iterations", type = int, default = defaults.get("iterations", 100))
    parser.add_argument("--threads", type = int, default = defaults.get("threads", 8))
    parser.add_argument("--repetitions", type = int, default = defaults.get("repetitions", 5))
    parser.add_argument("--cpu", type = int, help = "Pin each C process and its threads to an allowed CPU")
    parser.add_argument("--wait-ms", type = int, default = defaults.get("wait_ms", 0))
    parser.add_argument("--seed", type = int, default = defaults.get("seed", 477))
    parser.add_argument("--timeout", type = float, default = defaults.get("timeout_seconds", 120))
    parser.add_argument("--results", type = Path, default = project_path(defaults.get("results_path", "./results")))
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Run the driver as your normal user; only the environment supervisor uses sudo")
    if min(args.regions, args.iterations, args.threads, args.repetitions) <= 0 or args.wait_ms < 0 or args.seed < 0 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("Sizes/repetitions/timeout must be positive; seed and wait must be nonnegative")
    if args.cpu is not None and args.cpu not in os.sched_getaffinity(0):
        parser.error("--cpu must be in the current allowed affinity set")
    if args.environment == "observe" and args.enable_userfaultfd:
        parser.error("--enable-userfaultfd requires managed environment mode")
    if config.get("policy", {}).get("region_bytes") != 2097152 and args.environment == "managed":
        parser.error("Managed host settings currently target 2 MiB PMD THPs")

    binaries = {}
    for item in args.binary:
        name, separator, path = item.partition("=")
        if not separator or name not in test_names:
            parser.error("Each --binary must be TEST=PATH with a known test name")
        binary = Path(path).resolve()
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"Executable unavailable: {binary}")
        binaries[name] = binary
    selected = args.test or list(binaries)
    if not selected or any(name not in binaries for name in selected):
        parser.error("Supply a compiled --binary for every selected test")
    library = args.tcmalloc.resolve() if args.tcmalloc else None
    if library is not None and (not library.is_file() or any(character in str(library) for character in " :\t\n")):
        parser.error("TCMalloc must be an existing shared library path without preload separators")

    # Fresh directories prevent mixing stale results with this invocation.
    destination = args.results.resolve() / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ") + f"-{os.getpid()}")
    destination.mkdir(parents = True, exist_ok = False)
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd = project_root, capture_output = True, text = True).stdout.strip()
    manifest = {"schema_version": 1, "kernel": platform.uname()._asdict(), "config": config,
                "revision": revision, "settings_before": host_settings(), "arguments": vars(args).copy(),
                "binary_sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in binaries.items()},
                "cpu_affinity": sorted(os.sched_getaffinity(0)), "samples": [], "complete": False}
    manifest["arguments"]["results"] = str(args.results)
    manifest["arguments"]["tcmalloc"] = str(library) if library else None
    manifest["source_status"] = subprocess.run(["git", "status", "--porcelain"], cwd = project_root, capture_output = True, text = True).stdout
    manifest["build_flags"] = {}
    for name, binary in binaries.items():
        flags = binary.parent / "flags.txt"
        manifest["build_flags"][name] = flags.read_text() if flags.is_file() else "unknown external build"
    manifest["instrumented_build"] = any("-fsanitize" in flags for flags in manifest["build_flags"].values())
    if library:
        manifest["tcmalloc_sha256"] = hashlib.sha256(library.read_bytes()).hexdigest()
    save_json(destination / "manifest.json", manifest)

    schedule = []
    for repetition in range(args.repetitions):
        for name in selected:
            for policy in args.policy:
                if name == "allocation" and policy not in ("disabled", "always"):
                    continue
                for pattern in (["churn"] if name == "allocation" else args.pattern):
                    for allocator in (args.allocator if name == "allocation" else ["container"]):
                        schedule.append((name, policy, pattern, allocator, repetition))
    if not schedule:
        parser.error("No compatible test/policy combinations selected")
    random.Random(args.seed).shuffle(schedule)
    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, interrupted)
    supervisor = None
    child = None
    failed = False
    try:
        if args.environment == "managed":
            log("evaluation", "WARN", "Managed run changes host THP settings temporarily; other applications are affected")
            # Sudo authenticates on the terminal; the pipe carries only fixed setting requests.
            subprocess.run(["sudo", "-v"], check = True)
            supervisor = subprocess.Popen(["sudo", "-n", sys.executable, "-I", "-B", str(Path(__file__).with_name("environment.py"))],
                                          stdin = subprocess.PIPE, stdout = subprocess.PIPE, text = True, start_new_session = True)
            greeting = supervisor_reply(supervisor)
            if not greeting.get("ready"):
                raise RuntimeError("Environment supervisor did not start; see its diagnostic above")
            manifest["environment_original"] = greeting["original"]
        for number, (name, policy, pattern, allocator, repetition) in enumerate(schedule):
            sample = {"test": name, "policy": policy, "pattern": pattern, "allocator": allocator,
                      "repetition": repetition, "status": "pending"}
            manifest["samples"].append(sample)
            if allocator == "tcmalloc" and library is None:
                sample.update(status = "unavailable", reason = "Supply --tcmalloc /absolute/path/to/libtcmalloc.so")
                log("evaluation", "WARN", sample["reason"])
                failed = True
                continue
            if supervisor is not None:
                supervisor.stdin.write(json.dumps({"policy": policy, "enable_userfaultfd": args.enable_userfaultfd}) + "\n")
                supervisor.stdin.flush()
                reply = supervisor_reply(supervisor)
                if reply.get("applied") != policy:
                    raise RuntimeError("Environment supervisor failed to apply the requested policy")
            sample["settings"] = host_settings()
            sample_path = destination / f"{number:05d}-{name}-{policy}-{pattern}-{allocator}.json"
            sample["result_file"] = sample_path.name
            command = [str(binaries[name]), "--results", str(sample_path), "--policy", policy,
                       "--pattern", "dense" if name == "allocation" else pattern, "--regions", str(args.regions),
                       "--iterations", str(args.iterations), "--threads", str(args.threads),
                       "--wait-ms", str(args.wait_ms), "--seed", str(args.seed + repetition)]
            sample["command"] = command
            environment = os.environ.copy()
            environment.pop("LD_PRELOAD", None)
            environment.pop("EVALUATION_ALLOCATOR_LIBRARY", None)
            environment["LC_ALL"] = "C"
            if allocator == "tcmalloc":
                environment["LD_PRELOAD"] = str(library)
                environment["EVALUATION_ALLOCATOR_LIBRARY"] = str(library)
            log("evaluation", "INFO", f"{name}: {policy}/{pattern}/{allocator}, repetition {repetition + 1}")
            parent = os.getpid()
            started = time.monotonic_ns()
            def child_setup() -> None:
                """
                Install the death signal before applying optional CPU affinity.
                """
                terminate_child_on_parent_death(parent)
                if args.cpu is not None:
                    os.sched_setaffinity(0, {args.cpu})

            with sample_path.with_suffix(".log").open("w") as diagnostics:
                child = subprocess.Popen(command, cwd = project_root, env = environment, stdout = diagnostics,
                                         stderr = subprocess.STDOUT, start_new_session = True,
                                         preexec_fn = child_setup)
                try:
                    returncode = child.wait(timeout = args.timeout)
                    sample["returncode"] = returncode
                    sample["process_wall_ns"] = time.monotonic_ns() - started
                    if returncode != 0:
                        raise RuntimeError(f"C process exited with status {returncode}")
                    data = json.loads(sample_path.read_text())
                    if data.get("complete") is not True or data.get("test") != name or data.get("runtime_errno", 0) != 0:
                        raise ValueError("Incomplete or invalid C measurements")
                    if data.get("policy") != policy_names.index(policy) or data.get("seed") != args.seed + repetition:
                        raise ValueError("Result does not match the scheduled policy/seed")
                    data["active_total_ns"] = data["total_ns"] - data.get("wait_ns", 0)
                    sample["warnings"] = []
                    if data.get("failed_regions", 0):
                        sample["warnings"].append("Some selected regions failed promotion; inspect region_stats")
                    if data.get("arena_memory_errno", 0):
                        sample["warnings"].append("Arena coverage unavailable; process totals remain available")
                    for counter in data.get("perf", []):
                        counter["scaled"] = (counter["raw"] * counter["time_enabled"] / counter["time_running"]
                                             if counter["errno"] == 0 and counter["time_running"] > 0 else None)
                        if counter["scaled"] is None:
                            sample["warnings"].append(f"{counter['event']} unavailable (errno {counter['errno']})")
                    sample.update(status = "ok", measurements = data)
                except (subprocess.TimeoutExpired, OSError, ValueError, RuntimeError) as error:
                    if isinstance(error, subprocess.TimeoutExpired):
                        # Capture wait locations before killing a potentially deadlocked process.
                        sample["thread_waits"] = {}
                        for task in Path(f"/proc/{child.pid}/task").glob("*"):
                            state = {}
                            for field in ("comm", "wchan", "syscall"):
                                try:
                                    state[field] = (task / field).read_text().strip()
                                except OSError as detail:
                                    state[field] = str(detail)
                            sample["thread_waits"][task.name] = state
                    sample.update(status = "failed", reason = str(error))
                    failed = True
                finally:
                    if child.poll() is None:
                        os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
                    child = None
            print(sample_path.with_suffix(".log").read_text(), end = "", flush = True)
            save_json(destination / "manifest.json", manifest)
        manifest["complete"] = True
    except (KeyboardInterrupt, OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        manifest["error"] = str(error)
        failed = True
        log("evaluation", "ERROR", str(error))
    finally:
        # Repeated termination signals must not interrupt host-state restoration.
        signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM, signal.SIGHUP})
        # Kill/reap workloads before releasing the host-setting lease.
        if child is not None and child.poll() is None:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
        if supervisor is not None:
            try:
                supervisor.stdin.close()
            except BrokenPipeError:
                pass
            try:
                supervisor.wait(timeout = 30)
            except subprocess.TimeoutExpired:
                failed = True
                manifest["restoration_error"] = "Supervisor still running; do not start another run. Inspect and recover its journal."
            if supervisor.returncode not in (0, None):
                failed = True
                manifest["restoration_error"] = "Supervisor failed; use make evaluation-recover before another managed run"
        manifest["settings_after"] = host_settings()
        manifest["all_samples_passed"] = (manifest["complete"] and not failed and
                                          all(sample["status"] == "ok" for sample in manifest["samples"]))
        save_json(destination / "manifest.json", manifest)
        aggregate = summary(manifest["samples"])
        aggregate["instrumented_build"] = manifest["instrumented_build"]
        aggregate["controlled_host_policies"] = args.environment == "managed"
        aggregate["sample_status_counts"] = {status: sum(sample["status"] == status for sample in manifest["samples"])
                                             for status in ("ok", "failed", "unavailable", "pending")}
        if args.environment != "managed" or manifest["instrumented_build"]:
            # Observed host policies or instrumentation cannot support policy claims.
            for row in aggregate["groups"]:
                for key in ("within_five_percent_of_always", "fewer_process_huge_kib_than_always",
                            "active_total_ratio_to_always"):
                    row.pop(key, None)
            aggregate["note"] += " Policy conclusions are disabled without controlled, uninstrumented host runs."
        if manifest["instrumented_build"]:
            aggregate["note"] += " Sanitiser results are diagnostic, not performance measurements."
        save_json(destination / "summary.json", aggregate)
        log("evaluation", "INFO", f"Reports: {destination}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
