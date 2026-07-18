#pragma once

#include "model_settings_dialog.hpp"

#include <QJsonObject>
#include <QNetworkRequest>
#include <QObject>

class QNetworkAccessManager;
class QNetworkReply;

namespace quizpane::studio {

struct GenerationRequest {
    QString systemPrompt;
    QString userContent;
    QString modelName;
    // 可选的局部视觉上下文。调用方负责只传当前复核所需的小区域；客户端不会从
    // 文件系统读取图片，也不会自动上传整页或整份原卷。
    QByteArray imagePng;
};

struct GenerationResult {
    bool ok = false;
    QString rawText;
    int promptTokens = 0;
    int completionTokens = 0;
    QString error;
};

QJsonObject buildChatCompletionsBody(const GenerationRequest& request);
QNetworkRequest buildChatCompletionsRequest(const ModelSettings& settings,
                                             QString* error = nullptr);
GenerationResult parseChatCompletionsResponse(const QByteArray& payload,
                                               int httpStatus,
                                               const QString& networkError = {});

// 图片输入不能从 Endpoint 或厂商名称可靠推断：同一服务可能同时有文本、视觉模型。
// 调用方据此在上传前阻断错误请求，并引导用户在模型管理里显式声明能力。
[[nodiscard]] bool canSendImageInput(const ModelSettings& settings);

class ModelClient final : public QObject {
    Q_OBJECT
public:
    explicit ModelClient(QNetworkAccessManager* manager, QObject* parent = nullptr);
    void generate(const ModelSettings& settings, const GenerationRequest& request);
    void cancel();

signals:
    void finished(const quizpane::studio::GenerationResult& result);

private:
    QNetworkAccessManager* manager_;
    QNetworkReply* reply_ = nullptr;
};

}  // namespace quizpane::studio
