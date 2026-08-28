#include "mineru_settings_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
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
    dialog.setWindowTitle(QStringLiteral("云端解析配置"));
    dialog.setMinimumWidth(620);
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(QStringLiteral("云端解析配置"));
    title->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(title);
    // 这里是唯一提到具体服务商的地方：产品界面只说"本机/云端"，服务商属于
    // 配置细节，也该在这里给出应有的署名。
    layout->addWidget(mutedLabel(QStringLiteral(
        "不填也能用：题库制作器默认在本机解析文档，全程离线。"
        "复杂版面（扫描件、统计图表、图形选项）可以改用云端解析提高识别率。\n"
        "云端版面识别由开源项目 MinerU 提供。")));

    auto* form = new QFormLayout;
    form->setSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto* token = new QLineEdit;
    token->setEchoMode(QLineEdit::Password);
    token->setText(current.token);
    token->setClearButtonEnabled(true);
    token->setPlaceholderText(QStringLiteral("粘贴 MinerU Token"));

    auto* modelVersion = new QComboBox;
    modelVersion->addItem(QStringLiteral("vlm（推荐，版面与图表更准）"),
                          QStringLiteral("vlm"));
    modelVersion->addItem(QStringLiteral("pipeline（对照与降级）"),
                          QStringLiteral("pipeline"));
    const int savedModel = modelVersion->findData(current.modelVersion);
    modelVersion->setCurrentIndex(savedModel < 0 ? 0 : savedModel);

    auto* isOcr = new QCheckBox(QStringLiteral("强制 OCR（仅扫描件需要）"));
    isOcr->setChecked(current.isOcr);
    isOcr->setToolTip(QStringLiteral(
        "文字版 PDF 不要勾选：强制识别会把本来准确的文字层换成 OCR 结果，反而引入错字。"));

    auto* cloudEnabled = new QCheckBox(QStringLiteral("允许把文档上传到云端解析"));
    cloudEnabled->setChecked(current.cloudEnabled);
    cloudEnabled->setToolTip(QStringLiteral(
        "勾选后，选择云端解析的文档会被上传到 MinerU 服务器处理。未勾选时只使用本机解析。"));

    auto* links = new QLabel(QStringLiteral(
        "<a href=\"https://mineru.net/apiManage/token\">创建 Token</a> · "
        "<a href=\"https://mineru.net/apiManage/docs\">接口文档与额度说明</a>"));
    links->setOpenExternalLinks(true);
    links->setTextInteractionFlags(Qt::TextBrowserInteraction);

    form->addRow(QStringLiteral("MinerU Token"), token);
    form->addRow(QStringLiteral("解析模型"), modelVersion);
    form->addRow(QStringLiteral("扫描件"), isOcr);
    form->addRow(QStringLiteral("云端上传"), cloudEnabled);
    form->addRow(QStringLiteral("帮助"), links);
    layout->addLayout(form);

    // 隐私说明必须显式呈现，而不是藏在文档里：上传是把用户材料交给第三方。
    auto* privacy = mutedLabel(QStringLiteral(
        "选择云端解析时，所选文档会通过官方签名链接上传到 MinerU 的对象存储并由其服务解析。"
        "涉密或未获授权的材料请只用本机解析。Token 保存在系统钥匙串，不写入日志、"
        "配置文件或导出的题库包。免费额度以 mineru.net 控制台为准。"));
    privacy->setObjectName(QStringLiteral("notice"));
    layout->addWidget(privacy);

    // 鸣谢：MinerU 是 OpenDataLab（上海人工智能实验室）开源的文档解析项目，
    // 其许可要求基于它对外提供在线服务时显著标明使用了 MinerU。
    auto* credit = new QLabel(QStringLiteral(
        "云端版面识别由开源项目 "
        "<a href=\"https://github.com/opendatalab/MinerU\">MinerU</a> 提供，"
        "出品方 OpenDataLab · 上海人工智能实验室。感谢他们把高质量的文档解析能力开源。"));
    credit->setOpenExternalLinks(true);
    credit->setTextInteractionFlags(Qt::TextBrowserInteraction);
    credit->setWordWrap(true);
    credit->setObjectName(QStringLiteral("muted"));
    layout->addWidget(credit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    // 没有 Token 时勾选"允许上传"没有意义，直接联动禁用，避免用户以为已经生效。
    auto syncCloudToggle = [token, cloudEnabled] {
        const bool hasToken = !token->text().trimmed().isEmpty();
        cloudEnabled->setEnabled(hasToken);
        if (!hasToken)
            cloudEnabled->setChecked(false);
    };
    QObject::connect(token, &QLineEdit::textChanged, &dialog, syncCloudToggle);
    syncCloudToggle();

    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;

    MineruConfig result;
    result.token = token->text().trimmed();
    result.modelVersion = modelVersion->currentData().toString();
    result.isOcr = isOcr->isChecked();
    result.cloudEnabled = cloudEnabled->isChecked() && !result.token.isEmpty();
    return result;
}

}  // namespace quizpane::studio
