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

// 单像素扫描把近白背景转成透明（O(width*height)，一次遍历），保留彩色和
// 深色内容；不用 OpenCV，也不做卷积/边缘检测，纯定点数运算，低端设备也能跑。
// 具体判定逻辑（色度 chroma + 亮度 luma 双阈值）见 .cpp 实现。
ImagePrivacyResult removeNearWhiteBackground(
    const QImage& source,
    const WhiteBackgroundOptions& options = {});

}  // namespace quizpane
