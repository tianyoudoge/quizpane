#include "quizpane/studio/model_client.hpp"
#include "quizpane/diagnostic_logger.hpp"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>

namespace quizpane::studio {

bool canSendImageInput(const ModelSettings& settings) {
    return settings.supportsVision;
}

QJsonObject buildChatCompletionsBody(const GenerationRequest& request) {
    QJsonValue userContent = request.userContent;
    if (!request.imagePng.isEmpty()) {
        const QString imageUrl = QStringLiteral("data:image/png;base64,") +
            QString::fromLatin1(request.imagePng.toBase64());
        userContent = QJsonArray{
            QJsonObject{{"type", "text"}, {"text", request.userContent}},
            QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", imageUrl}}}}};
    }
    return {{"model", request.modelName},
            {"messages", QJsonArray{
                QJsonObject{{"role", "system"}, {"content", request.systemPrompt}},
                QJsonObject{{"role", "user"}, {"content", userContent}}}},
            {"response_format", QJsonObject{{"type", "json_object"}}},
            {"temperature", 0.2}};
}

QNetworkRequest buildChatCompletionsRequest(const ModelSettings& settings, QString* error) {
    QString endpoint = settings.endpoint.trimmed();
    while (endpoint.endsWith('/')) endpoint.chop(1);
    QUrl url(endpoint + QStringLiteral("/chat/completions"));
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
        if (error) *error = QStringLiteral("Endpoint 格式不正确");
        return {};
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("QuizPane-Question-Maker"));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(120000);
    if (settings.vendorId != QStringLiteral("ollama")) {
        if (settings.apiKey.trimmed().isEmpty()) {
            if (error) *error = QStringLiteral("请先在模型设置中填写 API Key");
            return {};
        }
        request.setRawHeader("Authorization", "Bearer " + settings.apiKey.trimmed().toUtf8());
    }
    return request;
}

GenerationResult parseChatCompletionsResponse(const QByteArray& payload, int httpStatus,
                                              const QString& networkError) {
    GenerationResult result;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    const QJsonObject object = document.object();
    if (!networkError.isEmpty() || httpStatus >= 400) {
        const QString apiMessage = object.value("error").toObject().value("message").toString();
        result.error = apiMessage.isEmpty() ? networkError : apiMessage;
        if (result.error.isEmpty()) result.error = QStringLiteral("模型请求失败（HTTP %1）").arg(httpStatus);
        return result;
    }
    if (!document.isObject()) {
        result.error = QStringLiteral("模型响应不是有效 JSON：%1").arg(parseError.errorString());
        return result;
    }
    const QJsonArray choices = object.value("choices").toArray();
    if (choices.isEmpty()) {
        result.error = QStringLiteral("模型响应缺少 choices");
        return result;
    }
    result.rawText = choices.first().toObject().value("message").toObject().value("content").toString();
    if (result.rawText.trimmed().isEmpty()) {
        result.error = QStringLiteral("模型返回了空内容");
        return result;
    }
    const QJsonObject usage = object.value("usage").toObject();
    result.promptTokens = usage.value("prompt_tokens").toInt();
    result.completionTokens = usage.value("completion_tokens").toInt();
    result.ok = true;
    return result;
}

ModelClient::ModelClient(QNetworkAccessManager* manager, QObject* parent)
    : QObject(parent), manager_(manager) {}

void ModelClient::generate(const ModelSettings& settings, const GenerationRequest& requestValue) {
    cancel();
    if (!requestValue.imagePng.isEmpty() && !canSendImageInput(settings)) {
        const QString error = QStringLiteral(
            "当前模型未声明支持图片输入，未发送局部截图。请到“模型管理”勾选“该模型支持图片输入”，"
            "或改用视觉模型后重试。");
        QMetaObject::invokeMethod(this, [this, error] {
            GenerationResult result; result.error = error; emit finished(result);
        }, Qt::QueuedConnection);
        return;
    }
    QString error;
    const QNetworkRequest networkRequest = buildChatCompletionsRequest(settings, &error);
    if (!error.isEmpty()) {
        diagnostic::event(QStringLiteral("model"), QStringLiteral("request-invalid"),
            {{QStringLiteral("vendor"), settings.vendorId},
             {QStringLiteral("error"), error}});
        QMetaObject::invokeMethod(this, [this, error] {
            GenerationResult result; result.error = error; emit finished(result);
        }, Qt::QueuedConnection);
        return;
    }
    diagnostic::event(QStringLiteral("model"), QStringLiteral("request-start"),
         {{QStringLiteral("vendor"), settings.vendorId},
          {QStringLiteral("model"), requestValue.modelName},
          {QStringLiteral("host"), networkRequest.url().host()},
         {QStringLiteral("inputChars"), requestValue.userContent.size()},
         {QStringLiteral("imageBytes"), requestValue.imagePng.size()}});
    QElapsedTimer elapsed;
    elapsed.start();
    QJsonObject requestBody = buildChatCompletionsBody(requestValue);
    // 百炼的 Qwen 3.7 系列默认会输出思考过程；视觉定位则要求稳定的
    // response_format=json_object，按官方兼容接口约定需显式关闭思考模式。
    if (settings.vendorId == QStringLiteral("dashscope"))
        requestBody.insert(QStringLiteral("enable_thinking"), false);
    reply_ = manager_->post(networkRequest,
        QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
    QNetworkReply* current = reply_;
    connect(current, &QNetworkReply::finished, this, [this, current, elapsed, settings] {
        if (reply_ == current) reply_ = nullptr;
        const int status = current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString networkError = current->error() == QNetworkReply::NoError
            ? QString() : current->errorString();
        auto result = parseChatCompletionsResponse(current->readAll(), status, networkError);
        if (current->error() == QNetworkReply::RemoteHostClosedError) {
            result.error = settings.vendorId == QStringLiteral("dashscope")
                ? QStringLiteral("百炼服务主动关闭了连接。已自动按视觉 JSON 模式请求；"
                                 "请稍后重试，并确认模型与 API Key 属于同一百炼地域。")
                : QStringLiteral("模型服务主动关闭了连接。请确认 Endpoint 是 OpenAI 兼容接口，"
                                 "且所选模型支持图片输入。");
        }
        diagnostic::event(QStringLiteral("model"), QStringLiteral("request-finished"),
            {{QStringLiteral("http"), status},
             {QStringLiteral("ok"), result.ok},
             {QStringLiteral("elapsedMs"), elapsed.elapsed()},
             {QStringLiteral("promptTokens"), result.promptTokens},
             {QStringLiteral("completionTokens"), result.completionTokens},
             {QStringLiteral("error"), result.error}});
        current->deleteLater();
        emit finished(result);
    });
}

void ModelClient::cancel() {
    if (!reply_) return;
    diagnostic::event(QStringLiteral("model"), QStringLiteral("request-cancelled"));
    disconnect(reply_, nullptr, this, nullptr);
    reply_->abort();
    reply_->deleteLater();
    reply_ = nullptr;
}

}  // namespace quizpane::studio
