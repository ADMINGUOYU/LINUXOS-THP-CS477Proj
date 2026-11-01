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

# -B avoids generating Python cache files during these administrative operations.
setup:
	cd "$(project_root)" && $(PYTHON) -B -m scripts.setup

uninstall:
	cd "$(project_root)" && $(PYTHON) -B -m scripts.uninstall
