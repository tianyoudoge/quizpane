#include "quizpane/studio/document_extractor.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const QString txt = directory.filePath(QStringLiteral("sample.txt"));
    const QString md = directory.filePath(QStringLiteral("sample.md"));
    for (const QString& path : {txt, md}) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return 2;
        file.write(QStringLiteral("题目：天空是什么颜色？\n答案：蓝色").toUtf8());
    }
    quizpane::studio::ExtractorRegistry registry;
    if (registry.extract(txt).plainText.isEmpty()) return 3;
    if (registry.extract(md).plainText.isEmpty()) return 4;
    const auto docx = registry.extract(directory.filePath(QStringLiteral("x.docx")));
    const auto pdf = registry.extract(directory.filePath(QStringLiteral("x.pdf")));
    if (!docx.error.contains(QStringLiteral("暂不支持 DOCX"))) return 5;
    if (!pdf.error.contains(QStringLiteral("暂不支持 PDF"))) return 6;
    return 0;
}
