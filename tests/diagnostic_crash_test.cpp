#include "quizpane/diagnostic_logger.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <exception>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setApplicationName(QStringLiteral("diagnostic-crash-test"));
    if (app.arguments().contains(QStringLiteral("--crash"))) {
        if (!quizpane::diagnostic::initialize(QStringLiteral("diagnostic-crash-test")))
            return 10;
        std::terminate();
    }

    const QString extension =
#ifdef Q_OS_WIN
        QStringLiteral(".dmp");
#else
        QStringLiteral(".log");
#endif
    const QString artifact = QDir(QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation)).filePath(
            QStringLiteral("QuizPane/logs/diagnostic-crash-test-crash") + extension);
    QFile::remove(artifact);
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--crash")});
    if (!child.waitForStarted() || !child.waitForFinished(10000))
        return 1;
    const QFileInfo result(artifact);
    if (!result.isFile() || result.size() == 0)
        return 2;
    return 0;
}
