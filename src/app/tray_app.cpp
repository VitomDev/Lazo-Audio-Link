#include "app/tray_app.h"
#include "app/vitool_tray.h"

#include "app/resource.h"
#include "app/settings_dialog.h"
#include "app/theme.h"
#include "core/http.h"
#include "core/network.h"
#include "core/paths.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>

namespace lazo {

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 10;
constexpr UINT WM_APP_SERVER = WM_APP + 11;

constexpr UINT_PTR kTickTimer = 1;
// Four times a second while recording, to mute the speakers right as a
// device starts listening; once a second otherwise.
constexpr UINT kTickMs = 250;
constexpr UINT kIdleTickMs = 1000;
constexpr ULONGLONG kNetworkCheckMs = 10'000;

enum MenuId {
    kMenuOpen = 1,
    kMenuCopyLink,
    kMenuPauseResume,
    kMenuSettings,
    kMenuAutostart,
    kMenuExit,
};

std::wstring ComputerName() {
    wchar_t name[256]{};
    DWORD size = static_cast<DWORD>(std::size(name));
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &size)) return L"your PC";
    return name;
}

// The PNG embedded for the phone's home screen.
std::string LoadTouchIcon(HINSTANCE instance) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(IDR_TOUCH_ICON), RT_RCDATA);
    if (!resource) return {};
    HGLOBAL loaded = LoadResource(instance, resource);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) return {};
    return std::string(static_cast<const char*>(data), SizeofResource(instance, resource));
}

bool CopyToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    bool done = false;
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(memory), text.c_str(), bytes);
        GlobalUnlock(memory);
        done = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        if (!done) GlobalFree(memory);
    }
    CloseClipboard();
    return done;
}

}  // namespace

TrayApp::TrayApp() = default;

TrayApp::~TrayApp() {
    capture_.Stop();
    server_.Stop();
    RestoreMute();
}

bool TrayApp::Initialize(HINSTANCE instance, bool startedInBackground) {
    instance_ = instance;
    settings_ = Settings::Load();
    // Undo a mute left behind by a crash.
    RestoreMute();
    if (settings_.token.empty()) {
        settings_.token = GenerateToken();
        settings_.Save();
    }
    touchIcon_ = LoadTouchIcon(instance);
    pcName_ = ComputerName();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kTrayWindowClass;
    if (!RegisterClassExW(&wc)) return false;
    // A hidden top-level window rather than a message-only one: it must
    // receive the TaskbarCreated broadcast, and the installer finds it by class.
    window_ = CreateWindowExW(0, kTrayWindowClass, L"Lazo Audio Link", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!window_) return false;
    server_.SetNotifyWindow(window_, WM_APP_SERVER);

    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();

    mainWindow_.view = [this] { return View(); };
    mainWindow_.onCopyLink = [this] { CopyLink(); };
    mainWindow_.onNewLink = [this] { NewLink(); };
    mainWindow_.onPauseResume = [this] {
        if (state_ == StreamState::Streaming) {
            PauseServer(StreamState::Paused);
        } else {
            StartServer();
        }
    };
    mainWindow_.onSettings = [this] { OpenSettings(); };
    mainWindow_.onClosed = [this] { OnWindowClosed(); };
    if (!mainWindow_.Create(instance)) return false;

    StartServer();
    SetTimer(window_, kTickTimer, kIdleTickMs, nullptr);
    if (!startedInBackground) mainWindow_.Show();
    return true;
}

bool TrayApp::PreTranslate(MSG& message) { return mainWindow_.PreTranslate(message); }

// --- Tray icon ---------------------------------------------------------------

void TrayApp::AddTrayIcon() {
    icon_ = {sizeof(icon_)};
    icon_.hWnd = window_;
    icon_.uID = 1;
    icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon_.uCallbackMessage = WM_APP_TRAY;
    LoadIconMetric(instance_, MAKEINTRESOURCEW(IDI_LAZO), LIM_SMALL, &icon_.hIcon);
    wcscpy_s(icon_.szTip, L"Lazo Audio Link");
    vitool_tray::NotifyIcon(NIM_ADD, &icon_);
    icon_.uVersion = NOTIFYICON_VERSION_4;
    vitool_tray::NotifyIcon(NIM_SETVERSION, &icon_);
    trayTip_.clear();
    UpdateTrayTip();
}

void TrayApp::UpdateTrayTip() {
    std::wstring tip = L"Lazo Audio Link · ";
    switch (state_) {
        case StreamState::Streaming:
            tip += listeners_.empty()          ? L"waiting for a device"
                   : listeners_.size() == 1 ? L"1 device listening"
                                            : std::to_wstring(listeners_.size()) + L" devices listening";
            break;
        case StreamState::Paused:
        case StreamState::Idle: tip += L"paused"; break;
        case StreamState::NoNetwork: tip += L"no local network"; break;
        case StreamState::PortBusy: tip += L"could not start"; break;
    }
    if (tip == trayTip_) return;
    trayTip_ = tip;
    wcsncpy_s(icon_.szTip, tip.c_str(), _TRUNCATE);
    NOTIFYICONDATAW data = icon_;
    data.uFlags = NIF_TIP | NIF_SHOWTIP;
    vitool_tray::NotifyIcon(NIM_MODIFY, &data);
}

void TrayApp::ShowTrayMenu(POINT point) {
    const bool streaming = state_ == StreamState::Streaming;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open Lazo");
    AppendMenuW(menu, MF_STRING | (streaming ? 0 : MF_GRAYED), kMenuCopyLink, L"Copy link");
    AppendMenuW(menu, MF_STRING | (state_ == StreamState::NoNetwork ? MF_GRAYED : 0), kMenuPauseResume,
                streaming ? L"Pause streaming" : L"Resume streaming");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings");
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), kMenuAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit Lazo");
    SetMenuDefaultItem(menu, kMenuOpen, FALSE);

    // Required so the menu closes when clicking elsewhere.
    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, point.x, point.y, window_, nullptr);
    DestroyMenu(menu);
    switch (command) {
        case kMenuOpen: ShowWindow(); break;
        case kMenuCopyLink: CopyLink(); break;
        case kMenuPauseResume:
            if (streaming) {
                PauseServer(StreamState::Paused);
            } else {
                StartServer();
            }
            break;
        case kMenuSettings: OpenSettings(); break;
        case kMenuAutostart: ToggleAutostart(); break;
        case kMenuExit: Exit(); break;
    }
    mainWindow_.Refresh();
}

void TrayApp::Notify(const std::wstring& message) {
    NOTIFYICONDATAW data = icon_;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcscpy_s(data.szInfoTitle, L"Lazo Audio Link");
    wcsncpy_s(data.szInfo, message.c_str(), _TRUNCATE);
    vitool_tray::NotifyIcon(NIM_MODIFY, &data);
}

// --- Streaming ------------------------------------------------------------------

void TrayApp::StartServer() {
    capture_.Stop();
    server_.Stop();
    listeners_.clear();
    const auto address = FindLocalAddress();
    lastNetworkCheck_ = GetTickCount64();
    if (!address) {
        state_ = StreamState::NoNetwork;
    } else {
        address_ = *address;
        ServerOptions options;
        options.address = address_;
        options.port = settings_.port;
        options.token = settings_.token;
        options.pcName = pcName_;
        options.touchIcon = touchIcon_;
        state_ = server_.Start(options) ? StreamState::Streaming : StreamState::PortBusy;
    }
    UpdateMute();
    UpdateTrayTip();
    mainWindow_.Refresh();
}

void TrayApp::PauseServer(StreamState reason) {
    capture_.Stop();
    server_.Stop();
    listeners_.clear();
    state_ = reason;
    UpdateMute();
    UpdateTrayTip();
    mainWindow_.Refresh();
}

void TrayApp::NewLink() {
    if (!listeners_.empty() &&
        MessageBoxW(mainWindow_.IsVisible() ? mainWindow_.window() : window_,
                    L"Devices listening now will stop and need the new code or link. Make a new link?", L"Lazo Audio Link",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    settings_.token = GenerateToken();
    settings_.Save();
    StartServer();
}

void TrayApp::CheckNetworkAndIdle() {
    const ULONGLONG now = GetTickCount64();
    if (state_ == StreamState::Streaming && settings_.idleStopMinutes > 0 && listeners_.empty() &&
        now - server_.LastListening() > static_cast<ULONGLONG>(settings_.idleStopMinutes) * 60'000) {
        PauseServer(StreamState::Idle);
        return;
    }
    if (now - lastNetworkCheck_ < kNetworkCheckMs) return;
    lastNetworkCheck_ = now;
    if (state_ != StreamState::Streaming && state_ != StreamState::NoNetwork) return;
    const auto address = FindLocalAddress();
    // Joined a network, or moved to another one: listen on the new address.
    if ((state_ == StreamState::NoNetwork && address) || (state_ == StreamState::Streaming && (!address || *address != address_))) {
        StartServer();
    }
}

void TrayApp::OnListenersChanged() {
    std::vector<Listener> current = server_.IsRunning() ? server_.Listeners() : std::vector<Listener>{};
    const auto has = [](const std::vector<Listener>& list, uint32_t id) {
        return std::any_of(list.begin(), list.end(), [id](const Listener& listener) { return listener.id == id; });
    };
    if (settings_.notifyConnections) {
        for (const Listener& listener : current) {
            if (!has(listeners_, listener.id)) Notify(listener.device + L" is listening.");
        }
        for (const Listener& listener : listeners_) {
            if (!has(current, listener.id)) Notify(listener.device + L" stopped listening.");
        }
    }
    listeners_ = std::move(current);

    // The output is only recorded while someone listens.
    if (!listeners_.empty() && !capture_.IsRunning()) {
        capture_.Start(
            settings_.outputId, [this](const int16_t* stereo, size_t frames) { server_.Broadcast(stereo, frames); },
            [this](uint32_t rate, size_t framesPerPacket) { server_.SetFormat(rate, framesPerPacket); });
    } else if (listeners_.empty() && capture_.IsRunning()) {
        capture_.Stop();
    }
    UpdateMute();
    UpdateTrayTip();
    SetTimer(window_, kTickTimer, capture_.IsRunning() ? kTickMs : kIdleTickMs, nullptr);
    mainWindow_.Refresh();
}

// The PC's speakers are muted while a phone listens, if asked, and given
// back exactly as they were. Windows keeps sending the sound to the
// loopback when the output is muted.
void TrayApp::UpdateMute() {
    const std::wstring wanted = settings_.muteSpeakers && capture_.IsRunning() ? capture_.DeviceId() : std::wstring();
    if (wanted == settings_.mutedOutputId) return;
    if (!wanted.empty() && IsOutputMuted(wanted)) return;  // muted by the user: leave it alone
    RestoreMute();
    if (!wanted.empty() && SetOutputMuted(wanted, true)) {
        settings_.mutedOutputId = wanted;
        settings_.Save();
    }
}

void TrayApp::RestoreMute() {
    if (settings_.mutedOutputId.empty()) return;
    SetOutputMuted(settings_.mutedOutputId, false);
    settings_.mutedOutputId.clear();
    settings_.Save();
}

ViewState TrayApp::View() const {
    ViewState view;
    view.state = state_;
    if (state_ == StreamState::Streaming) {
        view.url = server_.Url();
        view.address = L"http://" + FormatIPv4(address_) + L":" + std::to_wstring(server_.Port());
        view.listeners = server_.Listeners();
    }
    view.output = capture_.DeviceName();
    if (view.output.empty()) {
        // Not recording right now: show what will be recorded, looked up
        // every few seconds rather than on every repaint.
        const ULONGLONG now = GetTickCount64();
        if (now - idleOutputAt_ > 3000 || idleOutputAt_ == 0) {
            idleOutputAt_ = now;
            std::wstring name = settings_.outputId.empty() ? std::wstring() : OutputName(settings_.outputId);
            if (name.empty()) {
                const std::wstring id = DefaultOutputId();
                if (!id.empty()) name = OutputName(id);
            }
            idleOutputName_ = name;
        }
        view.output = idleOutputName_;
    }
    view.level = capture_.Level();
    view.speakersMuted = !settings_.mutedOutputId.empty();
    view.port = settings_.port;
    view.idleMinutes = settings_.idleStopMinutes;
    return view;
}

// --- Window and commands ---------------------------------------------------------

void TrayApp::ShowWindow() { mainWindow_.Show(); }

void TrayApp::OnWindowClosed() {
    if (closeHintShown_) return;
    closeHintShown_ = true;
    Notify(state_ == StreamState::Streaming ? L"Lazo keeps streaming from the notification area. Exit it from the icon's menu."
                                            : L"Lazo is still in the notification area. Exit it from the icon's menu.");
}

void TrayApp::CopyLink() {
    if (state_ != StreamState::Streaming) return;
    if (!CopyToClipboard(window_, server_.Url())) Notify(L"Couldn't access the clipboard. Try again.");
}

void TrayApp::OpenSettings() {
    ShowSettingsDialog(
        instance_, mainWindow_.IsVisible() ? mainWindow_.window() : nullptr, settings_, EnumerateOutputs(),
        [this](const Settings& settings, bool autostart, std::wstring& error) { return ApplySettings(settings, autostart, error); });
    mainWindow_.Refresh();
}

bool TrayApp::ApplySettings(const Settings& settings, bool autostart, std::wstring& error) {
    if (!IsValidPort(settings.port)) {
        error = L"Choose a port from 1024 to 65535.";
        return false;
    }
    if (autostart != IsAutostartEnabled() && !SetAutostartEnabled(autostart, ExecutablePath().wstring())) {
        error = L"Could not change Start with Windows.";
        return false;
    }
    const bool portChanged = settings.port != settings_.port;
    const bool outputChanged = settings.outputId != settings_.outputId;
    // Only the choices made in the dialog; the link and the mute record stay.
    settings_.port = settings.port;
    settings_.outputId = settings.outputId;
    settings_.idleStopMinutes = settings.idleStopMinutes;
    settings_.muteSpeakers = settings.muteSpeakers;
    settings_.notifyConnections = settings.notifyConnections;
    idleOutputAt_ = 0;
    if (!settings_.Save()) {
        error = L"Could not save the settings.";
        return false;
    }
    if (portChanged && state_ != StreamState::Paused && state_ != StreamState::Idle) {
        StartServer();
    } else if (outputChanged && capture_.IsRunning()) {
        RestoreMute();
        capture_.Stop();
        OnListenersChanged();
    }
    UpdateMute();
    return true;
}

void TrayApp::ToggleAutostart() {
    if (!SetAutostartEnabled(!IsAutostartEnabled(), ExecutablePath().wstring())) Notify(L"Could not change Start with Windows.");
}

void TrayApp::Exit() {
    if (exiting_) return;
    exiting_ = true;
    KillTimer(window_, kTickTimer);
    capture_.Stop();
    server_.Stop();
    RestoreMute();
    mainWindow_.Destroy();
    vitool_tray::NotifyIcon(NIM_DELETE, &icon_);
    if (icon_.hIcon) DestroyIcon(icon_.hIcon);
    DestroyWindow(window_);
}

// --- Window procedure ----------------------------------------------------------

LRESULT CALLBACK TrayApp::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* app = static_cast<TrayApp*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return app ? app->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT TrayApp::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (vitool_tray::HandleMessage(message, wParam)) return 0;
    if (message == taskbarCreatedMessage_ && taskbarCreatedMessage_ != 0) {
        AddTrayIcon();  // Explorer restarted.
        return 0;
    }
    switch (message) {
        case WM_APP_TRAY:
            switch (LOWORD(lParam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK:
                    ShowWindow();
                    break;
                case WM_CONTEXTMENU:
                    ShowTrayMenu({GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)});
                    break;
            }
            return 0;
        case WM_APP_SERVER:
            if (!exiting_) OnListenersChanged();
            return 0;
        case WM_APP_SHOW_WINDOW:
            ShowWindow();
            return 0;
        case WM_TIMER:
            if (wParam == kTickTimer && !exiting_) {
                CheckNetworkAndIdle();
                UpdateMute();
                UpdateTrayTip();
            }
            return 0;
        case WM_CLOSE:  // Sent by the installer before upgrading or uninstalling.
        case WM_ENDSESSION:
            if (message == WM_CLOSE || wParam) Exit();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace lazo
