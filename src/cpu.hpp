#pragma once
#include "diag.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

class Bus;

// ============================================================================
// PSR — Processor Status Register (S1C33 STD Core)
//
// Bit layout (mask 0x0FDF for STD Core):
//   0  N  Negative
//   1  Z  Zero
//   2  V  Overflow
//   3  C  Carry
//   4  IE Interrupt Enable
//   6  DS Delay Slot active
//   7  MO MAC Overflow (sticky)
//   8–11 IL[3:0] Interrupt Level
// ============================================================================
struct Psr {
    uint32_t raw = 0;

    bool n()  const { return (raw >> 0) & 1; }
    bool z()  const { return (raw >> 1) & 1; }
    bool v()  const { return (raw >> 2) & 1; }
    bool c()  const { return (raw >> 3) & 1; }
    bool ie() const { return (raw >> 4) & 1; }
    bool ds() const { return (raw >> 6) & 1; }
    bool mo() const { return (raw >> 7) & 1; }
    uint32_t il() const { return (raw >> 8) & 0xF; }

    void set_n(bool v)  { raw = (raw & ~(1u<<0)) | (uint32_t(v)<<0); }
    void set_z(bool v)  { raw = (raw & ~(1u<<1)) | (uint32_t(v)<<1); }
    void set_v(bool v)  { raw = (raw & ~(1u<<2)) | (uint32_t(v)<<2); }
    void set_c(bool v)  { raw = (raw & ~(1u<<3)) | (uint32_t(v)<<3); }
    void set_ie(bool v) { raw = (raw & ~(1u<<4)) | (uint32_t(v)<<4); }
    void set_ds(bool v) { raw = (raw & ~(1u<<6)) | (uint32_t(v)<<6); }
    void set_mo(bool v) { raw = (raw & ~(1u<<7)) | (uint32_t(v)<<7); }
    void set_il(uint32_t lv) { raw = (raw & ~(0xFu<<8)) | ((lv & 0xF)<<8); }

    void set_nz(uint32_t result) {
        set_n(result >> 31);
        set_z(result == 0);
    }
    void set_nzvc_add(uint32_t a, uint32_t b, uint64_t r64) {
        uint32_t r = static_cast<uint32_t>(r64);
        set_n(r >> 31);
        set_z(r == 0);
        set_c(r64 >> 32);
        set_v(((~(a ^ b)) & (a ^ r)) >> 31);
    }
    void set_nzvc_sub(uint32_t a, uint32_t b, uint64_t r64) {
        uint32_t r = static_cast<uint32_t>(r64);
        set_n(r >> 31);
        set_z(r == 0);
        set_c(r64 >> 32); // borrow
        set_v(((a ^ b) & (a ^ r)) >> 31);
    }
};

// ============================================================================
// CpuState — full architectural state of one S1C33209 core
// ============================================================================
struct CpuState {
    uint32_t r[16] = {};    // general-purpose registers R0–R15
    uint32_t pc    = 0;
    uint32_t sp    = 0;     // shadow of r[15] — keep in sync (piemu style: separate)
    uint32_t alr   = 0;     // MAC accumulator low
    uint32_t ahr   = 0;     // MAC accumulator high
    Psr      psr   = {};

    // EXT accumulation: pending_ext[0] = oldest (highest bits)
    uint32_t pending_ext[2] = {};
    int      pending_ext_count = 0;

    // Halt state (slp/halt): set to true by SLP/HALT, cleared by trap
    bool in_halt = false;

    // Emulator fault: set when an undefined/reserved instruction is executed.
    // Distinct from in_halt so callers can detect abnormal termination.
    bool fault = false;

    // Delay-slot state: when true, next instruction is in delay slot
    // (branching from within a delay slot is undefined; we won't handle it)
    bool in_delay_slot = false;
    uint32_t delay_target = 0; // branch target to take after delay slot

    // Which instruction initiated the current delay slot.
    // Used to warn about SP-modifying instructions in call.d/ret.d delay slots.
    // 0 = other (jp.d, jr**.d), 1 = ret.d, 2 = call.d
    uint8_t delay_caller = 0;

    // Trap Table Base Register — set by BcuAreaCtrl when BCU TTBR is written.
    // In P/ECE the kernel sets this to 0x400 (TPVECTORTOP).
    // Trap vector N is at address: ttbr + N * 4.
    uint32_t ttbr = 0x400;
};

// ============================================================================
// Cpu — executes S1C33209 instructions
// ============================================================================
class Cpu {
public:
    CpuState state;

    // Software breakpoints set via semihosting BKPT_SET/CLR.
    // When step() is about to execute an instruction at a breakpoint address,
    // it prints a register dump and halts.
    std::unordered_set<uint32_t> breakpoints;

    explicit Cpu(Bus& bus);

    // Reset: set PC from boot vector at 0xC00000
    void reset();

    // Execute one instruction (including any pending delay slot sequencing).
    // Returns number of cycles consumed.
    int step();

    // Disassemble one instruction at addr into a human-readable string.
    // Does NOT consume state or cycles.
    std::string disasm(uint32_t addr) const;

    // Attach a diagnostic sink.  Defaults to StderrDiagSink; never null.
    // The Cpu never owns the sink pointer.
    void set_diag(DiagSink* sink) { diag_ = sink ? sink : &default_sink_; }

    // Enable strict mode: violations that would be warnings become faults.
    // Called by GdbRsp::serve() when a debugger connects/disconnects.
    void set_strict(bool s) { strict_ = s; }

    // Raise a trap (interrupt/exception). Called by bus devices or internally.
    // no < 16: non-maskable; no >= 16: maskable (checks IE, IL).
    void assert_trap(int no, int level);

    // ---- EXT helpers (public so static handler functions can call them) ----
    // Zero-extend imm, combine with pending EXT (unsigned result)
    uint32_t ext_imm(uint32_t imm, int width) const;
    // Sign-extend at combined bit width
    int32_t  ext_simm(uint32_t imm, int width) const;
    // Unsigned byte displacement from EXT only (for [%rb] with EXT)
    uint32_t ext_rb() const;
    // PC-relative combined displacement (halfword units, sign-extended)
    int32_t  ext_pcrel(uint32_t imm8) const;
    void flush_ext();

    // Emit a warning-level diagnostic (does NOT halt the CPU).
    void diag_warn(const char* category, uint32_t pc, std::string detail);

    // Emit a fault-level diagnostic and halt the CPU (sets fault + in_halt).
    void diag_fault(const char* category, uint32_t pc, std::string detail);

    // In strict mode (debugger attached), escalate to fault; otherwise warn.
    void diag_warn_or_fault(const char* category, uint32_t pc, std::string detail);

private:
    StderrDiagSink default_sink_;
    DiagSink*      diag_    = &default_sink_;
    bool           strict_  = false;  // escalate soft violations to faults

    Bus& bus_;

    using Handler = void(*)(Cpu&, uint16_t);
    static const std::array<Handler, 65536> dispatch_;
    static std::array<Handler, 65536> build_table();

    // ---- Trap internals ----
    void do_trap(int no, int level);

    // ---- Instruction handlers (called from dispatch table) ----
    // Each handler receives the full 16-bit instruction word.

    // Class 0a
    static void h_nop(Cpu&, uint16_t);
    static void h_slp(Cpu&, uint16_t);
    static void h_halt(Cpu&, uint16_t);
    static void h_pushn(Cpu&, uint16_t);
    static void h_popn(Cpu&, uint16_t);
    static void h_brk(Cpu&, uint16_t);
    static void h_retd(Cpu&, uint16_t);
    static void h_int(Cpu&, uint16_t);
    static void h_reti(Cpu&, uint16_t);
    static void h_call_rb(Cpu&, uint16_t);
    static void h_ret(Cpu&, uint16_t);
    static void h_jp_rb(Cpu&, uint16_t);

    // Class 0b — conditional branches (jr**)
    static void h_jr(Cpu&, uint16_t, bool taken); // common helper
    static void h_jrgt(Cpu&, uint16_t);
    static void h_jrge(Cpu&, uint16_t);
    static void h_jrlt(Cpu&, uint16_t);
    static void h_jrle(Cpu&, uint16_t);
    static void h_jrugt(Cpu&, uint16_t);
    static void h_jruge(Cpu&, uint16_t);
    static void h_jrult(Cpu&, uint16_t);
    static void h_jrule(Cpu&, uint16_t);
    static void h_jreq(Cpu&, uint16_t);
    static void h_jrne(Cpu&, uint16_t);
    static void h_call_simm8(Cpu&, uint16_t);
    static void h_jp_simm8(Cpu&, uint16_t);

    // Class 1 — register-indirect load/store (no base immediate)
    static void h_ld_b_rd_rb (Cpu&, uint16_t);
    static void h_ld_ub_rd_rb(Cpu&, uint16_t);
    static void h_ld_h_rd_rb (Cpu&, uint16_t);
    static void h_ld_uh_rd_rb(Cpu&, uint16_t);
    static void h_ld_w_rd_rb (Cpu&, uint16_t);
    static void h_st_b_rb_rs (Cpu&, uint16_t);
    static void h_st_h_rb_rs (Cpu&, uint16_t);
    static void h_st_w_rb_rs (Cpu&, uint16_t);

    // Class 1B — register-indirect with post-increment
    static void h_ld_b_rd_rbx (Cpu&, uint16_t);
    static void h_ld_ub_rd_rbx(Cpu&, uint16_t);
    static void h_ld_h_rd_rbx (Cpu&, uint16_t);
    static void h_ld_uh_rd_rbx(Cpu&, uint16_t);
    static void h_ld_w_rd_rbx (Cpu&, uint16_t);
    static void h_st_b_rbx_rs (Cpu&, uint16_t);
    static void h_st_h_rbx_rs (Cpu&, uint16_t);
    static void h_st_w_rbx_rs (Cpu&, uint16_t);

    // Class 1C — register ALU
    static void h_add_rd_rs(Cpu&, uint16_t);
    static void h_sub_rd_rs(Cpu&, uint16_t);
    static void h_cmp_rd_rs(Cpu&, uint16_t);
    static void h_ld_w_rd_rs(Cpu&, uint16_t);
    static void h_and_rd_rs(Cpu&, uint16_t);
    static void h_or_rd_rs (Cpu&, uint16_t);
    static void h_xor_rd_rs(Cpu&, uint16_t);
    static void h_not_rd_rs(Cpu&, uint16_t);

    // Class 2 — SP-relative
    static void h_ld_b_rd_sp  (Cpu&, uint16_t);
    static void h_ld_ub_rd_sp (Cpu&, uint16_t);
    static void h_ld_h_rd_sp  (Cpu&, uint16_t);
    static void h_ld_uh_rd_sp (Cpu&, uint16_t);
    static void h_ld_w_rd_sp  (Cpu&, uint16_t);
    static void h_st_b_sp_rs  (Cpu&, uint16_t);
    static void h_st_h_sp_rs  (Cpu&, uint16_t);
    static void h_st_w_sp_rs  (Cpu&, uint16_t);

    // Class 3 — immediate ALU
    static void h_add_rd_imm6 (Cpu&, uint16_t);
    static void h_sub_rd_imm6 (Cpu&, uint16_t);
    static void h_cmp_rd_simm6(Cpu&, uint16_t);
    static void h_ld_w_rd_simm6(Cpu&, uint16_t);
    static void h_and_rd_simm6(Cpu&, uint16_t);
    static void h_or_rd_simm6 (Cpu&, uint16_t);
    static void h_xor_rd_simm6(Cpu&, uint16_t);
    static void h_not_rd_simm6(Cpu&, uint16_t);

    // Class 4A — SP adjust
    static void h_add_sp_imm10(Cpu&, uint16_t);
    static void h_sub_sp_imm10(Cpu&, uint16_t);

    // Class 4B — shift/rotate immediate
    static void h_srl_rd_imm4(Cpu&, uint16_t);
    static void h_sll_rd_imm4(Cpu&, uint16_t);
    static void h_sra_rd_imm4(Cpu&, uint16_t);
    static void h_sla_rd_imm4(Cpu&, uint16_t);
    static void h_rr_rd_imm4 (Cpu&, uint16_t);
    static void h_rl_rd_imm4 (Cpu&, uint16_t);

    // Class 4C — shift/rotate register
    static void h_srl_rd_rs(Cpu&, uint16_t);
    static void h_sll_rd_rs(Cpu&, uint16_t);
    static void h_sra_rd_rs(Cpu&, uint16_t);
    static void h_sla_rd_rs(Cpu&, uint16_t);
    static void h_rr_rd_rs (Cpu&, uint16_t);
    static void h_rl_rd_rs (Cpu&, uint16_t);

    // Class 4D — scan/swap/div
    static void h_scan0 (Cpu&, uint16_t);
    static void h_scan1 (Cpu&, uint16_t);
    static void h_swap  (Cpu&, uint16_t);
    static void h_mirror(Cpu&, uint16_t);
    static void h_div0s (Cpu&, uint16_t);
    static void h_div0u (Cpu&, uint16_t);
    static void h_div1  (Cpu&, uint16_t);
    static void h_div2s (Cpu&, uint16_t);
    static void h_div3s (Cpu&, uint16_t);

    // Class 5A — special reg / bit ops / adc / sbc
    static void h_ld_w_sd_rs(Cpu&, uint16_t); // ld.w %special, %rs
    static void h_ld_w_rd_ss(Cpu&, uint16_t); // ld.w %rd, %special
    static void h_btst(Cpu&, uint16_t);
    static void h_bclr(Cpu&, uint16_t);
    static void h_bset(Cpu&, uint16_t);
    static void h_bnot(Cpu&, uint16_t);
    static void h_adc (Cpu&, uint16_t);
    static void h_sbc (Cpu&, uint16_t);

    // Class 5B — byte/halfword register-register loads
    static void h_ld_b_rd_rs2 (Cpu&, uint16_t);
    static void h_ld_ub_rd_rs2(Cpu&, uint16_t);
    static void h_ld_h_rd_rs2 (Cpu&, uint16_t);
    static void h_ld_uh_rd_rs2(Cpu&, uint16_t);

    // Class 5C — multiply/MAC
    static void h_mlt_h  (Cpu&, uint16_t);
    static void h_mltu_h (Cpu&, uint16_t);
    static void h_mlt_w  (Cpu&, uint16_t);
    static void h_mltu_w (Cpu&, uint16_t);
    static void h_mac    (Cpu&, uint16_t);

    // Class 6 — EXT
    static void h_ext(Cpu&, uint16_t);

    // Undefined / reserved instruction
    static void h_undef(Cpu&, uint16_t);

    // ---- Common branch helpers ----
    void do_branch(uint32_t target, bool delayed);
    void do_call(uint32_t target, bool delayed);
};
