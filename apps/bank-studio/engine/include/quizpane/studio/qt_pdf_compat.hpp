#pragma once

#include <QPdfDocument>
#include <QSizeF>
#include <QString>
#include <QtGlobal>

namespace quizpane::studio {

inline int loadPdfDocument(QPdfDocument* document, const QString& path) {
    return static_cast<int>(document->load(path));
}

inline bool pdfLoadSucceeded(int error) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return error == static_cast<int>(QPdfDocument::Error::None);
#else
    return error == static_cast<int>(QPdfDocument::NoError);
#endif
}

inline QSizeF pdfPagePointSize(const QPdfDocument* document, int page) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return document->pagePointSize(page);
#else
    return document->pageSize(page);
#endif
}

}  // namespace quizpane::studio
