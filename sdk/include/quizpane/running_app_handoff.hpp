#pragma once

#include <QString>

namespace quizpane {

// 题库制作器与已经运行的小窗刷题之间的本机控制通道。协议只传已安装题库的
// 绝对入口路径，绝不传题目、答案或 API Key；服务端由当前用户的本地 socket
// 权限保护。版本写入名称，今后变更协议不会与旧客户端互相误解。
QString runningAppControlServerName();

// 将新安装题库交给当前运行的小窗刷题。返回 true 表示对方已确认接收；false
// 表示当前没有可接收实例、通信失败或对方拒绝加载，调用方可根据 error 决定是否
// 回退为启动同一安装包内的主程序。
bool handoffProviderToRunningApp(const QString& providerEntryPath, QString* error = nullptr);

} // namespace quizpane
