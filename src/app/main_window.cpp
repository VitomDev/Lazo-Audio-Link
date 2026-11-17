#include "app/main_window.h"

#include "app/resource.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace lazo {

namespace {

constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshMs = 100;

// Layout in 96-DPI pixels.
constexpr int kClientWidth = 440;
constexpr int kMargin = 24;
constexpr int kQrSize = 232;
constexpr int kButtonHeight = 32;
constexpr int kMaxListenerRows = 3;
// The PC's share of the delay: a 10 ms packet plus capture and network.
constexpr int kPcDelayMs = 15;

enum ControlId {
    kCopyLink = 101,
    kNewLink,
    kPauseResume,
    kSettings,
};

void DrawTextIn(HDC dc, HFONT font, COLORREF color, const std::wstring& text, RECT rect, UINT format) {
    HGDIOBJ previous = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_NOPREFIX);
    SelectObject(dc, previous);
}

COLORREF Green(const Theme& theme) { return theme.dark ? RGB(95, 208, 124) : RGB(30, 142, 62); }
COLORREF Red(const Theme& theme) { return theme.dark ? RGB(255, 123, 107) : RGB(196, 43, 28); }

bool IsPhoneOrTablet(const std::wstring& device) {
    return device == L"iPhone" || device == L"iPad" || device.starts_with(L"Android");
}

std::wstring Duration(ULONGLONG milliseconds) {
    const ULONGLONG minutes = milliseconds / 60'000;
    if (minutes < 1) return L"just now";
    if (minutes < 60) return std::to_wstring(minutes) + L" min";
    return std::to_wstring(minutes / 60) + L" h " + std::to_wstring(minutes % 60) + L" min";
}

std::wstring StatusLine(const ViewState& view) {
    switch (view.state) {
        case StreamState::Streaming:
            if (view.listeners.empty()) return L"Waiting for a device";
            return view.listeners.size() == 1 ? L"1 device listening" : std::to_wstring(view.listeners.size()) + L" devices listening";
        case StreamState::Paused: return L"Paused";
        case StreamState::Idle: return L"Paused: nobody was listening";
        case StreamState::NoNetwork: return L"No local network";
        case StreamState::PortBusy: return L"Could not start";
    }
    return {};
}

std::wstring DetailLine(const ViewState& view) {
    switch (view.state) {
        case StreamState::Streaming: return view.address;
        case StreamState::Paused: return L"Devices can't connect until you resume";
        case StreamState::Idle:
            return L"Stopped after " + std::to_wstring(view.idleMinutes) + L" minutes. Resume to listen again.";
        case StreamState::NoNetwork: return L"Lazo starts as soon as the PC is on a network";
        case StreamState::PortBusy:
            return L"Ports " + std::to_wstring(view.port) + L"–" + std::to_wstring(view.port + 9) +
                   L" are in use. Choose another in Settings.";
    }
    return {};
}

}  // namespace

MainWindow::~MainWindow() {
    Destroy();
    for (HGDIOBJ object : std::initializer_list<HGDIOBJ>{background_, titleFont_, headingFont_, textFont_, smallFont_, iconFont_}) {
        if (object) DeleteObject(object);
    }
    if (icon_) DestroyIcon(icon_);
}

bool MainWindow::Create(HINSTANCE instance) {
    instance_ = instance;
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_LAZO));
    // The title bar uses the 16 px image, not a shrunken 32 px one.
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_LAZO), IMAGE_ICON,
                                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kMainWindowClass;
    if (!RegisterClassExW(&windowClass)) return false;

    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, kMainWindowClass, L"Lazo Audio Link",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, CW_USEDEFAULT,
                              CW_USEDEFAULT, 100, 100, nullptr, nullptr, instance, this);
    if (!window_) return false;
    dpi_ = GetDpiForWindow(window_);

    for (const int id : {kCopyLink, kNewLink, kPauseResume, kSettings}) {
        HWND button = CreateWindowExW(0, WC_BUTTONW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                                      window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        EnableHover(button);
    }
    RefreshStyle();
    Resize();
    return true;
}

void MainWindow::Destroy() {
    if (!window_) return;
    KillTimer(window_, kRefreshTimer);
    HWND window = window_;
    window_ = nullptr;
    DestroyWindow(window);
}

void MainWindow::Show() {
    if (!window_) return;
    Refresh();
    if (!IsWindowVisible(window_)) {
        // The first time, in the middle of the screen with the pointer.
        static bool placed = false;
        if (!placed) {
            placed = true;
            POINT cursor;
            GetCursorPos(&cursor);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
            RECT bounds;
            GetWindowRect(window_, &bounds);
            const RECT& work = monitor.rcWork;
            const int width = bounds.right - bounds.left;
            const int height = bounds.bottom - bounds.top;
            SetWindowPos(window_, nullptr, work.left + (work.right - work.left - width) / 2,
                         std::max<int>(work.top, work.top + (work.bottom - work.top - height) / 2), 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ShowWindow(window_, SW_SHOW);
        // No focus outline until the keyboard is used, like native dialogs.
        SendMessageW(window_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    }
    if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
    SetForegroundWindow(window_);
    SetTimer(window_, kRefreshTimer, kRefreshMs, nullptr);
}

void MainWindow::Hide() {
    if (!window_) return;
    KillTimer(window_, kRefreshTimer);
    ShowWindow(window_, SW_HIDE);
}

bool MainWindow::PreTranslate(MSG& message) {
    if (!window_ || !IsWindowVisible(window_) || (message.hwnd != window_ && !IsChild(window_, message.hwnd))) return false;
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
        SendMessageW(window_, WM_CLOSE, 0, 0);
        return true;
    }
    return IsDialogMessageW(window_, &message) != FALSE;
}

void MainWindow::Refresh() {
    if (!window_ || !view) return;
    ViewState next = view();
    const bool layoutChanged = next.listeners.size() != view_.listeners.size();
    const bool textChanged = next.state != view_.state || next.url != view_.url || next.address != view_.address ||
                             next.output != view_.output || next.speakersMuted != view_.speakersMuted;
    bool listenersChanged = layoutChanged;
    for (size_t i = 0; !listenersChanged && i < next.listeners.size(); ++i) {
        const Listener& a = next.listeners[i];
        const Listener& b = view_.listeners[i];
        listenersChanged = a.id != b.id || a.hasStats != b.hasStats || a.stats.bufferMs != b.stats.bufferMs ||
                           a.stats.outputMs != b.stats.outputMs || a.stats.dropouts != b.stats.dropouts;
    }
    const bool levelChanged = std::abs(next.level - view_.level) > 0.004f;
    view_ = std::move(next);

    if (view_.url != qrText_) {
        qrText_ = view_.url;
        qr_ = {};
        if (!qrText_.empty()) {
            if (auto matrix = EncodeQrCode(qrText_)) qr_ = std::move(*matrix);
        }
        InvalidateRect(window_, &qrRect_, FALSE);
    }
    if (textChanged) {
        InvalidateRect(window_, nullptr, FALSE);
        for (const int id : {kCopyLink, kNewLink, kPauseResume}) {
            HWND button = GetDlgItem(window_, id);
            const bool streaming = view_.state == StreamState::Streaming;
            EnableWindow(button, id == kPauseResume ? view_.state != StreamState::NoNetwork : streaming);
            InvalidateRect(button, nullptr, FALSE);
        }
    }
    if (listenersChanged) {
        InvalidateRect(window_, &listenersRect_, FALSE);
        InvalidateRect(window_, &statusRect_, FALSE);
        InvalidateRect(window_, &hintRect_, FALSE);
    }
    if (levelChanged) InvalidateRect(window_, &meterRect_, FALSE);
    // "12 min" ages too, once a minute is plenty.
    static ULONGLONG lastMinute = 0;
    if (GetTickCount64() / 60'000 != lastMinute) {
        lastMinute = GetTickCount64() / 60'000;
        InvalidateRect(window_, &listenersRect_, FALSE);
    }
}

// --- Window -------------------------------------------------------------------

LRESULT CALLBACK MainWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return self && self->window_ ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window_, &paint);
            RECT client;
            GetClientRect(window_, &client);
            // Double-buffered: the meter repaints ten times a second.
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ previous = SelectObject(memory, bitmap);
            Paint(memory);
            BitBlt(dc, paint.rcPaint.left, paint.rcPaint.top, paint.rcPaint.right - paint.rcPaint.left,
                   paint.rcPaint.bottom - paint.rcPaint.top, memory, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
            SelectObject(memory, previous);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window_, nullptr, suggested->left, suggested->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            RefreshStyle();
            Resize();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
                RefreshStyle();
                InvalidateRect(window_, nullptr, FALSE);
            }
            break;
        case WM_DRAWITEM:
            DrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_COMMAND: {
            const auto call = [](const std::function<void()>& callback) {
                if (callback) callback();
            };
            switch (LOWORD(wParam)) {
                case kCopyLink: call(onCopyLink); break;
                case kNewLink: call(onNewLink); break;
                case kPauseResume: call(onPauseResume); break;
                case kSettings: call(onSettings); break;
            }
            Refresh();
            return 0;
        }
        case WM_TIMER:
            if (wParam == kRefreshTimer) Refresh();
            return 0;
        case WM_CLOSE:
            Hide();
            if (onClosed) onClosed();
            return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

void MainWindow::RefreshStyle() {
    theme_ = Theme::Current();
    for (HGDIOBJ object : std::initializer_list<HGDIOBJ>{background_, titleFont_, headingFont_, textFont_, smallFont_, iconFont_}) {
        if (object) DeleteObject(object);
    }
    background_ = CreateSolidBrush(theme_.background);
    titleFont_ = CreateUiFont(dpi_, 16, FW_SEMIBOLD);
    headingFont_ = CreateUiFont(dpi_, 11, FW_SEMIBOLD);
    textFont_ = CreateUiFont(dpi_, 10);
    smallFont_ = CreateUiFont(dpi_, 9);
    iconFont_ = CreateIconFont(dpi_, 11);
    if (icon_) DestroyIcon(icon_);
    icon_ = nullptr;
    LoadIconWithScaleDown(instance_, MAKEINTRESOURCEW(IDI_LAZO), Scale(36), Scale(36), &icon_);
    ApplyWindowStyle(window_, theme_);
}

void MainWindow::Resize() {
    Layout();
    RECT frame{0, 0, Scale(kClientWidth), hintRect_.bottom + Scale(kMargin) - Scale(4)};
    AdjustWindowRectExForDpi(&frame, GetWindowLongW(window_, GWL_STYLE), FALSE, GetWindowLongW(window_, GWL_EXSTYLE), dpi_);
    SetWindowPos(window_, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(window_, nullptr, FALSE);
}

void MainWindow::Layout() {
    const int margin = Scale(kMargin);
    const int width = Scale(kClientWidth);
    const int right = width - margin;
    const int gap = Scale(8);
    const int buttonHeight = Scale(kButtonHeight);
    const auto place = [&](int id, int x, int y, int w, int h) {
        SetWindowPos(GetDlgItem(window_, id), nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    int y = Scale(20);
    headerRect_ = {margin, y, right, y + Scale(40)};
    y = headerRect_.bottom + Scale(20);

    const int qr = Scale(kQrSize);
    qrRect_ = {(width - qr) / 2, y, (width + qr) / 2, y + qr};
    y = qrRect_.bottom + Scale(16);

    statusRect_ = {margin, y, right, y + Scale(46)};
    y = statusRect_.bottom + Scale(8);
    const int half = (right - margin - gap) / 2;
    place(kCopyLink, margin, y, half, buttonHeight);
    place(kNewLink, margin + half + gap, y, right - margin - half - gap, buttonHeight);
    y += buttonHeight + Scale(24);

    soundRect_ = {margin, y, right, y + Scale(44)};
    meterRect_ = {margin, soundRect_.bottom + Scale(2), right, soundRect_.bottom + Scale(8)};
    y = meterRect_.bottom + Scale(20);

    listenersRect_ = {margin, y, right, y + Scale(22) + kMaxListenerRows * Scale(24)};
    y = listenersRect_.bottom + Scale(16);

    const ButtonFonts fonts{textFont_, iconFont_};
    const int pauseWidth = std::max(ButtonWidth(window_, fonts, icons::kPause, L"Pause streaming", dpi_),
                                    ButtonWidth(window_, fonts, icons::kPlay, L"Resume streaming", dpi_));
    const int settingsWidth = ButtonWidth(window_, fonts, icons::kSettings, L"Settings", dpi_);
    place(kPauseResume, margin, y, pauseWidth, buttonHeight);
    place(kSettings, right - settingsWidth, y, settingsWidth, buttonHeight);
    y += buttonHeight + Scale(16);

    hintRect_ = {margin, y, right, y + Scale(36)};
}

void MainWindow::Paint(HDC dc) {
    RECT client;
    GetClientRect(window_, &client);
    FillRect(dc, &client, background_);
    const int margin = Scale(kMargin);
    const int right = client.right - margin;

    // Header: the icon, the name and what Lazo does.
    const int iconSize = Scale(36);
    if (icon_) {
        DrawIconEx(dc, headerRect_.left, headerRect_.top + (headerRect_.bottom - headerRect_.top - iconSize) / 2, icon_, iconSize,
                   iconSize, 0, nullptr, DI_NORMAL);
    }
    const int textLeft = headerRect_.left + iconSize + Scale(12);
    DrawTextIn(dc, titleFont_, theme_.text, L"Lazo", {textLeft, headerRect_.top - Scale(2), right, headerRect_.top + Scale(22)},
               DT_LEFT | DT_SINGLELINE);
    DrawTextIn(dc, smallFont_, theme_.secondaryText, L"Audio Link \u00B7 hear this PC on another device",
               {textLeft, headerRect_.top + Scale(22), right, headerRect_.bottom}, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    PaintQr(dc);

    // Status: a colored dot, a short line and the details, centered under the code.
    const bool streaming = view_.state == StreamState::Streaming;
    const COLORREF dotColor = !streaming ? (view_.state == StreamState::Paused || view_.state == StreamState::Idle
                                                ? theme_.secondaryText
                                                : Red(theme_))
                              : view_.listeners.empty() ? theme_.accent
                                                        : Green(theme_);
    const std::wstring status = StatusLine(view_);
    const int dot = Scale(8);
    const int lineHeight = Scale(22);
    HGDIOBJ previous = SelectObject(dc, headingFont_);
    SIZE statusSize{};
    GetTextExtentPoint32W(dc, status.c_str(), static_cast<int>(status.size()), &statusSize);
    SelectObject(dc, previous);
    const int statusWidth = std::min<int>(dot + Scale(8) + statusSize.cx, right - margin);
    const int statusLeft = (client.right - statusWidth) / 2;
    FillRounded(dc, {statusLeft, statusRect_.top + (lineHeight - dot) / 2, statusLeft + dot, statusRect_.top + (lineHeight + dot) / 2},
                dotColor, dot / 2);
    DrawTextIn(dc, headingFont_, theme_.text, status,
               {statusLeft + dot + Scale(8), statusRect_.top, statusLeft + statusWidth, statusRect_.top + lineHeight},
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawTextIn(dc, smallFont_, theme_.secondaryText, DetailLine(view_),
               {statusRect_.left, statusRect_.top + lineHeight, statusRect_.right, statusRect_.bottom},
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // The sound being sent, with a level meter.
    RECT divider{margin, soundRect_.top - Scale(12), right, soundRect_.top - Scale(11)};
    SetDCBrushColor(dc, theme_.divider);
    FillRect(dc, &divider, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    DrawTextIn(dc, smallFont_, theme_.secondaryText, L"Sound from", {soundRect_.left, soundRect_.top, right, soundRect_.top + Scale(18)},
               DT_LEFT | DT_SINGLELINE);
    const RECT outputLine{soundRect_.left, soundRect_.top + Scale(18), right, soundRect_.bottom - Scale(4)};
    DrawTextIn(dc, iconFont_, theme_.text, view_.appSource ? icons::kApp : icons::kSpeakers, {outputLine.left, outputLine.top, outputLine.left + Scale(20), outputLine.bottom},
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    std::wstring mutedNote = view_.speakersMuted ? L"Muted on this PC" : L"";
    const int noteWidth = mutedNote.empty() ? 0 : TextWidth(window_, smallFont_, mutedNote.c_str()) + Scale(8);
    DrawTextIn(dc, textFont_, theme_.text, view_.output.empty() ? L"No output" : view_.output,
               {outputLine.left + Scale(26), outputLine.top, outputLine.right - noteWidth, outputLine.bottom},
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!mutedNote.empty()) {
        DrawTextIn(dc, smallFont_, theme_.secondaryText, mutedNote, outputLine, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    const int meterHeight = meterRect_.bottom - meterRect_.top;
    FillRounded(dc, meterRect_, theme_.divider, meterHeight / 2);
    const int fill = static_cast<int>((meterRect_.right - meterRect_.left) * std::clamp(view_.level, 0.0f, 1.0f));
    if (fill >= meterHeight) {
        FillRounded(dc, {meterRect_.left, meterRect_.top, meterRect_.left + fill, meterRect_.bottom}, Green(theme_), meterHeight / 2);
    }

    // Who is listening.
    DrawTextIn(dc, smallFont_, theme_.secondaryText, L"Listening",
               {listenersRect_.left, listenersRect_.top, right, listenersRect_.top + Scale(18)}, DT_LEFT | DT_SINGLELINE);
    int y = listenersRect_.top + Scale(22);
    const int rowHeight = Scale(24);
    if (view_.listeners.empty()) {
        DrawTextIn(dc, textFont_, theme_.secondaryText,
                   streaming ? L"Nobody yet. Scan the code or open the link, then press Listen." : L"Nobody",
                   {listenersRect_.left, y, right, y + rowHeight}, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    const ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < view_.listeners.size() && i < kMaxListenerRows; ++i) {
        const Listener& listener = view_.listeners[i];
        if (i == kMaxListenerRows - 1 && view_.listeners.size() > kMaxListenerRows) {
            DrawTextIn(dc, textFont_, theme_.secondaryText, L"+" + std::to_wstring(view_.listeners.size() - i) + L" more",
                       {listenersRect_.left + Scale(26), y, right, y + rowHeight}, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        DrawTextIn(dc, iconFont_, Green(theme_), IsPhoneOrTablet(listener.device) ? icons::kPhone : icons::kLaptop, {listenersRect_.left, y, listenersRect_.left + Scale(20), y + rowHeight},
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        // Right: the delay the device reports, dropouts if any, and for how long.
        std::wstring detail;
        COLORREF detailColor = theme_.secondaryText;
        if (listener.hasStats) {
            detail = L"≈" + std::to_wstring(listener.stats.bufferMs + listener.stats.outputMs + kPcDelayMs) + L" ms · ";
            if (listener.stats.dropouts > 0) {
                detail += std::to_wstring(listener.stats.dropouts) + (listener.stats.dropouts == 1 ? L" dropout" : L" dropouts");
                detailColor = Red(theme_);
            } else {
                detail += Duration(now - listener.since);
            }
        } else {
            detail = Duration(now - listener.since);
        }
        if (listener.saver) detail = L"Data saver · " + detail;
        const int detailWidth = TextWidth(window_, smallFont_, detail.c_str()) + Scale(4);
        DrawTextIn(dc, textFont_, theme_.text, listener.device + L" · " + FormatIPv4(listener.address),
                   {listenersRect_.left + Scale(26), y, right - detailWidth - Scale(8), y + rowHeight},
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextIn(dc, smallFont_, detailColor, detail, {right - detailWidth, y, right, y + rowHeight}, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        y += rowHeight;
    }

    DrawTextIn(dc, smallFont_, theme_.secondaryText,
               view_.listeners.empty() ? L"Open the link on any phone, tablet or computer on the same network as this PC."
                                       : L"Bluetooth headphones add 100–200 ms. iPhones count it in the delay shown; "
                                         L"other devices may not.",
               hintRect_, DT_LEFT | DT_WORDBREAK);
}

void MainWindow::PaintQr(HDC dc) {
    // The QR code on a white card, so phones read it in dark mode too.
    const bool showQr = view_.state == StreamState::Streaming && !qr_.empty();
    FillRounded(dc, qrRect_, showQr ? RGB(255, 255, 255) : theme_.hover, Scale(8));
    if (showQr) {
        const int modules = qr_.size + 2 * kQuietZoneModules;
        const int side = qrRect_.right - qrRect_.left;
        const int module = std::max(1, side / modules);
        const int offset = (side - module * modules) / 2 + module * kQuietZoneModules;
        HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        for (int row = 0; row < qr_.size; ++row) {
            for (int col = 0; col < qr_.size; ++col) {
                if (!qr_.IsDark(col, row)) continue;
                RECT cell{qrRect_.left + offset + col * module, qrRect_.top + offset + row * module, 0, 0};
                cell.right = cell.left + module;
                cell.bottom = cell.top + module;
                FillRect(dc, &cell, black);
            }
        }
        return;
    }
    RECT inner = qrRect_;
    InflateRect(&inner, -Scale(24), 0);
    const wchar_t* message = view_.state == StreamState::Paused || view_.state == StreamState::Idle
                                 ? L"Streaming is paused.\nResume to show the code."
                             : view_.state == StreamState::PortBusy ? L"The port is in use.\nChoose another one in Settings."
                                                                    : L"Connect this PC to the same\nnetwork as the other device.";
    RECT measure = inner;
    DrawTextIn(dc, textFont_, theme_.secondaryText, message, measure, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    const int height = measure.bottom - measure.top;
    inner.top = (qrRect_.top + qrRect_.bottom - height) / 2;
    inner.bottom = inner.top + height;
    DrawTextIn(dc, textFont_, theme_.secondaryText, message, inner, DT_CENTER | DT_WORDBREAK);
}

void MainWindow::DrawItem(const DRAWITEMSTRUCT& item) {
    const ButtonFonts fonts{textFont_, iconFont_};
    const int radius = Scale(4);
    const bool streaming = view_.state == StreamState::Streaming;
    switch (item.CtlID) {
        case kCopyLink:
            DrawButton(item, theme_, fonts, ButtonStyle::Standard, icons::kCopy, L"Copy link", radius);
            break;
        case kNewLink:
            DrawButton(item, theme_, fonts, ButtonStyle::Standard, icons::kRefresh, L"New link", radius);
            break;
        case kPauseResume:
            DrawButton(item, theme_, fonts, streaming ? ButtonStyle::Standard : ButtonStyle::Primary,
                       streaming ? icons::kPause : icons::kPlay,
                       streaming ? L"Pause streaming" : view_.state == StreamState::PortBusy ? L"Try again" : L"Resume streaming",
                       radius);
            break;
        case kSettings:
            DrawButton(item, theme_, fonts, ButtonStyle::Subtle, icons::kSettings, L"Settings", radius);
            break;
    }
}

}  // namespace lazo
