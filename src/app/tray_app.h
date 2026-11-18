#pragma once

#include "app/main_window.h"
#include "core/audio.h"
#include "core/mdns.h"
#include "core/server.h"
#include "core/settings.h"

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace lazo {

inline constexpr wchar_t kTrayWindowClass[] = L"LazoAudioLinkTray";
inline constexpr UINT WM_APP_SHOW_WINDOW = WM_APP + 12;

// The application: the server and the capture, the tray icon and the main
// window. Closing the window keeps Lazo streaming from the notification
// area; Exit in the tray menu ends it.
class TrayApp {
public:
    TrayApp();
    ~TrayApp();
    bool Initialize(HINSTANCE instance, bool startedInBackground);
    bool PreTranslate(MSG& message);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void AddTrayIcon();
    void UpdateTrayTip();
    // The icon shows a green dot while a device listens.
    void UpdateTrayIcon();
    void ShowTrayMenu(POINT point);
    void Notify(const std::wstring& message);

    void StartServer();
    void PauseServer(StreamState reason);
    void NewLink();
    void CheckNetworkAndIdle();
    void OnListenersChanged();
    // Asks the PC's user about each page that wants the new link.
    void AnswerLinkRequests();
    void UpdateMute();
    void RestoreMute();

    void ShowWindow();
    void OnWindowClosed();
    void CopyLink();
    void OpenSettings();
    bool ApplySettings(const Settings& settings, bool autostart, std::wstring& error);
    void ToggleAutostart();
    void Exit();

    ViewState View() const;
    CaptureSource Source() const;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    NOTIFYICONDATAW icon_{};
    bool exiting_ = false;
    bool closeHintShown_ = false;
    bool askingAboutLink_ = false;

    Settings settings_;
    Server server_;
    Capture capture_;
    MdnsResponder mdns_;
    std::string hostLabel_;  // lazo-<pc>, the stable link's name
    HICON idleIcon_ = nullptr;
    HICON liveIcon_ = nullptr;
    bool showingLive_ = false;
    MainWindow mainWindow_;
    std::string touchIcon_;
    std::wstring pcName_;

    StreamState state_ = StreamState::NoNetwork;
    uint32_t address_ = 0;
    ULONGLONG lastNetworkCheck_ = 0;
    std::vector<Listener> listeners_;
    std::wstring trayTip_;
    mutable std::wstring idleOutputName_;
    mutable ULONGLONG idleOutputAt_ = 0;
};

}  // namespace lazo
