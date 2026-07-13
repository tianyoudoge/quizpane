#include "window_pinning.hpp"

#include <QWidget>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace quizpane::platform {

void applyNativeWindowPin(QWidget* window, bool pinned) {
    if (!window) return;
#if defined(Q_OS_WIN)
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    SetWindowPos(handle, pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
#else
    // X11/Wayland 继续由 Qt::WindowStaysOnTopHint 交给窗口管理器处理。
    Q_UNUSED(pinned);
#endif
}

}  // namespace quizpane::platform
