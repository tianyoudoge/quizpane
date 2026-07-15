#pragma once

#include <QJsonObject>
#include <QString>

namespace quizpane {

enum class ProviderRoute {
    Initialize, AuthBegin, AuthPoll, Catalog, AttemptCreate, Questions,
    FinalSave, Submit, Report, Solutions, SaveAnswer, Unknown
};

struct ProviderResponseEnvelope {
    QString id;
    ProviderRoute route = ProviderRoute::Unknown;
    QJsonObject result;
    QJsonObject error;
    bool failed = false;
};

ProviderResponseEnvelope routeProviderResponse(const QJsonObject& response);

}  // namespace quizpane
