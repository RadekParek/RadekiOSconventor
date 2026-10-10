#include "compat_runtime/darwin_compat_shims.hpp"

#include "compat_runtime/openal_backend.hpp"
#include "compat_runtime/virtual_file_system.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace radek::compat_runtime::darwin_compat {
namespace {

constexpr GuestAddress kCalloutBegin = 0xf0050000u;
constexpr GuestAddress kCalloutEnd = 0xf0080000u;
constexpr std::size_t kMaximumDiagnostics = 64;
constexpr std::size_t kDataPageBytes = 64U * 1024U;

// Darwin `_CTYPE_*` classification bits, as used by __maskrune/__istype.
constexpr std::uint32_t kCtypeAlpha = 0x00000100;
constexpr std::uint32_t kCtypeControl = 0x00000200;
constexpr std::uint32_t kCtypeDigit = 0x00000400;
constexpr std::uint32_t kCtypeGraph = 0x00000800;
constexpr std::uint32_t kCtypeLower = 0x00001000;
constexpr std::uint32_t kCtypePunct = 0x00002000;
constexpr std::uint32_t kCtypeSpace = 0x00004000;
constexpr std::uint32_t kCtypeUpper = 0x00008000;
constexpr std::uint32_t kCtypeXdigit = 0x00010000;
constexpr std::uint32_t kCtypeBlank = 0x00020000;

// OpenAL enum values the guest uses, so the mixer can answer state queries and
// decode buffer formats without guessing.
constexpr std::uint32_t kAlSourceState = 0x1010;
constexpr std::uint32_t kAlPlaying = 0x1012;
constexpr std::uint32_t kAlStopped = 0x1014;
constexpr std::uint32_t kAlBuffersProcessed = 0x1016;
constexpr std::uint32_t kAlLooping = 0x1007;
constexpr std::uint32_t kAlGain = 0x100A;
constexpr std::uint32_t kAlBuffer = 0x1009;
constexpr std::uint32_t kAlFormatMono8 = 0x1100;
constexpr std::uint32_t kAlFormatMono16 = 0x1101;
constexpr std::uint32_t kAlFormatStereo8 = 0x1102;
constexpr std::uint32_t kAlFormatStereo16 = 0x1103;

std::uint32_t classificationOf(int character) {
    if (character < 0 || character > 127)
        return 0;
    const auto value = static_cast<unsigned>(character);
    std::uint32_t mask = 0;
    if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')) mask |= kCtypeAlpha;
    if (value < 0x20 || value == 0x7f) mask |= kCtypeControl;
    if (value >= '0' && value <= '9') mask |= kCtypeDigit;
    if (value > 0x20 && value < 0x7f) mask |= kCtypeGraph;
    if (value >= 'a' && value <= 'z') mask |= kCtypeLower;
    if (value >= 'A' && value <= 'Z') mask |= kCtypeUpper;
    if (value == ' ' || value == '\t') mask |= kCtypeBlank;
    if (value == ' ') mask |= kCtypeSpace;
    if (value >= 0x09 && value <= 0x0d) mask |= kCtypeSpace;
    if ((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F'))
        mask |= kCtypeXdigit;
    if (value > 0x20 && value < 0x7f && !(mask & (kCtypeAlpha | kCtypeDigit)))
        mask |= kCtypePunct;
    return mask;
}

bool writeWord(GuestAddressSpace &memory, GuestAddress address, std::uint32_t value) {
    const std::array<std::uint8_t, 4> bytes{static_cast<std::uint8_t>(value & 0xff),
                                            static_cast<std::uint8_t>((value >> 8) & 0xff),
                                            static_cast<std::uint8_t>((value >> 16) & 0xff),
                                            static_cast<std::uint8_t>((value >> 24) & 0xff)};
    return memory.write(address, bytes.data(), bytes.size());
}

} // namespace

ShimAdapter::ShimAdapter(objc::ShimAdapter *strings) : strings_(strings) {}

void ShimAdapter::note(const std::string &detail) {
    if (diagnostics_.size() < kMaximumDiagnostics)
        diagnostics_.push_back(detail);
}

void ShimAdapter::registerFunction(ShimRegistry &registry, const std::string &symbol,
                                   const std::string &adapterName, Invoke invoke) {
    if (nextCallout_ < kCalloutBegin || nextCallout_ > kCalloutEnd - 4U)
        throw std::overflow_error("Darwin compatibility callout range is exhausted");
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libSystem.B.dylib";
    binding.adapterName = adapterName;
    binding.guestAddress = nextCallout_;
    binding.invoke = std::move(invoke);
    nextCallout_ += 4U;
    registry.registerBinding(std::move(binding));
    ++boundSymbols_;
}

void ShimAdapter::registerExceptionFunction(ShimRegistry &registry, const std::string &symbol,
                                            const std::string &adapterName, Invoke invoke) {
    if (nextCallout_ < kCalloutBegin || nextCallout_ > kCalloutEnd - 4U)
        throw std::overflow_error("Darwin compatibility callout range is exhausted");
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libgcc_s.1.dylib";
    binding.adapterName = adapterName;
    binding.guestAddress = nextCallout_;
    binding.invokeException = std::move(invoke);
    nextCallout_ += 4U;
    registry.registerBinding(std::move(binding));
    ++boundSymbols_;
}

void ShimAdapter::registerData(
    ShimRegistry &registry, const std::string &symbol, const std::string &adapterName,
    std::function<bool(GuestAddressSpace &, GuestAddress &, std::string &)> resolve) {
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libSystem.B.dylib";
    binding.adapterName = adapterName;
    binding.resolveGuestAddress = std::move(resolve);
    registry.registerBinding(std::move(binding));
    ++boundSymbols_;
}

GuestAddress ShimAdapter::dataPage(GuestAddressSpace &memory, const std::string &label) {
    const auto found = dataPages_.find(&memory);
    if (found != dataPages_.end())
        return found->second;
    const auto address = memory.mapAny(kDataPageBytes,
        MemoryPermission::Read | MemoryPermission::Write, label);
    dataPages_.emplace(&memory, address);
    return address;
}

GuestAddress ShimAdapter::writableCell(GuestAddressSpace &memory, const std::string &label,
                                       std::uint32_t value) {
    const auto page = dataPage(memory, label);
    if (page == 0)
        return 0;
    if (!writeWord(memory, page, value))
        return 0;
    return page;
}

GuestAddress ShimAdapter::constantString(GuestAddressSpace &memory, const std::string &symbol,
                                         const std::string &value, std::string &reason) {
    auto &cache = constantStrings_[&memory];
    const auto found = cache.find(symbol);
    if (found != cache.end())
        return found->second;
    if (strings_ == nullptr) {
        reason = symbol + " needs the bounded Foundation string subset, which is not attached";
        return 0;
    }
    const auto address = strings_->createConstantString(memory, value, reason);
    if (address != 0)
        cache.emplace(symbol, address);
    return address;
}

GuestAddress ShimAdapter::errorCell(GuestAddressSpace &memory, std::string &reason) {
    const auto found = errnoCells_.find(&memory);
    if (found != errnoCells_.end())
        return found->second;
    const auto address = writableCell(memory, "darwin-errno-cell", 0);
    if (address == 0) {
        reason = "__error could not materialize a guest errno cell";
        return 0;
    }
    note("__error returns a single guest errno cell; per-thread errno is not reproduced");
    errnoCells_.emplace(&memory, address);
    return address;
}

void ShimAdapter::registerBindings(ShimRegistry &registry) {
    // --- Darwin ctype sweep functions (ASCII/C-locale) ------------------------
    registerFunction(registry, "___tolower", "darwin-ctype-tolower-ASCII",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++ctypeCalls_;
            const auto character = static_cast<int>(registers.r[0] & 0xffU);
            registers.r[0] = static_cast<std::uint32_t>(
                character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
            return true;
        });
    registerFunction(registry, "___toupper", "darwin-ctype-toupper-ASCII",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++ctypeCalls_;
            const auto character = static_cast<int>(registers.r[0] & 0xffU);
            registers.r[0] = static_cast<std::uint32_t>(
                character >= 'a' && character <= 'z' ? character - 'a' + 'A' : character);
            return true;
        });
    registerFunction(registry, "___maskrune", "darwin-ctype-maskrune-ASCII",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++ctypeCalls_;
            const auto character = static_cast<int>(registers.r[0] & 0xffU);
            registers.r[0] = classificationOf(character) & registers.r[1];
            return true;
        });

    // --- per-thread errno accessor -------------------------------------------
    registerFunction(registry, "___error", "darwin-error-accessor",
        [this](CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason) {
            const auto cell = errorCell(memory, reason);
            if (cell == 0)
                return false;
            registers.r[0] = cell;
            return true;
        });

    // --- standard streams ----------------------------------------------------
    const auto streamBinding = [this](const std::string &symbol,
                                      VirtualFileSystem::StandardStream stream,
                                      const std::string &label) {
        return [this, symbol, stream, label](GuestAddressSpace &memory, GuestAddress &address,
                                             std::string &reason) {
            const auto handle = guestFileSystem().standardStream(stream);
            if (handle == 0) {
                reason = symbol + " has no usable process stream";
                return false;
            }
            const auto cell = writableCell(memory, label, handle);
            if (cell == 0) {
                reason = symbol + " could not materialize its guest cell";
                return false;
            }
            ++streamCells_;
            note(symbol + " is materialized as a real process-stream handle");
            address = cell;
            return true;
        };
    };
    registerData(registry, "___stdinp", "darwin-stdin-cell",
                 streamBinding("__stdinp", VirtualFileSystem::StandardStream::Input, "darwin-stdin-cell"));
    registerData(registry, "___stdoutp", "darwin-stdout-cell",
                 streamBinding("__stdoutp", VirtualFileSystem::StandardStream::Output, "darwin-stdout-cell"));
    registerData(registry, "___stderrp", "darwin-stderr-cell",
                 streamBinding("__stderrp", VirtualFileSystem::StandardStream::Error, "darwin-stderr-cell"));

    // --- EAGL drawable string keys -------------------------------------------
    const auto eaglKey = [this](const std::string &symbol, const std::string &value) {
        return [this, symbol, value](GuestAddressSpace &memory, GuestAddress &address,
                                     std::string &reason) {
            const auto object = constantString(memory, symbol, value, reason);
            if (object == 0)
                return false;
            address = object;
            return true;
        };
    };
    registerData(registry, "_kEAGLColorFormatRGB565", "eagl-key-rgb565",
                 eaglKey("kEAGLColorFormatRGB565", "GL_RGB565"));
    registerData(registry, "_kEAGLColorFormatRGBA8", "eagl-key-rgba8",
                 eaglKey("kEAGLColorFormatRGBA8", "GL_RGBA8"));
    registerData(registry, "_kEAGLDrawablePropertyColorFormat", "eagl-key-color-format",
                 eaglKey("kEAGLDrawablePropertyColorFormat", "kEAGLDrawablePropertyColorFormat"));
    registerData(registry, "_kEAGLDrawablePropertyRetainedBacking", "eagl-key-retained-backing",
                 eaglKey("kEAGLDrawablePropertyRetainedBacking",
                         "kEAGLDrawablePropertyRetainedBacking"));

    // --- rune table and CoreFoundation constant-string class ------------------
    registerData(registry, "__DefaultRuneLocale", "darwin-default-runelocale-page",
        [this](GuestAddressSpace &memory, GuestAddress &address, std::string &reason) {
            const auto page = dataPage(memory, "darwin-default-runelocale");
            if (page == 0) {
                reason = "__DefaultRuneLocale could not materialize its guest page";
                return false;
            }
            note("__DefaultRuneLocale is a zeroed guest page; rune-table layout is not "
                 "reproduced and __maskrune does not read it");
            address = page;
            return true;
        });
    registerData(registry, "___CFConstantStringClassReference", "cf-constant-string-class-token",
        [this](GuestAddressSpace &memory, GuestAddress &address, std::string &reason) {
            const auto page = writableCell(memory, "cf-constant-string-class", 0);
            if (page == 0) {
                reason = "__CFConstantStringClassReference could not materialize its guest token";
                return false;
            }
            note("__CFConstantStringClassReference is a zeroed class token; CoreFoundation "
                 "string classes are not implemented");
            address = page;
            return true;
        });

    // --- OpenAL: state-only emulation, no audio output ------------------------
    registerFunction(registry, "_alGenBuffers", "openal-gen-buffers-state-only",
        [this](CpuRegisterState &registers, GuestAddressSpace &memory, std::string &) {
            ++openalCalls_;
            const auto count = registers.r[0];
            const auto out = registers.r[1];
            if (count > 0 && count < 4096 && out != 0) {
                for (std::uint32_t index = 0; index < count; ++index) {
                    if (!writeWord(memory, out + index * 4U, nextBufferId_++))
                        note("alGenBuffers could not write every generated name");
                }
            }
            return true;
        });
    registerFunction(registry, "_alDeleteBuffers", "openal-delete-buffers-mixer",
        [this](CpuRegisterState &registers, GuestAddressSpace &memory, std::string &) {
            ++openalCalls_;
            const auto count = registers.r[0];
            const auto names = registers.r[1];
            for (std::uint32_t index = 0; index < count && index < 4096; ++index) {
                std::uint8_t raw[4]{};
                if (names == 0 || !memory.read(names + index * 4U, raw, sizeof(raw)))
                    break;
                const auto name = static_cast<std::uint32_t>(raw[0]) |
                                  (static_cast<std::uint32_t>(raw[1]) << 8) |
                                  (static_cast<std::uint32_t>(raw[2]) << 16) |
                                  (static_cast<std::uint32_t>(raw[3]) << 24);
                openal::engine().deleteBuffer(name);
            }
            return true;
        });
    registerFunction(registry, "_alGenSources", "openal-gen-sources-state-only",
        [this](CpuRegisterState &registers, GuestAddressSpace &memory, std::string &) {
            ++openalCalls_;
            const auto count = registers.r[0];
            const auto out = registers.r[1];
            if (count > 0 && count < 4096 && out != 0) {
                for (std::uint32_t index = 0; index < count; ++index) {
                    if (!writeWord(memory, out + index * 4U, nextSourceId_++))
                        note("alGenSources could not write every generated name");
                }
            }
            return true;
        });
    registerFunction(registry, "_alDeleteSources", "openal-delete-sources-state-only",
        [this](CpuRegisterState &registers, GuestAddressSpace &memory, std::string &) {
            ++openalCalls_;
            const auto count = registers.r[0];
            const auto names = registers.r[1];
            for (std::uint32_t index = 0; index < count && index < 4096; ++index) {
                std::uint8_t raw[4]{};
                if (names == 0 || !memory.read(names + index * 4U, raw, sizeof(raw)))
                    break;
                const auto name = static_cast<GuestAddress>(
                    static_cast<std::uint32_t>(raw[0]) | (static_cast<std::uint32_t>(raw[1]) << 8) |
                    (static_cast<std::uint32_t>(raw[2]) << 16) |
                    (static_cast<std::uint32_t>(raw[3]) << 24));
                sourceIntState_.erase(name);
                sourceFloatBits_.erase(name);
                sourceQueues_.erase(name);
                openal::engine().deleteSource(static_cast<std::uint32_t>(name));
            }
            return true;
        });
    registerFunction(registry, "_alSourcePlay", "openal-source-play-mixer",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++openalCalls_;
            openal::engine().play(registers.r[0]);
            return true;
        });
    registerFunction(registry, "_alSourceStop", "openal-source-stop-mixer",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++openalCalls_;
            openal::engine().stop(registers.r[0]);
            return true;
        });
    registerFunction(registry, "_alSourceQueueBuffers", "openal-source-queue-mixer",
        [this](CpuRegisterState &registers, GuestAddressSpace &memory, std::string &) {
            ++openalCalls_;
            const auto source = registers.r[0];
            const auto count = registers.r[1];
            const auto names = registers.r[2];
            for (std::uint32_t index = 0; index < count && index < 4096; ++index) {
                std::uint8_t raw[4]{};
                if (names == 0 || !memory.read(names + index * 4U, raw, sizeof(raw)))
                    break;
                const auto name = static_cast<std::uint32_t>(raw[0]) |
                                  (static_cast<std::uint32_t>(raw[1]) << 8) |
                                  (static_cast<std::uint32_t>(raw[2]) << 16) |
                                  (static_cast<std::uint32_t>(raw[3]) << 24);
                sourceQueues_[source].push_back(name);
                openal::engine().queue(source, name);
            }
            return true;
        });
    registerFunction(registry, "_alSourceUnqueueBuffers", "openal-source-unqueue-mixer",
        [this](CpuRegisterState &registers, GuestAddressSpace &memory, std::string &) {
            ++openalCalls_;
            const auto source = registers.r[0];
            const auto count = registers.r[1];
            const auto out = registers.r[2];
            // The mixer is the source of truth for which buffers have actually
            // been consumed; hand those back and mirror the removal in the
            // shim's own bookkeeping.
            const auto names = openal::engine().unqueue(source, count);
            auto &queue = sourceQueues_[source];
            for (std::uint32_t index = 0; index < count && index < 4096; ++index) {
                const std::uint32_t name = index < names.size() ? names[index] : 0;
                if (!queue.empty() && index < names.size())
                    queue.erase(queue.begin());
                if (out != 0 && !writeWord(memory, out + index * 4U, name))
                    break;
            }
            return true;
        });
    registerFunction(registry, "_alSourcei", "openal-source-int-state-only",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++openalCalls_;
            const auto sourceName = registers.r[0];
            const auto parameter = registers.r[1];
            const auto value = registers.r[2];
            sourceIntState_[sourceName][parameter] = value;
            if (parameter == kAlLooping) {
                openal::engine().setLooping(sourceName, value != 0);
            } else if (parameter == kAlBuffer && value != 0) {
                // alSourcei(AL_BUFFER, id) attaches a one-shot buffer directly;
                // treat it as a single-entry queue so the mixer can play it.
                openal::engine().queue(sourceName, value);
            }
            return true;
        });
    registerFunction(registry, "_alSource3i", "openal-source-3int-state-only",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++openalCalls_;
            sourceIntState_[registers.r[0]][registers.r[1]] = registers.r[2];
            return true;
        });
    registerFunction(registry, "_alSourcef", "openal-source-float-state-only",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++openalCalls_;
            const auto bits = static_cast<std::uint32_t>(registers.d[0] & 0xffffffffU);
            sourceFloatBits_[registers.r[0]][registers.r[1]] = bits;
            if (registers.r[1] == kAlGain) {
                float gain = 1.0f;
                std::memcpy(&gain, &bits, sizeof(gain));
                openal::engine().setGain(registers.r[0], gain);
            }
            return true;
        });
    registerFunction(registry, "_alSource3f", "openal-source-3float-state-only",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++openalCalls_;
            sourceFloatBits_[registers.r[0]][registers.r[1]] =
                static_cast<std::uint32_t>(registers.d[0] & 0xffffffffU);
            return true;
        });
    registerFunction(registry, "_alGetSourcei", "openal-get-source-int-state-only",
        [this](CpuRegisterState &registers, GuestAddressSpace &memory, std::string &) {
            ++openalCalls_;
            const auto sourceName = registers.r[0];
            const auto parameter = registers.r[1];
            std::uint32_t value = 0;
            bool answered = false;
            if (parameter == kAlBuffersProcessed) {
                // Streaming games poll this to know which buffers to unqueue
                // and refill; answer from the mixer's real playback position.
                value = openal::engine().processedCount(sourceName);
                answered = true;
            } else if (parameter == kAlSourceState) {
                value = openal::engine().isPlaying(sourceName) ? kAlPlaying : kAlStopped;
                answered = true;
            }
            if (!answered) {
                const auto found = sourceIntState_.find(sourceName);
                if (found != sourceIntState_.end()) {
                    const auto entry = found->second.find(parameter);
                    if (entry != found->second.end())
                        value = entry->second;
                }
            }
            if (registers.r[2] != 0 && !writeWord(memory, registers.r[2], value))
                note("alGetSourcei could not write its guest out-parameter");
            return true;
        });
    registerFunction(registry, "_alGetSourcef", "openal-get-source-float-state-only",
        [this](CpuRegisterState &registers, GuestAddressSpace &memory, std::string &) {
            ++openalCalls_;
            const auto found = sourceFloatBits_.find(registers.r[0]);
            std::uint32_t bits = 0;
            if (found != sourceFloatBits_.end()) {
                const auto entry = found->second.find(registers.r[1]);
                if (entry != found->second.end())
                    bits = entry->second;
            }
            if (registers.r[2] != 0 && !writeWord(memory, registers.r[2], bits))
                note("alGetSourcef could not write its guest out-parameter");
            return true;
        });
    registerFunction(registry, "_alBufferData", "openal-buffer-data-capture",
        [this](CpuRegisterState &registers, GuestAddressSpace &memory, std::string &) {
            ++openalCalls_;
            // alBufferData(buffer, format, data, size, freq): AAPCS puts buffer,
            // format, data and size in r0..r3; freq arrives on the stack.
            const auto bufferName = registers.r[0];
            const auto format = registers.r[1];
            const auto data = registers.r[2];
            const auto size = registers.r[3];
            std::uint32_t stackArguments[2] = {0, 0};
            std::uint32_t frequency = 44100;
            if (registers.r[13] != 0 &&
                memory.read(registers.r[13], stackArguments, sizeof(std::uint32_t)))
                frequency = stackArguments[0];
            openal::PcmBuffer buffer;
            buffer.name = bufferName;
            switch (format) {
                case kAlFormatMono8: buffer.channels = 1; buffer.bitsPerSample = 8; break;
                case kAlFormatMono16: buffer.channels = 1; buffer.bitsPerSample = 16; break;
                case kAlFormatStereo8: buffer.channels = 2; buffer.bitsPerSample = 8; break;
                case kAlFormatStereo16: buffer.channels = 2; buffer.bitsPerSample = 16; break;
                default: buffer.channels = 1; buffer.bitsPerSample = 16; break;
            }
            buffer.sampleRate = frequency == 0 ? 44100 : frequency;
            if (size > 0 && size <= 64U * 1024U * 1024U && data != 0) {
                buffer.bytes.resize(size);
                if (!memory.read(data, buffer.bytes.data(), size)) {
                    note("alBufferData could not read the guest PCM payload");
                    buffer.bytes.clear();
                }
            }
            openal::engine().storeBuffer(buffer);
            return true;
        });
    registerFunction(registry, "_alcOpenDevice", "openal-open-device-mixer",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++openalCalls_;
            // Open the real output stream where one exists (AAudio on Android).
            // When no backend is available the mixer still tracks state, so the
            // game stays silent instead of crashing.
            const bool opened = openal::engine().openDevice();
            registers.r[0] = nextDeviceToken_++;
            note(opened ? "alcOpenDevice opened the host audio output stream"
                        : "alcOpenDevice has no host audio backend on this build; the "
                          "mixer runs without device output");
            return true;
        });
    registerFunction(registry, "_alcCloseDevice", "openal-close-device-state-only",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++openalCalls_;
            registers.r[0] = 1; // ALC_TRUE
            return true;
        });
    registerFunction(registry, "_alcCreateContext", "openal-create-context-state-only",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++openalCalls_;
            registers.r[0] = nextContextToken_++;
            return true;
        });
    registerFunction(registry, "_alcDestroyContext", "openal-destroy-context-state-only",
        [this](CpuRegisterState &, GuestAddressSpace &, std::string &) {
            ++openalCalls_;
            return true;
        });
    registerFunction(registry, "_alcMakeContextCurrent", "openal-make-context-current-state-only",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
            ++openalCalls_;
            // Making a context current is the point the game starts issuing
            // plays from; make sure the output stream is open (idempotent).
            openal::engine().openDevice();
            registers.r[0] = 1; // ALC_TRUE
            return true;
        });

    // --- GCC SJLJ personality: explicit, fail-closed boundary -----------------
    registerExceptionFunction(registry, "___gxx_personality_sj0",
        "gxx-personality-sj0-unsupported-boundary",
        [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &reason) {
            ++personalityBoundaries_;
            reason = "__gxx_personality_sj0 reached with guest exception object " +
                     std::to_string(registers.r[0]) +
                     "; guest SJLJ personality dispatch, catch matching and landing-pad "
                     "transfer are unsupported.";
            return true;
        });
}

} // namespace radek::compat_runtime::darwin_compat
