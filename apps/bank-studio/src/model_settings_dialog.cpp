#include "model_settings_dialog.hpp"

#include <QColor>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace quizpane::studio {
namespace {

// 厂商目录是 UI 与远端 API 之间的兼容配置，类似后端项目中的只读配置表。
// 普通厂商的 Endpoint 固定，只有“自定义供应商”允许用户修改，避免非开发用户
// 因误删路径片段而得到难以理解的网络错误。
struct ModelVendor {
    QString id;
    QString name;
    QString iconResource;
    QColor logoBackground;
    QString endpoint;
    QString accountUrl;
    QString tutorialUrl;
    QStringList fallbackModels;
    bool local = false;
    bool custom = false;
    bool anthropicProtocol = false;
};

const QList<ModelVendor>& modelVendors() {
    static const QList<ModelVendor> vendors{
        {QStringLiteral("openai"), QStringLiteral("OpenAI"),
         QStringLiteral(":/icons/vendor/openai.svg"),
         QColor(QStringLiteral("#2f6f61")), QStringLiteral("https://api.openai.com/v1"),
         QStringLiteral("https://platform.openai.com/api-keys"),
         QStringLiteral("https://platform.openai.com/docs/quickstart"),
         {QStringLiteral("gpt-5.2"), QStringLiteral("gpt-5-mini"),
          QStringLiteral("gpt-4.1-mini")}},
        {QStringLiteral("anthropic"), QStringLiteral("Anthropic"),
         QStringLiteral(":/icons/vendor/anthropic.svg"),
         QColor(QStringLiteral("#8b6248")), QStringLiteral("https://api.anthropic.com/v1"),
         QStringLiteral("https://console.anthropic.com/settings/keys"),
         QStringLiteral("https://docs.anthropic.com/en/api/getting-started"),
         {QStringLiteral("claude-sonnet-4-5"), QStringLiteral("claude-haiku-4-5")},
         false, false, true},
        {QStringLiteral("dashscope"), QStringLiteral("阿里云百炼（通义千问）"),
         QStringLiteral(":/icons/vendor/qwen.svg"),
         QColor(QStringLiteral("#6a55a6")),
         QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1"),
         QStringLiteral("https://bailian.console.aliyun.com/?apiKey=1"),
         QStringLiteral("https://help.aliyun.com/zh/model-studio/get-api-key"),
         {QStringLiteral("qwen3.7-plus"), QStringLiteral("qwen3.6-flash"),
          QStringLiteral("qwen-plus")}},
        {QStringLiteral("zhipu"), QStringLiteral("智谱 AI（GLM）"),
         QStringLiteral(":/icons/vendor/zhipu.svg"),
         QColor(QStringLiteral("#3c6c8c")),
         QStringLiteral("https://open.bigmodel.cn/api/paas/v4"),
         QStringLiteral("https://open.bigmodel.cn/usercenter/apikeys"),
         QStringLiteral("https://docs.bigmodel.cn/cn/guide/develop/http/introduction"),
         {QStringLiteral("glm-5.1"), QStringLiteral("glm-5-turbo"),
          QStringLiteral("glm-4.7")}},
        {QStringLiteral("ollama"), QStringLiteral("Ollama（本地模型）"),
         QStringLiteral(":/icons/vendor/ollama.svg"),
         QColor(QStringLiteral("#4e5964")), QStringLiteral("http://127.0.0.1:11434/v1"),
         QStringLiteral("https://ollama.com/download"),
         QStringLiteral("https://docs.ollama.com/api/introduction"),
         {QStringLiteral("qwen3:8b"), QStringLiteral("qwen3:4b"),
          QStringLiteral("llama3.2:3b")}, true},
        {QStringLiteral("custom"), QStringLiteral("自定义 OpenAI 兼容服务（高级）"), QString(),
         QColor(QStringLiteral("#59616a")), QString(), QString(),
         QStringLiteral(
             "https://github.com/tianyoudoge/quizpane/blob/master/"
             "docs/题库生成器UI与集成方案.md"),
         {QStringLiteral("填写模型名称")}, false, true}
    };
    return vendors;
}

const ModelVendor& vendorById(const QString& id) {
    for (const auto& vendor : modelVendors()) {
        if (vendor.id == id) return vendor;
    }
    return modelVendors().first();
}

QIcon vendorIcon(const ModelVendor& vendor) {
    // 官方品牌标识随资源包分发；运行时只负责放进统一的品牌色底座，使深色界面中
    // 的小尺寸图标仍清晰可辨。自定义兼容服务保留通用“+”标识，不伪装成任何厂商。
    QPixmap pixmap(28, 28);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(vendor.logoBackground);
    painter.drawRoundedRect(QRectF(2, 2, 24, 24), 6, 6);
    if (!vendor.iconResource.isEmpty()) {
        const QPixmap logo = QIcon(vendor.iconResource).pixmap(16, 16);
        painter.drawPixmap(QRect(6, 6, 16, 16), logo);
    } else {
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(14);
        painter.setFont(font);
        painter.setPen(QColor(QStringLiteral("#f0f2f4")));
        painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("+"));
    }
    return QIcon(pixmap);
}

QString modelsEndpoint(const ModelVendor& vendor, const QString& endpoint) {
    if (vendor.local) return QStringLiteral("http://127.0.0.1:11434/api/tags");
    QString base = endpoint.trimmed();
    while (base.endsWith('/')) base.chop(1);
    return base + QStringLiteral("/models");
}

QStringList parseModelNames(const QByteArray& payload, bool ollama) {
    // OpenAI 兼容接口返回 data[].id，Ollama 返回 models[].name。这里先归一化成
    // QStringList，后续 UI 不再关心供应商协议差异，相当于 Adapter 层输出统一 DTO。
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) return {};
    const QJsonArray values = document.object()
        .value(ollama ? QStringLiteral("models") : QStringLiteral("data")).toArray();
    QSet<QString> unique;
    for (const QJsonValue& value : values) {
        const QJsonObject object = value.toObject();
        const QString name = object.value(ollama ? QStringLiteral("name")
                                                 : QStringLiteral("id")).toString().trimmed();
        if (!name.isEmpty()) unique.insert(name);
    }
    QStringList names(unique.begin(), unique.end());
    names.sort(Qt::CaseInsensitive);
    return names;
}

QLabel* mutedLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("muted"));
    label->setWordWrap(true);
    return label;
}

}  // namespace

std::optional<ModelSettings> editModelSettings(QWidget* parent,
                                               const ModelSettings& current) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("模型管理"));
    dialog.setMinimumWidth(660);
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(QStringLiteral("模型管理"));
    title->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(title);
    layout->addWidget(mutedLabel(
        QStringLiteral("选择厂商并填写 API Key。可在供应商控制台查询用量；"
                       "网络失败时模型列表会回退到内置推荐。")));

    auto* form = new QFormLayout;
    form->setSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    // 与“模型”字段保持同一种原生下拉控件，厂商图标仅作为选项前缀。
    auto* service = new QComboBox;
    for (const auto& vendor : modelVendors()) {
        service->addItem(vendorIcon(vendor), vendor.name, vendor.id);
    }
    const int savedVendor = service->findData(current.vendorId);
    service->setCurrentIndex(savedVendor < 0 ? 0 : savedVendor);
    auto* model = new QComboBox;
    model->setEditable(true);
    auto* endpoint = new QLineEdit(current.endpoint);
    endpoint->setCursorPosition(0);
    auto* apiKey = new QLineEdit;
    apiKey->setEchoMode(QLineEdit::Password);
    apiKey->setText(current.apiKey);
    apiKey->setClearButtonEnabled(true);
    apiKey->setPlaceholderText(QStringLiteral("粘贴 API Key"));
    auto* links = new QLabel;
    links->setOpenExternalLinks(true);
    links->setTextInteractionFlags(Qt::TextBrowserInteraction);
    auto* hint = mutedLabel(QString());
    auto* supportsVision = new QCheckBox(QStringLiteral("该模型支持图片输入（用于 AI 修正）"));
    supportsVision->setChecked(current.supportsVision);
    supportsVision->setToolTip(QStringLiteral(
        "仅在所选模型确实支持图片输入时勾选。AI 修正只会发送当前裁切位置附近的局部图。"));
    hint->setObjectName(QStringLiteral("notice"));
    form->addRow(QStringLiteral("模型厂商"), service);
    form->addRow(QStringLiteral("模型"), model);
    form->addRow(QStringLiteral("Endpoint"), endpoint);
    form->addRow(QStringLiteral("API Key"), apiKey);
    form->addRow(QStringLiteral("图片输入"), supportsVision);
    form->addRow(QStringLiteral("帮助"), links);
    layout->addLayout(form);
    layout->addWidget(hint);

    // QNetworkAccessManager 的 parent 是 dialog，因此对话框关闭时网络资源会自动
    // 回收。currentReply 只负责“新请求覆盖旧请求”的取消语义，不拥有对象。
    auto* manager = new QNetworkAccessManager(&dialog);
    QNetworkReply* currentReply = nullptr;
    const auto selectedVendor = [service]() -> const ModelVendor& {
        return vendorById(service->currentData().toString());
    };
    const auto loadModels = [model](const QStringList& names, const QString& preferred) {
        model->clear();
        model->addItems(names);
        if (!preferred.isEmpty() && model->findText(preferred) < 0) {
            model->insertItem(0, preferred);
        }
        if (!preferred.isEmpty()) model->setCurrentText(preferred);
        else if (!names.isEmpty()) model->setCurrentIndex(0);
    };
    const auto loadFallback = [hint, loadModels](const ModelVendor& vendor,
                                                const QString& reason,
                                                const QString& preferred = QString()) {
        loadModels(vendor.fallbackModels, preferred);
        hint->setText(reason.isEmpty()
            ? QStringLiteral("已加载内置推荐模型。填写 API Key 后可获取账号当前可用"
                             "列表。")
            : QStringLiteral("%1；已回退到内置推荐模型。").arg(reason));
    };

    auto* fetch = new QPushButton(QStringLiteral("获取最新模型"));
    fetch->setObjectName(QStringLiteral("secondaryButton"));
    layout->addWidget(fetch, 0, Qt::AlignLeft);

    std::function<void()> fetchModels;
    fetchModels = [&, model, endpoint, apiKey, hint, fetch, manager,
                   selectedVendor, loadFallback, loadModels] {
        const ModelVendor vendor = selectedVendor();
        if (currentReply) {
            currentReply->abort();
            currentReply->deleteLater();
            currentReply = nullptr;
        }
        if (!vendor.local && apiKey->text().trimmed().isEmpty()) {
            loadFallback(vendor, QStringLiteral("请先填写 API Key"), model->currentText());
            return;
        }
        const QUrl url(modelsEndpoint(vendor, endpoint->text()));
        if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
            loadFallback(vendor, QStringLiteral("Endpoint 格式不正确"), model->currentText());
            return;
        }
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("QuizPane-Question-Maker"));
        request.setRawHeader("Accept", "application/json");
        request.setTransferTimeout(12000);
        const QByteArray key = apiKey->text().trimmed().toUtf8();
        if (vendor.anthropicProtocol) {
            request.setRawHeader("x-api-key", key);
            request.setRawHeader("anthropic-version", "2023-06-01");
        } else if (!vendor.local) {
            request.setRawHeader("Authorization", "Bearer " + key);
        }
        fetch->setEnabled(false);
        fetch->setText(QStringLiteral("正在获取…"));
        hint->setText(QStringLiteral("正在从 %1 获取可用模型，不会上传题目资料…")
                          .arg(vendor.name));
        currentReply = manager->get(request);
        QNetworkReply* reply = currentReply;
        QObject::connect(reply, &QNetworkReply::finished, &dialog,
                         [&, reply, vendor, model, hint, fetch, loadFallback, loadModels] {
            if (currentReply == reply) currentReply = nullptr;
            fetch->setEnabled(true);
            fetch->setText(QStringLiteral("获取最新模型"));
            const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray payload = reply->readAll();
            const QString preferred = model->currentText();
            if (reply->error() != QNetworkReply::NoError || status >= 400) {
                const QString reason = status == 401 || status == 403
                    ? QStringLiteral("API Key 无效或没有模型列表权限")
                    : QStringLiteral("获取失败：%1").arg(reply->errorString());
                loadFallback(vendor, reason, preferred);
            } else {
                const QStringList names = parseModelNames(payload, vendor.local);
                if (names.isEmpty()) {
                    loadFallback(vendor, QStringLiteral("厂商没有返回可识别的模型列表"),
                                 preferred);
                } else {
                    loadModels(names, names.contains(preferred) ? preferred : QString());
                    hint->setText(QStringLiteral("已从 %1 获取 %2 个可用模型。")
                                      .arg(vendor.name).arg(names.size()));
                }
            }
            reply->deleteLater();
        });
    };

    const auto refreshVendor = [=, &current](bool preserveCurrent) {
        const ModelVendor& vendor = selectedVendor();
        const QString preferred = preserveCurrent ? model->currentText() : QString();
        endpoint->setReadOnly(!vendor.custom);
        endpoint->setText(vendor.custom
            ? (vendor.id == current.vendorId && !current.endpoint.isEmpty()
                   ? current.endpoint : QStringLiteral("https://your-endpoint.example/v1"))
            : vendor.endpoint);
        endpoint->setCursorPosition(0);
        apiKey->setEnabled(!vendor.local);
        apiKey->setPlaceholderText(vendor.local ? QStringLiteral("本地 Ollama 不需要 API Key")
                                                : QStringLiteral("粘贴 API Key"));
        QStringList linkParts;
        if (!vendor.accountUrl.isEmpty()) {
            linkParts << QStringLiteral("<a href=\"%1\" style=\"color:#aeb8c3\">%2</a>")
                             .arg(vendor.accountUrl,
                                  vendor.local ? QStringLiteral("安装 Ollama")
                                               : QStringLiteral("注册并申请 API Key"));
        }
        if (!vendor.tutorialUrl.isEmpty()) {
            linkParts << QStringLiteral("<a href=\"%1\" style=\"color:#aeb8c3\">配置教程</a>")
                             .arg(vendor.tutorialUrl);
        }
        links->setText(linkParts.join(QStringLiteral("　·　")));
        loadFallback(vendor, QString(), preferred);
        if (vendor.custom) {
            hint->setText(QStringLiteral(
                "高级模式：填写 OpenAI 兼容 Endpoint、API Key 和模型名。"));
        } else if (vendor.local) {
            hint->setText(
                QStringLiteral("本地模式：点击获取已安装模型，资料不会离开电脑。"));
        }
    };
    refreshVendor(false);
    if (current.vendorId == service->currentData().toString() &&
        !current.modelName.isEmpty()) {
        if (model->findText(current.modelName) < 0) model->insertItem(0, current.modelName);
        model->setCurrentText(current.modelName);
    }
    QObject::connect(service, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                     [&, refreshVendor](int) {
        apiKey->clear();
        refreshVendor(false);
        if (selectedVendor().local) QTimer::singleShot(0, &dialog, fetchModels);
    });
    QObject::connect(fetch, &QPushButton::clicked, &dialog, fetchModels);
    QObject::connect(apiKey, &QLineEdit::editingFinished, &dialog, [&, selectedVendor] {
        if (!selectedVendor().local && !apiKey->text().trimmed().isEmpty()) fetchModels();
    });
    if (selectedVendor().local) QTimer::singleShot(0, &dialog, fetchModels);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (model->currentText().trimmed().isEmpty() || endpoint->text().trimmed().isEmpty()) {
            hint->setText(QStringLiteral("请填写模型名称和 Endpoint。"));
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;

    return ModelSettings{
        service->currentData().toString(),
        service->currentText(),
        model->currentText().trimmed(),
        endpoint->text().trimmed(),
        apiKey->text().trimmed(),
        supportsVision->isChecked()
    };
}

}  // namespace quizpane::studio
