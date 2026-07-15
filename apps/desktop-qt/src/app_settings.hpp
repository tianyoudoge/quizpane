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
inline constexpr int kHeaderHeight = 28;
inline constexpr int kControlBarHeight = 30;
inline constexpr int kMinimumPracticeViewportHeight = 120;
inline constexpr int kSubmitBubbleInset = 7;
}
}  // namespace quizpane
