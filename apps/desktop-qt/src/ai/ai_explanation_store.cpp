#include "ai_explanation_store.hpp"

#include "quizpane/bank_validator.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace quizpane {
namespace {
constexpr qint64 kMaxBankBytes = 128 * 1024 * 1024;

QJsonObject readBank(const QString& path) {
    if (QFileInfo(path).fileName() != QStringLiteral("bank.json")) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxBankBytes) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
}  // namespace

QHash<QString, QString> loadAiExplanations(const QString& bankPath) {
    QHash<QString, QString> result;
    const QJsonObject bank = readBank(bankPath);
    for (const QJsonValue& value : bank.value(QStringLiteral("questions")).toArray()) {
        const QJsonObject question = value.toObject();
        const QString id = question.value(QStringLiteral("id")).toString();
        const QString html = question.value(QStringLiteral("aiSolution")).toString();
        if (!id.isEmpty() && !html.isEmpty()) result.insert(id, html);
    }
    return result;
}

bool saveAiExplanation(const QString& bankPath, const QString& questionId,
                       const QString& html, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (questionId.isEmpty() || html.trimmed().isEmpty() || html.size() > 20000)
        return fail(QStringLiteral("AI 解析为空或超过 20000 字，无法保存"));
    QJsonObject bank = readBank(bankPath);
    if (bank.isEmpty()) return fail(QStringLiteral("当前题库没有可写入的 bank.json"));
    QJsonArray questions = bank.value(QStringLiteral("questions")).toArray();
    bool found = false;
    for (qsizetype index = 0; index < questions.size(); ++index) {
        QJsonObject question = questions.at(index).toObject();
        if (question.value(QStringLiteral("id")).toString() != questionId) continue;
        question.insert(QStringLiteral("aiSolution"), html);
        questions.replace(index, question);
        found = true;
        break;
    }
    if (!found) return fail(QStringLiteral("题库中找不到当前题目，未保存 AI 解析"));
    bank.insert(QStringLiteral("questions"), questions);
    QString validationError;
    if (!validateBank(bank, &validationError))
        return fail(QStringLiteral("题库校验未通过：%1").arg(validationError));
    QSaveFile file(bankPath);
    if (!file.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("无法写入题库文件：%1").arg(file.errorString()));
    const QByteArray payload = QJsonDocument(bank).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size() || !file.commit())
        return fail(QStringLiteral("保存题库文件失败：%1").arg(file.errorString()));
    return true;
}

}  // namespace quizpane
