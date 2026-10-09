#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace radek::compat_runtime::openal {

/**
 * A captured OpenAL PCM buffer. The guest's sample bytes are copied into host
 * memory at alBufferData time (on the emulation thread) so the audio render
 * thread never reads guest address space.
 */
struct PcmBuffer {
    std::uint32_t name = 0;
    std::uint32_t channels = 1;      // 1 = mono, 2 = stereo
    std::uint32_t bitsPerSample = 16; // 8 or 16
    std::uint32_t sampleRate = 44100;
    std::vector<std::uint8_t> bytes; // raw guest copy, pre-conversion
};

/** Normalized playback view of a buffer: interleaved float32 stereo at the
 *  engine's output rate. Produced lazily once per (buffer, output-rate). */
struct NormalizedBuffer {
    std::vector<float> frames; // interleaved stereo: L,R,L,R,...
    std::uint64_t frameCount = 0;
};

/** One OpenAL source's playback state. */
struct SourceState {
    std::uint32_t name = 0;
    bool playing = false;
    bool looping = false;
    float gain = 1.0f;
    std::vector<std::uint32_t> queue; // buffer names still to play, in order
    std::size_t cursor = 0;           // index into queue of the current buffer
    std::uint64_t framePosition = 0;  // frame offset within the current buffer
    std::uint32_t processed = 0;      // buffers fully consumed since last unqueue
};

/**
 * A tiny software OpenAL mixer with a device sink.
 *
 * The mixer itself is platform-free and unit-testable: `render()` advances all
 * playing sources and writes interleaved float32 stereo at `outputRate()`. On
 * Android the sink is an AAudio output stream whose data callback calls
 * `render()`; everywhere else the sink is absent and the mixer can still be
 * driven directly from tests. The engine never claims gameplay - it only
 * produces samples from whatever buffers the guest queued.
 */
class Engine {
  public:
    static constexpr std::uint32_t kOutputRate = 44100;
    static constexpr std::uint32_t kOutputChannels = 2;

    Engine();
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    std::uint32_t outputRate() const noexcept { return kOutputRate; }

    // --- buffer / source management (emulation thread) --------------------
    void storeBuffer(const PcmBuffer &buffer);
    void deleteBuffer(std::uint32_t name);
    void deleteSource(std::uint32_t name);
    void queue(std::uint32_t sourceName, std::uint32_t bufferName);
    void play(std::uint32_t sourceName);
    void stop(std::uint32_t sourceName);
    /** Buffers fully consumed and ready to be unqueued. */
    std::uint32_t processedCount(std::uint32_t sourceName);
    /** Pops up to `count` processed buffer names off the source. */
    std::vector<std::uint32_t> unqueue(std::uint32_t sourceName, std::uint32_t count);
    void setLooping(std::uint32_t sourceName, bool looping);
    void setGain(std::uint32_t sourceName, float gain);
    /** Thread-safe snapshot of whether the source is currently playing. */
    bool isPlaying(std::uint32_t sourceName);

    // --- mixing (audio thread, or tests) ----------------------------------
    /**
     * Renders `frameCount` interleaved stereo float frames into `out`. Returns
     * the number of sources that contributed audio.
     */
    std::size_t render(float *out, std::uint64_t frameCount);

    // --- device sink (Android: AAudio) ------------------------------------
    /** Opens the output stream. Safe to call repeatedly; returns false when no
     *  audio backend is available (the mixer then only serves tests). */
    bool openDevice();
    void closeDevice();
    bool deviceOpen() const noexcept;

  private:
    const NormalizedBuffer &normalized(const PcmBuffer &buffer);
    void renderSource(SourceState &source, float *out, std::uint64_t frameCount);

    mutable std::mutex mutex_;
    std::map<std::uint32_t, PcmBuffer> buffers_;
    std::map<std::uint32_t, SourceState> sources_;
    // Normalized caches are keyed by buffer name; rebuilt if the buffer bytes
    // change (alBufferData on an existing name).
    struct CacheEntry {
        std::uint64_t generation = 0;
        NormalizedBuffer normalized;
    };
    std::map<std::uint32_t, CacheEntry> normalizedCache_;
    std::map<std::uint32_t, std::uint64_t> bufferGeneration_;
    bool deviceOpen_ = false;
    void *stream_ = nullptr; // AAudioStream* on Android, opaque elsewhere
};

/** Process-wide engine shared by the OpenAL shims. */
Engine &engine();

} // namespace radek::compat_runtime::openal
