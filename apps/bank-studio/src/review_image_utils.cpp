#include "review_image_utils.hpp"

#include <QImage>
#include <QPainter>
#include <QCache>
#include <QDateTime>
#include <QFileInfo>
#include "quizpane/studio/pdf_memory_budget.hpp"
#ifdef QUIZPANE_HAS_QT_PDF
#include "quizpane/studio/qt_pdf_compat.hpp"
#endif

namespace quizpane::studio {

QImage flattenReviewPage(const QImage& source) {
    if (source.isNull())
        return {};
    QImage flattened(source.size(), QImage::Format_RGB32);
    if (flattened.isNull()) return {};
    flattened.fill(Qt::white);
    QPainter painter(&flattened);
    painter.drawImage(0, 0, source);
    return flattened;
}

class ReviewPdfCache::Impl {
public:
    explicit Impl(int maxKiB) : pages(qMax(0, maxKiB)) {}
    QString identity;
    QCache<int, QImage> pages;
#ifdef QUIZPANE_HAS_QT_PDF
    std::unique_ptr<QPdfDocument> document;
#endif
};

ReviewPdfCache::ReviewPdfCache(int maxKiB) : impl_(std::make_unique<Impl>(maxKiB)) {}
ReviewPdfCache::~ReviewPdfCache() = default;

QString ReviewPdfCache::sourceIdentity(const QString& sourcePath) {
    const QFileInfo info(sourcePath);
    if (!info.isFile()) return {};
    return info.canonicalFilePath() + QChar('\n') + QString::number(info.size()) +
        QChar('\n') + QString::number(info.lastModified().toMSecsSinceEpoch());
}

void ReviewPdfCache::clear() {
    impl_->pages.clear();
    impl_->identity.clear();
#ifdef QUIZPANE_HAS_QT_PDF
    impl_->document.reset();
#endif
}

int ReviewPdfCache::cachedKiB() const { return impl_->pages.totalCost(); }

QImage ReviewPdfCache::renderPage(const QString& sourcePath, int page, QString* error) {
    if (error) error->clear();
#ifdef QUIZPANE_HAS_QT_PDF
    const QString identity = sourceIdentity(sourcePath);
    if (identity != impl_->identity || identity.isEmpty())
        clear();
    if (identity.isEmpty() || page < 1) {
        if (error) *error = QStringLiteral("无法打开原卷第 %1 页").arg(page);
        return {};
    }
    if (!impl_->document) {
        auto document = std::make_unique<QPdfDocument>();
        if (!pdfLoadSucceeded(loadPdfDocument(document.get(), sourcePath))) {
            if (error) *error = QStringLiteral("无法打开原卷第 %1 页").arg(page);
            return {};
        }
        impl_->document = std::move(document);
        impl_->identity = identity;
    }
    if (page > impl_->document->pageCount()) {
        if (error) *error = QStringLiteral("原卷不存在第 %1 页").arg(page);
        return {};
    }
    if (const auto* cached = impl_->pages.object(page))
        return *cached;
    const QSizeF points = pdfPagePointSize(impl_->document.get(), page - 1);
    const QSize pixels(qBound(1, qRound(points.width() * 1.7), 1800),
                       qBound(1, qRound(points.height() * 1.7), 2400));
    if (!pdfImageAllocationAllowed(pixels)) {
        // Give the request one chance after releasing disposable cached pages.
        impl_->pages.clear();
        if (!pdfImageAllocationAllowed(pixels)) {
            if (error) *error = QStringLiteral("内存不足，无法预览原卷，请缩小文件后重试。");
            return {};
        }
    }
    const QImage image = flattenReviewPage(impl_->document->render(page - 1, pixels));
    if (image.isNull()) {
        if (error) *error = QStringLiteral("无法渲染原卷第 %1 页").arg(page);
        return {};
    }
    const int cost = int((qint64(image.bytesPerLine()) * image.height() + 1023) / 1024);
    impl_->pages.insert(page, new QImage(image), cost);
    return image;
#else
    Q_UNUSED(sourcePath)
    Q_UNUSED(page)
    if (error) *error = QStringLiteral("当前兼容构建未包含 PDF 原卷预览");
    return {};
#endif
}

}  // namespace quizpane::studio
