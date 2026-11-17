#pragma once

#include "app/theme.h"
#include "core/qr.h"
#include "core/server.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace lazo {

inline constexpr wchar_t kMainWindowClass[] = L"LazoAudioLinkWindow";

enum class StreamState {
    Streaming,  // the server runs; phones can listen
    Paused,     // stopped from the window or the tray menu
    Idle,       // stopped after a while with nobody listening
    NoNetwork,  // the PC is on no private network
    PortBusy,   // no free port to listen on
};

// What the window shows, read from the app on every refresh.
struct ViewState {
    StreamState state = StreamState::NoNetwork;
    std::wstring url;      // the phone's link, while streaming
    std::wstring address;  // http://192.168.1.20:8765
    std::vector<Listener> listeners;
    std::wstring output;  // the output whose sound is sent
    float level = 0.0f;
    bool speakersMuted = false;
    int port = 0;
    int idleMinutes = 0;
};

// Lazo's window: the QR code to scan, the link, who is listening and the
// sound being sent. Closing it only hides it.
class MainWindow {
public:
    std::function<ViewState()> view;
    std::function<void()> onCopyLink;
    std::function<void()> onNewLink;
    std::function<void()> onPauseResume;
    std::function<void()> onSettings;
    std::function<void()> onClosed;

    MainWindow() = default;
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(HINSTANCE instance);
    void Destroy();
    void Show();
    void Hide();
    bool IsVisible() const { return window_ && IsWindowVisible(window_); }
    HWND window() const { return window_; }
    // Reads the state again and repaints what changed.
    void Refresh();
    bool PreTranslate(MSG& message);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void RefreshStyle();
    void Resize();
    void Layout();
    void Paint(HDC dc);
    void PaintQr(HDC dc);
    void DrawItem(const DRAWITEMSTRUCT& item);

    int Scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HICON icon_ = nullptr;
    ViewState view_;
    std::wstring qrText_;
    QrMatrix qr_;

    UINT dpi_ = 96;
    Theme theme_{};
    HBRUSH background_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    HFONT textFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT iconFont_ = nullptr;

    // Layout in client pixels, computed by Layout and used by Paint.
    RECT headerRect_{};
    RECT qrRect_{};
    RECT statusRect_{};
    RECT soundRect_{};
    RECT meterRect_{};
    RECT listenersRect_{};
    RECT hintRect_{};
};

}  // namespace lazo
