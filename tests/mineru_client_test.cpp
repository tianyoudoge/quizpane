// MinerU 客户端协议测试。
//
// 两部分：
//   1. 纯函数：请求体、鉴权、两种错误信封、轮询状态解析。不触网。
//   2. 全链路状态机：用本地 QTcpServer 桩服务跑完 申请链接 → 上传 → 轮询 →
//      下载 的完整流程。真实 API 需要凭据且会消耗额度，不适合进 CI。

#include "quizpane/studio/mineru_client.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

// 极简 HTTP 桩服务。按请求路径返回预置响应，足以驱动客户端状态机。
class StubServer : public QTcpServer {
public:
    explicit StubServer(QObject* parent = nullptr) : QTcpServer(parent) {}

    int pollCount = 0;
    int uploadCount = 0;
    int uploadTicketRequestCount = 0;
    int transientTicketFailures = 0;
    int transientDownloadFailures = 0;
    int transientPollFailures = 0;
    int downloadCount = 0;
    int latestZipTicket = 0;
    QByteArray uploadedBody;
    QByteArray downloadPayload = "PK\x03\x04stub-zip-bytes";
    bool chunkedDownload = false;
    bool oversizedDownloadHeader = false;
    bool holdDownload = false;
    std::function<void()> onDownload;

protected:
    void incomingConnection(qintptr descriptor) override {
        auto* socket = new QTcpSocket(this);
        socket->setSocketDescriptor(descriptor);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            buffers_[socket].append(socket->readAll());
            QByteArray& buffer = buffers_[socket];
            const int headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;
            const QByteArray header = buffer.left(headerEnd);
            // 只在拿到完整请求体后作答，否则 PUT 上传会被截断。
            int contentLength = 0;
            for (const QByteArray& line : header.split('\r')) {
                const QByteArray trimmed = line.trimmed();
                if (trimmed.toLower().startsWith("content-length:"))
                    contentLength = trimmed.mid(trimmed.indexOf(':') + 1).trimmed().toInt();
            }
            const QByteArray body = buffer.mid(headerEnd + 4);
            if (body.size() < contentLength)
                return;
            respond(socket, header, body);
            buffers_.remove(socket);
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }

private:
    void respond(QTcpSocket* socket, const QByteArray& header, const QByteArray& body) {
        const QByteArray requestLine = header.left(header.indexOf('\r'));
        const bool authorized = header.toLower().contains("authorization: bearer test-token");

        auto send = [socket](const QByteArray& status, const QByteArray& payload,
                             const QByteArray& contentType = "application/json") {
            const QByteArray response = "HTTP/1.1 " + status + "\r\nContent-Type: " + contentType +
                                        "\r\nContent-Length: " +
                                        QByteArray::number(payload.size()) +
                                        "\r\nConnection: close\r\n\r\n" + payload;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
        };

        if (requestLine.contains("/api/v4/file-urls/batch")) {
            ++uploadTicketRequestCount;
            if (transientTicketFailures > 0) {
                --transientTicketFailures;
                send("503 Service Unavailable", R"({"msg":"busy"})");
                return;
            }
            if (!authorized) {
                // 官方鉴权失败用 {msgCode, msg, success} 信封，且可能带 200。
                send("200 OK",
                     R"({"traceId":"t","msgCode":"A0202","msg":"user authenticate failed",)"
                     R"("data":null,"success":false,"total":0})");
                return;
            }
            const QByteArray uploadUrl =
                "http://127.0.0.1:" + QByteArray::number(serverPort()) + "/upload/slot";
            send("200 OK", R"({"code":0,"msg":"ok","data":{"batch_id":"batch-1","file_urls":[")" +
                               uploadUrl + R"("]}})");
            return;
        }
        if (requestLine.startsWith("PUT") && requestLine.contains("/upload/slot")) {
            ++uploadCount;
            uploadedBody = body;
            send("200 OK", "");
            return;
        }
        if (requestLine.contains("/api/v4/extract-results/batch/batch-1")) {
            if (transientPollFailures > 0) {
                --transientPollFailures;
                send("503 Service Unavailable", R"({"msg":"temporary poll failure"})");
                return;
            }
            ++pollCount;
            if (pollCount < 2) {
                // 第一轮还在解析，带进度；客户端应继续轮询而不是报错。
                send("200 OK",
                     R"({"code":0,"data":{"extract_result":[{"state":"running",)"
                     R"("extract_progress":{"extracted_pages":3,"total_pages":10}}]}})");
                return;
            }
            latestZipTicket = pollCount;
            const QByteArray zipUrl =
                "http://127.0.0.1:" + QByteArray::number(serverPort()) + "/result.zip?ticket=" +
                QByteArray::number(latestZipTicket);
            send("200 OK", R"({"code":0,"data":{"extract_result":[{"state":"done",)"
                           R"("full_zip_url":")" + zipUrl + R"("}]}})");
            return;
        }
        if (requestLine.contains("/result.zip")) {
            ++downloadCount;
            const QByteArray currentTicket = "ticket=" + QByteArray::number(latestZipTicket);
            if (!requestLine.contains(currentTicket)) {
                send("403 Forbidden", R"({"msg":"stale signed url"})");
                return;
            }
            if (transientDownloadFailures > 0) {
                --transientDownloadFailures;
                // 使用 503 而非直接断连：Qt 会自行重发 HTTP 0 场景，导致桩服务
                // 难以稳定观察应用层逻辑；两者都会走同一个 retryTransient 分支。
                send("503 Service Unavailable", R"({"msg":"temporary download failure"})");
                return;
            }
            if (oversizedDownloadHeader) {
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 536870913\r\n\r\n");
                socket->flush();
                return;
            }
            if (chunkedDownload) {
                socket->write("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
                const QByteArray first = downloadPayload.left(1000);
                socket->write(QByteArray::number(first.size(), 16) + "\r\n" + first + "\r\n");
                socket->flush();
                if (onDownload) onDownload();
                if (!holdDownload) {
                    const QByteArray rest = downloadPayload.mid(first.size());
                    QTimer::singleShot(20, socket, [socket, rest] {
                        if (!rest.isEmpty())
                            socket->write(QByteArray::number(rest.size(), 16) + "\r\n" + rest + "\r\n");
                        socket->write("0\r\n\r\n");
                        socket->disconnectFromHost();
                    });
                }
                return;
            }
            send("200 OK", downloadPayload, "application/zip");
            return;
        }
        send("404 Not Found", "{}");
    }

    QHash<QTcpSocket*, QByteArray> buffers_;
};

// 等待信号，带超时。测试绝不允许无限挂起。
bool waitForFinish(quizpane::studio::MineruExtractionJob* job, bool* okOut, QString* zipOut,
                   QString* errorOut) {
    QEventLoop loop;
    bool fired = false;
    QObject::connect(job, &quizpane::studio::MineruExtractionJob::finished, &loop,
                     [&](bool ok, const QString& zip, const QString& error) {
                         *okOut = ok;
                         *zipOut = zip;
                         *errorOut = error;
                         fired = true;
                         loop.quit();
                     });
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec();
    return fired;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace quizpane::studio;

    // —— 纯函数 ——

    MineruSettings settings;
    settings.token = QStringLiteral("test-token");

    const QJsonObject body = buildUploadUrlRequestBody(settings, QStringLiteral("paper.pdf"));
    if (body.value(QStringLiteral("model_version")).toString() != QStringLiteral("vlm"))
        return fail("default model_version should be vlm");
    if (body.value(QStringLiteral("language")).toString() != QStringLiteral("ch"))
        return fail("default language should be ch");
    if (!body.value(QStringLiteral("enable_formula")).toBool() ||
        !body.value(QStringLiteral("enable_table")).toBool())
        return fail("formula/table extraction should default to on");
    const QJsonArray files = body.value(QStringLiteral("files")).toArray();
    if (files.size() != 1 ||
        files.first().toObject().value(QStringLiteral("name")).toString() !=
            QStringLiteral("paper.pdf"))
        return fail("upload body must carry exactly one named file");
    // 文字型 PDF 不该请求 OCR：多余的识别只会引入误差。
    if (files.first().toObject().contains(QStringLiteral("is_ocr")))
        return fail("is_ocr must be omitted unless explicitly requested");
    MineruSettings ocrSettings = settings;
    ocrSettings.isOcr = true;
    if (!buildUploadUrlRequestBody(ocrSettings, QStringLiteral("scan.pdf"))
             .value(QStringLiteral("files"))
             .toArray()
             .first()
             .toObject()
             .value(QStringLiteral("is_ocr"))
             .toBool())
        return fail("is_ocr must be forwarded when requested");

    QString requestError;
    const QNetworkRequest request =
        buildMineruRequest(settings, QStringLiteral("/api/v4/file-urls/batch"), &requestError);
    if (!requestError.isEmpty())
        return fail("valid settings should build a request");
    if (request.rawHeader("Authorization") != QByteArrayLiteral("Bearer test-token"))
        return fail("Bearer token header missing");

    // 没有 Token 时必须在本地失败，不发出请求——避免把明显无效的调用打到线上。
    MineruSettings tokenless;
    QString tokenlessError;
    buildMineruRequest(tokenless, QStringLiteral("/api/v4/file-urls/batch"), &tokenlessError);
    if (tokenlessError.isEmpty())
        return fail("missing token must be rejected locally");

    // 鉴权失败信封（HTTP 200 + success=false）必须被识别为错误，并给出可操作提示。
    const MineruUploadTicket authFailure = parseUploadUrlResponse(
        QByteArrayLiteral(R"({"msgCode":"A0202","msg":"user authenticate failed",)"
                          R"("success":false})"),
        200);
    if (authFailure.error.isEmpty())
        return fail("success=false envelope must be treated as failure");
    if (!authFailure.error.contains(QStringLiteral("Token")))
        return fail("A0202 should be explained in terms of the token");

    // 过期 Token 与额度耗尽都要给出下一步动作，而不是抛出裸错误码。
    if (!parseUploadUrlResponse(QByteArrayLiteral(R"({"msgCode":"A0211","msg":"expired",)"
                                                  R"("success":false})"),
                                200)
             .error.contains(QStringLiteral("重新生成")))
        return fail("A0211 should tell the user to regenerate the token");
    if (!parsePollResponse(QByteArrayLiteral(R"({"code":-60018,"msg":"quota"})"), 200)
             .error.contains(QStringLiteral("额度")))
        return fail("-60018 should be explained as a quota problem");
    if (!parsePollResponse(QByteArrayLiteral(R"({"code":-60012,"msg":"task not found"})"), 200)
             .error.contains(QStringLiteral("任务")))
        return fail("-60012 should be explained as a missing task");

    const MineruUploadTicket ticket = parseUploadUrlResponse(
        QByteArrayLiteral(R"({"code":0,"data":{"batch_id":"b1","file_urls":["https://x/y"]}})"),
        200);
    if (!ticket.error.isEmpty() || ticket.batchId != QStringLiteral("b1") ||
        ticket.uploadUrl != QStringLiteral("https://x/y"))
        return fail("valid upload ticket not parsed");

    // 轮询状态机：running 继续等，failed/done 结束。
    const MineruPollResult running = parsePollResponse(
        QByteArrayLiteral(R"({"code":0,"data":{"extract_result":[{"state":"running",)"
                          R"("extract_progress":{"extracted_pages":4,"total_pages":9}}]}})"),
        200);
    if (running.finished || running.extractedPages != 4 || running.totalPages != 9)
        return fail("running state should report progress and keep polling");
    // 批次刚建立时条目为空，这不是错误。
    if (parsePollResponse(QByteArrayLiteral(R"({"code":0,"data":{"extract_result":[]}})"), 200)
            .finished)
        return fail("empty extract_result should keep polling");
    const MineruPollResult failed = parsePollResponse(
        QByteArrayLiteral(R"({"code":0,"data":{"extract_result":[{"state":"failed",)"
                          R"("err_msg":"broken pdf"}]}})"),
        200);
    if (!failed.finished || failed.error.isEmpty() ||
        failed.error.contains(QStringLiteral("broken pdf")))
        return fail("failed state must use a safe user-facing message");
    const MineruPollResult done = parsePollResponse(
        QByteArrayLiteral(R"({"code":0,"data":{"extract_result":[{"state":"done",)"
                          R"("full_zip_url":"https://x/r.zip"}]}})"),
        200);
    if (!done.finished || done.zipUrl != QStringLiteral("https://x/r.zip") ||
        !done.error.isEmpty())
        return fail("done state must yield a zip url");
    // done 但没有下载地址是协议异常，必须显式失败而不是静默产出空结果。
    if (parsePollResponse(
            QByteArrayLiteral(R"({"code":0,"data":{"extract_result":[{"state":"done"}]}})"), 200)
            .error.isEmpty())
        return fail("done without a zip url must be an error");

    if (describeMineruStage(MineruStage::Polling).isEmpty())
        return fail("stages need human readable descriptions");
    if (!isTransientMineruFailure(429, 0) || !isTransientMineruFailure(503, 0) ||
        isTransientMineruFailure(401, 0) || isTransientMineruFailure(0, 5))
        return fail("transient failure classification is unsafe");
    if (mineruRetryDelayMs(1) != 1000 || mineruRetryDelayMs(3) != 4000 ||
        mineruRetryDelayMs(1, 7) != 7000)
        return fail("retry delay must use exponential backoff and Retry-After");

    // —— 全链路状态机（本地桩服务）——

    QTemporaryDir directory;
    if (!directory.isValid())
        return fail("temporary directory unavailable");
    const QString sourcePath = directory.filePath(QStringLiteral("paper.pdf"));
    {
        QFile source(sourcePath);
        if (!source.open(QIODevice::WriteOnly))
            return fail("cannot stage source pdf");
        source.write(QByteArrayLiteral("%PDF-1.7 stub content"));
    }

    StubServer server;
    if (!server.listen(QHostAddress::LocalHost))
        return fail("stub server failed to listen");

    MineruSettings stubSettings;
    stubSettings.token = QStringLiteral("test-token");
    stubSettings.baseUrl =
        QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());

    QNetworkAccessManager manager;
    const QString zipPath = directory.filePath(QStringLiteral("out/result.zip"));
    {
        // 首次申请链接和首次结果下载均返回 503。客户端必须自动退避，且结果
        // 下载要重新轮询拿新的预签名地址，不能复用旧地址。
        server.transientTicketFailures = 1;
        server.transientDownloadFailures = 1;
        MineruExtractionJob job(&manager);
        QList<MineruStage> stages;
        QString submittedBatch;
        QObject::connect(&job, &MineruExtractionJob::stageChanged, &job,
                         [&stages](MineruStage stage, const QString&) { stages.append(stage); });
        QObject::connect(&job, &MineruExtractionJob::taskSubmitted, &job,
                         [&submittedBatch](const QString& batchId) { submittedBatch = batchId; });
        int lastExtracted = -1;
        int lastTotal = -1;
        QObject::connect(&job, &MineruExtractionJob::progress, &job,
                         [&](int extracted, int total) {
                             lastExtracted = extracted;
                             lastTotal = total;
                         });

        bool ok = false;
        QString resultZip;
        QString error;
        job.start(stubSettings, sourcePath, zipPath);
        if (!waitForFinish(&job, &ok, &resultZip, &error))
            return fail("job never finished");
        if (!ok)
            return fail(QStringLiteral("stubbed job failed: %1").arg(error).toUtf8().constData());
        if (resultZip != zipPath)
            return fail("job reported an unexpected zip path");
        if (!QFile::exists(zipPath))
            return fail("result zip was not written to disk");
        // 上传的内容必须与源文件逐字节一致。
        if (server.uploadCount != 1 ||
            server.uploadedBody != QByteArrayLiteral("%PDF-1.7 stub content"))
            return fail("uploaded body does not match the source file");
        if (submittedBatch != QStringLiteral("batch-1"))
            return fail("uploaded task must expose its batch id for persistence");
        if (server.uploadTicketRequestCount < 2)
            return fail("transient upload-ticket failure was not retried");
        // running 之后必须继续轮询，而不是当成结束。
        if (server.pollCount < 2)
            return fail("client should keep polling while the task is running");
        if (server.pollCount < 3 || server.downloadCount != 2)
            return fail("download retry must refresh the signed result url through polling");
        if (lastExtracted != 3 || lastTotal != 10)
            return fail("progress from the running state was not surfaced");
        if (!stages.contains(MineruStage::Uploading) || !stages.contains(MineruStage::Polling) ||
            !stages.contains(MineruStage::Downloading) || !stages.contains(MineruStage::Done))
            return fail("job did not pass through the expected stages");
        if (job.stage() != MineruStage::Done)
            return fail("final stage should be Done");
    }

    // 已上传任务的恢复：弱网轮询暂时失败时只延后重试；恢复路径不能再次申请
    // 上传链接或上传原文件。
    {
        const int uploadsBeforeResume = server.uploadCount;
        const int ticketsBeforeResume = server.uploadTicketRequestCount;
        server.transientPollFailures = 2;
        MineruExtractionJob job(&manager);
        bool ok = false;
        QString resultZip;
        QString error;
        const QString resumedZip = directory.filePath(QStringLiteral("resumed/result.zip"));
        job.resume(stubSettings, QStringLiteral("batch-1"), resumedZip);
        if (!waitForFinish(&job, &ok, &resultZip, &error))
            return fail("resumed job never finished");
        if (!ok || resultZip != resumedZip || !QFile::exists(resumedZip))
            return fail("resumed task must download its result after weak-network polling");
        if (server.uploadCount != uploadsBeforeResume ||
            server.uploadTicketRequestCount != ticketsBeforeResume)
            return fail("resumed task must not upload the original file again");
    }

    // Unknown-length/chunked response: publish only a complete, byte-identical
    // file. Cancellation and header overflow must preserve an existing result.
    {
        const QString streamedZip = directory.filePath(QStringLiteral("分块结果.zip"));
        server.chunkedDownload = true;
        server.downloadPayload = QByteArray(700000, 'z');
        server.downloadPayload.replace(0, 4, "PK\x03\x04");
        MineruExtractionJob job(&manager);
        bool ok = false;
        QString resultZip, error;
        job.resume(stubSettings, QStringLiteral("batch-1"), streamedZip);
        if (!waitForFinish(&job, &ok, &resultZip, &error) || !ok)
            return fail("chunked download failed");
        QFile result(streamedZip);
        if (!result.open(QIODevice::ReadOnly) || result.readAll() != server.downloadPayload)
            return fail("streamed bytes must match the response exactly");
        result.close();
        const QByteArray previous = server.downloadPayload;
        server.holdDownload = true;
        server.downloadPayload = QByteArray(700000, 'n');
        server.onDownload = [&job] { QTimer::singleShot(0, &job, &MineruExtractionJob::cancel); };
        job.resume(stubSettings, QStringLiteral("batch-1"), streamedZip);
        if (!waitForFinish(&job, &ok, &resultZip, &error) || ok ||
            job.stage() != MineruStage::Cancelled)
            return fail("in-flight download must be cancellable");
        server.onDownload = {};
        server.holdDownload = false;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (!result.open(QIODevice::ReadOnly) || result.readAll() != previous)
            return fail("cancelled download must not overwrite a complete result");
        result.close();
        server.chunkedDownload = false;
        server.oversizedDownloadHeader = true;
        job.resume(stubSettings, QStringLiteral("batch-1"), streamedZip);
        if (!waitForFinish(&job, &ok, &resultZip, &error) || ok ||
            !error.contains(QStringLiteral("超出")))
            return fail("oversized response must stop before receiving its body");
        server.oversizedDownloadHeader = false;
        if (!result.open(QIODevice::ReadOnly) || result.readAll() != previous)
            return fail("overflow must preserve an existing complete result");
        result.close();
        server.downloadPayload = {};
        const QString emptyZip = directory.filePath(QStringLiteral("empty.zip"));
        job.resume(stubSettings, QStringLiteral("batch-1"), emptyZip);
        if (!waitForFinish(&job, &ok, &resultZip, &error) || ok || QFile::exists(emptyZip))
            return fail("empty download must not create a completed result");
        server.downloadPayload = "PK\x03\x04stub-zip-bytes";
        // A directory cannot be used as the output file, even if networking works.
        job.resume(stubSettings, QStringLiteral("batch-1"), directory.path());
        if (!waitForFinish(&job, &ok, &resultZip, &error) || ok || error.isEmpty())
            return fail("unwritable output must fail without retrying a signed url");
    }

    // 错误路径：Token 不被桩服务接受时必须失败并给出提示，不能写出结果文件。
    {
        MineruSettings badSettings = stubSettings;
        badSettings.token = QStringLiteral("wrong-token");
        const QString badZip = directory.filePath(QStringLiteral("bad/result.zip"));
        MineruExtractionJob job(&manager);
        bool ok = true;
        QString resultZip;
        QString error;
        job.start(badSettings, sourcePath, badZip);
        if (!waitForFinish(&job, &ok, &resultZip, &error))
            return fail("rejected job never finished");
        if (ok || error.isEmpty())
            return fail("bad token must fail the job");
        if (QFile::exists(badZip))
            return fail("failed job must not leave a result file behind");
        if (job.stage() != MineruStage::Failed)
            return fail("final stage should be Failed");
    }

    // 不存在的源文件在本地即失败，不发起任何网络请求。
    {
        MineruExtractionJob job(&manager);
        bool ok = true;
        QString resultZip;
        QString error;
        job.start(stubSettings, directory.filePath(QStringLiteral("missing.pdf")),
                  directory.filePath(QStringLiteral("missing/out.zip")));
        if (!waitForFinish(&job, &ok, &resultZip, &error))
            return fail("missing-source job never finished");
        if (ok || !error.contains(QStringLiteral("找不到")))
            return fail("missing source file must fail locally");
    }

    // 取消：状态机必须停在 Cancelled，且报告失败而不是静默结束。
    {
        MineruExtractionJob job(&manager);
        bool ok = true;
        QString resultZip;
        QString error;
        job.start(stubSettings, sourcePath, directory.filePath(QStringLiteral("c/out.zip")));
        job.cancel();
        if (!waitForFinish(&job, &ok, &resultZip, &error))
            return fail("cancelled job never reported");
        if (ok)
            return fail("cancelled job must not report success");
        if (job.stage() != MineruStage::Cancelled)
            return fail("final stage should be Cancelled");
    }

    // 取消上传后必须能删除源文件。Windows 上打开中的文件不允许删除，因此
    // 这条断言能在真实目标平台（Win7/Win10）抓住"取消时泄漏文件句柄"的回归。
    //
    // 注意：POSIX 允许删除打开中的文件，所以本断言在 macOS/Linux 上恒为真、
    // 起不到把关作用（已实测确认）。保留它是为了在 Windows CI 上有效；
    // QFile 生命周期挂在 reply 而非 finished 回调上的理由见 mineru_client.cpp。
    {
        const QString deleteProbe = directory.filePath(QStringLiteral("delete-probe.pdf"));
        {
            QFile probe(deleteProbe);
            if (!probe.open(QIODevice::WriteOnly))
                return fail("cannot stage delete probe file");
            probe.write(QByteArray(512 * 1024, 'x'));
        }
        MineruExtractionJob job(&manager);
        QEventLoop uploadWait;
        QObject::connect(&job, &MineruExtractionJob::stageChanged, &uploadWait,
                         [&uploadWait](MineruStage stage, const QString&) {
                             if (stage == MineruStage::Uploading)
                                 uploadWait.quit();
                         });
        QTimer::singleShot(10000, &uploadWait, &QEventLoop::quit);
        bool ok = true;
        QString resultZip;
        QString error;
        job.start(stubSettings, deleteProbe, directory.filePath(QStringLiteral("dp/out.zip")));
        uploadWait.exec();
        job.cancel();
        if (!waitForFinish(&job, &ok, &resultZip, &error))
            return fail("cancelled upload never reported");
        if (ok)
            return fail("cancelled upload must not report success");
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        if (!QFile::remove(deleteProbe))
            return fail("source file still open after cancelling the upload");
    }

    std::fprintf(stdout, "mineru_client_test ok\n");
    return 0;
}
