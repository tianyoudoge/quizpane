#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QTemporaryDir>

#include <algorithm>

#include "quizpane/provider_installer.hpp"
#include "quizpane/zip_archive.hpp"

namespace {

QByteArray manifest(const QString& platform, const QString& library) {
    return QJsonDocument(QJsonObject{
        {"manifestVersion", 2},
        {"id", "org.quizpane.installer-test"},
        {"name", "Installer Test"},
        {"version", "1.2.3"},
        {"kind", "native"},
        {"providerAbi", 1},
        {"runtime", QJsonObject{{"libraries", QJsonObject{{platform, library}}}}},
        {"permissions", QJsonObject{{"network", false}}}})
        .toJson(QJsonDocument::Compact);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1) {
        quizpane::ProviderInstaller installer;
        quizpane::ProviderPackageInfo package;
        QString error;
        return installer.inspect(QString::fromLocal8Bit(argv[1]), &package, &error)
            ? 0 : 10;
    }
    QCoreApplication::setApplicationName(QStringLiteral("QuizPaneInstallerTest"));
    QTemporaryDir temp;
    if (!temp.isValid()) return 1;

    qputenv("QUIZPANE_PROVIDERS_ROOT",
            QDir(temp.path()).filePath(QStringLiteral("providers")).toUtf8());
    const QString platform = quizpane::ProviderInstaller::currentPlatformKey();
    const QString library = QStringLiteral("bin/provider-test.bin");
    const QString packagePath = QDir(temp.path()).filePath(
        QStringLiteral("valid.quizpane-provider"));
    {
        if (!quizpane::writeZipArchive(packagePath, {
                {QStringLiteral("manifest.json"), manifest(platform, library)},
                {library, QByteArrayLiteral("provider-binary")}})) return 12;
    }

    quizpane::ProviderInstaller installer;
    quizpane::ProviderPackageInfo info;
    QString error;
    if (!installer.inspect(packagePath, &info, &error)) {
        qCritical() << error;
        return 2;
    }
    if (info.id != QStringLiteral("org.quizpane.installer-test") ||
        info.packageSha256.isEmpty()) return 3;
    quizpane::ProviderInstallResult result;
    if (!installer.install(info, &result, &error)) {
        qCritical() << error;
        return 4;
    }
    if (!QFileInfo(result.entryPath).isFile()) return 5;
    quizpane::ProviderInstallResult repeatedResult;
    if (!installer.install(info, &repeatedResult, &error) ||
        repeatedResult.entryPath != result.entryPath) return 6;
    const auto installed = installer.listInstalled(&error);
    if (installed.size() != 1 ||
        installed.first().id != QStringLiteral("org.quizpane.installer-test") ||
        installed.first().entryPath != result.entryPath) return 8;

    const QString declarativePath = QDir(temp.path()).filePath(
        QStringLiteral("declarative.quizpane-provider"));
    const QByteArray declarativeManifest = QJsonDocument(QJsonObject{
        {"manifestVersion", 2}, {"id", "org.quizpane.local-test"},
        {"name", "Local Test"}, {"version", "1.0.0"},
        {"kind", "declarative"},
        {"runtime", QJsonObject{{"format", "quizpane.bank+json"},
            {"schemaVersion", 2}, {"entry", "content/bank.json"}}},
        {"permissions", QJsonObject{{"network", false}}}}).toJson();
    {
        if (!quizpane::writeZipArchive(declarativePath, {
                {QStringLiteral("manifest.json"), declarativeManifest},
                {QStringLiteral("content/bank.json"), QByteArrayLiteral("{}")}})) return 13;
    }
    quizpane::ProviderPackageInfo declarativeInfo;
    quizpane::ProviderInstallResult declarativeResult;
    if (!installer.inspect(declarativePath, &declarativeInfo, &error) ||
        declarativeInfo.kind != QStringLiteral("declarative") ||
        !installer.install(declarativeInfo, &declarativeResult, &error) ||
        !QFileInfo(declarativeResult.entryPath).isFile()) return 11;
    const auto providersBeforeExport = installer.listInstalled(&error);
    const auto exportedProvider = std::find_if(
        providersBeforeExport.cbegin(), providersBeforeExport.cend(),
        [](const quizpane::InstalledProviderInfo& item) {
            return item.id == QStringLiteral("org.quizpane.local-test");
        });
    const QString exportedPath = QDir(temp.path()).filePath(
        QStringLiteral("exported.quizpane-provider"));
    quizpane::ProviderPackageInfo exportedInfo;
    if (exportedProvider == providersBeforeExport.cend() ||
        !installer.exportDeclarative(*exportedProvider, exportedPath, &error) ||
        !installer.inspect(exportedPath, &exportedInfo, &error) ||
        exportedInfo.id != QStringLiteral("org.quizpane.local-test")) return 17;

    // v1 声明式包直接拒绝，不保留兼容安装路径。
    const QString legacyDeclarativePath = QDir(temp.path()).filePath(
        QStringLiteral("legacy-v1.quizpane-provider"));
    QJsonObject legacyManifestObject = QJsonDocument::fromJson(declarativeManifest).object();
    QJsonObject legacyRuntime = legacyManifestObject.value("runtime").toObject();
    legacyRuntime.insert("schemaVersion", 1);
    legacyManifestObject.insert("runtime", legacyRuntime);
    if (!quizpane::writeZipArchive(legacyDeclarativePath, {
            {QStringLiteral("manifest.json"), QJsonDocument(legacyManifestObject).toJson()},
            {QStringLiteral("content/bank.json"), QByteArrayLiteral("{}")}})) return 15;
    if (installer.inspect(legacyDeclarativePath, &declarativeInfo, &error)) return 16;

    const QString unsafePath = QDir(temp.path()).filePath(
        QStringLiteral("unsafe.quizpane-provider"));
    {
        if (!quizpane::writeZipArchive(unsafePath, {
                {QStringLiteral("manifest.json"), manifest(platform, library)},
                {QStringLiteral("unsafe\\escape"), QByteArrayLiteral("bad")},
                {library, QByteArrayLiteral("provider-binary")}})) return 14;
    }
    if (installer.inspect(unsafePath, &info, &error)) return 7;
    if (!installer.removeInstalled(QStringLiteral("org.quizpane.installer-test"),
                                   &error) ||
        !installer.removeInstalled(QStringLiteral("org.quizpane.local-test"), &error) ||
        !installer.listInstalled(&error).isEmpty()) return 9;
    return 0;
}
