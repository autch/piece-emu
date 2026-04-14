#include "peripheral_intc.hpp"
#include "bus.hpp"

// ============================================================================
// Source table: IrqSource → (trap_no, pri_byte, pri_shift, ien_byte, ien_bit,
//                             isr_byte, isr_bit)
//
// Priority register layout (c_INTCtag, offsets within regs_[]):
//   [0]  rPP01L   — Port 0 [2:0], Port 1 [6:4]
//   [1]  rPP23L   — Port 2 [2:0], Port 3 [6:4]
//   [2]  rPK01L   — Key 0 [2:0], Key 1 [6:4]
//   [3]  rPHSD01L — HSDMA0 [2:0], HSDMA1 [6:4]
//   [4]  rPHSD23L — HSDMA2 [2:0], HSDMA3 [6:4]
//   [5]  rPDM     — IDMA [2:0]
//   [6]  rP16T01  — T16_0 [2:0], T16_1 [6:4]
//   [7]  rP16T23  — T16_2 [2:0], T16_3 [6:4]
//   [8]  rP16T45  — T16_4 [2:0], T16_5 [6:4]
//   [9]  rP8TM_PSIO0  — T8(0-3) [2:0], SIF0 [6:4]
//   [10] rPSIO1_PAD   — SIF1 [2:0], AD [6:4]
//   [11] rPCTM    — CLK_TIMER [2:0]
//   [12] rPP45L   — Port 4 [2:0], Port 5 [6:4]
//   [13] rPP67L   — Port 6 [2:0], Port 7 [6:4]
//
// IEN register layout (offsets 16..23 = rIEN1..rIEN8):
//   [16] rIEN1 — Port0[0], Port1[1], Port2[2], Port3[3], Key0[4], Key1[5]
//   [17] rIEN2 — HSDMA0[0], HSDMA1[1], HSDMA2[2], HSDMA3[3], IDMA[4]
//   [18] rIEN3 — T16_CRB0[2], T16_CRA0[3], T16_CRB1[6], T16_CRA1[7]
//   [19] rIEN4 — T16_CRB2[2], T16_CRA2[3], T16_CRB3[6], T16_CRA3[7]
//   [20] rIEN5 — T16_CRB4[2], T16_CRA4[3], T16_CRB5[6], T16_CRA5[7]
//   [21] rIEN6 — T8_UF0[0], T8_UF1[1], T8_UF2[2], T8_UF3[3]
//   [22] rIEN7 — SIF0_ERR[0], SIF0_RX[1], SIF0_TX[2], SIF1_ERR[3], SIF1_RX[4], SIF1_TX[5]
//   [23] rIEN8 — AD[0], CLK_TIMER[1], Port4[2], Port5[3], Port6[4], Port7[5]
//
// ISR register layout (offsets 32..39 = rISR1..rISR8): same bit positions as IEN.
//
// Trap numbers (from piemu/include/c33209e.h TRAP_ defines):
//   PORT0-3: 16-19, KEY0-1: 20-21
//   HSDMA0-3: 22-25, IDMA: 26
//   T16_CRB0: 30, T16_CRA0: 31, T16_CRB1: 34, T16_CRA1: 35
//   T16_CRB2: 38, T16_CRA2: 39, T16_CRB3: 42, T16_CRA3: 43
//   T16_CRB4: 46, T16_CRA4: 47, T16_CRB5: 50, T16_CRA5: 51
//   T8_UF0-3: 52-55
//   SIF0_ERR: 56, SIF0_RX: 57, SIF0_TX: 58
//   SIF1_ERR: 60, SIF1_RX: 61, SIF1_TX: 62
//   AD: 64, CLK_TIMER: 65
//   PORT4-7: 68-71
// ============================================================================

using Src = InterruptController::IrqSource;

const InterruptController::SrcInfo
InterruptController::src_table_[static_cast<int>(Src::NUM_SOURCES)] = {
    // src          trap  pri_b shi  ien_b  ien_bit  isr_b  isr_bit
    /* PORT0   */ {  16,    0,   0,   16,    0,       32,    0 },
    /* PORT1   */ {  17,    0,   4,   16,    1,       32,    1 },
    /* PORT2   */ {  18,    1,   0,   16,    2,       32,    2 },
    /* PORT3   */ {  19,    1,   4,   16,    3,       32,    3 },
    /* KEY0    */ {  20,    2,   0,   16,    4,       32,    4 },
    /* KEY1    */ {  21,    2,   4,   16,    5,       32,    5 },
    /* HSDMA0  */ {  22,    3,   0,   17,    0,       33,    0 },
    /* HSDMA1  */ {  23,    3,   4,   17,    1,       33,    1 },
    /* HSDMA2  */ {  24,    4,   0,   17,    2,       33,    2 },
    /* HSDMA3  */ {  25,    4,   4,   17,    3,       33,    3 },
    /* IDMA    */ {  26,    5,   0,   17,    4,       33,    4 },
    /* T16_CRB0*/ {  30,    6,   0,   18,    2,       34,    2 },
    /* T16_CRA0*/ {  31,    6,   0,   18,    3,       34,    3 },
    /* T16_CRB1*/ {  34,    6,   4,   18,    6,       34,    6 },
    /* T16_CRA1*/ {  35,    6,   4,   18,    7,       34,    7 },
    /* T16_CRB2*/ {  38,    7,   0,   19,    2,       35,    2 },
    /* T16_CRA2*/ {  39,    7,   0,   19,    3,       35,    3 },
    /* T16_CRB3*/ {  42,    7,   4,   19,    6,       35,    6 },
    /* T16_CRA3*/ {  43,    7,   4,   19,    7,       35,    7 },
    /* T16_CRB4*/ {  46,    8,   0,   20,    2,       36,    2 },
    /* T16_CRA4*/ {  47,    8,   0,   20,    3,       36,    3 },
    /* T16_CRB5*/ {  50,    8,   4,   20,    6,       36,    6 },
    /* T16_CRA5*/ {  51,    8,   4,   20,    7,       36,    7 },
    /* T8_UF0  */ {  52,    9,   0,   21,    0,       37,    0 },
    /* T8_UF1  */ {  53,    9,   0,   21,    1,       37,    1 },
    /* T8_UF2  */ {  54,    9,   0,   21,    2,       37,    2 },
    /* T8_UF3  */ {  55,    9,   0,   21,    3,       37,    3 },
    /* SIF0_ERR*/ {  56,    9,   4,   22,    0,       38,    0 },
    /* SIF0_RX */ {  57,    9,   4,   22,    1,       38,    1 },
    /* SIF0_TX */ {  58,    9,   4,   22,    2,       38,    2 },
    /* SIF1_ERR*/ {  60,   10,   0,   22,    3,       38,    3 },
    /* SIF1_RX */ {  61,   10,   0,   22,    4,       38,    4 },
    /* SIF1_TX */ {  62,   10,   0,   22,    5,       38,    5 },
    /* AD      */ {  64,   10,   4,   23,    0,       39,    0 },
    /* CLK_TIMER*/{  65,   11,   0,   23,    1,       39,    1 },
    /* PORT4   */ {  68,   12,   0,   23,    2,       39,    2 },
    /* PORT5   */ {  69,   12,   4,   23,    3,       39,    3 },
    /* PORT6   */ {  70,   13,   0,   23,    4,       39,    4 },
    /* PORT7   */ {  71,   13,   4,   23,    5,       39,    5 },
};

void InterruptController::attach(Bus& bus,
                                  std::function<void(int, int)> assert_trap)
{
    assert_trap_ = std::move(assert_trap);

    // Register I/O handlers for 0x040260..0x04029F (32 halfwords = 64 bytes)
    for (uint32_t off = 0; off < REG_COUNT; off += 2) {
        uint32_t addr = BASE_ADDR + off;
        bus.register_io(addr, {
            [this, off](uint32_t) -> uint16_t {
                return io_read(off);
            },
            [this, off](uint32_t addr, uint16_t val) {
                io_write(off, addr, val);
            }
        });
    }
}

uint16_t InterruptController::io_read(uint32_t off)
{
    // Little-endian halfword read from two consecutive bytes
    return static_cast<uint16_t>(regs_[off]) |
           (static_cast<uint16_t>(regs_[off + 1]) << 8);
}

void InterruptController::io_write(uint32_t off, uint32_t addr, uint16_t val)
{
    // ISR registers are at offsets 32..39 in regs_[].
    // ISR write semantics depend on rRESET.RSTONLY (bit 0 of byte at offset 63):
    //
    //   RSTONLY=0 (default): ISR is fully read/write.  Writing 0 clears the flag
    //                        (write-0-to-clear); writing 1 force-sets it.  The kernel
    //                        uses read-modify-write "bp[isr] &= ~mask" to clear a
    //                        specific bit — this is the normal case (kernel sets
    //                        bp[0x29f]=0x06 which leaves RSTONLY=0).
    //
    //   RSTONLY=1:           ISR can only be cleared, not force-set.  Writing 0 to a
    //                        bit clears it; writing 1 has no effect.
    bool rstonly = (regs_[63] & 0x01) != 0;

    auto write_byte = [&](int byte_off, uint8_t byte_val) {
        if (byte_off >= 32 && byte_off <= 39) {
            // ISR register: selective clear
            if (rstonly) {
                regs_[byte_off] &= byte_val;
            } else {
                regs_[byte_off] = byte_val;
            }
        } else {
            regs_[byte_off] = byte_val;
        }
    };

    if (addr & 1) {
        // Byte write to odd address: only update the high byte (regs_[off+1]).
        // The bus passes the byte value in the low bits of val.
        write_byte(static_cast<int>(off) + 1, static_cast<uint8_t>(val));
    } else {
        // Halfword write or byte write to even address: update both bytes.
        write_byte(static_cast<int>(off),     static_cast<uint8_t>(val));
        write_byte(static_cast<int>(off) + 1, static_cast<uint8_t>(val >> 8));
    }
}

void InterruptController::raise(IrqSource src)
{
    try_deliver(src);
}

void InterruptController::try_deliver(IrqSource src)
{
    int idx = static_cast<int>(src);
    const SrcInfo& info = src_table_[idx];

    // Set ISR flag unconditionally
    regs_[info.isr_byte] |= static_cast<uint8_t>(1 << info.isr_bit);

    // Check interrupt enable
    if (!(regs_[info.ien_byte] & (1 << info.ien_bit))) return;

    // Get 3-bit priority level from the priority register
    int level = (regs_[info.pri_byte] >> info.pri_shift) & 0x07;
    if (level == 0) return; // priority 0 = disabled

    // Deliver to CPU (CPU will check IE and IL internally)
    assert_trap_(info.trap_no, level);
}
