#include "../apps/desktop-qt/src/ui/solution_page_controller.hpp"
#include "../apps/desktop-qt/src/ui/formula_formatter.hpp"
#include "quizpane/provider_loader.hpp"

#include <QApplication>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
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
    if (!page->findChild<QPushButton*>(QStringLiteral("aiConfigButton"))) return 12;
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
    if (status->isHidden() || !explanation->isHidden()) return 10;
    single.insert("correctChoice", 0);
    single.insert("contentHtml", "<p>资料分析题</p>");
    single.insert("options", QJsonArray{
        QJsonObject{{"label", "A"}, {"contentHtml", "<p>低于 12.0%</p>"}},
        QJsonObject{{"label", "D"}, {"contentHtml", "<p>超过 88.0%</p>"},
                    {"imageUrl", "file:///tmp/option.png"}},
        QJsonObject{{"label", "E"}, {"contentHtml", "<p>第一行</p><p>第二行</p>"}}});
    single.insert("solutionHtml", "<p> </p>");
    session.solutions = QJsonArray{single};
    controller.showSolution(0);
    auto* question = page->findChild<QLabel*>(QStringLiteral("solutionQuestion"));
    if (!question || !question->text().contains(QStringLiteral("<b>A.</b> 低于 12.0%")) ||
        !question->text().contains(QStringLiteral("<b>D.</b> 超过 88.0%")) ||
        !question->text().contains(QStringLiteral("<p>第一行</p><p>第二行</p>")) ||
        !question->text().contains(QStringLiteral("file:///tmp/option.png")) ||
        !explanation->isHidden()) return 15;
    single.insert("solutionHtml", "<p>题库原解析</p>");
    session.solutions = QJsonArray{single};
    controller.showSolution(0);
    if (explanation->isHidden()) return 16;
    single.insert("solutionHtml", "<p><img src=\"file:///tmp/solution.png\"></p>");
    session.solutions = QJsonArray{single};
    controller.showSolution(0);
    if (explanation->isHidden()) return 17;
    single.insert("id", "formula-q1");
    single.insert("aiSolutionHtml", "<div>\\sin{x} + x^{2}</div>");
    session.solutions = QJsonArray{single};
    controller.showSolution(0);
    auto* aiPanel = page->findChild<QFrame*>(QStringLiteral("aiExplainPanel"));
    auto* aiContent = page->findChild<QLabel*>(QStringLiteral("aiExplainContent"));
    if (!aiPanel || aiPanel->isHidden() || !aiContent ||
        !aiContent->text().contains(QStringLiteral("sin(x) + x<sup>2</sup>"))) return 18;
    QJsonObject other = single;
    other.insert("id", "formula-q2");
    other.remove("aiSolutionHtml");
    session.solutions = QJsonArray{single, other};
    session.answers = {{0}, {0}};
    controller.showSolution(1);
    if (!aiPanel->isHidden()) return 19;
    controller.showSolution(0);
    if (aiPanel->isHidden() ||
        !aiContent->text().contains(QStringLiteral("sin(x) + x<sup>2</sup>"))) return 20;
    return 0;
}

