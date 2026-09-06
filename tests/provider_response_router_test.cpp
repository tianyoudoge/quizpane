#include "quizpane/provider_response_router.hpp"
#include "quizpane/pending_call.hpp"
#include <QJsonArray>

int main() {
    using namespace quizpane;

    // routeProviderResponse round-trips
    const auto catalog = routeProviderResponse({{"id", "catalog-list"},
        {"result", QJsonObject{{"nodes", QJsonArray{}}}}});
    if (catalog.route != ProviderRoute::Catalog || catalog.failed) return 1;
    const auto save = routeProviderResponse({{"id", "save-2-1"},
        {"error", QJsonObject{{"code", -1}, {"message", "failed"}}}});
    if (save.route != ProviderRoute::SaveAnswer || !save.failed) return 2;

    // makePendingCall builds matching id/method and round-trips through routeProviderResponse
    const QJsonObject init = makePendingCall(ProviderRoute::Initialize,
        QJsonObject{{"hostVersion", "0.1.0"}}).build();
    if (init.value("id").toString() != QStringLiteral("host-init-1")) return 3;
    if (init.value("method").toString() != QStringLiteral("provider.initialize")) return 4;
    if (routeProviderResponse({{"id", init.value("id")}, {"result", QJsonObject{}}}).route
        != ProviderRoute::Initialize) return 5;

    const QJsonObject saveCall = makePendingCall(ProviderRoute::SaveAnswer,
        QJsonObject{{"attemptId", "x"}},
        QStringLiteral("7-2")).build();
    if (saveCall.value("id").toString() != QStringLiteral("save-7-2")) return 6;
    if (saveCall.value("method").toString() != QStringLiteral("attempt.saveAnswers")) return 7;
    if (routeProviderResponse({{"id", saveCall.value("id")}, {"result", QJsonObject{}}}).route
        != ProviderRoute::SaveAnswer) return 8;

    // Verify all stable routes produce non-empty id and method
    for (const auto route : {ProviderRoute::AuthBegin, ProviderRoute::AuthPoll,
            ProviderRoute::Catalog, ProviderRoute::AttemptCreate, ProviderRoute::Questions,
            ProviderRoute::FinalSave, ProviderRoute::Submit, ProviderRoute::Report,
            ProviderRoute::Solutions}) {
        const auto call = makePendingCall(route);
        if (call.id.isEmpty() || call.method.isEmpty()) return 9;
        const auto roundTrip = routeProviderResponse({{"id", call.id}, {"result", QJsonObject{}}});
        if (roundTrip.route != route) return 10;
    }

    return 0;
}
