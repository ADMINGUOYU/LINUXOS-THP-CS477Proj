# file: commons.py
# this universal helper module for general script run
# config management, logging, interaction, ... etc.

"""
Shared configuration, logging and process helpers for other scripts.
"""

from __future__ import annotations

# import libraries
import json
import os
from pathlib import Path
import subprocess
from typing import Any

# [CONFIGURATION] =======================
# NOTE: this is the only place to hard set paths
#       this path is needed to load the actual project configurations
# Configuration: all relative project paths are anchored here
# Path automatically resolved in respect to this file
project_root = Path(__file__).resolve().parents[1]
project_config = ("config.default.json", "config.local.json")  # relative to project_root
# LOGGING (This is the only place to hard set logging configuration)
logging_fancy = True  # enable ANSI colour codes in log output
logging_colours = {"INFO": 34, "WARN": 33, "OK": 32, "ERROR": 31}
# =======================================


# [LOGGING] (pretty printing is specified)
def log(module: str, level: str, message: str) -> None:
    """
    Print messages using the project's requested diagnostic format.
    """
    if logging_fancy:
        print(f"\033[1;{logging_colours[level]}m[{module}] {level}\033[0m: {message}", flush = True)
    else:
        print(f"[{module}] {level}: {message}", flush = True)


# [CONFIGURATION] =======================
def merge_config(base: dict[str, Any], overrides: dict[str, Any]) -> None:
    """
    Apply nested local overrides without discarding sibling settings.
    NOTE: should be without copying issue if the base entries are not mutable (i.e. str)
    """
    for key, value in overrides.items():
        if isinstance(value, dict) and isinstance(base.get(key), dict):
            merge_config(base[key], value)
        else:
            base[key] = value


def load_config() -> dict[str, Any]:
    """
    Read tracked defaults followed by optional untracked machine settings.
    NOTE: local configs will be merged into the default config
    """
    config = {}
    # read both and override the default with local settings if present
    for config_file in project_config:
        config_path = project_root / config_file
        if config_path.exists():
            merge_config(config, load_json(config_path))
    return config
# [CONFIGURATION] ENDs ==================


# [HELPER] parse and expand {kernel_release}
# may be used to resolve the path to current kernel's headers
def fill_current_os_release(value: str) -> str:
    """
    replaces {kernel_release} with the current kernel release
    returns the original value if {kernel_release} is not present
    pass in "{kernel_release}" to get the current kernel release
    """
    # try to expand {kernel_release}
    return value.replace("{kernel_release}", os.uname().release)


def project_path(value: str) -> Path:
    """
    Resolve a configured path relative to the project,
    expanding the kernel release.
    """
    path = Path(fill_current_os_release(value)).expanduser()
    return (project_root / path).resolve()
    

# [JSON helper] =========================
def save_json(path: Path, data: Any) -> None:
    """
    Writes a JSON document with given data
    Replace a JSON document atomically; do not leave partially written state.
    """
    path.parent.mkdir(parents = True, exist_ok = True)
    temporary_path = path.with_suffix(path.suffix + ".tmp")
    temporary_path.write_text(json.dumps(data, indent = 2) + "\n")
    temporary_path.replace(path)


def load_json(path: Path) -> Any:                                                                                                   
    """
    Reads a JSON document and returns the data
    """
    with path.open() as stream:
        return json.load(stream)
# [JSON helper] ENDs ====================


# [PROCESS] run a command
def run(command: list[str], cwd: Path | None = None, capture: bool = False) -> str:
    """
    Execute an argument vector without invoking a shell.

    Arguments:
        command: the command to run, as a list of strings
        cwd: the working directory to run the command in (default: current directory)
        capture: whether to capture and return the command's stdout (default: False)
    """
    result = subprocess.run(command, cwd = cwd, check = True, text = True,
                            stdout = subprocess.PIPE if capture else None)
    return result.stdout.strip() if capture else ""


# [INTERACTION] confirm an action
def confirm(message: str) -> bool:
    """
    Require an explicit affirmative response for an interactive action.
    """
    try:
        return input(f"{message} [y/N] ").strip().lower() in {"y", "yes"}
    except EOFError:
        return False


# Entry point (for testing)
if __name__ == "__main__":

    # normally should not run
    raise RuntimeError("This module is not intended to be run directly.")
