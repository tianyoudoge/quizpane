#include "quizpane/studio/document_extractor.hpp"
#include "quizpane/zip_archive.hpp"

#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QPainter>
#include <QPdfWriter>
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

    const QString pdfPath = directory.filePath(QStringLiteral("sample.pdf"));
    {
        QPdfWriter writer(pdfPath);
        // 使用 PDF 点作为绘制单位并放大字体，确保没有文字层、必须走 OCR 的
        // 平台也能稳定识别；默认 1200 DPI 下用小坐标绘制会得到极小文字。
        writer.setResolution(72);
        QPainter painter(&writer);
        QFont font = painter.font();
        font.setPointSize(24);
        painter.setFont(font);
        painter.drawText(QPointF(72, 144), QStringLiteral("1. PDF question"));
        painter.drawText(QPointF(72, 216), QStringLiteral("A. first option"));
    }
    const auto pdf = registry.extract(pdfPath);
    // Qt PDF 的文字层提取与 Tesseract 的具体识别文本会随平台和字体栅格化
    // 结果变化。端到端测试只要求成功得到非空分页文本，不把 OCR 字符级精度
    // 误当成文档提取器的跨平台契约。
    const bool missingExpectedText =
        !pdf.usedOcr && !pdf.plainText.contains(QStringLiteral("PDF question"));
    if (!pdf.error.isEmpty() || !pdf.hasPageBoundaries || pdf.plainText.trimmed().isEmpty() ||
        missingExpectedText) {
        const QByteArray diagnostic =
            QStringLiteral("PDF extraction failed: error=%1 boundaries=%2 ocr=%3 text=%4")
                .arg(pdf.error)
                .arg(pdf.hasPageBoundaries)
                .arg(pdf.usedOcr)
                .arg(pdf.plainText)
                .toUtf8();
        std::fprintf(stderr, "%s\n", diagnostic.constData());
        return 7;
    }

    const auto invalidDocx = registry.extract(directory.filePath(QStringLiteral("missing.docx")));
    if (invalidDocx.error.isEmpty())
        return 8;
    return 0;
}
