#include "app_settings.hpp"

#include <QSettings>

namespace quizpane {
namespace keys {
constexpr auto kPinned = "window/pinned";
constexpr auto kUiSize = "ui/size";
constexpr auto kColorTheme = "ui/colorTheme";
constexpr auto kBossKey = "bossKey/sequence";
constexpr auto kAutoAdvance = "practice/autoAdvanceMs";
constexpr auto kLastProvider = "provider/lastLibraryPath";
constexpr auto kPendingDelete = "providers/pendingDelete";
}
bool AppSettings::windowPinned() { return QSettings().value(keys::kPinned, true).toBool(); }
void AppSettings::setWindowPinned(bool value) { QSettings().setValue(keys::kPinned, value); }
QString AppSettings::uiSize() { return QSettings().value(keys::kUiSize, QStringLiteral("medium")).toString(); }
void AppSettings::setUiSize(const QString& value) { QSettings().setValue(keys::kUiSize, value); }
QString AppSettings::colorTheme() {
    const QString value = QSettings().value(keys::kColorTheme, QStringLiteral("dark")).toString();
    return value == QStringLiteral("light") ? value : QStringLiteral("dark");
}
void AppSettings::setColorTheme(const QString& value) {
    QSettings().setValue(keys::kColorTheme,
                         value == QStringLiteral("light") ? value : QStringLiteral("dark"));
}
QString AppSettings::bossKey() { return QSettings().value(keys::kBossKey, QStringLiteral("Ctrl+Shift+H")).toString(); }
void AppSettings::setBossKey(const QString& value) { QSettings().setValue(keys::kBossKey, value); }
int AppSettings::autoAdvanceMs() { return qBound(0, QSettings().value(keys::kAutoAdvance, 700).toInt(), 10000); }
QString AppSettings::lastProviderPath() { return QSettings().value(keys::kLastProvider).toString(); }
void AppSettings::setLastProviderPath(const QString& value) { QSettings().setValue(keys::kLastProvider, value); }
void AppSettings::clearLastProviderPath() { QSettings().remove(keys::kLastProvider); }
QStringList AppSettings::pendingProviderDeletions() { return QSettings().value(keys::kPendingDelete).toStringList(); }
void AppSettings::setPendingProviderDeletions(const QStringList& value) { QSettings().setValue(keys::kPendingDelete, value); }
}  // namespace quizpane
