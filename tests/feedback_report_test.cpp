#include "quizpane/feedback_report.hpp"
#include "quizpane/diagnostic_logger.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include <cstdio>

int main(int argc, char** argv) {
    const auto fail = [](int code, const char* reason) {
        std::fprintf(stderr, "feedback_report_test failed (%d): %s\\n", code, reason);
        return code;
    };
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("feedback-report-test"));
    QStandardPaths::setTestModeEnabled(true);
    if (!quizpane::diagnostic::initialize(QStringLiteral("feedback-report-test")))
        return fail(1, "cannot initialize diagnostic logger");
    quizpane::diagnostic::setDiagnosticsEnabled(true);
    for (int index = 0; index < 1100; ++index) {
        quizpane::diagnostic::event(QStringLiteral("feedback-test"), QStringLiteral("entry"),
                                    {{QStringLiteral("sequence"), index}});
    }
    quizpane::diagnostic::event(QStringLiteral("feedback-test"), QStringLiteral("unicode"),
                                {{QStringLiteral("detail"),
                                  QStringLiteral("内存不足：规则整理")}});
    const QString tail = quizpane::feedback::buildLogTail();
    if (tail.split(QChar('\n'), Qt::SkipEmptyParts).size() > 1000 ||
        tail.toUtf8().size() > 512 * 1024 ||
        !tail.contains(QStringLiteral("sequence=1099")) ||
        !tail.contains(QStringLiteral("detail=内存不足：规则整理")))
        return fail(2, "log tail bounds or newest event assertion failed");
    quizpane::diagnostic::shutdown();

    QTemporaryDir exportDirectory;
    if (!exportDirectory.isValid())
        return fail(3, "cannot create temporary export directory");
    const QString exportPath = exportDirectory.filePath(QStringLiteral("feedback.json"));
    quizpane::feedback::ReportOptions exportOptions;
    exportOptions.description = QStringLiteral("离线导出");
    exportOptions.includeLogs = true;
    exportOptions.includeCrash = false;
    const auto exported = quizpane::feedback::exportReport(exportOptions, exportPath);
    QFile exportedFile(exportPath);
    if (!exported.success || !exportedFile.open(QIODevice::ReadOnly))
        return fail(4, "offline export contents assertion failed");
    const QJsonObject exportedPayload =
        QJsonDocument::fromJson(exportedFile.readAll()).object();
    if (exportedPayload.value(QStringLiteral("description")).toString() !=
            QStringLiteral("离线导出") ||
        !exportedPayload.value(QStringLiteral("logs")).toString().contains(
            QStringLiteral("detail=内存不足：规则整理")))
        return fail(4, "offline export unicode assertion failed");
    exportedFile.close();

    QFile oldCrash(quizpane::diagnostic::crashArtifactPath());
    if (!oldCrash.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        oldCrash.write("old crash") < 0)
        return fail(5, "cannot prepare old crash artifact");
    oldCrash.close();
    if (!oldCrash.open(QIODevice::ReadWrite) ||
        !oldCrash.setFileTime(QDateTime::currentDateTime().addDays(-2),
                              QFileDevice::FileModificationTime))
        return fail(5, "cannot age crash artifact");
    oldCrash.close();
    exportOptions.includeCrash = true;
    const auto oldCrashExport = quizpane::feedback::exportReport(exportOptions, exportPath);
    if (!oldCrashExport.success || !exportedFile.open(QIODevice::ReadOnly))
        return fail(6, "cannot export report with old crash artifact");
    const QJsonObject oldCrashPayload =
        QJsonDocument::fromJson(exportedFile.readAll()).object();
    exportedFile.close();
    if (oldCrashPayload.contains(QStringLiteral("crashFile")))
        return fail(7, "old crash artifact was included");

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost))
        return fail(8, "cannot listen for success-case feedback server");

    QByteArray request;
    bool replied = false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        QTcpSocket* socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            request += socket->readAll();
            // 等到 JSON 主体到齐再答复，确认 sendReport() 确实完成了请求。
            if (!replied && request.contains("\"description\":\"works\"")) {
                replied = true;
                socket->write("HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: 11\r\n"
                              "Connection: close\r\n\r\n"
                              "{\"ok\":true}");
                socket->flush();
            }
        });
    });

    const QString endpoint = QStringLiteral("http://127.0.0.1:%1/feedback")
                                 .arg(server.serverPort());
    quizpane::feedback::ReportOptions sendOptions;
    sendOptions.description = QStringLiteral("works");
    sendOptions.includeLogs = false;
    sendOptions.includeCrash = false;
    const auto success = quizpane::feedback::sendReport(sendOptions, endpoint, 1000);
    if (!success.success || !replied || !request.startsWith("POST /feedback HTTP/1.1"))
        return fail(9, "successful feedback request assertion failed");

    QTcpServer silentServer;
    if (!silentServer.listen(QHostAddress::LocalHost))
        return fail(10, "cannot listen for timeout-case feedback server");
    sendOptions.description = QStringLiteral("times out");
    const auto timeout = quizpane::feedback::sendReport(
        sendOptions,
        QStringLiteral("http://127.0.0.1:%1/feedback").arg(silentServer.serverPort()), 20);
    if (timeout.success || !timeout.message.contains(QStringLiteral("超时")))
        return fail(11, "feedback timeout assertion failed");

    quint16 closedPort = 0;
    {
        QTcpServer temporaryServer;
        if (!temporaryServer.listen(QHostAddress::LocalHost))
            return fail(12, "cannot allocate a closed-port test endpoint");
        closedPort = temporaryServer.serverPort();
    }
    sendOptions.description = QStringLiteral("refused");
    const auto refused = quizpane::feedback::sendReport(
        sendOptions, QStringLiteral("http://127.0.0.1:%1/feedback").arg(closedPort), 1000);
    // Windows 上关闭的 loopback 端口可能等待 SYN 超时，而不是立即报告
    // ConnectionRefused；两种结果均是用户可读的预期失败路径。
    if (refused.success ||
        (!refused.message.contains(QStringLiteral("无法连接反馈服务")) &&
         !refused.message.contains(QStringLiteral("超时"))))
        return fail(13, "closed-port error message assertion failed");
    return 0;
}
