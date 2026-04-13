#include "gdb_rsp.hpp"
#include "gdb_rsp_impl.hpp"
#include "cpu.hpp"

// ============================================================================
// GDB RSP — register access and target description
// ============================================================================

// ---------------------------------------------------------------------------
// Register read / write
// ---------------------------------------------------------------------------

std::string GdbRsp::reg_read_all() {
    std::string s;
    for (int i = 0; i < 16; i++) s += to_hex32(cpu_.state.r[i]);
    s += to_hex32(cpu_.state.sp);
    s += to_hex32(cpu_.state.pc);
    s += to_hex32(cpu_.state.psr.raw);
    s += to_hex32(cpu_.state.alr);
    s += to_hex32(cpu_.state.ahr);
    return s;
}

void GdbRsp::reg_write_all(const std::string& hex) {
    if (hex.size() < 21 * 8) return;
    std::size_t off = 0;
    for (int i = 0; i < 16; i++, off += 8) cpu_.state.r[i]    = from_hex32(hex, off);
    cpu_.state.sp      = from_hex32(hex, off); off += 8;
    cpu_.state.pc      = from_hex32(hex, off); off += 8;
    cpu_.state.psr.raw = from_hex32(hex, off); off += 8;
    cpu_.state.alr     = from_hex32(hex, off); off += 8;
    cpu_.state.ahr     = from_hex32(hex, off);
}

std::string GdbRsp::reg_read_n(int n) {
    if (n < 0 || n > 20) return "E00";
    if (n < 16) return to_hex32(cpu_.state.r[n]);
    switch (n) {
    case 16: return to_hex32(cpu_.state.sp);
    case 17: return to_hex32(cpu_.state.pc);
    case 18: return to_hex32(cpu_.state.psr.raw);
    case 19: return to_hex32(cpu_.state.alr);
    case 20: return to_hex32(cpu_.state.ahr);
    }
    return "E00";
}

void GdbRsp::reg_write_n(int n, uint32_t val) {
    if (n < 0 || n > 20) return;
    if (n < 16) { cpu_.state.r[n] = val; return; }
    switch (n) {
    case 16: cpu_.state.sp      = val; break;
    case 17: cpu_.state.pc      = val; break;
    case 18: cpu_.state.psr.raw = val; break;
    case 19: cpu_.state.alr     = val; break;
    case 20: cpu_.state.ahr     = val; break;
    }
}

// LLDB qRegisterInfo<n> response.
// Fields: name, bitsize, offset (in the g-packet register block), encoding,
//         format, set, dwarf register number, optional generic role.
std::string GdbRsp::reg_info(int n) {
    if (n < 0 || n > 20) return "E45";

    struct Info { const char* name; const char* generic; const char* set; };
    static constexpr Info kInfo[21] = {
        {"r0",  nullptr,  "General Purpose Registers"},
        {"r1",  nullptr,  "General Purpose Registers"},
        {"r2",  nullptr,  "General Purpose Registers"},
        {"r3",  nullptr,  "General Purpose Registers"},
        {"r4",  nullptr,  "General Purpose Registers"},
        {"r5",  nullptr,  "General Purpose Registers"},
        {"r6",  nullptr,  "General Purpose Registers"},
        {"r7",  nullptr,  "General Purpose Registers"},
        {"r8",  nullptr,  "General Purpose Registers"},
        {"r9",  nullptr,  "General Purpose Registers"},
        {"r10", nullptr,  "General Purpose Registers"},
        {"r11", nullptr,  "General Purpose Registers"},
        {"r12", nullptr,  "General Purpose Registers"},
        {"r13", nullptr,  "General Purpose Registers"},
        {"r14", nullptr,  "General Purpose Registers"},
        {"r15", nullptr,  "General Purpose Registers"},
        {"sp",  "sp",     "General Purpose Registers"},
        {"pc",  "pc",     "General Purpose Registers"},
        {"psr", "flags",  "General Purpose Registers"},
        {"alr", nullptr,  "Multiply Registers"},
        {"ahr", nullptr,  "Multiply Registers"},
    };

    std::string resp = std::format(
        "name:{};bitsize:32;offset:{};encoding:uint;format:hex;endian:little;set:{};dwarf:{};",
        kInfo[n].name, n * 4, kInfo[n].set, n);
    if (kInfo[n].generic)
        resp += std::format("generic:{};", kInfo[n].generic);
    return resp;
}

// ---------------------------------------------------------------------------
// Target XML for qXfer:features:read:target.xml
// ---------------------------------------------------------------------------

static constexpr const char* kTargetXml = R"(<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target version="1.0">
  <feature name="org.gnu.gdb.s1c33.core">
    <reg name="r0"  bitsize="32" regnum="0"  type="uint32"/>
    <reg name="r1"  bitsize="32" regnum="1"  type="uint32"/>
    <reg name="r2"  bitsize="32" regnum="2"  type="uint32"/>
    <reg name="r3"  bitsize="32" regnum="3"  type="uint32"/>
    <reg name="r4"  bitsize="32" regnum="4"  type="uint32"/>
    <reg name="r5"  bitsize="32" regnum="5"  type="uint32"/>
    <reg name="r6"  bitsize="32" regnum="6"  type="uint32"/>
    <reg name="r7"  bitsize="32" regnum="7"  type="uint32"/>
    <reg name="r8"  bitsize="32" regnum="8"  type="uint32"/>
    <reg name="r9"  bitsize="32" regnum="9"  type="uint32"/>
    <reg name="r10" bitsize="32" regnum="10" type="uint32"/>
    <reg name="r11" bitsize="32" regnum="11" type="uint32"/>
    <reg name="r12" bitsize="32" regnum="12" type="uint32"/>
    <reg name="r13" bitsize="32" regnum="13" type="uint32"/>
    <reg name="r14" bitsize="32" regnum="14" type="uint32"/>
    <reg name="r15" bitsize="32" regnum="15" type="uint32"/>
    <reg name="sp"  bitsize="32" regnum="16" type="data_ptr"/>
    <reg name="pc"  bitsize="32" regnum="17" type="code_ptr"/>
    <reg name="psr" bitsize="32" regnum="18" type="uint32"/>
    <reg name="alr" bitsize="32" regnum="19" type="uint32"/>
    <reg name="ahr" bitsize="32" regnum="20" type="uint32"/>
  </feature>
</target>)";

std::string GdbRsp::handle_qXfer(const std::string& pkt) {
    // pkt = "qXfer:features:read:target.xml:OFF,LEN"
    auto colon = pkt.rfind(':');
    auto comma  = pkt.find(',', colon);
    unsigned off = std::stoul(pkt.substr(colon + 1, comma - colon - 1), nullptr, 16);
    unsigned len = std::stoul(pkt.substr(comma + 1), nullptr, 16);

    std::string_view xml(kTargetXml);
    if (off >= xml.size()) return "l";
    auto chunk = xml.substr(off, len);
    bool last  = (off + chunk.size() >= xml.size());
    return std::string(1, last ? 'l' : 'm') + std::string(chunk);
}
