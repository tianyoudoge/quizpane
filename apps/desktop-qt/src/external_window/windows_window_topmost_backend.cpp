#include "windows_window_topmost_backend.hpp"

#include "quizpane/diagnostic_logger.hpp"

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

QString handleText(HWND window) {
    return QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(window), 0, 16);
}

QString windowClass(HWND window) {
    wchar_t buffer[256]{};
    const int length = GetClassNameW(window, buffer, 256);
    return length > 0 ? QString::fromWCharArray(buffer, length) : QString();
}

QVariantMap windowDetails(HWND window) {
    QVariantMap details;
    details.insert(QStringLiteral("hwnd"), handleText(window));
    details.insert(QStringLiteral("valid"), window != nullptr && IsWindow(window));
    if (window == nullptr || !IsWindow(window)) return details;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    details.insert(QStringLiteral("pid"), static_cast<qulonglong>(processId));
    details.insert(QStringLiteral("class"), windowClass(window));
    details.insert(QStringLiteral("visible"), IsWindowVisible(window) != FALSE);
    details.insert(QStringLiteral("exStyle"),
                   QStringLiteral("0x%1").arg(
                       static_cast<qulonglong>(GetWindowLongPtrW(window, GWL_EXSTYLE)), 0, 16));
    return details;
}

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
    diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-attach-search"),
                      {{QStringLiteral("bindingTokenLength"),
                        static_cast<qlonglong>(request.bindingToken.size())}});
    WindowSearch search{request.bindingToken};
    EnumWindows(&findWindowCallback, reinterpret_cast<LPARAM>(&search));
    if (search.result == nullptr) {
        diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-attach-not-found"));
        return makeResult(request, false, QStringLiteral("没有找到已绑定的 Chrome 或 Edge 课程小窗"));
    }
    window_ = search.result;
    GetWindowThreadProcessId(static_cast<HWND>(window_), &browserProcessId_);
    bindingToken_ = request.bindingToken;
    pinned_ = true;
    visible_ = true;
    diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-attach-target"),
                      windowDetails(static_cast<HWND>(window_)));
    if (!enforceTopmost()) {
        const DWORD systemError = GetLastError();
        diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-attach-failed"),
                          {{QStringLiteral("error"), static_cast<qulonglong>(systemError)}});
        window_ = nullptr;
        browserProcessId_ = 0;
        bindingToken_.clear();
        return makeResult(request, false, QStringLiteral("Windows 拒绝将课程小窗置顶（错误 %1）")
            .arg(systemError));
    }
    startEventTracking();
    diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-attach-success"),
                      windowDetails(static_cast<HWND>(window_)));
    return makeResult(request, true);
}

void WindowsWindowTopmostBackend::detach() {
    diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-detach"),
                      windowDetails(static_cast<HWND>(window_)));
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
    diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-set-pinned"),
                      {{QStringLiteral("pinned"), pinned},
                       {QStringLiteral("hwnd"), handleText(window)}});
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
    diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-set-visible"),
                      {{QStringLiteral("visible"), visible},
                       {QStringLiteral("hwnd"), handleText(window)}});
    enforceTopmost();
}

bool WindowsWindowTopmostBackend::enforceTopmost() {
    if (!pinned_ || enforcing_) {
        diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-topmost-skipped"),
                          {{QStringLiteral("pinned"), pinned_},
                           {QStringLiteral("alreadyEnforcing"), enforcing_}});
        return false;
    }
    enforcing_ = true;
    struct ResetEnforcing final {
        bool& value;
        ~ResetEnforcing() { value = false; }
    } reset{enforcing_};

    HWND window = static_cast<HWND>(window_);
    if ((window == nullptr || !IsWindow(window)) && !reacquireWindow()) {
        diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-topmost-target-lost"));
        return false;
    }
    window = static_cast<HWND>(window_);

    // WS_EX_TOPMOST only records whether this window belongs to the topmost
    // band of the z-order; it says nothing about whether another topmost
    // window (Start menu, volume OSD, etc.) has since been brought above it.
    // GW_HWNDPREV re-walks the live z-order and returns null only when no
    // window currently sits above this one, so it reflects the window's
    // actual on-screen front-most state instead of a sticky style bit.
    const bool actuallyTopmost = GetWindow(window, GW_HWNDPREV) == nullptr;
    const bool windowVisible = IsWindowVisible(window) != FALSE;
    if (actuallyTopmost && windowVisible == visible_) {
        QVariantMap details = windowDetails(window);
        details.insert(QStringLiteral("desiredVisible"), visible_);
        diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-topmost-already"), details);
        return true;
    }

    // Chromium can rewrite a popup's z-order while handling focus and display
    // changes. Reassert only in response to those Windows events; never take
    // keyboard focus from the user.
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
        (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    const BOOL positioned = SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, flags);
    const DWORD systemError = positioned != FALSE ? ERROR_SUCCESS : GetLastError();
    ShowWindowAsync(window, visible_ ? SW_SHOWNOACTIVATE : SW_HIDE);
    QVariantMap details = windowDetails(window);
    details.insert(QStringLiteral("desiredVisible"), visible_);
    details.insert(QStringLiteral("success"), positioned != FALSE);
    details.insert(QStringLiteral("error"), static_cast<qulonglong>(systemError));
    diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-topmost-applied"), details);
    return positioned != FALSE;
}

bool WindowsWindowTopmostBackend::reacquireWindow() {
    if (bindingToken_.isEmpty()) return false;
    WindowSearch search{bindingToken_, static_cast<DWORD>(browserProcessId_)};
    EnumWindows(&findWindowCallback, reinterpret_cast<LPARAM>(&search));
    if (search.result == nullptr) {
        diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-reacquire-not-found"),
                          {{QStringLiteral("browserPid"),
                            static_cast<qulonglong>(browserProcessId_)}});
        return false;
    }
    window_ = search.result;
    GetWindowThreadProcessId(static_cast<HWND>(window_), &browserProcessId_);
    diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-reacquired"),
                      windowDetails(static_cast<HWND>(window_)));
    return true;
}

void WindowsWindowTopmostBackend::startEventTracking() {
    if (foregroundEventHook_ != nullptr || objectEventHook_ != nullptr) return;
    eventBackend = this;
    foregroundEventHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, &windowEventCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
    const DWORD foregroundError = foregroundEventHook_ == nullptr ? GetLastError() : ERROR_SUCCESS;
    objectEventHook_ = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_LOCATIONCHANGE,
        nullptr, &windowEventCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
    const DWORD objectError = objectEventHook_ == nullptr ? GetLastError() : ERROR_SUCCESS;
    diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-event-hooks"),
                      {{QStringLiteral("foregroundHook"), handleText(static_cast<HWND>(foregroundEventHook_))},
                       {QStringLiteral("foregroundError"),
                        static_cast<qulonglong>(foregroundError)},
                       {QStringLiteral("objectHook"), handleText(static_cast<HWND>(objectEventHook_))},
                       {QStringLiteral("objectError"), static_cast<qulonglong>(objectError)}});
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
    diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-event-hooks-stopped"));
}

void WindowsWindowTopmostBackend::handleWindowEvent(unsigned long event, void* rawWindow) {
    if (!pinned_ || enforcing_) return;
    const HWND eventWindow = static_cast<HWND>(rawWindow);
    const HWND currentWindow = static_cast<HWND>(window_);

    if (event == EVENT_OBJECT_DESTROY && eventWindow == currentWindow) {
        diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-event-target-destroyed"),
                          windowDetails(eventWindow));
        window_ = nullptr;
        return;
    }
    if (event == EVENT_SYSTEM_FOREGROUND || eventWindow == currentWindow) {
        diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-event-relevant"),
                          {{QStringLiteral("event"), static_cast<qulonglong>(event)},
                           {QStringLiteral("eventHwnd"), handleText(eventWindow)},
                           {QStringLiteral("targetHwnd"), handleText(currentWindow)}});
        (void)enforceTopmost();
        return;
    }

    // A Chromium popup can be recreated while retaining its browser process.
    // Only search again when that process emits a window event.
    if (currentWindow == nullptr && eventWindow != nullptr) {
        DWORD eventProcessId = 0;
        GetWindowThreadProcessId(eventWindow, &eventProcessId);
        if (eventProcessId == browserProcessId_) {
            diagnostic::event(QStringLiteral("external-window"), QStringLiteral("windows-event-reacquire"),
                              {{QStringLiteral("event"), static_cast<qulonglong>(event)},
                               {QStringLiteral("eventHwnd"), handleText(eventWindow)},
                               {QStringLiteral("browserPid"),
                                static_cast<qulonglong>(browserProcessId_)}});
            (void)enforceTopmost();
        }
    }
}

}  // namespace quizpane::external_window
