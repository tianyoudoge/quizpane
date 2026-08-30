#include "../apps/desktop-qt/src/ui/result_image_preview.hpp"

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QScrollArea>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (quizpane::ui::resultImagePixelWidth() > 680)
        return 1;

    const auto compactRows = quizpane::ui::compactResultRows(
        {QStringLiteral("A"), QStringLiteral("A"), QStringLiteral("B"),
         QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("?"),
         QStringLiteral("[AC]")});
    if (compactRows.size() != 2
        || compactRows.at(0).rangeLabel != QStringLiteral("1–5题")
        || compactRows.at(0).answerSequence != QStringLiteral("AABBC")
        || compactRows.at(1).rangeLabel != QStringLiteral("6–7题")
        || compactRows.at(1).answerSequence != QStringLiteral("?[AC]"))
        return 2;

    QScrollArea scroll;
    scroll.setWidgetResizable(false);
    auto* preview = new QLabel;
    scroll.setWidget(preview);

    const QImage resultImage(quizpane::ui::resultImagePixelWidth(), 356,
                             QImage::Format_ARGB32_Premultiplied);
    quizpane::ui::setResultPreviewImage(preview, resultImage);

    if (preview->size() != resultImage.size())
        return 3;
    if (scroll.widget() != preview || preview->sizeHint() != resultImage.size())
        return 4;
    return 0;
}
