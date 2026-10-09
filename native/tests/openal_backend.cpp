// OpenAL mixer backend: proves the software mixer really produces audio samples
// from captured guest PCM, handles 8/16-bit decoding, mono->stereo, resampling,
// looping, processed-buffer bookkeeping, and gain, all without any device.
#include "compat_runtime/openal_backend.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression))                                                                           \
            throw std::runtime_error("CHECK failed: " #expression);                                  \
    } while (false)

namespace {
using radek::compat_runtime::openal::Engine;
using radek::compat_runtime::openal::PcmBuffer;

constexpr std::uint32_t kRate = 44100;

// Build a mono 16-bit sine buffer at the given rate.
PcmBuffer makeMono16(std::uint32_t name, std::uint32_t frames, std::uint32_t rate, float amplitude,
                     float frequency = 440.0f) {
    PcmBuffer buffer;
    buffer.name = name;
    buffer.channels = 1;
    buffer.bitsPerSample = 16;
    buffer.sampleRate = rate;
    buffer.bytes.resize(static_cast<std::size_t>(frames) * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double phase = 2.0 * M_PI * static_cast<double>(frequency) * i / rate;
        const auto sample = static_cast<std::int16_t>(std::sin(phase) * amplitude * 32767.0);
        std::memcpy(buffer.bytes.data() + static_cast<std::size_t>(i) * 2, &sample, 2);
    }
    return buffer;
}

double rms(const std::vector<float> &samples) {
    double sum = 0.0;
    for (const float value : samples)
        sum += static_cast<double>(value) * value;
    return std::sqrt(sum / static_cast<double>(samples.size() == 0 ? 1 : samples.size()));
}

void testMono16ProducesStereoSamples() {
    Engine engine;
    const std::uint32_t frames = 4410; // 100 ms
    engine.storeBuffer(makeMono16(1, frames, kRate, 0.5f));
    engine.queue(10, 1);
    engine.play(10);

    std::vector<float> out(static_cast<std::size_t>(frames) * 2, 0.0f);
    const auto active = engine.render(out.data(), frames);
    CHECK(active == 1);
    CHECK(rms(out) > 0.01);
    // Mono is duplicated: each frame's L must equal R.
    CHECK(out[0] == out[1]);
    CHECK(out[2] == out[3]);
}

void testGainZeroIsSilent() {
    Engine engine;
    engine.storeBuffer(makeMono16(1, 4410, kRate, 0.5f));
    engine.queue(10, 1);
    engine.play(10);
    engine.setGain(10, 0.0f);
    std::vector<float> out(4410 * 2, 0.0f);
    engine.render(out.data(), 4410);
    CHECK(rms(out) == 0.0);
}

void testStopSilencesSource() {
    Engine engine;
    engine.storeBuffer(makeMono16(1, 4410, kRate, 0.5f));
    engine.queue(10, 1);
    engine.play(10);
    engine.stop(10);
    CHECK(!engine.isPlaying(10));
    std::vector<float> out(4410 * 2, 0.0f);
    const auto active = engine.render(out.data(), 4410);
    CHECK(active == 0);
    CHECK(rms(out) == 0.0);
}

void testProcessedAndUnqueue() {
    Engine engine;
    // A short non-looping buffer; render far past its end so it fully plays.
    engine.storeBuffer(makeMono16(7, 1000, kRate, 0.5f));
    engine.queue(10, 7);
    engine.play(10);
    std::vector<float> out(4410 * 2, 0.0f);
    engine.render(out.data(), 4410);
    CHECK(engine.processedCount(10) == 1);
    const auto names = engine.unqueue(10, 4);
    CHECK(names.size() == 1);
    CHECK(names[0] == 7);
    CHECK(engine.processedCount(10) == 0);
    CHECK(!engine.isPlaying(10)); // exhausted buffer stops the source
}

void testLoopingNeverReportsProcessedAndKeepsPlaying() {
    Engine engine;
    engine.storeBuffer(makeMono16(7, 200, kRate, 0.5f));
    engine.queue(10, 7);
    engine.setLooping(10, true);
    engine.play(10);
    // Render far longer than the buffer so it must wrap many times.
    std::vector<float> out(4410 * 2, 0.0f);
    const auto active = engine.render(out.data(), 4410);
    CHECK(active == 1);
    CHECK(engine.isPlaying(10));            // still looping
    CHECK(engine.processedCount(10) == 0);  // looping buffers don't count
    CHECK(rms(out) > 0.01);                 // audibly continuous
}

void testStereo16KeepsChannelSeparation() {
    Engine engine;
    PcmBuffer buffer;
    buffer.name = 3;
    buffer.channels = 2;
    buffer.bitsPerSample = 16;
    buffer.sampleRate = kRate;
    const std::uint32_t frames = 256;
    buffer.bytes.resize(static_cast<std::size_t>(frames) * 4);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::int16_t leftSample = 10000;
        const std::int16_t rightSample = -10000;
        std::memcpy(buffer.bytes.data() + i * 4, &leftSample, 2);
        std::memcpy(buffer.bytes.data() + i * 4 + 2, &rightSample, 2);
    }
    engine.storeBuffer(buffer);
    engine.queue(10, 3);
    engine.play(10);
    std::vector<float> out(frames * 2, 0.0f);
    engine.render(out.data(), frames);
    CHECK(out[0] > 0.2f);   // left positive
    CHECK(out[1] < -0.2f);  // right negative
}

void testUnsigned8BitDecodesAroundSilencePoint() {
    Engine engine;
    PcmBuffer buffer;
    buffer.name = 4;
    buffer.channels = 1;
    buffer.bitsPerSample = 8;
    buffer.sampleRate = kRate;
    const std::uint32_t frames = 256;
    buffer.bytes.resize(frames);
    for (std::uint32_t i = 0; i < frames; ++i)
        buffer.bytes[i] = static_cast<std::uint8_t>(i % 2 == 0 ? 255 : 1); // square wave
    engine.storeBuffer(buffer);
    engine.queue(10, 4);
    engine.play(10);
    std::vector<float> out(frames * 2, 0.0f);
    engine.render(out.data(), frames);
    CHECK(rms(out) > 0.3); // full-scale square wave is loud
}

void testResamplingHalvesLengthForHalfRate() {
    Engine engine;
    // 22050 Hz source should stretch to ~2x frames at 44100 output: 2205 input
    // frames become ~4410 output frames.
    const std::uint32_t inFrames = 2205; // 100 ms at 22050
    engine.storeBuffer(makeMono16(5, inFrames, 22050, 0.5f));
    engine.queue(10, 5);
    engine.play(10);
    // Render slightly past the stretched length so the completion registers.
    std::vector<float> out(5000 * 2, 0.0f);
    engine.render(out.data(), 5000);
    // If the buffer had been played at its raw 22050 rate it would be silent
    // from frame 2205 on. Resampling must keep it audible through frame ~4410.
    double secondHalfEnergy = 0.0;
    for (std::size_t i = 2205 * 2; i < 4410 * 2; ++i)
        secondHalfEnergy += static_cast<double>(out[i]) * out[i];
    CHECK(secondHalfEnergy > 0.0);
    CHECK(engine.processedCount(10) == 1); // fully consumed by the end
}

void testMissingBufferIsSkippedNotWedged() {
    Engine engine;
    engine.queue(10, 999); // buffer that was never stored
    engine.queue(10, 8);
    engine.storeBuffer(makeMono16(8, 500, kRate, 0.5f));
    engine.play(10);
    std::vector<float> out(1000 * 2, 0.0f);
    const auto active = engine.render(out.data(), 1000);
    CHECK(active == 1);
    CHECK(rms(out) > 0.0); // the valid buffer still played
}
} // namespace

int main() {
    testMono16ProducesStereoSamples();
    testGainZeroIsSilent();
    testStopSilencesSource();
    testProcessedAndUnqueue();
    testLoopingNeverReportsProcessedAndKeepsPlaying();
    testStereo16KeepsChannelSeparation();
    testUnsigned8BitDecodesAroundSilencePoint();
    testResamplingHalvesLengthForHalfRate();
    testMissingBufferIsSkippedNotWedged();
    return 0;
}
