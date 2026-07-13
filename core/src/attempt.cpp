#include "quizpane/attempt.hpp"

#include <QJsonArray>

namespace quizpane {
namespace {
QString managementName(AttemptManagement value) {
    return value == AttemptManagement::ProviderManaged ? "provider_managed"
                                                        : "host_managed";
}
QString stateName(AttemptState value) {
    switch (value) {
        case AttemptState::Preparing: return "preparing";
        case AttemptState::Answering: return "answering";
        case AttemptState::Submitting: return "submitting";
        case AttemptState::Submitted: return "submitted";
        case AttemptState::Failed: return "failed";
    }
    return "failed";
}
}  // namespace

QJsonObject Attempt::toJson() const {
    QJsonArray questionIdsJson;
    for (const auto& questionId : questionIds) questionIdsJson.append(questionId);
    QJsonArray answersJson;
    for (const auto& answer : answers) {
        QJsonArray choices;
        for (const auto& choice : answer.choices) choices.append(choice);
        answersJson.append(QJsonObject{{"questionId", answer.questionId},
                                       {"choices", choices},
                                       {"elapsedSeconds", answer.elapsedSeconds}});
    }
    return {{"id", id},
            {"providerId", providerId},
            {"remoteId", remoteId},
            {"catalogNodeId", catalogNodeId},
            {"questionIds", questionIdsJson},
            {"answers", answersJson},
            {"management", managementName(management)},
            {"state", stateName(state)},
            {"requestedCount", requestedCount},
            {"actualCount", actualCount}};
}

Attempt Attempt::fromJson(const QJsonObject& json) {
    Attempt result;
    result.id = json.value("id").toString();
    result.providerId = json.value("providerId").toString();
    result.remoteId = json.value("remoteId").toString();
    result.catalogNodeId = json.value("catalogNodeId").toString();
    result.management = json.value("management") == "provider_managed"
                            ? AttemptManagement::ProviderManaged
                            : AttemptManagement::HostManaged;
    const QString state = json.value("state").toString();
    if (state == "answering") result.state = AttemptState::Answering;
    else if (state == "submitting") result.state = AttemptState::Submitting;
    else if (state == "submitted") result.state = AttemptState::Submitted;
    else if (state == "failed") result.state = AttemptState::Failed;
    result.requestedCount = json.value("requestedCount").toInt();
    result.actualCount = json.value("actualCount").toInt();
    for (const auto value : json.value("questionIds").toArray())
        result.questionIds.append(value.toString());
    for (const auto value : json.value("answers").toArray()) {
        const auto object = value.toObject();
        AnswerDraft answer;
        answer.questionId = object.value("questionId").toString();
        answer.elapsedSeconds = object.value("elapsedSeconds").toInt();
        for (const auto choice : object.value("choices").toArray())
            answer.choices.append(choice.toString());
        result.answers.append(answer);
    }
    return result;
}

}  // namespace quizpane
