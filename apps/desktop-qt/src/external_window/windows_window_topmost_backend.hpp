#pragma once

#include "external_window_types.hpp"

#include <QString>

namespace quizpane::external_window {

// Windows 能直接控制 Chromium 原生顶层窗口，因此不复制画面，也不改变视频
// 解码与音频所属进程。window_ 保存 HWND，刻意不在公共头文件引入 windows.h。
class WindowsWindowTopmostBackend final {
public:
    AttachResult attach(const AttachRequest& request);
    void detach();
    void setVisible(bool visible);
    [[nodiscard]] bool enforceTopmost();
    // Called by the private WinEvent callback registered for the attached
    // browser popup. It is public only to keep windows.h out of this header.
    void handleWindowEvent(unsigned long event, void* window, long objectId, long childId);

private:
    [[nodiscard]] bool reacquireWindow();
    void startEventTracking();
    void stopEventTracking();

    void* window_ = nullptr;
    void* foregroundEventHook_ = nullptr;
    void* objectEventHook_ = nullptr;
    unsigned long browserProcessId_ = 0;
    QString bindingToken_;
    bool visible_ = true;
    bool enforcing_ = false;
};

}  // namespace quizpane::external_window
