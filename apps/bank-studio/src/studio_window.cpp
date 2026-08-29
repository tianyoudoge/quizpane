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
#include "source_row_widget.hpp"
#include "review_draft_bank.hpp"
#include "source_validation.hpp"
#include "styled_dropdown.hpp"

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
    QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
    const QString value = settings.value(QStringLiteral("ui/colorTheme"),
                                         QStringLiteral("dark")).toString();
    return value == QStringLiteral("light") ? value : QStringLiteral("dark");
}

void storeStudioColorTheme(const QString& value) {
    QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
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
        painter.fillRect(rect(), QColor(QStringLiteral("#0b0d10")));
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

QImage renderPdfReviewPage(const QString& sourcePath, int page, QString* error) {
#ifdef QUIZPANE_HAS_QT_PDF
    QPdfDocument document;
    if (!pdfLoadSucceeded(loadPdfDocument(&document, sourcePath)) || page < 1 ||
        page > document.pageCount()) {
        *error = QStringLiteral("无法打开原卷第 %1 页").arg(page);
        return {};
    }
    const QSizeF points = pdfPagePointSize(&document, page - 1);
    const QSize pixels = QSize(qBound(1, qRound(points.width() * 1.7), 1800),
                               qBound(1, qRound(points.height() * 1.7), 2400));
    const QImage image = document.render(page - 1, pixels);
    if (image.isNull())
        *error = QStringLiteral("无法渲染原卷第 %1 页").arg(page);
    return image;
#else
    Q_UNUSED(sourcePath)
    Q_UNUSED(page)
    *error = QStringLiteral("当前兼容构建未包含 PDF 原卷预览");
    return {};
#endif
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
    QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
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
    QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
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
    QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
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
        group.hasAnswerKey = settings.value(QStringLiteral("hasAnswerKey"), true).toBool();
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
    QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
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
        settings.setValue(QStringLiteral("hasAnswerKey"), group.hasAnswerKey);
        settings.setValue(QStringLiteral("mineruZipPath"), group.mineruZipPath);
        settings.setValue(QStringLiteral("mineruAnswerZipPath"), group.mineruAnswerZipPath);
    }
    settings.endArray();
    settings.endGroup();
    settings.sync();
}

void removePersistedCloudTask(const QString& cacheDir, bool removeCachedResults) {
    QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
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
    pages_->addWidget(buildSourcePage());
    pages_->addWidget(buildProgressPage());
    pages_->addWidget(buildReviewPage());
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
    QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
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

QWidget* StudioWindow::buildReviewPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    layout->addWidget(pageHeader(
        QStringLiteral("第三步"), QStringLiteral("检查题目"),
        QStringLiteral("图片和扫描件已自动处理。只需处理标红的问题，也可以直接继续设置题库。")));
    auto* filterBar = new QFrame;
    filterBar->setObjectName(QStringLiteral("reviewFilterBar"));
    auto* filters = new QHBoxLayout(filterBar);
    filters->setContentsMargins(0, 0, 0, 0);
    filters->setSpacing(0);
    reviewFilterGroup_ = new QButtonGroup(this);
    reviewFilterGroup_->setExclusive(true);
    allReviewButton_ = new QPushButton(QStringLiteral("需要处理  0"));
    allQuestionsButton_ = new QPushButton(QStringLiteral("全部题目  0"));
    missingAnswerButton_ = new QPushButton(QStringLiteral("缺少答案  0"));
    duplicateButton_ = new QPushButton(QStringLiteral("疑似重复  0"));
    allQuestionsButton_->setProperty("reviewFilter", QString());
    allReviewButton_->setProperty("reviewFilter", QStringLiteral("__any_review__"));
    missingAnswerButton_->setProperty("reviewFilter", QStringLiteral("__missing_answer__"));
    duplicateButton_->setProperty("reviewFilter", QStringLiteral("__duplicate__"));
    for (auto* button : {allReviewButton_, allQuestionsButton_, missingAnswerButton_, duplicateButton_}) {
        button->setObjectName(QStringLiteral("reviewFilterTab"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        reviewFilterGroup_->addButton(button);
        filters->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, button] {
            activeReviewFilter_ = button->property("reviewFilter").toString();
            applyReviewFilter();
        });
    }
    filters->addStretch();
    layout->addWidget(filterBar);

    // 这里只解释是否可以继续。图片、扫描件和版式提示由程序自动处理，不再
    // 暴露成需要用户逐类确认的内部检测信号。
    riskCategoryPanel_ = new QFrame;
    riskCategoryPanel_->setObjectName(QStringLiteral("panel"));
    riskCategoryLayout_ = new QVBoxLayout(riskCategoryPanel_);
    riskCategoryLayout_->setContentsMargins(16, 12, 16, 12);
    riskCategoryLayout_->setSpacing(8);
    riskCategoryPanel_->setVisible(false);
    layout->addWidget(riskCategoryPanel_);

    auto* reviewSplit = new QSplitter(Qt::Horizontal);
    reviewSplit->setObjectName(QStringLiteral("reviewSplit"));
    reviewSplit->setChildrenCollapsible(false);
    auto* navigator = new QWidget;
    auto* navigatorLayout = new QVBoxLayout(navigator);
    navigatorLayout->setContentsMargins(0, 0, 0, 0);
    navigatorLayout->setSpacing(8);
    navigatorLayout->addWidget(mutedLabel(
        QStringLiteral("标红项需要决定：修正后收录，或不收录。其余题目已自动收录。")));
    reviewTree_ = new QTreeWidget;
    reviewTree_->header()->setObjectName(QStringLiteral("reviewTreeHeader"));
    reviewTree_->setColumnCount(2);
    reviewTree_->setHeaderLabels({QStringLiteral("材料 / 题目"), QStringLiteral("问题")});
    // 窄列表仍保留末尾的分套/同号序次，避免两个重号题被省略成相同文字。
    reviewTree_->setTextElideMode(Qt::ElideMiddle);
    reviewTree_->header()->setStretchLastSection(true);
    reviewTree_->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    reviewTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    reviewTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    reviewTree_->setAlternatingRowColors(false);
    reviewTree_->setRootIsDecorated(true);
    navigatorLayout->addWidget(reviewTree_, 1);
    reviewSplit->addWidget(navigator);

    auto* detail = new QFrame;
    detail->setObjectName(QStringLiteral("panel"));
    auto* detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(18, 16, 18, 16);
    detailLayout->setSpacing(10);
    reviewDetailTitle_ = new QLabel(QStringLiteral("选择左侧题目以预览"));
    reviewDetailTitle_->setObjectName(QStringLiteral("sectionTitle"));
    reviewDetailStatus_ = mutedLabel(
        QStringLiteral("待复核原因、完整题干和答案会显示在这里。"));
    reviewDetailStatus_->setWordWrap(true);
    detailLayout->addWidget(reviewDetailTitle_);
    detailLayout->addWidget(reviewDetailStatus_);
    reviewVisualPanel_ = new QWidget;
    reviewVisualLayout_ = new QVBoxLayout(reviewVisualPanel_);
    reviewVisualLayout_->setContentsMargins(0, 0, 0, 0);
    reviewVisualLayout_->setSpacing(8);
    reviewVisualPanel_->setVisible(false);
    detailLayout->addWidget(reviewVisualPanel_);
    reviewStemLabel_ = new QLabel(QStringLiteral("题干"));
    detailLayout->addWidget(reviewStemLabel_);
    reviewStemEditor_ = new QTextEdit;
    reviewStemEditor_->setObjectName(QStringLiteral("reviewStemEditor"));
    reviewStemEditor_->setPlaceholderText(QStringLiteral("题干"));
    reviewStemEditor_->setMinimumHeight(58);
    reviewStemEditor_->setMaximumHeight(240);
    reviewStemEditor_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailLayout->addWidget(reviewStemEditor_);
    manualMaterialUnderlineButton_ = new QPushButton(QStringLiteral("修正下划线 / 填空"));
    manualMaterialUnderlineButton_->setObjectName(QStringLiteral("secondaryButton"));
    manualMaterialUnderlineButton_->setVisible(false);
    detailLayout->addWidget(manualMaterialUnderlineButton_, 0, Qt::AlignLeft);
    reviewQuestionEditorPanel_ = new QWidget;
    auto* questionEditorLayout = new QVBoxLayout(reviewQuestionEditorPanel_);
    questionEditorLayout->setContentsMargins(0, 0, 0, 0);
    questionEditorLayout->setSpacing(10);
    auto* optionsHeader = new QHBoxLayout;
    optionsHeader->addWidget(new QLabel(QStringLiteral("选项")));
    optionsHeader->addStretch();
    auto* addOptionButton = new QPushButton(QStringLiteral("＋ 添加选项"));
    addOptionButton->setObjectName(QStringLiteral("secondaryButton"));
    optionsHeader->addWidget(addOptionButton);
    questionEditorLayout->addLayout(optionsHeader);
    reviewOptionsPanel_ = new QWidget;
    reviewOptionsLayout_ = new QVBoxLayout(reviewOptionsPanel_);
    reviewOptionsLayout_->setContentsMargins(0, 0, 0, 0);
    reviewOptionsLayout_->setSpacing(6);
    questionEditorLayout->addWidget(reviewOptionsPanel_);
    reviewAnswerLabel_ = new QLabel(QStringLiteral("正确答案"));
    questionEditorLayout->addWidget(reviewAnswerLabel_);
    reviewAnswerEditor_ = new QLineEdit;
    reviewAnswerEditor_->setPlaceholderText(QStringLiteral("例如 A；多选题填写 A、B"));
    questionEditorLayout->addWidget(reviewAnswerEditor_);
    reviewSolutionLabel_ = new QLabel(QStringLiteral("解析（可留空）"));
    questionEditorLayout->addWidget(reviewSolutionLabel_);
    reviewSolutionEditor_ = new QPlainTextEdit;
    reviewSolutionEditor_->setPlaceholderText(QStringLiteral("解析"));
    reviewSolutionEditor_->setMinimumHeight(80);
    questionEditorLayout->addWidget(reviewSolutionEditor_);
    auto* actions = new QHBoxLayout;
    confirmReviewButton_ = new QPushButton(QStringLiteral("保存并收录"));
    confirmReviewButton_->setObjectName(QStringLiteral("primaryButton"));
    excludeReviewButton_ = new QPushButton(QStringLiteral("不收录本题"));
    excludeReviewButton_->setObjectName(QStringLiteral("dangerTextButton"));
    actions->addWidget(excludeReviewButton_);
    actions->addStretch();
    actions->addWidget(confirmReviewButton_);
    questionEditorLayout->addLayout(actions);
    detailLayout->addWidget(reviewQuestionEditorPanel_);
    auto* detailScroll = new QScrollArea;
    detailScroll->setObjectName(QStringLiteral("reviewDetailScroll"));
    detailScroll->setWidgetResizable(true);
    detailScroll->setFrameShape(QFrame::NoFrame);
    detailScroll->setWidget(detail);
    reviewSplit->addWidget(detailScroll);
    reviewSplit->setStretchFactor(0, 4);
    reviewSplit->setStretchFactor(1, 6);
    reviewSplit->setSizes({360, 540});
    layout->addWidget(reviewSplit, 1);
    connect(reviewTree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                // 切题前把当前打开且改过的题提交回去；草稿无法保存时允许用户
                // 取消切换，避免编辑器里的修改被静默丢弃。
                if (currentReviewItem_ && currentReviewItem_ != current &&
                    !commitOpenReviewQuestion(QStringLiteral("切换题目"))) {
                    reviewTree_->blockSignals(true);
                    reviewTree_->setCurrentItem(currentReviewItem_);
                    reviewTree_->blockSignals(false);
                    return;
                }
                showReviewQuestion(current);
            });
    connect(confirmReviewButton_, &QPushButton::clicked, this,
            &StudioWindow::confirmCurrentReviewQuestion);
    connect(excludeReviewButton_, &QPushButton::clicked, this,
            &StudioWindow::excludeCurrentReviewQuestion);
    connect(addOptionButton, &QPushButton::clicked, this, [this] { addReviewOption(); });
    connect(reviewStemEditor_, &QTextEdit::textChanged,
            this, &StudioWindow::updateReviewStemHeight);
    connect(manualMaterialUnderlineButton_, &QPushButton::clicked, this,
            &StudioWindow::addManualMaterialUnderline);
    confirmReviewButton_->setEnabled(false);
    excludeReviewButton_->setEnabled(false);
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
        hasAnswerKeyByQuestion_.insert(absolute, true);
        ++added;
        auto* row = new SourceRowWidget(absolute);
        sourceRows_.insert(absolute, row);
        // 插入到末尾的拉伸占位之前，保持新行始终追加在列表最下方。
        sourceListLayout_->insertWidget(sourceListLayout_->count() - 1, row);
        connect(row, &SourceRowWidget::hasAnswerKeyChanged, this,
                [this, absolute](bool hasAnswerKey) {
            hasAnswerKeyByQuestion_.insert(absolute, hasAnswerKey);
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
    hasAnswerKeyByQuestion_.insert(question, true);
    if (auto* row = sourceRows_.value(question)) row->setPairedAnswer(answer);
}

void StudioWindow::removeSource(const QString& question) {
    sourcePaths_.removeAll(question);
    answerPathsByQuestion_.remove(question);
    hasAnswerKeyByQuestion_.remove(question);
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
    const bool cloudActive = mineruJob_ &&
        mineruJob_->stage() != MineruStage::Idle &&
        mineruJob_->stage() != MineruStage::Done &&
        mineruJob_->stage() != MineruStage::Failed &&
        mineruJob_->stage() != MineruStage::Cancelled;
    if (pages_->currentIndex() == 1 && cloudActive && !cloudBatchId_.isEmpty()) {
        persistCloudTask();
        closePreservingCloudTask_ = true;
        close();
        return;
    }
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
    const bool resumableCloud = page == 1 && cloudActive && !cloudBatchId_.isEmpty();
    backButton_->setVisible(page > 0 && (!cloudActive && !workflowActive || resumableCloud));
    backButton_->setText(resumableCloud ? QStringLiteral("后台等待并关闭")
                                        : QStringLiteral("上一步"));
    nextButton_->setVisible(page == 0 || page == 2);
    startButton_->setVisible(page == 1 || page == 3);
    nextButton_->setEnabled(page != 0 || !sourcePaths_.isEmpty());
    if (page == 0) {
        nextButton_->setText(mineruConfig_.cloudEnabled
            ? QStringLiteral("开始智能解析  →") : QStringLiteral("开始规则解析  →"));
    } else if (page == 2) {
        int selected = 0;
        for (QTreeWidgetItemIterator it(reviewTree_); *it; ++it)
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
        if (generatedQuestions_.isEmpty() && reviewQuestions_.isEmpty()) {
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
    const bool hasAnswerKey = hasAnswerKeyByQuestion_.value(sourcePaths_.first(), true);
    for (const QString& question : sourcePaths_) {
        if (hasAnswerKeyByQuestion_.value(question, true) != hasAnswerKey) {
            QMessageBox::warning(this, QStringLiteral("请统一答案设置"),
                                 QStringLiteral("同一题库暂不能混合“有答案”和“无答案”资料。"
                                                "请在文件列表中统一选择答案位置后再整理。"));
            activityTimer_->stop();
            activitySpinner_->hide();
            updateNavigation();
            return;
        }
        groups.append({question, answerPathsByQuestion_.value(question), hasAnswerKey, {}, {}});
    }
    startCloudParseThenGenerate(groups);
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
    // Win7 兼容构建不含 Qt PDF，文件选择器也只接受 TXT/Markdown/DOCX，
    // 这些格式本机解析已经是无损的，云端版面识别没有用武之地。
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
        QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
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
    const int value = qBound(0, base + within, 100);
    progressBar_->setValue(value);
    progressStatus_->setText(QStringLiteral("%1%").arg(value));
    phaseLabel_->setText(phase);
    phaseDetail_->setText(progress.detail);
    sourceCount_->setText(QString::number(progress.completedSourceBlocks));
}

void StudioWindow::populateReview(const GeneratedBankCandidate& candidate) {
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("review-populated"),
        {{QStringLiteral("materials"), candidate.materials.size()},
         {QStringLiteral("accepted"), candidate.questions.size()},
         {QStringLiteral("needsReview"), candidate.needsReviewQuestions.size()}});
#ifdef QUIZPANE_VERBOSE_DIAGNOSTICS
    diagnostic::payload(QStringLiteral("studio"), QStringLiteral("candidate"),
        QStringLiteral("materials-and-questions"),
        QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("materials"), candidate.materials},
            {QStringLiteral("questions"), candidate.questions},
            {QStringLiteral("needsReviewQuestions"), candidate.needsReviewQuestions}})
                .toJson(QJsonDocument::Compact)),
        128 * 1024);
#endif
    generatedMaterials_ = candidate.materials;
    generatedQuestions_ = candidate.questions;
    reviewQuestions_ = candidate.needsReviewQuestions;
    generatedAssets_ = candidate.assets;
    reviewSourceImages_ = candidate.reviewSourceImages;
    reviewAssets_ = candidate.reviewAssets;
    generatedHasAnswerKey_ = candidate.hasAnswerKey;
    generatedCount_->setText(QString::number(generatedQuestions_.size()));
    reviewCount_->setText(QString::number(reviewQuestions_.size()));
    activeReviewFilter_.clear();
    allQuestionsButton_->setChecked(true);
    // 重建树前先把指向旧节点的指针置空，clear() 会销毁旧节点，
    // 切题钩子还会解引用 currentReviewItem_，留着就是悬垂指针。
    currentReviewItem_ = nullptr;
    currentMaterialItem_ = nullptr;
    reviewTree_->clear();
    int hardReviewCount = 0;
    int automaticallyIncludedCount = 0;
    QHash<QString, QTreeWidgetItem*> groups;
    for (const auto& value : generatedMaterials_) {
        const QJsonObject material = value.toObject();
        const QString id = material.value("id").toString();
        const QString title = material.value("title").toString(id);
        const QJsonObject review = material.value("review").toObject();
        const bool isSoftRisk = review.value("needsReview").toBool() &&
                                review.value("riskLevel").toString() == QStringLiteral("soft");
        QStringList signalList;
        for (const QJsonValue& signal : review.value("signals").toArray())
            signalList.append(signal.toString());
        auto* item = new QTreeWidgetItem(reviewTree_, {title,
            isSoftRisk ? QStringLiteral("资料待复核") : QStringLiteral("共享材料")});
        item->setData(0, Qt::UserRole, material);
        item->setData(0, Qt::UserRole + 1, signalList);
        item->setData(0, Qt::UserRole + 2, false);
        item->setData(0, Qt::UserRole + 3, isSoftRisk);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        item->setCheckState(0, Qt::Checked);
        if (isSoftRisk) item->setForeground(1, QColor(QStringLiteral("#d9a441")));
        groups.insert(id, item);
    }
    auto* independent = new QTreeWidgetItem(reviewTree_,
        {QStringLiteral("独立题目"), QStringLiteral("不引用共享材料")});
    independent->setFlags(independent->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
    independent->setCheckState(0, Qt::Checked);
    QTreeWidgetItem* brokenReferences = nullptr;

    int missingAnswers = 0;
    int duplicates = 0;
    const auto appendQuestions = [&](const QJsonArray& questions) {
        for (const auto& value : questions) {
            const QJsonObject question = value.toObject();
            const QJsonObject review = question.value("review").toObject();
            const bool needsReview = review.value("needsReview").toBool();
            const QString riskLevel = review.value("riskLevel").toString();
            const bool isHardRisk = needsReview && riskLevel != QStringLiteral("soft");
            const bool isSoftRisk = needsReview && riskLevel == QStringLiteral("soft");
            if (isHardRisk) ++hardReviewCount;
            else ++automaticallyIncludedCount;
            const QString reason = review.value("reason").toString();
            if (isHardRisk && reason.contains(QStringLiteral("答案"))) ++missingAnswers;
            if (isHardRisk && reason.contains(QStringLiteral("重复"))) ++duplicates;
            QStringList signalList;
            for (const auto& signal : review.value("signals").toArray())
                signalList.append(signal.toString());
            const QString materialId = question.value("materialId").toString();
            QTreeWidgetItem* parent = independent;
            if (!materialId.isEmpty()) {
                parent = groups.value(materialId, nullptr);
                if (!parent) {
                    if (!brokenReferences) {
                        brokenReferences = new QTreeWidgetItem(reviewTree_,
                            {QStringLiteral("引用断裂"), QStringLiteral("必须丢弃或修正")});
                        brokenReferences->setFlags(brokenReferences->flags() |
                            Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
                        brokenReferences->setCheckState(0, Qt::Checked);
                    }
                    parent = brokenReferences;
                }
            }
            QString statusText;
            if (isHardRisk) statusText = reason.left(240);
            else if (isSoftRisk) {
                statusText.clear();
            } else {
                statusText.clear();
            }
            const QString questionLabel = reviewQuestionTitle(question);
            auto* item = new QTreeWidgetItem(parent, {questionLabel, statusText});
            item->setToolTip(0, questionLabel);
            item->setTextAlignment(0, Qt::AlignLeft | Qt::AlignVCenter);
            item->setTextAlignment(1, Qt::AlignLeft | Qt::AlignVCenter);
            item->setData(0, Qt::UserRole, question);
            item->setData(0, Qt::UserRole + 1, signalList);
            item->setData(0, Qt::UserRole + 2, isHardRisk);
            item->setData(0, Qt::UserRole + 3, isSoftRisk);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            // 图片、OCR 来源和版式线索属于程序已经处理过的提示，默认收录；
            // 只有答案缺失、重复冲突等实质问题等待用户决定。
            item->setCheckState(0, isHardRisk ? Qt::Unchecked : Qt::Checked);
            if (isHardRisk) item->setForeground(1, QColor(QStringLiteral("#c94b4b")));
        }
    };
    appendQuestions(generatedQuestions_);
    appendQuestions(reviewQuestions_);
    if (independent->childCount() == 0) delete independent;
    reviewTree_->expandToDepth(0);
    allQuestionsButton_->setText(QStringLiteral("全部题目  %1")
        .arg(generatedQuestions_.size() + reviewQuestions_.size()));
    allReviewButton_->setText(QStringLiteral("需要处理  %1").arg(hardReviewCount));
    allReviewButton_->setToolTip(QStringLiteral("只显示会影响题库内容、需要你决定的问题。"));
    missingAnswerButton_->setText(QStringLiteral("缺少答案  %1").arg(missingAnswers));
    missingAnswerButton_->setVisible(generatedHasAnswerKey_);
    duplicateButton_->setText(QStringLiteral("疑似重复  %1").arg(duplicates));

    reviewCount_->setText(QString::number(hardReviewCount));

    // 单行结果说明替代原来的风险分类、批量按钮和多段操作说明。
    clearLayout(riskCategoryLayout_);
    reviewSummary_ = new QLabel;
    reviewSummary_->setObjectName(QStringLiteral("reviewSummary"));
    reviewSummary_->setWordWrap(true);
    if (hardReviewCount == 0) {
        reviewSummary_->setText(QStringLiteral("✓ 已自动收录 %1 项，没有必须处理的问题，可直接继续。")
            .arg(automaticallyIncludedCount));
    } else {
        reviewSummary_->setText(QStringLiteral(
            "已自动收录 %1 项；还有 %2 项需要决定。未处理的题不会收录，不影响继续。")
            .arg(automaticallyIncludedCount).arg(hardReviewCount));
    }
    riskCategoryLayout_->addWidget(reviewSummary_);
    riskCategoryPanel_->setVisible(true);
    missingAnswerButton_->setVisible(false);
    duplicateButton_->setVisible(false);
    activeReviewFilter_ = hardReviewCount > 0 ? QStringLiteral("__any_review__") : QString();
    applyReviewFilter();
}

void StudioWindow::displayReviewAssets(const QList<QJsonObject>& assets) {
    // titleRow 是嵌套布局。只 delete 外层 QLayoutItem 会遗留其中的按钮，切换
    // 材料后就会把页选择器和“手动修正”按钮一组组叠加到页面上。
    clearLayout(reviewVisualLayout_);
    QList<QJsonObject> validAssets;
    for (const QJsonObject& asset : assets) {
        const QString path = asset.value(QStringLiteral("path")).toString();
        const QByteArray bytes = reviewAssets_.contains(path)
            ? reviewAssets_.value(path) : generatedAssets_.value(path);
        QPixmap pixmap;
        if (path.isEmpty() || bytes.isEmpty() || !pixmap.loadFromData(bytes, "PNG"))
            continue;
        validAssets.append(asset);
    }
    if (validAssets.isEmpty()) {
        reviewVisualPanel_->setVisible(false);
        return;
    }

    // 跨页材料以前会按页重复“原卷材料版式 / 手动修正”。现在把所有
    // 图片收进一个页选择器，操作仅作用于当前页，标题和按钮只保留一套。
    auto* titleRow = new QHBoxLayout;
    auto* title = mutedLabel(validAssets.first().value(QStringLiteral("alt")).toString());
    titleRow->addWidget(title, 1);
    QComboBox* picker = nullptr;
    if (validAssets.size() > 1) {
        picker = new QComboBox;
        for (int index = 0; index < validAssets.size(); ++index) {
            const int sourcePage = validAssets.at(index).value(QStringLiteral("sourcePage")).toInt();
            picker->addItem(sourcePage > 0
                ? QStringLiteral("原卷第 %1 页").arg(sourcePage)
                : QStringLiteral("图片 %1").arg(index + 1));
        }
        titleRow->addWidget(picker);
    }
    auto* recrop = new QPushButton(QStringLiteral("手动修正"));
    recrop->setObjectName(QStringLiteral("reviewActionButton"));
    titleRow->addWidget(recrop);
    reviewVisualLayout_->addLayout(titleRow);

    auto* image = new QLabel;
    image->setAlignment(Qt::AlignCenter);
    reviewVisualLayout_->addWidget(image);
    const auto selectedAsset = [validAssets, picker] {
        return validAssets.at(picker ? picker->currentIndex() : 0);
    };
    const auto showAsset = [this, validAssets, title, image, recrop](int index) {
        const QJsonObject asset = validAssets.at(index);
        QPixmap pixmap;
        const QString path = asset.value(QStringLiteral("path")).toString();
        const QByteArray bytes = reviewAssets_.contains(path)
            ? reviewAssets_.value(path) : generatedAssets_.value(path);
        pixmap.loadFromData(bytes, "PNG");
        image->setPixmap(pixmap.scaledToWidth(520, Qt::SmoothTransformation));
        image->setToolTip(asset.value(QStringLiteral("path")).toString());
        title->setText(asset.value(QStringLiteral("alt")).toString());
        const bool canRecrop = !asset.value(QStringLiteral("reviewOnly")).toBool() &&
            asset.value(QStringLiteral("sourcePage")).toInt() > 0 &&
            !asset.value(QStringLiteral("sourceDocument")).toString().isEmpty();
        recrop->setVisible(canRecrop);
        recrop->setEnabled(canRecrop);
    };
    showAsset(0);
    if (picker)
        connect(picker, qOverload<int>(&QComboBox::currentIndexChanged), this, showAsset);
    connect(recrop, &QPushButton::clicked, this,
            [this, selectedAsset] { recropReviewAsset(selectedAsset()); });
    reviewVisualPanel_->setVisible(true);
}

void StudioWindow::recropReviewAsset(const QJsonObject& asset) {
    const QString documentName = asset.value(QStringLiteral("sourceDocument")).toString();
    const int page = asset.value(QStringLiteral("sourcePage")).toInt();
    QString sourcePath;
    for (const QString& candidate : sourcePaths_) {
        if (QFileInfo(candidate).fileName() == documentName) {
            sourcePath = candidate;
            break;
        }
    }
    if (sourcePath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("找不到原卷"),
            QStringLiteral("找不到“%1”。请重新添加原题 PDF 后再次整理。").arg(documentName));
        return;
    }
    QString error;
    const QImage pageImage = renderPdfReviewPage(sourcePath, page, &error);
    if (pageImage.isNull()) {
        QMessageBox::warning(this, QStringLiteral("无法重新裁切"), error);
        return;
    }
    const QRectF automaticCrop = cropRectFromJson(asset.value(QStringLiteral("autoCrop")).toObject());
    const QRectF initialCrop = cropRectFromJson(asset.value(QStringLiteral("crop")).toObject());
    const QRectF fallbackCrop(0.05, 0.05, 0.90, 0.90);
    const QRectF automatic = automaticCrop.isEmpty() ? fallbackCrop : automaticCrop;
    const QRectF contextAnchor = initialCrop.isEmpty() ? automatic : initialCrop;
    CropDialog dialog(pageImage, cropContextAround(contextAnchor), contextAnchor, this);
    if (!initialCrop.isEmpty())
        dialog.setSelection(initialCrop);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (commitReviewCrop(asset, pageImage, dialog.selection()))
        reviewDetailStatus_->setText(QStringLiteral("已从原卷第 %1 页更新截图；保存草稿后再确认本题。").arg(page));
}

bool StudioWindow::commitReviewCrop(const QJsonObject& asset, const QImage& pageImage,
                                    const QRectF& normalizedCrop) {
    const QRect crop(qFloor(normalizedCrop.x() * pageImage.width()),
                     qFloor(normalizedCrop.y() * pageImage.height()),
                     qMax(1, qCeil(normalizedCrop.width() * pageImage.width())),
                     qMax(1, qCeil(normalizedCrop.height() * pageImage.height())));
    const QImage clipped = pageImage.copy(crop.intersected(pageImage.rect()));
    QByteArray png;
    QBuffer buffer(&png);
    if (clipped.isNull() || !buffer.open(QIODevice::WriteOnly) || !clipped.save(&buffer, "PNG")) {
        QMessageBox::warning(this, QStringLiteral("无法重新裁切"), QStringLiteral("无法保存新截图。"));
        return false;
    }
    const QString path = asset.value(QStringLiteral("path")).toString();
    generatedAssets_.insert(path, png);
    QJsonObject replacement = asset;
    replacement.insert(QStringLiteral("crop"), cropRectToJson(normalizedCrop));
    QTreeWidgetItem* item = reviewTree_->currentItem();
    if (!item) return false;
    QJsonObject entry = item->data(0, Qt::UserRole).toJsonObject();
    const auto replaceAsset = [&replacement, &path](QJsonObject* owner, const QString& key) {
        QJsonObject image = owner->value(key).toObject();
        if (image.value(QStringLiteral("path")).toString() != path)
            return false;
        owner->insert(key, replacement);
        return true;
    };
    if (entry.contains(QStringLiteral("body"))) {
        QJsonArray images = entry.value(QStringLiteral("images")).toArray();
        for (int index = 0; index < images.size(); ++index) {
            if (images.at(index).toObject().value(QStringLiteral("path")).toString() == path)
                images[index] = replacement;
        }
        entry.insert(QStringLiteral("images"), images);
        const QString id = entry.value(QStringLiteral("id")).toString();
        for (int index = 0; index < generatedMaterials_.size(); ++index) {
            if (generatedMaterials_.at(index).toObject().value(QStringLiteral("id")).toString() == id)
                generatedMaterials_[index] = entry;
        }
    } else {
        replaceAsset(&entry, QStringLiteral("stemImage"));
        QJsonArray options = entry.value(QStringLiteral("options")).toArray();
        for (int index = 0; index < options.size(); ++index) {
            QJsonObject option = options.at(index).toObject();
            if (replaceAsset(&option, QStringLiteral("image")))
                options[index] = option;
        }
        entry.insert(QStringLiteral("options"), options);
    }
    item->setData(0, Qt::UserRole, entry);
    showReviewQuestion(item);
    return true;
}



void StudioWindow::setReviewOptions(const QJsonArray& options) {
    QLayoutItem* child;
    while ((child = reviewOptionsLayout_->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }
    reviewOptionEditors_.clear();
    for (const QJsonValue& value : options) {
        const QJsonObject option = value.toObject();
        addReviewOption(option.value(QStringLiteral("id")).toString(),
                        option.value(QStringLiteral("text")).toString());
    }
}

void StudioWindow::addReviewOption(const QString& requestedId, const QString& text) {
    QString id = requestedId.trimmed().toLower();
    if (id.isEmpty()) {
        for (int index = 0; index < 26; ++index) {
            const QString candidate(QChar(u'a' + index));
            const bool used = std::any_of(reviewOptionEditors_.cbegin(), reviewOptionEditors_.cend(),
                [&candidate](const QLineEdit* editor) {
                    return editor->property("optionId").toString() == candidate;
                });
            if (!used) {
                id = candidate;
                break;
            }
        }
    }
    if (id.isEmpty())
        id = QStringLiteral("option-%1").arg(reviewOptionEditors_.size() + 1);
    auto* row = new QFrame;
    row->setObjectName(QStringLiteral("reviewOptionRow"));
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 5, 6, 5);
    layout->setSpacing(8);
    auto* badge = new QLabel(id.toUpper());
    badge->setObjectName(QStringLiteral("reviewOptionId"));
    badge->setFixedWidth(22);
    badge->setAlignment(Qt::AlignCenter);
    auto* editor = new QLineEdit(text);
    editor->setPlaceholderText(QStringLiteral("选项内容"));
    editor->setProperty("optionId", id);
    auto* remove = new QPushButton(QStringLiteral("删除"));
    remove->setObjectName(QStringLiteral("reviewOptionRemoveButton"));
    remove->setToolTip(QStringLiteral("删除此选项"));
    remove->setFixedWidth(52);
    layout->addWidget(badge);
    layout->addWidget(editor, 1);
    layout->addWidget(remove);
    reviewOptionsLayout_->addWidget(row);
    reviewOptionEditors_.append(editor);
    connect(remove, &QPushButton::clicked, this, [this, editor, row] {
        reviewOptionEditors_.removeAll(editor);
        reviewOptionsLayout_->removeWidget(row);
        row->deleteLater();
    });
}

QJsonArray StudioWindow::reviewOptions() const {
    QJsonArray result;
    for (const QLineEdit* editor : reviewOptionEditors_) {
        const QString text = editor->text().trimmed();
        if (text.isEmpty())
            return {};
        const QString id = editor->property("optionId").toString();
        result.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("text"), text}});
    }
    return result;
}

void StudioWindow::showReviewQuestion(QTreeWidgetItem* item) {
    reviewStemEditor_->setExtraSelections({});
    const QJsonObject entry = item ? item->data(0, Qt::UserRole).toJsonObject() : QJsonObject{};
    const bool isMaterial = entry.contains(QStringLiteral("body"));
    currentReviewItem_ = entry.isEmpty() || isMaterial ? nullptr : item;
    currentMaterialItem_ = isMaterial ? item : nullptr;
    const bool available = currentReviewItem_ != nullptr;
    confirmReviewButton_->setEnabled(available);
    excludeReviewButton_->setEnabled(available);
    if (!available) {
        reviewStemEditor_->setReadOnly(isMaterial);
        reviewQuestionEditorPanel_->setVisible(false);
        reviewAnswerEditor_->setReadOnly(isMaterial);
        reviewSolutionEditor_->setReadOnly(isMaterial);
        if (isMaterial) {
            const QStringList reviewSignals = item->data(0, Qt::UserRole + 1).toStringList();
            manualMaterialUnderlineButton_->setVisible(
                reviewSignals.contains(QStringLiteral("material-layout:underline-or-blank")) ||
                entry.value(QStringLiteral("body")).toString().contains(QStringLiteral("〔填空〕")) ||
                !entry.value(QStringLiteral("underlines")).toArray().isEmpty());
            reviewStemLabel_->setText(QStringLiteral("材料文本"));
            reviewDetailTitle_->setText(QStringLiteral("共享材料：%1")
                .arg(entry.value(QStringLiteral("title")).toString()));
            reviewDetailStatus_->setText(QStringLiteral(
                "这是原始共享材料。下方先显示保留版式的原卷截图，便于核对下划线、填空和图表。"));
            reviewStemEditor_->setHtml(materialPreviewHtml(
                entry.value(QStringLiteral("body")).toString(),
                entry.value(QStringLiteral("underlines")).toArray()));
            updateReviewStemHeight();
            setReviewOptions({});
            reviewAnswerEditor_->clear();
            reviewSolutionEditor_->clear();
            QList<QJsonObject> images;
            for (const QJsonValue& value : entry.value(QStringLiteral("images")).toArray())
                images.append(value.toObject());
            displayReviewAssets(images);
            return;
        }
        manualMaterialUnderlineButton_->setVisible(false);
        reviewDetailTitle_->setText(QStringLiteral("选择左侧题目以预览"));
        reviewDetailStatus_->setText(QStringLiteral("待复核原因、完整题干和答案会显示在这里。"));
        reviewStemLabel_->setText(QStringLiteral("题干"));
        reviewStemEditor_->clear();
        updateReviewStemHeight();
        setReviewOptions({});
        reviewAnswerEditor_->clear();
        reviewSolutionEditor_->clear();
        displayReviewAssets({});
        return;
    }

    const QStringList reviewSignals = item->data(0, Qt::UserRole + 1).toStringList();
    manualMaterialUnderlineButton_->setVisible(
        reviewSignals.contains(QStringLiteral("stem-layout:underline-or-blank")) ||
        entry.value(QStringLiteral("stem")).toString().contains(QStringLiteral("〔填空〕")) ||
        !entry.value(QStringLiteral("stemUnderlines")).toArray().isEmpty());
    reviewStemEditor_->setReadOnly(false);
    reviewStemLabel_->setText(QStringLiteral("题干"));
    reviewQuestionEditorPanel_->setVisible(true);
    reviewAnswerLabel_->setVisible(generatedHasAnswerKey_);
    reviewAnswerEditor_->setVisible(generatedHasAnswerKey_);
    reviewSolutionLabel_->setVisible(generatedHasAnswerKey_);
    reviewSolutionEditor_->setVisible(generatedHasAnswerKey_);
    reviewAnswerEditor_->setReadOnly(false);
    reviewSolutionEditor_->setReadOnly(false);
    const QJsonObject question = entry;
    reviewDetailTitle_->setText(reviewQuestionTitle(question));
    const QJsonObject review = question.value("review").toObject();
    QString status;
    const bool hardRisk = item->data(0, Qt::UserRole + 2).toBool();
    const bool softRisk = item->data(0, Qt::UserRole + 3).toBool();
    if (hardRisk)
        status = QStringLiteral("需要处理：%1").arg(review.value("reason").toString());
    else if (softRisk)
        status = QStringLiteral("已自动收录。图片、扫描内容和版式已处理；如有偏差可在下方修改。");
    else
        status = QStringLiteral("已自动收录，可直接继续。");
    reviewDetailStatus_->setText(status);
    confirmReviewButton_->setText(hardRisk ? QStringLiteral("保存并收录")
                                           : QStringLiteral("保存修改"));
    reviewStemEditor_->setPlainText(stemForReviewEditor(question.value("stem").toString()));
    QList<QTextEdit::ExtraSelection> underlineSelections;
    for (const auto& value : question.value("stemUnderlines").toArray()) {
        const auto range = value.toObject();
        QTextEdit::ExtraSelection selection;
        selection.cursor = reviewStemEditor_->textCursor();
        selection.cursor.setPosition(range.value("start").toInt());
        selection.cursor.setPosition(range.value("start").toInt() + range.value("length").toInt(), QTextCursor::KeepAnchor);
        selection.format.setFontUnderline(true);
        underlineSelections.append(selection);
    }
    reviewStemEditor_->setExtraSelections(underlineSelections);
    updateReviewStemHeight();
    setReviewOptions(question.value("options").toArray());
    if (generatedHasAnswerKey_) {
        QStringList answerIds;
        for (const QJsonValue& value : question.value("answer").toObject().value("optionIds").toArray())
            answerIds.append(value.toString());
        reviewAnswerEditor_->setText(answerIds.join(QStringLiteral(", ")));
        reviewSolutionEditor_->setPlainText(question.value("solution").toString());
    } else {
        reviewAnswerEditor_->clear();
        reviewSolutionEditor_->clear();
    }
    QList<QJsonObject> images;
    QSet<QString> imagePaths;
    const auto appendImage = [&images, &imagePaths](const QJsonObject& image) {
        const QString path = image.value(QStringLiteral("path")).toString();
        if (image.isEmpty() || path.isEmpty() || imagePaths.contains(path)) return;
        imagePaths.insert(path);
        images.append(image);
    };
    appendImage(reviewSourceImages_.value(question.value(QStringLiteral("id")).toString()));
    const QJsonObject stemImage = question.value(QStringLiteral("stemImage")).toObject();
    appendImage(stemImage);
    for (const QJsonValue& value : question.value(QStringLiteral("options")).toArray()) {
        const QJsonObject image = value.toObject().value(QStringLiteral("image")).toObject();
        appendImage(image);
    }
    displayReviewAssets(images);
}

void StudioWindow::addManualMaterialUnderline() {
    QTreeWidgetItem* target = currentMaterialItem_ ? currentMaterialItem_ : currentReviewItem_;
    if (!target) return;
    if (currentReviewItem_ && reviewQuestionIsDirty()) {
        QMessageBox::information(this, QStringLiteral("请先保存修改"),
            QStringLiteral("请先保存题干和选项的修改，再标记下划线，以免位置发生偏移。"));
        return;
    }
    const bool isMaterial = currentMaterialItem_ != nullptr;
    const QString underlineKey = isMaterial ? QStringLiteral("underlines") : QStringLiteral("stemUnderlines");
    QJsonObject material = target->data(0, Qt::UserRole).toJsonObject();
    const QString body = material.value(isMaterial ? QStringLiteral("body") : QStringLiteral("stem")).toString();
    if (body.isEmpty())
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("修正下划线 / 填空"));
    dialog.setMinimumSize(620, 420);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->addWidget(mutedLabel(QStringLiteral(
        "如果文字被误写成“〔填空〕”，请先关闭此窗口并在题干中改回原文。"
        "然后选中需要带下划线的词句，点击“设为下划线”。")));
    auto* editor = new QPlainTextEdit;
    editor->setPlainText(body);
    editor->setReadOnly(true);
    editor->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(editor, 1);
    auto* buttons = new QDialogButtonBox;
    auto* add = buttons->addButton(QStringLiteral("设为下划线"), QDialogButtonBox::AcceptRole);
    add->setObjectName(QStringLiteral("primaryButton"));
    auto* cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    auto* clear = buttons->addButton(QStringLiteral("清除全部下划线"), QDialogButtonBox::ResetRole);
    const auto saveRanges = [&](const QJsonArray& ranges) {
        material.insert(underlineKey, ranges);
        target->setData(0, Qt::UserRole, material);
        if (isMaterial) {
            const QString id = material.value(QStringLiteral("id")).toString();
            for (int index = 0; index < generatedMaterials_.size(); ++index)
                if (generatedMaterials_.at(index).toObject().value(QStringLiteral("id")).toString() == id) {
                    generatedMaterials_[index] = material;
                    break;
                }
        }
        dialog.accept();
    };
    connect(clear, &QPushButton::clicked, &dialog, [&] { saveRanges({}); });
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(add, &QPushButton::clicked, &dialog, [&] {
        const QTextCursor cursor = editor->textCursor();
        const int start = cursor.selectionStart();
        const int length = cursor.selectionEnd() - start;
        if (length <= 0) {
            QMessageBox::information(&dialog, QStringLiteral("请先选择文字"),
                QStringLiteral("请在文本中选中一个词或一句话。"));
            return;
        }
        QList<QPair<int, int>> ranges;
        for (const QJsonValue& value : material.value(underlineKey).toArray()) {
            const QJsonObject range = value.toObject();
            const int rangeStart = range.value(QStringLiteral("start")).toInt(-1);
            const int rangeLength = range.value(QStringLiteral("length")).toInt();
            if (rangeStart >= 0 && rangeLength > 0 && rangeStart + rangeLength <= body.size())
                ranges.append({rangeStart, rangeLength});
        }
        ranges.append({start, length});
        std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        QJsonArray merged;
        for (const auto& range : ranges) {
            if (merged.isEmpty()) {
                merged.append(QJsonObject{{QStringLiteral("start"), range.first},
                                          {QStringLiteral("length"), range.second}});
                continue;
            }
            QJsonObject previous = merged.last().toObject();
            const int previousEnd = previous.value(QStringLiteral("start")).toInt() +
                previous.value(QStringLiteral("length")).toInt();
            if (range.first <= previousEnd) {
                previous.insert(QStringLiteral("length"),
                    qMax(previousEnd, range.first + range.second) -
                    previous.value(QStringLiteral("start")).toInt());
                merged[merged.size() - 1] = previous;
            } else {
                merged.append(QJsonObject{{QStringLiteral("start"), range.first},
                                          {QStringLiteral("length"), range.second}});
            }
        }
        saveRanges(merged);
    });
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted)
        showReviewQuestion(target);
}

bool StudioWindow::saveCurrentReviewQuestion() {
    if (!currentReviewItem_)
        return false;
    QJsonObject question = currentReviewItem_->data(0, Qt::UserRole).toJsonObject();
    const QString stem = stemFromReviewEditor(reviewStemEditor_->toPlainText()).trimmed();
    if (stem.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法保存草稿"), QStringLiteral("题干不能为空。"));
        return false;
    }

    const QJsonArray options = reviewOptions();
    if (options.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法保存草稿"), QStringLiteral("至少需要一个选项。"));
        return false;
    }
    if (stem != question.value(QStringLiteral("stem")).toString()) {
        if (!question.value(QStringLiteral("stemUnderlines")).toArray().isEmpty() &&
            QMessageBox::question(this, QStringLiteral("请重新核对下划线"),
                QStringLiteral("题干已修改，原下划线位置可能失效。保存后将清除旧标记，"
                               "可用“手动标记下划线”重新设置。继续保存吗？"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return false;
        // 编辑题干后旧偏移不再可信，不能把下划线静默移到另一个词上。
        question.remove(QStringLiteral("stemUnderlines"));
        reviewStemEditor_->setExtraSelections({});
    }
    question.insert(QStringLiteral("stem"), stem);
    question.insert(QStringLiteral("options"), options);
    if (generatedHasAnswerKey_) {
        QJsonArray answerIds;
        const QStringList rawAnswerIds = reviewAnswerEditor_->text().split(
            QRegularExpression(QStringLiteral("[,，\\s]+")), Qt::SkipEmptyParts);
        for (const QString& id : rawAnswerIds)
            answerIds.append(id.trimmed().toLower());
        if (answerIds.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("无法保存草稿"), QStringLiteral("请填写答案选项 ID。"));
            return false;
        }
        question.insert(QStringLiteral("answer"), QJsonObject{{QStringLiteral("optionIds"), answerIds}});
        question.insert(QStringLiteral("solution"), reviewSolutionEditor_->toPlainText().trimmed());
    } else {
        question.remove(QStringLiteral("answer"));
        question.remove(QStringLiteral("solution"));
    }

    QJsonArray materials;
    const QString materialId = question.value("materialId").toString();
    if (!materialId.isEmpty()) {
        for (const QJsonValue& value : generatedMaterials_) {
            if (value.toObject().value("id").toString() == materialId) {
                materials.append(value);
                break;
            }
        }
    }
    const QJsonObject bank = makeReviewDraftBank(question, materials, generatedHasAnswerKey_);
    QString error;
    if (!quizpane::validateBank(bank, &error)) {
        QMessageBox::warning(this, QStringLiteral("草稿尚不完整"),
                             QStringLiteral("请修正后再保存：%1").arg(error));
        return false;
    }
    currentReviewItem_->setData(0, Qt::UserRole, question);
    const bool needsReview = question.value("review").toObject().value("needsReview").toBool();
    currentReviewItem_->setText(1, needsReview
        ? QStringLiteral("草稿已保存 · 仍待确认") : QStringLiteral("已人工确认"));
    reviewDetailStatus_->setText(needsReview
        ? QStringLiteral("草稿已保存。确认本题后才会进入最终题库。")
        : QStringLiteral("已保存并确认，将进入最终题库。"));
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("review-draft-saved"),
        {{QStringLiteral("questionId"), question.value("id").toString()}});
    return true;
}

bool StudioWindow::reviewQuestionIsDirty() const {
    if (!currentReviewItem_)
        return false;
    const QJsonObject question = currentReviewItem_->data(0, Qt::UserRole).toJsonObject();
    if (question.isEmpty())
        return false;
    // 与 saveCurrentReviewQuestion 的写入口径对齐：只比较保存后会发生变化的字段，
    // 没动过编辑器的题不因格式差异（如题干尾部空白）被误判为脏。
    if (stemFromReviewEditor(reviewStemEditor_->toPlainText()).trimmed() !=
        question.value("stem").toString())
        return true;
    const QJsonArray options = reviewOptions();
    const QJsonArray savedOptions = question.value("options").toArray();
    if (options.size() != savedOptions.size())
        return true;
    for (int index = 0; index < options.size(); ++index) {
        const QJsonObject saved = savedOptions.at(index).toObject();
        const QJsonObject current = options.at(index).toObject();
        if (saved.value("id") != current.value("id") ||
            saved.value("text") != current.value("text"))
            return true;
    }
    if (generatedHasAnswerKey_) {
        QJsonArray editorAnswerIds;
        const QStringList rawAnswerIds = reviewAnswerEditor_->text().split(
            QRegularExpression(QStringLiteral("[,，\\s]+")), Qt::SkipEmptyParts);
        for (const QString& id : rawAnswerIds)
            editorAnswerIds.append(id.trimmed().toLower());
        const QJsonArray savedAnswerIds =
            question.value("answer").toObject().value("optionIds").toArray();
        if (editorAnswerIds != savedAnswerIds)
            return true;
        if (reviewSolutionEditor_->toPlainText().trimmed() !=
            question.value("solution").toString())
            return true;
    }
    return false;
}

bool StudioWindow::commitOpenReviewQuestion(const QString& consequence) {
    if (!currentReviewItem_ || !reviewQuestionIsDirty())
        return true;
    if (saveCurrentReviewQuestion())
        return true;
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this, QStringLiteral("有未保存的修改"),
        QStringLiteral("当前题目的草稿无法保存，%1将丢弃未保存的修改。\n仍要继续吗？")
            .arg(consequence),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return choice == QMessageBox::Yes;
}

void StudioWindow::confirmCurrentReviewQuestion() {
    const bool wasHardRisk = currentReviewItem_ &&
        currentReviewItem_->data(0, Qt::UserRole + 2).toBool();
    if (!saveCurrentReviewQuestion())
        return;
    QJsonObject question = currentReviewItem_->data(0, Qt::UserRole).toJsonObject();
    QJsonObject review = question.value("review").toObject();
    review.insert(QStringLiteral("needsReview"), false);
    review.insert(QStringLiteral("reason"), QStringLiteral("已人工确认"));
    question.insert(QStringLiteral("review"), review);
    currentReviewItem_->setData(0, Qt::UserRole, question);
    currentReviewItem_->setData(0, Qt::UserRole + 2, false);
    currentReviewItem_->setData(0, Qt::UserRole + 3, false);
    currentReviewItem_->setCheckState(0, Qt::Checked);
    currentReviewItem_->setText(1, QStringLiteral("已人工确认"));
    reviewDetailStatus_->setText(QStringLiteral("已确认；它会进入最终题库。"));
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("review-question-confirmed"),
        {{QStringLiteral("questionId"), question.value("id").toString()}});
    refreshReviewDecisionState();
    if (wasHardRisk)
        advanceToNextReviewIssue();
}

void StudioWindow::excludeCurrentReviewQuestion() {
    if (!currentReviewItem_)
        return;
    currentReviewItem_->setCheckState(0, Qt::Unchecked);
    currentReviewItem_->setData(0, Qt::UserRole + 2, false);
    currentReviewItem_->setData(0, Qt::UserRole + 3, false);
    currentReviewItem_->setText(1, QStringLiteral("暂不采用"));
    reviewDetailStatus_->setText(QStringLiteral("本题不会进入最终题库；重新勾选或确认本题即可恢复。"));
    diagnostic::event(QStringLiteral("studio"), QStringLiteral("review-question-excluded"),
        {{QStringLiteral("questionId"), currentReviewItem_->data(0, Qt::UserRole)
            .toJsonObject().value("id").toString()}});
    refreshReviewDecisionState();
    advanceToNextReviewIssue();
}

void StudioWindow::refreshReviewDecisionState() {
    int included = 0;
    int remaining = 0;
    for (QTreeWidgetItemIterator it(reviewTree_); *it; ++it) {
        QTreeWidgetItem* item = *it;
        const QJsonObject entry = item->data(0, Qt::UserRole).toJsonObject();
        if (entry.isEmpty() || entry.contains(QStringLiteral("body")))
            continue;
        if (item->data(0, Qt::UserRole + 2).toBool())
            ++remaining;
        else if (item->checkState(0) == Qt::Checked)
            ++included;
    }
    allReviewButton_->setText(QStringLiteral("需要处理  %1").arg(remaining));
    reviewCount_->setText(QString::number(remaining));
    if (reviewSummary_) {
        reviewSummary_->setText(remaining == 0
            ? QStringLiteral("✓ 已收录 %1 题，没有待处理问题，可以继续生成题库。").arg(included)
            : QStringLiteral("已收录 %1 题；还有 %2 题需要决定。未处理的题不会收录。")
                  .arg(included).arg(remaining));
    }
    if (remaining == 0) {
        activeReviewFilter_.clear();
        allQuestionsButton_->setChecked(true);
    }
    applyReviewFilter();
    updateNavigation();
}

void StudioWindow::advanceToNextReviewIssue() {
    for (QTreeWidgetItemIterator it(reviewTree_); *it; ++it) {
        QTreeWidgetItem* item = *it;
        if (!item->isHidden() && item->data(0, Qt::UserRole + 2).toBool()) {
            reviewTree_->blockSignals(true);
            reviewTree_->setCurrentItem(item);
            reviewTree_->blockSignals(false);
            showReviewQuestion(item);
            return;
        }
    }
    if (currentReviewItem_) {
        reviewTree_->blockSignals(true);
        reviewTree_->setCurrentItem(currentReviewItem_);
        reviewTree_->blockSignals(false);
        showReviewQuestion(currentReviewItem_);
    }
}



void StudioWindow::updateReviewStemHeight() {
    if (!reviewStemEditor_)
        return;
    const qreal documentHeight = reviewStemEditor_->document()->size().height();
    const int contentHeight = qCeil(documentHeight) + 18;
    constexpr int minimumHeight = 58;
    constexpr int maximumHeight = 240;
    const int height = qBound(minimumHeight, contentHeight, maximumHeight);
    reviewStemEditor_->setFixedHeight(height);
    reviewStemEditor_->setVerticalScrollBarPolicy(contentHeight > maximumHeight
        ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
}

void StudioWindow::applyReviewFilter() {
    for (auto* button : reviewFilterGroup_->buttons()) {
        if (button->property("reviewFilter").toString() == activeReviewFilter_)
            button->setChecked(true);
    }
    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) -> bool {
        const auto matches = [this](QTreeWidgetItem* candidate) {
            if (activeReviewFilter_.isEmpty()) return true;
            const bool hardRisk = candidate->data(0, Qt::UserRole + 2).toBool();
            const bool softRisk = candidate->data(0, Qt::UserRole + 3).toBool();
            if (activeReviewFilter_ == QStringLiteral("__any_review__"))
                return hardRisk;
            if (activeReviewFilter_ == QStringLiteral("__missing_answer__"))
                return hardRisk && candidate->text(1).contains(QStringLiteral("答案"));
            if (activeReviewFilter_ == QStringLiteral("__duplicate__"))
                return hardRisk && candidate->text(1).contains(QStringLiteral("重复"));
            static const QString signalPrefix = QStringLiteral("__signal:");
            if (activeReviewFilter_.startsWith(signalPrefix))
                return candidate->data(0, Qt::UserRole + 1).toStringList().contains(
                    activeReviewFilter_.mid(signalPrefix.size()));
            return true;
        };
        const bool selfMatches = matches(item);
        bool anyChildVisible = false;
        for (int index = 0; index < item->childCount(); ++index) {
            QTreeWidgetItem* child = item->child(index);
            if (visit(child)) anyChildVisible = true;
        }
        // 命中的是材料本身时，只显示材料行；命中子题时保留其父级路径。
        const bool visible = selfMatches || anyChildVisible;
        item->setHidden(!visible);
        return visible;
    };
    for (int index = 0; index < reviewTree_->topLevelItemCount(); ++index)
        visit(reviewTree_->topLevelItem(index));
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
    rememberSourceOrder(generatedQuestions_);
    rememberSourceOrder(reviewQuestions_);
    QSet<QString> usedMaterialIds;
    for (int topIndex = 0; topIndex < reviewTree_->topLevelItemCount(); ++topIndex) {
        QTreeWidgetItem* group = reviewTree_->topLevelItem(topIndex);
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
    for (const auto& value : generatedMaterials_) {
        const QJsonObject material = value.toObject();
        if (usedMaterialIds.contains(material.value("id").toString()))
            selectedMaterials.append(material);
    }
    QJsonObject bank{{"schemaVersion", 3}, {"title", title},
        {"answerPolicy", generatedHasAnswerKey_ ? QStringLiteral("included") : QStringLiteral("none")},
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
    for (auto it = generatedAssets_.cbegin(); it != generatedAssets_.cend(); ++it)
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
    QSettings practiceSettings(QStringLiteral("QuizPane Project"), QStringLiteral("小窗刷题"));
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
    if (closePreservingCloudTask_ && cloudActive && !cloudBatchId_.isEmpty()) {
        persistCloudTask();
        event->accept();
        return;
    }
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
