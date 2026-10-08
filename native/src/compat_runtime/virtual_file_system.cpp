#include "compat_runtime/virtual_file_system.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace radek::compat_runtime {
namespace {

constexpr std::size_t kMaximumDiagnostics = 64;
constexpr std::size_t kMaximumOpenPaths = 64;

bool isWriteMode(const std::string &mode) {
    return mode.find('w') != std::string::npos || mode.find('a') != std::string::npos ||
           mode.find('+') != std::string::npos;
}

bool isSyntheticUserStatePath(const std::string &path) {
    for (const auto *name : {"/highscores.lua", "/settings.lua"}) {
        if (path == name || path == name + 1 || path == std::string("./") + (name + 1))
            return true;
        if (path.size() >= std::strlen(name) &&
            path.compare(path.size() - std::strlen(name), std::strlen(name), name) == 0)
            return true;
    }
    return false;
}

/** True when `path` contains no empty, "." or ".." component. */
bool isContainedRelativePath(const std::string &path) {
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto length = (end == std::string::npos ? path.size() : end) - start;
        const std::string component = path.substr(start, length);
        if (component == "." || component == "..")
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

std::string trimLeadingSlashes(const std::string &path) {
    std::size_t start = 0;
    while (start < path.size() && path[start] == '/')
        ++start;
    return path.substr(start);
}

std::string trimTrailingSlashes(std::string path) {
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

} // namespace

const char *bundleGuestPath() { return "/radek-bundle/App.app"; }

void VirtualFileSystem::mount(std::string guestPrefix, std::string hostDirectory, bool writable) {
    guestPrefix = trimTrailingSlashes(std::move(guestPrefix));
    if (guestPrefix.empty() || guestPrefix.front() != '/')
        throw std::invalid_argument("guest mount prefixes must be absolute");
    if (hostDirectory.empty())
        throw std::invalid_argument("guest mounts require a host directory");
    for (auto &existing : mounts_) {
        if (existing.guestPrefix == guestPrefix) {
            existing.hostDirectory = std::move(hostDirectory);
            existing.writable = writable;
            return;
        }
    }
    mounts_.push_back(Mount{std::move(guestPrefix), std::move(hostDirectory), writable});
    // Longest prefix first so a bundle mount wins over the writable home mount.
    std::sort(mounts_.begin(), mounts_.end(), [](const Mount &left, const Mount &right) {
        return left.guestPrefix.size() > right.guestPrefix.size();
    });
}

bool VirtualFileSystem::resolve(const std::string &guestPath, std::string &hostPath,
                                bool &writable) const {
    for (const auto &mount : mounts_) {
        if (guestPath.size() < mount.guestPrefix.size())
            continue;
        if (guestPath.compare(0, mount.guestPrefix.size(), mount.guestPrefix) != 0)
            continue;
        const auto remainder = guestPath.substr(mount.guestPrefix.size());
        if (!remainder.empty() && remainder.front() != '/')
            continue;
        const auto relative = trimLeadingSlashes(remainder);
        if (!isContainedRelativePath(relative))
            return false;
        hostPath = mount.hostDirectory;
        if (!relative.empty()) {
            if (hostPath.back() != '/')
                hostPath.push_back('/');
            hostPath += relative;
        }
        writable = mount.writable;
        return true;
    }
    return false;
}

void VirtualFileSystem::note(const std::string &detail) {
    if (diagnostics_.size() < kMaximumDiagnostics)
        diagnostics_.push_back(detail);
}

GuestAddress VirtualFileSystem::open(const std::string &guestPath, const std::string &mode,
                                     std::string &detail) {
    std::string hostPath;
    bool writable = false;
    if (mounts_.empty()) {
        ++refused_;
        detail = "guest file I/O refused: no filesystem mount is configured for '" + guestPath + "'";
        note(detail);
        return 0;
    }
    if (!resolve(guestPath, hostPath, writable)) {
        ++refused_;
        detail = "guest file I/O refused: '" + guestPath + "' is outside every mounted directory";
        note(detail);
        return 0;
    }
    const bool wantsWrite = isWriteMode(mode);
    if (wantsWrite && !writable) {
        ++refused_;
        detail = "guest file I/O refused: '" + guestPath + "' is mounted read-only";
        note(detail);
        return 0;
    }
    const std::string nativeMode = mode.empty() ? "rb" : mode;
    std::FILE *stream = std::fopen(hostPath.c_str(), nativeMode.c_str());
    if (stream == nullptr && !wantsWrite &&
        (guestPath == "data/bundleIndex.idx" ||
         guestPath == "./data/bundleIndex.idx" ||
         (guestPath.size() > std::strlen("/data/bundleIndex.idx") &&
          guestPath.compare(guestPath.size() - std::strlen("/data/bundleIndex.idx"),
                            std::strlen("/data/bundleIndex.idx"),
                            "/data/bundleIndex.idx") == 0))) {
        // Older iOS game builds use a generated CSV-like bundle index, while
        // the authorized Angry Birds fixture carries the loose data files but
        // no generated index. Match the existing translated runtime's explicit
        // compatibility behavior: an empty index with one reserved marker
        // makes the guest bundle parser expose no packed entries and continue
        // to its direct-file fallback. This is not an invented asset; every
        // actual data read still resolves against the mounted bundle directory.
        stream = std::tmpfile();
        if (stream != nullptr) {
            static constexpr char emptyIndex[] = "__loose_files__,0,0,0\n";
            const auto written = std::fwrite(emptyIndex, 1, sizeof(emptyIndex) - 1, stream);
            if (written != sizeof(emptyIndex) - 1) {
                std::fclose(stream);
                stream = nullptr;
            } else {
                std::rewind(stream);
            }
        }
        if (stream != nullptr)
            note("guest fopen: using synthetic loose-file bundle index for '" + guestPath + "'");
    }
    if (stream == nullptr && !wantsWrite && isSyntheticUserStatePath(guestPath)) {
        // A first launch has no persisted highscores/settings file. Use the
        // same empty seekable state as the translated runtime so the guest's
        // normal defaults path can proceed without manufacturing saved data.
        stream = std::tmpfile();
        if (stream != nullptr)
            note("guest fopen: using empty synthetic user state for '" + guestPath + "'");
    }
    if (stream == nullptr) {
        ++refused_;
        detail = "guest file I/O refused: '" + guestPath + "' could not be opened (" +
                 std::strerror(errno) + ")";
        note(detail);
        return 0;
    }
    const auto handle = nextHandle_++;
    files_.emplace(handle, File{stream, guestPath, writable});
    ++opens_;
    if (openPaths_.size() < kMaximumOpenPaths)
        openPaths_.push_back(guestPath);
    return handle;
}

std::size_t VirtualFileSystem::read(GuestAddress handle, void *destination, std::size_t size,
                                    std::string &detail) {
    const auto found = files_.find(handle);
    if (found == files_.end()) {
        detail = "guest file read refused: unknown file handle";
        return 0;
    }
    const auto readBytes = std::fread(destination, 1, size, found->second.stream);
    ++reads_;
    bytesRead_ += readBytes;
    detail = std::string();
    return readBytes;
}

std::size_t VirtualFileSystem::write(GuestAddress handle, const void *source, std::size_t size,
                                     std::string &detail) {
    const auto found = files_.find(handle);
    if (found == files_.end() || !found->second.writable) {
        detail = "guest file write refused: unknown or read-only file handle";
        return 0;
    }
    const auto written = std::fwrite(source, 1, size, found->second.stream);
    ++writes_;
    bytesWritten_ += written;
    detail = std::string();
    return written;
}

bool VirtualFileSystem::seek(GuestAddress handle, long offset, int whence, std::string &detail) {
    const auto found = files_.find(handle);
    if (found == files_.end()) {
        detail = "guest file seek refused: unknown file handle";
        return false;
    }
    if (std::fseek(found->second.stream, offset, whence) != 0) {
        detail = "guest file seek failed";
        return false;
    }
    return true;
}

long VirtualFileSystem::tell(GuestAddress handle, std::string &detail) {
    const auto found = files_.find(handle);
    if (found == files_.end()) {
        detail = "guest file tell refused: unknown file handle";
        return -1;
    }
    return static_cast<long>(std::ftell(found->second.stream));
}

long VirtualFileSystem::length(GuestAddress handle, std::string &detail) {
    const auto found = files_.find(handle);
    if (found == files_.end()) {
        detail = "guest file length refused: unknown file handle";
        return -1;
    }
    const auto original = std::ftell(found->second.stream);
    if (original < 0 || std::fseek(found->second.stream, 0, SEEK_END) != 0) {
        detail = "guest file length could not seek to the end of the file";
        return -1;
    }
    const auto size = std::ftell(found->second.stream);
    const bool restored = std::fseek(found->second.stream, original, SEEK_SET) == 0;
    if (size < 0 || !restored) {
        detail = "guest file length could not restore the current position";
        return -1;
    }
    return static_cast<long>(size);
}

bool VirtualFileSystem::eof(GuestAddress handle) const {
    const auto found = files_.find(handle);
    return found == files_.end() || std::feof(found->second.stream) != 0;
}

bool VirtualFileSystem::failed(GuestAddress handle) const {
    const auto found = files_.find(handle);
    return found == files_.end() || std::ferror(found->second.stream) != 0;
}

bool VirtualFileSystem::flush(GuestAddress handle, std::string &detail) {
    const auto found = files_.find(handle);
    if (found == files_.end()) {
        detail = "guest file flush refused: unknown file handle";
        return false;
    }
    return std::fflush(found->second.stream) == 0;
}

bool VirtualFileSystem::close(GuestAddress handle) {
    const auto found = files_.find(handle);
    if (found == files_.end())
        return false;
    if (found->second.standard) {
        // The process owns its standard streams; a guest fclose is a no-op that
        // must not close stdout/stderr for the whole app.
        note("guest fclose on " + found->second.guestPath + " is ignored: the stream is owned by the process");
        return true;
    }
    std::fclose(found->second.stream);
    files_.erase(found);
    return true;
}

GuestAddress VirtualFileSystem::standardStream(StandardStream stream) {
    const char *name = stream == StandardStream::Input  ? "<stdin>"
                       : stream == StandardStream::Output ? "<stdout>"
                                                          : "<stderr>";
    for (const auto &[handle, file] : files_) {
        if (file.standard && file.guestPath == name)
            return handle;
    }
    std::FILE *native = stream == StandardStream::Input  ? stdin
                        : stream == StandardStream::Output ? stdout
                                                           : stderr;
    if (native == nullptr) {
        ++refused_;
        note(std::string("standard stream ") + name + " is unavailable in this process");
        return 0;
    }
    const auto handle = nextHandle_++;
    File file;
    file.stream = native;
    file.guestPath = name;
    file.writable = stream != StandardStream::Input;
    file.standard = true;
    files_.emplace(handle, std::move(file));
    openPaths_.push_back(name);
    return handle;
}

bool VirtualFileSystem::isOpen(GuestAddress handle) const {
    return files_.find(handle) != files_.end();
}

std::vector<std::string> VirtualFileSystem::openPaths() const { return openPaths_; }

VirtualFileSystem &guestFileSystem() {
    static VirtualFileSystem filesystem;
    return filesystem;
}

} // namespace radek::compat_runtime
