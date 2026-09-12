#include "../apps/desktop-qt/src/ui/solution_page_controller.hpp"
#include "quizpane/provider_loader.hpp"

#include <QApplication>
#include <QLabel>
#include <QStackedWidget>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QStackedWidget pages;
    auto* page = new QWidget;
    auto* catalog = new QWidget;
    pages.addWidget(page);
    pages.addWidget(catalog);
    quizpane::ProviderLoader provider;
    quizpane::AttemptSession session;
    quizpane::UiSize size = quizpane::UiSize::Small;
    quizpane::SolutionPageController controller;
    controller.init(page, &pages, catalog, provider, session, size, [] {});
    controller.buildInto();
    auto* status = page->findChild<QLabel*>(QStringLiteral("answerStatus"));
    if (!status) return 1;
    const auto check = [&](const QJsonObject& solution, const QSet<int>& selected,
                           bool expected) {
        session.solutions = QJsonArray{solution};
        session.answers = {selected};
        controller.showSolution(0);
        return status->property("correct").toBool() == expected &&
            status->text() == (expected ? QStringLiteral("✓ 正确") : QStringLiteral("✗ 错误"));
    };
    // Native providers (including demo/template) return only correctChoice.
    QJsonObject single{{"type", "single_choice"}, {"correctChoice", 0}};
    if (!check(single, {}, false)) return 2;
    if (!check(single, {0}, true)) return 3;
    if (!check(single, {1}, false)) return 4;
    // Declarative providers return both representations for single choice.
    single.insert("correctChoices", QJsonArray{0});
    if (!check(single, {0}, true) || !check(single, {}, false)) return 5;
    QJsonObject multiple{{"type", "multiple_choice"},
                         {"correctChoices", QJsonArray{0, 2}}};
    if (!check(multiple, {0, 2}, true) || !check(multiple, {0}, false) ||
        !check(multiple, {0, 1, 2}, false) || !check(multiple, {}, false)) return 6;
    // Missing answers must never make an unanswered question appear correct.
    multiple.remove("correctChoices");
    if (!check(multiple, {}, false)) return 7;
    single.remove("correctChoice");
    single.remove("correctChoices");
    if (!check(single, {}, false)) return 8;
    // Switching between answerless and answered attempts restores visibility.
    session.attemptHasAnswerKey = false;
    controller.showSolution(0);
    auto* explanation = page->findChild<QLabel*>(QStringLiteral("solutionText"));
    if (!status->isHidden() || !explanation || !explanation->isHidden()) return 9;
    session.attemptHasAnswerKey = true;
    controller.showSolution(0);
    if (status->isHidden() || explanation->isHidden()) return 10;
    return 0;
}
