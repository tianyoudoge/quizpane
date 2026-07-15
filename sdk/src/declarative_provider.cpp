#include "quizpane/declarative_provider.hpp"

#include "quizpane/bank_validator.hpp"

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

    // manifest.runtime.schemaVersion 和 bank.schemaVersion 必须一致，否则可能
    // 出现安装包声明的版本与实际 bank 内容不符这类静默不一致。
    const int runtimeSchemaVersion =
        manifest.value("runtime").toObject().value("schemaVersion").toInt();
    if (runtimeSchemaVersion != 2)
        return fail(errorOutput, QStringLiteral("声明式题库运行时版本不受支持"));
    if (bank.value("schemaVersion").toInt() != runtimeSchemaVersion)
        return fail(errorOutput, QStringLiteral("manifest 与题库的 Schema 版本不一致"));

    const auto errors = validateBankDetailed(bank);
    if (!errors.isEmpty())
        return fail(errorOutput, errors.first().message);

    providerId_ = manifest.value("id").toString();
    providerName_ = manifest.value("name").toString();
    providerVersion_ = manifest.value("version").toString();
    bankTitle_ = bank.value("title").toString(providerName_);
    catalogs_ = bank.value("catalogs").toArray();
    questions_ = bank.value("questions").toArray();
    materials_ = bank.value("materials").toArray();
    if (providerId_.isEmpty() || providerName_.isEmpty()) {
        unload(); return fail(errorOutput, QStringLiteral("题库名称或标识为空"));
    }
    for (const auto& value : materials_)
        materialsById_.insert(value.toObject().value("id").toString(), value.toObject());
    // 预算 catalogId -> 题目数索引，catalog.list 直接查表，避免每分类遍历全量题。
    questionCountByCatalog_.reserve(qMax(int(questions_.size()), 1));
    for (const auto& value : questions_)
        ++questionCountByCatalog_[value.toObject().value("catalogId").toString()];
    return true;
}

void DeclarativeProvider::unload() {
    providerId_.clear(); providerName_.clear(); providerVersion_.clear(); bankTitle_.clear();
    activeCatalogTitle_.clear();
    catalogs_ = {}; questions_ = {}; materials_ = {}; activeQuestions_ = {};
    materialsById_.clear(); questionCountByCatalog_.clear(); answers_.clear();
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

QVector<QJsonArray> DeclarativeProvider::buildUnits(const QString& catalogId) const {
    // 普通题是只含一道题的单元；共享同一 materialId 的题目合并成一个不可拆分
    // 的题组单元。单元出现的顺序和组内题目顺序都取 questions_ 数组中的首次
    // 出现位置，不做任何重排——重排交给调用方（例如随机模式打乱单元顺序）。
    QVector<QJsonArray> units;
    QHash<QString, int> unitIndexByMaterial;
    for (const auto& value : questions_) {
        const QJsonObject question = value.toObject();
        if (question.value("catalogId").toString() != catalogId) continue;
        const QString materialId = question.value("materialId").toString();
        if (materialId.isEmpty()) {
            units.append(QJsonArray{question});
            continue;
        }
        const auto it = unitIndexByMaterial.constFind(materialId);
        if (it == unitIndexByMaterial.constEnd()) {
            unitIndexByMaterial.insert(materialId, static_cast<int>(units.size()));
            units.append(QJsonArray{question});
        } else {
            QJsonArray unit = units[it.value()];
            unit.append(question);
            units[it.value()] = unit;
        }
    }
    return units;
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
        if (source.contains("materialId")) question.insert("materialId", source.value("materialId"));
        if (withSolutions) {
            question.insert("correctChoice", correct);
            question.insert("solutionHtml", paragraph(source.value("solution").toString()));
        }
        result.append(question);
    }
    return result;
}

QJsonArray DeclarativeProvider::hostMaterials() const {
    // 只返回当前作答题目实际引用到的材料，且按题目中首次出现的顺序排列，
    // 避免把整份题库的材料都发给客户端。materialId -> material 缓存这里保留
    // 在 Provider 侧；主程序侧的 materialId -> material 缓存是另一层，用于
    // 避免题组内切题时重复解析 HTML。
    QJsonArray result;
    QSet<QString> seen;
    for (const auto& value : activeQuestions_) {
        const QString materialId = value.toObject().value("materialId").toString();
        if (materialId.isEmpty() || seen.contains(materialId)) continue;
        seen.insert(materialId);
        const QJsonObject material = materialsById_.value(materialId);
        result.append(QJsonObject{{"id", materialId}, {"title", material.value("title")},
            {"contentHtml", paragraph(material.value("body").toString())}});
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
            const QJsonObject catalog = value.toObject(); const QString catalogId = catalog.value("id").toString();
            const int count = questionCountByCatalog_.value(catalogId, 0);
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
        QVector<QJsonArray> units = buildUnits(catalog.value("id").toString());
        if (catalog.value("practice").toObject().value("mode") == QStringLiteral("random"))
            std::shuffle(units.begin(), units.end(), *QRandomGenerator::global());
        int totalQuestions = 0;
        for (const auto& unit : units) totalQuestions += static_cast<int>(unit.size());
        const int target = qBound(1, params.value("count").toInt(totalQuestions), qMax(1, totalQuestions));
        // 逐个单元整体加入，直到题量达到或超过目标；题组永远整体加入，
        // 即使最后一个题组会让总题量超过目标也不截断（见交接方案 8.3）。
        QJsonArray selected;
        for (const auto& unit : units) {
            if (selected.size() >= target) break;
            for (const auto& question : unit) selected.append(question);
        }
        activeQuestions_ = selected;
        answers_.clear(); activeCatalogTitle_ = catalog.value("title").toString(bankTitle_);
        return {{"id", id}, {"result", QJsonObject{{"attemptId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"title", activeCatalogTitle_}, {"status", "active"}, {"questionCount", selected.size()}}}};
    }
    if (method == "attempt.questions") return {{"id", id}, {"result", QJsonObject{
        {"materials", hostMaterials()}, {"questions", hostQuestions(false)}}}};
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
    if (method == "attempt.solutions") return {{"id", id}, {"result", QJsonObject{
        {"materials", hostMaterials()}, {"solutions", hostQuestions(true)}}}};
    return error(id, QStringLiteral("声明式题库不支持此操作：%1").arg(method));
}

}  // namespace quizpane
