# file: setup.py
# prepare the Python environment and select, build or install an experiment kernel
# launch with `make setup` or `python -B -m scripts.setup`

"""
Interactive setup using the shared defaults and machine-local configuration.
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
from urllib.request import urlopen
import venv

from scripts.commons import confirm, load_config, load_json, log, project_config, project_path, project_root, run, save_json

# [CONFIGURATION] =======================
# Paths and revision choices come from commons and config.default/local.json.
# Only feature requirements and Linux installation ABI locations are fixed here.
required_features = (
    "TRANSPARENT_HUGEPAGE", "TRANSPARENT_HUGEPAGE_MADVISE", "USERFAULTFD",
    "PROC_PAGE_MONITOR", "PERF_EVENTS",
)
hyper_v_features = (
    "HYPERV", "HYPERV_VMBUS", "HYPERV_STORAGE", "HYPERV_NET",
    "HYPERV_KEYBOARD", "HYPERV_UTILS", "HYPERV_BALLOON",
)
boot_file_stems = ("vmlinuz", "vmlinux", "System.map", "config", "initrd.img")
# =======================================


def check_kernel(config_path: Path) -> bool:
    """
    Check required experiment features;
    NOTE: absent Hyper-V features only produce warnings.
    """
    # Parse enabled options without treating commented-out options as enabled.
    if not config_path.is_file():
        log("setup", "WARN", f"Kernel configuration is unavailable: {config_path}")
        return False
    try:
        config_lines = config_path.read_text().splitlines()
    except OSError as error:
        log("setup", "WARN", f"Could not read {config_path}: {error}")
        return False
    options = {}
    for line in config_lines:
        if line.startswith("CONFIG_") and "=" in line:
            name, value = line.split("=", 1)
            options[name.removeprefix("CONFIG_")] = value

    # The experiment options are built-in booleans; VM drivers may be modules.
    missing = [name for name in required_features if options.get(name) != "y"]
    for name in missing:
        log("setup", "WARN", f"Required CONFIG_{name}=y is absent")
    for name in hyper_v_features:
        if options.get(name) not in {"y", "m"}:
            log("setup", "WARN", f"Optional Hyper-V support CONFIG_{name} is absent")
    if not missing:
        log("setup", "OK", f"Required kernel features are enabled in {config_path}")
    return not missing


def setup_python(config: dict[str, Any]) -> None:
    """
    Create the selected virtual environment and persist its flag after pip succeeds.
    """
    python_config = config["environment"]["python"]
    if python_config.get("installed"):
        log("setup", "INFO", f"Python setup is already recorded: {python_config['venv_path']}")
        return

    # Resolve user input against the project and never repurpose a populated folder.
    log("setup", "INFO", "Setting up venv environment")
    log("setup", "INFO", "Enter a path or leave blank to use the default")
    selection = input(f"Virtual environment path [{python_config['venv_path']}]: ").strip()
    venv_path = project_path(selection or python_config["venv_path"])
    if venv_path == project_root or venv_path in project_root.parents:
        raise ValueError("The virtual environment must not be the project or one of its ancestors")
    if venv_path.exists() and (not venv_path.is_dir() or any(venv_path.iterdir())):
        if not (venv_path / "pyvenv.cfg").is_file():
            raise ValueError(f"Refusing to overwrite a non-venv directory: {venv_path}")
        if not confirm(f"Reuse the existing virtual environment at {venv_path} and install requirements?"):
            log("setup", "INFO", "Setup cancelled; run make setup again when ready")
            return
    requirements_path = project_path(python_config["requirements"])
    if not requirements_path.is_file():
        raise ValueError(f"Requirements file is unavailable: {requirements_path}")

    # Keep failed installs unflagged so a subsequent invocation can retry them.
    log("setup", "INFO", f"Preparing Python in {venv_path}")
    venv.EnvBuilder(with_pip = True).create(venv_path)
    run([str(venv_path / "bin/python"), "-B", "-m", "pip", "install",
         "--no-cache-dir", "--no-compile", "-r", str(requirements_path)])
    python_config.update({"venv_path": str(venv_path), "installed": True})
    save_json(project_root / project_config[1], config)
    log("setup", "OK", "Python environment and requirements are installed")


def default_kernel_name(kernel_config: dict[str, Any]) -> str:
    """
    Read only the pinned revision's Makefile, without fetching a Git source tree.
    """
    # The metadata endpoint is configurable independently of the clone repository.
    revision = kernel_config["revision"]
    if not re.fullmatch(r"[0-9a-fA-F]{40}", revision):
        raise ValueError("The default kernel revision must be a full 40-character commit hash")
    url_template = kernel_config.get("version_makefile_url", "")
    if not url_template.startswith("https://") or "{revision}" not in url_template:
        raise ValueError("kernel.version_makefile_url must be an HTTPS Makefile URL containing {revision}")
    url = url_template.replace("{revision}", revision)

    # Bound the lookup and retain nothing on disk; errors leave the menu usable.
    size_limit = 256 * 1024
    with urlopen(url, timeout = 15) as response:
        payload = response.read(size_limit + 1)
    if len(payload) > size_limit:
        raise ValueError("The version metadata response exceeds the Makefile size limit")
    makefile = payload.decode("utf-8")
    values = dict(re.findall(r"^(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION)[ \t]*=[ \t]*([^\n]*)", makefile, re.MULTILINE))
    for key in ("VERSION", "PATCHLEVEL", "SUBLEVEL"):
        if not re.fullmatch(r"[0-9]+", values.get(key, "").strip()):
            raise ValueError(f"The metadata response does not contain a valid kernel {key}")
    return ".".join(values[key].strip() for key in ("VERSION", "PATCHLEVEL", "SUBLEVEL")) + values.get("EXTRAVERSION", "").strip()


def build_kernel(source_path: Path, build_path: Path, kernel_config: dict[str, Any]) -> bool:
    """
    Preserve a base VM configuration, set experiment options and build with chosen jobs.
    """
    # Obtain the label and CPU count before any expensive compilation begins
    log("setup", "INFO", "Configuring the kernel build")
    log("setup", "INFO", "Enter kernel name or leave blank to use default")
    label = input(f"Custom kernel name [{kernel_config['name']}]: ").strip() or kernel_config["name"]
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.+-]{0,39}", label):
        raise ValueError("Use 1-40 letters, digits, dots, underscores, plus signs or hyphens; start with a letter or digit")
    log("setup", "INFO", "Enter the number of build threads or leave blank to use default")
    jobs_text = input(f"Build threads [{os.cpu_count() or 1}]: ").strip()
    jobs = int(jobs_text) if jobs_text else (os.cpu_count() or 1)
    if jobs < 1:
        raise ValueError("The build thread count must be positive")
    build_path.mkdir(parents = True, exist_ok = True)
    make_command = ["make", "-C", str(source_path), "O=" + str(build_path)]

    # Existing build configs take priority; otherwise copy the current VM's config.
    config_path = build_path / ".config"
    if source_path != build_path and (source_path / ".config").exists():
        raise ValueError("This source tree is configured in place; select the checkout itself as the build directory or use a fresh source tree")
    if not config_path.exists():
        candidates = [source_path / ".config", Path("/lib/modules") / os.uname().release / "build/.config",
                      Path("/boot") / ("config-" + os.uname().release)]
        base_path = next((path for path in candidates if path.is_file()), None)
        if base_path is None:
            supplied_path = input("Path to a base kernel .config: ").strip()
            if not supplied_path:
                raise ValueError("A base kernel configuration is required")
            base_path = project_path(supplied_path)
        shutil.copyfile(base_path, config_path)
    else:
        if not confirm(f"Update {config_path} for the experiment (a backup will be retained)?"):
            log("setup", "INFO", "Build cancelled; run make setup again when ready")
            return False
        # Keep the first pre-setup configuration across repeated invocations.
        backup_path = config_path.with_name(".config.before-thp")
        if not backup_path.exists():
            shutil.copy2(config_path, backup_path)

    # Use the kernel's own configuration utility, retaining existing VM drivers.
    command = [str(source_path / "scripts/config"), "--file", str(config_path)]
    for name in required_features:
        command.extend(["--enable", name])
    command.extend(["--enable", "MODULES", "--set-str", "LOCALVERSION", "-" + label,
                    "--disable", "LOCALVERSION_AUTO"])
    # Ubuntu certificate file names are not available in a generic upstream tree.
    command.extend(["--set-str", "SYSTEM_TRUSTED_KEYS", "", "--set-str", "SYSTEM_REVOCATION_KEYS", ""])
    run(command)
    run(make_command + ["olddefconfig"])
    if not check_kernel(config_path):
        if not confirm("Required features are missing after configuration. Build anyway?"):
            log("setup", "INFO", "Run make setup again after correcting the configuration")
            return False

    # Compilation is explicit; no experiments or module loading occur here.
    log("setup", "WARN", "Kernel compilation may take hours and tens of GiB of disk space")
    run(make_command + [f"-j{jobs}"])
    return True


def install_kernel(config: dict[str, Any], source_path: Path, build_path: Path) -> None:
    """
    Install an explicitly approved build and save local kernel selection on success.
    """
    make_command = ["make", "-C", str(source_path), "O=" + str(build_path)]
    release = run(make_command + ["-s", "--no-print-directory", "kernelrelease"], capture = True)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", release):
        raise ValueError(f"Unexpected kernel release: {release!r}")
    if release == os.uname().release:
        raise ValueError("Refusing to overwrite the running kernel; rebuild with a unique name")
    if not (build_path / "vmlinux").is_file() or not (build_path / "System.map").is_file():
        raise ValueError("This build directory has no completed vmlinux/System.map; choose the build flow")
    compiled_header = build_path / "include/generated/utsrelease.h"
    if not compiled_header.is_file() or f'#define UTS_RELEASE "{release}"' not in compiled_header.read_text():
        raise ValueError("The build's generated release does not match kernelrelease; rebuild before installing")

    # Read Git provenance before installation, allowing checkouts without a remote.
    revision = run(["git", "rev-parse", "HEAD"], cwd = source_path, capture = True)
    remote = subprocess.run(["git", "remote", "get-url", "origin"], cwd = source_path,
                            text = True, capture_output = True)
    repository = remote.stdout.strip() if remote.returncode == 0 else None
    log("setup", "INFO", f"Ready to install {release} from {build_path}")
    if not confirm("Install this kernel using sudo? This changes /boot, /lib/modules and GRUB"):
        log("setup", "INFO", f"Run make setup, choose option 3, enter {source_path}, answer yes to already built, and use build directory {build_path}")
        return

    # Claim only a fresh release; a separate journal permits partial-install cleanup.
    record_path = project_path(config["kernel"]["install_record"])
    if record_path.exists():
        raise ValueError("An installation journal already exists; use make uninstall before installing again")
    boot_paths = [Path("/boot") / f"{stem}-{release}{suffix}"
                  for stem in boot_file_stems for suffix in ("", ".old", ".bak")]
    modules_path = Path("/lib/modules") / release
    for path in [modules_path, *boot_paths]:
        if path.exists() or path.is_symlink():
            raise ValueError(f"Refusing to overwrite existing kernel files: {path}")
    # Preserve the targets of generic boot symlinks touched by installkernel hooks.
    boot_links = {}
    for directory in (Path("/"), Path("/boot")):
        for stem in boot_file_stems:
            for suffix in ("", ".old"):
                path = directory / (stem + suffix)
                if path.exists() and not path.is_symlink():
                    raise ValueError(f"A generic boot path is a regular file: {path}; refusing to risk replacing it")
                boot_links[str(path)] = os.readlink(path) if path.is_symlink() else None
    if shutil.which("update-grub") is None:
        raise ValueError("update-grub is required for this Ubuntu/GRUB installation flow")
    record = {"name": release, "checkout_path": str(source_path), "build_path": str(build_path),
              "repository": repository, "revision": revision, "project_root": str(project_root),
              "boot_links": boot_links}
    run(["sudo", "-v"])
    save_json(record_path, record)

    # The journal remains if an installation command fails; the installed flag does not.
    log("setup", "INFO", f"Installation recovery record: {record_path}")
    run(["sudo", *make_command, "modules_install"])
    run(["sudo", *make_command, "install"])
    # Some make-install hooks create an initramfs, while other systems need this step.
    initramfs_path = Path("/boot") / ("initrd.img-" + release)
    if not initramfs_path.exists() and shutil.which("update-initramfs"):
        run(["sudo", "update-initramfs", "-c", "-k", release])
    if not initramfs_path.exists():
        raise ValueError("No initramfs was created; installation is incomplete. Use make uninstall to clean up before retrying")
    run(["sudo", "update-grub"])
    config["kernel"].update({"repository": repository, "revision": revision,
                             "checkout_path": str(source_path), "build_path": str(build_path),
                             "headers_path": str(source_path / "include"),
                             "name": release, "installed": "installed"})
    save_json(project_root / project_config[1], config)
    log("setup", "OK", f"Installed {release}; restart and select it in GRUB to use it")
    log("setup", "INFO", "If the boot menu is hidden, review GRUB_TIMEOUT_STYLE and GRUB_TIMEOUT in /etc/default/grub, then run sudo update-grub")


def main() -> None:
    """
    Resume completed stages and offer stock, default or existing-checkout selection.
    """
    if os.geteuid() == 0:
        raise ValueError("Run make setup as your normal user; installation requests sudo when needed")
    config = load_config()
    defaults = load_json(project_root / project_config[0])
    setup_python(config)
    if not config["environment"]["python"].get("installed"):
        return
    kernel_config = config["kernel"]
    if kernel_config.get("installed") in {"stock", "installed"}:
        log("setup", "INFO", f"Kernel setup already completed: {kernel_config['name']} ({kernel_config['installed']})")
        return
    if project_path(kernel_config["install_record"]).exists():
        raise ValueError("An unfinished kernel installation exists; use make uninstall to clean it before retrying")

    # Query the pinned revision for the menu without keeping a second source tree.
    default_kernel = defaults["kernel"]
    log("setup", "INFO", "Reading the default kernel revision's Makefile")
    try:
        default_name = default_kernel_name(default_kernel)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        log("setup", "WARN", f"Could not read the remote version: {error}; stock/local selection remains available")
        default_name = f"{default_kernel['name']} (revision {default_kernel['revision']}, version unverified)"
    log("setup", "INFO", f"[1] Use current kernel \"{os.uname().release}\"")
    log("setup", "INFO", f"[2] Clone and use default kernel \"{default_name}\"")
    log("setup", "INFO", "[3] Specify a kernel checkout")
    selection = input("Select a kernel [1/2/3]: ").strip()

    # A rejected feature check must not persist a new kernel selection.
    if selection == "1":
        headers_path = Path("/lib/modules") / os.uname().release / "build"
        if not check_kernel(headers_path / ".config"):
            if not confirm("The current kernel did not pass the feature check. Continue anyway?"):
                log("setup", "INFO", "Kernel selection unchanged; run make setup again when ready")
                return
        kernel_config.update({"repository": None, "revision": None, "checkout_path": None,
                              "headers_path": str(headers_path), "name": os.uname().release, "installed": "stock"})
        save_json(project_root / project_config[1], config)
        log("setup", "OK", f"Selected stock kernel {os.uname().release}")
        return

    # Fetch exactly one commit instead of cloning the full kernel history.
    if selection == "2":
        if not re.fullmatch(r"[0-9a-fA-F]{40}", default_kernel["revision"]):
            raise ValueError("The default kernel revision must be a full 40-character commit hash")
        log("setup", "INFO", "Enter the path to the kernel checkout or leave blank to use default")
        supplied_path = input(f"Kernel checkout path [{default_kernel['checkout_path']}]: ").strip()
        source_path = project_path(supplied_path or default_kernel["checkout_path"])
        if source_path.exists():
            raise ValueError("The checkout destination exists; choose option 3 to use an existing checkout")
        if source_path == project_root or source_path in project_root.parents:
            raise ValueError("Unsafe kernel checkout path")
        source_path.mkdir(parents = True)
        run(["git", "init", "--quiet", str(source_path)])
        run(["git", "remote", "add", "origin", default_kernel["repository"]], cwd = source_path)
        run(["git", "fetch", "--depth=1", "origin", default_kernel["revision"]], cwd = source_path)
        run(["git", "checkout", "--detach", "FETCH_HEAD"], cwd = source_path)
        build_path = project_path(default_kernel["build_path"])
        already_built = False
        selected_config = default_kernel
    elif selection == "3":
        log("setup", "INFO", "Enter the path to an existing kernel checkout (absolute or relative to the project)")
        supplied_path = input("Kernel checkout path (absolute or relative to the project): ").strip()
        if not supplied_path:
            raise ValueError("A kernel checkout path is required")
        source_path = project_path(supplied_path)
        if not (source_path / "Makefile").is_file() or not (source_path / "scripts/config").is_file():
            raise ValueError(f"Not a Linux source checkout: {source_path}")
        git_root = run(["git", "rev-parse", "--show-toplevel"], cwd = source_path, capture = True)
        if Path(git_root).resolve() != source_path:
            raise ValueError("Select the root of the kernel Git checkout")
        already_built = confirm("Have you already built this kernel?")
        default_build = str(source_path) if (source_path / ".config").is_file() else kernel_config["build_path"]
        log("setup", "INFO", "Enter the path to the kernel build directory (contains .config) or leave blank to use default")
        supplied_build = input(f"Kernel build directory (contains .config) [{default_build}]: ").strip()
        build_path = project_path(supplied_build or default_build)
        selected_config = kernel_config
    else:
        raise ValueError("Choose 1, 2 or 3")

    # Out-of-tree builds stay in build/ by default; existing in-tree builds also work.
    if build_path == project_root or build_path in project_root.parents:
        raise ValueError("Unsafe kernel build path")
    if build_path in source_path.parents:
        raise ValueError("The build directory must not contain the source checkout")
    if already_built:
        if not check_kernel(build_path / ".config"):
            if not confirm("This built kernel did not pass the feature check. Continue to installation anyway?"):
                log("setup", "INFO", "Kernel selection unchanged; run make setup again after correcting the configuration")
                return
    elif not build_kernel(source_path, build_path, selected_config):
        return
    install_kernel(config, source_path, build_path)


# Entry point
if __name__ == "__main__":

    try:
        main()
    except (OSError, ValueError, KeyError, EOFError, subprocess.CalledProcessError) as error:
        log("setup", "ERROR", str(error))
        sys.exit(1)
    except KeyboardInterrupt:
        log("setup", "WARN", "Setup interrupted; completed stages remain recorded")
        sys.exit(130)
