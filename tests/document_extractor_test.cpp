#include "quizpane/studio/document_extractor.hpp"
#include "quizpane/zip_archive.hpp"

#include <QFile>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

namespace {
bool setTessdataDirectory(const QString& path) {
#ifdef Q_OS_WIN
    // qputenv uses the narrow MSVC environment API and corrupts paths that
    // cannot be represented by the active code page. qEnvironmentVariable,
    // used by the extractor, reads the wide environment on Windows.
    const wchar_t* value = path.isNull()
        ? L""
        : reinterpret_cast<const wchar_t*>(path.utf16());
    return _wputenv_s(L"TESSDATA_DIR", value) == 0;
#else
    return path.isNull()
        ? qunsetenv("TESSDATA_DIR")
        : qputenv("TESSDATA_DIR", path.toUtf8());
#endif
}

int ocrSmoke(const QString& path) {
#if defined(QUIZPANE_HAS_QT_PDF) && defined(DOCUMENT_EXTRACTOR_HAS_OCR)
    const auto pdf = quizpane::studio::ExtractorRegistry{}.extract(path);
    const bool ok = pdf.error.isEmpty() && pdf.usedOcr && pdf.hasPageBoundaries &&
        pdf.ocrFailedPages == 0 && pdf.ocrSkippedPages == 0 &&
        pdf.plainText.contains(QStringLiteral("QUIZPANE"), Qt::CaseInsensitive) &&
        pdf.plainText.contains(QStringLiteral("first option"), Qt::CaseInsensitive);
    std::fprintf(stderr, "OCR smoke: ok=%d usedOcr=%d failedPages=%d error=%s\n",
                 ok, pdf.usedOcr, pdf.ocrFailedPages, pdf.error.toUtf8().constData());
    return ok ? 0 : 8;
#else
    Q_UNUSED(path);
    std::fprintf(stderr, "OCR smoke requires both QtPdf and OCR in this build\n");
    return 8;
#endif
}
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
    // Run from the unpacked ZIP, with no build-machine model paths. Also useful
    // for manual Windows 7 VM acceptance; fail if OCR was accidentally disabled.
    if (app.arguments().size() == 3 && app.arguments().at(1) == "--ocr-smoke")
        return ocrSmoke(app.arguments().at(2));
    // 透明 PDF 背景、字底笔画、单字下划线、无文字的空白横线必须区分。
    {
        QImage image(240, 100, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.fillRect(QRect(20, 33, 18, 2), Qt::black);
        painter.fillRect(QRect(42, 48, 18, 2), Qt::black);
        painter.fillRect(QRect(100, 35, 18, 2), Qt::black);
        painter.fillRect(QRect(18, 51, 23, 1), Qt::black);
        painter.fillRect(QRect(63, 51, 34, 1), Qt::black);
        painter.end();
        const QList<QRectF> boxes{{20.0 / 240, .3, 18.0 / 240, .2},
            {42.0 / 240, .3, 18.0 / 240, .2}, {}, {100.0 / 240, .3, 18.0 / 240, .2}};
        const auto result = quizpane::studio::detectRenderedLineDecorations(image, QStringLiteral("甲乙 丙"), boxes);
        if (result.ranges != QList<QPair<int, int>>{{0, 1}} ||
            result.blanks != QList<QPair<int, int>>{{2, 1}}) return 20;
    }
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;
    const QString txt = directory.filePath(QStringLiteral("sample.txt"));
    const QString md = directory.filePath(QStringLiteral("sample.md"));
    for (const QString& path : {txt, md}) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return 2;
        file.write(QStringLiteral("题目：天空是什么颜色？\n答案：蓝色").toUtf8());
    }
    quizpane::studio::ExtractorRegistry registry;
    if (registry.extract(txt).plainText.isEmpty())
        return 3;
    if (registry.extract(md).plainText.isEmpty())
        return 4;
    // 记事本"UTF-8 (带 BOM)"保存的文件：Qt5/Qt6 两条解码路径都必须剥掉
    // 行首 U+FEFF，否则题干首字前会多出一个不可见字符。
    {
        QFile file(directory.filePath(QStringLiteral("bom.txt")));
        if (!file.open(QIODevice::WriteOnly))
            return 12;
        file.write(QByteArrayLiteral("\xEF\xBB\xBF") +
                   QStringLiteral("题目：带 BOM 的记事本文件\n答案：正常").toUtf8());
        file.close();
        const auto bom = registry.extract(file.fileName());
        if (!bom.error.isEmpty() || bom.plainText.startsWith(QChar(0xFEFF))) {
            const QByteArray diagnostic =
                QStringLiteral("BOM test failed: error=%1 first=U+%2")
                    .arg(bom.error)
                    .arg(bom.plainText.isEmpty() ? 0 : bom.plainText.at(0).unicode(), 4, 16, QChar('0'))
                    .toUtf8();
            std::fprintf(stderr, "%s\n", diagnostic.constData());
            return 13;
        }
    }
    const QString docxPath = directory.filePath(QStringLiteral("sample.docx"));
    QString zipError;
    const QByteArray documentXml = QByteArrayLiteral(
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
        <w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
          <w:body><w:p><w:r><w:t>1. DOCX 题目</w:t></w:r></w:p>
          <w:p><w:r><w:t>A. 选项甲</w:t></w:r></w:p></w:body>
        </w:document>)");
    if (!quizpane::writeZipArchive(docxPath, {{QStringLiteral("word/document.xml"), documentXml}},
                                   &zipError))
        return 5;
    const auto docx = registry.extract(docxPath);
    if (!docx.error.isEmpty() || !docx.plainText.contains(QStringLiteral("DOCX 题目")))
        return 6;

#ifdef QUIZPANE_HAS_QT_PDF
    // 固定扫描夹具只有图像，没有 PDF 文字层；它由仓库管理，不随系统字体或
    // Qt 的 PDF 写入实现变化。该断言只验证 OCR 回退这条真实功能路径。
    const QString pdfPath = QStringLiteral(DOCUMENT_EXTRACTOR_OCR_FIXTURE);
    const auto pdf = registry.extract(pdfPath);
#ifdef DOCUMENT_EXTRACTOR_HAS_OCR
    if (ocrSmoke(pdfPath) != 0) return 8;
    // Build scripts supply the models explicitly. Exercise Unicode model paths
    // and corruption without touching those originals or relying on fallback.
    const QString previousModels = qEnvironmentVariable("TESSDATA_DIR");
    if (!previousModels.isEmpty()) {
        const QDir originals(previousModels);
        const QString unicodeModels = directory.filePath(QStringLiteral("中文路径 OCR 模型"));
        if (!QDir().mkpath(unicodeModels)) return 21;
        for (const QString& language : {QStringLiteral("chi_sim"), QStringLiteral("eng")}) {
            const QString name = language + QStringLiteral(".traineddata");
            if (!QFile::copy(originals.filePath(name), QDir(unicodeModels).filePath(name))) return 21;
        }
        if (!setTessdataDirectory(unicodeModels)) return 21;
        const int unicodeResult = ocrSmoke(pdfPath);
        QFile corrupt(QDir(unicodeModels).filePath(QStringLiteral("chi_sim.traineddata")));
        if (!corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 22;
        corrupt.write("invalid test model");
        corrupt.close();
        const auto invalidModels = registry.extract(pdfPath);
        if (!setTessdataDirectory(previousModels)) return 22;
        if (unicodeResult != 0 || invalidModels.ocrFailedPages == 0 || invalidModels.usedOcr ||
            !invalidModels.error.contains(QStringLiteral("识别模型缺失或损坏"))) return 22;
    }
    if (!pdf.error.isEmpty() || !pdf.usedOcr || !pdf.hasPageBoundaries ||
        pdf.plainText.trimmed().isEmpty()) {
        const QByteArray diagnostic =
            QStringLiteral("PDF extraction failed: error=%1 boundaries=%2 ocr=%3 text=%4")
                .arg(pdf.error)
                .arg(pdf.hasPageBoundaries)
                .arg(pdf.usedOcr)
                .arg(pdf.plainText)
                .toUtf8();
        std::fprintf(stderr, "%s\n", diagnostic.constData());
        return 8;
    }
#else
    if (pdf.error.isEmpty() || pdf.usedOcr || !pdf.hasPageBoundaries || pdf.ocrSkippedPages == 0)
        return 8;
#endif
#endif

    const auto invalidDocx = registry.extract(directory.filePath(QStringLiteral("missing.docx")));
    if (invalidDocx.error.isEmpty())
        return 9;

#ifdef QUIZPANE_HAS_QT_PDF
    // 可选的人工回归夹具：不提交受版权保护的真题 PDF，但在本地提供路径时验证
    // 下划线来自 PDF 原始渲染和文字坐标的交叉检测，而不是题目选项猜测。
    const QString underlineFixture = qEnvironmentVariable("QUIZPANE_UNDERLINE_FIXTURE");
    if (!underlineFixture.isEmpty()) {
        auto underlined = registry.extract(underlineFixture);
        if (!underlined.error.isEmpty()) return 10;
        const QStringList pages = underlined.plainText.split(QChar('\f'));
        if (pages.size() < 3) return 10;
        // 提取阶段不再扫描整卷；只有生成器根据题干语义回溯到材料时，才把该
        // 材料的视觉行交给精确像素检测。这里模拟那一步。
        detectPdfUnderlinesForCandidateLines(
            &underlined, {{3, pages.at(2).split(QChar('\n'), Qt::SkipEmptyParts)}});
        bool found = false;
        for (const auto& decoration : underlined.underlineDecorations.value(3)) {
            const int target = decoration.text.indexOf(QStringLiteral("采取"));
            if (target < 0) continue;
            for (const auto& range : decoration.ranges) {
                if (range.first <= target && range.first + range.second >= target + 2) {
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) return 11;
    }
#endif
    return 0;
}
