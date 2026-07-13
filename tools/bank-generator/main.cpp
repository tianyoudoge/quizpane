#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>

#include "quizpane/bank_validator.hpp"
#include "quizpane/zip_archive.hpp"

namespace {

QJsonObject readObject(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取 %1：%2").arg(path, file.errorString()); return {};
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        *error = QStringLiteral("%1 不是有效 JSON：%2").arg(path, parseError.errorString()); return {};
    }
    return document.object();
}

int validateFile(const QString& path) {
    QString error; const QJsonObject bank = readObject(path, &error);
    if (bank.isEmpty() || !quizpane::validateBank(bank, &error)) { qCritical().noquote() << error; return 2; }
    qInfo().noquote() << QStringLiteral("校验通过：%1（%2 题）").arg(bank.value("title").toString()).arg(bank.value("questions").toArray().size());
    return 0;
}

int package(const QString& bankPath, const QString& manifestPath, const QString& outputPath) {
    QString error; const QJsonObject bank = readObject(bankPath, &error); const QJsonObject manifest = readObject(manifestPath, &error);
    if (bank.isEmpty() || manifest.isEmpty() || !quizpane::validateBank(bank, &error)) { qCritical().noquote() << error; return 2; }
    const QJsonObject runtime = manifest.value("runtime").toObject();
    if (manifest.value("manifestVersion").toInt() != 2 || manifest.value("kind") != QStringLiteral("declarative") ||
        runtime.value("format") != QStringLiteral("quizpane.bank+json") || runtime.value("schemaVersion").toInt() != 2 ||
        runtime.value("entry") != QStringLiteral("content/bank.json")) {
        qCritical() << "Manifest 必须是入口为 content/bank.json 的 declarative v2"; return 3;
    }
    if (!quizpane::writeZipArchive(outputPath, {
            {QStringLiteral("manifest.json"), QJsonDocument(manifest).toJson(QJsonDocument::Indented)},
            {QStringLiteral("content/bank.json"), QJsonDocument(bank).toJson(QJsonDocument::Indented)}},
            &error)) {
        qCritical().noquote() << QStringLiteral("题库包写入失败：%1").arg(error); return 4;
    }
    qInfo().noquote() << QStringLiteral("已生成：%1").arg(QFileInfo(outputPath).absoluteFilePath());
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() == 3 && args.at(1) == QStringLiteral("--validate")) return validateFile(args.at(2));
    if (args.size() == 5 && args.at(1) == QStringLiteral("--package")) return package(args.at(2), args.at(3), args.at(4));
    QTextStream(stdout) << "QuizPane 声明式题库 Harness\n\n"
        << "校验：quizpane-bank-generator --validate <bank.json>\n"
        << "打包：quizpane-bank-generator --package <bank.json> <manifest.json> <output.quizpane-provider>\n";
    return 0;
}
