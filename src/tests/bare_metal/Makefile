# Makefile — bare-metal S1C33 tests for piece_emu
#
# Uses the LLVM S1C33 toolchain at /home/autch/src/llvm-c33.
# Output ELFs load into IRAM (0x000000–0x001FFF, 8 KB).
#
# Usage:
#   make              # build all test ELFs
#   make run          # build and run all tests on the emulator
#   make clean        # remove build artifacts

C33HOME  := /home/autch/src/llvm-c33
BIN      := $(C33HOME)/build/bin
SYSROOT  := $(C33HOME)/sysroot/s1c33-none-elf

CC       := $(BIN)/clang --target=s1c33-none-elf --sysroot=$(SYSROOT)
AS       := $(BIN)/clang --target=s1c33-none-elf --sysroot=$(SYSROOT)
LD       := $(BIN)/ld.lld

CFLAGS   := -O1 -ffreestanding -fno-builtin -Wall -Wextra
ASFLAGS  := -ffreestanding
LDFLAGS  := -T iram.ld --entry _start

EMU      := $(abspath ../../..)/build-src/piece-emu

CRT_OBJS := crt0.o crt_init.o

TESTS    := test_basic.elf \
            test_ext_imm.elf \
            test_alu_flags.elf \
            test_load_store.elf \
            test_shifts.elf \
            test_branches.elf \
            test_multiply.elf \
            test_div.elf \
            test_misc.elf

all: $(TESTS)

%.o: %.s
	$(AS) $(ASFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.elf: $(CRT_OBJS) %.o
	$(LD) $(LDFLAGS) -o $@ $^

run: $(TESTS)
	@pass=0; fail=0; \
	for t in $(TESTS); do \
		printf "%-30s " "$$t:"; \
		$(EMU) --max-cycles 2000000 "$$t"; \
		if [ $$? -eq 0 ]; then pass=$$((pass+1)); else fail=$$((fail+1)); fi; \
	done; \
	echo ""; \
	echo "Results: $$pass passed, $$fail failed"

clean:
	rm -f *.o *.elf *.map
