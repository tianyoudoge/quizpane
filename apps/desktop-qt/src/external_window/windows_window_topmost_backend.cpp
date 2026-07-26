#include "windows_window_topmost_backend.hpp"

#include <windows.h>

#include <QString>

#include <string>

namespace quizpane::external_window {
namespace {

struct WindowSearch {
    QString token;
    HWND result = nullptr;
};

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
    search->result = window;
    return FALSE;
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
    if (!SetWindowPos(static_cast<HWND>(window_), HWND_TOPMOST, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        const DWORD systemError = GetLastError();
        window_ = nullptr;
        return makeResult(request, false, QStringLiteral("Windows 拒绝将课程小窗置顶（错误 %1）")
            .arg(systemError));
    }
    return makeResult(request, true);
}

void WindowsWindowTopmostBackend::detach() {
    HWND window = static_cast<HWND>(window_);
    if (window != nullptr && IsWindow(window)) {
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    window_ = nullptr;
}

void WindowsWindowTopmostBackend::setPinned(bool pinned) {
    HWND window = static_cast<HWND>(window_);
    if (window == nullptr || !IsWindow(window)) return;
    SetWindowPos(window, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void WindowsWindowTopmostBackend::setVisible(bool visible) {
    HWND window = static_cast<HWND>(window_);
    if (window == nullptr || !IsWindow(window)) return;
    ShowWindow(window, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

}  // namespace quizpane::external_window
