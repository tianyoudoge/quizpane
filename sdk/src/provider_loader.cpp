#include "quizpane/provider_loader.hpp"
#include "quizpane/diagnostic_logger.hpp"
#include "quizpane/secret_store.hpp"

#include <QJsonDocument>
#include <QMetaObject>
#include <QFileInfo>
#include <QTimer>

namespace quizpane {

ProviderLoader::ProviderLoader(QObject* parent) : QObject(parent) {
    // 组装“依赖注入表”。Provider 只能调用这些显式开放的 Host 能力，不能获得
    // MainWindow 或其他业务对象，从边界上限制插件与主程序的耦合。
    hostApi_.abi_version = QP_PROVIDER_ABI_V1;
    hostApi_.struct_size = sizeof(hostApi_);
    hostApi_.host_context = this;
    hostApi_.log = &ProviderLoader::logThunk;
    hostApi_.secure_read = &ProviderLoader::secureReadThunk;
    hostApi_.secure_write = &ProviderLoader::secureWriteThunk;
    hostApi_.secure_delete = &ProviderLoader::secureDeleteThunk;
}

ProviderLoader::~ProviderLoader() { unload(); }

bool ProviderLoader::load(const QString& libraryPath, QString* error) {
    // QLibrary 对应 Java 的 System.loadLibrary，但这里还会逐个 resolve 导出函数，
    // 并校验 ABI/descriptor，防止把任意 DLL 当作题库执行。
    unload();
    ++generation_;  // 让任何仍在排队的旧响应在 acceptResponse 里被判为陈旧。
    diagnostic::event(QStringLiteral("provider"), QStringLiteral("load-start"),
        {{QStringLiteral("file"), QFileInfo(libraryPath).fileName()}});
    if (QFileInfo(libraryPath).suffix().compare(QStringLiteral("json"),
                                                Qt::CaseInsensitive) == 0) {
        if (!declarative_.load(libraryPath, error)) {
            diagnostic::event(QStringLiteral("provider"), QStringLiteral("load-failed"),
                {{QStringLiteral("kind"), QStringLiteral("declarative")},
                 {QStringLiteral("error"), error ? *error : QString{}}});
            return false;
        }
        descriptor_ = declarative_.descriptor();
        providerId_ = descriptor_.value(QStringLiteral("id")).toString();
        diagnostic::event(QStringLiteral("provider"), QStringLiteral("load-success"),
            {{QStringLiteral("kind"), QStringLiteral("declarative")},
             {QStringLiteral("id"), providerId_}});
        return true;
    }
    library_.setFileName(libraryPath);
    // Qt-based providers can register metatypes and process-wide callbacks.
    // Keep their code mapped after destroying the provider instance so those
    // registrations cannot point into an unloaded image.
    library_.setLoadHints(QLibrary::PreventUnloadHint);
    if (!library_.load()) {
        if (error) *error = library_.errorString();
        diagnostic::event(QStringLiteral("provider"), QStringLiteral("library-load-failed"),
            {{QStringLiteral("error"), library_.errorString()}});
        return false;
    }

    auto abi = reinterpret_cast<qp_provider_abi_version_fn>(
        library_.resolve("qp_provider_abi_version"));
    auto descriptorFn = reinterpret_cast<qp_provider_descriptor_json_fn>(
        library_.resolve("qp_provider_descriptor_json"));
    auto create = reinterpret_cast<qp_provider_create_fn>(
        library_.resolve("qp_provider_create"));
    requestFn_ = reinterpret_cast<qp_provider_request_fn>(
        library_.resolve("qp_provider_request"));
    cancelFn_ = reinterpret_cast<qp_provider_cancel_fn>(
        library_.resolve("qp_provider_cancel"));
    destroyFn_ = reinterpret_cast<qp_provider_destroy_fn>(
        library_.resolve("qp_provider_destroy"));

    if (!abi || !descriptorFn || !create || !requestFn_ || !cancelFn_ ||
        !destroyFn_) {
        if (error) *error = QStringLiteral("题库文件不完整");
        unload();
        return false;
    }
    if (abi() != QP_PROVIDER_ABI_V1) {
        if (error) *error = QStringLiteral("题库版本与当前应用不兼容");
        unload();
        return false;
    }

    const auto descriptorDoc =
        QJsonDocument::fromJson(QByteArray(descriptorFn()));
    if (!descriptorDoc.isObject()) {
        if (error) *error = QStringLiteral("题库文件描述信息损坏");
        unload();
        return false;
    }
    descriptor_ = descriptorDoc.object();
    providerId_ = descriptor_.value(QStringLiteral("id")).toString();
    if (providerId_.isEmpty()) {
        if (error) *error = QStringLiteral("题库文件缺少来源标识");
        unload();
        return false;
    }
    if (create(&hostApi_, &handle_) != 0 || !handle_) {
        if (error) *error = QStringLiteral("题库启动失败");
        unload();
        return false;
    }
    diagnostic::event(QStringLiteral("provider"), QStringLiteral("load-success"),
        {{QStringLiteral("kind"), QStringLiteral("native")},
         {QStringLiteral("id"), providerId_},
         {QStringLiteral("version"), descriptor_.value(QStringLiteral("version")).toString()}});
    return true;
}

void ProviderLoader::unload() {
    if (handle_ && destroyFn_) destroyFn_(handle_);
    handle_ = nullptr;
    requestFn_ = nullptr;
    cancelFn_ = nullptr;
    destroyFn_ = nullptr;
    descriptor_ = {};
    providerId_.clear();
    declarative_.unload();
    // PreventUnloadHint 告诉 Qt 在卸载原生题库后不要真正把动态库从进程地址空间
    // 移除：基于 Qt 的题库可能注册了 metatype 或进程级回调，卸载镜像会让那些
    // 指针悬空。因此这里只销毁题库实例，绝不调用 library_.unload()，与 load 时
    // 设置的 hint 保持一致的语义。
}

bool ProviderLoader::isLoaded() const { return handle_ != nullptr || declarative_.isLoaded(); }

QJsonObject ProviderLoader::descriptor() const { return descriptor_; }

bool ProviderLoader::request(const QJsonObject& request, QString* error) {
    diagnostic::event(QStringLiteral("provider"), QStringLiteral("request"),
        {{QStringLiteral("id"), request.value(QStringLiteral("id")).toString()},
         {QStringLiteral("method"), request.value(QStringLiteral("method")).toString()}});
    if (declarative_.isLoaded()) {
        const QJsonObject response = declarative_.request(request);
        const quint64 generation = generation_;
        QTimer::singleShot(0, this, [this, generation, response] {
            // 0ms 投递窗口内用户可能已切换/卸载题库；代号不匹配说明这是上一份
            // 题库的回包，直接丢弃，避免把旧响应错路由到当前页面。
            if (generation_ != generation) return;
            emit responseReceived(response);
        });
        return true;
    }
    if (!handle_ || !requestFn_) {
        if (error) *error = QStringLiteral("题库尚未加载");
        return false;
    }
    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    const int result = requestFn_(handle_, payload.constData(),
                                  static_cast<size_t>(payload.size()),
                                  &ProviderLoader::responseThunk, this);
    if (result != 0 && error) {
        *error = QStringLiteral("题库请求失败，错误码 %1").arg(result);
    }
    if (result != 0)
        diagnostic::event(QStringLiteral("provider"), QStringLiteral("request-rejected"),
            {{QStringLiteral("code"), result}});
    return result == 0;
}

bool ProviderLoader::cancel(const QString& requestId, QString* error) {
    if (declarative_.isLoaded()) {
        Q_UNUSED(requestId);
        return true;
    }
    if (!handle_ || !cancelFn_) {
        if (error) *error = QStringLiteral("题库尚未加载");
        return false;
    }
    const QByteArray encoded = requestId.toUtf8();
    const int result = cancelFn_(handle_, encoded.constData(),
                                 static_cast<size_t>(encoded.size()));
    if (result != 0 && error)
        *error = QStringLiteral("题库请求无法取消，错误码 %1").arg(result);
    return result == 0;
}

void ProviderLoader::responseThunk(void* userData, const char* json, size_t size) {
    if (!userData || !json) return;
    auto* self = static_cast<ProviderLoader*>(userData);
    const QByteArray copy(json, static_cast<qsizetype>(size));
    const quint64 generation = self->generation_;
    // Provider 的网络回调可能来自任意线程。QueuedConnection 把处理任务投递回
    // self 所在线程（桌面端通常是 UI 主线程），类似把结果 dispatch 到前端主线程。
    // 代号在主线程真正执行 lambda 前再核对一次，覆盖“投递后用户又切了题库”的情形。
    QMetaObject::invokeMethod(self, [self, copy, generation] {
        self->acceptResponse(copy, generation);
    }, Qt::QueuedConnection);
}

void ProviderLoader::logThunk(void* hostContext, qp_log_level level,
                              const char* message, size_t size) {
    if (!hostContext || !message) return;
    auto* self = static_cast<ProviderLoader*>(hostContext);
    const QString copy = QString::fromUtf8(message, static_cast<qsizetype>(size));
    QMetaObject::invokeMethod(
        self, [self, level, copy] { emit self->providerLog(level, copy); },
        Qt::QueuedConnection);
}

int ProviderLoader::secureReadThunk(void* hostContext, const char* key,
                                    size_t keySize, uint8_t* output,
                                    size_t* inoutOutputSize) {
    if (!hostContext || !key || !inoutOutputSize) return 3;
    const auto* self = static_cast<ProviderLoader*>(hostContext);
    return SecretStore::read(self->providerId_,
        QByteArray(key, static_cast<qsizetype>(keySize)), output, inoutOutputSize);
}

int ProviderLoader::secureWriteThunk(void* hostContext, const char* key,
                                     size_t keySize, const uint8_t* value,
                                     size_t valueSize) {
    if (!hostContext || !key || (!value && valueSize)) return 3;
    const auto* self = static_cast<ProviderLoader*>(hostContext);
    return SecretStore::write(self->providerId_,
        QByteArray(key, static_cast<qsizetype>(keySize)), value, valueSize);
}

int ProviderLoader::secureDeleteThunk(void* hostContext, const char* key,
                                      size_t keySize) {
    if (!hostContext || !key) return 3;
    const auto* self = static_cast<ProviderLoader*>(hostContext);
    return SecretStore::remove(self->providerId_,
        QByteArray(key, static_cast<qsizetype>(keySize)));
}

void ProviderLoader::acceptResponse(const QByteArray& json, quint64 generation) {
    if (generation != generation_) {
        // 回包属于已被卸载或替换掉的旧题库，丢弃以免错路由。
        diagnostic::event(QStringLiteral("provider"), QStringLiteral("response-stale"),
            {{QStringLiteral("bytes"), json.size()}});
        return;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (!document.isObject()) {
        diagnostic::event(QStringLiteral("provider"), QStringLiteral("response-invalid-json"),
            {{QStringLiteral("bytes"), json.size()},
             {QStringLiteral("error"), parseError.errorString()}});
        emit providerLog(QP_LOG_ERROR,
                         QStringLiteral("题库返回的数据无法读取：%1")
                             .arg(parseError.errorString()));
        return;
    }
    const QJsonObject response = document.object();
    diagnostic::event(QStringLiteral("provider"), QStringLiteral("response"),
        {{QStringLiteral("id"), response.value(QStringLiteral("id")).toString()},
         {QStringLiteral("error"), response.contains(QStringLiteral("error"))},
         {QStringLiteral("bytes"), json.size()}});
    emit responseReceived(response);
}

}  // namespace quizpane
