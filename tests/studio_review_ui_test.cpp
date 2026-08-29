#include "studio_window.hpp"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTreeWidget>

namespace quizpane::studio {
class StudioWindowReviewTest {
public:
    static QJsonObject question(const QString& id, const QString& risk = {},
                               const QString& signal = {}, const QString& reason = {}) {
        QJsonObject q{{"id", id}, {"stem", QStringLiteral("请核对题干与选项是否和原题一致。")},
            {"type", "single_choice"}, {"catalogId", "generated"},
            {"source", QJsonObject{{"document", QStringLiteral("测试资料.pdf")},
                                    {"questionNumber", id.mid(1).toInt()}}},
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
        if (!window.parseModeCard_ || !window.ruleModeCard_ || !window.smartModeCard_ ||
            !window.smartModeCard_->text().contains(QStringLiteral("智能解析")) ||
            !window.mineruConfigButton_ || !window.mineruConfigSummary_ ||
            !window.mineruConfigSummary_->text().contains(QStringLiteral("准确识别")) ||
            !window.parseStatusChip_ || !window.parseStatusText_ ||
            window.parseStatusText_->text() != QStringLiteral("智能模式") ||
            !window.progressBar_ || !window.progressStatus_ ||
            window.progressStatus_->text() != QStringLiteral("准备中")) return 19;
        auto* buildVersion = window.findChild<QAction*>(QStringLiteral("studioBuildVersionAction"));
        auto* about = window.findChild<QAction*>(QStringLiteral("studioAboutAction"));
        if (!buildVersion || buildVersion->isEnabled() ||
            !buildVersion->text().startsWith(QStringLiteral("构建版本：")) || !about ||
            about->text() != QStringLiteral("关于题库制作器")) return 24;
        // 已提交云端任务的恢复状态只落非敏感信息；Token 不在 QSettings 内。
        QTemporaryDir cloudCache;
        if (!cloudCache.isValid()) return 22;
        window.cloudSessionId_ = QStringLiteral("resume-test");
        window.cloudCacheDir_ = cloudCache.path();
        window.cloudBatchId_ = QStringLiteral("batch-test");
        window.pendingGroups_ = {{QStringLiteral("/tmp/question.pdf"), {}, true,
                                  QStringLiteral("/tmp/result.zip"), {}}};
        window.cloudIndex_ = 0;
        window.cloudParsingAnswer_ = false;
        window.persistCloudTask();
        QSettings taskSettings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
        taskSettings.beginGroup(QStringLiteral("question-maker/mineru/pending-cloud-task"));
        const bool persisted = taskSettings.value(QStringLiteral("sessionId")).toString() ==
                                   QStringLiteral("resume-test") &&
            taskSettings.value(QStringLiteral("batchId")).toString() == QStringLiteral("batch-test") &&
            taskSettings.beginReadArray(QStringLiteral("groups")) == 1;
        taskSettings.endArray();
        taskSettings.endGroup();
        if (!persisted) return 23;
        window.clearPersistedCloudTask(false);
        QTemporaryDir sourceDir;
        const QString sourcePath = sourceDir.filePath(QStringLiteral("题目.txt"));
        QFile sourceFile(sourcePath);
        if (!sourceFile.open(QIODevice::WriteOnly) || sourceFile.write("1. 测试题\nA. 对\nB. 错\n") < 1) return 20;
        sourceFile.close();
        window.appendSources({sourcePath});
        auto* answerLocation = window.findChild<QComboBox*>(QStringLiteral("answerLocation"));
        if (window.sourceRows_.size() != 1 || !answerLocation ||
            answerLocation->currentText() != QStringLiteral("含答案")) return 21;
        if (!window.nextButton_->text().contains(QStringLiteral("开始智能解析")) ||
            window.nextButton_->objectName() != QStringLiteral("primaryButton") ||
            !window.sourceModeHint_->text().contains(QStringLiteral("上传到 MinerU"))) return 26;

        window.mineruConfig_.cloudEnabled = false;
        window.mineruConfig_.modeSelectedByUser = true;
        window.updateNavigation();
        if (window.nextButton_->text() != QStringLiteral("开始规则解析  →") ||
            window.parseStatusText_->text() != QStringLiteral("规则模式") ||
            !window.sourceModeHint_->text().contains(QStringLiteral("不会上传"))) return 28;
        window.mineruConfig_.cloudEnabled = true;
        window.mineruConfig_.modeSelectedByUser = true;
        window.updateNavigation();
        if (window.nextButton_->text() != QStringLiteral("开始智能解析  →") ||
            window.parseStatusText_->text() != QStringLiteral("智能模式")) return 29;
        answerLocation->setCurrentIndex(1);
        if (window.hasAnswerKeyByQuestion_.value(sourcePath, true)) return 27;
        answerLocation->setCurrentIndex(0);
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
        if (!chips.isEmpty()) return 1;
        if (!window.allReviewButton_->isChecked() ||
            !window.allReviewButton_->text().endsWith("2") ||
            !window.allQuestionsButton_->text().endsWith("5") ||
            !group->child(0)->isHidden() || !group->child(1)->isHidden() ||
            group->child(3)->isHidden() || group->child(4)->isHidden() ||
            group->child(1)->checkState(0) != Qt::Checked) return 2;
        window.allReviewButton_->click();  // Active tabs cannot silently toggle filtering off.
        if (!window.allReviewButton_->isChecked() || !group->child(0)->isHidden()) return 3;
        if (!window.missingAnswerButton_->isHidden() || !window.duplicateButton_->isHidden()) return 4;
        window.allQuestionsButton_->click();
        for (int i = 0; i < group->childCount(); ++i)
            if (group->child(i)->isHidden()) return 7;

        // 精确回归真实事故：大量图片/OCR 软提示题必须保持收录；用户编辑并
        // 确认一个硬问题后，不能让最终打包输入退化成只剩当前这一题。
        window.showReviewQuestion(group->child(3));
        window.confirmCurrentReviewQuestion();
        int selectedAfterSingleEdit = 0;
        for (int i = 0; i < group->childCount(); ++i)
            if (group->child(i)->checkState(0) == Qt::Checked)
                ++selectedAfterSingleEdit;
        if (selectedAfterSingleEdit != 4 ||
            group->child(1)->checkState(0) != Qt::Checked ||
            group->child(2)->checkState(0) != Qt::Checked ||
            window.currentReviewItem_ != group->child(4) ||
            window.confirmReviewButton_->text() != QStringLiteral("保存并收录")) return 25;

        bool hasSimpleSummary = false;
        for (auto* label : window.riskCategoryPanel_->findChildren<QLabel*>())
            if (label->text().contains(QStringLiteral("还有 1 题需要决定")) && label->wordWrap())
                hasSimpleSummary = true;
        if (!hasSimpleSummary || window.nextButton_->text() != QStringLiteral("继续生成（收录 4 题） →") ||
            window.nextButton_->objectName() != QStringLiteral("primaryButton")) return 8;

        // Optional real-widget screenshots, without adding user fixtures to the repository.
        const QString previewDir = qEnvironmentVariable("QUIZPANE_UI_PREVIEW_DIR");
        if (!previewDir.isEmpty()) {
            window.allReviewButton_->click();
            window.reviewTree_->setCurrentItem(group->child(4));
            for (const QString& theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
                QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
                settings.setValue(QStringLiteral("ui/colorTheme"), theme);
                window.applyStyle();
                app.processEvents();
                if (!window.grab().save(QDir(previewDir).filePath(theme + ".png"))) return 9;
            }
        }
        window.excludeCurrentReviewQuestion();
        if (!window.allQuestionsButton_->isChecked() ||
            window.allReviewButton_->text() != QStringLiteral("需要处理  0") ||
            window.nextButton_->text() != QStringLiteral("继续生成（收录 4 题） →")) return 30;
        // Rebuilding must delete old nested rows/buttons and their group membership.
        window.populateReview(candidate);
        if (!window.findChildren<QPushButton*>(QStringLiteral("reviewCategoryChip")).isEmpty() ||
            window.reviewFilterGroup_->buttons().size() != 4) return 11;
        candidate.questions = {question("q1")};
        candidate.needsReviewQuestions = {};
        candidate.hasAnswerKey = false;
        window.populateReview(candidate);
        if (window.riskCategoryPanel_->isHidden() || !window.missingAnswerButton_->isHidden() ||
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
        window.allQuestionsButton_->click();
        auto* material = window.reviewTree_->topLevelItem(0);
        if (material->isHidden() || material->child(0)->isHidden()) return 13;
        candidate.materials = {};
        candidate.questions = {};
        for (int i = 1; i <= 2; ++i) {
            auto q = question(QStringLiteral("q%1").arg(i));
            q.insert("source", QJsonObject{{"questionNumber", 152}, {"questionLabel",
                QStringLiteral("原第 152 题 · 同号第 %1 处").arg(i)}});
            candidate.questions.append(q);
        }
        window.populateReview(candidate);
        auto* first = window.reviewTree_->topLevelItem(0)->child(0);
        auto* second = window.reviewTree_->topLevelItem(0)->child(1);
        if (first->text(0) == second->text(0) || !second->text(0).contains(QStringLiteral("同号第 2 处"))) return 15;
        window.showReviewQuestion(second);
        if (window.reviewDetailTitle_->text() != second->text(0)) return 16;
        if (window.reviewTree_->textElideMode() != Qt::ElideMiddle ||
            first->toolTip(0) != first->text(0) || second->toolTip(0) != second->text(0)) return 18;
        if (!previewDir.isEmpty()) {
            app.processEvents();
            if (!window.grab().save(QDir(previewDir).filePath("repeated-numbers.png"))) return 17;
        }
        // 删除选项必须是清晰可见的文字操作，并且真的从草稿中移除。
        auto removeButtons = window.findChildren<QPushButton*>(QStringLiteral("reviewOptionRemoveButton"));
        if (removeButtons.size() != 2 || removeButtons.first()->text() != QStringLiteral("删除")) return 5;
        removeButtons.first()->click();
        app.processEvents();
        if (window.reviewOptionEditors_.size() != 1) return 6;

        // 内部填空标记在校对页必须显示为真实横线，且未编辑直接保存时仍写回
        // 稳定的 schema 标记，不能把“〔填空〕”文案直接暴露给用户。
        auto fill = question(QStringLiteral("q6"));
        fill.insert(QStringLiteral("stem"), QStringLiteral("青色文化〔填空〕，又显得〔填空〕了。"));
        candidate.questions = {fill};
        candidate.needsReviewQuestions = {};
        window.populateReview(candidate);
        auto* fillItem = window.reviewTree_->topLevelItem(0)->child(0);
        window.showReviewQuestion(fillItem);
        if (window.reviewStemEditor_->toPlainText().contains(QStringLiteral("〔填空〕")) ||
            window.reviewStemEditor_->toPlainText().count(QStringLiteral("＿＿＿＿")) != 2 ||
            window.reviewQuestionIsDirty() || !window.saveCurrentReviewQuestion() ||
            fillItem->data(0, Qt::UserRole).toJsonObject().value(QStringLiteral("stem"))
                .toString().count(QStringLiteral("〔填空〕")) != 2) return 32;
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
