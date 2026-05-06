#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

// ============================================================================
// FlashDevice — abstract interface for the external NOR flash chip on the
// P/ECE memory bus (BCU Area 6, address 0xC00000+).
//
// Two implementations exist:
//   FlatFlashRom (this header)       — read-only flat byte buffer, used by
//                                       bare-metal tests and headless runs
//                                       that do not need write semantics.
//   Sst39vf      (piece_board layer) — full SST 39VF400A / 39VF160 model
//                                       with CFI Query, Software ID, Word
//                                       Program, Sector/Block/Chip erase,
//                                       and 4 KB sector dirty tracking.
//
// Bus delegates flash region reads/writes to whichever device is installed.
// Instruction fetch (fetch16) bypasses the virtual call and reads directly
// from mem_ptr() — flash code execution must not pay a virtual dispatch
// per fetch.
// ============================================================================

class FlashDevice {
public:
    virtual ~FlashDevice() = default;

    // Total device capacity in bytes (must be a power of two for Sst39vf).
    virtual std::size_t size() const = 0;

    // Halfword read at byte offset within the device.  In a write-capable
    // implementation the returned value depends on the current command-mode
    // state (Normal → memory; CfiQuery → CFI table; SoftwareId → IDs).
    virtual uint16_t read16(uint32_t offset) const = 0;

    // Byte read.  Provided as a convenience; defaults to a halfword read
    // followed by byte selection.
    virtual uint8_t read8(uint32_t offset) const {
        const uint16_t hw = read16(offset & ~uint32_t{1});
        return (offset & 1) ? static_cast<uint8_t>(hw >> 8)
                            : static_cast<uint8_t>(hw);
    }

    // Halfword write — drives the command state machine in writable
    // implementations; ignored in read-only ROMs.
    virtual void write16(uint32_t offset, uint16_t value) = 0;

    // Bulk-load `data` (length `size` bytes) starting at byte offset
    // `offset`.  Used by the PFI loader to pre-populate the device.
    // Implementations should NOT mark sectors as dirty for this call —
    // the loader is bringing the device into sync with the host file.
    virtual void load_image(uint32_t offset,
                            const uint8_t* data, std::size_t size) = 0;

    // Pointer to the underlying flash bytes (for fast-path instruction
    // fetch and direct-memory data reads when the device is in its
    // "Normal / read" mode).  Bus uses this in the fetch16 hot path.
    virtual const uint8_t* mem_ptr() const = 0;

    // True when the device is in its read-only / Normal command mode.
    // When false, read16 may return CFI / Software-ID values rather than
    // memory contents — Bus must not bypass the virtual call.  Default
    // implementation reports always-readable (suitable for ROMs).
    virtual bool in_read_mode() const { return true; }

    // ---- Dirty tracking (for host PFI writeback) --------------------------
    // Erase-granular sector size in bytes (e.g. 4096 for the SST39VF
    // family).  Devices without write semantics return 0.
    virtual std::size_t sector_size() const { return 0; }

    // True iff at least one sector has been written since the most recent
    // clear_dirty().  Default: false (read-only ROM).
    virtual bool any_dirty() const { return false; }

    // Visit each dirty sector index in ascending order.  Default: no-op.
    virtual void for_each_dirty_sector(
        const std::function<void(uint32_t)>& /*cb*/) const {}

    // Reset the dirty bitset.  Default: no-op.
    virtual void clear_dirty() {}

    // Install a callback that fires once per sector when it transitions
    // from clean to dirty.  The callback runs on the thread that drives
    // the bus (the CPU thread in piece-emu-system).  Default: ignored.
    virtual void set_dirty_callback(std::function<void(uint32_t)> /*cb*/) {}
};

// ---------------------------------------------------------------------------
// FlatFlashRom — minimal read-only flash backing.
//
// Used as the default Bus flash device when no writable model is installed.
// Writes are silently dropped (matches pre-Sst39vf behaviour); reads always
// return raw memory, so in_read_mode() is always true.
// ---------------------------------------------------------------------------
class FlatFlashRom final : public FlashDevice {
public:
    explicit FlatFlashRom(std::size_t bytes)
        : mem_(bytes, 0xFF) {}

    std::size_t size() const override { return mem_.size(); }

    uint16_t read16(uint32_t offset) const override {
        offset &= static_cast<uint32_t>(mem_.size() - 1);
        offset &= ~uint32_t{1};
        return static_cast<uint16_t>(mem_[offset])
             | static_cast<uint16_t>(static_cast<uint16_t>(mem_[offset + 1]) << 8);
    }

    void write16(uint32_t /*offset*/, uint16_t /*value*/) override {
        // Read-only ROM; writes silently dropped.
    }

    void load_image(uint32_t offset,
                    const uint8_t* data, std::size_t size) override {
        if (static_cast<std::size_t>(offset) + size > mem_.size())
            return;
        std::memcpy(mem_.data() + offset, data, size);
    }

    const uint8_t* mem_ptr() const override { return mem_.data(); }

private:
    std::vector<uint8_t> mem_;
};
