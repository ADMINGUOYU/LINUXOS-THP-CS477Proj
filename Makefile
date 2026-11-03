# Python scripts are launched as modules from the project root
# ?= -> set if absent / := -> set immediately / = -> set at expansion time
PYTHON ?= python3
project_root := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# .PHONY is used to declare targets that are not actual files,
# so make will always execute their recipes.
.PHONY: help setup uninstall
.DEFAULT_GOAL := help

help:
	@printf '%s\n' 'make setup      Prepare Python and select/build/install a kernel' 'make uninstall  Remove the Python environment or selected kernel'
	@printf '%s\n' 'make evaluation-build        Compile all C evaluation executables only' 'make evaluation-correctness  Run correctness/concurrency samples' 'make evaluation-matrix       Run matrix-vector samples' 'make evaluation-chase        Run pointer-chase samples' 'make evaluation-hash         Run hash-lookup samples' 'make evaluation-allocation   Compare glibc and a supplied TCMalloc library' 'make evaluation-suite        Run all selected evaluations' 'make evaluation-recover      Restore a saved host-setting journal' 'Use EVAL_ARGS="--environment observe ..." to avoid host-setting changes.'

# -B avoids generating Python cache files during these administrative operations.
setup:
	cd "$(project_root)" && $(PYTHON) -B -m scripts.setup

uninstall:
	cd "$(project_root)" && $(PYTHON) -B -m scripts.uninstall

# Evaluation targets compile first; evaluation-build never launches tests or sudo.
include $(project_root)src/evaluation/common.mk
include $(project_root)src/evaluation/correctness.mk
include $(project_root)src/evaluation/benchmarks.mk

evaluation-build: $(evaluation_binaries)
