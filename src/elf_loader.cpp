#include "elf_loader.hpp"
#include "bus.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <format>

// Minimal ELF32 little-endian loader.
// Only reads PT_LOAD segments and the entry point; no dynamic linking.

namespace {

// ELF32 header and program header (little-endian, from elf.h spec)
#pragma pack(push, 1)
struct Elf32_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf32_Phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};
#pragma pack(pop)

static constexpr uint32_t PT_LOAD = 1;
static constexpr uint8_t  ELFMAG0 = 0x7F;
static constexpr uint8_t  ELFCLASS32 = 1;
static constexpr uint8_t  ELFDATA2LSB = 1; // little-endian
static constexpr uint16_t EM_S1C33 = 0xA001; // EPSON S1C33 family (provisional)

// Read a little-endian value from a byte buffer
static uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
}

} // namespace

uint32_t elf_load(Bus& bus, const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error(std::format("Cannot open ELF file: {}", path));

    // Read entire file
    f.seekg(0, std::ios::end);
    std::size_t size = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), size);
    if (!f)
        throw std::runtime_error("Failed to read ELF file");

    if (size < sizeof(Elf32_Ehdr))
        throw std::runtime_error("File too small to be ELF");

    const auto* ehdr = reinterpret_cast<const Elf32_Ehdr*>(data.data());

    // Validate magic
    if (ehdr->e_ident[0] != ELFMAG0 ||
        ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' ||
        ehdr->e_ident[3] != 'F')
        throw std::runtime_error("Not an ELF file");
    if (ehdr->e_ident[4] != ELFCLASS32)
        throw std::runtime_error("Not a 32-bit ELF");
    if (ehdr->e_ident[5] != ELFDATA2LSB)
        throw std::runtime_error("Not a little-endian ELF");

    uint32_t entry   = le32(reinterpret_cast<const uint8_t*>(&ehdr->e_entry));
    uint32_t phoff   = le32(reinterpret_cast<const uint8_t*>(&ehdr->e_phoff));
    uint16_t phnum   = ehdr->e_phnum;
    uint16_t phentsz = ehdr->e_phentsize;

    if (phnum == 0)
        throw std::runtime_error("ELF has no program headers");

    for (uint16_t i = 0; i < phnum; i++) {
        std::size_t phdr_off = phoff + i * phentsz;
        if (phdr_off + sizeof(Elf32_Phdr) > size)
            throw std::runtime_error("Program header out of bounds");

        const auto* phdr = reinterpret_cast<const Elf32_Phdr*>(data.data() + phdr_off);
        uint32_t p_type   = le32(reinterpret_cast<const uint8_t*>(&phdr->p_type));
        uint32_t p_offset = le32(reinterpret_cast<const uint8_t*>(&phdr->p_offset));
        uint32_t p_paddr  = le32(reinterpret_cast<const uint8_t*>(&phdr->p_paddr));
        uint32_t p_filesz = le32(reinterpret_cast<const uint8_t*>(&phdr->p_filesz));
        uint32_t p_memsz  = le32(reinterpret_cast<const uint8_t*>(&phdr->p_memsz));

        if (p_type != PT_LOAD) continue;
        if (p_filesz == 0 && p_memsz == 0) continue;

        if (p_offset + p_filesz > size)
            throw std::runtime_error(
                std::format("PT_LOAD segment at 0x{:06X} extends past file end", p_paddr));

        // Determine destination region and load
        const uint8_t* src = data.data() + p_offset;

        if (p_paddr >= Bus::FLASH_BASE && p_paddr < Bus::FLASH_BASE + Bus::FLASH_MAX) {
            if (p_filesz > 0)
                bus.load_flash(p_paddr - Bus::FLASH_BASE, src, p_filesz);
        } else if (p_paddr >= Bus::SRAM_BASE && p_paddr < Bus::SRAM_BASE + Bus::SRAM_SIZE) {
            if (p_filesz > 0)
                bus.load_sram(p_paddr - Bus::SRAM_BASE, src, p_filesz);
        } else if (p_paddr < Bus::IRAM_SIZE) {
            // Load into internal RAM
            if (p_filesz > 0) {
                if (p_paddr + p_filesz > Bus::IRAM_SIZE)
                    throw std::runtime_error("PT_LOAD segment overflows internal RAM");
                std::memcpy(bus.iram_ptr() + p_paddr, src, p_filesz);
            }
        } else {
            // Unknown region — warn but continue
            std::fprintf(stderr,
                "Warning: PT_LOAD at 0x%08X not in a known memory region, skipping\n",
                p_paddr);
            continue;
        }

        // Zero-fill BSS (memsz > filesz)
        if (p_memsz > p_filesz) {
            uint32_t bss_addr = p_paddr + p_filesz;
            uint32_t bss_size = p_memsz - p_filesz;
            for (uint32_t off = 0; off < bss_size; off++)
                bus.write8(bss_addr + off, 0);
        }
    }

    return entry;
}
