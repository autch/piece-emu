#pragma once
#include "flash_device.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// ============================================================================
// Sst39vf — SST 39VF400A / 39VF160 NOR flash device model
//
// P/ECE uses one of:
//   SST39VF400A (4 Mbit  =  512 KB), Device ID 0x2780, CFI device_size = 0x13
//   SST39VF160  (16 Mbit =    2 MB), Device ID 0x2782, CFI device_size = 0x15
//
// The two parts share an identical command/CFI interface; only the device
// ID, CFI device_size byte, sector count and block count differ.
//
// All reads and writes are halfword-granular at the device interface (the
// chip is wired x16 in P/ECE).  Addresses passed to read16/write16 are
// byte offsets within the chip (0-based, masked to mem_size_-1).
//
// Implemented commands (matching exactly what the P/ECE kernel issues):
//   - CFI Query Entry (98H) / Exit (F0H, short)
//   - Software ID Entry (90H) / Exit (F0H, short)
//   - Word Program (A0H)            — overwrites the halfword (no AND mask)
//   - Sector Erase (30H, 4 KB)      — fills the sector with 0xFF
//   - Block Erase  (50H, 64 KB)     — fills the block with 0xFF
//   - Chip Erase   (10H)            — fills the entire device with 0xFF
//   - Short Exit XX/F0              — returns to read mode from any state
//
// Polling (DQ7 / DQ6) is not modelled: every command completes synchronously,
// so a kernel polling loop satisfies its termination condition on the very
// first iteration.  This matches piemu's flash.c behaviour, which the same
// author wrote for both piemu and the P/ECE kernel.
//
// Protocol-violation behaviour: lenient.  Unknown sequences silently reset
// the state machine to Normal rather than aborting.  The P/ECE kernel is
// the only software that drives this device; strict checking does not pay
// off, and real overflow during stale reset state is harmless (the kernel
// re-issues the unlock sequence each time).
//
// Dirty tracking:
//   The device records which 4 KB sectors have been altered since the last
//   call to clear_dirty().  Writebacks (PfiWriteback) consume this bitset
//   to decide which sectors to push back to the host PFI file.  An optional
//   on_dirty callback fires when a sector first transitions from clean to
//   dirty, enabling the host to start a debounce timer.
// ============================================================================

class Sst39vf final : public FlashDevice {
public:
    static constexpr std::size_t SECTOR_SIZE = 0x1000;  // 4 KB
    static constexpr std::size_t BLOCK_SIZE  = 0x10000; // 64 KB

    // Construct a device sized to `mem_size` bytes.  Must be a power of two
    // matching one of the supported parts (SST39VF400A=0x80000, SST39VF160=
    // 0x200000).  Other power-of-two sizes are accepted (the CFI device_size
    // is computed as log2(mem_size)) but the device ID falls back to 0x2780.
    explicit Sst39vf(std::size_t mem_size = 0x200000); // default: 2 MB (SST39VF160)

    // Factory: pick the SST39VF variant whose capacity covers `min_bytes`.
    // 512 KB or smaller → SST39VF400A; otherwise SST39VF160.
    static Sst39vf for_min_bytes(std::size_t min_bytes);

    // Reset the state machine to Normal (does not modify memory).
    void reset();

    // ---- FlashDevice interface --------------------------------------------
    std::size_t size() const override { return mem_.size(); }

    // Halfword read at byte-offset `offset` (low bit ignored).  Returns the
    // device's current view: real memory in Normal state, CFI table in
    // CfiQuery, ID/Device-ID in SoftwareId.
    uint16_t read16(uint32_t offset) const override;

    // Halfword write at byte-offset `offset`.  Drives the command state
    // machine.  Word-Program writes are committed immediately; erases are
    // applied immediately; Exit (F0H) returns to Normal.
    void write16(uint32_t offset, uint16_t data) override;

    // Byte read at byte-offset `offset` — used by the bus for unaligned
    // 8-bit accesses to the flash region.  In Normal state, returns the
    // raw byte; in CfiQuery / SoftwareId, returns the appropriate byte of
    // the corresponding halfword (low byte for even offset, high byte for
    // odd).  Does NOT advance the command state machine.
    uint8_t read8(uint32_t offset) const override;

    // ---- Image loading & raw access ---------------------------------------
    // Bulk-load `data` (length `size` bytes) starting at byte-offset `offset`
    // into device memory.  Used by the PFI loader.  Does NOT mark sectors
    // as dirty — the loader is bringing the device into sync with the file,
    // not modifying it.
    void load_image(uint32_t offset,
                    const uint8_t* data, std::size_t size) override;

    // Raw memory pointers (for fetch16 fast path).
    const uint8_t* mem_ptr() const override { return mem_.data(); }
    std::size_t    mem_size() const { return mem_.size(); }

    // True iff the chip is in its read-only "Normal" command mode.
    // Bus uses this to decide whether to bypass the virtual call.
    bool in_read_mode() const override { return state_ == State::Normal; }

    // ---- Dirty tracking (overrides FlashDevice) ---------------------------
    std::size_t sector_size() const override { return SECTOR_SIZE; }
    bool        any_dirty()   const override;
    void        clear_dirty() override;
    void for_each_dirty_sector(
        const std::function<void(uint32_t)>& cb) const override;
    void set_dirty_callback(std::function<void(uint32_t)> cb) override {
        on_dirty_ = std::move(cb);
    }

    // Sst39vf-specific helpers (not on FlashDevice).
    bool        is_sector_dirty(uint32_t sector_idx) const;
    std::size_t sector_count() const { return dirty_.size(); }

    // ---- Identification ---------------------------------------------------
    uint16_t manufacturer_id() const { return mfr_id_; }
    uint16_t device_id()       const { return dev_id_; }

private:
    enum class State : uint8_t {
        Normal,
        Enter1,        // got 5555:AA
        Enter2,        // got 5555:AA, 2AAA:55
        Enter3,        // got ...80                     (next: 5555:AA  start of erase 2nd half)
        Enter4,        // got ...80, 5555:AA            (next: 2AAA:55)
        Enter5,        // got ...80, 5555:AA, 2AAA:55   (next: SA:30 / BA:50 / 5555:10)
        SoftwareId,    // got 5555:AA, 2AAA:55, 5555:90
        CfiQuery,      // got 5555:AA, 2AAA:55, 5555:98
        WordProgram    // got 5555:AA, 2AAA:55, 5555:A0 (next: WA:Data)
    };

    // Halfword address (offset >> 1 masked to mem_size_ - 1) the chip
    // observes as 0x5555 (command address 1) and 0x2AAA (command address 2).
    static constexpr uint32_t CMD_ADDR_1 = 0x5555;
    static constexpr uint32_t CMD_ADDR_2 = 0x2AAA;

    void mark_dirty(uint32_t byte_offset, std::size_t span_bytes);

    // CFI Query table: returns the value at CFI half-word address `cfi_addr`
    // (0x10..0x34 typically).  Outside the supported range returns 0.
    uint16_t cfi_query(uint32_t cfi_halfword_addr) const;

    // Software-ID read: returns the value the chip reports at halfword
    // address `id_addr` (0 = manufacturer, 1 = device).  Outside the
    // supported range returns 0.
    uint16_t software_id(uint32_t id_halfword_addr) const;

    std::vector<uint8_t> mem_;
    std::vector<bool>    dirty_;     // mem_size_ / SECTOR_SIZE entries
    State                state_ = State::Normal;

    // Identification — set by the constructor based on mem_.size().
    uint16_t mfr_id_           = 0x00BF; // SST
    uint16_t dev_id_           = 0x2782; // SST39VF160
    uint8_t  cfi_device_size_  = 0x15;   // log2(mem_size); 2 MB → 0x15
    uint16_t cfi_sector_count_ = 511;    // (sectors - 1)
    uint16_t cfi_block_count_  = 31;     // (blocks  - 1)

    std::function<void(uint32_t)> on_dirty_;
};
