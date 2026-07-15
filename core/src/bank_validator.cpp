#include "quizpane/bank_validator.hpp"

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

namespace quizpane {
namespace {

const QStringList& practiceModes() {
    static const QStringList modes{"all", "sequential", "random"};
    return modes;
}

const QStringList& questionTypes() {
    static const QStringList types{"single_choice", "true_false"};
    return types;
}

bool validId(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,95}$"));
    return pattern.match(id).hasMatch();
}

bool hasOnlyKeys(const QJsonObject& object, const QSet<QString>& allowed) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        if (!allowed.contains(it.key())) return false;
    return true;
}

bool validAsset(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject asset = value.toObject();
    static const QSet<QString> keys{"path", "alt"};
    static const QRegularExpression pathPattern(
        QStringLiteral("^assets/[A-Za-z0-9._/-]+$"));
    return hasOnlyKeys(asset, keys) && asset.value("path").isString() &&
        pathPattern.match(asset.value("path").toString()).hasMatch() &&
        (!asset.contains("alt") || (asset.value("alt").isString() &&
                                    asset.value("alt").toString().size() <= 500));
}

QSet<QString> validateCatalogs(const QJsonArray& catalogs, QList<BankValidationError>* errors) {
    QSet<QString> catalogIds;
    for (const auto& value : catalogs) {
        if (!value.isObject()) {
            errors->append({-1, {}, QStringLiteral("分类必须是 JSON 对象"), {}});
            continue;
        }
        const auto catalog = value.toObject();
        static const QSet<QString> catalogKeys{"id", "title", "description", "practice"};
        static const QSet<QString> practiceKeys{"mode", "questionCount", "preferMistakes"};
        const QString id = catalog.value("id").toString();
        const QString title = catalog.value("title").toString();
        const QJsonObject practice = catalog.value("practice").toObject();
        const QString mode = practice.value("mode").toString();
        const int count = practice.value("questionCount").toInt();
        // 把原先一条超长 OR 拆成多条细校验：每种失败给不同 message，便于题库
        // 制作器在"检查问题"页定位到底是 id 重复、标题空、模式非法还是题量越界。
        const auto reject = [&](const QString& reason) {
            errors->append({-1, {}, QStringLiteral("分类 %1：%2").arg(id.isEmpty() ? QStringLiteral("（无标识）") : id, reason), {}});
        };
        if (!hasOnlyKeys(catalog, catalogKeys)) { reject(QStringLiteral("包含 Schema 未声明的字段")); continue; }
        if (!hasOnlyKeys(practice, practiceKeys)) { reject(QStringLiteral("组卷配置包含未声明字段")); continue; }
        if (!validId(id)) { reject(QStringLiteral("标识不符合规范")); continue; }
        if (catalogIds.contains(id)) { reject(QStringLiteral("标识重复")); continue; }
        if (!catalog.value("title").isString() || title.trimmed().isEmpty()) { reject(QStringLiteral("标题为空")); continue; }
        if (title.size() > 80) { reject(QStringLiteral("标题超过 80 字")); continue; }
        if (!catalog.value("practice").isObject()) { reject(QStringLiteral("缺少组卷配置")); continue; }
        if (practice.contains("questionCount") && (!practice.value("questionCount").isDouble() ||
            practice.value("questionCount").toDouble() != count || count < 1 || count > 200)) {
            reject(QStringLiteral("每套题数量需为 1–200")); continue;
        }
        if (practice.contains("preferMistakes") && !practice.value("preferMistakes").isBool()) {
            reject(QStringLiteral("preferMistakes 必须是布尔值")); continue;
        }
        if (catalog.contains("description") && (!catalog.value("description").isString() ||
            catalog.value("description").toString().size() > 500)) {
            reject(QStringLiteral("描述超过 500 字")); continue;
        }
        if (!practiceModes().contains(mode)) { reject(QStringLiteral("组卷模式 %1 不支持").arg(mode)); continue; }
        catalogIds.insert(id);
    }
    return catalogIds;
}

// 材料 ID -> 材料对象，供题目按 materialId 交叉校验分类一致性和正文重复。
QHash<QString, QJsonObject> validateMaterials(const QJsonArray& materials, const QSet<QString>& catalogIds,
                                              QList<BankValidationError>* errors) {
    QHash<QString, QJsonObject> materialsById;
    for (const auto& value : materials) {
        if (!value.isObject()) {
            errors->append({-1, {}, QStringLiteral("材料必须是 JSON 对象"), {}});
            continue;
        }
        const auto material = value.toObject();
        static const QSet<QString> materialKeys{"id", "catalogId", "title", "body", "images", "source"};
        const QString id = material.value("id").toString();
        const QString title = material.value("title").toString();
        const QString body = material.value("body").toString();
        const auto images = material.value("images").toArray();
        bool imagesValid = true;
        for (const auto& imageValue : images) {
            if (!validAsset(imageValue)) { imagesValid = false; break; }
        }
        // 材料可以是纯文本、纯图片或两者组合：body 与 images 至少一个非空。
        const bool hasBody = material.contains("body") && material.value("body").isString() &&
            !body.trimmed().isEmpty() && body.size() <= 100000;
        const bool hasImages = material.contains("images") && !images.isEmpty();
        if (!hasOnlyKeys(material, materialKeys) || !validId(id) || materialsById.contains(id) ||
            !catalogIds.contains(material.value("catalogId").toString()) ||
            (material.contains("title") &&
                (!material.value("title").isString() || title.size() > 200)) ||
            (material.contains("images") && (!material.value("images").isArray() || images.size() > 20 ||
                !imagesValid)) ||
            (!hasBody && !hasImages)) {
            errors->append({-1, {}, QStringLiteral("材料 %1 的标识、分类或内容无效").arg(id), id});
            continue;
        }
        if (material.contains("source")) {
            const QJsonObject source = material.value("source").toObject();
            static const QSet<QString> sourceKeys{"document", "page"};
            const int page = source.value("page").toInt();
            if (!material.value("source").isObject() || !hasOnlyKeys(source, sourceKeys) ||
                (source.contains("document") && (!source.value("document").isString() ||
                    source.value("document").toString().size() > 300)) ||
                (source.contains("page") && (!source.value("page").isDouble() ||
                    source.value("page").toDouble() != page || page < 1))) {
                errors->append({-1, {}, QStringLiteral("材料 %1 的来源信息无效").arg(id), id});
            }
        }
        materialsById.insert(id, material);
    }
    return materialsById;
}

// 校验单道题的选项、答案、review、source 子结构。materialId 相关的引用规则
// （不存在的材料、分类不一致、正文完整复制进 stem）由调用方在此之前处理，
// 因为需要访问 materialsById，不属于"题目内部结构"的范畴。
void validateQuestionCommon(const QJsonObject& question, int index, const QString& id,
                            QList<BankValidationError>* errors) {
    const auto options = question.value("options").toArray();
    const auto answers = question.value("answer").toObject().value("optionIds").toArray();
    QSet<QString> optionIds;
    bool optionsValid = true;
    for (const auto& optionValue : options) {
        if (!optionValue.isObject()) {
            errors->append({index, id, QStringLiteral("第 %1 题的选项必须是 JSON 对象").arg(index + 1), {}});
            optionsValid = false;
            break;
        }
        const auto option = optionValue.toObject();
        static const QSet<QString> optionKeys{"id", "text", "image"};
        const QString optionId = option.value("id").toString();
        const QString optionText = option.value("text").toString();
        if (!hasOnlyKeys(option, optionKeys) || !validId(optionId) || optionIds.contains(optionId) ||
            !option.value("text").isString() || optionText.trimmed().isEmpty() ||
            optionText.size() > 4000 || (option.contains("image") && !validAsset(option.value("image")))) {
            errors->append({index, id, QStringLiteral("第 %1 题存在空白或重复选项").arg(index + 1), {}});
            optionsValid = false;
            break;
        }
        optionIds.insert(optionId);
    }
    if (!optionsValid) return;

    if (!answers.isEmpty() && !optionIds.contains(answers.first().toString())) {
        errors->append({index, id, QStringLiteral("第 %1 题的答案没有对应选项").arg(index + 1), {}});
    }
    if (question.contains("review")) {
        const QJsonObject review = question.value("review").toObject();
        static const QSet<QString> reviewKeys{"confidence", "needsReview", "reason"};
        const double confidence = review.value("confidence").toDouble(-1);
        if (!question.value("review").isObject() || !hasOnlyKeys(review, reviewKeys) ||
            (review.contains("confidence") && (confidence < 0 || confidence > 1)) ||
            (review.contains("needsReview") && !review.value("needsReview").isBool()) ||
            (review.contains("reason") && !review.value("reason").isString()) ||
            review.value("reason").toString().size() > 1000) {
            errors->append({index, id, QStringLiteral("第 %1 题的复核信息无效").arg(index + 1), {}});
        }
    }
    if (question.contains("source")) {
        const QJsonObject source = question.value("source").toObject();
        static const QSet<QString> sourceKeys{"document", "page"};
        const int page = source.value("page").toInt();
        if (!question.value("source").isObject() || !hasOnlyKeys(source, sourceKeys) ||
            (source.contains("document") && (!source.value("document").isString() ||
                source.value("document").toString().size() > 300)) ||
            (source.contains("page") && (!source.value("page").isDouble() ||
                source.value("page").toDouble() != page || page < 1))) {
            errors->append({index, id, QStringLiteral("第 %1 题的来源信息无效").arg(index + 1), {}});
        }
    }
}

}  // namespace

QList<BankValidationError> validateBankDetailed(const QJsonObject& bank) {
    QList<BankValidationError> errors;
    static const QSet<QString> bankKeys{
        "schemaVersion", "title", "description", "catalogs", "materials", "questions"};
    if (!hasOnlyKeys(bank, bankKeys))
        errors.append({-1, {}, QStringLiteral("题库包含 Schema 未声明的字段"), {}});
    const QString bankTitle = bank.value("title").toString();
    if (bank.value("schemaVersion").toInt() != 2 || !bank.value("title").isString() ||
        bankTitle.trimmed().isEmpty() || bankTitle.size() > 120) {
        errors.append({-1, {}, QStringLiteral("题库必须包含 schemaVersion=2 和非空 title"), {}});
    }
    if (bank.contains("description") && (!bank.value("description").isString() ||
        bank.value("description").toString().size() > 2000))
        errors.append({-1, {}, QStringLiteral("题库描述格式无效或超过 2000 字"), {}});
    if (!bank.value("catalogs").isArray())
        errors.append({-1, {}, QStringLiteral("catalogs 必须是数组"), {}});
    if (bank.contains("materials") && !bank.value("materials").isArray())
        errors.append({-1, {}, QStringLiteral("materials 必须是数组"), {}});
    if (!bank.value("questions").isArray())
        errors.append({-1, {}, QStringLiteral("questions 必须是数组"), {}});
    const QJsonArray catalogs = bank.value("catalogs").toArray();
    const QJsonArray materials = bank.value("materials").toArray();
    const QJsonArray questions = bank.value("questions").toArray();
    if (catalogs.isEmpty()) errors.append({-1, {}, QStringLiteral("题库至少需要一个分类"), {}});
    if (questions.isEmpty()) errors.append({-1, {}, QStringLiteral("题库至少需要一道题"), {}});

    const QSet<QString> catalogIds = validateCatalogs(catalogs, &errors);
    const QHash<QString, QJsonObject> materialsById = validateMaterials(materials, catalogIds, &errors);

    QSet<QString> questionIds;
    QSet<QString> referencedMaterialIds;
    for (qsizetype index = 0; index < questions.size(); ++index) {
        if (!questions.at(index).isObject()) {
            errors.append({int(index), {}, QStringLiteral("第 %1 题必须是 JSON 对象").arg(index + 1), {}});
            continue;
        }
        const auto question = questions.at(index).toObject();
        static const QSet<QString> questionKeys{"id", "catalogId", "materialId", "type", "stem",
            "stemImage", "options", "answer", "solution", "source", "review"};
        const QString id = question.value("id").toString();
        const QString type = question.value("type").toString();
        const QString stem = question.value("stem").toString();
        const auto options = question.value("options").toArray();
        static const QSet<QString> answerKeys{"optionIds"};
        const auto answers = question.value("answer").toObject().value("optionIds").toArray();

        const auto reject = [&](const QString& reason) {
            errors.append({int(index), id,
                QStringLiteral("第 %1 题：%2").arg(index + 1).arg(reason), {}});
        };
        bool invalid = false;
        const auto require = [&](bool condition, const QString& reason) {
            if (!condition && !invalid) { reject(reason); invalid = true; }
        };
        require(hasOnlyKeys(question, questionKeys), QStringLiteral("包含 Schema 未声明的字段"));
        require(validId(id), QStringLiteral("题目标识不符合规范"));
        require(!questionIds.contains(id), QStringLiteral("题目标识重复"));
        require(catalogIds.contains(question.value("catalogId").toString()), QStringLiteral("引用了不存在的分类"));
        require(questionTypes().contains(type), QStringLiteral("题型不受支持"));
        require(question.value("stem").isString() && !stem.trimmed().isEmpty() && stem.size() <= 20000,
                QStringLiteral("题干为空、格式错误或超过 20000 字"));
        require(question.value("options").isArray() && options.size() >= 2 && options.size() <= 20,
                QStringLiteral("选项必须为 2–20 项的数组"));
        require(question.value("answer").isObject() &&
                hasOnlyKeys(question.value("answer").toObject(), answerKeys) &&
                question.value("answer").toObject().value("optionIds").isArray() && answers.size() == 1,
                QStringLiteral("单选答案必须且只能包含一个 optionId"));
        require(question.contains("solution") && question.value("solution").isString() &&
                question.value("solution").toString().size() <= 20000,
                QStringLiteral("解析缺失、格式错误或超过 20000 字"));
        require(!question.contains("stemImage") || validAsset(question.value("stemImage")),
                QStringLiteral("题干图片资源无效"));
        if (invalid) continue;
        questionIds.insert(id);

        if (question.contains("materialId")) {
            const QString materialId = question.value("materialId").toString();
            if (!validId(materialId) || !materialsById.contains(materialId)) {
                errors.append({int(index), id,
                    QStringLiteral("第 %1 题引用了不存在的材料：%2").arg(index + 1).arg(materialId),
                    materialId});
            } else {
                const QJsonObject material = materialsById.value(materialId);
                if (material.value("catalogId").toString() != question.value("catalogId").toString()) {
                    errors.append({int(index), id,
                        QStringLiteral("第 %1 题与其材料 %2 的分类不一致").arg(index + 1).arg(materialId),
                        materialId});
                }
                // 不允许材料正文完整复制进关联题目的 stem。用包含关系覆盖
                // "复制材料再补一点文字"的明显回退，而不仅仅是逐字相等。
                const QString body = material.value("body").toString().trimmed();
                if (body.size() >= 20 && stem.trimmed().contains(body)) {
                    errors.append({int(index), id,
                        QStringLiteral("第 %1 题的题干完整复制了材料 %2 的正文").arg(index + 1).arg(materialId),
                        materialId});
                }
                referencedMaterialIds.insert(materialId);
            }
        }
        validateQuestionCommon(question, int(index), id, &errors);
    }

    // 禁止孤立材料：至少一道题必须引用每份材料。
    for (auto it = materialsById.constBegin(); it != materialsById.constEnd(); ++it) {
        if (!referencedMaterialIds.contains(it.key())) {
            errors.append({-1, {}, QStringLiteral("材料 %1 没有任何题目引用").arg(it.key()), it.key()});
        }
    }
    return errors;
}

bool validateBank(const QJsonObject& bank, QString* error) {
    const auto errors = validateBankDetailed(bank);
    if (errors.isEmpty()) {
        if (error) error->clear();
        return true;
    }
    if (error) *error = errors.first().message;
    return false;
}

}  // namespace quizpane
