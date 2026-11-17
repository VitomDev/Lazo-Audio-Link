#include "app/theme.h"
#include "app/tray_app.h"

#include <commctrl.h>
#include <objbase.h>

#include <cwchar>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    // "--background" is used by the Start with Windows entry and by silent
    // upgrades: start in the notification area without opening the window.
    const bool startedInBackground = wcsstr(commandLine, L"--background") != nullptr;

    // One instance per user session. A second launch opens the running one's
    // window.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LazoAudioLink.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND running = FindWindowW(lazo::kTrayWindowClass, nullptr); running && !startedInBackground) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(running, lazo::WM_APP_SHOW_WINDOW, 0, 0);
        }
        return 0;
    }

    // Core Audio (output names, the mute switch) is COM.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    lazo::EnableDarkMenus();

    int exitCode = 0;
    {
        lazo::TrayApp app;
        if (app.Initialize(instance, startedInBackground)) {
            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (app.PreTranslate(message)) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        } else {
            exitCode = 1;
        }
    }
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return exitCode;
}
