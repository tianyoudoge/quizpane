#include "quizpane/studio/generation_workflow.hpp"

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
    return 0;
}
