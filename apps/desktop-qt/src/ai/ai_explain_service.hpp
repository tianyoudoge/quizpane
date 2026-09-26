#pragma once

#include <QJsonArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace quizpane {

class AiExplainService final : public QObject {
    Q_OBJECT
public:
    explicit AiExplainService(QObject* parent = nullptr);

    void explain(const QString& questionHtml, const QJsonArray& options,
                 const QString& apiKey);
    bool isBusy() const { return reply_ != nullptr; }

signals:
    void requestStarted();
    void responseReady(const QString& html);
    void requestFailed(const QString& error);

private:
    void onFinished(QNetworkReply* reply);

    QNetworkAccessManager* nam_;
    QNetworkReply* reply_ = nullptr;
};

}  // namespace quizpane
