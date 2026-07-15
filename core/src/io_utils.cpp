#include "quizpane/io_utils.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>

namespace quizpane {

QJsonObject readJsonObjectFile(const QString& path, qint64 maximumBytes,
                               QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, file.errorString());
        return {};
    }
    if (file.size() < 0 || file.size() > maximumBytes) {
        fail(error, QStringLiteral("JSON 文件超过 %1 字节限制").arg(maximumBytes));
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        fail(error, QStringLiteral("JSON 无效：%1").arg(parseError.errorString()));
        return {};
    }
    return document.object();
}

}  // namespace quizpane
