#pragma once
#include "diag.hpp"
#include "flash_device.hpp"
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// Watchpoints
// ============================================================================

enum class WpType { WRITE, READ, RW };

struct Watchpoint {
    uint32_t addr;
    uint32_t size = 4;
    WpType   type = WpType::WRITE;
};

// Callback signature: (watchpoint, accessed_addr, value, width_bytes, is_write)
using WpCallback = std::function<void(const Watchpoint&, uint32_t, uint32_t, int, bool)>;

// ============================================================================
// Bus — BCU (Bus Control Unit) for S1C33209
//
// Address space (28-bit):
//   0x000000–0x001FFF  Internal RAM 8 KB (16-bit, 0 wait)
//   0x002000–0x002FFF  Internal RAM mirror
//   0x030000–0x04FFFF  Peripheral registers (16-bit)
//   0x060000–0x07FFFF  Semihosting port (handled before BCU dispatch)
//   0x100000–0x12FFFF  External SRAM 256 KB (16-bit, configurable wait)
//   0x0C00000–...       External Flash (16-bit, configurable wait)
//
// Wait cycle model (BCU):
//   Internal:         always 0 wait, no penalty
//   External (16-bit device):
//     byte  read  = wait + 1 cycles
//     byte  write = wait + 2 cycles
//     half  read  = wait + 1 cycles
//     half  write = wait + 2 cycles
//     word  read  = (wait + 1) * 2 cycles  (two 16-bit accesses)
//     word  write = (wait + 2) * 2 cycles
// ============================================================================

// I/O handler pair — one per 2-byte aligned I/O register
struct IoHandler {
    std::function<uint16_t(uint32_t addr)>           read;
    std::function<void(uint32_t addr, uint16_t val)> write;
};

class Bus {
public:
    // Fixed hardware addresses (S1C33209 internal / memory-map constants)
    static constexpr uint32_t IRAM_BASE   = 0x000000;
    static constexpr uint32_t IRAM_SIZE   = 0x002000; // 8 KB (internal, fixed)
    static constexpr uint32_t SRAM_BASE   = 0x100000; // external SRAM start
    static constexpr uint32_t SRAM_WINDOW = 0x100000; // BCU-decoded window: 1 MB (0x100000–0x1FFFFF)
    static constexpr uint32_t FLASH_BASE    = 0xC00000; // external Flash start
    // Area 1: 0x030000–0x05FFFF.  I/O registers live at 0x040000–0x04FFFF;
    // 0x030000–0x03FFFF and 0x050000–0x05FFFF are mirrors of that window.
    // io_handlers_ is indexed relative to IOREG_BASE after mirror normalization.
    static constexpr uint32_t AREA1_BASE    = 0x030000; // Area 1 start (includes mirrors)
    static constexpr uint32_t AREA1_END     = 0x060000; // Area 1 end (exclusive)
    static constexpr uint32_t IOREG_BASE    = 0x040000; // canonical I/O base
    static constexpr uint32_t IOREG_SIZE    = 0x010000; // 64 KB actual I/O window
    // Semihosting sits above Area 1 and is not subject to mirroring.
    static constexpr uint32_t SEMIHOST_BASE = 0x060000;
    static constexpr uint32_t SEMIHOST_SIZE = 0x020000; // 0x060000–0x07FFFF
    // Total io_handlers_ span: IOREG_BASE to end of semihosting (0x040000–0x07FFFF)
    static constexpr uint32_t IOHANDLER_SPAN = (SEMIHOST_BASE + SEMIHOST_SIZE) - IOREG_BASE;

    // External memory sizes — set by constructor; vary by hardware revision.
    // Standard P/ECE: sram=256 KB, flash=1 MB.  2 MB Flash mod also exists.
    std::size_t sram_size()  const { return sram_.size(); }
    std::size_t flash_size() const { return flash_device_->size(); }

    // Access the installed flash device.  Always non-null after construction.
    FlashDevice*       flash_device()       { return flash_device_.get(); }
    const FlashDevice* flash_device() const { return flash_device_.get(); }

    // Replace the flash device (e.g., swap the default FlatFlashRom for an
    // Sst39vf with full CFI / write semantics).  Must be called BEFORE
    // pfi_load() so that the new device receives the loaded image.
    void install_flash_device(std::unique_ptr<FlashDevice> dev) {
        flash_device_ = std::move(dev);
    }

    // External area wait cycles (default: 3 for SRAM, 3 for Flash)
    int sram_wait  = 3;
    int flash_wait = 3;

    // Cycle counter (incremented by each access)
    uint32_t cycles = 0;

    // Debug: current CPU PC for watchpoint logging (set by emulator main loop)
    uint32_t debug_pc = 0;

    // Attach a diagnostic sink (optional; defaults to StderrDiagSink).
    // The Bus never owns the sink pointer.
    void set_diag(DiagSink* sink) { diag_ = sink ? sink : &default_sink_; }

    // Returns true (and clears the flag) if a bus-level fault occurred since
    // the last call.  Cpu::step() polls this after each dispatch to halt the CPU.
    bool take_fault() { bool f = fault_pending_; fault_pending_ = false; return f; }

    // ---- Watchpoints --------------------------------------------------------
    // add_watchpoint: addr/size define the monitored byte range; type selects
    // which access kind fires the callback.  Multiple watchpoints may be added.
    void add_watchpoint(uint32_t addr, uint32_t size = 4,
                        WpType type = WpType::WRITE);
    // Remove the watchpoint that exactly matches addr+size+type.
    void remove_watchpoint(uint32_t addr, uint32_t size, WpType type);
    void clear_watchpoints();
    void set_wp_callback(WpCallback cb) { wp_cb_ = std::move(cb); }
    bool has_watchpoints() const { return !watchpoints_.empty(); }

    // ---- Shadow SRAM --------------------------------------------------------
    // Returns the PC of the last instruction that wrote to the given SRAM
    // address, or 0xFFFF'FFFF if the byte has never been written since reset.
    uint32_t shadow_last_writer(uint32_t addr) const;

    // sram_size:  external SRAM in bytes (default 256 KB — standard P/ECE)
    // flash_size: external Flash in bytes (default 512 KB — standard P/ECE;
    //             use 0x100000 for 1 MB or 0x200000 for 2 MB Flash-modded units)
    explicit Bus(std::size_t sram_size  = 0x040000,
                 std::size_t flash_size = 0x080000);

    // Load data into flash ROM
    void load_flash(uint32_t offset, const uint8_t* data, std::size_t size);

    // Load data into SRAM
    void load_sram(uint32_t offset, const uint8_t* data, std::size_t size);

    // Register an I/O handler (addr must be in IOREG_BASE range, 2-byte aligned)
    void register_io(uint32_t addr, IoHandler handler);

    // ---- Data access (full BCU dispatch, may trigger I/O, accrues cycles) ----
    uint8_t  read8 (uint32_t addr);
    uint16_t read16(uint32_t addr);
    uint32_t read32(uint32_t addr);
    void write8 (uint32_t addr, uint8_t  val);
    void write16(uint32_t addr, uint16_t val);
    void write32(uint32_t addr, uint32_t val);

    // ---- Instruction fetch (internal RAM + Flash only, no I/O, no cycle charge) ----
    uint16_t fetch16(uint32_t addr);

    // Raw pointer into internal RAM (for ELF load / debug)
    uint8_t*       iram_ptr()       { return iram_.data(); }
    uint8_t*       sram_ptr()       { return sram_.data(); }
    const uint8_t* sram_ptr() const { return sram_.data(); }

private:
    std::vector<uint8_t> iram_;
    std::vector<uint8_t> sram_;
    std::unique_ptr<FlashDevice> flash_device_;
    std::vector<IoHandler> io_handlers_; // indexed by (addr - IOREG_BASE) / 2

    // Shadow SRAM: parallel to sram_, one PC entry per byte.
    // Initialised to 0xFFFF'FFFF ("never written").
    std::vector<uint32_t> shadow_sram_;

    // Watchpoint list and callback.
    std::vector<Watchpoint> watchpoints_;
    WpCallback              wp_cb_;

    StderrDiagSink default_sink_;
    DiagSink*      diag_         = &default_sink_;
    bool           fault_pending_ = false;

    enum class Region { IRAM, SRAM, FLASH, IO, NONE };
    Region classify(uint32_t addr) const;

    // Watchpoint helpers.
    void fire_wp(uint32_t addr, uint32_t val, int width, bool is_write);
    void update_shadow(uint32_t sram_off, int width); // record debug_pc

    uint8_t* ptr_for(uint32_t addr, std::size_t size);
    const uint8_t* cptr_for(uint32_t addr, std::size_t size) const;
};
