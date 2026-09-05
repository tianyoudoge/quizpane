#include "studio_window.hpp"
#include "mineru_settings_dialog.hpp"
#include "review_image_utils.hpp"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QBuffer>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QProgressBar>
#include <QSettings>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
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
        bool creditMatches = false;
        QTimer::singleShot(0, [&creditMatches] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QLabel* credit = nullptr;
            if (dialog) {
                for (auto* label : dialog->findChildren<QLabel*>()) {
                    if (label->property("testId").toString() == QStringLiteral("mineruCredit")) {
                        credit = label;
                        break;
                    }
                }
            }
            creditMatches = credit &&
                credit->text().contains(QStringLiteral("MinerU</a> 免费提供")) &&
                credit->text().contains(QStringLiteral("鸣谢出品方：上海人工智能实验室"));
            if (dialog)
                dialog->reject();
        });
        if (editMineruSettings(nullptr, MineruConfig{}).has_value() || !creditMatches)
            return 39;

        StudioWindow window;
        if (!window.parseModeCard_ || !window.ruleModeCard_ || !window.smartModeCard_ ||
            !window.smartModeCard_->text().contains(QStringLiteral("智能解析")) ||
            !window.mineruConfigButton_ || !window.mineruConfigSummary_ ||
            !window.mineruConfigSummary_->text().contains(QStringLiteral("准确识别")) ||
            !window.parseStatusChip_ || !window.parseStatusText_ ||
            window.parseStatusText_->text() != QStringLiteral("智能模式") ||
            !window.progressBar_ || !window.progressStatus_ ||
            window.progressStatus_->text() != QStringLiteral("准备中")) {
            qCritical().noquote()
                << "initial UI mismatch: smart="
                << (window.smartModeCard_ ? window.smartModeCard_->text() : QStringLiteral("<null>"))
                << "summary="
                << (window.mineruConfigSummary_ ? window.mineruConfigSummary_->text() : QStringLiteral("<null>"))
                << "mode="
                << (window.parseStatusText_ ? window.parseStatusText_->text() : QStringLiteral("<null>"))
                << "progress="
                << (window.progressStatus_ ? window.progressStatus_->text() : QStringLiteral("<null>"));
            return 19;
        }
        WorkflowProgress detailedProgress;
        detailedProgress.stage = WorkflowStage::Chunking;
        detailedProgress.percent = 68;
        detailedProgress.rulePass = QStringLiteral("答案策略探测");
        detailedProgress.detail = QStringLiteral("测试资料.pdf · 第一套 · 本套第 3 / 10 题");
        detailedProgress.completedSourceBlocks = 1;
        detailedProgress.totalSourceBlocks = 1;
        detailedProgress.questionIndex = 3;
        detailedProgress.questionCount = 10;
        detailedProgress.acceptedQuestions = 2;
        detailedProgress.reviewQuestions = 1;
        window.updateWorkflowProgress(detailedProgress);
        if (window.progressBar_->value() != 68 ||
            !window.progressStatus_->text().contains(QStringLiteral("3/10 题")) ||
            !window.phaseLabel_->text().contains(QStringLiteral("答案策略探测")) ||
            window.generatedCount_->text() != QStringLiteral("2") ||
            window.reviewCount_->text() != QStringLiteral("1"))
            return 41;
        for (auto* button : window.findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("后台等待并关闭")) return 42;
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
        window.pendingGroups_ = {{QStringLiteral("/tmp/question.pdf"), {}, AnswerPolicyHint::Included,
                                  QStringLiteral("/tmp/result.zip"), {}}};
        window.cloudIndex_ = 0;
        window.cloudParsingAnswer_ = false;
        window.persistCloudTask();
        QSettings taskSettings(QSettings::IniFormat, QSettings::UserScope,
                               QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
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
            answerLocation->currentText() != QStringLiteral("自动检测")) return 21;
        if (!window.nextButton_->text().contains(QStringLiteral("开始智能解析")) ||
            window.nextButton_->objectName() != QStringLiteral("primaryButton")) return 26;
#ifdef QUIZPANE_HAS_QT_PDF
        if (!window.sourceModeHint_ ||
            !window.sourceModeHint_->text().contains(QStringLiteral("上传到 MinerU"))) return 26;
#else
        if (window.sourceModeHint_) return 26;
#endif

        window.mineruConfig_.cloudEnabled = false;
        window.mineruConfig_.modeSelectedByUser = true;
        window.updateNavigation();
        if (window.nextButton_->text() != QStringLiteral("开始规则解析  →") ||
            window.parseStatusText_->text() != QStringLiteral("规则模式")) return 28;
#ifdef QUIZPANE_HAS_QT_PDF
        if (!window.sourceModeHint_ ||
            !window.sourceModeHint_->text().contains(QStringLiteral("不会上传"))) return 28;
#endif
        window.mineruConfig_.cloudEnabled = true;
        window.mineruConfig_.modeSelectedByUser = true;
        window.updateNavigation();
        if (window.nextButton_->text() != QStringLiteral("开始智能解析  →") ||
            window.parseStatusText_->text() != QStringLiteral("智能模式")) return 29;
        answerLocation->setCurrentIndex(2);
        if (window.answerPolicyByQuestion_.value(sourcePath, AnswerPolicyHint::Auto) !=
            AnswerPolicyHint::None) return 27;
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

        // 原卷校对图不是正式附件，但智能解析缺少行级坐标时要靠它让用户重新
        // 框选题目范围。此前 reviewOnly 会把“手动修正”入口隐藏，导致无法校正。
        QImage sourcePage(200, 300, QImage::Format_RGB32);
        sourcePage.fill(Qt::white);
        QByteArray sourceBytes;
        QBuffer sourceBuffer(&sourceBytes);
        if (!sourceBuffer.open(QIODevice::WriteOnly) || !sourcePage.save(&sourceBuffer, "PNG")) return 33;
        const QString previewPath = QStringLiteral("assets/review-only-reference.png");
        const QJsonObject preview{{"path", previewPath}, {"alt", QStringLiteral("原卷题目")},
            {"reviewOnly", true}, {"sourceDocument", QStringLiteral("测试资料.pdf")},
            {"sourcePage", 1}, {"autoCrop", QJsonObject{{"x", 0.0}, {"y", 0.0},
                {"width", 1.0}, {"height", 1.0}}}};
        candidate.questions = {question(QStringLiteral("q7"))};
        candidate.needsReviewQuestions = {};
        candidate.reviewSourceImages = {{QStringLiteral("q7"), preview}};
        candidate.reviewAssets = {{previewPath, sourceBytes}};
        window.populateReview(candidate);
        auto* previewItem = window.reviewTree_->topLevelItem(0)->child(0);
        window.reviewTree_->setCurrentItem(previewItem);
        window.showReviewQuestion(previewItem);
        auto* recrop = window.findChild<QPushButton*>(QStringLiteral("reviewActionButton"));
        if (!recrop || recrop->isHidden() || !recrop->isEnabled() ||
            recrop->text() != QStringLiteral("调整原卷区域")) return 34;
        auto* detailScroll = window.findChild<QScrollArea*>(QStringLiteral("reviewDetailScroll"));
        app.processEvents();
        if (!detailScroll || recrop->mapTo(detailScroll->viewport(), QPoint()).x() < 0 ||
            recrop->mapTo(detailScroll->viewport(), QPoint(recrop->width(), 0)).x() >
                detailScroll->viewport()->width()) return 37;
        QImage transparentPage(20, 20, QImage::Format_ARGB32_Premultiplied);
        transparentPage.fill(Qt::transparent);
        transparentPage.setPixelColor(10, 10, Qt::black);
        const QImage flattenedPage = flattenReviewPage(transparentPage);
        if (flattenedPage.hasAlphaChannel() || flattenedPage.pixelColor(0, 0) != QColor(Qt::white) ||
            flattenedPage.pixelColor(10, 10) != QColor(Qt::black)) return 38;
        if (!window.commitReviewCrop(preview, sourcePage, QRectF(0.10, 0.10, 0.50, 0.50)) ||
            window.reviewAssets_.value(previewPath) == sourceBytes ||
            window.reviewSourceImages_.value(QStringLiteral("q7")).value(QStringLiteral("crop"))
                .toObject().value(QStringLiteral("width")).toDouble() != 0.50 ||
            window.generatedAssets_.contains(previewPath)) return 35;

#ifdef QUIZPANE_HAS_QT_PDF
        // 大题本的普通题只携带页码和 bbox；选中题目时才渲染当前校对图，
        // 不得在规则生成阶段把数百张 PNG 一次性塞进 reviewAssets。
        const QString lazyPdfPath = sourceDir.filePath(QStringLiteral("懒加载原卷.pdf"));
        {
            QPdfWriter writer(lazyPdfPath);
            QPainter painter(&writer);
            painter.drawText(QPoint(120, 180), QStringLiteral("1. 懒加载校对图"));
            painter.end();
        }
        window.sourcePaths_.append(lazyPdfPath);
        const QString lazyPath = QStringLiteral("assets/lazy-review-reference.png");
        const QJsonObject lazyPreview{
            {"path", lazyPath}, {"alt", QStringLiteral("原卷题目")},
            {"reviewOnly", true}, {"lazyReview", true},
            {"sourceDocument", QFileInfo(lazyPdfPath).fileName()}, {"sourcePage", 1},
            {"autoCrop", QJsonObject{{"x", 0.0}, {"y", 0.0},
                {"width", 1.0}, {"height", 0.3}}},
            {"reviewSegments", QJsonArray{QJsonObject{
                {"sourcePage", 1}, {"crop", QJsonObject{{"x", 0.0}, {"y", 0.0},
                    {"width", 1.0}, {"height", 0.3}}}}}}};
        candidate.questions = {question(QStringLiteral("q8"))};
        candidate.needsReviewQuestions = {};
        candidate.reviewSourceImages = {{QStringLiteral("q8"), lazyPreview}};
        candidate.reviewAssets = {};
        window.populateReview(candidate);
        auto* lazyItem = window.reviewTree_->topLevelItem(0)->child(0);
        window.showReviewQuestion(lazyItem);
        if (window.reviewAssets_.contains(lazyPath) ||
            window.lazyReviewAssets_.isEmpty() ||
            window.ensureReviewAssetBytes(lazyPreview).isEmpty() ||
            window.reviewVisualPanel_->isHidden()) return 43;
        const QByteArray lazyBytes = window.ensureReviewAssetBytes(lazyPreview);
        window.lazyReviewAssets_.setMaxCost(0);
        if (!window.lazyReviewAssets_.isEmpty() ||
            window.ensureReviewAssetBytes(lazyPreview) != lazyBytes ||
            !window.lazyReviewAssets_.isEmpty()) return 44;
        // Manual edits are not disposable, even if the descriptor still carries
        // lazyReview from its original automatic preview.
        window.reviewAssets_.insert(lazyPath, sourceBytes);
        if (window.ensureReviewAssetBytes(lazyPreview) != sourceBytes) return 45;
        window.reviewAssets_.remove(lazyPath);
        window.lazyReviewAssets_.setMaxCost(16 * 1024);

        ReviewPdfCache pages(24 * 1024);
        QString pageError;
        const QImage firstPage = pages.renderPage(lazyPdfPath, 1, &pageError);
        const QImage repeatedPage = pages.renderPage(lazyPdfPath, 1, &pageError);
        if (firstPage.isNull() || !pageError.isEmpty() ||
            firstPage.cacheKey() != repeatedPage.cacheKey() ||
            pages.cachedKiB() <= 0 || pages.cachedKiB() > 24 * 1024) return 46;
        ReviewPdfCache noPages(0);
        if (noPages.renderPage(lazyPdfPath, 1, &pageError) != firstPage ||
            noPages.cachedKiB() != 0) return 47;
        if (!pages.renderPage(lazyPdfPath, 2, &pageError).isNull() || pageError.isEmpty()) return 48;
        pages.clear();
        if (pages.cachedKiB() != 0) return 49;
        // Invalid sources must never reuse a previously cached PDF page.
        if (!pages.renderPage(sourceDir.filePath("missing.pdf"), 1, &pageError).isNull() ||
            pageError.isEmpty()) return 50;
        if (!window.reviewTree_->updatesEnabled()) return 51;
#endif

        // 开始下一次整理前必须释放上一批完整候选与逐题校对图，避免低内存
        // Windows 在“旧复核结果 + 新任务中间产物”同时驻留时触发分配失败。
        window.generatedAssets_.insert(QStringLiteral("assets/formal.png"), sourceBytes);
        window.pendingCropAsset_ = preview;
        window.pendingCropPage_ = sourcePage;
        window.discardPreviousGenerationForNewTask();
        if (!window.generatedMaterials_.isEmpty() || !window.generatedQuestions_.isEmpty() ||
            !window.reviewQuestions_.isEmpty() || !window.generatedAssets_.isEmpty() ||
            !window.reviewSourceImages_.isEmpty() || !window.reviewAssets_.isEmpty() ||
            !window.lazyReviewAssets_.isEmpty() || window.reviewPdfCache_.cachedKiB() != 0 ||
            !window.pendingCropAsset_.isEmpty() || !window.pendingCropPage_.isNull() ||
            window.currentReviewItem_ || window.currentMaterialItem_ ||
            window.reviewTree_->topLevelItemCount() != 0 ||
            !window.reviewOptionEditors_.isEmpty() || !window.reviewVisualPanel_->isHidden())
            return 40;
        return 0;
    }
};
}  // namespace quizpane::studio

int main(int argc, char** argv) {
    QTemporaryDir settings;
    if (!settings.isValid()) return 36;
    // 题库制作器在测试模式下显式使用这个 Ini 目录，避免 macOS
    // CFPreferences 读写开发机上真实的解析方式和云任务状态。
    qputenv("QUIZPANE_TEST_SETTINGS_DIR", settings.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    return quizpane::studio::StudioWindowReviewTest::run(app);
}
