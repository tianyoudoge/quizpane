#include "quizpane/studio/document_extractor.hpp"

#include "quizpane/diagnostic_logger.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QImageWriter>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QSet>
#include <QStringConverter>
#include <QStringDecoder>
#include <QTextCodec>
#include <QXmlStreamReader>

#include <miniz.h>

#ifdef QUIZPANE_HAS_TESSERACT_OCR
#if __has_include(<tesseract/baseapi.h>)
#include <tesseract/baseapi.h>
#else
#include <baseapi.h>
#endif
#endif

#include <cmath>
#include <limits>

namespace quizpane::studio {
namespace {

bool hasSuffix(const QString& path, const QStringList& suffixes) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffixes.contains(suffix);
}

// UTF-8 严格解码失败（出现非法字节序列）时回退到 GB18030，覆盖国内用户
// 常见的 Windows 记事本"ANSI"编码保存的 TXT 文件。BOM 由 QStringDecoder
// 自动识别并跳过。GB18030 不在 QStringConverter 内置编码里，需要
// Qt6::Core5Compat 提供的 QTextCodec。
QString decodeText(const QByteArray& bytes, QString* error) {
    QStringDecoder utf8Decoder(QStringConverter::Utf8);
    const QString utf8Text = utf8Decoder.decode(bytes);
    if (!utf8Decoder.hasError())
        return utf8Text;

    QTextCodec* gbCodec = QTextCodec::codecForName("GB18030");
    if (!gbCodec) {
        *error = QStringLiteral("文件编码既不是有效 UTF-8，也无法用 GB18030 解码");
        return {};
    }
    QTextCodec::ConverterState state;
    const QString gbText = gbCodec->toUnicode(bytes.constData(), bytes.size(), &state);
    if (state.invalidChars > 0) {
        *error = QStringLiteral("无法识别文件编码，请转存为 UTF-8 后重试");
        return {};
    }
    return gbText;
}

// DOCX 是标准 OOXML ZIP。这里只读取承载正文的 word/document.xml，限制压缩包和
// 单项大小，既避免把 Office 当运行时依赖，也避免畸形文档造成无界内存分配。
QByteArray readDocxDocumentXml(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法打开 DOCX：%1").arg(file.errorString());
        return {};
    }
    constexpr qint64 kMaximumArchiveBytes = 128 * 1024 * 1024;
    if (file.size() <= 0 || file.size() > kMaximumArchiveBytes) {
        *error = QStringLiteral("DOCX 为空或超过 128 MiB 限制");
        return {};
    }
    const QByteArray archiveBytes = file.readAll();
    mz_zip_archive archive{};
    mz_zip_zero_struct(&archive);
    if (!mz_zip_reader_init_mem(&archive, archiveBytes.constData(),
                                static_cast<size_t>(archiveBytes.size()), 0)) {
        *error = QStringLiteral("DOCX ZIP 结构无效");
        return {};
    }
    const int index = mz_zip_reader_locate_file(&archive, "word/document.xml", nullptr, 0);
    if (index < 0) {
        mz_zip_reader_end(&archive);
        *error = QStringLiteral("DOCX 缺少 word/document.xml");
        return {};
    }
    mz_zip_archive_file_stat stat{};
    constexpr mz_uint64 kMaximumXmlBytes = 32 * 1024 * 1024;
    if (!mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(index), &stat) ||
        stat.m_uncomp_size > kMaximumXmlBytes ||
        stat.m_uncomp_size >
            static_cast<mz_uint64>((std::numeric_limits<qsizetype>::max)())) {
        mz_zip_reader_end(&archive);
        *error = QStringLiteral("DOCX 正文 XML 过大或无法读取");
        return {};
    }
    size_t size = 0;
    void* bytes = mz_zip_reader_extract_to_heap(&archive, static_cast<mz_uint>(index), &size, 0);
    // extract_to_heap 在成功时返回非空指针；失败时返回空指针（此时 size 仍可能
    // 非零，是把"需要多大"写回给调用方的信号）。必须把"有 size 无指针"当作
    // 解压失败，而不是构造一个空的 QByteArray 当成功。
    if (!bytes) {
        mz_zip_reader_end(&archive);
        *error = QStringLiteral("DOCX 正文 XML 解压失败");
        return {};
    }
    QByteArray result(static_cast<const char*>(bytes), static_cast<qsizetype>(size));
    mz_free(bytes);
    mz_zip_reader_end(&archive);
    if (result.isEmpty())
        *error = QStringLiteral("DOCX 正文 XML 为空");
    return result;
}

QString docxPlainText(const QByteArray& xmlBytes, QString* error) {
    QXmlStreamReader xml(xmlBytes);
    QString result;
    QString paragraph;
    bool inParagraph = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == u"p") {
                inParagraph = true;
                paragraph.clear();
            } else if (name == u"t" && inParagraph) {
                paragraph += xml.readElementText(QXmlStreamReader::IncludeChildElements);
            } else if ((name == u"tab" || name == u"br") && inParagraph) {
                paragraph += name == u"tab" ? QChar('\t') : QChar('\n');
            }
        } else if (xml.isEndElement() && xml.name() == u"p" && inParagraph) {
            const QString cleaned = paragraph.trimmed();
            if (!cleaned.isEmpty()) {
                if (!result.isEmpty())
                    result += QChar('\n');
                result += cleaned;
            }
            inParagraph = false;
        }
    }
    if (xml.hasError()) {
        *error = QStringLiteral("DOCX 正文 XML 无效：%1").arg(xml.errorString());
        return {};
    }
    return result;
}

bool hasVisibleInk(const QImage& source) {
    if (source.isNull())
        return false;
    const QImage image = source.convertToFormat(QImage::Format_Grayscale8);
    const int stepX = qMax(1, image.width() / 300);
    const int stepY = qMax(1, image.height() / 300);
    qsizetype samples = 0;
    qsizetype dark = 0;
    for (int y = 0; y < image.height(); y += stepY) {
        const uchar* row = image.constScanLine(y);
        for (int x = 0; x < image.width(); x += stepX) {
            ++samples;
            if (row[x] < 235)
                ++dark;
        }
    }
    return samples > 0 && static_cast<double>(dark) / samples > 0.001;
}

QImage renderPdfPage(QPdfDocument* document, int page) {
    // 原来每页按 2x 渲染后再 PNG 压缩，即使这页最终没有任何图表/材料会进入题库。
    // 对屏幕预览、原卷局部裁切和 AI 定位而言 1.5x（约 108 DPI）仍有足够的笔画
    // 细节，却把每页像素量降至原来的 56%，是整理阶段最主要的确定性加速点。
    constexpr qreal kPreviewScale = 1.5;
    const QSizeF points = document->pagePointSize(page);
    const QSize pixels(qBound(1, static_cast<int>(std::ceil(points.width() * kPreviewScale)), 5000),
                       qBound(1, static_cast<int>(std::ceil(points.height() * kPreviewScale)), 5000));
    return document->render(page, pixels);
}

bool writePreviewPng(const QImage& image, QByteArray* destination) {
    if (!destination || image.isNull())
        return false;
    QBuffer buffer(destination);
    if (!buffer.open(QIODevice::WriteOnly))
        return false;
    QImageWriter writer(&buffer, "PNG");
    // 这些 PNG 只是本次生成期间的中间缓存；最终写入题库的是局部裁切后的资源。
    // 使用低压缩级别显著减少 Windows 上每页 PNG 压缩时间，同时不损失任何像素。
    writer.setCompression(1);
    return writer.write(image);
}

QRectF normalizedSelectionBounds(QPdfDocument* document, int page, int start, int length) {
    if (length <= 0)
        return {};
    const QSizeF pageSize = document->pagePointSize(page);
    if (pageSize.width() <= 0.0 || pageSize.height() <= 0.0)
        return {};
    const QRectF bounds = document->getSelectionAtIndex(page, start, length).boundingRectangle();
    if (bounds.isEmpty())
        return {};
    return {bounds.x() / pageSize.width(), bounds.y() / pageSize.height(),
            bounds.width() / pageSize.width(), bounds.height() / pageSize.height()};
}

bool hasRenderedUnderline(const QImage& image, const QRectF& normalizedBounds) {
    if (image.isNull() || normalizedBounds.isEmpty())
        return false;
    const int left = qBound(0, qFloor(normalizedBounds.left() * image.width()), image.width() - 1);
    const int right = qBound(left + 1, qCeil(normalizedBounds.right() * image.width()), image.width());
    const int top = qBound(0, qFloor(normalizedBounds.top() * image.height()), image.height() - 1);
    const int bottom = qBound(top + 1, qCeil(normalizedBounds.bottom() * image.height()), image.height());
    const int width = right - left;
    const int height = bottom - top;
    if (width < 5 || height < 5)
        return false;
    // 下划线位于文字选择框的下 1/4。该位置的像素应在几乎整个字符宽度内连续
    // 为深色，且是很薄的横向笔画；这同时适用于 PDF 中的 vector rectangle 和
    // 栅格化的原始下划线，不依赖 OCR 或题目选项文本。
    const int scanLeft = left + qMax(1, width / 10);
    const int scanRight = right - qMax(1, width / 10);
    const int requiredDark = qMax(2, qCeil((scanRight - scanLeft) * 0.80));
    const int firstRow = top + qFloor(height * 0.72);
    const int lastRow = qMin(image.height() - 1, bottom + qMax(1, height / 12));
    for (int y = firstRow; y <= lastRow; ++y) {
        const uchar* row = image.constScanLine(y);
        int dark = 0;
        for (int x = scanLeft; x < scanRight; ++x)
            if (row[x] < 100) ++dark;
        if (dark >= requiredDark)
            return true;
    }
    return false;
}

void collectPdfTextAnchors(QPdfDocument* document, int page, const QString& text,
                           ExtractedDocument* result) {
    static const QRegularExpression questionMarker(
        QStringLiteral(R"((?:^|\n)\s*(\d{1,4})\s*[、.．])"));
    static const QRegularExpression optionMarker(
        QStringLiteral(R"((?<![A-Za-z0-9])([A-D])\s*[、.．])"));
    auto questions = questionMarker.globalMatch(text);
    while (questions.hasNext()) {
        const auto match = questions.next();
        const QRectF bounds = normalizedSelectionBounds(
            document, page, match.capturedStart(1), match.capturedLength(1));
        if (!bounds.isEmpty())
            result->questionAnchors[page + 1].append({match.captured(1), bounds});
    }
    auto options = optionMarker.globalMatch(text);
    while (options.hasNext()) {
        const auto match = options.next();
        const QRectF bounds = normalizedSelectionBounds(
            document, page, match.capturedStart(1), match.capturedLength(1));
        if (!bounds.isEmpty())
            result->optionLabelAnchors[page + 1].append({match.captured(1).toLower(), bounds});
    }

    // QPdfDocument 的纯文字 API 不会告诉我们字体下划线或独立绘制的填空横线。
    // 提取阶段只保留行锚点；真正的像素检测会等生成器找到“划线/横线/标注/标记”
    // 类小题所关联的共享材料后再按需执行。
    int lineStart = 0;
    while (lineStart < text.size()) {
        const int lineEnd = text.indexOf(u'\n', lineStart);
        const int end = lineEnd < 0 ? text.size() : lineEnd;
        const QString line = text.mid(lineStart, end - lineStart).trimmed();
        if (!line.isEmpty()) {
            const int leading = text.mid(lineStart, end - lineStart).indexOf(line);
            const int textStart = lineStart + qMax(0, leading);
            const QRectF bounds = normalizedSelectionBounds(
                document, page, textStart, line.size());
            if (!bounds.isEmpty()) {
                result->lineAnchors[page + 1].append({line, bounds});
            }
        }
        if (lineEnd < 0)
            break;
        lineStart = lineEnd + 1;
    }
}

#ifdef QUIZPANE_HAS_TESSERACT_OCR
QString bundledTessdataPath() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    QStringList candidates;
    // 开发、测试和自动构建环境通常不会先把语言数据复制到应用目录，允许
    // 调用方显式指定路径；正式发行包则使用后面的应用内相对位置。
    for (const char* variable : {"TESSDATA_DIR", "TESSDATA_PREFIX"}) {
        const QString configured = qEnvironmentVariable(variable).trimmed();
        if (!configured.isEmpty())
            candidates.append(configured);
    }
    candidates.append(appDir.filePath(QStringLiteral("tessdata")));
    candidates.append(appDir.filePath(QStringLiteral("../Resources/tessdata")));
    candidates.append(appDir.filePath(QStringLiteral("../share/quizpane/tessdata")));
    for (const QString& candidate : candidates) {
        const QDir directory(QDir::cleanPath(candidate));
        if (directory.exists(QStringLiteral("eng.traineddata")) &&
            directory.exists(QStringLiteral("chi_sim.traineddata")))
            return directory.absolutePath();
    }
    return {};
}

QString recognizePage(const QImage& source, QString* error) {
    QImage image = source.convertToFormat(QImage::Format_RGB888);
    tesseract::TessBaseAPI api;
    const QByteArray tessdataPath = QFile::encodeName(bundledTessdataPath());
    const char* dataPath = tessdataPath.isEmpty() ? nullptr : tessdataPath.constData();
    if (api.Init(dataPath, "chi_sim+eng") != 0 && api.Init(dataPath, "eng") != 0) {
        *error = QStringLiteral("Tesseract 初始化失败，发行包中的 chi_sim/eng 语言数据不可用");
        return {};
    }
    api.SetImage(image.constBits(), image.width(), image.height(), 3, image.bytesPerLine());
    const auto recognize = [&api](tesseract::PageSegMode mode) {
        api.SetPageSegMode(mode);
        api.Recognize(nullptr);
        char* utf8 = api.GetUTF8Text();
        const QString text = utf8 ? QString::fromUtf8(utf8).trimmed() : QString{};
        delete[] utf8;
        return text;
    };
    QString text = recognize(tesseract::PSM_AUTO);
    // 自动版面分析对只有一两行内容的截图或扫描页偶尔会返回空结果；稀疏
    // 文字模式更适合这类题干图片，作为空结果兜底不会影响普通整页文档。
    if (text.isEmpty())
        text = recognize(tesseract::PSM_SPARSE_TEXT);
    api.End();
    if (text.isEmpty())
        *error = QStringLiteral("本地 OCR 未识别出文字");
    return text;
}
#endif

} // namespace

bool TxtMarkdownExtractor::supports(const QString& path) const {
    return hasSuffix(path, {"txt", "md", "markdown"});
}

ExtractedDocument TxtMarkdownExtractor::extract(const QString& path) const {
    ExtractedDocument result;
    result.sourcePath = path;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("无法打开文件：%1").arg(file.errorString());
        return result;
    }
    const QByteArray bytes = file.readAll();
    QString error;
    const QString text = decodeText(bytes, &error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    if (text.trimmed().isEmpty()) {
        result.error = QStringLiteral("文件内容为空");
        return result;
    }
    result.plainText = text;
    return result;
}

bool DocxExtractor::supports(const QString& path) const {
    return hasSuffix(path, {"docx"});
}

ExtractedDocument DocxExtractor::extract(const QString& path) const {
    ExtractedDocument result;
    result.sourcePath = path;
    QString error;
    const QByteArray xml = readDocxDocumentXml(path, &error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    result.plainText = docxPlainText(xml, &error);
    if (!error.isEmpty())
        result.error = error;
    else if (result.plainText.trimmed().isEmpty())
        result.error = QStringLiteral("DOCX 没有可提取的段落或表格文字");
    return result;
}

bool PdfExtractor::supports(const QString& path) const {
    return hasSuffix(path, {"pdf"});
}

ExtractedDocument PdfExtractor::extract(const QString& path) const {
    ExtractedDocument result;
    result.sourcePath = path;
    QElapsedTimer elapsed;
    elapsed.start();
    QPdfDocument document;
    const QPdfDocument::Error loadError = document.load(path);
    if (loadError != QPdfDocument::Error::None || document.pageCount() <= 0) {
        result.error = QStringLiteral("无法读取 PDF（错误码 %1）").arg(static_cast<int>(loadError));
        return result;
    }
    QStringList pages;
    qint64 previewBytes = 0;
    for (int page = 0; page < document.pageCount(); ++page) {
        QString text = document.getAllText(page).text().trimmed();
        QImage pageImage;
        if (!text.isEmpty()) {
            collectPdfTextAnchors(&document, page, text, &result);
        }
        if (text.isEmpty()) {
            pageImage = renderPdfPage(&document, page);
            if (hasVisibleInk(pageImage)) {
#ifdef QUIZPANE_HAS_TESSERACT_OCR
                QString ocrError;
                text = recognizePage(pageImage, &ocrError);
                if (!ocrError.isEmpty()) {
                    result.warnings.append(
                        QStringLiteral("第 %1 页 OCR 失败：%2").arg(page + 1).arg(ocrError));
                    pages.append(QString{});
                    continue;
                }
                result.usedOcr = true;
#else
                result.warnings.append(QStringLiteral(
                    "第 %1 页是扫描内容，当前构建未启用 OCR，已跳过").arg(page + 1));
                pages.append(QString{});
                continue;
#endif
            }
        }
        // 扫描页的渲染图要保留，供 OCR 来源复核与图片题回退使用；文字型 PDF
        // 则在生成器已确认“这页确实含材料/题图/图片选项”后再懒加载。此前对
        // 每一页都渲染并压缩 PNG，是大题库整理明显变慢的主因。
        if (!pageImage.isNull()) {
            QByteArray png;
            if (writePreviewPng(pageImage, &png)) {
                previewBytes += png.size();
                result.pageImages.insert(page + 1, png);
            }
        }
        pages.append(text);
    }
    result.plainText = pages.join(QChar('\f'));
    result.hasPageBoundaries = true;
    if (result.plainText.trimmed().isEmpty()) {
        result.error = result.warnings.isEmpty()
            ? QStringLiteral("PDF 没有可提取的文字内容")
            : QStringLiteral("PDF 所有页面均无法提取：%1").arg(result.warnings.join(QStringLiteral("；")));
    }
    diagnostic::event(QStringLiteral("extractor"), QStringLiteral("pdf-finished"),
        {{QStringLiteral("file"), QFileInfo(path).fileName()},
         {QStringLiteral("pages"), document.pageCount()},
         {QStringLiteral("ocr"), result.usedOcr},
         {QStringLiteral("previewBytes"), previewBytes},
         {QStringLiteral("elapsedMs"), elapsed.elapsed()}});
    return result;
}

void detectPdfUnderlinesForCandidateLines(
    ExtractedDocument* extracted, const QHash<int, QStringList>& candidateLinesByPage) {
    if (!extracted || candidateLinesByPage.isEmpty() ||
        !hasSuffix(extracted->sourcePath, {"pdf"}))
        return;

    QPdfDocument document;
    if (document.load(extracted->sourcePath) != QPdfDocument::Error::None)
        return;

    for (auto pageIt = candidateLinesByPage.cbegin(); pageIt != candidateLinesByPage.cend(); ++pageIt) {
        const int pageNumber = pageIt.key();
        const int page = pageNumber - 1;
        if (page < 0 || page >= document.pageCount())
            continue;
        QSet<QString> candidates;
        for (const QString& line : pageIt.value()) {
            const QString simplified = line.simplified();
            if (!simplified.isEmpty())
                candidates.insert(simplified);
        }
        if (candidates.isEmpty())
            continue;

        const QString text = document.getAllText(page).text().trimmed();
        QImage pageImage;
        if (extracted->pageImages.contains(pageNumber))
            pageImage.loadFromData(extracted->pageImages.value(pageNumber), "PNG");
        if (pageImage.isNull()) {
            pageImage = renderPdfPage(&document, page);
            QByteArray png;
            if (writePreviewPng(pageImage, &png))
                extracted->pageImages.insert(pageNumber, png);
        }
        const QImage grayPage = pageImage.convertToFormat(QImage::Format_Grayscale8);
        if (grayPage.isNull())
            continue;

        int lineStart = 0;
        while (lineStart < text.size()) {
            const int lineEnd = text.indexOf(u'\n', lineStart);
            const int end = lineEnd < 0 ? text.size() : lineEnd;
            const QString rawLine = text.mid(lineStart, end - lineStart);
            const QString line = rawLine.trimmed();
            if (!line.isEmpty() && candidates.contains(line.simplified())) {
                const int leading = rawLine.indexOf(line);
                const int textStart = lineStart + qMax(0, leading);
                const QRectF bounds = normalizedSelectionBounds(
                    &document, page, textStart, line.size());
                if (!bounds.isEmpty()) {
                    QList<QPair<int, int>> ranges;
                    int rangeStart = -1;
                    for (int offset = 0; offset < line.size(); ++offset) {
                        const QRectF characterBounds = normalizedSelectionBounds(
                            &document, page, textStart + offset, 1);
                        const bool underlined = !line.at(offset).isSpace() &&
                            hasRenderedUnderline(grayPage, characterBounds);
                        if (underlined && rangeStart < 0)
                            rangeStart = offset;
                        if ((!underlined || offset + 1 == line.size()) && rangeStart >= 0) {
                            const int rangeEnd = underlined && offset + 1 == line.size()
                                ? offset + 1 : offset;
                            ranges.append({rangeStart, rangeEnd - rangeStart});
                            rangeStart = -1;
                        }
                    }
                    if (!ranges.isEmpty())
                        extracted->underlineDecorations[pageNumber].append({line, ranges, bounds});
                }
            }
            if (lineEnd < 0)
                break;
            lineStart = lineEnd + 1;
        }
    }
}

void ensurePdfPageImages(ExtractedDocument* extracted, const QList<int>& pageNumbers) {
    if (!extracted || pageNumbers.isEmpty() || !hasSuffix(extracted->sourcePath, {"pdf"}))
        return;
    // 大量图题可能共享同一页。先在内存缓存中去重并短路，避免每道题都重新打开
    // 一次 PDF（Windows 上打开复杂 PDF 本身就会产生可感知的延迟）。
    QSet<int> requested;
    for (const int pageNumber : pageNumbers)
        if (pageNumber > 0 && !extracted->pageImages.contains(pageNumber))
            requested.insert(pageNumber);
    if (requested.isEmpty())
        return;
    QPdfDocument document;
    if (document.load(extracted->sourcePath) != QPdfDocument::Error::None)
        return;
    for (const int pageNumber : requested) {
        if (pageNumber > document.pageCount())
            continue;
        const QImage image = renderPdfPage(&document, pageNumber - 1);
        QByteArray png;
        if (writePreviewPng(image, &png))
            extracted->pageImages.insert(pageNumber, png);
    }
}

ExtractorRegistry::ExtractorRegistry() = default;

ExtractedDocument ExtractorRegistry::extract(const QString& path) const {
    if (txtMarkdown_.supports(path))
        return txtMarkdown_.extract(path);
    if (docx_.supports(path))
        return docx_.extract(path);
    if (pdf_.supports(path))
        return pdf_.extract(path);
    ExtractedDocument result;
    result.sourcePath = path;
    result.error = QStringLiteral("不支持的文件格式：%1").arg(QFileInfo(path).suffix());
    return result;
}

} // namespace quizpane::studio
