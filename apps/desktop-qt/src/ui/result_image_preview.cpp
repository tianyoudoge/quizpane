#include "result_image_preview.hpp"

#include <QImage>
#include <QLabel>
#include <QPixmap>

namespace quizpane::ui {

int resultImagePixelWidth() {
    return 680;
}

QList<CompactResultRow> compactResultRows(const QStringList& answerTokens,
                                          int groupSize) {
    QList<CompactResultRow> rows;
    if (groupSize <= 0)
        return rows;
    for (int first = 0; first < answerTokens.size(); first += groupSize) {
        const int count = qMin(groupSize, answerTokens.size() - first);
        CompactResultRow row;
        row.rangeLabel = QStringLiteral("%1–%2题").arg(first + 1).arg(first + count);
        for (int offset = 0; offset < count; ++offset)
            row.answerSequence += answerTokens.at(first + offset);
        rows.append(row);
    }
    return rows;
}

void setResultPreviewImage(QLabel* preview, const QImage& image) {
    if (!preview)
        return;
    preview->setPixmap(QPixmap::fromImage(image));
    preview->adjustSize();
}

}  // namespace quizpane::ui
