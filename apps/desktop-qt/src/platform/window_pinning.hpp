#pragma once

class QWidget;

namespace quizpane::platform {

// Qt::WindowStaysOnTopHint 是跨平台意图，原生 API 才是最终窗口层级。
// 每次窗口重新显示或切换 PIN 时调用，防止系统重建 native window 后丢失层级。
void applyNativeWindowPin(QWidget* window, bool pinned);

}  // namespace quizpane::platform
