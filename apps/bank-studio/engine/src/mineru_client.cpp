#include "quizpane/studio/mineru_client.hpp"

#include "quizpane/diagnostic_logger.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QUrl>

namespace quizpane::studio {
namespace {

constexpr int kPollIntervalMs = 3000;
// 上限约 15 分钟。超时后显式失败，而不是无限等待一个可能已经卡住的任务。
constexpr int kMaxPollAttempts = 300;
constexpr int kMaxTransientRetries = 4;
// 结果 ZIP 上限。官方单文件限 200MB，输出包含切图与原始 PDF，留出余量。
constexpr qint64 kMaxZipBytes = 512LL * 1024 * 1024;

// MinerU 的错误信封有两种形状：鉴权类返回 {msgCode, msg, success}，任务类返回
// {code, msg}。两种都要认，否则用户只能看到一个空洞的 HTTP 状态码。
QString extractApiError(const QJsonObject& object, int httpStatus, const QString& networkError) {
    const QString message = object.value(QStringLiteral("msg")).toString().trimmed();
    const QString msgCode = object.value(QStringLiteral("msgCode")).toString().trimmed();
    const int code = object.value(QStringLiteral("code")).toInt(0);

    // 把官方错误码翻译成可操作的提示。用户看到"A0202"无从下手，看到"Token 无效"
    // 就知道该去哪里改。
    if (msgCode == QStringLiteral("A0202"))
        return QStringLiteral("MinerU Token 无效，请在设置中重新填写。");
    if (msgCode == QStringLiteral("A0211"))
        return QStringLiteral("MinerU Token 已过期，请到 mineru.net 重新生成后更新设置。");
    if (code == -60005 || code == -60006)
        return QStringLiteral("文档超出 MinerU 限制（单文件最大 200 MB、600 页）。");
    if (code == -60012)
        return QStringLiteral("MinerU 任务不存在或已过期，请重新提交解析。");
    if (code == -60018)
        return QStringLiteral("今日 MinerU 解析额度已用尽，请稍后再试或改用本地解析。");
    if (httpStatus == 429)
        return QStringLiteral("MinerU 请求过于频繁，请稍后重试。");

    if (!message.isEmpty()) {
        const QString identifier = msgCode.isEmpty()
                                       ? (code != 0 ? QString::number(code) : QString())
                                       : msgCode;
        return identifier.isEmpty()
                   ? QStringLiteral("MinerU 返回错误：%1").arg(message)
                   : QStringLiteral("MinerU 返回错误：%1（%2）").arg(message, identifier);
    }
    if (!networkError.isEmpty())
        return networkError;
    return QStringLiteral("MinerU 请求失败（HTTP %1）").arg(httpStatus);
}

// 判断响应是否为失败。不能只看 HTTP 状态：鉴权失败也可能带 200 而在体里用
// success=false 表达。
bool isFailureEnvelope(const QJsonObject& object, int httpStatus, const QString& networkError) {
    if (!networkError.isEmpty() || httpStatus >= 400)
        return true;
    if (object.contains(QStringLiteral("success")) &&
        !object.value(QStringLiteral("success")).toBool(true))
        return true;
    // code 字段 0 表示成功；负值是官方错误码。
    return object.contains(QStringLiteral("code")) &&
           object.value(QStringLiteral("code")).toInt(0) != 0;
}

} // namespace

bool isTransientMineruFailure(int httpStatus, int networkErrorCode) {
    // QNetworkReply::OperationCanceledError == 5，用户取消绝不能被自动重试。
    if (networkErrorCode == static_cast<int>(QNetworkReply::OperationCanceledError))
        return false;
    if (httpStatus == 408 || httpStatus == 425 || httpStatus == 429 || httpStatus >= 500)
        return true;
    // 没拿到 HTTP 响应的连接中断、DNS、超时等通常是暂时故障；已有明确的
    // 4xx 响应则属于请求/权限问题，直接把服务端提示交给用户。
    return networkErrorCode != static_cast<int>(QNetworkReply::NoError) && httpStatus == 0;
}

int mineruRetryDelayMs(int retryAttempt, int retryAfterSeconds) {
    if (retryAfterSeconds > 0)
        return qBound(1000, retryAfterSeconds * 1000, 60000);
    const int exponent = qBound(0, retryAttempt - 1, 5);
    return qMin(30000, 1000 * (1 << exponent));
}

QString describeMineruStage(MineruStage stage) {
    switch (stage) {
    case MineruStage::Idle:
        return QStringLiteral("待开始");
    case MineruStage::RequestingUploadUrl:
        return QStringLiteral("正在申请上传链接");
    case MineruStage::Uploading:
        return QStringLiteral("正在上传文档");
    case MineruStage::Polling:
        return QStringLiteral("云端解析中");
    case MineruStage::Downloading:
        return QStringLiteral("正在下载解析结果");
    case MineruStage::Done:
        return QStringLiteral("解析完成");
    case MineruStage::Failed:
        return QStringLiteral("解析失败");
    case MineruStage::Cancelled:
        return QStringLiteral("已取消");
    }
    return QStringLiteral("未知状态");
}

QJsonObject buildUploadUrlRequestBody(const MineruSettings& settings, const QString& fileName) {
    QJsonObject file{{QStringLiteral("name"), fileName}};
    if (settings.isOcr)
        file.insert(QStringLiteral("is_ocr"), true);
    return {{QStringLiteral("enable_formula"), settings.enableFormula},
            {QStringLiteral("enable_table"), settings.enableTable},
            {QStringLiteral("language"), settings.language},
            {QStringLiteral("model_version"), settings.modelVersion},
            {QStringLiteral("files"), QJsonArray{file}}};
}

QNetworkRequest buildMineruRequest(const MineruSettings& settings, const QString& path,
                                   QString* error) {
    if (settings.token.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("请先在设置中填写 MinerU Token");
        return {};
    }
    QString base = settings.baseUrl.trimmed();
    while (base.endsWith(QChar(u'/')))
        base.chop(1);
    const QUrl url(base + path);
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
        if (error)
            *error = QStringLiteral("MinerU 服务地址格式不正确");
        return {};
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("QuizPane-Question-Maker"));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization", "Bearer " + settings.token.trimmed().toUtf8());
    request.setTransferTimeout(60000);
    return request;
}

MineruUploadTicket parseUploadUrlResponse(const QByteArray& payload, int httpStatus,
                                          const QString& networkError) {
    MineruUploadTicket ticket;
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    const QJsonObject object = document.object();
    if (isFailureEnvelope(object, httpStatus, networkError)) {
        ticket.error = extractApiError(object, httpStatus, networkError);
        return ticket;
    }
    if (!document.isObject()) {
        ticket.error = QStringLiteral("MinerU 响应不是有效 JSON：%1").arg(parseError.errorString());
        return ticket;
    }
    const QJsonObject data = object.value(QStringLiteral("data")).toObject();
    ticket.batchId = data.value(QStringLiteral("batch_id")).toString();
    const QJsonArray urls = data.value(QStringLiteral("file_urls")).toArray();
    if (!urls.isEmpty())
        ticket.uploadUrl = urls.first().toString();
    if (ticket.batchId.isEmpty() || ticket.uploadUrl.isEmpty())
        ticket.error = QStringLiteral("MinerU 未返回上传链接");
    return ticket;
}

MineruPollResult parsePollResponse(const QByteArray& payload, int httpStatus,
                                  const QString& networkError) {
    MineruPollResult result;
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    const QJsonObject object = document.object();
    if (isFailureEnvelope(object, httpStatus, networkError)) {
        result.error = extractApiError(object, httpStatus, networkError);
        result.finished = true;
        return result;
    }
    if (!document.isObject()) {
        result.error = QStringLiteral("MinerU 响应不是有效 JSON：%1").arg(parseError.errorString());
        result.finished = true;
        return result;
    }
    const QJsonArray results =
        object.value(QStringLiteral("data")).toObject().value(QStringLiteral("extract_result")).toArray();
    if (results.isEmpty()) {
        // 批次刚创建时可能还没有条目。这不是错误，继续轮询。
        result.state = QStringLiteral("pending");
        return result;
    }
    const QJsonObject entry = results.first().toObject();
    result.state = entry.value(QStringLiteral("state")).toString();
    result.zipUrl = entry.value(QStringLiteral("full_zip_url")).toString();
    const QJsonObject extractProgress =
        entry.value(QStringLiteral("extract_progress")).toObject();
    result.extractedPages = extractProgress.value(QStringLiteral("extracted_pages")).toInt();
    result.totalPages = extractProgress.value(QStringLiteral("total_pages")).toInt();

    if (result.state == QStringLiteral("failed")) {
        const QString reason = entry.value(QStringLiteral("err_msg")).toString().trimmed();
        result.error = reason.isEmpty() ? QStringLiteral("MinerU 解析失败")
                                       : QStringLiteral("MinerU 解析失败：%1").arg(reason);
        result.finished = true;
        return result;
    }
    if (result.state == QStringLiteral("done")) {
        if (result.zipUrl.isEmpty()) {
            result.error = QStringLiteral("MinerU 解析完成但未返回结果下载地址");
        }
        result.finished = true;
    }
    return result;
}

MineruExtractionJob::MineruExtractionJob(QNetworkAccessManager* manager, QObject* parent)
    : QObject(parent), manager_(manager) {}

void MineruExtractionJob::setStage(MineruStage stage, const QString& detail) {
    stage_ = stage;
    emit stageChanged(stage, detail.isEmpty() ? describeMineruStage(stage) : detail);
}

void MineruExtractionJob::clearReply() {
    if (!reply_)
        return;
    disconnect(reply_, nullptr, this, nullptr);
    reply_->abort();
    reply_->deleteLater();
    reply_ = nullptr;
}

void MineruExtractionJob::failWith(const QString& error) {
    clearReply();
    setStage(MineruStage::Failed, error);
    diagnostic::event(QStringLiteral("mineru"), QStringLiteral("job-failed"),
                      {{QStringLiteral("error"), error}});
    // 排到事件循环再发：start() 里的前置校验（缺文件、缺 Token）如果同步发信号，
    // 调用方还没来得及连接就已经错过了结果。与 ModelClient 的前置失败同一处理。
    const quint64 generation = generation_;
    QMetaObject::invokeMethod(this, [this, error, generation] {
        if (generation == generation_)
            emit finished(false, QString(), error);
    }, Qt::QueuedConnection);
}

bool MineruExtractionJob::retryTransient(MineruStage stage, int httpStatus,
                                         int networkErrorCode, int retryAfterSeconds,
                                         const QString& detail,
                                         const std::function<void()>& action) {
    if (!isTransientMineruFailure(httpStatus, networkErrorCode) ||
        transientRetryAttempts_ >= kMaxTransientRetries)
        return false;
    const int attempt = ++transientRetryAttempts_;
    const int delay = mineruRetryDelayMs(attempt, retryAfterSeconds);
    setStage(stage, QStringLiteral("%1，%2 秒后重试（%3/%4）")
                        .arg(detail)
                        .arg((delay + 999) / 1000)
                        .arg(attempt)
                        .arg(kMaxTransientRetries));
    const quint64 generation = generation_;
    QTimer::singleShot(delay, this, [this, generation, stage, action] {
        if (generation == generation_ && stage_ == stage)
            action();
    });
    return true;
}

void MineruExtractionJob::start(const MineruSettings& settings, const QString& sourcePath,
                                const QString& outputZipPath) {
    clearReply();
    ++generation_;
    stage_ = MineruStage::Idle;
    settings_ = settings;
    sourcePath_ = sourcePath;
    outputZipPath_ = outputZipPath;
    batchId_.clear();
    pollAttempts_ = 0;
    transientRetryAttempts_ = 0;

    const QFileInfo info(sourcePath_);
    if (!info.exists() || !info.isFile()) {
        failWith(QStringLiteral("找不到要解析的文件：%1").arg(sourcePath_));
        return;
    }
    // 诊断日志刻意不记录文件名与 Token：原文属于用户材料，凭据属于机密。
    diagnostic::event(QStringLiteral("mineru"), QStringLiteral("job-start"),
                      {{QStringLiteral("modelVersion"), settings_.modelVersion},
                       {QStringLiteral("isOcr"), settings_.isOcr},
                       {QStringLiteral("fileBytes"), info.size()}});
    requestUploadUrl();
}

void MineruExtractionJob::requestUploadUrl() {
    setStage(MineruStage::RequestingUploadUrl);
    QString error;
    const QNetworkRequest request =
        buildMineruRequest(settings_, QStringLiteral("/api/v4/file-urls/batch"), &error);
    if (!error.isEmpty()) {
        failWith(error);
        return;
    }
    const QJsonObject body =
        buildUploadUrlRequestBody(settings_, QFileInfo(sourcePath_).fileName());
    reply_ = manager_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QNetworkReply* current = reply_;
    connect(current, &QNetworkReply::finished, this, [this, current] {
        if (reply_ != current)
            return;
        reply_ = nullptr;
        const int status = current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString networkError =
            current->error() == QNetworkReply::NoError ? QString() : current->errorString();
        const int networkErrorCode = static_cast<int>(current->error());
        const int retryAfter = current->rawHeader("Retry-After").toInt();
        const MineruUploadTicket ticket =
            parseUploadUrlResponse(current->readAll(), status, networkError);
        current->deleteLater();
        if (!ticket.error.isEmpty()) {
            if (retryTransient(MineruStage::RequestingUploadUrl, status, networkErrorCode,
                               retryAfter, QStringLiteral("申请上传链接暂时失败"),
                               [this] { requestUploadUrl(); }))
                return;
            failWith(ticket.error);
            return;
        }
        batchId_ = ticket.batchId;
        uploadFile(ticket);
    });
}

void MineruExtractionJob::uploadFile(const MineruUploadTicket& ticket) {
    setStage(MineruStage::Uploading);
    auto* file = new QFile(sourcePath_, this);
    if (!file->open(QIODevice::ReadOnly)) {
        const QString error = QStringLiteral("无法读取文件：%1").arg(file->errorString());
        file->deleteLater();
        failWith(error);
        return;
    }
    // 官方要求 PUT 到预签名链接时不要带 Content-Type，否则签名校验失败。
    QNetworkRequest request(QUrl(ticket.uploadUrl));
    request.setTransferTimeout(600000);
    reply_ = manager_->put(request, file);
    QNetworkReply* current = reply_;
    // QFile 的生命周期挂在 reply 上，而不是下面的 finished lambda 上。
    // cancel() 会 disconnect 这个 lambda，若由它负责关闭文件，取消上传就会
    // 泄漏文件句柄——Windows 上还会一直锁住用户的原始文档。
    file->setParent(current);
    connect(current, &QObject::destroyed, file, [file] { file->close(); });
    connect(current, &QNetworkReply::finished, this, [this, current] {
        if (reply_ != current)
            return;
        reply_ = nullptr;
        const int status = current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = current->error() == QNetworkReply::NoError && status < 400;
        const QString networkError = current->errorString();
        const int networkErrorCode = static_cast<int>(current->error());
        const int retryAfter = current->rawHeader("Retry-After").toInt();
        current->deleteLater();
        if (!ok) {
            if (retryTransient(MineruStage::Uploading, status, networkErrorCode, retryAfter,
                               QStringLiteral("上传暂时中断"),
                               [this] { requestUploadUrl(); }))
                return;
            failWith(QStringLiteral("上传文档失败（HTTP %1）：%2").arg(status).arg(networkError));
            return;
        }
        transientRetryAttempts_ = 0;
        // 上传完成即自动开始解析，无需再调提交接口。
        setStage(MineruStage::Polling);
        schedulePoll();
    });
}

void MineruExtractionJob::schedulePoll() {
    // 解析时间越长，轮询越稀疏，降低大文件对官方接口的持续压力；前五次仍保持
    // 3 秒以兼顾普通十页卷的完成体感，之后逐级退避，最多 30 秒。
    const int exponent = qBound(0, pollAttempts_ / 5, 3);
    const int delay = qMin(30000, kPollIntervalMs * (1 << exponent));
    const quint64 generation = generation_;
    QTimer::singleShot(delay, this, [this, generation] {
        if (generation == generation_ && stage_ == MineruStage::Polling)
            poll();
    });
}

void MineruExtractionJob::poll() {
    if (++pollAttempts_ > kMaxPollAttempts) {
        failWith(QStringLiteral("MinerU 解析超时，请稍后重试或改用本地解析。"));
        return;
    }
    QString error;
    const QNetworkRequest request = buildMineruRequest(
        settings_, QStringLiteral("/api/v4/extract-results/batch/") + batchId_, &error);
    if (!error.isEmpty()) {
        failWith(error);
        return;
    }
    reply_ = manager_->get(request);
    QNetworkReply* current = reply_;
    connect(current, &QNetworkReply::finished, this, [this, current] {
        if (reply_ != current)
            return;
        reply_ = nullptr;
        const int status = current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString networkError =
            current->error() == QNetworkReply::NoError ? QString() : current->errorString();
        const int networkErrorCode = static_cast<int>(current->error());
        const int retryAfter = current->rawHeader("Retry-After").toInt();
        const MineruPollResult result = parsePollResponse(current->readAll(), status, networkError);
        current->deleteLater();

        if (retryTransient(MineruStage::Polling, status, networkErrorCode, retryAfter,
                           QStringLiteral("查询解析进度暂时失败"), [this] { poll(); }))
            return;
        transientRetryAttempts_ = 0;
        if (result.totalPages > 0)
            emit progress(result.extractedPages, result.totalPages);
        if (!result.error.isEmpty()) {
            failWith(result.error);
            return;
        }
        if (result.finished && !result.zipUrl.isEmpty()) {
            download(result.zipUrl);
            return;
        }
        schedulePoll();
    });
}

void MineruExtractionJob::download(const QString& zipUrl) {
    setStage(MineruStage::Downloading);
    const QUrl url(zipUrl);
    if (!url.isValid() || url.scheme().isEmpty()) {
        failWith(QStringLiteral("MinerU 返回的结果地址无效"));
        return;
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(600000);
    reply_ = manager_->get(request);
    QNetworkReply* current = reply_;
    connect(current, &QNetworkReply::finished, this, [this, current, zipUrl] {
        if (reply_ != current)
            return;
        reply_ = nullptr;
        const int status = current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = current->error() == QNetworkReply::NoError && status < 400;
        const QString networkError = current->errorString();
        const int networkErrorCode = static_cast<int>(current->error());
        const int retryAfter = current->rawHeader("Retry-After").toInt();
        const QByteArray payload = current->readAll();
        current->deleteLater();
        if (!ok) {
            if (retryTransient(MineruStage::Downloading, status, networkErrorCode, retryAfter,
                               QStringLiteral("下载结果暂时失败"),
                               [this, zipUrl] { download(zipUrl); }))
                return;
            failWith(QStringLiteral("下载解析结果失败（HTTP %1）：%2").arg(status).arg(networkError));
            return;
        }
        transientRetryAttempts_ = 0;
        if (payload.isEmpty()) {
            failWith(QStringLiteral("MinerU 返回了空的结果包"));
            return;
        }
        if (payload.size() > kMaxZipBytes) {
            failWith(QStringLiteral("MinerU 结果包超出体积上限，已拒绝写入"));
            return;
        }
        QDir().mkpath(QFileInfo(outputZipPath_).absolutePath());
        QFile output(outputZipPath_);
        if (!output.open(QIODevice::WriteOnly) || output.write(payload) != payload.size()) {
            failWith(QStringLiteral("无法写出解析结果：%1").arg(output.errorString()));
            return;
        }
        output.close();
        setStage(MineruStage::Done);
        diagnostic::event(QStringLiteral("mineru"), QStringLiteral("job-done"),
                          {{QStringLiteral("zipBytes"), payload.size()}});
        emit finished(true, outputZipPath_, QString());
    });
}

void MineruExtractionJob::cancel() {
    if (stage_ == MineruStage::Idle || stage_ == MineruStage::Done ||
        stage_ == MineruStage::Failed || stage_ == MineruStage::Cancelled) {
        clearReply();
        return;
    }
    clearReply();
    ++generation_;
    setStage(MineruStage::Cancelled);
    diagnostic::event(QStringLiteral("mineru"), QStringLiteral("job-cancelled"));
    // 同 failWith：取消可能紧跟 start() 发生，异步发信号才能被可靠接收。
    const quint64 generation = generation_;
    QMetaObject::invokeMethod(this, [this, generation] {
        if (generation == generation_)
            emit finished(false, QString(), QStringLiteral("已取消云解析"));
    }, Qt::QueuedConnection);
}

} // namespace quizpane::studio
