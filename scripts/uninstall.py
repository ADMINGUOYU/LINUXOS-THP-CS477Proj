# file: uninstall.py
# remove a selected virtual environment or experiment kernel and reset its config
# launch with `make uninstall` or `python -B -m scripts.uninstall`

"""
Interactive removal of resources recorded by the setup script.
"""

from __future__ import annotations

# import libraries
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any

from scripts.commons import confirm, load_config, load_json, log, project_config, project_path, project_root, run, save_json
from scripts.setup import boot_file_stems

# [CONFIGURATION] =======================
# Configuration files and project paths are provided by commons.
# Kernel installation records supply the selected release and source/build paths.
module_name = "uninstall"
# =======================================


def uninstall_python(config: dict[str, Any]) -> None:
    """
    Remove only a recognisable virtual environment, then clear its completion flag.
    """
    python_config = config["environment"]["python"]
    venv_path = project_path(python_config["venv_path"])
    if venv_path == project_root or venv_path in project_root.parents:
        raise ValueError("Refusing to remove the project or an ancestor as a virtual environment")
    if venv_path == Path(sys.prefix).resolve():
        raise ValueError("This interpreter is running from that environment; use make uninstall PYTHON=python3 with the system Python")
    if venv_path.exists() and not (venv_path / "pyvenv.cfg").is_file():
        raise ValueError(f"Refusing to remove a directory without pyvenv.cfg: {venv_path}")

    # Removing an already absent venv still clears a stale completion flag.
    if not confirm(f"Delete the virtual environment {venv_path} and clear its installed flag?"):
        return
    if venv_path.exists():
        shutil.rmtree(venv_path)
    python_config.pop("installed", None)
    save_json(project_root / project_config[1], config)
    log(module_name, "OK", "Virtual environment removed; run make setup to recreate it")


def clean_checkout(source_path: Path, build_path: Path) -> None:
    """
    Offer to retain sources, clean build output or delete the chosen checkout.
    """
    # Validate stored paths before passing them to make or deleting a directory.
    for path in (source_path, build_path):
        if path == project_root or path in project_root.parents or path == Path.home().resolve():
            raise ValueError(f"Refusing source/build cleanup at {path}")
    if not source_path.exists() and not build_path.exists():
        return
    log(module_name, "INFO", "[1] Keep source/build files  [2] Clean build cache  [3] Delete the entire checkout")
    selection = input("Source cleanup [1]: ").strip() or "1"
    if selection == "1":
        return
    if not (source_path / "scripts/config").is_file() or not (source_path / "Makefile").is_file():
        raise ValueError("The recorded source directory is no longer a Linux checkout; clean it manually")
    if selection == "2":
        if confirm(f"Run make clean in {build_path}? The kernel .config will be retained"):
            run(["make", "-C", str(source_path), "O=" + str(build_path), "clean"])
            log(module_name, "OK", "Kernel build cache cleaned")
    elif selection == "3":
        # Do not silently discard modifications or a separately located build tree.
        if build_path in source_path.parents:
            raise ValueError("The build directory contains the source checkout; clean these paths manually")
        if not confirm(f"Permanently delete {source_path}, including any local changes?"):
            return
        shutil.rmtree(source_path)
        if build_path.exists() and build_path != source_path and source_path not in build_path.parents:
            if not (build_path / ".config").is_file():
                raise ValueError("Separate build directory lacks .config; refusing automatic deletion")
            if confirm(f"Also permanently delete the separate kernel build directory {build_path}?"):
                shutil.rmtree(build_path)
        log(module_name, "OK", "Selected source files removed")
    else:
        raise ValueError("Choose 1, 2 or 3")


def uninstall_kernel(config: dict[str, Any], defaults: dict[str, Any]) -> None:
    """
    Remove a recorded custom release or simply reset a stock-kernel selection.
    """
    kernel_config = config["kernel"]
    record_path = project_path(kernel_config["install_record"])
    if kernel_config.get("installed") == "stock":
        # Stock selection is configuration only: never delete a distribution kernel.
        if not confirm("Reset the stock-kernel selection to defaults? The system kernel will remain installed"):
            return
        config["kernel"] = defaults["kernel"].copy()
        save_json(project_root / project_config[1], config)
        log(module_name, "OK", "Kernel configuration reset to defaults")
        return
    if not record_path.is_file():
        if kernel_config.get("installed") == "installed":
            raise ValueError("Installation record is missing; refusing to guess which system files to remove")
        log(module_name, "INFO", "No installed custom kernel is recorded")
        if confirm("Reset the kernel configuration to defaults?"):
            config["kernel"] = defaults["kernel"].copy()
            save_json(project_root / project_config[1], config)
        return

    # The same journal also supports cleanup after an interrupted installation.
    record = load_json(record_path)
    release = record["name"]
    if record.get("project_root") != str(project_root):
        raise ValueError("The installation record belongs to another project directory")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", release):
        raise ValueError("Invalid kernel release in the installation record")
    if release == os.uname().release:
        log(module_name, "WARN", f"Cannot remove running kernel {release}; boot another kernel and run make uninstall again")
        return
    if kernel_config.get("installed") == "installed" and kernel_config["name"] != release:
        raise ValueError("The selected kernel and installation record disagree")
    source_path = project_path(record["checkout_path"])
    build_path = project_path(record["build_path"])
    boot_paths = [Path("/boot") / f"{stem}-{release}{suffix}"
                  for stem in boot_file_stems for suffix in ("", ".old", ".bak")]
    modules_path = Path("/lib/modules") / release

    # A later package-manager installation must be removed by its package manager.
    for path in boot_paths:
        result = subprocess.run(["dpkg-query", "-S", str(path)], text = True, capture_output = True)
        if result.returncode == 0:
            raise ValueError(f"{path} is now owned by a package; use the package manager to remove it")
    log(module_name, "INFO", f"Removing {release}: matching images/configuration/initramfs in /boot and {modules_path}")
    if not confirm("Permanently remove this custom kernel using sudo and refresh GRUB?"):
        return
    if shutil.which("update-grub") is None:
        raise ValueError("update-grub is required before removing this kernel")
    initramfs_record = Path("/var/lib/initramfs-tools") / release
    if initramfs_record.exists() and shutil.which("update-initramfs") is None:
        raise ValueError("update-initramfs is required to remove the recorded initramfs state cleanly")

    # Remove the initramfs through its tool so its own tracking state is also cleaned.
    run(["sudo", "-v"])
    if shutil.which("update-initramfs") and (
        (Path("/boot") / f"initrd.img-{release}").exists() or (Path("/var/lib/initramfs-tools") / release).exists()
    ):
        run(["sudo", "update-initramfs", "-d", "-k", release])
    run(["sudo", "rm", "-f", "--", *[str(path) for path in boot_paths]])
    run(["sudo", "rm", "-rf", "--", str(modules_path)])

    # Repair distribution-maintained default-kernel symlinks before rebuilding GRUB.
    for directory in (Path("/"), Path("/boot")):
        for stem in boot_file_stems:
            for suffix in ("", ".old"):
                link_path = directory / (stem + suffix)
                if link_path.is_symlink() and link_path.resolve() in boot_paths:
                    previous_target = record.get("boot_links", {}).get(str(link_path))
                    if previous_target and (link_path.parent / previous_target).exists():
                        run(["sudo", "ln", "-sfn", "--", previous_target, str(link_path)])
                    else:
                        run(["sudo", "rm", "--", str(link_path)])
    if shutil.which("linux-update-symlinks"):
        run(["sudo", "linux-update-symlinks", "remove", release, str(Path("/boot") / f"vmlinuz-{release}")])
    run(["sudo", "update-grub"])

    # Reset only after all system-removal commands succeed; preserve recovery on error.
    config["kernel"] = defaults["kernel"].copy()
    save_json(project_root / project_config[1], config)
    record_path.unlink()
    log(module_name, "OK", f"Removed {release}; kernel configuration reset to defaults")
    clean_checkout(source_path, build_path)


def main() -> None:
    """
    Select one resource to uninstall without touching the other completed stage.
    """
    if os.geteuid() == 0:
        raise ValueError("Run make uninstall as your normal user; system removal requests sudo")
    config = load_config()
    defaults = load_json(project_root / project_config[0])
    kernel_name = config["kernel"]["name"]
    record_path = project_path(config["kernel"]["install_record"])
    if record_path.is_file():
        kernel_name = load_json(record_path)["name"]
    log(module_name, "INFO", f"[1] Python virtual environment \"{config['environment']['python']['venv_path']}\"")
    log(module_name, "INFO", f"[2] Installed/selected kernel \"{kernel_name}\"")
    selection = input("Select what to uninstall [1/2]: ").strip()
    if selection == "1":
        uninstall_python(config)
    elif selection == "2":
        uninstall_kernel(config, defaults)
    else:
        raise ValueError("Choose 1 or 2")


# Entry point
if __name__ == "__main__":

    try:
        main()
    except (OSError, ValueError, KeyError, EOFError, subprocess.CalledProcessError) as error:
        log(module_name, "ERROR", str(error))
        sys.exit(1)
    except KeyboardInterrupt:
        log(module_name, "WARN", "Removal interrupted; unfinished kernel removal retains its recovery record")
        sys.exit(130)
