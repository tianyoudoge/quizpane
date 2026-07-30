#pragma once

#include <QString>
#include <QVariantMap>

namespace quizpane::diagnostic {

// DEBUG 包调用 initialize() 后，Qt 的 qInfo/qWarning/qCritical 会同时写入滚动日志。
// Release 构建中这些函数是空操作，不创建文件，也不改变 Qt 的消息处理器。
// initialize 用 QUIZPANE_DIAGNOSTIC_LOGGING 编译期宏切换两套实现
// （见 .cpp 里的 #ifdef/#else），而不是运行时 if，这样 Release
// 二进制里连日志/崩溃处理相关代码都不会被链接进去。
bool initialize(const QString& component);
// 结构化的一行日志：area 是模块名（如 "mac-source-window"），
// name 是事件名，fields 是附加的 key=value 集合，最终格式化
// 成一行文本写入日志文件。
void event(const QString& area, const QString& name, const QVariantMap& fields = {});
// 记录一段较长的原始内容（如 Provider 返回的完整 JSON）。只
// 在 QUIZPANE_VERBOSE_DIAGNOSTICS 打开时才真正写入，避免默认
// 诊断包体积暴涨或意外把敏感 payload 落盘。
void payload(const QString& area, const QString& name, const QString& label,
             const QString& content, qsizetype maximumCharacters = 32768);
void shutdown();
QString logFilePath();
QString crashArtifactPath();
bool openLogFile();

} // namespace quizpane::diagnostic
