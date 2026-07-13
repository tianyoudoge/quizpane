#include "quizpane/studio/chunker.hpp"

#include <QCoreApplication>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    quizpane::studio::Chunker chunker(10);
    const auto chunks = chunker.split(QStringLiteral("source.md"), QString(100, QChar('x')));
    if (chunks.size() != 4) return 1;
    for (int index = 0; index < chunks.size(); ++index) {
        if (chunks.at(index).index != index || chunks.at(index).estimatedTokens > 10 ||
            chunks.at(index).sourcePath != QStringLiteral("source.md")) return 2;
    }
    return 0;
}
