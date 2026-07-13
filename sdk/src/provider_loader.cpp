#include "quizpane/provider_loader.hpp"

#include <QJsonDocument>
#include <QMetaObject>
#include <QFileInfo>
#include <QTimer>

#include <cstring>

#if defined(Q_OS_MACOS)
#include <Security/Security.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <wincred.h>
#elif defined(QUIZPANE_HAVE_LIBSECRET)
#include <libsecret/secret.h>
#endif

namespace quizpane {
namespace {
#if defined(QUIZPANE_HAVE_LIBSECRET)
const SecretSchema kQuizPaneSecretSchema = {
    "org.quizpane.Provider", SECRET_SCHEMA_NONE,
    {{"provider", SECRET_SCHEMA_ATTRIBUTE_STRING},
     {"key", SECRET_SCHEMA_ATTRIBUTE_STRING},
     {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}};
#endif

QString credentialTarget(const QString& providerId, const QByteArray& key) {
    return QStringLiteral("QuizPane/%1/%2")
        .arg(providerId, QString::fromUtf8(key));
}
}  // namespace

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
    if (QFileInfo(libraryPath).suffix().compare(QStringLiteral("json"),
                                                Qt::CaseInsensitive) == 0) {
        if (!declarative_.load(libraryPath, error)) return false;
        descriptor_ = declarative_.descriptor();
        providerId_ = descriptor_.value(QStringLiteral("id")).toString();
        return true;
    }
    library_.setFileName(libraryPath);
    // Qt-based providers can register metatypes and process-wide callbacks.
    // Keep their code mapped after destroying the provider instance so those
    // registrations cannot point into an unloaded image.
    library_.setLoadHints(QLibrary::PreventUnloadHint);
    if (!library_.load()) {
        if (error) *error = library_.errorString();
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
    if (library_.isLoaded()) library_.unload();
}

bool ProviderLoader::isLoaded() const { return handle_ != nullptr || declarative_.isLoaded(); }

QJsonObject ProviderLoader::descriptor() const { return descriptor_; }

bool ProviderLoader::request(const QJsonObject& request, QString* error) {
    if (declarative_.isLoaded()) {
        const QJsonObject response = declarative_.request(request);
        QTimer::singleShot(0, this, [this, response] { emit responseReceived(response); });
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
    // Provider 的网络回调可能来自任意线程。QueuedConnection 把处理任务投递回
    // self 所在线程（桌面端通常是 UI 主线程），类似把结果 dispatch 到前端主线程。
    QMetaObject::invokeMethod(self, [self, copy] { self->acceptResponse(copy); },
                              Qt::QueuedConnection);
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
    // 同一个接口在编译期选择平台实现。#if 类似 Maven profile，但发生在 C/C++
    // 预处理阶段：最终二进制只包含当前系统的分支。
#if defined(Q_OS_MACOS)
    const QByteArray service = QByteArrayLiteral("org.quizpane.provider.") +
                               self->providerId_.toUtf8();
    const QByteArray account(key, static_cast<qsizetype>(keySize));
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount,
                          kSecReturnData, kSecMatchLimit};
    const void* values[] = {kSecClassGenericPassword,
        CFStringCreateWithBytes(nullptr,
            reinterpret_cast<const UInt8*>(service.constData()), service.size(),
            kCFStringEncodingUTF8, false),
        CFStringCreateWithBytes(nullptr,
            reinterpret_cast<const UInt8*>(account.constData()), account.size(),
            kCFStringEncodingUTF8, false), kCFBooleanTrue, kSecMatchLimitOne};
    CFDictionaryRef query = CFDictionaryCreate(nullptr, keys, values, 5,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    CFRelease(values[1]);
    CFRelease(values[2]);
    if (status == errSecItemNotFound) return 1;
    if (status != errSecSuccess || !result) return 3;
    const auto data = static_cast<CFDataRef>(result);
    const size_t required = static_cast<size_t>(CFDataGetLength(data));
    if (!output || *inoutOutputSize < required) {
        *inoutOutputSize = required;
        CFRelease(result);
        return output ? 2 : 0;
    }
    memcpy(output, CFDataGetBytePtr(data), required);
    *inoutOutputSize = required;
    CFRelease(result);
    return 0;
#elif defined(Q_OS_WIN)
    const QByteArray account(key, static_cast<qsizetype>(keySize));
    const QString target = credentialTarget(self->providerId_, account);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()),
                   CRED_TYPE_GENERIC, 0, &credential))
        return GetLastError() == ERROR_NOT_FOUND ? 1 : 3;
    const size_t required = credential->CredentialBlobSize;
    if (!output || *inoutOutputSize < required) {
        *inoutOutputSize = required;
        CredFree(credential);
        return output ? 2 : 0;
    }
    memcpy(output, credential->CredentialBlob, required);
    *inoutOutputSize = required;
    CredFree(credential);
    return 0;
#elif defined(QUIZPANE_HAVE_LIBSECRET)
    const QByteArray account(key, static_cast<qsizetype>(keySize));
    const QByteArray provider = self->providerId_.toUtf8();
    GError* error = nullptr;
    gchar* encoded = secret_password_lookup_sync(&kQuizPaneSecretSchema,
        nullptr, &error, "provider", provider.constData(), "key",
        account.constData(), nullptr);
    if (error) { g_error_free(error); return 3; }
    if (!encoded) return 1;
    const QByteArray value = QByteArray::fromBase64(QByteArray(encoded));
    secret_password_free(encoded);
    const size_t required = static_cast<size_t>(value.size());
    if (!output || *inoutOutputSize < required) {
        *inoutOutputSize = required;
        return output ? 2 : 0;
    }
    memcpy(output, value.constData(), required);
    *inoutOutputSize = required;
    return 0;
#else
    Q_UNUSED(self);
    Q_UNUSED(output);
    return 4;
#endif
}

int ProviderLoader::secureWriteThunk(void* hostContext, const char* key,
                                     size_t keySize, const uint8_t* value,
                                     size_t valueSize) {
    if (!hostContext || !key || (!value && valueSize)) return 3;
    const auto* self = static_cast<ProviderLoader*>(hostContext);
    // service/target 同时包含 providerId，因此不同题库即使都使用
    // "session.cookies" 作为 key，也不会互相覆盖登录态。
#if defined(Q_OS_MACOS)
    const QByteArray service = QByteArrayLiteral("org.quizpane.provider.") +
                               self->providerId_.toUtf8();
    const QByteArray account(key, static_cast<qsizetype>(keySize));
    CFStringRef serviceRef = CFStringCreateWithBytes(nullptr,
        reinterpret_cast<const UInt8*>(service.constData()), service.size(),
        kCFStringEncodingUTF8, false);
    CFStringRef accountRef = CFStringCreateWithBytes(nullptr,
        reinterpret_cast<const UInt8*>(account.constData()), account.size(),
        kCFStringEncodingUTF8, false);
    CFDataRef dataRef = CFDataCreate(nullptr, value, static_cast<CFIndex>(valueSize));
    const void* queryKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void* queryValues[] = {kSecClassGenericPassword, serviceRef, accountRef};
    CFDictionaryRef query = CFDictionaryCreate(nullptr, queryKeys, queryValues, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void* updateKeys[] = {kSecValueData};
    const void* updateValues[] = {dataRef};
    CFDictionaryRef update = CFDictionaryCreate(nullptr, updateKeys, updateValues, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus status = SecItemUpdate(query, update);
    if (status == errSecItemNotFound) {
        const void* addKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount,
                                 kSecValueData};
        const void* addValues[] = {kSecClassGenericPassword, serviceRef,
                                   accountRef, dataRef};
        CFDictionaryRef add = CFDictionaryCreate(nullptr, addKeys, addValues, 4,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        status = SecItemAdd(add, nullptr);
        CFRelease(add);
    }
    CFRelease(update);
    CFRelease(query);
    CFRelease(dataRef);
    CFRelease(accountRef);
    CFRelease(serviceRef);
    return status == errSecSuccess ? 0 : 3;
#elif defined(Q_OS_WIN)
    const QByteArray account(key, static_cast<qsizetype>(keySize));
    QString target = credentialTarget(self->providerId_, account);
    QString userName = self->providerId_;
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = reinterpret_cast<LPWSTR>(target.data());
    credential.UserName = reinterpret_cast<LPWSTR>(userName.data());
    credential.CredentialBlobSize = static_cast<DWORD>(valueSize);
    credential.CredentialBlob = const_cast<LPBYTE>(value);
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) ? 0 : 3;
#elif defined(QUIZPANE_HAVE_LIBSECRET)
    const QByteArray account(key, static_cast<qsizetype>(keySize));
    const QByteArray provider = self->providerId_.toUtf8();
    const QByteArray encoded(reinterpret_cast<const char*>(value),
                             static_cast<qsizetype>(valueSize));
    const QByteArray base64 = encoded.toBase64();
    GError* error = nullptr;
    const gboolean ok = secret_password_store_sync(&kQuizPaneSecretSchema,
        SECRET_COLLECTION_DEFAULT, "QuizPane Provider session",
        base64.constData(), nullptr, &error, "provider", provider.constData(),
        "key", account.constData(), nullptr);
    if (error) g_error_free(error);
    return ok ? 0 : 3;
#else
    Q_UNUSED(self);
    return 4;
#endif
}

int ProviderLoader::secureDeleteThunk(void* hostContext, const char* key,
                                      size_t keySize) {
    if (!hostContext || !key) return 3;
    const auto* self = static_cast<ProviderLoader*>(hostContext);
#if defined(Q_OS_MACOS)
    const QByteArray service = QByteArrayLiteral("org.quizpane.provider.") +
                               self->providerId_.toUtf8();
    const QByteArray account(key, static_cast<qsizetype>(keySize));
    CFStringRef serviceRef = CFStringCreateWithBytes(nullptr,
        reinterpret_cast<const UInt8*>(service.constData()), service.size(),
        kCFStringEncodingUTF8, false);
    CFStringRef accountRef = CFStringCreateWithBytes(nullptr,
        reinterpret_cast<const UInt8*>(account.constData()), account.size(),
        kCFStringEncodingUTF8, false);
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void* values[] = {kSecClassGenericPassword, serviceRef, accountRef};
    CFDictionaryRef query = CFDictionaryCreate(nullptr, keys, values, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    CFRelease(accountRef);
    CFRelease(serviceRef);
    return status == errSecSuccess || status == errSecItemNotFound ? 0 : 3;
#elif defined(Q_OS_WIN)
    const QByteArray account(key, static_cast<qsizetype>(keySize));
    const QString target = credentialTarget(self->providerId_, account);
    if (CredDeleteW(reinterpret_cast<LPCWSTR>(target.utf16()),
                    CRED_TYPE_GENERIC, 0)) return 0;
    return GetLastError() == ERROR_NOT_FOUND ? 0 : 3;
#elif defined(QUIZPANE_HAVE_LIBSECRET)
    const QByteArray account(key, static_cast<qsizetype>(keySize));
    const QByteArray provider = self->providerId_.toUtf8();
    GError* error = nullptr;
    const gboolean ok = secret_password_clear_sync(&kQuizPaneSecretSchema,
        nullptr, &error, "provider", provider.constData(), "key",
        account.constData(), nullptr);
    if (error) g_error_free(error);
    return ok ? 0 : 3;
#else
    Q_UNUSED(self);
    return 4;
#endif
}

void ProviderLoader::acceptResponse(const QByteArray& json) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (!document.isObject()) {
        emit providerLog(QP_LOG_ERROR,
                         QStringLiteral("题库返回的数据无法读取：%1")
                             .arg(parseError.errorString()));
        return;
    }
    emit responseReceived(document.object());
}

}  // namespace quizpane
