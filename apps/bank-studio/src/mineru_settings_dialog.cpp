#include "mineru_settings_dialog.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

std::optional<MineruConfig> editMineruSettings(QWidget* parent, const MineruConfig& current) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("智能解析设置"));
    dialog.setMinimumWidth(580);
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(QStringLiteral("智能解析设置"));
    title->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(title);
    layout->addWidget(mutedLabel(QStringLiteral(
        "适合扫描件、图表和复杂版面。需要识别的文件会上传到 MinerU。")));

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
    auto* save = buttons->addButton(QStringLiteral("保存设置"), QDialogButtonBox::AcceptRole);
    cancel->setObjectName(QStringLiteral("dialogCancelButton"));
    save->setObjectName(QStringLiteral("primaryButton"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;

    MineruConfig result;
    result.token = token->text().trimmed();
    result.modelVersion = modelVersion->currentData().toString();
    // 是否扫描件由 MinerU 自动判断；不再让用户为每一批资料做全局选择。
    result.isOcr = false;
    result.cloudEnabled = !result.token.isEmpty();
    return result;
}

}  // namespace quizpane::studio
