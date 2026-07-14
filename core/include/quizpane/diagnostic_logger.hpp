#pragma once

#include <QString>
#include <QVariantMap>

namespace quizpane::diagnostic {

// DEBUG 包调用 initialize() 后，Qt 的 qInfo/qWarning/qCritical 会同时写入滚动日志。
// Release 构建中这些函数是空操作，不创建文件，也不改变 Qt 的消息处理器。
bool initialize(const QString& component);
void event(const QString& area, const QString& name, const QVariantMap& fields = {});
void payload(const QString& area, const QString& name, const QString& label,
             const QString& content, qsizetype maximumCharacters = 32768);
void shutdown();
QString logFilePath();
QString crashArtifactPath();
bool openLogFile();

} // namespace quizpane::diagnostic
