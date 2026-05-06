#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// ============================================================================
// PlatformFile — minimal cross-platform random-access file with disk sync.
//
// Wraps POSIX (pread/pwrite/fsync via int fd) on Linux/macOS and Win32
// (WriteFile + OVERLAPPED.Offset + FlushFileBuffers via HANDLE) on Windows.
//
// Used by PfiWriteback to push individual 4 KB flash sectors back to a
// host PFI file at fixed byte offsets.  Single-threaded usage is assumed
// (the writeback path is driven from the main thread of piece-emu-system,
// or from the headless main loop of piece-emu).
//
// All methods return `true` on success, `false` on failure.  Errors are
// reported via std::fprintf(stderr, ...) at the call site; we deliberately
// do not raise exceptions because writeback is a best-effort operation
// that should not abort emulator shutdown.
// ============================================================================

class PlatformFile {
public:
    PlatformFile() = default;
    ~PlatformFile() { close(); }

    PlatformFile(const PlatformFile&)            = delete;
    PlatformFile& operator=(const PlatformFile&) = delete;
    PlatformFile(PlatformFile&& o) noexcept;
    PlatformFile& operator=(PlatformFile&& o) noexcept;

    // Open `path` for read-write, existing file (no creation, no truncate).
    bool open_rw(const std::string& path);

    // Atomic positional write — `data.size()` bytes at byte offset `offset`
    // in the file.  Does NOT advance any internal cursor on either platform
    // (no concurrent-access guarantees from us; we just want pread/pwrite
    // semantics for clarity).
    bool write_at(uint64_t offset, std::span<const uint8_t> data);

    // Force the file's contents to disk.  POSIX: fsync(fd).  Windows:
    // FlushFileBuffers(handle).
    bool sync();

    // Close the file.  Idempotent.
    void close();

    // True iff a file is currently open.
    bool is_open() const;

private:
#ifdef _WIN32
    void* handle_ = nullptr; // HANDLE
#else
    int  fd_ = -1;
#endif
};
