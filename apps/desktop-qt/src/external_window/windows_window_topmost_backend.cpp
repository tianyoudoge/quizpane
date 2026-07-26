#include "windows_window_topmost_backend.hpp"

#include <windows.h>

#include <QString>

#include <string>

namespace quizpane::external_window {
namespace {

struct WindowSearch {
    QString token;
    DWORD processId = 0;
    HWND result = nullptr;
};

WindowsWindowTopmostBackend* eventBackend = nullptr;

bool isSupportedBrowserProcess(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) return false;
    wchar_t path[MAX_PATH]{};
    DWORD pathLength = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    const bool queried = QueryFullProcessImageNameW(process, 0, path, &pathLength) != FALSE;
    CloseHandle(process);
    if (!queried) return false;
    const QString executable = QString::fromWCharArray(path, static_cast<qsizetype>(pathLength)).toLower();
    return executable.endsWith(QStringLiteral("\\chrome.exe")) ||
           executable.endsWith(QStringLiteral("\\msedge.exe")) ||
           executable.endsWith(QStringLiteral("\\chromium.exe"));
}

BOOL CALLBACK findWindowCallback(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    const int titleLength = GetWindowTextLengthW(window);
    if (titleLength <= 0 || titleLength > 512) return TRUE;
    std::wstring titleBuffer(static_cast<size_t>(titleLength) + 1, L'\0');
    const int copiedLength = GetWindowTextW(window, titleBuffer.data(), titleLength + 1);
    if (copiedLength <= 0) return TRUE;
    const QString title = QString::fromWCharArray(titleBuffer.data(), copiedLength);
    if (!title.contains(search->token)) return TRUE;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0 || !isSupportedBrowserProcess(processId)) return TRUE;
    if (search->processId != 0 && processId != search->processId) return TRUE;
    search->result = window;
    return FALSE;
}

void CALLBACK windowEventCallback(HWINEVENTHOOK, DWORD event, HWND window,
                                  LONG, LONG, DWORD, DWORD) {
    // The hooks are registered out-of-context on the Qt GUI thread.  The
    // backend owns their lifetime and removes them before it is destroyed.
    if (eventBackend != nullptr) eventBackend->handleWindowEvent(event, window);
}

AttachResult makeResult(const AttachRequest& request, bool success, const QString& error = {}) {
    AttachResult result;
    result.sessionId = request.sessionId;
    result.success = success;
    result.backend = BackendType::WindowsNativeTopmost;
    result.capabilities = {false, false, false, false, false};
    result.error = error;
    return result;
}

}  // namespace

AttachResult WindowsWindowTopmostBackend::attach(const AttachRequest& request) {
    detach();
    WindowSearch search{request.bindingToken};
    EnumWindows(&findWindowCallback, reinterpret_cast<LPARAM>(&search));
    if (search.result == nullptr) {
        return makeResult(request, false, QStringLiteral("没有找到已绑定的 Chrome 或 Edge 课程小窗"));
    }
    window_ = search.result;
    GetWindowThreadProcessId(static_cast<HWND>(window_), &browserProcessId_);
    bindingToken_ = request.bindingToken;
    pinned_ = true;
    visible_ = true;
    if (!enforceTopmost()) {
        const DWORD systemError = GetLastError();
        window_ = nullptr;
        browserProcessId_ = 0;
        bindingToken_.clear();
        return makeResult(request, false, QStringLiteral("Windows 拒绝将课程小窗置顶（错误 %1）")
            .arg(systemError));
    }
    startEventTracking();
    return makeResult(request, true);
}

void WindowsWindowTopmostBackend::detach() {
    stopEventTracking();
    HWND window = static_cast<HWND>(window_);
    if (window != nullptr && IsWindow(window)) {
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    window_ = nullptr;
    browserProcessId_ = 0;
    bindingToken_.clear();
    pinned_ = true;
    visible_ = true;
}

void WindowsWindowTopmostBackend::setPinned(bool pinned) {
    pinned_ = pinned;
    HWND window = static_cast<HWND>(window_);
    if (window == nullptr || !IsWindow(window)) return;
    SetWindowPos(window, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (pinned) startEventTracking();
    else stopEventTracking();
}

void WindowsWindowTopmostBackend::setVisible(bool visible) {
    HWND window = static_cast<HWND>(window_);
    if (window == nullptr || !IsWindow(window)) return;
    visible_ = visible;
    enforceTopmost();
}

bool WindowsWindowTopmostBackend::enforceTopmost() {
    if (!pinned_ || enforcing_) return false;
    enforcing_ = true;
    struct ResetEnforcing final {
        bool& value;
        ~ResetEnforcing() { value = false; }
    } reset{enforcing_};

    HWND window = static_cast<HWND>(window_);
    if ((window == nullptr || !IsWindow(window)) && !reacquireWindow()) return false;
    window = static_cast<HWND>(window_);

    const bool alreadyTopmost =
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (alreadyTopmost && (IsWindowVisible(window) != FALSE) == visible_) return true;

    // Chromium can rewrite a popup's z-order while handling focus and display
    // changes. Reassert only in response to those Windows events; never take
    // keyboard focus from the user.
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
        (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    const BOOL positioned = SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, flags);
    ShowWindowAsync(window, visible_ ? SW_SHOWNOACTIVATE : SW_HIDE);
    return positioned != FALSE;
}

bool WindowsWindowTopmostBackend::reacquireWindow() {
    if (bindingToken_.isEmpty()) return false;
    WindowSearch search{bindingToken_, static_cast<DWORD>(browserProcessId_)};
    EnumWindows(&findWindowCallback, reinterpret_cast<LPARAM>(&search));
    if (search.result == nullptr) return false;
    window_ = search.result;
    GetWindowThreadProcessId(static_cast<HWND>(window_), &browserProcessId_);
    return true;
}

void WindowsWindowTopmostBackend::startEventTracking() {
    if (foregroundEventHook_ != nullptr || objectEventHook_ != nullptr) return;
    eventBackend = this;
    foregroundEventHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, &windowEventCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
    objectEventHook_ = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_LOCATIONCHANGE,
        nullptr, &windowEventCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
}

void WindowsWindowTopmostBackend::stopEventTracking() {
    if (foregroundEventHook_ != nullptr) {
        UnhookWinEvent(static_cast<HWINEVENTHOOK>(foregroundEventHook_));
        foregroundEventHook_ = nullptr;
    }
    if (objectEventHook_ != nullptr) {
        UnhookWinEvent(static_cast<HWINEVENTHOOK>(objectEventHook_));
        objectEventHook_ = nullptr;
    }
    if (eventBackend == this) eventBackend = nullptr;
}

void WindowsWindowTopmostBackend::handleWindowEvent(unsigned long event, void* rawWindow) {
    if (!pinned_ || enforcing_) return;
    const HWND eventWindow = static_cast<HWND>(rawWindow);
    const HWND currentWindow = static_cast<HWND>(window_);

    if (event == EVENT_OBJECT_DESTROY && eventWindow == currentWindow) {
        window_ = nullptr;
        return;
    }
    if (event == EVENT_SYSTEM_FOREGROUND || eventWindow == currentWindow) {
        enforceTopmost();
        return;
    }

    // A Chromium popup can be recreated while retaining its browser process.
    // Only search again when that process emits a window event.
    if (currentWindow == nullptr && eventWindow != nullptr) {
        DWORD eventProcessId = 0;
        GetWindowThreadProcessId(eventWindow, &eventProcessId);
        if (eventProcessId == browserProcessId_) enforceTopmost();
    }
}

}  // namespace quizpane::external_window
