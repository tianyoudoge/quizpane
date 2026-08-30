#include "quizpane/studio/generation_workflow.hpp"
#include "quizpane/zip_archive.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTimer>
#include <QTemporaryDir>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    qputenv("QUIZPANE_GENERATION_TASKS_ROOT", directory.filePath("tasks").toUtf8());

    const QString questionPath = directory.filePath(QStringLiteral("questions.txt"));
    QFile questions(questionPath);
    if (!questions.open(QIODevice::WriteOnly)) return 2;
    questions.write("1. Which one is A?\nA. Alpha\nB. Beta\n");
    questions.close();

    const QString answerPath = directory.filePath(QStringLiteral("answers.txt"));
    QFile answers(answerPath);
    if (!answers.open(QIODevice::WriteOnly)) return 3;
    answers.write("答案及解析\n1. Alpha is the first option.\n故正确答案为 A。\n");
    answers.close();

    // 工作流只编排离线资料组：不监听端口、不启动服务器，也不依赖网络调度。
    quizpane::studio::GenerationWorkflow workflow;
    quizpane::studio::GeneratedBankCandidate ready;
    bool finished = false;
    QObject::connect(&workflow, &quizpane::studio::GenerationWorkflow::questionsReady, &app,
                     [&](const auto& candidate) { ready = candidate; });
    QObject::connect(&workflow, &quizpane::studio::GenerationWorkflow::finished, &app,
                     [&] { finished = true; });
    const QList<quizpane::studio::SourceMaterialGroup> sources{
        {questionPath, answerPath}};
    workflow.startRuleBased(sources);

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &app, &QCoreApplication::quit);
    QObject::connect(&workflow, &quizpane::studio::GenerationWorkflow::finished,
                     &app, &QCoreApplication::quit);
    timeout.start(5000);
    app.exec();
    if (!finished) return 4;
    if (ready.questions.size() != 1 || !ready.needsReviewQuestions.isEmpty()) return 4;
    const QJsonObject question = ready.questions.first().toObject();
    if (question.value("answer").toObject().value("optionIds").toArray() !=
        QJsonArray{"a"}) return 5;

    // 答案另册走 MinerU 时必须使用 mineruAnswerZipPath，而不是悄悄回到本地
    // PDF 文字层。夹具还带一个 discarded footer，用于确保适配器输出才是实际
    // 被合并进题目文档的内容。
    QFile answerLayout(QStringLiteral(MINERU_ANSWER_LAYOUT_FIXTURE));
    if (!answerLayout.open(QIODevice::ReadOnly)) return 6;
    const QString mineruAnswerZip = directory.filePath(QStringLiteral("answer-result.zip"));
    QString zipError;
    if (!quizpane::writeZipArchive(
            mineruAnswerZip,
            {{QStringLiteral("layout.json"), answerLayout.readAll()}}, &zipError))
        return 7;
    const QString cloudAnswerPath = directory.filePath(QStringLiteral("answers.pdf"));
    QFile cloudAnswer(cloudAnswerPath);
    if (!cloudAnswer.open(QIODevice::WriteOnly)) return 8;
    cloudAnswer.write("%PDF-1.7 answer companion placeholder");
    cloudAnswer.close();

    ready = {};
    finished = false;
    workflow.startRuleBased(QList<quizpane::studio::SourceMaterialGroup>{
        {questionPath, cloudAnswerPath, quizpane::studio::AnswerPolicyHint::Included,
         {}, mineruAnswerZip}});
    timeout.start(5000);
    app.exec();
    if (!finished || ready.questions.size() != 1 || !ready.needsReviewQuestions.isEmpty())
        return 9;
    const QJsonObject cloudAnswerQuestion = ready.questions.first().toObject();
    if (cloudAnswerQuestion.value("answer").toObject().value("optionIds").toArray() !=
        QJsonArray{"a"})
        return 10;
    if (ready.warnings.join(QStringLiteral(";")).contains(QStringLiteral("答案册页脚")))
        return 11;

    // 默认 Auto：没有任何答案证据时应自动生成无答案题库，不能把整份资料因
    // “缺少答案”打成 hard 复核。
    ready = {};
    finished = false;
    workflow.startRuleBased(QStringList{questionPath});
    timeout.start(5000);
    app.exec();
    if (!finished || ready.hasAnswerKey || ready.questions.size() != 1 ||
        !ready.needsReviewQuestions.isEmpty() ||
        ready.questions.first().toObject().contains(QStringLiteral("answer"))) return 12;

    // 只识别到“参考答案”标题、却没有任何可解析答案值时，标题不能把 Auto
    // 锁死为含答案库。此类残缺/扫描资料应按无答案语义重跑并正常收录题目。
    const QString emptyAnswerSectionPath =
        directory.filePath(QStringLiteral("empty-answer-section.txt"));
    QFile emptyAnswerSection(emptyAnswerSectionPath);
    if (!emptyAnswerSection.open(QIODevice::WriteOnly)) return 19;
    emptyAnswerSection.write(
        "1. Which one is A?\nA. Alpha\nB. Beta\n参考答案\n答案内容未识别\n");
    emptyAnswerSection.close();
    ready = {};
    finished = false;
    workflow.startRuleBased(QStringList{emptyAnswerSectionPath});
    timeout.start(5000);
    app.exec();
    if (!finished || ready.hasAnswerKey || ready.questions.size() != 1 ||
        !ready.needsReviewQuestions.isEmpty() ||
        ready.questions.first().toObject().contains(QStringLiteral("answer"))) return 20;

    // 孤立答案记录没有匹配到任何具体题目，覆盖率为 0，不能再一票否决地把
    // Auto 锁成含答案题库。
    const QString unmatchedAnswerPath =
        directory.filePath(QStringLiteral("unmatched-answer.txt"));
    QFile unmatchedAnswer(unmatchedAnswerPath);
    if (!unmatchedAnswer.open(QIODevice::WriteOnly)) return 21;
    unmatchedAnswer.write(
        "1. Which one is A?\nA. Alpha\nB. Beta\n参考答案\n99. A\n");
    unmatchedAnswer.close();
    ready = {};
    finished = false;
    workflow.startRuleBased(QStringList{unmatchedAnswerPath});
    timeout.start(5000);
    app.exec();
    if (!finished || ready.hasAnswerKey || ready.questions.size() != 1 ||
        !ready.needsReviewQuestions.isEmpty()) return 22;

    const auto writeCoverageFixture = [&](const QString& name, int answered) {
        const QString path = directory.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return QString();
        QByteArray content;
        for (int number = 1; number <= 20; ++number) {
            content += QByteArray::number(number) + ". Coverage question?\nA. Alpha\nB. Beta\n";
            if (number <= answered)
                content += "答案：A\n";
        }
        if (file.write(content) != content.size()) return QString();
        return path;
    };

    // 20 题只匹配 1 题答案，覆盖率恰为 5%：按产品规则视为无答案题库。
    const QString fivePercentPath = writeCoverageFixture(QStringLiteral("five-percent.txt"), 1);
    if (fivePercentPath.isEmpty()) return 23;
    ready = {};
    finished = false;
    workflow.startRuleBased(QStringList{fivePercentPath});
    timeout.start(5000);
    app.exec();
    if (!finished || ready.hasAnswerKey || ready.questions.size() != 20 ||
        !ready.needsReviewQuestions.isEmpty()) return 24;

    // 高于阈值则保留含答案语义；未匹配题仍进入复核，不猜答案。
    const QString tenPercentPath = writeCoverageFixture(QStringLiteral("ten-percent.txt"), 2);
    if (tenPercentPath.isEmpty()) return 25;
    ready = {};
    finished = false;
    workflow.startRuleBased(QStringList{tenPercentPath});
    timeout.start(5000);
    app.exec();
    if (!finished || !ready.hasAnswerKey || ready.questions.size() != 2 ||
        ready.needsReviewQuestions.size() != 18) return 26;

    const QString embeddedPath = directory.filePath(QStringLiteral("embedded-answer.txt"));
    QFile embedded(embeddedPath);
    if (!embedded.open(QIODevice::WriteOnly)) return 17;
    embedded.write("1. The embedded answer is (B).\nA. Alpha\nB. Beta\n");
    embedded.close();
    ready = {};
    finished = false;
    workflow.startRuleBased(QStringList{embeddedPath});
    timeout.start(5000);
    app.exec();
    if (!finished || !ready.hasAnswerKey || ready.questions.size() != 1 ||
        !ready.needsReviewQuestions.isEmpty() ||
        ready.questions.first().toObject().value(QStringLiteral("answer")).toObject()
            .value(QStringLiteral("optionIds")).toArray() != QJsonArray{"b"}) return 18;

    // 同一个答案册覆盖多套、且各套题号从 1 重启时，必须先按套题标题分发，
    // 不能把答案全文只留给最后一套或按裸题号串错。
    const QString bookletPath = directory.filePath(QStringLiteral("booklet.txt"));
    QFile booklet(bookletPath);
    if (!booklet.open(QIODevice::WriteOnly)) return 13;
    booklet.write(
        "第1套试题\n1. First?\nA. Alpha\nB. Beta\n"
        "第2套试题\n1. Second?\nA. Alpha\nB. Beta\n");
    booklet.close();
    const QString bookletAnswersPath = directory.filePath(QStringLiteral("booklet-answers.txt"));
    QFile bookletAnswers(bookletAnswersPath);
    if (!bookletAnswers.open(QIODevice::WriteOnly)) return 14;
    bookletAnswers.write("第1套试题\n1.A\n第2套试题\n1.B\n");
    bookletAnswers.close();
    ready = {};
    finished = false;
    workflow.startRuleBased(QList<quizpane::studio::SourceMaterialGroup>{
        {bookletPath, bookletAnswersPath}});
    timeout.start(5000);
    app.exec();
    if (!finished || !ready.hasAnswerKey || ready.questions.size() != 2 ||
        !ready.needsReviewQuestions.isEmpty()) return 15;
    const QJsonObject first = ready.questions.at(0).toObject();
    const QJsonObject second = ready.questions.at(1).toObject();
    if (first.value(QStringLiteral("answer")).toObject()
            .value(QStringLiteral("optionIds")).toArray() != QJsonArray{"a"} ||
        second.value(QStringLiteral("answer")).toObject()
            .value(QStringLiteral("optionIds")).toArray() != QJsonArray{"b"} ||
        first.value(QStringLiteral("source")).toObject()
            .value(QStringLiteral("sectionTitle")).toString() != QStringLiteral("第1套试题") ||
        second.value(QStringLiteral("source")).toObject()
            .value(QStringLiteral("sectionTitle")).toString() != QStringLiteral("第2套试题")) return 16;
    return 0;
}
