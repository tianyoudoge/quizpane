#include "ai_explain_service.hpp"

#include <QBuffer>
#include <QFile>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace quizpane {

namespace {

const QRegularExpression kImageTag(QStringLiteral("<img\\b[^>]*>"),
                                   QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kImageSource(QStringLiteral("\\bsrc\\s*=\\s*(['\"])(.*?)\\1"),
                                      QRegularExpression::CaseInsensitiveOption);

QString withoutImagePaths(QString html) {
    return html.replace(kImageTag, QStringLiteral("【图片见附件】"));
}

struct ImageRef { QString label; QString url; };

bool appendHtmlImages(const QString& html, const QString& label, QList<ImageRef>* images) {
    auto match = kImageTag.globalMatch(html);
    while (match.hasNext()) {
        const auto source = kImageSource.match(match.next().captured());
        if (!source.hasMatch() || source.captured(2).isEmpty()) return false;
        images->append({label, source.captured(2)});
    }
    return true;
}

QString visionModel(const QString& requested, bool hasImages) {
    const QString model = requested.isEmpty() ? QStringLiteral("deepseek-chat") : requested;
    if (!hasImages) return model;
    static const QHash<QString, QString> replacements{
        {QStringLiteral("deepseek-chat"), QStringLiteral("deepseek-flash")},
        {QStringLiteral("qwen-plus"), QStringLiteral("qwen3-vl-plus")},
        {QStringLiteral("doubao-pro-4k"), QStringLiteral("doubao-seed-1.6-vision")},
        {QStringLiteral("glm-4-flash"), QStringLiteral("glm-4.5v")},
        {QStringLiteral("moonshot-v1-8k"), QStringLiteral("kimi-k2.6")},
    };
    return replacements.value(model, model);
}

static QString buildPrompt(const QString& questionHtml, const QJsonArray& options,
                           const QString& materialHtml,
                           const QString& correctAnswer) {
    QString optionLines;
    for (const auto& val : options) {
        const auto opt = val.toObject();
        optionLines += QStringLiteral("%1. %2\n")
            .arg(opt.value(QStringLiteral("label")).toString(),
                 withoutImagePaths(opt.value(QStringLiteral("contentHtml")).toString()));
    }
    return QStringLiteral(
        "你是一位专业考试辅导老师。请对以下题目进行解析。\n"
        "格式要求：\n"
        "- 用 Markdown 排版：重点用 **加粗**，并列要点用以 \"- \" 开头的列表，段落之间空一行；"
        "不要使用表格、标题（#）或 HTML 标签\n"
        "- 所有数学内容都写成 LaTeX 并用 $ 包裹，包括单个变量和数值算式，例如 $a$、"
        "$\\frac{A}{B}\\times\\frac{a-b}{1+a}$、$\\frac{6240-5800}{5800}\\times100\\%\\approx7.59\\%$；"
        "较长的关键推导单独成行并用 $$ 包裹\n"
        "- 分数一律用 \\frac{分子}{分母}，乘号用 \\times，除号用 \\div，不要用 / 或 * 表示运算\n"
        "- 按以下四个部分输出，每个部分标题单独一行并用【】包裹：\n"
        "  【答案确认】正确选项及简要理由\n"
        "  【解题步骤】分步骤详细推导\n"
        "  【知识点】涉及的核心知识点\n"
        "  【速算技巧】如有速算方法或公式推导请列出，否则省略此部分\n\n"
        "材料：\n%4\n\n"
        "题目：\n%1\n\n"
        "选项：\n%2\n"
        "标准答案：%3\n"
        "请结合题目文字及附带的材料、题干和选项图片独立推导。"
        "不同分类口径的统计量不可直接视为同一范围；若材料只能确定上界或下界，"
        "必须用不等式判断选项，不得把界限写成精确值。"
        "若标准答案为未提供，不要声称已核对标准答案。"
    ).arg(withoutImagePaths(questionHtml), optionLines,
          correctAnswer.isEmpty() ? QStringLiteral("未提供") : correctAnswer,
          materialHtml.isEmpty() ? QStringLiteral("无") : withoutImagePaths(materialHtml));
}

}  // namespace

AiExplainService::AiExplainService(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

void AiExplainService::cancel() {
    if (!reply_) return;
    QNetworkReply* oldReply = reply_;
    reply_ = nullptr;
    oldReply->abort();
    oldReply->deleteLater();
}

void AiExplainService::explain(const QString& questionHtml, const QJsonArray& options,
                                const QString& materialHtml, const QJsonArray& materialImageUrls,
                                const QString& correctAnswer,
                                const QString& apiKey, const QString& baseUrl,
                                const QString& model) {
    cancel();

    QList<ImageRef> images;
    for (const QJsonValue& url : materialImageUrls) {
        if (!url.isString() || url.toString().isEmpty()) {
            emit requestFailed(QStringLiteral("材料图片地址无效，未发送解析请求"));
            return;
        }
        images.append({QStringLiteral("材料图片"), url.toString()});
    }
    if (!appendHtmlImages(materialHtml, QStringLiteral("材料内图片"), &images) ||
        !appendHtmlImages(questionHtml, QStringLiteral("题干图片"), &images)) {
        emit requestFailed(QStringLiteral("题目图片地址无法解析，未发送解析请求"));
        return;
    }
    for (const QJsonValue& value : options) {
        const QJsonObject option = value.toObject();
        const QString label = option.value(QStringLiteral("label")).toString();
        if (option.contains(QStringLiteral("imageUrl")) &&
            !option.value(QStringLiteral("imageUrl")).isString()) {
            emit requestFailed(QStringLiteral("选项图片地址无效，未发送解析请求"));
            return;
        }
        const QString imageUrl = option.value(QStringLiteral("imageUrl")).toString();
        if (!imageUrl.isEmpty()) images.append({QStringLiteral("选项 %1 图片").arg(label), imageUrl});
        if (!appendHtmlImages(option.value(QStringLiteral("contentHtml")).toString(),
                              QStringLiteral("选项 %1 图片").arg(label), &images)) {
            emit requestFailed(QStringLiteral("选项图片地址无法解析，未发送解析请求"));
            return;
        }
    }

    const QString prompt = buildPrompt(questionHtml, options, materialHtml, correctAnswer);
    QJsonArray contentParts{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                        {QStringLiteral("text"), prompt}}};
    QSet<QString> seen;
    qint64 totalBytes = 0;
    for (const ImageRef& image : images) {
        if (seen.contains(image.url)) continue;
        seen.insert(image.url);
        const QUrl url(image.url);
        if (!url.isLocalFile()) {
            emit requestFailed(QStringLiteral("无法读取%1：仅支持题库中的本地图片").arg(image.label));
            return;
        }
        QFile file(url.toLocalFile());
        if (!file.open(QIODevice::ReadOnly) || file.size() > 24 * 1024 * 1024) {
            emit requestFailed(QStringLiteral("无法读取%1或图片过大").arg(image.label));
            return;
        }
        const QByteArray bytes = file.readAll();
        totalBytes += bytes.size();
        if (totalBytes > 30 * 1024 * 1024) {
            emit requestFailed(QStringLiteral("当前题目的图片总量过大，未发送解析请求"));
            return;
        }
        const QString mime = QMimeDatabase().mimeTypeForData(bytes).name();
        if (mime != QStringLiteral("image/png") && mime != QStringLiteral("image/jpeg") &&
            mime != QStringLiteral("image/webp") && mime != QStringLiteral("image/gif")) {
            emit requestFailed(QStringLiteral("%1不是支持的图片格式").arg(image.label));
            return;
        }
        QBuffer buffer;
        buffer.setData(bytes);
        buffer.open(QIODevice::ReadOnly);
        if (!QImageReader(&buffer).canRead()) {
            emit requestFailed(QStringLiteral("%1已损坏，未发送解析请求").arg(image.label));
            return;
        }
        const QString dataUrl = QStringLiteral("data:%1;base64,%2")
            .arg(mime, QString::fromLatin1(bytes.toBase64()));
        contentParts.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                        {QStringLiteral("text"), image.label}});
        contentParts.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                                        {QStringLiteral("image_url"), QJsonObject{
                                            {QStringLiteral("url"), dataUrl}}}});
    }
    const QString effectiveModel = visionModel(model, !seen.isEmpty());
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
                {QStringLiteral("content"), seen.isEmpty() ? QJsonValue(prompt)
                                                      : QJsonValue(contentParts)}
            }
        }},
        {QStringLiteral("temperature"), 0.3},
        // DeepSeek's output budget includes reasoning tokens; 128K is supported by
        // deepseek-flash. Keep the smaller cap for other OpenAI-compatible providers.
        {QStringLiteral("max_tokens"), effectiveModel.startsWith(QStringLiteral("deepseek-"))
                                             ? 131072 : seen.isEmpty() ? 2048 : 8192}
    };

    QUrl url(endpoint);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey).toUtf8());

    reply_ = nam_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    emit requestStarted();
    connect(reply_, &QNetworkReply::finished, this, [this, reply = reply_] {
        if (reply != reply_) return;
        onFinished(reply);
    });
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
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    const QString content = (choices.isEmpty() ? QJsonObject() : choices.first().toObject())
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

    emit responseReady(content, usage);
}

}  // namespace quizpane
