#include "quizpane/draft_store.hpp"
#include "quizpane/io_utils.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

namespace quizpane {
QString DraftStore::pathForProvider(const QString& providerId) const {
    // 不直接把 providerId 当文件名，避免特殊字符造成路径问题；SHA-256 只用于
    // 生成稳定文件名，并不是加密。登录 Cookie 从不进入草稿文件。
    const QByteArray digest = QCryptographicHash::hash(
        providerId.toUtf8(), QCryptographicHash::Sha256).toHex();
    QString root = qEnvironmentVariable("QUIZPANE_DRAFTS_ROOT");
    if (root.isEmpty())
        root = QDir(QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("drafts"));
    return QDir(root).filePath(QString::fromLatin1(digest) + QStringLiteral(".json"));
}

QString DraftStore::pathForAttempt(const QString& providerId,
                                   const QString& attemptId) const {
    const QByteArray providerDigest = QCryptographicHash::hash(
        providerId.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QByteArray attemptDigest = QCryptographicHash::hash(
        attemptId.toUtf8(), QCryptographicHash::Sha256).toHex();
    QString root = qEnvironmentVariable("QUIZPANE_DRAFTS_ROOT");
    if (root.isEmpty())
        root = QDir(QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("drafts"));
    return QDir(root).filePath(QStringLiteral("%1/%2.json")
        .arg(QString::fromLatin1(providerDigest), QString::fromLatin1(attemptDigest)));
}

bool DraftStore::save(const DraftSnapshot& snapshot, QString* error) const {
    if (snapshot.providerId.isEmpty() || snapshot.attemptId.isEmpty() ||
        snapshot.questions.isEmpty())
        return fail(error, QStringLiteral("草稿缺少题库来源、练习编号或题目"));
    const QString path = pathForAttempt(snapshot.providerId, snapshot.attemptId);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(error, QStringLiteral("无法创建草稿目录"));
    QJsonArray answers;
    for (int answer : snapshot.answers) answers.append(answer);
    const QJsonObject value{{"version", 2}, {"providerId", snapshot.providerId},
        {"attemptId", snapshot.attemptId}, {"title", snapshot.title},
        {"questions", snapshot.questions}, {"materials", snapshot.materials},
        {"answers", answers}, {"currentQuestionIndex", snapshot.currentQuestionIndex},
        {"updatedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    // QSaveFile 先写临时文件，commit 时再替换目标文件。即使进程崩溃或断电，
    // 也不会留下半截 JSON；可以类比“事务提交后才对外可见”。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
    file.write(QJsonDocument(value).toJson(QJsonDocument::Compact));
    if (!file.commit()) return fail(error, file.errorString());
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool DraftStore::load(const QString& providerId, DraftSnapshot* snapshot,
                      QString* error) const {
    if (!snapshot || providerId.isEmpty()) return false;
    const QList<DraftSnapshot> drafts = list(providerId, error);
    if (drafts.isEmpty()) return false;
    *snapshot = drafts.first();
    return true;
}

namespace {
bool readDraftFile(const QString& path, const QString& providerId,
                   DraftSnapshot* snapshot, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject())
        return fail(error, QStringLiteral("草稿 JSON 无效：%1").arg(parseError.errorString()));
    const QJsonObject value = document.object();
    const int version = value.value("version").toInt();
    if ((version != 1 && version != 2) ||
        value.value("providerId").toString() != providerId)
        return fail(error, QStringLiteral("草稿版本或题库来源不匹配"));
    DraftSnapshot result;
    result.providerId = providerId;
    result.attemptId = value.value("attemptId").toString();
    result.title = value.value("title").toString();
    result.questions = value.value("questions").toArray();
    result.materials = value.value("materials").toArray();
    result.currentQuestionIndex = value.value("currentQuestionIndex").toInt();
    result.updatedAt = QDateTime::fromString(value.value("updatedAt").toString(),
                                             Qt::ISODateWithMs);
    if (!result.updatedAt.isValid())
        result.updatedAt = QFileInfo(path).lastModified().toUTC();
    for (const auto& answer : value.value("answers").toArray())
        result.answers.append(answer.toInt(-1));
    if (result.attemptId.isEmpty() || result.questions.isEmpty())
        return fail(error, QStringLiteral("草稿内容不完整"));
    *snapshot = result;
    return true;
}
}

QList<DraftSnapshot> DraftStore::list(const QString& providerId,
                                      QString* error) const {
    QList<DraftSnapshot> result;
    if (providerId.isEmpty()) return result;
    const QFileInfo sample(pathForAttempt(providerId, QStringLiteral("sample")));
    QDir directory(sample.absolutePath());
    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files, QDir::Time);
    for (const QFileInfo& file : files) {
        DraftSnapshot snapshot;
        QString ignored;
        if (readDraftFile(file.absoluteFilePath(), providerId, &snapshot, &ignored))
            result.append(snapshot);
    }
    // 兼容 0.2.4 以前每个题库只有一份的草稿；下一次保存会自然迁移到多草稿目录。
    const QString legacyPath = pathForProvider(providerId);
    if (QFileInfo::exists(legacyPath)) {
        DraftSnapshot legacy;
        if (readDraftFile(legacyPath, providerId, &legacy, error) &&
            std::none_of(result.cbegin(), result.cend(), [&](const DraftSnapshot& item) {
                return item.attemptId == legacy.attemptId;
            })) result.append(legacy);
    }
    std::sort(result.begin(), result.end(), [](const DraftSnapshot& left,
                                                const DraftSnapshot& right) {
        return left.updatedAt > right.updatedAt;
    });
    return result;
}

bool DraftStore::clearAttempt(const QString& providerId, const QString& attemptId,
                              QString* error) const {
    const QString path = pathForAttempt(providerId, attemptId);
    bool ok = !QFile::exists(path) || QFile::remove(path);
    const QString legacyPath = pathForProvider(providerId);
    if (QFile::exists(legacyPath)) {
        DraftSnapshot legacy;
        QString ignored;
        if (readDraftFile(legacyPath, providerId, &legacy, &ignored) &&
            legacy.attemptId == attemptId && !QFile::remove(legacyPath)) ok = false;
    }
    return ok ? true : fail(error, QStringLiteral("无法删除练习草稿"));
}

bool DraftStore::clear(const QString& providerId, QString* error) const {
    bool ok = true;
    const QString legacy = pathForProvider(providerId);
    if (QFile::exists(legacy) && !QFile::remove(legacy)) ok = false;
    const QFileInfo sample(pathForAttempt(providerId, QStringLiteral("sample")));
    QDir directory(sample.absolutePath());
    if (directory.exists() && !directory.removeRecursively()) ok = false;
    return ok ? true : fail(error, QStringLiteral("无法删除草稿"));
}

}  // namespace quizpane
