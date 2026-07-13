#pragma once

#include <QImage>
#include <QRect>

namespace quizpane {

// 图片隐蔽化算法的参数对象，类似 Spring 中传给 service 的配置 DTO。
// 默认值针对白底题图：白色越接近背景，透明度越低，并可裁掉空白边缘。
struct WhiteBackgroundOptions {
    int fullyTransparentLuma = 248;
    int fullyOpaqueLuma = 218;
    int colorProtectionChroma = 18;
    int cropAlphaThreshold = 8;
    int cropPadding = 6;
    bool cropTransparentMargins = true;
};

struct ImagePrivacyResult {
    QImage image;
    QRect contentRectInSource;
    qsizetype processedPixels = 0;
};

// Converts neutral near-white pixels to transparency in one linear pass.
// Colored pixels and dark information are preserved. No OpenCV or convolution
// is used, making the operation suitable for low-end desktop hardware.
ImagePrivacyResult removeNearWhiteBackground(
    const QImage& source,
    const WhiteBackgroundOptions& options = {});

}  // namespace quizpane
