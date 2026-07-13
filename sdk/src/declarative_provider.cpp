#include "quizpane/declarative_provider.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace quizpane {
namespace {
bool fail(QString* out, const QString& text) { if (out) *out = text; return false; }
QJsonObject readObject(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 128 * 1024 * 1024) return {};
    const auto document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}
QJsonObject findManifest(const QString& entry) {
    QDir directory = QFileInfo(entry).absoluteDir();
    for (int depth = 0; depth < 8; ++depth) {
        const auto manifest = readObject(directory.filePath(QStringLiteral("manifest.json")));
        if (!manifest.isEmpty()) return manifest;
        if (!directory.cdUp()) break;
    }
    return {};
}
QString paragraph(const QString& text) {
    QString escaped = text.toHtmlEscaped();
    escaped.replace('\n', QStringLiteral("<br>"));
    return QStringLiteral("<p>%1</p>").arg(escaped);
}
}  // namespace

bool DeclarativeProvider::load(const QString& bankPath, QString* errorOutput) {
    unload();
    const QJsonObject manifest = findManifest(bankPath);
    const QJsonObject bank = readObject(bankPath);
    if (manifest.value("manifestVersion").toInt() != 2 ||
        manifest.value("kind").toString() != QStringLiteral("declarative"))
        return fail(errorOutput, QStringLiteral("声明式题库缺少有效的 manifest.json"));
    if (bank.value("schemaVersion").toInt() != 1 || !bank.value("catalogs").isArray() ||
        !bank.value("questions").isArray())
        return fail(errorOutput, QStringLiteral("题库数据格式无效"));
    providerId_ = manifest.value("id").toString();
    providerName_ = manifest.value("name").toString();
    providerVersion_ = manifest.value("version").toString();
    bankTitle_ = bank.value("title").toString(providerName_);
    catalogs_ = bank.value("catalogs").toArray();
    questions_ = bank.value("questions").toArray();
    if (providerId_.isEmpty() || providerName_.isEmpty() || catalogs_.isEmpty() || questions_.isEmpty()) {
        unload(); return fail(errorOutput, QStringLiteral("题库名称、分类或题目为空"));
    }

    QSet<QString> catalogIds, questionIds;
    for (const auto& value : catalogs_) {
        const QString id = value.toObject().value("id").toString();
        if (id.isEmpty() || catalogIds.contains(id)) { unload(); return fail(errorOutput, QStringLiteral("题库分类标识为空或重复")); }
        catalogIds.insert(id);
    }
    for (const auto& value : questions_) {
        const QJsonObject question = value.toObject();
        const QString id = question.value("id").toString();
        const QJsonArray options = question.value("options").toArray();
        const QJsonArray answers = question.value("answer").toObject().value("optionIds").toArray();
        if (id.isEmpty() || questionIds.contains(id) ||
            !catalogIds.contains(question.value("catalogId").toString()) ||
            options.size() < 2 || answers.size() != 1) {
            unload(); return fail(errorOutput, QStringLiteral("题目 %1 的结构无效").arg(id));
        }
        const QString answerId = answers.first().toString();
        bool found = false;
        QSet<QString> optionIds;
        for (const auto& optionValue : options) {
            const QString optionId = optionValue.toObject().value("id").toString();
            if (optionId.isEmpty() || optionIds.contains(optionId)) { unload(); return fail(errorOutput, QStringLiteral("题目 %1 的选项标识无效").arg(id)); }
            optionIds.insert(optionId); found |= optionId == answerId;
        }
        if (!found) { unload(); return fail(errorOutput, QStringLiteral("题目 %1 的答案没有对应选项").arg(id)); }
        questionIds.insert(id);
    }
    return true;
}

void DeclarativeProvider::unload() {
    providerId_.clear(); providerName_.clear(); providerVersion_.clear(); bankTitle_.clear();
    activeCatalogTitle_.clear(); catalogs_ = {}; questions_ = {}; activeQuestions_ = {}; answers_.clear();
}

QJsonObject DeclarativeProvider::descriptor() const {
    return {{"id", providerId_}, {"name", providerName_}, {"version", providerVersion_}, {"kind", "declarative"}};
}
QJsonObject DeclarativeProvider::error(const QJsonValue& id, const QString& message) const {
    return {{"id", id}, {"error", QJsonObject{{"code", -32602}, {"message", message}}}};
}
QJsonObject DeclarativeProvider::findCatalog(const QString& id) const {
    for (const auto& value : catalogs_) if (value.toObject().value("id").toString() == id) return value.toObject();
    return {};
}

QJsonArray DeclarativeProvider::hostQuestions(bool withSolutions) const {
    QJsonArray result;
    for (const auto& value : activeQuestions_) {
        const QJsonObject source = value.toObject();
        const QString answerId = source.value("answer").toObject().value("optionIds").toArray().first().toString();
        QJsonArray options;
        int correct = -1;
        const auto sourceOptions = source.value("options").toArray();
        for (qsizetype index = 0; index < sourceOptions.size(); ++index) {
            const auto option = sourceOptions.at(index).toObject();
            if (option.value("id").toString() == answerId) correct = static_cast<int>(index);
            options.append(QJsonObject{{"index", index}, {"label", QString(QChar(char16_t(u'A' + index)))},
                {"contentHtml", paragraph(option.value("text").toString())}});
        }
        QJsonObject question{{"id", source.value("id")}, {"type", source.value("type")},
            {"contentHtml", paragraph(source.value("stem").toString())}, {"options", options}};
        if (withSolutions) {
            question.insert("correctChoice", correct);
            question.insert("solutionHtml", paragraph(source.value("solution").toString()));
        }
        result.append(question);
    }
    return result;
}

QJsonObject DeclarativeProvider::request(const QJsonObject& requestValue) {
    const QJsonValue id = requestValue.value("id");
    const QString method = requestValue.value("method").toString();
    const QJsonObject params = requestValue.value("params").toObject();
    if (method == "provider.initialize") return {{"id", id}, {"result", QJsonObject{
        {"providerId", providerId_}, {"providerVersion", providerVersion_}, {"requiresLogin", false}, {"sessionRestored", true}}}};
    if (method == "provider.capabilities") return {{"id", id}, {"result", QJsonObject{{"loginMethods", QJsonArray{"none"}}}}};
    if (method == "catalog.list") {
        QJsonArray nodes;
        for (const auto& value : catalogs_) {
            const QJsonObject catalog = value.toObject(); const QString catalogId = catalog.value("id").toString(); int count = 0;
            for (const auto& question : questions_) count += question.toObject().value("catalogId").toString() == catalogId;
            const QJsonObject practice = catalog.value("practice").toObject();
            int suggested = practice.value("mode").toString() == QStringLiteral("all")
                ? count : practice.value("questionCount").toInt(qMin(15, count));
            suggested = qBound(1, suggested, qMax(1, count));
            nodes.append(QJsonObject{{"id", catalogId}, {"title", catalog.value("title")},
                {"availableQuestionCount", count}, {"canStartAttempt", count > 0},
                {"suggestedCounts", QJsonArray{suggested}}});
        }
        return {{"id", id}, {"result", QJsonObject{{"nodes", nodes}}}};
    }
    if (method == "attempt.create") {
        const QJsonObject catalog = findCatalog(params.value("categoryId").toString());
        if (catalog.isEmpty()) return error(id, QStringLiteral("练习分类不存在"));
        QList<QJsonValue> candidates;
        for (const auto& question : questions_) if (question.toObject().value("catalogId") == catalog.value("id")) candidates.append(question);
        if (catalog.value("practice").toObject().value("mode") == QStringLiteral("random"))
            std::shuffle(candidates.begin(), candidates.end(), *QRandomGenerator::global());
        const int count = qBound(1, params.value("count").toInt(candidates.size()), static_cast<int>(candidates.size()));
        activeQuestions_ = {}; for (int i = 0; i < count; ++i) activeQuestions_.append(candidates.at(i));
        answers_.clear(); activeCatalogTitle_ = catalog.value("title").toString(bankTitle_);
        return {{"id", id}, {"result", QJsonObject{{"attemptId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"title", activeCatalogTitle_}, {"status", "active"}, {"questionCount", count}}}};
    }
    if (method == "attempt.questions") return {{"id", id}, {"result", QJsonObject{{"questions", hostQuestions(false)}}}};
    if (method == "attempt.saveAnswers") {
        for (const auto& value : params.value("answers").toArray()) { const auto answer = value.toObject(); bool ok = false;
            const int choice = answer.value("answer").toObject().value("choice").toString().toInt(&ok);
            if (ok) answers_.insert(answer.value("questionIndex").toInt(), choice); }
        return {{"id", id}, {"result", QJsonObject{{"ok", true}}}};
    }
    if (method == "attempt.submit") return {{"id", id}, {"result", QJsonObject{{"ok", true}}}};
    if (method == "attempt.report") { int correct = 0; const auto solutions = hostQuestions(true);
        for (qsizetype i = 0; i < solutions.size(); ++i) correct += answers_.value(static_cast<int>(i), -1) == solutions.at(i).toObject().value("correctChoice").toInt(-2);
        return {{"id", id}, {"result", QJsonObject{{"questionCount", activeQuestions_.size()}, {"answerCount", answers_.size()}, {"correctCount", correct}}}};
    }
    if (method == "attempt.solutions") return {{"id", id}, {"result", QJsonObject{{"solutions", hostQuestions(true)}}}};
    return error(id, QStringLiteral("声明式题库不支持此操作：%1").arg(method));
}

}  // namespace quizpane
