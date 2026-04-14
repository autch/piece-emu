#pragma once
#include <cstdint>
#include <functional>

class Bus;

// ============================================================================
// InterruptController — S1C33209 interrupt controller (c_INTC)
//
// I/O base: 0x040260 (64 bytes, c_INTCtag layout)
//
// Register layout in regs_[]:
//   [0..13]  Priority registers (two 3-bit levels per byte): rPP01L..rPP67L, rPDM
//   [14..15] Dummy
//   [16..23] Interrupt enable registers: rIEN1..rIEN8
//   [24..31] Dummy
//   [32..39] Interrupt status/flag registers: rISR1..rISR8
//   [40..47] Dummy
//   [48..55] IDMA request registers: rIDMAREQ1..rIDMAREQ4, Dummy
//   [56..63] IDMA enable + trigger + rRESET
//
// Interrupt delivery:
//   1. Device calls raise(src)
//   2. ISR flag is set in regs_[]
//   3. If IEN bit clear → stop
//   4. Read 3-bit priority from priority register
//   5. If priority == 0 → stop (disabled)
//   6. Call assert_trap_(trap_no, priority_level)
//
// ISR write semantics (rRESET.RSTONLY = 0, normal mode):
//   Writing 1 to an ISR bit clears it (acknowledge).
// ISR write semantics (rRESET.RSTONLY = 1):
//   Writing 0 to an ISR bit clears it.
// ============================================================================
class InterruptController {
public:
    // Interrupt sources — order must match src_table_[]
    enum class IrqSource {
        PORT0 = 0, PORT1, PORT2, PORT3,   // port input 0-3
        KEY0, KEY1,                         // key input 0-1
        HSDMA0, HSDMA1, HSDMA2, HSDMA3,  // high-speed DMA ch.0-3
        IDMA,                               // intelligent DMA
        T16_CRB0, T16_CRA0,               // 16-bit timer 0 comparison B/A
        T16_CRB1, T16_CRA1,
        T16_CRB2, T16_CRA2,
        T16_CRB3, T16_CRA3,
        T16_CRB4, T16_CRA4,
        T16_CRB5, T16_CRA5,               // 16-bit timer 5 comparison B/A
        T8_UF0, T8_UF1, T8_UF2, T8_UF3, // 8-bit timer 0-3 underflow
        SIF0_ERR, SIF0_RX, SIF0_TX,      // serial I/F ch.0
        SIF1_ERR, SIF1_RX, SIF1_TX,      // serial I/F ch.1
        AD,                                // A/D converter
        CLK_TIMER,                         // clock timer
        PORT4, PORT5, PORT6, PORT7,        // port input 4-7
        NUM_SOURCES
    };

    // Register all I/O handlers with the bus (addr 0x040260..0x04029F).
    // assert_trap: called when a maskable interrupt is accepted by the controller.
    void attach(Bus& bus,
                std::function<void(int trap_no, int level)> assert_trap);

    // Called by peripheral devices when an interrupt event occurs.
    // Sets the ISR flag, checks IEN and priority, delivers if accepted.
    void raise(IrqSource src);

    // Direct read access to raw register bytes (for unit tests).
    uint8_t reg(int offset) const { return regs_[offset]; }

private:
    static constexpr uint32_t BASE_ADDR = 0x040260;
    static constexpr int      REG_COUNT = 64;

    uint8_t regs_[REG_COUNT] = {};
    std::function<void(int, int)> assert_trap_;

    // Per-source descriptor
    struct SrcInfo {
        int trap_no;
        int pri_byte;   // byte offset in regs_ for priority register
        int pri_shift;  // 0 = bits [2:0], 4 = bits [6:4]
        int ien_byte;   // byte offset in regs_ for IEN register
        int ien_bit;    // bit number in IEN register
        int isr_byte;   // byte offset in regs_ for ISR register
        int isr_bit;    // bit number in ISR register
    };
    static const SrcInfo src_table_[static_cast<int>(IrqSource::NUM_SOURCES)];

    void try_deliver(IrqSource src);

    // I/O handlers
    uint16_t io_read(uint32_t off);
    void     io_write(uint32_t off, uint32_t addr, uint16_t val);
};
