#pragma once
#include "bus.hpp"

#include <cstdint>
#include <string>
#include <utility>

struct CpuState;

// Parse "0xADDR" or "0xADDR:SIZE" → {addr, size}.
// Size defaults to 4 bytes when the ":SIZE" suffix is omitted.
std::pair<uint32_t, uint32_t> parse_addr_size(const std::string& s);

// Default watchpoint-hit callback: prints addr / val / width / preceding writer.
WpCallback make_wp_callback(const Bus& bus);

// Print a full CPU register snapshot to stderr.
void print_reg_snapshot(const CpuState& s);
