#include "quizpane/feedback_report.hpp"
#include "quizpane/diagnostic_logger.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("feedback-report-test"));
    QStandardPaths::setTestModeEnabled(true);
    if (!quizpane::diagnostic::initialize(QStringLiteral("feedback-report-test")))
        return 1;
    quizpane::diagnostic::setDiagnosticsEnabled(true);
    for (int index = 0; index < 1100; ++index) {
        quizpane::diagnostic::event(QStringLiteral("feedback-test"), QStringLiteral("entry"),
                                    {{QStringLiteral("sequence"), index}});
    }
    const QString tail = quizpane::feedback::buildLogTail();
    if (tail.split(QChar('\n'), Qt::SkipEmptyParts).size() > 1000 ||
        tail.toUtf8().size() > 512 * 1024 ||
        !tail.contains(QStringLiteral("sequence=1099")))
        return 2;
    quizpane::diagnostic::shutdown();

    QTemporaryDir exportDirectory;
    if (!exportDirectory.isValid())
        return 3;
    const QString exportPath = exportDirectory.filePath(QStringLiteral("feedback.json"));
    const auto exported = quizpane::feedback::exportReport(
        QStringLiteral("offline export"), false, false, exportPath);
    QFile exportedFile(exportPath);
    if (!exported.success || !exportedFile.open(QIODevice::ReadOnly) ||
        !exportedFile.readAll().contains("\"description\":\"offline export\""))
        return 4;

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost))
        return 5;

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
        return 6;

    QTcpServer silentServer;
    if (!silentServer.listen(QHostAddress::LocalHost))
        return 7;
    const auto timeout = quizpane::feedback::sendReport(
        QStringLiteral("times out"), false, false,
        QStringLiteral("http://127.0.0.1:%1/feedback").arg(silentServer.serverPort()), 20);
    if (timeout.success || !timeout.message.contains(QStringLiteral("超时")))
        return 8;

    quint16 closedPort = 0;
    {
        QTcpServer temporaryServer;
        if (!temporaryServer.listen(QHostAddress::LocalHost))
            return 9;
        closedPort = temporaryServer.serverPort();
    }
    const auto refused = quizpane::feedback::sendReport(
        QStringLiteral("refused"), false, false,
        QStringLiteral("http://127.0.0.1:%1/feedback").arg(closedPort), 1000);
    if (refused.success || !refused.message.contains(QStringLiteral("无法连接反馈服务")))
        return 10;
    return 0;
}
