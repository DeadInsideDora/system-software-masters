CC = cc
LEX = flex
YACC = bison
BUILD_DIR ?= build
CFLAGS ?= -g -Wall -Wextra
override CFLAGS += -MMD -MP
override CPPFLAGS += -Isrc -Ifrontend -Ibackend -I$(BUILD_DIR)
MOAR ?= ./tools/moar.sh
PLOT_PYTHON ?= $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)
LAB3_PROFILE_SPACES ?= 128 32
LAB3_ANIMAL_PROFILE_SPACES ?= 64 128
LAB3_HEAP_WORDS ?= 4096
LAB3_SPACE_WORDS ?= 128
LAB3_ROOTS ?= 16
LAB3_ROUNDS ?= 200
LAB3_TIMEOUT ?= 30

BACKEND = cfg optree objects moarvm_model moarvm_image moarvm_direct moarvm_dump
OBJECTS = $(addprefix $(BUILD_DIR)/backend/,$(addsuffix .o,$(BACKEND))) \
          $(BUILD_DIR)/frontend/ast.o $(BUILD_DIR)/frontend/parse_module.o \
          $(BUILD_DIR)/parser.tab.o $(BUILD_DIR)/lex.yy.o $(BUILD_DIR)/src/compile_main.o

.PHONY: all lab3 demo demo-lab3 setup-vm clean
all lab3: $(BUILD_DIR)/lab3c

$(BUILD_DIR) $(BUILD_DIR)/src $(BUILD_DIR)/frontend $(BUILD_DIR)/backend:
	mkdir -p $@

$(BUILD_DIR)/parser.tab.c: frontend/parser.y | $(BUILD_DIR)
	$(YACC) -d -v -o $@ $<
$(BUILD_DIR)/parser.tab.h: $(BUILD_DIR)/parser.tab.c
	@test -f $@ || $(YACC) -d -v -o $(BUILD_DIR)/parser.tab.c frontend/parser.y
$(BUILD_DIR)/lex.yy.c: frontend/lexer.l $(BUILD_DIR)/parser.tab.h
	$(LEX) -o $@ $<
$(BUILD_DIR)/parser.tab.o: $(BUILD_DIR)/parser.tab.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/lex.yy.o: $(BUILD_DIR)/lex.yy.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/frontend/parse_module.o: $(BUILD_DIR)/parser.tab.h
$(BUILD_DIR)/frontend/%.o: frontend/%.c | $(BUILD_DIR)/frontend
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/backend/%.o: backend/%.c | $(BUILD_DIR)/backend
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/src/%.o: src/%.c src/gc_layout.def | $(BUILD_DIR)/src
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/lab3c: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

demo demo-lab3: lab3
	python3 tools/lab3_demo.py --compiler $(BUILD_DIR)/lab3c --vm $(MOAR) \
		--heap-words $(LAB3_HEAP_WORDS) --space $(LAB3_SPACE_WORDS) --roots $(LAB3_ROOTS) \
		--rounds $(LAB3_ROUNDS) --timeout $(LAB3_TIMEOUT) --output output
setup-vm:
	./tools/install_moarvm.sh
clean:
	rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/*.d $(BUILD_DIR)/src/*.d $(BUILD_DIR)/frontend/*.d $(BUILD_DIR)/backend/*.d)

.PHONY: profile profile-lab3 profile-copying profile-animals setup-plots
setup-plots:
	python3 -m venv .venv
	.venv/bin/python -m pip install -r requirements-profile.txt
profile profile-lab3: profile-copying profile-animals profile-blackbox

profile-copying: lab3
	$(PLOT_PYTHON) tools/lab3_profile.py --demo copying --compiler $(BUILD_DIR)/lab3c --vm $(MOAR) \
		--heap-words $(LAB3_HEAP_WORDS) --spaces $(LAB3_PROFILE_SPACES) --roots $(LAB3_ROOTS) \
		--rounds $(LAB3_ROUNDS) --timeout $(LAB3_TIMEOUT)

.PHONY: demo-animals
LAB3_ANIMAL_SPACE_WORDS ?= 64
demo-animals: lab3
	mkdir -p output/animals
	$(BUILD_DIR)/lab3c --heap-words $(LAB3_HEAP_WORDS) --arg $(LAB3_ANIMAL_SPACE_WORDS) \
		--arg $(LAB3_ROOTS) --arg $(LAB3_ROUNDS) output/animals/animals.moarvm \
		runtime/gc.src examples/animals_gc.src
	$(MOAR) output/animals/animals.moarvm > output/animals/stdout.txt
	cat output/animals/stdout.txt
	$(MOAR) --dump output/animals/animals.moarvm > output/animals/animals.official-dump.txt

profile-animals: lab3
	$(PLOT_PYTHON) tools/lab3_profile.py --demo animals --compiler $(BUILD_DIR)/lab3c --vm $(MOAR) \
		--heap-words $(LAB3_HEAP_WORDS) --spaces $(LAB3_ANIMAL_PROFILE_SPACES) --roots $(LAB3_ROOTS) \
		--rounds $(LAB3_ROUNDS) --timeout $(LAB3_TIMEOUT)

.PHONY: demo-blackbox profile-blackbox
LAB3_BLACKBOX_SEED ?= 1
LAB3_BLACKBOX_STEPS ?= 1000
LAB3_BLACKBOX_SPACES ?= 128 256
LAB3_BLACKBOX_INTERVAL ?= 100
BLACKBOX_ARGS = --compiler $(BUILD_DIR)/lab3c --vm $(MOAR) --seed $(LAB3_BLACKBOX_SEED) \
                --steps $(LAB3_BLACKBOX_STEPS) --spaces $(LAB3_BLACKBOX_SPACES) \
                --interval $(LAB3_BLACKBOX_INTERVAL) --heap-words $(LAB3_HEAP_WORDS) --timeout $(LAB3_TIMEOUT)

demo-blackbox: lab3
	python3 tools/lab3_blackbox.py $(BLACKBOX_ARGS) --data-only

profile-blackbox: lab3
	$(PLOT_PYTHON) tools/lab3_blackbox.py $(BLACKBOX_ARGS)
