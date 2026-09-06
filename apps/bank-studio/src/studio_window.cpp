#include "studio_window.hpp"
#include "quizpane/diagnostic_logger.hpp"

#include "quizpane/feedback_report.hpp"
#include "quizpane/bank_validator.hpp"
#include "quizpane/declarative_provider.hpp"
#include "quizpane/provider_installer.hpp"
#include "quizpane/running_app_handoff.hpp"
#include "quizpane/secret_store.hpp"
#include "quizpane/studio/mineru_client.hpp"
#include "quizpane/studio/mineru_output_adapter.hpp"
#include "quizpane/studio/generation_workflow.hpp"
#ifdef QUIZPANE_HAS_QT_PDF
#include "quizpane/studio/qt_pdf_compat.hpp"
#endif
#include "quizpane/zip_archive.hpp"
#include "ui/source_row_widget.hpp"
#include "review/review_draft_bank.hpp"
#include "review/review_image_utils.hpp"
#include "quizpane/studio/pdf_memory_budget.hpp"
#include "review/source_validation.hpp"
#include "ui/styled_dropdown.hpp"

#include <QCloseEvent>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QDropEvent>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMimeData>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QtMath>
#ifdef QUIZPANE_HAS_QT_PDF
#include <QPdfDocument>
#endif
#include <QProgressBar>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QSet>
#include <QSize>
#include <QTimer>
#include <QSettings>
#include <QProcess>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>

#ifndef QUIZPANE_BUILD_VERSION
#define QUIZPANE_BUILD_VERSION "dev"
#endif

namespace quizpane::studio {
namespace {

// 仅供题库制作器页面复用的轻量控件工厂。返回的控件在加入布局后由 Qt 父子
// 对象树托管，调用方不需要手工 delete。
QLabel* mutedLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("muted"));
    label->setWordWrap(true);
    return label;
}

QSettings projectSettings(const QString& application = QStringLiteral("题库制作器")) {
    const QString testDirectory = qEnvironmentVariable("QUIZPANE_TEST_SETTINGS_DIR");
    if (!testDirectory.isEmpty()) {
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testDirectory);
        return QSettings(QSettings::IniFormat, QSettings::UserScope,
                         QStringLiteral("QuizPane Project"), application);
    }
    return QSettings(QStringLiteral("QuizPane Project"), application);
}

QString reviewQuestionTitle(const QJsonObject& question) {
    const auto source = question.value("source").toObject();
    const int number = source.value("questionNumber").toInt();
    QString label = source.value("questionLabel").toString();
    if (label.isEmpty() || label == QString::number(number))
        label = number > 0 ? QStringLiteral("第 %1 题").arg(number) : question.value("id").toString();
    const QString section = source.value("sectionTitle").toString();
    if (!section.isEmpty()) label.prepend(section + QStringLiteral(" · "));
    return label;
}

QString studioColorTheme() {
    QSettings settings = projectSettings();
    const QString value = settings.value(QStringLiteral("ui/colorTheme"),
                                         QStringLiteral("dark")).toString();
    return value == QStringLiteral("light") ? value : QStringLiteral("dark");
}

void storeStudioColorTheme(const QString& value) {
    QSettings settings = projectSettings();
    settings.setValue(QStringLiteral("ui/colorTheme"),
                      value == QStringLiteral("light") ? value : QStringLiteral("dark"));
}

void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget())
            delete widget;
        if (QLayout* nested = item->layout()) {
            clearLayout(nested);
        }
        // item 是 addLayout() 创建的 QLayoutItem，负责销毁其所持有的 nested。
        // 这里不能再 delete nested，否则切换材料清理旧标题栏时会二次释放。
        delete item;
    }
}

bool launchQuizPaneForProvider(const QString& providerEntryPath) {
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
#if defined(Q_OS_MACOS)
    // 正式 macOS 包中制作器位于小窗刷题.app/Contents/Helpers；优先启动同一
    // Bundle 内的主程序，避免 open -a 因同名应用而选到 /Applications 里的旧版。
    candidates << QDir(appDir).absoluteFilePath(
        QStringLiteral("../../../../MacOS/小窗刷题"))
        // 开发构建中两个 .app 是 build/apps 下的兄弟目录。
        << QDir(appDir).absoluteFilePath(
            QStringLiteral("../../../../desktop-qt/小窗刷题.app/Contents/MacOS/小窗刷题"))
        << QStringLiteral("/Applications/小窗刷题.app/Contents/MacOS/小窗刷题");
#elif defined(Q_OS_WIN)
    // Win7 绿色包使用 ASCII 文件名；中文名回退兼容其他 Windows 包。
#if defined(QUIZPANE_WINDOWS7_COMPAT)
    candidates << QDir(appDir).filePath(QStringLiteral("QuizPane.exe"))
               << QDir(appDir).filePath(QStringLiteral("小窗刷题.exe"));
#else
    candidates << QDir(appDir).filePath(QStringLiteral("小窗刷题.exe"))
               << QDir(appDir).filePath(QStringLiteral("QuizPane.exe"));
#endif
#else
    candidates << QDir(appDir).filePath(QStringLiteral("小窗刷题"))
               << QStandardPaths::findExecutable(QStringLiteral("小窗刷题"));
#endif
    for (const QString& candidate : candidates) {
        if (candidate.isEmpty() || !QFileInfo(candidate).isExecutable()) continue;
        if (QProcess::startDetached(candidate, {QStringLiteral("--provider"), providerEntryPath}))
            return true;
    }
    return false;
}

QString materialPreviewHtml(const QString& text, const QJsonArray& underlines) {
    // 材料文本仍然按纯文本保存，避免把展示层 HTML 写入题库；这里只把 OCR 已识别
    // 只接受引擎根据 PDF 原始页面的文字框与水平细线交叉检测得到的字符范围；
    // 不从“【甲】”样式或子题选项猜测。没有可靠检测结果时宁可不加下划线。
    QList<QPair<int, int>> ranges;
    for (const QJsonValue& value : underlines) {
        const QJsonObject range = value.toObject();
        const int start = range.value(QStringLiteral("start")).toInt(-1);
        const int length = range.value(QStringLiteral("length")).toInt();
        if (start >= 0 && length > 0 && start + length <= text.size())
            ranges.append({start, length});
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    QString html;
    int cursor = 0;
    for (const auto& range : ranges) {
        if (range.first < cursor) continue;
        html += text.mid(cursor, range.first - cursor).toHtmlEscaped();
        html += QStringLiteral("<span style=\"text-decoration:underline; text-decoration-thickness:1px;\">%1</span>")
            .arg(text.mid(range.first, range.second).toHtmlEscaped());
        cursor = range.first + range.second;
    }
    html += text.mid(cursor).toHtmlEscaped();
    const QRegularExpression blank(QStringLiteral("(?:〔填空〕|_{2,}|＿{2,})"));
    html.replace(blank, QStringLiteral("<span style=\"text-decoration:underline; letter-spacing:2px;\">&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;</span>"));
    html.replace(QStringLiteral("\n"), QStringLiteral("<br/>"));
    return html;
}

QString stemForReviewEditor(QString stem) {
    // 内部用稳定标记保存无文字横线；校对页只展示原卷形态。四个全角下划线与
    // “〔填空〕”同为 4 个 UTF-16 单元，因而不会破坏已有文字下划线的偏移。
    stem.replace(QStringLiteral("〔填空〕"), QStringLiteral("＿＿＿＿"));
    return stem;
}

QString stemFromReviewEditor(QString stem) {
    stem.replace(QRegularExpression(QStringLiteral("(?:_{2,}|＿{2,})")),
                 QStringLiteral("〔填空〕"));
    return stem;
}

QFrame* metricCard(const QString& name, QLabel** value) {
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("metricCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(4);
    layout->addWidget(mutedLabel(name));
    *value = new QLabel(QStringLiteral("0"));
    (*value)->setObjectName(QStringLiteral("metricValue"));
    layout->addWidget(*value);
    return card;
}

bool confirmAction(QWidget* parent, const QString& title, const QString& text,
                   const QString& acceptText) {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    auto* buttons = new QDialogButtonBox;
    auto* cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    auto* accept = buttons->addButton(acceptText, QDialogButtonBox::AcceptRole);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(accept, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(label); layout->addWidget(buttons);
    dialog.setMinimumWidth(360);
    return dialog.exec() == QDialog::Accepted;
}

QRectF cropRectFromJson(const QJsonObject& value) {
    const QRectF crop(value.value(QStringLiteral("x")).toDouble(),
                      value.value(QStringLiteral("y")).toDouble(),
                      value.value(QStringLiteral("width")).toDouble(),
                      value.value(QStringLiteral("height")).toDouble());
    return crop.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
}

QJsonObject cropRectToJson(const QRectF& crop) {
    return {{QStringLiteral("x"), crop.x()}, {QStringLiteral("y"), crop.y()},
            {QStringLiteral("width"), crop.width()}, {QStringLiteral("height"), crop.height()}};
}

class CropCanvas final : public QWidget {
public:
    CropCanvas(const QImage& image, const QRectF& crop, QWidget* parent = nullptr)
        : QWidget(parent), image_(image), selection_(crop) {
        const QSize limited = image.size().scaled(QSize(980, 680), Qt::KeepAspectRatio);
        setFixedSize(limited.isEmpty() ? QSize(640, 420) : limited);
        setCursor(Qt::CrossCursor);
    }

    QRectF selection() const { return selection_; }
    void setSelection(const QRectF& crop) { selection_ = crop; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        // 即使上游传入了透明页面，也不能让透明区域透出黑色画布，
        // 否则浅色主题会出现用户截图中的黑底黑字。
        painter.fillRect(rect(), QColor(QStringLiteral("#dfe4ea")));
        painter.drawImage(rect(), image_);
        const QRect selectionRect(qRound(selection_.x() * width()),
                                  qRound(selection_.y() * height()),
                                  qRound(selection_.width() * width()),
                                  qRound(selection_.height() * height()));
        painter.setBrush(QColor(255, 77, 133, 30));
        painter.setPen(QPen(QColor(QStringLiteral("#ff4d85")), 2));
        painter.drawRect(selectionRect);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton)
            return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        dragStart_ = normalized(event->position());
#else
        dragStart_ = normalized(event->localPos());
#endif
        selection_ = QRectF(dragStart_, QSizeF());
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton))
            return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPointF position = event->position();
#else
        const QPointF position = event->localPos();
#endif
        selection_ = QRectF(dragStart_, normalized(position)).normalized()
            .intersected(QRectF(0.0, 0.0, 1.0, 1.0));
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton)
            mouseMoveEvent(event);
    }

private:
    QPointF normalized(const QPointF& position) const {
        return {qBound(0.0, position.x() / qMax(1, width()), 1.0),
                qBound(0.0, position.y() / qMax(1, height()), 1.0)};
    }

    QImage image_;
    QRectF selection_;
    QPointF dragStart_;
};

class CropDialog final : public QDialog {
public:
    CropDialog(const QImage& page, const QRectF& pageContext, const QRectF& automaticCrop,
               QWidget* parent = nullptr)
        : QDialog(parent), pageContext_(pageContext), automaticCrop_(automaticCrop) {
        setWindowTitle(QStringLiteral("从原卷重新裁切"));
        auto* layout = new QVBoxLayout(this);
        auto* description = mutedLabel(QStringLiteral(
            "这里仅显示当前定位附近的原卷区域，粉色框是当前裁切框。拖拽重新框选要保留的区域；"
            "不会上传原卷或图片。"));
        layout->addWidget(description);
        const QRect contextPixels(qFloor(pageContext.x() * page.width()),
                                  qFloor(pageContext.y() * page.height()),
                                  qCeil(pageContext.width() * page.width()),
                                  qCeil(pageContext.height() * page.height()));
        const QImage contextImage = page.copy(contextPixels.intersected(page.rect()));
        canvas_ = new CropCanvas(contextImage, toLocal(automaticCrop), this);
        layout->addWidget(canvas_, 0, Qt::AlignCenter);
        auto* buttons = new QDialogButtonBox;
        auto* reset = buttons->addButton(QStringLiteral("恢复当前定位"), QDialogButtonBox::ResetRole);
        auto* cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
        auto* useCrop = buttons->addButton(QStringLiteral("使用此裁切"), QDialogButtonBox::AcceptRole);
        QObject::connect(reset, &QPushButton::clicked, this,
                         [this] { canvas_->setSelection(toLocal(automaticCrop_)); });
        QObject::connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        QObject::connect(useCrop, &QPushButton::clicked, this, [this] {
            if (canvas_->selection().width() < 0.01 || canvas_->selection().height() < 0.01) {
                QMessageBox::warning(this, QStringLiteral("裁切范围过小"),
                    QStringLiteral("请拖拽出一个足够大的矩形区域。"));
                return;
            }
            QDialog::accept();
        });
        layout->addWidget(buttons);
    }

    QRectF selection() const { return toPage(canvas_->selection()); }
    void setSelection(const QRectF& crop) { canvas_->setSelection(toLocal(crop)); }

private:
    QRectF toLocal(const QRectF& crop) const {
        return {(crop.x() - pageContext_.x()) / pageContext_.width(),
                (crop.y() - pageContext_.y()) / pageContext_.height(),
                crop.width() / pageContext_.width(), crop.height() / pageContext_.height()};
    }
    QRectF toPage(const QRectF& crop) const {
        return {pageContext_.x() + crop.x() * pageContext_.width(),
                pageContext_.y() + crop.y() * pageContext_.height(),
                crop.width() * pageContext_.width(), crop.height() * pageContext_.height()};
    }
    CropCanvas* canvas_ = nullptr;
    QRectF pageContext_;
    QRectF automaticCrop_;
};

QRectF cropContextAround(const QRectF& crop) {
    const qreal horizontalPadding = qMax<qreal>(0.08, crop.width() * 0.85);
    const qreal verticalPadding = qMax<qreal>(0.10, crop.height() * 0.85);
    return QRectF(crop.x() - horizontalPadding, crop.y() - verticalPadding,
                  crop.width() + horizontalPadding * 2.0,
                  crop.height() + verticalPadding * 2.0)
        .intersected(QRectF(0.0, 0.0, 1.0, 1.0));
}

QImage cropNormalizedImage(const QImage& page, const QRectF& normalizedCrop) {
    const QRect pixels(qFloor(normalizedCrop.x() * page.width()),
                       qFloor(normalizedCrop.y() * page.height()),
                       qMax(1, qCeil(normalizedCrop.width() * page.width())),
                       qMax(1, qCeil(normalizedCrop.height() * page.height())));
    return page.copy(pixels.intersected(page.rect()));
}


QString loadMineruToken() {
    size_t size = 0;
    if (quizpane::SecretStore::read(QStringLiteral("question-maker"),
                                    QByteArrayLiteral("mineru-token"), nullptr, &size) != 0 ||
        size == 0)
        return {};
    QByteArray bytes(static_cast<qsizetype>(size), '\0');
    if (quizpane::SecretStore::read(QStringLiteral("question-maker"),
                                    QByteArrayLiteral("mineru-token"),
                                    reinterpret_cast<uint8_t*>(bytes.data()), &size) != 0)
        return {};
    bytes.truncate(static_cast<qsizetype>(size));
    return QString::fromUtf8(bytes);
}

MineruConfig loadStoredMineruConfig() {
    QSettings settings = projectSettings();
    settings.beginGroup(QStringLiteral("question-maker/mineru"));
    MineruConfig result;
    result.modelVersion =
        settings.value(QStringLiteral("modelVersion"), result.modelVersion).toString();
    result.isOcr = settings.value(QStringLiteral("isOcr"), result.isOcr).toBool();
    result.modeSelectedByUser =
        settings.value(QStringLiteral("modeSelectedByUser"), false).toBool();
    // v0.5.6 之前 cloudEnabled=false 既可能是默认值，也可能来自一次空 Token 保存，
    // 无法代表用户的明确选择。没有新标记的旧配置统一迁移到智能模式默认值。
    if (result.modeSelectedByUser) {
        result.cloudEnabled =
            settings.value(QStringLiteral("cloudEnabled"), result.cloudEnabled).toBool();
    }
    settings.endGroup();
    // 不在启动时读取钥匙串。macOS 对从 DMG 直接运行、或尚未用稳定 Developer ID
    // 签名的 App 可能每次读取都要求授权；只有用户实际使用云解析或打开设置时
    // 才按需访问 Token，避免每次打开题库制作器都弹系统密码。
    return result;
}

bool storeMineruConfig(const MineruConfig& value, QString* error) {
    QSettings settings = projectSettings();
    settings.beginGroup(QStringLiteral("question-maker/mineru"));
    settings.setValue(QStringLiteral("modelVersion"), value.modelVersion);
    settings.setValue(QStringLiteral("isOcr"), value.isOcr);
    settings.setValue(QStringLiteral("cloudEnabled"), value.cloudEnabled);
    settings.setValue(QStringLiteral("modeSelectedByUser"), value.modeSelectedByUser);
    settings.endGroup();
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (error) *error = QStringLiteral("无法保存云解析的非敏感设置");
        return false;
    }
    const QByteArray token = value.token.toUtf8();
    // Token 为空表示用户主动清除，删除凭据而不是写入空串。
    const int status = token.isEmpty()
        ? quizpane::SecretStore::remove(QStringLiteral("question-maker"),
                                        QByteArrayLiteral("mineru-token"))
        : quizpane::SecretStore::write(QStringLiteral("question-maker"),
              QByteArrayLiteral("mineru-token"),
              reinterpret_cast<const uint8_t*>(token.constData()),
              static_cast<size_t>(token.size()));
    // 某些极简 Linux 环境没有 Secret Service/libsecret。仍保存非敏感配置并允许
    // 当前会话继续使用 Token，但绝不把凭据退化写入 QSettings 明文。
    if (status == 4) {
        if (error) *error = QStringLiteral(
            "当前系统没有可用的安全凭据服务；访问凭据仅在本次运行中保留，"
            "下次启动需要重新输入。");
        return true;
    }
    // remove 在凭据本来不存在时返回 1（不存在），这不是失败。
    if (status != 0 && !(token.isEmpty() && status == 1)) {
        if (error) *error = QStringLiteral("无法写入系统凭据库（错误码 %1）").arg(status);
        return false;
    }
    return true;
}

// QSettings 使用各平台原生的用户配置目录；本项目已在 Win7/Win10/macOS 上用它
// 保存主题与非敏感配置。云任务数量很小，不需要为了这一点状态额外带 SQLite。
struct PersistedCloudTask {
    QString sessionId;
    QString cacheDir;
    QString batchId;
    QList<SourceMaterialGroup> groups;
    int sourceIndex = 0;
    bool parsingAnswer = false;
};

constexpr auto kCloudTaskSettingsGroup = "question-maker/mineru/pending-cloud-task";

std::optional<PersistedCloudTask> loadPersistedCloudTask() {
    QSettings settings = projectSettings();
    settings.beginGroup(QString::fromLatin1(kCloudTaskSettingsGroup));
    PersistedCloudTask task;
    task.sessionId = settings.value(QStringLiteral("sessionId")).toString();
    task.cacheDir = settings.value(QStringLiteral("cacheDir")).toString();
    task.batchId = settings.value(QStringLiteral("batchId")).toString();
    task.sourceIndex = settings.value(QStringLiteral("sourceIndex"), 0).toInt();
    task.parsingAnswer = settings.value(QStringLiteral("parsingAnswer"), false).toBool();
    const int count = settings.beginReadArray(QStringLiteral("groups"));
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        SourceMaterialGroup group;
        group.questionPath = settings.value(QStringLiteral("questionPath")).toString();
        group.answerPath = settings.value(QStringLiteral("answerPath")).toString();
        if (settings.contains(QStringLiteral("answerPolicy"))) {
            group.answerPolicy = static_cast<AnswerPolicyHint>(
                settings.value(QStringLiteral("answerPolicy")).toInt());
        } else {
            // 兼容升级前尚未完成的云任务。
            group.answerPolicy = settings.value(QStringLiteral("hasAnswerKey"), true).toBool()
                ? AnswerPolicyHint::Included : AnswerPolicyHint::None;
        }
        group.mineruZipPath = settings.value(QStringLiteral("mineruZipPath")).toString();
        group.mineruAnswerZipPath =
            settings.value(QStringLiteral("mineruAnswerZipPath")).toString();
        if (!group.questionPath.isEmpty())
            task.groups.append(group);
    }
    settings.endArray();
    settings.endGroup();
    if (task.sessionId.isEmpty() || task.cacheDir.isEmpty() || task.groups.isEmpty() ||
        task.sourceIndex < 0 || task.sourceIndex >= task.groups.size())
        return std::nullopt;
    return task;
}

void savePersistedCloudTask(const PersistedCloudTask& task) {
    QSettings settings = projectSettings();
    settings.remove(QString::fromLatin1(kCloudTaskSettingsGroup));
    settings.beginGroup(QString::fromLatin1(kCloudTaskSettingsGroup));
    settings.setValue(QStringLiteral("sessionId"), task.sessionId);
    settings.setValue(QStringLiteral("cacheDir"), task.cacheDir);
    settings.setValue(QStringLiteral("batchId"), task.batchId);
    settings.setValue(QStringLiteral("sourceIndex"), task.sourceIndex);
    settings.setValue(QStringLiteral("parsingAnswer"), task.parsingAnswer);
    settings.beginWriteArray(QStringLiteral("groups"), task.groups.size());
    for (int index = 0; index < task.groups.size(); ++index) {
        settings.setArrayIndex(index);
        const SourceMaterialGroup& group = task.groups.at(index);
        settings.setValue(QStringLiteral("questionPath"), group.questionPath);
        settings.setValue(QStringLiteral("answerPath"), group.answerPath);
        settings.setValue(QStringLiteral("answerPolicy"), static_cast<int>(group.answerPolicy));
        settings.setValue(QStringLiteral("mineruZipPath"), group.mineruZipPath);
        settings.setValue(QStringLiteral("mineruAnswerZipPath"), group.mineruAnswerZipPath);
    }
    settings.endArray();
    settings.endGroup();
    settings.sync();
}

void removePersistedCloudTask(const QString& cacheDir, bool removeCachedResults) {
    QSettings settings = projectSettings();
    settings.remove(QString::fromLatin1(kCloudTaskSettingsGroup));
    settings.sync();
    if (!removeCachedResults || cacheDir.isEmpty())
        return;
    const QString root = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("mineru-tasks"));
    const QString normalizedRoot = QDir::cleanPath(root) + QDir::separator();
    const QString normalizedCache = QDir::cleanPath(cacheDir);
    // 只清本应用预期目录，防止损坏配置导致递归删除任意用户路径。
    if (normalizedCache.startsWith(normalizedRoot))
        QDir(normalizedCache).removeRecursively();
}

}  // namespace

// ===== 应用外壳与四步向导装配 =====

StudioWindow::StudioWindow(QWidget* parent) : QMainWindow(parent) {
#if defined(Q_OS_WIN)
    // Windows 会把 QApplication 的 display name 追加到窗口标题。这里仅保留产品名，
    // 避免出现“题库制作器 · 小窗刷题 - 题库制作器”的重复标题。
    setWindowTitle(QStringLiteral("小窗刷题"));
#else
    setWindowTitle(QStringLiteral("题库制作器 · 小窗刷题"));
#endif
    setMinimumSize(820, 600);
    resize(1040, 720);
    setAcceptDrops(true);

    auto* root = new QWidget;
    setCentralWidget(root);
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* sidebar = new QFrame;
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(224);
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(22, 24, 18, 20);
    sideLayout->setSpacing(9);
    auto* brand = new QLabel(QStringLiteral("题库制作器"));
    brand->setObjectName(QStringLiteral("brand"));
    sideLayout->addWidget(brand);
    sideLayout->addWidget(mutedLabel(QStringLiteral("把你的文档整理成可安装题库")));
    sideLayout->addSpacing(22);
    const QStringList steps{QStringLiteral("01  选择资料"), QStringLiteral("02  自动整理"),
        QStringLiteral("03  检查问题"), QStringLiteral("04  完成")};
    for (int index = 0; index < steps.size(); ++index) {
        auto* step = new QLabel(steps.at(index));
        step->setObjectName(QStringLiteral("sideStep"));
        step->setProperty("stepIndex", index);
        sideLayout->addWidget(step);
    }
    sideLayout->addStretch();
    parseStatusChip_ = new QFrame;
    parseStatusChip_->setObjectName(QStringLiteral("parseStatusChip"));
    auto* parseStatusLayout = new QHBoxLayout(parseStatusChip_);
    parseStatusLayout->setContentsMargins(12, 9, 12, 9);
    parseStatusLayout->setSpacing(7);
    auto* parseStatusLight = new QLabel(QStringLiteral("●"));
    parseStatusLight->setObjectName(QStringLiteral("parseStatusLight"));
    parseStatusText_ = new QLabel;
    parseStatusText_->setObjectName(QStringLiteral("parseStatusText"));
    parseStatusLayout->addWidget(parseStatusLight);
    parseStatusLayout->addWidget(parseStatusText_);
    parseStatusLayout->addStretch();
    sideLayout->addWidget(parseStatusChip_);
    sideLayout->addSpacing(8);
    auto* support = new QFrame;
    support->setObjectName(QStringLiteral("sidebarSupport"));
    auto* supportLayout = new QVBoxLayout(support);
    supportLayout->setContentsMargins(10, 9, 10, 9);
    supportLayout->setSpacing(2);
    auto* supportButton = new QPushButton(QStringLiteral("赞赏支持  ›"));
    supportButton->setObjectName(QStringLiteral("sidebarSupportButton"));
    supportButton->setCursor(Qt::PointingHandCursor);
    supportLayout->addWidget(supportButton);
    auto* supportHint = new QLabel(QStringLiteral("您的支持是继续更新的动力"));
    supportHint->setObjectName(QStringLiteral("sidebarSupportHint"));
    supportHint->setWordWrap(true);
    supportLayout->addWidget(supportHint);
    connect(supportButton, &QPushButton::clicked, this, &StudioWindow::showDonationDialog);
    sideLayout->addWidget(support);
    rootLayout->addWidget(sidebar);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("content"));
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(34, 26, 34, 24);
    contentLayout->setSpacing(18);
    pages_ = new QStackedWidget;
    reviewController_.init(this, sourcePaths_, [this] { updateNavigation(); });
    pages_->addWidget(buildSourcePage());
    pages_->addWidget(buildProgressPage());
    pages_->addWidget(reviewController_.buildPage());
    pages_->addWidget(buildFinishPage());
    contentLayout->addWidget(pages_, 1);

    auto* navigation = new QHBoxLayout;
    backButton_ = new QPushButton(QStringLiteral("上一步"));
    backButton_->setObjectName(QStringLiteral("secondaryButton"));
    nextButton_ = new QPushButton(QStringLiteral("下一步"));
    startButton_ = new QPushButton(QStringLiteral("开始整理"));
    startButton_->setObjectName(QStringLiteral("primaryButton"));
    navigation->addWidget(backButton_);
    navigation->addStretch();
    navigation->addWidget(nextButton_);
    navigation->addWidget(startButton_);
    contentLayout->addLayout(navigation);
    rootLayout->addWidget(content, 1);

    connect(backButton_, &QPushButton::clicked, this, &StudioWindow::handleBackNavigation);
    connect(nextButton_, &QPushButton::clicked, this, [this] {
        if (pages_->currentIndex() == 0)
            startFromSources();
        else
            movePage(1);
    });
    connect(startButton_, &QPushButton::clicked, this, &StudioWindow::beginPreflight);
    connect(pages_, &QStackedWidget::currentChanged, this, &StudioWindow::updateNavigation);
    mineruConfig_ = loadStoredMineruConfig();
    updateNavigation();
    networkManager_ = new QNetworkAccessManager(this);
    auto* settingsMenu = menuBar()->addMenu(QStringLiteral("设置"));
#ifdef QUIZPANE_HAS_QT_PDF
    settingsMenu->addAction(QStringLiteral("解析方式…"), this,
                            &StudioWindow::editParseModeSettings);
#endif
    auto* appearanceMenu = settingsMenu->addMenu(QStringLiteral("外观"));
    auto* themeActions = new QActionGroup(appearanceMenu);
    themeActions->setExclusive(true);
    auto* darkThemeAction = appearanceMenu->addAction(QStringLiteral("深色模式"));
    auto* lightThemeAction = appearanceMenu->addAction(QStringLiteral("浅色模式"));
    darkThemeAction->setCheckable(true);
    lightThemeAction->setCheckable(true);
    themeActions->addAction(darkThemeAction);
    themeActions->addAction(lightThemeAction);
    const bool lightTheme = studioColorTheme() == QStringLiteral("light");
    lightThemeAction->setChecked(lightTheme);
    darkThemeAction->setChecked(!lightTheme);
    connect(darkThemeAction, &QAction::triggered, this, [this] {
        storeStudioColorTheme(QStringLiteral("dark"));
        applyStyle();
    });
    connect(lightThemeAction, &QAction::triggered, this, [this] {
        storeStudioColorTheme(QStringLiteral("light"));
        applyStyle();
    });
#ifdef QUIZPANE_DIAGNOSTIC_LOGGING
    settingsMenu->addAction(QStringLiteral("查看调试日志…"), this, [] {
        diagnostic::openLogFile();
    });
#endif
    settingsMenu->addAction(QStringLiteral("问题反馈…"), this,
                            &StudioWindow::showFeedbackDialog);
    {
        auto* diagnosticsAction = settingsMenu->addAction(QStringLiteral("记录诊断日志"));
        diagnosticsAction->setCheckable(true);
        diagnosticsAction->setChecked(diagnostic::isDiagnosticsEnabled());
        connect(diagnosticsAction, &QAction::toggled, this,
                [](bool enabled) { diagnostic::setDiagnosticsEnabled(enabled); });
    }
    auto* helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
    helpMenu->addAction(QStringLiteral("赞赏支持…"), this, &StudioWindow::showDonationDialog);
    auto* buildVersion = helpMenu->addAction(
        QStringLiteral("构建版本：%1").arg(QStringLiteral(QUIZPANE_BUILD_VERSION)));
    buildVersion->setObjectName(QStringLiteral("studioBuildVersionAction"));
    buildVersion->setEnabled(false);
    auto* about = helpMenu->addAction(QStringLiteral("关于题库制作器"), this, [this] {
        QMessageBox::about(this, QStringLiteral("关于题库制作器"),
            QStringLiteral("题库制作器\n版本 %1\n构建版本 %2")
                .arg(QApplication::applicationVersion(), QStringLiteral(QUIZPANE_BUILD_VERSION)));
    });
    about->setObjectName(QStringLiteral("studioAboutAction"));
    applyStyle();
    updateNavigation();
    // UI 建好后再询问，避免启动阶段抢在主窗口出现前弹对话框。
    QTimer::singleShot(0, this, &StudioWindow::offerCloudTaskResume);
}

void StudioWindow::persistCloudTask() {
    if (cloudSessionId_.isEmpty() || cloudCacheDir_.isEmpty() || pendingGroups_.isEmpty())
        return;
    savePersistedCloudTask({cloudSessionId_, cloudCacheDir_, cloudBatchId_, pendingGroups_,
                            cloudIndex_, cloudParsingAnswer_});
}

void StudioWindow::clearPersistedCloudTask(bool removeCachedResults) {
    removePersistedCloudTask(cloudCacheDir_, removeCachedResults);
    cloudSessionId_.clear();
    cloudCacheDir_.clear();
    cloudBatchId_.clear();
}

void StudioWindow::offerCloudTaskResume() {
    const auto saved = loadPersistedCloudTask();
    if (!saved)
        return;

    QMessageBox choice(this);
    choice.setWindowTitle(QStringLiteral("发现未完成的智能解析"));
    choice.setIcon(QMessageBox::Information);
    choice.setText(QStringLiteral("上次提交的云端任务仍可继续等待。"));
    choice.setInformativeText(QStringLiteral(
        "继续等待不会重复上传文件；云端任务会从上次进度继续。\n"
        "若不再等待，只清除本机记录和缓存，云端已提交任务可能仍会自行完成。"));
    auto* resume = choice.addButton(QStringLiteral("继续等待"), QMessageBox::AcceptRole);
    auto* discard = choice.addButton(QStringLiteral("不再等待"), QMessageBox::DestructiveRole);
    choice.exec();
    if (choice.clickedButton() == discard) {
        removePersistedCloudTask(saved->cacheDir, true);
        return;
    }
    if (choice.clickedButton() != resume)
        return;
    if (loadMineruToken().trimmed().isEmpty()) {
        if (!editMineruSettings(QStringLiteral("继续等待上次的云端任务，需要先配置 MinerU Token。")))
            return;
    }
    cloudSessionId_ = saved->sessionId;
    cloudCacheDir_ = saved->cacheDir;
    cloudBatchId_ = saved->batchId;
    pendingGroups_ = saved->groups;
    cloudIndex_ = saved->sourceIndex;
    cloudParsingAnswer_ = saved->parsingAnswer;
    // 本机缓存被系统清理时，重新下载即可；不能把不存在的 ZIP 交给规则工作流。
    for (SourceMaterialGroup& group : pendingGroups_) {
        if (!group.mineruZipPath.isEmpty() && !QFileInfo::exists(group.mineruZipPath))
            group.mineruZipPath.clear();
        if (!group.mineruAnswerZipPath.isEmpty() && !QFileInfo::exists(group.mineruAnswerZipPath))
            group.mineruAnswerZipPath.clear();
    }
    QDir().mkpath(cloudCacheDir_);
    pages_->setCurrentIndex(1);
    phaseLabel_->setText(QStringLiteral("恢复云端解析"));
    phaseDetail_->setText(QStringLiteral("正在连接上次提交的任务。"));
    activitySpinner_->show();
    activityTimer_->start(120);
    processNextCloudSource();
}

void StudioWindow::updateParseModeSummary() {
    const bool cloud = mineruConfig_.cloudEnabled;
    // 云端解析只对 PDF/图片生效。本次没有这类资料时仍是纯本地流程，不该让用户
    // 以为文档会被上传。
    const bool cloudThisRun = cloud && shouldUseCloudParse();

    if (parseModeCard_) {
        parseModeCard_->setProperty("mode", cloud ? QStringLiteral("cloud")
                                                   : QStringLiteral("local"));
        for (QPushButton* card : {ruleModeCard_, smartModeCard_}) {
            if (!card) continue;
            const bool active = (card == smartModeCard_) == cloud;
            card->setProperty("active", active);
            card->style()->unpolish(card);
            card->style()->polish(card);
        }
    }
    if (parseStatusChip_ && parseStatusText_) {
        parseStatusChip_->setProperty("mode", cloud ? QStringLiteral("cloud")
                                                      : QStringLiteral("local"));
        parseStatusText_->setText(cloud ? QStringLiteral("智能模式")
                                         : QStringLiteral("规则模式"));
        parseStatusChip_->style()->unpolish(parseStatusChip_);
        parseStatusChip_->style()->polish(parseStatusChip_);
    }
    if (parseModeSummary_) {
        parseModeSummary_->setText(cloudThisRun
            ? QStringLiteral("当前方式：智能解析 · PDF 会上传处理，其余资料仍在本机整理")
            : (cloud ? QStringLiteral("本次资料无需智能增强，会按规则在本机整理")
                     : QStringLiteral("规则解析 · 资料不会离开这台电脑")));
    }
    if (sourceModeHint_) {
        sourceModeHint_->setText(cloud
            ? QStringLiteral("PDF 将上传到 MinerU；TXT、Markdown 和 DOCX 仍在本机整理。")
            : QStringLiteral("所有资料都在本机整理，不会上传。扫描版 PDF 处理时间可能较长。"));
    }
    updateMineruConfigSummary();
}

void StudioWindow::updateMineruConfigSummary() {
    if (!mineruConfigSummary_ || !mineruConfigButton_)
        return;
    const QString model = mineruConfig_.modelVersion == QStringLiteral("pipeline")
        ? QStringLiteral("兼容识别") : QStringLiteral("准确识别（推荐）");
    QSettings settings = projectSettings();
    const int submitted = settings.value(
        QStringLiteral("question-maker/mineru/usage/%1/submittedFiles")
            .arg(QDate::currentDate().toString(Qt::ISODate)), 0).toInt();
    mineruConfigSummary_->setText(mineruConfig_.cloudEnabled
        ? QStringLiteral("已配置 · %1 · 今日提交 %2 个文件").arg(model).arg(submitted)
        : QStringLiteral("智能解析未启用"));
    mineruConfigButton_->setVisible(mineruConfig_.cloudEnabled);
}

void StudioWindow::selectParseMode(bool cloud) {
    // 智能解析需要 Token。没有凭据时，直接带用户去配置，而不是把看似选中的
    // 智能模式又悄悄切回规则模式。
    if (cloud && loadMineruToken().trimmed().isEmpty()) {
        editMineruSettings();
        return;
    }
    if (cloud == mineruConfig_.cloudEnabled && mineruConfig_.modeSelectedByUser)
        return;
    mineruConfig_.cloudEnabled = cloud;
    mineruConfig_.modeSelectedByUser = true;
    MineruConfig persisted = mineruConfig_;
    persisted.token = loadMineruToken();
    QString error;
    storeMineruConfig(persisted, &error);
    updateNavigation();
}

// 菜单里的备用入口；首屏卡片用于快速切换，这里保留两个方式的完整说明。
void StudioWindow::editParseModeSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("解析方式"));
    dialog.setMinimumWidth(520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(QStringLiteral("解析方式"));
    title->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(title);

    auto* localRadio = new QRadioButton(QStringLiteral("规则解析（本机）"));
    auto* localHint = mutedLabel(QStringLiteral(
        "适合文字清晰、题号和选项规整的文档；资料全程留在这台电脑。"));
    auto* cloudRadio = new QRadioButton(QStringLiteral("智能解析（推荐）"));
    auto* cloudHint = mutedLabel(QStringLiteral(
        "扫描件、统计图表、图形选项等复杂版面交给云端识别版面，识别率更高；"
        "所选 PDF 会上传到 MinerU 处理，首次使用需要填写 Token。"));
    localRadio->setChecked(!mineruConfig_.cloudEnabled);
    cloudRadio->setChecked(mineruConfig_.cloudEnabled);
    layout->addWidget(localRadio);
    layout->addWidget(localHint);
    layout->addSpacing(10);
    layout->addWidget(cloudRadio);
    layout->addWidget(cloudHint);

    auto* configureButton = new QPushButton(QStringLiteral("云端解析配置…"));
    configureButton->setObjectName(QStringLiteral("secondaryButton"));
    layout->addWidget(configureButton, 0, Qt::AlignLeft);
    QObject::connect(configureButton, &QPushButton::clicked, &dialog, [this, cloudRadio] {
        editMineruSettings();
        // 配置页里可能刚填好凭据，回来时同步选中状态。
        cloudRadio->setEnabled(true);
    });

    auto* buttons = new QDialogButtonBox;
    auto* cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    auto* confirm = buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
    cancel->setObjectName(QStringLiteral("dialogCancelButton"));
    confirm->setObjectName(QStringLiteral("primaryButton"));
    const auto updateConfirmText = [cloudRadio, confirm] {
        confirm->setText(cloudRadio->isChecked() ? QStringLiteral("启用智能解析")
                                                  : QStringLiteral("使用规则解析"));
    };
    updateConfirmText();
    QObject::connect(cloudRadio, &QRadioButton::toggled, &dialog, updateConfirmText);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const bool wantCloud = cloudRadio->isChecked();
    if (wantCloud && loadMineruToken().trimmed().isEmpty()) {
        // 没有凭据时不能假装已启用，否则整理时才失败会更让人困惑。
        const auto choice = QMessageBox::question(this, QStringLiteral("还需要配置访问凭据"),
            QStringLiteral("智能解析需要先填写 MinerU Token。现在去配置吗？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (choice == QMessageBox::Yes)
            editMineruSettings();
        return;
    }
    if (wantCloud == mineruConfig_.cloudEnabled && mineruConfig_.modeSelectedByUser)
        return;
    mineruConfig_.cloudEnabled = wantCloud;
    mineruConfig_.modeSelectedByUser = true;
    MineruConfig persisted = mineruConfig_;
    persisted.token = loadMineruToken();
    QString error;
    storeMineruConfig(persisted, &error);
    updateNavigation();
}

bool StudioWindow::editMineruSettings(const QString& notice) {
    MineruConfig configForEditor = mineruConfig_;
    configForEditor.token = loadMineruToken();
    const std::optional<MineruConfig> updated =
        quizpane::studio::editMineruSettings(this, configForEditor, notice);
    if (!updated)
        return false;
    QString error;
    if (!storeMineruConfig(*updated, &error)) {
        QMessageBox::warning(this, QStringLiteral("云端解析配置未保存"), error);
        return false;
    }
    mineruConfig_ = *updated;
    // Token 只保留在钥匙串里；成员只留非敏感配置，避免它随窗口对象长期驻留内存。
    mineruConfig_.token.clear();
    updateNavigation();
    return true;
}

void StudioWindow::showDonationDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("赞赏支持"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("<h2 align='center'>请作者喝杯咖啡</h2>"));
    auto* description = new QLabel(QStringLiteral(
        "一个人慢慢把小窗刷题做好并不容易。您的支持，是我继续下去的最大动力。"));
    description->setAlignment(Qt::AlignCenter);
    description->setWordWrap(true);
    auto* code = new QLabel;
    code->setAlignment(Qt::AlignCenter);
    auto* caption = new QLabel;
    caption->setObjectName(QStringLiteral("muted"));
    caption->setAlignment(Qt::AlignCenter);
    auto* paymentRow = new QHBoxLayout;
    auto* previous = new QPushButton(QStringLiteral("‹"));
    auto* wechat = new QPushButton(QStringLiteral("微信支付"));
    auto* alipay = new QPushButton(QStringLiteral("支付宝"));
    auto* next = new QPushButton(QStringLiteral("›"));
    wechat->setCheckable(true);
    alipay->setCheckable(true);
    paymentRow->addStretch();
    paymentRow->addWidget(previous);
    paymentRow->addWidget(wechat);
    paymentRow->addWidget(alipay);
    paymentRow->addWidget(next);
    paymentRow->addStretch();
    bool showingAlipay = false;
    const auto updatePayment = [&] {
        const QString resource = showingAlipay
            ? QStringLiteral(":/icons/alipay-payment.jpg")
            : QStringLiteral(":/icons/wechat-payment.jpg");
        code->setPixmap(QPixmap(resource).scaled(
            220, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        caption->setText(showingAlipay ? QStringLiteral("支付宝扫码赞赏")
                                       : QStringLiteral("微信扫码赞赏"));
        wechat->setChecked(!showingAlipay);
        alipay->setChecked(showingAlipay);
    };
    connect(previous, &QPushButton::clicked, &dialog, [&] {
        showingAlipay = !showingAlipay; updatePayment();
    });
    connect(next, &QPushButton::clicked, &dialog, [&] {
        showingAlipay = !showingAlipay; updatePayment();
    });
    connect(wechat, &QPushButton::clicked, &dialog, [&] {
        showingAlipay = false; updatePayment();
    });
    connect(alipay, &QPushButton::clicked, &dialog, [&] {
        showingAlipay = true; updatePayment();
    });
    updatePayment();
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(title);
    layout->addWidget(description);
    layout->addWidget(code);
    layout->addWidget(caption);
    layout->addLayout(paymentRow);
    layout->addWidget(close);
    dialog.setFixedWidth(380);
    dialog.exec();
}

void StudioWindow::showFeedbackDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("问题反馈"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 18);
    layout->setSpacing(10);
    auto* hint = new QLabel(QStringLiteral(
        "请尽量描述复现步骤和实际/预期表现。发送会附带运行环境信息；"
        "勾选的日志与崩溃信息已做脱敏（不含账号、题目与完整路径），"
        "将上传到 xutianyou.cc 供排查。没有网络时，也可导出诊断包后转交。"));
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("muted"));
    auto* editor = new QPlainTextEdit;
    editor->setPlaceholderText(QStringLiteral("出了什么问题？怎么触发的？"));
    editor->setFixedHeight(140);
    auto* logsCheck = new QCheckBox(QStringLiteral("附上最近的运行日志（已脱敏）"));
    logsCheck->setChecked(true);
    auto* crashCheck = new QCheckBox(QStringLiteral("附上崩溃信息（如有，最近 24 小时内）"));
    crashCheck->setChecked(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* send = buttons->addButton(QStringLiteral("发送"), QDialogButtonBox::AcceptRole);
    auto* exportBundle = buttons->addButton(QStringLiteral("导出诊断包…"),
                                             QDialogButtonBox::ActionRole);
    send->setEnabled(false);
    exportBundle->setEnabled(false);
    connect(editor, &QPlainTextEdit::textChanged, send,
            [editor, send, exportBundle] {
                const bool hasDescription = !editor->toPlainText().trimmed().isEmpty();
                send->setEnabled(hasDescription);
                exportBundle->setEnabled(hasDescription);
            });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(exportBundle, &QPushButton::clicked,
            [&dialog, logsCheck, crashCheck, editor] {
                const QString suggested = QDir(QStandardPaths::writableLocation(
                    QStandardPaths::DocumentsLocation)).filePath(
                        QStringLiteral("quizpane-feedback.json"));
                const QString path = QFileDialog::getSaveFileName(
                    &dialog, QStringLiteral("导出诊断包"), suggested,
                    QStringLiteral("QuizPane 诊断包 (*.json)"));
                if (path.isEmpty())
                    return;
                feedback::ReportOptions options;
                options.description = editor->toPlainText();
                options.includeLogs = logsCheck->isChecked();
                options.includeCrash = crashCheck->isChecked();
                const auto result = feedback::exportReport(options, path);
                if (result.success)
                    QMessageBox::information(&dialog, QStringLiteral("导出诊断包"), result.message);
                else
                    QMessageBox::warning(&dialog, QStringLiteral("导出诊断包"), result.message);
            });
    connect(send, &QPushButton::clicked,
            [&dialog, logsCheck, crashCheck, editor] {
                feedback::ReportOptions options;
                options.description = editor->toPlainText();
                options.includeLogs = logsCheck->isChecked();
                options.includeCrash = crashCheck->isChecked();
                const auto result = feedback::sendReport(options);
                if (result.success) {
                    dialog.accept();
                    QMessageBox::information(&dialog, QStringLiteral("问题反馈"), result.message);
                } else {
                    QMessageBox::warning(&dialog, QStringLiteral("问题反馈"), result.message);
                }
            });
    layout->addWidget(hint);
    layout->addWidget(editor);
    layout->addWidget(logsCheck);
    layout->addWidget(crashCheck);
    layout->addWidget(buttons);
    dialog.setMinimumWidth(440);
    dialog.exec();
}

QWidget* StudioWindow::pageHeader(const QString& eyebrow, const QString& title,
                                  const QString& description) {
    auto* widget = new QWidget;
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto* eyebrowLabel = new QLabel(eyebrow);
    eyebrowLabel->setObjectName(QStringLiteral("eyebrow"));
    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(eyebrowLabel);
    layout->addWidget(titleLabel);
    layout->addWidget(mutedLabel(description));
    return widget;
}

// ===== 第 1～4 步页面构建 =====

QWidget* StudioWindow::buildSourcePage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(pageHeader(
        QStringLiteral("第一步"), QStringLiteral("添加题目资料"),
#ifdef QUIZPANE_HAS_QT_PDF
        QStringLiteral("支持 TXT、Markdown、DOCX 和 PDF。题目和答案分在两个文件里也可以一起整理。")));
#else
        QStringLiteral("Win7 兼容版支持 TXT、Markdown 和 DOCX。题目和答案分在两个文件里也可以一起整理。")));
#endif
    parseModeCard_ = new QFrame;
    parseModeCard_->setObjectName(QStringLiteral("parseModeCard"));
    auto* modeLayout = new QVBoxLayout(parseModeCard_);
    modeLayout->setContentsMargins(16, 14, 16, 16);
    modeLayout->setSpacing(10);
    auto* modeHeader = new QHBoxLayout;
    auto* modeTitle = new QLabel(QStringLiteral("选择解析方式"));
    modeTitle->setObjectName(QStringLiteral("parseModeTitle"));
    auto* modeHint = new QLabel(QStringLiteral("选择适合资料的方式"));
    modeHint->setObjectName(QStringLiteral("parseModeHint"));
    modeHeader->addWidget(modeTitle);
    modeHeader->addStretch();
    modeHeader->addWidget(modeHint);
    modeLayout->addLayout(modeHeader);
    auto* modeCards = new QHBoxLayout;
    modeCards->setSpacing(10);
    ruleModeCard_ = new QPushButton(QStringLiteral("规则解析\n文字清晰、版式规整"));
    ruleModeCard_->setObjectName(QStringLiteral("ruleModeCard"));
    ruleModeCard_->setIcon(QIcon(QStringLiteral(":/icons/rule-parse.svg")));
    ruleModeCard_->setIconSize(QSize(28, 28));
    ruleModeCard_->setCursor(Qt::PointingHandCursor);
    ruleModeCard_->setToolTip(QStringLiteral("规则解析：资料只在本机处理。"));
    auto* smartModeWrapper = new QFrame;
    smartModeWrapper->setObjectName(QStringLiteral("smartModeWrapper"));
    auto* smartModeLayout = new QGridLayout(smartModeWrapper);
    smartModeLayout->setContentsMargins(0, 0, 0, 0);
    smartModeCard_ = new QPushButton(QStringLiteral("智能解析\n扫描件、图表、复杂版面"));
    smartModeCard_->setObjectName(QStringLiteral("smartModeCard"));
    smartModeCard_->setIcon(QIcon(QStringLiteral(":/icons/mineru-spark.svg")));
    smartModeCard_->setIconSize(QSize(30, 30));
    smartModeCard_->setCursor(Qt::PointingHandCursor);
    smartModeCard_->setToolTip(QStringLiteral("智能解析：由 MinerU 识别复杂版面。"));
    auto* smartModeBadge = new QLabel(QStringLiteral("推荐"), smartModeWrapper);
    smartModeBadge->setObjectName(QStringLiteral("smartModeBadge"));
    smartModeBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
    smartModeLayout->addWidget(smartModeCard_, 0, 0);
    smartModeLayout->addWidget(smartModeBadge, 0, 0, Qt::AlignTop | Qt::AlignRight);
    modeCards->addWidget(ruleModeCard_);
    modeCards->addWidget(smartModeWrapper);
    modeLayout->addLayout(modeCards);
    auto* configRow = new QHBoxLayout;
    mineruConfigSummary_ = mutedLabel(QString());
    mineruConfigSummary_->setObjectName(QStringLiteral("mineruConfigSummary"));
    mineruConfigButton_ = new QPushButton(QStringLiteral("查看配置与额度"));
    mineruConfigButton_->setObjectName(QStringLiteral("secondaryButton"));
    configRow->addWidget(mineruConfigSummary_);
    configRow->addStretch();
    configRow->addWidget(mineruConfigButton_);
    modeLayout->addLayout(configRow);
    connect(ruleModeCard_, &QPushButton::clicked, this, [this] { selectParseMode(false); });
    connect(smartModeCard_, &QPushButton::clicked, this, [this] { selectParseMode(true); });
    connect(mineruConfigButton_, &QPushButton::clicked, this, [this] { editMineruSettings(); });
    layout->addWidget(parseModeCard_);
    auto* drop = new QFrame;
    drop->setObjectName(QStringLiteral("dropZone"));
    auto* dropLayout = new QVBoxLayout(drop);
    dropLayout->setContentsMargins(22, 24, 22, 24);
    auto* dropTitle = new QLabel(QStringLiteral("拖入文件，或从电脑中选择"));
    dropTitle->setObjectName(QStringLiteral("sectionTitle"));
    dropTitle->setAlignment(Qt::AlignCenter);
    auto* addButton = new QPushButton(QStringLiteral("添加题目或资料"));
    addButton->setObjectName(QStringLiteral("primaryButton"));
    addButton->setFixedWidth(120);
    dropLayout->addWidget(dropTitle);
#ifdef QUIZPANE_HAS_QT_PDF
    sourceModeHint_ = mutedLabel(QString());
    sourceModeHint_->setAlignment(Qt::AlignCenter);
    dropLayout->addWidget(sourceModeHint_);
#endif
    dropLayout->addWidget(addButton, 0, Qt::AlignHCenter);
    layout->addWidget(drop);
    connect(addButton, &QPushButton::clicked, this, &StudioWindow::addSourceFiles);

    sourcePanel_ = new QWidget;
    auto* sourcePanelLayout = new QVBoxLayout(sourcePanel_);
    sourcePanelLayout->setContentsMargins(0, 0, 0, 0);
    sourcePanelLayout->setSpacing(8);
    auto* listHeader = new QHBoxLayout;
    auto* listTitle = new QLabel(QStringLiteral("已添加资料"));
    listTitle->setObjectName(QStringLiteral("sectionTitle"));
    sourceSummary_ = mutedLabel(QStringLiteral("尚未添加文件"));
    listHeader->addWidget(listTitle);
    listHeader->addWidget(sourceSummary_);
    listHeader->addStretch();
    sourcePanelLayout->addLayout(listHeader);

    sourceScroll_ = new QScrollArea;
    sourceScroll_->setObjectName(QStringLiteral("sourceScroll"));
    sourceScroll_->setWidgetResizable(true);
    sourceScroll_->setFrameShape(QFrame::NoFrame);
    sourceScroll_->setMaximumHeight(320);
    auto* sourceListContent = new QWidget;
    sourceListContent->setObjectName(QStringLiteral("sourceListContent"));
    sourceListLayout_ = new QVBoxLayout(sourceListContent);
    sourceListLayout_->setContentsMargins(0, 4, 4, 4);
    sourceListLayout_->setSpacing(10);
    sourceListLayout_->addStretch();
    sourceScroll_->setWidget(sourceListContent);
    sourcePanelLayout->addWidget(sourceScroll_);

    sourcePanel_->setVisible(false);
    layout->addWidget(sourcePanel_);
    layout->addStretch();
    return page;
}

QWidget* StudioWindow::buildProgressPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);
    layout->addWidget(pageHeader(
        QStringLiteral("第二步"), QStringLiteral("整理题目"),
        QStringLiteral("会先读取资料并检查题目结构；已提交的云端解析可在下次打开时继续等待。")));
    // 这句是隐私声明，必须反映真实状态：开启云解析后"资料不会离开电脑"就是错的。
    parseModeSummary_ = mutedLabel(QString());
    parseModeSummary_->setObjectName(QStringLiteral("notice"));
    layout->addWidget(parseModeSummary_);
    phaseLabel_ = new QLabel(QStringLiteral("等待开始"));
    phaseLabel_->setObjectName(QStringLiteral("phaseTitle"));
    phaseDetail_ =
        mutedLabel(QStringLiteral("点击下方“开始整理”后，先在本地检查资料。"));
    activitySpinner_ = new QLabel(QStringLiteral("◐ 运行中"));
    activitySpinner_->setObjectName(QStringLiteral("activitySpinner"));
    activitySpinner_->hide();
    activityTimer_ = new QTimer(this);
    connect(activityTimer_, &QTimer::timeout, this, [this] {
        static const QStringList frames{QStringLiteral("◐"), QStringLiteral("◓"),
                                        QStringLiteral("◑"), QStringLiteral("◒")};
        activitySpinner_->setText(frames.at(spinnerFrame_++ % frames.size()) +
                                  QStringLiteral(" 运行中"));
    });
    progressBar_ = new QProgressBar;
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(false);
    progressStatus_ = mutedLabel(QStringLiteral("准备中"));
    progressStatus_->setObjectName(QStringLiteral("progressStatus"));
    layout->addWidget(phaseLabel_);
    layout->addWidget(phaseDetail_);
    layout->addWidget(activitySpinner_);
    auto* progressRow = new QHBoxLayout;
    progressRow->setSpacing(10);
    progressRow->addWidget(progressBar_, 1);
    progressRow->addWidget(progressStatus_);
    layout->addLayout(progressRow);
    auto* metrics = new QHBoxLayout;
    metrics->addWidget(metricCard(QStringLiteral("已读取资料"), &sourceCount_));
    metrics->addWidget(metricCard(QStringLiteral("已整理题目"), &generatedCount_));
    metrics->addWidget(metricCard(QStringLiteral("待复核"), &reviewCount_));
    layout->addLayout(metrics);
    auto* stages = new QFrame;
    stages->setObjectName(QStringLiteral("panel"));
    auto* stagesLayout = new QVBoxLayout(stages);
    stagesLayout->setContentsMargins(18, 16, 18, 16);
    stagesLayout->addWidget(new QLabel(QStringLiteral("处理阶段")));
    stagesLayout->addWidget(mutedLabel(
        QStringLiteral("读取资料  →  识别题目、选项和答案  →  检查结果")));
    layout->addWidget(stages);
    layout->addStretch();
    return page;
}

QWidget* StudioWindow::buildFinishPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(pageHeader(
        QStringLiteral("第四步"), QStringLiteral("生成并添加到小窗刷题"),
        QStringLiteral("确认题库名称和组卷方式。完成后会自动添加到小窗刷题，无需再手动导入。")));
    auto* panel = new QFrame;
    panel->setObjectName(QStringLiteral("panel"));
    auto* form = new QVBoxLayout(panel);
    form->setContentsMargins(20, 18, 20, 18);
    form->addWidget(new QLabel(QStringLiteral("题库名称")));
    bankName_ = new QLineEdit;
    bankName_->setPlaceholderText(QStringLiteral("例如：我的行测常识题库"));
    form->addWidget(bankName_);
    form->addWidget(new QLabel(QStringLiteral("默认每套题数量")));
    questionCount_ = new StyledDropdown;
    questionCount_->addItems({QStringLiteral("5 题"), QStringLiteral("10 题"),
                     QStringLiteral("15 题"), QStringLiteral("全部题目")});
    form->addWidget(questionCount_);
    finishPath_ = mutedLabel(QStringLiteral("完成后会自动保存到小窗刷题的题库目录。"));
    form->addWidget(finishPath_);
    layout->addWidget(panel);
    layout->addStretch();
    return page;
}

// ===== 资料列表 =====

void StudioWindow::addSourceFiles() {
    appendSources(QFileDialog::getOpenFileNames(this, QStringLiteral("添加题目或资料"), {},
#ifdef QUIZPANE_HAS_QT_PDF
        QStringLiteral("题目资料 (*.txt *.md *.markdown *.docx *.pdf)")));
#else
        QStringLiteral("题目资料 (*.txt *.md *.markdown *.docx)")));
#endif
}

void StudioWindow::appendSources(const QStringList& paths) {
    int added = 0;
    for (const QString& path : paths) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        if (!acceptedSource(absolute) || sourcePaths_.contains(absolute)) continue;
        sourcePaths_.append(absolute);
        answerPolicyByQuestion_.insert(absolute, AnswerPolicyHint::Auto);
        ++added;
        auto* row = new SourceRowWidget(absolute);
        sourceRows_.insert(absolute, row);
        // 插入到末尾的拉伸占位之前，保持新行始终追加在列表最下方。
        sourceListLayout_->insertWidget(sourceListLayout_->count() - 1, row);
        connect(row, &SourceRowWidget::answerPolicyChanged, this,
                [this, absolute](AnswerPolicyHint policy) {
            answerPolicyByQuestion_.insert(absolute, policy);
        });
        connect(row, &SourceRowWidget::answerRequested, this, [this, absolute] {
            const QString answer = QFileDialog::getOpenFileName(
                this, QStringLiteral("添加答案或解析"), {},
#ifdef QUIZPANE_HAS_QT_PDF
                QStringLiteral("答案或解析 (*.txt *.md *.markdown *.docx *.pdf)"));
#else
                QStringLiteral("答案或解析 (*.txt *.md *.markdown *.docx)"));
#endif
            if (answer.isEmpty()) return;
            pairAnswer(absolute, QFileInfo(answer).absoluteFilePath());
        });
        connect(row, &SourceRowWidget::answerDropped, this, [this, absolute](const QString& answer) {
            pairAnswer(absolute, answer);
        });
        connect(row, &SourceRowWidget::answerCleared, this, [this, absolute] {
            answerPathsByQuestion_.remove(absolute);
            if (auto* r = sourceRows_.value(absolute)) r->clearPairedAnswer();
        });
        connect(row, &SourceRowWidget::removeRequested, this, [this, absolute] {
            removeSource(absolute);
        });
    }
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("sources-updated"),
        {{QStringLiteral("offered"), paths.size()},
         {QStringLiteral("added"), added},
         {QStringLiteral("total"), sourcePaths_.size()}});
    sourceSummary_->setText(sourcePaths_.isEmpty() ? QStringLiteral("尚未添加文件")
        : QStringLiteral("%1 个文件").arg(sourcePaths_.size()));
    sourcePanel_->setVisible(!sourcePaths_.isEmpty());
    updateNavigation();
}

void StudioWindow::pairAnswer(const QString& question, const QString& answer) {
    if (answer == question || !acceptedSource(answer)) return;
    answerPathsByQuestion_.insert(question, answer);
    answerPolicyByQuestion_.insert(question, AnswerPolicyHint::Included);
    if (auto* row = sourceRows_.value(question)) row->setPairedAnswer(answer);
}

void StudioWindow::removeSource(const QString& question) {
    sourcePaths_.removeAll(question);
    answerPathsByQuestion_.remove(question);
    answerPolicyByQuestion_.remove(question);
    if (auto* row = sourceRows_.take(question)) {
        sourceListLayout_->removeWidget(row);
        row->deleteLater();
    }
    sourceSummary_->setText(sourcePaths_.isEmpty() ? QStringLiteral("尚未添加文件")
        : QStringLiteral("%1 个文件").arg(sourcePaths_.size()));
    sourcePanel_->setVisible(!sourcePaths_.isEmpty());
    updateNavigation();
}

// ===== 向导状态机与本地预检 =====

void StudioWindow::movePage(int delta) {
    pages_->setCurrentIndex(qBound(0, pages_->currentIndex() + delta, pages_->count() - 1));
}

void StudioWindow::startFromSources() {
    if (sourcePaths_.isEmpty())
        return;
    if (mineruConfig_.cloudEnabled && loadMineruToken().trimmed().isEmpty()) {
        if (!editMineruSettings() || !mineruConfig_.cloudEnabled ||
            loadMineruToken().trimmed().isEmpty())
            return;
    }
    pages_->setCurrentIndex(1);
    beginPreflight();
}

void StudioWindow::handleBackNavigation() {
    movePage(-1);
}

void StudioWindow::updateNavigation() {
    updateParseModeSummary();
    const int page = pages_->currentIndex();
    const bool cloudActive = mineruJob_ &&
        mineruJob_->stage() != MineruStage::Idle &&
        mineruJob_->stage() != MineruStage::Done &&
        mineruJob_->stage() != MineruStage::Failed &&
        mineruJob_->stage() != MineruStage::Cancelled;
    const bool workflowActive = workflow_ && workflow_->isActive();
    // 整理期间没有“后台等待并关闭”入口。旧入口实际会关闭进程，并不能在本机
    // 后台继续工作；运行中只保留明确的取消/放弃操作。
    backButton_->setVisible(page > 0 && !cloudActive && !workflowActive);
    backButton_->setText(QStringLiteral("上一步"));
    nextButton_->setVisible(page == 0 || page == 2);
    startButton_->setVisible(page == 1 || page == 3);
    nextButton_->setEnabled(page != 0 || !sourcePaths_.isEmpty());
    if (page == 0) {
        nextButton_->setText(mineruConfig_.cloudEnabled
            ? QStringLiteral("开始智能解析  →") : QStringLiteral("开始规则解析  →"));
    } else if (page == 2) {
        int selected = 0;
        for (QTreeWidgetItemIterator it(reviewController_.reviewTree_); *it; ++it)
            if (!(*it)->data(0, Qt::UserRole).toJsonObject().isEmpty() &&
                !(*it)->data(0, Qt::UserRole).toJsonObject().contains(QStringLiteral("body")) &&
                (*it)->checkState(0) == Qt::Checked)
                ++selected;
        nextButton_->setText(QStringLiteral("继续生成（收录 %1 题） →").arg(selected));
    }
    nextButton_->setObjectName(page == 0 || page == 2
        ? QStringLiteral("primaryButton") : QString());
    nextButton_->style()->unpolish(nextButton_);
    nextButton_->style()->polish(nextButton_);
    startButton_->setEnabled(true);
    if (page == 3)
        startButton_->setText(QStringLiteral("生成并打开题库"));
    else if (cloudActive && !cloudBatchId_.isEmpty())
        startButton_->setText(QStringLiteral("放弃此任务"));
    else if (cloudActive || workflowActive)
        startButton_->setText(QStringLiteral("取消整理"));
    else
        startButton_->setText(QStringLiteral("重新开始整理"));
    startButton_->setObjectName(cloudActive && !cloudBatchId_.isEmpty()
        ? QStringLiteral("dangerButton") : QStringLiteral("primaryButton"));
    startButton_->style()->unpolish(startButton_);
    startButton_->style()->polish(startButton_);
    const auto steps = findChildren<QLabel*>(QStringLiteral("sideStep"));
    for (QLabel* step : steps) {
        step->setProperty("active", step->property("stepIndex").toInt() == page);
        step->style()->unpolish(step); step->style()->polish(step);
    }
}

void StudioWindow::beginPreflight() {
    if (pages_->currentIndex() == 3) {
        packageProvider();
        return;
    }
    const bool cloudActive = mineruJob_ &&
        mineruJob_->stage() != MineruStage::Idle &&
        mineruJob_->stage() != MineruStage::Done &&
        mineruJob_->stage() != MineruStage::Failed &&
        mineruJob_->stage() != MineruStage::Cancelled;
    if (cloudActive || (workflow_ && workflow_->isActive())) {
        if (cloudActive && !cloudBatchId_.isEmpty() &&
            QMessageBox::warning(this, QStringLiteral("放弃云端任务？"),
                QStringLiteral("放弃后将清除本机任务记录和已下载缓存；云端已经提交的任务可能仍会完成。"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        if (cloudActive)
            mineruJob_->cancel();
        if (workflow_ && workflow_->isActive()) {
            workflow_->cancel();
            activityTimer_->stop();
            activitySpinner_->hide();
            progressBar_->setValue(0);
            phaseLabel_->setText(QStringLiteral("已取消"));
            phaseDetail_->setText(QStringLiteral("本次整理已取消，可重新开始。"));
            updateNavigation();
        }
        return;
    }
    if (sourcePaths_.isEmpty()) return;
    // 只要用户选了智能模式，Token 就必须先配好。即便这批恰好全是 TXT/DOCX，
    // 也不能把“智能模式”悄悄当成规则模式执行；配置完成后，程序仍会仅对真正
    // 需要版面识别的 PDF/图片调用 MinerU。
    if (mineruConfig_.cloudEnabled &&
        loadMineruToken().trimmed().isEmpty()) {
        if (!editMineruSettings() || !mineruConfig_.cloudEnabled ||
            loadMineruToken().trimmed().isEmpty()) {
            return;
        }
    }
    discardPreviousGenerationForNewTask();
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("generation-start"),
        {{QStringLiteral("mode"), mineruConfig_.cloudEnabled
            ? QStringLiteral("smart") : QStringLiteral("rules")},
         {QStringLiteral("sources"), sourcePaths_.size()}});
    if (workflow_) workflow_->deleteLater();
    workflow_ = new GenerationWorkflow(this);
    connect(workflow_, &GenerationWorkflow::progressChanged,
            this, &StudioWindow::updateWorkflowProgress);
    connect(workflow_, &GenerationWorkflow::questionsReady,
            this, &StudioWindow::populateReview);
    connect(workflow_, &GenerationWorkflow::failed, this, [this](const QString& error) {
        activityTimer_->stop();
        activitySpinner_->hide();
        updateNavigation();
        QMessageBox::warning(this, QStringLiteral("整理未完成"), error);
        clearPersistedCloudTask();
    });
    connect(workflow_, &GenerationWorkflow::finished, this, [this] {
        activityTimer_->stop();
        activitySpinner_->hide();
        updateNavigation();
        if (reviewController_.generatedQuestions_.isEmpty() && reviewController_.reviewQuestions_.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("没有生成题目"),
                                 QStringLiteral("没有从资料中识别出可用题目。"));
            return;
        }
        clearPersistedCloudTask();
        pages_->setCurrentIndex(2);
    });
    updateParseModeSummary();
    progressBar_->setValue(0);
    progressStatus_->setText(QStringLiteral("准备中"));
    sourceCount_->setText(QString::number(sourcePaths_.size()));
    generatedCount_->setText(QStringLiteral("0"));
    reviewCount_->setText(QStringLiteral("0"));
    startButton_->setEnabled(true);
    startButton_->setText(QStringLiteral("取消整理"));
    spinnerFrame_ = 0;
    activitySpinner_->setText(QStringLiteral("◐ 运行中"));
    activitySpinner_->show();
    activityTimer_->start(120);
    QList<SourceMaterialGroup> groups;
    for (const QString& question : sourcePaths_) {
        groups.append({question, answerPathsByQuestion_.value(question),
                       answerPolicyByQuestion_.value(question, AnswerPolicyHint::Auto), {}, {}});
    }
    startCloudParseThenGenerate(groups);
}

void StudioWindow::discardPreviousGenerationForNewTask()
{
    reviewController_.discardForNewTask();
}

// 云解析在规则工作流之前完成：MinerU 是异步任务，而工作流内部的提取运行在
// 工作线程里同步调用。逐份串行处理，避免一次性把多份文档推给云端。
void StudioWindow::startCloudParseThenGenerate(const QList<SourceMaterialGroup>& groups) {
    if (!shouldUseCloudParse()) {
        workflow_->startRuleBased(groups);
        return;
    }
    const QString root = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("mineru-tasks"));
    cloudSessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    cloudCacheDir_ = QDir(root).filePath(cloudSessionId_);
    if (!QDir().mkpath(cloudCacheDir_)) {
        activityTimer_->stop();
        activitySpinner_->hide();
        progressBar_->setValue(0);
        pages_->setCurrentIndex(0);
        updateNavigation();
        QMessageBox::warning(this, QStringLiteral("无法使用智能解析"),
                             QStringLiteral("无法准备本机任务缓存，请检查存储空间后重试。"));
        return;
    }
    pendingGroups_ = groups;
    cloudIndex_ = 0;
    cloudBatchId_.clear();
    persistCloudTask();
    processNextCloudSource();
}

bool StudioWindow::shouldUseCloudParse() const {
#ifndef QUIZPANE_HAS_QT_PDF
    // 不含 Qt PDF 的裁剪构建只接受 TXT/Markdown/DOCX；Win7 正式包会额外
    // 部署 Qt5Pdf 和 OpenSSL，因而同样支持 PDF/图片的 MinerU 智能解析。
    return false;
#else
    if (!mineruConfig_.cloudEnabled)
        return false;
    // 只有 PDF 与图片能受益于版面理解；纯文本与 DOCX 本机解析已经是无损的。
    for (const QString& path : sourcePaths_) {
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == QStringLiteral("pdf") || suffix == QStringLiteral("png") ||
            suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg"))
            return true;
        const QString answerSuffix =
            QFileInfo(answerPathsByQuestion_.value(path)).suffix().toLower();
        if (answerSuffix == QStringLiteral("pdf") || answerSuffix == QStringLiteral("png") ||
            answerSuffix == QStringLiteral("jpg") || answerSuffix == QStringLiteral("jpeg"))
            return true;
    }
    return false;
#endif
}

void StudioWindow::processNextCloudSource() {
    // 所有资料处理完毕（或都不需要云解析）后再跑规则引擎。
    const auto benefitsFromCloud = [](const QString& path) {
        const QString suffix = QFileInfo(path).suffix().toLower();
        return suffix == QStringLiteral("pdf") || suffix == QStringLiteral("png") ||
               suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg");
    };
    while (cloudIndex_ < pendingGroups_.size()) {
        const SourceMaterialGroup& group = pendingGroups_.at(cloudIndex_);
        if (benefitsFromCloud(group.questionPath) && group.mineruZipPath.isEmpty()) {
            cloudParsingAnswer_ = false;
            break;
        }
        if (!group.answerPath.isEmpty() && benefitsFromCloud(group.answerPath) &&
            group.mineruAnswerZipPath.isEmpty()) {
            cloudParsingAnswer_ = true;
            break;
        }
        ++cloudIndex_;
        cloudBatchId_.clear();
        persistCloudTask();
    }
    if (cloudIndex_ >= pendingGroups_.size()) {
        workflow_->startRuleBased(pendingGroups_);
        return;
    }

    const QString sourcePath = cloudParsingAnswer_
        ? pendingGroups_.at(cloudIndex_).answerPath
        : pendingGroups_.at(cloudIndex_).questionPath;
    MineruSettings settings;
    settings.token = loadMineruToken();
    settings.modelVersion = mineruConfig_.modelVersion;
    settings.isOcr = mineruConfig_.isOcr;
    if (settings.token.trimmed().isEmpty()) {
        // 这通常只会发生在用户拒绝钥匙串授权等运行时情形；绝不暗自换模式。
        activityTimer_->stop();
        activitySpinner_->hide();
        progressBar_->setValue(0);
        pages_->setCurrentIndex(0);
        updateNavigation();
        QMessageBox::warning(this, QStringLiteral("需要配置 MinerU Token"),
                             QStringLiteral("无法读取 MinerU Token。请重新配置后再开始整理。"));
        editMineruSettings();
        return;
    }

    if (mineruJob_)
        mineruJob_->deleteLater();
    mineruJob_ = new MineruExtractionJob(networkManager_, this);
    connect(mineruJob_, &MineruExtractionJob::taskSubmitted, this, [this](const QString& batchId) {
        cloudBatchId_ = batchId;
        persistCloudTask();
        QSettings settings = projectSettings();
        const QString key = QStringLiteral("question-maker/mineru/usage/%1/submittedFiles")
            .arg(QDate::currentDate().toString(Qt::ISODate));
        settings.setValue(key, settings.value(key, 0).toInt() + 1);
        settings.sync();
        updateNavigation();
    });
    connect(mineruJob_, &MineruExtractionJob::stageChanged, this,
            [this, sourcePath](MineruStage stage, const QString& detail) {
                phaseLabel_->setText(QStringLiteral("云端解析"));
                phaseDetail_->setText(QStringLiteral("%1：%2")
                                          .arg(QFileInfo(sourcePath).fileName(), detail));
                progressStatus_->setText(describeMineruStage(stage));
            });
    connect(mineruJob_, &MineruExtractionJob::progress, this, [this](int extracted, int total) {
        if (total > 0) {
            progressBar_->setValue(qBound(0, 20 * extracted / total, 20));
            progressStatus_->setText(QStringLiteral("%1 / %2 页").arg(extracted).arg(total));
        }
    });
    connect(mineruJob_, &MineruExtractionJob::finished, this,
            [this](bool ok, const QString& zipPath, const QString& error) {
                if (!ok) {
                    if (mineruJob_ && mineruJob_->stage() == MineruStage::Cancelled) {
                        pendingGroups_.clear();
                        clearPersistedCloudTask();
                        activityTimer_->stop();
                        activitySpinner_->hide();
                        progressBar_->setValue(0);
                        phaseLabel_->setText(QStringLiteral("已取消"));
                        phaseDetail_->setText(QStringLiteral("本次整理已取消，可重新开始。"));
                        updateNavigation();
                        return;
                    }
                    // 网络、额度和鉴权错误都不能替用户把“智能模式”切成规则模式。
                    // 停在资料页；鉴权错误会把原始提示直接带进配置页。
                    activityTimer_->stop();
                    activitySpinner_->hide();
                    progressBar_->setValue(0);
                    pages_->setCurrentIndex(0);
                    updateNavigation();
                    const bool tokenProblem = error.contains(QStringLiteral("Token"),
                                                             Qt::CaseInsensitive);
                    if (tokenProblem) {
                        editMineruSettings(error);
                    } else {
                        QMessageBox::warning(this, QStringLiteral("智能解析未完成"),
                                             error + QStringLiteral("\n任务已保留，可稍后重新打开继续等待。"));
                    }
                    return;
                }
                if (cloudParsingAnswer_)
                    pendingGroups_[cloudIndex_].mineruAnswerZipPath = zipPath;
                else
                    pendingGroups_[cloudIndex_].mineruZipPath = zipPath;
                cloudBatchId_.clear();
                persistCloudTask();
                processNextCloudSource();
            });
    const QString zipPath = QDir(cloudCacheDir_).filePath(
        QStringLiteral("mineru-%1-%2.zip")
            .arg(cloudIndex_)
            .arg(cloudParsingAnswer_ ? QStringLiteral("answer") : QStringLiteral("question")));
    if (cloudBatchId_.isEmpty())
        mineruJob_->start(settings, sourcePath, zipPath);
    else
        mineruJob_->resume(settings, cloudBatchId_, zipPath);
}

void StudioWindow::updateWorkflowProgress(const WorkflowProgress& progress) {
    int base = 0, span = 0;
    QString phase;
    switch (progress.stage) {
    case WorkflowStage::Extracting: base = 20; span = 40; phase = QStringLiteral("读取资料"); break;
    case WorkflowStage::Chunking: base = 60; span = 30; phase = QStringLiteral("规则整理"); break;
    case WorkflowStage::Done: base = 100; phase = QStringLiteral("整理完成"); break;
    case WorkflowStage::Failed: phase = QStringLiteral("任务中断"); break;
    default: phase = QStringLiteral("等待继续"); break;
    }
    const int within = progress.totalSourceBlocks > 0
        ? span * progress.completedSourceBlocks / progress.totalSourceBlocks : 0;
    const int value = progress.percent >= 0
        ? qBound(0, progress.percent, 100) : qBound(0, base + within, 100);
    if (progress.stage == WorkflowStage::Chunking && !progress.rulePass.isEmpty())
        phase += QStringLiteral(" · ") + progress.rulePass;
    progressBar_->setValue(value);
    if (progress.stage == WorkflowStage::Chunking && progress.questionCount > 0) {
        progressStatus_->setText(QStringLiteral("%1% · %2/%3 题")
            .arg(value).arg(progress.questionIndex).arg(progress.questionCount));
    } else {
        progressStatus_->setText(QStringLiteral("%1%").arg(value));
    }
    phaseLabel_->setText(phase);
    phaseDetail_->setText(progress.detail);
    sourceCount_->setText(QString::number(progress.completedSourceBlocks));
    if (progress.stage == WorkflowStage::Chunking) {
        generatedCount_->setText(QString::number(progress.acceptedQuestions));
        reviewCount_->setText(QString::number(progress.reviewQuestions));
    }
}

void StudioWindow::populateReview(const GeneratedBankCandidate& candidate)
{
    reviewController_.populateReview(candidate);
}

QByteArray StudioWindow::ensureReviewAssetBytes(const QJsonObject& asset)
{
    return reviewController_.ensureReviewAssetBytes(asset);
}

void StudioWindow::displayReviewAssets(const QList<QJsonObject>& assets)
{
    reviewController_.displayReviewAssets(assets);
}

void StudioWindow::recropReviewAsset(const QJsonObject& asset)
{
    reviewController_.recropReviewAsset(asset);
}

bool StudioWindow::commitReviewCrop(const QJsonObject& asset, const QImage& pageImage,
                                    const QRectF& normalizedCrop)
{
    return reviewController_.commitReviewCrop(asset, pageImage, normalizedCrop);
}



void StudioWindow::setReviewOptions(const QJsonArray& options)
{
    reviewController_.setReviewOptions(options);
}

void StudioWindow::addReviewOption(const QString& id, const QString& text)
{
    reviewController_.addReviewOption(id, text);
}

QJsonArray StudioWindow::reviewOptions() const
{
    return reviewController_.reviewOptions();
}

void StudioWindow::showReviewQuestion(QTreeWidgetItem* item)
{
    reviewController_.showReviewQuestion(item);
}

void StudioWindow::addManualMaterialUnderline()
{
    reviewController_.addManualMaterialUnderline();
}

bool StudioWindow::saveCurrentReviewQuestion()
{
    return reviewController_.saveCurrentReviewQuestion();
}

bool StudioWindow::reviewQuestionIsDirty() const
{
    return reviewController_.reviewQuestionIsDirty();
}

bool StudioWindow::commitOpenReviewQuestion(const QString& consequence)
{
    return reviewController_.commitOpenReviewQuestion(consequence);
}

void StudioWindow::confirmCurrentReviewQuestion()
{
    reviewController_.confirmCurrentReviewQuestion();
}

void StudioWindow::excludeCurrentReviewQuestion()
{
    reviewController_.excludeCurrentReviewQuestion();
}

void StudioWindow::refreshReviewDecisionState()
{
    reviewController_.refreshReviewDecisionState();
}

void StudioWindow::advanceToNextReviewIssue()
{
    reviewController_.advanceToNextReviewIssue();
}



void StudioWindow::updateReviewStemHeight()
{
    reviewController_.updateReviewStemHeight();
}

void StudioWindow::applyReviewFilter()
{
    reviewController_.applyReviewFilter();
}

void StudioWindow::packageProvider() {
    // 打包前先把当前打开且改过的题提交回树节点数据，避免编辑器里的修改被静默丢弃。
    if (!commitOpenReviewQuestion(QStringLiteral("继续打包")))
        return;
    QList<QJsonObject> selectedObjects;
    QHash<QString, int> sourceOrder;
    int sourceOrdinal = 0;
    const auto rememberSourceOrder = [&](const QJsonArray& questions) {
        for (const auto& value : questions)
            sourceOrder.insert(value.toObject().value("id").toString(), sourceOrdinal++);
    };
    rememberSourceOrder(reviewController_.generatedQuestions_);
    rememberSourceOrder(reviewController_.reviewQuestions_);
    QSet<QString> usedMaterialIds;
    for (int topIndex = 0; topIndex < reviewController_.reviewTree_->topLevelItemCount(); ++topIndex) {
        QTreeWidgetItem* group = reviewController_.reviewTree_->topLevelItem(topIndex);
        for (int childIndex = 0; childIndex < group->childCount(); ++childIndex) {
            QTreeWidgetItem* child = group->child(childIndex);
            if (child->checkState(0) == Qt::Unchecked) continue;
            const QJsonObject question = child->data(0, Qt::UserRole).toJsonObject();
            if (question.isEmpty()) continue;
            selectedObjects.append(question);
            const QString materialId = question.value("materialId").toString();
            if (!materialId.isEmpty()) usedMaterialIds.insert(materialId);
        }
    }
    if (selectedObjects.isEmpty()) {
        diagnostic::event(QStringLiteral("studio"), QStringLiteral("package-rejected"),
            {{QStringLiteral("reason"), QStringLiteral("no-selected-questions")}});
        QMessageBox::warning(this, QStringLiteral("无法生成"), QStringLiteral("至少需要采纳一道题。"));
        return;
    }
    std::stable_sort(selectedObjects.begin(), selectedObjects.end(),
        [&sourceOrder](const QJsonObject& left, const QJsonObject& right) {
            return sourceOrder.value(left.value("id").toString(), std::numeric_limits<int>::max()) <
                   sourceOrder.value(right.value("id").toString(), std::numeric_limits<int>::max());
        });
    QJsonArray selected;
    for (const QJsonObject& question : std::as_const(selectedObjects))
        selected.append(question);
    QString title = bankName_->text().trimmed();
    if (title.isEmpty()) title = QStringLiteral("我的题库");
    int questionCount = 0;
    if (questionCount_->currentIndex() < 3)
        questionCount = QList<int>{5, 10, 15}.at(questionCount_->currentIndex());
    QJsonObject practice{{"mode", questionCount == 0 ? "all" : "sequential"}};
    if (questionCount > 0) practice.insert("questionCount", qMin(questionCount, selected.size()));
    QJsonArray selectedMaterials;
    for (const auto& value : reviewController_.generatedMaterials_) {
        const QJsonObject material = value.toObject();
        if (usedMaterialIds.contains(material.value("id").toString()))
            selectedMaterials.append(material);
    }
    QJsonObject bank{{"schemaVersion", 3}, {"title", title},
        {"answerPolicy", reviewController_.generatedHasAnswerKey_ ? QStringLiteral("included") : QStringLiteral("none")},
        {"catalogs", QJsonArray{QJsonObject{{"id", "generated"}, {"title", title},
            {"practice", practice}}}}, {"questions", selected}};
    if (!selectedMaterials.isEmpty()) bank.insert("materials", selectedMaterials);
    QString error;
    if (!quizpane::validateBank(bank, &error)) {
        diagnostic::event(QStringLiteral("studio"), QStringLiteral("package-validation-failed"),
            {{QStringLiteral("questions"), selected.size()},
             {QStringLiteral("materials"), selectedMaterials.size()},
             {QStringLiteral("error"), error}});
        QMessageBox::warning(this, QStringLiteral("题库校验失败"), error);
        return;
    }
    const QString slug = QString::fromLatin1(QCryptographicHash::hash(
        title.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    const QString version = QStringLiteral("1.0.%1")
        .arg(QDateTime::currentSecsSinceEpoch());
    const QJsonObject manifest{{"manifestVersion", 2}, {"id", "local.generated." + slug},
        {"name", title}, {"version", version}, {"kind", "declarative"},
        {"runtime", QJsonObject{{"format", "quizpane.bank+json"}, {"schemaVersion", 3},
            {"entry", "content/bank.json"}}},
        {"permissions", QJsonObject{{"network", false}}}};
    QTemporaryDir packageDirectory;
    if (!packageDirectory.isValid()) {
        QMessageBox::critical(this, QStringLiteral("无法生成题库"),
                              QStringLiteral("无法创建临时题库目录。"));
        return;
    }
    const QString output = packageDirectory.filePath(QStringLiteral("generated.quizpane-provider"));
    QList<quizpane::ZipFile> packageFiles{
        {QStringLiteral("manifest.json"), QJsonDocument(manifest).toJson(QJsonDocument::Indented)},
        {QStringLiteral("content/bank.json"), QJsonDocument(bank).toJson(QJsonDocument::Indented)}};
    for (auto it = reviewController_.generatedAssets_.cbegin(); it != reviewController_.generatedAssets_.cend(); ++it)
        packageFiles.append({it.key(), it.value()});
    if (!quizpane::writeZipArchive(output, packageFiles, &error)) {
        diagnostic::event(QStringLiteral("studio"), QStringLiteral("package-write-failed"),
            {{QStringLiteral("error"), error}});
        QMessageBox::critical(this, QStringLiteral("打包失败"), error);
        return;
    }
    quizpane::ProviderPackageInfo info;
    quizpane::ProviderInstaller installer;
    if (!installer.inspect(output, &info, &error)) {
        diagnostic::event(QStringLiteral("studio"), QStringLiteral("package-inspect-failed"),
            {{QStringLiteral("error"), error}});
        QFile::remove(output);
        QMessageBox::critical(this, QStringLiteral("安装包自检失败"), error);
        return;
    }
    // 从刚写出的 ZIP 重新读取文件再交给声明式运行时，而不是验证打包前的
    // 内存对象，确保压缩、路径和读取链路本身也进入最终自检。
    quizpane::ZipArchiveReader archive(output);
    const QByteArray packagedManifest = archive.fileData(QStringLiteral("manifest.json"));
    const QByteArray packagedBank = archive.fileData(QStringLiteral("content/bank.json"));
    if (!archive.isReadable() || packagedManifest.isEmpty() || packagedBank.isEmpty()) {
        QFile::remove(output);
        QMessageBox::critical(this, QStringLiteral("最终验证失败"),
                              QStringLiteral("无法从生成的安装包重新读取题库内容"));
        return;
    }
    QTemporaryDir staging;
    QDir().mkpath(staging.filePath(QStringLiteral("content")));
    QFile manifestFile(staging.filePath(QStringLiteral("manifest.json")));
    QFile bankFile(staging.filePath(QStringLiteral("content/bank.json")));
    if (!manifestFile.open(QIODevice::WriteOnly) || !bankFile.open(QIODevice::WriteOnly)) {
        QFile::remove(output);
        QMessageBox::critical(this, QStringLiteral("最终验证失败"), QStringLiteral("无法创建临时验证目录"));
        return;
    }
    manifestFile.write(packagedManifest); manifestFile.close();
    bankFile.write(packagedBank); bankFile.close();
    quizpane::DeclarativeProvider provider;
    if (!provider.load(staging.filePath(QStringLiteral("content/bank.json")), &error)) {
        QFile::remove(output);
        QMessageBox::critical(this, QStringLiteral("最终验证失败"), error);
        return;
    }
    quizpane::ProviderInstallResult installed;
    if (!installer.install(info, &installed, &error)) {
        QMessageBox::critical(this, QStringLiteral("无法添加题库"), error);
        return;
    }
    // 制作器与小窗是独立进程。先把题库交给已有的小窗进程，它会直接切换题库并
    // 进入“选择练习数量”页；绝不能通过 open -a 再拉一个同名应用实例，否则
    // macOS 可能命中另一份旧安装包，进而显示错误的 Schema 不匹配提示。
    QSettings practiceSettings = projectSettings(QStringLiteral("小窗刷题"));
    practiceSettings.setValue(QStringLiteral("provider/lastLibraryPath"), installed.entryPath);
    practiceSettings.sync();
    QString handoffError;
    const bool handedOff = quizpane::handoffProviderToRunningApp(installed.entryPath, &handoffError);
    const bool launched = handedOff || launchQuizPaneForProvider(installed.entryPath);
    if (!launched) {
        QMessageBox::critical(this, QStringLiteral("无法打开小窗刷题"),
            handoffError.isEmpty()
                ? QStringLiteral("未找到可启动的小窗刷题。请确认主程序仍在安装包内。")
                : handoffError + QStringLiteral("；且未找到可启动的小窗刷题。"));
        return;
    }
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("package-success"),
        {{QStringLiteral("file"), QFileInfo(output).fileName()},
         {QStringLiteral("questions"), selected.size()},
         {QStringLiteral("materials"), selectedMaterials.size()},
         {QStringLiteral("bytes"), QFileInfo(output).size()}});
    finishPath_->setText(QStringLiteral("已添加到小窗刷题：%1\n%2")
        .arg(title, QDir::toNativeSeparators(installed.installDirectory)));
    if (workflow_) workflow_->cancel();
    QMessageBox::information(this, QStringLiteral("题库已添加"), handedOff
        ? QStringLiteral("“%1”已在当前小窗刷题中打开，请选择练习题数。制作器将关闭。").arg(title)
        : QStringLiteral("“%1”已打开到小窗刷题，请选择练习题数。制作器将关闭。").arg(title));
    QTimer::singleShot(0, this, &QWidget::close);
}

// ===== 桌面文件拖放与统一样式 =====

void StudioWindow::dragEnterEvent(QDragEnterEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile() || !acceptedSource(url.toLocalFile())) continue;
        event->acceptProposedAction();
        return;
    }
}

void StudioWindow::dropEvent(QDropEvent* event) {
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) paths.append(url.toLocalFile());
    }
    appendSources(paths);
    event->acceptProposedAction();
}

void StudioWindow::closeEvent(QCloseEvent* event) {
    const bool cloudActive = mineruJob_ &&
        mineruJob_->stage() != MineruStage::Idle &&
        mineruJob_->stage() != MineruStage::Done &&
        mineruJob_->stage() != MineruStage::Failed &&
        mineruJob_->stage() != MineruStage::Cancelled;
    const bool workflowActive = workflow_ && workflow_->isActive();
    if (!cloudActive && !workflowActive) {
        event->accept();
        return;
    }
    const bool canResumeCloud = cloudActive && !cloudBatchId_.isEmpty();
    const QString title = canResumeCloud ? QStringLiteral("保留云端任务并关闭？")
                                         : QStringLiteral("结束正在整理？");
    const QString text = canResumeCloud
        ? QStringLiteral("文件已提交到云端。关闭后云端会继续处理；下次打开题库制作器时，"
                         "可选择继续等待，不会重复上传。")
        : QStringLiteral("本次整理尚未完成。关闭后需要重新开始整理。");
    const QString acceptText = canResumeCloud ? QStringLiteral("保留并关闭")
                                               : QStringLiteral("结束并关闭");
    if (!confirmAction(this, title, text, acceptText)) {
        event->ignore();
        return;
    }
    if (cloudActive) {
        if (canResumeCloud)
            persistCloudTask();
        else
            clearPersistedCloudTask();
        mineruJob_->cancel();
    }
    if (workflowActive)
        workflow_->cancel();
    event->accept();
}

void StudioWindow::applyStyle() {
    const bool lightTheme = studioColorTheme() == QStringLiteral("light");
    // Windows 的原生控件（尤其是 CheckBox、禁用 Label、编辑器 viewport）有时
    // 不继承父 QWidget 的 QSS color。先设应用级 Palette，再加载组件级 QSS，
    // 让文字、按钮文字和输入文本在三端都有稳定对比度。
    QPalette palette = QApplication::palette();
    if (lightTheme) {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#f4f6f8")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#27313b")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#f4f6f8")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#27313b")));
        palette.setColor(QPalette::Button, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#334252")));
        palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#d9ebf8")));
        palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#17324a")));
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#8995a0")));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#8995a0")));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#8995a0")));
    } else {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#0c0e12")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#c8cdd3")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#111419")));
        palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#15191e")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#cfd4da")));
        palette.setColor(QPalette::Button, QColor(QStringLiteral("#20252b")));
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#cfd4da")));
        palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#3b424b")));
        palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#8c959f")));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#8c959f")));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#8c959f")));
    }
    QApplication::setPalette(palette);
    const QString path = lightTheme
        ? QStringLiteral(":/styles/studio-light.qss")
        : QStringLiteral(":/styles/studio.qss");
    QFile style(path);
    if (!style.open(QIODevice::ReadOnly)) {
        qWarning("Unable to load embedded studio stylesheet");
        return;
    }
    setStyleSheet(QString::fromUtf8(style.readAll()));
}

}  // namespace quizpane::studio
