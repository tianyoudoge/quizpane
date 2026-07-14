#include "quizpane/studio/document_extractor.hpp"
#include "quizpane/zip_archive.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <array>
#include <cstdio>

namespace {

// 构造只使用 PDF 内置 Helvetica 字体的最小文字型 PDF。这里不再使用
// QPdfWriter 和系统默认字体，否则不同平台会生成不同的字体子集及编码，导致
// 测试在 macOS/Linux 验证文字层、在 Windows 却意外验证 OCR 回退。
QByteArray minimalTextPdf() {
    const QByteArray stream = QByteArrayLiteral(
        "BT\n/F1 24 Tf\n72 700 Td\n(PDF question) Tj\n"
        "0 -36 Td\n(A. first option) Tj\nET\n");
    const std::array<QByteArray, 5> objects = {
        QByteArrayLiteral("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArrayLiteral("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArrayLiteral(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>"),
        QByteArrayLiteral("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
        QByteArrayLiteral("<< /Length ") + QByteArray::number(stream.size()) +
            QByteArrayLiteral(" >>\nstream\n") + stream + QByteArrayLiteral("endstream")};

    QByteArray pdf = QByteArrayLiteral("%PDF-1.4\n");
    std::array<qsizetype, 5> offsets{};
    for (size_t index = 0; index < objects.size(); ++index) {
        offsets[index] = pdf.size();
        pdf += QByteArray::number(index + 1) + QByteArrayLiteral(" 0 obj\n") + objects[index] +
               QByteArrayLiteral("\nendobj\n");
    }
    const qsizetype xrefOffset = pdf.size();
    pdf += QByteArrayLiteral("xref\n0 6\n0000000000 65535 f \n");
    for (const qsizetype offset : offsets)
        pdf += QByteArray::number(offset).rightJustified(10, '0') + QByteArrayLiteral(" 00000 n \n");
    pdf += QByteArrayLiteral("trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n") +
           QByteArray::number(xrefOffset) + QByteArrayLiteral("\n%%EOF\n");
    return pdf;
}

} // namespace

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
        QFile file(pdfPath);
        const QByteArray bytes = minimalTextPdf();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            return 7;
    }
    const auto pdf = registry.extract(pdfPath);
    if (!pdf.error.isEmpty() || pdf.usedOcr || !pdf.hasPageBoundaries ||
        !pdf.plainText.contains(QStringLiteral("PDF question"))) {
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
