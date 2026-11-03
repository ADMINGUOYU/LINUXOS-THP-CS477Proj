# file: benchmarks.mk
# Each workload owns its object list; only support modules are shared.
matrix_objects := $(evaluation_shared_objects) $(evaluation_build)/evaluation/matrix.o
chase_objects := $(evaluation_shared_objects) $(evaluation_build)/evaluation/chase.o
hash_objects := $(evaluation_shared_objects) $(evaluation_build)/evaluation/hash.o
allocation_objects := $(evaluation_shared_objects) $(evaluation_build)/evaluation/allocation.o
matrix_binary := $(evaluation_build)/matrix
chase_binary := $(evaluation_build)/chase
hash_binary := $(evaluation_build)/hash
allocation_binary := $(evaluation_build)/allocation
evaluation_binaries += $(matrix_binary) $(chase_binary) $(hash_binary) $(allocation_binary)

$(matrix_binary): $(matrix_objects)
	$(CC) $(EVAL_CFLAGS) $(EVAL_LDFLAGS) $^ $(EVAL_LDLIBS) -o "$@"
$(chase_binary): $(chase_objects)
	$(CC) $(EVAL_CFLAGS) $(EVAL_LDFLAGS) $^ $(EVAL_LDLIBS) -o "$@"
$(hash_binary): $(hash_objects)
	$(CC) $(EVAL_CFLAGS) $(EVAL_LDFLAGS) $^ $(EVAL_LDLIBS) -o "$@"
$(allocation_binary): $(allocation_objects)
	$(CC) $(EVAL_CFLAGS) $(EVAL_LDFLAGS) $^ $(EVAL_LDLIBS) -o "$@"

.PHONY: evaluation-matrix evaluation-chase evaluation-hash evaluation-allocation evaluation-suite
evaluation-matrix: $(matrix_binary)
	cd "$(project_root)" && $(PYTHON) -B -m scripts.evaluation.driver --test matrix --binary "matrix=$(matrix_binary)" $(EVAL_ARGS)
evaluation-chase: $(chase_binary)
	cd "$(project_root)" && $(PYTHON) -B -m scripts.evaluation.driver --test chase --binary "chase=$(chase_binary)" $(EVAL_ARGS)
evaluation-hash: $(hash_binary)
	cd "$(project_root)" && $(PYTHON) -B -m scripts.evaluation.driver --test hash --binary "hash=$(hash_binary)" $(EVAL_ARGS)
evaluation-allocation: $(allocation_binary)
	cd "$(project_root)" && $(PYTHON) -B -m scripts.evaluation.driver --test allocation --binary "allocation=$(allocation_binary)" $(EVAL_ARGS)
evaluation-suite: $(evaluation_binaries)
	cd "$(project_root)" && $(PYTHON) -B -m scripts.evaluation.driver $(foreach name,correctness matrix chase hash allocation,--binary "$(name)=$($(name)_binary)") $(EVAL_ARGS)

-include $(matrix_objects:.o=.d) $(chase_objects:.o=.d) $(hash_objects:.o=.d) $(allocation_objects:.o=.d)
