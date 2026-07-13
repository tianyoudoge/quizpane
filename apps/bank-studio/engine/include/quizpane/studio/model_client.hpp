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
