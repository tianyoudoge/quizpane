#include "quizpane/provider_response_router.hpp"

namespace quizpane {

ProviderResponseEnvelope routeProviderResponse(const QJsonObject& response) {
    ProviderResponseEnvelope envelope;
    envelope.id = response.value(QStringLiteral("id")).toString();
    envelope.result = response.value(QStringLiteral("result")).toObject();
    envelope.error = response.value(QStringLiteral("error")).toObject();
    envelope.failed = !envelope.error.isEmpty();
    if (envelope.id.startsWith(QStringLiteral("save-"))) envelope.route = ProviderRoute::SaveAnswer;
    else if (envelope.id == QStringLiteral("host-init-1")) envelope.route = ProviderRoute::Initialize;
    else if (envelope.id == QStringLiteral("auth-begin")) envelope.route = ProviderRoute::AuthBegin;
    else if (envelope.id == QStringLiteral("auth-poll")) envelope.route = ProviderRoute::AuthPoll;
    else if (envelope.id == QStringLiteral("catalog-list")) envelope.route = ProviderRoute::Catalog;
    else if (envelope.id == QStringLiteral("attempt-create")) envelope.route = ProviderRoute::AttemptCreate;
    else if (envelope.id == QStringLiteral("attempt-questions")) envelope.route = ProviderRoute::Questions;
    else if (envelope.id == QStringLiteral("final-save")) envelope.route = ProviderRoute::FinalSave;
    else if (envelope.id == QStringLiteral("attempt-submit")) envelope.route = ProviderRoute::Submit;
    else if (envelope.id == QStringLiteral("attempt-report")) envelope.route = ProviderRoute::Report;
    else if (envelope.id == QStringLiteral("attempt-solutions")) envelope.route = ProviderRoute::Solutions;
    return envelope;
}

}  // namespace quizpane
