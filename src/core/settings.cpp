#include "core/settings.h"

#include "core/http.h"
#include "core/text.h"

#include <algorithm>
#include <iterator>

namespace lazo {

namespace {

bool ReadDword(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
}

bool WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
}

bool ReadString(HKEY key, const wchar_t* name, std::wstring& value) {
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size > 4096) return false;
    std::wstring text(size / sizeof(wchar_t) + 1, L'\0');
    size = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, text.data(), &size) != ERROR_SUCCESS) return false;
    text.resize(wcsnlen(text.c_str(), text.size()));
    value = std::move(text);
    return true;
}

bool WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

}  // namespace

bool IsValidPort(int port) { return port >= 1024 && port <= 65535; }

Settings Settings::Load(const wchar_t* keyPath) {
    Settings settings;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return settings;

    DWORD value = 0;
    if (ReadDword(key, L"Port", value) && IsValidPort(static_cast<int>(value))) settings.port = static_cast<int>(value);
    if (ReadDword(key, L"IdleStopMinutes", value) &&
        std::find(std::begin(kIdleChoices), std::end(kIdleChoices), static_cast<int>(value)) != std::end(kIdleChoices)) {
        settings.idleStopMinutes = static_cast<int>(value);
    }
    if (ReadDword(key, L"MuteSpeakers", value)) settings.muteSpeakers = value != 0;
    if (ReadDword(key, L"NotifyConnections", value)) settings.notifyConnections = value != 0;
    ReadString(key, L"Output", settings.outputId);
    ReadString(key, L"MutedOutput", settings.mutedOutputId);
    std::wstring token;
    if (ReadString(key, L"Token", token) && IsValidToken(ToUtf8(token))) settings.token = ToUtf8(token);
    RegCloseKey(key);
    return settings;
}

bool Settings::Save(const wchar_t* keyPath) const {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const bool saved = WriteDword(key, L"Port", static_cast<DWORD>(port)) &&
                       WriteDword(key, L"IdleStopMinutes", static_cast<DWORD>(idleStopMinutes)) &&
                       WriteDword(key, L"MuteSpeakers", muteSpeakers ? 1 : 0) &&
                       WriteDword(key, L"NotifyConnections", notifyConnections ? 1 : 0) &&
                       WriteString(key, L"Output", outputId) && WriteString(key, L"MutedOutput", mutedOutputId) &&
                       WriteString(key, L"Token", FromUtf8(token));
    RegCloseKey(key);
    return saved;
}

std::wstring AutostartCommand(const std::wstring& exePath) { return L"\"" + exePath + L"\" --background"; }

bool IsAutostartEnabled(const wchar_t* valueName) {
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, valueName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    // Task Manager's "Disable" keeps the Run value and flags it here instead:
    // an odd first byte means disabled.
    BYTE state[12]{};
    DWORD size = sizeof(state);
    if (RegGetValueW(HKEY_CURRENT_USER, kStartupApprovedKey, valueName, RRF_RT_REG_BINARY, nullptr, state, &size) ==
            ERROR_SUCCESS &&
        size > 0) {
        return (state[0] & 1) == 0;
    }
    return true;
}

bool SetAutostartEnabled(bool enabled, const std::wstring& exePath, const wchar_t* valueName) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS status;
    if (enabled) {
        // Setting one named value can never create a duplicate entry.
        const std::wstring command = AutostartCommand(exePath);
        status = RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, valueName);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    // Drop any Task Manager override so the Run value alone decides.
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kStartupApprovedKey, valueName);
    return status == ERROR_SUCCESS;
}

}  // namespace lazo
