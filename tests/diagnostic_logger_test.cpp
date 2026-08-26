#include "quizpane/diagnostic_logger.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QStandardPaths>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("diagnostic-logger-test"));
    QCoreApplication::setApplicationVersion(QStringLiteral("test"));
    QStandardPaths::setTestModeEnabled(true);
    const QString expectedLog = QDir(QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation)).filePath(
            QStringLiteral("QuizPane/logs/diagnostic-test-debug.log"));
    QFile::remove(expectedLog);
    QFile::remove(expectedLog + QStringLiteral(".1"));
    QFile::remove(expectedLog + QStringLiteral(".2"));
    if (!quizpane::diagnostic::initialize(QStringLiteral("diagnostic-test")))
        return 1;
    // 测试运行若继承了此前关闭的设置，先恢复为启用状态，确保后续断言稳定。
    quizpane::diagnostic::setDiagnosticsEnabled(true);
    qInfo().noquote() << "info-only /Users/alice/private/info";
    qWarning().noquote() << "warning-path /Users/alice/private/visible";
    qWarning().noquote() << "warning-windows C:\\Users\\alice\\private\\visible";
    quizpane::diagnostic::event(QStringLiteral("test"), QStringLiteral("breadcrumb"),
        {{QStringLiteral("count"), 3},
         {QStringLiteral("apiKey"), QStringLiteral("must-not-appear")}});
#ifdef QUIZPANE_VERBOSE_DIAGNOSTICS
    quizpane::diagnostic::payload(QStringLiteral("test"), QStringLiteral("payload"),
        QStringLiteral("source"), QStringLiteral("abcdef"), 4);
#endif
    quizpane::diagnostic::setDiagnosticsEnabled(false);
    qWarning().noquote() << "after-disabled";
    quizpane::diagnostic::event(QStringLiteral("test"), QStringLiteral("after-disabled"));
    quizpane::diagnostic::setDiagnosticsEnabled(true);
    quizpane::diagnostic::shutdown();
    QFile file(quizpane::diagnostic::logFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return 2;
    const QByteArray contents = file.readAll();
    if (!contents.contains("[test] breadcrumb") ||
        !contents.contains("apiKey=<redacted>") ||
        contents.contains("must-not-appear") ||
        quizpane::diagnostic::crashArtifactPath().isEmpty() ||
        !contents.contains("[session] end exit=clean"))
        return 3;
#ifdef QUIZPANE_VERBOSE_DIAGNOSTICS
    if (!contents.contains("captured=4 truncated=true") ||
        !contents.contains("abcd") || contents.contains("abcdef"))
        return 4;
#else
    if (contents.contains("captured=") || contents.contains("abcdef"))
        return 5;
#endif
#if defined(QUIZPANE_PROD_DIAGNOSTICS) && !defined(QUIZPANE_DIAGNOSTIC_LOGGING)
    // Release 语义：仅 warning+，用户路径不落盘，并且运行时关闭后立即停写。
    if (contents.contains("info-only") ||
        !contents.contains("warning-path .../private/visible") ||
        !contents.contains("warning-windows ...\\private\\visible") ||
        contents.contains("/Users/alice/") || contents.contains("C:\\Users\\alice") ||
        contents.contains("after-disabled"))
        return 6;
#endif
    return 0;
}
