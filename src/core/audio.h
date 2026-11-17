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

// An application with an audio session: one that plays, or played, sound.
struct AudioApp {
    std::wstring exe;   // "chrome.exe", how Lazo finds it again
    std::wstring name;  // "Google Chrome"
};

// Recording a single application needs Windows 10 version 2004 or newer.
bool AppCaptureSupported();
// Applications with audio sessions on any active output, Lazo excluded.
std::vector<AudioApp> EnumerateAudioApps();

// What Capture records: everything an output plays, or one application.
struct CaptureSource {
    std::wstring outputId;  // empty follows the Windows default output
    std::wstring appExe;    // set: only this application (and its child processes)
    std::wstring appName;   // shown while the application is not running

    bool operator==(const CaptureSource&) const = default;
};

// Records what an output or an application plays (WASAPI loopback) and
// hands it on as 16-bit stereo packets of 10 ms, on its own thread. Silence
// keeps flowing while nothing plays, so the listener's buffer never runs dry.
// Follows the Windows default output when no output is chosen, and starts
// again on its own when the output changes or disappears, or when the
// application closes and opens again.
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

    void Start(const CaptureSource& source, PacketSink packets, FormatSink format);
    void Stop();
    bool IsRunning() const { return thread_.joinable(); }

    // The output being recorded right now, empty while none is (and while
    // recording an application).
    std::wstring DeviceId() const;
    // The output's or the application's name while recording, else empty.
    std::wstring DeviceName() const;
    // Peak level of the last packets, 0..1, falling slowly like a meter.
    float Level() const { return level_.load(); }
    // Times Windows reported lost audio (should stay 0).
    uint32_t Discontinuities() const { return discontinuities_.load(); }
    // Silent stretches the capture had to fill itself.
    uint32_t SilenceFills() const { return silenceFills_.load(); }

private:
    struct Watcher;
    struct Stream;

    void Run();
    // Record one output or application until it fails, changes or Stop is
    // called. They return false when they could not start.
    bool RunOutput(void* enumerator);
    bool RunApp();
    // The shared read loop. `extraWait` (may be null) ends it when signaled.
    void Pump(Stream& stream, HANDLE extraWait);
    void SetDevice(const std::wstring& id, const std::wstring& name);

    CaptureSource source_;
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
