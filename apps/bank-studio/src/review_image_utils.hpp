#pragma once

class QImage;

namespace quizpane::studio {

// QPdfDocument 在部分平台返回带透明背景的页面；裁切界面必须先铺成白纸，
// 否则透明区域会透出深色画布，形成黑底黑字。
QImage flattenReviewPage(const QImage& source);

}  // namespace quizpane::studio
