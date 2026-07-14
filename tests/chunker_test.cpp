#include "quizpane/studio/chunker.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QSet>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1) {
        QFile file(QString::fromLocal8Bit(argv[1]));
        if (!file.open(QIODevice::ReadOnly))
            return 10;
        quizpane::studio::Chunker diagnosticChunker(2000);
        const QString text = QString::fromUtf8(file.readAll());
        const auto blocks = diagnosticChunker.blocks(file.fileName(), text);
        const auto chunks = diagnosticChunker.split(file.fileName(), text);
        QSet<int> covered;
        int indivisible = 0;
        for (const auto& block : blocks) {
            if (block.text.trimmed().isEmpty() || covered.contains(block.index))
                return 11;
            if (block.indivisible)
                ++indivisible;
            covered.insert(block.index);
        }
        QSet<int> requested;
        for (const auto& chunk : chunks)
            for (int index : chunk.sourceBlockIndices)
                requested.insert(index);
        if (covered != requested)
            return 12;
        qInfo().noquote() << QStringLiteral("真实文档切块：%1 字符，%2 个 SourceBlock，"
                                            "%3 个不可拆块，%4 次模型请求")
                                 .arg(text.size())
                                 .arg(blocks.size())
                                 .arg(indivisible)
                                 .arg(chunks.size());
        return 0;
    }
    quizpane::studio::Chunker chunker(10);
    const auto chunks = chunker.split(QStringLiteral("source.md"), QString(100, QChar('x')));
    if (chunks.size() != 4)
        return 1;
    for (int index = 0; index < chunks.size(); ++index) {
        if (chunks.at(index).index != index || chunks.at(index).estimatedTokens > 10 ||
            chunks.at(index).sourcePath != QStringLiteral("source.md"))
            return 2;
    }

    const QString grouped = QStringLiteral(
        "根据以下资料，回答第 1～3 题\n\n"
        "这是一篇足够长的阅读材料，内容用于检验材料正文与连续子题不能被 token 预算拆散。\n\n"
        "1. 第一题是什么？\nA. 甲\nB. 乙\nC. 丙\nD. 丁\n\n"
        "2. 第二题是什么？\nA. 甲\nB. 乙\nC. 丙\nD. 丁\n\n"
        "3. 第三题是什么？\nA. 甲\nB. 乙\nC. 丙\nD. 丁");
    const auto groupedBlocks = chunker.blocks(QStringLiteral("reading.txt"), grouped, 7);
    const auto groupedChunks = chunker.split(QStringLiteral("reading.txt"), grouped, 7);
    if (groupedBlocks.size() != 1 || !groupedBlocks.first().indivisible ||
        groupedBlocks.first().index != 7)
        return 3;
    if (groupedChunks.size() != 1 || groupedChunks.first().sourceBlockIndices != QVector<int>{7} ||
        !groupedChunks.first().text.contains(QStringLiteral("第一题")) ||
        !groupedChunks.first().text.contains(QStringLiteral("第三题")))
        return 4;
    return 0;
}
