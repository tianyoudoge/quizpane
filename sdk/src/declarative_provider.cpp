#include "quizpane/declarative_provider.hpp"

#include "quizpane/bank_validator.hpp"
#include "quizpane/io_utils.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QUrl>
#include <QSettings>
#include <algorithm>

namespace quizpane {
namespace {
QJsonObject readObject(const QString& path) {
    return readJsonObjectFile(path, 128 * 1024 * 1024);
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
    // 规则导入器用这些保留 token 表达 PDF 文字层看不见的填空横线；在宿主端
    // 统一转成真正可见的下划线，既不把占位混成普通空格，也不会影响普通题干。
    escaped.replace(QStringLiteral("〔填空〕"),
                    QStringLiteral("<span style=\"display:inline-block; min-width:4em; border-bottom:1px solid #c8cdd3;\">&nbsp;</span>"));
    escaped.replace('\n', QStringLiteral("<br>"));
    return QStringLiteral("<p>%1</p>").arg(escaped);
}
enum class Method {
    Initialize, Capabilities, CatalogList, AttemptCreate, AttemptQuestions,
    SaveAnswers, Submit, Report, Solutions, Unknown
};
Method methodForName(const QString& name) {
    static const QHash<QString, Method> methods{
        {QStringLiteral("provider.initialize"), Method::Initialize},
        {QStringLiteral("provider.capabilities"), Method::Capabilities},
        {QStringLiteral("catalog.list"), Method::CatalogList},
        {QStringLiteral("attempt.create"), Method::AttemptCreate},
        {QStringLiteral("attempt.questions"), Method::AttemptQuestions},
        {QStringLiteral("attempt.saveAnswers"), Method::SaveAnswers},
        {QStringLiteral("attempt.submit"), Method::Submit},
        {QStringLiteral("attempt.report"), Method::Report},
        {QStringLiteral("attempt.solutions"), Method::Solutions}};
    return methods.value(name, Method::Unknown);
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
    bankDirectory_ = QFileInfo(bankPath).absoluteDir().absolutePath();
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
    materialsById_.clear(); questionCountByCatalog_.clear(); answers_.clear(); bankDirectory_.clear();
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

QString DeclarativeProvider::assetUrl(const QJsonObject& asset) const {
    const QString relative = asset.value("path").toString();
    if (relative.isEmpty() || bankDirectory_.isEmpty()) return {};
    const QString path = QDir(bankDirectory_).absoluteFilePath(QStringLiteral("../") + relative);
    return QFileInfo::exists(path) ? QUrl::fromLocalFile(path).toString() : QString{};
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
            units[it.value()].append(question);
        }
    }
    return units;
}

QJsonArray DeclarativeProvider::hostQuestions(bool withSolutions) const {
    QJsonArray result;
    for (const auto& value : activeQuestions_) {
        const QJsonObject source = value.toObject();
        const QJsonArray answerIds = source.value("answer").toObject().value("optionIds").toArray();
        QJsonArray options;
        int correct = -1;
        const auto sourceOptions = source.value("options").toArray();
        for (qsizetype index = 0; index < sourceOptions.size(); ++index) {
            const auto option = sourceOptions.at(index).toObject();
            for (const auto& answerId : answerIds)
                if (option.value("id").toString() == answerId.toString()) correct = static_cast<int>(index);
            QJsonObject hostedOption{{"index", index},
                                     {"label", QString(QChar(char16_t(u'A' + index)))},
                                     {"contentHtml", paragraph(option.value("text").toString())}};
            const QString imageUrl = assetUrl(option.value("image").toObject());
            if (!imageUrl.isEmpty())
                hostedOption.insert("imageUrl", imageUrl);
            options.append(hostedOption);
        }
        QString content = paragraph(source.value("stem").toString());
        const QString stemImageUrl = assetUrl(source.value("stemImage").toObject());
        if (!stemImageUrl.isEmpty())
            content += QStringLiteral("<p><img src=\"%1\" width=\"340\"></p>").arg(stemImageUrl);
        QJsonObject question{{"id", source.value("id")}, {"type", source.value("type")},
            {"contentHtml", content}, {"options", options}};
        if (source.contains("materialId")) question.insert("materialId", source.value("materialId"));
        if (withSolutions) {
            question.insert("correctChoice", correct);
            QJsonArray correctChoices;
            for (const auto& answerId : answerIds) {
                for (qsizetype index = 0; index < sourceOptions.size(); ++index)
                    if (sourceOptions.at(index).toObject().value("id").toString() == answerId.toString())
                        correctChoices.append(index);
            }
            question.insert("correctChoices", correctChoices);
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
        QString content = paragraph(material.value("body").toString());
        QJsonArray imageUrls;
        for (const auto& image : material.value("images").toArray()) {
            const QString url = assetUrl(image.toObject());
            if (!url.isEmpty()) imageUrls.append(url);
        }
        result.append(QJsonObject{{"id", materialId}, {"title", material.value("title")},
            {"contentHtml", content}, {"imageUrls", imageUrls}});
    }
    return result;
}

QJsonObject DeclarativeProvider::request(const QJsonObject& requestValue) {
    const QJsonValue id = requestValue.value("id");
    const QString method = requestValue.value("method").toString();
    const Method dispatch = methodForName(method);
    const QJsonObject params = requestValue.value("params").toObject();
    if (dispatch == Method::Initialize) return {{"id", id}, {"result", QJsonObject{
        {"providerId", providerId_}, {"providerVersion", providerVersion_}, {"requiresLogin", false}, {"sessionRestored", true}}}};
    if (dispatch == Method::Capabilities) return {{"id", id}, {"result", QJsonObject{{"loginMethods", QJsonArray{"none"}}}}};
    if (dispatch == Method::CatalogList) {
        QJsonArray nodes;
        QSettings settings;
        for (const auto& value : catalogs_) {
            const QJsonObject catalog = value.toObject(); const QString catalogId = catalog.value("id").toString();
            const int count = questionCountByCatalog_.value(catalogId, 0);
            const QJsonObject practice = catalog.value("practice").toObject();
            int suggested = practice.value("mode").toString() == QStringLiteral("all")
                ? count : practice.value("questionCount").toInt(qMin(15, count));
            suggested = qBound(1, suggested, qMax(1, count));
            int mastered = 0, mistakes = 0;
            for (const auto& questionValue : questions_) {
                const QJsonObject question = questionValue.toObject();
                if (question.value("catalogId").toString() != catalogId) continue;
                const QVariant state = settings.value(QStringLiteral("practice/history/%1/%2")
                    .arg(providerId_, question.value("id").toString()));
                if (state.toString() == QStringLiteral("correct")) ++mastered;
                else if (state.toString() == QStringLiteral("wrong")) ++mistakes;
            }
            nodes.append(QJsonObject{{"id", catalogId}, {"title", catalog.value("title")},
                {"availableQuestionCount", count}, {"canStartAttempt", count > 0},
                {"suggestedCounts", QJsonArray{suggested}}, {"masteredCount", mastered},
                {"mistakeCount", mistakes}});
        }
        return {{"id", id}, {"result", QJsonObject{{"nodes", nodes}}}};
    }
    if (dispatch == Method::AttemptCreate) {
        const QJsonObject catalog = findCatalog(params.value("categoryId").toString());
        if (catalog.isEmpty()) return error(id, QStringLiteral("练习分类不存在"));
        QVector<QJsonArray> units = buildUnits(catalog.value("id").toString());
        if (!params.value("includePreviouslyAnswered").toBool(false)) {
            QSettings settings;
            QVector<QJsonArray> filtered;
            for (const QJsonArray& unit : units) {
                bool allMastered = true;
                for (const auto& value : unit)
                    allMastered = allMastered && settings.value(
                        QStringLiteral("practice/history/%1/%2").arg(
                            providerId_, value.toObject().value("id").toString())).toString() ==
                        QStringLiteral("correct");
                if (!allMastered) filtered.append(unit);
            }
            units = filtered;
        }
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
    if (dispatch == Method::AttemptQuestions) return {{"id", id}, {"result", QJsonObject{
        {"materials", hostMaterials()}, {"questions", hostQuestions(false)}}}};
    if (dispatch == Method::SaveAnswers) {
        for (const auto& value : params.value("answers").toArray()) {
            const auto answer = value.toObject(); QSet<int> choices;
            const QJsonValue choice = answer.value("answer").toObject().value("choice");
            const QJsonArray array = choice.toArray();
            if (!array.isEmpty()) for (const auto& item : array) { bool ok = false; const int index = item.toString().toInt(&ok); if (ok) choices.insert(index); }
            else { bool ok = false; const int index = choice.toString().toInt(&ok); if (ok) choices.insert(index); }
            const int index = answer.value("questionIndex").toInt(-1);
            if (index >= 0) { if (choices.isEmpty()) answers_.remove(index); else answers_.insert(index, choices); }
        }
        return {{"id", id}, {"result", QJsonObject{{"ok", true}}}};
    }
    if (dispatch == Method::Submit) return {{"id", id}, {"result", QJsonObject{{"ok", true}}}};
    if (dispatch == Method::Report) { int correct = 0; const auto solutions = hostQuestions(true); QSettings settings;
        for (qsizetype i = 0; i < solutions.size(); ++i) { QSet<int> expected; for (const auto& choice : solutions.at(i).toObject().value("correctChoices").toArray()) expected.insert(choice.toInt()); const bool passed = answers_.value(static_cast<int>(i)) == expected; if (passed) ++correct; settings.setValue(QStringLiteral("practice/history/%1/%2").arg(providerId_, solutions.at(i).toObject().value("id").toString()), passed ? QStringLiteral("correct") : QStringLiteral("wrong")); }
        return {{"id", id}, {"result", QJsonObject{{"questionCount", activeQuestions_.size()}, {"answerCount", answers_.size()}, {"correctCount", correct}}}};
    }
    if (dispatch == Method::Solutions) return {{"id", id}, {"result", QJsonObject{
        {"materials", hostMaterials()}, {"solutions", hostQuestions(true)}}}};
    return error(id, QStringLiteral("声明式题库不支持此操作：%1").arg(method));
}

}  // namespace quizpane
