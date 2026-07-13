#include "quizpane/draft_store.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace quizpane {
namespace {
bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}
}

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

bool DraftStore::save(const DraftSnapshot& snapshot, QString* error) const {
    if (snapshot.providerId.isEmpty() || snapshot.attemptId.isEmpty() ||
        snapshot.questions.isEmpty())
        return fail(error, QStringLiteral("草稿缺少题库来源、练习编号或题目"));
    const QString path = pathForProvider(snapshot.providerId);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(error, QStringLiteral("无法创建草稿目录"));
    QJsonArray answers;
    for (int answer : snapshot.answers) answers.append(answer);
    const QJsonObject value{{"version", 1}, {"providerId", snapshot.providerId},
        {"attemptId", snapshot.attemptId}, {"title", snapshot.title},
        {"questions", snapshot.questions}, {"materials", snapshot.materials},
        {"answers", answers}, {"currentQuestionIndex", snapshot.currentQuestionIndex}};
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
    QFile file(pathForProvider(providerId));
    if (!file.exists()) return false;
    if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject())
        return fail(error, QStringLiteral("草稿 JSON 无效：%1").arg(parseError.errorString()));
    const QJsonObject value = document.object();
    if (value.value("version").toInt() != 1 ||
        value.value("providerId").toString() != providerId)
        return fail(error, QStringLiteral("草稿版本或题库来源不匹配"));
    DraftSnapshot result;
    result.providerId = providerId;
    result.attemptId = value.value("attemptId").toString();
    result.title = value.value("title").toString();
    result.questions = value.value("questions").toArray();
    result.materials = value.value("materials").toArray();
    result.currentQuestionIndex = value.value("currentQuestionIndex").toInt();
    for (const auto& answer : value.value("answers").toArray())
        result.answers.append(answer.toInt(-1));
    if (result.attemptId.isEmpty() || result.questions.isEmpty())
        return fail(error, QStringLiteral("草稿内容不完整"));
    *snapshot = result;
    return true;
}

bool DraftStore::clear(const QString& providerId, QString* error) const {
    const QString path = pathForProvider(providerId);
    if (!QFile::exists(path) || QFile::remove(path)) return true;
    return fail(error, QStringLiteral("无法删除草稿"));
}

}  // namespace quizpane
