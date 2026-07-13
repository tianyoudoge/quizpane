#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QtCore/private/qzipwriter_p.h>

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

bool validateBank(const QJsonObject& root, QString* error) {
    if (root.value("schemaVersion").toInt() != 1 || root.value("title").toString().trimmed().isEmpty()) {
        *error = QStringLiteral("题库必须包含 schemaVersion=1 和非空 title"); return false;
    }
    const QJsonArray catalogs = root.value("catalogs").toArray();
    const QJsonArray questions = root.value("questions").toArray();
    if (catalogs.isEmpty() || questions.isEmpty()) { *error = QStringLiteral("题库至少需要一个分类和一道题"); return false; }
    QSet<QString> catalogIds, questionIds;
    for (const auto& value : catalogs) {
        const auto catalog = value.toObject(); const QString id = catalog.value("id").toString();
        const QString mode = catalog.value("practice").toObject().value("mode").toString();
        if (id.isEmpty() || catalogIds.contains(id) || catalog.value("title").toString().isEmpty() ||
            !QStringList{"all", "sequential", "random"}.contains(mode)) {
            *error = QStringLiteral("分类标识重复，或分类标题/组卷模式无效：%1").arg(id); return false;
        }
        catalogIds.insert(id);
    }
    for (qsizetype index = 0; index < questions.size(); ++index) {
        const auto question = questions.at(index).toObject(); const QString id = question.value("id").toString();
        const QString type = question.value("type").toString(); const auto options = question.value("options").toArray();
        const auto answers = question.value("answer").toObject().value("optionIds").toArray();
        if (id.isEmpty() || questionIds.contains(id) || !catalogIds.contains(question.value("catalogId").toString()) ||
            !QStringList{"single_choice", "true_false"}.contains(type) || question.value("stem").toString().trimmed().isEmpty() ||
            options.size() < 2 || answers.size() != 1) {
            *error = QStringLiteral("第 %1 题的标识、分类、类型、题干、选项或答案无效").arg(index + 1); return false;
        }
        QSet<QString> optionIds;
        for (const auto& optionValue : options) {
            const auto option = optionValue.toObject(); const QString optionId = option.value("id").toString();
            if (optionId.isEmpty() || optionIds.contains(optionId) || option.value("text").toString().trimmed().isEmpty()) {
                *error = QStringLiteral("第 %1 题存在空白或重复选项").arg(index + 1); return false;
            }
            optionIds.insert(optionId);
        }
        if (!optionIds.contains(answers.first().toString())) {
            *error = QStringLiteral("第 %1 题的答案没有对应选项").arg(index + 1); return false;
        }
        questionIds.insert(id);
    }
    return true;
}

int validateFile(const QString& path) {
    QString error; const QJsonObject bank = readObject(path, &error);
    if (bank.isEmpty() || !validateBank(bank, &error)) { qCritical().noquote() << error; return 2; }
    qInfo().noquote() << QStringLiteral("校验通过：%1（%2 题）").arg(bank.value("title").toString()).arg(bank.value("questions").toArray().size());
    return 0;
}

int package(const QString& bankPath, const QString& manifestPath, const QString& outputPath) {
    QString error; const QJsonObject bank = readObject(bankPath, &error); const QJsonObject manifest = readObject(manifestPath, &error);
    if (bank.isEmpty() || manifest.isEmpty() || !validateBank(bank, &error)) { qCritical().noquote() << error; return 2; }
    const QJsonObject runtime = manifest.value("runtime").toObject();
    if (manifest.value("manifestVersion").toInt() != 2 || manifest.value("kind") != QStringLiteral("declarative") ||
        runtime.value("format") != QStringLiteral("quizpane.bank+json") || runtime.value("schemaVersion").toInt() != 1 ||
        runtime.value("entry") != QStringLiteral("content/bank.json")) {
        qCritical() << "Manifest 必须是入口为 content/bank.json 的 declarative v2"; return 3;
    }
    QZipWriter zip(outputPath);
    zip.setCompressionPolicy(QZipWriter::AutoCompress);
    zip.addFile(QStringLiteral("manifest.json"), QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    zip.addFile(QStringLiteral("content/bank.json"), QJsonDocument(bank).toJson(QJsonDocument::Indented));
    zip.close();
    if (zip.status() != QZipWriter::NoError) { qCritical() << "题库包写入失败"; return 4; }
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
