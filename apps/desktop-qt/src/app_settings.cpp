#include "app_settings.hpp"

#include <QSettings>

namespace quizpane {
namespace keys {
constexpr auto kPinned = "window/pinned";
constexpr auto kUiSize = "ui/size";
constexpr auto kBackgroundVisibility = "ui/backgroundVisibility";
constexpr auto kColorTheme = "ui/colorTheme";
constexpr auto kBossKey = "bossKey/sequence";
constexpr auto kAutoAdvance = "practice/autoAdvanceMs";
constexpr auto kLastProvider = "provider/lastLibraryPath";
constexpr auto kPendingDelete = "providers/pendingDelete";
}
namespace {
// 每次调用方法都新建 QSettings 在 Windows 上意味着每次都 RegOpenKeyEx；这里
// 复用同一个实例，因为默认构造只依赖已在 main() 里设置好的组织名/应用名，且
// AppSettings 只从 UI 主线程访问。
QSettings& settings() {
    static QSettings instance;
    return instance;
}
}
bool AppSettings::windowPinned() { return settings().value(keys::kPinned, true).toBool(); }
void AppSettings::setWindowPinned(bool value) { settings().setValue(keys::kPinned, value); }
QString AppSettings::uiSize() { return settings().value(keys::kUiSize, QStringLiteral("medium")).toString(); }
void AppSettings::setUiSize(const QString& value) { settings().setValue(keys::kUiSize, value); }
int AppSettings::backgroundVisibility() {
    return qBound(0, settings().value(keys::kBackgroundVisibility, 100).toInt(), 100);
}
void AppSettings::setBackgroundVisibility(int value) {
    settings().setValue(keys::kBackgroundVisibility, qBound(0, value, 100));
}
QString AppSettings::colorTheme() {
    const QString value = settings().value(keys::kColorTheme, QStringLiteral("dark")).toString();
    return value == QStringLiteral("light") ? value : QStringLiteral("dark");
}
void AppSettings::setColorTheme(const QString& value) {
    settings().setValue(keys::kColorTheme,
                        value == QStringLiteral("light") ? value : QStringLiteral("dark"));
}
QString AppSettings::bossKey() { return settings().value(keys::kBossKey, QStringLiteral("Ctrl+H")).toString(); }
void AppSettings::setBossKey(const QString& value) { settings().setValue(keys::kBossKey, value); }
int AppSettings::autoAdvanceMs() { return qBound(0, settings().value(keys::kAutoAdvance, 700).toInt(), 10000); }
QString AppSettings::lastProviderPath() { return settings().value(keys::kLastProvider).toString(); }
void AppSettings::setLastProviderPath(const QString& value) { settings().setValue(keys::kLastProvider, value); }
void AppSettings::clearLastProviderPath() { settings().remove(keys::kLastProvider); }
QStringList AppSettings::pendingProviderDeletions() { return settings().value(keys::kPendingDelete).toStringList(); }
void AppSettings::setPendingProviderDeletions(const QStringList& value) { settings().setValue(keys::kPendingDelete, value); }
}  // namespace quizpane
