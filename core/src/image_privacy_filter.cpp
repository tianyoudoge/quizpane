#include "quizpane/image_privacy_filter.hpp"

#include <QtGlobal>

namespace quizpane {

ImagePrivacyResult removeNearWhiteBackground(
    const QImage& source, const WhiteBackgroundOptions& options) {
    // 单次像素扫描，时间复杂度 O(width * height)，不依赖 OpenCV。对白底题图，
    // 按亮度把近白像素映射为透明，比边缘检测更符合“只隐藏纸张背景”的目标。
    if (source.isNull()) return {};

    QImage output = source.convertToFormat(QImage::Format_ARGB32);
    const int transparentLuma = qBound(1, options.fullyTransparentLuma, 255);
    const int opaqueLuma = qBound(0, options.fullyOpaqueLuma,
                                  transparentLuma - 1);
    const int lumaRange = transparentLuma - opaqueLuma;
    const int protectedChroma = qMax(0, options.colorProtectionChroma);
    const int contentAlpha = qBound(1, options.cropAlphaThreshold, 255);

    int left = output.width();
    int top = output.height();
    int right = -1;
    int bottom = -1;

    for (int y = 0; y < output.height(); ++y) {
        auto* pixels = reinterpret_cast<QRgb*>(output.scanLine(y));
        for (int x = 0; x < output.width(); ++x) {
            const QRgb pixel = pixels[x];
            int red = qRed(pixel);
            int green = qGreen(pixel);
            int blue = qBlue(pixel);
            const int originalAlpha = qAlpha(pixel);
            const int maximum = qMax(red, qMax(green, blue));
            const int minimum = qMin(red, qMin(green, blue));
            const int chroma = maximum - minimum;
            const int luma = (77 * red + 150 * green + 29 * blue) >> 8;

            int alpha = originalAlpha;
            if (originalAlpha > 0 && chroma <= protectedChroma &&
                luma > opaqueLuma) {
                const int coverage = qBound(
                    0, ((transparentLuma - luma) * 255) / lumaRange, 255);
                alpha = (originalAlpha * coverage + 127) / 255;

                // 去掉抗锯齿边缘原有的白色底色，避免文字放到暗色毛玻璃上后
                // 出现浅色光晕。coverage 是该像素仍属于前景的比例。
                if (originalAlpha == 255 && coverage > 0 && coverage < 255) {
                    red = qBound(0, 255 - ((255 - red) * 255) / coverage, 255);
                    green = qBound(0, 255 - ((255 - green) * 255) / coverage, 255);
                    blue = qBound(0, 255 - ((255 - blue) * 255) / coverage, 255);
                }
            }
            pixels[x] = qRgba(red, green, blue, alpha);
            if (alpha >= contentAlpha) {
                left = qMin(left, x);
                right = qMax(right, x);
                top = qMin(top, y);
                bottom = qMax(bottom, y);
            }
        }
    }

    ImagePrivacyResult result;
    result.processedPixels = qsizetype(output.width()) * output.height();
    if (right < left || bottom < top) {
        result.image = QImage(1, 1, QImage::Format_ARGB32);
        result.image.fill(Qt::transparent);
        return result;
    }

    QRect content(QPoint(left, top), QPoint(right, bottom));
    if (options.cropTransparentMargins) {
        const int padding = qMax(0, options.cropPadding);
        content.adjust(-padding, -padding, padding, padding);
        content = content.intersected(output.rect());
        result.image = output.copy(content);
    } else {
        result.image = output;
    }
    result.contentRectInSource = content;
    return result;
}

}  // namespace quizpane
