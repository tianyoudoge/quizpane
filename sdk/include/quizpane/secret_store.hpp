#pragma once

#include <QByteArray>
#include <QString>
#include <cstddef>
#include <cstdint>

namespace quizpane {

// 平台凭据存储边界。返回值与 provider ABI 的 secure_* 约定一致：
// 0 成功、1 不存在、2 缓冲区不足、3 失败、4 当前平台无安全存储后端。
class SecretStore final {
public:
    static int read(const QString& providerId, const QByteArray& key,
                    uint8_t* output, size_t* inoutOutputSize);
    static int write(const QString& providerId, const QByteArray& key,
                     const uint8_t* value, size_t valueSize);
    static int remove(const QString& providerId, const QByteArray& key);
};

}  // namespace quizpane
