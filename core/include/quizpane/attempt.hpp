#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace quizpane {

// 这组类型是纯领域模型，可以类比 Java Web 项目中的 entity/DTO。
// 它们不依赖窗口和网络，因此可以独立测试、序列化和复用。
enum class AttemptManagement { ProviderManaged, HostManaged };
enum class AttemptState { Preparing, Answering, Submitting, Submitted, Failed };

struct AnswerDraft {
    QString questionId;
    QStringList choices;
    int elapsedSeconds = 0;
};

struct Attempt {
    QString id;
    QString providerId;
    QString remoteId;
    QString catalogNodeId;
    QStringList questionIds;
    QVector<AnswerDraft> answers;
    AttemptManagement management = AttemptManagement::HostManaged;
    AttemptState state = AttemptState::Preparing;
    int requestedCount = 0;
    int actualCount = 0;

    // [[nodiscard]] 类似静态检查规则：调用方丢弃返回值时编译器会给出警告。
    [[nodiscard]] QJsonObject toJson() const;
    static Attempt fromJson(const QJsonObject& json);
};

}  // namespace quizpane
