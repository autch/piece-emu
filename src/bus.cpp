#include "bus.hpp"
#include <cstring>
#include <format>
#include <stdexcept>

// ---- helpers ----------------------------------------------------------------

static uint16_t le16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
static uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
}
static void put_le16(uint8_t* p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void put_le32(uint8_t* p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

// ---- IO address helpers -----------------------------------------------------

// Normalize an Area 1 address (0x030000–0x05FFFF) to the canonical I/O window
// (0x040000–0x04FFFF).  Addresses already in the canonical window are unchanged.
// Semihosting addresses (0x060000+) are passed through unchanged.
static uint32_t normalize_io(uint32_t addr) {
    if (addr < Bus::SEMIHOST_BASE)
        return Bus::IOREG_BASE | (addr & (Bus::IOREG_SIZE - 1));
    return addr;
}

// ---- Bus --------------------------------------------------------------------

Bus::Bus(std::size_t sram_size, std::size_t flash_size)
    : iram_(IRAM_SIZE, 0)
    , sram_(sram_size, 0)
    , flash_(flash_size, 0xFF)
    , io_handlers_(IOHANDLER_SPAN / 2) // covers 0x040000–0x07FFFF (IO + semihosting)
{}

void Bus::load_flash(uint32_t offset, const uint8_t* data, std::size_t size) {
    if (offset + size > flash_.size())
        throw std::runtime_error("ELF segment exceeds flash capacity");
    std::memcpy(flash_.data() + offset, data, size);
}

void Bus::load_sram(uint32_t offset, const uint8_t* data, std::size_t size) {
    if (offset + size > sram_.size())
        throw std::runtime_error("ELF segment exceeds SRAM capacity");
    std::memcpy(sram_.data() + offset, data, size);
}

void Bus::register_io(uint32_t addr, IoHandler handler) {
    // Accept both canonical and mirror addresses; normalize before storing.
    addr = normalize_io(addr);
    // Handlers are halfword-granular; register at the even (aligned) address.
    // Byte accesses to the odd address are handled automatically by read8/write8.
    if (addr < IOREG_BASE || addr >= IOREG_BASE + IOHANDLER_SPAN || (addr & 1))
        throw std::runtime_error(std::format(
            "invalid IO register address 0x{:06X} (must be even, in 0x{:06X}–0x{:06X})",
            addr, IOREG_BASE, IOREG_BASE + IOHANDLER_SPAN - 2));
    io_handlers_[(addr - IOREG_BASE) / 2] = std::move(handler);
}

Bus::Region Bus::classify(uint32_t addr) const {
    addr &= 0x0FFF'FFFF; // 28-bit
    if (addr < IRAM_SIZE) return Region::IRAM;
    if (addr >= 0x002000 && addr < 0x003000) return Region::IRAM; // mirror
    // Area 1 (peripherals) and semihosting above it share Region::IO;
    // caller normalizes to canonical address before indexing io_handlers_.
    if (addr >= AREA1_BASE && addr < SEMIHOST_BASE + SEMIHOST_SIZE) return Region::IO;
    // SRAM window is the full BCU-decoded range; accesses beyond sram_.size()
    // behave as open-bus (required for kernel SRAM size detection).
    if (addr >= SRAM_BASE && addr < SRAM_BASE + SRAM_WINDOW) return Region::SRAM;
    if (addr >= FLASH_BASE && addr < FLASH_BASE + flash_.size()) return Region::FLASH;
    return Region::NONE;
}

// ---- read -------------------------------------------------------------------

uint8_t Bus::read8(uint32_t addr) {
    addr &= 0x0FFF'FFFF;
    switch (classify(addr)) {
    case Region::IRAM:
        return iram_[addr & (IRAM_SIZE - 1)];
    case Region::SRAM:
        cycles += sram_wait + 1;
        if (addr - SRAM_BASE >= sram_.size()) return 0xFF; // open-bus beyond installed SRAM
        return sram_[addr - SRAM_BASE];
    case Region::FLASH:
        cycles += flash_wait + 1;
        return flash_[addr - FLASH_BASE];
    case Region::IO: {
        uint32_t norm = normalize_io(addr);
        uint32_t idx  = (norm - IOREG_BASE) / 2;
        if (idx < io_handlers_.size() && io_handlers_[idx].read) {
            uint16_t hw = io_handlers_[idx].read(norm & ~1u);
            // Return the correct byte: odd address → high byte, even → low byte
            return (addr & 1) ? uint8_t(hw >> 8) : uint8_t(hw);
        }
        return 0xFF; // open-bus
    }
    default:
        return 0xFF;
    }
}

uint16_t Bus::read16(uint32_t addr) {
    addr &= 0x0FFF'FFFF;
    if (addr & 1) {
        diag_->report({DiagLevel::Fault, "misalign_read16", 0,
            std::format("misaligned halfword read at 0x{:06X}", addr), ""});
        fault_pending_ = true;
        addr &= ~1u;
    }
    switch (classify(addr)) {
    case Region::IRAM:
        return le16(&iram_[addr & (IRAM_SIZE - 1)]);
    case Region::SRAM:
        cycles += sram_wait + 1;
        if (addr - SRAM_BASE >= sram_.size()) return 0xFFFF; // open-bus
        return le16(&sram_[addr - SRAM_BASE]);
    case Region::FLASH:
        cycles += flash_wait + 1;
        return le16(&flash_[addr - FLASH_BASE]);
    case Region::IO: {
        uint32_t idx = (normalize_io(addr) - IOREG_BASE) / 2;
        if (idx < io_handlers_.size() && io_handlers_[idx].read)
            return io_handlers_[idx].read(addr);
        return 0xFFFF;
    }
    default:
        return 0xFFFF;
    }
}

uint32_t Bus::read32(uint32_t addr) {
    addr &= 0x0FFF'FFFF;
    if (addr & 3) {
        diag_->report({DiagLevel::Fault, "misalign_read32", 0,
            std::format("misaligned word read at 0x{:06X}", addr), ""});
        fault_pending_ = true;
        addr &= ~3u;
    }
    switch (classify(addr)) {
    case Region::IRAM:
        return le32(&iram_[addr & (IRAM_SIZE - 1)]);
    case Region::SRAM:
        cycles += (sram_wait + 1) * 2;
        if (addr - SRAM_BASE >= sram_.size()) return 0xFFFF'FFFF; // open-bus
        return le32(&sram_[addr - SRAM_BASE]);
    case Region::FLASH:
        cycles += (flash_wait + 1) * 2;
        return le32(&flash_[addr - FLASH_BASE]);
    case Region::IO: {
        // 32-bit IO read: two 16-bit reads
        uint16_t lo = read16(addr);
        uint16_t hi = read16(addr + 2);
        return uint32_t(lo) | (uint32_t(hi) << 16);
    }
    default:
        return 0xFFFF'FFFF;
    }
}

// ---- write ------------------------------------------------------------------

void Bus::write8(uint32_t addr, uint8_t val) {
    addr &= 0x0FFF'FFFF;
    switch (classify(addr)) {
    case Region::IRAM:
        iram_[addr & (IRAM_SIZE - 1)] = val;
        return;
    case Region::SRAM:
        cycles += sram_wait + 2;
        if (addr - SRAM_BASE < sram_.size()) sram_[addr - SRAM_BASE] = val; // else open-bus
        return;
    case Region::IO: {
        uint32_t norm = normalize_io(addr);
        uint32_t idx  = (norm - IOREG_BASE) / 2;
        if (idx < io_handlers_.size() && io_handlers_[idx].write)
            // Pass the original (possibly odd) address so the handler can
            // distinguish high-byte (odd) from low-byte (even) writes.
            io_handlers_[idx].write(norm, val);
        return;
    }
    default:
        return; // writes to FLASH / unknown are silently dropped
    }
}

void Bus::write16(uint32_t addr, uint16_t val) {
    addr &= 0x0FFF'FFFF;
    if (addr & 1) {
        diag_->report({DiagLevel::Fault, "misalign_write16", 0,
            std::format("misaligned halfword write at 0x{:06X}", addr), ""});
        fault_pending_ = true;
        addr &= ~1u;
    }
    switch (classify(addr)) {
    case Region::IRAM:
        put_le16(&iram_[addr & (IRAM_SIZE - 1)], val);
        return;
    case Region::SRAM:
        cycles += sram_wait + 2;
        if (addr - SRAM_BASE < sram_.size()) put_le16(&sram_[addr - SRAM_BASE], val); // else open-bus
        return;
    case Region::IO: {
        uint32_t idx = (normalize_io(addr) - IOREG_BASE) / 2;
        if (idx < io_handlers_.size() && io_handlers_[idx].write)
            io_handlers_[idx].write(addr, val);
        return;
    }
    default:
        return;
    }
}

void Bus::write32(uint32_t addr, uint32_t val) {
    addr &= 0x0FFF'FFFF;
    if (addr & 3) {
        diag_->report({DiagLevel::Fault, "misalign_write32", 0,
            std::format("misaligned word write at 0x{:06X}", addr), ""});
        fault_pending_ = true;
        addr &= ~3u;
    }
    switch (classify(addr)) {
    case Region::IRAM:
        put_le32(&iram_[addr & (IRAM_SIZE - 1)], val);
        return;
    case Region::SRAM:
        cycles += (sram_wait + 2) * 2;
        if (addr - SRAM_BASE < sram_.size()) put_le32(&sram_[addr - SRAM_BASE], val); // else open-bus
        return;
    case Region::IO:
        write16(addr,     static_cast<uint16_t>(val));
        write16(addr + 2, static_cast<uint16_t>(val >> 16));
        return;
    default:
        return;
    }
}

// ---- fetch ------------------------------------------------------------------

uint16_t Bus::fetch16(uint32_t addr) {
    addr &= 0x0FFF'FFFF;
    addr &= ~1u;
    // Internal RAM
    if (addr < IRAM_SIZE)
        return le16(&iram_[addr]);
    if (addr >= 0x002000 && addr < 0x003000)
        return le16(&iram_[addr & (IRAM_SIZE - 1)]);
    // Flash
    if (addr >= FLASH_BASE && addr < FLASH_BASE + flash_.size())
        return le16(&flash_[addr - FLASH_BASE]);
    // SRAM (code in external RAM)
    if (addr >= SRAM_BASE && addr < SRAM_BASE + SRAM_WINDOW) {
        uint32_t off = addr - SRAM_BASE;
        if (off < sram_.size()) return le16(&sram_[off]);
        return 0xFFFF; // open-bus beyond installed SRAM
    }
    return 0xFFFF;
}
