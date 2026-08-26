#pragma once

#include <QJsonObject>
#include <QString>

namespace quizpane::feedback {

// 组装上报用的环境快照：应用名/版本、OS、架构、Qt 版本、磁盘剩余空间、
// 最近崩溃产物的存在性与时间。刻意不含用户名、机器名、文件完整路径、
// 题目内容等敏感信息（日志部分另行脱敏）。
QJsonObject buildEnvironmentInfo();

// 取日志文件最后 maxLines 行并做脱敏（Bearer/ApiKey + 用户路径），上报内容
// 还会限制在 512 KiB 内，防止高频事件挤占整个反馈包。
// 日志不存在或已关闭时返回空字符串。
QString buildLogTail(int maxLines = 1000);

struct SendResult {
    bool success = false;
    QString message;
};

// 将与在线上报相同的脱敏诊断包写入 filePath，供无网络时由用户手动转交。
// 文件是 JSON，包含问题描述、环境信息和用户勾选的日志/崩溃附件。
SendResult exportReport(const QString& description, bool includeLogs,
                        bool includeCrash, const QString& filePath);

// 组装 payload 并 POST 到 endpoint（默认 https://xutianyou.cc/quizpane/api/feedback）。
// 同步等待（内部临时跑事件循环），适合对话框线程调用；timeoutMs 默认 30 秒。
SendResult sendReport(const QString& description, bool includeLogs,
                      bool includeCrash, const QString& endpoint = QString(),
                      int timeoutMs = 30000);

} // namespace quizpane::feedback
