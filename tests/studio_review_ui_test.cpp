#include "studio_window.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QDir>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeWidget>

namespace quizpane::studio {
class StudioWindowReviewTest {
public:
    static QJsonObject question(const QString& id, const QString& risk = {},
                               const QString& signal = {}, const QString& reason = {}) {
        QJsonObject q{{"id", id}, {"stem", QStringLiteral("请核对题干与选项是否和原题一致。")},
            {"type", "single_choice"},
            {"source", QJsonObject{{"questionNumber", id.mid(1).toInt()}}},
            {"options", QJsonArray{QJsonObject{{"id", "a"}, {"text", "选项 A"}},
                                   QJsonObject{{"id", "b"}, {"text", "选项 B"}}}},
            {"answer", QJsonObject{{"optionIds", QJsonArray{"a"}}}}};
        if (!risk.isEmpty())
            q.insert("review", QJsonObject{{"needsReview", true}, {"riskLevel", risk},
                {"signals", QJsonArray{signal}}, {"reason", reason}});
        return q;
    }

    static int run(QApplication& app) {
        StudioWindow window;
        GeneratedBankCandidate candidate;
        candidate.hasAnswerKey = true;
        candidate.questions = {question("q1"), question("q2", "soft", "image-content"),
                               question("q3", "soft", "image-content")};
        candidate.needsReviewQuestions = {
            question("q4", "hard", "missing-answer", QStringLiteral("未识别到答案")),
            question("q5", "hard", "duplicate", QStringLiteral("疑似重复"))};
        window.populateReview(candidate);
        window.pages_->setCurrentIndex(2);
        window.resize(1040, 800);
        window.show();
        app.processEvents();
        if (window.reviewTree_->header()->viewport()->height() <
            window.reviewTree_->header()->fontMetrics().height()) return 14;
        auto* group = window.reviewTree_->topLevelItem(0);
        auto chips = window.findChildren<QPushButton*>(QStringLiteral("reviewCategoryChip"));
        if (chips.size() != 1 || !chips.first()->isCheckable() ||
            chips.first()->cursor().shape() != Qt::PointingHandCursor) return 1;
        if (!window.allReviewButton_->isChecked() ||
            !window.allReviewButton_->text().endsWith("4") ||
            !window.allQuestionsButton_->text().endsWith("5") ||
            !group->child(0)->isHidden() || group->child(1)->isHidden()) return 2;
        window.allReviewButton_->click();  // Active tabs cannot silently toggle filtering off.
        if (!window.allReviewButton_->isChecked() || !group->child(0)->isHidden()) return 3;
        chips.first()->click();
        if (!chips.first()->isChecked() || window.allReviewButton_->isChecked() ||
            group->child(1)->isHidden() || group->child(2)->isHidden() ||
            !group->child(3)->isHidden() ||
            group->child(1)->checkState(0) != Qt::Unchecked) return 4;
        window.missingAnswerButton_->click();
        if (chips.first()->isChecked() || group->child(3)->isHidden() ||
            !group->child(2)->isHidden()) return 5;
        window.duplicateButton_->click();
        if (group->child(4)->isHidden() || !group->child(3)->isHidden()) return 6;
        window.allQuestionsButton_->click();
        for (int i = 0; i < group->childCount(); ++i)
            if (group->child(i)->isHidden()) return 7;
        bool hasGenerationHint = false;
        for (auto* label : window.riskCategoryPanel_->findChildren<QLabel*>())
            if (label->text().contains(QStringLiteral("未勾选的题目不会纳入题库")) && label->wordWrap())
                hasGenerationHint = true;
        if (!hasGenerationHint) return 8;

        // Optional real-widget screenshots, without adding user fixtures to the repository.
        const QString previewDir = qEnvironmentVariable("QUIZPANE_UI_PREVIEW_DIR");
        if (!previewDir.isEmpty()) {
            window.allReviewButton_->click();
            window.reviewTree_->setCurrentItem(group->child(1));
            for (const QString& theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
                QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
                settings.setValue(QStringLiteral("ui/colorTheme"), theme);
                window.applyStyle();
                app.processEvents();
                if (!window.grab().save(QDir(previewDir).filePath(theme + ".png"))) return 9;
            }
        }
        window.confirmRiskCategory("image-content");
        if (group->child(1)->checkState(0) != Qt::Checked ||
            group->child(2)->checkState(0) != Qt::Checked ||
            group->child(3)->checkState(0) != Qt::Unchecked) return 10;

        // Rebuilding must delete old nested rows/buttons and their group membership.
        window.populateReview(candidate);
        if (window.findChildren<QPushButton*>(QStringLiteral("reviewCategoryChip")).size() != 1 ||
            window.reviewFilterGroup_->buttons().size() != 5) return 11;
        candidate.questions = {question("q1")};
        candidate.needsReviewQuestions = {};
        candidate.hasAnswerKey = false;
        window.populateReview(candidate);
        if (!window.riskCategoryPanel_->isHidden() || !window.missingAnswerButton_->isHidden() ||
            !window.allQuestionsButton_->isChecked() || window.reviewFilterGroup_->buttons().size() != 4)
            return 12;

        // A matching material must not hide its own matching child questions.
        auto child = question("q2", "soft", "image-content");
        child.insert("materialId", "m1");
        candidate.questions.append(child);
        candidate.materials = {QJsonObject{{"id", "m1"}, {"title", "材料"}, {"text", "材料正文"},
            {"review", QJsonObject{{"needsReview", true}, {"riskLevel", "soft"},
                {"signals", QJsonArray{"image-content"}}}}}};
        window.populateReview(candidate);
        window.findChild<QPushButton*>(QStringLiteral("reviewCategoryChip"))->click();
        auto* material = window.reviewTree_->topLevelItem(0);
        if (material->isHidden() || material->child(0)->isHidden()) return 13;
        return 0;
    }
};
}  // namespace quizpane::studio

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QStandardPaths::setTestModeEnabled(true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    return quizpane::studio::StudioWindowReviewTest::run(app);
}
