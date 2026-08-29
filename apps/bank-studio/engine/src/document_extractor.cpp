#include "quizpane/studio/document_extractor.hpp"

#include "quizpane/diagnostic_logger.hpp"
#ifdef QUIZPANE_HAS_QT_PDF
#include "quizpane/studio/qt_pdf_compat.hpp"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#ifdef QUIZPANE_HAS_QT_PDF
#include <QPdfDocument>
#include <QPdfSelection>
#endif
#include <QSet>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringConverter>
#include <QStringDecoder>
#endif
#include <QTextCodec>
#include <QtMath>
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
#include <algorithm>

namespace quizpane::studio {
namespace {

QImage whiteBackground(const QImage& source) {
    if (source.isNull()) return {};
    QImage flat(source.size(), QImage::Format_RGB32);
    flat.fill(Qt::white);
    QPainter painter(&flat);
    painter.drawImage(0, 0, source);
    painter.end();
    return flat;
}

bool hasSuffix(const QString& path, const QStringList& suffixes) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffixes.contains(suffix);
}

// UTF-8 严格解码失败（出现非法字节序列）时回退到 GB18030，覆盖国内用户
// 常见的 Windows 记事本"ANSI"编码保存的 TXT 文件。Qt 6 用
// QStringDecoder，Qt 5 用 QTextCodec；两条路径都严格拒绝非法 UTF-8，再回退
// GB18030。Qt 6 的 QTextCodec 来自 Core5Compat，Qt 5 则由 Core 直接提供。
QString decodeText(const QByteArray& bytes, QString* error) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QStringDecoder utf8Decoder(QStringConverter::Utf8);
    const QString utf8Text = utf8Decoder.decode(bytes);
    if (!utf8Decoder.hasError())
        return utf8Text;
#else
    // 默认构造的 ConverterState 不含 ConvertInvalidBytes/ConvertInvalidChars
    // 等标志，QTextCodec 会对 UTF-8 自动识别并跳过开头的 BOM；与 Qt 6
    // QStringDecoder 的行为一致。此处显式加守卫，避免部署环境拿不到 UTF-8
    // 编解码器时空指针解引用。
    QTextCodec* utf8Codec = QTextCodec::codecForName("UTF-8");
    if (!utf8Codec) {
        *error = QStringLiteral("系统缺少 UTF-8 编解码器，无法读取文本");
        return {};
    }
    QTextCodec::ConverterState utf8State;
    const QString utf8Text = utf8Codec->toUnicode(
        bytes.constData(), bytes.size(), &utf8State);
    if (utf8State.invalidChars == 0)
        return utf8Text;
#endif

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
            const auto name = xml.name();
            if (name == QLatin1String("p")) {
                inParagraph = true;
                paragraph.clear();
            } else if (name == QLatin1String("t") && inParagraph) {
                paragraph += xml.readElementText(QXmlStreamReader::IncludeChildElements);
            } else if ((name == QLatin1String("tab") || name == QLatin1String("br")) &&
                       inParagraph) {
                paragraph += name == QLatin1String("tab") ? QChar('\t') : QChar('\n');
            }
        } else if (xml.isEndElement() && xml.name() == QLatin1String("p") && inParagraph) {
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

#ifdef QUIZPANE_HAS_QT_PDF
bool hasVisibleInk(const QImage& source) {
    if (source.isNull())
        return false;
    const QImage image = whiteBackground(source).convertToFormat(QImage::Format_Grayscale8);
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
    const QSizeF points = pdfPagePointSize(document, page);
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
    const QSizeF pageSize = pdfPagePointSize(document, page);
    if (pageSize.width() <= 0.0 || pageSize.height() <= 0.0)
        return {};
    const QRectF bounds = document->getSelectionAtIndex(page, start, length).boundingRectangle();
    if (bounds.isEmpty())
        return {};
    return {bounds.x() / pageSize.width(), bounds.y() / pageSize.height(),
            bounds.width() / pageSize.width(), bounds.height() / pageSize.height()};
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
#endif

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
    QImage image = whiteBackground(source).convertToFormat(QImage::Format_RGB888);
    tesseract::TessBaseAPI api;
    const QByteArray tessdataPath = bundledTessdataPath().toUtf8();
    const char* dataPath = tessdataPath.isEmpty() ? nullptr : tessdataPath.constData();
#if defined(TESSERACT_VERSION) && TESSERACT_VERSION >= 0x050000
    // Tesseract's narrow file APIs cannot reliably open Chinese Windows paths.
    // Tesseract 5 exposes std::vector-based callbacks, so load model bytes through
    // Qt; no short-path names or system codepage is needed by the Win7 package.
    const auto reader = [](const char* path, std::vector<char>* bytes) {
        QFile file(QString::fromUtf8(path));
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
            file.size() > 128 * 1024 * 1024) return false;
        const QByteArray data = file.readAll();
        if (file.error() != QFileDevice::NoError || data.size() != file.size()) return false;
        bytes->assign(data.constData(), data.constData() + data.size());
        return true;
    };
    const int initialized = api.Init(dataPath, 0, "chi_sim+eng", tesseract::OEM_DEFAULT,
                                    nullptr, 0, nullptr, nullptr, false, reader);
    std::vector<std::string> languages;
    if (initialized == 0) api.GetLoadedLanguagesAsVector(&languages);
    const bool hasChinese =
        std::find(languages.begin(), languages.end(), "chi_sim") != languages.end();
    const bool hasEnglish =
        std::find(languages.begin(), languages.end(), "eng") != languages.end();
#else
    // Ubuntu 22.04 ships Tesseract 4.1, whose public API still uses the legacy
    // GenericVector types. The simple Init overload is common to 3.x/4.x/5.x and
    // avoids leaking those private container types into this translation unit.
    const int initialized = api.Init(dataPath, "chi_sim+eng", tesseract::OEM_DEFAULT);
    const char* initializedLanguages =
        initialized == 0 ? api.GetInitLanguagesAsString() : nullptr;
    const QList<QByteArray> languages =
        initializedLanguages ? QByteArray(initializedLanguages).split('+') : QList<QByteArray>{};
    const bool hasChinese = languages.contains(QByteArrayLiteral("chi_sim"));
    const bool hasEnglish = languages.contains(QByteArrayLiteral("eng"));
#endif
    if (initialized != 0 || !hasChinese || !hasEnglish) {
        *error = QStringLiteral("文字识别无法启动：中英文识别模型缺失或损坏。请重新完整解压安装包，保留 tessdata 文件夹");
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

PdfUnderlineDecoration detectRenderedLineDecorations(
    const QImage& source, const QString& text, const QList<QRectF>& characterBounds) {
    PdfUnderlineDecoration result;
    result.text = text;
    if (source.isNull() || characterBounds.size() != text.size()) return result;
    const QImage gray = source.format() == QImage::Format_Grayscale8 ? source
        : whiteBackground(source).convertToFormat(QImage::Format_Grayscale8);
    QList<QRectF> boxes;
    QList<qreal> heights, bottoms;
    qreal maxHeight = 0;
    for (int i = 0; i < text.size(); ++i) {
        const auto b = characterBounds.at(i);
        boxes.append(QRectF(b.x() * gray.width(), b.y() * gray.height(),
                           b.width() * gray.width(), b.height() * gray.height()));
        if (!b.isEmpty()) result.bounds = result.bounds.united(b);
        if (!text.at(i).isSpace()) maxHeight = qMax(maxHeight, boxes.last().height());
    }
    if (maxHeight < 5) return result;
    for (int i = 0; i < text.size(); ++i)
        if (!text.at(i).isSpace() && boxes.at(i).height() >= maxHeight * 0.6) {
            heights.append(boxes.at(i).height());
            bottoms.append(boxes.at(i).bottom());
        }
    if (heights.isEmpty()) return result;
    std::sort(heights.begin(), heights.end());
    std::sort(bottoms.begin(), bottoms.end());
    const qreal height = heights.at(heights.size() / 2);
    const qreal baseline = bottoms.at(bottoms.size() / 2);
    const int firstRow = qMax(0, qCeil(baseline - height * 0.07));
    const int lastRow = qMin(gray.height() - 1, qCeil(baseline + height * 0.3));
    const int left = qMax(0, qFloor(result.bounds.left() * gray.width() - height * 8));
    const int right = qMin(gray.width(), qCeil(result.bounds.right() * gray.width() + height * 8));
    const int minimumRun = qMax(6, qCeil(height * 1.03));
    QList<QRectF> segments;
    for (int y = firstRow; y <= lastRow; ++y) {
        const uchar* row = gray.constScanLine(y);
        int start = -1;
        for (int x = left; x <= right; ++x) {
            const bool dark = x < right && row[x] < 180;
            if (dark && start < 0) start = x;
            if ((dark && x + 1 < right) || start < 0) continue;
            const int end = dark ? x + 1 : x;
            if (end - start >= minimumRun && end - start < gray.width() * 0.85) {
                QRectF run(start, y, end - start, 1);
                bool merged = false;
                for (auto& segment : segments) {
                    const qreal overlap = qMin(segment.right(), run.right()) - qMax(segment.left(), run.left());
                    if (run.top() <= segment.bottom() + 1 &&
                        overlap >= qMin(segment.width(), run.width()) * 0.85) {
                        segment = segment.united(run);
                        merged = true;
                        break;
                    }
                }
                if (!merged) segments.append(run);
            }
            start = -1;
        }
    }
    QList<bool> marked;
    for (int i = 0; i < text.size(); ++i) marked.append(false);
    for (const auto& segment : segments) {
        if (segment.height() > qMax<qreal>(2, height * 0.22)) continue;
        QList<int> covered;
        for (int i = 0; i < text.size(); ++i) {
            const auto& box = boxes.at(i);
            if (box.isEmpty() || text.at(i).isSpace()) continue;
            const qreal overlap = qMin(box.right(), segment.right()) - qMax(box.left(), segment.left());
            if (box.center().x() >= segment.left() && box.center().x() <= segment.right() &&
                overlap >= box.width() * 0.65) covered.append(i);
        }
        bool hasText = false;
        for (int i : covered)
            if (text.at(i) != u'_' && text.at(i) != QChar(0xff3f)) hasText = true;
        if (hasText) {
            for (int i : covered) marked[i] = true;
        } else {
            int previous = -1, next = text.size();
            for (int i = 0; i < text.size(); ++i) {
                if (boxes.at(i).isEmpty() || text.at(i).isSpace() ||
                    text.at(i) == u'_' || text.at(i) == QChar(0xff3f)) continue;
                if (boxes.at(i).center().x() < segment.left()) previous = i;
                if (boxes.at(i).center().x() > segment.right()) { next = i; break; }
            }
            const int start = previous + 1;
            const QString gap = text.mid(start, next - start);
            if (next >= start && gap.trimmed().remove(u'_').remove(QChar(0xff3f)).isEmpty()) {
                const QPair<int, int> blank(start, next - start);
                if (!result.blanks.contains(blank)) result.blanks.append(blank);
            }
        }
    }
    int start = -1;
    for (int i = 0; i <= marked.size(); ++i) {
        if (i < marked.size() && marked.at(i)) { if (start < 0) start = i; }
        else if (start >= 0) { result.ranges.append({start, i - start}); start = -1; }
    }
    std::sort(result.blanks.begin(), result.blanks.end());
    return result;
}

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
#ifdef QUIZPANE_HAS_QT_PDF
    return hasSuffix(path, {"pdf"});
#else
    Q_UNUSED(path)
    return false;
#endif
}

ExtractedDocument PdfExtractor::extract(const QString& path) const {
    ExtractedDocument result;
    result.sourcePath = path;
#ifndef QUIZPANE_HAS_QT_PDF
    result.error = QStringLiteral("当前兼容构建未包含 PDF 导入，请改用 TXT、Markdown 或 DOCX");
    return result;
#else
    QElapsedTimer elapsed;
    elapsed.start();
    QPdfDocument document;
    const int loadError = loadPdfDocument(&document, path);
    if (!pdfLoadSucceeded(loadError) || document.pageCount() <= 0) {
        result.error = QStringLiteral("无法读取 PDF（错误码 %1）").arg(loadError);
        return result;
    }
    QStringList pages;
    qint64 previewBytes = 0;
    for (int page = 0; page < document.pageCount(); ++page) {
        const QString rawText = document.getAllText(page).text();
        QString text = rawText.trimmed();
        QImage pageImage;
        if (!text.isEmpty()) {
            collectPdfTextAnchors(&document, page, rawText, &result);
        }
        if (text.isEmpty()) {
            pageImage = renderPdfPage(&document, page);
            if (hasVisibleInk(pageImage)) {
#ifdef QUIZPANE_HAS_TESSERACT_OCR
                QString ocrError;
                text = recognizePage(pageImage, &ocrError);
                if (!ocrError.isEmpty()) {
                    ++result.ocrFailedPages;
                    result.warnings.append(
                        QStringLiteral("第 %1 页 OCR 失败：%2").arg(page + 1).arg(ocrError));
                    pages.append(QString{});
                    continue;
                }
                result.usedOcr = true;
#else
                ++result.ocrSkippedPages;
                result.warnings.append(QStringLiteral(
                    "第 %1 页没有可提取的文字，需要文字识别（OCR）；当前版本未启用 OCR，已跳过。请使用带 OCR 的版本或先将文件转换为带文字层的 PDF").arg(page + 1));
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
        // Avoid repeating the same model/feature error hundreds of times for
        // a long scanned book. Keep detailed page warnings separately.
        if (result.ocrSkippedPages > 0)
            result.error = QStringLiteral("此 PDF 有 %1 页需要文字识别（OCR），当前版本未启用，无法提取题目。请使用带 OCR 的版本或先转换为带文字层的 PDF")
                .arg(result.ocrSkippedPages);
        else if (result.ocrFailedPages > 0)
            result.error = QStringLiteral("此 PDF 有 %1 页文字识别（OCR）失败：%2")
                .arg(result.ocrFailedPages).arg(result.warnings.value(0));
        else
            result.error = QStringLiteral("PDF 没有可提取的文字内容");
    }
    diagnostic::event(QStringLiteral("extractor"), QStringLiteral("pdf-finished"),
        {{QStringLiteral("file"), QFileInfo(path).fileName()},
         {QStringLiteral("pages"), document.pageCount()},
         {QStringLiteral("ocr"), result.usedOcr},
         {QStringLiteral("previewBytes"), previewBytes},
         {QStringLiteral("elapsedMs"), elapsed.elapsed()}});
    return result;
#endif
}

void detectPdfUnderlinesForCandidateLines(
    ExtractedDocument* extracted, const QHash<int, QStringList>& candidateLinesByPage) {
#ifndef QUIZPANE_HAS_QT_PDF
    Q_UNUSED(extracted)
    Q_UNUSED(candidateLinesByPage)
    return;
#else
    if (!extracted || candidateLinesByPage.isEmpty() ||
        !hasSuffix(extracted->sourcePath, {"pdf"}))
        return;

    QPdfDocument document;
    if (!pdfLoadSucceeded(loadPdfDocument(&document, extracted->sourcePath)))
        return;

    for (auto pageIt = candidateLinesByPage.cbegin(); pageIt != candidateLinesByPage.cend(); ++pageIt) {
        const int pageNumber = pageIt.key();
        const int page = pageNumber - 1;
        if (page < 0 || page >= document.pageCount())
            continue;
        // MinerU 会规范化空白，例如把 PDF 文字层中的四个空格压成一个，或删掉
        // 数字前后的空格。若继续要求整行 simplified() 完全一致，恰好承载填空
        // 横线的行反而最容易匹配失败。匹配时忽略空白，检测完成后再把坐标映射
        // 回 MinerU 的原始行，保证后续生成器使用同一套 UTF-16 偏移。
        const auto matchKey = [](const QString& value) {
            QString key;
            key.reserve(value.size());
            for (const QChar character : value)
                if (!character.isSpace()) key.append(character);
            return key;
        };
        QHash<QString, QStringList> candidates;
        for (const QString& line : pageIt.value()) {
            const QString candidate = line.trimmed();
            const QString key = matchKey(candidate);
            if (!key.isEmpty() && !candidates[key].contains(candidate))
                candidates[key].append(candidate);
        }
        if (candidates.isEmpty())
            continue;

        const auto remapDecoration = [&matchKey](const PdfUnderlineDecoration& source,
                                                  const QString& target) {
            PdfUnderlineDecoration mapped;
            mapped.text = target;
            mapped.bounds = source.bounds;
            const QString sourceKey = matchKey(source.text);
            const QString targetKey = matchKey(target);
            QList<int> sourceCharacters;
            QList<int> targetCharacters;
            for (int i = 0; i < source.text.size(); ++i)
                if (!source.text.at(i).isSpace()) sourceCharacters.append(i);
            for (int i = 0; i < target.size(); ++i)
                if (!target.at(i).isSpace()) targetCharacters.append(i);

            // 带下划线文字必须保持逐字完全一致才转交范围。仅凭“去空白后相等”
            // 映射文字范围会在页眉/页脚附近产生错误偏移；空白横线没有文字，
            // 才需要下面按相邻字符序号映射。
            for (const auto& range : source.text == target
                    ? source.ranges : QList<QPair<int, int>>{}) {
                int first = -1;
                int last = -1;
                const int end = range.first + range.second;
                for (int ordinal = 0; ordinal < sourceCharacters.size(); ++ordinal) {
                    const int position = sourceCharacters.at(ordinal);
                    if (position >= range.first && position < end) {
                        if (first < 0) first = ordinal;
                        last = ordinal;
                    }
                }
                if (first >= 0 && last >= first) {
                    const int start = targetCharacters.at(first);
                    mapped.ranges.append({start, targetCharacters.at(last) + 1 - start});
                }
            }
            for (const auto& blank : source.blanks) {
                const int end = blank.first + blank.second;
                if (sourceKey == targetKey &&
                    sourceCharacters.size() == targetCharacters.size()) {
                    int previous = -1;
                    int next = sourceCharacters.size();
                    for (int ordinal = 0; ordinal < sourceCharacters.size(); ++ordinal) {
                        const int position = sourceCharacters.at(ordinal);
                        if (position < blank.first) previous = ordinal;
                        if (position >= end) { next = ordinal; break; }
                    }
                    const int start = previous >= 0 ? targetCharacters.at(previous) + 1 : 0;
                    const int targetEnd = next < targetCharacters.size()
                        ? targetCharacters.at(next) : target.size();
                    if (targetEnd >= start)
                        mapped.blanks.append({start, targetEnd - start});
                    continue;
                }

                // MinerU 偶尔会在横线上幻读出单个 C/0/逗号，或把下一行“填入
                // 横线……”粘到题干末尾。用横线前后最多 10 个真实字符作上下文，
                // 只在前文唯一命中且后文命中（或横线本来就在行末）时替换该段。
                int beforeOrdinal = 0;
                while (beforeOrdinal < sourceCharacters.size() &&
                       sourceCharacters.at(beforeOrdinal) < blank.first)
                    ++beforeOrdinal;
                int afterOrdinal = beforeOrdinal;
                while (afterOrdinal < sourceCharacters.size() &&
                       sourceCharacters.at(afterOrdinal) < end)
                    ++afterOrdinal;
                const int contextStart = qMax(0, beforeOrdinal - 10);
                const QString before = sourceKey.mid(contextStart, beforeOrdinal - contextStart);
                const QString after = sourceKey.mid(afterOrdinal, 10);
                if (before.size() < 4)
                    continue;
                const int beforeAt = targetKey.lastIndexOf(before);
                if (beforeAt < 0 || targetKey.indexOf(before) != beforeAt)
                    continue;
                const int startOrdinal = beforeAt + before.size();
                int endOrdinal = -1;
                if (!after.isEmpty())
                    endOrdinal = targetKey.indexOf(after, startOrdinal);
                const bool sourceBlankAtEnd = afterOrdinal >= sourceKey.size() - 1;
                if (endOrdinal < 0 && !sourceBlankAtEnd)
                    continue;
                const int start = startOrdinal > 0 && startOrdinal <= targetCharacters.size()
                    ? targetCharacters.at(startOrdinal - 1) + 1 : 0;
                int targetEnd = endOrdinal >= 0 && endOrdinal < targetCharacters.size()
                    ? targetCharacters.at(endOrdinal) : target.size();
                static const QRegularExpression instruction(
                    QStringLiteral(R"((?:依次)?填入[^\n]{0,16}(?:横线|划横线|画横线))"));
                const auto instructionMatch = instruction.match(target, start);
                if (instructionMatch.hasMatch())
                    targetEnd = qMin(targetEnd, instructionMatch.capturedStart());
                if (targetEnd >= start)
                    mapped.blanks.append({start, targetEnd - start});
            }
            return mapped;
        };

        const auto matchingCandidates = [&candidates](const QString& key) {
            if (candidates.contains(key))
                return candidates.value(key);
            QString bestKey;
            int bestRun = 0;
            for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
                int run = 0;
                int compared = 0;
                for (int start = 0; start + 8 <= key.size(); ++start) {
                    const QString seed = key.mid(start, 8);
                    const int targetStart = it.key().indexOf(seed);
                    if (targetStart < 0 || it.key().indexOf(seed, targetStart + 1) >= 0)
                        continue;
                    const int limit = qMin(key.size() - start, it.key().size() - targetStart);
                    int current = 0;
                    while (current < limit &&
                           key.at(start + current) == it.key().at(targetStart + current))
                        ++current;
                    if (current > run) {
                        run = current;
                        compared = limit;
                    }
                }
                // 8 个连续字符足以排除同页不同题，60% 覆盖率允许分行位置略有
                // 差异、末尾一个幻读字符，或下一行提示语被粘到题干后面。
                if (run >= 8 && run * 10 >= compared * 6 && run > bestRun) {
                    bestRun = run;
                    bestKey = it.key();
                }
            }
            return bestKey.isEmpty() ? QStringList{} : candidates.value(bestKey);
        };

        const QString text = document.getAllText(page).text();
        QImage pageImage;
        if (extracted->pageImages.contains(pageNumber))
            pageImage.loadFromData(extracted->pageImages.value(pageNumber), "PNG");
        if (pageImage.isNull()) {
            pageImage = renderPdfPage(&document, page);
            QByteArray png;
            if (writePreviewPng(pageImage, &png))
                extracted->pageImages.insert(pageNumber, png);
        }
        const QImage grayPage = whiteBackground(pageImage).convertToFormat(QImage::Format_Grayscale8);
        if (grayPage.isNull())
            continue;

        QSet<QString> decoratedCandidates;
        int lineStart = 0;
        while (lineStart < text.size()) {
            const int lineEnd = text.indexOf(u'\n', lineStart);
            const int end = lineEnd < 0 ? text.size() : lineEnd;
            const QString rawLine = text.mid(lineStart, end - lineStart);
            const QString line = rawLine.trimmed();
            const QString key = matchKey(line);
            QStringList targets = matchingCandidates(key);
            if (!line.isEmpty()) {
                const int leading = rawLine.indexOf(line);
                const int textStart = lineStart + qMax(0, leading);
                const QRectF bounds = normalizedSelectionBounds(
                    &document, page, textStart, line.size());
                if (!bounds.isEmpty()) {
                    // 文本已经被 MinerU 幻读或粘行时，文字相似度可能不足；两条
                    // 链路的页面坐标仍指向同一视觉行。只在纵向中心几乎重合且该
                    // MinerU 行确实属于候选题干时采用，避免跨题误配。
                    if (targets.isEmpty()) {
                        qreal bestDistance = std::numeric_limits<qreal>::max();
                        QString bestKey;
                        for (const PdfTextAnchor& anchor :
                             extracted->lineAnchors.value(pageNumber)) {
                            const QString anchorKey = matchKey(anchor.text);
                            if (!candidates.contains(anchorKey) || anchor.bounds.isEmpty())
                                continue;
                            const qreal distance = qAbs(anchor.bounds.center().y() -
                                                        bounds.center().y());
                            const qreal tolerance = qMax(anchor.bounds.height(), bounds.height()) * 0.8;
                            if (distance <= tolerance && distance < bestDistance) {
                                bestDistance = distance;
                                bestKey = anchorKey;
                            }
                        }
                        if (!bestKey.isEmpty())
                            targets = candidates.value(bestKey);
                    }
                    if (targets.isEmpty()) {
                        if (lineEnd < 0) break;
                        lineStart = lineEnd + 1;
                        continue;
                    }
                    QList<QRectF> characters;
                    for (int offset = 0; offset < line.size(); ++offset) {
                        characters.append(normalizedSelectionBounds(
                            &document, page, textStart + offset, 1));
                    }
                    const auto decoration = detectRenderedLineDecorations(grayPage, line, characters);
                    if (!decoration.ranges.isEmpty() || !decoration.blanks.isEmpty()) {
                        for (const QString& candidate : targets) {
                            const auto mapped = remapDecoration(decoration, candidate);
                            if (!mapped.ranges.isEmpty() || !mapped.blanks.isEmpty()) {
                                extracted->underlineDecorations[pageNumber].append(mapped);
                                decoratedCandidates.insert(candidate);
                            }
                        }
                    }
                }
            }
            if (lineEnd < 0)
                break;
            lineStart = lineEnd + 1;
        }

        // 云端分行和 QPdfDocument 的本地分行并不总是一致，尤其是“因此，____”
        // 这种句尾横线：MinerU 行锚点只到逗号，本地文字层则可能把前后两行合并。
        // 对仍未获得装饰的候选行，直接使用 MinerU 已验证的行 bbox，在原 PDF
        // 渲染图上补做几何检测。字符宽度以真实像素行高估算；MinerU 压缩掉的
        // 空白宽度会分配回空格字符，因此不会再把长横线误贴到后面的文字上。
        const auto estimatedCharacterBounds = [&grayPage](const QRectF& normalized,
                                                           const QString& value) {
            QList<QRectF> result;
            if (normalized.isEmpty() || value.isEmpty()) return result;
            const QRectF pixels(normalized.x() * grayPage.width(),
                                normalized.y() * grayPage.height(),
                                normalized.width() * grayPage.width(),
                                normalized.height() * grayPage.height());
            QList<qreal> widths;
            widths.reserve(value.size());
            int spaces = 0;
            qreal fixedWidth = 0.0;
            for (const QChar character : value) {
                if (character.isSpace()) {
                    widths.append(0.0);
                    ++spaces;
                    continue;
                }
                const ushort unicode = character.unicode();
                const bool ascii = unicode < 128;
                const bool punctuation = character.category() == QChar::Punctuation_Connector ||
                    character.category() == QChar::Punctuation_Dash ||
                    character.category() == QChar::Punctuation_Open ||
                    character.category() == QChar::Punctuation_Close ||
                    character.category() == QChar::Punctuation_InitialQuote ||
                    character.category() == QChar::Punctuation_FinalQuote ||
                    character.category() == QChar::Punctuation_Other;
                const qreal width = pixels.height() * (ascii ? 0.52 : punctuation ? 0.58 : 0.88);
                widths.append(width);
                fixedWidth += width;
            }
            qreal scale = 1.0;
            qreal spaceWidth = 0.0;
            if (spaces > 0 && pixels.width() > fixedWidth)
                spaceWidth = (pixels.width() - fixedWidth) / spaces;
            else if (fixedWidth > 0.0)
                scale = pixels.width() / fixedWidth;
            qreal x = pixels.left();
            for (int i = 0; i < value.size(); ++i) {
                const qreal width = value.at(i).isSpace() ? spaceWidth : widths.at(i) * scale;
                result.append(QRectF(x / grayPage.width(), pixels.top() / grayPage.height(),
                                     width / grayPage.width(), pixels.height() / grayPage.height()));
                x += width;
            }
            return result;
        };
        for (auto candidateIt = candidates.cbegin(); candidateIt != candidates.cend(); ++candidateIt) {
            for (const QString& candidate : candidateIt.value()) {
                if (decoratedCandidates.contains(candidate)) continue;
                QRectF bounds;
                for (const PdfTextAnchor& anchor : extracted->lineAnchors.value(pageNumber))
                    if (matchKey(anchor.text) == candidateIt.key()) {
                        bounds = anchor.bounds;
                        break;
                    }
                if (bounds.isEmpty()) continue;
                const auto characters = estimatedCharacterBounds(bounds, candidate);
                if (characters.size() != candidate.size()) continue;
                const auto decoration =
                    detectRenderedLineDecorations(grayPage, candidate, characters);
                // 估算字符宽度足以定位“文字之间/句尾”的空白横线，但不足以把
                // 一条细线精确归属到某几个字。文字下划线只采用上面的 QPdf
                // 逐字符选择框结果，避免将字形横画或页脚边框误报为下划线。
                QRegularExpression instructionAtGap(
                    QStringLiteral(R"((?:依次)?填入[^\n]{0,16}(?:横线|划横线|画横线))"));
                QList<QPair<int, int>> reliableBlanks;
                for (const auto& blank : decoration.blanks) {
                    const QString gap = candidate.mid(blank.first, blank.second);
                    const auto cue = instructionAtGap.match(candidate, blank.first);
                    const bool whitespaceGap = blank.second > 0 && gap.trimmed().isEmpty();
                    const bool atLineEnd = blank.first >= candidate.size();
                    const bool beforeInstruction = cue.hasMatch() &&
                        cue.capturedStart() <= blank.first + blank.second + 2;
                    if (whitespaceGap || atLineEnd || beforeInstruction)
                        reliableBlanks.append(blank);
                }
                if (!reliableBlanks.isEmpty()) {
                    PdfUnderlineDecoration blanksOnly = decoration;
                    blanksOnly.ranges.clear();
                    blanksOnly.blanks = reliableBlanks;
                    extracted->underlineDecorations[pageNumber].append(blanksOnly);
                }
            }
        }
    }
#endif
}

void ensurePdfPageImages(ExtractedDocument* extracted, const QList<int>& pageNumbers) {
#ifndef QUIZPANE_HAS_QT_PDF
    Q_UNUSED(extracted)
    Q_UNUSED(pageNumbers)
    return;
#else
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
    if (!pdfLoadSucceeded(loadPdfDocument(&document, extracted->sourcePath)))
        return;
    for (const int pageNumber : requested) {
        if (pageNumber > document.pageCount())
            continue;
        const QImage image = renderPdfPage(&document, pageNumber - 1);
        QByteArray png;
        if (writePreviewPng(image, &png))
            extracted->pageImages.insert(pageNumber, png);
    }
#endif
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
