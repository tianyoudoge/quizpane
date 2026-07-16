#include "../apps/desktop-qt/src/ui/question_navigator.hpp"
#include "../apps/desktop-qt/src/app_settings.hpp"

#include <QApplication>
#include <QPushButton>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    quizpane::ui::QuestionNavigator navigator;

    if (quizpane::layout_metrics::kNavIconPixels <=
            quizpane::layout_metrics::kIconPixels ||
        quizpane::layout_metrics::kNavIconButtonPixels <=
            quizpane::layout_metrics::kIconButtonPixels) return 8;

    navigator.setState(20, QSet<int>{0, 3, 18}, 3);
    auto* first = navigator.questionButton(0);
    auto* fourth = navigator.questionButton(3);
    auto* fifth = navigator.questionButton(4);
    if (!first || !fourth || !fifth || navigator.questionButton(20)) return 1;
    if (!first->property("answered").toBool()) return 2;
    if (!fourth->property("answered").toBool() ||
        !fourth->property("current").toBool()) return 3;
    if (fifth->property("answered").toBool() || fifth->property("current").toBool()) return 4;

    int selected = -1;
    QObject::connect(&navigator, &quizpane::ui::QuestionNavigator::questionSelected,
                     [&selected](int index) { selected = index; });
    fifth->click();
    if (selected != 4) return 5;

    navigator.setState(8, QSet<int>{1}, 7);
    if (navigator.questionButton(8) || !navigator.questionButton(7)) return 6;
    if (!navigator.questionButton(1)->property("answered").toBool()) return 7;
    return 0;
}
