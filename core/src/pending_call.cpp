#include "quizpane/pending_call.hpp"

namespace quizpane {

QJsonObject PendingCall::build() const {
    return {{"id", id}, {"method", method}, {"params", params}};
}

namespace {

struct RouteNames {
    QString id;
    QString method;
};

// id/method 字面量与 routeProviderResponse() 里按 id 反推 route 的规则一一对应；
// 改动任一处都要同步另一处，否则响应会被路由成 Unknown。
RouteNames routeNames(ProviderRoute route) {
    switch (route) {
    case ProviderRoute::Initialize: return {QStringLiteral("host-init-1"), QStringLiteral("provider.initialize")};
    case ProviderRoute::AuthBegin: return {QStringLiteral("auth-begin"), QStringLiteral("auth.begin")};
    case ProviderRoute::AuthPoll: return {QStringLiteral("auth-poll"), QStringLiteral("auth.poll")};
    case ProviderRoute::Catalog: return {QStringLiteral("catalog-list"), QStringLiteral("catalog.list")};
    case ProviderRoute::AttemptCreate: return {QStringLiteral("attempt-create"), QStringLiteral("attempt.create")};
    case ProviderRoute::Questions: return {QStringLiteral("attempt-questions"), QStringLiteral("attempt.questions")};
    case ProviderRoute::FinalSave: return {QStringLiteral("final-save"), QStringLiteral("attempt.saveAnswers")};
    case ProviderRoute::Submit: return {QStringLiteral("attempt-submit"), QStringLiteral("attempt.submit")};
    case ProviderRoute::Report: return {QStringLiteral("attempt-report"), QStringLiteral("attempt.report")};
    case ProviderRoute::Solutions: return {QStringLiteral("attempt-solutions"), QStringLiteral("attempt.solutions")};
    case ProviderRoute::SaveAnswer: return {QStringLiteral("save-"), QStringLiteral("attempt.saveAnswers")};
    case ProviderRoute::Unknown: break;
    }
    return {};
}

}  // namespace

PendingCall makePendingCall(ProviderRoute route, QJsonObject params, const QString& idSuffix) {
    const RouteNames names = routeNames(route);
    return PendingCall{names.id + idSuffix, names.method, std::move(params)};
}

}  // namespace quizpane
