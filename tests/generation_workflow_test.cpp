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
        {questionPath, cloudAnswerPath, true, {}, mineruAnswerZip}});
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
    return 0;
}
