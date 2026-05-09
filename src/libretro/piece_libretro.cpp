// piece_libretro — libretro core entry points for piece-emu.
//
// Phase 1 scope (see plans/cmake-so-libretro-ticklish-russell.md):
//   * retro_init / retro_load_game / retro_run / retro_unload_game / retro_deinit
//   * LCD video (RGB565, 128×88)
//   * RETRO_DEVICE_JOYPAD → K5/K6 button bits
//   * 32 kHz mono int16 audio (duplicated to stereo for audio_batch_cb)
//   * Fixed 60 fps; CPU runs cpu_clock_hz / 60 cycles per retro_run().
//
// Out of scope (Phase 2+): core options, SAVE_RAM (PFFS persistence),
// retro_serialize / retro_unserialize, ELF loading.

#include "libretro.h"

#include "bus.hpp"
#include "cpu.hpp"
#include "piece_peripherals.hpp"
#include "flash_sst39vf.hpp"
#include "pfi_loader.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

// ---- libretro callbacks (set by frontend in retro_set_*_cb) -----------------
retro_video_refresh_t       video_cb       = nullptr;
retro_audio_sample_batch_t  audio_batch_cb = nullptr;
retro_input_poll_t          input_poll_cb  = nullptr;
retro_input_state_t         input_state_cb = nullptr;
retro_environment_t         environ_cb     = nullptr;

// ---- Emulator state — singleton (libretro is single-instance per process) --
std::unique_ptr<Bus>              g_bus;
std::unique_ptr<Cpu>              g_cpu;
std::unique_ptr<PiecePeripherals> g_periph;
std::uint64_t                     g_total_cycles   = 0;
std::uint64_t                     g_next_timer_wake = 0;
std::uint32_t                     g_cached_cpu_hz   = 24'000'000;

// HSDMA Ch0 completion sets this; retro_run() consumes and clears.
bool                              g_frame_pending  = false;

// ---- Display ---------------------------------------------------------------
constexpr int FB_W = 128;
constexpr int FB_H = 88;
constexpr double TARGET_FPS = 60.0;

uint16_t      g_video_pixels[FB_W * FB_H];
std::uint8_t  g_lcd_pixels[FB_H][FB_W];

// 4-level grayscale → RGB565 (0=white, 1=light, 2=dark, 3=black).
constexpr uint16_t kPalette[4] = { 0xFFFF, 0xAD55, 0x52AA, 0x0000 };

// ---- Core options ----------------------------------------------------------
// Cached value of "piece_swap_ab".  Re-read whenever the frontend signals
// a variable update (RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE).
bool g_swap_ab = false;

// Core option definitions (v2).  The frontend keeps pointers to these,
// so they must outlive the core (static const is correct).
const retro_core_option_v2_definition kOptionDefs[] = {
    {
        "piece_swap_ab",
        "Swap A/B Buttons",
        nullptr,
        "Swap the mapping between RetroArch's A/B buttons and the P/ECE A/B "
        "buttons.  Off (default): A=P/ECE A, B=P/ECE B (RetroArch standard).  "
        "On: A=P/ECE B, B=P/ECE A.",
        nullptr,
        nullptr,
        {
            { "off", "Off (RetroArch standard)" },
            { "on",  "On (Swapped)" },
            { nullptr, nullptr },
        },
        "off",
    },
    { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr },
};

const retro_core_options_v2 kOptionsV2 = {
    nullptr,        // categories — none for now
    const_cast<retro_core_option_v2_definition*>(kOptionDefs),
};

// ---- Input descriptors -----------------------------------------------------
// Static array describing how each JOYPAD button maps onto the P/ECE.
// `description` strings must remain valid until retro_unload_game() returns.
const retro_input_descriptor kInputDescriptors[] = {
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up"    },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down"  },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left"  },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A Button"    },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B Button"    },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start"       },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"      },
    { 0, 0, 0, 0, nullptr },
};

// Read "piece_swap_ab" from the frontend and cache it in g_swap_ab.
void update_core_options()
{
    if (!environ_cb) return;
    retro_variable var = { "piece_swap_ab", nullptr };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        g_swap_ab = (std::strcmp(var.value, "on") == 0);
    }
}

// ---- Audio -----------------------------------------------------------------
// Drain buffer for one retro_run() iteration.  At 32 kHz / 60 fps the per-
// frame mono budget is ~533 samples; we size for 4× headroom in case the
// frontend pulls multiple frames at once or audio bursts run ahead.
constexpr std::size_t AUDIO_DRAIN = 4096;
std::int16_t g_audio_mono[AUDIO_DRAIN];
std::int16_t g_audio_stereo[AUDIO_DRAIN * 2];

} // namespace

// ===========================================================================
// libretro entry points
// ===========================================================================

extern "C" {

RETRO_API unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    // Register core options.  Only V2 is supported here — V2 has been the
    // recommended path since RetroArch 1.9 (2021), and falling back to V0/V1
    // would just clutter the file.  If the frontend doesn't accept V2 we
    // simply ship without options (g_swap_ab stays false).
    unsigned options_version = 0;
    if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &options_version)
        && options_version >= 2) {
        cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2,
           const_cast<retro_core_options_v2*>(&kOptionsV2));
    }
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)     { video_cb       = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t)          { /* unused — we use audio_batch */ }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)           { input_poll_cb  = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)         { input_state_cb = cb; }

RETRO_API void retro_get_system_info(struct retro_system_info* info)
{
    std::memset(info, 0, sizeof(*info));
    info->library_name     = "piece-emu";
    info->library_version  = "0.1";
    info->valid_extensions = "pfi";
    info->need_fullpath    = false;     // give us the data blob via game->data
    info->block_extract    = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info)
{
    std::memset(info, 0, sizeof(*info));
    info->geometry.base_width   = FB_W;
    info->geometry.base_height  = FB_H;
    info->geometry.max_width    = FB_W;
    info->geometry.max_height   = FB_H;
    info->geometry.aspect_ratio = static_cast<float>(FB_W) / static_cast<float>(FB_H);
    info->timing.fps            = TARGET_FPS;
    info->timing.sample_rate    = static_cast<double>(Sound::SAMPLE_RATE); // 32000
}

RETRO_API void retro_init(void)
{
    if (environ_cb) {
        enum retro_pixel_format pf = RETRO_PIXEL_FORMAT_RGB565;
        environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pf);
    }
}

RETRO_API void retro_deinit(void)
{
    g_bus.reset();
    g_cpu.reset();
    g_periph.reset();
    g_total_cycles    = 0;
    g_next_timer_wake = 0;
    g_frame_pending   = false;
}

RETRO_API void retro_set_controller_port_device(unsigned, unsigned) { /* fixed JOYPAD */ }

RETRO_API bool retro_load_game(const struct retro_game_info* game)
{
    if (!game || !game->data || game->size == 0) return false;

    const auto* data = static_cast<const std::uint8_t*>(game->data);
    SYSTEMINFO si;
    try {
        si = pfi_read_sysinfo_blob(data, game->size);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "piece_libretro: PFI header parse failed: %s\n", e.what());
        return false;
    }

    // Bus sizing follows piece-emu-system: derive from PFI SYSTEMINFO when
    // present, otherwise fall back to defaults.
    std::size_t sram_size  = 0x040000; // 256 KB
    std::size_t flash_size = 0x080000; // 512 KB
    if (si.sram_end > si.sram_top)     sram_size  = si.sram_end - si.sram_top;
    if (si.pffs_end > Bus::FLASH_BASE) flash_size = si.pffs_end - Bus::FLASH_BASE;

    g_bus    = std::make_unique<Bus>(sram_size, flash_size);
    g_cpu    = std::make_unique<Cpu>(*g_bus);
    g_periph = std::make_unique<PiecePeripherals>();
    g_periph->attach(*g_bus, *g_cpu);
    g_periph->sound.attach(*g_bus, g_periph->hsdma, g_periph->intc, g_periph->clk,
                           []() { return g_total_cycles; },
                           &g_periph->t16_ch[1]);

    // Single-thread mode: just flag "frame ready" on HSDMA Ch0 completion;
    // retro_run() converts and emits the frame.
    g_periph->hsdma.on_ch0_complete = []() { g_frame_pending = true; };

    g_bus->install_flash_device(
        std::make_unique<Sst39vf>(Sst39vf::for_min_bytes(flash_size)));

    try {
        pfi_load_blob(*g_bus, data, game->size);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "piece_libretro: PFI load failed: %s\n", e.what());
        return false;
    }

    g_cpu->state.pc      = g_bus->read32(Bus::FLASH_BASE);
    g_cached_cpu_hz      = g_periph->clk.cpu_clock_hz();
    g_total_cycles       = 0;
    g_next_timer_wake    = 0;
    g_frame_pending      = false;

    // Publish the input descriptor table so the frontend can show
    // P/ECE-specific labels in its input menu.
    if (environ_cb)
        environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
                   const_cast<retro_input_descriptor*>(kInputDescriptors));

    update_core_options();
    return true;
}

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t)
{
    return false;
}

RETRO_API void retro_unload_game(void)
{
    g_bus.reset();
    g_cpu.reset();
    g_periph.reset();
    g_total_cycles    = 0;
    g_next_timer_wake = 0;
    g_frame_pending   = false;
}

RETRO_API void retro_reset(void)
{
    if (!g_periph || !g_cpu) return;
    g_periph->reset(/*cold=*/true);
    g_cpu->reset();
    g_cpu->state.pc      = g_bus->read32(Bus::FLASH_BASE);
    g_total_cycles       = 0;
    g_next_timer_wake    = 0;
    g_frame_pending      = false;
}

RETRO_API void retro_run(void)
{
    if (!g_bus || !g_cpu || !g_periph) return;

    // -------- 1) Input → K5/K6 -------------------------------------------
    if (input_poll_cb) input_poll_cb();

    // Pick up frontend-side option changes.  GET_VARIABLE_UPDATE is cheap
    // (a single bool) and only triggers update_core_options() when the user
    // actually changed a setting.
    bool opts_dirty = false;
    if (environ_cb
        && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &opts_dirty)
        && opts_dirty)
        update_core_options();

    std::uint8_t k5 = 0xFF, k6 = 0xFF;
    if (input_state_cb) {
        auto down = [](unsigned id) -> bool {
            return input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
        };
        // K6: bit0=右 bit1=左 bit2=下 bit3=上 bit4=B bit5=A
        if (down(RETRO_DEVICE_ID_JOYPAD_RIGHT))  k6 &= ~(1 << 0);
        if (down(RETRO_DEVICE_ID_JOYPAD_LEFT))   k6 &= ~(1 << 1);
        if (down(RETRO_DEVICE_ID_JOYPAD_DOWN))   k6 &= ~(1 << 2);
        if (down(RETRO_DEVICE_ID_JOYPAD_UP))     k6 &= ~(1 << 3);
        // A/B mapping respects the swap_ab core option.
        const unsigned id_to_p_a = g_swap_ab ? RETRO_DEVICE_ID_JOYPAD_B
                                             : RETRO_DEVICE_ID_JOYPAD_A;
        const unsigned id_to_p_b = g_swap_ab ? RETRO_DEVICE_ID_JOYPAD_A
                                             : RETRO_DEVICE_ID_JOYPAD_B;
        if (down(id_to_p_b)) k6 &= ~(1 << 4); // P/ECE B = K64
        if (down(id_to_p_a)) k6 &= ~(1 << 5); // P/ECE A = K65
        // K5: bit3=SELECT bit4=START
        if (down(RETRO_DEVICE_ID_JOYPAD_SELECT)) k5 &= ~(1 << 3);
        if (down(RETRO_DEVICE_ID_JOYPAD_START))  k5 &= ~(1 << 4);
    }
    g_periph->portctrl.set_k5(k5);
    g_periph->portctrl.set_k6(k6);

    // -------- 2) CPU loop — one frame's worth of cycles ------------------
    // Mirror the fast-path inner loop in CpuRunner::run() but without any
    // pacing (libretro frontend handles wall-clock pacing).
    const std::uint64_t target =
        g_total_cycles + static_cast<std::uint64_t>(
            static_cast<double>(g_cached_cpu_hz) / TARGET_FPS);

    auto do_tick = [&]() {
        g_periph->tick(g_total_cycles);
        g_periph->intc.set_current_il(g_cpu->state.psr.il());
        g_periph->intc.poll();
        std::uint64_t w = g_periph->next_wake_cycle();
        g_next_timer_wake = (w == UINT64_MAX) ? target : std::min(w, target);
    };

    while (g_total_cycles < target && !g_cpu->state.fault) {
        std::uint64_t stop = std::min(g_next_timer_wake, target);
        while (!g_cpu->state.in_halt && !g_cpu->state.fault
               && g_total_cycles < stop) {
            g_cpu->step();
            ++g_total_cycles;
        }
        if (g_cpu->state.fault) break;

        if (g_total_cycles >= g_next_timer_wake) do_tick();

        if (g_cpu->state.in_halt) {
            const bool sleep = (g_cpu->state.halt_mode == CpuState::HaltMode::Slp);
            std::uint64_t wake = sleep ? g_periph->sleep_wake_cycle()
                                       : g_periph->next_wake_cycle();
            if (wake == UINT64_MAX || wake >= target) {
                // No wake within this frame — skip to frame end.  Button
                // input via set_k5/k6 was already published; the CPU stays
                // halted until the next retro_run() picks up a new event.
                g_total_cycles = target;
            } else {
                g_total_cycles = wake;
                do_tick();
            }
        }
    }

    // Re-cache CPU clock in case the kernel changed it mid-frame.  (Phase 2
    // will SET_SYSTEM_AV_INFO to inform the frontend.)
    g_cached_cpu_hz = g_periph->clk.cpu_clock_hz();

    // -------- 3) Video ---------------------------------------------------
    if (g_frame_pending) {
        g_periph->lcd.to_pixels(g_lcd_pixels);
        g_frame_pending = false;
    }
    // Always emit a frame (libretro expects video_cb every retro_run); on
    // frames with no LCD update we just resend the previous pixel buffer.
    for (int y = 0; y < FB_H; ++y)
        for (int x = 0; x < FB_W; ++x)
            g_video_pixels[y * FB_W + x] = kPalette[g_lcd_pixels[y][x] & 3];
    if (video_cb)
        video_cb(g_video_pixels, FB_W, FB_H, FB_W * sizeof(uint16_t));

    // -------- 4) Audio drain → audio_batch_cb ---------------------------
    if (audio_batch_cb) {
        std::size_t got = g_periph->sound.pop(g_audio_mono, AUDIO_DRAIN);
        if (got > 0) {
            // mono → stereo (duplicate L=R)
            for (std::size_t i = 0; i < got; ++i) {
                g_audio_stereo[i * 2 + 0] = g_audio_mono[i];
                g_audio_stereo[i * 2 + 1] = g_audio_mono[i];
            }
            audio_batch_cb(g_audio_stereo, got);
        }
    }
}

// ---- Save state stubs (Phase 2) --------------------------------------------
RETRO_API size_t retro_serialize_size(void)                           { return 0; }
RETRO_API bool   retro_serialize(void*, size_t)                       { return false; }
RETRO_API bool   retro_unserialize(const void*, size_t)               { return false; }

// ---- Cheats / memory / region (unused) -------------------------------------
RETRO_API void   retro_cheat_reset(void)                              { /* no-op */ }
RETRO_API void   retro_cheat_set(unsigned, bool, const char*)         { /* no-op */ }
RETRO_API void*  retro_get_memory_data(unsigned)                      { return nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned)                      { return 0; }
RETRO_API unsigned retro_get_region(void)                             { return RETRO_REGION_NTSC; }

} // extern "C"
