#pragma once

#include <QJsonArray>
#include <QString>
#include <QVector>

namespace quizpane {

// 一次未完成答题的持久化快照。这里直接保存标准化后的题目 JSON，恢复时
// 不必依赖网络；可以理解成前端 localStorage 中的表单草稿，但实际写入磁盘。
struct DraftSnapshot {
    QString providerId;
    QString attemptId;
    QString title;
    QJsonArray questions;
    QVector<int> answers;
    int currentQuestionIndex = 0;
};

class DraftStore final {
public:
    // final 表示该类不准备被继承，接近 Java 的 final class。
    // 方法本身无状态，因此保持 const，便于确认不会修改 DraftStore 对象。
    bool save(const DraftSnapshot& snapshot, QString* error = nullptr) const;
    bool load(const QString& providerId, DraftSnapshot* snapshot,
              QString* error = nullptr) const;
    bool clear(const QString& providerId, QString* error = nullptr) const;
    QString pathForProvider(const QString& providerId) const;
};

}  // namespace quizpane
