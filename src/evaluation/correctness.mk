# file: correctness.mk
correctness_objects := $(evaluation_shared_objects) $(evaluation_build)/evaluation/correctness.o
correctness_binary := $(evaluation_build)/correctness
evaluation_binaries += $(correctness_binary)

$(correctness_binary): $(correctness_objects)
	$(CC) $(EVAL_CFLAGS) $(EVAL_LDFLAGS) $^ $(EVAL_LDLIBS) -o "$@"

.PHONY: evaluation-correctness
evaluation-correctness: $(correctness_binary)
	cd "$(project_root)" && $(PYTHON) -B -m scripts.evaluation.driver --test correctness --binary "correctness=$(correctness_binary)" $(EVAL_ARGS)

-include $(correctness_objects:.o=.d)
