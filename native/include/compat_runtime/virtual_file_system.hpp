#pragma once

#include "compat_runtime/guest_memory.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace radek::compat_runtime {

/** The guest path the runtime reports for `-[NSBundle bundlePath]`. */
const char *bundleGuestPath();

/**
 * Guest-visible filesystem backed by host directories.
 *
 * iPhone apps spend much of their startup reading their own bundle (levels,
 * textures, audio) through `fopen`/`fread` and friends. The runtime serves those
 * calls from directories it trusts instead of emulating Darwin's `FILE` layout:
 * the guest only ever sees the opaque handle returned by open() and passes it
 * back to the other stdio shims.
 *
 * Mount prefixes are matched longest-first, so a bundle mount
 * ("/radek-bundle/App.app") wins over a broader writable home mount
 * ("/radek-home"). Mounting is explicit: with no mounts every open is refused
 * with a named diagnostic instead of silently inventing file contents.
 */
class VirtualFileSystem {
  public:
    struct Mount {
        std::string guestPrefix;
        std::string hostDirectory;
        bool writable = false;
    };

    /** Registers a mount; a later mount of the same prefix replaces it. */
    void mount(std::string guestPrefix, std::string hostDirectory, bool writable);

    /** True when at least one mount exists; otherwise file I/O is refused. */
    bool mounted() const noexcept { return !mounts_.empty(); }

    /** Maps a guest path to a host path; refuses escapes outside the mount. */
    bool resolve(const std::string &guestPath, std::string &hostPath, bool &writable) const;

    /** Opens a guest path. Returns 0 (never a valid handle) on refusal. */
    GuestAddress open(const std::string &guestPath, const std::string &mode, std::string &detail);

    /**
     * One of the guest's standard streams. The Darwin libc exposes them as the
     * data symbols stdin/stdout/stderr (`__stdinp`/`__stdoutp`/`__stderrp` in an
     * armv6 Mach-O); the runtime materializes those cells with these handles so
     * the guest's own fread/fwrite/fprintf calls work against the process
     * streams. Returns 0 only if the stream cannot be represented.
     */
    enum class StandardStream { Input, Output, Error };
    GuestAddress standardStream(StandardStream stream);
    std::size_t read(GuestAddress handle, void *destination, std::size_t size, std::string &detail);
    std::size_t write(GuestAddress handle, const void *source, std::size_t size,
                      std::string &detail);
    bool seek(GuestAddress handle, long offset, int whence, std::string &detail);
    long tell(GuestAddress handle, std::string &detail);
    long length(GuestAddress handle, std::string &detail);
    bool eof(GuestAddress handle) const;
    bool failed(GuestAddress handle) const;
    bool flush(GuestAddress handle, std::string &detail);
    bool close(GuestAddress handle);
    bool isOpen(GuestAddress handle) const;

    /** Opens every guest path named by the mount table; "a" appends. */
    std::vector<Mount> mounts() const { return mounts_; }

    // Observability for the boot report. Counters are evidence about what the
    // guest tried to read, never evidence of a playable conversion.
    std::uint64_t openCount() const noexcept { return opens_; }
    std::uint64_t readCount() const noexcept { return reads_; }
    std::uint64_t bytesRead() const noexcept { return bytesRead_; }
    std::uint64_t writeCount() const noexcept { return writes_; }
    std::uint64_t bytesWritten() const noexcept { return bytesWritten_; }
    std::uint64_t refusedCount() const noexcept { return refused_; }
    std::vector<std::string> openPaths() const;
    const std::vector<std::string> &diagnostics() const noexcept { return diagnostics_; }

  private:
    struct File {
        std::FILE *stream = nullptr;
        std::string guestPath;
        bool writable = false;
        // A standard stream is owned by the process, not by the guest: close()
        // must never fclose it.
        bool standard = false;
    };

    void note(const std::string &detail);

    std::vector<Mount> mounts_;
    std::map<GuestAddress, File> files_;
    GuestAddress nextHandle_ = 0x6F000000;
    std::uint64_t opens_ = 0;
    std::uint64_t reads_ = 0;
    std::uint64_t bytesRead_ = 0;
    std::uint64_t writes_ = 0;
    std::uint64_t bytesWritten_ = 0;
    std::uint64_t refused_ = 0;
    std::vector<std::string> diagnostics_;
    std::vector<std::string> openPaths_;
};

/** Process-wide filesystem the stdio shims serve; mounted by each front end. */
VirtualFileSystem &guestFileSystem();

} // namespace radek::compat_runtime
