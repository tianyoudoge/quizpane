#pragma once

#include <QString>
#include <memory>

class QImage;

namespace quizpane::studio {

// QPdfDocument 在部分平台返回带透明背景的页面；裁切界面必须先铺成白纸，
// 否则透明区域会透出深色画布，形成黑底黑字。
QImage flattenReviewPage(const QImage& source);

// UI-thread-only: one open source PDF, with a small byte-bounded page cache.
// Never shares the generation worker's QPdfDocument.
class ReviewPdfCache final {
public:
    explicit ReviewPdfCache(int maxKiB = 24 * 1024);
    ~ReviewPdfCache();
    QImage renderPage(const QString& sourcePath, int page, QString* error);
    void clear();
    int cachedKiB() const;
    static QString sourceIdentity(const QString& sourcePath);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace quizpane::studio
