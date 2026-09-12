#include "../apps/desktop-qt/src/ui/practice_page_controller.hpp"
#include "quizpane/draft_store.hpp"
#include "quizpane/provider_loader.hpp"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

// Exercise both host configurations: a plain page and a page whose course
// status bar already owns the layout, even while that bar is hidden.
bool checkPage(bool withCourseBar, bool withResizeHandle = true) {
    QWidget window;
    auto* root = new QVBoxLayout(&window);
    auto* header = new QWidget;
    auto* pages = new QStackedWidget;
    auto* resizeHandle = new QWidget;
    root->addWidget(header);
    root->addWidget(pages);
    root->addWidget(resizeHandle);
    auto* page = new QWidget;
    pages->addWidget(page);
    auto* catalog = new QWidget;
    pages->addWidget(catalog);
    QVBoxLayout* originalLayout = nullptr;
    QLabel* courseBar = nullptr;
    if (withCourseBar) {
        originalLayout = new QVBoxLayout(page);
        courseBar = new QLabel(QStringLiteral("Course connected"));
        originalLayout->addWidget(courseBar);
        courseBar->hide();
    }
    quizpane::ProviderLoader provider;
    quizpane::DraftStore drafts;
    quizpane::AttemptSession session;
    quizpane::UiSize size = quizpane::UiSize::Small;
    QString providerId;  // No draft writes or external provider requests.
    bool restoreChecked = false;
    quizpane::PracticePageController controller;
    controller.init(page, pages, catalog, header, &window,
                    withResizeHandle ? resizeHandle : nullptr,
                    provider, drafts, session, size, providerId, restoreChecked);
    controller.buildInto();
    if (originalLayout && page->layout() != originalLayout) return false;
    if (!page->isAncestorOf(controller.controlBar()) ||
        page->layout()->indexOf(controller.controlBar()) < 0) return false;
    auto* scroll = page->findChild<QScrollArea*>();
    auto* question = page->findChild<QLabel*>(QStringLiteral("questionText"));
    if (!scroll || !question || page->layout()->indexOf(scroll) < 0) return false;
    if (courseBar && page->layout()->itemAt(0)->widget() != courseBar) return false;

    session.questions = QJsonArray{QJsonObject{
        {"id", "q1"}, {"type", "single_choice"}, {"contentHtml", "Test question"},
        {"options", QJsonArray{QJsonObject{
            {"label", "A"}, {"contentHtml", "Test answer"}}}}}};
    controller.resetForNewQuestions();
    window.resize(400, 500);
    window.show();
    QApplication::processEvents();
    auto* option = page->findChild<QPushButton*>(QStringLiteral("answerOption"));
    if (!question->isVisible() || !option || !option->isVisible() ||
        !controller.controlBar()->isVisible() || scroll->height() <= 0) return false;
    if (courseBar) {
        courseBar->show();
        QApplication::processEvents();
        if (!courseBar->isVisible() || !option->isVisible() ||
            !controller.controlBar()->isVisible()) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    if (!checkPage(false)) return 1;
    if (!checkPage(true)) return 2;
    if (!checkPage(true, false)) return 3;
    return 0;
}
