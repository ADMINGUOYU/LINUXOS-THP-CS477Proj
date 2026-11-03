# file: environment.py
"""
Privileged, narrow host-setting supervisor, launched separately from unprivileged workloads.
A stdin lease keeps settings active. EOF, including driver death, triggers restoration.
SIGKILL or power loss of the supervisor requires explicit recovery from its journal.
"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import stat
import sys

# [CONFIGURATION] =======================
state_directory = Path("/var/lib/thp-evaluation")
thp_root = Path("/sys/kernel/mm/transparent_hugepage")
uffd_path = Path("/proc/sys/vm/unprivileged_userfaultfd")
boot_path = Path("/proc/sys/kernel/random/boot_id")
policy_values = {"disabled": "never", "always": "always", "khugepaged": "madvise", "selective": "madvise"}
# [CONFIGURATION] ENDs ==================


def read_setting(path: Path) -> str:
    """
    Read the selected sysfs choice or the scalar sysctl value.
    """
    value = path.read_text().strip()
    if "[" in value:
        return value.split("[", 1)[1].split("]", 1)[0]
    return value


def write_setting(path: Path, value: str) -> None:
    """
    Write one allowlisted setting and check that the kernel accepted it.
    """
    path.write_text(value + "\n")
    if read_setting(path) != value:
        raise RuntimeError(f"Kernel did not accept {path} = {value}")


def restore(journal: Path, allowed: set[str]) -> None:
    """
    Restore original settings; keep the journal if any write fails.
    """
    if not journal.exists():
        return
    record = json.loads(journal.read_text())
    if record["boot_id"] != boot_path.read_text().strip():
        # Runtime settings were reset by reboot; never apply stale previous-boot values.
        journal.unlink()
        return
    failures = []
    for name, value in reversed(list(record["original"].items())):
        if name not in allowed or not isinstance(value, str):
            raise RuntimeError("Recovery journal contains an unexpected setting")
        try:
            if read_setting(Path(name)) != value:
                write_setting(Path(name), value)
        except OSError as error:
            failures.append(f"{name}: {error}")
        except RuntimeError as error:
            failures.append(str(error))
    if failures:
        raise RuntimeError("Restoration incomplete; journal retained: " + "; ".join(failures))
    journal.unlink()


def interrupted(signum: int, frame: object) -> None:
    """
    Exit through finally so catchable signals restore settings.
    """
    raise SystemExit(128 + signum)


def main() -> int:
    """
    Hold an exclusive host lease, process fixed policy commands, then restore originals.
    """
    parser = argparse.ArgumentParser(description = __doc__)
    parser.add_argument("--recover", action = "store_true", help = "Restore the saved state after an interrupted supervisor")
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("This supervisor requires sudo; the evaluation driver must remain unprivileged")

    # A root-owned directory prevents unprivileged journal or lock replacement.
    os.umask(0o077)
    state_directory.mkdir(mode = 0o700, exist_ok = True)
    metadata = state_directory.lstat()
    if not stat.S_ISDIR(metadata.st_mode) or metadata.st_uid != 0 or metadata.st_mode & 0o077:
        raise RuntimeError(f"Expected a root-owned private directory: {state_directory}")
    descriptor = os.open(state_directory / "lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    journal = state_directory / "state.json"
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Disable smaller mTHP sizes during a managed 2 MiB experiment, restoring them later.
        size_paths = sorted(thp_root.glob("hugepages-*kB/enabled"))
        paths = [thp_root / "enabled", *size_paths]
        if uffd_path.exists():
            paths.append(uffd_path)
        allowed = {str(path) for path in paths}
        if args.recover:
            restore(journal, allowed)
            print(json.dumps({"restored": True}), flush = True)
            return 0
        if journal.exists():
            raise RuntimeError("Unrestored state exists; run the supervisor with --recover first")
        original = {str(path): read_setting(path) for path in paths}
        record = {"boot_id": boot_path.read_text().strip(), "original": original}
        # Persist the recovery record before the first system-setting write.
        with journal.open("x") as stream:
            json.dump(record, stream, indent = 2)
            stream.flush()
            os.fsync(stream.fileno())
        directory_fd = os.open(state_directory, os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(signum, interrupted)
        try:
            print(json.dumps({"ready": True, "original": original}), flush = True)
            for line in sys.stdin:
                command = json.loads(line)
                policy = command["policy"]
                if policy not in policy_values:
                    raise ValueError("Unknown policy")
                write_setting(thp_root / "enabled", policy_values[policy])
                for path in size_paths:
                    write_setting(path, "inherit" if path.parent.name == "hugepages-2048kB" else "never")
                if command.get("enable_userfaultfd", False):
                    write_setting(uffd_path, "1")
                print(json.dumps({"applied": policy, "settings": {str(path): read_setting(path) for path in paths}}), flush = True)
        finally:
            # Do not let a repeated Ctrl-C interrupt restoration halfway through.
            signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM, signal.SIGHUP})
            restore(journal, allowed)
    finally:
        os.close(descriptor)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(f"[evaluation environment] ERROR: {error}", file = sys.stderr, flush = True)
        raise SystemExit(1)
