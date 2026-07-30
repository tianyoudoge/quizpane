#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace quizpane {

// 纯领域模型（等价于 entity/DTO），不依赖 UI 和网络，可独立测试。

// 一次做题记录由谁持久化：ProviderManaged 表示答案提交给
// 远端题库来源保存，本地不留历史；HostManaged 表示由本
// 客户端自行落盘保存（见 DraftStore）。
enum class AttemptManagement { ProviderManaged, HostManaged };

// 状态机，只允许单向流转：Preparing -> Answering ->
// Submitting -> Submitted，任意阶段异常都会跳转到
// Failed（终态，不再流转）。
enum class AttemptState { Preparing, Answering, Submitting, Submitted, Failed };

struct AnswerDraft {
    QString questionId;
    QStringList choices;     // 存 optionId 而非选项文本，类似外键引用
    int elapsedSeconds = 0;
};

struct Attempt {
    QString id;              // 本地生成的练习 id，与 remoteId 分属两套命名空间
    QString providerId;
    QString remoteId;        // Provider 侧的记录 id，可能为空（纯本地练习）
    QString catalogNodeId;
    QStringList questionIds; // 与 answers 同序，按下标对应
    QVector<AnswerDraft> answers;
    AttemptManagement management = AttemptManagement::HostManaged;
    AttemptState state = AttemptState::Preparing;
    int requestedCount = 0;
    int actualCount = 0;     // 题库不足时 < requestedCount

    // Qt 的 JSON API 没有反射，序列化只能手写字段映射。
    [[nodiscard]] QJsonObject toJson() const;
    static Attempt fromJson(const QJsonObject& json);
};

}  // namespace quizpane
