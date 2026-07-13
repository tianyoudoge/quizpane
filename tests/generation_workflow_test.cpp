#include "quizpane/studio/generation_workflow.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

namespace {

QJsonObject question(const QString& materialId) {
    return {{"id", "q1"},
            {"catalogId", "generated"},
            {"materialId", materialId},
            {"type", "single_choice"},
            {"stem", "根据材料，正确答案是什么？"},
            {"options", QJsonArray{QJsonObject{{"id", "a"}, {"text", "甲"}},
                                   QJsonObject{{"id", "b"}, {"text", "乙"}}}},
            {"answer", QJsonObject{{"optionIds", QJsonArray{"a"}}}},
            {"solution", "材料明确说明答案是甲。"}};
}

QByteArray modelResponse(const QJsonObject& candidate) {
    const QString content =
        QString::fromUtf8(QJsonDocument(candidate).toJson(QJsonDocument::Compact));
    const QJsonObject response{
        {"choices", QJsonArray{QJsonObject{{"message", QJsonObject{{"content", content}}}}}},
        {"usage", QJsonObject{{"prompt_tokens", 10}, {"completion_tokens", 5}}}};
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

class FixedModelServer final : public QTcpServer {
  public:
    QList<QByteArray> requestBodies;

    explicit FixedModelServer(QObject* parent = nullptr) : QTcpServer(parent) {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    buffers_[socket] += socket->readAll();
                    const QByteArray& request = buffers_[socket];
                    const qsizetype headerEnd = request.indexOf("\r\n\r\n");
                    if (headerEnd < 0)
                        return;
                    int contentLength = 0;
                    for (const QByteArray& line : request.left(headerEnd).split('\n')) {
                        if (line.toLower().startsWith("content-length:"))
                            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    }
                    if (request.size() < headerEnd + 4 + contentLength)
                        return;
                    requestBodies.append(request.mid(headerEnd + 4, contentLength));

                    QJsonObject candidate;
                    if (requestBodies.size() == 1) {
                        candidate = {{"materials", QJsonArray{}},
                                     {"questions", QJsonArray{question("missing-material")}}};
                    } else {
                        candidate = {
                            {"materials", QJsonArray{QJsonObject{
                                              {"id", "reading"},
                                              {"catalogId", "generated"},
                                              {"title", "阅读材料"},
                                              {"body", "这是一段用于测试修复循环的材料正文。"}}}},
                            {"questions", QJsonArray{question("reading")}}};
                    }
                    const QByteArray payload = modelResponse(candidate);
                    socket->write(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                        QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" +
                        payload);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

  private:
    QHash<QTcpSocket*, QByteArray> buffers_;
};

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;
    qputenv("QUIZPANE_GENERATION_TASKS_ROOT", directory.filePath("tasks").toUtf8());
    const QString sourcePath = directory.filePath(QStringLiteral("reading.txt"));
    QFile source(sourcePath);
    if (!source.open(QIODevice::WriteOnly))
        return 2;
    source.write("根据以下资料，回答第 1～1 题\n\n这是一段材料。\n\n1. 正确答案是什么？");
    source.close();

    FixedModelServer server;
    if (!server.listen(QHostAddress::LocalHost, 0))
        return 3;
    QNetworkAccessManager manager;
    quizpane::studio::GenerationWorkflow workflow(&manager);
    quizpane::studio::GeneratedBankCandidate ready;
    QString failure;
    bool finished = false;
    QObject::connect(&workflow, &quizpane::studio::GenerationWorkflow::questionsReady, &app,
                     [&](const auto& candidate) { ready = candidate; });
    QObject::connect(&workflow, &quizpane::studio::GenerationWorkflow::failed, &app,
                     [&](const QString& error) {
                         failure = error;
                         app.quit();
                     });
    QObject::connect(&workflow, &quizpane::studio::GenerationWorkflow::finished, &app, [&] {
        finished = true;
        app.quit();
    });
    QTimer::singleShot(5000, &app, &QCoreApplication::quit);

    quizpane::studio::ModelSettings settings;
    settings.vendorId = QStringLiteral("openai-compatible");
    settings.modelName = QStringLiteral("fixed-model");
    settings.apiKey = QStringLiteral("test-only");
    settings.endpoint = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
    workflow.start({sourcePath}, settings);
    app.exec();

    if (!failure.isEmpty() || !finished || server.requestBodies.size() != 2)
        return 4;
    const QJsonObject repairRequest = QJsonDocument::fromJson(server.requestBodies.at(1)).object();
    const QString repairContent =
        repairRequest.value("messages").toArray().at(1).toObject().value("content").toString();
    if (!repairContent.contains(QStringLiteral("上一轮完整输出")) ||
        !repairContent.contains(QStringLiteral("missing-material")) ||
        !repairContent.contains(QStringLiteral("materialId")))
        return 5;
    if (ready.materials.size() != 1 || ready.questions.size() != 1 ||
        !ready.needsReviewQuestions.isEmpty())
        return 6;
    const QString materialId = ready.materials.first().toObject().value("id").toString();
    if (materialId.isEmpty() ||
        ready.questions.first().toObject().value("materialId").toString() != materialId)
        return 7;
    const auto& checkpoint = workflow.checkpoint();
    if (checkpoint.completedSourceBlocks.size() != 1 ||
        checkpoint.materialQuestionIds.value(materialId).toArray().size() != 1)
        return 8;
    return 0;
}
