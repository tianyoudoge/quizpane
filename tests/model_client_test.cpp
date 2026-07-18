#include "quizpane/studio/model_client.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    quizpane::studio::ModelSettings settings;
    settings.endpoint = QStringLiteral("https://example.test/v1/");
    settings.apiKey = QStringLiteral("secret");
    settings.modelName = QStringLiteral("test-model");
    settings.supportsVision = false;
    if (quizpane::studio::canSendImageInput(settings)) return 6;
    settings.supportsVision = true;
    if (!quizpane::studio::canSendImageInput(settings)) return 7;
    QString error;
    const auto request = quizpane::studio::buildChatCompletionsRequest(settings, &error);
    if (!error.isEmpty() || request.url().toString() != QStringLiteral("https://example.test/v1/chat/completions")) return 1;
    if (request.rawHeader("Authorization") != QByteArray("Bearer secret")) return 2;
    const auto body = quizpane::studio::buildChatCompletionsBody(
        {QStringLiteral("system"), QStringLiteral("content"), QStringLiteral("m")});
    if (body.value("model").toString() != QStringLiteral("m") ||
        body.value("messages").toArray().size() != 2 ||
        body.value("response_format").toObject().value("type") != QStringLiteral("json_object")) return 3;
    const auto visualBody = quizpane::studio::buildChatCompletionsBody(
        {QStringLiteral("system"), QStringLiteral("locate crop"), QStringLiteral("m"), QByteArrayLiteral("png")});
    const QJsonArray visualContent = visualBody.value("messages").toArray().at(1).toObject()
        .value("content").toArray();
    if (visualContent.size() != 2 || visualContent.at(1).toObject().value("image_url").toObject()
        .value("url").toString() != QStringLiteral("data:image/png;base64,cG5n")) return 5;
    const QJsonObject response{{"choices", QJsonArray{QJsonObject{{"message", QJsonObject{{"content", "{}"}}}}}},
        {"usage", QJsonObject{{"prompt_tokens", 11}, {"completion_tokens", 4}}}};
    const auto parsed = quizpane::studio::parseChatCompletionsResponse(
        QJsonDocument(response).toJson(), 200);
    if (!parsed.ok || parsed.promptTokens != 11 || parsed.completionTokens != 4 || parsed.rawText != QStringLiteral("{}")) return 4;
    return 0;
}
