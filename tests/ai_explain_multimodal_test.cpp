#include "../apps/desktop-qt/src/ai/ai_explain_service.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

namespace {

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

int imageCount(const QJsonArray& parts) {
    int count = 0;
    for (const QJsonValue& value : parts)
        if (value.toObject().value(QStringLiteral("type")) == QStringLiteral("image_url")) ++count;
    return count;
}

bool containsImage(const QJsonArray& parts, const QByteArray& bytes) {
    for (const QJsonValue& value : parts) {
        const QJsonObject part = value.toObject();
        if (part.value(QStringLiteral("type")) != QStringLiteral("image_url")) continue;
        const QString url = part.value(QStringLiteral("image_url")).toObject()
                                .value(QStringLiteral("url")).toString();
        const int separator = url.indexOf(QStringLiteral(";base64,"));
        if (separator >= 0 && QByteArray::fromBase64(url.mid(separator + 8).toLatin1()) == bytes)
            return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) return 1;
    QJsonObject received;
    int requests = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (auto* socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                if (socket->property("handled").toBool()) return;
                const QByteArray data = socket->property("data").toByteArray() + socket->readAll();
                socket->setProperty("data", data);
                const int split = data.indexOf("\r\n\r\n");
                if (split < 0) return;
                const QByteArray headers = data.left(split);
                const int start = headers.toLower().indexOf("content-length:");
                const int end = headers.indexOf("\r\n", start);
                if (start < 0 || end < 0) return;
                const int length = headers.mid(start + 15, end - start - 15).trimmed().toInt();
                if (data.size() - split - 4 < length) return;
                socket->setProperty("handled", true);
                received = QJsonDocument::fromJson(data.mid(split + 4, length)).object();
                ++requests;
                const QByteArray body = R"({"choices":[{"message":{"content":"解析完成"}}]})";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                              + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });

    QTemporaryDir dir;
    if (!dir.isValid()) return 2;
    QStringList paths;
    for (int i = 0; i < 3; ++i) {
        const QString path = dir.filePath(QStringLiteral("image-%1.png").arg(i));
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(i == 0 ? Qt::red : i == 1 ? Qt::green : Qt::blue);
        if (!image.save(path)) return 3;
        paths.append(path);
    }
    const QString materialUrl = QUrl::fromLocalFile(paths.at(0)).toString();
    const QString stemUrl = QUrl::fromLocalFile(paths.at(1)).toString();
    const QString optionUrl = QUrl::fromLocalFile(paths.at(2)).toString();

    quizpane::AiExplainService service;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool completed = false;
    bool failed = false;
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&service, &quizpane::AiExplainService::responseReady, &loop,
                     [&](const QString&, quizpane::AiUsage) { completed = true; loop.quit(); });
    QObject::connect(&service, &quizpane::AiExplainService::requestFailed, &loop,
                     [&](const QString&) { failed = true; loop.quit(); });
    const QString baseUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
    const auto run = [&](const QString& stem, const QJsonArray& options,
                         const QString& material, const QJsonArray& materialUrls,
                         const QString& answer) {
        completed = failed = false;
        service.explain(stem, options, material, materialUrls, answer,
                        QStringLiteral("test-key"), baseUrl, QStringLiteral("deepseek-chat"));
        timeout.start(3000);
        loop.exec();
        timeout.stop();
    };

    const QJsonArray options{QJsonObject{{QStringLiteral("label"), QStringLiteral("A")},
                                         {QStringLiteral("contentHtml"), QStringLiteral("图形选项")},
                                         {QStringLiteral("imageUrl"), optionUrl}}};
    run(QStringLiteral("带公式的题干<img src=\"%1\">").arg(stemUrl), options,
        QStringLiteral("材料正文"), QJsonArray{materialUrl}, QStringLiteral("A"));
    if (!completed || failed || requests != 1 ||
        received.value(QStringLiteral("model")) != QStringLiteral("deepseek-flash") ||
        received.value(QStringLiteral("max_tokens")).toInt() != 131072) return 4;
    const QJsonArray parts = received.value(QStringLiteral("messages")).toArray().first()
                                 .toObject().value(QStringLiteral("content")).toArray();
    if (imageCount(parts) != 3 ||
        !parts.first().toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("材料正文")) ||
        !parts.first().toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("图形选项")) ||
        !parts.first().toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("不得把界限写成精确值")) ||
        parts.first().toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("file://")) ||
        !containsImage(parts, readFile(paths.at(0))) ||
        !containsImage(parts, readFile(paths.at(1))) ||
        !containsImage(parts, readFile(paths.at(2)))) return 5;

    completed = failed = false;
    service.explain(QStringLiteral("题干"), {}, {},
                    QJsonArray{QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.png"))).toString()},
                    {}, QStringLiteral("test-key"), baseUrl, QStringLiteral("deepseek-chat"));
    if (!failed || completed || service.isBusy() || requests != 1) return 6;
    completed = failed = false;
    service.explain(QStringLiteral("<img src=broken>"), {}, {}, {}, {},
                    QStringLiteral("test-key"), baseUrl, QStringLiteral("deepseek-chat"));
    if (!failed || completed || service.isBusy() || requests != 1) return 9;

    // Optional local acceptance: run with QUIZPANE_Q119_BANK pointing at the installed 2011 bank.
    const QString bankPath = qEnvironmentVariable("QUIZPANE_Q119_BANK");
    if (!bankPath.isEmpty()) {
        const QJsonObject bank = QJsonDocument::fromJson(readFile(bankPath)).object();
        QJsonObject question, material;
        for (const QJsonValue& value : bank.value(QStringLiteral("questions")).toArray()) {
            const QJsonObject candidate = value.toObject();
            if (candidate.value(QStringLiteral("source")).toObject()
                         .value(QStringLiteral("questionNumber")).toInt() == 119) question = candidate;
        }
        for (const QJsonValue& value : bank.value(QStringLiteral("materials")).toArray()) {
            const QJsonObject candidate = value.toObject();
            if (candidate.value(QStringLiteral("id")) == question.value(QStringLiteral("materialId")))
                material = candidate;
        }
        if (question.isEmpty() || material.isEmpty()) return 7;
        QJsonArray realOptions;
        for (const QJsonValue& value : question.value(QStringLiteral("options")).toArray()) {
            const QJsonObject option = value.toObject();
            realOptions.append(QJsonObject{{QStringLiteral("label"),
                                            QString(QChar(char16_t(u'A' + realOptions.size())))},
                                           {QStringLiteral("contentHtml"),
                                            option.value(QStringLiteral("text"))}});
        }
        QJsonArray realImages;
        QByteArray realImageBytes;
        for (const QJsonValue& value : material.value(QStringLiteral("images")).toArray()) {
            const QString relative = value.toObject().value(QStringLiteral("path")).toString();
            const QString path = QDir(QFileInfo(bankPath).absolutePath())
                                     .absoluteFilePath(QStringLiteral("../") + relative);
            if (realImageBytes.isEmpty()) realImageBytes = readFile(path);
            realImages.append(QUrl::fromLocalFile(path).toString());
        }
        run(question.value(QStringLiteral("stem")).toString(), realOptions,
            material.value(QStringLiteral("body")).toString(), realImages,
            QStringLiteral("D"));
        const QJsonArray realParts = received.value(QStringLiteral("messages")).toArray().first()
                                         .toObject().value(QStringLiteral("content")).toArray();
        const QString prompt = realParts.first().toObject().value(QStringLiteral("text")).toString();
        if (!completed || failed || requests != 2 || imageCount(realParts) < 1 ||
            !prompt.contains(question.value(QStringLiteral("stem")).toString()) ||
            !prompt.contains(material.value(QStringLiteral("body")).toString().left(25)) ||
            !prompt.contains(QStringLiteral("低于 12.0%")) ||
            !prompt.contains(QStringLiteral("超过 88.0%")) ||
            !prompt.contains(QStringLiteral("标准答案：D")) ||
            !containsImage(realParts, realImageBytes)) return 8;
    }
    return 0;
}
