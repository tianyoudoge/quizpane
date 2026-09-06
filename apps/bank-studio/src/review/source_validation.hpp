#pragma once

#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace quizpane::studio {

inline bool acceptedSource(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
#ifdef QUIZPANE_HAS_QT_PDF
    return QStringList{"txt", "md", "markdown", "docx", "pdf"}.contains(suffix);
#else
    return QStringList{"txt", "md", "markdown", "docx"}.contains(suffix);
#endif
}

}  // namespace quizpane::studio
