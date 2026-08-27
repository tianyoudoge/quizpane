#pragma once

#include <QString>
#include <QVariantMap>

namespace quizpane::diagnostic {

// 调用 initialize() 后，Qt 的 Qt 消息会写入滚动日志（GenericDataLocation
// 下的 QuizPane/logs/{component}-debug.log），并安装崩溃捕获
// （Windows 的 minidump / POSIX 的信号 backtrace）。
//
// 两套构建语义由编译期宏切换（见 .cpp 里的 #ifdef/#else）：
// - QUIZPANE_DIAGNOSTIC_LOGGING（DEBUG 包）：qInfo 及以上全量落盘，
//   event()/payload() 走 qInfo 管道。
// - QUIZPANE_PROD_DIAGNOSTICS（Release 包，默认开）：只落 warning+ 的 Qt
//   消息与 event() 面包屑，payload() 为空实现；日志开关可在运行时切换
//   （isDiagnosticsEnabled()/setDiagnosticsEnabled()，QSettings 键
//   "diagnosticsEnabled"），崩溃捕获始终安装。
// - 两者都未定义时，所有函数是空操作，不创建文件，也不安装崩溃捕获。
bool initialize(const QString& component);
// 生产包（QUIZPANE_PROD_DIAGNOSTICS）下读取/写入「诊断日志」运行时开关；
// 其他构建下恒为 true / 空操作。
bool isDiagnosticsEnabled();
void setDiagnosticsEnabled(bool enabled);
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
