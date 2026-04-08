#!/usr/bin/env python3
"""
Create a minimal ELF32 for the P0 smoke test.
The "program" does:
  1. Load 42 into r0
  2. Write 0 to semihosting TEST_RESULT (PASS)
  3. Halt

S1C33 assembly (16-bit instructions, little-endian):
  ld.w %r0, 42        ; class 3, op1=3 (ld.w), rd=0, imm6=42 → 0x6C2A
                       ; bits[15:13]=011 (3), bits[12:10]=011 (3), bits[9:6]=0000, bits[5:0]=101010 (42)
                       ; = 0b 011 011 0000 101010 = 0x362A
  ld.w %r1, 0x0060    ; load upper address part via ext+ld.w
  ...
  halt

Actually let's use the simplest possible test:
- Load immediate 0 into r0
- Write to semihosting TEST_RESULT = 0 (PASS) at 0x060008
- Halt

Semihosting addr = 0x060008. Since it's in the I/O region, we need to write to it.
We can use: ext 0x0300 / ld.w %r1, 0x08  → r1 = 0x0300_0008? No...

Actually ext 0x0300 shifts left by 6: (0x0300 << 6) | 0x08 = 0xC008 = 49160.
That's not 0x060008.

For 0x060008:
With ld.w %r1, imm6 (class 3, sign-extend):
 Need 0x060008 = 393224
 With 1 EXT: combined = (ext13 << 6) | imm6, sign-extended at 19 bits
   ext13 = upper 13 bits, imm6 = lower 6
   0x060008 / 64 = 0x1800, 0x060008 % 64 = 8
   So ext13 = 0x1800, imm6 = 8
   But sign-extend(0x1800 << 6 | 8, 19) = sign_extend(0x60008, 19) = 0x60008 (bit 18 = 0) ✓

ext 0x1800 encodes as: class 6 = bits[15:13]=110, imm13 = 0x1800 → 0xD800
ld.w %r1, 8:  class3=011, op1=011 (ld.w), rd=0001, imm6=001000 →
  bits: 011 011 0001 001000 = 0b0110110001001000 = 0x6C48

Then: st.w [%r1], %r0 (store r0=0 to address in r1)
st.w [%rb] %rs: class 1, op2=00, op1=111 (st.w), rb=%r1, rs=%r0
  bits[15:13]=001, bits[12:11]=00, bits[10:8]=111, bits[7:4]=rb=0001, bits[3:0]=rs=0000
  = 0b 001 00 111 0001 0000 = 0x2710

halt: class0a, op1=0, op2=2 → bits[15:13]=000, bits[12:11]=00, bits[10:8]=000, bits[7:6]=10
  = 0b 000 00 000 10 xxxxxx = 0x0080

Let me verify the encodings more carefully using the piemu dispatch table logic.
"""

import struct

def le16(v): return struct.pack('<H', v & 0xFFFF)
def le32(v): return struct.pack('<I', v & 0xFFFFFFFF)

# ---- Instructions ----

# ld.w %r0, 0  (class 3, op1=011=3 meaning ld.w, rd field at bits[9:6], imm6 at bits[5:0])
# bits[15:13]=011, bits[12:10]=011, bits[9:6]=rd, bits[5:0]=imm6
# ld.w %r0, 0: rd=0, imm6=0
# = 0b 011 011 0000 000000 = 0x6C00
LD_W_R0_0   = 0x6C00   # ld.w %r0, 0

# ext 0x1800: class 6 = bits[15:13]=110, imm13=0x1800
# = 0b 110 0001 1000 0000 0 → wait, 110 = bits 15-13, then 13 bits of immediate
# = 0b 110 1 1000 0000 0000 = ? Let me be careful:
# bits[15:13] = 110, bits[12:0] = 0x1800 = 0001 1000 0000 0000
# = 1101 1000 0000 0000 = 0xD800
# 0x060008 needs 2 EXT to load as signed (bit18 is set in 19-bit range)
# Use: ext 0 / ext 0x1800 / ld.w %r1, 8
# combined (32-bit) = (0 << 19) | (0x1800 << 6) | 8 = 0x60008, sign_extend(.,32)=0x60008 ✓
EXT_0000    = 0xC000   # ext 0x0000: bits[15:13]=110, bits[12:0]=0 → 0xC000
EXT_1800    = 0xD800   # ext 0x1800: bits[15:13]=110, bits[12:0]=0x1800 → 0xD800

# ld.w %r1, 8: CLASS_3: rd=bits[3:0]=0001(r1), imm6=bits[9:4]=001000(8)
# = 0b 011 011 001000 0001 = 0x6C81
LD_W_R1_8   = 0x6C81   # ld.w %r1, 8 (with 2 exts above → r1 = 0x060008)

# st.w [%r1], %r0: class 1, op2=bits[9:8]=00 (no post-inc), op1=bits[12:10]=111 (st.w)
# rb=bits[7:4]=0001, rs=bits[3:0]=0000
# = 0b 001 111 00 0001 0000 = 0b 0011 1100 0001 0000 = 0x3C10
ST_W_R1_R0  = 0x3C10   # st.w [%r1], %r0

# halt: class 0a, op1=0, op2=2
# bits[15:13]=000, bits[12:11]=00, bits[10:8]=000, bits[7:6]=10, bits[5:0]=xxxxxx
# = 0b 000 00 000 10 000000 = 0x0080
HALT        = 0x0080   # halt

# Our test code at load address (IRAM = 0x000000)
# But we need a boot vector at 0xC00000 (Flash).
# The ELF loader sets PC to entry point directly, so we just need the code
# at whatever address we choose.

# Let's put the code in internal RAM at 0x000000.
code = (
    le16(LD_W_R0_0)  +   # ld.w %r0, 0
    le16(EXT_0000)   +   # ext 0x0000  (first of 2 exts)
    le16(EXT_1800)   +   # ext 0x1800  (second ext)
    le16(LD_W_R1_8)  +   # ld.w %r1, 8  → r1 = 0x060008 (semihosting TEST_RESULT)
    le16(ST_W_R1_R0) +   # st.w [%r1], %r0  → write 0 to TEST_RESULT → PASS
    le16(HALT)           # halt (shouldn't reach here; semihosting sets in_halt)
)

ENTRY = 0x000000
LOAD_ADDR = 0x000000

# ---- ELF header ----
# ELF32 little-endian, bare-metal (ET_EXEC), machine = 0 (no standard number for S1C33)
e_ident = bytes([
    0x7F, ord('E'), ord('L'), ord('F'),   # magic
    1,      # ELFCLASS32
    1,      # ELFDATA2LSB
    1,      # EV_CURRENT
    0,      # OS/ABI = ELFOSABI_NONE
]) + b'\x00' * 8   # padding

e_type    = 2       # ET_EXEC
e_machine = 0       # EM_NONE (no official S1C33 number)
e_version = 1
e_entry   = ENTRY
e_phoff   = 52      # sizeof(Elf32_Ehdr)
e_shoff   = 0
e_flags   = 0
e_ehsize  = 52
e_phentsize = 32    # sizeof(Elf32_Phdr)
e_phnum   = 1
e_shentsize = 40
e_shnum   = 0
e_shstrndx = 0

ehdr = (e_ident +
        struct.pack('<HHIIIIIHHHHHH',
            e_type, e_machine, e_version,
            e_entry, e_phoff, e_shoff, e_flags,
            e_ehsize, e_phentsize, e_phnum,
            e_shentsize, e_shnum, e_shstrndx))

# ---- Program header ----
p_type   = 1        # PT_LOAD
p_offset = 52 + 32  # data starts after ELF header + 1 phdr = 84
p_vaddr  = LOAD_ADDR
p_paddr  = LOAD_ADDR
p_filesz = len(code)
p_memsz  = len(code)
p_flags  = 5        # PF_R | PF_X
p_align  = 2

phdr = struct.pack('<IIIIIIII',
    p_type, p_offset, p_vaddr, p_paddr,
    p_filesz, p_memsz, p_flags, p_align)

elf = ehdr + phdr + code

import os, sys
out = os.path.join(os.path.dirname(__file__), 'smoke_test.elf')
with open(out, 'wb') as f:
    f.write(elf)
print(f"Wrote {len(elf)} bytes to {out}")
print(f"Entry: 0x{ENTRY:06X}")
print(f"Code: {' '.join(f'{b:02X}' for b in code)}")
