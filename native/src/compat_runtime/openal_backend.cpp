#include "compat_runtime/openal_backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif

namespace radek::compat_runtime::openal {
namespace {

float sampleToFloat(const std::uint8_t *data, std::uint32_t bits, std::size_t index) {
    if (bits == 8) {
        // OpenAL 8-bit PCM is unsigned with 128 as silence.
        const float v = static_cast<float>(data[index]) - 128.0f;
        return v / 128.0f;
    }
    std::int16_t s = 0;
    std::memcpy(&s, data + index * 2, sizeof(s));
    return static_cast<float>(s) / 32768.0f;
}

} // namespace

Engine::Engine() = default;

Engine::~Engine() { closeDevice(); }

void Engine::storeBuffer(const PcmBuffer &buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffers_[buffer.name] = buffer;
    bufferGeneration_[buffer.name]++;
    normalizedCache_.erase(buffer.name);
}

void Engine::deleteBuffer(std::uint32_t name) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffers_.erase(name);
    normalizedCache_.erase(name);
    bufferGeneration_.erase(name);
}

bool Engine::isPlaying(std::uint32_t name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sources_.find(name);
    return found != sources_.end() && found->second.playing;
}

void Engine::deleteSource(std::uint32_t name) {
    std::lock_guard<std::mutex> lock(mutex_);
    sources_.erase(name);
}

void Engine::queue(std::uint32_t sourceName, std::uint32_t bufferName) {
    std::lock_guard<std::mutex> lock(mutex_);
    sources_[sourceName].queue.push_back(bufferName);
}

void Engine::play(std::uint32_t sourceName) {
    std::lock_guard<std::mutex> lock(mutex_);
    sources_[sourceName].playing = true;
}

void Engine::stop(std::uint32_t sourceName) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sources_.find(sourceName);
    if (found == sources_.end())
        return;
    found->second.playing = false;
    found->second.cursor = 0;
    found->second.framePosition = 0;
}

std::uint32_t Engine::processedCount(std::uint32_t sourceName) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sources_.find(sourceName);
    return found == sources_.end() ? 0 : found->second.processed;
}

std::vector<std::uint32_t> Engine::unqueue(std::uint32_t sourceName, std::uint32_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint32_t> result;
    auto found = sources_.find(sourceName);
    if (found == sources_.end())
        return result;
    auto &source = found->second;
    // Only buffers before the current cursor are safe to hand back; clamp to
    // what the guest asked for and to what actually completed.
    const std::uint32_t available = static_cast<std::uint32_t>(
        std::min<std::size_t>(source.cursor, source.queue.size()));
    const std::uint32_t take = std::min(count, available);
    for (std::uint32_t index = 0; index < take; ++index)
        result.push_back(source.queue[index]);
    if (take > 0) {
        source.queue.erase(source.queue.begin(), source.queue.begin() + take);
        source.cursor -= take;
        source.processed = source.processed > take ? source.processed - take : 0;
    }
    return result;
}

void Engine::setLooping(std::uint32_t sourceName, bool looping) {
    std::lock_guard<std::mutex> lock(mutex_);
    sources_[sourceName].looping = looping;
}

void Engine::setGain(std::uint32_t sourceName, float gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    sources_[sourceName].gain = gain;
}

const NormalizedBuffer &Engine::normalized(const PcmBuffer &buffer) {
    // Caller must hold mutex_.
    const auto generation = bufferGeneration_[buffer.name];
    auto &entry = normalizedCache_[buffer.name];
    if (entry.generation == generation && !entry.normalized.frames.empty())
        return entry.normalized;

    NormalizedBuffer out;
    const std::uint32_t bytesPerSample = buffer.bitsPerSample / 8;
    const std::size_t totalSamples =
        bytesPerSample == 0 ? 0 : buffer.bytes.size() / bytesPerSample;
    const std::size_t inputFrames =
        buffer.channels == 0 ? 0 : totalSamples / buffer.channels;

    // Build mono/stereo float at the buffer's own rate first.
    std::vector<float> left(inputFrames), right(inputFrames);
    for (std::size_t frame = 0; frame < inputFrames; ++frame) {
        const float l = sampleToFloat(buffer.bytes.data(), buffer.bitsPerSample,
                                      frame * buffer.channels);
        const float r = buffer.channels >= 2
                            ? sampleToFloat(buffer.bytes.data(), buffer.bitsPerSample,
                                            frame * buffer.channels + 1)
                            : l;
        left[frame] = l;
        right[frame] = r;
    }

    // Linear-interpolate resample to the output rate.
    const double ratio = buffer.sampleRate == 0
                             ? 1.0
                             : static_cast<double>(buffer.sampleRate) / kOutputRate;
    const std::uint64_t outputFrames =
        inputFrames == 0 ? 0
                         : static_cast<std::uint64_t>(std::ceil(inputFrames / ratio));
    out.frames.resize(outputFrames * kOutputChannels);
    for (std::uint64_t index = 0; index < outputFrames; ++index) {
        const double srcPos = index * ratio;
        const std::size_t i0 = static_cast<std::size_t>(srcPos);
        const std::size_t i1 = std::min(i0 + 1, inputFrames == 0 ? 0 : inputFrames - 1);
        const float frac = static_cast<float>(srcPos - i0);
        const std::size_t clamped0 = std::min(i0, inputFrames == 0 ? 0 : inputFrames - 1);
        const float l = left[clamped0] * (1.0f - frac) + left[i1] * frac;
        const float r = right[clamped0] * (1.0f - frac) + right[i1] * frac;
        out.frames[index * 2] = l;
        out.frames[index * 2 + 1] = r;
    }
    out.frameCount = outputFrames;
    entry.generation = generation;
    entry.normalized = std::move(out);
    return entry.normalized;
}

void Engine::renderSource(SourceState &source, float *out, std::uint64_t frameCount) {
    // Caller must hold mutex_.
    for (std::uint64_t frame = 0; frame < frameCount; ++frame) {
        if (!source.playing)
            return;
        // Advance past any exhausted buffers.
        while (true) {
            if (source.cursor >= source.queue.size()) {
                if (source.looping && !source.queue.empty()) {
                    source.cursor = 0;
                    source.framePosition = 0;
                    continue;
                }
                source.playing = false;
                return;
            }
            const auto bufferFound = buffers_.find(source.queue[source.cursor]);
            if (bufferFound == buffers_.end()) {
                // Missing buffer: skip it so a deleted buffer cannot wedge us.
                source.cursor++;
                source.framePosition = 0;
                continue;
            }
            const NormalizedBuffer &buffer = normalized(bufferFound->second);
            if (source.framePosition < buffer.frameCount)
                break;
            // Fully consumed this buffer. Looping buffers auto-replay and are
            // never handed back to the guest, so only non-looping consumption
            // counts toward AL_BUFFERS_PROCESSED.
            source.cursor++;
            source.framePosition = 0;
            if (!source.looping)
                source.processed++;
        }
        const NormalizedBuffer &buffer = normalized(buffers_.at(source.queue[source.cursor]));
        const float *sample = buffer.frames.data() + source.framePosition * kOutputChannels;
        out[frame * 2] += sample[0] * source.gain;
        out[frame * 2 + 1] += sample[1] * source.gain;
        source.framePosition++;
    }
}

std::size_t Engine::render(float *out, std::uint64_t frameCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::memset(out, 0, static_cast<std::size_t>(frameCount) * kOutputChannels * sizeof(float));
    std::size_t active = 0;
    for (auto &pair : sources_) {
        if (!pair.second.playing)
            continue;
        const bool hadQueue = !pair.second.queue.empty();
        renderSource(pair.second, out, frameCount);
        if (hadQueue)
            active++;
    }
    return active;
}

#ifdef __ANDROID__

namespace {

aaudio_data_callback_result_t aaudioDataCallback(AAudioStream *, void *userData, void *audioData,
                                                 int32_t numFrames) {
    auto *engine = static_cast<Engine *>(userData);
    // Mix in float, then convert to the 16-bit stream format.
    std::vector<float> mix(static_cast<std::size_t>(numFrames) * Engine::kOutputChannels);
    engine->render(mix.data(), static_cast<std::uint64_t>(numFrames));
    auto *out = static_cast<std::int16_t *>(audioData);
    for (std::size_t index = 0; index < mix.size(); ++index) {
        const float clamped = std::max(-1.0f, std::min(1.0f, mix[index]));
        out[index] = static_cast<std::int16_t>(clamped * 32767.0f);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

} // namespace

bool Engine::openDevice() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deviceOpen_)
        return true;
    AAudioStreamBuilder *builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || builder == nullptr)
        return false;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(kOutputRate));
    AAudioStreamBuilder_setChannelCount(builder, static_cast<int32_t>(kOutputChannels));
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, aaudioDataCallback, this);
    AAudioStream *stream = nullptr;
    const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (opened != AAUDIO_OK || stream == nullptr)
        return false;
    if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
        AAudioStream_close(stream);
        return false;
    }
    stream_ = stream;
    deviceOpen_ = true;
    return true;
}

void Engine::closeDevice() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_ != nullptr) {
        auto *stream = static_cast<AAudioStream *>(stream_);
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
        stream_ = nullptr;
    }
    deviceOpen_ = false;
}

bool Engine::deviceOpen() const noexcept { return deviceOpen_; }

#else // host / no AAudio

bool Engine::openDevice() {
    // No audio backend off-device; the mixer still serves unit tests.
    return false;
}

void Engine::closeDevice() {}

bool Engine::deviceOpen() const noexcept { return false; }

#endif

Engine &engine() {
    static Engine instance;
    return instance;
}

} // namespace radek::compat_runtime::openal
