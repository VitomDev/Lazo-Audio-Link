#pragma once

#include <windows.h>

#include <string>

namespace lazo {

// Registry locations. The installer and the tests rely on these names.
inline constexpr wchar_t kSettingsKey[] = L"Software\\Lazo Audio Link";
inline constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kRunValue[] = L"Lazo Audio Link";
inline constexpr wchar_t kStartupApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

inline constexpr int kDefaultPort = 8765;
inline constexpr int kIdleChoices[] = {0, 15, 30, 60};

struct Settings {
    int port = kDefaultPort;
    // The output whose sound is sent; empty follows the Windows default.
    std::wstring outputId;
    // Set: only this application's sound is sent ("chrome.exe"), whatever
    // output it plays on. The name is shown while it is closed.
    std::wstring appExe;
    std::wstring appName;
    // The link uses the PC's name (lazo-<pc>.local) instead of its IP, so a
    // saved link keeps working when the router gives the PC another IP.
    bool stableLink = true;
    // Stop streaming after this long with nobody listening; 0 never stops.
    int idleStopMinutes = 0;
    bool muteSpeakers = false;
    bool notifyConnections = true;
    // The secret part of the phone's link. It stays the same so the page can
    // live on the phone's home screen; "New link" replaces it.
    std::string token;
    // The output Lazo muted, kept while it is muted so that a crash can be
    // undone on the next start.
    std::wstring mutedOutputId;

    // `keyPath` is relative to HKEY_CURRENT_USER; tests use a scratch key.
    static Settings Load(const wchar_t* keyPath = kSettingsKey);
    bool Save(const wchar_t* keyPath = kSettingsKey) const;
};

bool IsValidPort(int port);

// "Start with Windows" is the presence of Lazo's value in the per-user Run
// key, so the menu always shows the real state. It is off until turned on.
bool IsAutostartEnabled(const wchar_t* valueName = kRunValue);
bool SetAutostartEnabled(bool enabled, const std::wstring& exePath, const wchar_t* valueName = kRunValue);

std::wstring AutostartCommand(const std::wstring& exePath);

}  // namespace lazo
