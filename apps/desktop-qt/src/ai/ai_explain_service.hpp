#pragma once

#include <QJsonArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace quizpane {

struct AiUsage {
    int promptTokens = 0;
    int completionTokens = 0;
};

class AiExplainService final : public QObject {
    Q_OBJECT
public:
    explicit AiExplainService(QObject* parent = nullptr);

    void explain(const QString& questionHtml, const QJsonArray& options,
                 const QString& apiKey, const QString& baseUrl, const QString& model);
    bool isBusy() const { return reply_ != nullptr; }

signals:
    void requestStarted();
    void responseReady(const QString& html, quizpane::AiUsage usage);
    void requestFailed(const QString& error);

private:
    void onFinished(QNetworkReply* reply);

    QNetworkAccessManager* nam_;
    QNetworkReply* reply_ = nullptr;
};

}  // namespace quizpane
