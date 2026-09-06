#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>

namespace quizpane {

// 当前作答会话里，做题页（PracticePageController）和解析页
// （SolutionPageController）共用的状态。物理上属于 MainWindow，通过引用
// 传给两个控制器，避免各自拷贝一份导致 materialsById_/answers_ 等状态失步。
struct AttemptSession {
    QString attemptId;
    QString attemptTitle;
    bool attemptHasAnswerKey = true;
    QJsonArray questions;
    QJsonArray solutions;
    // materialId -> material（title/contentHtml）。attempt.questions 和
    // attempt.solutions 各自返回去重后的材料数组，这里合并成一份缓存供两个
    // 页面共用，避免每次切题都重新在数组里线性查找。
    QHash<QString, QJsonObject> materialsById;
    // 单选视为大小为 1 的集合，唯一来源避免单选/多选两处分别更新导致失步。
    QVector<QSet<int>> answers;
};

}  // namespace quizpane
