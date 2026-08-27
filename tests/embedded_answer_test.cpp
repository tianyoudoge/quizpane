#include "quizpane/studio/rule_based_generator.hpp"
#include "quizpane/bank_validator.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QJsonObject>

using namespace quizpane::studio;

namespace {
const QString options = QStringLiteral("\nA.朝议\nB.谏议\nC.祭祀\nD.太学\n");
struct Case { QString stem, expected, answer, suffix; bool review = false; };
bool check(const Case& c, bool hasAnswerKey = true) {
    ExtractedDocument doc;
    doc.sourcePath = "embedded.txt";
    doc.plainText = "1." + c.stem + options + c.suffix;
    const auto result = RuleBasedBankGenerator{}.generate({doc}, hasAnswerKey);
    const auto items = c.review ? result.needsReviewQuestions : result.questions;
    if (items.size() != 1) {
        qCritical() << "wrong classification" << c.stem << result.needsReviewQuestions;
        return false;
    }
    const auto q = items.first().toObject();
    if (q.value("stem").toString() != c.expected) {
        qCritical() << "stem mismatch" << c.stem << q.value("stem"); return false;
    }
    if (!hasAnswerKey) return !q.contains("answer");
    QString answer;
    for (const auto& v : q.value("answer").toObject().value("optionIds").toArray()) answer += v.toString();
    if (!c.review && answer != c.answer) { qCritical() << "wrong answer" << c.stem << answer; return false; }
    if (c.review && !q.value("review").toObject().value("reason").toString().contains(QStringLiteral("括号")))
        return false;
    return true;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QList<Case> cases{
        {QStringLiteral("周公旦（ A ），规定了吉礼（祭礼）。"), QStringLiteral("周公旦（　），规定了吉礼（祭礼）。"), "a"},
        {QStringLiteral("（ B ）是指某种制度。"), QStringLiteral("（　）是指某种制度。"), "b"},
        {QStringLiteral("该制度指（C）。"), QStringLiteral("该制度指（　）。"), "c"},
        {QStringLiteral("该制度指（D）"), QStringLiteral("该制度指（　）"), "d"},
        {QStringLiteral("该制度指(a)，不含注释（并非祭祀）。"), QStringLiteral("该制度指(　)，不含注释（并非祭祀）。"), "a"},
        {QStringLiteral("该制度指（Ａ）。"), QStringLiteral("该制度指（　）。"), "a"},
        {QStringLiteral("多选题：制度包括（ A、 C ）。"), QStringLiteral("多选题：制度包括（　）。"), "ac"},
        {QStringLiteral("多选题：制度包括（CA）。"), QStringLiteral("多选题：制度包括（　）。"), "ca", QStringLiteral("答案：AC\n")},
        {QStringLiteral("该制度指\n（ B\n）。"), QStringLiteral("该制度指（　）。"), "b"},
        {QStringLiteral("该制度指[A]。"), QStringLiteral("该制度指[　]。"), "a"},
        {QStringLiteral("该制度指（A）。"), QStringLiteral("该制度指（　）。"), "a", QStringLiteral("答案：朝议\n")},
        {QStringLiteral("该制度指（A）。"), QStringLiteral("该制度指（A）。"), {}, QStringLiteral("答案：B\n"), true},
        {QStringLiteral("该制度指（A）。"), QStringLiteral("该制度指（A）。"), {}, QStringLiteral("答案\n1.B\n"), true},
        {QStringLiteral("该制度指（A）。"), QStringLiteral("该制度指（A）。"), {}, QStringLiteral("答案：B\n答案：A\n"), true},
        {QStringLiteral("（A）与（B）哪个正确？"), QStringLiteral("（A）与（B）哪个正确？"), {}, {}, true},
        {QStringLiteral("（A）与（A）哪个正确？"), QStringLiteral("（A）与（A）哪个正确？"), {}, {}, true},
        {QStringLiteral("该制度指（F）。"), QStringLiteral("该制度指（F）。"), {}, {}, true},
        {QStringLiteral("该制度简称（ABC）。"), QStringLiteral("该制度简称（ABC）。"), {}, {}, true},
        {QStringLiteral("吉礼（祭礼）、军礼（行军，出征）如何分类？"), QStringLiteral("吉礼（祭礼）、军礼（行军，出征）如何分类？"), "b", QStringLiteral("答案：B\n")},
        {QStringLiteral("考察（2026）、（A+B=3）和（DNA）含义。"), QStringLiteral("考察（2026）、（A+B=3）和（DNA）含义。"), "a", QStringLiteral("答案：A\n")},
        {QStringLiteral("保留错误括号（A)。"), QStringLiteral("保留错误括号（A)。"), "a", QStringLiteral("答案：A\n")},
    };
    for (const auto& c : cases) if (!check(c)) return 1;
    if (!check(cases.first(), false) || !check(cases.at(14), false)) return 2;

    // 同行选项、续行前缀、选项内的括号注释不得混入题干或被当成答案。
    for (const QString& body : {
        QStringLiteral("1.（ B ）是指某制度。 A.朝议 B.谏议 C.祭祀 D.太学\n"),
        QStringLiteral("1.某制度\n指（ B ）。 A.朝议 B.谏议 C.祭祀 D.太学\n")}) {
        ExtractedDocument doc; doc.sourcePath = "inline.txt"; doc.plainText = body;
        const auto r = RuleBasedBankGenerator{}.generate({doc});
        if (r.questions.size() != 1) return 3;
        const auto q = r.questions.first().toObject();
        if (q.value("options").toArray().size() != 4 || !q.value("stem").toString().contains(QStringLiteral("（　）")) ||
            q.value("answer").toObject().value("optionIds").toArray() != QJsonArray{"b"}) return 4;
    }
    ExtractedDocument annotation; annotation.sourcePath = "option.txt";
    annotation.plainText = QStringLiteral("1.选出正确的项\nA.朝议（A）\nB.谏议（B）\n答案：B\n");
    const auto annotated = RuleBasedBankGenerator{}.generate({annotation});
    if (annotated.questions.size() != 1 || annotated.questions.first().toObject().value("options").toArray().first()
            .toObject().value("text").toString() != QStringLiteral("朝议（A）")) return 5;
    annotation.plainText = QStringLiteral("1.这是（ A ）题\n(1)甲\n(2)乙\n");
    if (RuleBasedBankGenerator{}.generate({annotation}).questions.size() != 1) return 10;
    annotation.plainText = QStringLiteral("1.这是（ A ）题\nA.甲\n");
    const auto incomplete = RuleBasedBankGenerator{}.generate({annotation});
    if (incomplete.needsReviewQuestions.size() != 1 ||
        !incomplete.needsReviewQuestions.first().toObject().value("stem").toString().contains("A")) return 11;
    annotation.plainText = QStringLiteral("1.甲（A）\nA.甲\nB.乙\n1.乙（B）\nA.甲\nB.乙\n");
    const auto duplicates = RuleBasedBankGenerator{}.generate({annotation});
    if (!duplicates.questions.isEmpty() || duplicates.needsReviewQuestions.size() != 2) return 12;
    for (const auto& value : duplicates.needsReviewQuestions) {
        const auto q = value.toObject();
        if (q.value("source").toObject().value("questionNumber").toInt() != 1 ||
            !q.value("review").toObject().value("reason").toString().contains(QStringLiteral("重复"))) return 13;
    }
    // 原卷空题号不能倒吞上一题的续行和分行选项。
    annotation.plainText = QStringLiteral("138.这是（B）的\n完整题干。\nA.甲\nB.乙\n139.\n140.另题（A）\nA.甲\nB.乙\n");
    const auto emptyNumber = RuleBasedBankGenerator{}.generate({annotation});
    if (emptyNumber.questions.size() != 2 || emptyNumber.needsReviewQuestions.size() != 1 ||
        emptyNumber.questions.first().toObject().value("stem").toString() != QStringLiteral("这是（　）的完整题干。") ||
        !emptyNumber.needsReviewQuestions.first().toObject().value("stem").toString().isEmpty()) return 14;
    annotation.plainText = QStringLiteral("1.①先提出观点\n②再阐述理由\n正确排序是（B）。\nA.①②\nB.②①\n");
    const auto numberedStatements = RuleBasedBankGenerator{}.generate({annotation});
    if (numberedStatements.questions.size() != 1) return 15;
    const auto numbered = numberedStatements.questions.first().toObject();
    if (!numbered.value("stem").toString().contains(QStringLiteral("①先提出观点")) ||
        !numbered.value("stem").toString().contains(QStringLiteral("②再阐述理由")) ||
        numbered.value("options").toArray().size() != 2) return 16;

    // 删除括号内部多个字节后，后面的下划线仍指向原词（包含 UTF-16 代理对）。
    ExtractedDocument decorated; decorated.sourcePath = "decorated.txt";
    decorated.hasPageBoundaries = true;
    const QString line = QStringLiteral("1.😀（ B ）之后的重要词");
    decorated.plainText = line + options;
    decorated.underlineDecorations[1].append({line, {{line.indexOf(QStringLiteral("重要词")), 3}}, {}, {}});
    const auto r = RuleBasedBankGenerator{}.generate({decorated});
    if (r.questions.size() != 1) return 6;
    const auto q = r.questions.first().toObject();
    const auto ranges = q.value("stemUnderlines").toArray();
    if (ranges.size() != 1) return 7;
    const auto range = ranges.first().toObject();
    if (q.value("stem").toString().mid(range.value("start").toInt(), range.value("length").toInt()) != QStringLiteral("重要词")) return 8;
    const QJsonObject bank{{"schemaVersion", 2}, {"title", "Test"},
        {"catalogs", QJsonArray{QJsonObject{{"id", "generated"}, {"title", "Test"},
            {"practice", QJsonObject{{"mode", "all"}}}}}}, {"questions", r.questions}};
    QString error;
    if (!quizpane::validateBank(bank, &error)) { qCritical() << error; return 9; }
    return 0;
}
