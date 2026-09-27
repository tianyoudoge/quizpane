#include "../apps/desktop-qt/src/ai/ai_explain_service.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) return 1;
    int requestCount = 0;
    bool sawAnswer = false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (auto* socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                QByteArray data = socket->property("requestData").toByteArray() + socket->readAll();
                socket->setProperty("requestData", data);
                const int split = data.indexOf("\r\n\r\n");
                if (split < 0) return;
                const QByteArray headers = data.left(split);
                const int lengthPos = headers.toLower().indexOf("content-length:");
                if (lengthPos < 0) return;
                const int endPos = headers.indexOf("\r\n", lengthPos);
                const int length = headers.mid(lengthPos + 15, endPos - lengthPos - 15).trimmed().toInt();
                if (data.size() - split - 4 < length) return;
                const auto request = QJsonDocument::fromJson(data.mid(split + 4, length)).object();
                const auto messages = request.value(QStringLiteral("messages")).toArray();
                const QString prompt = messages.first().toObject()
                                           .value(QStringLiteral("content")).toString();
                sawAnswer = prompt.contains(QStringLiteral("标准答案：B")) &&
                            prompt.contains(QStringLiteral("材料：\n阅读材料"));
                const QByteArray body = (++requestCount == 1)
                    ? QByteArray(R"({"choices":[{"message":{"content":"<b>步骤</b>"}}],"usage":{"prompt_tokens":12,"completion_tokens":5}})")
                    : QByteArray(R"({"choices":[]})");
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                              + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });

    quizpane::AiExplainService service;
    int success = 0;
    int failure = 0;
    quizpane::AiUsage usage;
    QEventLoop loop;
    QObject::connect(&service, &quizpane::AiExplainService::responseReady, &loop,
                     [&](const QString& html, quizpane::AiUsage result) {
        if (html.contains(QStringLiteral("步骤"))) ++success;
        usage = result;
        loop.quit();
    });
    QObject::connect(&service, &quizpane::AiExplainService::requestFailed, &loop,
                     [&](const QString&) { ++failure; loop.quit(); });
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    const QString baseUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
    const auto explain = [&] {
        service.explain(QStringLiteral("题干"), QJsonArray{}, QStringLiteral("阅读材料"),
                        QJsonArray{},
                        QStringLiteral("B"),
                        QStringLiteral("test-key"), baseUrl, QStringLiteral("test-model"));
        timer.start(3000);
        loop.exec();
        timer.stop();
    };
    explain();
    if (success != 1 || failure != 0 || !sawAnswer || usage.promptTokens != 12 ||
        usage.completionTokens != 5) return 2;
    explain();
    if (success != 1 || failure != 1) return 3;
    service.explain(QStringLiteral("旧题"), {}, {}, {}, {}, QStringLiteral("test-key"),
                    baseUrl, QStringLiteral("test-model"));
    service.cancel();
    QCoreApplication::processEvents();
    if (success != 1 || failure != 1 || service.isBusy()) return 4;
    return 0;
}
