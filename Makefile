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

$(BUILD_DIR)/moarvmdirect: $(BUILD_DIR)/parser.tab.o $(BUILD_DIR)/lex.yy.o $(BUILD_DIR)/ast.o $(BUILD_DIR)/parse_module.o $(BUILD_DIR)/optree.o $(BUILD_DIR)/cfg.o $(BUILD_DIR)/moarvm_model.o $(BUILD_DIR)/moarvm_image.o $(BUILD_DIR)/moarvm_direct.o $(BUILD_DIR)/moarvm_dump.o $(BUILD_DIR)/moarvm_direct_main.o
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
	rm -rf $(BUILD_DIR) output

.PHONY: all clean moarvm-direct

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: demo calc cats-dogs setup-vm
setup-vm:
	./tools/install_moarvm.sh

demo: $(BUILD_DIR)/moarvmdirect
	@set -e; \
	for name in strings calls arrays control; do \
		mkdir -p "output/lab1/graphs/$$name"; \
		echo "Demo: $$name.src"; \
		$(BUILD_DIR)/moarvmdirect --graphs "output/lab1/graphs/$$name" "output/lab1/$$name.moarvm" "examples/lab1/$$name.src"; \
		./tools/moar.sh "output/lab1/$$name.moarvm"; \
		./tools/moar.sh --dump "output/lab1/$$name.moarvm" > "output/lab1/$$name.official-dump.txt"; \
	done

calc: $(BUILD_DIR)/moarvmdirect
	mkdir -p output/lab1
	$(BUILD_DIR)/moarvmdirect output/lab1/calc.moarvm examples/lab1/calc.src
	./tools/moar.sh output/lab1/calc.moarvm

cats-dogs: $(BUILD_DIR)/moarvmdirect
	mkdir -p output/lab1
	$(BUILD_DIR)/moarvmdirect output/lab1/cats_dogs.moarvm examples/lab1/cats_dogs.src
	./tools/moar.sh output/lab1/cats_dogs.moarvm
