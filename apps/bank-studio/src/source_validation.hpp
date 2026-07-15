#pragma once

#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace quizpane::studio {

inline bool acceptedSource(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return QStringList{"txt", "md", "markdown", "docx", "pdf"}.contains(suffix);
}

}  // namespace quizpane::studio
