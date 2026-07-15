#pragma once

#include <QJsonObject>
#include <QString>

namespace quizpane {

inline bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}

QJsonObject readJsonObjectFile(const QString& path, qint64 maximumBytes,
                               QString* error = nullptr);

}  // namespace quizpane
