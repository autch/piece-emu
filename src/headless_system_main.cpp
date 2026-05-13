// piece-emu-headless-system — script-driven, SDL-free P/ECE system frontend.
//
// Goal:
//   Compare gcc33-built and Clang/LLVM-built P/ECE binaries by diffing their
//   per-frame VRAM hash streams.  The first frame where the hashes differ is
//   where codegen begins to misbehave.
//
// Design:
//   * Single thread.  No SDL, no audio, no real-time pacing.
//   * The HSDMA Ch0 completion callback flags "one frame done"; the inner
//     CPU step loop returns to the main loop on that flag.  The main loop
//     then applies any script actions, computes the VRAM FNV-1a-64 hash,
//     emits a stdout line, and re-enters the step loop.
//   * RTC is pinned via --rtc-fixed so two runs of the same PFI + script
//     produce byte-identical stdout.
//
// CLI summary:
//   piece-emu-headless-system <path.pfi> --script <path.txt> [options]
//
// See headless_cli_config.cpp for the full option list.

#include "bus.hpp"
#include "cpu.hpp"
#include "debug_utils.hpp"
#include "diag.hpp"
#include "flash_sst39vf.hpp"
#include "pfi_loader.hpp"
#include "piece_peripherals.hpp"
#include "button_input.hpp"
#include "named_button_input.hpp"
#include "screenshot.hpp"
#include "headless_cli_config.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Script representation
// ---------------------------------------------------------------------------

namespace {

enum class CmdKind { Press, Release, Snapshot, Hash, Quit };

struct ScriptCmd {
    uint64_t   frame = 0;
    CmdKind    kind  = CmdKind::Hash;
    std::string arg;  // button name for press/release, filename for snapshot
};

// Trim trailing comments and surrounding ASCII whitespace.
std::string strip(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    std::size_t hash = s.find('#');
    if (hash != std::string::npos) e = hash;
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> split_ws(const std::string& s)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        std::size_t b = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (b < i) out.emplace_back(s.substr(b, i - b));
    }
    return out;
}

uint64_t parse_u64(const std::string& tok)
{
    return std::stoull(tok, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Script parser
//
// Supported lines (one command per line, '#' for comments, blanks ignored):
//   frame <N> press   <BUTTON>
//   frame <N> release <BUTTON>
//   frame <N> snapshot <filename>
//   frame <N> hash
//   frame <N> quit
//   wait  <K> frames               # syntactic sugar relative to previous frame
//
// Frame numbers are absolute and must be non-decreasing.  Unknown commands
// and unknown button names raise a parse error with a 1-based line number.
// ---------------------------------------------------------------------------
std::vector<ScriptCmd> parse_script(const std::string& path)
{
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open script: " + path);

    std::vector<ScriptCmd> cmds;
    uint64_t last_frame = 0;
    bool     have_prev  = false;
    std::string line;
    std::size_t lineno = 0;

    auto fail = [&](const std::string& why) {
        std::ostringstream os;
        os << path << ":" << lineno << ": " << why;
        throw std::runtime_error(os.str());
    };

    while (std::getline(f, line)) {
        ++lineno;
        std::string s = strip(line);
        if (s.empty()) continue;
        auto tk = split_ws(s);
        if (tk.empty()) continue;

        if (tk[0] == "wait") {
            if (tk.size() != 3 || tk[2] != "frames")
                fail("expected 'wait <N> frames'");
            uint64_t delta;
            try { delta = parse_u64(tk[1]); }
            catch (...) { fail("invalid wait count"); }
            last_frame += delta;
            have_prev = true;
            continue;
        }

        if (tk[0] != "frame")
            fail("unknown command: " + tk[0]);
        if (tk.size() < 3) fail("'frame' needs <N> <verb> [arg]");
        uint64_t fr;
        try { fr = parse_u64(tk[1]); }
        catch (...) { fail("invalid frame number"); }
        if (have_prev && fr < last_frame)
            fail("frames must be non-decreasing");
        last_frame = fr;
        have_prev  = true;

        ScriptCmd c;
        c.frame = fr;

        const std::string& verb = tk[2];
        if (verb == "press" || verb == "release") {
            if (tk.size() != 4) fail("'" + verb + "' needs <BUTTON>");
            ButtonState dummy;
            if (!apply_named_button(tk[3], false, dummy))
                fail("unknown button: " + tk[3]);
            c.kind = (verb == "press") ? CmdKind::Press : CmdKind::Release;
            c.arg  = tk[3];
        } else if (verb == "snapshot") {
            if (tk.size() != 4) fail("'snapshot' needs <filename>");
            c.kind = CmdKind::Snapshot;
            c.arg  = tk[3];
        } else if (verb == "hash") {
            if (tk.size() != 3) fail("'hash' takes no argument");
            c.kind = CmdKind::Hash;
        } else if (verb == "quit") {
            if (tk.size() != 3) fail("'quit' takes no argument");
            c.kind = CmdKind::Quit;
        } else {
            fail("unknown verb: " + verb);
        }
        cmds.push_back(std::move(c));
    }
    return cmds;
}

// ---------------------------------------------------------------------------
// Determinism: parse ISO-8601 "YYYY-MM-DDThh:mm:ss" into seconds-since-2000.
// ---------------------------------------------------------------------------
int64_t days_from_2000_local(int yy, int mm, int dd)
{
    static const int mtbl[12]  = {0,31,59,90,120,151,181,212,243,273,304,334};
    static const int mtblu[12] = {0,31,60,91,121,152,182,213,244,274,305,335};
    int y = yy - 2000;
    int day = y * 365 + (y + 3) / 4;
    bool leap = (y & 3) == 0;
    day += (leap ? mtblu[mm-1] : mtbl[mm-1]);
    day += (dd - 1);
    return day;
}

int64_t parse_iso8601_to_sec2000(const std::string& s)
{
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d",
                    &Y, &M, &D, &h, &mi, &se) != 6)
        throw std::runtime_error("invalid --rtc-fixed: " + s);
    if (Y < 2000 || Y > 2099 || M < 1 || M > 12 || D < 1 || D > 31)
        throw std::runtime_error("--rtc-fixed out of range: " + s);
    return days_from_2000_local(Y, M, D) * 86400
         + static_cast<int64_t>(h)  * 3600
         + static_cast<int64_t>(mi) * 60
         + static_cast<int64_t>(se);
}

// ---------------------------------------------------------------------------
// FNV-1a 64-bit over 88*128 pixel bytes.
// ---------------------------------------------------------------------------
uint64_t fnv1a64(const uint8_t* data, std::size_t n)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    constexpr uint64_t P = 0x100000001B3ULL;
    for (std::size_t i = 0; i < n; i++) {
        h ^= data[i];
        h *= P;
    }
    return h;
}

// Signal-driven quit (SIGINT / SIGTERM).
std::atomic<bool>* g_quit_flag = nullptr;
extern "C" void on_terminate_signal(int) {
    if (g_quit_flag) g_quit_flag->store(true, std::memory_order_relaxed);
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    HeadlessConfig cfg = HeadlessConfig::parse(argc, argv);

    try {
        if (cfg.gdb_port > 0 || cfg.gdb_debug) {
            std::fprintf(stderr,
                "Error: --gdb-port / --gdb-debug not yet implemented in "
                "piece-emu-headless-system.\n"
                "       Use piece-emu-system for live GDB-RSP work.\n");
            return 2;
        }

        std::vector<ScriptCmd> script = parse_script(cfg.script_path);
        int64_t rtc_epoch_sec = parse_iso8601_to_sec2000(cfg.rtc_fixed);

        // Override sram/flash sizes from PFI header unless user pinned them.
        {
            SYSTEMINFO si = pfi_read_sysinfo(cfg.pfi_path);
            if (!cfg.sram_size_explicit && si.sram_end > si.sram_top) {
                cfg.sram_size = si.sram_end - si.sram_top;
                std::fprintf(stderr, "PFI SYSTEMINFO: sram_size=0x%X bytes\n",
                             static_cast<unsigned>(cfg.sram_size));
            }
            if (!cfg.flash_size_explicit && si.pffs_end > Bus::FLASH_BASE) {
                cfg.flash_size = si.pffs_end - Bus::FLASH_BASE;
                std::fprintf(stderr, "PFI SYSTEMINFO: flash_size=0x%X bytes\n",
                             static_cast<unsigned>(cfg.flash_size));
            }
        }

        auto bus = std::make_unique<Bus>(cfg.sram_size, cfg.flash_size);
        auto cpu = std::make_unique<Cpu>(*bus);

        StderrDiagSink diag_sink;
        cpu->set_diag(&diag_sink);
        bus->set_diag(&diag_sink);

        for (auto& s : cfg.wp_write_specs) { auto [a,sz]=parse_addr_size(s); bus->add_watchpoint(a,sz,WpType::WRITE); }
        for (auto& s : cfg.wp_read_specs)  { auto [a,sz]=parse_addr_size(s); bus->add_watchpoint(a,sz,WpType::READ);  }
        for (auto& s : cfg.wp_rw_specs)    { auto [a,sz]=parse_addr_size(s); bus->add_watchpoint(a,sz,WpType::RW);    }
        bus->set_wp_callback(make_wp_callback(*bus));

        std::set<uint32_t> break_addrs;
        for (auto& s : cfg.break_specs)
            break_addrs.insert(static_cast<uint32_t>(std::stoul(s, nullptr, 0)));

        auto periph = std::make_unique<PiecePeripherals>();
        periph->attach(*bus, *cpu);

        // Deterministic RTC base.  Must be applied before pfi_load + first
        // tick; the prescaler is already CPU-cycle-driven so this only
        // affects SEC/MIN/HOUR/DAY readouts and STOP→RUN offset compute.
        periph->rtc.set_fixed_epoch(rtc_epoch_sec);

        // Sound peripheral wiring (silent: no AudioOutput).  Keeping the
        // peripheral wired matters because some apps poll DMA/T16 state
        // even when there is nothing to hear.
        uint64_t total_cycles = 0;
        periph->sound.attach(*bus, periph->hsdma, periph->intc, periph->clk,
                             [&total_cycles]() { return total_cycles; },
                             &periph->t16_ch[1]);

        // Frame boundary: HSDMA Ch0 completion.  We snapshot pixels right
        // here so the LCD state can't slip during the rest of the frame's
        // handling on the same thread.
        bool   frame_ready = false;
        uint8_t pixels[88][128] = {};
        periph->hsdma.on_ch0_complete = [&]() {
            periph->lcd.to_pixels(pixels);
            frame_ready = true;
        };

        // Install SST39VF flash before pfi_load so the image lands in the
        // model.  Sst39vf::for_min_bytes picks the smallest part that
        // covers the requested flash size (400A 512 KB / 160 2 MB).
        bus->install_flash_device(
            std::make_unique<Sst39vf>(
                Sst39vf::for_min_bytes(cfg.flash_size)));

        // PFI is loaded read-only: PfiWriteback is not wired in.
        pfi_load(*bus, cfg.pfi_path);
        uint32_t entry = bus->read32(Bus::FLASH_BASE);
        std::fprintf(stderr, "PFI loaded, reset vector=0x%06X\n", entry);
        cpu->state.pc = entry;

        // Quit flag (set by signals or script 'quit')
        std::atomic<bool> quit_flag{false};
        g_quit_flag = &quit_flag;
        std::signal(SIGINT,  on_terminate_signal);
        std::signal(SIGTERM, on_terminate_signal);

        ButtonState btn;
        periph->portctrl.set_k5(0xFF);
        periph->portctrl.set_k6(0xFF);

        // Walk the script in order; for each emitted frame we consume the
        // commands whose frame number matches.
        std::size_t script_idx = 0;
        uint64_t    frame_no   = 0;

        // Wall-clock guard.  Checked at do_tick boundaries (every few
        // thousand cycles) inside step_until_frame, so apps that stop
        // refreshing the LCD still trip it.  0 disables the guard.
        const auto wall_start    = std::chrono::steady_clock::now();
        const auto wall_deadline = cfg.wall_timeout_sec > 0
            ? wall_start + std::chrono::seconds(cfg.wall_timeout_sec)
            : std::chrono::steady_clock::time_point::max();
        bool wall_timeout_tripped = false;

        // ----- inner CPU step ----------------------------------------------
        // Mirrors the do_tick / next_timer_wake / halt-handling pattern used
        // by CpuRunner, minus all the SDL / pacing / GDB scaffolding.
        // ----------------------------------------------------------------
        static constexpr uint64_t EVENT_INTERVAL = 10'000;
        static constexpr uint64_t MIN_TICK_BURST =  2'000;

        uint64_t next_timer_wake = total_cycles + EVENT_INTERVAL;

        auto update_timer_wake = [&]() {
            uint64_t w = periph->next_wake_cycle();
            if (w == UINT64_MAX) {
                next_timer_wake = total_cycles + EVENT_INTERVAL;
            } else {
                uint64_t earliest = total_cycles + MIN_TICK_BURST;
                uint64_t latest   = total_cycles + EVENT_INTERVAL;
                next_timer_wake   = std::clamp(w, earliest, latest);
            }
        };
        update_timer_wake();

        auto do_tick = [&]() {
            periph->tick(total_cycles);
            periph->intc.set_current_il(cpu->state.psr.il());
            periph->intc.poll();
            update_timer_wake();
            // Wall-clock guard: tied to do_tick frequency (~10k cycles)
            // so a runaway no-LCD-refresh state still gets bounded.
            if (cfg.wall_timeout_sec > 0
                && std::chrono::steady_clock::now() >= wall_deadline) {
                wall_timeout_tripped = true;
                quit_flag.store(true, std::memory_order_relaxed);
            }
        };

        // Clock-change reset: keep timer wake fresh; pacing anchors not
        // needed because we don't pace.
        periph->clk.on_clock_change = [&](uint32_t new_hz) {
            std::fprintf(stderr, "[CLK] CPU clock: %u MHz\n",
                         new_hz / 1'000'000);
            update_timer_wake();
        };

        const bool fast_path = !cfg.trace && break_addrs.empty()
                               && !cfg.max_cycles
                               && !bus->has_watchpoints();

        // Run CPU until the on_ch0_complete callback sets frame_ready, or
        // until we hit a terminal condition.
        auto step_until_frame = [&]() {
            while (!frame_ready && !cpu->state.fault
                   && !quit_flag.load(std::memory_order_relaxed))
            {
                if (cpu->state.in_halt) {
                    bool sleep = (cpu->state.halt_mode
                                  == CpuState::HaltMode::Slp);
                    uint64_t wake = sleep ? periph->sleep_wake_cycle()
                                          : periph->next_wake_cycle();
                    if (wake == UINT64_MAX) {
                        std::fprintf(stderr,
                            "Deadlock: CPU halted with no wakeup after %llu cycles\n",
                            (unsigned long long)total_cycles);
                        quit_flag.store(true, std::memory_order_relaxed);
                        break;
                    }
                    if (wake > total_cycles) total_cycles = wake;
                    do_tick();
                    continue;
                }

                uint64_t stop = next_timer_wake;
                if (fast_path) {
                    while (!cpu->state.in_halt && !cpu->state.fault
                           && !frame_ready && total_cycles < stop) {
                        cpu->step();
                        ++total_cycles;
                    }
                } else {
                    while (!cpu->state.in_halt && !cpu->state.fault
                           && !frame_ready && total_cycles < stop) {
                        if (cfg.trace) {
                            std::string dis = cpu->disasm(cpu->state.pc);
                            std::fprintf(stderr, "  %s\n", dis.c_str());
                        }
                        bus->debug_pc = cpu->state.pc;
                        if (!break_addrs.empty()
                                && break_addrs.count(cpu->state.pc)) {
                            std::fprintf(stderr,
                                "[BREAK] PC=0x%06X\n", cpu->state.pc);
                            print_reg_snapshot(cpu->state);
                        }
                        cpu->step();
                        ++total_cycles;
                        if (cfg.max_cycles
                                && total_cycles >= cfg.max_cycles) {
                            std::fprintf(stderr,
                                "Reached max-cycles limit (%llu)\n",
                                (unsigned long long)total_cycles);
                            quit_flag.store(true,
                                std::memory_order_relaxed);
                            break;
                        }
                    }
                }
                if (cpu->state.fault) break;
                if (total_cycles >= next_timer_wake) do_tick();
            }
        };

        // ----- outer frame loop --------------------------------------------
        bool script_quit = false;
        while (!quit_flag.load(std::memory_order_relaxed)
               && !cpu->state.fault
               && !script_quit
               && frame_no < cfg.max_frames)
        {
            frame_ready = false;
            step_until_frame();
            if (!frame_ready) break; // terminal condition hit

            // ---- apply script for this frame -----------------------------
            std::string press_col, release_col, snapshot_col;
            bool        force_hash = false;
            auto append = [](std::string& dst, const std::string& tok) {
                if (!dst.empty()) dst += ',';
                dst += tok;
            };
            while (script_idx < script.size()
                   && script[script_idx].frame == frame_no)
            {
                const ScriptCmd& c = script[script_idx++];
                switch (c.kind) {
                case CmdKind::Press:
                    apply_named_button(c.arg, true,  btn);
                    append(press_col, c.arg);
                    break;
                case CmdKind::Release:
                    apply_named_button(c.arg, false, btn);
                    append(release_col, c.arg);
                    break;
                case CmdKind::Snapshot: {
                    // std::filesystem::path::operator/ handles absolute
                    // script-side filenames correctly: it discards the
                    // base when the right-hand side is absolute.
                    std::filesystem::path p =
                        std::filesystem::path(cfg.snapshot_dir) / c.arg;
                    std::string saved = write_png_to_path(p.string(), pixels);
                    append(snapshot_col, saved.empty() ? c.arg : saved);
                    break;
                }
                case CmdKind::Hash:
                    force_hash = true;
                    break;
                case CmdKind::Quit:
                    script_quit = true;
                    break;
                }
            }

            // Push the latest ButtonState to PortCtrl now (before the next
            // frame's CPU work).  Same packing the SDL frontend uses.
            periph->portctrl.set_k5(btn.k5);
            periph->portctrl.set_k6(btn.k6);

            // ---- emit hash line -----------------------------------------
            bool emit = force_hash
                     || (cfg.hash_every <= 1)
                     || (frame_no % cfg.hash_every == 0)
                     || script_quit;
            if (emit) {
                uint64_t hash = fnv1a64(&pixels[0][0], 88u * 128u);
                std::printf("frame=%05" PRIu64
                            " cycles=%011" PRIu64
                            " hash=0x%016" PRIx64,
                            frame_no, total_cycles, hash);
                if (!press_col.empty())    std::printf("  press=%s",
                                                       press_col.c_str());
                if (!release_col.empty())  std::printf("  release=%s",
                                                       release_col.c_str());
                if (!snapshot_col.empty()) std::printf("  snapshot=%s",
                                                       snapshot_col.c_str());
                std::printf("\n");
                std::fflush(stdout);
            }

            ++frame_no;
        }

        if (script_idx < script.size())
            std::fprintf(stderr,
                "Note: %zu script command(s) unfired (script extends beyond "
                "actual frame stream)\n",
                script.size() - script_idx);

        periph->clk.on_clock_change = nullptr;
        g_quit_flag = nullptr;

        if (cpu->state.fault) {
            std::fprintf(stderr, "Faulted after %llu cycles, %llu frames\n",
                (unsigned long long)total_cycles,
                (unsigned long long)frame_no);
            print_reg_snapshot(cpu->state);
            return 1;
        }
        if (wall_timeout_tripped) {
            std::fprintf(stderr,
                "Wall-clock timeout (%d s) reached after %llu cycles, "
                "%llu frames\n",
                cfg.wall_timeout_sec,
                (unsigned long long)total_cycles,
                (unsigned long long)frame_no);
            return 3;
        }
        std::fprintf(stderr, "Stopped after %llu cycles, %llu frames\n",
            (unsigned long long)total_cycles,
            (unsigned long long)frame_no);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}

