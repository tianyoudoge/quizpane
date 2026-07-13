#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace {

int validateSource(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical().noquote() << "无法读取题库源文件：" << file.errorString();
        return 2;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        qCritical().noquote() << "JSON 格式错误：" << parseError.errorString();
        return 3;
    }

    const QJsonObject root = document.object();
    const QString title = root.value(QStringLiteral("title")).toString().trimmed();
    const QJsonArray questions = root.value(QStringLiteral("questions")).toArray();
    if (title.isEmpty() || questions.isEmpty()) {
        qCritical() << "题库必须包含非空 title 和 questions 数组。";
        return 4;
    }

    for (qsizetype i = 0; i < questions.size(); ++i) {
        const QJsonObject question = questions.at(i).toObject();
        const QJsonArray options = question.value(QStringLiteral("options")).toArray();
        const int answer = question.value(QStringLiteral("answer")).toInt(-1);
        if (question.value(QStringLiteral("content")).toString().trimmed().isEmpty() ||
            options.size() < 2 || answer < 0 || answer >= options.size()) {
            qCritical().noquote()
                << QStringLiteral("第 %1 题无效：需要 content、至少两个选项以及有效 answer。")
                       .arg(i + 1);
            return 5;
        }
    }

    qInfo().noquote() << QStringLiteral("校验通过：%1（%2 题）")
                             .arg(title)
                             .arg(questions.size());
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("quizpane-bank-generator"));

    const QStringList arguments = app.arguments();
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--validate"))
        return validateSource(arguments.at(2));

    QTextStream(stdout)
        << "QuizPane 题库生成器（开发中）\n\n"
        << "当前能力：校验题库源 JSON。\n"
        << "用法：quizpane-bank-generator --validate <bank-source.json>\n"
        << "正式 .quizpane-bank 分块、压缩、签名和加密输出尚未完成。\n";
    return 0;
}
