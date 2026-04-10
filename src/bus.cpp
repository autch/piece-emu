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

// ---- Bus --------------------------------------------------------------------

Bus::Bus(std::size_t flash_size)
    : iram_(IRAM_SIZE, 0)
    , sram_(SRAM_SIZE, 0)
    , flash_(flash_size, 0xFF)
    , io_handlers_(IOREG_SIZE / 2) // one entry per 16-bit register
{}

void Bus::load_flash(uint32_t offset, const uint8_t* data, std::size_t size) {
    if (offset + size > flash_.size())
        throw std::runtime_error("ELF segment exceeds flash capacity");
    std::memcpy(flash_.data() + offset, data, size);
}

void Bus::load_sram(uint32_t offset, const uint8_t* data, std::size_t size) {
    if (offset + size > SRAM_SIZE)
        throw std::runtime_error("ELF segment exceeds SRAM capacity");
    std::memcpy(sram_.data() + offset, data, size);
}

void Bus::register_io(uint32_t addr, IoHandler handler) {
    if (addr < IOREG_BASE || addr >= IOREG_BASE + IOREG_SIZE || (addr & 1))
        throw std::runtime_error(std::format("invalid IO register address 0x{:06X}", addr));
    io_handlers_[(addr - IOREG_BASE) / 2] = std::move(handler);
}

Bus::Region Bus::classify(uint32_t addr) const {
    addr &= 0x0FFF'FFFF; // 28-bit
    if (addr < IRAM_SIZE) return Region::IRAM;
    if (addr >= 0x002000 && addr < 0x003000) return Region::IRAM; // mirror
    if (addr >= IOREG_BASE && addr < IOREG_BASE + IOREG_SIZE) return Region::IO;
    if (addr >= SRAM_BASE && addr < SRAM_BASE + SRAM_SIZE) return Region::SRAM;
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
        return sram_[addr - SRAM_BASE];
    case Region::FLASH:
        cycles += flash_wait + 1;
        return flash_[addr - FLASH_BASE];
    case Region::IO: {
        uint32_t idx = (addr - IOREG_BASE) / 2;
        if (idx < io_handlers_.size() && io_handlers_[idx].read)
            return static_cast<uint8_t>(io_handlers_[idx].read(addr & ~1u));
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
        return le16(&sram_[addr - SRAM_BASE]);
    case Region::FLASH:
        cycles += flash_wait + 1;
        return le16(&flash_[addr - FLASH_BASE]);
    case Region::IO: {
        uint32_t idx = (addr - IOREG_BASE) / 2;
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
        sram_[addr - SRAM_BASE] = val;
        return;
    case Region::IO: {
        uint32_t idx = (addr - IOREG_BASE) / 2;
        if (idx < io_handlers_.size() && io_handlers_[idx].write)
            io_handlers_[idx].write(addr & ~1u, val);
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
        put_le16(&sram_[addr - SRAM_BASE], val);
        return;
    case Region::IO: {
        uint32_t idx = (addr - IOREG_BASE) / 2;
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
        put_le32(&sram_[addr - SRAM_BASE], val);
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
    if (addr >= SRAM_BASE && addr < SRAM_BASE + SRAM_SIZE)
        return le16(&sram_[addr - SRAM_BASE]);
    return 0xFFFF;
}
