#include "quizpane/declarative_provider.hpp"
#include "quizpane/draft_store.hpp"
#include "quizpane/provider_installer.hpp"
#include "quizpane/zip_archive.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;
    qputenv("QUIZPANE_PROVIDERS_ROOT", directory.filePath("providers").toUtf8());
    qputenv("QUIZPANE_DRAFTS_ROOT", directory.filePath("drafts").toUtf8());

    QFile fixture(QString::fromUtf8(DECLARATIVE_MATERIALS_BANK_PATH));
    if (!fixture.open(QIODevice::ReadOnly))
        return 2;
    const QByteArray bankJson = fixture.readAll();
    const QByteArray manifest =
        QJsonDocument(QJsonObject{{"manifestVersion", 2},
                                  {"id", "org.quizpane.material-e2e"},
                                  {"name", "Material E2E"},
                                  {"version", "1.0.0"},
                                  {"kind", "declarative"},
                                  {"runtime", QJsonObject{{"format", "quizpane.bank+json"},
                                                          {"schemaVersion", 2},
                                                          {"entry", "content/bank.json"}}},
                                  {"permissions", QJsonObject{{"network", false}}}})
            .toJson();
    const QString packagePath = directory.filePath(QStringLiteral("material.quizpane-provider"));
    if (!quizpane::writeZipArchive(packagePath, {{QStringLiteral("manifest.json"), manifest},
                                                 {QStringLiteral("content/bank.json"), bankJson}}))
        return 3;

    quizpane::ProviderInstaller installer;
    quizpane::ProviderPackageInfo info;
    quizpane::ProviderInstallResult installed;
    QString error;
    if (!installer.inspect(packagePath, &info, &error) ||
        !installer.install(info, &installed, &error))
        return 4;
    quizpane::DeclarativeProvider provider;
    if (!provider.load(installed.entryPath, &error))
        return 5;

    const QJsonObject attempt =
        provider
            .request({{"id", "create"},
                      {"method", "attempt.create"},
                      {"params", QJsonObject{{"categoryId", "verbal"}, {"count", 4}}}})
            .value("result")
            .toObject();
    if (attempt.value("attemptId").toString().isEmpty())
        return 6;
    const QJsonObject questionResult =
        provider
            .request(
                {{"id", "questions"}, {"method", "attempt.questions"}, {"params", QJsonObject{}}})
            .value("result")
            .toObject();
    if (questionResult.value("questions").toArray().size() != 6 ||
        questionResult.value("materials").toArray().size() != 2)
        return 7;

    quizpane::DraftSnapshot draft;
    draft.providerId = info.id;
    draft.attemptId = attempt.value("attemptId").toString();
    draft.title = QStringLiteral("材料题草稿");
    draft.questions = questionResult.value("questions").toArray();
    draft.materials = questionResult.value("materials").toArray();
    draft.answers = QVector<int>(draft.questions.size(), -1);
    draft.answers[0] = 0;
    draft.currentQuestionIndex = 1;
    quizpane::DraftStore drafts;
    if (!drafts.save(draft, &error))
        return 8;
    quizpane::DraftSnapshot restored;
    if (!drafts.load(info.id, &restored, &error) || restored.materials.size() != 2 ||
        restored.currentQuestionIndex != 1 || restored.answers.value(0) != 0)
        return 9;

    const QJsonObject submit = provider.request(
        {{"id", "submit"}, {"method", "attempt.submit"}, {"params", QJsonObject{}}});
    if (!submit.value("result").toObject().value("ok").toBool())
        return 10;
    const QJsonObject solutions =
        provider
            .request(
                {{"id", "solutions"}, {"method", "attempt.solutions"}, {"params", QJsonObject{}}})
            .value("result")
            .toObject();
    if (solutions.value("solutions").toArray().size() != 6 ||
        solutions.value("materials").toArray().size() != 2)
        return 11;
    return 0;
}
