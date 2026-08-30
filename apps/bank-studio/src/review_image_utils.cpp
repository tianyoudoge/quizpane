#include "review_image_utils.hpp"

#include <QImage>
#include <QPainter>

namespace quizpane::studio {

QImage flattenReviewPage(const QImage& source) {
    if (source.isNull())
        return {};
    QImage flattened(source.size(), QImage::Format_RGB32);
    flattened.fill(Qt::white);
    QPainter painter(&flattened);
    painter.drawImage(0, 0, source);
    return flattened;
}

}  // namespace quizpane::studio
