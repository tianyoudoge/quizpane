#include "quizpane/studio/document_extractor.hpp"
#include "quizpane/zip_archive.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
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

    // 固定扫描夹具只有图像，没有 PDF 文字层；它由仓库管理，不随系统字体或
    // Qt 的 PDF 写入实现变化。该断言只验证 OCR 回退这条真实功能路径。
    const QString pdfPath = QStringLiteral(DOCUMENT_EXTRACTOR_OCR_FIXTURE);
    const auto pdf = registry.extract(pdfPath);
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

    const auto invalidDocx = registry.extract(directory.filePath(QStringLiteral("missing.docx")));
    if (invalidDocx.error.isEmpty())
        return 9;
    return 0;
}
