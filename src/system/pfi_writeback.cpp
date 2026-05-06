#include "pfi_writeback.hpp"

#include <chrono>
#include <cstdio>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

uint64_t now_us_steady() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

// ---------------------------------------------------------------------------
// PfiWriteback
// ---------------------------------------------------------------------------

PfiWriteback::PfiWriteback() = default;

PfiWriteback::~PfiWriteback() {
    // Last-resort flush.  Frontend code should call shutdown_flush()
    // explicitly so that errors are reported before destructor; this is
    // a safety net for early-return / exception paths.
    if (mode_ == Mode::WriteBack && file_.is_open()) {
        if (flash_ && flash_->any_dirty()) {
            flush_dirty_sectors();
            file_.sync();
        }
    }
}

bool PfiWriteback::attach(FlashDevice*       flash,
                          Mode               mode,
                          const std::string& pfi_path,
                          uint32_t           flash_offset_in_pfi,
                          int                debounce_ms)
{
    flash_                = flash;
    path_                 = pfi_path;
    flash_offset_in_pfi_  = flash_offset_in_pfi;
    debounce_ms_          = debounce_ms;
    mode_                 = mode;

    if (mode_ == Mode::WriteBack && flash_ && flash_->sector_size() == 0) {
        // FlatFlashRom or other read-only model — writeback would never
        // see a dirty sector.  Downgrade silently.
        std::fprintf(stderr,
            "PfiWriteback: flash device has no dirty tracking — "
            "writeback downgraded to read-only\n");
        mode_ = Mode::ReadOnly;
    }

    if (mode_ == Mode::WriteBack) {
        if (!file_.open_rw(pfi_path)) {
            std::fprintf(stderr,
                "PfiWriteback: cannot open '%s' for writing — "
                "downgrading to read-only\n", pfi_path.c_str());
            mode_ = Mode::ReadOnly;
        }
    }

    if (flash_) {
        // Hook the dirty callback regardless of mode — even in ReadOnly we
        // may want diagnostic visibility into kernel write attempts later.
        flash_->set_dirty_callback([this](uint32_t /*sector*/) {
            // Use a steady clock; only deltas matter.
            last_dirty_us_.store(now_us_steady(),
                                 std::memory_order_relaxed);
        });
    }

    if (mode_ == Mode::WriteBack) {
        std::fprintf(stderr,
            "PfiWriteback: '%s' (flash offset 0x%X, debounce %d ms)\n",
            pfi_path.c_str(),
            static_cast<unsigned>(flash_offset_in_pfi_),
            debounce_ms_);
    }
    return true;
}

bool PfiWriteback::has_pending_dirty() const {
    return last_dirty_us_.load(std::memory_order_relaxed) != 0;
}

void PfiWriteback::poll(uint64_t now_us) {
    if (mode_ != Mode::WriteBack || !flash_ || !file_.is_open())
        return;
    const uint64_t last = last_dirty_us_.load(std::memory_order_relaxed);
    if (last == 0) return;                          // nothing pending
    if (now_us < last) return;                      // clock skew — wait
    if ((now_us - last) < static_cast<uint64_t>(debounce_ms_) * 1000ULL)
        return;                                     // still inside debounce

    // Snapshot and clear the dirty timestamp BEFORE we start the I/O.
    // If new writes arrive while we are flushing, they will set the
    // timestamp again and be picked up on the next poll().
    last_dirty_us_.store(0, std::memory_order_relaxed);

    flush_dirty_sectors();
    file_.sync();
}

std::size_t PfiWriteback::flush_dirty_sectors() {
    if (!flash_ || !file_.is_open()) return 0;

    const uint8_t*    mem = flash_->mem_ptr();
    const std::size_t sz  = flash_->sector_size();
    if (sz == 0) return 0; // device without dirty tracking

    std::size_t count = 0;

    flash_->for_each_dirty_sector([&](uint32_t sector_idx) {
        const uint64_t in_flash_offset = static_cast<uint64_t>(sector_idx) * sz;
        const uint64_t file_offset =
            static_cast<uint64_t>(flash_offset_in_pfi_) + in_flash_offset;
        std::span<const uint8_t> data{mem + in_flash_offset, sz};
        if (file_.write_at(file_offset, data)) {
            bytes_written_ += sz;
            ++count;
        }
    });

    if (count > 0) {
        ++flush_count_;
        flash_->clear_dirty();
    }
    return count;
}

void PfiWriteback::shutdown_flush() {
    if (mode_ != Mode::WriteBack || !flash_ || !file_.is_open())
        return;
    if (!flash_->any_dirty()) return;
    const std::size_t n = flush_dirty_sectors();
    file_.sync();
    std::fprintf(stderr,
        "PfiWriteback: shutdown flush — %zu sectors (%llu bytes total)\n",
        n, static_cast<unsigned long long>(bytes_written_));
}
