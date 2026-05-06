#pragma once
#include "platform_file.hpp"
#include "flash_device.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// PfiWriteback — flush dirty 4 KB flash sectors back to the host PFI file.
//
// Lifecycle (driven by piece-emu-system / piece-emu):
//
//   1. Construct: open the PFI file at `path` for read-write, record the
//      flash image's byte offset within the file (`flash_offset_in_pfi`).
//   2. attach(flash): wire the Sst39vf's on_dirty callback so each sector
//      transition from clean → dirty registers a "flush requested" timestamp.
//   3. poll(now_us) — call from the main loop ~60 Hz.  When `now_us` is at
//      least `debounce_ms` after the most recent dirty event, push every
//      dirty sector back to the file and clear the bits.
//   4. shutdown_flush() — call on quit / SIGINT / SIGTERM.  Flushes
//      unconditionally and fsync's the file before close.
//
// Modes:
//   ReadOnly   — no host file is opened; on_dirty fires but writeback is
//                inert.  Used by --read-only and by headless tests to
//                guarantee idempotency.
//   WriteBack  — full read-modify-write of the PFI flash region in 4 KB
//                pwrites; debounced as described above.
//
// Thread model:
//   The Sst39vf::on_dirty callback is invoked from the CPU thread.
//   PfiWriteback only updates atomic timestamps from that path.
//   poll() / flush() / shutdown_flush() run on the main thread and do
//   the actual disk I/O.  No locks are needed because:
//     - dirty bits live inside Sst39vf, which is not concurrently
//       observed (Bus reads/writes flash from the CPU thread only).
//     - timestamps are std::atomic.
//   shutdown_flush() must be called AFTER the CPU thread has joined.
// ============================================================================

class PfiWriteback {
public:
    enum class Mode { ReadOnly, WriteBack };

    // Construct without opening anything.  Configure via attach().
    PfiWriteback();
    ~PfiWriteback();

    PfiWriteback(const PfiWriteback&)            = delete;
    PfiWriteback& operator=(const PfiWriteback&) = delete;

    // Attach to a writable FlashDevice and (optionally) open the host PFI
    // file.
    //   mode == ReadOnly  → file is not opened; flush() is a no-op
    //   mode == WriteBack → open `pfi_path` for read-write; failure logs
    //                       a warning and downgrades to ReadOnly.
    // `flash_offset_in_pfi` is the byte offset of the flash image inside
    // the PFI file (PFIHEADER.offset).
    // `debounce_ms` is the minimum idle interval after the most recent
    // dirty event before flush() will actually write to disk.
    // The FlashDevice must support sector-granular dirty tracking; if its
    // sector_size() returns 0 (FlatFlashRom), writeback degrades to a no-op.
    bool attach(FlashDevice*       flash,
                Mode               mode,
                const std::string& pfi_path,
                uint32_t           flash_offset_in_pfi,
                int                debounce_ms = 2000);

    // Drive the debounce timer.  `now_us` is a monotonic timestamp in
    // microseconds (any consistent clock works — wall clock, steady_clock,
    // etc.; only deltas matter).
    void poll(uint64_t now_us);

    // Force an immediate flush of every dirty sector and fsync the file.
    // Safe to call even when there are no dirty sectors (no-op).
    void shutdown_flush();

    // Stats / diagnostics.
    Mode        mode()                    const { return mode_; }
    uint64_t    bytes_written()           const { return bytes_written_; }
    uint64_t    flush_count()             const { return flush_count_; }
    bool        has_pending_dirty()       const;

private:
    // Push every dirty sector to the file (no fsync).  Returns the number
    // of sectors written.
    std::size_t flush_dirty_sectors();

    FlashDevice*   flash_                = nullptr;
    Mode           mode_                 = Mode::ReadOnly;
    std::string    path_;
    uint32_t       flash_offset_in_pfi_  = 0;
    int            debounce_ms_          = 2000;

    PlatformFile   file_;

    // Timestamp (µs) of the most recent dirty-sector transition.
    // Updated by the Sst39vf dirty callback (CPU thread); read by poll()
    // (main thread).  Monotonic; 0 means "no pending dirty".
    std::atomic<uint64_t> last_dirty_us_{0};

    uint64_t bytes_written_ = 0;
    uint64_t flush_count_   = 0;
};
