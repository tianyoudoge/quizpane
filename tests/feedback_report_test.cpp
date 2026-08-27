#include "quizpane/feedback_report.hpp"
#include "quizpane/diagnostic_logger.hpp"

#include <QCoreApplication>
#include <QFile>
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
    const QString tail = quizpane::feedback::buildLogTail();
    if (tail.split(QChar('\n'), Qt::SkipEmptyParts).size() > 1000 ||
        tail.toUtf8().size() > 512 * 1024 ||
        !tail.contains(QStringLiteral("sequence=1099")))
        return fail(2, "log tail bounds or newest event assertion failed");
    quizpane::diagnostic::shutdown();

    QTemporaryDir exportDirectory;
    if (!exportDirectory.isValid())
        return fail(3, "cannot create temporary export directory");
    const QString exportPath = exportDirectory.filePath(QStringLiteral("feedback.json"));
    const auto exported = quizpane::feedback::exportReport(
        QStringLiteral("offline export"), false, false, exportPath);
    QFile exportedFile(exportPath);
    if (!exported.success || !exportedFile.open(QIODevice::ReadOnly) ||
        !exportedFile.readAll().contains("\"description\":\"offline export\""))
        return fail(4, "offline export contents assertion failed");

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost))
        return fail(5, "cannot listen for success-case feedback server");

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
    const auto success = quizpane::feedback::sendReport(
        QStringLiteral("works"), false, false, endpoint, 1000);
    if (!success.success || !replied || !request.startsWith("POST /feedback HTTP/1.1"))
        return fail(6, "successful feedback request assertion failed");

    QTcpServer silentServer;
    if (!silentServer.listen(QHostAddress::LocalHost))
        return fail(7, "cannot listen for timeout-case feedback server");
    const auto timeout = quizpane::feedback::sendReport(
        QStringLiteral("times out"), false, false,
        QStringLiteral("http://127.0.0.1:%1/feedback").arg(silentServer.serverPort()), 20);
    if (timeout.success || !timeout.message.contains(QStringLiteral("超时")))
        return fail(8, "feedback timeout assertion failed");

    quint16 closedPort = 0;
    {
        QTcpServer temporaryServer;
        if (!temporaryServer.listen(QHostAddress::LocalHost))
            return fail(9, "cannot allocate a closed-port test endpoint");
        closedPort = temporaryServer.serverPort();
    }
    const auto refused = quizpane::feedback::sendReport(
        QStringLiteral("refused"), false, false,
        QStringLiteral("http://127.0.0.1:%1/feedback").arg(closedPort), 1000);
    // Windows 上关闭的 loopback 端口可能等待 SYN 超时，而不是立即报告
    // ConnectionRefused；两种结果均是用户可读的预期失败路径。
    if (refused.success ||
        (!refused.message.contains(QStringLiteral("无法连接反馈服务")) &&
         !refused.message.contains(QStringLiteral("超时"))))
        return fail(10, "closed-port error message assertion failed");
    return 0;
}
