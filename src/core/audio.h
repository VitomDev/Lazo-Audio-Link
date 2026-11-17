#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lazo {

struct OutputDevice {
    std::wstring id;
    std::wstring name;
};

// The active outputs (speakers, headphones, HDMI...). Needs COM on the thread.
std::vector<OutputDevice> EnumerateOutputs();
// The default output for media (the console role); empty if there is none.
std::wstring DefaultOutputId();
std::wstring OutputName(const std::wstring& id);

// The output's mute switch, the one in the Windows volume flyout.
bool IsOutputMuted(const std::wstring& id);
bool SetOutputMuted(const std::wstring& id, bool muted);

// Records what an output plays (WASAPI loopback) and hands it on as 16-bit
// stereo packets of 10 ms, on its own thread. Silence keeps flowing while
// nothing plays, so the phone's buffer never runs dry. Follows the Windows
// default output when no output is chosen, and starts again on its own when
// the output changes or disappears.
class Capture {
public:
    // Called on the capture thread.
    using PacketSink = std::function<void(const int16_t* stereo, size_t frames)>;
    // The stream's format, called before the first packet and on every change.
    using FormatSink = std::function<void(uint32_t rate, size_t framesPerPacket)>;

    Capture() = default;
    ~Capture() { Stop(); }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    void Start(const std::wstring& outputId, PacketSink packets, FormatSink format);
    void Stop();
    bool IsRunning() const { return thread_.joinable(); }

    // The output being recorded right now, empty while none is.
    std::wstring DeviceId() const;
    std::wstring DeviceName() const;
    // Peak level of the last packets, 0..1, falling slowly like a meter.
    float Level() const { return level_.load(); }
    // Times Windows reported lost audio (should stay 0).
    uint32_t Discontinuities() const { return discontinuities_.load(); }
    // Silent stretches the capture had to fill itself.
    uint32_t SilenceFills() const { return silenceFills_.load(); }

private:
    struct Watcher;

    void Run();
    // Records one device until it fails, changes or Stop is called.
    // Returns false when it could not start.
    bool RunDevice(void* enumerator);
    void SetDevice(const std::wstring& id, const std::wstring& name);

    std::wstring outputId_;
    PacketSink packets_;
    FormatSink format_;

    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    HANDLE changeEvent_ = nullptr;

    mutable std::mutex mutex_;
    std::wstring deviceId_;
    std::wstring deviceName_;
    std::atomic<float> level_{0.0f};
    std::atomic<uint32_t> discontinuities_{0};
    std::atomic<uint32_t> silenceFills_{0};
};

}  // namespace lazo
