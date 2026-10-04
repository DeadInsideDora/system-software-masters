CC = gcc
LEX = flex
YACC = bison
INCLUDES = -I.
CFLAGS ?= -g -Wall -Wextra
CPPFLAGS += ${INCLUDES} -I$(BUILD_DIR)
override CFLAGS += -MMD -MP
LIBS =

BUILD_DIR = build

all: $(BUILD_DIR)/moarvmdirect

moarvm-direct: $(BUILD_DIR)/moarvmdirect

$(BUILD_DIR)/moarvmdirect: $(BUILD_DIR)/parser.tab.o $(BUILD_DIR)/lex.yy.o $(BUILD_DIR)/ast.o $(BUILD_DIR)/parse_module.o $(BUILD_DIR)/optree.o $(BUILD_DIR)/cfg.o $(BUILD_DIR)/moarvm_model.o $(BUILD_DIR)/objects.o $(BUILD_DIR)/moarvm_image.o $(BUILD_DIR)/moarvm_direct.o $(BUILD_DIR)/moarvm_dump.o $(BUILD_DIR)/moarvm_direct_main.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/parser.tab.c: parser.y | $(BUILD_DIR)
	$(YACC) -d -v -o $(BUILD_DIR)/parser.tab.c parser.y

$(BUILD_DIR)/parser.tab.h: $(BUILD_DIR)/parser.tab.c
	@test -f $@ || $(YACC) -d -v -o $(BUILD_DIR)/parser.tab.c parser.y

$(BUILD_DIR)/parser.tab.o: $(BUILD_DIR)/parser.tab.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/lex.yy.c: lexer.l $(BUILD_DIR)/parser.tab.h | $(BUILD_DIR)
	$(LEX) -o $(BUILD_DIR)/lex.yy.c lexer.l

$(BUILD_DIR)/lex.yy.o: $(BUILD_DIR)/lex.yy.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/parse_module.o: $(BUILD_DIR)/parser.tab.h

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean moarvm-direct

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: demo-lab2 setup-vm
setup-vm:
	./tools/install_moarvm.sh

LAB2_EXAMPLES = counter compose shared_state local_recursion lazy_animals
.PHONY: demo-interactive
demo-interactive: $(BUILD_DIR)/moarvmdirect
	mkdir -p output/lab2
	$(BUILD_DIR)/moarvmdirect --graphs output/lab2/interactive-graphs output/lab2/interactive.moarvm examples/lab2/interactive.src
	./tools/moar.sh output/lab2/interactive.moarvm

demo-lab2: $(BUILD_DIR)/moarvmdirect
	mkdir -p output/lab2
	@set -e; for name in $(LAB2_EXAMPLES); do \
		$(BUILD_DIR)/moarvmdirect --graphs output/lab2/$$name-graphs \
			output/lab2/$$name.moarvm examples/lab2/$$name.src; \
		./tools/moar.sh output/lab2/$$name.moarvm; \
		./tools/moar.sh --dump output/lab2/$$name.moarvm > output/lab2/$$name.official-dump.txt; \
	done

.PHONY: demo-lazy
demo-lazy: $(BUILD_DIR)/moarvmdirect
	mkdir -p output/lab2
	$(BUILD_DIR)/moarvmdirect --graphs output/lab2/lazy_animals-graphs output/lab2/lazy_animals.moarvm examples/lab2/lazy_animals.src
	./tools/moar.sh output/lab2/lazy_animals.moarvm
	./tools/moar.sh --dump output/lab2/lazy_animals.moarvm > output/lab2/lazy_animals.official-dump.txt
