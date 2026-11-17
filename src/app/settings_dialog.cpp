#include "app/settings_dialog.h"

#include "app/resource.h"
#include "app/theme.h"
#include "version.h"

#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>
#include <iterator>

namespace lazo {

namespace {

constexpr wchar_t kClassName[] = L"LazoSettings";

enum ControlId {
    kOutputButton = 100,
    kMuteToggle,
    kPortEdit,
    kIdleButton,
    kNotifyToggle,
    kAutostartToggle,
    kCancel = IDCANCEL,
    kSave = IDOK,
};

// Layout in 96-DPI pixels.
constexpr int kWidth = 480;
constexpr int kPadding = 24;
constexpr int kRowHeight = 40;
constexpr int kFieldWidth = 220;
constexpr int kFieldHeight = 32;
constexpr int kSectionGap = 22;
constexpr int kRadius = 4;

enum class TextStyle { Title, Caption, Label, Hint };

struct TextItem {
    RECT rect;
    std::wstring text;
    TextStyle style;
};

struct State {
    HWND window = nullptr;
    Settings settings;
    std::vector<OutputDevice> outputs;
    ApplySettings apply;
    bool autostart = false;
    std::wstring status;
    bool done = false;

    Theme theme{};
    HBRUSH background = nullptr;
    HBRUSH fieldBrush = nullptr;
    HFONT titleFont = nullptr;
    HFONT captionFont = nullptr;
    HFONT textFont = nullptr;
    HFONT iconFont = nullptr;
    std::vector<TextItem> texts;
    RECT portField{};
    RECT statusRect{};
};

HWND openWindow = nullptr;

int Scale(HWND window, int value) { return MulDiv(value, GetDpiForWindow(window), 96); }

std::wstring OutputLabel(const State& state) {
    if (state.settings.outputId.empty()) return L"Default output";
    for (const auto& output : state.outputs) {
        if (output.id == state.settings.outputId) return output.name;
    }
    return L"Unavailable output";
}

std::wstring IdleLabel(int minutes) {
    if (minutes == 0) return L"Never";
    if (minutes == 60) return L"After 1 hour";
    return L"After " + std::to_wstring(minutes) + L" minutes";
}

// --- Style and layout ------------------------------------------------------------

void RefreshStyle(State& state) {
    state.theme = Theme::Current();
    for (HGDIOBJ object : {static_cast<HGDIOBJ>(state.background), static_cast<HGDIOBJ>(state.fieldBrush),
                           static_cast<HGDIOBJ>(state.titleFont), static_cast<HGDIOBJ>(state.captionFont),
                           static_cast<HGDIOBJ>(state.textFont), static_cast<HGDIOBJ>(state.iconFont)}) {
        if (object) DeleteObject(object);
    }
    state.background = CreateSolidBrush(state.theme.background);
    state.fieldBrush = CreateSolidBrush(state.theme.pressed);
    const UINT dpi = GetDpiForWindow(state.window);
    state.titleFont = CreateUiFont(dpi, 16, FW_SEMIBOLD);
    state.captionFont = CreateUiFont(dpi, 10, FW_SEMIBOLD);
    state.textFont = CreateUiFont(dpi, 10);
    state.iconFont = CreateIconFont(dpi, 9);
    ApplyWindowStyle(state.window, state.theme);
    SendMessageW(GetDlgItem(state.window, kPortEdit), WM_SETFONT, reinterpret_cast<WPARAM>(state.textFont), TRUE);
}

void Layout(State& state) {
    HWND window = state.window;
    const auto scale = [&](int value) { return Scale(window, value); };
    const int width = scale(kWidth);
    const int padding = scale(kPadding);
    const int row = scale(kRowHeight);
    const int fieldWidth = scale(kFieldWidth);
    const int fieldHeight = scale(kFieldHeight);
    const auto place = [&](int id, int x, int y, int w, int h) {
        SetWindowPos(GetDlgItem(window, id), nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    state.texts.clear();
    const auto text = [&](int x, int y, int w, int h, std::wstring value, TextStyle style) {
        state.texts.push_back({{x, y, x + w, y + h}, std::move(value), style});
    };
    const auto labelRow = [&](int y, const wchar_t* label, int controlId, int controlWidth, int controlHeight) {
        text(padding, y, width - 2 * padding - controlWidth - scale(12), row, label, TextStyle::Label);
        place(controlId, width - padding - controlWidth, y + (row - controlHeight) / 2, controlWidth, controlHeight);
    };

    int y = padding;
    text(padding, y, width - 2 * padding, scale(32), L"Settings", TextStyle::Title);
    y += scale(32) + scale(kSectionGap) - scale(6);

    text(padding, y, width - 2 * padding, scale(24), L"Sound", TextStyle::Caption);
    y += scale(24);
    labelRow(y, L"Send the sound of", kOutputButton, fieldWidth, fieldHeight);
    y += row;
    labelRow(y, L"Mute this PC while a device listens", kMuteToggle, scale(44), scale(20));
    y += row + scale(kSectionGap);

    text(padding, y, width - 2 * padding, scale(24), L"Connection", TextStyle::Caption);
    y += scale(24);
    const int portWidth = scale(110);
    text(padding, y, width - 2 * padding - portWidth, row, L"Port", TextStyle::Label);
    state.portField = {width - padding - portWidth, y + (row - fieldHeight) / 2, width - padding, y + (row + fieldHeight) / 2};
    // The edit sits inside a drawn field, vertically centered.
    const int editHeight = scale(20);
    place(kPortEdit, state.portField.left + scale(10), state.portField.top + (fieldHeight - editHeight) / 2,
          portWidth - scale(20), editHeight);
    y += row;
    labelRow(y, L"Pause when nobody listens", kIdleButton, fieldWidth, fieldHeight);
    y += row;
    text(padding, y, width - 2 * padding, scale(20),
         L"Allow Lazo on private networks if Windows Firewall asks.", TextStyle::Hint);
    y += scale(20) + scale(kSectionGap);

    text(padding, y, width - 2 * padding, scale(24), L"General", TextStyle::Caption);
    y += scale(24);
    labelRow(y, L"Notify when a device connects", kNotifyToggle, scale(44), scale(20));
    y += row;
    labelRow(y, L"Start with Windows", kAutostartToggle, scale(44), scale(20));
    y += row + scale(12);

    state.statusRect = {padding, y, width - padding, y + scale(20)};
    y += scale(20) + scale(16);

    const int buttonWidth = scale(96);
    text(padding, y, width - 2 * padding - 2 * buttonWidth - scale(16), fieldHeight,
         L"Lazo Audio Link " LAZO_VERSION_STRING L" · VitomDev", TextStyle::Hint);
    place(kSave, width - padding - buttonWidth, y, buttonWidth, fieldHeight);
    place(kCancel, width - padding - 2 * buttonWidth - scale(8), y, buttonWidth, fieldHeight);
    y += fieldHeight + padding;

    // Grow the window around the client area.
    RECT frame{0, 0, width, y};
    AdjustWindowRectExForDpi(&frame, GetWindowLongW(window, GWL_STYLE), FALSE, GetWindowLongW(window, GWL_EXSTYLE),
                             GetDpiForWindow(window));
    SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(window, nullptr, TRUE);
}

// --- Painting --------------------------------------------------------------------

void Paint(State& state) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(state.window, &paint);
    RECT client;
    GetClientRect(state.window, &client);
    FillRect(dc, &client, state.background);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ previous = SelectObject(dc, state.textFont);
    for (auto& item : state.texts) {
        COLORREF color = state.theme.text;
        HFONT font = state.textFont;
        switch (item.style) {
            case TextStyle::Title: font = state.titleFont; break;
            case TextStyle::Caption: font = state.captionFont; break;
            case TextStyle::Label: break;
            case TextStyle::Hint: color = state.theme.secondaryText; break;
        }
        SelectObject(dc, font);
        SetTextColor(dc, color);
        DrawTextW(dc, item.text.c_str(), -1, &item.rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    // The port field around its edit control.
    const int radius = Scale(state.window, kRadius);
    const bool focused = GetFocus() == GetDlgItem(state.window, kPortEdit);
    FillRounded(dc, state.portField, state.theme.pressed, radius);
    OutlineRounded(dc, state.portField, focused ? state.theme.accent : state.theme.divider, radius, focused ? 2 : 1);
    if (!state.status.empty()) {
        SelectObject(dc, state.textFont);
        SetTextColor(dc, RGB(232, 72, 85));
        DrawTextW(dc, state.status.c_str(), -1, &state.statusRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, previous);
    EndPaint(state.window, &paint);
}

void DrawToggle(State& state, const DRAWITEMSTRUCT& item, bool on) {
    // Switch: a pill with a knob, accent when on.
    HDC dc = item.hDC;
    const RECT& rect = item.rcItem;
    const Theme& theme = state.theme;
    FillRect(dc, &rect, state.background);
    const int height = rect.bottom - rect.top;
    if (on) {
        FillRounded(dc, rect, theme.accent, height / 2);
    } else {
        if (IsHot(item.hwndItem)) FillRounded(dc, rect, theme.hover, height / 2);
        OutlineRounded(dc, rect, theme.secondaryText, height / 2);
    }
    const int knob = Scale(state.window, 12);
    const int knobX = on ? rect.right - Scale(state.window, 4) - knob : rect.left + Scale(state.window, 4);
    RECT knobRect{knobX, rect.top + (height - knob) / 2, knobX + knob, rect.top + (height + knob) / 2};
    FillRounded(dc, knobRect, on ? theme.accentText : theme.secondaryText, knob / 2);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        RECT focus = rect;
        InflateRect(&focus, 2, 2);
        DrawFocusRect(dc, &focus);
    }
}

// Input-like field with a chevron that opens a menu.
void DrawChoice(State& state, const DRAWITEMSTRUCT& item, const std::wstring& label) {
    HDC dc = item.hDC;
    const RECT& rect = item.rcItem;
    const Theme& theme = state.theme;
    const int radius = Scale(state.window, kRadius);
    FillRect(dc, &rect, state.background);
    FillRounded(dc, rect, IsHot(item.hwndItem) ? theme.hover : theme.pressed, radius);
    OutlineRounded(dc, rect, theme.divider, radius);
    SetBkMode(dc, TRANSPARENT);
    RECT inner{rect.left + Scale(state.window, 12), rect.top, rect.right - Scale(state.window, 12), rect.bottom};
    HGDIOBJ previous = SelectObject(dc, state.iconFont);
    SetTextColor(dc, theme.secondaryText);
    DrawTextW(dc, icons::kChevronDown, -1, &inner, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    inner.right -= Scale(state.window, 20);
    SelectObject(dc, state.textFont);
    SetTextColor(dc, theme.text);
    DrawTextW(dc, label.c_str(), -1, &inner, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, previous);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        RECT focus = rect;
        InflateRect(&focus, -2, -2);
        DrawFocusRect(dc, &focus);
    }
}

void DrawControl(State& state, const DRAWITEMSTRUCT& item) {
    const ButtonFonts fonts{state.textFont, state.iconFont};
    const int radius = Scale(state.window, kRadius);
    switch (item.CtlID) {
        case kOutputButton: DrawChoice(state, item, OutputLabel(state)); break;
        case kIdleButton: DrawChoice(state, item, IdleLabel(state.settings.idleStopMinutes)); break;
        case kMuteToggle: DrawToggle(state, item, state.settings.muteSpeakers); break;
        case kNotifyToggle: DrawToggle(state, item, state.settings.notifyConnections); break;
        case kAutostartToggle: DrawToggle(state, item, state.autostart); break;
        case kSave: DrawButton(item, state.theme, fonts, ButtonStyle::Primary, nullptr, L"Save", radius); break;
        case kCancel: DrawButton(item, state.theme, fonts, ButtonStyle::Standard, nullptr, L"Cancel", radius); break;
    }
}

void SetStatus(State& state, const std::wstring& status) {
    state.status = status;
    InvalidateRect(state.window, &state.statusRect, TRUE);
}

// --- Actions ---------------------------------------------------------------------

void Save(State& state) {
    wchar_t text[16]{};
    GetWindowTextW(GetDlgItem(state.window, kPortEdit), text, static_cast<int>(std::size(text)));
    state.settings.port = _wtoi(text);
    std::wstring error;
    if (!state.apply(state.settings, state.autostart, error)) {
        SetStatus(state, error);
        return;
    }
    state.done = true;
}

// A menu under the field; returns the chosen index or -1.
int ChooseFromMenu(State& state, int fieldId, const std::vector<std::wstring>& labels, int checked) {
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < labels.size(); ++i) {
        AppendMenuW(menu, MF_STRING | (static_cast<int>(i) == checked ? MF_CHECKED : 0), i + 1, labels[i].c_str());
    }
    RECT field;
    GetWindowRect(GetDlgItem(state.window, fieldId), &field);
    const int chosen = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, field.left, field.bottom,
                                        state.window, nullptr);
    DestroyMenu(menu);
    return chosen - 1;
}

void ChooseOutput(State& state) {
    std::vector<std::wstring> labels{L"Default output (follows Windows)"};
    int checked = state.settings.outputId.empty() ? 0 : -1;
    for (size_t i = 0; i < state.outputs.size(); ++i) {
        labels.push_back(state.outputs[i].name);
        if (state.outputs[i].id == state.settings.outputId) checked = static_cast<int>(i) + 1;
    }
    const int chosen = ChooseFromMenu(state, kOutputButton, labels, checked);
    if (chosen < 0) return;
    state.settings.outputId = chosen == 0 ? std::wstring() : state.outputs[static_cast<size_t>(chosen) - 1].id;
    InvalidateRect(GetDlgItem(state.window, kOutputButton), nullptr, FALSE);
}

void ChooseIdle(State& state) {
    std::vector<std::wstring> labels;
    int checked = -1;
    for (size_t i = 0; i < std::size(kIdleChoices); ++i) {
        labels.push_back(IdleLabel(kIdleChoices[i]));
        if (kIdleChoices[i] == state.settings.idleStopMinutes) checked = static_cast<int>(i);
    }
    const int chosen = ChooseFromMenu(state, kIdleButton, labels, checked);
    if (chosen < 0) return;
    state.settings.idleStopMinutes = kIdleChoices[chosen];
    InvalidateRect(GetDlgItem(state.window, kIdleButton), nullptr, FALSE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    }
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!state) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const auto toggle = [&](bool& value) {
                value = !value;
                InvalidateRect(GetDlgItem(window, id), nullptr, FALSE);
            };
            switch (id) {
                case kCancel: state->done = true; break;
                case kSave: Save(*state); break;
                case kOutputButton: ChooseOutput(*state); break;
                case kIdleButton: ChooseIdle(*state); break;
                case kMuteToggle: toggle(state->settings.muteSpeakers); break;
                case kNotifyToggle: toggle(state->settings.notifyConnections); break;
                case kAutostartToggle: toggle(state->autostart); break;
                case kPortEdit:
                    if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) {
                        RECT field = state->portField;
                        InflateRect(&field, 2, 2);
                        InvalidateRect(window, &field, FALSE);
                    }
                    break;
            }
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, state->theme.text);
            SetBkColor(dc, state->theme.pressed);
            return reinterpret_cast<LRESULT>(state->fieldBrush);
        }
        case WM_DRAWITEM:
            DrawControl(*state, *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_PAINT:
            Paint(*state);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETTINGCHANGE:
            RefreshStyle(*state);
            Layout(*state);
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            RefreshStyle(*state);
            Layout(*state);
            return 0;
        }
        case WM_CLOSE:
            state->done = true;
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

void ShowSettingsDialog(HINSTANCE instance, HWND owner, const Settings& current, const std::vector<OutputDevice>& outputs,
                        const ApplySettings& apply) {
    if (openWindow) {
        SetForegroundWindow(openWindow);
        return;
    }
    static const ATOM windowClass = [instance] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_LAZO));
        wc.lpszClassName = kClassName;
        return RegisterClassExW(&wc);
    }();
    if (!windowClass) return;

    State state;
    state.settings = current;
    state.outputs = outputs;
    state.apply = apply;
    state.autostart = IsAutostartEnabled();

    // Over the main window when it is open, otherwise where the mouse is.
    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR monitorHandle = owner ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(monitorHandle, &monitor);
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT | (owner ? 0 : WS_EX_APPWINDOW), kClassName, L"Lazo Settings",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, monitor.rcWork.left,
                                  monitor.rcWork.top, 100, 100, owner, nullptr, instance, &state);
    if (!window) return;
    state.window = window;
    openWindow = window;
    if (owner) EnableWindow(owner, FALSE);

    const auto add = [&](int id) {
        HWND control = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                                       window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        EnableHover(control);
        return control;
    };
    add(kOutputButton);
    add(kMuteToggle);
    HWND port = CreateWindowExW(0, L"EDIT", std::to_wstring(current.port).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPortEdit)), instance, nullptr);
    SendMessageW(port, EM_LIMITTEXT, 5, 0);
    add(kIdleButton);
    add(kNotifyToggle);
    add(kAutostartToggle);
    add(kCancel);
    add(kSave);

    RefreshStyle(state);
    Layout(state);
    RECT bounds;
    GetWindowRect(window, &bounds);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    RECT center = monitor.rcWork;
    if (owner) GetWindowRect(owner, &center);
    const RECT& work = monitor.rcWork;
    const int x = std::clamp<int>((center.left + center.right - width) / 2, work.left, std::max<int>(work.left, work.right - width));
    const int y = std::clamp<int>((center.top + center.bottom - height) / 2, work.top, std::max<int>(work.top, work.bottom - height));
    SetWindowPos(window, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    // No focus outline until the keyboard is used, like native dialogs.
    SendMessageW(window, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);

    // Modal loop; the window also closes if Lazo exits meanwhile.
    MSG message;
    while (!state.done && IsWindow(window)) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    openWindow = nullptr;
    if (owner) EnableWindow(owner, TRUE);
    if (IsWindow(window)) DestroyWindow(window);
    if (owner) SetForegroundWindow(owner);
    for (HGDIOBJ object : {static_cast<HGDIOBJ>(state.background), static_cast<HGDIOBJ>(state.fieldBrush),
                           static_cast<HGDIOBJ>(state.titleFont), static_cast<HGDIOBJ>(state.captionFont),
                           static_cast<HGDIOBJ>(state.textFont), static_cast<HGDIOBJ>(state.iconFont)}) {
        if (object) DeleteObject(object);
    }
}

}  // namespace lazo
