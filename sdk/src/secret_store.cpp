#include "quizpane/secret_store.hpp"

#include <cstring>

#if defined(Q_OS_MACOS)
#include <Security/Security.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <wincred.h>
#elif defined(QUIZPANE_HAVE_LIBSECRET)
#ifdef signals
#define QUIZPANE_RESTORE_QT_SIGNALS
#undef signals
#endif
#include <libsecret/secret.h>
#ifdef QUIZPANE_RESTORE_QT_SIGNALS
#define signals Q_SIGNALS
#undef QUIZPANE_RESTORE_QT_SIGNALS
#endif
#endif

namespace quizpane {
namespace {
#if defined(QUIZPANE_HAVE_LIBSECRET)
const SecretSchema kSchema = {
    "org.quizpane.Provider", SECRET_SCHEMA_NONE,
    {{"provider", SECRET_SCHEMA_ATTRIBUTE_STRING},
     {"key", SECRET_SCHEMA_ATTRIBUTE_STRING},
     {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}};
#endif
#if defined(Q_OS_WIN)
QString credentialTarget(const QString& providerId, const QByteArray& key) {
    return QStringLiteral("QuizPane/%1/%2").arg(providerId, QString::fromUtf8(key));
}
#endif
}

int SecretStore::read(const QString& providerId, const QByteArray& key,
                      uint8_t* output, size_t* inoutOutputSize) {
    if (providerId.isEmpty() || key.isEmpty() || !inoutOutputSize) return 3;
#if defined(Q_OS_MACOS)
    const QByteArray service = QByteArrayLiteral("org.quizpane.provider.") + providerId.toUtf8();
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData, kSecMatchLimit};
    const void* values[] = {kSecClassGenericPassword,
        CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(service.constData()), service.size(), kCFStringEncodingUTF8, false),
        CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(key.constData()), key.size(), kCFStringEncodingUTF8, false),
        kCFBooleanTrue, kSecMatchLimitOne};
    CFDictionaryRef query = CFDictionaryCreate(nullptr, keys, values, 5,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query); CFRelease(values[1]); CFRelease(values[2]);
    if (status == errSecItemNotFound) return 1;
    if (status != errSecSuccess || !result) return 3;
    const auto data = static_cast<CFDataRef>(result);
    const size_t required = static_cast<size_t>(CFDataGetLength(data));
    if (!output || *inoutOutputSize < required) {
        *inoutOutputSize = required; CFRelease(result); return output ? 2 : 0;
    }
    std::memcpy(output, CFDataGetBytePtr(data), required);
    *inoutOutputSize = required; CFRelease(result); return 0;
#elif defined(Q_OS_WIN)
    const QString target = credentialTarget(providerId, key);
    if (target.size() > CRED_MAX_GENERIC_TARGET_NAME_LENGTH)
        return 3;
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0, &credential))
        return GetLastError() == ERROR_NOT_FOUND ? 1 : 3;
    const size_t required = credential->CredentialBlobSize;
    if (!output || *inoutOutputSize < required) {
        *inoutOutputSize = required; CredFree(credential); return output ? 2 : 0;
    }
    std::memcpy(output, credential->CredentialBlob, required);
    *inoutOutputSize = required; CredFree(credential); return 0;
#elif defined(QUIZPANE_HAVE_LIBSECRET)
    const QByteArray provider = providerId.toUtf8();
    GError* error = nullptr;
    gchar* encoded = secret_password_lookup_sync(&kSchema, nullptr, &error,
        "provider", provider.constData(), "key", key.constData(), nullptr);
    if (error) { g_error_free(error); return 3; }
    if (!encoded) return 1;
    const QByteArray value = QByteArray::fromBase64(QByteArray(encoded));
    secret_password_free(encoded);
    const size_t required = static_cast<size_t>(value.size());
    if (!output || *inoutOutputSize < required) {
        *inoutOutputSize = required; return output ? 2 : 0;
    }
    std::memcpy(output, value.constData(), required);
    *inoutOutputSize = required; return 0;
#else
    Q_UNUSED(output); return 4;
#endif
}

int SecretStore::write(const QString& providerId, const QByteArray& key,
                       const uint8_t* value, size_t valueSize) {
    if (providerId.isEmpty() || key.isEmpty() || (!value && valueSize)) return 3;
#if defined(Q_OS_MACOS)
    const QByteArray service = QByteArrayLiteral("org.quizpane.provider.") + providerId.toUtf8();
    CFStringRef serviceRef = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(service.constData()), service.size(), kCFStringEncodingUTF8, false);
    CFStringRef accountRef = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(key.constData()), key.size(), kCFStringEncodingUTF8, false);
    CFDataRef dataRef = CFDataCreate(nullptr, value, static_cast<CFIndex>(valueSize));
    const void* queryKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void* queryValues[] = {kSecClassGenericPassword, serviceRef, accountRef};
    CFDictionaryRef query = CFDictionaryCreate(nullptr, queryKeys, queryValues, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void* updateKeys[] = {kSecValueData}; const void* updateValues[] = {dataRef};
    CFDictionaryRef update = CFDictionaryCreate(nullptr, updateKeys, updateValues, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus status = SecItemUpdate(query, update);
    if (status == errSecItemNotFound) {
        const void* addKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
        const void* addValues[] = {kSecClassGenericPassword, serviceRef, accountRef, dataRef};
        CFDictionaryRef add = CFDictionaryCreate(nullptr, addKeys, addValues, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        status = SecItemAdd(add, nullptr); CFRelease(add);
    }
    CFRelease(update); CFRelease(query); CFRelease(dataRef); CFRelease(accountRef); CFRelease(serviceRef);
    return status == errSecSuccess ? 0 : 3;
#elif defined(Q_OS_WIN)
    if (valueSize > CRED_MAX_CREDENTIAL_BLOB_SIZE)
        return 3;
    QString target = credentialTarget(providerId, key); QString userName = providerId;
    if (target.size() > CRED_MAX_GENERIC_TARGET_NAME_LENGTH)
        return 3;
    CREDENTIALW credential{}; credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = reinterpret_cast<LPWSTR>(target.data());
    credential.UserName = reinterpret_cast<LPWSTR>(userName.data());
    credential.CredentialBlobSize = static_cast<DWORD>(valueSize);
    credential.CredentialBlob = const_cast<LPBYTE>(value);
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) ? 0 : 3;
#elif defined(QUIZPANE_HAVE_LIBSECRET)
    const QByteArray provider = providerId.toUtf8();
    const QByteArray encoded(reinterpret_cast<const char*>(value), static_cast<qsizetype>(valueSize));
    const QByteArray base64 = encoded.toBase64(); GError* error = nullptr;
    const gboolean ok = secret_password_store_sync(&kSchema, SECRET_COLLECTION_DEFAULT,
        "QuizPane Provider session", base64.constData(), nullptr, &error,
        "provider", provider.constData(), "key", key.constData(), nullptr);
    if (error) g_error_free(error); return ok ? 0 : 3;
#else
    return 4;
#endif
}

int SecretStore::remove(const QString& providerId, const QByteArray& key) {
    if (providerId.isEmpty() || key.isEmpty()) return 3;
#if defined(Q_OS_MACOS)
    const QByteArray service = QByteArrayLiteral("org.quizpane.provider.") + providerId.toUtf8();
    CFStringRef serviceRef = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(service.constData()), service.size(), kCFStringEncodingUTF8, false);
    CFStringRef accountRef = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(key.constData()), key.size(), kCFStringEncodingUTF8, false);
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void* values[] = {kSecClassGenericPassword, serviceRef, accountRef};
    CFDictionaryRef query = CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query); CFRelease(accountRef); CFRelease(serviceRef);
    return status == errSecSuccess || status == errSecItemNotFound ? 0 : 3;
#elif defined(Q_OS_WIN)
    const QString target = credentialTarget(providerId, key);
    if (target.size() > CRED_MAX_GENERIC_TARGET_NAME_LENGTH)
        return 3;
    if (CredDeleteW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0)) return 0;
    return GetLastError() == ERROR_NOT_FOUND ? 0 : 3;
#elif defined(QUIZPANE_HAVE_LIBSECRET)
    const QByteArray provider = providerId.toUtf8(); GError* error = nullptr;
    const gboolean ok = secret_password_clear_sync(&kSchema, nullptr, &error,
        "provider", provider.constData(), "key", key.constData(), nullptr);
    if (error) g_error_free(error); return ok ? 0 : 3;
#else
    return 4;
#endif
}

}  // namespace quizpane
