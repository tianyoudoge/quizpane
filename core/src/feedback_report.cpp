#include "quizpane/feedback_report.hpp"

#include "quizpane/diagnostic_logger.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QStandardPaths>
#include <QSysInfo>
#include <QStorageInfo>
#include <QUrl>

namespace quizpane::feedback {
namespace {

// 与 diagnostic_logger 的脱敏同口径：Bearer token / API Key 不上报。
QString redact(QString message) {
    static const QRegularExpression bearer(
        QStringLiteral("(?i)(authorization\\s*[:=]\\s*bearer\\s+)[^\\s,;]+"));
    static const QRegularExpression apiKey(
        QStringLiteral("(?i)((?:api[_-]?key|token)\\s*[:=]\\s*)[^\\s,;]+"));
    message.replace(bearer, QStringLiteral("\\1<redacted>"));
    message.replace(apiKey, QStringLiteral("\\1<redacted>"));
    return message;
}

// 上报内容离开本机，完整用户路径只保留末两级。
QString maskPaths(QString message) {
    auto shorten = [](const QString& matched, QChar separator) {
        const QStringList parts = matched.split(separator, Qt::SkipEmptyParts);
        if (parts.size() <= 2)
            return matched;
        return QStringLiteral("...") + separator + parts.mid(parts.size() - 2).join(separator);
    };
    static const QRegularExpression unixPath(
        QStringLiteral(R"((?:/Users|/home)/[\w.-]+(?:/[\w.-]+)*)"));
    static const QRegularExpression windowsPath(
        QStringLiteral(R"(([A-Za-z]:\\(Users|Public|程序数据|ProgramData)\\[\w.-]+(\\[\w.-]+)*)"));
    // Qt 的 replace 没有回调重载，这里按 globalMatch 的结果手动拼接。
    auto apply = [&shorten](QString& text, const QRegularExpression& pattern, QChar separator) {
        QString result;
        qsizetype position = 0;
        auto matcher = pattern.globalMatch(text);
        while (matcher.hasNext()) {
            const auto match = matcher.next();
            result += text.mid(position, match.capturedStart() - position);
            result += shorten(match.captured(0), separator);
            position = match.capturedEnd();
        }
        result += text.mid(position);
        text = result;
    };
    apply(message, unixPath, QChar('/'));
    apply(message, windowsPath, QChar('\\'));
    return message;
}

qint64 diskFreeBytes() {
    const QStorageInfo storage = QStorageInfo(QDir::rootPath());
    return storage.isValid() ? storage.bytesAvailable() : -1;
}

void appendCrashInfo(QJsonObject& environment, const QString& crashPath) {
    const QFileInfo info(crashPath);
    environment.insert(QStringLiteral("crashArtifactPresent"), info.isFile() && info.size() > 0);
    if (!info.isFile() || info.size() == 0)
        return;
    environment.insert(QStringLiteral("crashArtifactAt"),
                       info.lastModified().toString(Qt::ISODateWithMs));
    // 崩溃产物在日志目录里，路径带用户名，只上报文件名。
    environment.insert(QStringLiteral("crashArtifactName"), info.fileName());
}

QString crashFileBase64(const QString& crashPath, qint64 maximumBytes) {
    QFile file(crashPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = file.read(maximumBytes);
    return data.toBase64();
}

QString readTailLines(const QString& path, int maxLines) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    // 日志有 5MiB 轮转上限，直接全量读入再取尾部即可。
    const QString content = QString::fromUtf8(file.readAll());
    const QStringList lines = content.split(QChar('\n'));
    const int start = qMax(0, lines.size() - maxLines);
    QStringList selected;
    for (int index = start; index < lines.size(); ++index)
        selected.append(maskPaths(redact(lines.at(index))));
    return selected.join(QChar('\n'));
}

} // namespace

QJsonObject buildEnvironmentInfo() {
    QJsonObject info;
    info.insert(QStringLiteral("app"), QCoreApplication::applicationName());
    info.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());
    info.insert(QStringLiteral("platform"), QSysInfo::prettyProductName());
    info.insert(QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture());
    info.insert(QStringLiteral("qtVersion"), QString::fromLatin1(qVersion()));
    info.insert(QStringLiteral("clientBuild"),
#if defined(QUIZPANE_PROD_DIAGNOSTICS)
               QStringLiteral("prod-diagnostics"));
#elif defined(QUIZPANE_DIAGNOSTIC_LOGGING)
               QStringLiteral("debug-diagnostics"));
#else
               QStringLiteral("no-diagnostics"));
#endif
    info.insert(QStringLiteral("diskFreeBytes"), diskFreeBytes());
    appendCrashInfo(info, diagnostic::crashArtifactPath());
    return info;
}

QString buildLogTail(int maxLines) {
    return readTailLines(diagnostic::logFilePath(), maxLines);
}

SendResult sendReport(const QString& description, bool includeLogs,
                      bool includeCrash, const QString& endpoint, int timeoutMs) {
    const QString descriptionText = description.trimmed();
    if (descriptionText.isEmpty())
        return {false, QStringLiteral("请先填写问题描述。")};
    if (descriptionText.size() > 8000)
        return {false, QStringLiteral("问题描述过长，请精简后再发送。")};

    QJsonObject payload;
    payload.insert(QStringLiteral("type"), QStringLiteral("user-feedback"));
    payload.insert(QStringLiteral("client"), QStringLiteral("quizpane"));
    payload.insert(QStringLiteral("description"), descriptionText);
    payload.insert(QStringLiteral("environment"), buildEnvironmentInfo());
    payload.insert(QStringLiteral("submittedAt"),
                   QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    const QString logs = includeLogs ? readTailLines(diagnostic::logFilePath(), 200)
                                     : QString();
    payload.insert(QStringLiteral("logs"), logs);
    if (includeCrash) {
        const QString crashPath = diagnostic::crashArtifactPath();
        const QFileInfo info(crashPath);
        // 只附最近 24h 内的崩溃产物；Windows 的 minidump 限制 5MiB 以内。
        if (info.isFile() && info.size() > 0 &&
            QDateTime::currentDateTime().secsTo(info.lastModified()) < 24 * 3600) {
            const qint64 limit =
#if defined(Q_OS_WIN)
                5 * 1024 * 1024;
#else
                1 * 1024 * 1024;
#endif
            payload.insert(QStringLiteral("crashFile"), crashFileBase64(crashPath, limit));
            payload.insert(QStringLiteral("crashFileTruncated"),
                           info.size() > limit ? true : false);
        }
    }
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (body.size() > 8 * 1024 * 1024)
        return {false, QStringLiteral("上报内容超过大小上限，请取消「附上日志」后重试。")};

    QNetworkAccessManager manager;
    const QUrl url = endpoint.isEmpty()
        ? QUrl(QStringLiteral("https://xutianyou.cc/quizpane/api/feedback"))
        : QUrl(endpoint);
    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json; charset=utf-8"));
    request.setRawHeader("User-Agent",
                         QStringLiteral("QuizPane/%1 QuizPane-Feedback").arg(
                             QCoreApplication::applicationVersion()).toUtf8());

    SendResult result;
    QEventLoop loop;
    QNetworkReply* reply = nullptr;
    QObject::connect(&manager, &QNetworkAccessManager::finished,
                     [&loop, &result](QNetworkReply* finished) {
                         const int status =
                             finished->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                                 .toInt();
                         finished->deleteLater();
                         if (status == 200) {
                             result.success = true;
                             result.message = QStringLiteral("反馈已发送，感谢！");
                         } else if (status == 413) {
                             result.message =
                                 QStringLiteral("内容超过服务器大小上限，请取消「附上日志」后重试。");
                         } else if (status == 400) {
                             result.message =
                                 QStringLiteral("反馈内容不符合要求，请稍后重试或邮件联系。");
                         } else if (status == 429) {
                             result.message =
                                 QStringLiteral("发送过于频繁，请稍后再试。");
                         } else {
                             result.message = QStringLiteral("发送失败（HTTP %1），请稍后重试。")
                                                 .arg(status);
                         }
                         loop.quit();
                     });
    reply = manager.post(request, body);
    const QDeadlineTimer deadline(timeoutMs);
    while (loop.isRunning() && !deadline.hasExpired())
        loop.processEvents(QEventLoop::AllEvents, 50);
    if (loop.isRunning() && reply) {
        reply->abort();
        result.message = QStringLiteral("发送超时，请稍后重试。");
    }
    return result;
}

} // namespace quizpane::feedback
