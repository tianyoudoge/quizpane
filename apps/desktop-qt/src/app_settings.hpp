#pragma once

#include <QString>
#include <QStringList>

namespace quizpane {

class AppSettings final {
public:
    static bool windowPinned();
    static void setWindowPinned(bool value);
    static QString uiSize();
    static void setUiSize(const QString& value);
    // 0 表示纯文字，100 保留主题的默认卡片背景强度。
    static int backgroundVisibility();
    static void setBackgroundVisibility(int value);
    // "dark" / "light"。主题是跨平台 UI 偏好，不依赖各系统的默认控件颜色，
    // 这样 Windows、macOS 和 Linux 都会得到相同的可读性。
    static QString colorTheme();
    static void setColorTheme(const QString& value);
    static QString bossKey();
    static void setBossKey(const QString& value);
    static int autoAdvanceMs();
    static QString lastProviderPath();
    static void setLastProviderPath(const QString& value);
    static void clearLastProviderPath();
    static QStringList pendingProviderDeletions();
    static void setPendingProviderDeletions(const QStringList& value);
};

namespace layout_metrics {
inline constexpr int kWindowMargin = 14;
inline constexpr int kControlSpacing = 7;
inline constexpr int kIconPixels = 15;
inline constexpr int kIconButtonPixels = 26;
inline constexpr int kNavIconPixels = 19;
inline constexpr int kNavIconButtonPixels = 34;
inline constexpr int kHeaderHeight = 28;
inline constexpr int kControlBarHeight = 38;
inline constexpr int kMinimumPracticeViewportHeight = 120;
inline constexpr int kSubmitBubbleInset = 7;
}
}  // namespace quizpane
