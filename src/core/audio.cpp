#include "core/audio.h"

#include "core/pcm.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>

namespace lazo {

namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { Reset(); }
    void Reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** operator&() {
        Reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// The capture asks for 50 ms: with 10 ms Windows dropped audio now and then
// (AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY), heard as clicks. A larger buffer
// adds no delay; packets are still read every few milliseconds.
constexpr REFERENCE_TIME kCaptureBuffer = 50 * 10'000;
constexpr REFERENCE_TIME kSilentRenderBuffer = 100 * 10'000;
constexpr DWORD kPollMs = 5;
constexpr DWORD kRetryMs = 1000;

IMMDeviceEnumerator* CreateEnumerator() {
    IMMDeviceEnumerator* enumerator = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    return enumerator;
}

std::wstring DeviceId(IMMDevice* device) {
    LPWSTR id = nullptr;
    std::wstring result;
    if (SUCCEEDED(device->GetId(&id)) && id) result = id;
    CoTaskMemFree(id);
    return result;
}

std::wstring FriendlyName(IMMDevice* device) {
    Com<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

// The chosen output when it is active, otherwise the default one.
IMMDevice* OpenOutput(IMMDeviceEnumerator* enumerator, const std::wstring& id) {
    IMMDevice* device = nullptr;
    if (!id.empty() && SUCCEEDED(enumerator->GetDevice(id.c_str(), &device))) {
        DWORD state = 0;
        if (SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE) return device;
        device->Release();
        device = nullptr;
    }
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return nullptr;
    return device;
}

IAudioEndpointVolume* OpenVolume(const std::wstring& id) {
    Com<IMMDeviceEnumerator> enumerator;
    enumerator.p = CreateEnumerator();
    if (!enumerator) return nullptr;
    Com<IMMDevice> device;
    device.p = OpenOutput(enumerator.p, id);
    if (!device) return nullptr;
    IAudioEndpointVolume* volume = nullptr;
    device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&volume));
    return volume;
}

// Now, counted in frames at `rate`.
uint64_t NowInFrames(uint32_t rate) {
    static const LONGLONG frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const auto ticks = static_cast<uint64_t>(counter.QuadPart);
    const auto freq = static_cast<uint64_t>(frequency);
    return ticks / freq * rate + ticks % freq * rate / freq;
}

}  // namespace

std::vector<OutputDevice> EnumerateOutputs() {
    std::vector<OutputDevice> outputs;
    Com<IMMDeviceEnumerator> enumerator;
    enumerator.p = CreateEnumerator();
    if (!enumerator) return outputs;
    Com<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) return outputs;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        Com<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        outputs.push_back({DeviceId(device.p), FriendlyName(device.p)});
    }
    std::sort(outputs.begin(), outputs.end(), [](const OutputDevice& a, const OutputDevice& b) { return a.name < b.name; });
    return outputs;
}

std::wstring DefaultOutputId() {
    Com<IMMDeviceEnumerator> enumerator;
    enumerator.p = CreateEnumerator();
    if (!enumerator) return {};
    Com<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return {};
    return DeviceId(device.p);
}

std::wstring OutputName(const std::wstring& id) {
    Com<IMMDeviceEnumerator> enumerator;
    enumerator.p = CreateEnumerator();
    if (!enumerator) return {};
    Com<IMMDevice> device;
    if (FAILED(enumerator->GetDevice(id.c_str(), &device))) return {};
    return FriendlyName(device.p);
}

bool IsOutputMuted(const std::wstring& id) {
    Com<IAudioEndpointVolume> volume;
    volume.p = OpenVolume(id);
    BOOL muted = FALSE;
    return volume && SUCCEEDED(volume->GetMute(&muted)) && muted;
}

bool SetOutputMuted(const std::wstring& id, bool muted) {
    Com<IAudioEndpointVolume> volume;
    volume.p = OpenVolume(id);
    return volume && SUCCEEDED(volume->SetMute(muted, nullptr));
}

// --- Device notifications --------------------------------------------------------

// Core Audio calls this from its own threads; it only wakes the capture thread.
struct Capture::Watcher : IMMNotificationClient {
    Capture* owner;
    std::atomic<ULONG> references{1};

    explicit Watcher(Capture* capture) : owner(capture) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --references;
        if (count == 0) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow != eRender || role != eConsole) return S_OK;
        // Matters when following the default, or when the chosen output is
        // missing and the default stands in for it.
        if (owner->outputId_.empty() || owner->DeviceId() != owner->outputId_) SetEvent(owner->changeEvent_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        if (!id) return S_OK;
        const std::wstring changed = id;
        const bool current = changed == owner->DeviceId();
        const bool chosenIsBack = changed == owner->outputId_ && state == DEVICE_STATE_ACTIVE && !current;
        if ((current && state != DEVICE_STATE_ACTIVE) || chosenIsBack) SetEvent(owner->changeEvent_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

// --- Capture ---------------------------------------------------------------------

void Capture::Start(const std::wstring& outputId, PacketSink packets, FormatSink format) {
    Stop();
    outputId_ = outputId;
    packets_ = std::move(packets);
    format_ = std::move(format);
    level_ = 0.0f;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    changeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread(&Capture::Run, this);
}

void Capture::Stop() {
    if (!thread_.joinable()) return;
    SetEvent(stopEvent_);
    thread_.join();
    CloseHandle(stopEvent_);
    CloseHandle(changeEvent_);
    stopEvent_ = changeEvent_ = nullptr;
    SetDevice({}, {});
    level_ = 0.0f;
}

std::wstring Capture::DeviceId() const {
    std::lock_guard lock(mutex_);
    return deviceId_;
}

std::wstring Capture::DeviceName() const {
    std::lock_guard lock(mutex_);
    return deviceName_;
}

void Capture::SetDevice(const std::wstring& id, const std::wstring& name) {
    std::lock_guard lock(mutex_);
    deviceId_ = id;
    deviceName_ = name;
}

void Capture::Run() {
    // Audio work gets the scheduling Windows gives to media players.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = CreateEnumerator();
    Watcher* watcher = nullptr;
    if (enumerator) {
        watcher = new Watcher(this);
        enumerator->RegisterEndpointNotificationCallback(watcher);
    }
    const HANDLE events[] = {stopEvent_, changeEvent_};
    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        ResetEvent(changeEvent_);
        if (!enumerator || !RunDevice(enumerator)) {
            SetDevice({}, {});
            level_ = 0.0f;
            // No output right now: try again soon, or as soon as one appears.
            WaitForMultipleObjects(2, events, FALSE, kRetryMs);
        }
    }
    if (enumerator) {
        enumerator->UnregisterEndpointNotificationCallback(watcher);
        enumerator->Release();
    }
    if (watcher) watcher->Release();
    if (SUCCEEDED(coInit)) CoUninitialize();
}

bool Capture::RunDevice(void* enumeratorPointer) {
    auto* enumerator = static_cast<IMMDeviceEnumerator*>(enumeratorPointer);
    Com<IMMDevice> device;
    device.p = OpenOutput(enumerator, outputId_);
    if (!device) return false;

    Com<IAudioClient> captureClient;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&captureClient)))) {
        return false;
    }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(captureClient->GetMixFormat(&mix)) || !mix) return false;
    const auto format = DescribeFormat(*mix);
    Com<IAudioCaptureClient> capture;
    const bool captureReady =
        format &&
        SUCCEEDED(captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kCaptureBuffer, 0, mix, nullptr)) &&
        SUCCEEDED(captureClient->GetService(IID_PPV_ARGS(&capture)));
    if (!captureReady) {
        CoTaskMemFree(mix);
        return false;
    }

    // A silent stream played on the same output keeps Windows mixing it, so
    // the loopback delivers packets (of silence) even when nothing plays,
    // timed by the output's own clock. If it cannot be opened, the silence is
    // filled in below instead.
    Com<IAudioClient> renderClient;
    Com<IAudioRenderClient> render;
    UINT32 renderFrames = 0;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&renderClient))) ||
        FAILED(renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kSilentRenderBuffer, 0, mix, nullptr)) ||
        FAILED(renderClient->GetBufferSize(&renderFrames)) || FAILED(renderClient->GetService(IID_PPV_ARGS(&render)))) {
        render.Reset();
        renderClient.Reset();
    }
    CoTaskMemFree(mix);
    const auto topUpSilence = [&] {
        if (!render) return;
        UINT32 padding = 0;
        if (FAILED(renderClient->GetCurrentPadding(&padding)) || padding >= renderFrames) return;
        BYTE* buffer = nullptr;
        const UINT32 free = renderFrames - padding;
        if (SUCCEEDED(render->GetBuffer(free, &buffer))) render->ReleaseBuffer(free, AUDCLNT_BUFFERFLAGS_SILENT);
    };

    SetDevice(::lazo::DeviceId(device.p), FriendlyName(device.p));
    const uint32_t rate = format->rate;
    const size_t framesPerPacket = rate / 100;
    if (format_) format_(rate, framesPerPacket);

    Packetizer packetizer(framesPerPacket, [this](const int16_t* stereo, size_t frames) {
        const float peak = PeakLevel(stereo, frames * 2);
        level_ = std::max(peak, level_.load() * 0.85f);
        if (packets_) packets_(stereo, frames);
    });
    // 40 ms without packets is not jitter any more: nothing is playing.
    SilenceFill silence(rate * 40 / 1000);
    std::vector<int16_t> stereo;
    stereo.reserve(framesPerPacket * 8);

    topUpSilence();
    if (renderClient) renderClient->Start();
    captureClient->Start();
    silence.Start(NowInFrames(rate));

    const HANDLE events[] = {stopEvent_, changeEvent_};
    bool failed = false;
    bool first = true;
    while (WaitForMultipleObjects(2, events, FALSE, kPollMs) == WAIT_TIMEOUT) {
        topUpSilence();
        UINT32 next = 0;
        HRESULT result;
        while (SUCCEEDED(result = capture->GetNextPacketSize(&next)) && next > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            result = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(result)) break;
            // The very first packet is always flagged; only later ones mean loss.
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && !first) ++discontinuities_;
            first = false;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                packetizer.PushSilence(frames);
            } else {
                stereo.clear();
                AppendStereo16(data, frames, *format, stereo);
                packetizer.Push(stereo.data(), frames);
            }
            capture->ReleaseBuffer(frames);
            silence.OnAudio(NowInFrames(rate));
        }
        if (FAILED(result)) {
            // Unplugged, disabled or taken over in exclusive mode.
            failed = true;
            break;
        }
        const bool wasFilling = silence.Filling();
        if (const uint64_t due = silence.Due(NowInFrames(rate))) {
            if (!wasFilling) ++silenceFills_;
            // After sleep or a long stall, send a little and move on: the
            // phone must not receive minutes of silence at once.
            packetizer.PushSilence(static_cast<size_t>(std::min<uint64_t>(due, rate / 5)));
        }
    }
    captureClient->Stop();
    if (renderClient) renderClient->Stop();
    SetDevice({}, {});
    if (failed) WaitForMultipleObjects(2, events, FALSE, kRetryMs / 2);
    return true;
}

}  // namespace lazo
