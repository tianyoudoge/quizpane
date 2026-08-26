#pragma once

#include <QJsonObject>
#include <QString>

namespace quizpane::feedback {

// 组装上报用的环境快照：应用名/版本、OS、架构、Qt 版本、磁盘剩余空间、
// 最近崩溃产物的存在性与时间。刻意不含用户名、机器名、文件完整路径、
// 题目内容等敏感信息（日志部分另行脱敏）。
QJsonObject buildEnvironmentInfo();

// 取日志文件最后 maxLines 行并做脱敏（Bearer/ApiKey + 用户路径）。
// 日志不存在或已关闭时返回空字符串。
QString buildLogTail(int maxLines = 200);

struct SendResult {
    bool success = false;
    QString message;
};

// 组装 payload 并 POST 到 endpoint（默认 https://xutianyou.cc/quizpane/api/feedback）。
// 同步等待（内部临时跑事件循环），适合对话框线程调用；timeoutMs 默认 30 秒。
SendResult sendReport(const QString& description, bool includeLogs,
                      bool includeCrash, const QString& endpoint = QString(),
                      int timeoutMs = 30000);

} // namespace quizpane::feedback
