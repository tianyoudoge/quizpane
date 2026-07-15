#include "quizpane/studio/generation_workflow.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QNetworkAccessManager>
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

    // 工作流的模型 HTTP 边界由 model_client_test 覆盖；这里仅验证离线资料组，
    // 不监听端口、不启动服务器，也不依赖网络调度。
    QNetworkAccessManager manager;
    quizpane::studio::GenerationWorkflow workflow(&manager);
    quizpane::studio::GeneratedBankCandidate ready;
    bool finished = false;
    QObject::connect(&workflow, &quizpane::studio::GenerationWorkflow::questionsReady, &app,
                     [&](const auto& candidate) { ready = candidate; });
    QObject::connect(&workflow, &quizpane::studio::GenerationWorkflow::finished, &app,
                     [&] { finished = true; });
    const QList<quizpane::studio::SourceMaterialGroup> sources{
        {questionPath, answerPath}};
    workflow.startRuleBased(sources);

    if (!finished) return 4;
    if (ready.questions.size() != 1 || !ready.needsReviewQuestions.isEmpty()) return 4;
    const QJsonObject question = ready.questions.first().toObject();
    if (question.value("answer").toObject().value("optionIds").toArray() !=
        QJsonArray{"a"}) return 5;
    return 0;
}
