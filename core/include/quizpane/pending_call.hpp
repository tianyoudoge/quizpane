#pragma once

#include "quizpane/provider_response_router.hpp"
#include <QJsonObject>
#include <QString>

namespace quizpane {

// 构造一次 provider RPC 请求体。id 与 method 从路由表派生，与 routeProviderResponse()
// 用的同一组字符串保持一致，避免发起侧和响应侧分别维护字面量。
// idSuffix 仅用于 attempt.saveAnswers 的动态 id（"save-<index>-<choice>"）。
struct PendingCall {
    QJsonObject build() const;
    QString id;
    QString method;
    QJsonObject params;
};

PendingCall makePendingCall(ProviderRoute route, QJsonObject params = {},
                            const QString& idSuffix = {});

}  // namespace quizpane
