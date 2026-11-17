#include "core/audio.h"

#include "core/pcm.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <map>

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
// What an application's sound is converted to by Windows.
constexpr uint32_t kAppRate = 48000;

IMMDeviceEnumerator* CreateEnumerator() {
    IMMDeviceEnumerator* enumerator = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    return enumerator;
}

std::wstring EndpointId(IMMDevice* device) {
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

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

std::wstring ProcessPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH * 2]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) result.assign(path, size);
    CloseHandle(process);
    return result;
}

// "Google Chrome" from chrome.exe's version information, else "chrome".
std::wstring ProgramName(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size > 0) {
        std::vector<uint8_t> data(size);
        struct Translation {
            WORD language;
            WORD codePage;
        }* translations = nullptr;
        UINT length = 0;
        if (GetFileVersionInfoW(path.c_str(), 0, size, data.data()) &&
            VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length) &&
            length >= sizeof(Translation)) {
            wchar_t key[64];
            swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[0].language, translations[0].codePage);
            wchar_t* description = nullptr;
            if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&description), &length) && description && length > 1) {
                std::wstring text(description);
                while (!text.empty() && text.back() == L' ') text.pop_back();
                if (!text.empty()) return text;
            }
        }
    }
    return std::filesystem::path(path).stem().wstring();
}

// The first process of `exe` whose parent is not `exe` too: recording it
// with its process tree covers browsers' separate audio processes.
DWORD FindAppRoot(const std::wstring& exe) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    std::map<DWORD, DWORD> parents;  // pid -> parent, for processes of `exe`
    PROCESSENTRY32W entry{sizeof(entry)};
    const std::wstring wanted = Lower(exe);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (Lower(entry.szExeFile) == wanted) parents[entry.th32ProcessID] = entry.th32ParentProcessID;
    }
    CloseHandle(snapshot);
    for (const auto& [pid, parent] : parents) {
        if (!parents.contains(parent)) return pid;
    }
    return parents.empty() ? 0 : parents.begin()->first;
}

// ActivateAudioInterfaceAsync reports back on another thread.
struct ActivationHandler : IActivateAudioInterfaceCompletionHandler {
    std::atomic<ULONG> references{1};
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    ~ActivationHandler() { CloseHandle(done); }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --references;
        if (count == 0) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        // IAgileObject has nothing beyond IUnknown: callable from any thread.
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler) || iid == __uuidof(IAgileObject)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done);
        return S_OK;
    }
};

// An IAudioClient that hears only `pid` and the processes it started.
IAudioClient* ActivateProcessLoopback(DWORD pid) {
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    parameters.ProcessLoopbackParams.TargetProcessId = pid;
    parameters.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(parameters);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);

    auto* handler = new ActivationHandler();
    Com<IActivateAudioInterfaceAsyncOperation> operation;
    IAudioClient* client = nullptr;
    if (SUCCEEDED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activation, handler,
                                              &operation)) &&
        WaitForSingleObject(handler->done, 5000) == WAIT_OBJECT_0) {
        HRESULT result = E_FAIL;
        Com<IUnknown> unknown;
        if (SUCCEEDED(operation->GetActivateResult(&result, &unknown)) && SUCCEEDED(result) && unknown) {
            unknown->QueryInterface(IID_PPV_ARGS(&client));
        }
    }
    handler->Release();
    return client;
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
        outputs.push_back({EndpointId(device.p), FriendlyName(device.p)});
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
    return EndpointId(device.p);
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

bool AppCaptureSupported() {
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    static const bool supported = [] {
        OSVERSIONINFOW version{sizeof(version)};
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto get = ntdll ? reinterpret_cast<RtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        return get && get(&version) == 0 && (version.dwMajorVersion > 10 || version.dwBuildNumber >= 19041);
    }();
    return supported;
}

std::vector<AudioApp> EnumerateAudioApps() {
    std::vector<AudioApp> apps;
    Com<IMMDeviceEnumerator> enumerator;
    enumerator.p = CreateEnumerator();
    if (!enumerator) return apps;
    Com<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) return apps;
    UINT count = 0;
    collection->GetCount(&count);
    const DWORD self = GetCurrentProcessId();
    for (UINT i = 0; i < count; ++i) {
        Com<IMMDevice> device;
        Com<IAudioSessionManager2> manager;
        Com<IAudioSessionEnumerator> sessions;
        if (FAILED(collection->Item(i, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager))) ||
            FAILED(manager->GetSessionEnumerator(&sessions))) {
            continue;
        }
        int sessionCount = 0;
        sessions->GetCount(&sessionCount);
        for (int s = 0; s < sessionCount; ++s) {
            Com<IAudioSessionControl> control;
            Com<IAudioSessionControl2> control2;
            DWORD pid = 0;
            if (FAILED(sessions->GetSession(s, &control)) ||
                FAILED(control->QueryInterface(IID_PPV_ARGS(&control2))) || control2->IsSystemSoundsSession() == S_OK ||
                FAILED(control2->GetProcessId(&pid)) || pid == 0 || pid == self) {
                continue;
            }
            const std::wstring path = ProcessPath(pid);
            if (path.empty()) continue;
            const std::wstring exe = Lower(std::filesystem::path(path).filename().wstring());
            const bool known = std::any_of(apps.begin(), apps.end(), [&](const AudioApp& app) { return app.exe == exe; });
            if (!known) apps.push_back({exe, ProgramName(path)});
        }
    }
    std::sort(apps.begin(), apps.end(), [](const AudioApp& a, const AudioApp& b) { return Lower(a.name) < Lower(b.name); });
    return apps;
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
        // An application is heard wherever it plays; outputs don't matter.
        if (flow != eRender || role != eConsole || !owner->source_.appExe.empty()) return S_OK;
        // Matters when following the default, or when the chosen output is
        // missing and the default stands in for it.
        if (owner->source_.outputId.empty() || owner->DeviceId() != owner->source_.outputId) SetEvent(owner->changeEvent_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        if (!id || !owner->source_.appExe.empty()) return S_OK;
        const std::wstring changed = id;
        const bool current = changed == owner->DeviceId();
        const bool chosenIsBack = changed == owner->source_.outputId && state == DEVICE_STATE_ACTIVE && !current;
        if ((current && state != DEVICE_STATE_ACTIVE) || chosenIsBack) SetEvent(owner->changeEvent_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

// --- Capture ---------------------------------------------------------------------

// One open recording: what Pump reads from.
struct Capture::Stream {
    IAudioClient* captureClient = nullptr;
    IAudioCaptureClient* capture = nullptr;
    SampleFormat format;
    // Optional: the silent stream that keeps an output's loopback flowing.
    IAudioClient* renderClient = nullptr;
    IAudioRenderClient* render = nullptr;
    UINT32 renderFrames = 0;
    // Optional: signaled by Windows when packets are ready.
    HANDLE ready = nullptr;
};

void Capture::Start(const CaptureSource& source, PacketSink packets, FormatSink format) {
    Stop();
    source_ = source;
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
        const bool ran = !source_.appExe.empty() ? RunApp() : enumerator && RunOutput(enumerator);
        if (!ran) {
            SetDevice({}, {});
            level_ = 0.0f;
            // Nothing to record right now (no output, the application is
            // closed): try again soon, or as soon as an output appears.
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

bool Capture::RunOutput(void* enumeratorPointer) {
    auto* enumerator = static_cast<IMMDeviceEnumerator*>(enumeratorPointer);
    Com<IMMDevice> device;
    device.p = OpenOutput(enumerator, source_.outputId);
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
    // timed by the output's own clock. If it cannot be opened, Pump fills
    // the silence in instead.
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

    SetDevice(EndpointId(device.p), FriendlyName(device.p));
    Stream stream;
    stream.captureClient = captureClient.p;
    stream.capture = capture.p;
    stream.format = *format;
    stream.renderClient = renderClient.p;
    stream.render = render.p;
    stream.renderFrames = renderFrames;
    Pump(stream, nullptr);
    return true;
}

bool Capture::RunApp() {
    if (!AppCaptureSupported()) return false;
    const DWORD pid = FindAppRoot(source_.appExe);
    if (!pid) return false;
    // When the application closes, start over and wait for it to open again.
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;

    Com<IAudioClient> captureClient;
    captureClient.p = ActivateProcessLoopback(pid);
    // Windows converts the application's sound to this format.
    WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT, 2, kAppRate, kAppRate * 8, 8, 32, 0};
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Com<IAudioCaptureClient> capture;
    const bool captureReady =
        captureClient &&
        SUCCEEDED(captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                            kCaptureBuffer, 0, &format, nullptr)) &&
        SUCCEEDED(captureClient->SetEventHandle(ready)) && SUCCEEDED(captureClient->GetService(IID_PPV_ARGS(&capture)));
    if (captureReady) {
        const std::wstring path = ProcessPath(pid);
        SetDevice({}, path.empty() ? source_.appName : ProgramName(path));
        Stream stream;
        stream.captureClient = captureClient.p;
        stream.capture = capture.p;
        stream.format = *DescribeFormat(format);
        stream.ready = ready;
        Pump(stream, process);
    }
    CloseHandle(ready);
    CloseHandle(process);
    return captureReady;
}

void Capture::Pump(Stream& stream, HANDLE extraWait) {
    const auto topUpSilence = [&] {
        if (!stream.render) return;
        UINT32 padding = 0;
        if (FAILED(stream.renderClient->GetCurrentPadding(&padding)) || padding >= stream.renderFrames) return;
        BYTE* buffer = nullptr;
        const UINT32 free = stream.renderFrames - padding;
        if (SUCCEEDED(stream.render->GetBuffer(free, &buffer))) stream.render->ReleaseBuffer(free, AUDCLNT_BUFFERFLAGS_SILENT);
    };

    const uint32_t rate = stream.format.rate;
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
    if (stream.renderClient) stream.renderClient->Start();
    stream.captureClient->Start();
    silence.Start(NowInFrames(rate));

    HANDLE events[4] = {stopEvent_, changeEvent_};
    DWORD eventCount = 2;
    if (extraWait) events[eventCount++] = extraWait;
    const DWORD readyIndex = eventCount;
    if (stream.ready) events[eventCount++] = stream.ready;
    bool failed = false;
    bool first = true;
    for (;;) {
        const DWORD woke = WaitForMultipleObjects(eventCount, events, FALSE, kPollMs);
        if (woke != WAIT_TIMEOUT && woke != WAIT_OBJECT_0 + readyIndex) break;
        topUpSilence();
        UINT32 next = 0;
        HRESULT result;
        while (SUCCEEDED(result = stream.capture->GetNextPacketSize(&next)) && next > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            result = stream.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(result)) break;
            // The very first packet is always flagged; only later ones mean loss.
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && !first) ++discontinuities_;
            first = false;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                packetizer.PushSilence(frames);
            } else {
                stereo.clear();
                AppendStereo16(data, frames, stream.format, stereo);
                packetizer.Push(stereo.data(), frames);
            }
            stream.capture->ReleaseBuffer(frames);
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
            // listener must not receive minutes of silence at once.
            packetizer.PushSilence(static_cast<size_t>(std::min<uint64_t>(due, rate / 5)));
        }
    }
    stream.captureClient->Stop();
    if (stream.renderClient) stream.renderClient->Stop();
    SetDevice({}, {});
    if (failed) {
        const HANDLE wait[] = {stopEvent_, changeEvent_};
        WaitForMultipleObjects(2, wait, FALSE, kRetryMs / 2);
    }
}

}  // namespace lazo
