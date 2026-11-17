#include "core/pcm.h"

#include "core/http.h"
#include "core/text.h"

#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace lazo {

namespace {

float ReadSample(const uint8_t* p, SampleType type) {
    switch (type) {
        case SampleType::Float32: {
            float value;
            std::memcpy(&value, p, sizeof(value));
            // NaN or infinity from a broken driver must not reach the phone.
            return std::isfinite(value) ? value : 0.0f;
        }
        case SampleType::Int16: {
            int16_t value;
            std::memcpy(&value, p, sizeof(value));
            return value / 32768.0f;
        }
        case SampleType::Int24: {
            const int32_t value = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) |
                                                       (static_cast<uint32_t>(p[2]) << 24)) >> 8;
            return value / 8388608.0f;
        }
        case SampleType::Int32: {
            int32_t value;
            std::memcpy(&value, p, sizeof(value));
            return static_cast<float>(value / 2147483648.0);
        }
    }
    return 0.0f;
}

int BytesPerSample(SampleType type) {
    switch (type) {
        case SampleType::Int16: return 2;
        case SampleType::Int24: return 3;
        default: return 4;
    }
}

// How much of each input channel goes to the left and right outputs.
struct Weights {
    float left;
    float right;
};

std::vector<Weights> DownmixWeights(const SampleFormat& format) {
    std::vector<Weights> weights(static_cast<size_t>(format.channels), {0.0f, 0.0f});
    if (format.channels == 1) {
        weights[0] = {1.0f, 1.0f};
        return weights;
    }
    constexpr float kMinus3dB = 0.70710678f;
    // Without a mask, the first two channels are taken as left and right.
    uint32_t mask = format.channelMask;
    if (mask == 0) mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    int channel = 0;
    for (uint32_t bit = 1; bit != 0 && channel < format.channels; bit <<= 1) {
        if (!(mask & bit)) continue;
        Weights& w = weights[static_cast<size_t>(channel++)];
        switch (bit) {
            case SPEAKER_FRONT_LEFT: w = {1.0f, 0.0f}; break;
            case SPEAKER_FRONT_RIGHT: w = {0.0f, 1.0f}; break;
            case SPEAKER_FRONT_CENTER: w = {kMinus3dB, kMinus3dB}; break;
            case SPEAKER_BACK_LEFT:
            case SPEAKER_SIDE_LEFT:
            case SPEAKER_FRONT_LEFT_OF_CENTER: w = {kMinus3dB, 0.0f}; break;
            case SPEAKER_BACK_RIGHT:
            case SPEAKER_SIDE_RIGHT:
            case SPEAKER_FRONT_RIGHT_OF_CENTER: w = {0.0f, kMinus3dB}; break;
            case SPEAKER_BACK_CENTER: w = {0.5f, 0.5f}; break;
            default: break;  // LFE and height channels
        }
    }
    return weights;
}

}  // namespace

std::optional<SampleFormat> DescribeFormat(const WAVEFORMATEX& format) {
    SampleFormat result;
    result.rate = format.nSamplesPerSec;
    result.channels = format.nChannels;
    result.blockAlign = format.nBlockAlign;
    WORD tag = format.wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        result.channelMask = extensible.dwChannelMask;
        if (IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            tag = WAVE_FORMAT_PCM;
        } else {
            return std::nullopt;
        }
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32) {
        result.type = SampleType::Float32;
    } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16) {
        result.type = SampleType::Int16;
    } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 24) {
        result.type = SampleType::Int24;
    } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 32) {
        // Also 24-bit audio in 32-bit containers: the low bits are just zero.
        result.type = SampleType::Int32;
    } else {
        return std::nullopt;
    }
    if (result.channels < 1 || result.rate < 8000 || result.rate > 384000 ||
        result.blockAlign < result.channels * BytesPerSample(result.type)) {
        return std::nullopt;
    }
    return result;
}

int16_t FloatToInt16(float sample) {
    if (std::isnan(sample)) return 0;
    if (sample <= -1.0f) return -32767;
    if (sample >= 1.0f) return 32767;
    return static_cast<int16_t>(std::lrintf(sample * 32767.0f));
}

void AppendStereo16(const uint8_t* data, size_t frames, const SampleFormat& format, std::vector<int16_t>& out) {
    const size_t start = out.size();
    out.resize(start + frames * 2);
    int16_t* target = out.data() + start;
    const int bytes = BytesPerSample(format.type);
    if (format.channels == 2) {
        for (size_t i = 0; i < frames; ++i) {
            const uint8_t* frame = data + i * static_cast<size_t>(format.blockAlign);
            target[2 * i] = FloatToInt16(ReadSample(frame, format.type));
            target[2 * i + 1] = FloatToInt16(ReadSample(frame + bytes, format.type));
        }
        return;
    }
    const std::vector<Weights> weights = DownmixWeights(format);
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* frame = data + i * static_cast<size_t>(format.blockAlign);
        float left = 0.0f;
        float right = 0.0f;
        for (int c = 0; c < format.channels; ++c) {
            const float sample = ReadSample(frame + c * bytes, format.type);
            left += sample * weights[static_cast<size_t>(c)].left;
            right += sample * weights[static_cast<size_t>(c)].right;
        }
        target[2 * i] = FloatToInt16(left);
        target[2 * i + 1] = FloatToInt16(right);
    }
}

float PeakLevel(const int16_t* samples, size_t count) {
    int peak = 0;
    for (size_t i = 0; i < count; ++i) peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
    return std::min(1.0f, peak / 32767.0f);
}

Packetizer::Packetizer(size_t framesPerPacket, Sink sink) : framesPerPacket_(framesPerPacket), sink_(std::move(sink)) {
    pending_.reserve(framesPerPacket_ * 4);
}

void Packetizer::Push(const int16_t* stereo, size_t frames) {
    pending_.insert(pending_.end(), stereo, stereo + frames * 2);
    Flush();
}

void Packetizer::PushSilence(size_t frames) {
    pending_.resize(pending_.size() + frames * 2, 0);
    Flush();
}

void Packetizer::Flush() {
    const size_t packetSamples = framesPerPacket_ * 2;
    size_t offset = 0;
    while (pending_.size() - offset >= packetSamples) {
        sink_(pending_.data() + offset, framesPerPacket_);
        offset += packetSamples;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(offset));
}

void SilenceFill::Start(uint64_t now) {
    lastAudio_ = now;
    cursor_ = now;
    filling_ = false;
}

void SilenceFill::OnAudio(uint64_t now) {
    lastAudio_ = now;
    filling_ = false;
}

uint64_t SilenceFill::Due(uint64_t now) {
    if (!filling_) {
        if (now < lastAudio_ + tolerance_) return 0;
        // Audio stopped at lastAudio_: everything since then is silence.
        filling_ = true;
        cursor_ = lastAudio_;
    }
    if (now <= cursor_) return 0;
    const uint64_t due = now - cursor_;
    cursor_ = now;
    return due;
}

std::string StreamConfigJson(uint32_t rate, size_t framesPerPacket, std::wstring_view pcName) {
    return "{\"rate\":" + std::to_string(rate) + ",\"channels\":2,\"frame\":" + std::to_string(framesPerPacket) +
           ",\"pc\":\"" + JsonEscape(ToUtf8(pcName)) + "\"}";
}

}  // namespace lazo
