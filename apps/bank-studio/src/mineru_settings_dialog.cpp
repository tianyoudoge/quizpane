#include "mineru_settings_dialog.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QDate>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVBoxLayout>

namespace quizpane::studio {
namespace {

QLabel* mutedLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("muted"));
    label->setWordWrap(true);
    return label;
}

}  // namespace

std::optional<MineruConfig> editMineruSettings(QWidget* parent, const MineruConfig& current,
                                                const QString& notice) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("智能解析设置"));
    dialog.setMinimumWidth(580);
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(QStringLiteral("智能解析设置"));
    title->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(title);
    layout->addWidget(mutedLabel(QStringLiteral(
        "适合扫描件、图表和复杂版面。需要识别的文件会上传到 MinerU。")));
    if (!notice.trimmed().isEmpty()) {
        auto* problem = new QLabel(notice);
        problem->setObjectName(QStringLiteral("mineruTokenNotice"));
        problem->setWordWrap(true);
        layout->addWidget(problem);
    }

    auto* form = new QFormLayout;
    form->setSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto* token = new QLineEdit;
    token->setEchoMode(QLineEdit::Password);
    token->setText(current.token);
    token->setClearButtonEnabled(true);
    token->setPlaceholderText(QStringLiteral("粘贴 MinerU Token"));

    auto* modelVersion = new QComboBox;
    modelVersion->addItem(QStringLiteral("准确识别（推荐）"), QStringLiteral("vlm"));
    modelVersion->addItem(QStringLiteral("兼容模式（识别异常时使用）"),
                          QStringLiteral("pipeline"));
    const int savedModel = modelVersion->findData(current.modelVersion);
    modelVersion->setCurrentIndex(savedModel < 0 ? 0 : savedModel);

    auto* links = new QLabel(QStringLiteral(
        "<a href=\"https://mineru.net/apiManage/token\">获取 Token</a> · "
        "<a href=\"https://mineru.net/apiManage/docs\">使用说明</a>"));
    links->setOpenExternalLinks(true);
    links->setTextInteractionFlags(Qt::TextBrowserInteraction);

    form->addRow(QStringLiteral("MinerU Token"), token);
    form->addRow(QStringLiteral("识别方式"), modelVersion);
    form->addRow(QString(), links);
    layout->addLayout(form);

    auto* quotaPanel = new QLabel;
    quotaPanel->setObjectName(QStringLiteral("notice"));
    quotaPanel->setWordWrap(true);
    const auto localSubmittedFiles = [] {
        QSettings settings(QStringLiteral("QuizPane Project"), QStringLiteral("题库制作器"));
        return settings.value(QStringLiteral("question-maker/mineru/usage/%1/submittedFiles")
                                  .arg(QDate::currentDate().toString(Qt::ISODate)), 0).toInt();
    };
    quotaPanel->setText(QStringLiteral("今日云端用量\n本应用已提交：%1 个文件\n高优页数：点击“查询额度”获取")
                            .arg(localSubmittedFiles()));
    auto* refreshQuota = new QPushButton(QStringLiteral("查询额度"));
    refreshQuota->setObjectName(QStringLiteral("secondaryButton"));
    auto* quotaRow = new QHBoxLayout;
    quotaRow->addWidget(quotaPanel, 1);
    quotaRow->addWidget(refreshQuota, 0, Qt::AlignTop);
    layout->addLayout(quotaRow);
    auto* quotaHint = mutedLabel(QStringLiteral(
        "说明：MinerU 的实时高优页数查询仅对企业代管 Token 保证可用；"
        "文件项仅统计本应用今日提交数。"));
    layout->addWidget(quotaHint);
    auto* quotaManager = new QNetworkAccessManager(&dialog);
    QObject::connect(refreshQuota, &QPushButton::clicked, &dialog,
                     [token, quotaPanel, refreshQuota, quotaManager, localSubmittedFiles] {
        const QString value = token->text().trimmed();
        if (value.isEmpty()) {
            quotaPanel->setText(QStringLiteral("请先填写 Token，再查询高优页数额度。"));
            return;
        }
        refreshQuota->setEnabled(false);
        quotaPanel->setText(QStringLiteral("正在查询高优页数额度…"));
        QNetworkRequest request(QUrl(QStringLiteral("https://mineru.net/api/v4/quota")));
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + value.toUtf8());
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        auto* reply = quotaManager->get(request);
        QObject::connect(reply, &QNetworkReply::finished, quotaManager,
                         [reply, quotaPanel, refreshQuota, localSubmittedFiles] {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonDocument json = QJsonDocument::fromJson(reply->readAll());
            reply->deleteLater();
            refreshQuota->setEnabled(true);
            const QJsonObject data = json.object().value(QStringLiteral("data")).toObject();
            if (status < 400 && json.object().value(QStringLiteral("code")).toInt(-1) == 0 &&
                data.contains(QStringLiteral("user_left_quota"))) {
                quotaPanel->setText(QStringLiteral(
                    "今日云端用量\n本应用已提交：%1 个文件\n高优页数剩余：%2 页")
                    .arg(localSubmittedFiles())
                    .arg(data.value(QStringLiteral("user_left_quota")).toInt()));
                return;
            }
            // 不显示 HTTP、服务端 msg 或网络库错误，避免暴露服务实现细节。
            quotaPanel->setText(QStringLiteral(
                "今日云端用量\n本应用已提交：%1 个文件\n高优页数：暂时无法查询，可稍后再试")
                .arg(localSubmittedFiles()));
        });
    });

    auto* credit = new QLabel(QStringLiteral(
        "智能解析由 <a href=\"https://github.com/opendatalab/MinerU\">MinerU</a> 提供"
        " · OpenDataLab（上海人工智能实验室）"));
    credit->setOpenExternalLinks(true);
    credit->setTextInteractionFlags(Qt::TextBrowserInteraction);
    credit->setWordWrap(true);
    credit->setObjectName(QStringLiteral("muted"));
    layout->addStretch();
    layout->addWidget(credit);

    auto* buttons = new QDialogButtonBox;
    auto* cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    auto* useRules = buttons->addButton(QStringLiteral("改用规则解析"), QDialogButtonBox::ActionRole);
    auto* save = buttons->addButton(QStringLiteral("保存设置"), QDialogButtonBox::AcceptRole);
    cancel->setObjectName(QStringLiteral("dialogCancelButton"));
    useRules->setObjectName(QStringLiteral("secondaryButton"));
    save->setObjectName(QStringLiteral("primaryButton"));
    // Token 是智能模式真正开始工作的前提。空值不能被当作“保存后改用规则模式”，
    // 否则用户刚选择智能模式就会被静默改回去。
    save->setEnabled(!token->text().trimmed().isEmpty());
    QObject::connect(token, &QLineEdit::textChanged, &dialog, [token, save] {
        save->setEnabled(!token->text().trimmed().isEmpty());
    });
    layout->addWidget(buttons);
    bool choseRules = false;
    QObject::connect(save, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(useRules, &QPushButton::clicked, &dialog, [&dialog, &choseRules] {
        choseRules = true;
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;

    MineruConfig result = current;
    if (choseRules) {
        result.token.clear();
        result.cloudEnabled = false;
        result.modeSelectedByUser = true;
        return result;
    }
    result.token = token->text().trimmed();
    result.modelVersion = modelVersion->currentData().toString();
    // 是否扫描件由 MinerU 自动判断；不再让用户为每一批资料做全局选择。
    result.isOcr = false;
    result.cloudEnabled = true;
    result.modeSelectedByUser = true;
    return result;
}

}  // namespace quizpane::studio
