#include "quizpane/studio/document_extractor.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPdfDocument>
#include <QPdfSelection>
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
        stat.m_uncomp_size > static_cast<mz_uint64>(std::numeric_limits<qsizetype>::max())) {
        mz_zip_reader_end(&archive);
        *error = QStringLiteral("DOCX 正文 XML 过大或无法读取");
        return {};
    }
    size_t size = 0;
    void* bytes = mz_zip_reader_extract_to_heap(&archive, static_cast<mz_uint>(index), &size, 0);
    QByteArray result;
    if (bytes || size == 0)
        result = QByteArray(static_cast<const char*>(bytes), static_cast<qsizetype>(size));
    mz_free(bytes);
    mz_zip_reader_end(&archive);
    if (result.isEmpty())
        *error = QStringLiteral("DOCX 正文 XML 为空或解压失败");
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
    const QSizeF points = document->pagePointSize(page);
    const QSize pixels(qBound(1, static_cast<int>(std::ceil(points.width() * 2.0)), 5000),
                       qBound(1, static_cast<int>(std::ceil(points.height() * 2.0)), 5000));
    return document->render(page, pixels);
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
    QPdfDocument document;
    const QPdfDocument::Error loadError = document.load(path);
    if (loadError != QPdfDocument::Error::None || document.pageCount() <= 0) {
        result.error = QStringLiteral("无法读取 PDF（错误码 %1）").arg(static_cast<int>(loadError));
        return result;
    }
    QStringList pages;
    for (int page = 0; page < document.pageCount(); ++page) {
        QString text = document.getAllText(page).text().trimmed();
        if (text.isEmpty()) {
            const QImage image = renderPdfPage(&document, page);
            if (hasVisibleInk(image)) {
#ifdef QUIZPANE_HAS_TESSERACT_OCR
                QString ocrError;
                text = recognizePage(image, &ocrError);
                if (!ocrError.isEmpty()) {
                    result.error = QStringLiteral("PDF 第 %1 页：%2").arg(page + 1).arg(ocrError);
                    return result;
                }
                result.usedOcr = true;
#else
                result.error =
                    QStringLiteral("PDF 第 %1 页是扫描内容；当前构建未启用 Tesseract C++ OCR 后端")
                        .arg(page + 1);
                return result;
#endif
            }
        }
        pages.append(text);
    }
    result.plainText = pages.join(QChar('\f'));
    result.hasPageBoundaries = true;
    if (result.plainText.trimmed().isEmpty())
        result.error = QStringLiteral("PDF 没有可提取的文字内容");
    return result;
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
