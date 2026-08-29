#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace quizpane::studio {

// MinerU 云解析客户端。
//
// 定位：它只负责“把本地 PDF 换成一个已下载好的结果 ZIP”，不理解题库语义。
// 拿到 ZIP 之后由 mineru_output_adapter 适配成 ExtractedDocument，再走现有的
// 规则引擎——与本地解析路径在同一点汇合。
//
// 官方接口是异步任务：本地文件不能直接 POST，必须先申请预签名上传链接、PUT
// 上传，再轮询批次状态，最后下载结果 ZIP。因此这里是状态机而不是单次请求。

struct MineruSettings {
    // mineru.net「API 管理」页创建的 Token。存系统钥匙串，不写日志/配置/题库包。
    QString token;
    // 官方推荐 vlm；pipeline 作为对照与降级。
    QString modelVersion = QStringLiteral("vlm");
    QString language = QStringLiteral("ch");
    bool enableFormula = true;
    bool enableTable = true;
    // 扫描件需要 OCR。文字型 PDF 保持 false，避免不必要的识别误差。
    bool isOcr = false;
    // 便于测试指向本地桩服务；正式路径使用官方地址。
    QString baseUrl = QStringLiteral("https://mineru.net");
};

// 任务阶段。UI 据此显示进度；每个阶段都可取消。
enum class MineruStage {
    Idle,
    RequestingUploadUrl,
    Uploading,
    Polling,
    Downloading,
    Done,
    Failed,
    Cancelled,
};

QString describeMineruStage(MineruStage stage);

// —— 以下为纯函数，便于在没有网络的情况下测试协议细节 ——

// 申请上传链接的请求体。files 只含一个条目：首版一次处理一份文档，但仍走官方
// 的批量接口，因为它是本地文件唯一的上传方式。
QJsonObject buildUploadUrlRequestBody(const MineruSettings& settings, const QString& fileName);

// 构造带 Bearer 鉴权的请求。token 为空时返回错误，不发起请求。
QNetworkRequest buildMineruRequest(const MineruSettings& settings, const QString& path,
                                   QString* error = nullptr);

struct MineruUploadTicket {
    QString batchId;
    QString uploadUrl;
    QString error;
};
MineruUploadTicket parseUploadUrlResponse(const QByteArray& payload, int httpStatus,
                                          const QString& networkError = {});

struct MineruPollResult {
    // waiting-file / pending / running / converting / done / failed
    QString state;
    QString zipUrl;
    QString error;
    int extractedPages = 0;
    int totalPages = 0;
    bool finished = false;
};
MineruPollResult parsePollResponse(const QByteArray& payload, int httpStatus,
                                   const QString& networkError = {});

// 仅对限流、服务端错误和短暂网络故障重试；鉴权、额度、文档格式等确定性错误
// 立即返回。纯函数公开用于协议回归测试。
bool isTransientMineruFailure(int httpStatus, int networkErrorCode);
int mineruRetryDelayMs(int retryAttempt, int retryAfterSeconds = -1);

// 单文件解析任务。发出阶段与进度信号，最终给出本地 ZIP 路径或错误。
// 网络回调都在调用线程（GUI 线程）执行，不阻塞 UI；ZIP 落到调用方指定的
// 临时目录，由调用方在完成或取消后清理。
class MineruExtractionJob final : public QObject {
    Q_OBJECT
public:
    explicit MineruExtractionJob(QNetworkAccessManager* manager, QObject* parent = nullptr);

    // sourcePath：本地 PDF；outputZipPath：结果 ZIP 的落盘位置。
    void start(const MineruSettings& settings, const QString& sourcePath,
               const QString& outputZipPath);
    // 恢复一个已经上传完成的云端任务。只重新轮询和下载结果，绝不重复上传原文件。
    void resume(const MineruSettings& settings, const QString& batchId,
                const QString& outputZipPath);
    void cancel();

    [[nodiscard]] MineruStage stage() const { return stage_; }

signals:
    void stageChanged(quizpane::studio::MineruStage stage, const QString& detail);
    // 文件上传成功、云端开始处理后发出。调用方可安全持久化 batchId，用于下次启动恢复。
    void taskSubmitted(const QString& batchId);
    // 解析进度。总页数未知时 totalPages 为 0。
    void progress(int extractedPages, int totalPages);
    void finished(bool ok, const QString& zipPath, const QString& error);

private:
    void requestUploadUrl();
    void uploadFile(const MineruUploadTicket& ticket);
    void schedulePoll();
    void poll();
    void download(const QString& zipUrl);
    bool retryTransient(MineruStage stage, int httpStatus, int networkErrorCode,
                        int retryAfterSeconds, const QString& detail,
                        const std::function<void()>& action);
    void failWith(const QString& error);
    void setStage(MineruStage stage, const QString& detail = {});
    void clearReply();

    QNetworkAccessManager* manager_;
    QNetworkReply* reply_ = nullptr;
    MineruSettings settings_;
    QString sourcePath_;
    QString outputZipPath_;
    QString batchId_;
    MineruStage stage_ = MineruStage::Idle;
    int pollAttempts_ = 0;
    int transientRetryAttempts_ = 0;
    int weakNetworkAttempts_ = 0;
    quint64 generation_ = 0;
};

} // namespace quizpane::studio
