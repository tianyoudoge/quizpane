#include "quizpane/provider_installer.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

#include "quizpane/provider_abi.h"
#include "quizpane/zip_archive.hpp"

namespace quizpane {
namespace {

constexpr qsizetype kMaxEntries = 256;
constexpr qint64 kMaxExpandedBytes = 128 * 1024 * 1024;
constexpr qint64 kMaxManifestBytes = 1024 * 1024;

bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}

bool safeRelativePath(const QString& path) {
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains('\\'))
        return false;
    const QString clean = QDir::cleanPath(path);
    return clean != QStringLiteral("..") && !clean.startsWith(QStringLiteral("../")) &&
           clean == path && !QFileInfo(path).isAbsolute();
}

bool writeJson(const QString& path, const QJsonObject& value, QString* error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, QStringLiteral("无法写入 %1：%2").arg(path, file.errorString()));
    file.write(QJsonDocument(value).toJson(QJsonDocument::Indented));
    if (!file.commit())
        return fail(error, QStringLiteral("无法提交 %1：%2").arg(path, file.errorString()));
    return true;
}

QByteArray sha256File(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result();
}

QJsonObject readJson(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxManifestBytes)
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

bool validProviderId(const QString& id) {
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z0-9]+(?:[.-][a-z0-9-]+)+$"));
    return pattern.match(id).hasMatch();
}

}  // namespace

QString ProviderInstaller::currentPlatformKey() {
#if defined(Q_OS_MACOS)
#  if defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("macos-arm64");
#  else
    return QStringLiteral("macos-x86_64");
#  endif
#elif defined(Q_OS_WIN)
#  if defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("windows-arm64");
#  else
    return QStringLiteral("windows-x64");
#  endif
#elif defined(Q_OS_LINUX)
#  if defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("linux-arm64");
#  else
    return QStringLiteral("linux-x86_64");
#  endif
#else
    return QStringLiteral("unknown");
#endif
}

QString ProviderInstaller::providersRoot() {
    const QString overridePath = qEnvironmentVariable("QUIZPANE_PROVIDERS_ROOT");
    if (!overridePath.isEmpty()) return overridePath;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("QuizPane/providers"));
}

bool ProviderInstaller::inspect(const QString& packagePath, ProviderPackageInfo* info,
                                QString* error) const {
    // 注意：不能直接 unzip。这里先逐项检查相对路径和符号链接，阻止 zip-slip
    // 把文件写到安装目录外；同时限制文件数量和解压后总体积，避免压缩炸弹。
    if (!info) return fail(error, QStringLiteral("缺少安装包信息输出参数"));
    if (!packagePath.endsWith(QStringLiteral(".quizpane-provider"),
                              Qt::CaseInsensitive))
        return fail(error, QStringLiteral("文件扩展名必须是 .quizpane-provider"));

    ZipArchiveReader zip(packagePath);
    if (!zip.isReadable())
        return fail(error, QStringLiteral("安装包不是可读取的 ZIP 文件"));
    const auto entries = zip.entries();
    if (entries.isEmpty() || entries.size() > kMaxEntries)
        return fail(error, QStringLiteral("安装包文件数量不合法"));

    qint64 expandedBytes = 0;
    bool hasManifest = false;
    for (const auto& entry : entries) {
        if (!safeRelativePath(entry.path) || entry.isSymbolicLink)
            return fail(error, QStringLiteral("安装包包含不安全路径：%1").arg(entry.path));
        if (entry.size < 0 || expandedBytes > kMaxExpandedBytes - entry.size)
            return fail(error, QStringLiteral("安装包解压后体积超过 128 MiB"));
        expandedBytes += entry.size;
        if (entry.path == QStringLiteral("manifest.json") && entry.isFile)
            hasManifest = true;
    }
    if (!hasManifest) return fail(error, QStringLiteral("安装包缺少 manifest.json"));

    const QByteArray manifestBytes = zip.fileData(QStringLiteral("manifest.json"));
    if (manifestBytes.isEmpty() || manifestBytes.size() > kMaxManifestBytes)
        return fail(error, QStringLiteral("manifest.json 为空或过大"));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (!document.isObject())
        return fail(error, QStringLiteral("manifest.json 无效：%1").arg(parseError.errorString()));

    const QJsonObject manifest = document.object();
    const QString id = manifest.value(QStringLiteral("id")).toString();
    const QString name = manifest.value(QStringLiteral("name")).toString();
    const QString version = manifest.value(QStringLiteral("version")).toString();
    static const QRegularExpression idPattern(
        QStringLiteral("^[a-z0-9]+(?:[.-][a-z0-9-]+)+$"));
    static const QRegularExpression versionPattern(
        QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$"));
    const QString kind = manifest.value(QStringLiteral("kind")).toString();
    if (manifest.value(QStringLiteral("manifestVersion")).toInt() != 2 ||
        (kind != QStringLiteral("native") && kind != QStringLiteral("declarative")) ||
        !idPattern.match(id).hasMatch() || name.trimmed().isEmpty() ||
        !versionPattern.match(version).hasMatch())
        return fail(error, QStringLiteral("题库安装包的名称、版本或标识不合法"));

    const QString platform = currentPlatformKey();
    QString entry;
    if (kind == QStringLiteral("native")) {
        if (manifest.value(QStringLiteral("providerAbi")).toInt() != QP_PROVIDER_ABI_V1)
            return fail(error, QStringLiteral("原生题库 ABI 与当前应用不兼容"));
        entry = manifest.value(QStringLiteral("runtime")).toObject()
                    .value(QStringLiteral("libraries")).toObject().value(platform).toString();
        if (!safeRelativePath(entry))
            return fail(error, QStringLiteral("安装包不支持当前平台：%1").arg(platform));
    } else {
        const QJsonObject runtime = manifest.value(QStringLiteral("runtime")).toObject();
        if (runtime.value(QStringLiteral("format")).toString() !=
                QStringLiteral("quizpane.bank+json") ||
            runtime.value(QStringLiteral("schemaVersion")).toInt() != 1)
            return fail(error, QStringLiteral("声明式题库格式与当前应用不兼容"));
        entry = runtime.value(QStringLiteral("entry")).toString();
        if (!safeRelativePath(entry) || !entry.endsWith(QStringLiteral(".json")))
            return fail(error, QStringLiteral("声明式题库入口路径不合法"));
        if (manifest.value(QStringLiteral("permissions")).toObject()
                .value(QStringLiteral("network")).toBool())
            return fail(error, QStringLiteral("声明式题库不能申请网络权限"));
    }
    const bool hasEntry = std::any_of(entries.cbegin(), entries.cend(), [&entry](const auto& item) {
        return item.isFile && item.path == entry;
    });
    if (!hasEntry)
        return fail(error, QStringLiteral("安装包缺少题库入口：%1").arg(entry));

    const QByteArray packageHash = sha256File(packagePath);
    if (packageHash.isEmpty()) return fail(error, QStringLiteral("无法计算安装包摘要"));
    *info = ProviderPackageInfo{
        packagePath, id, name.trimmed(), version, kind, platform, entry,
        manifest.value(QStringLiteral("permissions")).toObject()
            .value(QStringLiteral("network")).toBool(),
        packageHash,
        manifest};
    return true;
}

bool ProviderInstaller::install(const ProviderPackageInfo& package,
                                ProviderInstallResult* result,
                                QString* error) const {
    ProviderPackageInfo verified;
    if (!inspect(package.packagePath, &verified, error)) return false;
    if (verified.id != package.id || verified.version != package.version ||
        verified.packageSha256 != package.packageSha256)
        return fail(error, QStringLiteral("安装包在确认后发生变化"));

    const QString root = providersRoot();
    const QString providerRoot = QDir(root).filePath(verified.id);
    const QString target = QDir(providerRoot).filePath(verified.version);
    if (QFileInfo::exists(target)) {
        const QString installedEntry =
            QDir(target).filePath(verified.entryRelativePath);
        if (!QFileInfo(installedEntry).isFile())
            return fail(error, QStringLiteral("已导入的题库不完整，请先移除后重试"));
        if (result) {
            result->installDirectory = target;
            result->entryPath = installedEntry;
        }
        return true;
    }
    if (!QDir().mkpath(providerRoot))
        return fail(error, QStringLiteral("无法创建题库安装目录"));

    // 先解压到随机 staging，完整成功后再原子 rename 到版本目录。思路与数据库
    // 事务或蓝绿发布相同：用户不会看到“安装到一半”的题库。
    const QString staging = QDir(root).filePath(
        QStringLiteral(".staging-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QDir().mkpath(staging);
    ZipArchiveReader zip(verified.packagePath);
    for (const auto& entry : zip.entries()) {
        const QString destination = QDir(staging).filePath(entry.path);
        if (entry.isDirectory) {
            if (!QDir().mkpath(destination)) {
                QDir(staging).removeRecursively();
                return fail(error, QStringLiteral("无法创建安装目录：%1").arg(entry.path));
            }
            continue;
        }
        if (!entry.isFile) continue;
        QDir().mkpath(QFileInfo(destination).absolutePath());
        QFile output(destination);
        if (!output.open(QIODevice::WriteOnly) || output.write(zip.fileData(entry.path)) != entry.size) {
            QDir(staging).removeRecursively();
            return fail(error, QStringLiteral("无法解压文件：%1").arg(entry.path));
        }
        output.close();
        output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }
    if (!QDir().rename(staging, target)) {
        QDir(staging).removeRecursively();
        return fail(error, QStringLiteral("无法启用题库安装目录"));
    }

    if (!writeJson(QDir(providerRoot).filePath(QStringLiteral("active.json")),
                   QJsonObject{{"version", verified.version}}, error))
        return false;
    if (result) {
        result->installDirectory = target;
        result->entryPath = QDir(target).filePath(verified.entryRelativePath);
    }
    return true;
}

QList<InstalledProviderInfo> ProviderInstaller::listInstalled(QString* error) const {
    QList<InstalledProviderInfo> result;
    QDir root(providersRoot());
    if (!root.exists()) return result;
    const QString platform = currentPlatformKey();
    for (const QFileInfo& providerEntry : root.entryInfoList(
             QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString id = providerEntry.fileName();
        if (!validProviderId(id)) continue;
        const QJsonObject active = readJson(
            QDir(providerEntry.filePath()).filePath(QStringLiteral("active.json")));
        const QString version = active.value(QStringLiteral("version")).toString();
        if (version.isEmpty()) continue;
        const QString installDirectory =
            QDir(providerEntry.filePath()).filePath(version);
        const QJsonObject manifest = readJson(
            QDir(installDirectory).filePath(QStringLiteral("manifest.json")));
        const QString kind = manifest.value(QStringLiteral("kind")).toString();
        const QJsonObject runtime = manifest.value(QStringLiteral("runtime")).toObject();
        const QString entryRelative = kind == QStringLiteral("native")
            ? runtime.value(QStringLiteral("libraries")).toObject().value(platform).toString()
            : runtime.value(QStringLiteral("entry")).toString();
        const QString entryPath = QDir(installDirectory).filePath(entryRelative);
        if (manifest.value(QStringLiteral("id")).toString() != id ||
            manifest.value(QStringLiteral("manifestVersion")).toInt() != 2 ||
            !safeRelativePath(entryRelative) || !QFileInfo::exists(entryPath))
            continue;
        result.append(InstalledProviderInfo{
            id,
            manifest.value(QStringLiteral("name")).toString(id),
            version,
            installDirectory,
            kind,
            entryPath,
            manifest
        });
    }
    if (error) error->clear();
    return result;
}

bool ProviderInstaller::removeInstalled(const QString& providerId,
                                        QString* error) const {
    if (!validProviderId(providerId))
        return fail(error, QStringLiteral("题库标识不合法"));
    const QString rootPath = QDir(providersRoot()).absolutePath();
    const QString target = QDir(rootPath).filePath(providerId);
    if (!QFileInfo::exists(target)) return true;
    if (!QDir(target).removeRecursively())
        return fail(error, QStringLiteral("题库文件正在使用，将在重新启动后删除"));
    return true;
}

}  // namespace quizpane
