#include "quizpane/studio/suite_answer_matcher.hpp"

#include <QCoreApplication>

namespace {

using quizpane::studio::SuiteQuestionAnchor;

void appendSuite(QStringList* lines, QList<SuiteQuestionAnchor>* anchors,
                 const QString& title, int firstNumber, int count) {
    lines->append(title);
    for (int index = 0; index < count; ++index) {
        lines->append(QStringLiteral("%1. 第 %1 题").arg(firstNumber + index));
        anchors->append({static_cast<int>(lines->size() - 1), firstNumber + index});
        lines->append(QStringLiteral("A. 甲 B. 乙 C. 丙 D. 丁"));
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace quizpane::studio;

    if (normalizeQilinSuiteKey(QStringLiteral("２０２１ 联考 资料分析 ａ")) !=
            QStringLiteral("2021联考A") ||
        normalizeQilinSuiteKey(QStringLiteral("2022 四川 数量关系（上半年）")) !=
            QStringLiteral("2022四川(上半年)") ||
        normalizeQilinSuiteKey(QStringLiteral("模拟预测 ７")) != QStringLiteral("模拟预测7") ||
        !normalizeQilinSuiteKey(QStringLiteral("表：2021联考资料分析A" )).isEmpty())
        return 1;

    // 02/03：题目标题含科目词，末尾答案标题省略科目词。题号不是从 1 开始，
    // 答案仍须按套内连续顺序映射，而不是拿字符下标当原始题号。
    QStringList dataLines;
    QList<SuiteQuestionAnchor> dataAnchors;
    appendSuite(&dataLines, &dataAnchors, QStringLiteral("2021联考资料分析A"), 106, 10);
    appendSuite(&dataLines, &dataAnchors,
                QStringLiteral("2022四川资料分析（上半年）"), 86, 10);
    dataLines.append(QStringLiteral("【参考答案】"));
    dataLines.append(QStringLiteral("2021 联考 A"));
    dataLines.append(QStringLiteral("【参考答案】ABCDABCDAB"));
    dataLines.append(QStringLiteral("2022 四川（上半年）"));
    dataLines.append(QStringLiteral("【参考答案】DCBADCBA DC"));
    const auto data = matchQilinSuiteAnswers(dataLines, dataAnchors);
    if (!data.recognized || data.suiteQuestionLines.size() != 20 ||
        data.answersByQuestionLine.size() != 20 || !data.rejectedQuestionLines.isEmpty() ||
        data.answersByQuestionLine.value(dataAnchors.at(0).line) != QStringLiteral("A") ||
        data.answersByQuestionLine.value(dataAnchors.at(9).line) != QStringLiteral("B") ||
        data.answersByQuestionLine.value(dataAnchors.at(10).line) != QStringLiteral("D") ||
        data.answersByQuestionLine.value(dataAnchors.at(19).line) != QStringLiteral("C"))
        return 2;

    // 标题必须严格占整行；材料中提及的相似字符串不可建 section。
    QStringList strictLines{
        QStringLiteral("本节回顾 2021联考资料分析A"),
        QStringLiteral("1. 普通题"),
        QStringLiteral("A. 甲 B. 乙"),
        QStringLiteral("【参考答案】"),
        QStringLiteral("2021 联考 A"),
        QStringLiteral("【参考答案】A")};
    const auto strict = matchQilinSuiteAnswers(strictLines, {{1, 1}});
    if (strict.recognized || !strict.answersByQuestionLine.isEmpty())
        return 3;

    // 缺题号或错题号时整套拒绝，不能为了让长度相等把后面的答案前移。
    QStringList brokenLines;
    QList<SuiteQuestionAnchor> brokenAnchors;
    appendSuite(&brokenLines, &brokenAnchors,
                QStringLiteral("2024联考资料分析B"), 111, 10);
    brokenAnchors[4].number = 114; // 原应 115，制造重复/不连续题号。
    brokenLines.append(QStringLiteral("【参考答案】"));
    brokenLines.append(QStringLiteral("2024 联考 B"));
    brokenLines.append(QStringLiteral("【参考答案】ABCDABCDAB"));
    const auto broken = matchQilinSuiteAnswers(brokenLines, brokenAnchors);
    if (!broken.recognized || !broken.answersByQuestionLine.isEmpty() ||
        broken.rejectedQuestionLines.size() != 10 || broken.warnings.isEmpty())
        return 4;

    // 06：答案位于各套末尾；相同题号在下一套重新出现也不能相互覆盖。
    QStringList quantityLines;
    QList<SuiteQuestionAnchor> quantityAnchors;
    appendSuite(&quantityLines, &quantityAnchors, QStringLiteral("2024四川数量关系"), 46, 10);
    quantityLines.append(QStringLiteral("【参考答案】ABCDABCDAB"));
    appendSuite(&quantityLines, &quantityAnchors, QStringLiteral("2025四川数量关系"), 46, 10);
    quantityLines.append(QStringLiteral("【参考答案】DCBADCBA DC"));
    const auto quantity = matchQilinSuiteAnswers(quantityLines, quantityAnchors);
    if (!quantity.recognized || quantity.answersByQuestionLine.size() != 20 ||
        quantity.answersByQuestionLine.value(quantityAnchors.at(0).line) != QStringLiteral("A") ||
        quantity.answersByQuestionLine.value(quantityAnchors.at(10).line) != QStringLiteral("D") ||
        quantity.answerLines.size() != 2)
        return 5;

    // 明示“缺失”的答案行不能压缩成 13 个答案再错配到现存题目。
    QStringList missingLines;
    QList<SuiteQuestionAnchor> missingAnchors;
    appendSuite(&missingLines, &missingAnchors, QStringLiteral("2020浙江数量关系"), 6, 3);
    for (int number = 11; number <= 20; ++number) {
        missingLines.append(QStringLiteral("%1. 第 %1 题").arg(number));
        missingAnchors.append({static_cast<int>(missingLines.size() - 1), number});
        missingLines.append(QStringLiteral("A. 甲 B. 乙 C. 丙 D. 丁"));
    }
    missingLines.append(QStringLiteral("【参考答案】ACD 缺失 ADBDA CCBBB"));
    const auto missing = matchQilinSuiteAnswers(missingLines, missingAnchors);
    if (!missing.recognized || !missing.answersByQuestionLine.isEmpty() ||
        missing.rejectedQuestionLines.size() != 13 || missing.warnings.isEmpty())
        return 6;

    // 07 一天一题：严格等长的 111-115、141-145 可安全映射；21-25 有附加
    // 练习答案而多出字符，整组必须跳过，不能把后续答案左移。
    QStringList dailyLines{QStringLiteral("一天一题学数量")};
    QList<SuiteQuestionAnchor> dailyAnchors;
    for (int number = 1; number <= 145; ++number) {
        dailyLines.append(QStringLiteral("%1-题干").arg(number));
        dailyAnchors.append({static_cast<int>(dailyLines.size() - 1), number});
    }
    dailyLines.append(QStringLiteral("【参考答案】一天一题学数量"));
    dailyLines.append(QStringLiteral("21-25 DACDBAC"));
    dailyLines.append(QStringLiteral("111-115 AABDD        116-120 ADCBC"));
    dailyLines.append(QStringLiteral("141-145 BACDD"));
    dailyLines.append(QStringLiteral("【参考答案】葫芦兄弟系列"));
    const auto daily = matchQilinDailyQuantityAnswers(dailyLines, dailyAnchors);
    if (daily.contains(dailyAnchors.at(20).line) ||
        daily.value(dailyAnchors.at(118).line) != QStringLiteral("B") ||
        daily.value(dailyAnchors.at(144).line) != QStringLiteral("D"))
        return 7;

    return 0;
}
