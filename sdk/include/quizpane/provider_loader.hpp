#pragma once

#include <QJsonObject>
#include <QLibrary>
#include <QObject>
#include <QString>

#include "quizpane/provider_abi.h"
#include "quizpane/declarative_provider.hpp"

namespace quizpane {

// ProviderLoader 是 Host 与动态库之间的适配器，可以类比 Java 中的插件
// ClassLoader + RPC client：它解析导出函数、发送 JSON 请求并把回调转为 Qt signal。
class ProviderLoader final : public QObject {
    Q_OBJECT

public:
    explicit ProviderLoader(QObject* parent = nullptr);
    ~ProviderLoader() override;

    bool load(const QString& libraryPath, QString* error = nullptr);
    void unload();
    [[nodiscard]] bool isLoaded() const;
    [[nodiscard]] QJsonObject descriptor() const;
    bool request(const QJsonObject& request, QString* error = nullptr);
    bool cancel(const QString& requestId, QString* error = nullptr);

signals:
    // signal 不需要调用方轮询；MainWindow 使用 connect() 订阅，类似浏览器事件或
    // Spring ApplicationEvent，但默认在 Qt 主线程事件循环中分发。
    void responseReceived(const QJsonObject& response);
    void providerLog(int level, const QString& message);

private:
    // C ABI 只能传函数指针，不能直接传 C++ 成员函数。下面的静态 thunk 先从
    // void* context 找回 this，再转发到对象方法，作用类似 JNI/native callback 桥。
    static void responseThunk(void* userData, const char* json, size_t size);
    static void logThunk(void* hostContext, qp_log_level level,
                         const char* message, size_t size);
    static int secureReadThunk(void* hostContext, const char* key, size_t keySize,
                               uint8_t* output, size_t* inoutOutputSize);
    static int secureWriteThunk(void* hostContext, const char* key, size_t keySize,
                                const uint8_t* value, size_t valueSize);
    static int secureDeleteThunk(void* hostContext, const char* key, size_t keySize);
    void acceptResponse(const QByteArray& json, quint64 generation);

    QLibrary library_;
    DeclarativeProvider declarative_;
    QJsonObject descriptor_;
    QString providerId_;
    qp_provider_handle* handle_ = nullptr;
    qp_provider_request_fn requestFn_ = nullptr;
    qp_provider_cancel_fn cancelFn_ = nullptr;
    qp_provider_destroy_fn destroyFn_ = nullptr;
    qp_host_api_v1 hostApi_{};
    // 每次 load 递增的代。Provider 的响应回调可能跨线程异步到达，
    // 当用户在 0ms 投递窗口内切换/卸载题库时，排队中的回包会携带旧代号，
    // acceptResponse 据此丢弃属于已卸载题库的陈旧响应，避免错路由。
    quint64 generation_ = 0;
};

}  // namespace quizpane
