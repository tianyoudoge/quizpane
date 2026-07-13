#include "quizpane/provider_abi.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>

struct qp_provider_handle {
    qp_host_api_v1 host{};
    QHash<int, int> answers;
};

namespace {
QJsonObject errorResponse(const QJsonValue& id, int code, const QString& message) {
    return {{"id", id},
            {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

QJsonArray demoQuestions(bool includeSolutions = false) {
    const struct Item { const char* question; const char* options[4]; int answer; const char* solution; } items[] = {
        {"水在标准大气压下的沸点是多少？", {"0℃", "50℃", "100℃", "150℃"}, 2, "标准大气压下，水的沸点是 100℃。"},
        {"某学习小组连续五天记录阅读时间。第一天阅读三十分钟，第二天比第一天多十分钟，第三天与第二天相同，第四天因临时事务只阅读二十分钟，第五天恢复到五十分钟。小组希望在不改变总阅读时间的前提下，把每天的阅读时间调整得更加均衡。若调整后五天阅读时间完全相同，那么平均每天应安排多少分钟？", {"30 分钟", "36 分钟", "40 分钟", "45 分钟"}, 1, "五天合计阅读 180 分钟，除以 5，平均每天是 36 分钟。"},
        {"中国的首都是哪座城市？", {"北京", "上海", "广州", "深圳"}, 0, "中华人民共和国首都是北京。"},
        {"一年通常有多少个月？", {"10", "11", "12", "13"}, 2, "公历一年分为 12 个月。"},
        {"太阳从哪个方向升起？", {"东", "南", "西", "北"}, 0, "地球自西向东自转，因此通常观察到太阳从东方升起。"},
    };
    QJsonArray result;
    for (int i = 0; i < 5; ++i) {
        QJsonArray options;
        for (int j = 0; j < 4; ++j)
            options.append(QJsonObject{{"index", j},
                {"label", QString(QChar(char16_t(u'A' + j)))},
                {"contentHtml", QString::fromUtf8(items[i].options[j])}});
        QJsonObject question{{"id", QStringLiteral("demo-q%1").arg(i + 1)},
            {"type", "single_choice"},
            {"contentHtml", QStringLiteral("<p>%1</p>").arg(QString::fromUtf8(items[i].question))},
            {"difficulty", 1}, {"options", options}};
        if (includeSolutions) {
            question.insert("correctChoice", items[i].answer);
            question.insert("solutionHtml", QStringLiteral("<p>%1</p>").arg(
                QString::fromUtf8(items[i].solution)));
        }
        result.append(question);
    }
    return result;
}

QJsonObject handleRequest(qp_provider_handle* handle, const QJsonObject& request) {
    const auto id = request.value("id");
    const QString method = request.value("method").toString();
    if (method == "provider.initialize") {
        return {{"id", id},
                {"result", QJsonObject{{"providerId", "org.quizpane.demo"},
                                       {"providerVersion", "0.1.0"},
                                       {"providerAbi", 1},
                                       {"requiresLogin", false},
                                       {"sessionRestored", true}}}};
    }
    if (method == "provider.capabilities") {
        return {{"id", id},
                {"result", QJsonObject{
                               {"loginMethods", QJsonArray{"none"}},
                               {"attempt", QJsonObject{{"management", "host_managed"},
                                                       {"suggestedCounts", QJsonArray{5, 10, 15}},
                                                       {"canResume", true}}}}}};
    }
    if (method == "catalog.list") {
        return {{"id", id},
                {"result", QJsonObject{{"nodes", QJsonArray{
                    QJsonObject{{"id", "demo-general"},
                                {"title", QStringLiteral("示例常识")},
                                {"availableQuestionCount", 15},
                                {"canStartAttempt", true}}}}}}};
    }
    if (method == "attempt.create" || method == "attempt.get") {
        if (method == "attempt.create") handle->answers.clear();
        return {{"id", id}, {"result", QJsonObject{
            {"attemptId", "demo-attempt-1"}, {"title", QStringLiteral("示例常识")},
            {"status", "active"}, {"questionCount", 5}, {"elapsedSeconds", 0}}}};
    }
    if (method == "attempt.questions")
        return {{"id", id}, {"result", QJsonObject{{"questions", demoQuestions()}}}};
    if (method == "attempt.saveAnswers") {
        for (const auto& answerValue : request.value("params").toObject()
                 .value("answers").toArray()) {
            const auto answer = answerValue.toObject();
            bool ok = false;
            const int choice = answer.value("answer").toObject()
                                   .value("choice").toString().toInt(&ok);
            if (ok) handle->answers.insert(answer.value("questionIndex").toInt(), choice);
        }
        return {{"id", id}, {"result", QJsonObject{{"ok", true}}}};
    }
    if (method == "attempt.submit")
        return {{"id", id}, {"result", QJsonObject{{"ok", true}}}};
    if (method == "attempt.report") {
        const int correctChoices[] = {2, 1, 0, 2, 0};
        int correctCount = 0;
        for (int i = 0; i < 5; ++i)
            if (handle->answers.value(i, -1) == correctChoices[i]) ++correctCount;
        return {{"id", id}, {"result", QJsonObject{{"attemptId", "demo-attempt-1"},
            {"title", QStringLiteral("示例常识")}, {"questionCount", 5},
            {"answerCount", handle->answers.size()}, {"correctCount", correctCount},
            {"elapsedSeconds", 42}}}};
    }
    if (method == "attempt.solutions")
        return {{"id", id}, {"result", QJsonObject{{"solutions", demoQuestions(true)}}}};
    return errorResponse(id, -32601, QStringLiteral("方法不存在：%1").arg(method));
}
}  // namespace

extern "C" QP_PROVIDER_EXPORT uint32_t qp_provider_abi_version() {
    return QP_PROVIDER_ABI_V1;
}

extern "C" QP_PROVIDER_EXPORT const char* qp_provider_descriptor_json() {
    return R"({"id":"org.quizpane.demo","name":"示例题库","version":"0.1.0","providerAbi":1})";
}

extern "C" QP_PROVIDER_EXPORT int qp_provider_create(
    const qp_host_api_v1* host, qp_provider_handle** out_handle) {
    if (!host || !out_handle || host->abi_version != QP_PROVIDER_ABI_V1) return 1;
    auto* handle = new qp_provider_handle;
    handle->host = *host;
    *out_handle = handle;
    return 0;
}

extern "C" QP_PROVIDER_EXPORT int qp_provider_request(
    qp_provider_handle* handle, const char* request_json, size_t request_size,
    qp_response_callback callback, void* user_data) {
    if (!handle || !request_json || !callback) return 1;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(request_json, static_cast<qsizetype>(request_size)), &error);
    QJsonObject response;
    if (!document.isObject()) {
        response = errorResponse({}, -32700, error.errorString());
    } else {
        response = handleRequest(handle, document.object());
    }
    const QByteArray bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
    callback(user_data, bytes.constData(), static_cast<size_t>(bytes.size()));
    return 0;
}

extern "C" QP_PROVIDER_EXPORT int qp_provider_cancel(
    qp_provider_handle*, const char*, size_t) {
    return 0;
}

extern "C" QP_PROVIDER_EXPORT void qp_provider_destroy(qp_provider_handle* handle) {
    delete handle;
}
