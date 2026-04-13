#pragma once
#include <cstdint>
#include <string>

class Bus;

// Load a bare-metal ELF file into the Bus memory.
// Reads PT_LOAD segments and writes them to the appropriate bus regions.
// Returns the ELF entry point address, or throws std::runtime_error on failure.
uint32_t elf_load(Bus& bus, const std::string& path);
