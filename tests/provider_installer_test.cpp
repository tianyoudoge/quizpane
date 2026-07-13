#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QTemporaryDir>
#include <QtCore/private/qzipwriter_p.h>

#include "quizpane/provider_installer.hpp"

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
        QZipWriter zip(packagePath);
        zip.addFile(QStringLiteral("manifest.json"), manifest(platform, library));
        zip.addFile(library, QByteArrayLiteral("provider-binary"));
        zip.close();
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
            {"schemaVersion", 1}, {"entry", "content/bank.json"}}},
        {"permissions", QJsonObject{{"network", false}}}}).toJson();
    {
        QZipWriter zip(declarativePath);
        zip.addFile(QStringLiteral("manifest.json"), declarativeManifest);
        zip.addFile(QStringLiteral("content/bank.json"), QByteArrayLiteral("{}"));
        zip.close();
    }
    quizpane::ProviderPackageInfo declarativeInfo;
    quizpane::ProviderInstallResult declarativeResult;
    if (!installer.inspect(declarativePath, &declarativeInfo, &error) ||
        declarativeInfo.kind != QStringLiteral("declarative") ||
        !installer.install(declarativeInfo, &declarativeResult, &error) ||
        !QFileInfo(declarativeResult.entryPath).isFile()) return 11;

    const QString unsafePath = QDir(temp.path()).filePath(
        QStringLiteral("unsafe.quizpane-provider"));
    {
        QZipWriter zip(unsafePath);
        zip.addFile(QStringLiteral("manifest.json"), manifest(platform, library));
        zip.addFile(QStringLiteral("unsafe\\escape"), QByteArrayLiteral("bad"));
        zip.addFile(library, QByteArrayLiteral("provider-binary"));
        zip.close();
    }
    if (installer.inspect(unsafePath, &info, &error)) return 7;
    if (!installer.removeInstalled(QStringLiteral("org.quizpane.installer-test"),
                                   &error) ||
        !installer.removeInstalled(QStringLiteral("org.quizpane.local-test"), &error) ||
        !installer.listInstalled(&error).isEmpty()) return 9;
    return 0;
}
