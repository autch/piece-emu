#include "flash_sst39vf.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {

bool is_power_of_two(std::size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

uint8_t log2_size(std::size_t n) {
    uint8_t b = 0;
    while ((std::size_t(1) << b) < n) ++b;
    return b;
}

void put_le16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0])
         | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

} // namespace

// ---- construction ----------------------------------------------------------

Sst39vf::Sst39vf(std::size_t mem_size)
    : mem_(mem_size, 0xFF)
    , dirty_(mem_size / SECTOR_SIZE, false)
{
    if (!is_power_of_two(mem_size) || mem_size < SECTOR_SIZE)
        throw std::invalid_argument(
            "Sst39vf: mem_size must be a power of two >= 4 KB");

    cfi_device_size_  = log2_size(mem_size);
    cfi_sector_count_ = static_cast<uint16_t>(mem_size / SECTOR_SIZE - 1);
    cfi_block_count_  = static_cast<uint16_t>(mem_size / BLOCK_SIZE - 1);

    // Pick a plausible Device ID for the recognised P/ECE variants.
    switch (mem_size) {
    case 0x80000:  dev_id_ = 0x2780; break; // SST39VF400A (4 Mbit / 512 KB)
    case 0x100000: dev_id_ = 0x2781; break; // SST39VF800A (8 Mbit / 1 MB)
    case 0x200000: dev_id_ = 0x2782; break; // SST39VF160  (16 Mbit / 2 MB)
    default:       dev_id_ = 0x2780; break; // unknown — pick something stable
    }
}

Sst39vf Sst39vf::for_min_bytes(std::size_t min_bytes) {
    return Sst39vf(min_bytes <= 0x80000 ? 0x80000 : 0x200000);
}

void Sst39vf::reset() {
    state_ = State::Normal;
}

// ---- read ------------------------------------------------------------------

uint16_t Sst39vf::read16(uint32_t offset) const {
    offset &= static_cast<uint32_t>(mem_.size() - 1);
    offset &= ~uint32_t{1};

    switch (state_) {
    case State::Normal:
    case State::Enter1:
    case State::Enter2:
    case State::Enter3:
    case State::Enter4:
    case State::Enter5:
    case State::WordProgram:
        // Reads in any non-query state simply return device memory.
        // (Real silicon returns Data#-Polling / Toggle status during an
        //  in-flight Program/Erase, but our emulator completes commands
        //  synchronously.)
        return le16(&mem_[offset]);

    case State::CfiQuery:
        return cfi_query(offset >> 1);

    case State::SoftwareId:
        return software_id(offset >> 1);
    }
    return 0xFFFF; // unreachable
}

uint8_t Sst39vf::read8(uint32_t offset) const {
    uint16_t hw = read16(offset & ~uint32_t{1});
    return (offset & 1) ? static_cast<uint8_t>(hw >> 8)
                        : static_cast<uint8_t>(hw);
}

// ---- write -----------------------------------------------------------------

void Sst39vf::write16(uint32_t offset, uint16_t data) {
    offset &= static_cast<uint32_t>(mem_.size() - 1);
    offset &= ~uint32_t{1};

    const uint32_t hw_addr = offset >> 1;
    const uint8_t  cmd     = static_cast<uint8_t>(data);

    // Short Exit (XX:F0) — recognised from any non-program state.
    // The kernel issues this defensively before each Word-Program.  Real
    // silicon would treat F0 inside WordProgram as data 0x00F0 to write,
    // so we honour the "any non-WP" rule following piemu.
    if (state_ != State::WordProgram && cmd == 0xF0) {
        state_ = State::Normal;
        return;
    }

    switch (state_) {
    case State::Normal:
        if (hw_addr == CMD_ADDR_1 && cmd == 0xAA)
            state_ = State::Enter1;
        // else: lenient — stale or corrupt sequence is silently ignored
        break;

    case State::Enter1: // got 5555:AA
        if (hw_addr == CMD_ADDR_2 && cmd == 0x55)
            state_ = State::Enter2;
        else
            state_ = State::Normal;
        break;

    case State::Enter2: // got 5555:AA / 2AAA:55
        if (hw_addr == CMD_ADDR_1) {
            switch (cmd) {
            case 0x80: state_ = State::Enter3;      break; // erase setup
            case 0x90: state_ = State::SoftwareId;  break;
            case 0x98: state_ = State::CfiQuery;    break;
            case 0xA0: state_ = State::WordProgram; break;
            default:   state_ = State::Normal;      break;
            }
        } else {
            state_ = State::Normal;
        }
        break;

    case State::Enter3: // got AA / 55 / 80; next must be 5555:AA
        if (hw_addr == CMD_ADDR_1 && cmd == 0xAA)
            state_ = State::Enter4;
        else
            state_ = State::Normal;
        break;

    case State::Enter4: // got AA / 55 / 80 / AA; next must be 2AAA:55
        if (hw_addr == CMD_ADDR_2 && cmd == 0x55)
            state_ = State::Enter5;
        else
            state_ = State::Normal;
        break;

    case State::Enter5: // got the full unlock; this write encodes the erase
        switch (cmd) {
        case 0x10: // Chip-Erase
            if (hw_addr == CMD_ADDR_1) {
                std::fill(mem_.begin(), mem_.end(), 0xFF);
                std::fill(dirty_.begin(), dirty_.end(), true);
                if (on_dirty_) {
                    for (std::size_t i = 0; i < dirty_.size(); ++i)
                        on_dirty_(static_cast<uint32_t>(i));
                }
            }
            break;
        case 0x30: { // Sector-Erase (4 KB)
            const uint32_t base =
                offset & ~static_cast<uint32_t>(SECTOR_SIZE - 1);
            std::fill(mem_.begin() + base,
                      mem_.begin() + base + SECTOR_SIZE, 0xFF);
            mark_dirty(base, SECTOR_SIZE);
            break;
        }
        case 0x50: { // Block-Erase (64 KB)
            const uint32_t base =
                offset & ~static_cast<uint32_t>(BLOCK_SIZE - 1);
            std::fill(mem_.begin() + base,
                      mem_.begin() + base + BLOCK_SIZE, 0xFF);
            mark_dirty(base, BLOCK_SIZE);
            break;
        }
        default:
            // Unknown 6th-cycle command — silently ignore.
            break;
        }
        state_ = State::Normal;
        break;

    case State::SoftwareId:
    case State::CfiQuery:
        // While in ID/CFI mode, AA/55 are part of the long-form Exit
        // sequence (5555/2AAA/5555/F0); the F0 is already caught above.
        // We do NOT transition to an unlock state here — the Exit is
        // recognised on F0.  Other writes are silently ignored.
        break;

    case State::WordProgram:
        // Commit the halfword to memory.  Real silicon performs an AND
        // (programming can only flip 1→0); we follow piemu and overwrite.
        put_le16(&mem_[offset], data);
        mark_dirty(offset, 2);
        state_ = State::Normal;
        break;
    }
}

// ---- helpers ---------------------------------------------------------------

void Sst39vf::mark_dirty(uint32_t byte_offset, std::size_t span_bytes) {
    if (mem_.empty()) return;
    const uint32_t start = byte_offset / static_cast<uint32_t>(SECTOR_SIZE);
    const uint32_t last_byte = byte_offset + static_cast<uint32_t>(span_bytes) - 1;
    const uint32_t end   = last_byte / static_cast<uint32_t>(SECTOR_SIZE);
    for (uint32_t s = start; s <= end && s < dirty_.size(); ++s) {
        if (!dirty_[s]) {
            dirty_[s] = true;
            if (on_dirty_) on_dirty_(s);
        }
    }
}

uint16_t Sst39vf::cfi_query(uint32_t cfi_halfword_addr) const {
    // CFI halfword address layout (Tables 5/6/7 of the SST39VF400A and
    // SST39VF160 datasheets — geometry differs only at 0x27/0x2D-0x34).
    switch (cfi_halfword_addr) {
    // CFI Query Identification String
    case 0x10: return 0x0051; // 'Q'
    case 0x11: return 0x0052; // 'R'
    case 0x12: return 0x0059; // 'Y'

    // Primary OEM command set (AMD/Fujitsu compat = 0x0007)
    case 0x13: return 0x0001;
    case 0x14: return 0x0007;
    case 0x15: return 0x0000;
    case 0x16: return 0x0000;
    case 0x17: return 0x0000; // alternate OEM command set: none
    case 0x18: return 0x0000;
    case 0x19: return 0x0000;
    case 0x1A: return 0x0000;

    // System interface information
    case 0x1B: return 0x0027; // VDD min  (2.7 V) — SST39VF series
    case 0x1C: return 0x0036; // VDD max  (3.6 V)
    case 0x1D: return 0x0000; // VPP min
    case 0x1E: return 0x0000; // VPP max
    case 0x1F: return 0x0004; // typ. word-program time = 2^4 µs = 16 µs
    case 0x20: return 0x0000; // multi-byte buffer not supported
    case 0x21: return 0x0004; // typ. sector/block erase  = 2^4 ms = 16 ms
    case 0x22: return 0x0006; // typ. chip erase          = 2^6 ms = 64 ms
    case 0x23: return 0x0001; // max factor for word-program timeout
    case 0x24: return 0x0000;
    case 0x25: return 0x0001;
    case 0x26: return 0x0001;

    // Device geometry
    case 0x27: return cfi_device_size_;       // log2(bytes)
    case 0x28: return 0x0001;                 // x16 only
    case 0x29: return 0x0000;
    case 0x2A: return 0x0000;                 // multi-byte buffer
    case 0x2B: return 0x0000;
    case 0x2C: return 0x0002;                 // 2 erase region descriptors
    // Region 1: uniform 4 KB sectors
    case 0x2D: return static_cast<uint16_t>(cfi_sector_count_ & 0x00FFu);
    case 0x2E: return static_cast<uint16_t>((cfi_sector_count_ >> 8) & 0x00FFu);
    case 0x2F: return 0x0010;                 // z = 16 (× 256 bytes = 4 KB)
    case 0x30: return 0x0000;
    // Region 2: uniform 64 KB blocks
    case 0x31: return static_cast<uint16_t>(cfi_block_count_ & 0x00FFu);
    case 0x32: return static_cast<uint16_t>((cfi_block_count_ >> 8) & 0x00FFu);
    case 0x33: return 0x0000;                 // z low byte
    case 0x34: return 0x0001;                 // z = 256 (× 256 bytes = 64 KB)
    default:
        return 0x0000;
    }
}

uint16_t Sst39vf::software_id(uint32_t id_halfword_addr) const {
    switch (id_halfword_addr) {
    case 0x0000: return mfr_id_;  // 0x00BF
    case 0x0001: return dev_id_;
    default:     return 0x0000;
    }
}

// ---- bulk operations -------------------------------------------------------

void Sst39vf::load_image(uint32_t offset, const uint8_t* data, std::size_t size) {
    if (static_cast<std::size_t>(offset) + size > mem_.size())
        throw std::out_of_range("Sst39vf::load_image: offset+size exceeds device size");
    std::memcpy(mem_.data() + offset, data, size);
}

bool Sst39vf::any_dirty() const {
    return std::any_of(dirty_.begin(), dirty_.end(),
                       [](bool b){ return b; });
}

bool Sst39vf::is_sector_dirty(uint32_t sector_idx) const {
    return sector_idx < dirty_.size() && dirty_[sector_idx];
}

void Sst39vf::clear_dirty() {
    std::fill(dirty_.begin(), dirty_.end(), false);
}

void Sst39vf::for_each_dirty_sector(const std::function<void(uint32_t)>& cb) const {
    for (std::size_t i = 0; i < dirty_.size(); ++i)
        if (dirty_[i]) cb(static_cast<uint32_t>(i));
}
