CC = gcc
LEX = flex
YACC = bison
INCLUDES = -I.
CFLAGS ?= -g -Wall -Wextra
CPPFLAGS += ${INCLUDES} -I$(BUILD_DIR)
override CFLAGS += -MMD -MP
LIBS =

BUILD_DIR = build
PERL ?= perl
MOARVM_PREFIX ?= $(CURDIR)/.local/moarvm

all: lab4

$(BUILD_DIR)/write_ffi_runtime: tools/write_ffi_runtime.c $(BUILD_DIR)/ast.o $(BUILD_DIR)/moarvm_model.o $(BUILD_DIR)/objects.o $(BUILD_DIR)/moarvm_image.o $(BUILD_DIR)/moarvm_direct.o $(BUILD_DIR)/moarvm_dump.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(filter %.c %.o,$^)

$(BUILD_DIR)/ffi_runtime.h: $(BUILD_DIR)/write_ffi_runtime
	$< $(BUILD_DIR)/ffi_runtime.moarvm $@

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

.PHONY: demo setup-vm
setup-vm:
	./tools/install_moarvm.sh

demo: demo-lab4

.PHONY: perl-bridge lab4 demo-lab4
perl-bridge: $(BUILD_DIR)/ffi_runtime.h
	cd ffi/perl && MOARVM_PREFIX="$(MOARVM_PREFIX)" SPO_BUILD_DIR="$(abspath $(BUILD_DIR))" $(PERL) Makefile.PL
	$(MAKE) -C ffi/perl

output/lab4/operations.moarvm: examples/lab4/operations.src $(BUILD_DIR)/moarvmdirect
	mkdir -p output/lab4
	$(BUILD_DIR)/moarvmdirect --library --graphs output/lab4/graphs $@ $<

lab4: perl-bridge output/lab4/operations.moarvm

demo-lab4: lab4
	$(PERL) examples/lab4/app.pl stats 7 -3 12 4
	$(PERL) examples/lab4/app.pl scale 3 7 -3 12 4
	$(PERL) examples/lab4/app.pl greet Мир Perl
	./tools/moar.sh --dump output/lab4/operations.moarvm > output/lab4/operations.official-dump.txt
