# Makefile — auto-generated bare-metal S1C33 ALU regression tests.
#
# Run:
#   make            # regenerate test_gen_alu.c, build ELF
#   make run        # build + run on piece-emu, expect exit 0
#   make clean

BARE     := $(abspath ..)
C33HOME  := /home/autch/src/llvm-c33
BIN      := $(C33HOME)/build/bin
SYSROOT  := $(C33HOME)/sysroot/s1c33-none-elf

CC       := $(BIN)/clang --target=s1c33-none-elf --sysroot=$(SYSROOT)
LD       := $(BIN)/ld.lld

CFLAGS   := -O1 -ffreestanding -fno-builtin -Wall -Wextra
LDFLAGS  := -T sram.ld --entry _start

EMU      := $(abspath ../../../..)/build-src/piece-emu

CRT_OBJS := $(BARE)/crt0.o $(BARE)/crt_init.o

TESTS    := test_gen_alu.elf test_gen_mem.elf test_gen_sp.elf test_gen_shift.elf test_gen_muldiv.elf test_gen_branch.elf test_gen_pushpop.elf test_gen_mac.elf test_gen_bitop.elf test_gen_special.elf test_gen_calldrb.elf test_gen_trap.elf

all: $(TESTS)

test_gen_alu.c: gen_alu.py parse_qref.py
	python3 gen_alu.py

test_gen_mem.c: gen_mem.py
	python3 gen_mem.py

test_gen_sp.c: gen_sp.py
	python3 gen_sp.py

test_gen_shift.c: gen_shift.py
	python3 gen_shift.py

test_gen_muldiv.c: gen_muldiv.py
	python3 gen_muldiv.py

test_gen_branch.c: gen_branch.py
	python3 gen_branch.py

test_gen_pushpop.c: gen_pushpop.py
	python3 gen_pushpop.py

test_gen_mac.c: gen_mac.py
	python3 gen_mac.py

test_gen_bitop.c: gen_bitop.py
	python3 gen_bitop.py

test_gen_special.c: gen_special.py
	python3 gen_special.py

test_gen_calldrb.c: gen_calldrb.py
	python3 gen_calldrb.py

test_gen_trap.c: gen_trap.py
	python3 gen_trap.py

%.o: %.c
	$(CC) $(CFLAGS) -I$(BARE) -c $< -o $@

test_gen_alu.elf: $(CRT_OBJS) test_gen_alu.o
	$(LD) $(LDFLAGS) -o $@ $^

test_gen_mem.elf: $(CRT_OBJS) test_gen_mem.o
	$(LD) $(LDFLAGS) -o $@ $^

test_gen_sp.elf: $(CRT_OBJS) test_gen_sp.o
	$(LD) $(LDFLAGS) -o $@ $^

test_gen_shift.elf: $(CRT_OBJS) test_gen_shift.o
	$(LD) $(LDFLAGS) -o $@ $^

test_gen_muldiv.elf: $(CRT_OBJS) test_gen_muldiv.o
	$(LD) $(LDFLAGS) -o $@ $^

test_gen_branch.elf: $(CRT_OBJS) test_gen_branch.o
	$(LD) $(LDFLAGS) -o $@ $^

test_gen_pushpop.elf: $(CRT_OBJS) test_gen_pushpop.o
	$(LD) $(LDFLAGS) -o $@ $^

test_gen_mac.elf: $(CRT_OBJS) test_gen_mac.o
	$(LD) $(LDFLAGS) -o $@ $^

test_gen_bitop.elf: $(CRT_OBJS) test_gen_bitop.o
	$(LD) $(LDFLAGS) -o $@ $^

test_gen_special.elf: $(CRT_OBJS) test_gen_special.o
	$(LD) $(LDFLAGS) -o $@ $^

test_gen_calldrb.elf: $(CRT_OBJS) test_gen_calldrb.o
	$(LD) $(LDFLAGS) -o $@ $^

test_gen_trap.elf: $(CRT_OBJS) test_gen_trap.o
	$(LD) $(LDFLAGS) -o $@ $^

run: $(TESTS)
	@for t in $(TESTS); do \
		printf "%-22s " "$$t:"; \
		$(EMU) --max-cycles 5000000 ./$$t 2>&1 | grep -E '\[(PASS|FAIL)' || echo "(no result)"; \
	done

clean:
	rm -f test_gen_*.c test_gen_*.o test_gen_*.elf entries.json parse.log

.PHONY: all run clean
