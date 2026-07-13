#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace quizpane {

// 安装前从压缩包读取出的“只读检查结果”。先 inspect、让用户确认，再 install，
// 类似 Web 后端的 validate -> confirm -> persist 三阶段，避免确认期间文件被替换。
struct ProviderPackageInfo {
    QString packagePath;
    QString id;
    QString name;
    QString version;
    QString platformKey;
    QString libraryRelativePath;
    bool requestsNetwork = false;
    QByteArray packageSha256;
    QJsonObject manifest;
};

struct ProviderInstallResult {
    QString installDirectory;
    QString libraryPath;
};

struct InstalledProviderInfo {
    QString id;
    QString name;
    QString version;
    QString installDirectory;
    QString libraryPath;
    QJsonObject manifest;
};

class ProviderInstaller final {
public:
    // ProviderInstaller 只负责文件系统和清单，不负责加载动态库或调用题库接口。
    // 这种边界等价于把 repository 与业务 service 分开。
    static QString currentPlatformKey();
    static QString providersRoot();

    bool inspect(const QString& packagePath, ProviderPackageInfo* info,
                 QString* error = nullptr) const;
    bool install(const ProviderPackageInfo& package,
                 ProviderInstallResult* result,
                 QString* error = nullptr) const;
    QList<InstalledProviderInfo> listInstalled(QString* error = nullptr) const;
    bool removeInstalled(const QString& providerId,
                         QString* error = nullptr) const;
};

}  // namespace quizpane
