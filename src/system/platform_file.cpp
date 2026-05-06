#include "platform_file.hpp"

#include <cstdio>
#include <utility>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
#  include <cstring>
#endif

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------

PlatformFile::PlatformFile(PlatformFile&& o) noexcept {
#ifdef _WIN32
    handle_ = o.handle_;
    o.handle_ = nullptr;
#else
    fd_ = o.fd_;
    o.fd_ = -1;
#endif
}

PlatformFile& PlatformFile::operator=(PlatformFile&& o) noexcept {
    if (this != &o) {
        close();
#ifdef _WIN32
        handle_ = o.handle_;
        o.handle_ = nullptr;
#else
        fd_ = o.fd_;
        o.fd_ = -1;
#endif
    }
    return *this;
}

// ---------------------------------------------------------------------------
// open / close / is_open
// ---------------------------------------------------------------------------

#ifdef _WIN32

bool PlatformFile::open_rw(const std::string& path) {
    close();
    HANDLE h = ::CreateFileA(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,             // allow concurrent readers (USB-style tools)
        nullptr,
        OPEN_EXISTING,               // do not create or truncate
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr,
            "PlatformFile: CreateFileA(%s) failed: %lu\n",
            path.c_str(), ::GetLastError());
        return false;
    }
    handle_ = h;
    return true;
}

void PlatformFile::close() {
    if (handle_) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

bool PlatformFile::is_open() const {
    return handle_ != nullptr;
}

bool PlatformFile::write_at(uint64_t offset, std::span<const uint8_t> data) {
    if (!handle_) return false;
    OVERLAPPED ov{};
    ov.Offset     = static_cast<DWORD>(offset & 0xFFFF'FFFFu);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD written = 0;
    BOOL  ok = ::WriteFile(static_cast<HANDLE>(handle_),
                           data.data(),
                           static_cast<DWORD>(data.size()),
                           &written,
                           &ov);
    if (!ok || written != data.size()) {
        std::fprintf(stderr,
            "PlatformFile: WriteFile failed at offset %llu: %lu (wrote %lu / %zu)\n",
            static_cast<unsigned long long>(offset),
            ::GetLastError(),
            static_cast<unsigned long>(written),
            data.size());
        return false;
    }
    return true;
}

bool PlatformFile::sync() {
    if (!handle_) return false;
    if (!::FlushFileBuffers(static_cast<HANDLE>(handle_))) {
        std::fprintf(stderr,
            "PlatformFile: FlushFileBuffers failed: %lu\n", ::GetLastError());
        return false;
    }
    return true;
}

#else // POSIX -----------------------------------------------------------------

bool PlatformFile::open_rw(const std::string& path) {
    close();
    int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        std::fprintf(stderr,
            "PlatformFile: open(%s, O_RDWR) failed: %s\n",
            path.c_str(), std::strerror(errno));
        return false;
    }
    fd_ = fd;
    return true;
}

void PlatformFile::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool PlatformFile::is_open() const {
    return fd_ >= 0;
}

bool PlatformFile::write_at(uint64_t offset, std::span<const uint8_t> data) {
    if (fd_ < 0) return false;
    const uint8_t* p   = data.data();
    std::size_t    rem = data.size();
    off_t          off = static_cast<off_t>(offset);
    while (rem > 0) {
        ssize_t n = ::pwrite(fd_, p, rem, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr,
                "PlatformFile: pwrite at %llu failed: %s\n",
                static_cast<unsigned long long>(offset),
                std::strerror(errno));
            return false;
        }
        if (n == 0) {
            std::fprintf(stderr,
                "PlatformFile: pwrite returned 0 at %llu\n",
                static_cast<unsigned long long>(offset));
            return false;
        }
        p   += n;
        off += n;
        rem -= static_cast<std::size_t>(n);
    }
    return true;
}

bool PlatformFile::sync() {
    if (fd_ < 0) return false;
    if (::fsync(fd_) != 0) {
        std::fprintf(stderr,
            "PlatformFile: fsync failed: %s\n", std::strerror(errno));
        return false;
    }
    return true;
}

#endif
