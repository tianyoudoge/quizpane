#include "quizpane/studio/mineru_client.hpp"

#include "quizpane/diagnostic_logger.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <memory>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QUrl>
#include <QtGlobal>

namespace quizpane::studio {
namespace {

constexpr int kPollIntervalMs = 3000;
constexpr int kMaxTransientRetries = 4;
// 结果 ZIP 上限。官方单文件限 200MB，输出包含切图与原始 PDF，留出余量。
constexpr qint64 kMaxZipBytes = 512LL * 1024 * 1024;

// MinerU 的错误信封有两种形状：鉴权类返回 {msgCode, msg, success}，任务类返回
// {code, msg}。两种都要认，否则用户只能看到一个空洞的 HTTP 状态码。
QString extractApiError(const QJsonObject& object, int httpStatus, const QString& networkError) {
    const QString msgCode = object.value(QStringLiteral("msgCode")).toString().trimmed();
    const int code = object.value(QStringLiteral("code")).toInt(0);

    // 把官方错误码翻译成可操作的提示。用户看到"A0202"无从下手，看到"Token 无效"
    // 就知道该去哪里改。
    if (msgCode == QStringLiteral("A0202"))
        return QStringLiteral("MinerU Token 无效，请在设置中重新填写。");
    if (msgCode == QStringLiteral("A0211"))
        return QStringLiteral("MinerU Token 已过期，请到 mineru.net 重新生成后更新设置。");
    if (code == -60005 || code == -60006)
        return QStringLiteral("文件不符合云端解析限制，请缩小文件或拆分后重试。");
    if (code == -60012)
        return QStringLiteral("MinerU 任务不存在或已过期，请重新提交解析。");
    if (code == -60018)
        return QStringLiteral("今日 MinerU 解析额度已用尽，请稍后再试或改用本地解析。");
    if (code == -60013)
        return QStringLiteral("当前账号无法访问这项云端任务，请重新开始整理。");
    if (code == -60010 || code == -60015 || code == -60016)
        return QStringLiteral("云端未能完成这份文件的解析，请检查文件后重试。");
    Q_UNUSED(httpStatus)
    Q_UNUSED(networkError)
    // 服务端 message / QNetworkReply::errorString() 可能含内部地址、请求细节或
    // 原始文件信息。它们只进入诊断日志，绝不直接呈现给用户。
    return QStringLiteral("云端服务暂时不可用，请稍后再试。");
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
        Q_UNUSED(parseError)
        ticket.error = QStringLiteral("云端服务返回异常，请稍后再试。");
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
        Q_UNUSED(parseError)
        result.error = QStringLiteral("云端服务返回异常，请稍后再试。");
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
        const int code = entry.value(QStringLiteral("err_code")).toInt(0);
        result.error = code != 0
            ? extractApiError(QJsonObject{{QStringLiteral("code"), code}}, 200, {})
            : QStringLiteral("云端未能完成这份文件的解析，请检查文件后重试。");
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
    weakNetworkAttempts_ = 0;

    const QFileInfo info(sourcePath_);
    if (!info.exists() || !info.isFile()) {
        failWith(QStringLiteral("找不到要解析的文件。请确认文件仍在原位置后重试。"));
        return;
    }
    // 诊断日志刻意不记录文件名与 Token：原文属于用户材料，凭据属于机密。
    diagnostic::event(QStringLiteral("mineru"), QStringLiteral("job-start"),
                      {{QStringLiteral("modelVersion"), settings_.modelVersion},
                       {QStringLiteral("isOcr"), settings_.isOcr},
                       {QStringLiteral("fileBytes"), info.size()}});
    requestUploadUrl();
}

void MineruExtractionJob::resume(const MineruSettings& settings, const QString& batchId,
                                 const QString& outputZipPath) {
    clearReply();
    ++generation_;
    settings_ = settings;
    outputZipPath_ = outputZipPath;
    sourcePath_.clear();
    batchId_ = batchId.trimmed();
    pollAttempts_ = 0;
    transientRetryAttempts_ = 0;
    weakNetworkAttempts_ = 0;
    if (settings_.token.trimmed().isEmpty()) {
        failWith(QStringLiteral("请先在设置中填写 MinerU Token"));
        return;
    }
    if (batchId_.isEmpty()) {
        failWith(QStringLiteral("未找到可恢复的云端任务，请重新开始整理。"));
        return;
    }
    setStage(MineruStage::Polling, QStringLiteral("已恢复云端任务，正在查询进度"));
    poll();
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
        file->deleteLater();
        failWith(QStringLiteral("无法读取文件，请确认文件可访问后重试。"));
        return;
    }
    // 官方要求 PUT 到预签名链接时不要带 Content-Type，否则签名校验失败。
    QNetworkRequest request(QUrl(ticket.uploadUrl));
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    // MinerU 返回的是 OSS 预签名地址。实际环境中 HTTP/2 连接会偶发被边缘节点
    // 主动关闭（HTTP 0 / Connection closed）；改用兼容性更好的 HTTP/1.1，并允许
    // HTTPS 间的安全跳转。与官方 curl 示例及本机端到端实测保持一致。
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
#endif
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
            Q_UNUSED(networkError)
            failWith(QStringLiteral("文件没有上传完成，请检查网络后重新开始整理。"));
            return;
        }
        transientRetryAttempts_ = 0;
        // 上传完成即自动开始解析，无需再调提交接口。
        emit taskSubmitted(batchId_);
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
    ++pollAttempts_;
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

        // 轮询是可恢复任务的生命线：弱网下的偶发超时、限流和 5xx 只提示并延后
        // 重试，绝不能把本地流程判定为失败。真正 state=failed 才结束任务。
        if (isTransientMineruFailure(status, networkErrorCode)) {
            const int delay = mineruRetryDelayMs(++weakNetworkAttempts_, retryAfter);
            setStage(MineruStage::Polling,
                     QStringLiteral("网络不稳定，%1 秒后继续查询（不会丢失任务）")
                         .arg((delay + 999) / 1000));
            const quint64 generation = generation_;
            QTimer::singleShot(delay, this, [this, generation] {
                if (generation == generation_ && stage_ == MineruStage::Polling)
                    poll();
            });
            return;
        }
        transientRetryAttempts_ = 0;
        weakNetworkAttempts_ = 0;
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
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    // 结果 ZIP 也走预签名 OSS 地址，必须与上传保持相同的传输兼容策略。
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
#endif
    request.setTransferTimeout(600000);
    struct DownloadState {
        explicit DownloadState(const QString& path) : output(path) {}
        QSaveFile output;
        qint64 bytes = 0;
        QString error;
    };
    QDir().mkpath(QFileInfo(outputZipPath_).absolutePath());
    const auto state = std::make_shared<DownloadState>(outputZipPath_);
    if (!state->output.open(QIODevice::WriteOnly)) {
        failWith(QStringLiteral("无法保存解析结果，请检查本机存储空间后重试。"));
        return;
    }
    reply_ = manager_->get(request);
    QNetworkReply* current = reply_;
    current->setReadBufferSize(256 * 1024);
    const auto drain = [current, state] {
        if (!state->error.isEmpty())
            return;
        while (current->bytesAvailable() > 0) {
            const QByteArray chunk = current->read(64 * 1024);
            if (chunk.isEmpty())
                break;
            if (chunk.size() > kMaxZipBytes - state->bytes) {
                state->error = QStringLiteral("MinerU 结果包超出体积上限，已停止下载");
                state->output.cancelWriting();
                current->abort();
                return;
            }
            if (state->output.write(chunk) != chunk.size()) {
                state->error = QStringLiteral("无法保存解析结果，请检查本机存储空间后重试。");
                state->output.cancelWriting();
                current->abort();
                return;
            }
            state->bytes += chunk.size();
        }
    };
    connect(current, &QNetworkReply::metaDataChanged, this, [current, state] {
        if (current->header(QNetworkRequest::ContentLengthHeader).toLongLong() > kMaxZipBytes) {
            state->error = QStringLiteral("MinerU 结果包超出体积上限，已停止下载");
            state->output.cancelWriting();
            current->abort();
        }
    });
    connect(current, &QIODevice::readyRead, this, drain);
    connect(current, &QNetworkReply::finished, this, [this, current, state, drain] {
        if (reply_ != current)
            return;
        reply_ = nullptr;
        const int status = current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = current->error() == QNetworkReply::NoError && status < 400;
        const int networkErrorCode = static_cast<int>(current->error());
        const int retryAfter = current->rawHeader("Retry-After").toInt();
        if (ok)
            drain();
        current->deleteLater();
        if (!state->error.isEmpty()) {
            failWith(state->error);
            return;
        }
        if (!ok) {
            state->output.cancelWriting();
            if (retryTransient(MineruStage::Downloading, status, networkErrorCode, retryAfter,
                               QStringLiteral("下载结果暂时失败"), [this] { poll(); }))
                return;
            failWith(QStringLiteral("解析结果暂时无法下载，请稍后重新打开任务继续等待。"));
            return;
        }
        transientRetryAttempts_ = 0;
        if (state->bytes == 0) {
            state->output.cancelWriting();
            failWith(QStringLiteral("MinerU 返回了空的结果包"));
            return;
        }
        if (!state->output.commit()) {
            failWith(QStringLiteral("无法保存解析结果，请检查本机存储空间后重试。"));
            return;
        }
        setStage(MineruStage::Done);
        diagnostic::event(QStringLiteral("mineru"), QStringLiteral("job-done"),
                          {{QStringLiteral("zipBytes"), state->bytes}});
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
