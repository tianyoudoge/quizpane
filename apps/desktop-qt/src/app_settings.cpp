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
constexpr auto kAiApiKey = "ai/apiKey";
constexpr auto kAiProvider = "ai/provider";
constexpr auto kAiBaseUrl = "ai/baseUrl";
constexpr auto kAiModel = "ai/model";
constexpr auto kAiTotalRequests = "ai/stats/totalRequests";
constexpr auto kAiTotalPromptTokens = "ai/stats/promptTokens";
constexpr auto kAiTotalCompletionTokens = "ai/stats/completionTokens";
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
QString AppSettings::aiApiKey() { return settings().value(keys::kAiApiKey).toString(); }
void AppSettings::setAiApiKey(const QString& value) { settings().setValue(keys::kAiApiKey, value); }
QString AppSettings::aiProvider() { return settings().value(keys::kAiProvider, QStringLiteral("deepseek")).toString(); }
void AppSettings::setAiProvider(const QString& v) { settings().setValue(keys::kAiProvider, v); }
QString AppSettings::aiBaseUrl() { return settings().value(keys::kAiBaseUrl).toString(); }
void AppSettings::setAiBaseUrl(const QString& v) { settings().setValue(keys::kAiBaseUrl, v); }
QString AppSettings::aiModel() { return settings().value(keys::kAiModel).toString(); }
void AppSettings::setAiModel(const QString& v) { settings().setValue(keys::kAiModel, v); }
int AppSettings::aiTotalRequests() { return settings().value(keys::kAiTotalRequests, 0).toInt(); }
void AppSettings::incrementAiTotalRequests() { settings().setValue(keys::kAiTotalRequests, aiTotalRequests() + 1); }
qint64 AppSettings::aiTotalPromptTokens() { return settings().value(keys::kAiTotalPromptTokens, 0).toLongLong(); }
void AppSettings::addAiPromptTokens(qint64 t) { settings().setValue(keys::kAiTotalPromptTokens, aiTotalPromptTokens() + t); }
qint64 AppSettings::aiTotalCompletionTokens() { return settings().value(keys::kAiTotalCompletionTokens, 0).toLongLong(); }
void AppSettings::addAiCompletionTokens(qint64 t) { settings().setValue(keys::kAiTotalCompletionTokens, aiTotalCompletionTokens() + t); }
QStringList AppSettings::pendingProviderDeletions() { return settings().value(keys::kPendingDelete).toStringList(); }
void AppSettings::setPendingProviderDeletions(const QStringList& value) { settings().setValue(keys::kPendingDelete, value); }
}  // namespace quizpane
