# file: common.mk
# Build configuration is editable independently of the Python run arguments.
# Example: EVAL_BUILD_MODE=asan EVAL_CFLAGS='-O1 -g -std=c99 -Wall -Wextra -Wpedantic -fsanitize=address,undefined,leak -fno-omit-frame-pointer'
# Instrumented executables are for diagnostics; use the release build for timings.

# NOTE: EVAL_ARGS provides overrides (even overrides the json file settings)

CC ?= cc
EVAL_BUILD_MODE ?= release
EVAL_CFLAGS ?= -O2 -g -std=c99 -Wall -Wextra -Wpedantic
EVAL_CPPFLAGS ?=
EVAL_LDFLAGS ?=
EVAL_LDLIBS ?= -pthread -ldl
EVAL_ARGS ?=
evaluation_build := $(project_root)build/evaluation/$(EVAL_BUILD_MODE)
evaluation_flags := $(evaluation_build)/flags.txt
evaluation_shared_objects := $(addprefix $(evaluation_build)/,commons.o cjson.o thp_container.o thp_handler.o evaluation/evaluation.o)
evaluation_binaries :=

.PHONY: evaluation-force evaluation-build evaluation-recover
evaluation-force:

# Changing flags refreshes this prerequisite; unchanged flags preserve incremental builds.
$(evaluation_flags): evaluation-force
	@mkdir -p "$(@D)"
	@printf '%s\n' "CC=$(CC)" "CPPFLAGS=$(CPPFLAGS) $(EVAL_CPPFLAGS)" "CFLAGS=$(EVAL_CFLAGS)" "LDFLAGS=$(EVAL_LDFLAGS)" "LDLIBS=$(EVAL_LDLIBS)" > "$@.pending"
	@cmp -s "$@.pending" "$@" || cp "$@.pending" "$@"
	@$(PYTHON) -B -c 'from pathlib import Path; Path("$@.pending").unlink()'

$(evaluation_build)/%.o: $(project_root)src/%.c $(evaluation_flags)
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(EVAL_CPPFLAGS) -DCOMMONS_PROJECT_ROOT='"$(project_root)"' -I"$(project_root)src" $(EVAL_CFLAGS) -pthread -MMD -MP -c "$<" -o "$@"

# Recovery only restores saved settings; it never launches a workload.
evaluation-recover:
	sudo $(PYTHON) -I -B "$(project_root)scripts/evaluation/environment.py" --recover
