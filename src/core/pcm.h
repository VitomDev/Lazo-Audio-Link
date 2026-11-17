#pragma once

// Turning what Windows mixes into the stream the phone plays: 16-bit stereo
// PCM in packets of 10 ms. No Windows calls, so all of it is tested directly.

#include <windows.h>
#include <mmreg.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lazo {

enum class SampleType { Float32, Int16, Int24, Int32 };

struct SampleFormat {
    SampleType type = SampleType::Float32;
    uint32_t rate = 48000;
    int channels = 2;
    uint32_t channelMask = 0;  // SPEAKER_* bits, 0 when unknown
    int blockAlign = 8;        // bytes per frame
};

// The device's mix format, if Lazo can read it.
std::optional<SampleFormat> DescribeFormat(const WAVEFORMATEX& format);

// Clamped to [-1, 1] and rounded, so loud peaks clip instead of wrapping.
int16_t FloatToInt16(float sample);

// Appends `frames` frames of `data` to `out` as interleaved 16-bit stereo.
// Mono is copied to both sides; surround is folded down into left and right
// (centre and rear at -3 dB, LFE left out).
void AppendStereo16(const uint8_t* data, size_t frames, const SampleFormat& format, std::vector<int16_t>& out);

// The loudest sample of interleaved 16-bit audio, 0..1.
float PeakLevel(const int16_t* samples, size_t count);

// Cuts a continuous stereo stream into packets of a fixed number of frames.
class Packetizer {
public:
    using Sink = std::function<void(const int16_t* stereo, size_t frames)>;

    Packetizer(size_t framesPerPacket, Sink sink);
    size_t FramesPerPacket() const { return framesPerPacket_; }

    void Push(const int16_t* stereo, size_t frames);
    void PushSilence(size_t frames);
    // Drops a partial packet, e.g. when the device changes.
    void Reset() { pending_.clear(); }

private:
    void Flush();

    size_t framesPerPacket_;
    Sink sink_;
    std::vector<int16_t> pending_;
};

// Loopback capture delivers nothing while nothing plays. This tracks the
// time, in frames, since the last real audio and says how much silence keeps
// the stream flowing, so the phone never runs dry and never has to rebuffer.
class SilenceFill {
public:
    // `toleranceFrames`: how long a pause in the capture is normal jitter.
    explicit SilenceFill(uint64_t toleranceFrames) : tolerance_(toleranceFrames) {}

    void Start(uint64_t now);
    void OnAudio(uint64_t now);
    // Frames of silence due at `now` (0 while audio arrives normally).
    uint64_t Due(uint64_t now);
    bool Filling() const { return filling_; }

private:
    uint64_t tolerance_;
    uint64_t lastAudio_ = 0;
    uint64_t cursor_ = 0;
    bool filling_ = false;
};

// The data saver stream: stereo folded to mono at half the sample rate,
// a quarter of the data, for weak Wi-Fi. A low-pass filter before dropping
// every other sample keeps the highs from folding back as noise. Keeps its
// state between packets, so the stream stays continuous.
class SaverDownsampler {
public:
    SaverDownsampler();
    // Appends the mono samples for `frames` frames of 16-bit stereo.
    void Process(const int16_t* stereo, size_t frames, std::vector<int16_t>& mono);
    void Reset();

private:
    std::vector<float> taps_;
    std::vector<float> history_;  // the last taps - 1 input samples
    bool odd_ = false;            // the next input sample is dropped
};

// The first message of every stream: what the PCM that follows is.
std::string StreamConfigJson(uint32_t rate, int channels, size_t framesPerPacket, std::wstring_view pcName);

}  // namespace lazo
