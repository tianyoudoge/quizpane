#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>

#include "quizpane/image_privacy_filter.hpp"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    QImage sample(100, 80, QImage::Format_ARGB32);
    sample.fill(Qt::white);
    for (int y = 15; y < 65; ++y)
        for (int x = 20; x < 80; ++x)
            sample.setPixelColor(x, y, (x < 50) ? Qt::black : QColor(60, 130, 220));

    const auto filtered = quizpane::removeNearWhiteBackground(sample);
    if (filtered.image.isNull() || filtered.processedPixels != 8000) return 1;
    if (filtered.contentRectInSource.left() > 20 ||
        filtered.contentRectInSource.top() > 15 ||
        filtered.contentRectInSource.right() < 79 ||
        filtered.contentRectInSource.bottom() < 64) return 2;
    if (qAlpha(filtered.image.pixel(0, 0)) != 0) return 3;
    const QPoint blackPoint(20 - filtered.contentRectInSource.left(),
                            20 - filtered.contentRectInSource.top());
    if (qAlpha(filtered.image.pixel(blackPoint)) != 255) return 4;

    QImage coloredNearWhite(1, 1, QImage::Format_ARGB32);
    coloredNearWhite.setPixelColor(0, 0, QColor(225, 240, 255));
    quizpane::WhiteBackgroundOptions noCrop;
    noCrop.cropTransparentMargins = false;
    const auto protectedColor =
        quizpane::removeNearWhiteBackground(coloredNearWhite, noCrop);
    if (qAlpha(protectedColor.image.pixel(0, 0)) != 255) return 5;

    QImage large(1920, 1080, QImage::Format_ARGB32);
    large.fill(Qt::white);
    QElapsedTimer timer;
    timer.start();
    const auto benchmark = quizpane::removeNearWhiteBackground(large, noCrop);
    if (benchmark.processedPixels != qsizetype(1920) * 1080) return 6;
    // Generous debug-build guard: catches accidental multi-pass or quadratic work.
    if (timer.elapsed() > 1500) return 7;
    return 0;
}
