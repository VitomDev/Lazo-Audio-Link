#pragma once

#include "core/audio.h"
#include "core/settings.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace lazo {

// Applies confirmed settings (saves, restarts what changed, sets autostart).
// Returns false with a message when they cannot be used.
using ApplySettings = std::function<bool(const Settings& settings, bool autostart, std::wstring& error)>;

// The small Settings window. Modal; a second request focuses the open one.
// `apps` are offered as single sources when `appsSupported`.
void ShowSettingsDialog(HINSTANCE instance, HWND owner, const Settings& current, const std::vector<OutputDevice>& outputs,
                        const std::vector<AudioApp>& apps, bool appsSupported, const ApplySettings& apply);

}  // namespace lazo
