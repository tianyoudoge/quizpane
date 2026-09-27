#include "ai_explain_service.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace quizpane {

static QString buildPrompt(const QString& questionHtml, const QJsonArray& options) {
    QString optionLines;
    for (const auto& val : options) {
        const auto opt = val.toObject();
        optionLines += QStringLiteral("%1. %2\n")
            .arg(opt.value(QStringLiteral("label")).toString(),
                 opt.value(QStringLiteral("contentHtml")).toString());
    }
    return QStringLiteral(
        "你是一位专业考试辅导老师。请对以下题目进行解析。\n"
        "格式要求：\n"
        "- 用HTML标签排版（<b>、<sup>、<sub>、<br>等），不要使用LaTeX或Markdown语法\n"
        "- 数学公式用Unicode符号和HTML上下标：指数写 x<sup>2</sup>，分数写 a/b，根号写 √\n"
        "- 按以下四个部分输出，每个部分标题用【】包裹：\n"
        "  【答案确认】正确选项及简要理由\n"
        "  【解题步骤】分步骤详细推导\n"
        "  【知识点】涉及的核心知识点\n"
        "  【速算技巧】如有速算方法或公式推导请列出，否则省略此部分\n\n"
        "题目：\n%1\n\n"
        "选项：\n%2"
    ).arg(questionHtml, optionLines);
}

AiExplainService::AiExplainService(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

void AiExplainService::explain(const QString& questionHtml, const QJsonArray& options,
                                const QString& apiKey, const QString& baseUrl,
                                const QString& model) {
    if (reply_) {
        reply_->abort();
        reply_ = nullptr;
    }

    const QString prompt = buildPrompt(questionHtml, options);
    const QString effectiveModel = model.isEmpty() ? QStringLiteral("deepseek-chat") : model;
    const QString endpoint = baseUrl.isEmpty()
        ? QStringLiteral("https://api.deepseek.com/v1/chat/completions")
        : (baseUrl.endsWith(QStringLiteral("/"))
               ? baseUrl + QStringLiteral("chat/completions")
               : baseUrl + QStringLiteral("/chat/completions"));

    QJsonObject body{
        {QStringLiteral("model"), effectiveModel},
        {QStringLiteral("messages"), QJsonArray{
            QJsonObject{
                {QStringLiteral("role"), QStringLiteral("user")},
                {QStringLiteral("content"), prompt}
            }
        }},
        {QStringLiteral("temperature"), 0.3},
        {QStringLiteral("max_tokens"), 2048}
    };

    QUrl url(endpoint);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey).toUtf8());

    reply_ = nam_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    emit requestStarted();
    connect(reply_, &QNetworkReply::finished, this, [this] { onFinished(reply_); });
}

void AiExplainService::onFinished(QNetworkReply* reply) {
    reply_ = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit requestFailed(reply->errorString());
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    const QJsonObject root = doc.object();
    const QString content = root
        .value(QStringLiteral("choices")).toArray()
        .first().toObject()
        .value(QStringLiteral("message")).toObject()
        .value(QStringLiteral("content")).toString();

    if (content.isEmpty()) {
        emit requestFailed(QStringLiteral("AI 返回了空响应，请重试"));
        return;
    }

    AiUsage usage;
    const QJsonObject usageObj = root.value(QStringLiteral("usage")).toObject();
    usage.promptTokens = usageObj.value(QStringLiteral("prompt_tokens")).toInt();
    usage.completionTokens = usageObj.value(QStringLiteral("completion_tokens")).toInt();

    emit responseReady(QStringLiteral("<div>%1</div>").arg(content), usage);
}

}  // namespace quizpane
