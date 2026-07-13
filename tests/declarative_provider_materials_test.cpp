#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTimer>

#include "quizpane/provider_loader.hpp"

namespace {
QJsonObject call(quizpane::ProviderLoader& loader, const QString& id,
                 const QString& method, const QJsonObject& params = {}) {
    QEventLoop loop;
    QJsonObject response;
    const auto connection = QObject::connect(
        &loader, &quizpane::ProviderLoader::responseReceived, &loop,
        [&](const QJsonObject& candidate) {
            if (candidate.value("id").toString() == id) { response = candidate; loop.quit(); }
        });
    QString error;
    if (!loader.request({{"id", id}, {"method", method}, {"params", params}}, &error)) return {};
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(connection);
    return response;
}
}  // namespace

// 验证共享材料题库能被 DeclarativeProvider 正确加载并按题组组卷：
// - attempt.questions/attempt.solutions 返回去重后的材料数组；
// - 顺序模式下同一材料的题目保持相邻，不被拆散；
// - 目标题量落在题组中间时保留完整题组，报告的 questionCount 大于目标值。
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    quizpane::ProviderLoader loader;
    QString error;
    if (!loader.load(QString::fromUtf8(DECLARATIVE_MATERIALS_BANK_PATH), &error)) return 1;

    // 目标题量 4：材料一有 3 道子题，材料二有 3 道子题，独立题各 1 道。
    // 顺序单元是 [材料一组(3), 材料二组(3), 独立题1, 独立题2]。
    // 累加到目标 4 时，加入材料一组(3) < 4，继续加入材料二组(3)，总数变成 6，
    // 超过目标但题组必须整体保留，因此实际题量应为 6，而不是被截断成 4。
    const auto attempt = call(loader, "create", "attempt.create",
        {{"categoryId", "verbal"}, {"count", 4}}).value("result").toObject();
    if (attempt.value("attemptId").toString().isEmpty()) return 2;
    if (attempt.value("questionCount").toInt() != 6) return 3;

    const auto questionsResult = call(loader, "questions", "attempt.questions")
                                      .value("result").toObject();
    const auto questions = questionsResult.value("questions").toArray();
    const auto materials = questionsResult.value("materials").toArray();
    if (questions.size() != 6) return 4;
    if (materials.size() != 2) return 5;

    // 顺序模式必须保持材料内部的原始顺序：同一 materialId 的题目连续出现，
    // 不能被拆开或与另一材料的题目交叉排列。
    QString previousMaterialId;
    QSet<QString> seenMaterialIds;
    for (const auto& value : questions) {
        const QString materialId = value.toObject().value("materialId").toString();
        if (materialId.isEmpty()) continue;
        if (materialId != previousMaterialId) {
            if (seenMaterialIds.contains(materialId)) return 6;
            seenMaterialIds.insert(materialId);
            previousMaterialId = materialId;
        }
    }

    // 每份材料都要带 contentHtml，且不能把材料正文拼进题目 contentHtml。
    for (const auto& value : materials) {
        const QJsonObject material = value.toObject();
        if (material.value("id").toString().isEmpty()) return 7;
        if (material.value("contentHtml").toString().isEmpty()) return 8;
    }
    for (const auto& value : questions) {
        const QJsonObject question = value.toObject();
        const QString materialId = question.value("materialId").toString();
        if (materialId.isEmpty()) continue;
        // 找到对应材料，确认题目 contentHtml 不包含材料正文（防止拼接兜底）。
        for (const auto& materialValue : materials) {
            const QJsonObject material = materialValue.toObject();
            if (material.value("id").toString() != materialId) continue;
            const QString materialHtml = material.value("contentHtml").toString();
            if (materialHtml.size() > 20 &&
                question.value("contentHtml").toString().contains(materialHtml))
                return 9;
        }
    }

    const auto solutionsResult = call(loader, "solutions", "attempt.solutions")
                                      .value("result").toObject();
    if (solutionsResult.value("solutions").toArray().size() != 6) return 10;
    if (solutionsResult.value("materials").toArray().size() != 2) return 11;

    return 0;
}
